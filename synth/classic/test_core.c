#include <stdlib.h>

#include "synth.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
int main(void) {
    Synth s;
    int16_t out[512];
    int i, j, k, p;
    char line[100];
    for (p = 0; p < NP; p++) {
        synth_init(&s, p, 1);
        assert(!synth_control(&s, "P nan 0 0 1 0"));
        assert(!synth_control(&s, "V nan"));
        for (i = 0; i < 6000; i++) {
            if (i % 20 == 0) {
                snprintf(line, sizeof(line), "P %d %f .7 3 60", i % 400 < 200 ? 0 : 10000,
                         (i % 100) / 50. - 1);
                assert(synth_control(&s, line));
            }
            synth_render(&s, out, 256);
            for (j = 0; j < NP; j++) {
                Voice *v = &s.voice[j];
                assert(isfinite(v->hp) && isfinite(v->cabin));
                for (k = 0; k < 12; k++)
                    assert(isfinite(v->c[k].slow) && isfinite(v->c[k].fast));
                for (k = 0; k < 1024; k++)
                    assert(isfinite(v->collector.buf[k]) && isfinite(v->bank[0].buf[k]) &&
                           isfinite(v->bank[1].buf[k]));
            }
            for (j = 0; j < 512; j++)
                assert(abs(out[j]) <= 29164);
        }
    }
    puts("PASS: finite internal DSP states and limiter across extreme RPM/load sweeps");
    return 0;
}
