#ifndef ES_SYNTH_H
#define ES_SYNTH_H
/* The Bass sound engine: active sound design for the car's own diesel. An rpm-locked order bank (the factory ASD / ESE
 * principle) that enriches the real B47 instead of imitating a foreign engine; see README.md. */
#include <stdint.h>

#define SR 48000
#define BLOCK 256
#define NP 4    /* presets */
#define NH 24   /* harmonics of the 720-degree cycle: k = 1..24 is engine order k/2 = 0.5..12 */
#define NK 4    /* rpm knots of the order tables */
#define MAXROW 12
#define CTRL 32 /* control-rate period, samples */

/* One row of an order table: engine order q, gain at full positive load (dB re the reference amplitude) at the
 * rpm knots, and the off-gas offset (dB added at zero load, i.e. idle / light cruise / coasting). */
typedef struct {
    float q, full[NK], off[NK];
} Row;

typedef struct {
    const char *name;
    int n, turbo;           /* cylinders; turbo on by default */
    float idle, redline;    /* real diesel rpm (the app does not map rpm for this engine) */
    int nrow;
    Row row[MAXROW];
    int bank[8];            /* bank of each firing slot (+1 / -1), firing order; 0 = inline */
    float bank_trim;        /* firing-to-firing level difference between the banks (V burble), fraction */
    float cyl_trim;         /* static per-cylinder spread, fraction */
    float am_rand;          /* random per-firing amplitude spread, fraction (rises on overrun / light load) */
    float wobble;           /* slow random modulation depth of the half orders (the burble lumps) */
    float ov_int, ov_half;  /* dB added on the overrun to integer / half orders */
    float off_trim;         /* dB added to every off-gas offset (sets off-gas against full load) */
    float tex_db;           /* filtered noise riding on the firing pulses, dB re reference at full load */
    float lp_lo, lp_hi;     /* order roll-off corner (3rd order) at zero / full load, Hz; cabin LP at 2.2x */
    float bass_db;          /* the preset's own bass shelf at bass = 1 */
    float pops;             /* overrun burble / crackle scale at pops = 1 */
    float turbo_db, bov_db; /* whistle / recirculation sigh level at X = 1, dB re reference */
    float gain;             /* output trim, dB */
} Preset;
extern const Preset presets[NP];
int preset_find(const char *s);

typedef struct {
    float b0, b1, b2, a1, a2, z1, z2;
} Biquad;

/* the firing-pulse layer for one firing count (6 or 8 events per cycle): per-firing amplitude irregularity and
 * the pulse envelope the noise texture rides on */
typedef struct {
    int n, slot;
    float am, am_t; /* smoothed per-firing amplitude and its target */
    float draw[8];  /* last random draw per slot */
} Firing;

typedef struct {
    /* telemetry targets and smoothed values */
    float trpm, tload, tthrottle, tspeed;
    float rpm1, rpm, load, throttle, speed;
    float lw, lw2, lov; /* the level's load (quick up, gliding down) and overrun amount */
    float pr_base, pr_slope, pr_h, pr_last_slope; /* rpm prediction along a consistent ramp */
    double pr_t0, last_p;
    int gear, selected, turbo, mute, quit;
    float volume, tvolume, gate;
    float weight[NP];
    /* X settings: target, smoothed */
    float x_t[5], x[5];
    /* order bank */
    double psi;          /* 720-degree cycle phase, radians, wrapped at 2 pi */
    float jit;           /* common phase perturbation (shaft radians) */
    float g[NH], gt[NH]; /* per-harmonic gain at the end of the current / next control tick */
    float gs[NH], dg[NH]; /* per-sample gain ramp */
    float rot_r, rot_i, cr, ci, dpsi; /* the cycle phasor and its per-sample rotation */
    int kmax;
    float wob, wob2, wob_depth;
    Firing f6, f8;
    float w8;            /* weight of the 8-firing layer (V8 preset) */
    float bank_trim, cyl_trim, am_rand;
    /* noise on the firing pulses */
    Biquad tex_bp, tex_lp;
    float tex_lvl, tex_lvl_t, tex_sharp;
    /* master */
    Biquad hp, hp2, shelf, cab[2];
    float shelf_db, cab_fc, out_gain, out_gain_t;
    /* turbo */
    float spool, spool_t;
    float wh_lvl, wh_g, wh_fc, wh_ph, boost_t;
    /* recirculation sigh */
    Biquad bov_bp, bov_lp;
    float bov_env, bov_att, bov_amp, thr_peak, bov_cool;
    /* overrun burble / crackle */
    Biquad pop_lp;
    float pop_env, pop_att, pop_ph, pop_amp, pop_win, pop_age, pop_gap, pop_res_f, load_avg, ov_t;
    int pop_armed;
    float hold;          /* hold-off after a gear change, s */
    uint32_t seed;
    double time;
    unsigned tick;
} Synth;

void synth_init(Synth *s, int preset, float volume);
int synth_control(Synth *s, const char *line);
void synth_render(Synth *s, int16_t *out, unsigned frames);
void demo_control(Synth *s, double t, double duration);
#endif
