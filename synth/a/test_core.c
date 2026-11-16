#include <stdlib.h>

#include "synth.h"
#include <assert.h>
#include <math.h>
#include <stdio.h>
/* Every recursive DSP state must stay finite across extreme RPM/load sweeps, and the limiter must hold. */
static void finite_pipe(const Pipe *w) {
    int k;
    assert(isfinite(w->lp.z1) && isfinite(w->lp.z2));
    for (k = 0; k < DLEN; k++) assert(isfinite(w->d.buf[k]) && fabsf(w->d.buf[k]) < 1e4f);
}
int main(void) {
    Synth s;
    int16_t out[512];
    int i, j, k, p;
    char line[100];
    for (p = 0; p < NP; p++) {
        synth_init(&s, p, 1);
        assert(!synth_control(&s, "P nan 0 0 1 0"));
        assert(!synth_control(&s, "V nan"));
        assert(synth_control(&s, p & 1 ? "T 1" : "T 0"));
        for (i = 0; i < 6000; i++) {
            if (i % 20 == 0) {
                snprintf(line, sizeof(line), "P %d %f %f 3 60", i % 400 < 200 ? 0 : 10000, (i % 100) / 50. - 1,
                         (i % 70) / 69.);
                assert(synth_control(&s, line));
            }
            if (i % 997 == 0) assert(synth_control(&s, i % 2 ? "S 3 4" : "S 4 3"));
            synth_render(&s, out, 256);
            for (j = 0; j < NP; j++) {
                Voice *v = &s.voice[j];
                assert(isfinite(v->crank) && isfinite(v->v) && isfinite(v->spool) && isfinite(v->crack_env));
                for (k = 0; k < 3; k++) assert(isfinite(v->lp[k]) && isfinite(v->prev_p[k]));
                for (k = 0; k < v->nev; k++) assert(isfinite(v->ev[k].p.ea) && isfinite(v->ev[k].p.eb));
                finite_pipe(&v->bank[0]);
                finite_pipe(&v->bank[1]);
                finite_pipe(&v->coll);
            }
            for (k = 0; k < 3; k++) assert(isfinite(s.env[k]) && isfinite(s.gr[k]));
            for (j = 0; j < 512; j++) assert(abs(out[j]) <= 29164);
        }
    }
    puts("PASS: finite internal DSP states and limiter across extreme RPM/load sweeps");
    return 0;
}
