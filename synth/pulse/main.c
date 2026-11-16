#define _POSIX_C_SOURCE 200809L
#include "out_alsa.h"
#include "synth.h"
#include "protect.h" /* synth/common: speaker protection */
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#if defined(__SSE__)
#include <xmmintrin.h>
#endif
static volatile sig_atomic_t stop;
static void handler(int sig) {
    (void)sig;
    stop = 1;
}
static double clock_s(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return t.tv_sec + t.tv_nsec * 1e-9;
}
static void pace(double deadline) {
    double d = deadline - clock_s();
    if (d > 0) {
        struct timespec t = {(time_t)d, (long)((d - floor(d)) * 1e9)};
        nanosleep(&t, 0);
    }
}
static void u16(FILE *f, unsigned v) {
    fputc(v & 255, f);
    fputc((v >> 8) & 255, f);
}
static void u32(FILE *f, uint32_t v) {
    u16(f, v & 65535);
    u16(f, v >> 16);
}
static void wav_header(FILE *f, uint32_t frames) {
    fwrite("RIFF", 1, 4, f);
    u32(f, 36 + frames * 4);
    fwrite("WAVEfmt ", 1, 8, f);
    u32(f, 16);
    u16(f, 1);
    u16(f, 2);
    u32(f, SR);
    u32(f, SR * 4);
    u16(f, 4);
    u16(f, 16);
    fwrite("data", 1, 4, f);
    u32(f, frames * 4);
}
/* bounded stdin work: never wait for a complete line, discard oversized lines */
static void controls(Synth *s) {
    static char line[512];
    static unsigned len;
    static int discard;
    char data[4096];
    ssize_t n = read(0, data, sizeof(data));
    ssize_t i;
    for (i = 0; i < n; i++) {
        char c = data[i];
        if (c == '\n') {
            if (!discard) {
                line[len] = 0;
                synth_control(s, line);
            }
            len = 0;
            discard = 0;
        } else if (len < sizeof(line) - 1 && !discard)
            line[len++] = c;
        else
            discard = 1;
    }
}
static void usage(void) {
    fprintf(stderr, "es-synth --out alsa:PCM|wav:FILE|raw [--preset i6|v8|v10|v12] [--volume 0..1] [--demo "
                    "SECONDS|--script FILE]\nes-synth --info | --bench SECONDS | --tone alsa:PCM\n");
}
int main(int argc, char **argv) {
    const char *out = 0, *script = 0;
    double demo = 0, bench = 0, duration = 0, start, origin, quit_at = -1;
    float volume = .8f;
    int p = 0, tone = 0, i;
    FILE *fp = 0, *sf = 0;
    Synth s;
    Alsa alsa = {0};
    Protect prot;
    int16_t buf[BLOCK * 2];
    uint64_t frames = 0;
    double script_at = 0;
    char script_line[512], command[512];
    int pending = 0, paced = 0, alsa_out = 0;
    /* measurement hook: ES_SOLO=1 engine only, 2 turbo only, 3 blow-off only (check.py) */
    if (getenv("ES_SOLO")) es_solo = atoi(getenv("ES_SOLO"));
#if defined(__SSE__)
    _mm_setcsr(_mm_getcsr() | 0x8040);
#endif
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--info")) {
            printf("{\"sample_rate\":48000,\"presets\":[");
            for (p = 0; p < NP; p++)
                printf(
                    "%s{\"id\":\"%s\",\"idle\":%.0f,\"redline\":%.0f,\"cylinders\":%d,\"turbo_default\":%s}",
                    p ? "," : "", presets[p].name, presets[p].idle, presets[p].redline, presets[p].n,
                    presets[p].turbo ? "true" : "false");
            puts("]}");
            return 0;
        }
        if (i + 1 >= argc) {
            usage();
            return 2;
        }
        if (!strcmp(argv[i], "--out"))
            out = argv[++i];
        else if (!strcmp(argv[i], "--preset")) {
            p = preset_find(argv[++i]);
            if (p < 0) return 2;
        } else if (!strcmp(argv[i], "--volume"))
            volume = strtof(argv[++i], 0);
        else if (!strcmp(argv[i], "--demo"))
            demo = strtod(argv[++i], 0);
        else if (!strcmp(argv[i], "--bench"))
            bench = strtod(argv[++i], 0);
        else if (!strcmp(argv[i], "--script"))
            script = argv[++i];
        else if (!strcmp(argv[i], "--tone")) {
            tone = 1;
            out = argv[++i];
        } else {
            usage();
            return 2;
        }
    }
    if (!isfinite(volume) || volume < 0 || volume > 1 || !isfinite(demo) || demo < 0 || !isfinite(bench) ||
        bench < 0)
        return 2;
    if (bench > 0) {
        volatile uint32_t checksum = 0;
        for (p = 0; p < NP; p++) {
            synth_init(&s, p, volume);
            synth_control(&s, "T 1"); /* worst case: the turbo layer on every preset */
            start = clock_s();
            for (frames = 0; frames < (uint64_t)(bench * SR); frames += BLOCK) {
                demo_control(&s, (double)frames / SR, bench);
                synth_render(&s, buf, BLOCK);
                checksum += buf[0];
            }
            printf("%s: %.1fx realtime (%.3fs for %.2fs audio)\n", presets[p].name,
                   bench / (clock_s() - start), clock_s() - start, bench);
        }
        return checksum == 123456789 ? 1 : 0;
    }
    if (!out || (strncmp(out, "alsa:", 5) && strncmp(out, "wav:", 4) && strcmp(out, "raw"))) {
        usage();
        return 2;
    }
    if (tone && strncmp(out, "alsa:", 5)) {
        usage();
        return 2;
    }
    if (!strncmp(out, "wav:", 4)) {
        if (!demo && !script) {
            usage();
            return 2;
        }
        fp = fopen(out + 4, "wb");
        if (!fp) {
            perror(out + 4);
            return 1;
        }
        wav_header(fp, 0);
    }
    if (script) {
        sf = fopen(script, "r");
        if (!sf) {
            perror(script);
            return 1;
        } /* first pass finds end, no heap allocation */
        while (fgets(script_line, sizeof(script_line), sf)) {
            double t;
            if (sscanf(script_line, "%lf", &t) == 1 && isfinite(t) && t >= 0 && t > duration) duration = t;
        }
        rewind(sf);
        duration += 1;
    }
    if (demo) duration = demo;
    if (tone) duration = 10;
    if (duration > 22000) {
        fprintf(stderr, "duration exceeds WAV limit\n");
        return 2;
    }
    alsa_out = !strncmp(out, "alsa:", 5);
    protect_init(&prot);
    synth_init(&s, p, volume);
    signal(SIGINT, handler);
    signal(SIGTERM, handler);
    signal(SIGPIPE, handler);
    fcntl(0, F_SETFL, fcntl(0, F_GETFL) | O_NONBLOCK);
    start = clock_s();
    origin = start;
    frames = 0;
    while (!duration || frames < (uint64_t)(duration * SR)) {
        unsigned n = BLOCK, j;
        double now = (double)frames / SR;
        if (duration && frames + n > (uint64_t)(duration * SR))
            n = (unsigned)((uint64_t)(duration * SR) - frames);
        if (!fp && !demo && !sf) s.time = clock_s() - origin;
        /* live ALSA: the device's queue paces us (no drift against its clock); the wall clock only while it
         * is not open */
        paced = 0;
        if (alsa_out) {
            long room = alsa_room(&alsa, out + 5, clock_s());
            if (room >= 0) {
                if (room < (long)n) {
                    if (!tone) controls(&s);
                    if (stop) break;
                    pace(clock_s() + .002);
                    continue;
                }
                paced = 1;
            }
        }
        if (demo)
            demo_control(&s, now, duration);
        else if (sf) {
            while (1) {
                if (!pending) {
                    while (fgets(script_line, sizeof(script_line), sf)) {
                        int off = 0;
                        if (sscanf(script_line, "%lf %n", &script_at, &off) == 1 && off &&
                            isfinite(script_at) && script_at >= 0) {
                            strncpy(command, script_line + off, sizeof(command) - 1);
                            command[sizeof(command) - 1] = 0;
                            size_t z = strlen(command);
                            while (z && (command[z - 1] == '\n' || command[z - 1] == '\r'))
                                command[--z] = 0;
                            pending = 1;
                            break;
                        }
                    }
                }
                if (!pending || script_at > now) break;
                synth_control(&s, command);
                pending = 0;
            }
        } else if (!tone)
            controls(&s);
        if (stop) s.quit = 1;
        if (s.quit && quit_at < 0) quit_at = s.time;
        if (quit_at >= 0 && s.time - quit_at > .8) break;
        if (tone) {
            for (j = 0; j < n; j++) {
                double t = now + (double)j / SR, phase = fmod(t, 1. / 3),
                       e = phase < .09 ? fmin(1, phase / .008) * fmin(1, (.09 - phase) / .008) : 0;
                float x = (float)(.12 * e * sin(6.283185307 * 880 * t));
                buf[j * 2] = buf[j * 2 + 1] = (int16_t)(32767 * x);
            }
            s.time += n / (double)SR;
        } else
            synth_render(&s, buf, n);
            /* (the tone too) */
        protect_run(&prot, buf, n); /* speaker protection, last stage */
        if (fp) {
            if (fwrite(buf, 4, n, fp) != n) {
                perror("write");
                break;
            }
        } else if (!strcmp(out, "raw")) {
            if (fwrite(buf, 4, n, stdout) != n) break;
        } else
            alsa_write(&alsa, buf, n, clock_s());
        frames += n;
        if (!fp && !paced) {
            double deadline = start + (double)frames / SR;
            double wall = clock_s();
            if (wall - deadline > .1) {
                start = wall - (double)frames / SR;
                deadline = wall;
            }
            pace(deadline);
        }
    }
    alsa_close(&alsa);
    if (sf) fclose(sf);
    if (fp) {
        rewind(fp);
        wav_header(fp, (uint32_t)frames);
        if (fclose(fp)) return 1;
    }
    return 0;
}
