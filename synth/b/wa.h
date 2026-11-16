/* wa.h - C equivalents of the Web Audio nodes Engine_Sim uses (Chromium semantics).
 *
 * Everything runs in render quanta of QN = 128 frames, like an AudioContext. Parameters follow AudioParam
 * semantics: setTargetAtTime is a per-sample one-pole toward the target (Chromium's discrete time constant
 * 1 - e^(-1/(sr*tc))), and the few params that need setValueAtTime / linearRamp / exponentialRamp /
 * cancelScheduledValues use a small event timeline (AP). Biquad coefficients use Chromium's formulas
 * (lowpass/highpass Q in dB, every other type linear, shelves with S = 1). */
#ifndef ES_WA_H
#define ES_WA_H
#include <math.h>
#include <stdint.h>
#include <string.h>

#define QN 128
#define SR_HZ 48000
#define SR_F 48000.0f
#define NYQ_F 24000.0f
#define SUB 16 /* biquad coefficients / oscillator table choice are refreshed every SUB frames */

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : x > hi ? hi : x;
}
static inline float finf(float x, float d) {
    return isfinite(x) ? x : d;
}

/* ---- fast math: exp2 by exponent bits + degree-6 polynomial on [-.5,.5] (rel. err ~2e-7) ---- */
static inline float fast_exp2(float y) {
    float n, f, p;
    union {
        float f;
        int32_t i;
    } u;
    y = y < -126.f ? -126.f : y;
    y = y > 126.f ? 126.f : y;
    n = floorf(y + .5f);
    f = y - n;
    p = 1.5403530e-4f;
    p = p * f + 1.3333558e-3f;
    p = p * f + 9.6181291e-3f;
    p = p * f + 5.5504109e-2f;
    p = p * f + 2.4022651e-1f;
    p = p * f + 6.9314718e-1f;
    p = p * f + 1.0f;
    u.i = ((int32_t)n + 127) << 23;
    return p * u.f;
}
static inline float fast_exp(float x) {
    return fast_exp2(x * 1.44269504f);
}
/* log2 via mantissa in [sqrt.5, sqrt2) and an atanh series (abs err < 1e-7) */
static inline float fast_log2(float x) {
    union {
        float f;
        int32_t i;
    } u;
    float e, m, z, z2;
    u.f = x;
    e = (float)(((u.i >> 23) & 255) - 127);
    u.i = (u.i & 0x007fffff) | 0x3f800000;
    m = u.f;
    if (m > 1.41421356f) {
        m *= .5f;
        e += 1.f;
    }
    z = (m - 1.f) / (m + 1.f);
    z2 = z * z;
    return e + z * (2.8853901f + z2 * (0.9617967f + z2 * (0.5770780f + z2 * (0.4121986f + z2 * 0.3205989f))));
}
static inline float db_to_lin(float db) {
    return fast_exp2(db * 0.166096405f); /* 10^(db/20) */
}
static inline float lin_to_db(float x) {
    return 6.02059991f * fast_log2(x); /* 20 log10 x */
}

/* deterministic xorshift32 exactly as Engine_Sim writes it in JavaScript: `s ^= s >> 17` is a SIGNED shift there
 * (ToInt32), so the top bit is smeared in; reproduced so every seeded sequence matches the browser's */
static inline uint32_t xs32(uint32_t *s) {
    uint32_t x = *s;
    x ^= x << 13;
    x ^= (x >> 17) | ((x & 0x80000000u) ? 0xffff8000u : 0u);
    x ^= x << 5;
    *s = x;
    return x;
}
static inline double xs_unit(uint32_t *s) {
    return xs32(s) / 4294967296.0;
}

/* ---- smoothed param: setTargetAtTime chain ---- */
typedef struct {
    float v, t, k;
} SP;
float tc_k(float tc);
static inline void sp_init(SP *p, float v) {
    p->v = p->t = v;
    p->k = 0;
}
static inline void sp_set(SP *p, float target, float tc) {
    if (!isfinite(target)) target = 0;
    p->t = target;
    p->k = tc_k(tc);
}
static inline float sp_tick(SP *p) {
    p->v += (p->t - p->v) * p->k;
    return p->v;
}
/* advance n samples, writing values (out may be 0) */
static inline void sp_fill(SP *p, float *out, int n) {
    int i;
    if (p->v == p->t) {
        if (out)
            for (i = 0; i < n; i++) out[i] = p->v;
        return;
    }
    for (i = 0; i < n; i++) {
        p->v += (p->t - p->v) * p->k;
        if (out) out[i] = p->v;
    }
    if (fabsf(p->t - p->v) <= 1e-7f * fabsf(p->t) + 1e-30f) p->v = p->t;
}

/* ---- event timeline param (setValueAtTime / ramps / cancel) ---- */
enum { EV_SET, EV_LIN, EV_EXP, EV_TGT };
typedef struct {
    double t;
    float v, k;
    int type;
} PEv;
#define AP_MAX 10
typedef struct {
    float cur, v0, tgt, k;
    double t0;
    int mode, n; /* mode 0 hold, 1 target */
    PEv ev[AP_MAX];
} AP;
void ap_init(AP *a, float v);
void ap_cancel(AP *a, double t);
void ap_push(AP *a, int type, double t, float v, float tc);
float ap_at(AP *a, double t); /* advance to sample time t (monotonic) */
void ap_fill(AP *a, double t, float *out, int n);

/* ---- biquad (Chromium coefficient formulas, TDF2 run) ---- */
enum { BQ_LP, BQ_HP, BQ_BP, BQ_LS, BQ_HS, BQ_PK };
typedef struct {
    float b0, b1, b2, a1, a2, s1, s2;
} BQ;
void bq_coef(BQ *b, int type, float freq, float q, float gain_db);
static inline float bq_tick(BQ *b, float x) {
    float y = b->b0 * x + b->s1;
    b->s1 = b->b1 * x - b->a1 * y + b->s2;
    b->s2 = b->b2 * x - b->a2 * y;
    return y;
}
void bq_run(BQ *b, const float *in, float *out, int n);
void bq_flush(BQ *b);

/* biquad node: params smoothed per sample, coefficients refreshed every SUB frames when they moved */
typedef struct {
    BQ b;
    SP f, q, g;
    float cf, cq, cg;
    int type, fresh;
} BQN;
void bqn_init(BQN *n, int type, float f, float q, float g);
/* fmod: optional audio-rate input summed into frequency (like connecting a node to .frequency) */
void bqn_run(BQN *n, const float *in, float *out, int len, const float *fmod);
/* timeline-driven biquad (transient voices, BOV): f/q from APs at time t */
void bq_run_ap(BQ *b, int type, AP *f, AP *q, double t, const float *in, float *out, int len, float *cf, float *cq);

/* ---- delay line (DelayNode: linear interpolation, write-then-read) ---- */
typedef struct {
    float *buf;
    int mask, w;
} DL;
static inline void dl_write(DL *d, float x) {
    d->buf[d->w] = x;
    d->w = (d->w + 1) & d->mask;
}
/* read delay D samples behind the most recently written sample + 1, i.e. after dl_write(x[n]) gives x[n-D] */
static inline float dl_read(const DL *d, float D) {
    float pos = (float)(d->w - 1) - D;
    int i;
    float f;
    while (pos < 0) pos += (float)(d->mask + 1);
    i = (int)pos;
    f = pos - (float)i;
    return d->buf[i & d->mask] * (1.f - f) + d->buf[(i + 1) & d->mask] * f;
}
/* for loops: value D samples before the sample about to be written (needs D >= 1) */
static inline float dl_peek(const DL *d, float D) {
    float pos = (float)d->w - D;
    int i;
    float f;
    while (pos < 0) pos += (float)(d->mask + 1);
    i = (int)pos;
    f = pos - (float)i;
    return d->buf[i & d->mask] * (1.f - f) + d->buf[(i + 1) & d->mask] * f;
}

/* ---- WaveShaper with Chromium's 2x / 4x up/down samplers ---- */
typedef struct {
    float h[128 + 2 * QN * 2]; /* history + block for up stage */
} UpS;
typedef struct {
    float h[256 + 4 * QN];
} DownS;
typedef struct {
    const float *curve;
    int len, over; /* 1, 2, 4 */
    float uh1[127 + QN], uh2[127 + 2 * QN];
    float dh1[255 + 2 * QN], dh2[255 + 4 * QN];
} WS;
void ws_init(WS *w, const float *curve, int len, int over);
void ws_run(WS *w, const float *in, float *out, int n); /* n <= QN */

/* ---- band-limited wavetable oscillator (PeriodicWave) ---- */
#define PW_SIZE 4096
#define PW_RANGES 36
typedef struct {
    float *t[PW_RANGES]; /* each PW_SIZE+1 */
    int single;          /* one table for every range (pure sine) */
} Wave;
void wave_build(Wave *w, const float *real, const float *imag, int ncomp, int normalise, float *storage);
extern Wave WAVE_SINE, WAVE_SAW, WAVE_BLADE;
void waves_init(void);
typedef struct {
    const Wave *w;
    double ph; /* 0..1 */
} OSC;
/* freq: per-sample Hz (final, incl. detune); out written */
void osc_run(OSC *o, const float *freq, float *out, int n);

/* ---- looping buffer source ---- */
typedef struct {
    const float *buf;
    int len, pos;
} NSRC;
static inline void nsrc_init(NSRC *s, const float *buf, int len, double offset_s) {
    s->buf = buf;
    s->len = len;
    s->pos = ((int)(offset_s * SR_HZ)) % len;
}
void nsrc_run(NSRC *s, float *out, int n);

/* ---- DynamicsCompressorNode (Chromium DynamicsCompressorKernel) ---- */
typedef struct {
    float thr, knee, ratio, attack, release; /* current params */
    float c_thr, c_knee, c_ratio, lin_thr, slope, knee_thr, knee_thr_db, yknee_thr_db, k;
    float detector_average, compressor_gain, max_attack_diff_db;
    float pd[2][1024];
    int rd, wr;
} COMP;
void comp_init(COMP *c, float thr, float knee, float ratio, float attack, float release);
/* stereo in place (ch1 may equal ch0 pointer for mono-in, stereo-out is then handled by caller) */
void comp_run(COMP *c, float *l, float *r, int n);
float comp_makeup_db_js(float threshold, float ratio, float knee); /* fx.js compressorMakeupDb */

/* ---- FFT + uniformly/two-level partitioned zero-latency convolver ---- */
typedef struct {
    int n;
    float *cs; /* cos/sin table */
    int *rev;
} FFT;
void fft_init(FFT *f, int n, float *mem_cos, int *mem_rev);
void fft_real_fwd(const FFT *f, const float *in, float *re, float *im); /* n real -> n/2+1 bins */
void fft_real_inv(const FFT *f, const float *re, const float *im, float *out); /* scaled by 1/n */

typedef struct {
    /* stage A: block 128, FFT 256, taps [0, A_TAPS) ; stage B: block 1024, FFT 2048, taps [A_TAPS, len) */
    int pa, pb;
    float *HAr, *HAi, *XAr, *XAi; /* [pa][129] */
    float *HBr, *HBi, *XBr, *XBi; /* [pb][1025] */
    float inA[256], inB[2048], accB[1024], outB[1024];
    int ia, ib, posB; /* ring positions */
    int qcount;
} CONV;
int conv_init(CONV *c, const float *ir, int len);
void conv_run(CONV *c, const float *in, float *out); /* exactly QN frames */

#endif
