#ifndef ES_ALSA_H
#define ES_ALSA_H
#include <stdint.h>
/* ALSA through dlopen("libasound.so.2"): no ALSA headers or link-time dependency. Signatures as alsa-lib's.
 *
 * The engines render 48 kHz stereo. A SmartX endpoint has the format in its name (..._CF_48_STEREO, ..._CF_24_MONO:
 * the car's prompt and overlay channels are 24 kHz mono): the device is opened in that format and the blocks are
 * converted here (mono = (L + R) / 2; 24 kHz through a low-pass, every second sample). Frame counts in this
 * interface are always the engine's (48 kHz). */
#define ALSA_FIR 47 /* taps of the 2:1 decimator's low-pass */
typedef struct {
    void *lib, *pcm;
    unsigned rate, channels;      /* the device's format: 48000 or 24000 Hz, 2 or 1 channels */
    int div;                      /* engine frames per device frame: 1, or 2 at 24 kHz */
    float fir[ALSA_FIR], ring[2][ALSA_FIR]; /* the decimator: coefficients, the last input samples per channel */
    int ring_pos, phase;
    double retry, log_at;
    /* (the queue below is counted in the device's frames) */
    long target;                  /* frames to keep queued now: the whole buffer until playback runs, then `keep` */
    long buffer, keep;            /* the device's buffer; what is kept queued once it runs (a period less: lower latency) */
    /* dropouts: the device reported an underrun, or its queue was found empty while it played */
    long xruns, empties, low;     /* both counts since the device was opened; the smallest queue seen while it played */
    int dry;                      /* the queue is empty right now (counted once) */
    long rep_n;                   /* the dropouts the last report line said */
    double rep_at, run_at;        /* when the next report line may come; when playback first ran (0: not yet) */
    int rep_queue;                /* the one line with the queue's range was written */
    int started;                  /* the device was seen taking frames since the last (re)start: playback runs */
    long expect;                  /* the queue as it would be with nothing taken: the last delay + what was written since */
    long taken, refused;          /* since the last status line: engine frames the device accepted, blocks it refused (-EAGAIN) */
    double stat_at;               /* when the next status line is due */
    int stat_state;               /* what the last status line said: 0 nothing yet, 1 playing, 2 nobody reads */
    int rt, rt_errno;             /* real-time scheduling: 0 not asked yet, 1 on, -1 refused (why) */
    double last, gap;             /* the last look at the queue while it played; the longest time between two (s) */
    int (*open)(void **, const char *, int, int);
    int (*params)(void *, int, int, unsigned, unsigned, int, unsigned);
    long (*write)(void *, const void *, unsigned long);   /* snd_pcm_sframes_t */
    int (*recover)(void *, int, int);
    int (*close)(void *);
    int (*drop)(void *);
    int (*delay)(void *, long *);                          /* int snd_pcm_delay(snd_pcm_t *, snd_pcm_sframes_t *) */
    const char *(*error)(int);
    int (*get_params)(void *, unsigned long *, unsigned long *); /* buffer, period size */
} Alsa;
/* Opens the device when needed (retry every 2 s). Returns the engine frames (48 kHz) that fit before the queue
 * reaches its target, or -1 while the device is not open (the caller then paces by the clock). */
long alsa_room(Alsa *, const char *dev, double now);
/* a block of 48 kHz stereo frames (interleaved S16) */
void alsa_write(Alsa *, const int16_t *, unsigned, double now);
void alsa_close(Alsa *);
#endif
