#ifndef ES_DSP_H
#define ES_DSP_H
/* Small DSP building blocks: inline, allocation-free; coefficient designs run at init or control rate. */
#include "synth.h"
#include <math.h>

#define PI_F 3.14159265f
#define TAU_D 6.283185307179586

static inline float clampf(float x, float lo, float hi) { return x < lo ? lo : x > hi ? hi : x; }
/* flush denormals / runaway values in recursive states */
static inline float clean(float x) { return (x > 1e-15f || x < -1e-15f) && x < 1e6f && x > -1e6f ? x : 0; }
static inline float smooth01(float x) {
    x = clampf(x, 0, 1);
    return x * x * (3 - 2 * x);
}
static inline float sstep(float a, float b, float x) { return smooth01((x - a) / (b - a)); }
static inline float db2lin(float db) { return powf(10.f, db * .05f); }
/* coefficient of a one-pole with time constant tau seconds, updated every `period` samples */
static inline float tau_k(float tau, float period) { return 1.f - expf(-period / (tau * SR)); }

/* xorshift32: uniform -1..1 */
static inline float rnd(uint32_t *seed) {
    uint32_t x = *seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *seed = x;
    return (float)(x >> 8) * (1.f / 8388608.f) - 1.f;
}
/* roughly unit-variance, bounded Gaussian */
static inline float gauss(uint32_t *seed) { return (rnd(seed) + rnd(seed) + rnd(seed) + rnd(seed)) * .866f; }

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
static inline void bq_lowshelf(Biquad *f, float hz, float db) { /* shelf slope S = 1 */
    float w = 2 * PI_F * hz / SR, c = cosf(w), A = powf(10, db / 40), al = sinf(w) / 2 * sqrtf(2),
          sa = 2 * sqrtf(A) * al;
    bq_set(f, A * ((A + 1) - (A - 1) * c + sa), 2 * A * ((A - 1) - (A + 1) * c), A * ((A + 1) - (A - 1) * c - sa),
           (A + 1) + (A - 1) * c + sa, -2 * ((A - 1) + (A + 1) * c), (A + 1) + (A - 1) * c - sa);
}
#endif
