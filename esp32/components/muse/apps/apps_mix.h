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

#pragma once

/*
 * The mixer of app sound: APPS_MIX_VOICES voices of tones and clips, summed into the 16 kHz mono stream that the voice task, the
 * speaker's only writer, renders a chunk at a time on its idle passes (muse_voice.c, through muse_apps.h's muse_apps_sound_render).
 * The rest of the apps start and stop voices here and return: nothing here waits for the voice task.
 *
 * Any task may call any function. Each holds the mixer's lock (apps_port_mix_lock) only for bounded work: no allocation, no log
 * line, no Lua and no wait for anything but the lock. A render holds it while it synthesizes the frames asked for (the voice task's
 * are one chunk). A render with no voice live, apps_mix_active and apps_mix_stop_all with none take no lock: the voice task asks
 * every pass.
 *
 * A voice is named by an id that is never 0: its slot and a generation. Once the voice has ended, been stopped or been replaced the
 * id is stale: apps_mix_playing says false and apps_mix_stop does nothing, whatever the slot plays since.
 *
 * The clip-lifetime guarantee: a clip's samples are read in place, only under the lock, in a render, and never once
 * apps_mix_stop_owner for its owner (or apps_mix_stop_all) has returned. So its bytes may be freed then, and not before.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPS_MIX_VOICES 4        /* voices that play at once; a start when all play replaces the oldest */
#define APPS_MIX_RATE 16000      /* frames a second: MUSE_AUDIO_RATE, the duplex bus's one rate */
#define APPS_MIX_TONE_PEAK 8192  /* a tone's peak at volume 100: four of them sum to full scale */
#define APPS_MIX_FADE_MS 5       /* a tone's fade in and fade out, and any voice's fade on a stop or a replacement */

typedef enum { APPS_WAVE_SINE, APPS_WAVE_SQUARE, APPS_WAVE_TRIANGLE, APPS_WAVE_SAW, APPS_WAVE_NOISE } apps_wave_t;

/*
 * A tone of ms milliseconds whose pitch slides linearly from hz to to_hz, its phase accumulated a frame at a time. Square, triangle
 * and saw come from the phase, as the sine does (each starts at 0, the square at its top); noise is white, a xorshift32 of the
 * voice's own with a fixed seed, so hz and to_hz don't change it and every noise tone is the same noise. Its peak is
 * APPS_MIX_TONE_PEAK * volume / 100, with a linear fade in and fade out of APPS_MIX_FADE_MS each, or of half its length each for a
 * tone shorter than two fades: it starts and ends at 0. The sound module checks its arguments; the mixer clamps them anyway (hz
 * and to_hz to 1 to APPS_MIX_RATE / 2, ms to at least 1, volume to 0 to 100, and a wave it doesn't know is a sine). owner names
 * whose it is (apps_mix_stop_owner). Returns the voice's id.
 */
uint32_t apps_mix_tone(uint32_t owner, int hz, int to_hz, int ms, apps_wave_t wave, int volume);

/*
 * A clip: samples[0, frames) at volume / 100 of each (volume clamped to 0 to 100), as they are, with no fade but a stop's. With
 * loop it starts again after its last sample, seamlessly, until it is stopped. The samples are the caller's, read in place until
 * the clip-lifetime guarantee above lets them go. NULL samples or 0 frames play nothing: the id returned is already stale.
 */
uint32_t apps_mix_clip(uint32_t owner, const int16_t *samples, size_t frames, int volume, bool loop);

/* Whether id's voice still plays (a voice fading out after a stop or a replacement doesn't). False for 0 and a stale id. */
bool apps_mix_playing(uint32_t id);

/* Stops id's voice: it fades out over APPS_MIX_FADE_MS from where its envelope is, as a replaced voice does, and its id is stale
 * at once. Nothing for 0 or a stale id. */
void apps_mix_stop(uint32_t id);

/* Ends every voice of owner at once, those fading out too, with no fade: their clips' bytes may go as soon as it returns, which is
 * only once the lock is released (the clip-lifetime guarantee). Always takes the lock. */
void apps_mix_stop_owner(uint32_t owner);

/* Ends every voice at once, those fading out too, with no fade: Muse takes the speaker (a talk press, the dark), and nothing of the
 * sound may come out after it, even at the voice task's next render after a turn. */
void apps_mix_stop_all(void);

/* Mixes the next frames into out: the voices' sum, saturated to -32767 to 32767, never wrapped. False, with out all zeros, when no
 * voice was live; true when one was (a voice that ends within the frames leaves zeros after it). Voices advance only as they are
 * rendered. */
bool apps_mix_render(int16_t *out, size_t frames);

/* Whether any voice is live, one fading out included: a render would sound. */
bool apps_mix_active(void);
