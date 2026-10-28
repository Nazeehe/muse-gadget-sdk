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
 * The budget of the gadget's console echo of app log lines (apps_port_idf.c): a token bucket of bytes. On the 1.75C the
 * console is UART0 at 115200 with no driver, so each byte is a busy-wait of about 87 us on the task that prints (the app
 * task, inside the call's wall clock). The echo takes at most APPS_ECHO_RATE bytes a second, from a burst of
 * APPS_ECHO_BURST; the log ring and the status keep every line, echoed or not. Portable C, so tests/test_muse_apps_port.py
 * checks the arithmetic on the host.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPS_ECHO_RATE 1024u   /* bytes a second */
#define APPS_ECHO_BURST 1024u  /* bytes at once, after a quiet second */
#define APPS_ECHO_QUEUE (APPS_ECHO_BURST + APPS_ECHO_RATE)  /* bytes queued between drains: what a call can echo */

/* Zeroed, it is a full bucket that starts at its first take. */
typedef struct {
    bool started;
    int64_t at_ms;    /* when credit was last brought up to date */
    uint32_t credit;  /* thousandths of a byte the echo may take now: at most APPS_ECHO_BURST * 1000 */
} apps_echo_t;

/* Takes bytes from b at now_ms (a monotonic clock) if b holds them, after crediting the time since the last take at
 * APPS_ECHO_RATE, never above APPS_ECHO_BURST: whether it took them. A take it can't cover takes nothing. */
static inline bool apps_echo_take(apps_echo_t *b, int64_t now_ms, size_t bytes) {
    const uint64_t full = (uint64_t)APPS_ECHO_BURST * 1000u;
    if (!b->started) {
        b->started = true;
        b->credit = (uint32_t)full;
        b->at_ms = now_ms;
    } else if (now_ms > b->at_ms) {
        uint64_t elapsed = (uint64_t)(now_ms - b->at_ms);
        uint64_t credit = b->credit + (elapsed < 1000000u ? elapsed : 1000000u) * APPS_ECHO_RATE;
        b->credit = (uint32_t)(credit < full ? credit : full);
        b->at_ms = now_ms;
    }
    if (bytes > APPS_ECHO_BURST || bytes * 1000u > b->credit) {
        return false;
    }
    b->credit -= (uint32_t)(bytes * 1000u);
    return true;
}
