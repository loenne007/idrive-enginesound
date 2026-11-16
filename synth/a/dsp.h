#ifndef ES_DSP_H
#define ES_DSP_H
/* Small DSP building blocks. Everything is inline, allocation-free and libm-free per sample; the few
 * coefficient designs that need sin/cos/exp run at init or once per 32-sample control tick. */
#include "synth.h"
#include <math.h>

#define PI_F 3.14159265f

static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : x > hi ? hi : x;
}
/* flush denormals/NaN in recursive states (x86 FTZ/DAZ is also set in main) */
static inline float clean(float x) {
    return (x > 1e-15f || x < -1e-15f) && x < 1e6f && x > -1e6f ? x : 0;
}

/* xorshift32: uniform -1..1 */
static inline float rnd(uint32_t *seed) {
    uint32_t x = *seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *seed = x;
    return (float)(x >> 8) * (1.f / 8388608.f) - 1.f;
}
/* approximately unit-variance Gaussian (Irwin-Hall of 4 uniforms, bounded at about 3.5 sigma) */
static inline float gauss(uint32_t *seed) {
    return (rnd(seed) + rnd(seed) + rnd(seed) + rnd(seed)) * .866f;
}

/* one-pole coefficient for a corner frequency (init/control rate only) */
static inline float onepole_k(float hz) {
    return 1.f - expf(-2.f * PI_F * hz / SR);
}

/* ---- biquads (RBJ cookbook), transposed direct form II ---- */
static inline float bq(Biquad *f, float x) {
    float y = f->b0 * x + f->z1;
    f->z1 = clean(f->b1 * x - f->a1 * y + f->z2);
    f->z2 = clean(f->b2 * x - f->a2 * y);
    return y;
}
static inline void bq_set(Biquad *f, float b0, float b1, float b2, float a0, float a1, float a2) {
    f->b0 = b0 / a0;
    f->b1 = b1 / a0;
    f->b2 = b2 / a0;
    f->a1 = a1 / a0;
    f->a2 = a2 / a0;
}
static inline void bq_lowpass(Biquad *f, float hz, float q) {
    float w = 2 * PI_F * hz / SR, c = cosf(w), al = sinf(w) / (2 * q);
    bq_set(f, (1 - c) / 2, 1 - c, (1 - c) / 2, 1 + al, -2 * c, 1 - al);
}
static inline void bq_highpass(Biquad *f, float hz, float q) {
    float w = 2 * PI_F * hz / SR, c = cosf(w), al = sinf(w) / (2 * q);
    bq_set(f, (1 + c) / 2, -1 - c, (1 + c) / 2, 1 + al, -2 * c, 1 - al);
}
static inline void bq_bandpass(Biquad *f, float hz, float q) { /* 0 dB peak gain */
    float w = 2 * PI_F * hz / SR, c = cosf(w), al = sinf(w) / (2 * q);
    bq_set(f, al, 0, -al, 1 + al, -2 * c, 1 - al);
}
static inline void bq_peak(Biquad *f, float hz, float q, float db) {
    float w = 2 * PI_F * hz / SR, c = cosf(w), al = sinf(w) / (2 * q), A = powf(10, db / 40);
    bq_set(f, 1 + al * A, -2 * c, 1 - al * A, 1 + al / A, -2 * c, 1 - al / A);
}

static inline void bq_lowshelf(Biquad *f, float hz, float db) { /* shelf slope S = 1 (Web Audio) */
    float w = 2 * PI_F * hz / SR, c = cosf(w), A = powf(10, db / 40), al = sinf(w) / 2 * sqrtf(2), sa = 2 * sqrtf(A) * al;
    bq_set(f, A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa),
           (A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa);
}
static inline void bq_highshelf(Biquad *f, float hz, float db) {
    float w = 2 * PI_F * hz / SR, c = cosf(w), A = powf(10, db / 40), al = sinf(w) / 2 * sqrtf(2), sa = 2 * sqrtf(A) * al;
    bq_set(f, A * ((A + 1) + (A - 1) * c + sa), -2 * A * ((A - 1) + (A + 1) * c), A * ((A + 1) + (A - 1) * c - sa),
           (A + 1) - (A - 1) * c + sa, 2 * ((A - 1) - (A + 1) * c), (A + 1) - (A - 1) * c - sa);
}
/* Web Audio's lowpass/highpass take Q in dB (resonance): 0 dB is Q = 1, Butterworth is -3.01 dB. Engine_Sim's
 * numbers are in that convention, so its filters are reproduced through these two. */
static inline void wa_lowpass(Biquad *f, float hz, float q_db) {
    bq_lowpass(f, clampf(hz, 10, .45f * SR), powf(10, q_db / 20));
}
static inline void wa_highpass(Biquad *f, float hz, float q_db) {
    bq_highpass(f, clampf(hz, 10, .45f * SR), powf(10, q_db / 20));
}

/* ---- delay line with linearly interpolated fractional read ---- */
static inline void delay_write(Delay *d, float x) {
    d->buf[d->w++ & (DLEN - 1)] = x;
}
/* read the sample written `t` samples ago (t >= 1), after this sample's write: call before writing */
static inline float delay_read(const Delay *d, float t) {
    int i = (int)t;
    float f = t - i, a = d->buf[(d->w - i) & (DLEN - 1)], b = d->buf[(d->w - i - 1) & (DLEN - 1)];
    return a + f * (b - a);
}

/* tanh without libm (Pade 7/6, clamped): accurate to ~1e-4 over the range the shapers use */
static inline float tanh_f(float x) {
    float x2;
    if (x > 4.97f) return 1;
    if (x < -4.97f) return -1;
    x2 = x * x;
    return x * (135135 + x2 * (17325 + x2 * (378 + x2))) / (135135 + x2 * (62370 + x2 * (3150 + x2 * 28)));
}

/* Soft limiter: linear to 0.65, rational knee asymptotic to 0.89 (-1.0 dBFS). */
static inline float limit(float x) {
    float a = fabsf(x);
    if (a > .65f) x = copysignf(.65f + .24f * (a - .65f) / (.24f + a - .65f), x);
    return x;
}
#endif
