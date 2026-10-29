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

/* What the Muse apps' store logs, kept by tests/muse_apps_store_harness.c for a case to read back. */

void fake_esp_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

#define ESP_LOGE(tag, ...) fake_esp_log('E', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) fake_esp_log('W', tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) fake_esp_log('I', tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) fake_esp_log('D', tag, __VA_ARGS__)
