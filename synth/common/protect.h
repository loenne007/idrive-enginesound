/* protect.h - speaker protection: the last stage of every sound engine, between its output and the device.
 *
 * Whatever an engine (or a sound setting) does, what reaches the car's amplifier is kept safe for its speakers:
 *   0. the live gain: on the car (ALSA / raw output) the engine is raised by PROTECT_LIVE_GAIN_DB (6 dB, env
 *      ES_LIVE_GAIN_DB 0..20 overrides) to the loudness of radio and music, so the car's volume knob can stay where
 *      the radio is comfortable; WAV renders keep the engine's own level (ES_LIVE_GAIN=1 applies it there too)
 *   1. a 4th-order high-pass at 30 Hz: no DC, no infrasound, no woofer over-excursion
 *   2. a 2nd-order low-pass at 16 kHz: nothing harsh for the tweeters
 *   3. a bass limiter: the band below ~150 Hz (4th-order detector) is held under -3 dBFS (peak envelope), however much bass
 *      the engine or the bass setting asks for
 *   4. a power limiter: the 3 s RMS is held under -8 dBFS, the level of loud mastered music (long full-power
 *      stretches are turned down slowly)
 *   5. a look-ahead peak limiter (1 ms) at -1.5 dBFS, then a soft clip under -1 dBFS as the last resort
 *   6. a 60 ms fade-in at start, so the first block never clicks
 * Header only, C99, no allocation, a few dozen flops per frame. Engines include it from synth/common/ and call
 * protect_init() once and protect_run() on every block they output (interleaved S16 stereo, 48 kHz).
 */
#ifndef ES_PROTECT_H
#define ES_PROTECT_H
#include <math.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define PROTECT_SR 48000.0
#define PROTECT_LOOK 48 /* 1 ms look-ahead */
/* 6 dB: an engine's full load (about -15.5 dBFS RMS, peaks -5 at the default volume) lands just under the limiters
 * below, so they stay a safety net: with more, full load sat on the power limiter and the level no longer rose with rpm */
#define PROTECT_LIVE_GAIN_DB 6.0

typedef struct { float b0, b1, b2, a1, a2, z1[2], z2[2]; } ProtectBiquad;

typedef struct {
    ProtectBiquad hp1, hp2, lp, bass1, bass2; /* hp1 + hp2: 4th-order Butterworth high-pass; bass1 + 2: the low band's detector */
    float bass_env, bass_gain;          /* low-band peak envelope and its gain */
    float pow_ms, pow_gain;             /* mean square (3 s) and its gain */
    float pk_gain;                      /* the peak limiter's gain */
    float delay[PROTECT_LOOK][2];       /* look-ahead line */
    int dpos;
    long fade;                          /* frames into the fade-in */
    float gain;                         /* the live gain (linear) */
} Protect;

/* RBJ cookbook biquad, normalised; type 0 = low-pass, 1 = high-pass */
static void protect_biquad(ProtectBiquad *f, int hp, double fc, double q) {
    double w = 2 * 3.141592653589793 * fc / PROTECT_SR, c = cos(w), a = sin(w) / (2 * q), a0 = 1 + a;
    double b0 = hp ? (1 + c) / 2 : (1 - c) / 2, b1 = hp ? -(1 + c) : 1 - c;
    memset(f, 0, sizeof(*f));
    f->b0 = (float)(b0 / a0); f->b1 = (float)(b1 / a0); f->b2 = (float)(b0 / a0);
    f->a1 = (float)(-2 * c / a0); f->a2 = (float)((1 - a) / a0);
}

static inline float protect_tick(ProtectBiquad *f, int ch, float x) {
    /* transposed direct form II */
    float y = f->b0 * x + f->z1[ch];
    f->z1[ch] = f->b1 * x - f->a1 * y + f->z2[ch];
    f->z2[ch] = f->b2 * x - f->a2 * y;
    return y;
}

static void protect_init(Protect *p) {
    memset(p, 0, sizeof(*p));
    protect_biquad(&p->hp1, 1, 30, 0.5411961);   /* 4th-order Butterworth: Q 0.541 and 1.307 */
    protect_biquad(&p->hp2, 1, 30, 1.3065630);
    protect_biquad(&p->lp, 0, 16000, 0.7071068);
    protect_biquad(&p->bass1, 0, 150, 0.5411961);
    protect_biquad(&p->bass2, 0, 150, 1.3065630);
    p->bass_gain = p->pow_gain = p->pk_gain = p->gain = 1;
}

/* The live gain for car output: PROTECT_LIVE_GAIN_DB, or ES_LIVE_GAIN_DB (0..20 dB). */
static void protect_live(Protect *p) {
    const char *e = getenv("ES_LIVE_GAIN_DB");
    double db = PROTECT_LIVE_GAIN_DB;
    if (e) { double x = atof(e); if (x >= 0 && x <= 20) db = x; }
    p->gain = (float)pow(10.0, db / 20.0);
}

/* the block in place: interleaved S16 stereo, n frames */
static void protect_run(Protect *p, int16_t *buf, unsigned n) {
    const float BASS_MAX = 0.708f;    /* -3 dBFS */
    const float POW_MAX = 0.1585f;    /* (-8 dBFS)^2 */
    const float PEAK_MAX = 0.841f;    /* -1.5 dBFS */
    const float pow_a = 1.f / (float)(3 * PROTECT_SR), env_rel = 1.f / (float)(0.25 * PROTECT_SR);
    unsigned i;
    int c;
    for (i = 0; i < n; i++) {
        float x[2], lo = 0, peak = 0, g, target;
        for (c = 0; c < 2; c++) {
            float s = buf[2 * i + c] * (p->gain / 32768.f);
            s = protect_tick(&p->hp1, c, s);
            s = protect_tick(&p->hp2, c, s);
            s = protect_tick(&p->lp, c, s);
            x[c] = s;
        }
        /* bass limiter: the low band's envelope (detected on the mid), gain reduces the whole signal gently */
        lo = fabsf(protect_tick(&p->bass2, 0, protect_tick(&p->bass1, 0, 0.5f * (x[0] + x[1]))));
        p->bass_env = lo > p->bass_env ? lo : p->bass_env + (lo - p->bass_env) * env_rel;
        target = p->bass_env > BASS_MAX ? BASS_MAX / p->bass_env : 1;
        p->bass_gain += (target - p->bass_gain) * (target < p->bass_gain ? 0.002f : 0.00005f);
        /* power limiter: 3 s mean square of the signal after the bass limiter (measured before it, the two would
         * turn the same loudness down twice), slow */
        p->pow_ms += ((x[0] * x[0] + x[1] * x[1]) * 0.5f * p->bass_gain * p->bass_gain - p->pow_ms) * pow_a;
        target = p->pow_ms > POW_MAX ? sqrtf(POW_MAX / p->pow_ms) : 1;
        p->pow_gain += (target - p->pow_gain) * 0.00006f;   /* ~0.35 s */
        g = p->bass_gain * p->pow_gain;
        x[0] *= g; x[1] *= g;
        /* look-ahead peak limiter: the gain falls before a peak leaves the delay line */
        peak = fmaxf(fabsf(x[0]), fabsf(x[1]));
        target = peak > PEAK_MAX ? PEAK_MAX / peak : 1;
        if (target < p->pk_gain) p->pk_gain += (target - p->pk_gain) * 0.25f;   /* reaches it within the look-ahead */
        else p->pk_gain += (target - p->pk_gain) * 0.0002f;                     /* ~100 ms release */
        {
            float out[2];
            for (c = 0; c < 2; c++) {
                float d = p->delay[p->dpos][c], y;
                p->delay[p->dpos][c] = x[c];
                y = d * p->pk_gain;
                /* fade-in, then the soft clip under -1 dBFS (only a peak the limiter missed reaches it) */
                if (p->fade < (long)(0.06 * PROTECT_SR)) y *= (float)p->fade / (float)(0.06 * PROTECT_SR);
                if (y > 0.85f) y = 0.85f + 0.04f * tanhf((y - 0.85f) / 0.04f);
                else if (y < -0.85f) y = -0.85f + 0.04f * tanhf((y + 0.85f) / 0.04f);
                if (!(y == y)) y = 0;   /* NaN never reaches the speakers */
                out[c] = y;
            }
            if (p->fade < (long)(0.06 * PROTECT_SR)) p->fade++;
            p->dpos = (p->dpos + 1) % PROTECT_LOOK;
            buf[2 * i] = (int16_t)lrintf(out[0] * 32767.f);
            buf[2 * i + 1] = (int16_t)lrintf(out[1] * 32767.f);
        }
    }
}
#endif
