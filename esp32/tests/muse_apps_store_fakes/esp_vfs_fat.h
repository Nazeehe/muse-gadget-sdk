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

/* ESP-IDF v6.0.1's esp_vfs_fat.h, as much of it as the Muse apps' store calls (tests/muse_apps_store_harness.c defines
 * the functions over a directory of the host's own: the mount makes it, "formatting" it when it isn't there). The
 * declarations follow the real header's, components/fatfs/vfs/esp_vfs_fat.h, and wear_levelling.h's handle. */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

typedef int32_t wl_handle_t;
#define WL_INVALID_HANDLE -1

typedef struct {
    bool format_if_mount_failed;
    int max_files;
    size_t allocation_unit_size;
    bool disk_status_check_enable;
    bool use_one_fat;
} esp_vfs_fat_mount_config_t;

#define VFS_FAT_MOUNT_DEFAULT_CONFIG() \
    { \
        .format_if_mount_failed = false, \
        .max_files = 5, \
        .allocation_unit_size = 0, \
        .disk_status_check_enable = false, \
        .use_one_fat = false, \
    }

esp_err_t esp_vfs_fat_spiflash_mount_rw_wl(const char *base_path, const char *partition_label,
                                           const esp_vfs_fat_mount_config_t *mount_config, wl_handle_t *wl_handle);
esp_err_t esp_vfs_fat_spiflash_unmount_rw_wl(const char *base_path, wl_handle_t wl_handle);
esp_err_t esp_vfs_fat_info(const char *base_path, uint64_t *out_total_bytes, uint64_t *out_free_bytes);
