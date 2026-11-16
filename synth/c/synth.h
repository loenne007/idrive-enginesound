#ifndef ES_SYNTH_H
#define ES_SYNTH_H
#include <stdint.h>

#define SR 48000
#define BLOCK 256
#define NP 4          /* presets */
#define MAXCYL 12
#define MAXEV (2 * MAXCYL) /* scheduled events per 720-degree cycle: an exhaust and an intake event per cylinder */
#define MAXP 48       /* live pulse pool per voice */
#define DLEN 1024     /* delay-line size (power of two): 21 ms */
#define NCAL 24      /* loudness calibration bins, idle..redline */
#define CTRL 32       /* control-rate period, samples */

/* ---------------------------------------------------------------------------------------------------------------
 * Preset: the machine. Idle and redline are the app's numbers and must not change.
 * ------------------------------------------------------------------------------------------------------------- */
typedef struct {
    const char *name;
    int n, turbo;                /* cylinders; turbo on by default */
    float idle, redline;         /* rpm */
    int order[MAXCYL];           /* firing order (cylinder numbers) */
    float angle[MAXCYL];         /* firing angle of order[i], degrees of the 720-degree cycle */
    int banks, split;            /* exhaust paths 1 or 2; cylinders 1..split feed path 0, the rest path 1 */
    float bank_diff;             /* path 1's pulses weaker and wider by this fraction (manifold asymmetry) */
    float pulse_w;               /* pulse width scale (1 = nominal; < 1 = harder, shorter pulses) */
    float cov, jitter, trim;     /* cycle-to-cycle amplitude COV (at load), timing jitter (deg rms), static trims */
    float inertia, torque;       /* kg m^2, N m: crank-speed ripple per firing */
    float c_hot;                 /* speed of sound in the exhaust, m/s */
    float bank_len[2], collector, refl, damping; /* pipes (m), open-end reflection, wall loss */
    float muffler[3];            /* chamber lengths, m */
    float helmholtz, intake_q, intake_level, induction; /* airbox; induction roar */
    float rasp, rasp_hz;         /* turbulent noise riding on the pulses: amount, band centre at full load */
    float shaper;                /* load rasp (shock steepening) drive */
    float sub, sub_order;        /* sub layer level, order (per crank revolution) */
    float knee;                  /* radiation knee, engine orders */
    float crackle;               /* overrun pops propensity */
    float gain;                  /* output calibration */
} Preset;
extern const Preset presets[NP];
int preset_find(const char *s);

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } Biquad;
typedef struct { float buf[DLEN]; unsigned w; } Delay;

/* feedback waveguide: a pipe with a negative open-end reflection and a wall-loss lowpass in the loop */
typedef struct {
    Delay d;
    Biquad lp;
    float delay, lpbase, refl;
} Pipe;

/* one scheduled event of the 720-degree cycle */
typedef struct {
    float angle;          /* cycles 0..1 */
    int out, kind;        /* output (bank, or banks = intake); 0 exhaust, 1 intake */
    float amp_trim, w_trim; /* fixed per-cylinder trims */
} Event;

/* a live pressure pulse */
typedef struct {
    float age;            /* crank degrees since the valve opened */
    float A, rate;        /* strength, width (age speed) */
    int out, kind;
} Pulse;

/* the sound settings (X): smoothed multipliers */
enum { X_OFFGAS, X_TURBO, X_BOV, X_POPS, X_BASS, NX };

/* per-sample controls handed to a voice */
typedef struct {
    float rpm, load, throttle; /* smoothed telemetry; load includes the shift envelope (timbre) */
    float lvl_load;            /* the load the level map follows (without the shift's torque dip) */
    float level;               /* shift level trim (linear) */
    int turbo;                 /* turbo layer on */
    int shift_guard;           /* a shift happened recently: nothing may be triggered by it */
    const float *x;            /* smoothed X values */
} Ctl;

typedef struct {
    /* source */
    double crank;         /* absolute crank position in 720-degree cycles, never reset */
    double next;          /* absolute position of the next event */
    double cycle;
    int nev, evi, nout;
    Event ev[MAXEV];
    Pulse pool[MAXP];
    int np;
    float v;              /* crank-speed ripple */
    uint32_t seed, pseed;
    float gains[3][5];    /* gradient calibration per output over load */
    float srms[3][5];     /* pressure RMS per output over load (noise riding calibration) */
    float prev[3], lp[3];
    /* exhaust */
    Pipe bank[2], coll;
    Delay transit[2];
    float transit_d[2];
    Delay muf[3];
    float muf_d[3], muf_g[3], muf_trim, packing_base;
    Biquad packing, nl_dc, turbine;
    Biquad nz_bp[2];
    float nz_lo[2];
    /* intake */
    Biquad in_helm, in_direct, in_nbp;
    float in_pink;
    /* sub layer */
    Biquad sub_lp;
    /* turbo, blow-off */
    float spool, whine_ph, tb_lvl, tb_hz, bov_env, bov_t, bov_peak, thr_slow, bov_armed;
    Biquad tb_band, tb_air, tb_lp, bov_bp;
    /* pops */
    float load_avg, pop_t, pop_rate, pop_amp, pop_env, pop_dec, pop_lp, pop_lp2;
    int pop_armed, pop_bank;
    /* tip-in */
    float thr_lo, bark, prev_thr;
    /* control-rate values */
    float x, h, rasp_g, rasp_fc, nz_norm, nl_drive, nl_mk, lvl, lvl_full, f0, lpk, ripple, sub_g, sub_order;
    float in_hg, in_dg, in_ng, in_norm, rpm_n, over, tb_bnorm, tb_anorm, bov_norm, bov_tgt;
    float corr[2][NCAL];  /* loudness calibration: no load, full load */
    float corr_s;         /* the applied correction, smoothed */
    float weight;         /* crossfade weight */
    unsigned tick;
} Voice;

typedef struct {
    Voice voice[NP];
    int selected, turbo, mute, quit, gear;
    float rpm, rpm1, load, load1, throttle, trpm, tload, tthrottle, tspeed, volume, tvolume, gate;
    float shift_t;        /* time since an upshift (< 0: none) */
    float down_t;         /* time since a downshift (< 0: none) */
    float guard_t;        /* time since any shift */
    float xt[NX], xs[NX]; /* X targets, smoothed */
    float bass_db;        /* bass shelf currently designed for */
    Biquad hp[2], bass, boom, body, mid, cab_lp, shelf;
    double time, last_p;
    unsigned tick;
} Synth;

void synth_init(Synth *s, int preset, float volume);
int synth_control(Synth *s, const char *line);
void synth_render(Synth *s, int16_t *out, unsigned frames);
void demo_control(Synth *s, double t, double duration);
/* engine.c */
void voice_init(Voice *v, const Preset *p, int index);
float voice_step(Voice *v, const Preset *p, const Ctl *c);
/* debug/measurement: ES_SOLO=engine|turbo|bov|pops renders one layer only (0 = everything) */
extern int es_solo;
#endif
