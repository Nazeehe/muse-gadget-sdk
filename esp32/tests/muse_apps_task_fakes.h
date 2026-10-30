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
 * The FreeRTOS and ESP-IDF that apps_task.c and apps_io.c see on the host (tests/test_muse_apps_task.py stages their
 * FreeRTOS and ESP-IDF headers, lvgl.h among them, as one line each that includes this). It builds on
 * muse_apps_lv_fakes.h, the fakes of apps_lv.c (LVGL's types, esp_log, esp_timer, the heap caps), and adds what the
 * app task and the I/O worker need: tasks, queues and mutexes. tests/muse_apps_task_harness.c makes them real with
 * pthreads, so the app task, the LVGL task, the I/O worker and a caller run at once, as they do on the gadget. The
 * declarations follow ESP-IDF v6.0.1's (freertos/task.h, queue.h, semphr.h, idf_additions.h), as functions where
 * FreeRTOS has macros: only what apps_task.c and apps_io.c call, and what apps_port_idf.c does
 * (tests/muse_apps_port_harness.c, with its own definitions).
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "muse_apps_lv_fakes.h"

/* ---- esp_err.h ---- */

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_STATE 0x103

/* ---- esp_timer.h: what apps_alarm.c calls (esp_timer_get_time is muse_apps_lv_fakes.h's, each harness's own clock) ---- */

typedef struct fake_esp_timer *esp_timer_handle_t;
typedef void (*esp_timer_cb_t)(void *arg);
typedef enum { ESP_TIMER_TASK, ESP_TIMER_MAX } esp_timer_dispatch_t;
typedef struct {
    esp_timer_cb_t callback;
    void *arg;
    esp_timer_dispatch_t dispatch_method;
    const char *name;
    bool skip_unhandled_events;
} esp_timer_create_args_t;

esp_err_t esp_timer_create(const esp_timer_create_args_t *args, esp_timer_handle_t *out);
esp_err_t esp_timer_start_once(esp_timer_handle_t timer, uint64_t timeout_us);
esp_err_t esp_timer_start_periodic(esp_timer_handle_t timer, uint64_t period_us);
esp_err_t esp_timer_stop(esp_timer_handle_t timer);

/* tests/muse_apps_fake_esp_timer.c makes these real: one thread runs every callback, one at a time and in the order they are
 * due, as the esp_timer task does, polling esp_timer_get_time for the clock a harness gives. What a harness asks of it: */
bool fake_esp_timer_on_task(void);  /* the caller is that thread */
void fake_esp_timer_sync(void);     /* returns once every timer due now has run its callback, and the thread is idle */
int64_t fake_esp_timer_next_due_us(void);  /* when the earliest armed timer is due, or INT64_MAX when none is armed */
/* Runs the callback of the timer made with this name and arg (the arg as a number) once, on that thread, whether or not it is
 * armed: a call that was on its way when the timer was stopped or restarted, which a real esp_timer task can still make.
 * Returns when it has run. */
void fake_esp_timer_call(const char *name, uintptr_t arg);
/* From now on esp_timer_create makes timers while there are fewer than max_timers of them (made ones stay), and says
 * ESP_ERR_NO_MEM for the next: the esp_timer task's memory, which is internal RAM, run out. 32 is the most there is. */
void fake_esp_timer_set_limit(int max_timers);
/* The next esp_timer_stop of a timer that is armed is followed at once by a start of it (10 ms on), as if another task's start
 * came between that stop and the caller's next call: the caller's own start then finds it armed (ESP_ERR_INVALID_STATE). */
void fake_esp_timer_start_behind_next_stop(void);

/* ---- esp_attr.h: where a static goes. On the gadget EXT_RAM_BSS_ATTR puts it in PSRAM (when the build lets statics go there); here
 * everything is one memory ---- */

#define EXT_RAM_BSS_ATTR

/* ---- esp_heap_caps.h: what muse_apps_lv_fakes.h doesn't declare ---- */

#define MALLOC_CAP_INTERNAL (1u << 11)
void *heap_caps_malloc(size_t size, uint32_t caps);
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps);  /* apps_port_idf.c's (tests/muse_apps_port_harness.c) */
void heap_caps_free(void *ptr);
size_t heap_caps_get_largest_free_block(uint32_t caps);  /* what apps_io_submit no longer asks (R1) */

/* ---- FreeRTOS ---- */

typedef int BaseType_t;
typedef unsigned int UBaseType_t;
typedef uint32_t TickType_t;
typedef void (*TaskFunction_t)(void *);
typedef struct fake_task *TaskHandle_t;
typedef struct fake_queue *QueueHandle_t;
typedef struct fake_mutex *SemaphoreHandle_t;
/* Room for the harness's mutex: a pthread mutex and condition variable. */
typedef struct {
    _Alignas(16) unsigned char opaque[512];
} StaticSemaphore_t;

#define pdFALSE 0
#define pdTRUE 1
#define pdFAIL 0
#define pdPASS 1
/* The board builds at 1000 Hz (CONFIG_FREERTOS_HZ, sdkconfig.defaults), ESP-IDF's own default is 100: a harness built with
 * -DMUSE_FAKE_TICK_HZ=100 shows what the app task does when a tick is 10 ms (a wait of 1 to 9 ms is no tick at all). */
#ifndef MUSE_FAKE_TICK_HZ
#define MUSE_FAKE_TICK_HZ 1000
#endif
#define configTICK_RATE_HZ MUSE_FAKE_TICK_HZ
#define portMAX_DELAY ((TickType_t)0xffffffffu)
#define pdMS_TO_TICKS(ms) ((TickType_t)(((uint64_t)(ms) * configTICK_RATE_HZ) / 1000u))
#define tskNO_AFFINITY ((BaseType_t)0x7FFFFFFF)

void vTaskDelay(const TickType_t ticks);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskPrioritySet(TaskHandle_t task, UBaseType_t priority);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t code, const char *const name, const uint32_t stack, void *const arg,
                                           UBaseType_t priority, TaskHandle_t *const created, const BaseType_t core,
                                           UBaseType_t caps);
/* The I/O worker's end (apps_io.c): it suspends itself, and the task that makes the next worker deletes it once eTaskGetState
 * says it is suspended. */
typedef enum { eRunning = 0, eReady, eBlocked, eSuspended, eDeleted, eInvalid } eTaskState;
eTaskState eTaskGetState(TaskHandle_t task);
void vTaskSuspend(TaskHandle_t task);
void vTaskDeleteWithCaps(TaskHandle_t task);

QueueHandle_t xQueueCreateWithCaps(UBaseType_t length, UBaseType_t item_size, UBaseType_t caps);
void vQueueDeleteWithCaps(QueueHandle_t queue);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t queue);
UBaseType_t uxQueueMessagesWaiting(QueueHandle_t queue);

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer);
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex);
