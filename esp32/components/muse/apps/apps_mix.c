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

#include "apps_mix.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_attr.h"
#include "sdkconfig.h"
#if CONFIG_MUSE_BENCH
#include "esp_log.h"
#include "esp_timer.h"
#endif

#include "apps_port.h"
#include "muse_apps.h"

/*
 * The mixer (apps_mix.h has the contract). A slot holds a voice that plays, with its id, and a tail: a voice fading out after a
 * stop or a replacement, which has no id and lives out its fade (APPS_MIX_FADE_MS) beside the slot's next voice. A render sums
 * them all, a block of frames at a time on the caller's stack, and saturates the sum.
 *
 * The synthesis is integer but for the sine's polynomial: the phase is 48 bits of a cycle, advanced a frame at a time by an
 * increment that the slide changes linearly each frame; gains are Q15. A stop's fade starts from the gain the voice had and never
 * exceeds the voice's own envelope, so no fade rises, even one that begins during a fade in.
 *
 * Where it lives. The voices are plain memory that only the lock guards, in PSRAM (EXT_RAM_BSS_ATTR: empty unless the build lets
 * statics go there, which the apps build does). s_active, read without the lock, is an atomic and stays in internal RAM, as the
 * lock does (apps_port_idf.c). It is written under the lock and is true exactly when a voice or a tail is live, so the voice task's
 * pass with nothing to play takes no lock.
 *
 * The bench's measurements (CONFIG_MUSE_BENCH only), on the muse_apps tag, each read under the lock and logged once it is released:
 * - "sound start <ms> ms": a voice's start request (the call of apps_mix_tone or apps_mix_clip) to the render of its first chunk, once
 *   for each voice. What the I2S queue adds after the render is the board's to measure.
 * - "sound render <us> us (<n> frames, <v> voices)": how long a render held the lock, once for each number of voices (and tails) it
 *   rendered, so that the most the voice task, or a task that waits for the lock, can be held up shows on the board.
 * - "sound cut <n> voices (<ms> ms fade)": apps_mix_stop_all stopped n voices (a ring, a talk press, the app going behind or
 *   covered), those that had rendered fading out, logged at the next render.
 */

#define FRAMES_PER_MS (APPS_MIX_RATE / 1000)
#define FADE_FRAMES (APPS_MIX_FADE_MS * FRAMES_PER_MS)
#define UNITY 32768                                 /* a gain of 1, Q15 */
#define FULL 32767                                  /* a wave's top, and the sum's: -32768 never comes out */
#define PHASE_SHIFT 16                              /* the phase's bits under the 32 that the waves read */
#define PHASE_MASK ((UINT64_C(1) << (32 + PHASE_SHIFT)) - 1)
#define HZ_MAX (APPS_MIX_RATE / 2)
#define TONE_MS_MAX (1 << 20)                       /* 17 minutes: a tone's frames, and its slide's arithmetic, fit */
#define NOISE_SEED 0x2545f491u
#define SLOT_BITS 2
#define GEN_MAX (UINT32_MAX >> SLOT_BITS)
#define BLOCK 64                                    /* frames summed at a time: 256 bytes of the caller's stack */

#if CONFIG_MUSE_BENCH
static const char *TAG = "muse_apps";
#endif

_Static_assert(APPS_MIX_VOICES == 1 << SLOT_BITS, "an id keeps its slot in SLOT_BITS bits");
_Static_assert(FADE_FRAMES > 1, "a fade has frames");

typedef enum { FREE, TONE, CLIP } kind_t;

typedef struct {
    uint8_t kind;              /* kind_t */
    uint8_t wave;              /* a tone's apps_wave_t */
    bool loop;                 /* a clip's */
    uint32_t id;               /* a voice that plays: its id; a tail: 0 */
    uint32_t owner;
    uint32_t started;          /* M.starts when it started: the oldest is replaced */
    int32_t amp;               /* a tone's peak; a clip's volume, 0 to 100 */
    uint32_t t;                /* frames rendered */
    /* A tone. */
    uint32_t len;              /* its frames */
    uint32_t fade;             /* each fade's frames */
    uint64_t phase;            /* a cycle is 2^48 */
    int64_t inc, dinc;         /* the phase's step a frame, and the slide's change of it a frame */
    uint32_t noise;            /* xorshift32 */
    /* A clip. */
    const int16_t *samples;
    size_t frames, pos;
    /* A tail: frames left of its fade, and the gain the fade began at. */
    uint32_t kill;
    int32_t kill_gain;
#if CONFIG_MUSE_BENCH
    int64_t asked_us;          /* esp_timer_get_time() of its start request, until its first render: then 0 */
#endif
} voice_t;

typedef struct {
    voice_t play, tail;
} slot_t;

static EXT_RAM_BSS_ATTR struct {
    slot_t slot[APPS_MIX_VOICES];
    uint32_t gen;              /* the last generation handed out, 1 to GEN_MAX */
    uint32_t starts;           /* starts so far */
#if CONFIG_MUSE_BENCH
    uint32_t held_logged;      /* bit n: a render of n voices and tails has logged its hold */
    uint32_t cut;              /* voices apps_mix_stop_all stopped since the last render */
#endif
} M;
static atomic_bool s_active;

static int clamp(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static int32_t clamp_full(int32_t v) {
    return v > FULL ? FULL : v < -FULL ? -FULL : v;
}

static bool any_live(void) {
    for (int i = 0; i < APPS_MIX_VOICES; i++) {
        if (M.slot[i].play.kind != FREE || M.slot[i].tail.kind != FREE) {
            return true;
        }
    }
    return false;
}

/* Under the lock, after every change: s_active is true exactly when there is something to render. */
static void publish(void) {
    atomic_store(&s_active, any_live());
}

/* ---- Synthesis ---- */

/* sin(2 pi p / 2^32) in Q15: folded into a quarter cycle, then an odd polynomial (its error is under 4e-6). */
static int32_t sine(uint32_t p) {
    bool negative = (p & 0x80000000u) != 0;
    uint32_t h = p & 0x7fffffffu;
    if (h > 0x40000000u) {
        h = 0x80000000u - h;
    }
    float x = (float)h * (1.57079632679f / 1073741824.0f);
    float x2 = x * x;
    float s = x * (1.0f + x2 * (-1.0f / 6 + x2 * (1.0f / 120 + x2 * (-1.0f / 5040 + x2 * (1.0f / 362880)))));
    int32_t q = (int32_t)(s * FULL + 0.5f);
    q = q > FULL ? FULL : q;
    return negative ? -q : q;
}

/* The next frame's wave, in Q15, from the voice's phase (or its noise). */
static int32_t wave_of(voice_t *v) {
    uint32_t p = (uint32_t)(v->phase >> PHASE_SHIFT);
    switch (v->wave) {
    case APPS_WAVE_SQUARE:
        return p < 0x80000000u ? FULL : -FULL;
    case APPS_WAVE_TRIANGLE: {
        uint32_t w = p + 0x40000000u;  /* a quarter on, so it starts at 0 rising */
        uint32_t y = w < 0x80000000u ? w : 0u - w;
        return clamp_full((int32_t)(y >> 15) - UNITY);
    }
    case APPS_WAVE_SAW:
        return clamp_full((int32_t)(p >> 16) - (p >= 0x80000000u ? 65536 : 0));
    case APPS_WAVE_NOISE: {
        uint32_t x = v->noise;
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        v->noise = x;
        return clamp_full((int32_t)(x >> 16) - UNITY);
    }
    default:
        return sine(p);
    }
}

/* A tone's envelope at frame t, Q15: up from 0 over its fade in, down to 0 at its last frame. */
static int32_t tone_gain(const voice_t *v, uint32_t t) {
    uint32_t left = v->len - 1 - t;
    uint32_t edge = t < left ? t : left;
    return edge >= v->fade ? UNITY : (int32_t)(edge * UNITY / v->fade);
}

static void end(voice_t *v) {
    v->kind = FREE;
    v->id = 0;
}

/* Adds the voice's next n frames, or as many as it has, into acc. */
static void render_voice(voice_t *v, int32_t *acc, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int32_t s, gain;
        bool over;
        if (v->kind == TONE) {
            s = wave_of(v) * v->amp / UNITY;
            gain = tone_gain(v, v->t);
            v->phase = (v->phase + (uint64_t)v->inc) & PHASE_MASK;
            v->inc += v->dinc;
            over = v->t + 1 >= v->len;
        } else {
            s = v->samples[v->pos] * v->amp / 100;
            gain = UNITY;
            if (++v->pos == v->frames) {
                v->pos = 0;
            }
            over = v->pos == 0 && !v->loop;
        }
        if (v->kill > 0) {
            int32_t fading = v->kill_gain * (int32_t)v->kill / FADE_FRAMES;
            gain = fading < gain ? fading : gain;
            over = over || v->kill == 1;
            v->kill--;
        }
        acc[i] += gain == UNITY ? s : s * gain / UNITY;
        v->t++;
        if (over) {
            end(v);
            return;
        }
    }
}

/* ---- Voices ---- */

/* The slot's voice fades out as its tail (one that never sounded just goes), in place of any tail before it. */
static void fade_out(slot_t *slot) {
    voice_t *v = &slot->play;
    if (v->t > 0) {
        slot->tail = *v;
        slot->tail.id = 0;
        slot->tail.kill = FADE_FRAMES;
        slot->tail.kill_gain = v->kind == TONE ? tone_gain(v, v->t) : UNITY;
    }
    end(v);
}

/* A slot for a new voice: an empty one, else one whose voice has ended (its tail fades on), else the oldest voice's, which fades. */
static int take_slot(void) {
    for (int i = 0; i < APPS_MIX_VOICES; i++) {
        if (M.slot[i].play.kind == FREE && M.slot[i].tail.kind == FREE) {
            return i;
        }
    }
    for (int i = 0; i < APPS_MIX_VOICES; i++) {
        if (M.slot[i].play.kind == FREE) {
            return i;
        }
    }
    int oldest = 0;
    for (int i = 1; i < APPS_MIX_VOICES; i++) {
        if (M.starts - M.slot[i].play.started > M.starts - M.slot[oldest].play.started) {
            oldest = i;
        }
    }
    fade_out(&M.slot[oldest]);
    return oldest;
}

static uint32_t next_id(int slot) {
    M.gen = M.gen >= GEN_MAX ? 1 : M.gen + 1;
    return (M.gen << SLOT_BITS) | (uint32_t)slot;
}

/* Puts *v in a slot as a voice that plays, under the lock: its id. */
static uint32_t start_locked(voice_t *v) {
    int slot = take_slot();
    v->id = next_id(slot);
    v->started = M.starts++;
    M.slot[slot].play = *v;
    publish();
    return v->id;
}

static voice_t *voice_of(uint32_t id) {
    voice_t *v = &M.slot[id & (APPS_MIX_VOICES - 1)].play;
    return id != 0 && v->kind != FREE && v->id == id ? v : NULL;
}

uint32_t apps_mix_tone(uint32_t owner, int hz, int to_hz, int ms, apps_wave_t wave, int volume) {
    voice_t v = {
        .kind = TONE,
        .wave = (uint8_t)((int)wave >= 0 && (int)wave <= APPS_WAVE_NOISE ? wave : APPS_WAVE_SINE),
        .owner = owner,
        .amp = APPS_MIX_TONE_PEAK * clamp(volume, 0, 100) / 100,
        .len = (uint32_t)clamp(ms, 1, TONE_MS_MAX) * FRAMES_PER_MS,
        .noise = NOISE_SEED,
#if CONFIG_MUSE_BENCH
        .asked_us = esp_timer_get_time(),
#endif
    };
    v.fade = v.len / 2 < FADE_FRAMES ? v.len / 2 : FADE_FRAMES;
    int64_t from = ((int64_t)clamp(hz, 1, HZ_MAX) << (32 + PHASE_SHIFT)) / APPS_MIX_RATE;
    int64_t to = ((int64_t)clamp(to_hz, 1, HZ_MAX) << (32 + PHASE_SHIFT)) / APPS_MIX_RATE;
    v.inc = from;
    v.dinc = (to - from) / (int64_t)v.len;
    apps_port_mix_lock();
    uint32_t id = start_locked(&v);
    apps_port_mix_unlock();
    return id;
}

uint32_t apps_mix_clip(uint32_t owner, const int16_t *samples, size_t frames, int volume, bool loop) {
    voice_t v = {
        .kind = CLIP,
        .loop = loop,
        .owner = owner,
        .amp = clamp(volume, 0, 100),
        .samples = samples,
        .frames = frames,
#if CONFIG_MUSE_BENCH
        .asked_us = esp_timer_get_time(),
#endif
    };
    apps_port_mix_lock();
    /* Nothing to play: a generation of its own, which no slot will ever hold, so the id is stale from the start. */
    uint32_t id = samples == NULL || frames == 0 ? next_id(0) : start_locked(&v);
    apps_port_mix_unlock();
    return id;
}

bool apps_mix_playing(uint32_t id) {
    apps_port_mix_lock();
    bool playing = voice_of(id) != NULL;
    apps_port_mix_unlock();
    return playing;
}

void apps_mix_stop(uint32_t id) {
    apps_port_mix_lock();
    if (voice_of(id) != NULL) {
        fade_out(&M.slot[id & (APPS_MIX_VOICES - 1)]);
        publish();
    }
    apps_port_mix_unlock();
}

void apps_mix_stop_owner(uint32_t owner) {
    apps_port_mix_lock();
    for (int i = 0; i < APPS_MIX_VOICES; i++) {
        if (M.slot[i].play.kind != FREE && M.slot[i].play.owner == owner) {
            end(&M.slot[i].play);
        }
        if (M.slot[i].tail.kind != FREE && M.slot[i].tail.owner == owner) {
            end(&M.slot[i].tail);
        }
    }
    publish();
    apps_port_mix_unlock();
}

void apps_mix_stop_all(void) {
    if (!atomic_load(&s_active)) {
        return;
    }
    apps_port_mix_lock();
    for (int i = 0; i < APPS_MIX_VOICES; i++) {
        if (M.slot[i].play.kind != FREE) {
            fade_out(&M.slot[i]);
#if CONFIG_MUSE_BENCH
            M.cut++;
#endif
        }
    }
    publish();
    apps_port_mix_unlock();
}

bool apps_mix_render(int16_t *out, size_t frames) {
    if (!atomic_load(&s_active)) {
        if (frames > 0) {
            memset(out, 0, frames * sizeof *out);
        }
        return false;
    }
    apps_port_mix_lock();
    bool live = any_live();
#if CONFIG_MUSE_BENCH
    /* Read under the lock, logged after it (no log line under it). */
    int64_t began_us = esp_timer_get_time();
    int started_ms[APPS_MIX_VOICES], n_started = 0, rendering = 0, cut = (int)M.cut;
    M.cut = 0;
    for (int i = 0; i < APPS_MIX_VOICES; i++) {
        voice_t *v = &M.slot[i].play;
        rendering += (v->kind != FREE ? 1 : 0) + (M.slot[i].tail.kind != FREE ? 1 : 0);
        if (v->kind != FREE && v->asked_us != 0) {
            started_ms[n_started++] = (int)((began_us - v->asked_us + 500) / 1000);
            v->asked_us = 0;
        }
    }
#endif
    for (size_t done = 0; done < frames;) {
        int32_t acc[BLOCK] = {0};
        size_t n = frames - done < BLOCK ? frames - done : BLOCK;
        for (int i = 0; i < APPS_MIX_VOICES; i++) {
            if (M.slot[i].play.kind != FREE) {
                render_voice(&M.slot[i].play, acc, n);
            }
            if (M.slot[i].tail.kind != FREE) {
                render_voice(&M.slot[i].tail, acc, n);
            }
        }
        for (size_t i = 0; i < n; i++) {
            out[done + i] = (int16_t)clamp_full(acc[i]);
        }
        done += n;
    }
    publish();
#if CONFIG_MUSE_BENCH
    int held_us = (int)(esp_timer_get_time() - began_us);
    bool log_held = rendering > 0 && (M.held_logged & (1u << rendering)) == 0;
    M.held_logged |= log_held ? 1u << rendering : 0;
#endif
    apps_port_mix_unlock();
#if CONFIG_MUSE_BENCH
    for (int i = 0; i < n_started; i++) {
        ESP_LOGI(TAG, "sound start %d ms", started_ms[i]);
    }
    if (cut > 0) {
        ESP_LOGI(TAG, "sound cut %d voices (%d ms fade)", cut, APPS_MIX_FADE_MS);
    }
    if (log_held) {
        ESP_LOGI(TAG, "sound render %d us (%d frames, %d voices)", held_us, (int)frames, rendering);
    }
#endif
    return live;
}

bool apps_mix_active(void) {
    return atomic_load(&s_active);
}

/* ---- The voice task's hooks (muse_apps.h) ---- */

bool muse_apps_sound_render(int16_t *out, size_t frames) {
    return apps_mix_render(out, frames);
}

bool muse_apps_sound_active(void) {
    return apps_mix_active();
}

void muse_apps_sound_cut(void) {
    apps_mix_stop_all();
}
