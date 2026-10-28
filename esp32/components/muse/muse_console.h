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
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "sdkconfig.h"

/*
 * The serial port the Muse tools (tools/muse) talk over, shared with the log:
 * the chip's own USB Serial/JTAG port where it has one on the USB connector,
 * else the console UART behind the board's USB bridge (the Watcher, the
 * classic ESP32; CONFIG_MUSE_CONSOLE_UART).
 */

/* rx_buf bytes are kept; what doesn't fit is dropped. */
esp_err_t muse_console_install(size_t rx_buf);
/* Blocks for the next byte; false on a driver error. */
bool muse_console_getc(uint8_t *c);
void muse_console_write(const void *buf, size_t n);
/* A USB host is reading the port. A UART bridge can't tell: false. */
bool muse_console_host(void);

#if CONFIG_LV_USE_SNAPSHOT
/*
 * What a screenshot (muse_ui.c's send_snapshot) needs of the console, which is only built with screenshots. On the
 * USB-Serial-JTAG console its rows go out through the driver while printf and the log reach the cable another way, so
 * a line printed during the stream lands inside a row. muse_console_hold keeps stdout's lock, which every printf and
 * log line takes, until muse_console_release: they wait, and come out after the stream. Whatever else reaches the port
 * through stdio has to take the lock too (fsync takes none: apps_port_console_flush does). The writes and the flush
 * give up after a time they are given; the hold itself waits for whoever has stdout, which is another task's line,
 * reply or queue of echo lines. A UART console holds nothing: its stream takes as long as its bytes do at the baud
 * rate, about 40 s for the Watcher's screen, and no task could log for that long (see muse_console.c).
 */

/* Takes stdout's lock, waiting for whoever has it: a line, a reply (0.5 s at most) or the app echo's queue. It can't
 * give up: this libc has no try-lock that works (see muse_console.c). Nothing on a UART console. */
void muse_console_hold(void);
/* Gives the lock back. Nothing on a UART console. */
void muse_console_release(void);
/* muse_console_write, giving up when the console takes nothing for ms (a host that has stopped reading): false, and
 * none of buf went out. A UART takes bytes at its baud rate whether or not anyone reads, so it never gives up. */
bool muse_console_write_timeout(const void *buf, size_t n, uint32_t ms);
/* Waits until everything written has left the chip, for at most ms. False when some hasn't by then. */
bool muse_console_flush(uint32_t ms);
#endif
