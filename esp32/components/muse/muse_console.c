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

#include "muse_console.h"

#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"

#if !CONFIG_MUSE_CONSOLE_UART
#include "driver/usb_serial_jtag.h"
#if CONFIG_LV_USE_SNAPSHOT
#include <stdio.h>
#endif

esp_err_t muse_console_install(size_t rx_buf)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    cfg.rx_buffer_size = rx_buf;
    return usb_serial_jtag_driver_install(&cfg);
}

bool muse_console_getc(uint8_t *c)
{
    return usb_serial_jtag_read_bytes(c, 1, portMAX_DELAY) == 1;
}

void muse_console_write(const void *buf, size_t n)
{
    usb_serial_jtag_write_bytes(buf, n, portMAX_DELAY);
}

#if CONFIG_LV_USE_SNAPSHOT
/* printf and the log take stdout's lock for every call, whichever port is behind it, so holding it keeps them out of a
 * stream the console writes (see the header). flockfile waits for whoever has the lock, with no bound of its own, and
 * nothing here can bound it: ftrylockfile in this libc takes a global lock and returns 0 whatever the stream, so it
 * neither tells that stdout is busy nor holds it (a board run showed the log landing inside the stream, and the global
 * lock never given back), and the FILE's own lock is internal to the libc. The wait is for another task's line, which
 * the console bounds: a reply takes 0.5 s at most (6 KB at 115200 baud), the app echo's queue about 0.2 s, and each
 * flush after one of its lines 50 ms at most. */
void muse_console_hold(void)
{
    flockfile(stdout);
}

void muse_console_release(void)
{
    funlockfile(stdout);
}

bool muse_console_write_timeout(const void *buf, size_t n, uint32_t ms)
{
    /* The ring takes the whole write or none of it. */
    return usb_serial_jtag_write_bytes(buf, n, pdMS_TO_TICKS(ms)) == (int)n;
}

bool muse_console_flush(uint32_t ms)
{
    return usb_serial_jtag_wait_tx_done(pdMS_TO_TICKS(ms)) == ESP_OK;
}
#endif

bool muse_console_host(void)
{
    return usb_serial_jtag_is_connected();
}

#else
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
#include "esp_attr.h"
#include "esp_pm.h"
#include "esp_sleep.h"
#include "freertos/timers.h"
#endif

#define PORT CONFIG_ESP_CONSOLE_UART_NUM

#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
/*
 * In light sleep the UART hears nothing, so RX wakes the chip: WAKE_EDGES
 * rising edges, which a '\n' has, and those characters are lost. The chip
 * then stays up AWAKE_MS, longer while characters keep coming, for the line
 * sent after them. On battery the bridge is off and RX is still.
 */
#define WAKE_EDGES 3
#define AWAKE_MS 3000

static esp_pm_lock_handle_t s_awake;
static TimerHandle_t s_awake_timer;
static portMUX_TYPE s_awake_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_held;

static void let_sleep(TimerHandle_t t)
{
    (void)t;
    portENTER_CRITICAL(&s_awake_mux);
    bool held = s_held;
    s_held = false;
    portEXIT_CRITICAL(&s_awake_mux);
    if (held) {
        esp_pm_lock_release(s_awake);
    }
}

/* The idle task, after each light sleep, in a critical section. */
static esp_err_t IRAM_ATTR on_wake(int64_t slept_us, void *arg)
{
    (void)arg;
    if (slept_us <= 0 || !(esp_sleep_get_wakeup_causes() & BIT(ESP_SLEEP_WAKEUP_UART))) {
        return ESP_OK;
    }
    portENTER_CRITICAL_ISR(&s_awake_mux);
    bool was = s_held;
    s_held = true;
    portEXIT_CRITICAL_ISR(&s_awake_mux);
    if (!was) {
        esp_pm_lock_acquire(s_awake);
    }
    BaseType_t woken = pdFALSE;
    xTimerResetFromISR(s_awake_timer, &woken);
    return ESP_OK;
}

static void wake_on_rx(void)
{
    s_awake_timer = xTimerCreate("console", pdMS_TO_TICKS(AWAKE_MS), pdFALSE, NULL, let_sleep);
    if (!s_awake_timer || esp_pm_lock_create(ESP_PM_NO_LIGHT_SLEEP, 0, "console", &s_awake) != ESP_OK
        || uart_set_wakeup_threshold(PORT, WAKE_EDGES) != ESP_OK || esp_sleep_enable_uart_wakeup(PORT) != ESP_OK) {
        return;
    }
    esp_pm_sleep_cbs_register_config_t cbs = { .exit_cb = on_wake };
    esp_pm_light_sleep_register_cbs(&cbs);
}
#endif

/* The log goes through the driver too, so it can't break into one of our
 * writes (a snapshot line). It still comes between them, a character at a time. */
esp_err_t muse_console_install(size_t rx_buf)
{
    esp_err_t err = uart_driver_install(PORT, rx_buf, 0, 0, NULL, 0);
    if (err == ESP_OK) {
        uart_vfs_dev_use_driver(PORT);
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
        wake_on_rx();
#endif
    }
    return err;
}

bool muse_console_getc(uint8_t *c)
{
    if (uart_read_bytes(PORT, c, 1, portMAX_DELAY) != 1) {
        return false;
    }
#if CONFIG_PM_LIGHT_SLEEP_CALLBACKS
    if (s_held) {
        xTimerReset(s_awake_timer, 0);   /* awake while the line comes in */
    }
#endif
    return true;
}

void muse_console_write(const void *buf, size_t n)
{
    uart_write_bytes(PORT, buf, n);
}

#if CONFIG_LV_USE_SNAPSHOT
/* A UART console holds nothing, because its stream is as slow as the line: 10 bits a byte at the baud rate comes to
 * about 40 s for the Watcher's 412x412 at 115200, and for that long no task could log, Link and the voice pipeline
 * among them. The stream goes out as it did without a hold. A row is one uart_write_bytes call, which the driver
 * doesn't let the log break into, so the log's characters come between rows or a row between its characters, and
 * snap.py takes the rows out of that and retakes a capture whose CRC fails. */
void muse_console_hold(void)
{
}

void muse_console_release(void)
{
}

bool muse_console_write_timeout(const void *buf, size_t n, uint32_t ms)
{
    (void)ms;   /* a UART's FIFO empties at its baud rate whether or not anyone reads, so a write always ends */
    return uart_write_bytes(PORT, buf, n) == (int)n;
}

bool muse_console_flush(uint32_t ms)
{
    return uart_wait_tx_done(PORT, pdMS_TO_TICKS(ms)) == ESP_OK;
}
#endif

bool muse_console_host(void)
{
    return false;
}
#endif
