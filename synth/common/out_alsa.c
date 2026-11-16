#define _POSIX_C_SOURCE 200809L
#include "out_alsa.h"
#include <dlfcn.h>
#include <errno.h>
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

static int open_pcm(Alsa *a, const char *dev, double now) {
    unsigned us = 64000;
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
    /* S16_LE = 2, RW_INTERLEAVED = 3, 2 channels, 48 kHz, soft resample on */
    if (e >= 0) e = a->params(a->pcm, 2, 3, 2, 48000, 1, us);
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
        if (a->get_params(a->pcm, &buffer, &period) < 0 || buffer < 512) buffer = (unsigned long)us * 48 / 1000, period = 0;
        a->buffer = (long)buffer;
        a->keep = period >= 256 && buffer >= 3 * period ? (long)(buffer - period) : (long)buffer;
        if (k) {
            long x = strtol(k, 0, 10);
            if (x >= 512 && x <= a->buffer) a->keep = x;
        }
        a->target = a->buffer;
        a->started = 0;
        a->expect = -1;
        a->taken = a->refused = 0;
        a->stat_at = now + 2;
        a->stat_state = 0;
    }
    fprintf(stderr, "alsa: connected %s (%ld frames buffer, %ld kept queued)\n", dev, a->buffer, a->keep);
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
        return a->target;
    }
    if (delay < 0) delay = 0;
    status(a, now);
    /* the queue went down by itself: the device takes frames, playback runs: from now one period less is kept */
    if (!a->started && a->expect >= 0 && delay < a->expect) a->started = 1, a->target = a->keep;
    a->expect = delay;
    if (!a->started) return delay < a->buffer ? a->buffer : 0; /* fill it up (a last short write may drop its tail) */
    return delay >= a->target ? 0 : a->target - delay;          /* (never negative: that means "not open") */
}

void alsa_write(Alsa *a, const int16_t *buf, unsigned n, double now) {
    long r;
    if (!a->pcm) return;
    r = a->write(a->pcm, buf, n); /* a short write drops the tail: the queue is near its target anyway */
    if (r >= 0) {
        a->taken += r;
        if (a->expect >= 0) a->expect += r;
        return;
    }
    if (r == -EAGAIN) { /* full and nobody reads (the source not connected): the block goes */
        a->refused++;
        return;
    }
    if (a->recover(a->pcm, (int)r, 1) < 0) {
        if (now >= a->log_at) {
            fprintf(stderr, "alsa: %s; reconnecting\n", a->error((int)r));
            a->log_at = now + 2;
        }
        drop_pcm(a, now + 2);
    } else
        a->started = 0, a->target = a->buffer, a->expect = -1; /* an underrun recovered: fill up again */
}
