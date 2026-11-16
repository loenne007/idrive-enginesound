#ifndef ES_SYNTH_H
#define ES_SYNTH_H
#include <stdint.h>
#define SR 48000
#define BLOCK 256
#define NP 4
typedef struct {
    const char *name;
    int n, turbo;
    int order[12];
    double angle[12];
    float idle, redline, pipe[2], collector, hardness, gain, intake, rough, pop;
} Preset;
extern const Preset presets[NP];
int preset_find(const char *s);
typedef struct {
    float buf[1024], lp;
    unsigned pos, len;
    float fb;
} Pipe;
typedef struct {
    double next;
    float slow, fast, prev, a, b, amp;
} Cylinder;
typedef struct {
    double crank;
    uint32_t seed;
    Cylinder c[12];
    Pipe bank[2], collector;
    float radiation[2], muffler, cabin, hp, last, noise, mech, popenv, poplp, spool, bov, turbo_phase,
        weights;
    float a, b, cut;
} Voice;
typedef struct {
    Voice voice[NP];
    int selected, turbo, mute, quit, gear;
    float rpm, load, throttle, speed, trpm, tload, tthrottle, tspeed, volume, tvolume, gate, shift, blip;
    double time, last_p;
    unsigned tick;
} Synth;
void synth_init(Synth *s, int preset, float volume);
int synth_control(Synth *s, const char *line);
void synth_render(Synth *s, int16_t *out, unsigned frames);
void demo_control(Synth *s, double t, double duration);
#endif
