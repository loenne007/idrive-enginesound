/* test_core.c - internal checks of the Bass engine: finite states and output bounds on extreme input, rpm response
 * (step and ramp), engine-off silence, and that X turbo / bov / pops = 0 really remove those layers. */
#include "synth.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static Synth s;
static int16_t out[2 * BLOCK];

static void send(const char *fmt, double a, double b, double c, int g, double v) {
    char line[160];
    snprintf(line, sizeof(line), fmt, a, b, c, g, v);
    assert(synth_control(&s, line));
}
static float block_peak(void) {
    int j;
    float m = 0;
    for (j = 0; j < 2 * BLOCK; j++) m = fmaxf(m, fabsf(out[j] / 32768.f));
    return m;
}
static void finite(void) {
    int k;
    assert(isfinite(s.psi) && isfinite(s.rpm) && isfinite(s.rpm1) && isfinite(s.load));
    for (k = 0; k < NH; k++) assert(isfinite(s.g[k]) && isfinite(s.gt[k]) && s.g[k] < 1);
    assert(isfinite(s.f6.am) && isfinite(s.f8.am) && isfinite(s.wob2) && isfinite(s.jit));
    assert(isfinite(s.tex_bp.z1) && isfinite(s.cab[0].z1) && isfinite(s.shelf.z1) && isfinite(s.hp.z1));
    assert(isfinite(s.wh_g) && isfinite(s.wh_ph) && isfinite(s.bov_bp.z1) && isfinite(s.pop_lp.z1) && isfinite(s.tex_lp.z1));
}

/* a lift-off from boost: 2.5 s full load at 3500 rpm, then the pedal closes and the engine coasts; returns the
 * largest pop envelope, bov contribution and whistle level seen after the lift */
static void lift(const char *x1, const char *x2, const char *x3, float *pop, float *bov, float *wh) {
    int i;
    synth_init(&s, 2, 1);
    assert(synth_control(&s, "T 1"));
    if (x1) assert(synth_control(&s, x1));
    if (x2) assert(synth_control(&s, x2));
    if (x3) assert(synth_control(&s, x3));
    *pop = *bov = *wh = 0;
    for (i = 0; i < 900; i++) {
        double t = i * (double)BLOCK / SR;
        if (t < 2.5)
            send("P %.1f %.2f %.2f %d %.1f", 3500, .95, 1, 3, 70);
        else
            send("P %.1f %.2f %.2f %d %.1f", 3500 - 500 * (t - 2.5), -.6, 0, 3, 70);
        synth_render(&s, out, BLOCK);
        if (t > 2.5) {
            *pop = fmaxf(*pop, s.pop_env * s.pop_amp);
            *bov = fmaxf(*bov, s.bov_env * s.bov_amp * s.x[2] /* bov */);
        }
        if (t > 1.5 && t < 2.5) *wh = fmaxf(*wh, s.wh_g);
    }
}

int main(void) {
    int i, p, k;
    char line[160];
    float pop, bov, wh;
    /* malformed input is ignored */
    synth_init(&s, 0, 1);
    assert(!synth_control(&s, "P nan 0 0 1 0"));
    assert(!synth_control(&s, "P 2000 0 0 11 0"));
    assert(!synth_control(&s, "P 2000 0 0 1 0 junk"));
    assert(!synth_control(&s, "V nan"));
    assert(synth_control(&s, "V 2") && !synth_control(&s, "V 2.5"));   /* volume 0..2 */
    assert(!synth_control(&s, "X bass nan"));
    assert(!synth_control(&s, "X unknown 1"));
    assert(!synth_control(&s, "E v6"));
    assert(synth_control(&s, "X bass 5") && s.x_t[4] == 2);
    assert(synth_control(&s, "X pops -1") && s.x_t[3] == 0);
    /* extreme sweeps: rpm 0 / 8000 jumps, load -1..1, all settings at 2, preset switching, shifts */
    for (p = 0; p < NP; p++) {
        synth_init(&s, p, 1);
        for (k = 0; k < 5; k++) {
            static const char *const X[5] = {"offgas", "turbo", "bov", "pops", "bass"};
            snprintf(line, sizeof(line), "X %s 2", X[k]);
            assert(synth_control(&s, line));
        }
        for (i = 0; i < 4000; i++) {
            if (i % 4 == 0)
                send("P %.0f %.3f %.3f %d %.0f", i % 400 < 200 ? (i % 800 < 400 ? 0 : 8000) : 800 + (i % 37) * 120,
                     (i % 100) / 50. - 1, (i % 70) / 69., 1 + i % 8, 80);
            if (i % 997 == 0) assert(synth_control(&s, i % 2 ? "S 3 4" : "S 4 3"));
            if (i % 1300 == 0) {
                snprintf(line, sizeof(line), "E %s", presets[(p + i / 1300) % NP].name);
                assert(synth_control(&s, line));
            }
            synth_render(&s, out, BLOCK);
            finite();
            assert(block_peak() <= .708f); /* our own ceiling: -3 dBFS */
        }
    }
    /* rpm step 1500 -> 3000: 90 % within 30 ms (no slew limit), no overshoot */
    synth_init(&s, 2, 1);
    for (i = 0; i < 200; i++) {
        send("P %.1f %.2f %.2f %d %.1f", 1500, .5, .5, 3, 50);
        synth_render(&s, out, BLOCK);
    }
    send("P %.1f %.2f %.2f %d %.1f", 3000, .5, .5, 3, 50);
    {
        int n = 0;
        float peak = 0;
        double t90 = -1;
        for (i = 0; i < 40; i++) {
            synth_render(&s, out, 32);
            n += 32;
            if (t90 < 0 && s.rpm >= 2850) t90 = (double)n / SR;
        }
        for (i = 0; i < 100; i++) {
            synth_render(&s, out, 32);
            peak = fmaxf(peak, s.rpm);
        }
        printf("rpm step 1500 -> 3000: 90 %% after %.1f ms, overshoot %.1f rpm\n", t90 * 1000, peak - 3000);
        assert(t90 > 0 && t90 <= .030 && peak < 3010);
    }
    /* a 50 Hz telemetry ramp (-4000 rpm/s, a ZF upshift) that stops dead: lag during the ramp and overshoot */
    synth_init(&s, 2, 1);
    {
        float worst = 0, over = 0, r = 4500;
        double t = 0;
        int n;
        for (n = 0; n < 48000 * 2; n += 32, t += 32.0 / SR) {
            static double next = 0;
            if (t >= next) {
                r = t < 1 ? 4500 : t < 1.35 ? 4500 - 4000 * (float)(t - 1) : 3100;
                send("P %.1f %.2f %.2f %d %.1f", r, .9, 1, 3, 60);
                next += .02;
            }
            synth_render(&s, out, 32);
            if (t > 1.05 && t < 1.35) {
                float truth = 4500 - 4000 * (float)(t - 1);
                worst = fmaxf(worst, fabsf(s.rpm - truth));
            }
            if (t > 1.35) over = fmaxf(over, 3100 - s.rpm);
        }
        printf("50 Hz ramp -4000 rpm/s: tracking error %.0f rpm (%.0f ms of ramp), undershoot at its end %.0f rpm\n",
               worst, worst / 4.0, over);
        assert(worst < 120 && over < 100);
    }
    /* engine off: rpm 0 fades to silence */
    synth_init(&s, 2, 1);
    for (i = 0; i < 400; i++) {
        send("P %.1f %.2f %.2f %d %.1f", i < 200 ? 800 : 0, .05, 0, 0, 0);
        synth_render(&s, out, BLOCK);
        if (i > 260) assert(block_peak() < 1e-3f);
    }
    /* X turbo / bov / pops: on by default, 0 removes them */
    lift(0, 0, 0, &pop, &bov, &wh);
    printf("lift-off defaults: pop %.4f, sigh %.4f, whistle %.5f\n", pop, bov, wh);
    assert(pop > 0 && bov > 0 && wh > 0);
    lift("X pops 0", "X bov 0", "X turbo 0", &pop, &bov, &wh);
    assert(pop == 0 && bov == 0 && wh == 0);
    lift("T 0", 0, 0, &pop, &bov, &wh);
    assert(wh == 0 && bov > 0 && pop > 0);
    /* idle, cruise, coasting (and turbo on): no layer but the orders is active */
    {
        static const float ST[3][5] = {{800, .05f, 0, 1, 0}, {2000, .2f, .2f, 6, 80}, {2500, -.5f, 0, 5, 80}};
        for (k = 0; k < 3; k++) {
            synth_init(&s, 3, 1);
            assert(synth_control(&s, "T 1") && synth_control(&s, "X pops 2") && synth_control(&s, "X bov 2"));
            for (i = 0; i < 600; i++) {
                send("P %.1f %.2f %.2f %d %.1f", ST[k][0], ST[k][1], ST[k][2], (int)ST[k][3], ST[k][4]);
                synth_render(&s, out, BLOCK);
                assert(s.tex_lvl == 0 && s.wh_g == 0 && s.bov_env == 0 && s.pop_env == 0);
            }
        }
    }
    /* a shift during the lift does not start a pop window */
    synth_init(&s, 3, 1);
    for (i = 0; i < 600; i++) {
        double t = i * (double)BLOCK / SR;
        if (fabs(t - 2.5) < .003) assert(synth_control(&s, "S 3 4"));
        send("P %.1f %.2f %.2f %d %.1f", t < 2.5 ? 3500 : 3500 - 1000 * fmin(t - 2.5, .3), t < 2.5 ? .9 : t < 2.65 ? -.2 : .9,
             1, 3, 70);
        synth_render(&s, out, BLOCK);
        assert(s.pop_env == 0);
    }
    puts("PASS: finite DSP states, -3 dBFS ceiling, rpm response, engine off, X turbo/bov/pops off, no shift pops, no layer at idle/cruise/coast");
    return 0;
}
