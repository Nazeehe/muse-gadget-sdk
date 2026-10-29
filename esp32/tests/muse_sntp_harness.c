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

/*
 * muse_sntp.c, the SNTP helper the watch and the apps' time module share, run on the host. Usage: muse_sntp_harness
 * <case>, one case per test in tests/test_muse_sntp.py; it exits 0 when the case holds, and otherwise prints why.
 *
 * The ESP-IDF it calls is a fake (tests/test_muse_sntp.py stages the headers next to a copy of the source): a Wi-Fi the
 * case turns on and off, an SNTP client that counts its starts and can fail, a log that keeps what is logged, and time()
 * itself, which this file defines so that the case sets the clock the helper reads.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_netif_sntp.h"
#include "muse_sntp.h"

static int failures;

#define CHECK(cond, ...)                                 \
    do {                                                 \
        if (!(cond)) {                                   \
            fprintf(stderr, "FAIL line %d: ", __LINE__); \
            fprintf(stderr, __VA_ARGS__);                \
            fputc('\n', stderr);                         \
            failures++;                                  \
        }                                                \
    } while (0)

/* ---- The fakes ---- */

static bool wifi;
static time_t clock_now;

bool muse_wifi_connected(void) {
    return wifi;
}

time_t time(time_t *out) {
    if (out != NULL) {
        *out = clock_now;
    }
    return clock_now;
}

static int inits;
static char server[64];
static esp_err_t init_result;

esp_err_t esp_netif_sntp_init(const esp_sntp_config_t *config) {
    inits++;
    snprintf(server, sizeof server, "%s", config->server);
    return init_result;
}

const char *esp_err_to_name(esp_err_t err) {
    return err == ESP_ERR_NO_MEM ? "ESP_ERR_NO_MEM" : "ESP_ERR_OTHER";
}

static char logged[8][200];
static int n_logged;

void fake_esp_log(char level, const char *tag, const char *fmt, ...) {
    char text[160];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (n_logged < 8) {
        snprintf(logged[n_logged++], sizeof logged[0], "%c %s: %s", level, tag, text);
    }
}

static void dump_log(void) {
    for (int i = 0; i < n_logged; i++) {
        fprintf(stderr, "  log %d: %s\n", i, logged[i]);
    }
}

/* ---- The cases ---- */

#define TZ_LINE "muse_sntp: SNTP %s, time zone PST8PDT,M3.2.0,M11.1.0"

/* Nothing starts until Wi-Fi is up; then SNTP starts once, with pool.ntp.org, and the boot log says so; the helper never
 * starts it again, whatever Wi-Fi does after. */
static void starts_once_when_wifi_is_up(void) {
    for (int i = 0; i < 3; i++) {
        muse_sntp_poll();
    }
    CHECK(inits == 0 && n_logged == 0, "started without Wi-Fi: %d starts, %d lines", inits, n_logged);
    wifi = true;
    muse_sntp_poll();
    CHECK(inits == 1, "started %d times", inits);
    CHECK(strcmp(server, "pool.ntp.org") == 0, "the server is '%s'", server);
    char want[200];
    snprintf(want, sizeof want, "I " TZ_LINE, "started");
    CHECK(n_logged == 1 && strcmp(logged[0], want) == 0, "%d lines, the first '%s'", n_logged, n_logged > 0 ? logged[0] : "");
    muse_sntp_poll();
    wifi = false;
    muse_sntp_poll();
    wifi = true;
    muse_sntp_poll();
    CHECK(inits == 1 && n_logged == 1, "after more polls: %d starts, %d lines", inits, n_logged);
    if (failures != 0) {
        dump_log();
    }
}

/* A start that fails is logged with the error's name, and is not tried again: the old code set its flag the same way. */
static void a_failed_start_is_logged_and_not_retried(void) {
    wifi = true;
    init_result = ESP_ERR_NO_MEM;
    for (int i = 0; i < 4; i++) {
        muse_sntp_poll();
    }
    char want[200];
    snprintf(want, sizeof want, "I " TZ_LINE, "ESP_ERR_NO_MEM");
    CHECK(inits == 1, "tried %d times", inits);
    CHECK(n_logged == 1 && strcmp(logged[0], want) == 0, "%d lines, the first '%s'", n_logged, n_logged > 0 ? logged[0] : "");
    if (failures != 0) {
        dump_log();
    }
}

/* The clock counts as set from 2025-01-01T00:00:00Z: before SNTP the RTC counts from 1970. */
static void the_clock_is_valid_from_2025(void) {
    static const struct {
        long long seconds;
        bool valid;
    } steps[] = {
        {-1, false},          /* time() failing */
        {0, false},           /* 1970, the RTC's start */
        {1000, false},        /* a minute into a boot */
        {1735689599, false},  /* 2024-12-31T23:59:59Z */
        {1735689600, true},   /* 2025-01-01T00:00:00Z */
        {1767225600, true},   /* 2026-01-01T00:00:00Z */
        {4102444800LL, true}, /* 2100 */
    };
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        clock_now = (time_t)steps[i].seconds;
        CHECK(muse_time_valid() == steps[i].valid, "at %lld: %d", steps[i].seconds, muse_time_valid());
    }
    CHECK(inits == 0 && n_logged == 0, "asking started something: %d starts, %d lines", inits, n_logged);
}

int main(int argc, char **argv) {
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"starts_once_when_wifi_is_up", starts_once_when_wifi_is_up},
        {"a_failed_start_is_logged_and_not_retried", a_failed_start_is_logged_and_not_retried},
        {"the_clock_is_valid_from_2025", the_clock_is_valid_from_2025},
    };
    if (argc != 2) {
        fprintf(stderr, "usage: %s <case>\n", argv[0]);
        return 2;
    }
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        if (strcmp(argv[1], cases[i].name) == 0) {
            cases[i].run();
            return failures == 0 ? 0 : 1;
        }
    }
    fprintf(stderr, "no case '%s'\n", argv[1]);
    return 2;
}
