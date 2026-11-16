/* synth.c - the Bass sound engine: active sound design for the car's own diesel.
 *
 * Like the factory systems (BMW ASD, Audi sound actuator / active exhaust, Bose EHE, Harman ESS) it adds
 * rpm-locked ENGINE ORDERS to what the cabin already hears from the real engine:
 *   - one 720-degree cycle phase psi, integrated from the telemetry rpm (never reset), and an order bank of the
 *     harmonics k = 1..24 of it (engine order k / 2, so half orders stay continuous: no wrap clicks)
 *   - per-order gain tables over rpm x load (presets.c), interpolated in dB, enveloped (12 ms up / 50 ms down)
 *   - what keeps it from sounding like an organ: per-firing amplitude irregularity (bank / cylinder trims plus a
 *     random draw per firing event), a slow random "lump" on the half orders (the V6 burble), a small common
 *     phase jitter, and band-passed noise riding on the firing pulses, brighter and sharper under load
 *   - cabin chain: 34 Hz 4th-order high-pass, bass shelf (X bass), a load-dependent cabin low-pass
 *   - subtle turbo whistle / hiss, a soft recirculation sigh on a lift after boost, and gentle overrun burble /
 *     crackle after a genuine lift-off (X turbo / bov / pops)
 * Gear changes (S) trigger nothing: the sound follows the telemetry rpm, which is what the car's ZF does.
 */
#include "dsp.h"
#include <stdio.h>
#include <string.h>

#define REF .05f      /* linear amplitude of a 0 dB order before the output gain */
#define OUT_GAIN 4.15f /* calibration: demo bass peaks about -10 dBFS, 3 s RMS under -15 dBFS */
#define FLOOR_LO 32.f /* orders fade out below FLOOR_HI .. FLOOR_LO Hz (no infrasound) */
#define FLOOR_HI 42.f
/* layer mask for measurements (cc -DES_LAYERS=...): 1 pulse noise, 2 turbo, 4 sigh, 8 pops, 16 phase jitter, 32 orders */
#ifndef ES_LAYERS
#define ES_LAYERS 63
#endif
#define LAYER(m) ((ES_LAYERS & (m)) ? 1.f : 0.f)

enum { X_OFFGAS, X_TURBO, X_BOV, X_POPS, X_BASS, NX };
static const char *const XNAME[NX] = {"offgas", "turbo", "bov", "pops", "bass"};
static const float KNOT[NK] = {800, 1600, 2800, 4300};
/* level against rpm, both load ends: the low band (under ~100 Hz up to ~2000 rpm) is held back for the speaker
 * protection's bass limit, the sound grows towards the top like a factory ASD */
static const float RPM_TRIM[NK] = {0, 0, 1, 2.5f};
static const float CYL[8] = {.7f, -.4f, .2f, -.9f, .5f, -.2f, .6f, -.5f};
static const int BANK8[8] = {1, -1, 1, -1, -1, 1, -1, 1};

/* tables expanded per harmonic (built once from presets[]; constant afterwards) */
static float tab_full[NP][NH + 1][NK], tab_off[NP][NH + 1][NK];
static unsigned char tab_has[NP][NH + 1];
static float ph_c[NH + 1], ph_s[NH + 1]; /* fixed phase of each harmonic */
static float k_att, k_rel, k_rpm, k_load, k_glide, k_x, k_w, k_spool_up, k_spool_dn, k_slow;

static void tables(void) {
    int p, r, k, j;
    memset(tab_has, 0, sizeof(tab_has));
    for (p = 0; p < NP; p++)
        for (r = 0; r < presets[p].nrow; r++) {
            const Row *w = &presets[p].row[r];
            k = (int)(w->q * 2 + .5f);
            if (k < 1 || k > NH) continue;
            tab_has[p][k] = 1;
            for (j = 0; j < NK; j++) {
                tab_full[p][k][j] = w->full[j] + RPM_TRIM[j];
                tab_off[p][k][j] = w->full[j] + RPM_TRIM[j] + w->off[j] + presets[p].off_trim;
            }
        }
    for (k = 1; k <= NH; k++) {
        /* order 3 (k = 6) in quadrature with the half orders 1.5 / 4.5 (report: phases 0, pi/2, 0); the others
         * spread so the integer orders do not all peak together (lower crest factor) */
        float ph = k == 6 ? PI_F / 2 : (k == 3 || k == 9) ? 0 : .9f * k;
        ph_c[k] = cosf(ph);
        ph_s[k] = sinf(ph);
    }
    k_att = tau_k(.012f, CTRL);
    k_rel = tau_k(.050f, CTRL);
    k_rpm = tau_k(.006f, CTRL);
    k_load = tau_k(.015f, CTRL);
    k_glide = tau_k(.06f, CTRL);
    k_x = tau_k(.05f, CTRL);
    k_w = tau_k(.08f, CTRL);
    k_spool_up = tau_k(.5f, CTRL);
    k_spool_dn = tau_k(.3f, CTRL);
    k_slow = 1 - expf(-2 * PI_F * 4.f * CTRL / SR); /* 4 Hz at control rate */
}

void synth_init(Synth *s, int p, float vol) {
    int i;
    tables();
    memset(s, 0, sizeof(*s));
    s->selected = p;
    s->turbo = presets[p].turbo;
    s->weight[p] = 1;
    s->trpm = s->rpm1 = s->rpm = s->pr_base = presets[p].idle;
    s->tvolume = s->volume = vol;
    s->last_p = -1e9;
    s->pr_t0 = -1e9;
    for (i = 0; i < NX; i++) s->x_t[i] = s->x[i] = 1;
    s->seed = 0x9e3779b9u;
    s->f6.n = 6;
    s->f8.n = 8;
    s->f6.am = s->f6.am_t = s->f8.am = s->f8.am_t = 1;
    s->f6.slot = s->f8.slot = -1;
    s->w8 = presets[p].n == 8;
    s->wob = 0;
    /* 4th-order Butterworth high-pass at 34 Hz: also the 20 Hz sidebands of the per-firing modulation at idle */
    bq_highpass(&s->hp, 34, .5412f);
    bq_highpass(&s->hp2, 34, 1.3066f);
    s->shelf_db = -99;
    s->cab_fc = -1;
    bq_bandpass(&s->tex_bp, 400, .8f);
    bq_lowpass(&s->tex_lp, 200, .7f);
    bq_bandpass(&s->bov_bp, 1100, 1.2f);
    bq_lowpass(&s->bov_lp, 1200, .7f);
    bq_lowpass(&s->pop_lp, 300, .7f);
    s->pop_res_f = 120;
    s->pop_gap = 1;
}

int synth_control(Synth *s, const char *line) {
    float r, l, t, v;
    int g, k, a, b, n = 0;
    char name[32], extra;
    if (sscanf(line, "P %f %f %f %d %f %n", &r, &l, &t, &g, &v, &n) == 5 && n && line[n] == 0 && isfinite(r) &&
        isfinite(l) && isfinite(t) && isfinite(v) && g >= -1 && g <= 10) {
        double dt = s->time - s->last_p;
        r = clampf(r, 0, 8000);
        /* rpm prediction along a ramp: only when the last two intervals agree (a consistent slope), so a step
         * is never extrapolated; at most one telemetry interval ahead, slope capped at 15000 rpm/s */
        s->pr_slope = 0;
        if (dt > .004 && dt < .2) {
            float sl = (float)((r - s->trpm) / dt), lsl = s->pr_last_slope;
            if (sl * lsl > 0 && fabsf(sl) > 300 && fabsf(sl - lsl) < .5f * fmaxf(fabsf(sl), fabsf(lsl)))
                s->pr_slope = clampf(fabsf(sl) < fabsf(lsl) ? sl : lsl, -15000, 15000);
            s->pr_last_slope = sl;
            s->pr_h = (float)fmin(dt, .05);
        } else
            s->pr_last_slope = 0;
        s->pr_base = r;
        s->pr_t0 = s->time;
        s->trpm = r;
        s->tload = clampf(l, -1, 1);
        s->tthrottle = clampf(t, 0, 1);
        s->tspeed = clampf(v, 0, 400);
        s->gear = g;
        s->last_p = s->time;
        return 1;
    }
    if (sscanf(line, "E %31s %c", name, &extra) == 1 && (k = preset_find(name)) >= 0) {
        s->selected = k;
        s->turbo = presets[k].turbo;
        return 1;
    }
    if (sscanf(line, "T %d %c", &k, &extra) == 1 && (k == 0 || k == 1)) {
        s->turbo = k;
        return 1;
    }
    if (sscanf(line, "M %d %c", &k, &extra) == 1 && (k == 0 || k == 1)) {
        s->mute = k;
        return 1;
    }
    if (sscanf(line, "V %f %c", &v, &extra) == 1 && isfinite(v) && v >= 0 && v <= 2) {
        s->tvolume = v;
        return 1;
    }
    if (sscanf(line, "X %31s %f %c", name, &v, &extra) == 2 && isfinite(v)) {
        for (k = 0; k < NX; k++)
            if (!strcmp(name, XNAME[k])) {
                s->x_t[k] = clampf(v, 0, 2);
                return 1;
            }
        return 0;
    }
    if (sscanf(line, "S %d %d %c", &a, &b, &extra) == 2 && a >= -1 && a <= 10 && b >= -1 && b <= 10) {
        /* the ZF shifts under load without a gap; the rpm comes from the telemetry. A shift only holds off the
         * lift-off detection (its torque dip is not a lift). */
        s->hold = .4f;
        return 1;
    }
    if (line[0] == 'Q' && sscanf(line + 1, " %c", &extra) != 1) {
        s->quit = 1;
        return 1;
    }
    return 0;
}

static float interp(const float *t, int i, float f) { return t[i] + (t[i + 1] - t[i]) * f; }

/* one firing event per slot: a new amplitude target from the bank / cylinder pattern and a random draw */
static int firing(Synth *s, Firing *f, float pos, const int *bank, float bank_trim) {
    int slot = (int)pos;
    if (slot >= f->n) slot = f->n - 1;
    if (slot == f->slot) return 0;
    f->slot = slot;
    f->am_t = 1 + bank[slot] * bank_trim + CYL[slot] * s->cyl_trim + s->am_rand * gauss(&s->seed);
    return 1;
}

static void control(Synth *s) {
    const float dtc = (float)CTRL / SR;
    float rpm, load, w, ov, odb, ofull, fc_ord, cruise, on, wsum = 0, fr, x;
    float tex_db = 0, lp_lo = 0, lp_hi = 0, bass_db = 0, pops = 0, turbo_db = 0, bov_db = 0, wobble = 0;
    int i, k, idx;
    /* smoothing of the settings and the crossfade weights */
    for (i = 0; i < NX; i++) {
        s->x[i] += (s->x_t[i] - s->x[i]) * k_x;
        if (fabsf(s->x[i] - s->x_t[i]) < 1e-3f) s->x[i] = s->x_t[i]; /* lands exactly: 0 is off */
    }
    for (i = 0; i < NP; i++) {
        s->weight[i] += ((i == s->selected) - s->weight[i]) * k_w;
        if (s->weight[i] < 1e-5f) s->weight[i] = 0;
        wsum += s->weight[i];
    }
    /* rpm: the telemetry (predicted along a consistent ramp), then two 6 ms one-poles: a rounded step, 90 % in
     * about 23 ms */
    {
        float target = s->pr_base + s->pr_slope * (float)fmin(fmax(s->time - s->pr_t0, 0), s->pr_h);
        s->rpm1 += (target - s->rpm1) * k_rpm;
        s->rpm += (s->rpm1 - s->rpm) * k_rpm;
    }
    s->load += (s->tload - s->load) * k_load;
    s->throttle += (s->tthrottle - s->throttle) * k_load;
    s->speed += (s->tspeed - s->speed) * tau_k(.1f, CTRL);
    rpm = s->rpm;
    load = s->load;
    /* the level's load: up quickly (tip-in ~50 ms with the envelopes), down as a glide at 2 / s (a full lift
     * takes ~0.5 s: it sounds like going into overrun, not like a fade-out); the overrun trim glides in alike */
    w = powf(clampf(load, 0, 1), .7f);
    if (w > s->lw)
        s->lw += (w - s->lw) * k_load;
    else
        s->lw = fmaxf(w, s->lw - 2 * dtc);
    s->lw2 += (s->lw - s->lw2) * (s->lw > s->lw2 ? k_rpm : k_glide);
    w = s->lw2;
    ov = sstep(-.02f, -.25f, load);
    s->lov = ov > s->lov ? fminf(ov, s->lov + 2 * dtc) : s->lov + (ov - s->lov) * k_load;
    ov = s->lov;
    /* X offgas: 0..1 scales the off-gas end (dB), 1..2 moves it towards the full-load table (2 = as loud) */
    odb = 20 * log10f(fmaxf(fminf(s->x[X_OFFGAS], 1), .001f));
    ofull = fmaxf(s->x[X_OFFGAS] - 1, 0);
    /* -2 dB cruising in 7th / 8th at low load; idle 1 dB quieter than the rest of the off-gas range */
    cruise = (s->gear >= 7 ? -2.f * (1 - w) : 0) - 1.f * (1 - sstep(900, 1400, rpm)) * (1 - w);
    on = sstep(300, 600, rpm);
    /* rpm knot position */
    idx = 0;
    while (idx < NK - 2 && rpm > KNOT[idx + 1]) idx++;
    fr = clampf((rpm - KNOT[idx]) / (KNOT[idx + 1] - KNOT[idx]), 0, 1);
    for (k = 1; k <= NH; k++) s->gt[k - 1] = 0;
    s->bank_trim = s->cyl_trim = s->am_rand = 0;
    s->w8 = 0;
    for (i = 0; i < NP; i++) {
        const Preset *p = &presets[i];
        float wi = s->weight[i] / (wsum > 0 ? wsum : 1);
        if (wi <= 0) continue;
        for (k = 1; k <= NH; k++) {
            float full, off, db;
            if (!tab_has[i][k]) continue;
            full = interp(tab_full[i][k], idx, fr);
            off = interp(tab_off[i][k], idx, fr) + odb;
            off += (full - off) * ofull;
            db = off + (full - off) * w + ov * ((k & 1) ? p->ov_half : p->ov_int) + cruise + p->gain;
            s->gt[k - 1] += wi * db2lin(db);
        }
        if (p->n == 8) s->w8 += wi;
        s->bank_trim += wi * p->bank_trim;
        s->cyl_trim += wi * p->cyl_trim;
        s->am_rand += wi * p->am_rand * (1 + .6f * (1 - w) + .6f * ov);
        tex_db += wi * p->tex_db;
        lp_lo += wi * p->lp_lo;
        lp_hi += wi * p->lp_hi;
        bass_db += wi * p->bass_db;
        pops += wi * p->pops;
        turbo_db += wi * p->turbo_db;
        bov_db += wi * p->bov_db;
        wobble += wi * p->wobble;
    }
    /* D is a bass enhancer: every order is weighted by a 3rd-order low-pass magnitude at the preset's corner
     * (about 140 Hz off the gas, 185-200 Hz under load), so the energy sits at 30-150 Hz with a little up to
     * ~250 Hz; no infrasound: an order fades out between 42 and 32 Hz; engine off (rpm < 300-600): silence */
    fc_ord = lp_lo + (lp_hi - lp_lo) * w;
    for (k = 1; k <= NH; k++) {
        float f = k * rpm * (1.f / 120), r = f / fc_ord, roll = 1 / sqrtf(1 + r * r * r * r * r * r),
              tgt = s->gt[k - 1] * REF * on * roll * sstep(FLOOR_LO, FLOOR_HI, f), g = s->g[k - 1];
        s->gt[k - 1] = g + (tgt - g) * (tgt > g ? k_att : k_rel); /* the value reached at the end of this tick */
    }
    /* slow random lump of the half orders: two 4 Hz one-poles on a Gaussian, normalised to about unit variance */
    s->wob += (gauss(&s->seed) * 10.6f - s->wob) * k_slow;
    s->wob2 += (s->wob - s->wob2) * k_slow;
    s->wob_depth = wobble * (.7f + .5f * (1 - w) + .5f * ov) * (1 - .5f * sstep(2500, 4300, rpm));
    /* common phase jitter, shaft radians (scaled by order through the harmonic number) */
    s->jit += (gauss(&s->seed) * .077f * LAYER(16) - s->jit) * .042f; /* ~10 Hz wide, ~0.011 rad */
    /* a trace of low-passed texture on the firing pulses, under real load only (none at idle, cruise, coast) */
    x = tex_db + 4 * clampf((rpm - 800) / 3500, 0, 1);
    s->tex_lvl_t = REF * on * db2lin(x) * sstep(.35f, .9f, load);
    s->tex_sharp = w;
    bq_bandpass(&s->tex_bp, 80 + 80 * w + .02f * rpm, .7f);
    /* cabin low-pass: muffled off the gas, opening under load; bass shelf: preset + X bass (about +-9 dB) */
    {
        float fc = clampf(2.2f * fc_ord, 250, 500), db = bass_db + 9 * (s->x[X_BASS] - 1);
        if (fabsf(fc - s->cab_fc) > .002f * fc) {
            bq_lowpass(&s->cab[0], fc, .62f);
            s->cab_fc = fc;
        }
        if (fabsf(db - s->shelf_db) > .02f) {
            bq_lowshelf(&s->shelf, 120, db);
            s->shelf_db = db;
        }
    }
    /* boost proxy: only real high load at real rpm (500 ms up, 300 ms down); time spent near full load */
    s->spool_t = sstep(.6f, .95f, load) * sstep(1600, 2600, rpm);
    s->spool += (s->spool_t - s->spool) * (s->spool_t > s->spool ? k_spool_up : k_spool_dn);
    s->boost_t = load > .8f && rpm > 1700 ? s->boost_t + dtc : fmaxf(0, s->boost_t - (load > .3f ? .5f : 2) * dtc);
    /* turbo (T 1 + X turbo; off in this engine's own default): a faint tonal whistle at high boost only, no noise */
    {
        float lvl = s->turbo ? s->x[X_TURBO] : 0;
        s->wh_lvl = REF * on * db2lin(turbo_db) * lvl * sstep(.55f, .95f, s->spool);
        s->wh_fc = 2300 + 1500 * s->spool * clampf(rpm / 4300, .4f, 1.1f);
    }
    /* recirculation sigh: only a quick pedal lift (peak of the last ~150 ms above 70 %, now under 10 %) after
     * at least 0.5 s near full load; ~200 ms, low-passed, 3 s cool-down */
    s->thr_peak = fmaxf(s->throttle, s->thr_peak * (1 - tau_k(.15f, CTRL)));
    s->bov_cool -= dtc;
    if (s->bov_cool <= 0 && s->boost_t > .5f && s->throttle < .1f && s->thr_peak > .7f && load < .1f) {
        s->bov_amp = REF * on * db2lin(bov_db) * s->spool;
        s->bov_env = 1;
        s->bov_att = 0;
        s->bov_cool = 3;
        s->boost_t = 0;
    }
    s->bov_env *= 1 - tau_k(.06f, CTRL);
    if (s->bov_env > 1e-4f) bq_bandpass(&s->bov_bp, 600 + 500 * s->bov_env, 1.2f);
    /* overrun burble / crackle: armed by sustained positive load, fired by a genuine lift (pedal closed,
     * torque negative for 60 ms, not inside a gear change), lasting under a second */
    s->hold -= dtc;
    s->load_avg += (fmaxf(load, 0) - s->load_avg) * tau_k(.3f, CTRL);
    s->ov_t = load < -.05f && s->throttle < .05f ? s->ov_t + dtc : 0;
    if (load > .25f) s->pop_armed = 1;
    s->pop_age += dtc;
    if (s->pop_armed && s->ov_t > .06f && s->load_avg > .2f && rpm > 1300 && s->speed > 10 && s->hold <= 0) {
        s->pop_armed = 0;
        s->pop_age = 0;
        s->pop_win = 1;
    }
    {
        float pe = s->x[X_POPS] * pops, dur = .6f + .35f * fminf(pe, 2);
        if (s->pop_win > 0 && (s->pop_age > dur || load > .05f || rpm < 1100)) s->pop_win = 0;
        s->pop_amp = REF * on * db2lin(-6 + 3 * fminf(pe, 2)) * (pe > 0) * (rpm > 1100);
        /* events per second, decaying through the window, starting ~80 ms after the lift */
        s->pop_win = s->pop_win > 0 ? (s->pop_age < .08f ? 1e-3f : expf(-(s->pop_age - .08f) / (.45f * dur))) : 0;
        s->pop_win *= 6.f * fminf(pe, 2.5f);
    }
}

void synth_render(Synth *s, int16_t *out, unsigned frames) {
    unsigned f;
    int k;
    for (f = 0; f < frames; f++) {
        float y, acc_e = 0, acc_o = 0, zr, zi, z1r, z1i, rr, ri, tex, am, u, env, t;
        float gate = (!s->mute && !s->quit && s->time - s->last_p <= 1.5) ? 1.f : 0.f;
        if (!(s->tick % CTRL)) {
            float jit0 = s->jit;
            control(s);
            /* per-sample gain ramps to this tick's values; phase increment incl. the jitter's change */
            s->kmax = 0;
            for (k = 0; k < NH; k++) {
                s->gs[k] = s->g[k];
                s->dg[k] = (s->gt[k] - s->g[k]) * (1.f / CTRL);
                s->g[k] = s->gt[k];
                if (s->gs[k] > 1e-7f || s->g[k] > 1e-7f) s->kmax = k + 1;
            }
            s->dpsi = (float)(TAU_D * s->rpm / (120.0 * SR)) + .5f * (s->jit - jit0) / CTRL;
            s->rot_r = cosf(s->dpsi);
            s->rot_i = sinf(s->dpsi);
            s->cr = cosf((float)s->psi);
            s->ci = sinf((float)s->psi);
        }
        /* the cycle phasor */
        z1r = s->cr;
        z1i = s->ci;
        zr = z1r;
        zi = z1i;
        for (k = 0; k < s->kmax; k++) {
            float gk = s->gs[k];
            s->gs[k] += s->dg[k];
            if (k) {
                float t2 = zr * z1r - zi * z1i;
                zi = zr * z1i + zi * z1r;
                zr = t2;
            }
            if (gk > 1e-7f) {
                float v = gk * (zi * ph_c[k + 1] + zr * ph_s[k + 1]);
                if (k & 1)
                    acc_e += v; /* k+1 even: integer order */
                else
                    acc_o += v;
            }
        }
        /* advance the phase */
        s->psi += s->dpsi;
        if (s->psi >= TAU_D) s->psi -= TAU_D;
        rr = s->cr * s->rot_r - s->ci * s->rot_i;
        ri = s->cr * s->rot_i + s->ci * s->rot_r;
        s->cr = rr;
        s->ci = ri;
        /* firing events: per-firing amplitude irregularity (6- and 8-event layers, mixed by the V8's weight) */
        {
            float pos6 = (float)(s->psi * (6 / TAU_D)), pos8 = (float)(s->psi * (8 / TAU_D)), e6, e8;
            int fired = 0;
            float k_am = clampf(s->rpm * (1.f / 120) * 6 / (.3f * SR), .002f, .2f);
            if (s->w8 < .999f) fired |= firing(s, &s->f6, pos6, presets[2].bank, s->bank_trim) && s->w8 < .5f;
            if (s->w8 > .001f) fired |= firing(s, &s->f8, pos8, BANK8, s->bank_trim) && s->w8 >= .5f;
            s->f6.am += (s->f6.am_t - s->f6.am) * k_am;
            s->f8.am += (s->f8.am_t - s->f8.am) * k_am * .75f;
            am = s->f6.am + (s->f8.am - s->f6.am) * s->w8;
            u = 1 - (pos6 - (int)pos6);
            e6 = u * u * u;
            u = 1 - (pos8 - (int)pos8);
            e8 = u * u * u;
            env = e6 + (e8 - e6) * s->w8;
            env = env * (1 - s->tex_sharp) + env * env * 2.2f * s->tex_sharp;
            /* overrun pops: an event on a firing slot, at a decaying rate, never closer than ~90 ms */
            s->pop_gap += 1.f / SR;
            if (fired && s->pop_win > 0 && s->pop_gap > .09f) {
                float fire_hz = s->rpm * (1.f / 120) * (s->w8 >= .5f ? 8 : 6);
                if (rnd(&s->seed) * .5f + .5f < s->pop_win / fire_hz) {
                    s->pop_gap = 0;
                    s->pop_env = .55f + .45f * (rnd(&s->seed) * .5f + .5f);
                    s->pop_att = 0;
                    s->pop_ph = 0;
                    s->pop_res_f = 85 + 70 * (rnd(&s->seed) * .5f + .5f);
                }
            }
        }
        /* the order bank: integer orders + lumped half orders, per-firing irregularity */
        y = (acc_e + acc_o * fmaxf(.2f, 1 + s->wob_depth * s->wob2)) * am * LAYER(32);
        /* noise riding on the firing pulses */
        s->tex_lvl += (s->tex_lvl_t - s->tex_lvl) * .002f;
        if (s->tex_lvl > 1e-7f) {
            tex = bq(&s->tex_lp, bq(&s->tex_bp, rnd(&s->seed))) * env * s->tex_lvl * 3.f * LAYER(1);
            y += tex;
        }
        /* cabin: high-pass, bass shelf, low-pass */
        y = bq(&s->cab[0], bq(&s->shelf, bq(&s->hp2, bq(&s->hp, y))));
        /* turbo whistle: a quiet sine, its pitch with the boost, a slow wander; faded in and out by wh_g */
        s->wh_g += (s->wh_lvl - s->wh_g) * .0005f;
        if (s->wh_g > 1e-7f) {
            s->wh_ph += s->wh_fc * (1 + .004f * s->wob2) * (1.f / SR);
            s->wh_ph -= (float)(int)s->wh_ph;
            y += LAYER(2) * s->wh_g * sinf(6.2831853f * s->wh_ph);
        }
        /* recirculation sigh: 15 ms rise, ~60 ms decay, a falling band under 1.1 kHz, low-passed */
        if (s->bov_env > 1e-4f) {
            float a;
            s->bov_att += (1 - s->bov_att) * .0014f;
            a = s->bov_env * s->bov_att * s->bov_amp * s->x[X_BOV];
            y += bq(&s->bov_lp, bq(&s->bov_bp, rnd(&s->seed))) * a * 3.f * LAYER(4);
        }
        /* overrun burble: a damped low sine thump (85-155 Hz, 3 ms rise, ~28 ms decay) with a little rumble */
        if (s->pop_env > 1e-4f) {
            float th;
            s->pop_att += (1 - s->pop_att) * .007f;
            s->pop_ph += s->pop_res_f * (1.f / SR);
            s->pop_ph -= (float)(int)s->pop_ph;
            th = sinf(6.2831853f * s->pop_ph) * (1 + .35f * rnd(&s->seed));
            y += bq(&s->pop_lp, th * s->pop_env * s->pop_att) * s->pop_amp * .35f * LAYER(8);
            s->pop_env *= .99925f; /* ~28 ms */
        } else
            bq(&s->pop_lp, 0);
        /* output: volume, gate (mute, stale telemetry, quit), soft ceiling -3 dBFS */
        s->volume += (s->tvolume - s->volume) * .0004f;
        s->gate += (gate - s->gate) * .0005f;
        t = y * OUT_GAIN * s->volume * s->gate;
        if (t > .5f)
            t = .5f + .2f * tanhf((t - .5f) * 5);
        else if (t < -.5f)
            t = -.5f + .2f * tanhf((t + .5f) * 5);
        if (!isfinite(t)) t = 0;
        out[2 * f] = out[2 * f + 1] = (int16_t)lrintf(t * 32767);
        s->time += 1. / SR;
        s->tick++;
    }
}

/* ---- demo: a 40 s drive of the 320d (B47, ZF 8HP) with real diesel rpm ----
 * key frames: time s, rpm, load (crank torque, -1..1), pedal, gear. Short segments (shifts, tip-ins) are eased,
 * long ones linear. */
static const float DEMO[][5] = {
    {0.0f, 800, .05f, 0, 1},       {2.4f, 800, .05f, 0, 1},       {2.65f, 1100, .85f, .95f, 1},
    {3.1f, 1900, .95f, 1, 1},      {4.6f, 4450, .95f, 1, 1},      {4.95f, 3000, .8f, 1, 2},
    {5.1f, 3050, .95f, 1, 2},      {7.4f, 4450, .95f, 1, 2},      {7.75f, 3150, .8f, 1, 3},
    {7.9f, 3200, .95f, 1, 3},      {10.6f, 4450, .95f, 1, 3},     {10.95f, 3250, .8f, 1, 4},
    {11.1f, 3280, .95f, 1, 4},     {12.4f, 3750, .95f, 1, 4},     {12.6f, 3720, -.55f, 0, 4},
    {13.6f, 3550, -.6f, 0, 4},     {13.9f, 2850, -.55f, 0, 5},    {15.0f, 2650, -.55f, 0, 5},
    {15.3f, 2150, -.5f, 0, 6},     {16.5f, 2000, -.5f, 0, 6},     {16.8f, 2050, .25f, .25f, 6},
    {19.0f, 2050, .25f, .25f, 6},  {19.3f, 2150, .65f, .6f, 6},   {21.5f, 2900, .7f, .6f, 6},
    {21.85f, 2250, .6f, .6f, 7},   {22.0f, 2260, .65f, .6f, 7},   {23.0f, 2450, .65f, .6f, 7},
    {23.3f, 2400, -.5f, 0, 7},     {24.5f, 2150, -.55f, 0, 7},    {24.8f, 2500, -.55f, 0, 6},
    {26.0f, 2150, -.55f, 0, 6},    {26.3f, 2550, -.5f, 0, 5},     {27.0f, 2350, -.5f, 0, 5},
    {27.3f, 2400, .9f, 1, 5},      {27.6f, 3300, .95f, 1, 4},     {29.5f, 4350, .95f, 1, 4},
    {29.85f, 3300, .8f, 1, 5},     {30.0f, 3350, .95f, 1, 5},     {30.6f, 3600, .95f, 1, 5},
    {30.8f, 3580, -.55f, 0, 5},    {32.0f, 2700, -.6f, 0, 5},     {32.3f, 3000, -.55f, 0, 4},
    {33.4f, 2100, -.6f, 0, 4},     {33.7f, 2600, -.5f, 0, 3},     {34.7f, 1700, -.5f, 0, 3},
    {35.0f, 2000, -.4f, 0, 2},     {35.9f, 1200, -.3f, 0, 2},     {36.2f, 1000, -.1f, 0, 1},
    {36.8f, 800, .05f, 0, 1},      {40.0f, 800, .05f, 0, 1},
};
/* km/h per 1000 rpm in each gear (8HP, 320d) */
static const float KMH[9] = {0, 8, 12.5f, 19, 25, 32, 40, 52, 62};

void demo_control(Synth *s, double t, double duration) {
    const int n = (int)(sizeof(DEMO) / sizeof(DEMO[0]));
    double u = t * 40 / duration;
    int i = 0, g;
    float r, l, th, f, sp;
    char line[160];
    while (i < n - 2 && u >= DEMO[i + 1][0]) i++;
    f = clampf((float)((u - DEMO[i][0]) / (DEMO[i + 1][0] - DEMO[i][0])), 0, 1);
    if (DEMO[i + 1][0] - DEMO[i][0] < .6f) f = smooth01(f);
    r = DEMO[i][1] + (DEMO[i + 1][1] - DEMO[i][1]) * f;
    l = DEMO[i][2] + (DEMO[i + 1][2] - DEMO[i][2]) * f;
    th = DEMO[i][3] + (DEMO[i + 1][3] - DEMO[i][3]) * f;
    /* the gear changes at the start of a shift segment */
    g = (int)DEMO[i + 1][4];
    if (f <= 0) g = (int)DEMO[i][4];
    sp = r / 1000 * KMH[g];
    if (u < 2.5) sp = 0;
    if (u > 36.2) sp = fmaxf(0, (float)(37 - u) * 8);
    if (g != s->gear) {
        snprintf(line, sizeof(line), "S %d %d", s->gear, g);
        synth_control(s, line);
    }
    snprintf(line, sizeof(line), "P %.2f %.3f %.3f %d %.1f", r, l, th, g, sp);
    synth_control(s, line);
    if (t > duration - .5) s->mute = 1;
}
