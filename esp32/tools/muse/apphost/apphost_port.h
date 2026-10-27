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
 * What apphost's port (apphost_port.c, which implements apps_port.h on the host) gives apphost.c besides
 * the port itself: where the app's log lines go, and what the apps' heap holds.
 */

#include <stddef.h>

/* Where apps_port_log sends a line: apphost.c's event writer, or nothing (NULL, the start) for --cycles. */
typedef void (*apphost_log_fn)(const char *app_id, const char *line);
void apphost_set_log(apphost_log_fn fn);

/* Bytes the apps' heap holds now: what apps_port_realloc handed out and apps_port_free hasn't taken back.
 * 0 when everything an app (and its runtime) allocated has been freed. */
size_t apphost_heap_held(void);
