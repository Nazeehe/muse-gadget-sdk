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

#include <stdbool.h>

/*
 * The wall clock's source (CONFIG_MUSE_CLOCK): SNTP, from pool.ntp.org, once Wi-Fi is up. The pocket watch's hands and
 * the apps' time module both read the clock this sets. The time zone is the watch's (muse_clock_build).
 */

/* Every frame, in the LVGL task: starts SNTP once, as soon as Wi-Fi is up, and does nothing after that. */
void muse_sntp_poll(void);

/* From any task: the wall clock has been set, which is when it reads 2025 or later (before SNTP the RTC counts from
 * 1970). */
bool muse_time_valid(void);
