/* synth.c - control protocol, the control-input model that stands in for Engine_Sim's drivetrain (physics.js /
 * shift.js), voice crossfades, master/mute/watchdog gains and the final limiter. */
#include "synth.h"
#include <stdlib.h>
#include <string.h>

#define FRAME_DT ((float)(FRAME_Q * QN) / SR_F)
#define POP_MIN_RPM 2000
/* shift.js GEARBOX_TYPES.manual with the 'sports' vehicle's shiftTimeMs = 95 */
#define T_CUT 0.030f
#define T_OPEN 0.035f
#define T_SYNC 0.018f
#define T_ENGAGE 0.050f
#define T_LASH 0.030f
#define T_SHUFFLE 0.360f

static void enable_ftz(void) {
#if defined(__aarch64__)
    uint64_t fpcr;
    __asm__ volatile("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= 1ull << 24;
    __asm__ volatile("msr fpcr, %0" : : "r"(fpcr));
#elif defined(__SSE__)
    unsigned csr;
    __asm__ volatile("stmxcsr %0" : "=m"(csr));
    csr |= 0x8040;
    __asm__ volatile("ldmxcsr %0" : : "m"(csr));
#endif
}

static float drand(Drive *d) {
    return (float)xs_unit(&d->rnd);
}
static void fire(float *ev, float mag, float rpm, int is_pop) {
    if (!(mag > *ev)) return;
    if (is_pop && rpm < POP_MIN_RPM) return;
    *ev = mag > 1 ? 1 : mag;
}

static void activate(Synth *s, int k, double now) {
    Voice *v = &s->voice[k];
    if (!v->active) {
        voice_init(v, &presets[k], s->turbo);
        v->active = 1;
        v->weight = 0;
    } else
        voice_set_turbo(v, s->turbo, now);
    v->wtarget = 1;
}

void synth_init(Synth *s, int preset, float volume) {
    int k;
    enable_ftz();
    engine_global_init();
    memset(s, 0, sizeof(*s));
    fx_init(&s->fx);
    s->selected = preset;
    s->turbo = presets[preset].turbo;
    s->volume = volume;
    sp_init(&s->vol, volume);
    s->fx.master.v = s->fx.master.t = volume;
    sp_init(&s->mutegain, 1);
    sp_init(&s->stale, 0);
    sp_init(&s->quitg, 1);
    s->last_p = -1e9;
    s->opos = QN;
    s->rpm = presets[preset].idle;
    s->dr.rpm = s->dr.prevRpm = s->rpm;
    s->dr.rnd = 0x1234567u;
    for (k = 0; k < NP; k++) { /* warm every preset's combustion calibration so E never stalls the audio */
        voice_init(&s->voice[k], &presets[k], 0);
        s->voice[k].active = 0;
    }
    activate(s, preset, 0);
    s->voice[preset].weight = 1;
    {
        const char *dump = getenv("ES_DUMP_PARAMS");
        if (dump && *dump) s->dump = fopen(dump, "w");
    }
}

int synth_control(Synth *s, const char *line) {
    float r, l, t, v;
    int g, k, a, b, n = 0;
    char name[32], extra;
    if (sscanf(line, "P %f %f %f %d %f %n", &r, &l, &t, &g, &v, &n) == 5 && n && line[n] == 0 && isfinite(r) &&
        isfinite(l) && isfinite(t) && isfinite(v) && g >= -1 && g <= 10) {
        s->rpm = clampf(r, 0, 10000);
        s->load = clampf(l, -1, 1);
        s->throttle = clampf(t, 0, 1);
        s->speed = clampf(v, 0, 400);
        s->gear = g;
        s->last_p = s->time;
        return 1;
    }
    if (sscanf(line, "E %31s %c", name, &extra) == 1 && (k = preset_find(name)) >= 0) {
        int j;
        if (k != s->selected) {
            s->selected = k;
            s->turbo = presets[k].turbo;
            for (j = 0; j < NP; j++) s->voice[j].wtarget = 0;
            activate(s, k, s->audio_t);
        }
        return 1;
    }
    if (sscanf(line, "T %d %c", &k, &extra) == 1 && (k == 0 || k == 1)) {
        s->turbo = k;
        voice_set_turbo(&s->voice[s->selected], k, s->audio_t);
        return 1;
    }
    if (sscanf(line, "M %d %c", &k, &extra) == 1 && (k == 0 || k == 1)) {
        s->mute = k;
        return 1;
    }
    if (sscanf(line, "V %f %c", &v, &extra) == 1 && isfinite(v) && v >= 0 && v <= 1) {
        s->volume = v;
        return 1;
    }
    if (sscanf(line, "S %d %d %c", &a, &b, &extra) == 2 && a >= -1 && a <= 10 && b >= -1 && b <= 10) {
        if (a > 0 && b > 0 && a != b) {
            s->pending_shift_a = a;
            s->pending_shift_b = b;
        }
        return 1;
    }
    if (line[0] == 'Q' && sscanf(line + 1, " %c", &extra) != 1) {
        s->quit = 1;
        return 1;
    }
    return 0;
}

/* ---- shift.js: cut -> open -> sync -> engage -> lash -> shuffle, with its events ---- */
static void shift_start(Synth *s, Drive *d, int a, int b) {
    const Preset *pr = &presets[s->selected];
    float heat = clampf((d->egt - 0.45f) / 0.35f, 0, 1), mag = 0.85f * (0.25f + 0.75f * s->throttle) * heat;
    float pFire = clampf((0.12f + 0.55f * heat) * 0.85f, 0, 0.9f);
    float ra = pr->ratios[(a > pr->gears ? pr->gears : a) - 1], rb = pr->ratios[(b > pr->gears ? pr->gears : b) - 1];
    d->sphase = PH_CUT;
    d->st = 0;
    d->sdown = b < a;
    d->sratio = rb / ra;
    if (mag > 0.04f && drand(d) < pFire) fire(&d->evCut, mag, d->rpm, 0);
}
/* returns fuel cut (1 = injectors off), sets *override (rev-match blip, < 0 = none), *neutral, *clutch */
static float shift_step(Synth *s, Drive *d, float h, float *override, int *neutral, float *clutch, float *slip) {
    float cut = 0;
    *override = -1;
    *neutral = 0;
    *clutch = 1;
    *slip = 0;
    if (d->sphase == PH_NONE) return 0;
    d->st += h;
    for (;;) {
        if (d->sphase == PH_CUT && d->st >= T_CUT) {
            d->st -= T_CUT;
            d->sphase = PH_OPEN;
        } else if (d->sphase == PH_OPEN && d->st >= (d->sdown ? 2 * T_OPEN : T_OPEN)) {
            d->st -= d->sdown ? 2 * T_OPEN : T_OPEN;
            d->sphase = PH_SYNC;
        } else if (d->sphase == PH_SYNC && d->st >= T_SYNC) {
            d->st -= T_SYNC;
            d->sphase = PH_ENGAGE;
            /* clutch bite: slip left between the engine and the new gear's input shaft */
            d->sslip = d->sdown ? 120 : 0.5f * fabsf(d->rpm * (1 - d->sratio));
            fire(&d->evEngage, clampf(0.15f + d->sslip / 1800, 0, 1), d->rpm, 0);
        } else if (d->sphase == PH_ENGAGE && d->st >= T_ENGAGE) {
            d->st -= T_ENGAGE;
            d->sphase = PH_LASH;
            fire(&d->evShiftDone, 1, d->rpm, 0);
            /* the driveline takes up its backlash as torque comes back */
            fire(&d->evLash, clampf(0.12f + 0.45f * s->throttle, 0.05f, 1), d->rpm, 0);
        } else if (d->sphase == PH_LASH && d->st >= T_LASH) {
            d->st -= T_LASH;
            d->sphase = PH_SHUFFLE;
        } else if (d->sphase == PH_SHUFFLE && d->st >= T_SHUFFLE) {
            d->sphase = PH_NONE;
            d->st = 0;
            return 0;
        } else
            break;
    }
    switch (d->sphase) {
    case PH_CUT:
        cut = 1;
        break;
    case PH_OPEN:
        *neutral = 1;
        *clutch = 0;
        if (d->sdown)
            *override = 0.85f; /* blip */
        else
            cut = 1;
        break;
    case PH_SYNC:
        *neutral = 1;
        *clutch = 0;
        cut = 1;
        break;
    case PH_ENGAGE: {
        float u = clampf(d->st / T_ENGAGE, 0, 1), sm = u * u * (3 - 2 * u);
        *clutch = sm;
        *slip = d->sslip * (1 - sm);
        break;
    }
    default:
        break;
    }
    return cut;
}

/* physics.js _stepTurbo (boost pressure + the blow-off event) */
static void turbo_step(Drive *d, const Preset *pr, float h, float thr, int limiterActive) {
    float onset = 3500 < 0.65f * pr->redline ? 3500 : 0.65f * pr->redline, full, spoolRpm, target, tau;
    full = 0.6f * pr->redline > onset + 1500 ? 0.6f * pr->redline : onset + 1500;
    spoolRpm = clampf((d->rpm - onset) / (full - onset), 0, 1);
    target = clampf(thr, 0, 1) * spoolRpm;
    tau = d->boostDump > 0 ? 0.05f : (target > d->boost ? 0.25f + 0.42f * 0.9f : 0.18f + 0.42f * 0.4f);
    d->boost += (target - d->boost) * (h / tau < 1 ? h / tau : 1);
    d->boost = clampf(d->boost, 0, 1);
    if (d->boostDump > 0) d->boostDump -= h;
    if (thr > 0.45f)
        d->bovArm = 1;
    else if (d->bovArm && thr < 0.12f && d->boost > 0.18f && !limiterActive) {
        d->bovArm = 0;
        d->boostDump = 0.12f;
        fire(&d->evBov, clampf(d->boost * (0.4f + 0.6f * 0.40f), 0.1f, 1), d->rpm, 0);
    }
}

/* physics.js _stepExhaustThermal: exhaust heat, fuel cut and the pops */
static void thermal_step(Synth *s, Drive *d, const Preset *pr, float h, float thr, int neutral, float override,
                         int limiterActive) {
    float rpm = d->rpm, rpmN = clampf((rpm - pr->idle) / (pr->redline - pr->idle > 1 ? pr->redline - pr->idle : 1), 0, 1);
    float target = clampf(0.12f + 0.88f * powf(thr, 0.6f) * (0.35f + 0.65f * rpmN), 0, 1);
    float tau = target > d->egt ? 5.0f : 26.0f, pedal = s->throttle;
    int cutting;
    d->egt += (target - d->egt) * (h / tau < 1 ? h / tau : 1);
    /* DFCO: closed pedal, revs up, and the overrun has lasted a moment (engine braking reported by load < 0) */
    d->overrunHeld = s->load <= 0 ? d->overrunHeld + h : 0;
    cutting = pedal < 0.06f && rpm > pr->idle * 1.5f && !neutral && d->overrunHeld >= 0.1f;
    d->dfco = cutting;
    d->thrPeak = pedal > d->thrPeak - h / 0.7f ? pedal : d->thrPeak - h / 0.7f;
    d->popRefractory = d->popRefractory - h > 0 ? d->popRefractory - h : 0;
    d->limiterBark = d->limiterBark - h > 0 ? d->limiterBark - h : 0;
    if (limiterActive && !d->prevLimiter && d->egt > 0.5f && d->limiterBark <= 0) {
        d->limiterBark = 0.34f;
        fire(&d->evPop, clampf(0.16f + 0.14f * drand(d), 0, 1), rpm, 1);
    }
    d->prevLimiter = limiterActive;
    if (d->dfco != d->prevDfco) {
        d->dfcoEdge = 1;
        if (d->dfco) {
            float snap = clampf(d->thrPeak, 0, 1), rev = powf(rpmN, 1.7f), luck = 0.55f + drand(d) * 0.95f, pFire;
            d->cutRpmN = rpmN;
            d->popCharge = clampf(d->popCharge + (0.5f + 2.1f * rev) * (0.4f + 0.6f * snap) * luck, 0, 3.4f);
            pFire = clampf((d->egt - 0.42f) / 0.34f, 0, 1) * (0.18f + 0.82f * rev) * (0.45f + 0.55f * snap);
            if (d->egt > 0.42f && d->popRefractory <= 0 && drand(d) < pFire) {
                d->popRefractory = 0.45f;
                fire(&d->evPop,
                     clampf((d->egt - 0.35f) * 1.25f * (0.10f + 1.30f * rev) * (0.35f + 0.65f * snap) * luck, 0, 1), rpm,
                     1);
            }
        } else
            d->popCharge = clampf(d->popCharge + 0.35f, 0, 3.4f);
    }
    d->prevDfco = d->dfco;
    d->dfcoEdge = d->dfcoEdge - h / 0.85f > 0 ? d->dfcoEdge - h / 0.85f : 0;
    if (d->prevOverride > 0.3f && !(override > 0.3f)) {
        float pFire = clampf((d->egt - 0.32f) / 0.30f, 0, 1) * (0.35f + 0.65f * rpmN);
        d->popCharge = clampf(d->popCharge + 1.1f + 0.5f * rpmN, 0, 2.6f);
        if (d->egt > 0.32f && d->popRefractory <= 0 && drand(d) < pFire) {
            d->popRefractory = 0.45f;
            fire(&d->evPop, clampf((d->egt - 0.25f) * 1.45f * (0.5f + 0.5f * rpmN), 0, 1), rpm, 1);
        }
    }
    d->prevOverride = override < 0 ? 0 : override;
    d->popCharge = d->popCharge - h * (0.22f + 0.55f * (1 - d->dfco));
    if (d->popCharge < 0) d->popCharge = 0;
}

/* one animation frame: build the params object (physics.js _writeParams) and hand it to every voice */
static void frame(Synth *s, int immediate) {
    Drive *d = &s->dr;
    const Preset *pr = &presets[s->selected];
    Params p;
    float h = immediate ? 0 : FRAME_DT, override, clutch, slip, cut, thr, rpmN;
    int neutral, limiterActive, k, gear = s->gear;
    memset(&p, 0, sizeof(p));
    d->evCut = d->evLash = d->evEngage = d->evBov = d->evShiftDone = d->evPop = 0;
    /* rpm: the controller sends ~50 Hz steps; smooth them like a crank would (30 ms) */
    if (!d->started || immediate) {
        d->rpm = d->prevRpm = s->rpm;
        d->started = 1;
    } else
        d->rpm += (s->rpm - d->rpm) * (1 - expf(-h / 0.03f));
    if (s->pending_shift_a) {
        shift_start(s, d, s->pending_shift_a, s->pending_shift_b);
        s->pending_shift_a = s->pending_shift_b = 0;
    }
    cut = shift_step(s, d, h, &override, &neutral, &clutch, &slip);
    thr = (override >= 0 ? override : clampf(s->load, 0, 1)) * (1 - cut);
    limiterActive = d->rpm >= pr->redline && s->throttle > 0.5f;
    if (limiterActive) thr = 0;
    if (s->turbo) turbo_step(d, pr, h, thr, limiterActive);
    else
        d->boost = 0;
    thermal_step(s, d, pr, h, thr, neutral, override, limiterActive);
    rpmN = clampf((d->rpm - pr->idle) / (pr->redline - pr->idle), 0, 1);
    p.now = s->audio_t;
    p.dt = h;
    p.rpm = d->rpm;
    p.f0 = d->rpm / 120;
    p.rpmNorm = rpmN;
    if (h > 0) {
        float dr = (d->rpm - d->prevRpm) / h;
        d->dRpm += (dr - d->dRpm) * (1 - expf(-h / 0.05f));
    }
    d->prevRpm = d->rpm;
    p.dRpm = d->dRpm;
    p.throttle = s->throttle;
    p.limiter = ((limiterActive || d->rpm > pr->redline - 120) && s->throttle > 0.5f) ? 1 : 0;
    d->load += (thr - d->load) * (h > 0 ? clampf(h * 12, 0, 1) : 1);
    p.load = clampf(d->load, 0, 1);
    {
        float tgt = (thr < 0.05f && d->rpm > pr->idle * 1.7f && s->load < -0.02f) ? 1.f : 0.f;
        d->overrun += (tgt - d->overrun) * (h > 0 ? clampf(h * 8, 0, 1) : 1);
    }
    p.overrun = clampf(d->overrun, 0, 1);
    p.torqueSign = s->load > 0.01f ? 1.f : s->load < -0.01f ? -1.f : 1.f;
    p.gear = gear;
    k = gear > pr->gears ? pr->gears : gear;
    p.gearRatio = gear > 0 ? pr->ratios[k - 1] * 3.73f : 0;
    p.speed = s->speed / 3.6f;
    p.wheelRpm = p.speed / (2 * 3.14159265f * 0.33f) * 60 * 3.73f; /* propshaft rpm */
    p.clutchSlip = slip;
    p.clutchEngaged = clutch;
    p.boost = d->boost;
    p.shifting = d->sphase != PH_NONE;
    p.phase = d->sphase;
    p.evCut = d->evCut;
    p.evLash = d->evLash;
    p.evEngage = d->evEngage;
    p.evBov = d->evBov;
    p.evShiftDone = d->evShiftDone;
    p.evPop = d->evPop;
    p.popIntensity = clampf(((d->egt - 0.45f) > 0 ? d->egt - 0.45f : 0) / 0.55f * clampf(d->popCharge, 0, 1) *
                                (0.12f + 0.88f * d->dfcoEdge) * (0.45f + 0.55f * d->cutRpmN),
                            0, 1) *
                     (d->rpm >= POP_MIN_RPM ? 1.f : 0.f);
    for (k = 0; k < NP; k++)
        if (s->voice[k].active) voice_update(&s->voice[k], &p, immediate);
    if (s->dump)
        fprintf(s->dump,
                "%.6f %d %d %d %.4f %.6f %.4f %.4f %.4f %.5f %.5f %.5f %.2f %d %.4f %.4f %.3f %.3f %.4f %.5f %d %d %d %.4f %.4f "
                "%.4f %.4f %.4f %.4f %.5f\n",
                p.now, s->selected, s->turbo, immediate, p.dt, p.rpm, p.f0, p.dRpm, p.throttle, p.load, p.overrun,
                p.torqueSign, 0.0, p.gear, p.gearRatio, p.speed, p.wheelRpm, p.clutchSlip, p.clutchEngaged, p.boost,
                p.shifting, p.phase, p.limiter, p.evCut, p.evLash, p.evEngage, p.evBov, p.evShiftDone, p.evPop,
                p.popIntensity);
}

static void quantum(Synth *s) {
    Busses b;
    float mix[QN], g[QN];
    int i, k, any = 0;
    double t = s->audio_t;
    float stale_t = (s->time - s->last_p > 1.5) ? 0.f : 1.f;
    sp_set(&s->vol, s->volume, 0.05f);
    sp_set(&s->mutegain, s->mute ? 0.f : 1.f, 0.1f);
    sp_set(&s->stale, stale_t, stale_t > s->stale.v ? 0.15f : 0.1f);
    sp_set(&s->quitg, s->quit ? 0.f : 1.f, 0.1f);
    for (i = 0; i < QN; i++) g[i] = sp_tick(&s->mutegain) * sp_tick(&s->stale) * sp_tick(&s->quitg);
    sp_set(&s->fx.master, s->volume, 0.05f);
    /* fully faded (muted / no data / quitting): skip the DSP entirely */
    if (g[QN - 1] < 1e-6f && g[0] < 1e-6f && (s->mute || !stale_t || s->quit)) {
        for (i = 0; i < QN; i++) s->ol[i] = s->or_[i] = 0;
        s->sleeping = 1;
        s->quanta++;
        s->audio_t = (double)(s->quanta * QN) / SR_HZ;
        return;
    }
    if (s->sleeping || s->quanta % FRAME_Q == 0) frame(s, 0);
    s->sleeping = 0;
    memset(&b, 0, sizeof(b));
    for (k = 0; k < NP; k++) {
        Voice *v = &s->voice[k];
        if (!v->active) continue;
        voice_render(v, t, &b);
        if (v->wtarget == 0 && v->weight <= 0) v->active = 0;
        any = 1;
    }
    (void)any;
    for (i = 0; i < QN; i++)
        mix[i] = 0.95f * (1.0f * b.exhaust[i] + 0.75f * b.intake[i] + 0.45f * b.transmission[i] + 0.40f * b.turbo[i] +
                          0.42f * b.transients[i] + 0.25f * b.sub[i]);
    fx_run(&s->fx, mix, s->ol, s->or_);
    for (i = 0; i < QN; i++) {
        /* volume is engine-sim.js's master gain (before the safety clipper, in fx_run); mute / watchdog / quit
           fades follow it, and 0.89 keeps the clipper's 0.99 ceiling under -1 dBFS */
        s->ol[i] *= g[i] * 0.89f;
        s->or_[i] *= g[i] * 0.89f;
    }
    s->quanta++;
    s->audio_t = (double)(s->quanta * QN) / SR_HZ;
}

static int16_t to16(float x) {
    float y = x * 32767.f;
    if (!(y == y)) y = 0;
    if (y > 32767.f) y = 32767.f;
    if (y < -32768.f) y = -32768.f;
    return (int16_t)lrintf(y);
}

void synth_render(Synth *s, int16_t *out, unsigned frames) {
    unsigned i = 0;
    if (s->audio_t == 0 && s->quanta == 0) frame(s, 1);
    while (i < frames) {
        unsigned m;
        if (s->opos >= QN) {
            quantum(s);
            s->opos = 0;
        }
        m = QN - s->opos;
        if (m > frames - i) m = frames - i;
        while (m--) {
            out[2 * i] = to16(s->ol[s->opos]);
            out[2 * i + 1] = to16(s->or_[s->opos]);
            s->opos++;
            i++;
        }
    }
    s->time += (double)frames / SR;
}

void demo_control(Synth *s, double t, double duration) {
    const Preset *p = &presets[s->selected];
    double u = t * 40 / duration;
    float r = p->idle, l = .05f, th = .03f;
    int g = 0;
    char line[160];
    if (u >= 3 && u < 9) {
        double z = fmod(u - 3, 3) / 3;
        r = p->idle + (p->redline * .72f - p->idle) * sinf((float)(z * 3.14159265));
        th = z < .45 ? .7f : .03f;
        l = z < .45 ? .6f : -.5f;
    } else if (u >= 9 && u < 25) {
        double z = (u - 9) / 3.2;
        g = 1 + (int)z;
        r = p->redline * (.38f + .47f * (float)(z - floor(z)));
        l = .9f;
        th = .94f;
    } else if (u >= 25 && u < 31) {
        g = 5 - (int)((u - 25) / 1.5);
        r = p->redline * (.75f - .07f * (float)(u - 25));
        l = -.8f;
        th = 0;
    } else if (u >= 31 && u < 36) {
        g = 6;
        r = 2100;
        l = .12f;
        th = .18f;
    } else if (u >= 36) {
        g = 2;
        r = p->idle + (2100 - p->idle) * (float)fmax(0, (39 - u) / 3);
        l = -.3f;
        th = .02f;
    }
    if (g != s->gear) {
        snprintf(line, sizeof(line), "S %d %d", s->gear, g);
        synth_control(s, line);
    }
    snprintf(line, sizeof(line), "P %.3f %.3f %.3f %d %.2f", r, l, th, g, g > 0 ? r * .007 * g : 0);
    synth_control(s, line);
    if (t > duration - .5) s->mute = 1;
}
