#define _POSIX_C_SOURCE 200809L
#include "out_alsa.h"
#include <dlfcn.h>
#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int load(Alsa *a) {
#define GET(field, name)                                                                                     \
    do {                                                                                                     \
        void *sym = dlsym(a->lib, name);                                                                     \
        memcpy(&a->field, &sym, sizeof(sym));                                                                \
        if (!sym) return 0;                                                                                  \
    } while (0)
    a->lib = dlopen("libasound.so.2", RTLD_NOW | RTLD_LOCAL);
    if (!a->lib) return 0;
    GET(open, "snd_pcm_open");
    GET(params, "snd_pcm_set_params");
    GET(write, "snd_pcm_writei");
    GET(recover, "snd_pcm_recover");
    GET(close, "snd_pcm_close");
    GET(drop, "snd_pcm_drop");
    GET(delay, "snd_pcm_delay");
    GET(error, "snd_strerror");
    GET(get_params, "snd_pcm_get_params");
    return 1;
}

static void drop_pcm(Alsa *a, double retry_at) {
    if (a->pcm) {
        a->drop(a->pcm);
        a->close(a->pcm);
        a->pcm = 0;
    }
    a->retry = retry_at;
}

void alsa_close(Alsa *a) {
    drop_pcm(a, 0);
    if (a->lib) {
        dlclose(a->lib);
        a->lib = 0;
    }
}

/* The device's own format, from a SmartX endpoint's name: ..._CF_<kHz>_<MONO|STEREO...>; else 48 kHz stereo. */
static void format_of(const char *dev, unsigned *rate, unsigned *channels) {
    const char *p = strstr(dev, "_CF_");
    char *end;
    *rate = 48000, *channels = 2;
    if (!p) return;
    if (strtol(p + 4, &end, 10) == 24) *rate = 24000;
    if (strstr(end, "MONO")) *channels = 1;
}

/* The 2:1 decimator's low-pass: windowed sinc (Blackman), cut-off 11 kHz at 48 kHz: flat to ~8 kHz, 70 dB down from
 * ~14 kHz, so nothing audible folds back below 10 kHz at 24 kHz. */
static void decimator(Alsa *a) {
    double sum = 0;
    int k;
    for (k = 0; k < ALSA_FIR; k++) {
        double m = k - (ALSA_FIR - 1) / 2., x = 2 * 3.141592653589793 * 11000. / 48000. * m;
        double w = .42 - .5 * cos(2 * 3.141592653589793 * k / (ALSA_FIR - 1)) + .08 * cos(4 * 3.141592653589793 * k / (ALSA_FIR - 1));
        a->fir[k] = (float)((m == 0 ? 1 : sin(x) / x) * w);
        sum += a->fir[k];
    }
    for (k = 0; k < ALSA_FIR; k++) a->fir[k] = (float)(a->fir[k] / sum);
    memset(a->ring, 0, sizeof(a->ring));
    a->ring_pos = a->phase = 0;
}

static int open_pcm(Alsa *a, const char *dev, double now) {
    unsigned us = 64000, rate, channels;
    const char *env = getenv("ES_ALSA_US");
    int e;
    if (now < a->retry) return 0;
    a->retry = now + 2;
    if (env) {
        long x = strtol(env, 0, 10);
        if (x >= 16000 && x <= 200000) us = (unsigned)x;
    }
    if (!a->lib && !load(a)) {
        fprintf(stderr, "alsa: libasound unavailable; retry in 2s\n");
        alsa_close(a);
        a->retry = now + 2;
        return 0;
    }
    e = a->open(&a->pcm, dev, 0 /* PLAYBACK */, 1 /* SND_PCM_NONBLOCK */);
    /* S16_LE = 2, RW_INTERLEAVED = 3, soft resample on: the endpoint's own format; if the device refuses it,
     * 48 kHz stereo (an ALSA plug in front converts) */
    format_of(dev, &rate, &channels);
    if (e >= 0) {
        e = a->params(a->pcm, 2, 3, channels, rate, 1, us);
        if (e < 0 && (rate != 48000 || channels != 2)) {
            fprintf(stderr, "alsa: %s: %u Hz %u ch refused (%s); trying 48000 Hz stereo\n", dev, rate, channels, a->error(e));
            rate = 48000, channels = 2;
            e = a->params(a->pcm, 2, 3, channels, rate, 1, us);
        }
    }
    if (e < 0) {
        fprintf(stderr, "alsa: %s: %s; retry in 2s\n", dev, a->error(e));
        if (a->pcm) {
            a->close(a->pcm);
            a->pcm = 0;
        }
        return 0;
    }
    /* snd_pcm_set_params starts playback once the buffer is full (SmartX: 3 x 1024 frames = 64 ms). Fill it; once
     * it runs, keep one period less queued (2 x 1024 = 43 ms): the device still finds a whole period after each
     * read, and the sound is 21 ms closer to the engine. ES_ALSA_KEEP (frames) overrides. */
    {
        unsigned long buffer = 0, period = 0;
        const char *k = getenv("ES_ALSA_KEEP");
        a->rate = rate, a->channels = channels, a->div = rate == 24000 ? 2 : 1;
        decimator(a);
        if (a->get_params(a->pcm, &buffer, &period) < 0 || buffer < 256) buffer = (unsigned long)us * (rate / 1000) / 1000, period = 0;
        a->buffer = (long)buffer;
        a->keep = period >= 128 && buffer >= 3 * period ? (long)(buffer - period) : (long)buffer;
        if (k) {
            long x = strtol(k, 0, 10);
            if (x >= 256 && x <= a->buffer) a->keep = x;
        }
        a->target = a->buffer;
        a->started = 0;
        a->expect = -1;
        a->taken = a->refused = 0;
        a->stat_at = now + 2;
        a->stat_state = 0;
    }
    fprintf(stderr, "alsa: connected %s (%u Hz, %s, %ld frames buffer, %ld kept queued)\n", dev, a->rate,
            a->channels == 1 ? "mono" : "stereo", a->buffer, a->keep);
    return 1;
}

/* One line when it changes (the service keeps it for the app's Audio page): does the device take our frames?
 * A SmartX endpoint only takes them while the AudioManager has its source connected. */
static void status(Alsa *a, double now) {
    int state;
    if (now < a->stat_at) return;
    a->stat_at = now + 2;
    state = a->taken >= 48000 ? 1 : 2; /* 2 s at 48 kHz = 96000 frames when it plays */
    if (state != a->stat_state) {
        a->stat_state = state;
        fprintf(stderr, state == 1 ? "alsa: playing (the device takes the sound)\n"
                                   : "alsa: nobody reads this device (its audio source is not connected)\n");
    }
    a->taken = a->refused = 0;
}

long alsa_room(Alsa *a, const char *dev, double now) {
    long delay = 0;
    int e;
    if (!a->pcm && !open_pcm(a, dev, now)) return -1;
    e = a->delay(a->pcm, &delay);
    if (e < 0) {
        /* underrun (-EPIPE) or suspended: recover; whatever else: reopen */
        if (a->recover(a->pcm, e, 1) < 0) {
            if (now >= a->log_at) {
                fprintf(stderr, "alsa: %s; reconnecting\n", a->error(e));
                a->log_at = now + 2;
            }
            drop_pcm(a, now + 2);
            return -1;
        }
        a->started = 0, a->target = a->buffer, a->expect = -1; /* recovered: stopped again until the buffer is full */
        return a->target * a->div;
    }
    if (delay < 0) delay = 0;
    status(a, now);
    /* the queue went down by itself: the device takes frames, playback runs: from now one period less is kept */
    if (!a->started && a->expect >= 0 && delay < a->expect) a->started = 1, a->target = a->keep;
    a->expect = delay;
    if (!a->started) return delay < a->buffer ? a->buffer * a->div : 0; /* fill it up (a last short write may drop its tail) */
    return delay >= a->target ? 0 : (a->target - delay) * a->div;      /* (never negative: that means "not open") */
}

/* n engine frames (48 kHz stereo) in the device's format into out; returns the device frames. n <= ALSA_CHUNK. */
#define ALSA_CHUNK 1024
static unsigned convert(Alsa *a, const int16_t *in, unsigned n, int16_t *out) {
    unsigned i, m = 0;
    int c, k;
    if (a->div == 1) { /* 48 kHz mono */
        for (i = 0; i < n; i++) out[i] = (int16_t)((in[2 * i] + in[2 * i + 1]) / 2);
        return n;
    }
    for (i = 0; i < n; i++) {
        float l = in[2 * i], r = in[2 * i + 1];
        if (a->channels == 1) a->ring[0][a->ring_pos] = .5f * (l + r);
        else a->ring[0][a->ring_pos] = l, a->ring[1][a->ring_pos] = r;
        a->ring_pos = (a->ring_pos + 1) % ALSA_FIR;
        if ((a->phase ^= 1)) continue; /* every second frame gives one */
        for (c = 0; c < (int)a->channels; c++) {
            float y = 0;
            int p = a->ring_pos; /* the oldest sample */
            for (k = 0; k < ALSA_FIR; k++) {
                y += a->fir[k] * a->ring[c][p];
                if (++p == ALSA_FIR) p = 0;
            }
            out[m * a->channels + c] = (int16_t)lrintf(y > 32767 ? 32767 : y < -32768 ? -32768 : y);
        }
        m++;
    }
    return m;
}

/* one block in the device's format; returns 0 when the device was dropped */
static int write_dev(Alsa *a, const int16_t *buf, unsigned n, double now) {
    long r;
    if (!n) return 1;
    r = a->write(a->pcm, buf, n); /* a short write drops the tail: the queue is near its target anyway */
    if (r >= 0) {
        a->taken += r * a->div;
        if (a->expect >= 0) a->expect += r;
        return 1;
    }
    if (r == -EAGAIN) { /* full and nobody reads (the source not connected): the block goes */
        a->refused++;
        return 1;
    }
    if (a->recover(a->pcm, (int)r, 1) < 0) {
        if (now >= a->log_at) {
            fprintf(stderr, "alsa: %s; reconnecting\n", a->error((int)r));
            a->log_at = now + 2;
        }
        drop_pcm(a, now + 2);
        return 0;
    }
    a->started = 0, a->target = a->buffer, a->expect = -1; /* an underrun recovered: fill up again */
    return 1;
}

void alsa_write(Alsa *a, const int16_t *buf, unsigned n, double now) {
    int16_t tmp[ALSA_CHUNK * 2];
    if (!a->pcm) return;
    if (a->rate == 48000 && a->channels == 2) { /* as it is */
        write_dev(a, buf, n, now);
        return;
    }
    while (n && a->pcm) {
        unsigned part = n < ALSA_CHUNK ? n : ALSA_CHUNK;
        if (!write_dev(a, tmp, convert(a, buf, part, tmp), now)) return;
        buf += 2 * part, n -= part;
    }
}
