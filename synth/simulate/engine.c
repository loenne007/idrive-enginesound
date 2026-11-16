/* engine.c - one engine voice of the Pulse sound engine.
 *
 * A pulse-and-pipe model, cheap enough to run at well over 100x realtime and with no state that lags the
 * telemetry:
 *
 *   crank phase (double, never reset) with crank-speed ripple
 *   -> scheduled events (per cylinder: exhaust at its firing angle, intake 360 degrees later; fixed per-cylinder
 *      trims, cycle-to-cycle variation in strength/width, timing jitter on a fixed schedule)
 *   -> pressure pulses read from crank-angle tables (light / heavy load shapes blended by load), summed per path
 *   -> d/dphase (what an open pipe radiates) through a radiation knee, calibrated per path and load
 *   +  turbulent flow noise MULTIPLIED by the summed pulse pressure (the noise rides on every pulse: rasp)
 *   -> bank waveguides -> transit -> collector waveguide -> muffler chambers -> packing -> shock shaper
 *      (-> turbine darkening on turbo engines)
 *   +  intake (Helmholtz airbox driven by the intake pulses, induction noise riding on them), a crank-locked sub
 *      layer, a subtle turbo, a soft recirculation-valve whoosh, overrun pops through the same pipes
 *
 * Ideas and constants: the event source, pipe/muffler structure and airbox follow YigitSalihEmecen/Engine_Sim (MIT)
 * as the Waveguide engine re-implements it; crank-angle pulse tables and valve-timed intake/exhaust events after the
 * DasEtwas/enginesound and Antonio-R1 waveguide engines (MIT, Baldan et al. 2015); noise multiplied onto the
 * pressure signal after ange-yaghi/engine-sim (MIT). No code is copied from those three.
 *
 * Per sample there is no libm call and no allocation; coefficients and level maps update every CTRL samples.
 */
#include "dsp.h"
#include <string.h>

int es_solo;
static int calibrating;
static void voice_calibrate(Voice *v, const Preset *p);

/* ---- tables ---- */
#define PSPAN 330.f /* pulse table span, crank degrees after the valve opens (at pulse width 1) */
#define PTAB 512
#define PSCALE ((float)PTAB / PSPAN)
static float ptab[2][PTAB + 2]; /* exhaust blowdown pressure: light, heavy load (peak 1) */
static float itab[PTAB + 2];    /* intake: the induction pulse of a cylinder's intake stroke */
float sin_tab[SIN_N + 1];
static int tables_ready;

static float taper(float th) { /* smooth end over the last 60 degrees */
    float u = (th - (PSPAN - 60)) / 60;
    if (u <= 0) return 1;
    if (u >= 1) return 0;
    u = .5f + .5f * cosf(PI_F * u);
    return u * u;
}
static void tables_init(void) {
    int i, k;
    float mx[3] = {0, 0, 0};
    if (tables_ready) return;
    for (i = 0; i <= PTAB + 1; i++) {
        float th = (float)i / PSCALE, bump = th > 150 && th < 290 ? sinf(PI_F * (th - 150) / 140) : 0;
        bump *= bump;
        /* blowdown: fast rise (Gaussian onset), exponential fall; a small displacement bump from the exhaust
         * stroke. Under load the rise is harder and the gas keeps flowing longer. */
        ptab[0][i] = ((1 - expf(-(th / 15) * (th / 15))) * expf(-th / 66) + .13f * bump) * taper(th);
        ptab[1][i] = ((1 - expf(-(th / 7.5f) * (th / 7.5f))) * expf(-th / 100) + .09f * bump) * taper(th);
        itab[i] = (1 - expf(-(th / 26) * (th / 26))) * expf(-th / 58) * taper(th);
    }
    for (i = 0; i <= PTAB + 1; i++) {
        for (k = 0; k < 2; k++) mx[k] = fmaxf(mx[k], ptab[k][i]);
        mx[2] = fmaxf(mx[2], itab[i]);
    }
    for (i = 0; i <= PTAB + 1; i++) {
        for (k = 0; k < 2; k++) ptab[k][i] /= mx[k];
        itab[i] /= mx[2];
    }
    for (i = 0; i <= SIN_N; i++) sin_tab[i] = sinf(2 * PI_F * i / SIN_N);
    tables_ready = 1;
}
static inline float tab(const float *t, float idx) {
    int i = (int)idx;
    return t[i] + (idx - i) * (t[i + 1] - t[i]);
}

/* ---- pipes ---- */
static float lp_phase(const Biquad *f, float hz) { /* phase of the loop filter at hz */
    float t = 2 * PI_F * hz / SR, c1 = cosf(t), s1 = sinf(t), c2 = cosf(2 * t), s2 = sinf(2 * t);
    float nr = f->b0 + f->b1 * c1 + f->b2 * c2, ni = -(f->b1 * s1 + f->b2 * s2);
    float dr = 1 + f->a1 * c1 + f->a2 * c2, di = -(f->a1 * s1 + f->a2 * s2);
    return atan2f(ni, nr) - atan2f(di, dr);
}
/* quarter-wave pipe: negative open-end reflection, the round trip phase-tuned so the mode lands on c/4L */
static void pipe_init(Pipe *w, float len, float c, float refl, float damping) {
    float f = c / (4 * len), d;
    memset(w, 0, sizeof(*w));
    w->lpbase = clampf(f * 1.8f / damping, 150, 12000);
    w->refl = clampf(refl, 0, .92f);
    bq_lowpass(&w->lp, w->lpbase, .6f);
    d = (PI_F + lp_phase(&w->lp, f)) / (2 * PI_F * f) * SR;
    w->delay = clampf(d, 2, DLEN - 4);
}
static inline float pipe_step(Pipe *w, float x, float fb) {
    float y = bq(&w->lp, delay_read(&w->d, w->delay));
    delay_write(&w->d, x + fb * y);
    return .3f * x + .9f * y;
}

static int nbanks(const Preset *p) {
    return p->banks > 1 ? 2 : 1;
}
/* the pulse shape at table index idx and load blend h */
static inline float pulse_val(int kind, float idx, float h) {
    float a, b;
    if (kind) return tab(itab, idx);
    a = tab(ptab[0], idx);
    b = tab(ptab[1], idx);
    return a + h * (b - a);
}

void voice_init(Voice *v, const Preset *p, int index) {
    const int nb = nbanks(p);
    int i, j, k, o, L;
    float T[3], maxc = 0, prod = 1;
    tables_init();
    memset(v, 0, sizeof(*v));
    v->seed = 0x2545F491u ^ (uint32_t)(index * 2654435761u + 17);
    v->tseed = 0x1B873593u ^ (uint32_t)(index * 7919u + 11); /* turbo and whoosh: their own stream too */
    v->eseed = 0x3C6EF372u ^ (uint32_t)(index * 1103515245u + 12345); /* per event */
    v->dseed = 0xA54FF53Au ^ (uint32_t)(index * 22695477u + 1);      /* slow drifts */
    v->pseed = 0x68E31DA4u ^ (uint32_t)(index * 40503u + 3); /* pops: their own stream, the engine never changes */
    v->nout = nb + 1;
    /* events: exhaust at the firing angle, intake 360 degrees later; fixed per-cylinder trims */
    for (i = 0; i < p->n; i++) {
        int cyl = p->order[i], out = nb > 1 && cyl > p->split;
        float ang = p->angle[i] + rnd(&v->seed) * p->trim * 40, at = 1 + rnd(&v->seed) * p->trim * 3,
              wt = 1 + rnd(&v->seed) * p->trim * 2;
        if (out) {
            at *= 1 - p->bank_diff;
            wt *= 1 + p->bank_diff;
        }
        for (k = 0; k < 2; k++) {
            Event *e = &v->ev[v->nev++];
            e->angle = fmodf(ang + (k ? 360 : 0) + 720, 720) / 720;
            e->out = k ? nb : out;
            e->kind = k;
            e->amp_trim = at;
            e->w_trim = wt;
            e->cyl = i;
        }
    }
    /* sort by angle (insertion sort) */
    for (i = 1; i < v->nev; i++)
        for (j = i; j > 0 && v->ev[j].angle < v->ev[j - 1].angle; j--) {
            Event t = v->ev[j];
            v->ev[j] = v->ev[j - 1];
            v->ev[j - 1] = t;
        }
    v->evi = 0;
    v->next = v->ev[0].angle;
    /* calibration: one nominal cycle per output at five loads, gradient per unit phase through the radiation
     * knee (one-pole in the phase domain): scale each to 0.25 RMS; and the pressure RMS for the noise riding */
    {
        enum { N = 4096 };
        static float cyc[N];
        float kk = 1 - expf(-2 * PI_F * p->knee / N);
        for (o = 0; o < v->nout; o++)
            for (L = 0; L <= 4; L++) {
                float h = L / 4.f, lp = 0, ss = 0, sp = 0;
                int pass;
                memset(cyc, 0, sizeof(cyc));
                for (k = 0; k < v->nev; k++) {
                    Event *e = &v->ev[k];
                    float rate = 1 / (p->pulse_w * e->w_trim);
                    if (e->out != o) continue;
                    for (j = 0; j < N; j++) {
                        float d = (float)j / N - e->angle, idx;
                        if (d < 0) d += 1;
                        idx = d * 720 * rate * PSCALE;
                        if (idx < PTAB) cyc[j] += e->amp_trim * pulse_val(e->kind, idx, h);
                    }
                }
                for (pass = 0; pass < 2; pass++)
                    for (j = 0; j < N; j++) {
                        float g = (cyc[j] - cyc[(j + N - 1) % N]) * N;
                        lp += kk * (g - lp);
                        if (pass) {
                            ss += lp * lp;
                            sp += cyc[j] * cyc[j];
                        }
                    }
                ss = sqrtf(ss / N);
                sp = sqrtf(sp / N);
                v->gains[o][L] = .25f / (ss > 0 ? ss : 1);
                v->srms[o][L] = 1 / (sp > 0 ? sp : 1);
            }
    }
    /* exhaust: bank waveguides, transit, collector, muffler */
    for (i = 0; i < nb; i++) {
        pipe_init(&v->bank[i], p->bank_len[i], p->c_hot, p->refl, p->damping);
        v->transit_d[i] = clampf(p->bank_len[i] / p->c_hot * SR, 1, DLEN - 4);
    }
    pipe_init(&v->coll, p->collector, p->c_hot, p->refl * .7f, p->damping * 1.5f);
    for (i = 0; i < 3; i++) {
        /* chamber round trips, nudged apart so their comb peaks do not coincide */
        T[i] = 2 * p->muffler[i] / p->c_hot * (i == 1 ? 1.031f : i == 2 ? .967f : 1);
        v->muf_d[i] = clampf(T[i] * SR, 1, DLEN - 4);
        v->muf_g[i] = clampf(.55f + .30f * p->refl, .05f, .9f) * powf(.92f, (float)i);
        prod *= 1 + v->muf_g[i];
        if (p->muffler[i] > maxc) maxc = p->muffler[i];
    }
    v->muf_trim = 1 / fmaxf(1, prod * .72f);
    v->packing_base = clampf(p->c_hot / (4 * maxc) * 7, 500, 11000);
    bq_highpass(&v->nl_dc, 22, .7f);
    bq_lowpass(&v->tb_lp, 6500, .7f);
    bq_lowpass(&v->in_direct, p->helmholtz * 4, .7f);
    bq_bandpass(&v->in_helm, p->helmholtz, p->intake_q);
    /* sub layer: half the firing order, octave-shifted into the 38..85 Hz chest band at mid revs */
    {
        float order = p->sub_order > 0 ? p->sub_order : fmaxf(.5f, p->n / 4.f), fmid = (p->idle + p->redline) / 2 / 120;
        while (order * fmid < 38 && order < 8) order *= 2;
        while (order * fmid > 85 && order > .5f) order /= 2;
        v->sub_order = order;
    }
    v->thr_lo = 1;
    v->bov_t = 1;
    /* low foundation: 32 Hz high-pass, 115 Hz 4th-order low-pass (nothing reaches into the firing orders above) */
    bq_highpass(&v->fo_hp, 32, .7f);
    bq_lowpass(&v->fo_lp1, 115, .5412f);
    bq_lowpass(&v->fo_lp2, 115, 1.3066f);
    for (i = 0; i < 4; i++) v->fo_amp[i] = v->fo_tamp[i] = 1;
    bq_bandpass(&v->tick_bp, 4300, 1.3f);
    bq_lowshelf(&v->trim, 120, p->bass_trim);
    v->nl_mk = v->nl_drive = 1;
    v->weight = 0;
    voice_calibrate(v, p);
}

/* bandpassed white noise RMS normaliser: equivalent noise bandwidth of the 0 dB-peak RBJ bandpass ~ pi/2 fc/Q */
static float bp_norm(float hz, float q) {
    return 1 / sqrtf(.3333f * (1.5708f * hz / q) / (SR * .5f));
}

/* ---------------------------------------------------------------------------------------------------------------
 * Control rate
 * ------------------------------------------------------------------------------------------------------------- */
#define OFFGAS_AMP .60f /* off the throttle (idle, coast, light load): -4.4 dB against full load at offgas 1 */
#define TURBO_K .0009f   /* turbo at full boost: ~-40 dB under the engine, a faint whistle (set by measurement) */
#define BOV_K .0125f    /* recirculation whoosh peak: ~-16 dB under the full-load engine (CAL_LEVEL) */
#ifndef POP_K
#define POP_K 3.6f
#endif
#define POP_REF .55f     /* the pops' level against the overrun level they sit on */
#define NL_IN .3f
#define LVL_RPM 8.5f     /* full-load level rise from idle to redline, dB */
#define TICK_K .005f    /* injector/valvetrain ticks: faint, irregular */
#define CAL_LEVEL .05f  /* calibrated engine RMS before the level map */       /* shaper input staging */

static void voice_control(Voice *v, const Preset *p, const Ctl *c) {
    const float dt = (float)CTRL / SR, rpm = c->rpm, rpm_n = clampf(rpm / p->redline, 0, 1);
    const float pl = clampf(c->load, 0, 1), thr = c->throttle, *X = c->x, nb = (float)nbanks(p);
    float x, xl, over, off, s;
    int i;
    v->rpm_n = rpm_n;
    v->f0 = rpm / 120;
    /* tip-in: a throttle stab from a low-throttle state gives a short edge; the pedal is the driver's, so a gear
     * shift (which never moves the pedal) cannot bark */
    v->thr_lo = fminf(thr, v->thr_lo + dt / .4f);
    v->bark *= expf(-dt / .15f);
    if (v->thr_lo < .15f) v->bark = fmaxf(v->bark, clampf((thr - v->prev_thr) / dt / 8, 0, 1));
    v->prev_thr = thr;
    /* Lift-off / tip-in, as a real engine does it: the TIMBRE follows the load fast (two one-poles, 30 ms down:
     * 90 % in ~115 ms, the edge and rasp gone; 10 ms up), the LOUDNESS settles a little later (below). Both are
     * critically damped (monotonic, no overshoot, no dip-and-recover). The timbre path is an offset on top of the
     * shift-dipped load, so a shift's torque dip keeps its own shape. */
    {
        float raw = clampf(c->lvl_load, 0, 1), k = tau_k(raw > v->x_glide ? .010f : .030f, SR / (float)CTRL);
        v->x_g1 += (raw - v->x_g1) * k;
        v->x_glide += (v->x_g1 - v->x_glide) * k;
        x = clampf(pl + (v->x_glide - raw) + .25f * v->bark, 0, 1);
    }
    v->x = x;
    xl = clampf(clampf(c->lvl_load, 0, 1) + .25f * v->bark, 0, 1);
    v->h = x;
    /* overrun (fuel cut): its character comes in within ~50 ms; the firing returns over ~120 ms when the fuel comes
     * back near idle, in ~25 ms on a tip-in */
    over = (c->load < -.05f && rpm > p->idle + 250) ? 1.f : 0.f;
    v->over += (over - v->over) * tau_k(over > v->over ? .05f : thr > .2f ? .025f : .12f, SR / (float)CTRL);
    /* idle: the idle-speed control hunts a little (+-1.5 % around idle, ~0.3-1.5 Hz), only near idle and off load */
    {
        float near = clampf(1 - (rpm - p->idle) / 400, 0, 1) * (1 - x);
        v->idle_t -= dt;
        if (v->idle_t <= 0) {
            v->idle_tgt = gauss(&v->dseed) * .011f;
            v->idle_t = .35f + .5f * (.5f + .5f * rnd(&v->dseed));
        }
        v->idle_w += (v->idle_tgt - v->idle_w) * tau_k(.18f, SR / (float)CTRL);
        v->idle_mod = 1 + near * v->idle_w;
    }
    /* cylinders differ, and the differences drift: each cylinder's strength (+-2 %, more at low revs) and timing
     * (+-0.35 deg) wander towards new targets every ~1-2.5 s with 1.2 s smoothing (not white jitter: it persists) */
    {
        const float k = tau_k(1.2f, SR / (float)CTRL);
        v->cd_t -= dt;
        if (v->cd_t <= 0) {
            for (i = 0; i < p->n; i++) {
                v->cd_tamp[i] = clampf(gauss(&v->dseed) * .02f * (1 + .8f * (1 - rpm_n)), -.06f, .06f);
                v->cd_tang[i] = clampf(gauss(&v->dseed) * .35f, -1, 1) / 720;
            }
            v->cd_t = 1 + 1.5f * (.5f + .5f * rnd(&v->dseed));
        }
        for (i = 0; i < p->n; i++) {
            v->cd_amp[i] += (v->cd_tamp[i] - v->cd_amp[i]) * k;
            v->cd_ang[i] += (v->cd_tang[i] - v->cd_ang[i]) * k;
        }
    }
    /* the low foundation's orders drift: amplitude 0.75-1.25 and a phase that keeps sliding (0.1-0.6 cycles/s, sign and
     * speed changing every 1-2.5 s): the paths through the mounts and the body are not locked to the exhaust's own
     * low orders, so the two never sit in a fixed (rpm-dependent) constructive or destructive sum */
    {
        const float k = tau_k(.9f, SR / (float)CTRL);
        v->fo_t -= dt;
        if (v->fo_t <= 0) {
            for (i = 0; i < 4; i++) {
                v->fo_tamp[i] = clampf(1 + .15f * gauss(&v->dseed), .75f, 1.25f);
                v->fo_tph[i] = (rnd(&v->dseed) > 0 ? 1 : -1) * (.1f + .5f * (.5f + .5f * rnd(&v->dseed)));
            }
            v->fo_t = 1 + 1.5f * (.5f + .5f * rnd(&v->dseed));
        }
        for (i = 0; i < 4; i++) {
            v->fo_amp[i] += (v->fo_tamp[i] - v->fo_amp[i]) * k;
            v->fo_vel[i] += (v->fo_tph[i] - v->fo_vel[i]) * k;
            v->fo_ph[i] += v->fo_vel[i] * dt;
            if (v->fo_ph[i] > 1) v->fo_ph[i] -= 1;
            if (v->fo_ph[i] < 0) v->fo_ph[i] += 1;
        }
    }
    /* crank-speed ripple per firing ~ T (4 pi / n) / (J w^2), capped */
    {
        float w = fmaxf(40, rpm * PI_F / 30), T = p->torque * (.12f + .88f * x);
        v->ripple = fminf(.12f, .3f * T * (4 * PI_F / p->n) / (p->inertia * w * w));
    }
    /* radiation knee; motoring pulses on the overrun are rounder (the knee drops by a third) */
    v->lpk = 1 - expf(-2 * PI_F * fminf(p->knee * (1 - .33f * v->over) * v->f0, .45f * SR) / SR);
    /* levels. Off the gas sits OFFGAS_AMP (x the offgas setting) under full load; rpm adds ~4 dB to redline */
    off = clampf(OFFGAS_AMP * powf(10, p->offgas_trim / 20) * X[X_OFFGAS], 0, 1);
    /* full load rises LVL_RPM dB from idle to redline; the calibration takes the pipes' own colour out first */
    {
        float u = clampf((rpm - p->idle) / (p->redline - p->idle), 0, 1) * (NCAL - 1), cl, ch, cm;
        int k = u >= NCAL - 1 ? NCAL - 2 : (int)u;
        u -= k;
        cl = v->corr[0][k] + u * (v->corr[0][k + 1] - v->corr[0][k]);
        /* on the overrun the motoring timbre has its own calibration */
        cl += v->over * (v->corr[2][k] + u * (v->corr[2][k + 1] - v->corr[2][k]) - cl);
        ch = v->corr[1][k] + u * (v->corr[1][k + 1] - v->corr[1][k]);
        cm = v->corr[3][k] + u * (v->corr[3][k + 1] - v->corr[3][k]);
        /* the foundation's gain and the total's renormalisation (the calibration's, at full load) */
        {
            float fc = v->fcal[k] + u * (v->fcal[k + 1] - v->fcal[k]), fn = v->fnorm[k] + u * (v->fnorm[k + 1] - v->fnorm[k]);
            float kk = v->fcal_s > 0 ? tau_k(.01f, SR / (float)CTRL) : 1; /* (no pipe to build up: quick) */
            v->fcal_s += (fc - v->fcal_s) * kk;
            v->fnorm_s += (fn - v->fnorm_s) * kk;
        }
        v->lvl_full = powf(10, LVL_RPM / 20 * (u + k) / (NCAL - 1)) * c->level;
        /* the correction follows the rpm in ~40 ms: after a fast rpm drop the pipes' low resonances need that long to
         * build up, and an instant correction would leave a hole */
        {
            float k = v->corr_s > 0 ? tau_k(.04f, SR / (float)CTRL) : 1; /* (corr_s doubles as "started") */
            v->corr_l += (cl - v->corr_l) * k;
            v->corr_h += (ch - v->corr_h) * k;
            v->corr_m += (cm - v->corr_m) * k;
            /* the load blend follows the timbre's lift/tip-in path, but not a shift's torque dip (that would swell
             * the level by pulling in the light-load correction) */
            /* in dB, through the mid-load point: a 0.3-load timbre is much closer to full load than to no load */
            float xc = clampf(v->x_glide + .25f * v->bark, 0, 1);
            v->corr_s = xc < .4f ? v->corr_l * powf(v->corr_m / v->corr_l, xc / .4f)
                                 : v->corr_m * powf(v->corr_h / v->corr_m, (xc - .4f) / .6f);
        }
        /* the loudness settles after the timbre: two one-poles, 45 ms down (90 % in ~175 ms, settled by 300 ms),
         * 12 ms up (back on load in ~50 ms) */
        {
            float k = tau_k(xl > v->xl_s2 ? .012f : .045f, SR / (float)CTRL);
            v->xl_s += (xl - v->xl_s) * k;
            v->xl_s2 += (v->xl_s - v->xl_s2) * k;
        }
        float map = off + (1 - off) * powf(v->xl_s2, 1.5f), fl = .35f + .65f * v->xl_s2;
        /* the total (engine + foundation at this load) back to the level map */
        float nrm = 1 / sqrtf(1 + v->fnorm_s * v->fnorm_s * fl * fl);
        v->lvl = v->lvl_full * map * v->corr_s * nrm;
        /* the foundation grows with load (structure-borne: the torque the mounts pass on), a third of it left on
         * the overrun and at idle; it follows the loudness path, so lifts and tip-ins stay smooth */
        v->fo_g = v->lvl_full * map * v->fcal_s * fl * nrm;
        /* injector/valvetrain ticks: heard at idle and light load, drowned under load */
        v->tick_g = TICK_K * (1 - .8f * v->xl_s2) * (1 - .6f * rpm_n) * map; /* (X offgas 0 silences them too) */
        /* pops keep their own absolute level whatever the off-gas level and its glide */
        v->pop_norm = POP_REF / fmaxf(map, .05f);
    }
    /* the turbulent noise riding on the pulses: more with load and revs, half on the overrun */
    /* at high revs under load the rasp grows (the pipe "rips" rather than whines) */
    {
        /* (the rasp takes half of a shift's torque dip: the pulses soften, the level stays) */
        float xr = .5f * (x + clampf(v->x_glide + .25f * v->bark, 0, 1));
        v->rasp_g = .4f * p->rasp * (.3f + .7f * xr) * (.4f + .5f * rpm_n + .45f * rpm_n * rpm_n * xr) * (1 - .6f * v->over);
    }
    /* the band opens with load: dark at cruise and part load (no hiss), the full rasp under heavy load */
    v->rasp_fc = clampf(p->rasp_hz * (.3f + .7f * x * x) * (.7f + .5f * rpm_n), 200, 9000);
    /* broadband jet noise: lowpass at rasp_fc (a one-pole highpass at ~200 Hz in the sample loop: below that it would only feed the pipes' low modes) */
    for (i = 0; i < (int)nb; i++) bq_lowpass(&v->nz_bp[i], v->rasp_fc * (i ? 1.13f : 1), .6f);
    v->nz_norm = 1 / sqrtf(.3333f * 1.15f * v->rasp_fc / (SR * .5f));
    /* pipes: hotter, harder pulses keep more HF in the loops */
    for (i = 0; i < (int)nb; i++) bq_lowpass(&v->bank[i].lp, v->bank[i].lpbase * (1 + .35f * x), .6f);
    bq_lowpass(&v->coll.lp, v->coll.lpbase * (1 + .35f * x), .6f);
    for (i = 0; i < 3; i++)
        v->muf_g[i] = clampf(.55f + .30f * p->refl, .05f, .9f) * powf(.92f, (float)i) * (1 - .12f * x) * .6f;
    bq_lowpass(&v->packing, v->packing_base * (1 + .3f * x + .15f * rpm_n), .6f);
    /* shock steepening: drive rises with load^1.5 and revs; a little on the overrun */
    {
        /* (no boost term: it put level the calibration does not know and unwound unevenly after a lift) */
        float a = powf(x, 1.5f) * (.55f + .45f * rpm_n) + .08f * v->over;
        v->nl_drive = 1 + 3.2f * p->shaper * clampf(a, 0, 1);
        v->nl_mk = powf(v->nl_drive, -.72f) / NL_IN;
    }
    /* intake: airbox resonance and induction roar with the throttle */
    {
        float flow = clampf(thr * (.25f + .75f * rpm_n), 0, 1), ex = powf(fmaxf(x, 1e-3f), .7f);
        v->in_hg = (.15f + .85f * ex) * (.4f + .6f * rpm_n);
        v->in_dg = .3f * (.2f + .8f * ex);
        v->in_ng = p->induction * (.05f + .95f * powf(flow, 1.2f)) * .16f;
        s = clampf(p->helmholtz * (3 + 10 * flow), 100, 9000);
        bq_bandpass(&v->in_nbp, s, 1.1f);
        v->in_norm = bp_norm(s, 1.1f);
    }

    /* turbo: shaft spool lags the boost demand (0.35 s up, 0.4-0.8 s down); whistle and air rush at
     * ~-27 dB under the engine at full boost, nothing audible at cruise */
    {
        int on = c->turbo;
        /* boost only under real load: nothing below ~0.55, full at 1 */
        float demand = on ? powf(clampf((x - .55f) / .45f, 0, 1), 1.3f) * clampf((rpm - 1400) / 2200, 0, 1) : 0;
        float tau = demand > v->spool ? .35f : (thr < .12f ? .4f : .8f);
        v->spool += (demand - v->spool) * tau_k(tau, SR / (float)CTRL);
        v->spool = clean(v->spool);
        v->tb_hz = 1400 + 3200 * v->spool;
        bq_bandpass(&v->tb_band, v->tb_hz, 4);
        v->tb_bnorm = bp_norm(v->tb_hz, 4);
        bq_bandpass(&v->tb_air, 2600 + 1600 * v->spool, .9f);
        v->tb_anorm = bp_norm(2600 + 1600 * v->spool, .9f);
        v->tb_lvl = TURBO_K * v->spool * v->spool * X[X_TURBO];
        /* the turbine darkens the exhaust with the gas flowing through it: spool AND load (on a lift the flow stops
         * at once; a shelf that waited for the shaft to spool down left a dip-and-recover) */
        bq_highshelf(&v->turbine, 900, on ? -2.5f - 3.5f * v->spool * v->x_glide : 0);
        /* recirculation valve (BMW: no blow-off to atmosphere): a short soft whoosh when the pedal is released
         * fast at boost. Armed by boost, fired by the pedal; a gear shift never moves the pedal. */
        v->thr_slow += (thr - v->thr_slow) * tau_k(.12f, SR / (float)CTRL);
        if (thr > .45f && v->spool > .3f) v->bov_armed = 1;
        if (v->bov_armed && thr < .15f && v->thr_slow - thr > .25f && !c->shift_guard) {
            v->bov_peak = BOV_K * powf(v->spool, 1.5f) * X[X_BOV];
            v->bov_t = 0;
            v->bov_armed = 0;
        }
        if (v->bov_t < .4f) {
            float u = v->bov_t / .16f;
            bq_bandpass(&v->bov_bp, 2700 - 1300 * fminf(u, 1), 1.2f);
            v->bov_norm = bp_norm(2700 - 1300 * fminf(u, 1), 1.2f);
            v->bov_t += dt;
        } else
            v->bov_peak = 0;
        v->bov_tgt = v->bov_t < .03f ? v->bov_peak : 0;
    }

    /* overrun pops: a genuine lift-off after sustained load (0.6 s average) on a hot engine opens a window of
     * Poisson events; X pops 0 = none, 1 = subtle, 2 = loud sport exhaust, more often and longer */
    if (p->crackle > 0) {
        float pops = X[X_POPS];
        v->load_avg += (pl - v->load_avg) * tau_k(.6f, SR / (float)CTRL);
        if (thr > .3f && pl > .3f) v->pop_armed = 1;
        if (v->pop_armed && thr < .08f && c->load < .05f && !c->shift_guard) {
            if (v->load_avg > .35f && rpm > 2000 && pops > .02f) {
                float len = pops <= 1 ? .45f + .4f * pops : .85f + .9f * (pops - 1);
                v->pop_t = len * (.8f + .4f * (.5f + .5f * rnd(&v->pseed))) * (.5f + .5f * p->crackle);
                v->pop_rate = p->crackle * (pops <= 1 ? 4 * pops : 4 + 7 * (pops - 1));
                v->pop_amp = POP_K * (pops <= 1 ? .45f * pops : .45f + .55f * (pops - 1));
            }
            v->pop_armed = 0;
        }
        if (thr > .2f || rpm < 1400) v->pop_t = 0;
    }
}

/* ---------------------------------------------------------------------------------------------------------------
 * Per sample
 * ------------------------------------------------------------------------------------------------------------- */
float voice_step(Voice *v, const Preset *p, const Ctl *c) {
    const int nb = nbanks(p);
    float acc[3] = {0, 0, 0}, accr[3] = {0, 0, 0}, src[3], ex, in, found, tick, tb = 0, bov = 0, pop = 0, out;
    double dph;
    int i, o;

    if (!(v->tick % CTRL)) voice_control(v, p, c);
    v->tick++;

    /* ---- crank and events ---- */
    {
        const float x = v->x, low = 1 - v->rpm_n;
        const float cov = p->cov * (.7f + 2.3f * (1 - x) * (1 - x)) * (1 + low) * (1 + 1.5f * p->over_irr * v->over);
        /* timing jitter: more at idle; and at high revs under load, where it smears the high orders into a rasp */
        const float spread = p->jitter * (.6f + 1.4f * (1 - x)) * (1 + .8f * low) * (1 + 1.5f * v->rpm_n * x) / 720;
        float ddeg, h = v->h;
        int guard = 0;
        dph = c->rpm * v->idle_mod / (120. * SR) * (1 + v->v);
        /* compression drag + a leak standing in for the idle-speed control */
        v->v -= (float)((v->ripple * p->n + 6 * v->v) * (c->rpm / (120. * SR)));
        v->v = clampf(v->v, -.2f, .2f);
        v->crank += dph;
        while (v->crank >= v->next && guard++ < MAXEV) {
            Event *e = &v->ev[v->evi];
            if (v->np < MAXP) {
                Pulse *q = &v->pool[v->np++];
                float g = gauss(&v->eseed);
                q->kind = e->kind;
                q->out = e->out;
                q->rate = 1 / (p->pulse_w * e->w_trim * (1 + .35f * cov * gauss(&v->eseed)));
                q->A = e->amp_trim * (1 + v->cd_amp[e->cyl]) * fmaxf(.2f, 1 + g * cov * (e->kind ? .5f : 1));
                q->r = clampf(1 + .45f * gauss(&v->eseed), .3f, 2);
                /* an intake event: the injector / valvetrain tick that goes with it */
                if (e->kind && rnd(&v->eseed) > -.5f) v->tick_env += v->tick_g * (.75f + .45f * rnd(&v->eseed));
                /* motoring on the overrun: now and then a weak, lazy pulse (a V8 burbles, a V12 hardly) */
                if (!e->kind && v->over > .5f && .5f + .5f * rnd(&v->eseed) < .07f * p->over_irr) q->A *= .45f;
                q->age = (float)((v->crank - v->next) * 720) * q->rate;
                if (!e->kind) v->v += v->ripple * q->A;
            }
            if (++v->evi >= v->nev) {
                v->evi = 0;
                v->cycle += 1;
            }
            v->next = v->cycle + v->ev[v->evi].angle + v->cd_ang[v->ev[v->evi].cyl] +
                      clampf(gauss(&v->eseed) * spread, -.006f, .006f);
        }
        /* sum the live pulses */
        ddeg = (float)(dph * 720);
        for (i = 0; i < v->np;) {
            Pulse *q = &v->pool[i];
            float idx = q->age * PSCALE;
            if (idx >= PTAB) {
                v->pool[i] = v->pool[--v->np];
                continue;
            }
            {
                float pv = q->A * pulse_val(q->kind, idx, h);
                acc[q->out] += pv;
                accr[q->out] += pv * q->r;
            }
            q->age += ddeg * q->rate;
            i++;
        }
        /* pressure gradient per unit phase, radiation knee, calibration */
        {
            float invv = dph > 1e-9 ? (float)(1 / dph) : 0, lpk = fmaxf(v->lpk, 1e-3f), gi = h * 4, gf;
            int g0 = gi >= 3 ? 3 : (int)gi;
            gf = gi - g0;
            for (o = 0; o < v->nout; o++) {
                float grad = (acc[o] - v->prev[o]) * invv;
                float gain = v->gains[o][g0] + (v->gains[o][g0 + 1] - v->gains[o][g0]) * gf;
                float sr = v->srms[o][g0] + (v->srms[o][g0 + 1] - v->srms[o][g0]) * gf;
                v->prev[o] = acc[o];
                v->lp[o] = clean(v->lp[o] + lpk * (grad - v->lp[o]));
                src[o] = v->lp[o] * gain;
                acc[o] *= sr; /* normalised pressure: RMS 1 */
                accr[o] *= sr;
            }
        }
    }

    /* ---- overrun pops: unburnt fuel lighting in the hot pipe ---- */
    if (v->pop_t > 0 || v->pop_env > 1e-5f) {
        if (v->pop_t > 0) {
            v->pop_t -= 1.f / SR;
            if (.5f + .5f * rnd(&v->pseed) < v->pop_rate / SR) {
                float size = .5f + .5f * rnd(&v->pseed);
                size = .25f + size * size * size * 1.6f; /* mostly small, now and then a bang */
                v->pop_env += size * v->pop_amp;
                v->pop_dec = 1 - 1.f / ((.006f + .022f * (.5f + .5f * rnd(&v->pseed))) * SR);
                v->pop_bank = nb > 1 && rnd(&v->pseed) > 0;
            }
        }
        v->pop_env = clean(v->pop_env * v->pop_dec);
        /* ~1 ms rise through two one-poles (a muffled thump more than a crack); noisy, positive-biased */
        v->pop_lp = clean(v->pop_lp + .18f * ((rnd(&v->pseed) + .6f) * v->pop_env - v->pop_lp));
        v->pop_lp2 = clean(v->pop_lp2 + .12f * (v->pop_lp - v->pop_lp2));
        /* a thump, not sub-bass: high-passed (~150 Hz) before the pipes' low modes */
        v->pop_hp += .0194f * (v->pop_lp2 - v->pop_hp);
        pop = (v->pop_lp2 - v->pop_hp) * v->pop_norm;
    }

    /* ---- exhaust ---- */
    {
        float sum = 0, y, fb;
        int b;
        for (b = 0; b < nb; b++) {
            Pipe *w = &v->bank[b];
            /* turbulent flow noise riding on the pressure pulses of this path */
            float nz = bq(&v->nz_bp[b], rnd(&v->seed)), ride, t;
            v->nz_lo[b] += .026f * (nz - v->nz_lo[b]); /* ~200 Hz */
            ride = (nz - v->nz_lo[b]) * v->nz_norm * accr[b] * v->rasp_g; /* each combustion as rough as it is */
            float in0 = (es_solo == 4 ? 0 : src[b]) + (es_solo == 5 ? 0 : ride) + (b == v->pop_bank ? pop : 0);
            y = pipe_step(w, in0, -clampf(w->refl * (.92f + .08f * v->x), 0, .92f));
            t = delay_read(&v->transit[b], v->transit_d[b]);
            delay_write(&v->transit[b], y);
            sum += t;
        }
        sum *= nb > 1 ? .70710678f : 1;
        fb = -clampf(v->coll.refl * (.92f + .08f * v->x), 0, .92f);
        ex = pipe_step(&v->coll, sum, fb);
        for (b = 0; b < 3; b++) {
            float d = delay_read(&v->muf[b], v->muf_d[b]);
            delay_write(&v->muf[b], ex);
            ex += v->muf_g[b] * d;
        }
        ex = bq(&v->packing, ex) * v->muf_trim;
        {
            /* staged so a typical pulse peak sits around the knee at full drive: small signals gain drive^0.28 */
            float u = NL_IN * v->nl_drive * ex;
            u = u >= 0 ? 1.05f * tanh_f(u / 1.05f) : 1.36f * tanh_f(u / 1.36f);
            ex = bq(&v->nl_dc, u) * v->nl_mk;
        }
        ex = bq(&v->turbine, ex);
    }

    /* ---- intake ---- */
    {
        float w = rnd(&v->seed), nz, h;
        v->in_pink += .12f * (w - v->in_pink);
        nz = .6f * v->in_pink + .4f * w;
        h = bq(&v->in_helm, src[nb]);
        in = (es_solo == 6 ? 0 : h * v->in_hg + bq(&v->in_direct, src[nb]) * v->in_dg) +
             (es_solo == 7 ? 0 : bq(&v->in_nbp, nz) * v->in_norm * v->in_ng * (.35f + .65f * acc[nb]));
        in *= p->intake_level * (c->turbo ? .6f : 1);
    }

    /* ---- low foundation: engine orders 0.5/1/1.5/2 (crank-locked, drifting) + structure-borne rumble riding on
     * the firing pressure, all in 32-115 Hz, so the low end stays at any revs ---- */
    {
        static const float w[4] = {.45f, 1, .75f, .5f};
        float ton = 0, rum, pr = nb > 1 ? .5f * (acc[0] + acc[1]) : acc[0];
        int k;
        for (k = 0; k < 4; k++) {
            double ph = v->crank * (k + 1) + v->fo_ph[k];
            ton += w[k] * v->fo_amp[k] * sin_cyc((float)(ph - floor(ph)));
        }
        rum = 10 * rnd(&v->seed) * (.55f + .45f * pr);
        found = bq(&v->fo_lp2, bq(&v->fo_lp1, bq(&v->fo_hp, .55f * ton + rum)));
    }
    /* ---- ticks: short 4 kHz bursts ---- */
    v->tick_env = clean(v->tick_env * .986f);
    {
        float tn = rnd(&v->seed); /* drawn every sample: the noise stream never depends on event timing */
        tick = v->tick_env > 1e-6f ? bq(&v->tick_bp, tn) * v->tick_env : 0;
    }

    /* ---- turbo: blade-pass whistle (mostly narrow-band noise, a little tone) and compressor air ---- */
    if (v->tb_lvl > 1e-5f) {
        float wn = rnd(&v->tseed), s;
        v->whine_ph += v->tb_hz / SR;
        if (v->whine_ph >= 1) v->whine_ph -= 1;
        s = .35f * sin_cyc(v->whine_ph) + .8f * bq(&v->tb_band, wn) * v->tb_bnorm +
            .6f * bq(&v->tb_air, rnd(&v->tseed)) * v->tb_anorm;
        tb = bq(&v->tb_lp, s * v->tb_lvl);
    }
    if (v->bov_peak > 0 || v->bov_env > 1e-6f) {
        /* a soft 20 ms onset while the valve opens (30 ms), then a 70 ms decay: a whoosh, never a click */
        v->bov_env = clean(v->bov_env + (v->bov_tgt - v->bov_env) * (v->bov_tgt > v->bov_env ? 1.f / (.02f * SR) : 1.f / (.07f * SR)));
        bov = bq(&v->bov_bp, rnd(&v->tseed)) * v->bov_norm * v->bov_env;
    }

    if (calibrating == 1) return bq(&v->trim, ex + in);
    if (calibrating == 2) return found;
    switch (es_solo) {
    case 1: tb = bov = 0; break;
    case 2: ex = in = found = tick = bov = 0; break;
    case 3: ex = in = found = tick = tb = 0; break;
    case 4: in = found = tick = tb = bov = 0; break; /* the rasp only, through the exhaust */
    case 5: tb = bov = 0; break;                     /* everything but the rasp (engine minus rasp) */
    case 6: ex = found = tick = tb = bov = 0; break; /* the induction noise only */
    case 8: ex = in = tick = tb = bov = 0; break;    /* the low foundation only */
    }
    out = bq(&v->trim, ex + in) * v->lvl + found * v->fo_g + (tb + bov + tick) * v->lvl_full;
    return out;
}

/* Loudness calibration: at each of NCAL rpm points from idle to redline (40 ms to settle, then 140 ms measured while
 * sweeping slowly across the point's cell) at no load, at full load and on the overrun, offline on a copy, listening through the master's fixed
 * tone, and store the gain that brings it to CAL_LEVEL. The pipes and the muffler keep their colour (which harmonic
 * rings where) but not their overall tilt against rpm: the level map decides how loud the engine is.
 * Then the low foundation is sized per rpm: the full-load engine's own share below ~100 Hz is measured, and the
 * foundation fills it up to a target share (a little less towards redline, where the level map adds loudness), so
 * the absolute low end stays roughly constant from 1500 rpm to redline; the total is renormalised to CAL_LEVEL.
 * ~40 ms of CPU per voice at start-up. */
#define FB_TOP 1.5f  /* target share below ~100 Hz at idle, dB under the total ... */
#define FB_SLOPE 8.5f /* ... falling by this much towards redline */
static void voice_calibrate(Voice *v, const Preset *p) {
    static Voice t;
    static const float ones[NX] = {1, 1, 1, 1, 1};
    const long settle = SR / 24, meas = SR / 7;
    float bass1[NCAL], ftot[NCAL], fbass[NCAL], s[NCAL];
    int L, k;
    long i;
    Ctl c;
    for (L = 0; L < 4; L++) for (k = 0; k < NCAL; k++) v->corr[L][k] = 1;
    for (k = 0; k < NCAL; k++) {
        v->fcal[k] = 0;
        v->fnorm[k] = 0;
    }
    for (L = 0; L < 5; L++) { /* no load, full load, overrun (below idle + 250 rpm: no load), the foundation, mid load */
        Tone tn;
        Biquad bl[4]; /* the low band: 8th-order Butterworth at 100 Hz (a firing order just above 120 Hz must not count) */
        static const float bq_q[4] = {.5098f, .6013f, .9000f, 2.5629f};
        int j;
        calibrating = L == 3 ? 2 : 1;
        const int slot = L == 4 ? 3 : L;
        t = *v;
        tone_init(&tn);
        for (j = 0; j < 4; j++) {
            bq_lowpass(&bl[j], 100, bq_q[j]);
            bl[j].z1 = bl[j].z2 = 0;
        }
        c.load = c.lvl_load = L == 1 || L == 3 ? .95f : L == 2 ? -.6f : L == 4 ? .4f : 0;
        c.throttle = L == 1 || L == 3 ? 1 : L == 2 ? 0 : L == 4 ? .4f : .02f;
        c.level = 1;
        c.turbo = p->turbo; /* the turbine darkens the exhaust: calibrate as the preset runs by default */
        c.shift_guard = 1;
        c.x = ones;
        for (k = 0; k < NCAL; k++) {
            double sum = 0, bsum = 0;
            float r;
            c.rpm = p->idle + (p->redline - p->idle) * k / (NCAL - 1);
            /* the shaft spools in ~0.35 s: start each point at its steady boost */
            t.spool = p->turbo ? powf(clampf((clampf(c.load, 0, 1) - .55f) / .45f, 0, 1), 1.3f) * clampf((c.rpm - 1400) / 2200, 0, 1) : 0;
            for (i = 0; i < settle + meas; i++) {
                /* settle at the cell's lower edge, then sweep slowly across the cell (+-half a step), so a narrow
                 * pipe resonance between two points is averaged in, not missed */
                float cell = (p->redline - p->idle) / (NCAL - 1), w = i < settle ? 0 : (float)(i - settle) / meas;
                float y, b;
                c.rpm = fmaxf(p->idle, p->idle + cell * (k - .5f + w));
                y = tone_run(&tn, voice_step(&t, p, &c), 0);
                b = bq(&bl[3], bq(&bl[2], bq(&bl[1], bq(&bl[0], y))));
                if (i >= settle) {
                    sum += (double)y * y;
                    bsum += (double)b * b;
                }
            }
            r = (float)sqrt(sum / meas);
            if (L != 3) {
                v->corr[slot][k] = r > 1e-6f ? CAL_LEVEL / r : 1;
                if (L == 1) bass1[k] = (float)sqrt(bsum / meas) / (r > 1e-6f ? r : 1);
            } else {
                ftot[k] = r;
                fbass[k] = (float)sqrt(bsum / meas);
            }
        }
    }
    calibrating = 0;
    for (k = 0; k < NCAL; k++) {
        float u = (float)k / (NCAL - 1), r = powf(10, -(FB_TOP + FB_SLOPE * u) / 20), q = ftot[k] / fmaxf(fbass[k], 1e-9f);
        float num = fmaxf(r * r - bass1[k] * bass1[k], .35f * .35f * r * r), den = fmaxf(1 - r * r * q * q, .15f);
        float f = sqrtf(num / den);
        v->fcal[k] = CAL_LEVEL * f / fmaxf(fbass[k], 1e-9f);
        v->fnorm[k] = q * f; /* the foundation's full-band level against the engine's, at full load */
    }
    for (k = 0; k < NCAL; k++) s[k] = (v->fcal[k > 0 ? k - 1 : 0] + 2 * v->fcal[k] + v->fcal[k < NCAL - 1 ? k + 1 : k]) / 4;
    for (k = 0; k < NCAL; k++) v->fcal[k] = s[k];
}
