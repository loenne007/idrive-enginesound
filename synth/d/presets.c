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
    /* BMW B57 straight-six diesel ASD (X7 / X5 xDrive40d): smooth, deep, refined; order 3 carries it, 6 and 9 add
     * the load growl, the 1.5 / 4.5 half orders are only a faint roughness (an inline six has no separate banks). */
    {"i6", 6, 0, 800, 5000, 8,
     {
         {1.0f, {-30, -29, -31, -36}, {-3, -3, -4, -4}},
         {1.5f, {-27, -24, -26, -32}, {-5, -6, -6, -6}},
         {3.0f, {-2, 0, -1, -4}, {-6, -6, -6, -6}},
         {4.5f, {-29, -26, -26, -30}, {-8, -8, -9, -9}},
         {6.0f, {-13, -10, -9, -11}, {-9, -10, -10, -11}},
         {7.5f, {-37, -34, -32, -36}, {-11, -12, -12, -12}},
         {9.0f, {-29, -24, -21, -24}, {-12, -13, -14, -14}},
         {12.0f, {-36, -30, -27, -30}, {-14, -15, -16, -16}},
     },
     {0, 0, 0, 0, 0, 0, 0, 0}, 0.f, .025f, .045f, .10f, 0.f, 0.f, 2.5f, -51.f, 430, 820, 2.f, .5f, -36.f, -5.f, 0.f},
    /* Audi 4.0 V8 TDI (SQ7): cross-plane V8, order 4 main, a restrained order 2 (the B47 has its own), the uneven
     * per-bank firing (1-5-4-8-6-3-7-2: banks A B A B B A B A) gives the deep 1 / 3 rumble, 6 and 8 the push. */
    {"v8", 8, 0, 800, 5000, 10,
     {
         {1.0f, {-20, -22, -26, -32}, {-2, -3, -3, -3}},
         {1.5f, {-31, -29, -31, -36}, {-3, -3, -3, -3}},
         {2.0f, {-26, -24, -27, -34}, {-3, -4, -4, -4}},
         {2.5f, {-32, -30, -31, -36}, {-4, -4, -4, -4}},
         {3.0f, {-19, -18, -20, -26}, {-4, -4, -5, -5}},
         {4.0f, {-3, 0, -1, -5}, {-6, -6, -6, -6}},
         {5.0f, {-30, -27, -27, -31}, {-8, -8, -8, -8}},
         {6.0f, {-20, -15, -15, -20}, {-9, -10, -10, -10}},
         {8.0f, {-23, -15, -13, -17}, {-11, -12, -13, -13}},
         {10.0f, {-36, -30, -28, -31}, {-13, -14, -15, -15}},
     },
     {1, -1, 1, -1, -1, 1, -1, 1}, .05f, .02f, .05f, .15f, 0.f, 1.f, 2.5f, -50.f, 400, 760, 3.f, .8f, -37.f, -6.f, .6f},
    /* Audi 3.0 V6 TDI (S6 / S7): the burble. Two banks of three (1-4-3-6-2-5) give the 1.5 / 4.5 / 7.5 half-order
     * family on top of order 3; they stay up off the gas and grow on the overrun, and wobble slowly. */
    {"v10", 6, 0, 800, 5000, 8,
     {
         {1.0f, {-26, -27, -31, -36}, {-3, -3, -4, -4}},
         {1.5f, {-12, -9, -11, -19}, {-3, -4, -5, -5}},
         {3.0f, {-3, 0, -1, -5}, {-6, -6, -6, -6}},
         {4.5f, {-11, -10, -12, -21}, {-4, -5, -6, -6}},
         {6.0f, {-16, -12, -11, -15}, {-9, -10, -10, -11}},
         {7.5f, {-22, -20, -22, -29}, {-7, -8, -9, -9}},
         {9.0f, {-32, -27, -25, -29}, {-12, -13, -14, -14}},
         {10.5f, {-34, -31, -31, -36}, {-12, -13, -14, -14}},
     },
     {1, -1, 1, -1, 1, -1, 0, 0}, .04f, .02f, .055f, .28f, -1.f, 2.5f, 2.f, -51.f, 400, 780, 3.f, 1.f, -36.f, -4.f, -.3f},
    /* the same V6 TDI in a sport / dynamic calibration: more half orders and less off-gas reduction, deeper wobble,
     * brighter under load, more overrun burble */
    {"v12", 6, 0, 800, 5000, 8,
     {
         {1.0f, {-25, -25, -29, -34}, {-3, -3, -3, -3}},
         {1.5f, {-10, -7, -8, -16}, {-2, -3, -4, -4}},
         {3.0f, {-2, 0, 0, -3}, {-5, -5, -5, -5}},
         {4.5f, {-9, -8, -9, -18}, {-3, -4, -5, -5}},
         {6.0f, {-15, -10, -9, -12}, {-8, -9, -9, -10}},
         {7.5f, {-20, -17, -18, -25}, {-6, -7, -8, -8}},
         {9.0f, {-30, -24, -21, -25}, {-11, -12, -13, -13}},
         {10.5f, {-32, -28, -27, -32}, {-11, -12, -13, -13}},
     },
     {1, -1, 1, -1, 1, -1, 0, 0}, .05f, .02f, .06f, .32f, -1.f, 2.f, .5f, -50.f, 430, 860, 3.5f, 1.5f, -34.f, -2.f, -2.f},
};

int preset_find(const char *s) {
    int i;
    for (i = 0; i < NP; i++)
        if (!strcmp(s, presets[i].name)) return i;
    return -1;
}
