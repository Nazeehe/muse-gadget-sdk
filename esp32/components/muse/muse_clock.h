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

#include "lvgl.h"

/*
 * Pocket watch page (CONFIG_MUSE_CLOCK): an analog dial left of Muse's face.
 * The time comes from SNTP once Wi-Fi joins (muse_sntp.h), in CONFIG_MUSE_CLOCK_TZ,
 * which muse_clock_build sets. Both run in the LVGL task.
 */

/* Draws the watch into `tile`, the tileview's first tile. */
void muse_clock_build(lv_obj_t *tile);

/* Every frame: while the watch is on screen (`visible`), moves the hands when
 * the time has changed. */
void muse_clock_tick(bool visible);
