/* wa.c - Web Audio node equivalents. Formulas follow the Web Audio spec and Chromium's implementation
 * (third_party/blink/renderer/platform/audio: biquad.cc, up_sampler.cc, down_sampler.cc, periodic_wave.cc,
 * dynamics_compressor_kernel.cc, reverb.cc). */
#include "wa.h"
#include <stdlib.h>

#define PI_D 3.14159265358979323846

float tc_k(float tc) {
    if (!(tc > 0)) return 1.f;
    return (float)(1.0 - exp(-1.0 / (SR_HZ * (double)tc)));
}

/* ------------------------------------------------------------------------------------------------ AP */
void ap_init(AP *a, float v) {
    memset(a, 0, sizeof(*a));
    a->cur = a->v0 = v;
}
void ap_cancel(AP *a, double t) {
    int i, j = 0;
    for (i = 0; i < a->n; i++)
        if (a->ev[i].t < t) a->ev[j++] = a->ev[i];
    a->n = j;
}
void ap_push(AP *a, int type, double t, float v, float tc) {
    int i;
    if (a->n == AP_MAX) return;
    i = a->n;
    while (i > 0 && a->ev[i - 1].t > t) {
        a->ev[i] = a->ev[i - 1];
        i--;
    }
    a->ev[i].t = t;
    a->ev[i].v = v;
    a->ev[i].type = type;
    a->ev[i].k = type == EV_TGT ? tc_k(tc) : 0;
    a->n++;
}
static void ap_pop(AP *a) {
    int i;
    for (i = 1; i < a->n; i++) a->ev[i - 1] = a->ev[i];
    a->n--;
}
float ap_at(AP *a, double t) {
    while (a->n) {
        PEv *e = &a->ev[0];
        if (e->type == EV_SET || e->type == EV_TGT) {
            if (t < e->t) break;
            a->t0 = e->t;
            if (e->type == EV_SET) {
                a->cur = e->v;
                a->mode = 0;
            } else {
                a->mode = 1;
                a->tgt = e->v;
                a->k = e->k;
            }
            a->v0 = a->cur;
            ap_pop(a);
            continue;
        }
        if (t >= e->t) {
            a->cur = e->v;
            a->t0 = e->t;
            a->v0 = a->cur;
            a->mode = 0;
            ap_pop(a);
            continue;
        }
        {
            double span = e->t - a->t0, x = span > 0 ? (t - a->t0) / span : 1;
            if (x < 0) x = 0;
            if (e->type == EV_LIN)
                a->cur = (float)(a->v0 + (e->v - a->v0) * x);
            else if (a->v0 > 0 && e->v > 0)
                a->cur = (float)(a->v0 * pow(e->v / a->v0, x));
            else
                a->cur = a->v0;
        }
        return a->cur;
    }
    if (a->mode) a->cur += (a->tgt - a->cur) * a->k;
    return a->cur;
}
void ap_fill(AP *a, double t, float *out, int n) {
    int i;
    if (!a->n && !a->mode) {
        for (i = 0; i < n; i++) out[i] = a->cur;
        return;
    }
    for (i = 0; i < n; i++) out[i] = ap_at(a, t + (double)i / SR_HZ);
}

/* ------------------------------------------------------------------------------------------------ biquad */
static void bq_set(BQ *b, double b0, double b1, double b2, double a0, double a1, double a2) {
    double s = 1.0 / a0;
    b->b0 = (float)(b0 * s);
    b->b1 = (float)(b1 * s);
    b->b2 = (float)(b2 * s);
    b->a1 = (float)(a1 * s);
    b->a2 = (float)(a2 * s);
}
void bq_coef(BQ *b, int type, float freq, float q, float g) {
    double nf = freq / 24000.0, w, s, c, alpha, A;
    if (!(nf > 0)) nf = 0;
    if (nf > 1) nf = 1;
    w = PI_D * nf;
    switch (type) {
    case BQ_LP:
        if (nf >= 1)
            bq_set(b, 1, 0, 0, 1, 0, 0);
        else if (nf > 0) {
            double sh = sin(w * .5), beta = sh * sh; /* (1-cos w)/2 */
            alpha = sin(w) / (2 * pow(10.0, q / 20.0));
            c = cos(w);
            bq_set(b, beta, 2 * beta, beta, 1 + alpha, -2 * c, 1 - alpha);
        } else
            bq_set(b, 0, 0, 0, 1, 0, 0);
        break;
    case BQ_HP:
        if (nf >= 1)
            bq_set(b, 0, 0, 0, 1, 0, 0);
        else if (nf > 0) {
            double ch = cos(w * .5), beta = ch * ch; /* (1+cos w)/2 */
            alpha = sin(w) / (2 * pow(10.0, q / 20.0));
            c = cos(w);
            bq_set(b, beta, -2 * beta, beta, 1 + alpha, -2 * c, 1 - alpha);
        } else
            bq_set(b, 1, 0, 0, 1, 0, 0);
        break;
    case BQ_BP:
        if (nf > 0 && nf < 1) {
            if (q > 0) {
                alpha = sin(w) / (2 * q);
                c = cos(w);
                bq_set(b, alpha, 0, -alpha, 1 + alpha, -2 * c, 1 - alpha);
            } else
                bq_set(b, 1, 0, 0, 1, 0, 0);
        } else
            bq_set(b, 0, 0, 0, 1, 0, 0);
        break;
    case BQ_LS:
        A = pow(10.0, g / 40.0);
        if (nf >= 1)
            bq_set(b, A * A, 0, 0, 1, 0, 0);
        else if (nf > 0) {
            double k, k2, ap, am;
            s = sin(w);
            alpha = 0.5 * s * sqrt(2.0); /* S = 1 */
            k = cos(w);
            k2 = 2 * sqrt(A) * alpha;
            ap = A + 1;
            am = A - 1;
            bq_set(b, A * (ap - am * k + k2), 2 * A * (am - ap * k), A * (ap - am * k - k2), ap + am * k + k2,
                   -2 * (am + ap * k), ap + am * k - k2);
        } else
            bq_set(b, 1, 0, 0, 1, 0, 0);
        break;
    case BQ_HS:
        A = pow(10.0, g / 40.0);
        if (nf >= 1)
            bq_set(b, 1, 0, 0, 1, 0, 0);
        else if (nf > 0) {
            double k, k2, ap, am;
            s = sin(w);
            alpha = 0.5 * s * sqrt(2.0);
            k = cos(w);
            k2 = 2 * sqrt(A) * alpha;
            ap = A + 1;
            am = A - 1;
            bq_set(b, A * (ap + am * k + k2), -2 * A * (am + ap * k), A * (ap + am * k - k2), ap - am * k + k2,
                   2 * (am - ap * k), ap - am * k - k2);
        } else
            bq_set(b, A * A, 0, 0, 1, 0, 0);
        break;
    default: /* peaking */
        A = pow(10.0, g / 40.0);
        if (nf > 0 && nf < 1) {
            if (q > 0) {
                alpha = sin(w) / (2 * q);
                c = cos(w);
                bq_set(b, 1 + alpha * A, -2 * c, 1 - alpha * A, 1 + alpha / A, -2 * c, 1 - alpha / A);
            } else
                bq_set(b, A * A, 0, 0, 1, 0, 0);
        } else
            bq_set(b, 1, 0, 0, 1, 0, 0);
        break;
    }
}
void bq_run(BQ *b, const float *in, float *out, int n) {
    float b0 = b->b0, b1 = b->b1, b2 = b->b2, a1 = b->a1, a2 = b->a2, s1 = b->s1, s2 = b->s2;
    int i;
    for (i = 0; i < n; i++) {
        float x = in[i], y = b0 * x + s1;
        s1 = b1 * x - a1 * y + s2;
        s2 = b2 * x - a2 * y;
        out[i] = y;
    }
    b->s1 = s1;
    b->s2 = s2;
}
void bq_flush(BQ *b) {
    if (!isfinite(b->s1) || !isfinite(b->s2) || fabsf(b->s1) > 1e6f || fabsf(b->s2) > 1e6f) b->s1 = b->s2 = 0;
    if (fabsf(b->s1) < 1e-25f) b->s1 = 0;
    if (fabsf(b->s2) < 1e-25f) b->s2 = 0;
}
void bqn_init(BQN *n, int type, float f, float q, float g) {
    memset(n, 0, sizeof(*n));
    n->type = type;
    sp_init(&n->f, f);
    sp_init(&n->q, q);
    sp_init(&n->g, g);
    n->cf = f;
    n->cq = q;
    n->cg = g;
    bq_coef(&n->b, type, f, q, g);
    n->fresh = 1;
}
static int moved(float a, float b) {
    float d = fabsf(a - b);
    return d > 2e-6f * fabsf(b) + 1e-9f;
}
void bqn_run(BQN *n, const float *in, float *out, int len, const float *fmod) {
    int i;
    for (i = 0; i < len; i += SUB) {
        int m = len - i < SUB ? len - i : SUB;
        float f, q, g;
        sp_fill(&n->f, 0, m);
        sp_fill(&n->q, 0, m);
        sp_fill(&n->g, 0, m);
        f = n->f.v + (fmod ? fmod[i] : 0);
        q = n->q.v;
        g = n->g.v;
        if (moved(f, n->cf) || moved(q, n->cq) || moved(g, n->cg)) {
            n->cf = f;
            n->cq = q;
            n->cg = g;
            bq_coef(&n->b, n->type, f, q, g);
        }
        bq_run(&n->b, in + i, out + i, m);
    }
    bq_flush(&n->b);
}
void bq_run_ap(BQ *b, int type, AP *f, AP *q, double t, const float *in, float *out, int len, float *cf, float *cq) {
    int i;
    for (i = 0; i < len; i += SUB) {
        int m = len - i < SUB ? len - i : SUB, k;
        double ts = t + (double)i / SR_HZ;
        float fv = ap_at(f, ts), qv = ap_at(q, ts);
        (void)k;
        if (moved(fv, *cf) || moved(qv, *cq)) {
            *cf = fv;
            *cq = qv;
            bq_coef(b, type, fv, qv, 0);
        }
        bq_run(b, in + i, out + i, m);
    }
    bq_flush(b);
}

/* ------------------------------------------------------------------------------------------------ shaper */
static float UPK[128], DNK[128];
static int sampler_ready;
static void samplers_init(void) {
    double alpha = 0.16, a0 = 0.5 * (1.0 - alpha), a1 = 0.5, a2 = 0.5 * alpha;
    int i, n = 128, half = 64;
    for (i = 0; i < n; i++) { /* UpSampler::InitializeKernel, subsample offset -0.5 */
        double s = PI_D * (i - half + 0.5), sinc = s == 0 ? 1.0 : sin(s) / s, x = (i + 0.5) / n;
        double win = a0 - a1 * cos(2 * PI_D * x) + a2 * cos(2 * PI_D * 2.0 * x);
        UPK[i] = (float)(sinc * win);
    }
    n = 256;
    half = 128;
    for (i = 1; i < n; i += 2) { /* DownSampler::InitializeKernel: odd taps of a half-band */
        double s = 0.5 * PI_D * (i - half), sinc = s == 0 ? 1.0 : sin(s) / s, x = (double)i / n;
        double win = a0 - a1 * cos(2 * PI_D * x) + a2 * cos(2 * PI_D * 2.0 * x);
        sinc *= 0.5;
        DNK[(i - 1) / 2] = (float)(sinc * win);
    }
    sampler_ready = 1;
}
/* hist: 127 history + n new at hist[127..]; produces 2n */
static void upsample(float *hist, const float *in, float *out, int n) {
    float odd[2 * QN];
    int i, j;
    memcpy(hist + 127, in, sizeof(float) * n);
    for (i = 0; i < n; i++) odd[i] = 0;
    for (j = 0; j < 64; j++) { /* the kernel is symmetric: k[j] == k[127 - j] */
        const float k = UPK[j], *x = hist + 127 - j, *z = hist + j;
        for (i = 0; i < n; i++) odd[i] += k * (x[i] + z[i]);
    }
    for (i = 0; i < n; i++) {
        out[2 * i] = hist[127 + i - 64];
        out[2 * i + 1] = odd[i];
    }
    memmove(hist, hist + n, sizeof(float) * 127);
}
/* oh: odd-sample history (127) + n ; eh: even history (64) + n ; carry = last input sample of previous block.
 * in has 2n samples, produces n:  y[m] = 0.5 s[2m-128] + sum_j DNK[j] s[2m-1-2j] */
static void downsample(float *oh, float *eh, float *carry, const float *in, float *out, int n) {
    int m, j;
    for (m = 0; m < n; m++) {
        oh[127 + m] = m ? in[2 * m - 1] : *carry;
        eh[64 + m] = in[2 * m];
    }
    *carry = in[2 * n - 1];
    for (m = 0; m < n; m++) out[m] = 0.5f * eh[m]; /* eh[64 + m - 64] */
    for (j = 0; j < 64; j++) { /* symmetric half-band kernel */
        const float k = DNK[j], *x = oh + 127 - j, *z = oh + j;
        for (m = 0; m < n; m++) out[m] += k * (x[m] + z[m]);
    }
    memmove(oh, oh + n, sizeof(float) * 127);
    memmove(eh, eh + n, sizeof(float) * 64);
}
void ws_init(WS *w, const float *curve, int len, int over) {
    memset(w, 0, sizeof(*w));
    if (!sampler_ready) samplers_init();
    w->curve = curve;
    w->len = len;
    w->over = over;
}
static void ws_curve(const WS *w, const float *in, float *out, int n) {
    const float *c = w->curve;
    float half = (w->len - 1) * 0.5f, top = (float)(w->len - 1);
    int i;
    for (i = 0; i < n; i++) {
        float v = half * (in[i] + 1.f);
        if (!(v > 0))
            out[i] = c[0];
        else if (v >= top)
            out[i] = c[w->len - 1];
        else {
            int k = (int)v;
            float f = v - (float)k;
            out[i] = c[k] + (c[k + 1] - c[k]) * f;
        }
    }
}
void ws_run(WS *w, const float *in, float *out, int n) {
    float a[4 * QN], b[4 * QN];
    /* layout inside WS: uh1 (127+QN), uh2 (127+2QN), dh1 = odd hist for 2x->1x (127+QN) + even (64+QN),
       dh2 = odd hist 4x->2x (127+2QN) + even (64+2QN) */
    if (w->over == 1) {
        ws_curve(w, in, out, n);
        return;
    }
    if (w->over == 2) {
        upsample(w->uh1, in, a, n);
        ws_curve(w, a, a, 2 * n);
        downsample(w->dh1, w->dh1 + 127 + QN, &w->dh1[127 + QN + 64 + QN], a, out, n);
        return;
    }
    upsample(w->uh1, in, a, n);
    upsample(w->uh2, a, b, 2 * n);
    ws_curve(w, b, b, 4 * n);
    downsample(w->dh2, w->dh2 + 127 + 2 * QN, &w->dh2[127 + 2 * QN + 64 + 2 * QN], b, a, 2 * n);
    downsample(w->dh1, w->dh1 + 127 + QN, &w->dh1[127 + QN + 64 + QN], a, out, n);
}

/* ------------------------------------------------------------------------------------------------ FFT */
void fft_init(FFT *f, int n, float *cs, int *rev) {
    int m = n / 2, i, j, bits = 0;
    f->n = n;
    f->cs = cs;
    f->rev = rev;
    while ((1 << bits) < m) bits++;
    for (i = 0; i < m; i++) {
        int r = 0;
        for (j = 0; j < bits; j++)
            if (i & (1 << j)) r |= 1 << (bits - 1 - j);
        rev[i] = r;
    }
    /* cs[0..m): cos/sin(2 pi i / m) for the complex m-point FFT, cs[m..m+n/2+1): twiddles for the real split */
    for (i = 0; i < m / 2; i++) {
        cs[2 * i] = (float)cos(2 * PI_D * i / m);
        cs[2 * i + 1] = (float)-sin(2 * PI_D * i / m);
    }
    for (i = 0; i <= n / 2; i++) {
        cs[m + 2 * i] = (float)cos(2 * PI_D * i / n);
        cs[m + 2 * i + 1] = (float)-sin(2 * PI_D * i / n);
    }
}
/* in-place complex FFT of size m, sign -1 (forward) or +1 (inverse, unscaled) */
static void cfft(const FFT *f, float *re, float *im, int inverse) {
    int m = f->n / 2, i, len;
    for (i = 0; i < m; i++) {
        int r = f->rev[i];
        if (r > i) {
            float t = re[i];
            re[i] = re[r];
            re[r] = t;
            t = im[i];
            im[i] = im[r];
            im[r] = t;
        }
    }
    for (len = 2; len <= m; len <<= 1) {
        int half = len >> 1, step = m / len, k, s;
        for (s = 0; s < m; s += len) {
            for (k = 0; k < half; k++) {
                float wr = f->cs[2 * k * step], wi = f->cs[2 * k * step + 1];
                float *ar = re + s + k, *ai = im + s + k, *br = re + s + k + half, *bi = im + s + k + half;
                float tr, ti;
                if (inverse) wi = -wi;
                tr = *br * wr - *bi * wi;
                ti = *br * wi + *bi * wr;
                *br = *ar - tr;
                *bi = *ai - ti;
                *ar += tr;
                *ai += ti;
            }
        }
    }
}
void fft_real_fwd(const FFT *f, const float *in, float *re, float *im) {
    int n = f->n, m = n / 2, k;
    float zr[4096], zi[4096];
    const float *tw = f->cs + m;
    for (k = 0; k < m; k++) {
        zr[k] = in[2 * k];
        zi[k] = in[2 * k + 1];
    }
    cfft(f, zr, zi, 0);
    for (k = 0; k <= m; k++) {
        int a = k % m, b = (m - k) % m;
        float er = 0.5f * (zr[a] + zr[b]), ei = 0.5f * (zi[a] - zi[b]);
        float or_ = 0.5f * (zi[a] + zi[b]), oi = -0.5f * (zr[a] - zr[b]);
        float wr = tw[2 * k], wi = tw[2 * k + 1];
        re[k] = er + wr * or_ - wi * oi;
        im[k] = ei + wr * oi + wi * or_;
    }
}
void fft_real_inv(const FFT *f, const float *re, const float *im, float *out) {
    int n = f->n, m = n / 2, k;
    float zr[4096], zi[4096], s = 1.f / m;
    const float *tw = f->cs + m;
    for (k = 0; k < m; k++) {
        float xr = re[k], xi = im[k], cr = re[m - k], ci = -im[m - k];
        float er = 0.5f * (xr + cr), ei = 0.5f * (xi + ci);
        float dr = 0.5f * (xr - cr), di = 0.5f * (xi - ci);
        float wr = tw[2 * k], wi = -tw[2 * k + 1]; /* e^{+2 pi i k / n} */
        float or_ = dr * wr - di * wi, oi = dr * wi + di * wr;
        zr[k] = er - oi; /* E + iO */
        zi[k] = ei + or_;
    }
    cfft(f, zr, zi, 1);
    for (k = 0; k < m; k++) {
        out[2 * k] = zr[k] * s;
        out[2 * k + 1] = zi[k] * s;
    }
}

/* ------------------------------------------------------------------------------------------------ waves */
Wave WAVE_SINE, WAVE_SAW, WAVE_BLADE;
static float sine_store[PW_SIZE + 1], saw_store[PW_RANGES * (PW_SIZE + 1)], blade_store[PW_RANGES * (PW_SIZE + 1)];
static float fft4096_cs[2048 + 4096 + 4];
static int fft4096_rev[2048];
static FFT F4096;
void wave_build(Wave *w, const float *real, const float *imag, int ncomp, int normalise, float *st) {
    static float re[2049], im[2049];
    float scale = 1.f;
    int r, i;
    if (ncomp > PW_SIZE / 2) ncomp = PW_SIZE / 2;
    for (r = 0; r < PW_RANGES; r++) {
        int partials = (int)(pow(2.0, -(r * 400.0) / 1200.0) * (PW_SIZE / 2)), top;
        float *t = st + r * (PW_SIZE + 1);
        top = ncomp < partials + 1 ? ncomp : partials + 1;
        for (i = 0; i <= PW_SIZE / 2; i++) re[i] = im[i] = 0;
        for (i = 1; i < top; i++) {
            re[i] = (real ? real[i] : 0) * (PW_SIZE / 2);
            im[i] = -(imag ? imag[i] : 0) * (PW_SIZE / 2);
        }
        fft_real_inv(&F4096, re, im, t);
        if (normalise && r == 0) {
            float mx = 0;
            for (i = 0; i < PW_SIZE; i++)
                if (fabsf(t[i]) > mx) mx = fabsf(t[i]);
            if (mx > 0) scale = 1.f / mx;
        }
        for (i = 0; i < PW_SIZE; i++) t[i] *= scale;
        t[PW_SIZE] = t[0];
        w->t[r] = t;
    }
    w->single = 0;
}
void waves_init(void) {
    static int done;
    static float re[2048], im[2048];
    int i;
    if (done) return;
    done = 1;
    fft_init(&F4096, PW_SIZE, fft4096_cs, fft4096_rev);
    for (i = 0; i < PW_SIZE; i++) sine_store[i] = (float)sin(2 * PI_D * i / PW_SIZE);
    sine_store[PW_SIZE] = 0;
    for (i = 0; i < PW_RANGES; i++) WAVE_SINE.t[i] = sine_store;
    WAVE_SINE.single = 1;
    for (i = 0; i < 2048; i++) {
        re[i] = 0;
        im[i] = i ? (float)(2.0 * ((i & 1) ? 1 : -1) / (PI_D * i)) : 0; /* sawtooth b_n = 2(-1)^(n+1)/(pi n) */
    }
    wave_build(&WAVE_SAW, re, im, 2048, 1, saw_store);
    for (i = 0; i < 11; i++) {
        re[i] = 0;
        im[i] = i ? (float)(1.0 / pow(i, 1.4) * ((i % 2) ? 1.25 : 0.8)) : 0; /* layers.js bladeWave */
    }
    wave_build(&WAVE_BLADE, re, im, 11, 1, blade_store);
}
void osc_run(OSC *o, const float *freq, float *out, int n) {
    const Wave *w = o->w;
    double ph = o->ph;
    int i;
    for (i = 0; i < n; i += SUB) {
        int m = n - i < SUB ? n - i : SUB, k;
        const float *lo, *hi;
        float tf = 0;
        if (w->single)
            lo = hi = w->t[0];
        else {
            float f = fabsf(freq[i]), pr;
            pr = f > 0 ? 1.f + 3.f * fast_log2(f / (24000.f / 2048.f)) : 0.f;
            if (pr < 0) pr = 0;
            if (pr > PW_RANGES - 1) pr = PW_RANGES - 1;
            {
                int r1 = (int)pr, r2 = r1 < PW_RANGES - 1 ? r1 + 1 : r1;
                hi = w->t[r1];
                lo = w->t[r2];
                tf = pr - (float)r1;
            }
        }
        for (k = 0; k < m; k++) {
            double p = ph * PW_SIZE;
            int idx = (int)p;
            float fr = (float)(p - idx), sl, sh;
            sh = hi[idx] + (hi[idx + 1] - hi[idx]) * fr;
            sl = lo[idx] + (lo[idx + 1] - lo[idx]) * fr;
            out[i + k] = sh + (sl - sh) * tf;
            ph += freq[i + k] * (1.0 / SR_HZ);
            ph -= floor(ph);
        }
    }
    o->ph = ph;
}
void nsrc_run(NSRC *s, float *out, int n) {
    int i = 0;
    while (i < n) {
        int m = s->len - s->pos;
        if (m > n - i) m = n - i;
        memcpy(out + i, s->buf + s->pos, sizeof(float) * m);
        i += m;
        s->pos += m;
        if (s->pos >= s->len) s->pos = 0;
    }
}

/* ------------------------------------------------------------------------------------------------ compressor */
static float knee_curve(const COMP *c, float x, float k) {
    if (x < c->lin_thr) return x;
    return c->lin_thr + (1.f - expf(-k * (x - c->lin_thr))) / k;
}
static float l2db(float x) {
    return 20.f * log10f(x);
}
static float d2l(float x) {
    return powf(10.f, 0.05f * x);
}
static float slope_at(const COMP *c, float x, float k) {
    float x2, xd, x2d, yd, y2d;
    if (x < c->lin_thr) return 1;
    x2 = x * 1.001f;
    xd = l2db(x);
    x2d = l2db(x2);
    yd = l2db(knee_curve(c, x, k));
    y2d = l2db(knee_curve(c, x2, k));
    return (y2d - yd) / (x2d - xd);
}
static void comp_curve(COMP *c) {
    float x, mink = 0.1f, maxk = 10000, k = 5;
    int i;
    c->c_thr = c->thr;
    c->c_knee = c->knee;
    c->c_ratio = c->ratio;
    c->lin_thr = d2l(c->thr);
    c->slope = 1.f / c->ratio;
    x = d2l(c->thr + c->knee);
    for (i = 0; i < 15; i++) { /* KAtSlope */
        float s = slope_at(c, x, k);
        if (s < c->slope)
            maxk = k;
        else
            mink = k;
        k = sqrtf(mink * maxk);
    }
    c->knee_thr_db = c->thr + c->knee;
    c->knee_thr = d2l(c->knee_thr_db);
    c->yknee_thr_db = l2db(knee_curve(c, c->knee_thr, k));
    c->k = k;
}
static inline float saturate(const COMP *c, float x) {
    if (x < c->knee_thr) {
        if (x < c->lin_thr) return x;
        return c->lin_thr + (1.f - fast_exp(-c->k * (x - c->lin_thr))) / c->k;
    }
    return db_to_lin(c->yknee_thr_db + c->slope * (lin_to_db(x) - c->knee_thr_db));
}
void comp_init(COMP *c, float thr, float knee, float ratio, float attack, float release) {
    memset(c, 0, sizeof(*c));
    c->thr = thr;
    c->knee = knee;
    c->ratio = ratio;
    c->attack = attack;
    c->release = release;
    comp_curve(c);
    c->detector_average = 0;
    c->compressor_gain = 1;
    c->max_attack_diff_db = -1;
    c->rd = 0;
    c->wr = (int)(0.006f * SR_F); /* pre-delay 6 ms (SetPreDelayTime) */
}
static inline float fast_sin_half_pi(float g) { /* sin(pi/2 * g), g in [0,1] */
    float x = 1.57079633f * g, x2 = x * x;
    return x * (1.f + x2 * (-1.6666667e-1f + x2 * (8.3333310e-3f + x2 * (-1.9840874e-4f + x2 * 2.7525562e-6f))));
}
void comp_run(COMP *c, float *l, float *r, int n) {
    const float sr = SR_F;
    float k = c->k, full_gain, makeup, master, attack_frames, release_frames, sat_release_frames;
    float y1, y2, y3, y4, kA, kB, kC, kD, kE;
    int div, idx = 0;
    if (c->thr != c->c_thr || c->knee != c->c_knee || c->ratio != c->c_ratio) {
        comp_curve(c);
        k = c->k;
    }
    full_gain = c->knee_thr > 1 ? knee_curve(c, 1, k)
                                : powf(10.f, 0.05f * (c->yknee_thr_db + c->slope * (0 - c->knee_thr_db)));
    if (1 < c->lin_thr) full_gain = 1;
    makeup = powf(1.f / full_gain, 0.6f);
    master = makeup; /* post gain 0 dB */
    attack_frames = (c->attack > 0.001f ? c->attack : 0.001f) * sr;
    release_frames = sr * c->release;
    sat_release_frames = 0.0025f * sr;
    y1 = release_frames * 0.09f;
    y2 = release_frames * 0.16f;
    y3 = release_frames * 0.42f;
    y4 = release_frames * 0.98f;
    kA = 0.9999999999999998f * y1 + 1.8432219684323923e-16f * y2 - 1.9373394351676423e-16f * y3 +
         8.824516011816245e-18f * y4;
    kB = -1.5788320352845888f * y1 + 2.3305837032074286f * y2 - 0.9141194204840429f * y3 + 0.1623677525612032f * y4;
    kC = 0.5334142869106424f * y1 - 1.272736789213631f * y2 + 0.9258856042207512f * y3 - 0.18656310191776226f * y4;
    kD = 0.08783463138207234f * y1 - 0.1694162967925622f * y2 + 0.08588057951595272f * y3 -
         0.00429891410546283f * y4;
    kE = -0.042416883008123074f * y1 + 0.1115693827987602f * y2 - 0.09764676325265872f * y3 +
         0.028494263462021576f * y4;
    for (div = 0; div < n / 32; div++) {
        float desired, scaled, rate, diff_db;
        int releasing, f;
        if (!isfinite(c->detector_average)) c->detector_average = 1;
        desired = c->detector_average;
        scaled = asinf(desired) / 1.57079633f;
        releasing = scaled > c->compressor_gain;
        diff_db = l2db(c->compressor_gain / scaled);
        if (releasing) {
            float x, x2, x3, x4, rf;
            c->max_attack_diff_db = -1;
            if (!isfinite(diff_db)) diff_db = -1;
            x = clampf(diff_db, -12.f, 0.f);
            x = 0.25f * (x + 12);
            x2 = x * x;
            x3 = x2 * x;
            x4 = x2 * x2;
            rf = kA + kB * x + kC * x2 + kD * x3 + kE * x4;
            rate = d2l(5.f / rf);
        } else {
            float eff, x;
            if (!isfinite(diff_db)) diff_db = 1;
            if (c->max_attack_diff_db == -1 || c->max_attack_diff_db < diff_db) c->max_attack_diff_db = diff_db;
            eff = c->max_attack_diff_db > 0.5f ? c->max_attack_diff_db : 0.5f;
            x = 0.25f / eff;
            rate = 1 - powf(x, 1 / attack_frames);
        }
        {
            float det = c->detector_average, gain = c->compressor_gain;
            int rd = c->rd, wr = c->wr;
            for (f = 0; f < 32; f++, idx++) {
                float in = fabsf(l[idx]), a2 = fabsf(r[idx]), shaped, att, att_db, srr, total;
                c->pd[0][wr] = l[idx];
                c->pd[1][wr] = r[idx];
                if (a2 > in) in = a2;
                if (in < c->lin_thr) {
                    att = 1;
                    att_db = 0;
                } else {
                    shaped = saturate(c, in);
                    att = in <= 0.0001f ? 1 : shaped / in;
                    att_db = -lin_to_db(att);
                }
                if (att_db < 2) att_db = 2;
                srr = db_to_lin(att_db / sat_release_frames) - 1;
                det += (att - det) * (att > det ? srr : 1);
                if (det > 1) det = 1;
                if (!isfinite(det)) det = 1;
                if (rate < 1)
                    gain += (scaled - gain) * rate;
                else {
                    gain *= rate;
                    if (gain > 1) gain = 1;
                }
                total = master * fast_sin_half_pi(gain);
                l[idx] = c->pd[0][rd] * total;
                r[idx] = c->pd[1][rd] * total;
                rd = (rd + 1) & 1023;
                wr = (wr + 1) & 1023;
            }
            c->detector_average = det;
            c->compressor_gain = gain;
            c->rd = rd;
            c->wr = wr;
        }
    }
}
float comp_makeup_db_js(float threshold, float ratio, float knee) {
    float t = (threshold < 0 ? threshold : 0) + (knee > 0 ? knee : 0) / 2, r = ratio > 1 ? ratio : 1, v;
    v = -0.6f * (t < 0 ? t : 0) * (1 - 1 / r);
    return v > 0 ? v : 0;
}

/* ------------------------------------------------------------------------------------------------ convolver */
#define CA_B 128
#define CA_N 256
#define CA_BINS 129
#define CB_B 1024
#define CB_N 2048
#define CB_BINS 1025
#define CB_MAXP 48
#define CA_P 8
static float fa_cs[CA_N / 2 + CA_N + 4], fb_cs[CB_N / 2 + CB_N + 4];
static int fa_rev[CA_N / 2], fb_rev[CB_N / 2];
static FFT FA, FB;
static float HAr_s[CA_P * CA_BINS], HAi_s[CA_P * CA_BINS], XAr_s[CA_P * CA_BINS], XAi_s[CA_P * CA_BINS];
static float HBr_s[CB_MAXP * CB_BINS], HBi_s[CB_MAXP * CB_BINS], XBr_s[CB_MAXP * CB_BINS], XBi_s[CB_MAXP * CB_BINS];
int conv_init(CONV *c, const float *ir, int len) {
    float tmp[CB_N];
    int p, i;
    memset(c, 0, sizeof(*c));
    fft_init(&FA, CA_N, fa_cs, fa_rev);
    fft_init(&FB, CB_N, fb_cs, fb_rev);
    c->HAr = HAr_s;
    c->HAi = HAi_s;
    c->XAr = XAr_s;
    c->XAi = XAi_s;
    c->HBr = HBr_s;
    c->HBi = HBi_s;
    c->XBr = XBr_s;
    c->XBi = XBi_s;
    memset(XAr_s, 0, sizeof(XAr_s));
    memset(XAi_s, 0, sizeof(XAi_s));
    memset(XBr_s, 0, sizeof(XBr_s));
    memset(XBi_s, 0, sizeof(XBi_s));
    c->pa = CA_P;
    for (p = 0; p < CA_P; p++) {
        for (i = 0; i < CA_N; i++) tmp[i] = 0;
        for (i = 0; i < CA_B; i++)
            if (p * CA_B + i < len) tmp[i] = ir[p * CA_B + i];
        fft_real_fwd(&FA, tmp, c->HAr + p * CA_BINS, c->HAi + p * CA_BINS);
    }
    c->pb = len > CA_P * CA_B ? (len - CA_P * CA_B + CB_B - 1) / CB_B : 0;
    if (c->pb > CB_MAXP) c->pb = CB_MAXP;
    for (p = 0; p < c->pb; p++) {
        for (i = 0; i < CB_N; i++) tmp[i] = 0;
        for (i = 0; i < CB_B; i++) {
            int k = CA_P * CA_B + p * CB_B + i;
            if (k < len) tmp[i] = ir[k];
        }
        fft_real_fwd(&FB, tmp, c->HBr + p * CB_BINS, c->HBi + p * CB_BINS);
    }
    return 0;
}
void conv_run(CONV *c, const float *in, float *out) {
    float yr[CB_BINS], yi[CB_BINS], y[CB_N];
    int k, b, slot;
    /* stage B: at each 1024 boundary compute the next 1024 outputs from complete past blocks */
    if (c->pb && (c->qcount & 7) == 0 && c->qcount) {
        slot = c->ib;
        fft_real_fwd(&FB, c->inB, c->XBr + slot * CB_BINS, c->XBi + slot * CB_BINS);
        for (b = 0; b < CB_BINS; b++) yr[b] = yi[b] = 0;
        for (k = 0; k < c->pb; k++) {
            int s = (slot - k + c->pb) % c->pb;
            const float *xr = c->XBr + s * CB_BINS, *xi = c->XBi + s * CB_BINS, *hr = c->HBr + k * CB_BINS,
                        *hi = c->HBi + k * CB_BINS;
            for (b = 0; b < CB_BINS; b++) {
                yr[b] += xr[b] * hr[b] - xi[b] * hi[b];
                yi[b] += xr[b] * hi[b] + xi[b] * hr[b];
            }
        }
        fft_real_inv(&FB, yr, yi, y);
        memcpy(c->outB, y + CB_B, sizeof(float) * CB_B);
        c->ib = (c->ib + 1) % c->pb;
        memmove(c->inB, c->inB + CB_B, sizeof(float) * CB_B);
    }
    /* collect input for stage B */
    memcpy(c->inB + CB_B + (c->qcount & 7) * QN, in, sizeof(float) * QN);
    /* stage A */
    memcpy(c->inA + CA_B, in, sizeof(float) * QN);
    slot = c->ia;
    fft_real_fwd(&FA, c->inA, c->XAr + slot * CA_BINS, c->XAi + slot * CA_BINS);
    for (b = 0; b < CA_BINS; b++) yr[b] = yi[b] = 0;
    for (k = 0; k < CA_P; k++) {
        int s = (slot - k + CA_P) % CA_P;
        const float *xr = c->XAr + s * CA_BINS, *xi = c->XAi + s * CA_BINS, *hr = c->HAr + k * CA_BINS,
                    *hi = c->HAi + k * CA_BINS;
        for (b = 0; b < CA_BINS; b++) {
            yr[b] += xr[b] * hr[b] - xi[b] * hi[b];
            yi[b] += xr[b] * hi[b] + xi[b] * hr[b];
        }
    }
    fft_real_inv(&FA, yr, yi, y);
    c->ia = (c->ia + 1) % CA_P;
    memmove(c->inA, c->inA + CA_B, sizeof(float) * CA_B);
    {
        const float *ob = c->outB + (c->qcount & 7) * QN;
        for (k = 0; k < QN; k++) out[k] = y[CA_B + k] + (c->pb ? ob[k] : 0);
    }
    c->qcount++;
}
