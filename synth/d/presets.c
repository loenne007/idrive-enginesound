/* presets.c - engine D's four calibrations (the ids the app sends stay i6 / v8 / v10 / v12).
 *
 * Order tables: gain in dB re the common reference amplitude at FULL positive load at the rpm knots
 * 800 / 1600 / 2800 / 4300 rpm (piecewise linear in dB between knots, clamped outside), and the off-gas offset
 * (dB added at zero load: idle, light cruise, coasting). Intermediate load interpolates in dB. Orders whose
 * frequency is under ~32 Hz are faded out by the synth (at 800 rpm order 1.5 is 20 Hz). The cells start from the
 * research report's proposal (research4/REPORT.md, section 4) and were tuned by measurement and spectrogram; they
 * are design targets, not recovered factory calibrations. The real B47 supplies order 2 in the car: the six-
 * cylinder presets add none, the V8 only a little.
 */
#include "synth.h"
#include <string.h>

const Preset presets[NP] = {
    /* BMW B57 straight-six diesel ASD (X7 / X5 xDrive40d): smooth and deep. Order 3 carries it, 1.5 and (higher up)
     * order 1 add depth under it, a little order 6 gives definition on small speakers at low rpm. */
    {"i6", 6, 0, 800, 5000, 5,
     {
         {1.0f, {-14, -10, -6, -4}, {-4, -4, -4, -4}},
         {1.5f, {-10, -6, -4, -4}, {-4, -4, -4, -4}},
         {3.0f, {0, 0, -1, -2}, {-4, -4, -4, -4}},
         {4.5f, {-24, -22, -22, -24}, {-4, -4, -4, -4}},
         {6.0f, {-12, -11, -11, -13}, {-4, -4, -4, -4}},
     },
     {0, 0, 0, 0, 0, 0, 0, 0}, 0.f, .025f, .045f, .10f, 0.f, 0.f, .5f, -51.f, 140, 185, 1.f, .5f, -36.f, -5.f, .8f},
    /* Audi 4.0 V8 TDI (SQ7): order 4 (its firing order) and the deep order 1 / 3 rumble of the uneven per-bank
     * firing (1-5-4-8-6-3-7-2: banks A B A B B A B A). Order 2 stays a trace: it would beat against the B47's own. */
    {"v8", 8, 0, 800, 5000, 7,
     {
         {1.0f, {-10, -6, -3, -3}, {-4, -4, -4, -4}},
         {1.5f, {-20, -18, -18, -20}, {-4, -4, -4, -4}},
         {2.0f, {-26, -24, -26, -30}, {-4, -4, -4, -4}},
         {2.5f, {-20, -18, -18, -20}, {-4, -4, -4, -4}},
         {3.0f, {-6, -5, -5, -6}, {-4, -4, -4, -4}},
         {4.0f, {-1, -1, -2, -3}, {-4, -4, -4, -4}},
         {6.0f, {-16, -14, -14, -16}, {-4, -4, -4, -4}},
     },
     {1, -1, 1, -1, -1, 1, -1, 1}, .05f, .02f, .05f, .15f, 0.f, 0.f, .7f, -50.f, 140, 185, 2.f, .8f, -37.f, -6.f, .8f},
    /* Audi 3.0 V6 TDI (S6 / S7): the burble. Two banks of three (1-4-3-6-2-5): order 1.5 nearly as strong as 3, 4.5
     * under them, all lumping slowly; they stay up off the gas and grow on the overrun. */
    {"v10", 6, 0, 800, 5000, 6,
     {
         {1.0f, {-12, -9, -6, -5}, {-4, -4, -4, -4}},
         {1.5f, {-4, -2, -2, -3}, {-4, -4, -4, -4}},
         {3.0f, {-2, 0, -1, -2}, {-4, -4, -4, -4}},
         {4.5f, {-9, -8, -9, -12}, {-4, -4, -4, -4}},
         {6.0f, {-15, -13, -13, -15}, {-4, -4, -4, -4}},
         {7.5f, {-22, -21, -22, -24}, {-4, -4, -4, -4}},
     },
     {1, -1, 1, -1, 1, -1, 0, 0}, .04f, .02f, .055f, .28f, -1.f, 1.f, 0.f, -51.f, 140, 185, 2.f, 1.f, -36.f, -4.f, -.4f},
    /* the same V6 TDI in a sport / dynamic calibration: more half orders, deeper lump, a little more definition
     * under load, more overrun burble */
    {"v12", 6, 0, 800, 5000, 6,
     {
         {1.0f, {-11, -8, -5, -4}, {-4, -4, -4, -4}},
         {1.5f, {-3, -1, -1, -2}, {-4, -4, -4, -4}},
         {3.0f, {-2, 0, 0, -1}, {-4, -4, -4, -4}},
         {4.5f, {-7, -6, -7, -10}, {-4, -4, -4, -4}},
         {6.0f, {-14, -12, -11, -13}, {-4, -4, -4, -4}},
         {7.5f, {-20, -19, -20, -22}, {-4, -4, -4, -4}},
     },
     {1, -1, 1, -1, 1, -1, 0, 0}, .05f, .02f, .06f, .32f, -1.f, 1.f, 0.f, -50.f, 145, 200, 2.5f, 1.5f, -34.f, -2.f, -2.5f},
};

int preset_find(const char *s) {
    int i;
    for (i = 0; i < NP; i++)
        if (!strcmp(s, presets[i].name)) return i;
    return -1;
}
