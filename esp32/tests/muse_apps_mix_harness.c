/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * The mixer of app sound (apps_mix.c), alone, on the host. tests/test_muse_apps_mix.py writes a script to its stdin, one command a
 * line, and reads one line of answer for each:
 *   tone <owner> <hz> <to_hz> <ms> <wave> <volume>   apps_mix_tone: the id. wave is sine, square, triangle, saw, noise or a number
 *   make <name> <kind> <frames>                       a clip's samples in a heap block of exactly their size: "ok". kind is ramp
 *                                                     (-32767 up to 32767), high (32767), low (-32768) or a number (that level)
 *   samples <name>                                    a made clip's samples
 *   clip <owner> <name> <volume> <loop>               apps_mix_clip of a made clip (- for none): the id
 *   free <name>                                       frees a made clip's block, so AddressSanitizer reports any read of it after
 *   render <frames>                                   apps_mix_render: 1 or 0, then the frames
 *   playing <id>, active                              1 or 0
 *   stop <id>, stop_owner <owner>, stop_all           "ok"
 *   hook_render <frames>, hook_active, hook_cut       muse_apps_sound_render, _active and _cut, answered as above
 *   count                                             how many of the ids handed out so far are playing
 *   locks                                             how many times the mixer's lock was taken since the last ask
 *   race <rounds>                                     a render thread against stop_owner and free on this one: "ok"
 *   clock <us>                                        a bench build's (CONFIG_MUSE_BENCH): esp_timer_get_time() is us from now: "ok".
 *                                                     Its log lines go to stderr, "@log <tag> <line>", and one under the lock aborts
 * An id is a number, or #k for the id that the k-th start of the run (from 0) returned.
 *
 * The mixer's lock (apps_port.h) is a pthread mutex that aborts on a second take by its holder and on a give by a thread that
 * doesn't hold it, and every command checks that it is free again once the call has returned.
 */

#include <pthread.h>
#include <stdarg.h>
#include <sched.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "apps_mix.h"
#include "apps_port.h"
#include "muse_apps.h"
#include "sdkconfig.h"

/* ---- The mixer's lock ---- */

static pthread_mutex_t s_mutex = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local bool t_holding;
static atomic_int s_held;
static atomic_uint s_takes;

static void die(const char *why) {
    fprintf(stderr, "%s\n", why);
    abort();
}

void apps_port_mix_lock(void) {
    if (t_holding) {
        die("the mixer's lock was taken again by its holder");
    }
    pthread_mutex_lock(&s_mutex);
    t_holding = true;
    atomic_fetch_add(&s_held, 1);
    atomic_fetch_add(&s_takes, 1);
}

void apps_port_mix_unlock(void) {
    if (!t_holding) {
        die("the mixer's lock was given back by a thread that doesn't hold it");
    }
    t_holding = false;
    atomic_fetch_sub(&s_held, 1);
    pthread_mutex_unlock(&s_mutex);
}

#if CONFIG_MUSE_BENCH
/* ---- The bench's clock and log (esp_timer.h and esp_log.h as the test stages them) ---- */

static int64_t s_now_us = 1000000;

int64_t esp_timer_get_time(void) {
    return s_now_us;
}

void fake_esp_log(const char *tag, const char *fmt, ...) {
    if (t_holding) {
        die("a log line under the mixer's lock");
    }
    char text[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    fprintf(stderr, "@log %s %s\n", tag, text);
}
#endif

/* ---- Clips and ids ---- */

#define CLIPS 16
#define IDS 20000

static struct {
    char name[32];
    int16_t *samples;
    size_t frames;
} s_clips[CLIPS];

static uint32_t s_ids[IDS];
static size_t s_started;

static int16_t **clip_named(const char *name, size_t *frames) {
    for (size_t i = 0; i < CLIPS; i++) {
        if (s_clips[i].name[0] != '\0' && strcmp(s_clips[i].name, name) == 0) {
            *frames = s_clips[i].frames;
            return &s_clips[i].samples;
        }
    }
    die("no clip of that name");
    return NULL;
}

static uint32_t started(uint32_t id) {
    if (s_started == IDS) {
        die("more starts than the harness keeps");
    }
    if (id == 0) {
        die("a start returned the id 0");
    }
    s_ids[s_started++] = id;
    return id;
}

static uint32_t id_arg(const char *text) {
    if (text[0] == '#') {
        size_t k = strtoul(text + 1, NULL, 10);
        if (k >= s_started) {
            die("#k names a start that hasn't happened");
        }
        return s_ids[k];
    }
    return (uint32_t)strtoul(text, NULL, 10);
}

static int wave_arg(const char *text) {
    static const char *const NAMES[] = {"sine", "square", "triangle", "saw", "noise"};
    static const int WAVES[] = {APPS_WAVE_SINE, APPS_WAVE_SQUARE, APPS_WAVE_TRIANGLE, APPS_WAVE_SAW, APPS_WAVE_NOISE};
    for (size_t i = 0; i < sizeof NAMES / sizeof NAMES[0]; i++) {
        if (strcmp(text, NAMES[i]) == 0) {
            return WAVES[i];
        }
    }
    return atoi(text);
}

/* ---- Rendering ---- */

static void print_render(bool (*render)(int16_t *, size_t), size_t frames) {
    int16_t *out = malloc((frames ? frames : 1) * sizeof *out);  /* exactly the frames asked for: a write past them is reported */
    if (out == NULL) {
        die("out of memory");
    }
    for (size_t i = 0; i < frames; i++) {
        out[i] = 0x5a5a;  /* not 0: a render that says silence has to have written the zeros */
    }
    bool live = render(out, frames);
    printf("%d", live ? 1 : 0);
    for (size_t i = 0; i < frames; i++) {
        if (!live && out[i] != 0) {
            die("a render that said no voice was live left a frame that isn't 0");
        }
        printf(" %d", out[i]);
    }
    putchar('\n');
    free(out);
}

/* ---- The race: the clip-lifetime guarantee against a render on another thread ---- */

static atomic_bool s_race_done;
static atomic_ulong s_renders;

/* The voice task's renders, as fast as the lock allows: a pause after each (the device's is the 20 ms read), short, or this thread
 * would take the lock again before the other could (a pthread mutex isn't fair). */
static void *render_loop(void *arg) {
    (void)arg;
    int16_t out[320];
    const struct timespec pause = {.tv_nsec = 20000};
    while (!atomic_load(&s_race_done)) {
        apps_mix_render(out, sizeof out / sizeof out[0]);
        atomic_fetch_add(&s_renders, 1);
        nanosleep(&pause, NULL);
    }
    return NULL;
}

static void wait_renders(unsigned long n) {
    unsigned long from = atomic_load(&s_renders);
    while (atomic_load(&s_renders) - from < n) {
        sched_yield();
    }
}

/* Each round plays a looping clip of a fresh heap block for owner 7, lets the other thread render it, sometimes stops it first (so
 * a fading tail reads it), then stops the owner and frees the block at once: a render that read it after stop_owner returned is a
 * use after free, which AddressSanitizer reports. */
static void race(int rounds) {
    pthread_t thread;
    atomic_store(&s_race_done, false);
    if (pthread_create(&thread, NULL, render_loop, NULL) != 0) {
        die("no thread");
    }
    for (int r = 0; r < rounds; r++) {
        size_t frames = 100 + (size_t)(r % 7) * 37;
        int16_t *samples = malloc(frames * sizeof *samples);
        if (samples == NULL) {
            die("out of memory");
        }
        for (size_t i = 0; i < frames; i++) {
            samples[i] = (int16_t)(1000 + (int)i);
        }
        uint32_t id = apps_mix_clip(7, samples, frames, 100, true);
        uint32_t other = apps_mix_tone(8, 440, 880, 50, APPS_WAVE_SQUARE, 50);
        wait_renders(1 + (unsigned long)(r % 3));
        if (r % 2 == 1) {
            apps_mix_stop(id);  /* its fade reads the samples too */
        }
        if (r % 5 == 0) {
            sched_yield();
        }
        apps_mix_stop_owner(7);
        free(samples);
        if (apps_mix_playing(id)) {
            die("owner 7's clip still plays after stop_owner");
        }
        apps_mix_stop(other);
    }
    atomic_store(&s_race_done, true);
    pthread_join(thread, NULL);
    puts("ok");
}

/* ---- The script ---- */

int main(void) {
    char line[256];
    while (fgets(line, sizeof line, stdin) != NULL) {
        char *argv[8] = {0};
        int argc = 0;
        for (char *word = strtok(line, " \t\r\n"); word != NULL && argc < 8; word = strtok(NULL, " \t\r\n")) {
            argv[argc++] = word;
        }
        if (argc == 0) {
            continue;
        }
        const char *cmd = argv[0];
        if (strcmp(cmd, "tone") == 0 && argc == 7) {
            uint32_t id = apps_mix_tone((uint32_t)strtoul(argv[1], NULL, 10), atoi(argv[2]), atoi(argv[3]), atoi(argv[4]),
                                        (apps_wave_t)wave_arg(argv[5]), atoi(argv[6]));
            printf("%u\n", (unsigned)started(id));
        } else if (strcmp(cmd, "make") == 0 && argc == 4) {
            size_t slot = 0;
            while (slot < CLIPS && s_clips[slot].name[0] != '\0') {
                slot++;
            }
            if (slot == CLIPS || strlen(argv[1]) >= sizeof s_clips[0].name) {
                die("no room for the clip");
            }
            size_t frames = strtoul(argv[3], NULL, 10);
            int16_t *samples = malloc((frames ? frames : 1) * sizeof *samples);
            if (samples == NULL) {
                die("out of memory");
            }
            for (size_t i = 0; i < frames; i++) {
                if (strcmp(argv[2], "ramp") == 0) {
                    samples[i] = (int16_t)(frames > 1 ? -32767 + (int32_t)((int64_t)i * 65534 / (int64_t)(frames - 1)) : 0);
                } else if (strcmp(argv[2], "high") == 0) {
                    samples[i] = 32767;
                } else if (strcmp(argv[2], "low") == 0) {
                    samples[i] = -32768;
                } else {
                    samples[i] = (int16_t)atoi(argv[2]);
                }
            }
            strcpy(s_clips[slot].name, argv[1]);
            s_clips[slot].samples = samples;
            s_clips[slot].frames = frames;
            puts("ok");
        } else if (strcmp(cmd, "samples") == 0 && argc == 2) {
            size_t frames;
            int16_t *samples = *clip_named(argv[1], &frames);
            printf("%zu", frames);
            for (size_t i = 0; i < frames; i++) {
                printf(" %d", samples[i]);
            }
            putchar('\n');
        } else if (strcmp(cmd, "clip") == 0 && argc == 5) {
            size_t frames = 0;
            const int16_t *samples = strcmp(argv[2], "-") == 0 ? NULL : *clip_named(argv[2], &frames);
            uint32_t id = apps_mix_clip((uint32_t)strtoul(argv[1], NULL, 10), samples, frames, atoi(argv[3]), atoi(argv[4]) != 0);
            printf("%u\n", (unsigned)started(id));
        } else if (strcmp(cmd, "free") == 0 && argc == 2) {
            size_t frames;
            int16_t **samples = clip_named(argv[1], &frames);
            free(*samples);
            *samples = NULL;
            puts("ok");
        } else if (strcmp(cmd, "render") == 0 && argc == 2) {
            print_render(apps_mix_render, strtoul(argv[1], NULL, 10));
        } else if (strcmp(cmd, "hook_render") == 0 && argc == 2) {
            print_render(muse_apps_sound_render, strtoul(argv[1], NULL, 10));
        } else if (strcmp(cmd, "playing") == 0 && argc == 2) {
            printf("%d\n", apps_mix_playing(id_arg(argv[1])) ? 1 : 0);
        } else if (strcmp(cmd, "active") == 0 && argc == 1) {
            printf("%d\n", apps_mix_active() ? 1 : 0);
        } else if (strcmp(cmd, "hook_active") == 0 && argc == 1) {
            printf("%d\n", muse_apps_sound_active() ? 1 : 0);
        } else if (strcmp(cmd, "stop") == 0 && argc == 2) {
            apps_mix_stop(id_arg(argv[1]));
            puts("ok");
        } else if (strcmp(cmd, "stop_owner") == 0 && argc == 2) {
            apps_mix_stop_owner((uint32_t)strtoul(argv[1], NULL, 10));
            puts("ok");
        } else if (strcmp(cmd, "stop_all") == 0 && argc == 1) {
            apps_mix_stop_all();
            puts("ok");
        } else if (strcmp(cmd, "hook_cut") == 0 && argc == 1) {
            muse_apps_sound_cut();
            puts("ok");
        } else if (strcmp(cmd, "count") == 0 && argc == 1) {
            unsigned n = 0;
            for (size_t i = 0; i < s_started; i++) {
                n += apps_mix_playing(s_ids[i]) ? 1 : 0;
            }
            printf("%u\n", n);
        } else if (strcmp(cmd, "locks") == 0 && argc == 1) {
            printf("%u\n", atomic_exchange(&s_takes, 0));
        } else if (strcmp(cmd, "race") == 0 && argc == 2) {
            race(atoi(argv[1]));
#if CONFIG_MUSE_BENCH
        } else if (strcmp(cmd, "clock") == 0 && argc == 2) {
            s_now_us = strtoll(argv[1], NULL, 10);
            puts("ok");
#endif
        } else {
            die("a command the harness doesn't know, or the wrong number of words");
        }
        if (atomic_load(&s_held) != 0) {
            die("the mixer's lock was still held when the call returned");
        }
        fflush(stdout);
    }
    for (size_t i = 0; i < CLIPS; i++) {
        free(s_clips[i].samples);
    }
    return 0;
}
