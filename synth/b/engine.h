/* engine.h - one Engine_Sim engine voice (everything engine-sim.js builds in _buildVoices) and the shared
 * output stage (_buildGraph). Ported from Engine_Sim by Yigit Salih Emecen (MIT). */
#ifndef ES_ENGINE_H
#define ES_ENGINE_H
#include "wa.h"

/* profiles.js entry (+ our idle/redline, see presets.c) */
typedef struct {
    const char *name, *es_id;
    int n, turbo; /* cylinders, turbo default */
    float idle, redline;
    int nbanks, banks[2][6], bank_n[2];
    int order[12];
    float peakTorque, inertia, gasTemp;
    float attack, decay, hardness, jitter;
    float exBank, exBankB, exCollector, reflection, damping, muffler[3];
    float helmholtz, intakeQ, intakeLevel, voice;
    int gears;
    float teeth[8], ratios[8];
} Preset;

/* the per-frame params object (CONTRACT.md) */
enum { PH_NONE, PH_CUT, PH_OPEN, PH_SYNC, PH_ENGAGE, PH_LASH, PH_SHUFFLE };
typedef struct {
    double now;
    float dt, rpm, f0, rpmNorm, dRpm, throttle, load, overrun, torqueSign;
    int gear;
    float gearRatio, speed, wheelRpm, clutchSlip, clutchEngaged, boost;
    int shifting, phase, limiter;
    float evCut, evLash, evEngage, evBov, evShiftDone, evPop, popIntensity;
} Params;

/* ---- combustion-worklet.js ---- */
#define CMAXP 72
typedef struct {
    int nEv, nOut, nFire;
    int eout[24];
    double eangle[24], cycleOf[24], nextAt[24];
    float eamp[24], edecay[24];
    float attack, decay, hardness, ccv, jitterCycles, gains[3][5];
    double abs, v;
    uint32_t seed;
    int np;
    float pA[CMAXP], pa[CMAXP], pb[CMAXP], pts[CMAXP];
    double pt[CMAXP];
    int pout[CMAXP];
    double prevP[3], lp[3];
    SP f0, load, amp, ripple, low;
} Comb;

/* ---- resonators.js ---- */
typedef struct {
    int mode; /* 1 delay loop, 0 bandpass fallback */
    float length, gasTemp, reflection, damping, thermalSpan, thermalBias, nominalDelay, frequency, lpBase, loopQ;
    float maxDelayTime, tunedDelay, fMax;
    DL dl;
    float buf[2048], lprev[QN]; /* loop-filter output of the previous quantum (Chromium cycle lag) */
    SP delayT, fb;
    BQN loopLP;
    float dry, wet;
    int nm, morder[6];
    BQN bp[6];
    float bpg[6];
} WG;
typedef struct {
    int ns;
    float T[3], maxT[3], taper[3], branchGain, packingBase, thermalSpan, thermalBias, trim;
    DL dl[3];
    float buf[3][512];
    SP delayT[3], branch[3];
    BQN packing;
} Muffler;
typedef struct {
    float maxDrive, idleDrive;
    SP drive, makeup;
    WS shaper;
    BQN dcBlock;
} Nonlin;
typedef struct {
    int nb;
    WG bank[2], collector;
    DL tr[2];
    float trbuf[2][512];
    float trT[2], trMax[2];
    SP trDelay[2];
    float collectorGain, thermalSpan, thermalBias;
    Muffler muf;
    Nonlin nl;
} Exhaust;
typedef struct {
    float f0, q0, level;
    BQN helm, directLP, noiseHP, noiseBP;
    SP helmGain, directGain, noiseGain, noiseToHelm, out;
    NSRC noise;
} Intake;

/* ---- character.js ---- */
typedef struct {
    float pulseOrder, gain;
    NSRC src;
    BQN hp, body, peak;
    SP level, pfreq, pdepth;
    OSC pulse;
} ExNoise;
typedef struct {
    float order;
    OSC osc;
    SP freq, level;
    WS shaper;
    BQN tone;
} SubL;
typedef struct {
    OSC lfo[3];
    NSRC rough;
    BQN roughLP;
    SP trem;
} Character;

/* ---- layers.js ---- */
typedef struct {
    NSRC white, pink;
    OSC mesh1, mesh2, runout, tors;
    SP f1, f2, frun, ftors, meshH2, meshGain, ratGain;
    BQN meshBP, ratBP1, ratBP2;
    float teeth[8], finalDrive;
    int nteeth;
    float cyl, idleRpm;
} Trans;
typedef struct {
    int on, releasing;
    double releaseAt;
    float spool, boost, vent, surge, climb, liftRate, liftPitch, liftTau, prevThrottle, closedT;
    int prevShifting;
    uint32_t rnd;
    float inertia, bovLevel, surgeTrim, flutterHz, boostRpm, whineRef;
    int valve;
    NSRC white, grainN, airN;
    OSC whine, wander1, wander2, shaft, wg, hi, buzz, surgeOsc, fw;
    SP whineF, wd1, wd2, whineDrive, shaftF, shaftDepth, grainDepth, whineGain, hissGain, hissGrain, whineNoiseGain,
        airflowGain, wgF, wgLevel, wgGrain, hiF, hiGain, hiGrain, hiWander, buzzF, buzzGain, surgeF, surgeJitter,
        whooshGain, whooshGrain, surgeBodyLvl, surgeEdgeLvl, surgeChirp, fwF, hiSurgeLvl, hiChirp, hiChirpN, out,
        surgeSend;
    WS whineShaper, wgShaper, surgeShaper;
    BQN airLP, airShelf, whineBP, grainBP, hissBP, whineNoiseBP, airflowHP, airflowLP, wgBP, buzzBP, bovHissHP,
        surgeLP, whooshBP, surgeBody, fwBP, surgeEdge;
    BQ bovBP;
    AP bovF, bovQ, bovGain, bovHiss;
    float bovcf, bovcq;
} Turbo;
#define TV 16
typedef struct {
    NSRC src;
    BQ a, b;
    AP fa, qa, fb, qb, mixB, gain;
    float cfa, cqa, cfb, cqb;
    double busy;
    int comb;
} TVoice;
typedef struct {
    TVoice v[TV];
    int pool[2][TV], npool[2], cursor[2];
    uint32_t rnd;
    double nextPop, syncNext;
    int prevPhase, syncClicks;
    float sizeF, sizeA, cyl, idleRpm;
    int burstCount;
    float makeup;
} TBank;

typedef struct {
    const Preset *pr;
    int active;
    float weight, wtarget;
    float bark, prevLoad, turb;
    Comb comb;
    Exhaust ex;
    Intake in;
    ExNoise exn;
    SubL sub;
    Character ch;
    Trans tr;
    Turbo tu;
    TBank tb;
    SP trem, turbineShelfG, intakeTrim;
    BQN turbineShelf;
    float popSend;
} Voice;

/* bus outputs of one voice for one quantum */
typedef struct {
    float exhaust[QN], intake[QN], transmission[QN], turbo[QN], transients[QN], sub[QN];
} Busses;

void engine_global_init(void);
void voice_init(Voice *v, const Preset *p, int turbo);
void voice_set_turbo(Voice *v, int on, double now);
void voice_update(Voice *v, const Params *p, int immediate);
void voice_render(Voice *v, double t, Busses *out); /* adds weight-scaled contributions */
float voice_rpmnorm(const Preset *p, float rpm);

/* shared output stage */
typedef struct {
    BQN infra[2], blp1, blp2, bhp, blp3, rumble, body, presence, airCut, cabinLP, air, revHP, revDamp, tilt;
    WS bassWS, safety[2];
    CONV conv;
    DL sdl;
    float sbuf[4096];
    float sdelay, sg, revDry, revWet;
    BQ xo[2][8];
    COMP band[3], limiter;
    float bandMakeup[3], limiterTrim, makeup;
    SP master;
} Fx;
void fx_init(Fx *f);
/* mono mix bus in (already summed and scaled) -> stereo out (pre final scaling) */
void fx_run(Fx *f, const float *mix, float *l, float *r);

extern const Preset presets[];
#endif
