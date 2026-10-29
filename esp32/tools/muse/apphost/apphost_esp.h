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
 * The little of ESP-IDF and FreeRTOS that apps_alarm.c stands on, for apphost (apphost_esp.c makes it real). apphost runs the gadget's
 * own alarm table over it, so that an alarm does on the laptop what it does on the board, and a script that exercises one shows the
 * gadget's limits, ring and budget, and not a copy of them. Every header apps_alarm.c includes (esp_attr.h, esp_err.h, esp_log.h,
 * esp_timer.h, freertos/FreeRTOS.h, semphr.h, task.h, and the two of the gadget's, muse_input.h and muse_voice.h) is, in this directory,
 * a line that includes this file. The declarations follow ESP-IDF v6.0.1's and FreeRTOS's, only as far as apps_alarm.c calls them.
 *
 * Everything runs on the one thread of apphost, on its fake clock (apps_port_mono_ms, which only a script moves): a timer comes due
 * when the clock reaches it, and apphost_esp_run_timers, which apphost.c calls after each step of the clock, runs the callbacks that
 * are due, the earliest first, as the esp_timer task would. The lock is a flag that checks its contract: a lock taken again by its
 * holder, or given back when it isn't held, is a bug of the table's, and ends the run with a message, as the port's UI lock does.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ---- esp_err.h ---- */

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103

/* ---- esp_attr.h: everything is one memory here ---- */

#define EXT_RAM_BSS_ATTR

/* ---- esp_log.h: warnings are events of apphost, and the rest is the gadget's console and has no counterpart here ---- */

void apphost_esp_log(char level, const char *tag, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
#define ESP_LOGE(tag, ...) apphost_esp_log('E', tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) apphost_esp_log('W', tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) apphost_esp_log('I', tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) apphost_esp_log('D', tag, __VA_ARGS__)

/* Where a line of the log goes (level 'E', 'W', 'I' or 'D'; text is the message, with no tag and no newline): apphost.c's, which
 * prints the warnings (and errors) as events. NULL, the start, drops them all. */
typedef void (*apphost_esp_log_fn)(char level, const char *text);
void apphost_esp_set_log(apphost_esp_log_fn fn);

/* ---- esp_timer.h ---- */

typedef struct apphost_timer *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *arg);
typedef enum { ESP_TIMER_TASK, ESP_TIMER_MAX } esp_timer_dispatch_t;
typedef struct {
    esp_timer_cb_t callback;
    void *arg;
    esp_timer_dispatch_t dispatch_method;
    const char *name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;

/* As ESP-IDF's: a one-shot is no longer armed when its callback runs (the callback may start it again), a periodic timer's next time is
 * its last plus its period (set before the callback), starting a timer that is armed is ESP_ERR_INVALID_STATE, and so is stopping one
 * that isn't. Timers are made and never freed. */
esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);
int64_t esp_timer_get_time(void);  /* apps_port_mono_ms() in microseconds */

/* Runs the callbacks of every timer that is due at the clock's now, the earliest due first (of two together, the one started first), and
 * those that come due meanwhile, as the esp_timer task would after the clock moved. apphost.c calls it after each step. */
void apphost_esp_run_timers(void);

/* ---- FreeRTOS: the lock apps_alarm.c makes, and the delay it never takes here ---- */

typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef struct apphost_mutex *SemaphoreHandle_t;
typedef struct {
    _Alignas(8) unsigned char opaque[64];  /* room for the flag */
} StaticSemaphore_t;

#define pdFALSE 0
#define pdTRUE 1
#define configTICK_RATE_HZ 1000
#define portMAX_DELAY ((TickType_t)0xffffffffu)
#define pdMS_TO_TICKS(ms) ((TickType_t)(((uint64_t)(ms) * configTICK_RATE_HZ) / 1000u))

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer);
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex);
void vTaskDelay(TickType_t ticks);

/* ---- muse_input.h and muse_voice.h: what an alarm asks of the rest of the gadget (apphost_port.c answers) ---- */

void muse_input_wake(const char *why);  /* the screen wakes, as for a press: apphost's screen is lit */
void muse_voice_request_chirp(void);    /* the voice task plays a chirp: the ring's period has no counterpart here */
