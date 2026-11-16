/* Engine profiles from Engine_Sim src/profiles.js (Copyright (c) Yigit Salih Emecen, MIT): i6 = 'i6',
 * v8 = 'v8cross', v10 = 'v10', v12 = 'v12'. Deviation: idle/redline are this app's (the controller maps the car's
 * rpm onto them, and --info reports them); Engine_Sim's own are i6 750/7000, v8cross 700/7000, v10 900/8700,
 * v12 850/8500. Gear teeth/ratios are gearbox.js designGearbox() for the 'sports' vehicle (final drive 3.73). */
#include "engine.h"
#include "synth.h"
#include <string.h>

const Preset presets[NP] = {
    {"i6", "i6", 6, 1, 700, 7000, 1, {{1, 2, 3, 4, 5, 6}, {0}}, {6, 0}, {1, 5, 3, 6, 2, 4},
     420, .27f, 1.28f, 30, 5.0f, .58f, .7f,
     1.55f, -1, 1.20f, .54f, .38f, {.33f, .21f, .14f},
     95, 6.4f, .33f, .98f,
     7, {29, 26, 24, 22, 21, 20, 19}, {2.036f, 1.582f, 1.238f, .978f, .781f, .635f, .536f}},
    {"v8", "v8cross", 8, 1, 650, 7200, 2, {{1, 2, 3, 4}, {5, 6, 7, 8}}, {4, 4}, {1, 8, 7, 3, 6, 5, 4, 2},
     540, .38f, 1.26f, 26, 4.4f, .55f, 2.6f,
     1.75f, -1, 1.35f, .63f, .34f, {.40f, .26f, .17f},
     78, 5.0f, .30f, 1.06f,
     6, {28, 25, 23, 21, 20, 20}, {1.842f, 1.397f, 1.07f, .83f, .657f, .542f}},
    {"v10", "v10", 10, 0, 900, 8250, 2, {{1, 2, 3, 4, 5}, {6, 7, 8, 9, 10}}, {5, 5}, {1, 6, 5, 10, 2, 7, 3, 8, 4, 9},
     560, .30f, 1.32f, 48, 7.8f, .80f, .8f,
     .82f, -1, .66f, .46f, .27f, {.19f, .13f, .09f},
     148, 7.4f, .45f, 1.02f,
     7, {31, 28, 25, 23, 22, 21, 20}, {2.336f, 1.815f, 1.421f, 1.122f, .897f, .729f, .615f}},
    {"v12", "v12", 12, 0, 700, 7500, 2, {{1, 2, 3, 4, 5, 6}, {7, 8, 9, 10, 11, 12}}, {6, 6},
     {1, 7, 5, 11, 3, 9, 6, 12, 2, 8, 4, 10},
     690, .36f, 1.31f, 46, 7.4f, .78f, .5f,
     .90f, -1, .70f, .47f, .28f, {.20f, .14f, .09f},
     142, 7.0f, .44f, 1.04f,
     8, {31, 28, 26, 24, 22, 21, 20, 20}, {2.367f, 1.875f, 1.494f, 1.198f, .969f, .792f, .657f, .564f}},
};

int preset_find(const char *s) {
    int i;
    for (i = 0; i < NP; i++)
        if (!strcmp(s, presets[i].name)) return i;
    return -1;
}
