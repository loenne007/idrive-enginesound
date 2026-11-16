/* engine.c - one engine voice: Engine_Sim's combustion-worklet.js, pulse.js (combustionSpec, crankRipple),
 * resonators.js (Waveguide, Muffler, IntakeResonator, Nonlinearity, ExhaustSystem), character.js
 * (ExhaustNoise, SubLayer, CharacterModulator), layers.js (TransmissionLayer, TurboLayer, TransientBank) and the
 * per-frame mapping of engine-sim.js _writeParams. Copyright (c) Yigit Salih Emecen (Engine_Sim, MIT); C port. */
#include "engine.h"
#include <stdlib.h>

#define PI_D 3.14159265358979323846
#define C_SOUND 343.0
#define TC 0.02f
#define TC_DELAY 0.09f
#define LOOKAHEAD 0.004

static double dclamp(double v, double lo, double hi) {
    return v < lo ? lo : v > hi ? hi : v;
}
static float hz(float v, float d) {
    return clampf(finf(v, d), 10, 20000);
}

/* ------------------------------------------------------------------------------------------------ noise beds */
#define NB_LEN 96000 /* layers.js sharedNoise: 2 s */
static float WHITE[NB_LEN], PINK[NB_LEN], INTAKE_N[NB_LEN], EXN[129600], BROWN[148800];
static uint32_t mrand_state = 0x6d2b79f5u;
static double mrand(void) { /* stands in for Math.random */
    return xs_unit(&mrand_state);
}
static void seam(float *d, int len) {
    int i, fade = len >> 2 < 2048 ? len >> 2 : 2048;
    for (i = 0; i < fade; i++) {
        float t = (float)i / fade;
        d[i] = d[i] * t + d[len - fade + i] * (1 - t);
    }
}
static void noise_init(void) {
    uint32_t s;
    double b0 = 0, b1 = 0, b2 = 0, b3 = 0, b4 = 0, b5 = 0, b6 = 0, last = 0;
    int i;
    s = 0x9e3779b9u; /* pink */
    for (i = 0; i < NB_LEN; i++) {
        double w = xs_unit(&s) * 2 - 1;
        b0 = 0.99886 * b0 + w * 0.0555179;
        b1 = 0.99332 * b1 + w * 0.0750759;
        b2 = 0.96900 * b2 + w * 0.1538520;
        b3 = 0.86650 * b3 + w * 0.3104856;
        b4 = 0.55000 * b4 + w * 0.5329522;
        b5 = -0.7616 * b5 - w * 0.0168980;
        PINK[i] = (float)((b0 + b1 + b2 + b3 + b4 + b5 + b6 + w * 0.5362) * 0.11);
        b6 = w * 0.115926;
    }
    seam(PINK, NB_LEN);
    s = 0x85ebca6bu; /* white */
    for (i = 0; i < NB_LEN; i++) WHITE[i] = (float)(xs_unit(&s) * 2 - 1);
    seam(WHITE, NB_LEN);
    s = 0x9e3779b9u; /* IntakeResonator._makeNoise (no seam fade) */
    b0 = b1 = 0;
    for (i = 0; i < NB_LEN; i++) {
        double w = xs_unit(&s) * 2 - 1;
        b0 = 0.997 * b0 + w * 0.0555179;
        b1 = 0.985 * b1 + w * 0.3104856;
        INTAKE_N[i] = (float)dclamp((b0 + b1 + w * 0.1848) * 0.32, -1, 1);
    }
    for (i = 0; i < 129600; i++) EXN[i] = (float)(mrand() * 2 - 1); /* character.js noiseBuffer 2.7 s white */
    seam(EXN, 129600);
    for (i = 0; i < 148800; i++) { /* 3.1 s brown */
        last = (last + 0.02 * (mrand() * 2 - 1)) / 1.02;
        BROWN[i] = (float)(last * 3.5);
    }
    seam(BROWN, 148800);
}

/* ------------------------------------------------------------------------------------------------ curves */
static float SHOCK[4096], SUBC[2048], GRAIN[1024], SURGEC[1024];
static void curves_init(void) {
    int i;
    double cp = 1 / (3.2 / 3.2 > .2 ? 3.2 / 3.2 : .2) * 1.05, cn = 1 / (2.0 / 3.2 > .2 ? 2.0 / 3.2 : .2) * 0.85;
    for (i = 0; i < 4096; i++) { /* resonators.js shockCurve(3.2, 2.0, 4096, head 6) */
        double x = 6 * ((i / 4095.0) * 2 - 1);
        SHOCK[i] = (float)(x >= 0 ? cp * tanh(x / cp) : cn * tanh(x / cn));
    }
    for (i = 0; i < 2048; i++) { /* SubLayer._curve */
        double x = (i / 2047.0) * 2 - 1, sg = x > 0 ? 1 : x < 0 ? -1 : 0;
        SUBC[i] = (float)(tanh(1.8 * x) * 0.92 + 0.08 * tanh(2.6 * x * x * sg));
    }
    for (i = 0; i < 1024; i++) { /* grainCurve(1024, 0.55) */
        double x = (i / 1023.0) * 2 - 1, k = x >= 0 ? 2.4 * 1.55 : 2.4 * 0.45;
        GRAIN[i] = (float)(tanh(k * x) / tanh(k));
    }
    for (i = 0; i < 1024; i++) { /* surgePulseCurve(1024) */
        double t = i / 1023.0, RISE = 0.24, a = t < RISE ? 0.5 - 0.5 * cos(PI_D * t / RISE) : 1;
        SURGEC[i] = (float)(a * exp(-4.5 * (t - RISE > 0 ? t - RISE : 0)));
    }
}
void engine_global_init(void) {
    static int done;
    if (done) return;
    done = 1;
    waves_init();
    noise_init();
    curves_init();
}

/* ------------------------------------------------------------------------------------------------ helpers */
float voice_rpmnorm(const Preset *p, float rpm) {
    return clampf((rpm - p->idle) / (p->redline - p->idle), 0, 1);
}
static void firing_angles(const Preset *p, double *ang) { /* profiles.js firingAngles, indexed by cylinder */
    int i;
    for (i = 0; i < p->n; i++) ang[p->order[i]] = fmod(i * 720.0 / p->n + 720, 720);
}
static int bank_angles(const Preset *p, int b, double *out) {
    double ang[13];
    int i, j, n = p->bank_n[b];
    firing_angles(p, ang);
    for (i = 0; i < n; i++) out[i] = ang[p->banks[b][i]];
    for (i = 1; i < n; i++)
        for (j = i; j > 0 && out[j - 1] > out[j]; j--) {
            double t = out[j];
            out[j] = out[j - 1];
            out[j - 1] = t;
        }
    return n;
}
static double pulse_at(double d, double a, double b) {
    if (d < 0 || d > 1) return 0;
    return (1 - exp(-a * d)) * exp(-b * d);
}
static float crank_ripple(const Preset *p, float rpm, float load) {
    double J = p->inertia > .02 ? p->inertia : .02, n = p->n, w = rpm * PI_D / 30, T, r;
    if (w < 40) w = 40;
    T = p->peakTorque * (0.12 + 0.88 * dclamp(load, 0, 1));
    r = 0.3 * T * (4 * PI_D / n) / (J * w * w);
    return (float)dclamp(r, 0, 0.12);
}

/* ------------------------------------------------------------------------------------------------ combustion */
static void comb_init(Comb *c, const Preset *p) {
    double jit = p->jitter / 100.0, hardness, angles[6];
    uint32_t s;
    int b, i, k, nb = p->nbanks, ne = 0, ni = 0;
    double iang[12];
    float iamp[12], idec[12];
    struct {
        int out;
        double angle;
        float amp, dec;
    } ev[24], t;
    memset(c, 0, sizeof(*c));
    s = (uint32_t)((uint64_t)((uint64_t)p->n * 2654435761ull + (uint64_t)llround(p->attack * 97) + 7ull) &
                   0xffffffffull);
    if (!s) s = 1;
    for (b = 0; b < nb; b++) {
        int n = bank_angles(p, b, angles);
        for (i = 0; i < n; i++) {
            double ampTrim = 1 + (xs_unit(&s) * 2 - 1) * jit * 2.5;
            double angTrim = (xs_unit(&s) * 2 - 1) * jit * 6;
            double decayTrim = 1 + (xs_unit(&s) * 2 - 1) * jit * 1.5;
            ev[ne].out = b;
            ev[ne].angle = fmod(fmod(angles[i] + angTrim, 720) + 720, 720) / 720;
            ev[ne].amp = (float)ampTrim;
            ev[ne].dec = (float)decayTrim;
            ne++;
            iang[ni] = angles[i] + angTrim + 360;
            iamp[ni] = (float)ampTrim;
            idec[ni] = (float)decayTrim;
            ni++;
        }
    }
    for (i = 0; i < ni; i++) {
        ev[ne].out = nb;
        ev[ne].angle = fmod(fmod(iang[i], 720) + 720, 720) / 720;
        ev[ne].amp = iamp[i];
        ev[ne].dec = idec[i];
        ne++;
    }
    hardness = dclamp(p->hardness, 0, 1.5) / 0.65;
    /* calibration gains (combustionSpec) */
    {
        enum { N = 8192 };
        static double cyc[N];
        double kk = 1 - exp(-2 * PI_D * 48 / N);
        int o, L;
        for (o = 0; o <= nb; o++)
            for (L = 0; L <= 4; L++) {
                double load = L / 4.0, h = hardness * load, a = p->attack * (0.55 + 0.85 * h),
                       b0 = p->decay * (1.25 - 0.35 * h), lp = 0, ss = 0, rms;
                int pass;
                for (i = 0; i < N; i++) cyc[i] = 0;
                for (k = 0; k < ne; k++) {
                    if (ev[k].out != o) continue;
                    for (i = 0; i < N; i++) {
                        double d = (double)i / N - ev[k].angle;
                        if (d < 0) d += 1;
                        cyc[i] += ev[k].amp * pulse_at(d, a, b0 * ev[k].dec);
                    }
                }
                for (pass = 0; pass < 2; pass++)
                    for (i = 0; i < N; i++) {
                        double g = (cyc[i] - cyc[(i + N - 1) % N]) * N;
                        lp += kk * (g - lp);
                        if (pass) ss += lp * lp;
                    }
                rms = sqrt(ss / N);
                if (!(rms > 0)) rms = 1;
                c->gains[o][L] = (float)(0.25 / rms);
            }
    }
    /* stable sort by angle */
    for (i = 1; i < ne; i++)
        for (k = i; k > 0 && ev[k - 1].angle > ev[k].angle; k--) {
            t = ev[k];
            ev[k] = ev[k - 1];
            ev[k - 1] = t;
        }
    c->nEv = ne;
    c->nOut = nb + 1;
    c->nFire = 0;
    for (i = 0; i < ne; i++) {
        c->eout[i] = ev[i].out;
        c->eangle[i] = ev[i].angle;
        c->eamp[i] = ev[i].amp;
        c->edecay[i] = ev[i].dec;
        c->nextAt[i] = ev[i].angle;
        c->cycleOf[i] = 0;
        if (ev[i].out < c->nOut - 1) c->nFire++;
    }
    c->attack = p->attack;
    c->decay = p->decay;
    c->hardness = (float)hardness;
    c->ccv = (float)(0.012 + 0.012 * p->jitter);
    c->jitterCycles = (float)((0.4 + 0.5 * p->jitter) / 720.0);
    c->seed = (uint32_t)(p->n * 40503 + (int)llround(p->attack * 131));
    if (!c->seed) c->seed = 1;
    sp_init(&c->f0, 10);
    sp_init(&c->load, 0);
    sp_init(&c->amp, 0);
    sp_init(&c->ripple, 0);
    sp_init(&c->low, 0);
}
static double crand(Comb *c) {
    return xs_unit(&c->seed);
}
static double cgauss(Comb *c) {
    double a = crand(c), b = crand(c), d = crand(c), e = crand(c);
    return (a + b + d + e - 2) * 1.732;
}
static void comb_schedule(Comb *c, int k, double load, double low) {
    double spread = c->jitterCycles * (0.6 + 1.4 * (1 - load)) * (1 + 0.8 * low);
    c->cycleOf[k] += 1;
    c->nextAt[k] = c->cycleOf[k] + c->eangle[k] + dclamp(cgauss(c) * spread, -0.02, 0.02);
}
static void comb_fire(Comb *c, int k, double load, double low, double ripple, double past, double f0) {
    double cov = c->ccv * (0.7 + 2.3 * (1 - load) * (1 - load)) * (1 + low);
    double trim = 1 + (c->eamp[k] - 1) * (1 + 1.5 * low), g = 1 + cgauss(c) * cov;
    double A = trim * (g > 0.15 ? g : 0.15), h = c->hardness * load;
    double a = c->attack * (0.55 + 0.85 * h);
    double b = c->decay * (1.25 - 0.35 * h) * c->edecay[k] * (1 + 0.5 * cov * cgauss(c));
    int j = c->np;
    if (j < CMAXP) {
        c->pA[j] = (float)A;
        c->pa[j] = (float)a;
        c->pb[j] = (float)b;
        c->pt[j] = past;
        c->pts[j] = (float)(f0 > 0 ? past / f0 : 0);
        c->pout[j] = c->eout[k];
        c->np++;
    }
    if (c->eout[k] < c->nOut - 1) c->v += ripple * A;
}
#define ONSET_S 60e-6f
static void comb_run(Comb *c, float out[3][QN]) {
    float f0, load, amp, ripple, low, drag, lpk, gain[3], val[CMAXP], pt[CMAXP];
    double dph, dts = 1.0 / SR_HZ, gi;
    int i, k, o, g0, j;
    float gf;
    /* k-rate params: value at the quantum start */
    sp_fill(&c->f0, 0, QN);
    sp_fill(&c->load, 0, QN);
    sp_fill(&c->amp, 0, QN);
    sp_fill(&c->ripple, 0, QN);
    sp_fill(&c->low, 0, QN);
    f0 = clampf(c->f0.v, 0, 4000);
    load = clampf(c->load.v, 0, 1);
    amp = clampf(c->amp.v, 0, 8);
    ripple = clampf(c->ripple.v, 0, 0.2f);
    low = clampf(c->low.v, 0, 1);
    drag = ripple * c->nFire;
    dph = f0 / (double)SR_HZ;
    gi = load * 4;
    g0 = (int)floor(gi);
    if (g0 > 3) g0 = 3;
    gf = (float)(gi - g0);
    for (o = 0; o < c->nOut; o++) gain[o] = c->gains[o][g0] + (c->gains[o][g0 + 1] - c->gains[o][g0]) * gf;
    {
        double fc = 48 * f0 < 0.45 * SR_HZ ? 48 * f0 : 0.45 * SR_HZ;
        lpk = (float)(1 - exp(-2 * PI_D * fc / SR_HZ));
    }
    for (i = 0; i < QN; i++) {
        double dphv = dph * (1 + c->v), invv = dphv > 1e-9 ? 1 / dphv : 0, acc[3] = {0, 0, 0};
        c->v -= (drag + 6 * c->v) * dph;
        if (c->v > 0.2)
            c->v = 0.2;
        else if (c->v < -0.2)
            c->v = -0.2;
        c->abs += dphv;
        for (k = 0; k < c->nEv; k++) {
            if (c->abs >= c->nextAt[k]) {
                double past = c->abs - c->nextAt[k];
                if (past > 1) past = 1;
                comb_fire(c, k, load, low, ripple, past, f0);
                comb_schedule(c, k, load, low);
                while (c->nextAt[k] < c->abs) comb_schedule(c, k, load, low);
            }
        }
        /* drop spent pulses */
        for (j = c->np - 1; j >= 0; j--) {
            if (c->pt[j] > 1 || c->pb[j] * c->pt[j] > 9) {
                int l = c->np - 1;
                c->pA[j] = c->pA[l];
                c->pa[j] = c->pa[l];
                c->pb[j] = c->pb[l];
                c->pt[j] = c->pt[l];
                c->pts[j] = c->pts[l];
                c->pout[j] = c->pout[l];
                c->np--;
            }
        }
        for (j = 0; j < c->np; j++) pt[j] = (float)c->pt[j];
        for (j = 0; j < c->np; j++)
            val[j] = c->pA[j] * (1.f - fast_exp(-c->pa[j] * pt[j])) * fast_exp(-c->pb[j] * pt[j]);
        for (j = 0; j < c->np; j++) /* Gaussian onset; beyond 8 time constants it is 1 to float precision */
            if (c->pts[j] < 8 * ONSET_S) val[j] *= 1.f - fast_exp(-(c->pts[j] * c->pts[j]) * (1.f / (ONSET_S * ONSET_S)));
        for (j = 0; j < c->np; j++) {
            acc[c->pout[j]] += val[j];
            c->pt[j] += dphv;
            c->pts[j] += (float)dts;
        }
        for (o = 0; o < c->nOut; o++) {
            double grad = (acc[o] - c->prevP[o]) * invv;
            c->prevP[o] = acc[o];
            c->lp[o] += lpk * (grad - c->lp[o]);
            out[o][i] = (float)(c->lp[o] * gain[o] * amp);
        }
    }
}

/* ------------------------------------------------------------------------------------------------ waveguide */
static double lowpass_phase(double f, double fc, double Q, double sr) {
    double w = 2 * PI_D * dclamp(fc, 1, 0.49 * sr) / sr, cw = cos(w), sw = sin(w), al = sw / (2 * (Q > 1e-4 ? Q : 1e-4));
    double a0 = 1 + al, b0 = ((1 - cw) / 2) / a0, b1 = (1 - cw) / a0, b2 = b0, a1 = (-2 * cw) / a0, a2 = (1 - al) / a0;
    double t = 2 * PI_D * dclamp(f, 0.01, 0.49 * sr) / sr, c1 = cos(t), s1 = sin(t), c2 = cos(2 * t), s2 = sin(2 * t);
    double nr = b0 + b1 * c1 + b2 * c2, ni = -(b1 * s1 + b2 * s2), dr = 1 + a1 * c1 + a2 * c2, di = -(a1 * s1 + a2 * s2);
    double ph = atan2(ni, nr) - atan2(di, dr);
    return isfinite(ph) ? ph : 0;
}
static double tuned_loop_delay(double f, double fc, double Q, double sr) {
    double w = 2 * PI_D * f, d;
    if (!(w > 0)) return 0;
    d = (PI_D + lowpass_phase(f, fc, Q, sr)) / w;
    return d > 0 && isfinite(d) ? d : 1 / (2 * f);
}
static double wg_tuned(const WG *w, double f, double fc) {
    return tuned_loop_delay(dclamp(f, 10, w->fMax), dclamp(fc, 20, 0.49 * SR_HZ), w->loopQ, SR_HZ);
}
static float wg_fbmag(const WG *w, float load) {
    return clampf(w->reflection * (0.92f + 0.08f * clampf(load, 0, 1)), 0, 0.92f);
}
static void wg_init(WG *w, const Preset *p, float length, float reflection, float damping) {
    double minLoop = 128.0 / SR_HZ, maxHeat, shortest;
    memset(w, 0, sizeof(*w));
    w->length = clampf(length, 0.05f, 12);
    w->gasTemp = clampf(p->gasTemp, 0.5f, 2.5f);
    w->reflection = clampf(reflection, 0, 0.92f);
    w->damping = clampf(damping, 0.05f, 2);
    w->thermalSpan = 0.05f;
    w->thermalBias = 0.35f;
    w->nominalDelay = (float)(2 * (w->length > .05f ? w->length : .05f) / (C_SOUND * w->gasTemp));
    w->frequency = (float)(C_SOUND * w->gasTemp / (4 * (w->length > .05f ? w->length : .05f)));
    w->lpBase = clampf(w->frequency * (1.8f / w->damping), 150, 12000);
    w->loopQ = 0.5f;
    w->fMax = 20000;
    w->tunedDelay = (float)wg_tuned(w, w->frequency, w->lpBase);
    maxHeat = 1 + w->thermalSpan * (1 - w->thermalBias);
    shortest = wg_tuned(w, w->frequency * maxHeat, w->lpBase * maxHeat * 1.35);
    w->mode = shortest >= minLoop * 1.02;
    w->dry = 0.30f;
    w->wet = 0.90f;
    if (w->mode) {
        double maxT = w->nominalDelay / (1 - w->thermalSpan) * 1.25;
        if (maxT < 0.01) maxT = 0.01;
        w->maxDelayTime = (float)maxT;
        w->dl.buf = w->buf;
        w->dl.mask = 2047;
        sp_init(&w->delayT, (float)dclamp(w->tunedDelay, minLoop, maxT));
        bqn_init(&w->loopLP, BQ_LP, clampf(w->lpBase, 10, w->fMax), w->loopQ, 0);
        sp_init(&w->fb, -wg_fbmag(w, 0));
    } else {
        float a = wg_fbmag(w, 0), fq = clampf((float)(PI_D / (2 * (1 - a > .02f ? 1 - a : .02f))), .7f, 24), wsum = 0,
              wk[6], peak;
        int k;
        for (k = 0; k < 3; k++) {
            float f = w->frequency * (2 * k + 1), wall;
            if (f > w->fMax * 0.95f) break;
            wall = 1.f / sqrtf(1 + (f / w->lpBase) * (f / w->lpBase));
            wk[k] = powf(1.f / (2 * k + 1), 0.7f) * wall;
            wsum += wk[k];
        }
        w->nm = k;
        if (!k) {
            w->nm = 1;
            wk[0] = wsum = 1;
        }
        peak = 0.6f / (1 - a > .08f ? 1 - a : .08f);
        for (k = 0; k < w->nm; k++) {
            w->morder[k] = 2 * k + 1;
            bqn_init(&w->bp[k], BQ_BP, clampf(w->frequency * (2 * k + 1), 10, w->fMax), fq, 0);
            w->bpg[k] = wk[k] / wsum * peak;
        }
    }
}
static void wg_update(WG *w, float load, float rpmNorm, float tc) {
    float heat = clampf(0.6f * load + 0.4f * rpmNorm, 0, 1), scale = 1 + w->thermalSpan * (heat - w->thermalBias);
    int k;
    (void)tc;
    if (w->mode) {
        sp_set(&w->delayT, clampf(w->nominalDelay / scale, 128.f / SR_F, w->maxDelayTime), TC_DELAY);
        sp_set(&w->loopLP.f, clampf(w->lpBase * scale * (1 + 0.35f * load), 10, w->fMax), TC);
        sp_set(&w->fb, -wg_fbmag(w, load), TC);
    } else
        for (k = 0; k < w->nm; k++) sp_set(&w->bp[k].f, clampf(w->frequency * w->morder[k] * scale, 10, w->fMax), TC_DELAY);
}
static void wg_run(WG *w, const float *in, float *out) {
    int i, k;
    if (w->mode) {
        BQN *lp = &w->loopLP;
        for (i = 0; i < QN; i += SUB) {
            sp_fill(&lp->f, 0, SUB);
            sp_fill(&lp->q, 0, SUB);
            if (fabsf(lp->f.v - lp->cf) > 2e-6f * lp->cf) {
                lp->cf = lp->f.v;
                bq_coef(&lp->b, BQ_LP, lp->cf, lp->q.v, 0);
            }
            /* Chromium breaks the cycle where loopLP is re-entered: the feedback gain reads loopLP's output of
               the PREVIOUS render quantum, so the loop is delayTime + 128 frames long (measured against the
               browser: the pipe's modes sit at odd multiples of 1/(2(T + 128/sr))). */
            for (k = i; k < i + SUB; k++) {
                float D = sp_tick(&w->delayT) * SR_F, fbv = sp_tick(&w->fb), y;
                dl_write(&w->dl, in[k] + fbv * w->lprev[k]);
                y = bq_tick(&lp->b, dl_read(&w->dl, D));
                w->lprev[k] = y;
                out[k] = w->dry * in[k] + w->wet * y;
            }
        }
        bq_flush(&lp->b);
    } else {
        float tmp[QN];
        for (i = 0; i < QN; i++) out[i] = w->dry * in[i];
        for (k = 0; k < w->nm; k++) {
            bqn_run(&w->bp[k], in, tmp, QN, 0);
            for (i = 0; i < QN; i++) out[i] += w->wet * w->bpg[k] * tmp[i];
        }
    }
}

/* ------------------------------------------------------------------------------------------------ muffler */
static double peak_coincidence(const double *d, int n) {
    double cost = 0;
    int i, j, a, b;
    for (i = 0; i < n; i++)
        for (j = i + 1; j < n; j++)
            for (a = 1; a <= 6; a++)
                for (b = 1; b <= 6; b++) {
                    double fa = a / d[i], fb = b / d[j], x;
                    if (fa > 8000 || fb > 8000) continue;
                    x = (fa - fb) / 45;
                    cost += exp(-x * x) / (a * b);
                }
    return cost;
}
static void muffler_init(Muffler *m, const Preset *p) {
    double g = dclamp(p->gasTemp, .5, 2.5), base[3], out[3], peakProd = 1, mx = 0;
    int i, s;
    memset(m, 0, sizeof(*m));
    m->ns = 3;
    m->thermalSpan = 0.025f;
    m->thermalBias = 0.35f;
    for (i = 0; i < 3; i++) {
        double L = dclamp(p->muffler[i], 0.02, 3);
        base[i] = 2 * (L > .05 ? L : .05) / (C_SOUND * g);
        if (L > mx) mx = L;
    }
    for (i = 0; i < 3; i++) out[i] = base[i];
    for (i = 1; i < 3; i++) { /* _decorrelate, spread 0.06, 41 steps */
        double best = out[i], bestCost = 1e300, trial[3];
        for (s = 0; s < 41; s++) {
            double k = 1 + 0.06 * ((2.0 * s) / 40 - 1), c;
            int q;
            for (q = 0; q <= i; q++) trial[q] = out[q];
            trial[i] = base[i] * k;
            c = peak_coincidence(trial, i + 1);
            if (c < bestCost - 1e-12) {
                bestCost = c;
                best = base[i] * k;
            }
        }
        out[i] = best;
    }
    m->branchGain = (float)dclamp(0.55 + 0.30 * dclamp(p->reflection, 0, 1), 0.05, 0.9);
    m->packingBase = (float)dclamp(C_SOUND * g / (4 * (mx > .05 ? mx : .05)) * 7, 500, 11000);
    for (i = 0; i < 3; i++) {
        double maxT = out[i] / (1 - m->thermalSpan) * 1.5;
        if (maxT < 0.005) maxT = 0.005;
        m->T[i] = (float)out[i];
        m->maxT[i] = (float)maxT;
        m->taper[i] = powf(0.92f, (float)i);
        m->dl[i].buf = m->buf[i];
        m->dl[i].mask = 511;
        sp_init(&m->delayT[i], (float)out[i]);
        sp_init(&m->branch[i], m->branchGain * m->taper[i]);
        peakProd *= 1 + m->branchGain * m->taper[i];
    }
    bqn_init(&m->packing, BQ_LP, clampf(m->packingBase, 10, 20000), 0.6f, 0);
    m->trim = (float)(1 / (peakProd * 0.72 > 1 ? peakProd * 0.72 : 1));
}
static void muffler_update(Muffler *m, float load, float rpmNorm) {
    float heat = clampf(0.6f * load + 0.4f * rpmNorm, 0, 1), scale = 1 + m->thermalSpan * (heat - m->thermalBias);
    int i;
    for (i = 0; i < m->ns; i++) {
        sp_set(&m->delayT[i], clampf(m->T[i] / scale, 0, m->maxT[i]), TC_DELAY);
        sp_set(&m->branch[i], clampf(m->branchGain * m->taper[i] * (1 - 0.12f * load), 0, 0.95f), TC);
    }
    sp_set(&m->packing.f, clampf(m->packingBase * (1 + 0.30f * load) * scale, 10, 20000), TC);
}
static void muffler_run(Muffler *m, float *x) {
    int i, s;
    for (s = 0; s < m->ns; s++) {
        DL *d = &m->dl[s];
        for (i = 0; i < QN; i++) {
            float D = sp_tick(&m->delayT[s]) * SR_F, g = sp_tick(&m->branch[s]);
            dl_write(d, x[i]);
            x[i] = x[i] + g * dl_read(d, D);
        }
    }
    bqn_run(&m->packing, x, x, QN, 0);
    for (i = 0; i < QN; i++) x[i] *= m->trim;
}

/* ------------------------------------------------------------------------------------------------ exhaust */
static void exhaust_init(Exhaust *e, const Preset *p) {
    int i;
    memset(e, 0, sizeof(*e));
    e->nb = p->nbanks;
    e->collectorGain = 1.f / sqrtf((float)e->nb);
    for (i = 0; i < e->nb; i++) {
        float L = (i == 1 && p->exBankB > 0) ? p->exBankB : p->exBank;
        float t;
        wg_init(&e->bank[i], p, L, p->reflection, p->damping);
        t = e->bank[i].nominalDelay / 2;
        e->trT[i] = t;
        e->trMax[i] = 2 * t > 0.005f ? 2 * t : 0.005f;
        e->tr[i].buf = e->trbuf[i];
        e->tr[i].mask = 511;
        sp_init(&e->trDelay[i], t);
    }
    wg_init(&e->collector, p, p->exCollector, clampf(p->reflection * 0.7f, 0, 0.92f), clampf(p->damping * 1.5f, .05f, 2));
    muffler_init(&e->muf, p);
    e->thermalSpan = e->bank[0].thermalSpan;
    e->thermalBias = e->bank[0].thermalBias;
    e->nl.maxDrive = 4.5f;
    e->nl.idleDrive = 1.0f;
    sp_init(&e->nl.drive, 1.0f);
    sp_init(&e->nl.makeup, 1.0f);
    ws_init(&e->nl.shaper, SHOCK, 4096, 4);
    bqn_init(&e->nl.dcBlock, BQ_HP, 22, 0.7f, 0);
}
static void exhaust_update(Exhaust *e, const Params *p, float rpmNorm) {
    float load = clampf(p->load, 0, 1), heat = clampf(0.6f * load + 0.4f * rpmNorm, 0, 1),
          scale = 1 + e->thermalSpan * (heat - e->thermalBias), boost = clampf(p->boost, 0, 1),
          overrun = clampf(p->overrun, 0, 1), amp, od, d;
    int i;
    for (i = 0; i < e->nb; i++) {
        wg_update(&e->bank[i], load, rpmNorm, TC);
        sp_set(&e->trDelay[i], clampf(e->trT[i] / scale, 0, e->trMax[i]), TC_DELAY);
    }
    wg_update(&e->collector, load, rpmNorm, TC);
    muffler_update(&e->muf, load, rpmNorm);
    amp = clampf(powf(load, 1.5f) * (0.55f + 0.45f * rpmNorm) + 0.55f * boost * load, 0, 1.4f);
    od = 0.25f * overrun * (0.3f + 0.7f * rpmNorm);
    d = clampf(e->nl.idleDrive + (e->nl.maxDrive - e->nl.idleDrive) * clampf(amp + od, 0, 1), 0.1f, e->nl.maxDrive);
    sp_set(&e->nl.drive, d, TC);
    sp_set(&e->nl.makeup, clampf(1.f / powf(d > .05f ? d : .05f, 0.70f), 0.05f, 4), TC);
}
static void exhaust_run(Exhaust *e, float in[2][QN], float *out) {
    float tmp[QN], col[QN];
    int b, i;
    for (i = 0; i < QN; i++) col[i] = 0;
    for (b = 0; b < e->nb; b++) {
        wg_run(&e->bank[b], in[b], tmp);
        for (i = 0; i < QN; i++) {
            float D = sp_tick(&e->trDelay[b]) * SR_F;
            dl_write(&e->tr[b], tmp[i]);
            col[i] += dl_read(&e->tr[b], D) * e->collectorGain;
        }
    }
    wg_run(&e->collector, col, tmp);
    muffler_run(&e->muf, tmp);
    for (i = 0; i < QN; i++) tmp[i] *= sp_tick(&e->nl.drive) * (1.f / 6.f);
    ws_run(&e->nl.shaper, tmp, tmp, QN);
    bqn_run(&e->nl.dcBlock, tmp, tmp, QN, 0);
    for (i = 0; i < QN; i++) out[i] = tmp[i] * sp_tick(&e->nl.makeup);
}

/* ------------------------------------------------------------------------------------------------ intake */
static void intake_init(Intake *n, const Preset *p) {
    memset(n, 0, sizeof(*n));
    n->f0 = clampf(p->helmholtz, 10, 2000);
    n->q0 = clampf(p->intakeQ, 0.3f, 30);
    n->level = clampf(p->intakeLevel, 0, 4);
    sp_init(&n->out, n->level);
    bqn_init(&n->helm, BQ_BP, n->f0, n->q0, 0);
    sp_init(&n->helmGain, 0);
    bqn_init(&n->directLP, BQ_LP, n->f0 * 4, 0.7f, 0);
    sp_init(&n->directGain, 0);
    nsrc_init(&n->noise, INTAKE_N, NB_LEN, 0);
    bqn_init(&n->noiseHP, BQ_HP, n->f0 * 0.8f, 0.7f, 0);
    bqn_init(&n->noiseBP, BQ_BP, n->f0 * 3, 0.9f, 0);
    sp_init(&n->noiseGain, 0);
    sp_init(&n->noiseToHelm, 0);
}
static void intake_update(Intake *n, const Params *p, float rpmNorm) {
    float load = clampf(p->load, 0, 1), thr = clampf(p->throttle, 0, 1), ov = clampf(p->overrun, 0, 1);
    float excite = powf(load > 1e-3f ? load : 1e-3f, 0.7f), flow = clampf(thr * (0.25f + 0.75f * rpmNorm), 0, 1),
          coast = clampf(ov * (0.15f + 0.85f * rpmNorm), 0, 1);
    sp_set(&n->helm.q, clampf(n->q0 * (0.55f + 0.45f * thr), 0.3f, 30), 0.05f);
    sp_set(&n->helm.f, n->f0, 0.1f);
    sp_set(&n->helmGain, clampf((0.12f + 0.88f * excite) * (0.35f + 0.65f * rpmNorm), 0, 4), TC);
    sp_set(&n->directGain, clampf(0.30f * (0.2f + 0.8f * excite), 0, 4), TC);
    sp_set(&n->noiseBP.f, hz(n->f0 * (3 + 9 * flow) * (1 + 0.6f * coast), 10), TC);
    sp_set(&n->noiseBP.q, clampf(0.9f + 0.7f * flow, 0.3f, 6), 0.05f);
    sp_set(&n->noiseGain, clampf(0.42f * powf(flow, 1.2f) + 0.16f * coast, 0, 4), TC);
    sp_set(&n->noiseToHelm, clampf(0.20f * flow + 0.05f * coast, 0, 2), TC);
    sp_set(&n->out, n->level, 0.1f);
}
static void intake_run(Intake *n, const float *in, float *out) {
    float nz[QN], hp[QN], h[QN], d[QN], bp[QN];
    int i;
    nsrc_run(&n->noise, nz, QN);
    bqn_run(&n->noiseHP, nz, hp, QN, 0);
    for (i = 0; i < QN; i++) h[i] = in[i] + hp[i] * sp_tick(&n->noiseToHelm);
    bqn_run(&n->helm, h, h, QN, 0);
    bqn_run(&n->directLP, in, d, QN, 0);
    bqn_run(&n->noiseBP, hp, bp, QN, 0);
    for (i = 0; i < QN; i++)
        out[i] = (h[i] * sp_tick(&n->helmGain) + d[i] * sp_tick(&n->directGain) + bp[i] * sp_tick(&n->noiseGain)) *
                 sp_tick(&n->out);
}

/* ------------------------------------------------------------------------------------------------ character */
static void exn_init(ExNoise *e, const Preset *p) {
    memset(e, 0, sizeof(*e));
    e->pulseOrder = (float)p->n / 2 / (p->nbanks > 1 ? p->nbanks : 1);
    if (e->pulseOrder < 0.5f) e->pulseOrder = 0.5f;
    e->gain = 1;
    nsrc_init(&e->src, EXN, 129600, 0);
    bqn_init(&e->hp, BQ_HP, 55, 1, 0);
    bqn_init(&e->body, BQ_LP, 900, 0.9f, 0);
    bqn_init(&e->peak, BQ_PK, 140, 1.4f, 7);
    sp_init(&e->level, 0);
    sp_init(&e->pfreq, 40);
    sp_init(&e->pdepth, 0);
    e->pulse.w = &WAVE_SAW;
}
static void exn_update(ExNoise *e, const Params *p, float rpmNorm) {
    float load = clampf(p->load, 0, 1), ov = clampf(p->overrun, 0, 1), f0 = clampf(finf(p->f0, 10), 0.05f, 4000);
    float fire = clampf(f0 * e->pulseOrder * 2, 10, 6000), flow = 0.30f + 0.70f * load;
    float lvl = e->gain * 0.28f * flow * (0.25f + 0.75f * rpmNorm) * (1 + 0.35f * ov), d1 = lvl * 0.85f,
          d2 = lvl * (1.1f - 0.7f * rpmNorm);
    sp_set(&e->level, lvl, TC);
    sp_set(&e->pfreq, fire, TC);
    sp_set(&e->pdepth, d1 < d2 ? d1 : d2, TC);
    sp_set(&e->peak.f, clampf(fire, 30, 2000), TC);
    sp_set(&e->body.f, clampf(450 + 2600 * load + 1800 * rpmNorm, 120, 9000), TC);
    sp_set(&e->hp.f, clampf(40 + 30 * rpmNorm, 20, 400), TC);
}
static void exn_run(ExNoise *e, float *out) {
    float x[QN], fr[QN], saw[QN];
    int i;
    nsrc_run(&e->src, x, QN);
    bqn_run(&e->hp, x, x, QN, 0);
    bqn_run(&e->peak, x, x, QN, 0);
    bqn_run(&e->body, x, x, QN, 0);
    sp_fill(&e->pfreq, fr, QN);
    osc_run(&e->pulse, fr, saw, QN);
    for (i = 0; i < QN; i++) {
        float g = sp_tick(&e->level) + sp_tick(&e->pdepth) * saw[i];
        out[i] = x[i] * g;
    }
}
static void sub_init(SubL *s, const Preset *p) {
    float order = (float)p->n / 4, fMid = ((p->idle + p->redline) / 2) / 120;
    memset(s, 0, sizeof(*s));
    if (order < 0.5f) order = 0.5f;
    while (order * fMid < 38 && order < 8) order *= 2;
    while (order * fMid > 85 && order > 0.5f) order /= 2;
    s->order = order;
    s->osc.w = &WAVE_SINE;
    sp_init(&s->freq, 40);
    sp_init(&s->level, 0);
    ws_init(&s->shaper, SUBC, 2048, 2);
    bqn_init(&s->tone, BQ_LP, 220, 0.7f, 0);
}
static void sub_update(SubL *s, const Params *p, float rpmNorm) {
    float f0 = clampf(finf(p->f0, 10), 0.05f, 4000), load = clampf(p->load, 0, 1), ov = clampf(p->overrun, 0, 1);
    sp_set(&s->freq, clampf(f0 * s->order, 12, 400), TC);
    sp_set(&s->level, 0.34f * (0.18f + 0.82f * powf(load, 0.75f)) * (1 - 0.45f * rpmNorm) * (1 - 0.6f * ov), TC);
    sp_set(&s->tone.f, clampf(150 + 260 * load, 80, 800), TC);
}
static void sub_run(SubL *s, float *out) {
    float fr[QN], x[QN];
    int i;
    sp_fill(&s->freq, fr, QN);
    osc_run(&s->osc, fr, x, QN);
    ws_run(&s->shaper, x, x, QN);
    bqn_run(&s->tone, x, x, QN, 0);
    for (i = 0; i < QN; i++) out[i] = x[i] * sp_tick(&s->level);
}
static const float LFO_RATES[3] = {0.237f, 1.703f, 5.317f};
static void char_init(Character *c) {
    int i;
    memset(c, 0, sizeof(*c));
    for (i = 0; i < 3; i++) c->lfo[i].w = &WAVE_SINE;
    nsrc_init(&c->rough, BROWN, 148800, 0);
    bqn_init(&c->roughLP, BQ_LP, 22, 1, 0);
    sp_init(&c->trem, 0);
}
static void char_update(Character *c, const Params *p, float rpmNorm) {
    float load = clampf(p->load, 0, 1), lumpy = (1 - rpmNorm) * (1 - 0.55f * load);
    sp_set(&c->trem, 0.2f * 0.10f * (0.2f + 0.8f * lumpy), 0.08f);
    sp_set(&c->roughLP.f, clampf(8 + 40 * rpmNorm, 4, 200), 0.1f);
}
static void char_run(Character *c, float *trem) {
    float bus[QN], x[QN], fr[QN];
    int i, k;
    nsrc_run(&c->rough, x, QN);
    bqn_run(&c->roughLP, x, x, QN, 0);
    for (i = 0; i < QN; i++) bus[i] = 0.9f * x[i];
    for (k = 0; k < 3; k++) {
        for (i = 0; i < QN; i++) fr[i] = LFO_RATES[k];
        osc_run(&c->lfo[k], fr, x, QN);
        for (i = 0; i < QN; i++) bus[i] += x[i] * (1.f / 3);
    }
    for (i = 0; i < QN; i++) trem[i] = bus[i] * sp_tick(&c->trem);
}

/* ------------------------------------------------------------------------------------------------ transmission */
static void trans_init(Trans *t, const Preset *p) {
    int i;
    memset(t, 0, sizeof(*t));
    nsrc_init(&t->white, WHITE, NB_LEN, 0.71);
    nsrc_init(&t->pink, PINK, NB_LEN, 1.31);
    t->mesh1.w = t->mesh2.w = t->runout.w = &WAVE_SINE;
    t->tors.w = &WAVE_SAW;
    sp_init(&t->f1, 500);
    sp_init(&t->f2, 1000);
    sp_init(&t->frun, 20);
    sp_init(&t->ftors, 60);
    sp_init(&t->meshH2, 0.20f);
    sp_init(&t->meshGain, 0);
    sp_init(&t->ratGain, 0);
    bqn_init(&t->meshBP, BQ_BP, 500, 3.5f, 0);
    bqn_init(&t->ratBP1, BQ_BP, 1800, 2.0f, 0);
    bqn_init(&t->ratBP2, BQ_BP, 3600, 2.5f, 0);
    t->nteeth = p->gears;
    for (i = 0; i < p->gears; i++) t->teeth[i] = p->teeth[i];
    t->finalDrive = 3.73f;
    t->cyl = clampf((float)p->n, 1, 16);
    t->idleRpm = p->idle;
}
static void trans_update(Trans *t, const Params *p) {
    int gear = p->gear;
    float load = clampf(p->load, 0, 1), rpm = p->rpm > 0 ? p->rpm : 0, f0 = finf(p->f0, rpm / 120);
    float engaged = clampf(p->clutchEngaged, 0, 1), shaftRpm = fabsf(p->wheelRpm), mesh = 0, band = 0, toothLoad,
          inGear, phaseMute, speedRise, whine, dRpmTerm, slipTerm, nearIdle, idleRattle, lashOpen, drive, r;
    int gi = gear - 1;
    if (gi >= t->nteeth) gi = t->nteeth - 1; /* gears beyond the designed box reuse the top gear's teeth */
    if (gi >= 0) mesh = shaftRpm / 60 * t->teeth[gi];
    if (mesh > 0) {
        float hfRoll = 1.f / (1 + powf(mesh / 1800, 2.4f));
        band = clampf((mesh - 30) / 40, 0, 1) * hfRoll;
    }
    toothLoad = p->torqueSign >= 0 ? 0.22f + 0.85f * load : 0.34f;
    inGear = (gear > 0 && fabsf(p->gearRatio) > 1e-3f) ? 1.f : 0.f;
    phaseMute = (p->phase == PH_OPEN || p->phase == PH_SYNC || p->phase == PH_CUT) ? 0.05f : 1.f;
    speedRise = 0.45f + 0.55f * clampf(shaftRpm / 2500, 0, 1);
    whine = 0.065f * 0.42f * inGear * engaged * phaseMute * toothLoad * band * speedRise;
    sp_set(&t->f1, hz(mesh, 500), TC);
    sp_set(&t->f2, hz(mesh * 2, 1000), TC);
    sp_set(&t->meshH2, 0.20f / (1 + powf(mesh * 2 / 2600, 2.4f)), TC);
    sp_set(&t->meshBP.f, hz(mesh, 500), TC);
    sp_set(&t->frun, hz(shaftRpm / 60 * 2, 10), TC);
    sp_set(&t->meshGain, whine, TC);
    dRpmTerm = clampf(fabsf(p->dRpm) / 4000, 0, 1);
    slipTerm = clampf(fabsf(p->clutchSlip) / 800, 0, 1);
    nearIdle = clampf(1 - (rpm - t->idleRpm) / (200 > t->idleRpm * 1.6f ? 200 : t->idleRpm * 1.6f), 0, 1);
    idleRattle = engaged * nearIdle * (1 - 0.7f * load) * (inGear > 0 ? 0.4f : 1);
    lashOpen = 1 - 0.65f * load * inGear;
    drive = clampf(0.5f * idleRattle + 0.75f * dRpmTerm + 0.7f * slipTerm, 0, 1) * lashOpen;
    sp_set(&t->ftors, hz(t->cyl * f0, 10), TC);
    r = t->idleRpm * 0.6f;
    sp_set(&t->ratGain, 0.09f * drive * clampf(rpm / (r > 1 ? r : 1), 0, 1), TC);
}
static void trans_run(Trans *t, float *out) {
    float w[QN], pk[QN], fr[QN], o1[QN], o2[QN], run[QN], a[QN], b[QN];
    int i;
    nsrc_run(&t->white, w, QN);
    nsrc_run(&t->pink, pk, QN);
    if (t->meshGain.v == 0 && t->meshGain.t == 0 && t->ratGain.v == 0 && t->ratGain.t == 0) {
        /* silent: keep phases/filters moving cheaply */
        for (i = 0; i < QN; i++) out[i] = 0;
        sp_fill(&t->f1, fr, QN);
        osc_run(&t->mesh1, fr, o1, QN);
        sp_fill(&t->f2, fr, QN);
        osc_run(&t->mesh2, fr, o2, QN);
        sp_fill(&t->frun, fr, QN);
        osc_run(&t->runout, fr, run, QN);
        sp_fill(&t->ftors, fr, QN);
        osc_run(&t->tors, fr, run, QN);
        bqn_run(&t->meshBP, w, a, QN, 0);
        bqn_run(&t->ratBP1, pk, a, QN, 0);
        bqn_run(&t->ratBP2, pk, a, QN, 0);
        return;
    }
    sp_fill(&t->f1, fr, QN);
    osc_run(&t->mesh1, fr, o1, QN);
    sp_fill(&t->f2, fr, QN);
    osc_run(&t->mesh2, fr, o2, QN);
    sp_fill(&t->frun, fr, QN);
    osc_run(&t->runout, fr, run, QN);
    bqn_run(&t->meshBP, w, a, QN, 0);
    for (i = 0; i < QN; i++) {
        float am = 0.85f + 0.15f * run[i];
        out[i] = (o1[i] + o2[i] * sp_tick(&t->meshH2) + a[i] * 0.60f) * am * sp_tick(&t->meshGain);
    }
    sp_fill(&t->ftors, fr, QN);
    osc_run(&t->tors, fr, run, QN);
    bqn_run(&t->ratBP1, pk, a, QN, 0);
    bqn_run(&t->ratBP2, pk, b, QN, 0);
    for (i = 0; i < QN; i++) out[i] += (a[i] + 0.5f * b[i]) * (0.4f + 0.6f * run[i]) * sp_tick(&t->ratGain);
}

/* ------------------------------------------------------------------------------------------------ turbo */
#define TURBO_AIR_HZ 3800
#define TURBO_AIR_DB -5.5f
#define TURBO_WHINE_MAX_HZ 1150
/* Owner's taste for this car (deviation from Engine_Sim, whose turbo is a showpiece): the turbo is a faint
 * background layer that only comes up near full boost, and the BMW recirculating diverter valve makes a short soft
 * whoosh with no compressor flutter. TURBO_TRIM scales whistle/spool/rush at full spool (audibility ramps in with
 * spool from TURBO_ONSET), BOV_TRIM the blow-off. Calibrated: see README "Turbo level". */
#define TURBO_TRIM 0.085f
#define TURBO_ONSET 0.45f
#define BOV_TRIM 3.0f
/* ES_VALIDATE=1 restores Engine_Sim's own turbo (atmospheric valve, flutter, full level) for the comparison */
static int validating(void) {
    static int v = -1;
    if (v < 0) v = getenv("ES_VALIDATE") != 0;
    return v;
}
static void turbo_reset(Turbo *u, const Preset *p) {
    float redline = p->redline, order = 0.5f * 52;
    memset(u, 0, sizeof(*u));
    u->liftRate = 13;
    u->liftPitch = 1;
    u->liftTau = 0.26f;
    u->rnd = 0x51ed270bu;
    u->inertia = 0.42f;
    u->bovLevel = 0.40f;
    u->surgeTrim = 1;
    u->valve = validating() ? 1 : 2; /* recirculating diverter valve (BMW) */
    u->flutterHz = 13;
    u->boostRpm = 3500 < 0.65f * redline ? 3500 : 0.65f * redline;
    u->whineRef = clampf(redline / 120 * order, 200, TURBO_WHINE_MAX_HZ);
    nsrc_init(&u->white, WHITE, NB_LEN, 1.07);
    nsrc_init(&u->grainN, PINK, NB_LEN, 0.29);
    nsrc_init(&u->airN, PINK, NB_LEN, 1.31);
    u->whine.w = &WAVE_BLADE;
    u->wander1.w = u->wander2.w = u->shaft.w = u->hi.w = u->fw.w = &WAVE_SINE;
    u->wg.w = u->buzz.w = u->surgeOsc.w = &WAVE_SAW;
    sp_init(&u->out, 1);
    sp_init(&u->surgeSend, 0.55f);
    sp_init(&u->audible, 0);
    sp_init(&u->bovx, 1);
    sp_init(&u->whineF, 1200);
    sp_init(&u->wd1, 0);
    sp_init(&u->wd2, 0);
    sp_init(&u->whineDrive, 1);
    sp_init(&u->shaftF, 180);
    sp_init(&u->shaftDepth, 0);
    sp_init(&u->grainDepth, 0);
    sp_init(&u->whineGain, 0);
    sp_init(&u->hissGain, 0);
    sp_init(&u->hissGrain, 0);
    sp_init(&u->whineNoiseGain, 0);
    sp_init(&u->airflowGain, 0);
    sp_init(&u->wgF, 24);
    sp_init(&u->wgLevel, 0);
    sp_init(&u->wgGrain, 0);
    sp_init(&u->hiF, 6000);
    sp_init(&u->hiGain, 0);
    sp_init(&u->hiGrain, 0);
    sp_init(&u->hiWander, 0);
    sp_init(&u->buzzF, 60);
    sp_init(&u->buzzGain, 0);
    sp_init(&u->surgeF, 16);
    sp_init(&u->surgeJitter, 0);
    sp_init(&u->whooshGain, 0);
    sp_init(&u->whooshGrain, 0);
    sp_init(&u->surgeBodyLvl, 0);
    sp_init(&u->surgeEdgeLvl, 0);
    sp_init(&u->surgeChirp, 0);
    sp_init(&u->fwF, 7000);
    sp_init(&u->hiSurgeLvl, 0);
    sp_init(&u->hiChirp, 0);
    sp_init(&u->hiChirpN, 0);
    ws_init(&u->whineShaper, GRAIN, 1024, 4);
    ws_init(&u->wgShaper, SURGEC, 1024, 2);
    ws_init(&u->surgeShaper, SURGEC, 1024, 2);
    bqn_init(&u->airLP, BQ_LP, TURBO_AIR_HZ, -3.01f, 0);
    bqn_init(&u->airShelf, BQ_HS, 2600, 1, TURBO_AIR_DB);
    bqn_init(&u->whineBP, BQ_BP, 1500, 1.5f, 0);
    bqn_init(&u->grainBP, BQ_BP, 55, 0.6f, 0);
    bqn_init(&u->hissBP, BQ_BP, 2000, 0.8f, 0);
    bqn_init(&u->whineNoiseBP, BQ_BP, 800, 6, 0);
    bqn_init(&u->airflowHP, BQ_HP, 280, 0.5f, 0);
    bqn_init(&u->airflowLP, BQ_LP, 1400, 0.5f, 0);
    bqn_init(&u->wgBP, BQ_BP, 1500, 0.6f, 0);
    bqn_init(&u->buzzBP, BQ_BP, 1500, 1.2f, 0);
    bqn_init(&u->bovHissHP, BQ_HP, 1800, -3.01f, 0);
    bqn_init(&u->surgeLP, BQ_LP, 6500, -3.01f, 0);
    bqn_init(&u->whooshBP, BQ_BP, 5000, 0.7f, 0);
    bqn_init(&u->surgeBody, BQ_BP, 3000, 1.1f, 0);
    bqn_init(&u->fwBP, BQ_BP, 7000, 3, 0);
    bqn_init(&u->surgeEdge, BQ_BP, 5200, 0.8f, 0);
    ap_init(&u->bovF, 2400);
    ap_init(&u->bovQ, 7);
    ap_init(&u->bovGain, 0);
    ap_init(&u->bovHiss, 0);
    u->bovcf = 2400;
    u->bovcq = 7;
    bq_coef(&u->bovBP, BQ_BP, 2400, 7, 0);
}
static void turbo_fire_bov(Turbo *u, double now, float strength) {
    float stored = clampf(u->boost, 0, 1), amp, dur;
    double t;
    int recirc = u->valve == 2;
    if (u->valve == 0) return;
    amp = clampf(u->bovLevel * 1.35f * strength * (0.15f + 0.85f * stored) * (recirc ? 0.3f : 1), 0.0005f, 2.5f);
    dur = clampf(0.11f + 0.26f * stored, 0.05f, 0.5f);
    t = now + LOOKAHEAD;
    ap_cancel(&u->bovF, t);
    ap_push(&u->bovF, EV_SET, t, hz((recirc ? 1300 : 2600) + 1800 * stored, 2800), 0);
    ap_push(&u->bovF, EV_EXP, t + dur * 0.8, hz((recirc ? 300 : 520) + 180 * stored, 620), 0);
    ap_cancel(&u->bovQ, t);
    ap_push(&u->bovQ, EV_SET, t, recirc ? 1.5f : 4.5f, 0);
    ap_push(&u->bovQ, EV_LIN, t + dur * 0.6, recirc ? 3 : 11, 0);
    ap_cancel(&u->bovHiss, t);
    if (!recirc) { /* layers.js: only an atmospheric valve vents the "pssh" */
        ap_push(&u->bovHiss, EV_SET, t, 0.0004f, 0);
        ap_push(&u->bovHiss, EV_LIN, t + 0.008, amp * 0.45f, 0);
        ap_push(&u->bovHiss, EV_EXP, t + dur * 0.9, 0.0004f, 0);
        ap_push(&u->bovHiss, EV_SET, t + dur, 0, 0);
    }
    ap_cancel(&u->bovGain, t);
    ap_push(&u->bovGain, EV_SET, t, 0.0004f, 0);
    ap_push(&u->bovGain, EV_LIN, t + 0.012, amp, 0);
    ap_push(&u->bovGain, EV_EXP, t + dur, 0.0004f, 0);
    ap_push(&u->bovGain, EV_SET, t + dur + 0.005, 0, 0);
    u->vent = clampf(u->bovLevel, 0.15f, 1);
    u->spool *= 0.97f;
}
static double trnd(Turbo *u) {
    return xs_unit(&u->rnd);
}
static void turbo_update(Turbo *u, const Params *p, float rpmNorm, float redline) {
    double now = p->now;
    float dt = clampf(finf(p->dt, 1.f / 60), 0, 0.25f), load = clampf(p->load, 0, 1),
          throttle = clampf(p->throttle, 0, 1), rpm = p->rpm;
    int shifting = p->shifting;
    float full, onGate, flowLoad, drive, climbNow, tau, alpha, floor_, raw, whineHz, aero, flow, whineLvl, wander,
        rough, nearCeiling, gate, rush, nearSurge, whoosh, buzz, hi, bov;
    if (!u->on || u->releasing) return;
    full = 0.6f * redline > u->boostRpm + 1500 ? 0.6f * redline : u->boostRpm + 1500;
    onGate = clampf((rpm - u->boostRpm) / (full - u->boostRpm), 0, 1);
    flowLoad = p->limiter ? (load > 0.95f * throttle ? load : 0.95f * throttle) : load;
    drive = clampf(flowLoad * (0.08f + 0.92f * onGate), 0, 1);
    climbNow = clampf((drive - u->spool) * 3, 0, 1);
    u->climb += (climbNow - u->climb) * (dt / 0.08f < 1 ? dt / 0.08f : 1);
    tau = drive > u->spool ? u->inertia * 1.1f : u->inertia * (throttle < 0.12f ? 0.45f : 1.3f);
    alpha = dt > 0 ? 1 - expf(-dt / (tau > 1e-3f ? tau : 1e-3f)) : 0;
    u->spool += (drive - u->spool) * alpha;
    floor_ = 0.04f + 0.07f * rpmNorm;
    if (u->spool < floor_) u->spool = floor_;
    u->spool = clampf(finf(u->spool, floor_), 0, 1);
    if (u->vent > 0) {
        u->vent *= expf(-dt / 0.18f);
        if (u->vent < 1e-3f) u->vent = 0;
    }
    raw = u->spool * u->spool;
    u->boost = clampf(raw * (1 - 0.92f * u->vent), 0, 1);
    whineHz = u->whineRef * powf(u->spool > 0.05f ? u->spool : 0.05f, 1.55f);
    if (whineHz < 120) whineHz = 120;
    sp_set(&u->whineF, hz(whineHz, 1000), TC);
    sp_set(&u->whineBP.f, hz(whineHz * 1.25f, 1600), TC);
    sp_set(&u->whineNoiseBP.f, hz(whineHz, 800), TC);
    aero = u->spool * u->spool;
    flow = clampf(load > throttle * 0.7f ? load : throttle * 0.7f, 0, 1);
    whineLvl = 0.085f * aero * (0.62f + 0.38f * flow);
    sp_set(&u->whineGain, whineLvl, TC);
    sp_set(&u->whineNoiseGain, 0.30f * aero * (0.5f + 0.5f * flow), TC);
    sp_set(&u->airflowGain, 0.20f * (0.15f + 0.85f * u->spool) * u->spool * (0.25f + 0.75f * flow), 0.05f);
    sp_set(&u->airflowLP.f, hz(900 + 1300 * u->spool, 1400), TC);
    wander = 26 * (1 - 0.75f * u->spool);
    sp_set(&u->wd1, wander, 0.08f);
    sp_set(&u->wd2, wander * 0.4f, 0.08f);
    rough = 0.55f - 0.30f * u->spool;
    sp_set(&u->grainDepth, whineLvl * rough, 0.05f);
    sp_set(&u->whineDrive, 0.8f + 0.9f * u->spool, TC);
    sp_set(&u->grainBP.f, hz(28 + 120 * u->spool, 55), TC);
    sp_set(&u->shaftF, hz(clampf(whineHz / 11, 4, 400), 180), TC);
    sp_set(&u->shaftDepth, whineLvl * (0.52f - 0.28f * u->spool), TC);
    nearCeiling = clampf((u->spool - 0.40f) / 0.30f, 0, 1) * clampf((rpmNorm - 0.40f) / 0.30f, 0, 1);
    gate = nearCeiling * throttle * 0.7f;
    sp_set(&u->wgF, hz(18 + 12 * nearCeiling + 8 * rpmNorm, 24), 0.05f);
    sp_set(&u->wgBP.f, hz(1400 + 1300 * u->spool, 1500), TC);
    sp_set(&u->wgLevel, gate, 0.05f);
    sp_set(&u->wgGrain, gate * 0.5f, 0.05f);
    rush = 0.10f * aero * (0.45f + 0.55f * flow);
    sp_set(&u->hissGain, rush, TC);
    sp_set(&u->hissGrain, rush * 0.65f, 0.05f);
    sp_set(&u->hissBP.f, hz(clampf(whineHz * 2.2f, 900, 4200), 2000), TC);
    nearSurge = clampf((u->boost - 0.12f) / 0.45f, 0, 1) * throttle * (1 - 0.75f * rpmNorm);
    whoosh = 0.18f * (nearSurge + 0.6f * u->climb * sqrtf(u->spool));
    sp_set(&u->whooshGain, whoosh, 0.04f);
    sp_set(&u->whooshGrain, whoosh * 0.6f, 0.05f);
    sp_set(&u->whooshBP.f, hz(clampf(4000 + 3500 * u->spool, 3000, 8000), 5000), TC);
    buzz = 0.08f * clampf((u->spool - 0.62f) / 0.25f, 0, 1) * flow;
    sp_set(&u->buzzF, hz(clampf(whineHz / 11, 20, 400), 60), TC);
    sp_set(&u->buzzBP.f, hz(clampf(whineHz * 1.5f, 400, 6000), 1500), TC);
    sp_set(&u->buzzGain, buzz, TC);
    sp_set(&u->hiF, hz(clampf(5000 + 8000 * powf(u->spool, 1.4f), 3000, 15000), 6000), TC);
    hi = 0.022f * aero * (0.55f + 0.45f * flow);
    sp_set(&u->hiGain, hi, TC);
    sp_set(&u->hiGrain, hi * 0.4f, 0.05f);
    sp_set(&u->hiWander, 12 * (1 - 0.6f * u->spool), 0.08f);
    {
        float a = clampf((u->spool - TURBO_ONSET) / (1 - TURBO_ONSET), 0, 1);
        sp_set(&u->audible, validating() ? 1.f : TURBO_TRIM * a * a * (3 - 2 * a) * clampf(p->xturbo, 0, 2), 0.1f);
        sp_set(&u->bovx, (validating() ? 1.f : BOV_TRIM) * clampf(p->xbov, 0, 2), 0.05f);
    }
    bov = clampf(p->evBov, 0, 1);
    if (bov > 0) turbo_fire_bov(u, now, bov);
    { /* _stepSurge */
        const float BODY = 6.5f, FW = 1.6f;
        float head, relief, stall, arm, s, rate, overrun, engineMix, amp, fw;
        int closed, justClosed, dropped, shiftCut, reopening;
        u->surge *= expf(-dt / u->liftTau);
        u->closedT = throttle < 0.10f ? u->closedT + dt : 0;
        if (u->surge < 1e-3f) u->surge = 0;
        closed = throttle < 0.10f;
        justClosed = closed && u->prevThrottle >= 0.25f;
        dropped = (u->prevThrottle - throttle) > 0.2f && throttle < 0.5f;
        shiftCut = shifting && !u->prevShifting;
        head = clampf((u->spool - 0.12f) / 0.28f, 0, 1);
        relief = u->valve == 0 ? 1 : clampf(1 - 0.9f * u->bovLevel, 0.15f, 1);
        stall = head * relief * (validating() ? 1 : 0); /* no compressor surge: the diverter valve recirculates (owner's request) */
        arm = (justClosed || dropped) ? stall : shiftCut ? stall * 1.f /* manual */ : 0;
        if (arm > 0) {
            if (u->surge < 0.1f) {
                u->liftRate = u->flutterHz * (0.7f + 0.45f * rpmNorm + 0.35f * u->boost) * (float)(0.85 + 0.3 * trnd(u));
                u->liftPitch = (0.85f + 0.25f * rpmNorm) * (float)(0.92 + 0.16 * trnd(u));
                u->liftTau = (float)(0.10 + 0.10 * trnd(u) + 0.04 * u->boost);
            }
            if (arm > u->surge) u->surge = arm;
        }
        if (closed && stall > 0.06f && u->closedT < 0.15f && stall * 0.8f > u->surge) u->surge = stall * 0.8f;
        reopening = throttle > 0.5f || throttle > u->prevThrottle + 0.05f;
        if (!closed && !shifting && reopening) u->surge *= expf(-dt / 0.11f);
        s = clampf(u->surge, 0, 1);
        rate = u->liftRate * (0.7f + 0.45f * s);
        sp_set(&u->surgeF, hz(rate, 16), 0.03f);
        sp_set(&u->surgeBody.f, hz((2400 + 700 * s + 500 * rpmNorm) * u->liftPitch, 3000), 0.04f);
        sp_set(&u->surgeEdge.f, hz((4600 + 600 * s + 600 * rpmNorm) * u->liftPitch, 5200), 0.04f);
        sp_set(&u->surgeChirp, 600 * s, 0.04f);
        sp_set(&u->surgeJitter, 18 * s, 0.04f);
        overrun = clampf(p->overrun, 0, 1);
        engineMix = (0.6f + 0.4f * rpmNorm) * (0.8f + 0.3f * overrun);
        amp = u->surgeTrim;
        sp_set(&u->surgeBodyLvl, BODY * amp * s * engineMix, 0.03f);
        sp_set(&u->surgeEdgeLvl, 0.25f * amp * s * s * engineMix, 0.03f);
        fw = clampf(5500 + 3500 * u->spool, 4500, 9500);
        sp_set(&u->fwF, hz(fw, 7000), 0.03f);
        sp_set(&u->fwBP.f, hz(fw, 7000), 0.03f);
        sp_set(&u->hiSurgeLvl, FW * amp * s * engineMix, 0.03f);
        sp_set(&u->hiChirp, 900 * s, 0.04f);
        sp_set(&u->hiChirpN, 900 * s, 0.04f);
    }
    u->prevThrottle = throttle;
    u->prevShifting = shifting;
}
/* a gain that is (and is heading to) zero: what feeds only it need not be computed exactly this quantum */
static int quiet(const SP *a) {
    return fabsf(a->v) < 1e-7f && fabsf(a->t) < 1e-7f;
}
static float cents(float c) {
    return fast_exp2(c * (1.f / 1200));
}
/* out: turbo bus contribution; send: surgeSend (into the exhaust inputs) */
static void turbo_run(Turbo *u, double t, float *out, float *send) {
    float bv[QN], wn[QN], grain[QN], air[QN], fr[QN], x[QN], y[QN], bus[QN], w1[QN], w2[QN], env[QN], tmp[QN], hio[QN];
    int i;
    nsrc_run(&u->white, wn, QN);
    nsrc_run(&u->grainN, grain, QN);
    nsrc_run(&u->airN, air, QN);
    bqn_run(&u->grainBP, grain, grain, QN, 0);
    /* wander LFOs */
    for (i = 0; i < QN; i++) fr[i] = 0.31f;
    osc_run(&u->wander1, fr, w1, QN);
    for (i = 0; i < QN; i++) fr[i] = 2.17f;
    osc_run(&u->wander2, fr, w2, QN);
    /* blade tone */
    for (i = 0; i < QN; i++) {
        float det = sp_tick(&u->wd1) * w1[i] + sp_tick(&u->wd2) * w2[i];
        fr[i] = sp_tick(&u->whineF) * cents(det);
    }
    osc_run(&u->whine, fr, x, QN);
    for (i = 0; i < QN; i++) x[i] *= sp_tick(&u->whineDrive);
    ws_run(&u->whineShaper, x, x, QN);
    bqn_run(&u->whineBP, x, x, QN, 0);
    sp_fill(&u->shaftF, fr, QN);
    osc_run(&u->shaft, fr, y, QN);
    for (i = 0; i < QN; i++)
        bus[i] = x[i] * (sp_tick(&u->whineGain) + sp_tick(&u->grainDepth) * grain[i] + sp_tick(&u->shaftDepth) * y[i]);
    /* flow rush */
    bqn_run(&u->hissBP, wn, x, QN, 0);
    for (i = 0; i < QN; i++) bus[i] += x[i] * (sp_tick(&u->hissGain) + sp_tick(&u->hissGrain) * grain[i]);
    bqn_run(&u->whineNoiseBP, wn, x, QN, 0);
    for (i = 0; i < QN; i++) bus[i] += x[i] * sp_tick(&u->whineNoiseGain);
    bqn_run(&u->airflowHP, air, x, QN, 0);
    bqn_run(&u->airflowLP, x, x, QN, 0);
    for (i = 0; i < QN; i++) bus[i] += x[i] * sp_tick(&u->airflowGain);
    /* blow-off valve */
    bq_run_ap(&u->bovBP, BQ_BP, &u->bovF, &u->bovQ, t, wn, x, QN, &u->bovcf, &u->bovcq);
    ap_fill(&u->bovGain, t, y, QN);
    for (i = 0; i < QN; i++) bv[i] = x[i] * y[i];
    bqn_run(&u->bovHissHP, wn, x, QN, 0);
    ap_fill(&u->bovHiss, t, y, QN);
    for (i = 0; i < QN; i++) bv[i] += x[i] * y[i];
    /* wastegate stutter */
    for (i = 0; i < QN; i++) fr[i] = sp_tick(&u->wgF) + 6 * grain[i];
    osc_run(&u->wg, fr, x, QN);
    if (quiet(&u->wgLevel) && quiet(&u->wgGrain))
        for (i = 0; i < QN; i++) x[i] = 0; /* gated off: skip the 2x shaper */
    else
        ws_run(&u->wgShaper, x, x, QN);
    bqn_run(&u->wgBP, wn, y, QN, 0);
    for (i = 0; i < QN; i++) bus[i] += y[i] * (0.05f + x[i]) * (sp_tick(&u->wgLevel) + sp_tick(&u->wgGrain) * grain[i]);
    /* buzz-saw */
    sp_fill(&u->buzzF, fr, QN);
    osc_run(&u->buzz, fr, x, QN);
    bqn_run(&u->buzzBP, x, x, QN, 0);
    for (i = 0; i < QN; i++) bus[i] += x[i] * sp_tick(&u->buzzGain);
    /* tone stage */
    bqn_run(&u->airLP, bus, bus, QN, 0);
    bqn_run(&u->airShelf, bus, bus, QN, 0);
    /* high whistle (straight to out) */
    for (i = 0; i < QN; i++) fr[i] = sp_tick(&u->hiF) * cents(sp_tick(&u->hiWander) * w2[i]);
    osc_run(&u->hi, fr, hio, QN);
    for (i = 0; i < QN; i++) hio[i] *= sp_tick(&u->hiGain) + sp_tick(&u->hiGrain) * grain[i];
    /* surge envelope */
    for (i = 0; i < QN; i++) fr[i] = sp_tick(&u->surgeF) + sp_tick(&u->surgeJitter) * grain[i];
    osc_run(&u->surgeOsc, fr, x, QN);
    if (quiet(&u->surgeBodyLvl) && quiet(&u->surgeEdgeLvl) && quiet(&u->hiSurgeLvl))
        for (i = 0; i < QN; i++) env[i] = 0; /* no flutter: the envelope reaches nothing audible */
    else
        ws_run(&u->surgeShaper, x, env, QN);
    /* surge bus: body + edge + whoosh -> surgeLP */
    for (i = 0; i < QN; i++) tmp[i] = sp_tick(&u->surgeChirp) * env[i];
    bqn_run(&u->surgeBody, wn, x, QN, tmp);
    for (i = 0; i < QN; i++) {
        float body = x[i] * env[i] * sp_tick(&u->surgeBodyLvl);
        send[i] = body * sp_tick(&u->surgeSend) * u->audible.v;
        y[i] = 0.7f * body;
    }
    bqn_run(&u->surgeEdge, wn, x, QN, 0);
    for (i = 0; i < QN; i++) y[i] += 0.7f * x[i] * env[i] * sp_tick(&u->surgeEdgeLvl);
    bqn_run(&u->whooshBP, wn, x, QN, 0);
    for (i = 0; i < QN; i++) y[i] += x[i] * (sp_tick(&u->whooshGain) + sp_tick(&u->whooshGrain) * grain[i]);
    bqn_run(&u->surgeLP, y, y, QN, 0);
    /* flutter whistle */
    for (i = 0; i < QN; i++) fr[i] = sp_tick(&u->fwF) + sp_tick(&u->hiChirp) * env[i];
    osc_run(&u->fw, fr, x, QN);
    for (i = 0; i < QN; i++) tmp[i] = sp_tick(&u->hiChirpN) * env[i];
    bqn_run(&u->fwBP, wn, w1, QN, tmp);
    for (i = 0; i < QN; i++) {
        float hs = (0.2f * x[i] + 3.2f * w1[i]) * env[i] * sp_tick(&u->hiSurgeLvl);
        /* the valve's whoosh bypasses the layer's air cut (it sits at 0.3-1.5 kHz where that stage is flat) */
        out[i] = ((bus[i] + hio[i] + y[i] + hs) * sp_tick(&u->audible) + bv[i] * sp_tick(&u->bovx)) * sp_tick(&u->out);
    }
}

/* ------------------------------------------------------------------------------------------------ transients */
typedef struct {
    int comb;
    float fA, qA, fB, qB, mixB, atk, dec, lvl;
} TPre;
enum { T_BANG, T_CRACKLE, T_CLUNK, T_THUMP, T_CLICK };
static const TPre TPRE[5] = {
    {1, 112, 1.6f, 680, 2.0f, 0.58f, 0.0050f, 0.190f, 1.0f},  {1, 700, 3.0f, 1850, 3.6f, 0.62f, 0.0030f, 0.055f, 0.5f},
    {0, 330, 7.0f, 1250, 6.0f, 0.45f, 0.0035f, 0.075f, 0.62f}, {0, 95, 2.2f, 320, 3.0f, 0.35f, 0.0040f, 0.130f, 0.8f},
    {0, 1500, 5.0f, 2800, 6.0f, 0.40f, 0.0018f, 0.020f, 0.16f},
};
static void tb_init(TBank *b, const Preset *p) {
    int i, nComb = 10; /* round(16 * 0.62) */
    memset(b, 0, sizeof(*b));
    b->rnd = 0x2545f491u;
    b->makeup = 9;
    for (i = 0; i < TV; i++) {
        TVoice *v = &b->v[i];
        double off = fmod(i * 0.137, 1.9);
        nsrc_init(&v->src, WHITE, NB_LEN, off);
        ap_init(&v->fa, 1000);
        ap_init(&v->qa, 6);
        ap_init(&v->fb, 3000);
        ap_init(&v->qb, 8);
        ap_init(&v->mixB, 0.5f);
        ap_init(&v->gain, 0);
        v->cfa = 1000;
        v->cqa = 6;
        v->cfb = 3000;
        v->cqb = 8;
        bq_coef(&v->a, BQ_BP, 1000, 6, 0);
        bq_coef(&v->b, BQ_BP, 3000, 8, 0);
        v->busy = -1;
        v->comb = i < nComb;
        b->pool[v->comb ? 0 : 1][b->npool[v->comb ? 0 : 1]++] = i;
    }
    b->cyl = clampf((float)p->n, 1, 16);
    b->idleRpm = p->idle;
    b->sizeF = powf(6 / b->cyl, 0.35f);
    b->sizeA = powf(b->cyl / 6, 0.30f);
    b->burstCount = (int)clampf(1 + (float)floor(b->cyl / 5 + 0.5), 1, 3);
}
static double brnd(TBank *b) {
    return xs_unit(&b->rnd);
}
static void tb_trigger(TBank *b, int type, double now, double time, float amp, float fScale, float decScale) {
    const TPre *pre = &TPRE[type];
    double t = time > now + LOOKAHEAD ? time : now + LOOKAHEAD, end;
    float peak = clampf(amp * pre->lvl, 0.0008f, 4), fs = clampf(fScale, 0.2f, 5),
          dec = clampf(pre->dec * clampf(decScale, 0.2f, 6), 0.004f, 1.5f);
    int bus = pre->comb ? 0 : 1, n = b->npool[bus], idx = -1, k;
    TVoice *v;
    end = t + pre->atk + dec + 0.002;
    for (k = 0; k < n; k++) {
        int i = (b->cursor[bus] + k) % n;
        if (b->v[b->pool[bus][i]].busy <= t) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        int best = 0;
        for (k = 1; k < n; k++)
            if (b->v[b->pool[bus][k]].busy < b->v[b->pool[bus][best]].busy) best = k;
        idx = best;
    }
    b->cursor[bus] = (idx + 1) % n;
    v = &b->v[b->pool[bus][idx]];
    ap_cancel(&v->fa, t);
    ap_push(&v->fa, EV_SET, t, hz(pre->fA * fs, 1000), 0);
    ap_push(&v->qa, EV_SET, t, pre->qA, 0);
    ap_cancel(&v->fb, t);
    ap_push(&v->fb, EV_SET, t, hz(pre->fB * fs, 3000), 0);
    ap_push(&v->qb, EV_SET, t, pre->qB, 0);
    ap_cancel(&v->mixB, t);
    ap_push(&v->mixB, EV_SET, t, pre->mixB, 0);
    ap_cancel(&v->gain, t);
    ap_push(&v->gain, EV_SET, t, 0.0004f, 0);
    ap_push(&v->gain, EV_LIN, t + pre->atk, peak, 0);
    ap_push(&v->gain, EV_EXP, t + pre->atk + dec, 0.0004f, 0);
    ap_push(&v->gain, EV_SET, end, 0, 0);
    v->busy = end;
}
static void tb_update(TBank *b, const Params *p, float rpmNorm) {
    double now = p->now;
    float rpm = p->rpm > 0 ? p->rpm : 0, cut = clampf(p->evCut, 0, 1), popEv = clampf(p->evPop, 0, 1),
          lash = clampf(p->evLash, 0, 1), eng = clampf(p->evEngage, 0, 1), overrun = clampf(p->overrun, 0, 1), pop;
    int budget = 18, i, hot;
    if (cut > 0 && budget > 0) {
        int count = budget < b->burstCount ? budget : b->burstCount;
        double t = now + LOOKAHEAD + brnd(b) * 0.008;
        for (i = 0; i < count; i++) {
            float fall = (float)(pow(0.72, i) * (0.55 + 0.65 * brnd(b)));
            float amp = clampf(0.55f * cut * b->sizeA * fall * (0.5f + 0.7f * rpmNorm), 0.001f, 1);
            int type = i == 0 ? T_BANG : (brnd(b) < 0.45 ? T_BANG : T_CRACKLE);
            float fsc = b->sizeF * (float)(0.75 + 0.6 * brnd(b)), dsc = (float)(0.7 + 0.8 * brnd(b));
            tb_trigger(b, type, now, t, amp, fsc, dsc);
            t += 0.012 + brnd(b) * 0.048;
            budget--;
        }
    }
    if (popEv > 0 && budget > 0) {
        double roll = brnd(b), t;
        float bigness = 0.5f * popEv + 0.5f * rpmNorm, punch, size, gapScale;
        int shape, maxRun, spread, count;
        if (roll < 0.34 - 0.16 * bigness)
            shape = 0;
        else if (roll < 0.80 - 0.10 * bigness)
            shape = 1;
        else
            shape = 2;
        maxRun = (int)clampf((float)floor((1.0 + 6 / (b->cyl > 3 ? b->cyl : 3)) * (0.6 + 0.7 * brnd(b)) + 0.5), 2, 3);
        spread = shape == 0 ? 1 : shape == 1 ? 2 : maxRun;
        count = budget < spread ? budget : spread;
        punch = shape == 0 ? 2.6f : shape == 1 ? 2.0f : 1.6f;
        {
            double r1 = brnd(b), r2 = brnd(b);
            size = (float)(0.42 + 1.05 * r1 * r2);
        }
        t = now + LOOKAHEAD + brnd(b) * 0.006;
        gapScale = shape == 2 ? 0.55f : 1;
        for (i = 0; i < count; i++) {
            float fall = (float)(pow(0.86, i) * (0.6 + 0.8 * brnd(b)));
            float amp = clampf(punch * size * popEv * b->sizeA * fall * (0.5f + 0.65f * rpmNorm), 0.001f, 2);
            int type = i == 0 ? T_BANG : (brnd(b) < 0.5 ? T_BANG : T_CRACKLE);
            float fsc = b->sizeF * (float)(0.62 + 0.7 * brnd(b)) * (1 - 0.22f * popEv), dsc = (float)(0.7 + 0.9 * brnd(b));
            tb_trigger(b, type, now, t, amp, fsc, dsc);
            t += (0.055 + brnd(b) * 0.16) * (0.6 + 4 / (b->cyl > 3 ? b->cyl : 3)) * gapScale;
            budget--;
        }
    }
    if (lash > 0 && budget > 0) {
        double t = now + LOOKAHEAD;
        tb_trigger(b, T_CLUNK, now, t, clampf(0.5f * lash, 0.001f, 1), (float)(0.9 + 0.25 * brnd(b)), 0.8f + 0.5f * lash);
        budget--;
        if (lash > 0.35f && budget > 0) {
            double t2 = t + 0.018 + brnd(b) * 0.012;
            tb_trigger(b, T_CLUNK, now, t2, clampf(0.16f * lash, 0.001f, 1), 1.3f, 0.5f);
            budget--;
        }
    }
    if (eng > 0 && budget > 0) {
        tb_trigger(b, T_THUMP, now, now + LOOKAHEAD, clampf(0.45f * eng, 0.001f, 1), 1, 0.8f + 0.6f * eng);
        budget--;
    }
    if (p->phase == PH_SYNC) {
        if (b->prevPhase != PH_SYNC) {
            b->syncClicks = 0;
            b->syncNext = now;
        }
        if (b->syncClicks < 1 && now >= b->syncNext && budget > 0) {
            double a1 = brnd(b), a2 = brnd(b);
            tb_trigger(b, T_CLICK, now, now + LOOKAHEAD, (float)(0.30 + 0.22 * a1), (float)(0.85 + 0.4 * a2), 1);
            b->syncClicks++;
            b->syncNext = now + 0.035 + brnd(b) * 0.03;
            budget--;
        }
    } else if (b->prevPhase == PH_SYNC)
        b->syncClicks = 0;
    b->prevPhase = p->phase;
    pop = p->popIntensity >= 0 ? clampf(p->popIntensity, 0, 1) : overrun * 0.55f;
    hot = pop > 0.30f && rpm > b->idleRpm * 1.5f && !p->shifting;
    if (!hot) {
        b->nextPop = now;
        return;
    }
    if (b->nextPop < now) b->nextPop = now + 0.01 + brnd(b) * 0.04;
    {
        double horizon = now + 0.12;
        float rate = 0.35f + 1.2f * pop * (0.35f + 0.65f * rpmNorm) * (b->cyl / 6);
        while (b->nextPop < horizon && budget > 0) {
            double t = b->nextPop > now + LOOKAHEAD ? b->nextPop : now + LOOKAHEAD, r1 = brnd(b), r2 = brnd(b);
            float amp = clampf(0.30f * pop * b->sizeA * (float)(0.25 + 1.5 * r1 * r2) * (0.45f + 0.55f * rpmNorm), 0.001f, 1);
            int type = brnd(b) < 0.4 ? T_BANG : T_CRACKLE;
            float fsc = b->sizeF * (float)(0.7 + 0.9 * brnd(b)), dsc = (float)(0.6 + 0.9 * brnd(b));
            double r3, r4;
            tb_trigger(b, type, now, t, amp, fsc, dsc);
            r3 = brnd(b);
            r4 = brnd(b);
            b->nextPop = t + (0.25 + 2.2 * r3 * r4) / (rate > 0.4f ? rate : 0.4f);
            budget--;
        }
        if (b->nextPop < now) b->nextPop = now;
    }
}
static void tb_run(TBank *b, double t, float *mech, float *comb) {
    float x[QN], ya[QN], yb[QN], mx[QN], g[QN];
    int i, k;
    for (i = 0; i < QN; i++) mech[i] = comb[i] = 0;
    for (k = 0; k < TV; k++) {
        TVoice *v = &b->v[k];
        float *dst = v->comb ? comb : mech;
        if (!v->gain.n && !v->gain.mode && v->gain.cur == 0) {
            /* idle voice: its band-passed noise is multiplied by 0. Only the noise read position moves; the
               filters keep their (statistically identical) state for the next trigger. */
            v->src.pos = (v->src.pos + QN) % v->src.len;
            continue;
        }
        nsrc_run(&v->src, x, QN);
        bq_run_ap(&v->a, BQ_BP, &v->fa, &v->qa, t, x, ya, QN, &v->cfa, &v->cqa);
        bq_run_ap(&v->b, BQ_BP, &v->fb, &v->qb, t, x, yb, QN, &v->cfb, &v->cqb);
        ap_fill(&v->mixB, t, mx, QN);
        ap_fill(&v->gain, t, g, QN);
        for (i = 0; i < QN; i++) dst[i] += (ya[i] + yb[i] * mx[i]) * g[i];
    }
    for (i = 0; i < QN; i++) {
        mech[i] *= b->makeup;
        comb[i] *= b->makeup;
    }
}

/* ------------------------------------------------------------------------------------------------ voice */
void voice_init(Voice *v, const Preset *p, int turbo) {
    memset(v, 0, sizeof(*v));
    v->pr = p;
    { /* the calibration renders 5 loads x outputs x 8192 points: do it once per preset, not on every switch */
        static Comb proto[8];
        static const Preset *made[8];
        int k = (int)(p - presets);
        if (k < 0 || k >= 8) k = 0;
        if (made[k] != p) {
            comb_init(&proto[k], p);
            made[k] = p;
        }
        v->comb = proto[k];
    }
    exhaust_init(&v->ex, p);
    intake_init(&v->in, p);
    exn_init(&v->exn, p);
    sub_init(&v->sub, p);
    char_init(&v->ch);
    trans_init(&v->tr, p);
    tb_init(&v->tb, p);
    turbo_reset(&v->tu, p);
    v->tu.on = turbo;
    sp_init(&v->turbineShelfG, 0);
    sp_init(&v->intakeTrim, 1);
    sp_init(&v->popx, 1);
    bqn_init(&v->turbineShelf, BQ_HS, 700, 1, 0);
    v->popSend = 1.05f;
}
void voice_set_turbo(Voice *v, int on, double now) {
    if (on) {
        if (v->tu.on && !v->tu.releasing) return;
        turbo_reset(&v->tu, v->pr);
        v->tu.on = 1;
    } else if (v->tu.on && !v->tu.releasing) {
        v->tu.releasing = 1;
        v->tu.releaseAt = now + 0.5;
        sp_set(&v->tu.out, 0, 0.07f);
        sp_set(&v->tu.surgeSend, 0, 0.07f);
    }
}
void voice_update(Voice *v, const Params *pin, int immediate) {
    const Preset *pr = v->pr;
    Params p = *pin;
    float tc = immediate ? 0.002f : 0.02f, f0, x, ampl, turb;
    p.rpmNorm = voice_rpmnorm(pr, p.rpm);
    f0 = clampf(p.f0, 0.02f, 4000);
    if (p.dt > 0) {
        float rise = (p.load - v->prevLoad) / p.dt, b = v->bark * expf(-p.dt / 0.16f), r = p.noBark ? 0.f : clampf(rise / 10, 0, 1);
        v->bark = b > r ? b : r;
    }
    v->prevLoad = p.load;
    x = clampf(p.load + 0.3f * v->bark, 0, 1);
    ampl = (1 + 0.35f * v->bark) * (0.32f + 0.68f * powf(x, 0.7f)) * (0.45f + 0.55f * p.rpmNorm) *
           (1 - 0.45f * clampf(p.overrun, 0, 1)) * clampf(pr->voice, 0.2f, 2);
    sp_set(&v->comb.f0, f0, tc);
    sp_set(&v->comb.load, x, tc);
    sp_set(&v->comb.amp, ampl, tc);
    sp_set(&v->comb.ripple, crank_ripple(pr, p.rpm, x), tc);
    sp_set(&v->comb.low, 1 - p.rpmNorm, tc);
    turb = (v->tu.on && !v->tu.releasing) ? 0.55f + 0.45f * clampf(v->tu.spool, 0, 1) : 0;
    sp_set(&v->turbineShelf.g, -7 * turb, 0.12f);
    sp_set(&v->intakeTrim, 1 - 0.4f * turb, 0.12f);
    /* X pops: level of the combustion reports (0 = none; how often they come is set in the control model) */
    sp_set(&v->popx, clampf(p.xpops, 0, 2), 0.05f);
    exhaust_update(&v->ex, &p, p.rpmNorm);
    intake_update(&v->in, &p, p.rpmNorm);
    trans_update(&v->tr, &p);
    turbo_update(&v->tu, &p, p.rpmNorm, pr->redline);
    tb_update(&v->tb, &p, p.rpmNorm);
    exn_update(&v->exn, &p, p.rpmNorm);
    sub_update(&v->sub, &p, p.rpmNorm);
    char_update(&v->ch, &p, p.rpmNorm);
}
void voice_render(Voice *v, double t, Busses *o) {
    float cb[3][QN], trem[QN], en[QN], mech[QN], tcomb[QN], tout[QN], send[QN], in[2][QN], x[QN], w[QN];
    int i, b, turbo = v->tu.on;
    comb_run(&v->comb, cb);
    char_run(&v->ch, trem);
    exn_run(&v->exn, en);
    tb_run(&v->tb, t, mech, tcomb);
    for (i = 0; i < QN; i++) tcomb[i] *= sp_tick(&v->popx);
    if (turbo) {
        turbo_run(&v->tu, t, tout, send);
        if (v->tu.releasing && t >= v->tu.releaseAt) v->tu.on = v->tu.releasing = 0;
    } else
        for (i = 0; i < QN; i++) tout[i] = send[i] = 0;
    for (b = 0; b < v->ex.nb; b++)
        for (i = 0; i < QN; i++) in[b][i] = cb[b][i] + en[i] + v->popSend * tcomb[i] + send[i];
    /* crossfade weight, linear per sample */
    for (i = 0; i < QN; i++) {
        float d = v->wtarget - v->weight, step = 1.f / (0.3f * SR_F);
        v->weight += d > step ? step : d < -step ? -step : d;
        w[i] = v->weight;
    }
    exhaust_run(&v->ex, in, x);
    for (i = 0; i < QN; i++) x[i] *= 1 + trem[i];
    bqn_run(&v->turbineShelf, x, x, QN, 0);
    for (i = 0; i < QN; i++) o->exhaust[i] += x[i] * w[i];
    intake_run(&v->in, cb[v->ex.nb], x);
    for (i = 0; i < QN; i++) o->intake[i] += x[i] * sp_tick(&v->intakeTrim) * w[i];
    trans_run(&v->tr, x);
    for (i = 0; i < QN; i++) o->transmission[i] += x[i] * w[i];
    sub_run(&v->sub, x);
    for (i = 0; i < QN; i++) o->sub[i] += x[i] * w[i];
    for (i = 0; i < QN; i++) {
        o->turbo[i] += tout[i] * w[i];
        o->transients[i] += (mech[i] + 0.75f * tcomb[i]) * w[i];
    }
}
