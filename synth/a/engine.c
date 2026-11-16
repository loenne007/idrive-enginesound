/* engine.c - one engine voice.
 *
 * This is a C re-implementation of the sound path of YigitSalihEmecen/Engine_Sim (MIT, Copyright (c) 2026 Yigit
 * Salih Emecen): src/combustion-worklet.js + pulse.js (the source), resonators.js (pipes, muffler, intake,
 * nonlinearity), character.js (ExhaustNoise, SubLayer) and the turbo levels of layers.js, with these changes:
 *  - BMW firing geometry (cross-plane V8 1-5-4-8-6-3-7-2, S85 V10 90/54 degrees) instead of even intervals;
 *  - telemetry-driven: load/throttle/rpm come from the car, a fuel-cut overrun gives a lift-off crackle window
 *    (unburnt fuel in a hot pipe, a few Poisson events) instead of Engine_Sim's physics-driven pops;
 *  - Web Audio's render-quantum floor does not exist here, so every pipe is a true delay-line waveguide and the
 *    muffler/transit delays are fractional and exact; a smooth taper ends each pulse instead of truncating it.
 *
 * Signal flow per sample (all state preallocated; libm only at the 32-sample control rate):
 *
 *   crank phase with crank-speed ripple -> combustion events (per cylinder, cycle-to-cycle variation, timing
 *   jitter on a fixed schedule, sub-sample onset)  -> pressure pulse p(t) = A g(t) (1-e^-at) e^-bt per bank
 *   -> dp/dphase (what an open pipe radiates) -> radiation knee lowpass (24 orders) -> calibrated level
 *   -> + flow noise (firing-rate AM) + crackle -> bank waveguide -> transit -> collector waveguide
 *   -> muffler (feed-forward chamber combs + packing lowpass) -> shock nonlinearity -> turbine shelf
 *   + intake (Helmholtz airbox, direct path, flow/coast noise) + sub layer + turbo layer
 */
#include "dsp.h"
#include <string.h>

#define C_AIR 343.f
#define KNEE_HALF_ORDERS 48.f /* radiation knee: 24 engine orders */

/* ---- shared tables ---- */
#define ONSET_N 64
static float onset_tab[ONSET_N + 1]; /* 1 - exp(-(ts / 60 us)^2), ts over 0..9 samples */
#define SUB_N 1024
static float sub_tab[SUB_N + 1]; /* SubLayer's sine through its saturator, one period */
static int tables_ready;

static void tables_init(void) {
    int i;
    if (tables_ready) return;
    for (i = 0; i <= ONSET_N; i++) {
        float ts = 9.f * i / ONSET_N / SR / 60e-6f;
        onset_tab[i] = 1 - expf(-ts * ts);
    }
    for (i = 0; i <= SUB_N; i++) {
        float x = sinf(2 * PI_F * i / SUB_N), sg = x < 0 ? -1.f : 1.f;
        sub_tab[i] = tanhf(1.8f * x) * .92f + .08f * tanhf(2.6f * x * x * sg);
    }
    tables_ready = 1;
}

/* Engine_Sim's pulse law, used for the level calibration */
static float pulse_at(float d, float a, float b) {
    return (1 - expf(-a * d)) * expf(-b * d);
}

/* ---- pipes ---- */
/* phase of the loop lowpass at f, so the quarter-wave resonance lands exactly on c/4L (resonators.js) */
static float lp_phase(const Biquad *f, float hz) {
    float t = 2 * PI_F * hz / SR, c1 = cosf(t), s1 = sinf(t), c2 = cosf(2 * t), s2 = sinf(2 * t);
    float nr = f->b0 + f->b1 * c1 + f->b2 * c2, ni = -(f->b1 * s1 + f->b2 * s2);
    float dr = 1 + f->a1 * c1 + f->a2 * c2, di = -(f->a1 * s1 + f->a2 * s2);
    return atan2f(ni, nr) - atan2f(di, dr);
}
static void pipe_init(Pipe *w, float len, float gas, float refl, float damping) {
    float f = C_AIR * gas / (4 * len), d;
    memset(w, 0, sizeof(*w));
    w->lpbase = clampf(f * 1.8f / damping, 150, 12000); /* MODE_SURVIVAL 1.8: a handful of modes, then smear */
    w->refl = clampf(refl, 0, .92f);
    wa_lowpass(&w->lp, w->lpbase, .5f);
    d = (PI_F + lp_phase(&w->lp, f)) / (2 * PI_F * f) * SR;
    w->delay = clampf(d, 2, DLEN - 4);
}
/* Web Audio graph: in -> (+) -> delay -> lowpass -> x(-r) -> (+); out = 0.3 in + 0.9 lowpass */
static inline float pipe_step(Pipe *w, float x, float scale, float fb) {
    float y = bq(&w->lp, delay_read(&w->d, w->delay / scale));
    delay_write(&w->d, x + fb * y);
    return .3f * x + .9f * y;
}

/* ---- muffler chamber decorrelation (resonators.js Muffler.peakCoincidence / _decorrelate) ---- */
static float coincidence(const float *T, int n) {
    float cost = 0;
    int i, j, a, b;
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            for (a = 1; a <= 6; a++)
                for (b = 1; b <= 6; b++) {
                    float fa = a / T[i], fb = b / T[j], d = (fa - fb) / 45;
                    if (fa > 8000 || fb > 8000) continue;
                    cost += expf(-d * d) / (a * b);
                }
    return cost;
}

static int nbanks(const Preset *p) {
    return p->banks > 1 ? 2 : 1;
}

void voice_init(Voice *v, const Preset *p, int index) {
    const int nb = nbanks(p);
    const float jit = p->jitter / 100, hardness = clampf(p->hardness, 0, 1.5f) / .65f;
    float T[3], base[3], prod = 1, maxc = 0;
    int b, i, j, k, L, o;
    tables_init();
    memset(v, 0, sizeof(*v));
    v->seed = 0x9E3779B9u ^ (uint32_t)(index * 2654435761u + 7);
    v->fuel = 1;
    v->nout = nb + 1;

    /* --- combustion events: one per cylinder per bank output at its firing angle, one per cylinder on the intake
     * output 360 degrees later (the intake stroke), each with fixed trims (pulse.js combustionSpec) --- */
    for (i = 0; i < p->n; i++) {
        int cyl = p->order[i], bank = nb > 1 && cyl > p->n / 2;
        float amp_trim = 1 + rnd(&v->seed) * jit * 2.5f, ang = rnd(&v->seed) * jit * 6,
              decay_trim = 1 + rnd(&v->seed) * jit * 1.5f;
        for (k = 0; k < 2; k++) {
            Event *e = &v->ev[v->nev++];
            float a = fmodf(p->angle[i] + ang + (k ? 360 : 0) + 720, 720) / 720;
            e->out = k ? nb : bank;
            e->angle = a;
            e->amp_trim = amp_trim;
            e->decay_trim = decay_trim;
            e->next = a;
        }
    }
    v->nfire = p->n;
    /* --- calibration: render one nominal cycle per output at five loads with the same law (gradient per unit
     * phase, one-pole radiation knee in the phase domain) and scale each to 0.25 RMS --- */
    {
        enum { N = 4096 };
        static float cyc[N];
        float kk = 1 - expf(-2 * PI_F * KNEE_HALF_ORDERS / N);
        for (o = 0; o < v->nout; o++)
            for (L = 0; L <= 4; L++) {
                float load = L / 4.f, h = hardness * load, a = p->attack * (.55f + .85f * h),
                      b0 = p->decay * (1.25f - .35f * h), lp = 0, ss = 0;
                int pass;
                memset(cyc, 0, sizeof(cyc));
                for (k = 0; k < v->nev; k++) {
                    Event *e = &v->ev[k];
                    if (e->out != o) continue;
                    for (j = 0; j < N; j++) {
                        float d = (float)j / N - e->angle;
                        if (d < 0) d += 1;
                        cyc[j] += e->amp_trim * pulse_at(d, a, b0 * e->decay_trim);
                    }
                }
                for (pass = 0; pass < 2; pass++)
                    for (j = 0; j < N; j++) {
                        float g = (cyc[j] - cyc[(j + N - 1) % N]) * N;
                        lp += kk * (g - lp);
                        if (pass) ss += lp * lp;
                    }
                ss = sqrtf(ss / N);
                v->gains[o][L] = .25f / (ss > 0 ? ss : 1);
            }
    }

    /* --- exhaust: bank waveguides, transit delays, collector (reflection x0.7, damping x1.5) --- */
    for (b = 0; b < nb; b++) {
        float len = b ? p->bank_len_b : p->bank_len;
        pipe_init(&v->bank[b], len, p->gas, p->reflection, p->damping);
        v->transit_d[b] = clampf(len / (C_AIR * p->gas) * SR, 1, DLEN - 4);
    }
    pipe_init(&v->coll, p->collector, p->gas, p->reflection * .7f, p->damping * 1.5f);
    /* muffler: three feed-forward chamber combs, delays nudged so their peaks do not coincide */
    for (i = 0; i < 3; i++) {
        base[i] = T[i] = 2 * p->muffler[i] / (C_AIR * p->gas);
        if (p->muffler[i] > maxc) maxc = p->muffler[i];
    }
    for (i = 1; i < 3; i++) {
        float best = T[i], cost = 1e30f;
        for (k = 0; k < 41; k++) {
            float c;
            T[i] = base[i] * (1 + .06f * (2.f * k / 40 - 1));
            c = coincidence(T, i + 1);
            if (c < cost - 1e-12f) {
                cost = c;
                best = T[i];
            }
        }
        T[i] = best;
    }
    for (i = 0; i < 3; i++) {
        v->muf_d[i] = clampf(T[i] * SR, 1, DLEN - 4);
        v->muf_g[i] = clampf(.55f + .30f * p->reflection, .05f, .9f) * powf(.92f, (float)i);
        prod *= 1 + v->muf_g[i];
    }
    v->muf_trim = 1 / fmaxf(1, prod * .72f);
    v->packing_base = clampf(C_AIR * p->gas / (4 * maxc) * 7, 500, 11000);
    wa_highpass(&v->nl_dc, 22, .7f);
    wa_lowpass(&v->tb_lp, 4600, 0);
    bq_highshelf(&v->tb_shelf, 2600, -5.5f);
    v->scale = 1;
    v->nl_mk = 1;
    v->nl_drive = 1;
    /* sub layer: half the dominant firing order, octave-shifted into the 38..85 Hz chest band at mid revs */
    {
        float order = fmaxf(.5f, p->n / 4.f), fmid = (p->idle + p->redline) / 2 / 120;
        while (order * fmid < 38 && order < 8) order *= 2;
        while (order * fmid > 85 && order > .5f) order /= 2;
        v->sub_order = order;
    }
    v->weight = 0;
}

/* ---------------------------------------------------------------------------------------------------------------
 * Control rate: every CTRL samples, everything that needs a coefficient or a libm call.
 * ------------------------------------------------------------------------------------------------------------- */
static void voice_control(Voice *v, const Preset *p, float rpm, float load, float throttle, int turbo) {
    const float dt = (float)CTRL / SR, rpm_n = clampf(rpm / p->redline, 0, 1), pl = clampf(load, 0, 1);
    const float f0 = rpm / 120, nb = (float)nbanks(p);
    float x, overrun, heat, scale, flow, coast, excite, turb;
    int i;

    /* tip-in bark: a throttle stab gives an edge (engine-sim.js _writeParams). Only a stab from a low-load state
     * counts: load_lo remembers the recent minimum (it climbs back at 2.5/s), so the load recovering after a
     * gearshift's torque reduction, or any small wiggle under load, never barks. */
    v->load_lo = fminf(pl, v->load_lo + dt / .4f);
    v->bark *= expf(-dt / .16f);
    if (v->load_lo < .2f) v->bark = fmaxf(v->bark, clampf((pl - v->load) / dt / 10, 0, 1));
    v->load = pl;
    /* overrun: closed throttle above idle; fuel cut follows it (restored near idle) */
    overrun = (load < -.05f && rpm > p->idle + 250) ? 1.f : 0.f;
    v->overrun += (overrun - v->overrun) * (1 - expf(-dt / .08f));
    {
        float want = (load > -.06f || rpm < 1150) ? 1.f : (rpm > 1350 ? 0.f : (v->fuel > .5f));
        v->fuel += (want - v->fuel) * (1 - expf(-dt / (want > v->fuel ? .025f : .07f)));
    }
    x = clampf(pl + .3f * v->bark, 0, 1);
    v->x_load = x;
    v->amp = (1 + .35f * v->bark) * (.32f + .68f * powf(x, .7f)) * (.45f + .55f * rpm_n) * (1 - .45f * v->overrun) *
             p->voice;
    v->rpm_n = rpm_n;
    v->f0 = f0;
    /* radiation knee: one-pole lowpass at 24 engine orders */
    v->lpk = 1 - expf(-2 * PI_F * fminf(KNEE_HALF_ORDERS * f0, .45f * SR) / SR);
    /* Cabin loudness: Engine_Sim normalises every source to a fixed RMS and its dynamics squeeze the rest, which
     * leaves idle only ~6 dB under a full-load pull. In a car the range is closer to 15 dB, so the whole voice
     * follows load and rpm (the overrun keeps some level: the engine is still pumping). */
    {
        float xs = fmaxf(x, .3f * v->overrun);
        v->level = (.32f + .68f * powf(xs, .7f)) * (.55f + .45f * rpm_n);
    }

    /* refresh the live pulses' per-sample decay multipliers */
    {
        double dph = rpm / (120. * SR) * (1 + v->v);
        for (i = 0; i < v->nev; i++) {
            Pulse *q = &v->ev[i].p;
            if (!q->live) continue;
            q->ma = expf((float)(-q->a * dph));
            q->mb = expf((float)(-q->b * dph));
        }
    }

    /* pipes: gas temperature moves the sound speed a little with load/rpm; hotter, sharper pulses keep more HF */
    heat = clampf(.6f * x + .4f * rpm_n, 0, 1);
    scale = 1 + .05f * (heat - .35f);
    for (i = 0; i < (int)nb; i++) wa_lowpass(&v->bank[i].lp, v->bank[i].lpbase * scale * (1 + .35f * x), .5f);
    wa_lowpass(&v->coll.lp, v->coll.lpbase * scale * (1 + .35f * x), .5f);
    v->scale = scale;
    for (i = 0; i < 3; i++) v->muf_g[i] = clampf(.55f + .30f * p->reflection, .05f, .9f) * powf(.92f, (float)i) *
                                          (1 - .12f * x);
    wa_lowpass(&v->packing, v->packing_base * (1 + .30f * x) * (1 + .025f * (heat - .35f)), .6f);

    /* shock steepening: drive rises with load^1.5, boost and overrun */
    {
        float boost = v->spool * v->spool, a = clampf(powf(x, 1.5f) * (.55f + .45f * rpm_n) + .55f * boost * x, 0, 1.4f),
              od = .25f * v->overrun * (.3f + .7f * rpm_n);
        v->nl_drive = clampf(1 + 3.5f * clampf(a + od, 0, 1), .1f, 4.5f);
        v->nl_mk = powf(v->nl_drive, -.7f); /* undo ~70 % of the level the drive adds */
    }

    /* exhaust flow noise, AM'd by a sawtooth at the bank's firing rate (character.js ExhaustNoise) */
    {
        float fire = clampf(f0 * p->n / nb, 10, 6000), fl = .3f + .7f * x;
        v->xn_lvl = .38f * fl * (.25f + .75f * rpm_n) * (1 + .35f * v->overrun);
        v->xn_depth = fminf(v->xn_lvl * .85f, v->xn_lvl * (1.1f - .7f * rpm_n));
        wa_highpass(&v->xn_hp, 40 + 30 * rpm_n, 1);
        bq_peak(&v->xn_peak, clampf(fire, 30, 2000), 1.4f, 7);
        wa_lowpass(&v->xn_body, clampf(450 + 2600 * x + 1800 * rpm_n, 120, 9000), .9f);
    }

    /* intake (resonators.js IntakeResonator): fixed Helmholtz cavity, excitation and flow noise move */
    flow = clampf(throttle * (.25f + .75f * rpm_n), 0, 1);
    coast = clampf(v->overrun * (.15f + .85f * rpm_n), 0, 1);
    excite = powf(fmaxf(x, 1e-3f), .7f);
    bq_bandpass(&v->in_helm, p->helmholtz, clampf(p->intake_q * (.55f + .45f * throttle), .3f, 30));
    v->in_hg = (.12f + .88f * excite) * (.35f + .65f * rpm_n);
    v->in_dg = .30f * (.2f + .8f * excite);
    wa_lowpass(&v->in_direct, p->helmholtz * 4, .7f);
    wa_highpass(&v->in_nhp, p->helmholtz * .8f, .7f);
    bq_bandpass(&v->in_nbp, clampf(p->helmholtz * (3 + 9 * flow) * (1 + .6f * coast), 20, 18000),
                clampf(.9f + .7f * flow, .3f, 6));
    v->in_ng = .42f * powf(flow, 1.2f) + .16f * coast;
    v->in_nh = .20f * flow + .05f * coast;

    /* sub layer */
    v->sub_lvl = .34f * (.18f + .82f * powf(x, .75f)) * (1 - .45f * rpm_n) * (1 - .6f * v->overrun);
    wa_lowpass(&v->sub_lp, 150 + 260 * x, .7f);

    /* turbo (layers.js TurboLayer, reduced): shaft inertia, whine, blade-rate noise, airflow, rush */
    {
        float full = fmaxf(.6f * p->redline, 1800 + 1500), on = clampf((rpm - 1800) / (full - 1800), 0, 1);
        float drive = turbo ? clampf(x * (.08f + .92f * on), 0, 1) : 0, coasting = throttle < .12f, tau;
        float fl = clampf(fmaxf(x, throttle * .7f), 0, 1), floor = turbo ? .04f + .07f * rpm_n : 0;
        v->climb += (clampf((drive - v->spool) * 3, 0, 1) - v->climb) * fminf(1, dt / .08f);
        tau = drive > v->spool ? .42f * 1.1f : .42f * (coasting ? .45f : 1.3f);
        v->spool += (drive - v->spool) * (1 - expf(-dt / tau));
        if (v->spool < floor) v->spool = floor;
        if (!turbo) v->spool *= 1 - dt / .3f;
        v->tb_aero = v->spool * v->spool;
        v->tb_whine_hz = fmaxf(120, fminf(p->redline / 120 * 26, 1150) * powf(fmaxf(.05f, v->spool), 1.55f));
        bq_bandpass(&v->tb_noise, v->tb_whine_hz, 9);
        bq_bandpass(&v->tb_hiss, clampf(v->tb_whine_hz * 2.2f, 900, 4200), 1.2f);
        v->hiss_g = .22f * v->tb_aero * (.45f + .55f * fl);
        v->whoosh_g = .16f * (v->tb_aero * fl + .6f * v->climb * sqrtf(v->spool));
        bq_bandpass(&v->tb_whoosh, 2500 + 3000 * v->spool, .6f);
        /* surge flutter on a fast closure against a spinning compressor ("stu-stu"), short-lived */
        if (throttle < v->prev_thr - .2f * dt / .05f && throttle < .25f && v->spool > .12f) /* a real closure */
            v->surge = fmaxf(v->surge, clampf(v->spool * 1.4f, 0, 1));
        v->surge *= expf(-dt / .26f);
        bq_bandpass(&v->tb_surge, 1300 + 900 * v->surge, 3);
        v->prev_thr = throttle;
        turb = turbo ? .55f + .45f * v->spool : 0;
        bq_highshelf(&v->turbine, 700, -7 * turb);
    }
}

/* ---------------------------------------------------------------------------------------------------------------
 * Per sample
 * ------------------------------------------------------------------------------------------------------------- */
float voice_step(Voice *v, const Preset *p, float rpm, float load, float throttle, int turbo) {
    const int nb = nbanks(p);
    const float hardness = clampf(p->hardness, 0, 1.5f) / .65f;
    float src[3] = {0, 0, 0}, acc[3] = {0, 0, 0}, ex, in, sub, tb = 0, direct = 0, xn, crack = 0, scale;
    double dph, dphv;
    int i, o;

    if (!(v->tick % CTRL)) voice_control(v, p, rpm, load, throttle, turbo);
    v->tick++;
    scale = v->scale;

    /* ---- combustion source (combustion-worklet.js) ---- */
    {
        const float x = v->x_load, low = 1 - v->rpm_n, h = hardness * x;
        const float ccv = .012f + .012f * p->jitter, jdeg = (.4f + .5f * p->jitter) / 720;
        /* crank-speed ripple per firing ~ T (4 pi / n) / (J w^2), capped (pulse.js crankRipple) */
        float w = fmaxf(40, rpm * PI_F / 30), T = p->torque * (.12f + .88f * x);
        float ripple = fminf(.12f, .3f * T * (4 * PI_F / p->n) / (p->inertia * w * w));
        float gi = x * 4, gf;
        int g0 = gi >= 3 ? 3 : (int)gi;
        gf = gi - g0;
        dph = rpm / (120. * SR);
        dphv = dph * (1 + v->v);
        /* compression drag + a leak standing in for the idle-speed control */
        v->v -= (float)((ripple * v->nfire + 6 * v->v) * dph);
        v->v = clampf(v->v, -.2f, .2f);
        v->crank += dphv;
        for (i = 0; i < v->nev; i++) {
            Event *e = &v->ev[i];
            int guard = 0;
            while (v->crank >= e->next && guard++ < 8) {
                Pulse *q = &e->p;
                float cov = ccv * (.7f + 2.3f * (1 - x) * (1 - x)) * (1 + low);
                float trim = 1 + (e->amp_trim - 1) * (1 + 1.5f * low), past;
                float spread = jdeg * (.6f + 1.4f * (1 - x)) * (1 + .8f * low);
                q->A = trim * fmaxf(.15f, 1 + gauss(&v->seed) * cov);
                q->a = p->attack * (.55f + .85f * h);
                q->b = p->decay * (1.25f - .35f * h) * e->decay_trim * (1 + .5f * cov * gauss(&v->seed));
                past = (float)fmin(1., v->crank - e->next);
                q->t = past;
                q->ts = dphv > 0 ? (float)(past / dphv) : 0;
                q->ea = expf(-q->a * past);
                q->eb = expf(-q->b * past);
                q->ma = expf((float)(-q->a * dphv));
                q->mb = expf((float)(-q->b * dphv));
                q->live = 1;
                /* only a firing kicks the crank; the intake events mirror the firings */
                if (e->out < nb) v->v += ripple * q->A;
                e->cycle += 1;
                e->next = e->cycle + e->angle + clampf(gauss(&v->seed) * spread, -.02f, .02f);
            }
            /* sum the live pulse */
            {
                Pulse *q = &e->p;
                if (q->live) {
                    float on = q->ts >= 9 ? 1 : onset_tab[(int)(q->ts * (ONSET_N / 9.f))], val, u;
                    if (q->t > 1 || q->b * q->t > 9) {
                        q->live = 0;
                        continue;
                    }
                    val = q->A * on * (1 - q->ea) * q->eb;
                    /* smooth end instead of truncating at one cycle */
                    if (q->t > .9f) {
                        u = (q->t - .9f) * 10;
                        u = 1 - u * u;
                        val *= u * u;
                    }
                    acc[e->out] += val;
                    q->t += (float)dphv;
                    q->ts += 1;
                    q->ea *= q->ma;
                    q->eb *= q->mb;
                }
            }
        }
        /* pressure gradient per unit crank phase, radiation knee, calibrated level */
        {
            float invv = dphv > 1e-9 ? (float)(1 / dphv) : 0;
            /* (a floor so a stopped crank lets the state decay instead of holding a DC level) */
            float lpk = fmaxf(v->lpk, 1e-3f);
            for (o = 0; o < v->nout; o++) {
                float grad = (acc[o] - v->prev_p[o]) * invv, gain = v->gains[o][g0] + (v->gains[o][g0 + 1] - v->gains[o][g0]) * gf;
                v->prev_p[o] = acc[o];
                v->lp[o] = clean(v->lp[o] + lpk * (grad - v->lp[o]));
                src[o] = v->lp[o] * gain * v->amp;
            }
        }
    }

    /* ---- exhaust flow noise: white -> HP -> +7 dB at the firing rate -> body LP, sawtooth AM ---- */
    {
        float am;
        v->xn_saw += v->f0 * p->n / nb / SR;
        if (v->xn_saw >= 1) v->xn_saw -= 1;
        am = v->xn_lvl + v->xn_depth * (2 * v->xn_saw - 1);
        xn = bq(&v->xn_body, bq(&v->xn_peak, bq(&v->xn_hp, rnd(&v->seed)))) * am;
    }

    /* ---- overrun crackle: unburnt fuel lighting in the hot pipe right after a lift-off ---- */
    if (p->crackle > 0) {
        if (v->fuel > .5f)
            v->resid += (v->x_load * (.4f + .6f * v->rpm_n) - v->resid) * (1.f / (1.5f * SR));
        else
            v->resid *= 1 - 1.f / (.8f * SR);
        /* lift-off = the load goes negative after SUSTAINED load (0.5 s average), once per lift: a gearshift's
         * torque dip never goes negative, and a short downshift blip cannot build the average */
        v->load_avg += (fmaxf(load, 0) - v->load_avg) * (1.f / (.5f * SR));
        if (load > .1f) v->crack_armed = 1;
        if (load < -.05f && v->crack_armed) {
            if (v->load_avg >= .25f && rpm > 1800 && v->resid > .12f)
                v->crack_t = .2f + .5f * (.5f + .5f * rnd(&v->seed)); /* burst window 0.2..0.7 s */
            v->crack_armed = 0;
        }
        if (v->crack_t > 0) {
            v->crack_t -= 1.f / SR;
            if (.5f + .5f * rnd(&v->seed) < p->crackle * (2 + 5 * v->resid) / SR) { /* Poisson, ~2-6 per second */
                float size = .5f + .5f * rnd(&v->seed);
                v->crack_env += (.3f + 1.2f * size * size) * (.4f + v->resid);
                v->crack_dec = 1 - 1.f / ((.012f + .03f * (.5f + .5f * rnd(&v->seed))) * SR);
            }
        }
        v->crack_env = clean(v->crack_env * v->crack_dec);
        /* ~1 ms rise through the lowpass; noisy, damped */
        v->crack_lp = clean(v->crack_lp + .05f * (rnd(&v->seed) * v->crack_env + .3f * v->crack_env - v->crack_lp));
        crack = v->crack_lp;
    }

    /* ---- exhaust system (resonators.js ExhaustSystem) ---- */
    {
        float fb, sum = 0, y;
        int b;
        for (b = 0; b < nb; b++) {
            Pipe *w = &v->bank[b];
            float r = -clampf(w->refl * (.92f + .08f * v->x_load), 0, .92f), t;
            y = pipe_step(w, src[b] + xn + 1.05f * crack, scale, r);
            /* transit to the collector: a pure delay, not in a loop */
            t = delay_read(&v->transit[b], v->transit_d[b] / scale);
            delay_write(&v->transit[b], y);
            sum += t;
        }
        sum *= nb > 1 ? .70710678f : 1;
        fb = -clampf(v->coll.refl * (.92f + .08f * v->x_load), 0, .92f);
        ex = pipe_step(&v->coll, sum, scale, fb);
        /* muffler: chambers as feed-forward combs (notches where the reflections cancel), then the packing */
        for (b = 0; b < 3; b++) {
            float d = delay_read(&v->muf[b], v->muf_d[b] / (1 + .025f * (scale - 1) / .05f));
            delay_write(&v->muf[b], ex);
            ex += v->muf_g[b] * d;
        }
        ex = bq(&v->packing, ex) * v->muf_trim;
        /* shock steepening: asymmetric soft saturation (compression half saturates first), DC block, makeup */
        {
            float d = v->nl_drive, u = d * ex;
            u = u >= 0 ? 1.05f * tanh_f(u / 1.05f) : 1.36f * tanh_f(u / 1.36f);
            ex = bq(&v->nl_dc, u) * v->nl_mk;
        }
        ex = bq(&v->turbine, ex);
    }

    /* ---- intake ---- */
    {
        float w = rnd(&v->seed), pk, nz, h;
        /* pink-ish noise (resonators.js _makeNoise) */
        v->in_pink[0] = .997f * v->in_pink[0] + w * .0555179f;
        v->in_pink[1] = .985f * v->in_pink[1] + w * .3104856f;
        pk = (v->in_pink[0] + v->in_pink[1] + w * .1848f) * .32f;
        nz = bq(&v->in_nhp, pk);
        h = bq(&v->in_helm, src[nb] + v->in_nh * nz);
        in = h * v->in_hg + bq(&v->in_direct, src[nb]) * v->in_dg + bq(&v->in_nbp, nz) * v->in_ng;
        in *= p->intake_level * (turbo ? 1 - .4f * (.55f + .45f * v->spool) : 1);
    }

    /* ---- sub layer: saturated sine at the chest-band order ---- */
    {
        float ph;
        int k;
        v->sub_ph += v->f0 * v->sub_order / SR;
        if (v->sub_ph >= 1) v->sub_ph -= 1;
        ph = v->sub_ph * SUB_N;
        k = (int)ph;
        sub = bq(&v->sub_lp, sub_tab[k] + (ph - k) * (sub_tab[k + 1] - sub_tab[k])) * v->sub_lvl;
    }

    /* ---- turbo: whine (soft-saturated blade tone with shaft-rate AM), blade-rate noise, rush, flutter ---- */
    if (v->spool > .02f || v->surge > .01f) {
        float wn = rnd(&v->seed), s, shaft, fl;
        int k;
        v->whine_ph += v->tb_whine_hz / SR;
        if (v->whine_ph >= 1) v->whine_ph -= 1;
        v->shaft_ph += v->tb_whine_hz / 11 / SR;
        if (v->shaft_ph >= 1) v->shaft_ph -= 1;
        k = (int)(v->whine_ph * SUB_N);
        s = sub_tab[k];                                  /* the saturated sine: grain = even harmonics */
        shaft = 1 + (.52f - .28f * v->spool) * sub_tab[(int)(v->shaft_ph * SUB_N)];
        fl = .62f + .38f * v->x_load;
        /* the blade tone is kept well under the blade-rate noise band: a clean whistle reads as a synth */
        tb = .025f * v->tb_aero * fl * s * shaft + .30f * v->tb_aero * fl * bq(&v->tb_noise, wn);
        v->air_lp += (.06f + .1f * v->spool) * (wn - v->air_lp);
        tb += .20f * (.15f + .85f * v->spool) * v->spool * (.25f + .75f * v->x_load) * v->air_lp * .5f;
        tb += v->hiss_g * bq(&v->tb_hiss, wn);
        /* compressor/intake air: broadband whoosh, more while the shaft is climbing */
        tb += v->whoosh_g * bq(&v->tb_whoosh, rnd(&v->seed));
        /* flutter: a burst train at ~13 Hz through the "tu" band */
        if (v->surge > .01f) {
            float burst;
            v->surge_ph += 13 * (.7f + .45f * v->surge) / SR;
            if (v->surge_ph >= 1) v->surge_ph -= 1;
            burst = v->surge_ph < .35f ? sub_tab[(int)(v->surge_ph / .35f * SUB_N / 2)] : 0;
            tb += .5f * v->surge * burst * bq(&v->tb_surge, wn);
        }
    }

    /* turbo tone stage: it is the one voice living in the 2-6 kHz band */
    tb = bq(&v->tb_shelf, bq(&v->tb_lp, tb));
    /* the voice's bus mix (presets.js DEFAULT_SOUND.mix) */
    direct = .75f * crack;
    return (ex * 1.0f + in * .75f + tb * .40f + direct * .42f + sub * .25f) * v->level;
}
