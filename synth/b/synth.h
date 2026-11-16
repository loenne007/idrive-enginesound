#ifndef ES_SYNTH_H
#define ES_SYNTH_H
#include "engine.h"
#include <stdint.h>
#include <stdio.h>
#define SR 48000
#define BLOCK 256
#define NP 4
#define FRAME_Q 6 /* params are rebuilt every 6 render quanta = 768 frames = 16 ms (~ an animation frame) */
int preset_find(const char *s);

/* control-input model: turns P/S lines into Engine_Sim's per-frame params object */
typedef struct {
    float rpm, prevRpm, dRpm, load, overrun, egt, dfcoEdge, popCharge, thrPeak, cutRpmN, popRefractory,
        limiterBark, boost, boostDump, overrunHeld, prevOverride;
    int dfco, prevDfco, bovArm, prevLimiter, started;
    /* shift (shift.js 'manual', sports car 95 ms) */
    int sphase, sdown;
    float st, sslip, sratio;
    float evCut, evLash, evEngage, evBov, evShiftDone, evPop;
    uint32_t rnd;
} Drive;

typedef struct {
    Voice voice[NP];
    Fx fx;
    Drive dr;
    int selected, turbo, mute, quit, gear, pending_shift_a, pending_shift_b, sleeping;
    float rpm, load, throttle, speed, volume;
    double time, last_p, audio_t; /* time: watchdog clock (main.c may set it to the wall clock) */
    uint64_t quanta;
    SP vol, mutegain, stale, quitg;
    float ol[QN], or_[QN];
    int opos;
    FILE *dump;
} Synth;
void synth_init(Synth *s, int preset, float volume);
int synth_control(Synth *s, const char *line);
void synth_render(Synth *s, int16_t *out, unsigned frames);
void demo_control(Synth *s, double t, double duration);
#endif
