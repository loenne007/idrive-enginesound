/* fx.c - Engine_Sim's shared output stage (engine-sim.js _buildGraph, fx.js, resonators.js CabinFilter) with the
 * default sound of the demo samples: tone rumble/brightness/punch 1, EQ flat, reverb size 0.32 mix 0.14 damping
 * 4200 Hz, width 0.35, dynamics 1, exterior position, live inputs 0. Copyright (c) Yigit Salih Emecen, MIT.
 *
 * Exact simplifications (no audible difference by construction):
 *  - the 0 dB peaking/shelving stages (EQ, cabin boom/panel/shelf at the exterior position) are identities and
 *    are skipped, as is the Space node (environment 0: its loops have zero gain);
 *  - the ConvolverNode is fed a mono signal and the Stereoizer's ChannelMerger downmixes its stereo input to mono,
 *    so the stereo impulse response only ever contributes (L+R)/2: one convolution with that average IR. */
#include "engine.h"
#include <stdlib.h>

static float BASSC[4097], SAFEC[8193];
static uint32_t ir_rand = 0x3c6ef372u;

static void curves(void) {
    int i;
    for (i = 0; i < 4097; i++) { /* BassEnhancer curve */
        double x = 4 * ((i / 4096.0) * 2 - 1), t = tanh(x);
        BASSC[i] = (float)(t + 0.45 * t * t);
    }
    for (i = 0; i < 8193; i++) { /* SafetyClipper curve */
        double x = 4 * ((i / 8192.0) * 2 - 1), a = fabs(x), lin = 0.6, room = 0.99 - lin;
        double y = a <= lin ? a : lin + room * tanh((a - lin) / room);
        SAFEC[i] = (float)(x < 0 ? -y : x > 0 ? y : 0);
    }
}

static void reverb_ir(CONV *c) {
    enum { LEN = 38688 }; /* floor(48000 * (0.15 + 2.05 * 0.32)) */
    static float ch[2][LEN], avg[LEN];
    double size = 0.32, decay = 2.6 + 3.4 * (1 - size), power = 0, scale;
    int k, i, n;
    for (n = 0; n < 2; n++) {
        for (i = 0; i < LEN; i++) {
            double t = (double)i / LEN;
            ch[n][i] = (float)((xs_unit(&ir_rand) * 2 - 1) * pow(1 - t, decay));
        }
        for (k = 0; k < 7; k++) {
            int at = (int)floor((0.004 + 0.055 * xs_unit(&ir_rand)) * SR_HZ);
            float v = (float)((xs_unit(&ir_rand) * 2 - 1) * 0.55 * (1 - k / 7.0));
            if (at < LEN) ch[n][at] += v;
        }
    }
    /* Chromium Reverb::CalculateNormalizationScale (ConvolverNode.normalize = true) */
    for (n = 0; n < 2; n++)
        for (i = 0; i < LEN; i++) power += (double)ch[n][i] * ch[n][i];
    power = sqrt(power / (2.0 * LEN));
    if (!isfinite(power) || power < 0.000125) power = 0.000125;
    scale = 1 / power * pow(10, -58 * 0.05) * 44100.0 / SR_HZ;
    for (i = 0; i < LEN; i++) avg[i] = (float)(0.5 * (ch[0][i] + ch[1][i]) * scale);
    conv_init(c, avg, LEN);
}

void fx_init(Fx *f) {
    static const struct {
        float thr, ratio, attack, release, knee, makeup;
    } B[3] = {{-16, 2.6f, 0.018f, 0.26f, 10, 1.10f}, {-18, 2.0f, 0.022f, 0.20f, 14, 2.12f}, {-24, 3.0f, 0.003f, 0.12f, 8, 0.90f}};
    const float BW = -3.0103f;
    int c, i;
    memset(f, 0, sizeof(*f));
    curves();
    bqn_init(&f->infra[0], BQ_HP, 36, BW, 0);
    bqn_init(&f->infra[1], BQ_HP, 36, BW, 0);
    bqn_init(&f->blp1, BQ_LP, 140, BW, 0);
    bqn_init(&f->blp2, BQ_LP, 140, BW, 0);
    ws_init(&f->bassWS, BASSC, 4097, 2);
    bqn_init(&f->bhp, BQ_HP, 90, 0.7f, 0);
    bqn_init(&f->blp3, BQ_LP, 420, 0.6f, 0);
    bqn_init(&f->rumble, BQ_LS, 145, 1, 2);
    bqn_init(&f->body, BQ_PK, 78, 1.0f, 1.5f);
    bqn_init(&f->presence, BQ_PK, 3000, 0.85f, -1.5f);
    bqn_init(&f->airCut, BQ_HS, 2900, 1, -2);
    bqn_init(&f->cabinLP, BQ_LP, 17000, 0.6f, 0);
    bqn_init(&f->air, BQ_LP, 18000, 0.5f, 0);
    bqn_init(&f->revHP, BQ_HP, 110, 1, 0);
    bqn_init(&f->revDamp, BQ_LP, 4200, 0.7f, 0);
    f->revDry = (float)cos(0.14 * 3.14159265358979 / 2);
    f->revWet = (float)(sin(0.14 * 3.14159265358979 / 2) * 1.4);
    reverb_ir(&f->conv);
    f->sdl.buf = f->sbuf;
    f->sdl.mask = 4095;
    f->sdelay = (0.008f + 0.018f * 0.35f) * SR_F;
    f->sg = 0.35f * 0.85f;
    bqn_init(&f->tilt, BQ_HS, 1800, 1, -3);
    for (c = 0; c < 2; c++) {
        bq_coef(&f->xo[c][0], BQ_LP, 220, BW, 0);
        bq_coef(&f->xo[c][1], BQ_LP, 220, BW, 0);
        bq_coef(&f->xo[c][2], BQ_HP, 220, BW, 0);
        bq_coef(&f->xo[c][3], BQ_HP, 220, BW, 0);
        bq_coef(&f->xo[c][4], BQ_LP, 2000, BW, 0);
        bq_coef(&f->xo[c][5], BQ_LP, 2000, BW, 0);
        bq_coef(&f->xo[c][6], BQ_HP, 2000, BW, 0);
        bq_coef(&f->xo[c][7], BQ_HP, 2000, BW, 0);
    }
    for (i = 0; i < 3; i++) {
        comp_init(&f->band[i], B[i].thr, B[i].knee, B[i].ratio, B[i].attack, B[i].release);
        f->bandMakeup[i] = B[i].makeup * powf(10, -comp_makeup_db_js(B[i].thr, B[i].ratio, B[i].knee) / 20);
    }
    comp_init(&f->limiter, -1.5f, 4, 20, 0.004f, 0.16f);
    f->limiterTrim = powf(10, -comp_makeup_db_js(-1.5f, 20, 4) / 20);
    ws_init(&f->safety[0], SAFEC, 8193, 2);
    ws_init(&f->safety[1], SAFEC, 8193, 2);
    sp_init(&f->master, 1);
    /* ES_VALIDATE=1: unity, for sample-by-sample comparison against the browser */
    f->makeup = getenv("ES_VALIDATE") ? 1.f : 1.4125f;
}

void fx_run(Fx *f, const float *mix, float *l, float *r) {
    float a[QN], b[QN], w[QN], m[QN], s[QN], bl[3][QN], br[3][QN], t[QN];
    int i, k;
    bqn_run(&f->infra[0], mix, a, QN, 0);
    bqn_run(&f->infra[1], a, a, QN, 0);
    /* BassEnhancer: LR4 140 -> drive 2.2 -> span 1/4 -> shaper 2x -> HP 90 -> LP 420 -> 0.15 */
    bqn_run(&f->blp1, a, b, QN, 0);
    bqn_run(&f->blp2, b, b, QN, 0);
    for (i = 0; i < QN; i++) b[i] *= 2.2f * 0.25f;
    ws_run(&f->bassWS, b, b, QN);
    bqn_run(&f->bhp, b, b, QN, 0);
    bqn_run(&f->blp3, b, b, QN, 0);
    for (i = 0; i < QN; i++) a[i] += 0.15f * b[i];
    bqn_run(&f->rumble, a, a, QN, 0);
    bqn_run(&f->body, a, a, QN, 0);
    bqn_run(&f->presence, a, a, QN, 0);
    bqn_run(&f->airCut, a, a, QN, 0);
    bqn_run(&f->cabinLP, a, a, QN, 0);
    bqn_run(&f->air, a, a, QN, 0);
    /* reverb */
    bqn_run(&f->revHP, a, w, QN, 0);
    conv_run(&f->conv, w, w);
    bqn_run(&f->revDamp, w, w, QN, 0);
    for (i = 0; i < QN; i++) m[i] = f->revDry * a[i] + f->revWet * w[i];
    /* stereoizer */
    for (i = 0; i < QN; i++) {
        dl_write(&f->sdl, m[i]);
        s[i] = dl_read(&f->sdl, f->sdelay);
    }
    bqn_run(&f->tilt, s, s, QN, 0);
    for (i = 0; i < QN; i++) {
        l[i] = m[i] + f->sg * s[i];
        r[i] = m[i] - f->sg * s[i];
    }
    /* three-band dynamics (LR4 split) */
    bq_run(&f->xo[0][0], l, bl[0], QN);
    bq_run(&f->xo[0][1], bl[0], bl[0], QN);
    bq_run(&f->xo[1][0], r, br[0], QN);
    bq_run(&f->xo[1][1], br[0], br[0], QN);
    bq_run(&f->xo[0][2], l, bl[1], QN);
    bq_run(&f->xo[0][3], bl[1], bl[1], QN);
    bq_run(&f->xo[0][4], bl[1], bl[1], QN);
    bq_run(&f->xo[0][5], bl[1], bl[1], QN);
    bq_run(&f->xo[1][2], r, br[1], QN);
    bq_run(&f->xo[1][3], br[1], br[1], QN);
    bq_run(&f->xo[1][4], br[1], br[1], QN);
    bq_run(&f->xo[1][5], br[1], br[1], QN);
    bq_run(&f->xo[0][6], l, bl[2], QN);
    bq_run(&f->xo[0][7], bl[2], bl[2], QN);
    bq_run(&f->xo[1][6], r, br[2], QN);
    bq_run(&f->xo[1][7], br[2], br[2], QN);
    for (k = 0; k < 8; k++) {
        bq_flush(&f->xo[0][k]);
        bq_flush(&f->xo[1][k]);
    }
    for (i = 0; i < QN; i++) l[i] = r[i] = 0;
    for (k = 0; k < 3; k++) {
        comp_run(&f->band[k], bl[k], br[k], QN);
        for (i = 0; i < QN; i++) {
            l[i] += bl[k][i] * f->bandMakeup[k];
            r[i] += br[k][i] * f->bandMakeup[k];
        }
    }
    /* +3 dB into the brickwall: Engine_Sim's published renders are peak-normalised (+0.2..+5 dB, tools/samples.html);
       this brings the port to a similar loudness and lets the limiter, not the clipper, hold the peaks */
    for (i = 0; i < QN; i++) {
        l[i] *= f->makeup;
        r[i] *= f->makeup;
    }
    comp_run(&f->limiter, l, r, QN);
    sp_fill(&f->master, t, QN);
    for (i = 0; i < QN; i++) {
        float g = f->limiterTrim * t[i] * 0.25f;
        l[i] *= g;
        r[i] *= g;
    }
    ws_run(&f->safety[0], l, l, QN);
    ws_run(&f->safety[1], r, r, QN);
}
