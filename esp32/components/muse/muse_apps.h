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
 * Muse apps (CONFIG_MUSE_APPS), as the rest of the firmware sees them: the app task (components/muse/apps/apps_task.h
 * has its services) and the hooks muse_ui.c calls on the LVGL task. An app draws on a full-screen layer over the
 * pages, under the downloaded image and the top layer; the launcher and the crash card are cards over that layer.
 *
 * The LVGL task's hooks, in frame_tick's order: muse_apps_ui_turn when the voice mode changes;
 * muse_apps_ui_supervise before update_sleep(), so it runs while the screen sleeps; muse_apps_ui_tick after the
 * chrome. Other tasks never make an LVGL call here: they leave a request that the next frame carries out.
 */

#include <stdbool.h>

#include "esp_err.h"
#include "lvgl.h"
#include "sdkconfig.h"

#if CONFIG_MUSE_APPS

/* Makes the app task (apps_task.h). Call it once, after muse_ui_start() and before muse_input_start(). Out of memory
 * it returns ESP_ERR_NO_MEM, and the firmware runs on without apps. */
esp_err_t muse_apps_start(void);

/* Makes the app layer on screen (apps_lv_build) and takes its touches and swipes. In build_overlays(), after the
 * page dots and before the downloaded image, under the display lock and before the frame timer exists. */
void muse_apps_ui_build(lv_obj_t *screen);

/* The LVGL task, every frame, before update_sleep(): the hang supervisor (a call into the app that has run more than
 * APPS_TASK_HUNG_MS), the reset of an app that ended (before any apply in the frame), the front and back the app task
 * asked for (a front once the app's table is published), the layer's place after a voice turn has ended, and the
 * cards (neither of these two during a turn). */
void muse_apps_ui_supervise(void);

/* The LVGL task, every frame the screen is awake: the apply step (apps_lv_apply). True while the app layer or a card
 * covers the screen, and frame_tick then draws nothing under it. */
bool muse_apps_ui_tick(void);

/* The LVGL task, when the voice mode changes: in_turn while Muse listens, thinks or speaks. A turn hides an app that is
 * in front (hide() runs) and closes the cards; its end brings the app back (show() runs) if it was in front, or was
 * asked to the front during the turn. */
void muse_apps_ui_turn(bool in_turn);

/* Any task: the launcher opens at the next frame (after a voice turn). A long press on the watch calls this. */
void muse_apps_open_launcher(void);

/* Any task: the app layer is in front. */
bool muse_apps_front(void);

/* Any task: the app layer is in front or a card (the launcher, a crash card) is shown, as of the LVGL task's last
 * frame or change. The watch yields to it (muse_ui_clock_showing). */
bool muse_apps_covering(void);

#endif
