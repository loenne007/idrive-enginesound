/* synth.c - control protocol, the control-input model that stands in for Engine_Sim's drivetrain (physics.js /
 * shift.js), voice crossfades, master/mute/watchdog gains and the final limiter. */
#include "synth.h"
#include <stdlib.h>
#include <string.h>

#define FRAME_DT ((float)(FRAME_Q * QN) / SR_F)
#define POP_MIN_RPM 2000
/* The car is a ZF 8HP automatic: a shift is a short torque-management dip under a locked converter, not
 * Engine_Sim's manual sequence (ignition cut, clutch open, re-engage, clunk). Even shift.js's 'auto' type cuts
 * 55 % of the torque, which in this synth reads as the engine stopping; so: */
#define UP_DIP 0.15f     /* upshift: combustion load -15 % at most ... */
#define UP_PEAK 0.070f   /* ... reached smoothly 70 ms after the S line ... */
#define UP_END 0.180f    /* ... and fully recovered by 180 ms */
#define DN_BLIP 0.12f    /* downshift: a tiny blip, +0.12 load, 30 ms rise, 120 ms decay */
#define DN_RISE 0.030f
#define DN_TAU 0.120f
#define DN_END 0.600f
#define QUIET_AFTER 0.45f /* no bark from the recovery for this long after the envelope ends */
/* off-gas level correction, dB, per preset (i6 v8 v10 v12) for idle, light load and coasting: Engine_Sim's own
 * balance puts the overrun 12-17 dB under full load on the V engines; the owner wants every off-throttle state
 * about 8 dB under full load. Measured on fixed test points (see README). X offgas multiplies the result. */
static const float OFFGAS_DB[4][3] = {{0, -1, -3}, {-8.5f, -2, 5}, {2, -1, 7}, {0, 0, 8}};
static const char *X_NAMES[X_N] = {"offgas", "turbo", "bov", "pops", "bass"};
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
    for (k = 0; k < X_N; k++) s->x[k] = 1;
    sp_init(&s->offgas, 1);
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
    if (sscanf(line, "V %f %c", &v, &extra) == 1 && isfinite(v) && v >= 0 && v <= 2) {
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
    if (sscanf(line, "X %31s %f %c", name, &v, &extra) == 2 && isfinite(v)) {
        for (k = 0; k < X_N; k++)
            if (!strcmp(name, X_NAMES[k])) {
                s->x[k] = clampf(v, 0, 2);
                return 1;
            }
        return 0;
    }
    if (line[0] == 'Q' && sscanf(line + 1, " %c", &extra) != 1) {
        s->quit = 1;
        return 1;
    }
    return 0;
}

/* ---- automatic shift: a smooth torque envelope, nothing else ---- */
static void shift_start(Drive *d, int a, int b) {
    d->sactive = 1;
    d->st = 0;
    d->sdown = b < a;
}
static float smooth01(float x) {
    x = clampf(x, 0, 1);
    return x * x * (3 - 2 * x);
}
/* *mul scales the combustion load, *add is the rev-match blip */
static void shift_step(Drive *d, float h, float *mul, float *add) {
    *mul = 1;
    *add = 0;
    if (d->quiet > 0) d->quiet -= h;
    if (!d->sactive) return;
    d->st += h;
    if (!d->sdown) {
        float env = d->st < UP_PEAK ? smooth01(d->st / UP_PEAK) : 1 - smooth01((d->st - UP_PEAK) / (UP_END - UP_PEAK));
        *mul = 1 - UP_DIP * env;
        if (d->st >= UP_END) d->sactive = 0;
    } else {
        *add = DN_BLIP * (d->st < DN_RISE ? smooth01(d->st / DN_RISE) : expf(-(d->st - DN_RISE) / DN_TAU));
        if (d->st >= DN_END) d->sactive = 0;
    }
    d->quiet = QUIET_AFTER;
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
    if (thr > 0.45f && d->loadHeld >= 0.5f) /* only a sustained boost load, then a real lift, dumps the valve */
        d->bovArm = 1;
    else if (d->bovArm && thr < 0.12f && d->boost > 0.18f && !limiterActive) {
        d->bovArm = 0;
        d->boostDump = 0.12f;
        fire(&d->evBov, clampf(d->boost * (0.4f + 0.6f * 0.40f), 0.1f, 1), d->rpm, 0);
    }
}

/* physics.js _stepExhaustThermal: exhaust heat, fuel cut and the pops */
static void thermal_step(Synth *s, Drive *d, const Preset *pr, float h, float thr, int limiterActive) {
    float rpm = d->rpm, rpmN = clampf((rpm - pr->idle) / (pr->redline - pr->idle > 1 ? pr->redline - pr->idle : 1), 0, 1);
    float target = clampf(0.12f + 0.88f * powf(thr, 0.6f) * (0.35f + 0.65f * rpmN), 0, 1);
    float tau = target > d->egt ? 5.0f : 26.0f, pedal = s->throttle;
    int cutting;
    d->egt += (target - d->egt) * (h / tau < 1 ? h / tau : 1);
    /* DFCO: closed pedal, revs up, and the overrun has lasted a moment (engine braking reported by load < 0) */
    d->overrunHeld = s->load <= 0 ? d->overrunHeld + h : 0;
    cutting = pedal < 0.06f && rpm > pr->idle * 1.5f && d->overrunHeld >= 0.1f;
    /* pops need a genuine lift: the pedal held (> 0.3) for at least 0.5 s, then released */
    if (pedal > 0.3f)
        d->loadHeld += h;
    else if (d->loadHeld > 0) {
        d->heldBeforeLift = d->loadHeld;
        d->loadHeld = 0;
    }
    d->dfco = cutting;
    d->thrPeak = pedal > d->thrPeak - h / 0.7f ? pedal : d->thrPeak - h / 0.7f;
    d->popRefractory = d->popRefractory - h > 0 ? d->popRefractory - h : 0;
    d->limiterBark = d->limiterBark - h > 0 ? d->limiterBark - h : 0;
    if (limiterActive && !d->prevLimiter && d->egt > 0.5f && d->limiterBark <= 0 && s->x[X_POPS] > 0) {
        d->limiterBark = 0.34f;
        fire(&d->evPop, clampf(0.16f + 0.14f * drand(d), 0, 1), rpm, 1);
    }
    d->prevLimiter = limiterActive;
    if (d->dfco != d->prevDfco) {
        d->dfcoEdge = 1;
        if (d->dfco && d->heldBeforeLift >= 0.5f) {
            float snap = clampf(d->thrPeak, 0, 1), rev = powf(rpmN, 1.7f), luck = 0.55f + drand(d) * 0.95f, pFire;
            d->cutRpmN = rpmN;
            d->popCharge = clampf(d->popCharge + (0.5f + 2.1f * rev) * (0.4f + 0.6f * snap) * luck, 0, 3.4f);
            pFire = clampf((d->egt - 0.42f) / 0.34f, 0, 1) * (0.18f + 0.82f * rev) * (0.45f + 0.55f * snap);
            pFire = clampf(pFire * s->x[X_POPS], 0, 1); /* X pops: 0 never, 2 twice as often */
            if (d->egt > 0.42f && d->popRefractory <= 0 && drand(d) < pFire) {
                d->popRefractory = 0.45f;
                fire(&d->evPop,
                     clampf((d->egt - 0.35f) * 1.25f * (0.10f + 1.30f * rev) * (0.35f + 0.65f * snap) * luck, 0, 1), rpm,
                     1);
            }
            d->heldBeforeLift = 0;
        } else if (!d->dfco && d->popCharge > 0)
            d->popCharge = clampf(d->popCharge + 0.35f, 0, 3.4f);
    }
    d->prevDfco = d->dfco;
    d->dfcoEdge = d->dfcoEdge - h / 0.85f > 0 ? d->dfcoEdge - h / 0.85f : 0;
    d->popCharge = d->popCharge - h * (0.22f + 0.55f * (1 - d->dfco));
    if (d->popCharge < 0) d->popCharge = 0;
}

/* one animation frame: build the params object (physics.js _writeParams) and hand it to every voice */
static void frame(Synth *s, int immediate) {
    Drive *d = &s->dr;
    const Preset *pr = &presets[s->selected];
    Params p;
    float h = immediate ? 0 : FRAME_DT, mul, add, base, thr, rpmN;
    int limiterActive, k, gear = s->gear;
    memset(&p, 0, sizeof(p));
    d->evCut = d->evLash = d->evEngage = d->evBov = d->evShiftDone = d->evPop = 0;
    /* rpm follows P directly; the voices' own 20 ms setTargetAtTime (Engine_Sim) rounds the steps */
    if (!d->started || immediate) {
        d->rpm = d->prevRpm = s->rpm;
        d->started = 1;
    } else
        d->rpm = s->rpm;
    if (s->pending_shift_a) {
        shift_start(d, s->pending_shift_a, s->pending_shift_b);
        s->pending_shift_a = s->pending_shift_b = 0;
    }
    shift_step(d, h, &mul, &add);
    base = clampf(s->load, 0, 1);
    thr = clampf(base * mul + add, 0, 1);
    limiterActive = d->rpm >= pr->redline && s->throttle > 0.5f;
    if (limiterActive) thr = 0;
    if (limiterActive) base = 0;
    /* boost / blow-off and exhaust heat follow the driver's load, not the shift envelope */
    if (s->turbo) turbo_step(d, pr, h, base, limiterActive);
    else
        d->boost = 0;
    thermal_step(s, d, pr, h, base, limiterActive);
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
        float tgt = (base < 0.05f && d->rpm > pr->idle * 1.7f && s->load < -0.02f) ? 1.f : 0.f;
        d->overrun += (tgt - d->overrun) * (h > 0 ? clampf(h * 8, 0, 1) : 1);
    }
    p.overrun = clampf(d->overrun, 0, 1);
    p.torqueSign = s->load > 0.01f ? 1.f : s->load < -0.01f ? -1.f : 1.f;
    p.gear = gear;
    k = gear > pr->gears ? pr->gears : gear;
    p.gearRatio = gear > 0 ? pr->ratios[k - 1] * 3.73f : 0;
    p.speed = s->speed / 3.6f;
    p.wheelRpm = p.speed / (2 * 3.14159265f * 0.33f) * 60 * 3.73f; /* propshaft rpm */
    p.clutchSlip = 0;
    p.clutchEngaged = 1; /* converter locked through the shift */
    p.boost = d->boost;
    p.shifting = 0; /* no shift phase: nothing in the voices (surge, crackle gate, whine mute) reacts to it */
    p.phase = PH_NONE;
    p.noBark = d->quiet > 0;
    p.evCut = d->evCut;
    p.evLash = d->evLash;
    p.evEngage = d->evEngage;
    p.evBov = d->evBov;
    p.evShiftDone = d->evShiftDone;
    p.evPop = d->evPop;
    p.popIntensity = clampf(((d->egt - 0.45f) > 0 ? d->egt - 0.45f : 0) / 0.55f * clampf(d->popCharge, 0, 1) *
                                (0.12f + 0.88f * d->dfcoEdge) * (0.45f + 0.55f * d->cutRpmN),
                            0, 1) *
                     (d->rpm >= POP_MIN_RPM ? 1.f : 0.f) * s->x[X_POPS]; /* crackle stream: denser with X pops 2 */
    p.xturbo = s->x[X_TURBO];
    p.xbov = s->x[X_BOV];
    p.xpops = s->x[X_POPS];
    if (getenv("ES_VALIDATE")) p.xpops = p.xturbo = p.xbov = 1;
    { /* off-gas balance: blend of idle / light-load / coast corrections by the current state */
        int pi = (int)(pr - presets);
        float wf = clampf(p.load / 0.7f, 0, 1), wc, wi, db, gl;
        wf = wf * wf * (3 - 2 * wf);
        wc = p.overrun * (1 - wf);
        wi = (1 - wf - wc) * (1 - clampf(rpmN / 0.15f, 0, 1));
        db = OFFGAS_DB[pi][0] * wi + OFFGAS_DB[pi][1] * (1 - wf - wc - wi) + OFFGAS_DB[pi][2] * wc;
        gl = powf(10, db / 20) * (1 - wf) + wf;
        /* X offgas multiplies the off-throttle level (floored at -18 dB so the engine never vanishes) */
        gl *= 1 + (1 - wf) * ((s->x[X_OFFGAS] > 0.125f ? s->x[X_OFFGAS] : 0.125f) - 1);
        if (getenv("ES_VALIDATE")) gl = 1;
        sp_set(&s->offgas, gl, 0.08f);
    }
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
    sp_set(&s->fx.bass.g, 9 * (s->x[X_BASS] - 1), 0.05f);
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
    { /* ES_BUS=turbo|engine: calibration renders of the turbo layer alone / everything else */
        static int solo = -1;
        if (solo < 0) {
            const char *e = getenv("ES_BUS");
            solo = !e ? 0 : !strcmp(e, "turbo") ? 1 : 2;
        }
        if (solo)
            for (i = 0; i < QN; i++) {
                if (solo == 1)
                    b.exhaust[i] = b.intake[i] = b.transmission[i] = b.transients[i] = b.sub[i] = 0;
                else
                    b.turbo[i] = 0;
            }
    }
    for (i = 0; i < QN; i++)
        mix[i] = sp_tick(&s->offgas) * 0.95f * (1.0f * b.exhaust[i] + 0.75f * b.intake[i] + 0.45f * b.transmission[i] + 0.40f * b.turbo[i] +
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
        double z = (u - 9) / 3.2, ds = (z - floor(z)) * 3.2 * duration / 40; /* seconds into this gear */
        g = 1 + (int)z;
        /* gear 1 pulls from 38 %, later gears from 60 % of redline: an 8HP ratio step drops the revs ~30 % */
        r = p->redline * (g == 1 ? .38f + .47f * (float)(z - floor(z)) : .60f + .25f * (float)(z - floor(z)));
        /* like the car's telemetry, rpm ramps through the upshift in ~350 ms instead of stepping */
        if (g > 1 && ds < .35) {
            float w = (float)(ds / .35);
            w = w * w * (3 - 2 * w);
            r = p->redline * .85f + (r - p->redline * .85f) * w;
        }
        l = .9f;
        th = .94f;
    } else if (u >= 25 && u < 31) {
        double k = (u - 25) / 1.5, ds = (k - floor(k)) * 1.5 * duration / 40;
        float w = (float)(ds / .3), f;
        g = 5 - (int)k;
        w = w > 1 ? 1 : w * w * (3 - 2 * w);
        /* coasting: the revs sag in each gear and rise smoothly (~300 ms) on every downshift */
        f = k >= 1 ? 1 + .15f * w - .15f * (float)(k - floor(k)) : 1 - .15f * (float)(k - floor(k));
        r = p->redline * (.75f - .07f * (float)(u - 25)) * f;
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
