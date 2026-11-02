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
 * The apps' `sound` module: a chirp, the ring's stop, and an app's own tones and clips, each with a handle. Portable C, like the runtime
 * it belongs to (apps_rt.h): the calls run on the app task and hand each sound to the platform through the port (apps_port.h's
 * apps_port_sound_*), which on the gadget is the mixer (apps_mix.h) that the voice task plays, and in apphost a line of events. This
 * module never calls the mixer itself.
 *
 * The calls (apps_sound_funcs[]):
 *   sound.chirp()            one chirp at the gadget's volume (the voice task's, as the ring's are). Needs the "sound" permission:
 *                            `sound: add "sound" to "permissions" in manifest.json`
 *   sound.stop()             ends the ring of an alarm that is going, whichever app's it is; no permission, and nothing happens when
 *                            none is going
 *   sound.tone{hz, ms, to_hz, wave, volume}
 *                            a tone of ms milliseconds (APPS_SOUND_MS_MIN to APPS_SOUND_MS_MAX, APPS_SOUND_MS_DEFAULT unless given)
 *                            whose pitch slides from hz to to_hz (each APPS_SOUND_HZ_MIN to APPS_SOUND_HZ_MAX; hz is required, to_hz
 *                            is hz unless given), of the wave apps_sound_waves names (sine unless given), at volume 0 to
 *                            APPS_SOUND_VOLUME_MAX (that unless given), a share of the gadget's volume. Needs the "sound" permission
 *   sound.play(path, {volume = 100, loop = false})
 *                            the package's .wav at path (apps_rt_sound), at volume (as tone's), from its start again at its end with
 *                            loop, until it is stopped. Needs the "sound" permission. Any other path, a file of another type
 *                            included, is `sound.play: '<path>' is not one of this app's sounds (manifest.json "files")`
 * The arguments follow the ui module's property rules: a number is rounded, halves away from zero, and clamped to its range (an
 * infinity too); a value of the wrong type, or NaN, is an error that says what it must be; a key the call doesn't take is an error that
 * lists those it does, found before any value is looked at; and every error starts with the call's name. The permission comes first.
 *
 * The handle (APPS_SOUND_MT, protected as every metatable C hands out: X17), what tone and play return: the voice the port gave, and the
 * app's count of its sounds.
 *   h:stop()                 stops the sound, which fades out; nothing for one that has ended, was replaced (a start while every voice
 *                            plays replaces the oldest: APPS_MIX_VOICES), was done from the start or was stopped already
 *   tostring(h)              "sound#<n>", n counting the app's sounds from 1, those done from the start included
 *   any other key            `sound: unknown method '<key>' (valid: stop)`; a field set is `sound: a sound has no fields: h:stop()
 *                            stops it`
 *
 * Sound only in front. While the app isn't shown (apps_rt_is_shown) or the screen is dark (apps_port_dark), tone and play check their
 * arguments and return a handle that is already done, and nothing reaches the port. A sound that plays is ended by the runtime when the
 * app is hidden, stops or crashes (apps_rt.h), by the app task when it ends the app (apps_task.c), and cut, with a fade, by the LVGL task
 * when the app goes behind or the launcher or a card covers it (apps_lv.c), by the voice task in the dark and at a talk press
 * (muse_voice.c), and by an alarm that rings (apps_alarm.c).
 *
 * Memory: a handle is a userdata of the app's heap (8 bytes and Lua's header), and the module's count of the sounds made is one more,
 * an upvalue of tone and play that no Lua code can reach. The voices are the port's.
 */

#include "apps_rt.h"

typedef struct lua_State lua_State;

#define APPS_SOUND_MT "muse.sound"

#define APPS_SOUND_HZ_MIN 20
#define APPS_SOUND_HZ_MAX 8000  /* the mixer's top, half its rate: a sine there is silent, so tones sound best well below it */
#define APPS_SOUND_MS_MIN 10
#define APPS_SOUND_MS_MAX 2000
#define APPS_SOUND_MS_DEFAULT 100
#define APPS_SOUND_VOLUME_MAX 100

/* The waves sound.tone takes, by name, in apps_mix.h's apps_wave_t order (APPS_WAVE_SINE first). */
#define APPS_SOUND_WAVES 5
extern const char *const apps_sound_waves[APPS_SOUND_WAVES];

/* Installs `sound` into L's globals and the handle's metatable, for the app of rt, which must outlive L. Called by apps_svc_open, inside
 * its protected call, before main.lua's top level runs: a heap that refuses ends the app out of memory at its next call. */
void apps_sound_open(lua_State *L, apps_rt_t *rt);
