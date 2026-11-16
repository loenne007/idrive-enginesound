/* synth.c - control protocol, voice crossfades and the master chain.
 *
 * The master chain follows YigitSalihEmecen/Engine_Sim (MIT) src/engine-sim.js and fx.js: infrasonic guard,
 * bass enhancer, rumble shelf, body bump, presence/air cuts, cabin filter, a short room and the three-band
 * dynamics, then our soft limiter (peaks <= -1 dBFS). The cabin sits part-way between Engine_Sim's exterior and
 * interior settings (CABIN): inside a car, but not muffled.
 */
#include "dsp.h"
#include <stdio.h>
#include <string.h>

#define CABIN .30f /* 0 = outside the car, 1 = Engine_Sim's full interior (1 kHz panel lowpass) */
#define ROOM .10f  /* reverb mix (Engine_Sim default .14) */
#ifndef ROOM_T60
#define ROOM_T60 .15f /* a car cabin decays in ~0.1-0.2 s */
#endif
#define OUT_GAIN 1.3f

/* three-band dynamics (fx.js Dynamics.BANDS): threshold dB, ratio, attack s, release s, knee dB, makeup */
static const float DYN[3][6] = {
    {-16, 2.6f, .018f, .26f, 10, 1.10f},
    {-18, 2.0f, .022f, .20f, 14, 2.12f},
    {-24, 3.0f, .003f, .12f, 8, .90f},
};

/* derived constants, set by synth_init */
static float dyn_ka[3], dyn_kr[3], cabin_trim, room_dry, room_wet, rv_g[4], rv_norm;

void synth_init(Synth *s, int p, float vol) {
    int i;
    float pos = CABIN;
    memset(s, 0, sizeof(*s));
    for (i = 0; i < NP; i++) voice_init(&s->voice[i], &presets[i], i);
    s->selected = p;
    s->turbo = presets[p].turbo;
    s->rpm = s->trpm = presets[p].idle;
    s->tvolume = vol;
    s->last_p = -1e9;
    s->rpm_v = s->rpm;
    s->shift_t = -1;
    s->voice[p].weight = 1;
    /* tone stage */
    for (i = 0; i < 2; i++) wa_highpass(&s->infra[i], 36, -3.0103f);
    wa_lowpass(&s->bass_lp[0], 140, -3.0103f);
    wa_lowpass(&s->bass_lp[1], 140, -3.0103f);
    wa_highpass(&s->bass_hp, 90, .7f);
    wa_lowpass(&s->bass_lp3, 420, .6f);
    bq_lowshelf(&s->rumble, 145, 2);
    bq_peak(&s->bump, 78, 1, 1.5f);
    bq_peak(&s->presence, 3000, .85f, -1.5f);
    bq_highshelf(&s->aircut, 2900, -2);
    /* cabin (resonators.js CabinFilter at position CABIN): boom 62 Hz, floorpan 155 Hz, panel lowpass, trim shelf */
    bq_peak(&s->boom, 62, 3.2f, 6.5f * pos);
    bq_peak(&s->panel, 155, 2.4f, 3.5f * pos);
    wa_lowpass(&s->cab_lp, 17000 * powf(1050.f / 17000, pos), .6f);
    bq_highshelf(&s->shelf, 3200, -11 * pos);
    /* room: small feedback delay network standing in for Engine_Sim's 0.8 s procedural IR */
    wa_highpass(&s->rv_hp, 110, 0);
    wa_lowpass(&s->rv_lp, 4200, -3.0103f);
    {
        /* cabin-sized: 7..15 ms lines, ROOM_T60 decay, wet normalised to unit energy gain for broadband input */
        static const unsigned len[4] = {331, 457, 587, 733};
        float e = 0;
        for (i = 0; i < 4; i++) {
            s->rv_len[i] = len[i];
            rv_g[i] = powf(10, -3.f * len[i] / (SR * ROOM_T60));
            e += .25f * .25f / (1 - rv_g[i] * rv_g[i]);
        }
        rv_norm = .5f / sqrtf(e * 4);
    }
    /* dynamics crossover: Linkwitz-Riley 4 (two Butterworth sections per edge) at 220 Hz and 2 kHz */
    wa_lowpass(&s->xo[0][0], 220, -3.0103f);
    wa_lowpass(&s->xo[0][1], 220, -3.0103f);
    wa_highpass(&s->xo[1][0], 220, -3.0103f);
    wa_highpass(&s->xo[1][1], 220, -3.0103f);
    wa_lowpass(&s->xo[1][2], 2000, -3.0103f);
    wa_lowpass(&s->xo[1][3], 2000, -3.0103f);
    wa_highpass(&s->xo[2][0], 2000, -3.0103f);
    wa_highpass(&s->xo[2][1], 2000, -3.0103f);
    for (i = 0; i < 3; i++) {
        s->gr[i] = 1;
        dyn_ka[i] = 1 - expf(-1 / (DYN[i][2] * SR));
        dyn_kr[i] = 1 - expf(-1 / (DYN[i][3] * SR));
    }
    cabin_trim = powf(10, -4.5f * CABIN / 20);
    room_dry = cosf(ROOM * PI_F / 2);
    room_wet = sinf(ROOM * PI_F / 2) * 1.4f;
}

int synth_control(Synth *s, const char *line) {
    float r, l, t, v;
    int g, k, a, b, n = 0;
    char name[32], extra;
    if (sscanf(line, "P %f %f %f %d %f %n", &r, &l, &t, &g, &v, &n) == 5 && n && line[n] == 0 &&
        isfinite(r) && isfinite(l) && isfinite(t) && isfinite(v) && g >= -1 && g <= 10) {
        s->trpm = clampf(r, 0, 10000);
        s->tload = clampf(l, -1, 1);
        s->tthrottle = clampf(t, 0, 1);
        s->tspeed = clampf(v, 0, 400);
        s->gear = g;
        s->last_p = s->time;
        return 1;
    }
    if (sscanf(line, "E %31s %c", name, &extra) == 1 && (k = preset_find(name)) >= 0) {
        s->selected = k;
        s->turbo = presets[k].turbo;
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
    if (sscanf(line, "V %f %c", &v, &extra) == 1 && isfinite(v) && v >= 0 && v <= 1) {
        s->tvolume = v;
        return 1;
    }
    if (sscanf(line, "S %d %d %c", &a, &b, &extra) == 2 && a >= -1 && a <= 10 && b >= -1 && b <= 10) {
        /* An automatic (ZF 8HP-like) shifts under load without an audible cut: an upshift is a gentle torque
         * reduction, a downshift a small rev-match blip. The rpm itself comes from the telemetry. */
        if (b > a && a > 0)
            s->shift_t = 0;
        else if (b < a && b > 0)
            s->blip = 1;
        return 1;
    }
    if (line[0] == 'Q' && sscanf(line + 1, " %c", &extra) != 1) {
        s->quit = 1;
        return 1;
    }
    return 0;
}

/* compressor gain for a detector level (linear), soft knee, no auto-makeup */
static float comp_gain(float env, const float *c) {
    float db = 20 * log10f(env + 1e-9f), over = db - c[0], knee = c[4], red;
    if (over <= -knee / 2)
        return 1;
    if (over >= knee / 2)
        red = over * (1 - 1 / c[1]);
    else {
        float u = over + knee / 2;
        red = u * u / (2 * knee) * (1 - 1 / c[1]);
    }
    return powf(10, -red / 20);
}

static float master(Synth *s, float x) {
    float y, b, wet = 0, band[3], out = 0;
    int i;
    x *= .95f;
    x = bq(&s->infra[1], bq(&s->infra[0], x));
    /* bass enhancer: missing-fundamental harmonics of the low end for small speakers */
    b = bq(&s->bass_lp[1], bq(&s->bass_lp[0], x));
    b = tanh_f(2.2f * b);
    b = b + .45f * b * b;
    y = x + .15f * bq(&s->bass_lp3, bq(&s->bass_hp, b));
    y = bq(&s->aircut, bq(&s->presence, bq(&s->bump, bq(&s->rumble, y))));
    /* cabin */
    y = bq(&s->shelf, bq(&s->cab_lp, bq(&s->panel, bq(&s->boom, y)))) * cabin_trim;
    /* room: 4-line FDN (Householder mix), cabin-sized (~0.15 s decay), band-limited 110 Hz .. 4.2 kHz. A longer,
     * stronger room (0.5 s) had sharp modes: the near-sinusoidal firing harmonic crossing one swelled the whole
     * engine by 4-5 dB, heard as an onset in every upshift sweep. */
    {
        float in = bq(&s->rv_hp, y), d[4], m;
        for (i = 0; i < 4; i++) d[i] = s->rv_buf[i][(s->rv_w - s->rv_len[i]) & 4095];
        m = .5f * (d[0] + d[1] + d[2] + d[3]);
        for (i = 0; i < 4; i++) s->rv_buf[i][s->rv_w & 4095] = clean(rv_g[i] * (d[i] - m) + .5f * in);
        s->rv_w++;
        wet = bq(&s->rv_lp, d[0] - d[1] + d[2] - d[3]) * rv_norm;
        y = y * room_dry + wet * room_wet;
    }
    /* three-band dynamics: loud stays bright, harsh gets held down; the mid band carries +6.5 dB of makeup */
    band[0] = bq(&s->xo[0][1], bq(&s->xo[0][0], y));
    band[1] = bq(&s->xo[1][3], bq(&s->xo[1][2], bq(&s->xo[1][1], bq(&s->xo[1][0], y))));
    band[2] = bq(&s->xo[2][1], bq(&s->xo[2][0], y));
    for (i = 0; i < 3; i++) {
        float a = fabsf(band[i]);
        s->env[i] = clean(s->env[i] + (a - s->env[i]) * (a > s->env[i] ? dyn_ka[i] : dyn_kr[i]));
        out += band[i] * s->gr[i] * DYN[i][5];
    }
    return out;
}

void synth_render(Synth *s, int16_t *out, unsigned frames) {
    unsigned f;
    int i;
    for (f = 0; f < frames; f++) {
        float x = 0, load, gate = (!s->mute && !s->quit && s->time - s->last_p <= 1.5) ? 1 : 0;
        /* audio-rate parameter smoothing (~20 ms rpm, ~30 ms load) */
        /* rpm: slew-limited to 8000 rpm/s (a telemetry step never sounds like a cut or a restart), then a
         * ~20 ms one-pole rounds the corners of the ramp */
        s->rpm_v += clampf(s->trpm - s->rpm_v, -8000.f / SR, 8000.f / SR);
        s->rpm += (s->rpm_v - s->rpm) * .00104f;
        s->load += (s->tload - s->load) * .00069f;
        s->throttle += (s->tthrottle - s->throttle) * .00069f;
        s->speed += (s->tspeed - s->speed) * .0004f;
        s->volume += (s->tvolume - s->volume) * .00021f;
        s->gate += (gate - s->gate) * .0002083f;
        /* upshift torque reduction: a smooth bump u^2(1-u)^3 over 320 ms (C1 at both ends, peak at 128 ms, ahead
         * of the rpm drop like a transmission's torque intervention), load -15 % at its peak; combustion never
         * stops and nothing restarts */
        load = s->load;
        if (s->shift_t >= 0) {
            float u = s->shift_t / .32f, e = u * u * (1 - u) * (1 - u) * (1 - u) * 28.935f;
            if (load > 0) load *= 1 - .15f * e;
            s->shift_t += 1.f / SR;
            if (s->shift_t >= .32f) s->shift_t = -1;
        }
        /* downshift blip: ~30 ms rise, ~120 ms decay, +0.2 load at most */
        s->blip = clean(s->blip * (1 - 1.f / (.12f * SR)));
        s->blip_env = clean(s->blip_env + (s->blip - s->blip_env) * (1.f / (.03f * SR)));
        load = clampf(load + .2f * s->blip_env, -1, 1);
        for (i = 0; i < NP; i++) {
            Voice *v = &s->voice[i];
            v->weight += ((i == s->selected ? 1 : 0) - v->weight) * .0002083f;
            if (v->weight > 1e-5f)
                x += v->weight * voice_step(v, &presets[i], s->rpm, load, s->throttle, s->turbo) * presets[i].gain;
        }
        x = master(s, x);
        /* compressor detectors per sample, gains at control rate */
        if (!(s->tick % CTRL))
            for (i = 0; i < 3; i++) s->gr[i] = comp_gain(s->env[i], DYN[i]);
        x = limit(OUT_GAIN * x * s->volume * s->gate);
        if (!isfinite(x)) x = 0;
        out[2 * f] = out[2 * f + 1] = (int16_t)(x * 32767);
        s->time += 1. / SR;
        s->tick++;
    }
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
        /* pulls through gears 1-5, 0.38 -> 0.85 of redline per gear. Like the car's telemetry, the rpm falls to
         * the next gear over ~350 ms (smoothstep), it does not jump. */
        double z = (u - 9) / 3.2, f = z - floor(z), sh = .35 / 3.2;
        g = 1 + (int)z;
        r = p->redline * (.38f + .47f * (float)f);
        if (g > 1 && f < sh) {
            float k = (float)(f / sh);
            k = k * k * (3 - 2 * k);
            r = p->redline * (.85f + (.38f + .47f * (float)f - .85f) * k);
        }
        l = .9f;
        th = .94f;
    } else if (u >= 25 && u < 31) {
        /* coasting down 5-4-3-2: road speed falls, each downshift raises the revs smoothly over ~300 ms */
        static const float ratio[4] = {1, 1.25f, 1.6f, 2.1f};
        double z = (u - 25) / 1.5, f = z - floor(z);
        int k = (int)z;
        float speed = 1 - .12f * (float)(u - 25), from = ratio[k > 0 ? k - 1 : 0], to = ratio[k];
        if (f < .2 && k > 0) {
            float w = (float)(f / .2);
            to = from + (to - from) * w * w * (3 - 2 * w);
        }
        g = 5 - k;
        r = p->redline * .75f * speed * to;
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
