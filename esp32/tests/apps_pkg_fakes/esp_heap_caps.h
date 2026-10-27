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
 * The one call of ESP-IDF's heap API that apps_pkg.c makes under ESP_PLATFORM, for the host test
 * (tests/muse_apps_pkg_harness.c), which defines heap_caps_malloc so as to see the capabilities it is
 * asked for, to count what it hands out and to refuse.
 */

#include <stddef.h>
#include <stdint.h>

/* The values ESP-IDF gives them. */
#define MALLOC_CAP_8BIT (1u << 2)
#define MALLOC_CAP_SPIRAM (1u << 10)

void *heap_caps_malloc(size_t size, uint32_t caps);
