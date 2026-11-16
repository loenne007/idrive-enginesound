/* Engine presets.
 *
 * Pulse law, pipe lengths, reflection/damping, muffler chambers, airbox and level trims start from the matching
 * engines of YigitSalihEmecen/Engine_Sim src/profiles.js (MIT, Copyright (c) 2026 Yigit Salih Emecen): i6, v8cross,
 * v10, v12. The firing geometry is BMW's: B58 1-5-3-6-2-4; S63-style cross-plane V8 1-5-4-8-6-3-7-2 with banks
 * 1-4 / 5-8 (bank A fires at 0/180/450/630 degrees, bank B at 90/270/360/540: the uneven per-bank gaps are the
 * burble); S85 V10 with its alternating 90/54-degree intervals; V12 at 60 degrees. Idle and redline are the app's
 * numbers and must not change.
 */
#include "synth.h"
#include <string.h>

const Preset presets[NP] = {
    {
        .name = "i6", .n = 6, .turbo = 1, .idle = 700, .redline = 7000,
        .order = {1, 5, 3, 6, 2, 4},
        .angle = {0, 120, 240, 360, 480, 600},
        .banks = 1,
        .attack = 30, .decay = 5.0f, .hardness = .58f, .jitter = .7f,
        .inertia = .27f, .torque = 420, .gas = 1.28f,
        .bank_len = 1.55f, .bank_len_b = 1.55f, .collector = 1.20f, .reflection = .54f, .damping = .38f,
        .muffler = {.33f, .21f, .14f},
        .helmholtz = 95, .intake_q = 6.4f, .intake_level = .33f,
        .voice = .98f, .crackle = .25f, .gain = .95f,
    },
    {
        .name = "v8", .n = 8, .turbo = 1, .idle = 650, .redline = 7200,
        .order = {1, 5, 4, 8, 6, 3, 7, 2},
        .angle = {0, 90, 180, 270, 360, 450, 540, 630},
        .banks = 2,
        .attack = 26, .decay = 4.4f, .hardness = .55f, .jitter = 2.6f,
        .inertia = .38f, .torque = 540, .gas = 1.26f,
        .bank_len = 1.75f, .bank_len_b = 1.75f, .collector = 1.35f, .reflection = .63f, .damping = .34f,
        .muffler = {.40f, .26f, .17f},
        .helmholtz = 78, .intake_q = 5.0f, .intake_level = .30f,
        .voice = 1.06f, .crackle = .7f, .gain = 1.35f,
    },
    {
        .name = "v10", .n = 10, .turbo = 0, .idle = 900, .redline = 8250,
        .order = {1, 6, 5, 10, 2, 7, 3, 8, 4, 9},
        .angle = {0, 90, 144, 234, 288, 378, 432, 522, 576, 666},
        .banks = 2,
        .attack = 48, .decay = 7.8f, .hardness = .80f, .jitter = .8f,
        .inertia = .30f, .torque = 520, .gas = 1.32f,
        .bank_len = .82f, .bank_len_b = .82f, .collector = .66f, .reflection = .46f, .damping = .27f,
        .muffler = {.19f, .13f, .09f},
        .helmholtz = 148, .intake_q = 7.4f, .intake_level = .45f,
        .voice = 1.02f, .crackle = .5f, .gain = 1,
    },
    {
        .name = "v12", .n = 12, .turbo = 0, .idle = 700, .redline = 7500,
        .order = {1, 7, 5, 11, 3, 9, 6, 12, 2, 8, 4, 10},
        .angle = {0, 60, 120, 180, 240, 300, 360, 420, 480, 540, 600, 660},
        .banks = 2,
        .attack = 46, .decay = 7.4f, .hardness = .78f, .jitter = .5f,
        .inertia = .36f, .torque = 690, .gas = 1.31f,
        .bank_len = .90f, .bank_len_b = .90f, .collector = .70f, .reflection = .47f, .damping = .28f,
        .muffler = {.20f, .14f, .09f},
        .helmholtz = 142, .intake_q = 7.0f, .intake_level = .44f,
        .voice = 1.04f, .crackle = .15f, .gain = 1.15f,
    },
};

int preset_find(const char *s) {
    int i;
    for (i = 0; i < NP; i++)
        if (!strcmp(s, presets[i].name)) return i;
    return -1;
}
