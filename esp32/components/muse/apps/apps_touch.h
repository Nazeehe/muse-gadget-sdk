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
 * Link's app.control tap and drag: Muse's touches on the app it tests (muse_apps.h has the contract, muse_apps_touch). How its
 * three tasks meet, after snapshot's way (apps_snapshot.h):
 *
 * 1. The caller (the Noise session's task, or the console's) checks the parameters and claims the one touch there may be, then
 *    queues one job on the I/O worker (apps_io.h). It never waits.
 * 2. The job checks that id is the app that runs, wakes the screen (muse_input_wake("touch"), which lights a dark one and keeps a
 *    lit one lit), and asks the LVGL task for the touch with a deadline, MUSE_APPS_TOUCH_SCREEN_MS from then; it polls for the
 *    answer, running the app task's ops as it waits (apps_io_assist).
 * 3. The LVGL task, every frame (muse_apps_ui_supervise: apps_touch_supervise), hands the touch on the first frame the screen isn't
 *    dark to the touch player (muse_ui.h: muse_ui_guarded_drag), with the guard below, and after the touch waits for the scroll it
 *    began to come to rest, at most MUSE_APPS_TOUCH_SETTLE_MS, before it answers.
 *
 * The guard (the player calls it on the LVGL task, from inside LVGL's input read, before LVGL sees the point):
 * - admit, at the read that would press the first point (the player has seen no finger on the screen by then, in this read or in
 *   LVGL's last one): id must be the app that runs with its table on the layer and not hung (apps_task_touch_target, which reads
 *   its app_gen with the id), its layer in front with no card over it (apps_lv_front, apps_lv_cards_shown), no voice turn on
 *   (apps_port_in_turn), the screen lit, and the point on the app (apps_lv_hit: what LVGL would press there is the layer or inside
 *   it). Each refusal names what failed.
 * - keep, at every read after, with the point that read gives (the release's too): the same but the names lock, so atomics and the
 *   LVGL task's own state only, the app by its app_gen (apps_task_touch_alive). A keep that says no cuts the touch.
 * - end: the touch is over (played, refused or cut), at the first read after its release, when LVGL has sent all it made it send.
 * Nothing else of LVGL's runs between a check and LVGL's use of the point, so a check can't go stale: that is why they are made in
 * the read and not at a frame. The layer covers the whole screen, so while these hold no point of the touch can press anything but
 * the app's; and LVGL keeps a press on the object it began on (LV_OBJ_FLAG_PRESS_LOCK), so a touch never moves onto another.
 *
 * The handshake (one atomic word, in internal RAM): IDLE; ASKED (the job asked); QUEUED (the LVGL task gave it to the player);
 * PLAYING (admitted); SETTLING (ended: the LVGL task waits for the scroll); DONE (answered: the job reads the answer and sets IDLE).
 * At the screen's deadline the job takes back an ASKED one, or a QUEUED one the player hasn't taken (muse_ui_guarded_withdraw), as
 * IDLE; one the player has taken it waits for. At the whole touch's deadline it gives up on one that hasn't answered (ABANDONED):
 * keep then says no, which cuts it, and the LVGL task sets IDLE at its end. A job that finds the word not IDLE answers busy.
 *
 * What the LVGL task saw (the pressed widget, the events heard, the cut and why, the scroll boxes before and after) is its own until
 * it says DONE, then the job's. While a touch is PLAYING every touch event LVGL sends the app is the touch's (the player doesn't let
 * the real touch through to LVGL meanwhile), so apps_touch_tag tags them, and counts them as heard.
 */

#include <stdint.h>

#define APPS_TOUCH_MIN_LEFT_MS 9000  /* a job that starts with less of its deadline left does nothing: busy */
#define APPS_TOUCH_POLL_MS 10        /* how often the job looks for the LVGL task's answer */

/* The LVGL task, every frame, the dark ones included (muse_apps_ui_supervise): hands an asked touch to the player on the first frame
 * the screen isn't dark, and answers an ended one once its scroll has come to rest. It never waits for another task. */
void apps_touch_supervise(void);

/* The LVGL task, for each touch event the layer's widgets send the app (apps_task.c's on_widget_event, before it is posted):
 * event with APPS_UI_INJECTED while a touch of Muse's plays, counted as heard (slot's kind named as status names it); event as it
 * is otherwise. */
uint8_t apps_touch_tag(uint16_t slot, uint8_t event);

/* The LVGL task, inside the layer's right swipe (apps_task.c's on_close): while a touch of Muse's plays, its drag was the swipe. */
void apps_touch_closed(void);
