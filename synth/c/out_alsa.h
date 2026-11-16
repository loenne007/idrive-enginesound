#ifndef ES_ALSA_H
#define ES_ALSA_H
#include <stdint.h>
/* ALSA through dlopen("libasound.so.2"): no ALSA headers or link-time dependency. Signatures as alsa-lib's. */
typedef struct {
    void *lib, *pcm;
    double retry, log_at;
    long target;                  /* frames to keep queued: the device's whole buffer (playback starts when full) */
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
