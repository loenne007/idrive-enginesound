/* Core tests: response time of the smoothed controls, finite DSP state across extreme input, limiter bound. */
#include "synth.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

static void finite_pipe(const Pipe *w) {
    int k;
    assert(isfinite(w->lp.z1) && isfinite(w->lp.z2));
    for (k = 0; k < DLEN; k++) assert(isfinite(w->d.buf[k]) && fabsf(w->d.buf[k]) < 1e4f);
}

/* frames until the smoothed rpm covers 90 % of a P step */
static double step_ms(int preset, float from, float to) {
    static Synth s;
    int16_t out[2];
    char line[80];
    int n;
    synth_init(&s, preset, 1);
    snprintf(line, sizeof(line), "P %.0f 0.5 0.5 3 60", from);
    synth_control(&s, line);
    for (n = 0; n < SR / 2; n++) synth_render(&s, out, 1);
    snprintf(line, sizeof(line), "P %.0f 0.5 0.5 3 60", to);
    synth_control(&s, line);
    for (n = 0; n < SR; n++) {
        synth_render(&s, out, 1);
        if (fabsf(s.rpm - from) >= .9f * fabsf(to - from)) break;
    }
    return 1000. * n / SR;
}

int main(void) {
    static Synth s;
    int16_t out[512];
    int i, j, k, p;
    char line[100];
    clock_t t0 = clock();
    double up, down;
    synth_init(&s, 0, 1);
    printf("start-up (four voices calibrated): %.0f ms CPU\n", 1000. * (clock() - t0) / CLOCKS_PER_SEC);
    up = step_ms(0, 2000, 5000);
    down = step_ms(1, 6000, 2800);
    printf("rpm step to 90 %%: up %.1f ms, down %.1f ms\n", up, down);
    assert(up <= 30 && down <= 30);
    for (p = 0; p < NP; p++) {
        synth_init(&s, p, 1);
        assert(!synth_control(&s, "P nan 0 0 1 0"));
        assert(!synth_control(&s, "V nan"));
        assert(!synth_control(&s, "X bass inf"));
        assert(synth_control(&s, "X pops 2"));
        assert(synth_control(&s, "X turbo 2"));
        assert(synth_control(&s, "X bass 2"));
        assert(synth_control(&s, p & 1 ? "T 1" : "T 0"));
        for (i = 0; i < 6000; i++) {
            if (i % 20 == 0) {
                snprintf(line, sizeof(line), "P %d %f %f 3 60", i % 400 < 200 ? 0 : 10000, (i % 100) / 50. - 1,
                         (i % 70) / 69.);
                assert(synth_control(&s, line));
            }
            if (i % 997 == 0) assert(synth_control(&s, i % 2 ? "S 3 4" : "S 4 3"));
            if (i % 1500 == 0) assert(synth_control(&s, i % 3000 ? "E v8" : "E v12"));
            synth_render(&s, out, 256);
            for (j = 0; j < NP; j++) {
                Voice *v = &s.voice[j];
                assert(isfinite(v->crank) && isfinite(v->v) && isfinite(v->spool) && isfinite(v->pop_env));
                assert(v->np >= 0 && v->np <= MAXP);
                for (k = 0; k < 3; k++) assert(isfinite(v->lp[k]) && isfinite(v->prev[k]));
                for (k = 0; k < v->np; k++) assert(isfinite(v->pool[k].age) && isfinite(v->pool[k].A));
                for (k = 0; k < NCAL; k++) assert(isfinite(v->corr[0][k]) && isfinite(v->corr[1][k]));
                finite_pipe(&v->bank[0]);
                finite_pipe(&v->bank[1]);
                finite_pipe(&v->coll);
            }
            for (j = 0; j < 512; j++) assert(abs(out[j]) <= 23200); /* -3 dBFS */
        }
    }
    puts("PASS: rpm response <= 30 ms, finite internal DSP states and limiter across extreme sweeps");
    return 0;
}
