/* synth.c - control protocol, response smoothing, gear shifts, sound settings, voice crossfades, master chain.
 *
 * Response: the rpm follows P through two cascaded one-poles of 6.5 ms (90 % of a step in ~25 ms, no slew limit,
 * corners rounded so a step never clicks); load and throttle through 8 ms ones.
 * Master: infrasonic guard, the bass setting (low shelf), a light cabin colour (floor boom, body resonance,
 * panel lowpass, a small treble shelf: in the car, not muffled), output gain, soft limiter (peaks <= -3 dBFS).
 * No compressor and no reverb: levels are set explicitly by the voices (off-gas ~8 dB under full load) and the car
 * provides the room.
 */
#include "dsp.h"
#include <stdio.h>
#include <string.h>

#define OUT_GAIN 1.6f
#ifndef LIM_CEIL
#define LIM_CEIL .473f  /* -6.5 dBFS (after the protection's high-pass the peaks land around -5) */
#endif
#define WIDTH .35f      /* side signal: L/R correlation ~0.9 under load */
#define K_RPM .0031f    /* 1 - exp(-1 / (6.5 ms * SR)) */
#define K_LOAD .0026f   /* 8 ms */
#define K_SLOW .000208f /* 100 ms: volume, gate, crossfade */

static const char *xname[NX] = {"offgas", "turbo", "bov", "pops", "bass"};

static void design_bass(Synth *s, float db) {
    bq_lowshelf(&s->bass, 150, db);
    s->bass_db = db;
}

void synth_init(Synth *s, int p, float vol) {
    int i;
    memset(s, 0, sizeof(*s));
    for (i = 0; i < NP; i++) voice_init(&s->voice[i], &presets[i], i);
    s->selected = p;
    s->turbo = presets[p].turbo;
    s->rpm = s->rpm1 = s->trpm = presets[p].idle;
    s->tvolume = vol;
    s->last_p = -1e9;
    s->shift_t = s->down_t = -1;
    s->guard_t = 10;
    s->voice[p].weight = 1;
    for (i = 0; i < NX; i++) s->xt[i] = s->xs[i] = 1;
    tone_init(&s->tone);
    s->lk_g = 1;
    bq_highpass(&s->pr_hp[0], 30, .5412f);
    bq_highpass(&s->pr_hp[1], 30, 1.3066f);
    s->pr_hp[2] = s->pr_hp[0];
    s->pr_hp[3] = s->pr_hp[1];
    design_bass(s, 0);
    /* stereo width: two different allpass pairs (L gets +side, R -side), mono sum untouched */
    bq_allpass(&s->st_ap[0], 700, .6f);
    bq_allpass(&s->st_ap[1], 2900, .7f);
    bq_allpass(&s->st_ap[2], 1300, .5f);
    bq_allpass(&s->st_ap[3], 4700, .8f);
}

/* The fixed tone of the master: infrasonic guard (28 Hz, 4th order), a light cabin colour (a little floor boom,
 * body resonance, the presence of the firing harmonics' 2nd-5th orders, panel lowpass, treble shelf: in the car,
 * not muffled) and a few broad body/exhaust formants FIXED IN HZ (the engine orders sweep through them as the revs
 * change, as in a real car; Q <= 2.5, a few dB: colour, no ringing). The voices' loudness calibration listens
 * through the same chain, so the level map is what comes out. */
void tone_init(Tone *t) {
    memset(t, 0, sizeof(*t));
    bq_highpass(&t->f[0], 28, .5412f);
    bq_highpass(&t->f[1], 28, 1.3066f);
    bq_peak(&t->f[2], 58, 1.2f, 1.5f);
    bq_peak(&t->f[3], 125, 1.3f, 1.5f);
    bq_peak(&t->f[4], 600, .55f, 4.5f);
    bq_peak(&t->f[5], 235, 2.0f, 2.0f);   /* body panel */
    bq_peak(&t->f[6], 480, 2.2f, 1.8f);   /* exhaust/cabin formant */
    bq_peak(&t->f[7], 1180, 2.5f, 2.0f);  /* intake/firewall formant */
    bq_peak(&t->f[8], 2400, 1.5f, -2.0f); /* trim panel absorption */
    bq_lowpass(&t->f[9], 8000, .6f);
    bq_highshelf(&t->f[10], 3000, -3);
}
float tone_run(Tone *t, float x, Biquad *bass) {
    int i;
    x = bq(&t->f[1], bq(&t->f[0], x));
    if (bass) x = bq(bass, x);
    for (i = 2; i < NTONE; i++) x = bq(&t->f[i], x);
    return x;
}

int synth_control(Synth *s, const char *line) {
    float r, l, t, v;
    int g, k, a, b, n = 0;
    char name[32], extra;
    if (sscanf(line, "P %f %f %f %d %f %n", &r, &l, &t, &g, &v, &n) == 5 && n && line[n] == 0 && isfinite(r) &&
        isfinite(l) && isfinite(t) && isfinite(v) && g >= -1 && g <= 10) {
        s->trpm = clampf(r, 0, 10000);
        s->tload = clampf(l, -1, 1);
        s->tthrottle = clampf(t, 0, 1);
        s->tspeed = clampf(v, 0, 400);
        s->gear = g;
        s->last_p = s->time;
        return 1;
    }
    if (sscanf(line, "E %31s %c", name, &extra) == 1 && (k = preset_find(name)) >= 0) {
        if (k != s->selected) {
            /* the incoming voice starts from the current state, so nothing restarts audibly */
            s->selected = k;
            s->turbo = presets[k].turbo;
        }
        return 1;
    }
    if (sscanf(line, "T %d %c", &k, &extra) == 1 && (k == 0 || k == 1)) {
        s->turbo = k;
        return 1;
    }
    if (sscanf(line, "M %d %c", &k, &extra) == 1 && (k == 0 || k == 1)) {
        s->mute = k;
        return 1;
    }
    if (sscanf(line, "V %f %c", &v, &extra) == 1 && isfinite(v) && v >= 0 && v <= 2) {
        s->tvolume = v;
        return 1;
    }
    if (sscanf(line, "X %31s %f %c", name, &v, &extra) == 2 && isfinite(v)) {
        for (k = 0; k < NX; k++)
            if (!strcmp(name, xname[k])) {
                s->xt[k] = clampf(v, 0, 2);
                return 1;
            }
        return 0;
    }
    if (sscanf(line, "S %d %d %c", &a, &b, &extra) == 2 && a >= -1 && a <= 10 && b >= -1 && b <= 10) {
        /* ZF 8HP: no ignition cut. An upshift is a short torque intervention (timbre dip, a little level),
         * a downshift a small rev-match accent. The rpm itself comes from the telemetry. */
        if (b > a && a > 0)
            s->shift_t = 0;
        else if (b < a && b > 0)
            s->down_t = 0;
        if (a != b) s->guard_t = 0;
        return 1;
    }
    if (line[0] == 'Q' && sscanf(line + 1, " %c", &extra) != 1) {
        s->quit = 1;
        return 1;
    }
    return 0;
}

/* raised-cosine bump: 0 at t = 0, 1 at t = peak, 0 again at t = end (C1 everywhere) */
static float bump(float t, float peak, float end) {
    if (t <= 0 || t >= end) return 0;
    if (t < peak) return .5f - .5f * cosf(PI_F * t / peak);
    return .5f + .5f * cosf(PI_F * (t - peak) / (end - peak));
}

static void control_tick(Synth *s) {
    int i;
    const float k = 1 - expf(-(float)CTRL / (.08f * SR));
    for (i = 0; i < NX; i++) s->xs[i] += (s->xt[i] - s->xs[i]) * k;
    {
        float db = 9 * (s->xs[X_BASS] - 1);
        if (fabsf(db - s->bass_db) > .02f) design_bass(s, db);
    }
}

static float master(Synth *s, float x) {
    return tone_run(&s->tone, x, &s->bass);
}

void synth_render(Synth *s, int16_t *out, unsigned frames) {
    unsigned f;
    int i;
    Ctl c;
    for (f = 0; f < frames; f++) {
        float x = 0, gate = (!s->mute && !s->quit && s->time - s->last_p <= 1.5) ? 1 : 0, e;
        if (!(s->tick % CTRL)) control_tick(s);
        s->rpm1 += (s->trpm - s->rpm1) * K_RPM;
        s->rpm += (s->rpm1 - s->rpm) * K_RPM;
        s->load1 += (s->tload - s->load1) * K_LOAD;
        s->load += (s->load1 - s->load) * K_LOAD;
        s->throttle += (s->tthrottle - s->throttle) * K_LOAD;
        s->volume += (s->tvolume - s->volume) * K_SLOW;
        s->gate += (gate - s->gate) * K_SLOW;
        c.rpm = s->rpm;
        c.load = s->load;
        c.throttle = s->throttle;
        c.level = 1;
        c.lvl_load = c.load;
        /* upshift: torque intervention, peak at 70 ms, recovered at 180 ms */
        if (s->shift_t >= 0) {
            e = bump(s->shift_t, .07f, .18f);
            if (c.load > 0) c.load *= 1 - .32f * e; /* timbre: softer pulses, less rasp */
            c.level = 1 - .1f * e;                   /* level: -1 dB at the peak */
            s->shift_t += 1.f / SR;
            if (s->shift_t > .18f) s->shift_t = -1;
        }
        /* downshift: rev-match accent, peak at 40 ms, gone at 160 ms */
        if (s->down_t >= 0) {
            e = bump(s->down_t, .04f, .16f);
            c.load = clampf(c.load + .15f * e, -1, 1);
            c.level *= 1 + .12f * e;
            s->down_t += 1.f / SR;
            if (s->down_t > .16f) s->down_t = -1;
        }
        s->guard_t += 1.f / SR;
        c.shift_guard = s->guard_t < .4f;
        c.turbo = s->turbo;
        c.x = s->xs;
        for (i = 0; i < NP; i++) {
            Voice *v = &s->voice[i];
            v->weight += ((i == s->selected ? 1 : 0) - v->weight) * K_SLOW;
            if (v->weight > 1e-5f) x += v->weight * voice_step(v, &presets[i], &c) * presets[i].gain;
        }
        x = master(s, x);
        {
            /* a little width for the cabin: the band above ~300 Hz through two different allpass pairs gives a
             * side signal (L + side, R - side). The mono sum is exactly the engine; the bass stays mono. */
            float hp, side, l, r, g = OUT_GAIN * s->volume * s->gate;
            s->st_lp = clean(s->st_lp + .039f * (x - s->st_lp)); /* ~300 Hz */
            hp = x - s->st_lp;
            side = WIDTH * (bq(&s->st_ap[1], bq(&s->st_ap[0], hp)) - bq(&s->st_ap[3], bq(&s->st_ap[2], hp)));
            l = g * (x + side);
            r = g * (x - side);
            if (!isfinite(l) || !isfinite(r)) l = r = 0;
            /* look-ahead peak limiter (1.33 ms, ceiling -6.5 dBFS): the gain reaches its target before a peak
             * leaves the delay line, then recovers in ~20 ms (a shift's drop leaves no hole) */
            {
                /* the peaks are detected as the speaker protection will see them: after a replica of its 30 Hz
                 * 4th-order high-pass (which turns the low end's phase); the gain is applied to the signal itself */
                float hl = bq(&s->pr_hp[1], bq(&s->pr_hp[0], l)), hr = bq(&s->pr_hp[3], bq(&s->pr_hp[2], r));
                float pk = fmaxf(fabsf(hl), fabsf(hr)), tgt = pk > LIM_CEIL ? LIM_CEIL / pk : 1, dl, dr;
                if (tgt < s->lk_g) s->lk_g += (tgt - s->lk_g) * .12f;
                else s->lk_g += (tgt - s->lk_g) * .001f;
                dl = s->lk_buf[s->lk_pos][0];
                dr = s->lk_buf[s->lk_pos][1];
                s->lk_buf[s->lk_pos][0] = l;
                s->lk_buf[s->lk_pos][1] = r;
                s->lk_pos = (s->lk_pos + 1) % LOOK;
                l = limit(dl * s->lk_g);
                r = limit(dr * s->lk_g);
            }
            out[2 * f] = (int16_t)(l * 32767);
            out[2 * f + 1] = (int16_t)(r * 32767);
        }
        s->time += 1. / SR;
        s->tick++;
    }
}

/* ---------------------------------------------------------------------------------------------------------------
 * Demo drive (40 s, scaled to the duration): idle, two free revs, a full-throttle pull through gears 1-5 with
 * ZF 8HP ratios (the rpm falls to the next gear over 300 ms, like the car's telemetry), a lift-off and coast with
 * downshifts, light-load upshifts, a kickdown, another lift-off, coast to a stop, idle. P lines at 50 Hz.
 * ------------------------------------------------------------------------------------------------------------- */
static float sstep(float u) {
    u = clampf(u, 0, 1);
    return u * u * (3 - 2 * u);
}
static void demo_state(const Preset *p, float u, float *rpm, float *load, float *thr, int *gear) {
    const float R = p->redline, I = p->idle, hi = .88f * R;
    float r = I, l = .06f, th = 0;
    int g = 1;
    if (u < 3) {
    } else if (u < 6) { /* two free revs in neutral */
        float z = fmodf(u - 3, 1.5f);
        g = 0;
        if (z < .5f) {
            r = I + (.6f * R - I) * sstep(z / .5f) * (1 - .1f * (z / .5f));
            l = .65f;
            th = .75f;
        } else {
            float r0 = I + (.6f * R - I) * .9f;
            r = I + (r0 - I) * expf(-(z - .5f) / .3f);
            l = r > I + 250 ? -.4f : .06f;
        }
    } else if (u < 7) {
    } else if (u < 19.5f) { /* full-throttle pull, gears 1-5 */
        static const float t0[6] = {7, 8.6f, 10.8f, 13.6f, 17, 19.5f};
        static const float lo[5] = {.26f, .563f, .589f, .708f, .67f};
        int k;
        for (k = 0; k < 4 && u >= t0[k + 1]; k++) {
        }
        g = k + 1;
        {
            float tau = u - t0[k], dur = t0[k + 1] - t0[k], top = k == 4 ? .8f * R : hi;
            float lin = lo[k] * R + (top - lo[k] * R) * tau / dur;
            if (k == 0)
                r = I + (lin - I) * sstep(tau / .3f);
            else
                r = hi + (lin - hi) * sstep(tau / .3f);
        }
        l = .95f;
        th = 1;
    } else if (u < 25.5f) { /* lift-off, coast in 5th, downshifts 5-4-3 */
        float s = expf(-.22f * (u - 19.5f));
        l = -.6f;
        if (u < 22.5f) {
            g = 5;
            r = .8f * R * s;
        } else if (u < 24) {
            float z = sstep((u - 22.5f) / .3f);
            g = 4;
            r = .8f * R * s * (1 + .31f * z);
        } else {
            float z = sstep((u - 24) / .3f);
            g = 3;
            r = .8f * R * s * 1.31f * (1 + .244f * z);
        }
    } else if (u < 31) { /* light load: gentle upshifts 3-4-5 */
        float s0 = expf(-.22f * 6) * .8f * 1.31f * 1.244f; /* coast end, fraction of R */
        l = .25f;
        th = .2f;
        if (u < 27.5f) {
            g = 3;
            r = R * (s0 + (.42f - s0) * (u - 25.5f) / 2);
        } else if (u < 29.5f) {
            g = 4;
            r = R * (.42f + (.338f - .42f) * sstep((u - 27.5f) / .3f) + .03f * (u - 27.5f));
        } else {
            g = 5;
            r = R * (.398f + (.303f - .398f) * sstep((u - 29.5f) / .3f) + .005f * (u - 29.5f));
        }
        r = fmaxf(r, I + 300);
    } else if (u < 34.5f) { /* kickdown 5 -> 3, pull, upshift 3 -> 4 */
        float rs = .31f * R;
        l = .95f;
        th = 1;
        if (u < 33.2f) {
            float lin = rs * 1.63f + (hi - rs * 1.63f) * (u - 31) / 2.2f;
            g = 3;
            r = rs + (lin - rs) * sstep((u - 31) / .35f);
        } else {
            float lin = .708f * R + (.8f * R - .708f * R) * (u - 33.2f) / 1.3f;
            g = 4;
            r = hi + (lin - hi) * sstep((u - 33.2f) / .3f);
        }
    } else if (u < 38) { /* lift-off, braking, downshifts 4-3-2 */
        float s = expf(-.5f * (u - 34.5f));
        l = -.6f;
        if (u < 35.8f) {
            g = 4;
            r = .8f * R * s;
        } else if (u < 36.9f) {
            g = 3;
            r = .8f * R * s * (1 + .244f * sstep((u - 35.8f) / .3f));
        } else {
            g = 2;
            r = .8f * R * s * 1.244f * (1 + .495f * sstep((u - 36.9f) / .3f));
        }
        if (r < I + 250) {
            r = fmaxf(r, I);
            l = .06f;
        }
    } else {
        g = u < 38.3f ? 2 : 1;
    }
    *rpm = fmaxf(r, I);
    *load = l;
    *thr = th;
    *gear = g;
}

void demo_control(Synth *s, double t, double duration) {
    const Preset *p = &presets[s->selected];
    static long last = -1;
    long tick = (long)(t * 50);
    float r, l, th;
    int g;
    char line[160];
    if (t == 0 || tick < last) last = -1;
    if (tick != last) { /* telemetry at 50 Hz */
        last = tick;
        demo_state(p, (float)(t * 40 / duration), &r, &l, &th, &g);
        if (g != s->gear) {
            snprintf(line, sizeof(line), "S %d %d", s->gear, g);
            synth_control(s, line);
        }
        snprintf(line, sizeof(line), "P %.1f %.3f %.3f %d %.1f", r, l, th, g, g > 0 ? r * .006 * g : 0);
        synth_control(s, line);
    }
    if (t > duration - .5) s->mute = 1;
}
