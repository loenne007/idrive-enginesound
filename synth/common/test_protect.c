/* test_protect.c: the speaker protection against signals that would hurt speakers. Exit 1 on a failure. */
#include "protect.h"
#include <stdio.h>
#include <stdlib.h>
#define SR 48000
#define N (SR * 8)
static int16_t buf[N * 2];
static int fail;
static void check(const char *what, int ok) { printf("%s %s\n", ok ? "PASS" : "FAIL", what); if (!ok) fail = 1; }
/* fill: f(t) for 8 s, clipped to full scale; run the protection; measure the last 4 s */
static void run(const char *name, double (*f)(double), double *peak, double *rms, double *dc) {
    Protect p; unsigned i; double s = 0, ss = 0, pk = 0;
    for (i = 0; i < N; i++) { double v = f((double)i / SR); if (v > 1) v = 1; if (v < -1) v = -1;
        buf[2 * i] = buf[2 * i + 1] = (int16_t)(v * 32767); }
    protect_init(&p);
    for (i = 0; i < N; i += 256) protect_run(&p, buf + 2 * i, 256);
    for (i = N / 2; i < N; i++) { double v = buf[2 * i] / 32768.0; s += v; ss += v * v; if (fabs(v) > pk) pk = fabs(v); }
    *peak = 20 * log10(pk + 1e-12); *rms = 10 * log10(ss / (N / 2) + 1e-12); *dc = s / (N / 2);
    printf("  %-28s peak %6.2f dBFS  rms %6.2f dBFS  dc %+.5f\n", name, *peak, *rms, *dc);
}
static double dc_(double t) { (void)t; return 0.9; }
static double infra(double t) { return sin(2 * M_PI * 10 * t); }
static double bass(double t) { return 1.5 * sin(2 * M_PI * 45 * t) + 1.5 * sin(2 * M_PI * 90 * t); }
static double loud(double t) { return 3 * sin(2 * M_PI * 220 * t) * (0.5 + 0.5 * sin(2 * M_PI * 3 * t)); }
static double noise(double t) { (void)t; return 2.0 * rand() / RAND_MAX - 1; }
static double step(double t) { return t > 1 ? 1 : -1; }
int main(void) {
    double pk, rms, dc;
    run("DC 0.9", dc_, &pk, &rms, &dc);           check("DC removed", fabs(dc) < 0.001 && rms < -60);
    run("10 Hz full scale", infra, &pk, &rms, &dc); check("infrasound cut (>= 24 dB down)", rms < -3 - 24);
    run("45+90 Hz, 3x full scale", bass, &pk, &rms, &dc); check("bass held under -8 dBFS peak", pk < -8);
    run("220 Hz, clipped, 3 Hz AM", loud, &pk, &rms, &dc); check("peak <= -1 dBFS", pk <= -1.0); check("power <= -13 dBFS RMS", rms <= -13);
    run("white noise full scale", noise, &pk, &rms, &dc); check("noise peak <= -1 dBFS", pk <= -1.0); check("noise power <= -13 dBFS", rms <= -13);
    run("full-scale step", step, &pk, &rms, &dc);   check("step: no DC left", fabs(dc) < 0.001);
    return fail;
}
