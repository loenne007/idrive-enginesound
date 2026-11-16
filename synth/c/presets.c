/* Engine presets.
 *
 * Firing geometry is BMW's: B58 1-5-3-6-2-4 (one path: the twin-scroll turbine merges the scrolls); S63-style
 * cross-plane V8 1-5-4-8-6-3-7-2 with banks 1-4 / 5-8 (bank A fires at 0/180/450/630 degrees, bank B at
 * 90/270/360/540: the uneven per-bank gaps, through pipes of different length, are the burble); S85 V10
 * 1-6-5-10-2-7-3-8-4-9 with alternating 90/54-degree intervals; V12 at even 60 degrees.
 * Pipe lengths, reflection, damping, muffler chambers and airbox start from YigitSalihEmecen/Engine_Sim
 * src/profiles.js (MIT) i6 / v8cross / v10 / v12, as engine A uses them. Idle and redline are the app's numbers
 * and must not change.
 */
#include "synth.h"
#include <string.h>

const Preset presets[NP] = {
    {
        .name = "i6", .n = 6, .turbo = 1, .idle = 700, .redline = 7000,
        .order = {1, 5, 3, 6, 2, 4},
        .angle = {0, 120, 240, 360, 480, 600},
        .banks = 1, .split = 6,
        .pulse_w = 1.0f, .cov = .022f, .jitter = .45f, .trim = .006f,
        .inertia = .27f, .torque = 450, .c_hot = 440,
        .bank_len = {1.55f, 1.55f}, .collector = 1.20f, .refl = .54f, .damping = .38f,
        .muffler = {.33f, .21f, .14f},
        .helmholtz = 95, .intake_q = 6.4f, .intake_level = .36f, .induction = .55f,
        .rasp = .42f, .rasp_hz = 1500, .shaper = .85f,
        .sub = .26f, .sub_order = 0,
        .knee = 46, .crackle = .6f, .gain = 1.0f,
    },
    {
        .name = "v8", .n = 8, .turbo = 1, .idle = 650, .redline = 7200,
        .order = {1, 5, 4, 8, 6, 3, 7, 2},
        .angle = {0, 90, 180, 270, 360, 450, 540, 630},
        .banks = 2, .split = 4, .bank_diff = .2f,
        .pulse_w = .95f, .cov = .032f, .jitter = .9f, .trim = .03f,
        .inertia = .38f, .torque = 600, .c_hot = 432,
        .bank_len = {1.28f, 1.32f}, .collector = 1.10f, .refl = .63f, .damping = .34f,
        .muffler = {.40f, .26f, .17f},
        .helmholtz = 78, .intake_q = 5.0f, .intake_level = .32f, .induction = .5f,
        .rasp = .8f, .rasp_hz = 1500, .shaper = 1.0f,
        .sub = .12f, .sub_order = 0,
        .knee = 60, .crackle = 1.0f, .gain = .9f,
    },
    {
        .name = "v10", .n = 10, .turbo = 0, .idle = 900, .redline = 8250,
        .order = {1, 6, 5, 10, 2, 7, 3, 8, 4, 9},
        .angle = {0, 90, 144, 234, 288, 378, 432, 522, 576, 666},
        .banks = 2, .split = 5, .bank_diff = .04f,
        .pulse_w = .72f, .cov = .02f, .jitter = .55f, .trim = .012f,
        .inertia = .30f, .torque = 520, .c_hot = 453,
        .bank_len = {.82f, .87f}, .collector = .66f, .refl = .46f, .damping = .27f,
        .muffler = {.19f, .13f, .09f},
        .helmholtz = 148, .intake_q = 7.4f, .intake_level = .52f, .induction = .85f,
        .rasp = .75f, .rasp_hz = 2300, .shaper = 1.1f,
        .sub = .8f, .sub_order = 0,
        .knee = 64, .crackle = .8f, .gain = 1.0f,
    },
    {
        .name = "v12", .n = 12, .turbo = 0, .idle = 700, .redline = 7500,
        .order = {1, 7, 5, 11, 3, 9, 6, 12, 2, 8, 4, 10},
        .angle = {0, 60, 120, 180, 240, 300, 360, 420, 480, 540, 600, 660},
        .banks = 2, .split = 6,
        .pulse_w = .9f, .cov = .028f, .jitter = .55f, .trim = .015f,
        .inertia = .36f, .torque = 690, .c_hot = 449,
        .bank_len = {.90f, .98f}, .collector = .70f, .refl = .47f, .damping = .28f,
        .muffler = {.20f, .14f, .09f},
        .helmholtz = 142, .intake_q = 7.0f, .intake_level = .46f, .induction = .6f,
        .rasp = .72f, .rasp_hz = 1600, .shaper = .8f,
        .sub = 1.0f, .sub_order = 0,
        .knee = 44, .crackle = .3f, .gain = 1.0f,
    },
};

int preset_find(const char *s) {
    int i;
    for (i = 0; i < NP; i++)
        if (!strcmp(s, presets[i].name)) return i;
    return -1;
}
