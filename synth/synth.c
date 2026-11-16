/* Lean event/pressure/waveguide synthesis inspired by Engine_Sim (MIT),
 * Copyright (c) Yigit Salih Emecen. Independent C implementation; no samples,
 * convolution or Web Audio graph port. See README for attribution. */
#include "synth.h"
#include "dsp.h"
#include <stdio.h>
#include <string.h>
static float sine[2049];
static float osc(float *phase, float hz) {
    float x, f;
    int i;
    *phase += hz / SR;
    if (*phase >= 1) *phase -= 1;
    x = *phase * 2048;
    i = (int)x;
    f = x - i;
    return sine[i] + f * (sine[i + 1] - sine[i]);
}
void synth_init(Synth *s, int p, float vol) {
    int i, j;
    memset(s, 0, sizeof(*s));
    for (i = 0; i <= 2048; i++)
        sine[i] = sinf((float)(6.283185307179586 * i / 2048));
    s->selected = p;
    s->turbo = presets[p].turbo;
    s->rpm = s->trpm = presets[p].idle;
    s->tvolume = vol;
    s->last_p = -1e9;
    for (i = 0; i < NP; i++) {
        Voice *v = &s->voice[i];
        const Preset *q = &presets[i];
        v->seed = 19871 + i * 1741;
        for (j = 0; j < q->n; j++)
            v->c[j].next = q->angle[j] / 720.;
        for (j = 0; j < 2; j++) {
            v->bank[j].len = (unsigned)(SR * 4 * q->pipe[j] / 510);
            v->bank[j].fb = i == 1 ? .43f : .29f;
        }
        v->collector.len = (unsigned)(SR * 4 * q->collector / 510);
        v->collector.fb = .18f;
    }
    s->voice[p].weights = 1;
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
        if (b > a)
            s->shift = 1;
        else if (b < a)
            s->blip = 1;
        return 1;
    }
    if (line[0] == 'Q' && sscanf(line + 1, " %c", &extra) != 1) {
        s->quit = 1;
        return 1;
    }
    return 0;
}
static float voice_step(Voice *v, const Preset *p, float rpm, float load, float throttle, int turbo,
                        int coeff) {
    float norm = clampf(rpm / p->redline, 0, 1), power = fmaxf(load, 0), bank[2] = {0, 0}, noise = rnd(v), x,
          y, flow;
    int j;
    if (coeff) {
        float dur = (.0028f - .0015f * power) / (.7f + norm);
        v->a = expf(-1 / (SR * dur));
        v->b = expf(-1 / (SR * dur * (.13f / (p->hardness + .3f))));
        v->cut = 1 - expf(-6.2831853f * (950 + norm * 1300 + power * 1000) / SR);
    }
    v->crank += rpm / (120. * SR);
    for (j = 0; j < p->n; j++) {
        Cylinder *c = &v->c[j];
        int cyl = p->order[j], b = p->n == 6 ? 0 : (cyl > p->n / 2);
        if (v->crank >= c->next) {
            float strength = (.28f + .72f * power) * (1 + rnd(v) * (p->rough + .06f * (1 - norm)));
            if (load < 0) strength *= .48f;
            c->slow += strength;
            c->fast += strength;
            c->a = v->a;
            c->b = v->b;
            c->next = floor(c->next - p->angle[j] / 720. + .01) + 1 + p->angle[j] / 720. + rnd(v) * .0007;
        }
        c->slow = clean(c->slow * c->a);
        c->fast = clean(c->fast * c->b);
        x = c->slow - c->fast;
        bank[b] += (x - c->prev) * 7;
        c->prev = x;
    }
    if (load < -.1f && norm > .22f && p->pop > 0 && rnd(v) < -1 + 2 * p->pop * (-load) * 8 / SR)
        v->popenv += .25f + .6f * fabsf(noise);
    v->popenv = clean(v->popenv * .997f);
    v->poplp += .16f * (noise * v->popenv - v->poplp);
    bank[0] += v->poplp * .4f;
    bank[1] += v->poplp * .24f;
    v->noise += .10f * (noise - v->noise);
    flow = v->noise * (.012f + .055f * power) * norm;
    for (j = 0; j < 2; j++) {
        v->radiation[j] += v->cut * (bank[j] - v->radiation[j]);
        bank[j] = v->radiation[j] + .55f * pipe_step(&v->bank[j], v->radiation[j] + flow);
    }
    /* Unequal bank path weights preserve V8 half orders in mono. */
    x = bank[0] + bank[1] * (p->n == 8 ? .72f : .93f);
    y = pipe_step(&v->collector, x);
    x = .78f * x + .22f * y;
    v->muffler += .16f * (x - v->muffler);
    x = .66f * x + .34f * v->muffler;
    /* Induction turbulence is combustion modulated; mechanical texture stays quiet. */
    x += p->intake * throttle * (v->noise * (.15f + fabsf(bank[0])) + .13f * (bank[0] + bank[1]));
    v->mech += .35f * (noise - v->mech);
    x += .007f * (noise - v->mech) * (.3f + norm);
    {
        float target = turbo ? power * norm : 0, old = v->spool;
        v->spool += (target - v->spool) * (target > v->spool ? .000035f : .00015f);
        v->bov = fmaxf(v->bov, (old - v->spool) * 110);
        v->bov = clean(v->bov * .9995f);
        x += .014f * v->spool * osc(&v->turbo_phase, 850 + 1200 * v->spool) + .055f * v->bov * v->noise;
    }
    /* one-pole DC blocker / high-pass at 35 Hz: a 6-cylinder's idle firing (35 Hz) stays, the car's woofers take it */
    v->cabin += v->cut * (x - v->cabin);
    y = v->cabin - v->last + .99543f * v->hp;
    v->last = v->cabin;
    v->hp = clean(y);
    return y * p->gain;
}
void synth_render(Synth *s, int16_t *out, unsigned frames) {
    unsigned f;
    int i;
    for (f = 0; f < frames; f++) {
        float x = 0, load, gate = (!s->mute && !s->quit && s->time - s->last_p <= 1.5) ? 1 : 0;
        s->rpm += (s->trpm - s->rpm) * .00104f;
        s->load += (s->tload - s->load) * .00069f;
        s->throttle += (s->tthrottle - s->throttle) * .00069f;
        s->speed += (s->tspeed - s->speed) * .0004f;
        s->volume += (s->tvolume - s->volume) * .00021f;
        s->gate += (gate - s->gate) * .0002083f;
        s->shift = clean(s->shift * .99965f);
        s->blip = clean(s->blip * .9997f);
        load = s->load * (1 - .32f * s->shift) + .12f * s->blip;
        for (i = 0; i < NP; i++) {
            Voice *v = &s->voice[i];
            v->weights += ((i == s->selected ? 1 : 0) - v->weights) * .0002083f;
            if (v->weights > 1e-5f) {
                x += v->weights * voice_step(v, &presets[i], s->rpm * (1 + .018f * s->blip), load,
                                             s->throttle, s->turbo, !(s->tick & 31));
            }
        }
        /* +6 dB make-up: the car's source volume sits on top; the soft limiter keeps peaks under -1 dBFS */
        x = limit(2.f * x * s->volume * s->gate);
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
        double z = (u - 9) / 3.2;
        g = 1 + (int)z;
        r = p->redline * (.38f + .47f * (float)(z - floor(z)));
        l = .9f;
        th = .94f;
    } else if (u >= 25 && u < 31) {
        g = 5 - (int)((u - 25) / 1.5);
        r = p->redline * (.75f - .07f * (float)(u - 25));
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
