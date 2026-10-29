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

#include "muse_sntp.h"

#include <time.h>

#include "esp_log.h"
#include "esp_netif_sntp.h"
#include "sdkconfig.h"

#include "muse_wifi.h"

static const char *TAG = "muse_sntp";

/* 2025-01-01T00:00:00Z: before SNTP the RTC counts from 1970. */
#define VALID_AFTER_S 1735689600

static bool s_started;

void muse_sntp_poll(void)
{
    if (s_started || !muse_wifi_connected()) {
        return;
    }
    esp_sntp_config_t cfg = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
    esp_err_t err = esp_netif_sntp_init(&cfg);
    ESP_LOGI(TAG, "SNTP %s, time zone %s", err == ESP_OK ? "started" : esp_err_to_name(err),
             CONFIG_MUSE_CLOCK_TZ);
    s_started = true;
}

bool muse_time_valid(void)
{
    return time(NULL) >= VALID_AFTER_S;
}
