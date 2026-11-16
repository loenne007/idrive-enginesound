#ifndef ES_SYNTH_H
#define ES_SYNTH_H
#include <stdint.h>

#define SR 48000
#define BLOCK 256
#define NP 4         /* presets */
#define MAXCYL 12
#define MAXEV (2 * MAXCYL) /* combustion events per cycle: one exhaust + one intake event per cylinder */
#define DLEN 1024    /* delay-line size (power of two): 21 ms, longer than any pipe round trip here */
#define CTRL 32      /* control-rate period, samples (filter coefficients, envelopes) */

/* ---------------------------------------------------------------------------------------------------------------
 * Preset: the machine. Field meanings follow YigitSalihEmecen/Engine_Sim's profiles.js (MIT), whose numbers most
 * presets start from; the firing geometry is BMW's.
 * ------------------------------------------------------------------------------------------------------------- */
typedef struct {
    const char *name;
    int n, turbo;               /* cylinders; turbo on by default */
    float idle, redline;        /* rpm (the app depends on these numbers) */
    int order[MAXCYL];          /* firing order, cylinder numbers */
    float angle[MAXCYL];        /* firing angle of order[i], degrees of the 720-degree cycle */
    int banks;                  /* exhaust banks: 1, or 2 (cylinders 1..n/2 and n/2+1..n) */
    float attack, decay;        /* pressure pulse law (1-e^-a t) e^-b t, t in cycles */
    float hardness;             /* how much load sharpens the pulse */
    float jitter;               /* static per-cylinder spread, percent (drives trims, variation, timing jitter) */
    float inertia, torque;      /* kg m^2, N m: crank-speed ripple per firing */
    float gas;                  /* gas temperature factor on the speed of sound */
    float bank_len, bank_len_b, collector, reflection, damping; /* exhaust pipes, m; open-end reflection; wall loss */
    float muffler[3];           /* muffler chamber lengths, m */
    float helmholtz, intake_q, intake_level; /* airbox */
    float voice;                /* per-engine level trim */
    float crackle;              /* 0..1 overrun crackle after a lift-off */
    float gain;                 /* output calibration */
} Preset;
extern const Preset presets[NP];
int preset_find(const char *s);

typedef struct { float b0, b1, b2, a1, a2, z1, z2; } Biquad;
typedef struct { float buf[DLEN]; unsigned w; } Delay;

/* Feedback waveguide (one pipe): quarter-wave comb, negative open-end reflection, wall-loss lowpass in the loop. */
typedef struct {
    Delay d;
    Biquad lp;
    float delay, lpbase, refl, y; /* round-trip delay (samples, phase-tuned), loss corner, reflection, last return */
} Pipe;

/* A live combustion pressure pulse (one per event slot) */
typedef struct {
    float A, a, b;      /* strength, attack, decay (per cycle) */
    float t, ts;        /* age in cycles, in samples */
    float ea, eb;       /* e^-a t, e^-b t (advanced multiplicatively) */
    float ma, mb;       /* per-sample multipliers, refreshed at control rate */
    int live;
} Pulse;

typedef struct {
    int out;            /* output: bank index, or banks = intake */
    float angle;        /* cycles 0..1 */
    float amp_trim, decay_trim;
    double next, cycle; /* absolute crank position of the next event; its integer cycle index */
    Pulse p;
} Event;

typedef struct {
    /* source (combustion events) */
    double crank;       /* absolute crank position, 720-degree cycles; never reset */
    float v;            /* crank-speed deviation (ripple) */
    uint32_t seed;
    int nev, nfire, nout;
    Event ev[MAXEV];
    float gains[3][5];  /* calibration per output over load 0..1 */
    float prev_p[3], lp[3];
    /* exhaust */
    Pipe bank[2], coll;
    Delay transit[2];
    float transit_d[2];
    Delay muf[3];
    float muf_d[3], muf_g[3], muf_trim, packing_base;
    Biquad packing, nl_dc;
    float nl_drive;
    /* exhaust flow noise (ExhaustNoise) */
    Biquad xn_hp, xn_peak, xn_body;
    float xn_saw, xn_lvl, xn_depth;
    /* intake */
    Biquad in_helm, in_direct, in_nhp, in_nbp;
    float in_pink[3], in_hg, in_dg, in_ng, in_nh;
    /* sub layer */
    float sub_ph, sub_order, sub_lvl;
    Biquad sub_lp;
    /* turbo */
    float spool, climb, whine_ph, shaft_ph, surge, surge_ph, prev_thr, air_lp, hiss_g;
    Biquad tb_noise, tb_hiss, tb_surge, tb_lp, tb_shelf, turbine;
    float tb_whine_hz, tb_aero, whoosh_g;
    Biquad tb_whoosh;
    /* overrun crackle */
    float resid, crack_t, crack_env, crack_dec, crack_lp, load_avg; /* overrun crackle */
    int crack_armed;
    /* control-rate state */
    float load, load_lo, x_load, bark, overrun, fuel, amp, rpm_n, f0, scale, nl_mk, level, lpk;
    float weight;       /* crossfade weight of this voice */
    unsigned tick;
} Voice;

typedef struct {
    Voice voice[NP];
    int selected, turbo, mute, quit, gear;
    float rpm, load, throttle, speed, trpm, tload, tthrottle, tspeed, volume, tvolume, gate;
    float rpm_v;     /* rate-limited rpm (before the corner-rounding one-pole) */
    float shift_t;   /* time since an upshift started, s (< 0: none): smooth torque-reduction bump */
    float blip, blip_env; /* downshift: small rev-match blip (target, smoothed envelope) */
    /* master chain (Engine_Sim's tone stage, cabin, room, 3-band dynamics) */
    Biquad infra[2], rumble, bump, presence, aircut, boom, panel, cab_lp, shelf;
    Biquad bass_lp[2], bass_hp, bass_lp3;
    Biquad rv_hp, rv_lp;
    float rv_buf[4][4096];
    unsigned rv_w, rv_len[4];
    Biquad xo[3][4];  /* 3-band crossover: low (2x LP), mid (2x HP, 2x LP), high (2x HP) */
    float env[3], gr[3];
    double time, last_p;
    unsigned tick;
} Synth;

void synth_init(Synth *s, int preset, float volume);
int synth_control(Synth *s, const char *line);
void synth_render(Synth *s, int16_t *out, unsigned frames);
void demo_control(Synth *s, double t, double duration);
/* engine.c */
void voice_init(Voice *v, const Preset *p, int index);
float voice_step(Voice *v, const Preset *p, float rpm, float load, float throttle, int turbo);
#endif
