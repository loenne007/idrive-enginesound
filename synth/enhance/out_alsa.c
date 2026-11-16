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
    /* snd_pcm_set_params starts playback once the buffer is full: keep it full (SmartX: 3 x 1024 frames) */
    {
        unsigned long buffer = 0, period = 0;
        if (a->get_params(a->pcm, &buffer, &period) < 0 || buffer < 512) buffer = (unsigned long)us * 48 / 1000;
        a->target = (long)buffer;
    }
    fprintf(stderr, "alsa: connected %s (%ld frames buffered)\n", dev, a->target);
    return 1;
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
        return a->target;
    }
    if (delay < 0) delay = 0;
    return a->target - delay;
}

void alsa_write(Alsa *a, const int16_t *buf, unsigned n, double now) {
    long r;
    if (!a->pcm) return;
    r = a->write(a->pcm, buf, n); /* a short write drops the tail: the queue is near its target anyway */
    if (r >= 0 || r == -EAGAIN) return; /* -EAGAIN: nobody reads (the source not connected): the block goes */
    if (a->recover(a->pcm, (int)r, 1) < 0) {
        if (now >= a->log_at) {
            fprintf(stderr, "alsa: %s; reconnecting\n", a->error((int)r));
            a->log_at = now + 2;
        }
        drop_pcm(a, now + 2);
    }
}
