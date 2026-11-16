#include <stdlib.h>

#include "synth.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>

/* internal states stay finite and the output stays under the limiter ceiling across extreme rpm/load sweeps,
 * shifts, turbo toggles and engine switches */
static void check_voice(const Voice *v) {
    int k, i;
    if (!v->active) return;
    for (k = 0; k < 3; k++) assert(isfinite(v->comb.lp[k]) && isfinite(v->comb.prevP[k]));
    assert(isfinite(v->comb.v) && fabs(v->comb.v) <= 0.2);
    for (k = 0; k < v->ex.nb; k++)
        for (i = 0; i < 2048; i++) assert(isfinite(v->ex.bank[k].buf[i]) && fabsf(v->ex.bank[k].buf[i]) < 1e4f);
    for (i = 0; i < 2048; i++) assert(isfinite(v->ex.collector.buf[i]));
    for (i = 0; i < QN; i++) assert(isfinite(v->ex.bank[0].lprev[i]));
    assert(isfinite(v->tu.spool) && isfinite(v->tu.surge));
}
int main(void) {
    static Synth s;
    int16_t out[512];
    int i, j, p, peak = 0;
    char line[100];
    for (p = 0; p < NP; p++) {
        synth_init(&s, p, 1);
        assert(!synth_control(&s, "P nan 0 0 1 0"));
        assert(!synth_control(&s, "V nan"));
        assert(!synth_control(&s, "X unknown 1"));
        assert(!synth_control(&s, "X bass nan"));
        for (i = 0; i < 3000; i++) {
            if (i % 4 == 0) {
                snprintf(line, sizeof(line), "P %d %f %f %d 60", i % 400 < 200 ? 0 : 10000, (i % 100) / 50. - 1,
                         (i % 70) / 69., 1 + (i / 50) % 8);
                assert(synth_control(&s, line));
            }
            if (i % 700 == 350) assert(synth_control(&s, (i / 700) % 2 ? "X bass 2" : "X pops 0"));
            if (i % 300 == 150) assert(synth_control(&s, (i / 300) % 2 ? "S 3 4" : "S 4 3"));
            if (i % 500 == 250) assert(synth_control(&s, (i / 500) % 2 ? "T 0" : "T 1"));
            if (i % 900 == 450) {
                snprintf(line, sizeof(line), "E %s", presets[(p + i / 900 + 1) % NP].name);
                assert(synth_control(&s, line));
            }
            synth_render(&s, out, 256);
            for (j = 0; j < NP; j++) check_voice(&s.voice[j]);
            for (j = 0; j < 512; j++) {
                assert(abs(out[j]) <= 29164);
                if (abs(out[j]) > peak) peak = abs(out[j]);
            }
        }
    }
    printf("PASS: finite internal DSP states and limiter across extreme sweeps (peak %d / 29164)\n", peak);
    return 0;
}
