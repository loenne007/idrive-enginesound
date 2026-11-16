#ifndef ES_DSP_H
#define ES_DSP_H
#include "synth.h"
#include <math.h>
static inline float clampf(float x, float lo, float hi) {
    return x < lo ? lo : x > hi ? hi : x;
}
static inline float clean(float x) {
    return isfinite(x) && fabsf(x) > 1e-20f ? x : 0;
}
static inline float rnd(Voice *v) {
    uint32_t x = v->seed;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    v->seed = x;
    return (float)(x >> 8) * (1.f / 8388608.f) - 1.f;
}
static inline float pipe_step(Pipe *p, float x) {
    float y = p->buf[p->pos];
    p->lp = clean(p->lp + .32f * (y - p->lp));
    p->buf[p->pos] = clean(x + p->fb * p->lp);
    if (++p->pos == p->len) p->pos = 0;
    return y;
}
static inline float limit(float x) {
    float a = fabsf(x);
    if (a > .65f) x = copysignf(.65f + .24f * (a - .65f) / (.24f + a - .65f), x);
    return x;
}
#endif
