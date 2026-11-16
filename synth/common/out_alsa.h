#ifndef ES_ALSA_H
#define ES_ALSA_H
#include <stdint.h>
/* ALSA through dlopen("libasound.so.2"): no ALSA headers or link-time dependency. Signatures as alsa-lib's. */
typedef struct {
    void *lib, *pcm;
    double retry, log_at;
    long target;                  /* frames to keep queued now: the whole buffer until playback runs, then `keep` */
    long buffer, keep;            /* the device's buffer; what is kept queued once it runs (a period less: lower latency) */
    int started;                  /* the device was seen taking frames since the last (re)start: playback runs */
    long expect;                  /* the queue as it would be with nothing taken: the last delay + what was written since */
    long taken, refused;          /* since the last status line: frames the device accepted, blocks it refused (-EAGAIN) */
    double stat_at;               /* when the next status line is due */
    int stat_state;               /* what the last status line said: 0 nothing yet, 1 playing, 2 nobody reads */
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
/* Opens the device when needed (retry every 2 s). Returns the frames that fit before the queue reaches its target,
 * or -1 while the device is not open (the caller then paces by the clock). */
long alsa_room(Alsa *, const char *dev, double now);
void alsa_write(Alsa *, const int16_t *, unsigned, double now);
void alsa_close(Alsa *);
#endif
