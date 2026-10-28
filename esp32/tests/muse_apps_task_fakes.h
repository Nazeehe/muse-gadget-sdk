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
 * The FreeRTOS and ESP-IDF that apps_task.c sees on the host (tests/test_muse_apps_task.py stages its FreeRTOS and
 * ESP-IDF headers, lvgl.h among them, as one line each that includes this). It builds on muse_apps_lv_fakes.h, the
 * fakes of apps_lv.c (LVGL's types, esp_log, esp_timer, the heap caps), and adds what the app task needs: tasks,
 * queues and mutexes. tests/muse_apps_task_harness.c makes them real with pthreads, so the app task, the LVGL task
 * and a caller run at once, as they do on the gadget. The declarations follow ESP-IDF v6.0.1's (freertos/task.h,
 * queue.h, semphr.h, idf_additions.h), as functions where FreeRTOS has macros: only what apps_task.c calls, and what
 * apps_port_idf.c does (tests/muse_apps_port_harness.c, with its own definitions).
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

/* ---- esp_heap_caps.h: what muse_apps_lv_fakes.h doesn't declare ---- */

#define MALLOC_CAP_INTERNAL (1u << 11)
void *heap_caps_malloc(size_t size, uint32_t caps);
void *heap_caps_calloc(size_t n, size_t size, uint32_t caps);
void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps);  /* apps_port_idf.c's (tests/muse_apps_port_harness.c) */
void heap_caps_free(void *ptr);

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
#define configTICK_RATE_HZ 1000
#define portMAX_DELAY ((TickType_t)0xffffffffu)
#define pdMS_TO_TICKS(ms) ((TickType_t)(((uint64_t)(ms) * configTICK_RATE_HZ) / 1000u))

void vTaskDelay(const TickType_t ticks);
TaskHandle_t xTaskGetCurrentTaskHandle(void);
void vTaskPrioritySet(TaskHandle_t task, UBaseType_t priority);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t task);
BaseType_t xTaskCreatePinnedToCoreWithCaps(TaskFunction_t code, const char *const name, const uint32_t stack, void *const arg,
                                           UBaseType_t priority, TaskHandle_t *const created, const BaseType_t core,
                                           UBaseType_t caps);

QueueHandle_t xQueueCreateWithCaps(UBaseType_t length, UBaseType_t item_size, UBaseType_t caps);
void vQueueDeleteWithCaps(QueueHandle_t queue);
BaseType_t xQueueSend(QueueHandle_t queue, const void *item, TickType_t wait);
BaseType_t xQueueReceive(QueueHandle_t queue, void *item, TickType_t wait);
UBaseType_t uxQueueSpacesAvailable(QueueHandle_t queue);

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer);
BaseType_t xSemaphoreTake(SemaphoreHandle_t mutex, TickType_t wait);
BaseType_t xSemaphoreGive(SemaphoreHandle_t mutex);
