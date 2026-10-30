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
 * The store of Muse apps (apps_store.c), run on the host over the host's own files. Usage:
 *   muse_apps_store_harness <case> <dir> [arg]
 * The harness changes into dir, a fresh directory the test made, where APPS_STORE_BASE ("apps_fs": the test builds it so)
 * is the apps partition's file system; it exits 0 when the case holds, and otherwise says why on stderr.
 *
 * The rest of the gadget is fakes, defined here: the partition table (an "apps" partition, or none), the FAT mount (it makes
 * the directory, "formatting" a partition that has none), NVS (a map in memory for each partition, which fails the case if
 * anything touches the pairing's "nvs"), the reset reason, the clocks, the apps' heap (counted), the RAM store of
 * apps_install.h, the system's alarms, the I/O worker (its jobs wait for the case to run them) and the launcher's list.
 */

#include <dirent.h>
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <unistd.h>

#include "apps_alarm.h"
#include "apps_install.h"
#include "apps_io.h"
#include "apps_pkg.h"
#include "apps_port.h"
#include "apps_store.h"
#include "apps_task.h"
#include "apps_values.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

_Static_assert(APPS_RAM_APPS == 8, "RAM mode keeps at most 8 packages (the brief's 8)");

static int failures;

static void dump_log(void);

#define CHECK(cond, ...)                                       \
    do {                                                       \
        if (!(cond)) {                                         \
            fprintf(stderr, "FAIL line %d: ", __LINE__);       \
            fprintf(stderr, __VA_ARGS__);                      \
            fputc('\n', stderr);                               \
            failures++;                                        \
        }                                                      \
    } while (0)

/* ---- The log ---- */

#define LOG_MAX 160
static char logged[LOG_MAX][400];
static int n_logged;

void fake_esp_log(char level, const char *tag, const char *fmt, ...) {
    if (level == 'D') {
        return;
    }
    char text[360];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    if (n_logged < LOG_MAX) {
        snprintf(logged[n_logged++], sizeof logged[0], "%c %s: %s", level, tag, text);
    }
}

static int logged_count(const char *text) {
    int n = 0;
    for (int i = 0; i < n_logged; i++) {
        n += strstr(logged[i], text) != NULL ? 1 : 0;
    }
    return n;
}

static bool logged_has(const char *text) {
    return logged_count(text) > 0;
}

static void dump_log(void) {
    for (int i = 0; i < n_logged; i++) {
        fprintf(stderr, "  log %d: %s\n", i, logged[i]);
    }
}

/* ---- FreeRTOS: one thread, so a mutex is a flag that has to be taken and given in pairs ---- */

struct fake_store_mutex {
    bool held;
};
_Static_assert(sizeof(struct fake_store_mutex) <= sizeof(StaticSemaphore_t), "a fake mutex fits in its buffer");

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer) {
    struct fake_store_mutex *m = (struct fake_store_mutex *)(void *)buffer->opaque;
    m->held = false;
    return m;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait) {
    (void)wait;
    CHECK(!m->held, "a mutex taken by the task that holds it");
    m->held = true;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t m) {
    CHECK(m->held, "a mutex given that wasn't held");
    m->held = false;
    return pdTRUE;
}

/* ---- The clocks ---- */

static int64_t mono_ms = 10000;
static bool clock_set;
static int64_t epoch_now = 1767225600000LL;  /* 2026-01-01T00:00:00Z */

int64_t apps_port_mono_ms(void) {
    return mono_ms;
}

bool apps_port_epoch_ms(int64_t *out) {
    if (!clock_set) {
        return false;
    }
    *out = epoch_now;
    return true;
}

/* ---- The apps' heap, counted (apps_port_sha256 is tools/muse/apphost/sha256.c's), and refused when a case says so ---- */

static long blocks_held;
static size_t psram_free = (size_t)3 << 20;
static int alloc_fail_in = -1;  /* the new block this many allocations from now is refused (0: the next); -1 none */
static bool alloc_refuse_all;   /* every new block is refused */
static size_t alloc_refuse_size; /* a new block of exactly this many bytes is refused (0: none) */

void *apps_port_realloc(void *ptr, size_t n) {
    if (ptr == NULL && alloc_refuse_size != 0 && n == alloc_refuse_size) {
        return NULL;
    }
    if (ptr == NULL && (alloc_refuse_all || (alloc_fail_in >= 0 && alloc_fail_in-- == 0))) {
        return NULL;
    }
    void *block = realloc(ptr, n);
    if (ptr == NULL && block != NULL) {
        blocks_held++;
    }
    return block;
}

void apps_port_free(void *ptr) {
    if (ptr != NULL) {
        blocks_held--;
    }
    free(ptr);
}

size_t apps_port_psram_free(void) {
    return psram_free;
}

/* ---- The partition table ---- */

static bool has_apps_partition = true;
static const esp_partition_t APPS_PARTITION = {
    .type = ESP_PARTITION_TYPE_DATA, .subtype = ESP_PARTITION_SUBTYPE_DATA_FAT, .address = 0x830000, .size = 0x600000,
    .label = "apps"};

const esp_partition_t *esp_partition_find_first(esp_partition_type_t type, esp_partition_subtype_t subtype,
                                                const char *label) {
    bool apps = type == ESP_PARTITION_TYPE_DATA && subtype == ESP_PARTITION_SUBTYPE_DATA_FAT && label != NULL &&
                strcmp(label, "apps") == 0;
    return apps && has_apps_partition ? &APPS_PARTITION : NULL;
}

/* ---- The FAT mount: the directory, which a "format" makes ---- */

static int mounts;
static esp_err_t mount_answer = ESP_OK;
static esp_vfs_fat_mount_config_t mount_cfg;
static char mount_base[64], mount_label[24];
static uint64_t fat_free_bytes = (uint64_t)6 << 20;

esp_err_t esp_vfs_fat_spiflash_mount_rw_wl(const char *base_path, const char *partition_label,
                                           const esp_vfs_fat_mount_config_t *mount_config, wl_handle_t *wl_handle) {
    mounts++;
    snprintf(mount_base, sizeof mount_base, "%s", base_path);
    snprintf(mount_label, sizeof mount_label, "%s", partition_label);
    mount_cfg = *mount_config;
    if (mount_answer != ESP_OK) {
        return mount_answer;
    }
    struct stat st;
    if (stat(base_path, &st) != 0) {
        if (!mount_config->format_if_mount_failed) {
            return ESP_FAIL;  /* no file system, and no format asked for */
        }
        if (mkdir(base_path, 0777) != 0) {
            return ESP_FAIL;
        }
    }
    *wl_handle = 3;
    return ESP_OK;
}

esp_err_t esp_vfs_fat_spiflash_unmount_rw_wl(const char *base_path, wl_handle_t wl_handle) {
    (void)base_path;
    (void)wl_handle;
    return ESP_OK;
}

esp_err_t esp_vfs_fat_info(const char *base_path, uint64_t *out_total_bytes, uint64_t *out_free_bytes) {
    (void)base_path;
    *out_total_bytes = (uint64_t)0x5f0000;
    *out_free_bytes = fat_free_bytes;
    return ESP_OK;
}

/* ---- NVS: a map in memory for each partition; the pairing's "nvs" is never touched ---- */

#define NVS_ITEMS 96
typedef struct {
    bool used;
    char part[17], ns[16], key[16];
    uint8_t *blob;
    size_t len;
} nvs_item_t;

static nvs_item_t nvs_items[NVS_ITEMS];
static size_t nvs_capacity = 60000;  /* the bytes a partition's blobs may take, with 32 a key, before it is full */
static char nvs_fail_key[16];        /* a read of this key fails with nvs_fail_err (an error that passes: the blob is there) */
static esp_err_t nvs_fail_err = ESP_FAIL;
static esp_err_t init_answers[4];
static int n_init_answers, init_calls, erase_calls, opens, commits, blob_writes;
static char erased[4][17], opened_part[17], opened_ns[16];

static void pairing_touched(const char *what) {
    CHECK(false, "%s touched the pairing's NVS (\"nvs\"): only apps_nvs is the store's", what);
}

esp_err_t nvs_flash_init(void) {
    pairing_touched("nvs_flash_init()");
    return ESP_FAIL;
}

esp_err_t nvs_flash_erase(void) {
    pairing_touched("nvs_flash_erase()");
    return ESP_FAIL;
}

esp_err_t nvs_flash_init_partition(const char *part_name) {
    if (strcmp(part_name, "apps_nvs") != 0) {
        pairing_touched("nvs_flash_init_partition");
        return ESP_FAIL;
    }
    init_calls++;
    return init_calls <= n_init_answers ? init_answers[init_calls - 1] : ESP_OK;
}

esp_err_t nvs_flash_erase_partition(const char *part_name) {
    if (strcmp(part_name, "apps_nvs") != 0) {
        pairing_touched("nvs_flash_erase_partition");
        return ESP_FAIL;
    }
    snprintf(erased[erase_calls++ % 4], sizeof erased[0], "%s", part_name);
    for (int i = 0; i < NVS_ITEMS; i++) {
        if (nvs_items[i].used && strcmp(nvs_items[i].part, part_name) == 0) {
            free(nvs_items[i].blob);
            memset(&nvs_items[i], 0, sizeof nvs_items[i]);
        }
    }
    return ESP_OK;
}

esp_err_t nvs_open(const char *namespace_name, nvs_open_mode_t open_mode, nvs_handle_t *out_handle) {
    (void)namespace_name;
    (void)open_mode;
    (void)out_handle;
    pairing_touched("nvs_open");
    return ESP_FAIL;
}

esp_err_t nvs_open_from_partition(const char *part_name, const char *namespace_name, nvs_open_mode_t open_mode,
                                  nvs_handle_t *out_handle) {
    if (strcmp(part_name, "apps_nvs") != 0) {
        pairing_touched("nvs_open_from_partition");
        return ESP_FAIL;
    }
    CHECK(open_mode == NVS_READWRITE, "the store's namespace is opened to write");
    opens++;
    snprintf(opened_part, sizeof opened_part, "%s", part_name);
    snprintf(opened_ns, sizeof opened_ns, "%s", namespace_name);
    *out_handle = 1;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) {
    (void)handle;
}

static nvs_item_t *nvs_find(const char *part, const char *ns, const char *key) {
    for (int i = 0; i < NVS_ITEMS; i++) {
        nvs_item_t *it = &nvs_items[i];
        if (it->used && strcmp(it->part, part) == 0 && strcmp(it->ns, ns) == 0 && strcmp(it->key, key) == 0) {
            return it;
        }
    }
    return NULL;
}

static size_t nvs_bytes(const char *part) {
    size_t n = 0;
    for (int i = 0; i < NVS_ITEMS; i++) {
        if (nvs_items[i].used && strcmp(nvs_items[i].part, part) == 0) {
            n += nvs_items[i].len + 32;
        }
    }
    return n;
}

static esp_err_t nvs_put_raw(const char *part, const char *ns, const char *key, const void *value, size_t len) {
    if (strlen(key) >= NVS_KEY_NAME_MAX_SIZE) {
        CHECK(false, "an NVS key of %zu characters: '%s'", strlen(key), key);
        return ESP_FAIL;
    }
    nvs_item_t *it = nvs_find(part, ns, key);
    size_t after = nvs_bytes(part) - (it != NULL ? it->len + 32 : 0) + len + 32;
    if (after > nvs_capacity) {
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    }
    for (int i = 0; it == NULL && i < NVS_ITEMS; i++) {
        it = nvs_items[i].used ? NULL : &nvs_items[i];
    }
    if (it == NULL) {
        return ESP_ERR_NVS_NOT_ENOUGH_SPACE;
    }
    free(it->blob);
    it->used = true;
    snprintf(it->part, sizeof it->part, "%s", part);
    snprintf(it->ns, sizeof it->ns, "%s", ns);
    snprintf(it->key, sizeof it->key, "%s", key);
    it->blob = malloc(len > 0 ? len : 1);
    memcpy(it->blob, value, len);
    it->len = len;
    return ESP_OK;
}

esp_err_t nvs_set_blob(nvs_handle_t handle, const char *key, const void *value, size_t length) {
    (void)handle;
    blob_writes++;
    return nvs_put_raw(opened_part, opened_ns, key, value, length);
}

esp_err_t nvs_get_blob(nvs_handle_t handle, const char *key, void *out_value, size_t *length) {
    (void)handle;
    if (nvs_fail_key[0] != '\0' && strcmp(key, nvs_fail_key) == 0) {
        return nvs_fail_err;
    }
    nvs_item_t *it = nvs_find(opened_part, opened_ns, key);
    if (it == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    if (out_value == NULL) {
        *length = it->len;
        return ESP_OK;
    }
    if (*length < it->len) {
        return ESP_ERR_NVS_INVALID_LENGTH;
    }
    memcpy(out_value, it->blob, it->len);
    *length = it->len;
    return ESP_OK;
}

esp_err_t nvs_erase_key(nvs_handle_t handle, const char *key) {
    (void)handle;
    nvs_item_t *it = nvs_find(opened_part, opened_ns, key);
    if (it == NULL) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    free(it->blob);
    memset(it, 0, sizeof *it);
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle) {
    (void)handle;
    commits++;
    return ESP_OK;
}

esp_err_t nvs_get_stats(const char *part_name, nvs_stats_t *st) {
    if (strcmp(part_name, "apps_nvs") != 0) {
        pairing_touched("nvs_get_stats");
        return ESP_FAIL;
    }
    size_t used = 0;
    for (int i = 0; i < NVS_ITEMS; i++) {
        if (nvs_items[i].used && strcmp(nvs_items[i].part, part_name) == 0) {
            used += 1 + (nvs_items[i].len + 31) / 32;
        }
    }
    st->total_entries = 16 * 126;
    st->used_entries = used + 1;  /* the namespace's own entry */
    st->free_entries = st->total_entries - st->used_entries;
    st->available_entries = st->free_entries > 126 ? st->free_entries - 126 : 0;
    st->namespace_count = 1;
    return ESP_OK;
}

struct nvs_opaque_iterator_t {
    int at;
    char part[17], ns[16];
};

static int nvs_next_from(const struct nvs_opaque_iterator_t *it, int from) {
    for (int i = from; i < NVS_ITEMS; i++) {
        if (nvs_items[i].used && strcmp(nvs_items[i].part, it->part) == 0 && strcmp(nvs_items[i].ns, it->ns) == 0) {
            return i;
        }
    }
    return -1;
}

esp_err_t nvs_entry_find(const char *part_name, const char *namespace_name, nvs_type_t type,
                         nvs_iterator_t *output_iterator) {
    if (strcmp(part_name, "apps_nvs") != 0) {
        pairing_touched("nvs_entry_find");
        return ESP_FAIL;
    }
    CHECK(type == NVS_TYPE_BLOB || type == NVS_TYPE_ANY, "the store keeps blobs");
    struct nvs_opaque_iterator_t probe = {0};
    snprintf(probe.part, sizeof probe.part, "%s", part_name);
    snprintf(probe.ns, sizeof probe.ns, "%s", namespace_name);
    probe.at = nvs_next_from(&probe, 0);
    if (probe.at < 0) {
        *output_iterator = NULL;
        return ESP_ERR_NVS_NOT_FOUND;
    }
    *output_iterator = malloc(sizeof probe);
    **output_iterator = probe;
    return ESP_OK;
}

esp_err_t nvs_entry_next(nvs_iterator_t *iterator) {
    int next = nvs_next_from(*iterator, (*iterator)->at + 1);
    if (next < 0) {
        free(*iterator);
        *iterator = NULL;
        return ESP_ERR_NVS_NOT_FOUND;
    }
    (*iterator)->at = next;
    return ESP_OK;
}

esp_err_t nvs_entry_info(const nvs_iterator_t iterator, nvs_entry_info_t *out_info) {
    const nvs_item_t *item = &nvs_items[iterator->at];
    snprintf(out_info->namespace_name, sizeof out_info->namespace_name, "%s", item->ns);
    snprintf(out_info->key, sizeof out_info->key, "%s", item->key);
    out_info->type = NVS_TYPE_BLOB;
    return ESP_OK;
}

void nvs_release_iterator(nvs_iterator_t iterator) {
    free(iterator);
}

const char *esp_err_to_name(esp_err_t err) {
    switch (err) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NVS_NO_FREE_PAGES: return "ESP_ERR_NVS_NO_FREE_PAGES";
    case ESP_ERR_NVS_NEW_VERSION_FOUND: return "ESP_ERR_NVS_NEW_VERSION_FOUND";
    case ESP_ERR_NVS_NOT_ENOUGH_SPACE: return "ESP_ERR_NVS_NOT_ENOUGH_SPACE";
    case ESP_ERR_NVS_NOT_FOUND: return "ESP_ERR_NVS_NOT_FOUND";
    default: return "ERROR";
    }
}

/* The keys, as the brief has them: the CRC-32 (zlib's) of the id, or of "id/name" for an alarm. */
static uint32_t crc32_of(const char *s) {
    uint32_t crc = 0xffffffffu;
    for (; *s != '\0'; s++) {
        crc ^= (uint8_t)*s;
        for (int k = 0; k < 8; k++) {
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1u)));
        }
    }
    return ~crc;
}

/* The key of probe k (0 for the CRC-32's own, then +1, +2, ...: apps_store.h's probes). */
static void key_at(char out[16], char kind, const char *id, const char *name, unsigned k) {
    char what[64];
    snprintf(what, sizeof what, name != NULL ? "%s/%s" : "%s", id, name);
    snprintf(out, 16, "%c%08x", kind, (unsigned)(crc32_of(what) + k));
}

static void key_of(char out[16], char kind, const char *id, const char *name) {
    key_at(out, kind, id, name, 0);
}

/* Whether the blob of an item is (id[, name])'s: the id and a NUL first, and for a record the name in its field. */
static bool item_is(const nvs_item_t *it, char kind, const char *id, const char *name) {
    size_t n = strlen(id);
    if (it->len <= n || memcmp(it->blob, id, n) != 0 || it->blob[n] != '\0') {
        return false;
    }
    return kind != 'a' || (it->len > APPS_ID_MAX + 1 + strlen(name) && strcmp((const char *)it->blob + APPS_ID_MAX + 1, name) == 0);
}

/* The item of (kind, id[, name]): the first of its probes whose blob is its own (NULL when none is). */
static const nvs_item_t *apps_key(char kind, const char *id, const char *name) {
    for (unsigned k = 0; k < 8; k++) {
        char key[16];
        key_at(key, kind, id, name, k);
        const nvs_item_t *it = nvs_find("apps_nvs", "apps", key);
        if (it != NULL && item_is(it, kind, id, name)) {
            return it;
        }
    }
    return NULL;
}

/* The probe its own key is at, or -1. */
static int apps_key_probe(char kind, const char *id, const char *name) {
    const nvs_item_t *it = apps_key(kind, id, name);
    for (unsigned k = 0; it != NULL && k < 8; k++) {
        char key[16];
        key_at(key, kind, id, name, k);
        if (strcmp(it->key, key) == 0) {
            return (int)k;
        }
    }
    return -1;
}

/* An alarm's record as the brief has it: the id and the name (each NUL-padded to its field), the flags (bit 0 sound, bit 1
 * wake), then the epoch ms, little-endian. */
#define RECORD_LEN (APPS_ID_MAX + 1 + APPS_ALARM_NAME_MAX + 1 + 1 + 8)

static void put_record_at(unsigned k, const char *id, const char *name, int64_t epoch_ms, bool sound, bool wake) {
    uint8_t rec[RECORD_LEN] = {0};
    memcpy(rec, id, strlen(id));
    memcpy(rec + APPS_ID_MAX + 1, name, strlen(name));
    rec[APPS_ID_MAX + 1 + APPS_ALARM_NAME_MAX + 1] = (uint8_t)((sound ? 1 : 0) | (wake ? 2 : 0));
    for (int i = 0; i < 8; i++) {
        rec[RECORD_LEN - 8 + i] = (uint8_t)((uint64_t)epoch_ms >> (8 * i));
    }
    char key[16];
    key_at(key, 'a', id, name, k);
    CHECK(nvs_put_raw("apps_nvs", "apps", key, rec, sizeof rec) == ESP_OK, "a record");
}

static void put_record(const char *id, const char *name, int64_t epoch_ms, bool sound, bool wake) {
    put_record_at(0, id, name, epoch_ms, sound, wake);
}

static void put_values(const char *id, const char *json) {
    char key[16], blob[256];
    key_of(key, 's', id, NULL);
    size_t n = strlen(id) + 1 + strlen(json);
    memcpy(blob, id, strlen(id) + 1);
    memcpy(blob + strlen(id) + 1, json, strlen(json));
    CHECK(nvs_put_raw("apps_nvs", "apps", key, blob, n) == ESP_OK, "values");
}

/* A record's time and flags (bit 0 sound, bit 1 wake); -1 for no record. */
static int64_t record_epoch(const char *id, const char *name) {
    const nvs_item_t *rec = apps_key('a', id, name);
    if (rec == NULL || rec->len != RECORD_LEN) {
        return -1;
    }
    int64_t epoch = 0;
    for (int i = 0; i < 8; i++) {
        epoch |= (int64_t)rec->blob[RECORD_LEN - 8 + i] << (8 * i);
    }
    return epoch;
}

static int record_flags(const char *id, const char *name) {
    const nvs_item_t *rec = apps_key('a', id, name);
    return rec != NULL && rec->len == RECORD_LEN ? rec->blob[APPS_ID_MAX + 1 + APPS_ALARM_NAME_MAX + 1] : -1;
}

/* One name an app touched, as apps_values.c hands it to apps_store_sync_alarms. */
static apps_store_name_t touched(const char *name, int state, int64_t epoch_ms, bool sound, bool wake) {
    apps_store_name_t a;
    memset(&a, 0, sizeof a);
    snprintf(a.name, sizeof a.name, "%s", name);
    a.state = (uint8_t)state;
    a.epoch_ms = epoch_ms;
    a.sound = sound;
    a.wake = wake;
    return a;
}

/* A record written as the app task's sync writes one (apps_store_sync_alarms, the name armed with its time): true when it is there now, as
 * asked. */
static bool saved(const char *id, const char *name, int64_t epoch_ms, bool sound, bool wake) {
    apps_store_name_t a = touched(name, APPS_STORE_ARMED, epoch_ms, sound, wake);
    apps_store_sync_alarms(id, &a, 1, false);
    return record_epoch(id, name) == epoch_ms && record_flags(id, name) == ((sound ? 1 : 0) | (wake ? 2 : 0));
}

/* ---- The reset reason ---- */

static esp_reset_reason_t reset_reason = ESP_RST_POWERON;

esp_reset_reason_t esp_reset_reason(void) {
    return reset_reason;
}

/* ---- The RAM store (apps_install.h): copies kept here, most recent first ---- */

#define RAM_KEPT 8
static apps_pkg_t *ram_kept[RAM_KEPT];
static int ram_n, ram_commits, ram_loads, ram_removes;

static char *dup_block(const void *data, size_t n) {
    char *b = apps_port_realloc(NULL, n > 0 ? n : 1);
    if (b != NULL && n > 0) {
        memcpy(b, data, n);
    }
    return b;
}

static apps_pkg_t *pkg_copy(const apps_pkg_t *src) {
    apps_pkg_t *p = apps_port_realloc(NULL, sizeof *p);
    memcpy(p, src, sizeof *p);
    p->manifest.messages_json =
        src->manifest.messages_json != NULL ? dup_block(src->manifest.messages_json, strlen(src->manifest.messages_json) + 1) : NULL;
    p->manifest_json = src->manifest_json != NULL ? dup_block(src->manifest_json, src->manifest_len) : NULL;
    for (int i = 0; i < APPS_MAX_FILES; i++) {
        p->files[i].data = i < src->count ? (uint8_t *)dup_block(src->files[i].data, src->files[i].len) : NULL;
    }
    return p;
}

static void free_pkg(apps_pkg_t *p) {
    if (p != NULL) {
        apps_pkg_free(p, apps_port_free);
        apps_port_free(p);
    }
}

static int ram_find(const char *id) {
    for (int i = 0; i < ram_n; i++) {
        if (strcmp(ram_kept[i]->manifest.id, id) == 0) {
            return i;
        }
    }
    return -1;
}

bool apps_ram_commit(const apps_pkg_t *pkg, bool *unchanged, char *err, size_t err_len) {
    (void)err;
    (void)err_len;
    ram_commits++;
    *unchanged = false;
    int i = ram_find(pkg->manifest.id);
    if (i >= 0) {
        free_pkg(ram_kept[i]);
        memmove(&ram_kept[i], &ram_kept[i + 1], (size_t)(ram_n - i - 1) * sizeof ram_kept[0]);
        ram_n--;
    }
    if (ram_n == RAM_KEPT) {
        free_pkg(ram_kept[--ram_n]);
    }
    memmove(&ram_kept[1], &ram_kept[0], (size_t)ram_n * sizeof ram_kept[0]);
    ram_kept[0] = pkg_copy(pkg);
    ram_n++;
    return true;
}

bool apps_ram_load(const char *id, apps_pkg_t *out, char *err, size_t err_len) {
    ram_loads++;
    int i = ram_find(id);
    if (i < 0) {
        snprintf(err, err_len, "no such app on the gadget");
        memset(out, 0, sizeof *out);
        return false;
    }
    apps_pkg_t *copy = pkg_copy(ram_kept[i]);
    memcpy(out, copy, sizeof *out);
    apps_port_free(copy);  /* the struct: its blocks are out's now */
    return true;
}

bool apps_ram_has(const char *id) {
    return ram_find(id) >= 0;
}

bool apps_ram_remove(const char *id) {
    ram_removes++;
    int i = ram_find(id);
    if (i < 0) {
        return false;
    }
    free_pkg(ram_kept[i]);
    memmove(&ram_kept[i], &ram_kept[i + 1], (size_t)(ram_n - i - 1) * sizeof ram_kept[0]);
    ram_n--;
    return true;
}

int apps_ram_list(apps_ram_entry_t *out, int max) {
    int n = 0;
    for (; n < ram_n && n < max; n++) {
        snprintf(out[n].id, sizeof out[n].id, "%s", ram_kept[n]->manifest.id);
        snprintf(out[n].name, sizeof out[n].name, "%s", ram_kept[n]->manifest.name);
        snprintf(out[n].version, sizeof out[n].version, "%s", ram_kept[n]->manifest.version);
        snprintf(out[n].icon, sizeof out[n].icon, "%s", ram_kept[n]->manifest.icon);
    }
    return n;
}

bool apps_ram_manifest(const char *id, apps_manifest_t *m) {
    int i = ram_find(id);
    if (i < 0) {
        memset(m, 0, sizeof *m);
        return false;
    }
    *m = ram_kept[i]->manifest;
    const char *messages = ram_kept[i]->manifest.messages_json;
    m->messages_json = messages != NULL ? dup_block(messages, strlen(messages) + 1) : NULL;
    return true;
}

/* ---- The app task's: the launcher's list, and the app that runs ---- */

static int list_publishes, listed_n;
static apps_listed_t listed[APPS_TASK_LIST_MAX];
static char running_id[APPS_ID_MAX + 1];

void apps_task_set_list(const apps_listed_t *apps, int n) {
    list_publishes++;
    listed_n = n;
    for (int i = 0; i < n && i < APPS_TASK_LIST_MAX; i++) {
        listed[i] = apps[i];
    }
}

bool apps_task_running(char *id, size_t cap) {
    snprintf(id, cap, "%s", running_id);
    return running_id[0] != '\0';
}

/* The app that is loaded and what its end frees (apps_task_footprint): a case sets them; "" for none. */
static char footprint_id[APPS_ID_MAX + 1];
static size_t footprint_bytes;

bool apps_task_footprint(char *id, size_t cap, size_t *bytes) {
    snprintf(id, cap, "%s", footprint_id);
    *bytes = footprint_id[0] != '\0' ? footprint_bytes : 0;
    return footprint_id[0] != '\0';
}

/* ---- apps_values.h: the RAM map, asked to forget an app that goes, and the write budget, which a case can say is spent ---- */

static char forgotten[APPS_ID_MAX + 1];

void apps_values_forget(const char *app) {
    snprintf(forgotten, sizeof forgotten, "%s", app);
}

static bool budget_spent;     /* nothing fits until a case says otherwise */
static int budget_takes, budget_forced;
static uint32_t budget_bytes;  /* what was taken */

bool apps_values_budget_take(uint32_t bytes, bool force) {
    budget_takes++;
    if (budget_spent && !force) {
        return false;
    }
    budget_forced += force ? 1 : 0;
    budget_bytes += bytes;
    return true;
}

int64_t apps_values_budget_wait_ms(uint32_t bytes) {
    (void)bytes;
    return budget_spent ? 60000 : 0;
}

/* The notes the store leaves the app task: a record that had to wait for the budget ("<id>/<name>", in order), and an app whose records the
 * app task forgets (a remove's, a quarantine's). */
static char waits_noted[8][APPS_ID_MAX + APPS_ALARM_NAME_MAX + 2];
static int n_waits_noted;
static char records_forgotten[APPS_ID_MAX + 1];
static int forget_calls;

void apps_values_record_waits(const char *app, const char *name) {
    if (n_waits_noted < (int)(sizeof waits_noted / sizeof waits_noted[0])) {
        snprintf(waits_noted[n_waits_noted], sizeof waits_noted[0], "%s/%s", app, name);
    }
    n_waits_noted++;
}

void apps_values_forget_records(const char *app) {
    snprintf(records_forgotten, sizeof records_forgotten, "%s", app);
    forget_calls++;
}

/* ---- The system's alarms: the store cancels an app's (a remove, a quarantine) and arms none (the app task re-arms the records) ---- */

static int n_arms;
static char cancelled_app[APPS_ID_MAX + 1];

apps_alarm_result_t apps_alarm_arm(const char *app, const char *name, int64_t ms, bool sound, bool wake) {
    (void)app;
    (void)name;
    (void)ms;
    (void)sound;
    (void)wake;
    n_arms++;
    return APPS_ALARM_ARMED;
}

void apps_alarm_cancel_app(const char *app, const char *why) {
    (void)why;
    snprintf(cancelled_app, sizeof cancelled_app, "%s", app);
}

/* ---- The I/O worker: jobs wait here for the case to run them ---- */

#define JOBS 8
static void (*jobs[JOBS])(void *arg);
static void *job_args[JOBS];
static int n_jobs, submits;
static bool submit_refuses;

bool apps_io_submit(void (*fn)(void *arg), void *arg) {
    submits++;
    if (submit_refuses || n_jobs == JOBS) {
        return false;
    }
    jobs[n_jobs] = fn;
    job_args[n_jobs] = arg;
    n_jobs++;
    return true;
}

static void run_jobs(void) {
    while (n_jobs > 0) {
        void (*fn)(void *) = jobs[0];
        void *arg = job_args[0];
        memmove(&jobs[0], &jobs[1], (size_t)(n_jobs - 1) * sizeof jobs[0]);
        memmove(&job_args[0], &job_args[1], (size_t)(n_jobs - 1) * sizeof job_args[0]);
        n_jobs--;
        fn(arg);
    }
}

/* ---- Packages and files ---- */

typedef struct {
    const char *path;
    const char *text;
} file_t;

/* A package as the install pipeline hands it to the store: the manifest's own bytes, its parse, and the files, main first,
 * each with its SHA-256; every block from apps_port_realloc. files[0] is main.lua. */
static apps_pkg_t *make_pkg(const char *id, const char *name, const char *version, int n, const file_t *files) {
    char json[1024];
    int at = snprintf(json, sizeof json, "{\"id\":\"%s\",\"name\":\"%s\",\"version\":\"%s\",\"api\":1,\"files\":[", id, name,
                      version);
    for (int i = 1; i < n; i++) {
        at += snprintf(json + at, sizeof json - (size_t)at, "%s\"%s\"", i > 1 ? "," : "", files[i].path);
    }
    at += snprintf(json + at, sizeof json - (size_t)at, "],\"messages\":{\"start\":\"Starts it.\"}}");
    apps_pkg_t *p = apps_port_realloc(NULL, sizeof *p);
    memset(p, 0, sizeof *p);
    char err[256];
    bool ok = apps_manifest_parse(json, (size_t)at, id, 1024, &p->manifest, err, sizeof err);
    CHECK(ok, "the harness's manifest: %s", err);
    char *messages = p->manifest.messages_json;
    p->manifest.messages_json = messages != NULL ? dup_block(messages, strlen(messages) + 1) : NULL;
    free(messages);
    p->manifest_json = dup_block(json, (size_t)at);
    p->manifest_len = (size_t)at;
    for (int i = 0; i < n; i++) {
        apps_blob_t *b = &p->files[p->count++];
        snprintf(b->path, sizeof b->path, "%s", files[i].path);
        b->len = strlen(files[i].text);
        b->data = (uint8_t *)dup_block(files[i].text, b->len);
        apps_port_sha256(b->data, b->len, b->sha256);
    }
    return p;
}

static bool exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* The bytes of a file, NUL-terminated, malloc'd; NULL when there is none. */
static char *slurp(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        return NULL;
    }
    char *buf = malloc(65536);
    size_t n = fread(buf, 1, 65535, f);
    fclose(f);
    buf[n] = '\0';
    if (len != NULL) {
        *len = n;
    }
    return buf;
}

static bool file_is(const char *path, const char *text) {
    char *got = slurp(path, NULL);
    bool same = got != NULL && strcmp(got, text) == 0;
    if (!same) {
        fprintf(stderr, "  %s holds '%s', not '%s'\n", path, got != NULL ? got : "(no file)", text);
    }
    free(got);
    return same;
}

static void make_dirs_for(const char *path) {
    char dir[256];
    snprintf(dir, sizeof dir, "%s", path);
    for (char *p = dir + 1; *p != '\0'; p++) {
        if (*p == '/') {
            *p = '\0';
            mkdir(dir, 0777);
            *p = '/';
        }
    }
}

static void spit(const char *path, const char *text) {
    make_dirs_for(path);
    FILE *f = fopen(path, "wb");
    CHECK(f != NULL, "can't write %s: %s", path, strerror(errno));
    if (f != NULL) {
        fputs(text, f);
        fclose(f);
    }
}

/* An app's folder as the store writes one: dir/manifest.json and dir/main.lua (and the files the manifest lists, if extra). */
static void put_app(const char *dir, const char *id, const char *name, const char *main_text) {
    char path[256], json[512];
    snprintf(path, sizeof path, "%s/manifest.json", dir);
    snprintf(json, sizeof json, "{\"id\":\"%s\",\"name\":\"%s\",\"version\":\"1.0.0\",\"api\":1}", id, name);
    spit(path, json);
    snprintf(path, sizeof path, "%s/main.lua", dir);
    spit(path, main_text);
}

static void set_mtime(const char *path, time_t when) {
    struct timeval tv[2] = {{when, 0}, {when, 0}};
    CHECK(utimes(path, tv) == 0, "utimes %s", path);
}

static int list_ids(char ids[][APPS_ID_MAX + 1], char names[][25], int max) {
    return apps_store_list(ids, names, max);
}

/* ---- The cases: the brief's ---- */

/* A commit writes the stage and puts it in place of the app's folder, whole: files the new version doesn't have go. */
static void commit_creates_and_replaces(void) {
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "the partition is there: flash");
    long held = blocks_held;
    const file_t v1[] = {{"main.lua", "-- v1\n"}, {"util.lua", "return 1\n"}, {"lib/deep.lua", "return 2\n"}};
    apps_pkg_t *p = make_pkg("timer", "Timer", "1.0.0", 3, v1);
    bool unchanged = true;
    char err[256] = "?";
    CHECK(apps_store_commit(p, &unchanged, err, sizeof err) && !unchanged && err[0] == '\0', "commit v1: '%s'", err);
    CHECK(file_is("apps_fs/timer/main.lua", "-- v1\n") && file_is("apps_fs/timer/util.lua", "return 1\n") &&
              file_is("apps_fs/timer/lib/deep.lua", "return 2\n"),
          "v1's files");
    char *manifest = slurp("apps_fs/timer/manifest.json", NULL);
    CHECK(manifest != NULL && strlen(manifest) == p->manifest_len && memcmp(manifest, p->manifest_json, p->manifest_len) == 0,
          "manifest.json is the bytes installed: '%s'", manifest != NULL ? manifest : "(none)");
    free(manifest);
    CHECK(!exists("apps_fs/.old-timer") && !exists("apps_fs/.stage-timer") && !exists("apps_fs/.stage"),
          "nothing is left of the commit but the app");
    char ids[4][APPS_ID_MAX + 1], names[4][25];
    CHECK(list_ids(ids, names, 4) == 1 && strcmp(ids[0], "timer") == 0 && strcmp(names[0], "Timer") == 0, "listed");
    CHECK(apps_store_has("timer") && !apps_store_has("clock"), "has");
    CHECK(listed_n == 1 && strcmp(listed[0].id, "timer") == 0, "the launcher was given the list");
    free_pkg(p);

    const file_t v2[] = {{"main.lua", "-- v2\n"}};
    p = make_pkg("timer", "Timer Two", "2.0.0", 1, v2);
    CHECK(apps_store_commit(p, &unchanged, err, sizeof err) && !unchanged, "commit v2: %s", err);
    CHECK(file_is("apps_fs/timer/main.lua", "-- v2\n"), "v2's main.lua");
    CHECK(!exists("apps_fs/timer/util.lua") && !exists("apps_fs/timer/lib"), "v1's other files went with its folder");
    CHECK(!exists("apps_fs/.old-timer") && !exists("apps_fs/.stage-timer"), "the old copy is deleted");
    CHECK(list_ids(ids, names, 4) == 1 && strcmp(names[0], "Timer Two") == 0, "the list has the new name: %s", names[0]);

    apps_pkg_t *got = apps_port_realloc(NULL, sizeof *got);
    CHECK(apps_store_load("timer", got, err, sizeof err), "load: %s", err);
    CHECK(strcmp(got->manifest.version, "2.0.0") == 0 && got->count == 1 && strcmp(got->files[0].path, "main.lua") == 0 &&
              got->files[0].len == 6 && memcmp(got->files[0].data, "-- v2\n", 6) == 0,
          "the load is v2");
    CHECK(memcmp(got->files[0].sha256, p->files[0].sha256, 32) == 0, "with its SHA-256");
    CHECK(got->manifest_len == p->manifest_len && memcmp(got->manifest_json, p->manifest_json, p->manifest_len) == 0,
          "and the manifest's bytes");
    CHECK(got->manifest.messages_json != NULL && strstr(got->manifest.messages_json, "Starts it.") != NULL, "messages");
    free_pkg(got);
    free_pkg(p);
    CHECK(blocks_held == held, "%ld blocks of the apps' heap still held", blocks_held - held);
}

/* Power lost after the old copy was moved aside and before the stage took its place: the old copy comes back, and the stage
 * goes (an install whose end isn't known is no install). The stage is the app's sibling, .stage-<id> (P4). */
static void recovery_restores_old_when_app_missing(void) {
    put_app("apps_fs/.old-timer", "timer", "Timer", "-- old\n");
    put_app("apps_fs/.stage-timer", "timer", "Timer", "-- new\n");
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    CHECK(file_is("apps_fs/timer/main.lua", "-- old\n"), "the old copy is back");
    CHECK(!exists("apps_fs/.old-timer") && !exists("apps_fs/.stage-timer"), "and nothing else is left");
    CHECK(apps_store_has("timer"), "listed");
    CHECK(logged_has("store: restored timer from an interrupted install"), "said");
}

/* Strays: an unfinished stage (power lost while it was written), another app's stage, and an old copy whose app is in place (power
 * lost after the stage took its place, or while the old copy was being deleted) all go; the app stays as it is. */
static void recovery_deletes_stray_stage(void) {
    put_app("apps_fs/timer", "timer", "Timer", "-- current\n");
    spit("apps_fs/.stage-timer/manifest.json", "{\"id\":");
    spit("apps_fs/.stage-other/lib/x.lua", "x");
    put_app("apps_fs/.old-timer", "timer", "Timer", "-- previous\n");
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    CHECK(file_is("apps_fs/timer/main.lua", "-- current\n"), "the app is as it was");
    CHECK(!exists("apps_fs/.stage-timer") && !exists("apps_fs/.stage-other") && !exists("apps_fs/.old-timer"), "strays gone");
    CHECK(logged_has("store: removed the unfinished install of timer") && logged_has("store: removed the unfinished install of other"),
          "each stage is said");
    char ids[4][APPS_ID_MAX + 1], names[4][25];
    CHECK(list_ids(ids, names, 4) == 1, "one app");
    CHECK(logged_has("store: removed a stray copy of timer"), "the old copy is said");
}

/* The stage of the layout before P4, /apps/.stage/<id> (the board has one), is a leftover like any other: it goes at the mount, all of
 * it, and the app stays. */
static void old_layout_stage_is_deleted(void) {
    put_app("apps_fs/timer", "timer", "Timer", "-- current\n");
    put_app("apps_fs/.stage/timer", "timer", "Timer", "-- half\n");
    spit("apps_fs/.stage/clock/lib/deep.lua", "x");
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    CHECK(!exists("apps_fs/.stage"), "the old stage's folder went, with all it had");
    CHECK(file_is("apps_fs/timer/main.lua", "-- current\n") && apps_store_has("timer") && !apps_store_has("clock"), "the app stays");
    CHECK(logged_has("store: removed the unfinished install of timer") && logged_has("store: removed the unfinished install of clock") &&
              logged_has("store: removed the .stage folder of an earlier layout"),
          "said");
    /* The board's: an empty .stage, as a whole commit of that layout left it. */
    CHECK(mkdir("apps_fs/.stage", 0755) == 0, "an empty old stage");
    n_logged = 0;
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    CHECK(!exists("apps_fs/.stage") && logged_has("store: removed the .stage folder of an earlier layout") &&
              !logged_has("unfinished install"),
          "it goes too, with its one line");
}

/* A remove takes the folder and every key of the app (values, alarm records, quarantine), and nothing of another app's; it is
 * refused for the app that runs. */
static void remove_app(void) {
    clock_set = true;
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    const file_t files[] = {{"main.lua", "-- main\n"}};
    apps_pkg_t *timer = make_pkg("timer", "Timer", "1", 1, files), *clock = make_pkg("clock", "Clock", "1", 1, files);
    bool unchanged;
    char err[256];
    CHECK(apps_store_commit(timer, &unchanged, err, sizeof err) && apps_store_commit(clock, &unchanged, err, sizeof err), "%s", err);
    CHECK(apps_store_save_values("timer", "{\"a\":1}", 7) && apps_store_save_values("clock", "{\"b\":2}", 7), "values");
    CHECK(saved("timer", "done", epoch_now + 60000, true, true), "a record");
    CHECK(saved("clock", "tick", epoch_now + 60000, false, true), "another app's record");
    char key[16];
    key_of(key, 'q', "timer", NULL);
    CHECK(nvs_put_raw("apps_nvs", "apps", key, "timer", 6) == ESP_OK, "a quarantine");
    snprintf(running_id, sizeof running_id, "timer");
    CHECK(!apps_store_remove("timer", err, sizeof err) && strcmp(err, "timer is running; stop it first") == 0, "refused: '%s'", err);
    CHECK(exists("apps_fs/timer/main.lua") && apps_key('s', "timer", NULL) != NULL, "and nothing went");
    running_id[0] = '\0';
    cancelled_app[0] = '\0';
    CHECK(apps_store_remove("timer", err, sizeof err), "remove: %s", err);
    CHECK(!exists("apps_fs/timer"), "the folder went");
    CHECK(strcmp(cancelled_app, "timer") == 0, "its armed alarms are cancelled: '%s'", cancelled_app);
    CHECK(forget_calls == 1 && strcmp(records_forgotten, "timer") == 0, "and the app task forgets the records it was handed (#2): %d, '%s'",
          forget_calls, records_forgotten);
    CHECK(apps_key('s', "timer", NULL) == NULL && apps_key('a', "timer", "done") == NULL && apps_key('q', "timer", NULL) == NULL,
          "its keys went");
    CHECK(apps_key('s', "clock", NULL) != NULL && apps_key('a', "clock", "tick") != NULL && exists("apps_fs/clock/main.lua"),
          "the other app's stay");
    CHECK(!apps_store_has("timer") && apps_store_has("clock") && listed_n == 1 && strcmp(listed[0].id, "clock") == 0,
          "the list and the launcher's");
    const nvs_item_t *order = nvs_find("apps_nvs", "apps", "order");
    CHECK(order != NULL && order->len == 6 && memcmp(order->blob, "clock\0", 6) == 0, "and its place in the order went");
    CHECK(!apps_store_remove("timer", err, sizeof err) && strcmp(err, "no such app on the gadget") == 0, "again: '%s'", err);
    free_pkg(timer);
    free_pkg(clock);
}

/* The list reads each folder's manifest.json: its name; with no order kept yet (P2) the apps go by id, whatever times the folders
 * have (FAT's come from a clock that may not be set). A folder that isn't an app is left out (and left alone). */
static void list_reads_names(void) {
    put_app("apps_fs/alpha", "alpha", "Alpha", "-- a\n");
    put_app("apps_fs/beta", "beta", "Beta", "-- b\n");
    mkdir("apps_fs/notes", 0777);
    put_app("apps_fs/gamma", "delta", "Delta", "-- the id isn't the folder's\n");
    put_app("apps_fs/Bad", "bad", "Bad", "-- not an id\n");
    spit("apps_fs/readme.txt", "hello");
    set_mtime("apps_fs/alpha", 1700000000);
    set_mtime("apps_fs/beta", 1750000000);
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    char ids[8][APPS_ID_MAX + 1], names[8][25];
    int n = list_ids(ids, names, 8);
    CHECK(n == 2, "two apps, not %d", n);
    CHECK(n == 2 && strcmp(ids[0], "alpha") == 0 && strcmp(names[0], "Alpha") == 0 && strcmp(ids[1], "beta") == 0 &&
              strcmp(names[1], "Beta") == 0,
          "by id (beta's folder is newer), with the manifests' names");
    CHECK(list_ids(ids, names, 1) == 1, "max is kept");
    CHECK(listed_n == 2 && strcmp(listed[0].id, "alpha") == 0 && strcmp(listed[1].name, "Beta") == 0, "the launcher's list");
    apps_manifest_t *m = apps_port_realloc(NULL, sizeof *m);
    CHECK(apps_store_manifest("alpha", m) && strcmp(m->name, "Alpha") == 0 && strcmp(m->version, "1.0.0") == 0, "manifest");
    apps_manifest_clear(m, apps_port_free);
    CHECK(!apps_store_manifest("notes", m) && !apps_store_manifest("gamma", m), "no manifest for what isn't an app");
    apps_port_free(m);
    CHECK(exists("apps_fs/notes") && exists("apps_fs/readme.txt") && exists("apps_fs/gamma/main.lua"), "left alone");
}

static const char *list_text(void);
static void commit_app(const char *id);

/* put_app with an icon: the manifest names icon.bin, which is one of its files (the launcher's carousel reads it). */
static void put_app_icon(const char *dir, const char *id, const char *name, const char *icon) {
    char path[256], json[512];
    snprintf(path, sizeof path, "%s/manifest.json", dir);
    snprintf(json, sizeof json, "{\"id\":\"%s\",\"name\":\"%s\",\"version\":\"1.0.0\",\"api\":1,\"files\":[\"%s\"],\"icon\":\"%s\"}", id,
             name, icon, icon);
    spit(path, json);
    snprintf(path, sizeof path, "%s/main.lua", dir);
    spit(path, "-- main\n");
}

/* The launcher's list is built in a PSRAM block, not on the worker's small stack: refused, nothing is published (one W line) and the
 * next change of the store publishes it all. */
static void a_refused_list_block_publishes_nothing(void) {
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    commit_app("aa");
    int before = list_publishes;
    long held = blocks_held;
    alloc_refuse_size = APPS_TASK_LIST_MAX * sizeof(apps_listed_t);
    int from = n_logged;
    commit_app("bb");
    alloc_refuse_size = 0;
    int said = 0;
    for (int i = from; i < n_logged; i++) {
        said += strstr(logged[i], "W muse_apps: store: the launcher's list wasn't published: out of memory") != NULL;
    }
    CHECK(list_publishes == before && listed_n == 1, "nothing published: %d, %d apps", list_publishes - before, listed_n);
    CHECK(said == 1, "one W line: %d", said);
    CHECK(blocks_held == held, "nothing held");
    commit_app("cc");
    CHECK(list_publishes == before + 1 && listed_n == 3, "the next change publishes every app: %d", listed_n);
}

/* The launcher's list carries each app's icon path: the manifest's, or "" for an app with none. */
static void list_carries_icon_paths(void) {
    mkdir("apps_fs/art", 0777);
    put_app_icon("apps_fs/art", "art", "Art", "img/icon.bin");
    put_app("apps_fs/plain", "plain", "Plain", "-- p\n");
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    CHECK(listed_n == 2, "two apps, not %d", listed_n);
    CHECK(listed_n == 2 && strcmp(listed[0].id, "art") == 0 && strcmp(listed[0].icon, "img/icon.bin") == 0, "art lists its icon: '%s'",
          listed[0].icon);
    CHECK(listed_n == 2 && strcmp(listed[1].id, "plain") == 0 && listed[1].icon[0] == '\0', "plain lists none: '%s'", listed[1].icon);
}

/* The launcher's icons (apps_icons.c) read an app's file on the worker: APPS_STORE_BASE/<id>/<path>, at most max bytes, into a block
 * of the apps' heap (through its floor) with a NUL after it. Every refusal leaves nothing held and *out NULL. */
static void read_file_reads_an_apps_file(void) {
    mkdir("apps_fs/art", 0777);
    put_app_icon("apps_fs/art", "art", "Art", "img/icon.bin");
    spit("apps_fs/art/img/icon.bin", "ICONBYTES");
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    long held = blocks_held;
    uint8_t *data = NULL;
    size_t len = 0;
    char err[128] = "?";
    CHECK(apps_store_read_file("art", "img/icon.bin", 64, &data, &len, err, sizeof err) && err[0] == '\0', "read: '%s'", err);
    CHECK(data != NULL && len == 9 && memcmp(data, "ICONBYTES", 10) == 0, "its bytes, and a NUL after them");
    CHECK(blocks_held == held + 1, "one block of the apps' heap");
    apps_port_free(data);
    CHECK(apps_store_read_file("art", "img/icon.bin", 9, &data, &len, err, sizeof err) && len == 9, "max is the most it may have");
    apps_port_free(data);

    struct {
        const char *id, *path;
        size_t max;
        const char *says;  /* what err holds (its start) */
    } refused[] = {
        {"art", "img/icon.bin", 8, "larger than 0 KB"},
        {"art", "img/none.bin", 64, strerror(ENOENT)},
        {"nobody", "img/icon.bin", 64, APPS_STORE_NO_SUCH_APP},
        {"Art", "img/icon.bin", 64, APPS_STORE_NO_SUCH_APP},
        {NULL, "img/icon.bin", 64, APPS_STORE_NO_SUCH_APP},
        {"art", "../art/img/icon.bin", 64, "not a path an app's file may have"},
        {"art", "/etc/hosts", 64, "not a path an app's file may have"},
        {"art", NULL, 64, "not a path an app's file may have"},
    };
    for (size_t i = 0; i < sizeof refused / sizeof refused[0]; i++) {
        data = (uint8_t *)err;
        len = 7;
        CHECK(!apps_store_read_file(refused[i].id, refused[i].path, refused[i].max, &data, &len, err, sizeof err),
              "refusal %zu is refused", i);
        CHECK(data == NULL && len == 0, "refusal %zu: nothing out", i);
        CHECK(strncmp(err, refused[i].says, strlen(refused[i].says)) == 0, "refusal %zu: '%s', not '%s'", i, err, refused[i].says);
    }
    alloc_refuse_all = true;  /* the floor */
    CHECK(!apps_store_read_file("art", "img/icon.bin", 64, &data, &len, err, sizeof err) && data == NULL && len == 0,
          "the floor refused the block: refused");
    alloc_refuse_all = false;
    CHECK(strcmp(err, "out of memory") == 0, "out of memory: '%s'", err);
    CHECK(blocks_held == held, "nothing held: %ld", blocks_held - held);
}

/* Only the flash store has files: before the mount, and in RAM mode, nothing is read. */
static void read_file_needs_the_flash_store(void) {
    uint8_t *data = NULL;
    size_t len = 0;
    char err[128];
    CHECK(!apps_store_read_file("art", "icon.bin", 64, &data, &len, err, sizeof err) && strcmp(err, "storage not ready") == 0,
          "before the mount: '%s'", err);
    has_apps_partition = false;
    CHECK(apps_store_mount() == APPS_STORAGE_RAM, "RAM mode");
    const file_t files[] = {{"main.lua", "-- main\n"}};
    apps_pkg_t *p = make_pkg("art", "Art", "1", 1, files);
    bool unchanged;
    CHECK(apps_store_commit(p, &unchanged, err, sizeof err), "kept in RAM: %s", err);
    free_pkg(p);
    CHECK(!apps_store_read_file("art", "main.lua", 64, &data, &len, err, sizeof err) && data == NULL && len == 0,
          "RAM mode has no files to read");
    CHECK(strcmp(err, "the apps are kept in RAM") == 0, "'%s'", err);
}

/* The list isn't cut at the launcher's old eight: every app the store keeps (up to 32) is in it. */
static void list_has_every_app(void) {
    char dir[64], id[16];
    for (int i = 1; i <= 12; i++) {
        snprintf(id, sizeof id, "app%02d", i);
        snprintf(dir, sizeof dir, "apps_fs/%s", id);
        put_app(dir, id, id, "-- a\n");
    }
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    CHECK(listed_n == 12, "twelve apps, not %d", listed_n);
    CHECK(listed_n == 12 && strcmp(listed[11].id, "app12") == 0, "the last is there");
    CHECK(APPS_TASK_LIST_MAX == APPS_STORE_APPS_MAX, "the list holds as many as the store keeps");
}

/* Folders the order doesn't name follow the ones it does, by name (case-insensitively), ties by id. */
static void unordered_tail_is_alphabetical_by_name(void) {
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    commit_app("oo");
    put_app("apps_fs/aaa", "aaa", "zeta", "-- z\n");
    put_app("apps_fs/bbb", "bbb", "Alpha", "-- a\n");
    put_app("apps_fs/ccc", "ccc", "mid", "-- m\n");
    put_app("apps_fs/t2", "t2", "Same", "-- s\n");
    put_app("apps_fs/t1", "t1", "same", "-- s\n");
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "remount");
    CHECK(strcmp(list_text(), "oo bbb ccc t1 t2 aaa") == 0, "the order's, then by name, ties by id: '%s'", list_text());
    CHECK(listed_n == 6 && strcmp(listed[1].id, "bbb") == 0 && strcmp(listed[5].id, "aaa") == 0, "the launcher's list has it too");
}

/* A package whose every file is the stored copy's (and whose manifest is) commits nothing: the folder isn't touched. */
static void unchanged_skips_commit(void) {
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    const file_t files[] = {{"main.lua", "-- x\n"}, {"data.json", "{\"a\":1}"}};
    apps_pkg_t *p = make_pkg("timer", "Timer", "1.0.0", 2, files);
    bool unchanged = true;
    char err[256];
    CHECK(apps_store_commit(p, &unchanged, err, sizeof err) && !unchanged, "the first commit: %s", err);
    spit("apps_fs/timer/.kept", "a replace would take this");
    CHECK(apps_store_commit(p, &unchanged, err, sizeof err) && unchanged, "the same again: unchanged");
    CHECK(exists("apps_fs/timer/.kept") && !exists("apps_fs/.stage-timer"), "the folder wasn't touched");
    free_pkg(p);
    const file_t changed[] = {{"main.lua", "-- y\n"}, {"data.json", "{\"a\":1}"}};
    p = make_pkg("timer", "Timer", "1.0.0", 2, changed);
    CHECK(apps_store_commit(p, &unchanged, err, sizeof err) && !unchanged && !exists("apps_fs/timer/.kept"), "one byte: a commit");
    spit("apps_fs/timer/.kept", "again");
    free_pkg(p);
    p = make_pkg("timer", "Timer", "1.0.1", 2, changed);
    CHECK(apps_store_commit(p, &unchanged, err, sizeof err) && !unchanged && !exists("apps_fs/timer/.kept"),
          "the manifest alone changed: a commit");
    free_pkg(p);
}

/* An app's values are one blob in apps_nvs, s%08x of the CRC-32 of its id: the id, a NUL, the JSON. */
static void values_roundtrip(void) {
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    CHECK(opens >= 1 && strcmp(opened_part, "apps_nvs") == 0 && strcmp(opened_ns, "apps") == 0,
          "the namespace is \"apps\" in apps_nvs: %s/%s", opened_part, opened_ns);
    const file_t files[] = {{"main.lua", "-- main\n"}};
    apps_pkg_t *p = make_pkg("timer", "Timer", "1", 1, files);
    bool unchanged;
    char err[256];
    CHECK(apps_store_commit(p, &unchanged, err, sizeof err), "%s", err);
    free_pkg(p);
    long held = blocks_held;
    CHECK(apps_store_save_values("timer", "{\"a\":1}", 7), "save");
    const nvs_item_t *item = apps_key('s', "timer", NULL);
    CHECK(item != NULL && item->len == 13 && memcmp(item->blob, "timer\0{\"a\":1}", 13) == 0, "the blob: the id, a NUL, the JSON");
    size_t len = 0;
    char *json = apps_store_load_values("timer", &len);
    CHECK(json != NULL && len == 7 && strcmp(json, "{\"a\":1}") == 0, "load: %s", json != NULL ? json : "NULL");
    apps_port_free(json);
    json = apps_store_load_values("clock", &len);
    CHECK(json != NULL && len == 2 && strcmp(json, "{}") == 0, "an app with nothing saved: {}");
    apps_port_free(json);
    CHECK(apps_store_save_values("timer", "{}", 2) && apps_key('s', "timer", NULL) == NULL, "an empty store keeps no key");
    json = apps_store_load_values("timer", &len);
    CHECK(json != NULL && strcmp(json, "{}") == 0, "and reads back as {}");
    apps_port_free(json);
    char key[16];
    key_of(key, 's', "timer", NULL);
    CHECK(nvs_put_raw("apps_nvs", "apps", key, "other\0{\"b\":2}", 13) == ESP_OK, "another id under timer's key");
    json = apps_store_load_values("timer", &len);
    CHECK(json != NULL && strcmp(json, "{}") == 0, "a key whose blob isn't the app's is not its values: it has none");
    apps_port_free(json);
    CHECK(apps_store_save_values("timer", "{\"c\":3}", 7) && apps_key_probe('s', "timer", NULL) == 1,
          "a save takes the next key, and leaves the other's (E): probe %d", apps_key_probe('s', "timer", NULL));
    const nvs_item_t *other = nvs_find("apps_nvs", "apps", key);
    CHECK(other != NULL && other->len == 13 && memcmp(other->blob, "other\0", 6) == 0, "the other's blob is as it was");
    CHECK(apps_store_save_values("ghost", "{\"c\":3}", 7) && apps_key('s', "ghost", NULL) == NULL,
          "an app that isn't installed has no values written: the save is moot (it was removed), not a failure");
    CHECK(blocks_held == held, "%ld blocks held", blocks_held - held);
}

/* D: values that are there and can't be read now aren't an empty store, which the app's first save would write over: the load gives
 * NULL (the launch is refused, to be tried again), and the blob stays. Only a key that isn't there is "{}". */
static void values_that_cannot_be_read_are_not_empty(void) {
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    const file_t files[] = {{"main.lua", "-- main\n"}};
    apps_pkg_t *p = make_pkg("timer", "Timer", "1", 1, files);
    bool unchanged;
    char err[256];
    CHECK(apps_store_commit(p, &unchanged, err, sizeof err), "%s", err);
    free_pkg(p);
    size_t len = 99;
    char *json = apps_store_load_values("timer", &len);
    CHECK(json != NULL && len == 2 && strcmp(json, "{}") == 0, "nothing saved: {}");
    apps_port_free(json);
    CHECK(apps_store_save_values("timer", "{\"a\":1}", 7), "save");
    long held = blocks_held;
    alloc_fail_in = 0;  /* the blob's block */
    json = apps_store_load_values("timer", &len);
    alloc_fail_in = -1;
    CHECK(json == NULL && len == 0, "no memory for the blob: NULL, not {}");
    apps_port_free(json);
    CHECK(blocks_held == held, "%ld blocks held after it", blocks_held - held);
    key_of(nvs_fail_key, 's', "timer", NULL);
    json = apps_store_load_values("timer", &len);
    CHECK(json == NULL && len == 0, "an error of apps_nvs: NULL, not {}");
    apps_port_free(json);
    nvs_fail_key[0] = '\0';
    const nvs_item_t *item = apps_key('s', "timer", NULL);
    CHECK(item != NULL && item->len == 13 && memcmp(item->blob, "timer\0{\"a\":1}", 13) == 0, "the blob is there still");
    json = apps_store_load_values("timer", &len);
    CHECK(json != NULL && strcmp(json, "{\"a\":1}") == 0, "and reads once it can: %s", json != NULL ? json : "NULL");
    apps_port_free(json);
}

/* E: two ids whose CRC-32s are one (zlib's: "timer" and "zleca55_" are both 0x6ad0de1a, and so is every key that adds the same to
 * them, "timer/done" and "zleca55_/done") keep their values, quarantines and records apart: each finds its own key among the probes,
 * a new one takes the first that is free, and removing one leaves the other's. */
static void keys_that_collide_stay_apart(void) {
    char k0[16], k1[16];
    key_of(k0, 's', "timer", NULL);
    key_of(k1, 's', "zleca55_", NULL);
    CHECK(strcmp(k0, "s6ad0de1a") == 0 && strcmp(k1, "s6ad0de1a") == 0, "the known answer, and the collision: %s %s", k0, k1);
    clock_set = true;
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    const file_t files[] = {{"main.lua", "-- main\n"}};
    apps_pkg_t *timer = make_pkg("timer", "Timer", "1", 1, files), *z = make_pkg("zleca55_", "Z", "1", 1, files);
    bool unchanged;
    char err[256];
    CHECK(apps_store_commit(timer, &unchanged, err, sizeof err) && apps_store_commit(z, &unchanged, err, sizeof err), "%s", err);
    CHECK(apps_store_save_values("timer", "{\"t\":1}", 7) && apps_store_save_values("zleca55_", "{\"z\":2}", 7), "values");
    CHECK(apps_key_probe('s', "timer", NULL) == 0 && apps_key_probe('s', "zleca55_", NULL) == 1, "two keys: %d %d",
          apps_key_probe('s', "timer", NULL), apps_key_probe('s', "zleca55_", NULL));
    size_t len;
    char *a = apps_store_load_values("timer", &len), *b = apps_store_load_values("zleca55_", &len);
    CHECK(a != NULL && b != NULL && strcmp(a, "{\"t\":1}") == 0 && strcmp(b, "{\"z\":2}") == 0, "each reads its own: %s %s",
          a != NULL ? a : "NULL", b != NULL ? b : "NULL");
    apps_port_free(a);
    apps_port_free(b);
    CHECK(apps_store_save_values("zleca55_", "{}", 2) && apps_key('s', "zleca55_", NULL) == NULL && apps_key('s', "timer", NULL) != NULL,
          "an empty store erases its own key only");
    CHECK(apps_store_save_values("zleca55_", "{\"z\":3}", 7) && apps_key_probe('s', "zleca55_", NULL) == 1, "and saves to it again");
    CHECK(saved("timer", "done", epoch_now + 1000, true, true) && saved("zleca55_", "done", epoch_now + 2000, false, true),
          "two records under one CRC-32");
    CHECK(apps_key_probe('a', "timer", "done") == 0 && apps_key_probe('a', "zleca55_", "done") == 1, "two keys for them too");
    apps_store_drop_alarm("zleca55_", "done");
    CHECK(apps_key('a', "zleca55_", "done") == NULL && apps_key('a', "timer", "done") != NULL, "a drop takes its own");
    CHECK(saved("zleca55_", "done", epoch_now + 2000, false, true) && apps_key_probe('a', "zleca55_", "done") == 1, "again, at its probe");
    /* The quarantine: zleca55_ is quarantined at a boot, then timer (its q key is put where a quarantine of it would go: the next
     * probe); each load is refused for itself, and a reinstall of one clears its own. */
    apps_marker_t *m = apps_store_marker();
    memset(m, 0, sizeof *m);
    char q[APPS_ID_MAX + 1];
    apps_marker_launch(m, "zleca55_");
    (void)apps_marker_boot(m, q);
    apps_marker_launch(m, "zleca55_");
    reset_reason = ESP_RST_PANIC;
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    CHECK(apps_key_probe('q', "zleca55_", NULL) == 0, "zleca55_'s quarantine: %d", apps_key_probe('q', "zleca55_", NULL));
    CHECK(apps_key('a', "zleca55_", "done") == NULL && apps_key('a', "timer", "done") != NULL,
          "the quarantine dropped its own records only");
    key_at(k1, 'q', "timer", NULL, 1);
    CHECK(nvs_put_raw("apps_nvs", "apps", k1, "timer", 6) == ESP_OK, "timer's quarantine, at the next probe");
    CHECK(apps_key_probe('q', "timer", NULL) == 1, "two quarantines under one CRC-32");
    CHECK(apps_store_commit(z, &unchanged, err, sizeof err) && unchanged, "a reinstall of zleca55_: %s", err);
    CHECK(apps_key('q', "zleca55_", NULL) == NULL && apps_key('q', "timer", NULL) != NULL, "clears its own quarantine");
    apps_pkg_t *got = apps_port_realloc(NULL, sizeof *got);
    bool loaded = apps_store_load("timer", got, err, sizeof err);
    CHECK(!loaded && strstr(err, "timer was running during two restarts") != NULL, "timer is still quarantined: '%s'", err);
    if (loaded) {
        apps_pkg_free(got, apps_port_free);
    }
    CHECK(apps_store_load("zleca55_", got, err, sizeof err), "zleca55_ loads: %s", err);
    apps_pkg_free(got, apps_port_free);
    apps_port_free(got);
    /* Removing timer takes its keys, and none of zleca55_'s, which are found past the hole it leaves. */
    CHECK(apps_store_remove("timer", err, sizeof err), "remove: %s", err);
    CHECK(apps_key('s', "timer", NULL) == NULL && apps_key('q', "timer", NULL) == NULL && apps_key('a', "timer", "done") == NULL,
          "timer's keys went");
    CHECK(apps_key('s', "zleca55_", NULL) != NULL, "zleca55_'s values stayed");
    char *zj = apps_store_load_values("zleca55_", &len);
    CHECK(zj != NULL && strcmp(zj, "{\"z\":3}") == 0, "and read past the free key before them: %s", zj != NULL ? zj : "NULL");
    apps_port_free(zj);
    free_pkg(timer);
    free_pkg(z);
}

/* apps_nvs that is full, or of another version, is erased, and only it: the pairing's nvs is never touched. */
static void full_nvs_erases_only_apps_nvs(void) {
    init_answers[0] = ESP_ERR_NVS_NO_FREE_PAGES;
    n_init_answers = 1;
    put_values("timer", "{\"old\":1}");
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    CHECK(init_calls == 2 && erase_calls == 1 && strcmp(erased[0], "apps_nvs") == 0,
          "init, erase apps_nvs once, init again: %d inits, %d erases", init_calls, erase_calls);
    CHECK(logged_has("ESP_ERR_NVS_NO_FREE_PAGES") && logged_has("apps_nvs"), "said");
    CHECK(apps_key('s', "timer", NULL) == NULL, "what it held went with it");
    const file_t files[] = {{"main.lua", "-- main\n"}};
    apps_pkg_t *p = make_pkg("timer", "Timer", "1", 1, files);
    bool unchanged;
    char err[256];
    CHECK(apps_store_commit(p, &unchanged, err, sizeof err), "%s", err);
    free_pkg(p);
    CHECK(apps_store_save_values("timer", "{\"a\":1}", 7), "values after the erase");
    nvs_capacity = 64;
    CHECK(!apps_store_save_values("timer", "{\"a\":2,\"b\":\"a longer value than room is left for\"}", 47),
          "a full apps_nvs refuses the save");
    size_t len;
    char *json = apps_store_load_values("timer", &len);
    CHECK(json != NULL && strcmp(json, "{\"a\":1}") == 0, "and the last saved stays: %s", json != NULL ? json : "NULL");
    apps_port_free(json);
    apps_store_name_t done = touched("done", APPS_STORE_ARMED, epoch_now + 1000, true, true);
    apps_store_sync_alarms("timer", &done, 1, false);
    CHECK(apps_key('a', "timer", "done") == NULL && logged_has("alarm timer/done: no record was kept: it won't outlive a restart"),
          "and a record, which is said");
}

/* The same for an apps_nvs written by a newer NVS. */
static void new_nvs_version_erases_only_apps_nvs(void) {
    init_answers[0] = ESP_ERR_NVS_NEW_VERSION_FOUND;
    n_init_answers = 1;
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    CHECK(init_calls == 2 && erase_calls == 1 && strcmp(erased[0], "apps_nvs") == 0, "erased once");
}

/* A gadget whose table has no "apps" partition (updated over the air only) keeps its apps in PSRAM: the RAM store of
 * apps_install.h, and the values in RAM. Nothing of FAT or apps_nvs is touched. */
static void ram_mode_when_partition_missing(void) {
    has_apps_partition = false;
    CHECK(apps_store_mount() == APPS_STORAGE_RAM && apps_store_mode() == APPS_STORAGE_RAM, "RAM mode");
    CHECK(strcmp(apps_storage_name(apps_store_mode()), "ram") == 0, "named ram");
    CHECK(mounts == 0 && init_calls == 0 && opens == 0, "no FAT mount, no apps_nvs");
    CHECK(logged_has("reflash over USB once to keep apps"), "the hint is in the log");
    CHECK(apps_store_hint() != NULL && strcmp(apps_store_hint(), APPS_RAM_HINT) == 0, "and the store's: '%s'",
          apps_store_hint() != NULL ? apps_store_hint() : "NULL");
    CHECK(listed_n == 0 && list_publishes >= 1, "the launcher is told: no apps");
    const file_t files[] = {{"main.lua", "-- main\n"}};
    apps_pkg_t *p = make_pkg("timer", "Timer", "3.0", 1, files);
    bool unchanged = true;
    char err[256];
    CHECK(apps_store_commit(p, &unchanged, err, sizeof err) && !unchanged && ram_commits == 1, "the RAM store keeps it: %s", err);
    CHECK(!exists("apps_fs"), "no file is written");
    char ids[4][APPS_ID_MAX + 1], names[4][25];
    CHECK(list_ids(ids, names, 4) == 1 && strcmp(ids[0], "timer") == 0 && strcmp(names[0], "Timer") == 0, "listed");
    CHECK(apps_store_has("timer"), "has");
    apps_pkg_t *got = apps_port_realloc(NULL, sizeof *got);
    CHECK(apps_store_load("timer", got, err, sizeof err) && ram_loads == 1 && strcmp(got->manifest.version, "3.0") == 0,
          "loaded from it: %s", err);
    free_pkg(got);
    apps_manifest_t *m = apps_port_realloc(NULL, sizeof *m);
    CHECK(apps_store_manifest("timer", m) && strcmp(m->name, "Timer") == 0, "its manifest");
    apps_manifest_clear(m, apps_port_free);
    apps_port_free(m);
    CHECK(apps_store_free_kb() == (uint32_t)(psram_free / 1024), "the room is PSRAM's: %u KB", (unsigned)apps_store_free_kb());
    cancelled_app[0] = '\0';
    CHECK(apps_store_remove("timer", err, sizeof err) && ram_removes == 1 && strcmp(forgotten, "timer") == 0,
          "a remove takes the package and its values: %s", err);
    CHECK(strcmp(cancelled_app, "timer") == 0, "and cancels its armed alarms: '%s'", cancelled_app);
    CHECK(forget_calls == 1 && strcmp(records_forgotten, "timer") == 0, "and has the app task forget its records (#2): %d", forget_calls);
    CHECK(list_ids(ids, names, 4) == 0, "and the list");
    free_pkg(p);
}

/* The quarantine marker, portable: an app that was running during two resets in a row is quarantined. */
static void marker_quarantines_after_two_resets(void) {
    apps_marker_t m;
    char q[APPS_ID_MAX + 1];
    memset(&m, 0xa5, sizeof m);
    CHECK(!apps_marker_boot(&m, q), "a marker that isn't one (power-on: RTC memory is noise) quarantines nothing");
    CHECK(m.resets == 0 && m.running == 0 && m.id[0] == '\0', "and is zeroed (D19)");
    apps_marker_launch(&m, "timer");
    CHECK(!apps_marker_boot(&m, q) && m.resets == 1 && m.running == 0, "launch, boot: one reset, not quarantined");
    apps_marker_launch(&m, "timer");
    CHECK(m.resets == 1 && m.running == 1, "a launch of the same id keeps the count (D19)");
    q[0] = '\0';
    CHECK(apps_marker_boot(&m, q) && strcmp(q, "timer") == 0, "launch, boot again: quarantined");
    CHECK(m.resets == 0 && m.running == 0, "and the marker starts again");
    CHECK(!apps_marker_boot(&m, q), "a boot with nothing running counts nothing");

    apps_marker_launch(&m, "timer");
    CHECK(!apps_marker_boot(&m, q), "one");
    apps_marker_launch(&m, "timer");
    apps_marker_clean_stop(&m);
    CHECK(m.resets == 0 && m.running == 0, "a clean stop resets the count");
    apps_marker_launch(&m, "timer");
    CHECK(!apps_marker_boot(&m, q) && m.resets == 1, "so the next reset is the first again");

    apps_marker_launch(&m, "clock");
    CHECK(m.resets == 0 && strcmp(m.id, "clock") == 0, "a launch of another id starts the count again (D19)");
    CHECK(!apps_marker_boot(&m, q), "clock's first");
}

/* ---- The rulings' ---- */

/* D6/E25: until the mount job has run, the store is empty and not ready, and says so at once; the app task's tick submits the
 * job (once), and after it the committed app is listed. */
static void list_before_mount_is_empty(void) {
    put_app("apps_fs/timer", "timer", "Timer", "-- main\n");
    char ids[4][APPS_ID_MAX + 1], names[4][25], err[128] = "";
    CHECK(apps_store_mode() == APPS_STORAGE_NONE && strcmp(apps_storage_name(APPS_STORAGE_NONE), "none") == 0, "none yet");
    CHECK(list_ids(ids, names, 4) == 0 && !apps_store_has("timer") && apps_store_free_kb() == 0, "nothing listed");
    apps_pkg_t *got = apps_port_realloc(NULL, sizeof *got);
    CHECK(!apps_store_load("timer", got, err, sizeof err) && strcmp(err, "storage not ready") == 0, "not ready: '%s'", err);
    apps_store_tick(mono_ms);
    CHECK(submits == 1 && n_jobs == 1, "the tick submits the mount job");
    apps_store_tick(mono_ms + 5000);
    CHECK(submits == 1, "once: it is queued");
    run_jobs();
    CHECK(apps_store_mode() == APPS_STORAGE_FLASH && list_ids(ids, names, 4) == 1 && strcmp(ids[0], "timer") == 0,
          "after the job: listed");
    CHECK(apps_store_free_kb() == 6 * 1024, "%u KB free", (unsigned)apps_store_free_kb());
    apps_store_tick(mono_ms + 10000);
    CHECK(submits == 1, "nothing more once mounted");
    CHECK(apps_store_load("timer", got, err, sizeof err), "and it loads: %s", err);
    free_pkg(got);
}

/* A refused mount job is asked for again APPS_STORE_RETRY_MS later, not at every round. */
static void a_refused_mount_is_asked_again_later(void) {
    submit_refuses = true;
    apps_store_tick(10000);
    CHECK(submits == 1, "asked");
    apps_store_tick(10000 + APPS_STORE_RETRY_MS - 1);
    CHECK(submits == 1, "not again before %d ms", APPS_STORE_RETRY_MS);
    submit_refuses = false;
    apps_store_tick(10000 + APPS_STORE_RETRY_MS);
    CHECK(submits == 2 && n_jobs == 1, "again after them");
    run_jobs();
    CHECK(apps_store_mode() == APPS_STORAGE_FLASH, "mounted");
}

/* The marker in RTC memory as the mount reads it: launched twice with a reset of the kind the case gives in between (argv's
 * reason, as esp_reset_reason says it), then one more reset of that kind at this boot. Only a crash reset counts (the ruling);
 * the case prints what came of it. */
static void reset_reason_counts(int reason) {
    put_app("apps_fs/timer", "timer", "Timer", "-- main\n");
    apps_marker_t *m = apps_store_marker();
    memset(m, 0, sizeof *m);
    char q[APPS_ID_MAX + 1];
    apps_marker_launch(m, "timer");
    (void)apps_marker_boot(m, q);  /* an earlier crash reset */
    apps_marker_launch(m, "timer");
    reset_reason = (esp_reset_reason_t)reason;
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    apps_pkg_t *got = apps_port_realloc(NULL, sizeof *got);
    char err[200];
    bool loads = apps_store_load("timer", got, err, sizeof err);
    if (loads) {
        apps_pkg_free(got, apps_port_free);
    }
    apps_port_free(got);
    CHECK(loads || strcmp(err, "timer was running during two restarts in a row; reinstall it to run it again") == 0,
          "a load that fails is the quarantine's: '%s'", err);
    printf("%s\n", loads ? "cleared" : "quarantined");
    CHECK(logged_count("boot: reset reason ") == 1, "one boot line");
    CHECK(!loads || (m->running == 0 && m->resets == 0), "a reset that isn't a crash is a clean stop");
}

/* J: the boot's line has the marker's id and its count as read, before this boot's reset is counted or cleared: a crash that left the
 * count at 1 shows on the next line, even when that reset clears it. */
static void boot_line_says_the_count(void) {
    put_app("apps_fs/timer", "timer", "Timer", "-- main\n");
    apps_marker_t *m = apps_store_marker();
    memset(m, 0, sizeof *m);
    static const struct {
        esp_reset_reason_t why;
        const char *line;
    } boots[] = {
        {ESP_RST_PANIC, "boot: reset reason panic; app marker timer, resets 0, was running during a crash reset"},
        {ESP_RST_USB, "boot: reset reason usb; app marker timer, resets 1, was running, not a crash: cleared"},
        {ESP_RST_PANIC, "boot: reset reason panic; app marker timer, resets 0, was running during a crash reset"},
        {ESP_RST_TASK_WDT,
         "boot: reset reason task_wdt; app marker timer, resets 1, was running during 2 crash resets in a row: quarantined"},
    };
    for (size_t i = 0; i < sizeof boots / sizeof boots[0]; i++) {
        apps_marker_launch(m, "timer");
        reset_reason = boots[i].why;
        n_logged = 0;
        CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
        CHECK(logged_count(boots[i].line) == 1 && logged_count("boot: reset reason ") == 1, "boot %zu: '%s'", i, boots[i].line);
    }
    n_logged = 0;
    reset_reason = ESP_RST_SW;
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    CHECK(logged_has("boot: reset reason sw; app marker timer, resets 0, nothing was running"), "nothing ran: the count read is 0");
}

/* A quarantined app: its alarm records go (stored and armed), its load is refused with the message, until a reinstall. */
static void quarantine_refuses_until_reinstalled(void) {
    clock_set = true;
    put_app("apps_fs/timer", "timer", "Timer", "-- main\n");
    put_record("timer", "done", epoch_now + 60000, true, true);
    put_record("clock", "tick", epoch_now + 60000, false, true);
    put_app("apps_fs/clock", "clock", "Clock", "-- main\n");
    apps_marker_t *m = apps_store_marker();
    memset(m, 0, sizeof *m);
    char q[APPS_ID_MAX + 1], err[256];
    apps_marker_launch(m, "timer");
    (void)apps_marker_boot(m, q);
    apps_marker_launch(m, "timer");
    reset_reason = ESP_RST_PANIC;
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    CHECK(logged_has("boot: reset reason panic") && logged_has("timer") && logged_has("quarantined"), "the boot line says so");
    CHECK(apps_key('q', "timer", NULL) != NULL, "the quarantine is kept in apps_nvs");
    CHECK(apps_key('a', "timer", "done") == NULL && strcmp(cancelled_app, "timer") == 0, "its alarms go, stored and armed");
    CHECK(forget_calls == 1 && strcmp(records_forgotten, "timer") == 0, "and the app task's copies (#2): %d, '%s'", forget_calls,
          records_forgotten);
    CHECK(apps_key('a', "clock", "tick") != NULL, "another app's record stays");
    apps_pkg_t *got = apps_port_realloc(NULL, sizeof *got);
    CHECK(!apps_store_load("timer", got, err, sizeof err) &&
              strcmp(err, "timer was running during two restarts in a row; reinstall it to run it again") == 0,
          "refused: '%s'", err);
    CHECK(apps_store_has("timer"), "still listed");
    apps_store_record_t recs[APPS_STORE_RECORDS_MAX];
    int n = apps_store_take_records(recs, APPS_STORE_RECORDS_MAX);
    CHECK(n == 1 && strcmp(recs[0].id, "clock") == 0, "only the other app's record is handed over to be armed again: %d", n);
    const file_t files[] = {{"main.lua", "-- main\n"}};
    apps_pkg_t *p = make_pkg("timer", "Timer", "1", 1, files);
    bool unchanged;
    CHECK(apps_store_commit(p, &unchanged, err, sizeof err) && !unchanged, "a reinstall: %s", err);
    CHECK(apps_key('q', "timer", NULL) == NULL, "clears it");
    CHECK(apps_store_load("timer", got, err, sizeof err), "and it loads: %s", err);
    apps_pkg_free(got, apps_port_free);
    /* The same files pushed again, the likeliest answer to the refusal, are an unchanged commit: that clears it too (W). */
    char key[16];
    key_of(key, 'q', "timer", NULL);
    CHECK(nvs_put_raw("apps_nvs", "apps", key, "timer", 6) == ESP_OK, "quarantined again");
    CHECK(!apps_store_load("timer", got, err, sizeof err), "refused");
    CHECK(apps_store_commit(p, &unchanged, err, sizeof err) && unchanged, "the same package again: unchanged: %s", err);
    CHECK(apps_key('q', "timer", NULL) == NULL && logged_has("store: timer was reinstalled: its quarantine is over"), "clears it");
    CHECK(apps_store_load("timer", got, err, sizeof err), "and it loads: %s", err);
    apps_pkg_free(got, apps_port_free);
    apps_port_free(got);
    free_pkg(p);
}

/* The record of an alarm as the mount hands it over, or NULL. */
static const apps_store_record_t *record_in(const apps_store_record_t *recs, int n, const char *id, const char *name) {
    for (int i = 0; i < n; i++) {
        if (strcmp(recs[i].id, id) == 0 && strcmp(recs[i].name, name) == 0) {
            return &recs[i];
        }
    }
    return NULL;
}

/* P1: the mount reads the records and hands them to the app task, which arms them again (the store arms nothing, clock or none): each
 * with its time and options. A record of an app that isn't installed goes; so does one whose time isn't a record's (AA: before 2025, after
 * 2100, or the bytes of INT64_MIN, which the arithmetic would overflow on). The first take has them, the next none. */
static void records_are_handed_over_at_the_mount(void) {
    put_app("apps_fs/timer", "timer", "Timer", "-- main\n");
    put_record("timer", "done", epoch_now + 60000, true, true);
    put_record("timer", "late", epoch_now - 5000, false, true);
    put_record("ghost", "boo", epoch_now + 1000, true, true);
    put_record("timer", "old", 1700000000000LL, true, true);    /* 2023 */
    put_record("timer", "far", 4200000000000LL, true, true);    /* 2103 */
    put_record("timer", "min", INT64_MIN, true, true);          /* 00 .. 80 */
    apps_store_record_t recs[APPS_STORE_RECORDS_MAX];
    CHECK(apps_store_take_records(recs, APPS_STORE_RECORDS_MAX) == 0, "before the mount: none");
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash, with no valid clock");
    CHECK(n_arms == 0, "the store arms nothing: the app task does");
    int n = apps_store_take_records(recs, APPS_STORE_RECORDS_MAX);
    CHECK(n == 2, "two records handed over: %d", n);
    const apps_store_record_t *done = record_in(recs, n, "timer", "done"), *late = record_in(recs, n, "timer", "late");
    CHECK(done != NULL && done->epoch_ms == epoch_now + 60000 && done->sound && done->wake, "done, as kept");
    CHECK(late != NULL && late->epoch_ms == epoch_now - 5000 && !late->sound && late->wake, "late, as kept (past due: the app task's)");
    CHECK(apps_key('a', "ghost", "boo") == NULL, "an app that isn't installed: its record went");
    CHECK(apps_key('a', "timer", "old") == NULL && apps_key('a', "timer", "far") == NULL && apps_key('a', "timer", "min") == NULL,
          "records whose times aren't records' went");
    CHECK(apps_key('a', "timer", "done") != NULL && apps_key('a', "timer", "late") != NULL, "the others stay, for the app task");
    CHECK(apps_store_take_records(recs, APPS_STORE_RECORDS_MAX) == 0, "taken once");
}

/* S: a record that can't be read for now (an error of apps_nvs) stays as it is and isn't handed over; one that was read and isn't a
 * record (its length), or isn't under one of its own keys, goes. */
static void a_record_that_cannot_be_read_now_is_kept(void) {
    put_app("apps_fs/timer", "timer", "Timer", "-- main\n");
    put_record("timer", "busy", epoch_now + 60000, true, true);
    put_record("timer", "fine", epoch_now + 60000, true, true);
    char key[16];
    key_of(key, 'a', "timer", "short");
    CHECK(nvs_put_raw("apps_nvs", "apps", key, "timer\0short", 12) == ESP_OK, "a blob that isn't a record");
    uint8_t rec[RECORD_LEN] = {0};
    memcpy(rec, "timer", 5);
    memcpy(rec + APPS_ID_MAX + 1, "elsewhere", 9);
    rec[APPS_ID_MAX + 1 + APPS_ALARM_NAME_MAX + 1] = 3;
    for (int i = 0; i < 8; i++) {
        rec[RECORD_LEN - 8 + i] = (uint8_t)((uint64_t)(epoch_now + 60000) >> (8 * i));
    }
    CHECK(nvs_put_raw("apps_nvs", "apps", "a00000001", rec, sizeof rec) == ESP_OK, "a record under a key that isn't its own");
    key_of(nvs_fail_key, 'a', "timer", "busy");
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    nvs_fail_key[0] = '\0';
    apps_store_record_t recs[APPS_STORE_RECORDS_MAX];
    int n = apps_store_take_records(recs, APPS_STORE_RECORDS_MAX);
    CHECK(n == 1 && strcmp(recs[0].name, "fine") == 0, "the one that could be read is handed over: %d", n);
    CHECK(apps_key('a', "timer", "busy") != NULL, "the one that couldn't be read now stays");
    CHECK(nvs_find("apps_nvs", "apps", key) == NULL, "the blob that isn't a record went");
    CHECK(nvs_find("apps_nvs", "apps", "a00000001") == NULL, "and the record under another's key");
}

/* S: when the list couldn't be read (no memory at the mount), no record is dropped as an app's that isn't installed: they all wait for
 * a mount that can tell. */
static void records_survive_a_list_that_failed(void) {
    put_app("apps_fs/timer", "timer", "Timer", "-- main\n");
    put_record("timer", "done", epoch_now + 60000, true, true);
    put_record("ghost", "boo", epoch_now + 60000, true, true);
    alloc_refuse_all = true;  /* the apps' heap has nothing for the mount: the list's blocks among the rest */
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    alloc_refuse_all = false;
    CHECK(logged_has("store: out of memory: the list of apps wasn't read"), "said");
    CHECK(apps_key('a', "timer", "done") != NULL && apps_key('a', "ghost", "boo") != NULL, "every record stays");
    apps_store_record_t got[APPS_STORE_RECORDS_MAX];
    int handed = apps_store_take_records(got, APPS_STORE_RECORDS_MAX);
    CHECK(handed == 0, "none is handed over to be armed again (its launch wouldn't find the app, and its record would go): %d", handed);
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "a mount that reads the list");
    handed = apps_store_take_records(got, APPS_STORE_RECORDS_MAX);
    CHECK(handed == 1 && strcmp(got[0].id, "timer") == 0 && apps_key('a', "ghost", "boo") == NULL && apps_key('a', "timer", "done") != NULL,
          "hands timer's over, and drops ghost's: %d", handed);
}

/* A record is written as the brief has it (by the sync: the reader above reads it back), dropped by name, or all of an app's at once. */
static void alarm_records_round_trip(void) {
    clock_set = true;
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    const file_t files[] = {{"main.lua", "-- main\n"}};
    apps_pkg_t *p = make_pkg("timer", "Timer", "1", 1, files);
    bool unchanged;
    char err[256];
    CHECK(apps_store_commit(p, &unchanged, err, sizeof err), "%s", err);
    free_pkg(p);
    CHECK(saved("timer", "done", epoch_now + 30000, true, false), "save");
    const nvs_item_t *rec = apps_key('a', "timer", "done");
    CHECK(rec != NULL && rec->len == RECORD_LEN, "a%%08x of the CRC-32 of timer/done, %d bytes", RECORD_LEN);
    if (rec != NULL && rec->len == RECORD_LEN) {
        int64_t epoch = 0;
        for (int i = 0; i < 8; i++) {
            epoch |= (int64_t)rec->blob[RECORD_LEN - 8 + i] << (8 * i);
        }
        CHECK(strcmp((const char *)rec->blob, "timer") == 0 && strcmp((const char *)rec->blob + APPS_ID_MAX + 1, "done") == 0 &&
                  rec->blob[APPS_ID_MAX + 1 + APPS_ALARM_NAME_MAX + 1] == 1 && epoch == epoch_now + 30000,
              "the id, the name, the flags and the time");
    }
    apps_store_drop_alarm("timer", "done");
    CHECK(apps_key('a', "timer", "done") == NULL, "dropped");
    apps_store_drop_alarm("timer", "done");  /* nothing to drop: nothing happens */
    CHECK(saved("timer", "a", epoch_now + 1, true, true) && saved("timer", "b", epoch_now + 2, true, true),
          "two");
    apps_store_drop_alarm("timer", NULL);
    CHECK(apps_key('a', "timer", "a") == NULL && apps_key('a', "timer", "b") == NULL, "all of the app's at once");
    n_logged = 0;
    apps_store_name_t x = touched("x", APPS_STORE_ARMED, epoch_now + 1, true, true);
    apps_store_sync_alarms("ghost", &x, 1, false);
    CHECK(apps_key('a', "ghost", "x") == NULL && !logged_has("no record was kept"), "an app that isn't installed has no records: moot, not a failure");
    /* E: a key another's record holds is left to it; this one's record takes the next, and is found and dropped there. */
    char key[16];
    key_of(key, 'a', "timer", "done");
    put_record_at(0, "zleca55_", "done", epoch_now + 5, true, true);  /* "zleca55_/done" has timer/done's CRC-32 */
    CHECK(saved("timer", "done", epoch_now + 30000, true, false) && apps_key_probe('a', "timer", "done") == 1, "the next key");
    const nvs_item_t *theirs = nvs_find("apps_nvs", "apps", key);
    CHECK(theirs != NULL && strcmp((const char *)theirs->blob, "zleca55_") == 0, "the other's record stays");
    apps_store_drop_alarm("timer", "done");
    CHECK(apps_key('a', "timer", "done") == NULL && nvs_find("apps_nvs", "apps", key) != NULL, "a drop takes its own only");
}

/* P1: a sync (apps_store_sync_alarms: apps_values.c's op, on the worker) writes or drops the records of the names it is handed, and of no
 * other. An armed name's record is written when it is new, more than EPOCH_SLACK_MS off or with other options, and neither written again nor
 * charged within it; each write takes its cost from the budget, not forced. One armed with no clock loses its old record (its time isn't
 * known), and a gone one's goes. With all, every other record of the app goes too. Another app's records stay. */
static void sync_alarms_touches_only_the_names_it_is_handed(void) {
    clock_set = true;
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    const file_t files[] = {{"main.lua", "-- main\n"}};
    bool unchanged;
    char err[256];
    static const char *const ids[] = {"timer", "other"};
    for (size_t i = 0; i < 2; i++) {
        apps_pkg_t *p = make_pkg(ids[i], ids[i], "1", 1, files);
        CHECK(apps_store_commit(p, &unchanged, err, sizeof err), "%s", err);
        free_pkg(p);
    }
    CHECK(saved("other", "x", epoch_now + 5000, true, true), "another app's record");
    CHECK(saved("timer", "a", epoch_now + 10000, true, true) && saved("timer", "b", epoch_now + 20000, true, true),
          "a and b, handed over at a mount and not armed yet");
    int writes = blob_writes;
    apps_store_name_t c = touched("c", APPS_STORE_ARMED_NO_CLOCK, 0, true, true);  /* the app armed c with no clock */
    apps_store_sync_alarms("timer", &c, 1, false);
    CHECK(record_epoch("timer", "a") == epoch_now + 10000 && record_epoch("timer", "b") == epoch_now + 20000, "a and b, not handed, stay");
    CHECK(apps_key('a', "timer", "c") == NULL && blob_writes == writes, "c gets none (its time isn't known), nothing is written");
    /* The app armed a itself, still with no clock: a's old record goes, b's stays. */
    apps_store_name_t a = touched("a", APPS_STORE_ARMED_NO_CLOCK, 0, true, true);
    apps_store_sync_alarms("timer", &a, 1, false);
    CHECK(apps_key('a', "timer", "a") == NULL && record_epoch("timer", "b") == epoch_now + 20000, "a's old record went, b's stayed");
    /* The clock: a's new deadline is written, charged, not forced. */
    budget_takes = budget_forced = 0;
    a = touched("a", APPS_STORE_ARMED, epoch_now + 90000, true, true);
    apps_store_sync_alarms("timer", &a, 1, false);
    CHECK(record_epoch("timer", "a") == epoch_now + 90000 && record_flags("timer", "a") == 3, "a: its new deadline");
    CHECK(budget_takes == 1 && budget_forced == 0, "its cost taken, not forced: %d takes, %d forced", budget_takes, budget_forced);
    writes = blob_writes;
    a.epoch_ms += 700;  /* within the slack: the same alarm, its time left read a little later */
    apps_store_sync_alarms("timer", &a, 1, false);
    CHECK(blob_writes == writes && budget_takes == 1, "nothing written again, nor charged: %d writes, %d takes", blob_writes - writes,
          budget_takes);
    a.sound = false;  /* other options */
    apps_store_sync_alarms("timer", &a, 1, false);
    CHECK(blob_writes == writes + 1 && record_flags("timer", "a") == 2 && budget_takes == 2 && budget_forced == 0,
          "a written again, without sound: %d writes, %d takes", blob_writes - writes, budget_takes);
    /* b was cancelled by the app, and a was told: both go. */
    apps_store_name_t gone[2] = {touched("b", APPS_STORE_GONE, 0, false, false), touched("a", APPS_STORE_GONE, 0, false, false)};
    apps_store_sync_alarms("timer", gone, 2, false);
    CHECK(apps_key('a', "timer", "a") == NULL && apps_key('a', "timer", "b") == NULL, "a's and b's records went");
    /* all: every other record of the app goes; z, in the table and the same, stays. */
    CHECK(saved("timer", "y", epoch_now + 1000, true, true) && saved("timer", "z", epoch_now + 2000, true, true),
          "y and z");
    apps_store_name_t z = touched("z", APPS_STORE_ARMED, epoch_now + 2000, true, true);
    apps_store_sync_alarms("timer", &z, 1, true);
    CHECK(apps_key('a', "timer", "y") == NULL && record_epoch("timer", "z") == epoch_now + 2000, "all: y went, z stayed");
    CHECK(record_epoch("other", "x") == epoch_now + 5000, "another app's record stays");
}

/* #1: a record the budget can't take: the name's old record is dropped at once (its time is the replaced alarm's, and none is better than a
 * wrong one after a reset), nothing is written, and the name goes back to the app task (apps_values_record_waits), to be synced again once the
 * budget has room. A name with no record waits the same way. Drops never wait: another app's gone name loses its record with the budget
 * spent. */
static void a_record_that_must_wait_drops_the_old_one_at_once(void) {
    clock_set = true;
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    const file_t files[] = {{"main.lua", "-- main\n"}};
    bool unchanged;
    char err[256];
    static const char *const ids[] = {"timer", "clock"};
    for (size_t i = 0; i < 2; i++) {
        apps_pkg_t *p = make_pkg(ids[i], ids[i], "1", 1, files);
        CHECK(apps_store_commit(p, &unchanged, err, sizeof err), "%s", err);
        free_pkg(p);
    }
    CHECK(saved("timer", "done", epoch_now + 10000, true, true) && saved("clock", "nap", epoch_now + 20000, true, false), "two records");
    budget_spent = true;
    int writes = blob_writes;
    apps_store_name_t done = touched("done", APPS_STORE_ARMED, epoch_now + 90000, true, true);  /* armed again: a new countdown */
    apps_store_sync_alarms("timer", &done, 1, false);
    CHECK(apps_key('a', "timer", "done") == NULL, "done's old record went at once");
    CHECK(blob_writes == writes, "nothing written: %d writes", blob_writes - writes);
    CHECK(n_waits_noted == 1 && strcmp(waits_noted[0], "timer/done") == 0, "done goes back to wait for the budget: %d notes", n_waits_noted);
    apps_store_name_t fresh = touched("fresh", APPS_STORE_ARMED, epoch_now + 30000, false, true);  /* a name with no record yet */
    apps_store_sync_alarms("timer", &fresh, 1, false);
    CHECK(n_waits_noted == 2 && strcmp(waits_noted[1], "timer/fresh") == 0 && apps_key('a', "timer", "fresh") == NULL, "fresh waits too: %d",
          n_waits_noted);
    apps_store_name_t nap = touched("nap", APPS_STORE_GONE, 0, false, false);
    apps_store_sync_alarms("clock", &nap, 1, false);
    CHECK(apps_key('a', "clock", "nap") == NULL && n_waits_noted == 2, "another app's drop goes through with the budget spent");
    budget_spent = false;
    apps_store_sync_alarms("timer", &done, 1, false);
    CHECK(record_epoch("timer", "done") == epoch_now + 90000 && record_flags("timer", "done") == 3 && n_waits_noted == 2,
          "written once the budget has room");
}

/* #3: a sync never touches a record whose name it isn't handed (P1), whether the mount handed that record over or not: one the mount couldn't
 * read (S), which reads fine now, and one past the APPS_STORE_RECORDS_MAX the mount hands over both survive the app's sync of another name. */
static void a_sync_leaves_the_records_nobody_touched(void) {
    clock_set = true;
    put_app("apps_fs/timer", "timer", "Timer", "-- main\n");
    put_record("timer", "busy", epoch_now + 60000, true, true);
    char name[8];
    for (int i = 0; i <= APPS_STORE_RECORDS_MAX; i++) {
        snprintf(name, sizeof name, "r%02d", i);
        put_record("timer", name, epoch_now + 70000 + i, true, false);
    }
    key_of(nvs_fail_key, 'a', "timer", "busy");
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    nvs_fail_key[0] = '\0';
    apps_store_record_t got[APPS_STORE_RECORDS_MAX];
    int n = apps_store_take_records(got, APPS_STORE_RECORDS_MAX);
    CHECK(n == APPS_STORE_RECORDS_MAX && record_in(got, n, "timer", "busy") == NULL, "16 handed over, busy not: %d", n);
    CHECK(logged_has("more alarm records than the 16 the gadget arms again at once"), "and the 17th r is left, said");
    char left[8] = "";
    for (int i = 0; i <= APPS_STORE_RECORDS_MAX && left[0] == '\0'; i++) {
        snprintf(name, sizeof name, "r%02d", i);
        if (record_in(got, n, "timer", name) == NULL) {
            snprintf(left, sizeof left, "%s", name);
        }
    }
    CHECK(left[0] != '\0', "an r record past the 16 isn't handed over");
    apps_store_name_t nap = touched("nap", APPS_STORE_ARMED, epoch_now + 5000, true, true);
    apps_store_sync_alarms("timer", &nap, 1, false);
    CHECK(record_epoch("timer", "nap") == epoch_now + 5000, "nap's record is written");
    CHECK(record_epoch("timer", "busy") == epoch_now + 60000, "busy, unreadable at the mount, stays");
    CHECK(left[0] != '\0' && record_epoch("timer", left) > 0, "%s, past the 16, stays", left);
    int stayed = 0;
    for (int i = 0; i <= APPS_STORE_RECORDS_MAX; i++) {
        snprintf(name, sizeof name, "r%02d", i);
        stayed += record_epoch("timer", name) == epoch_now + 70000 + i ? 1 : 0;
    }
    CHECK(stayed == APPS_STORE_RECORDS_MAX + 1, "every r record stays as it was: %d", stayed);
}

/* The first mount of a partition with no file system formats it (format_if_mount_failed, 4 files, units of 4 KB), and the log
 * says so, with its time, the room and apps_nvs's use. */
static void first_mount_formats(void) {
    CHECK(!exists("apps_fs"), "a blank partition");
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    CHECK(mounts == 1 && strcmp(mount_base, "apps_fs") == 0 && strcmp(mount_label, "apps") == 0, "mounted at %s on %s", mount_base,
          mount_label);
    CHECK(mount_cfg.format_if_mount_failed && mount_cfg.max_files == 4 && mount_cfg.allocation_unit_size == 4096,
          "the brief's config");
    CHECK(exists("apps_fs"), "formatted");
    CHECK(logged_has("store: flash: mounted") && logged_has("a new file system") && logged_has(" ms") &&
              logged_has("KB free") && logged_has("apps_nvs ") && logged_has("entries used"),
          "the mount line");
}

/* A partition the store has mounted before isn't a new file system. */
static void a_second_mount_is_not_new(void) {
    mkdir("apps_fs", 0777);
    spit("apps_fs/.store", "");
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    CHECK(logged_has("store: flash: mounted") && !logged_has("a new file system"), "not new");
}

/* P7: a partition that can't be mounted even by formatting it leaves the store in RAM mode for this boot, with a line and a hint of its
 * own: a reflash wouldn't help, a restart may. */
static void an_unmountable_partition_is_ram_mode(void) {
    mount_answer = ESP_ERR_NO_MEM;
    CHECK(apps_store_hint() == NULL, "no hint before the mount");
    CHECK(apps_store_mount() == APPS_STORAGE_RAM, "RAM mode");
    CHECK(logged_has("store: the apps partition couldn't be mounted: apps are kept in RAM until the gadget restarts") && init_calls == 0,
          "said, and apps_nvs left alone");
    CHECK(!logged_has("reflash"), "no reflash advice");
    CHECK(apps_store_hint() != NULL && strcmp(apps_store_hint(), "the gadget couldn't open its app storage; restart it to try again") == 0,
          "its hint: '%s'", apps_store_hint() != NULL ? apps_store_hint() : "NULL");
}

/* A commit that doesn't fit is refused before anything is written; one whose writing fails leaves the old copy as it was. */
static void a_commit_that_cant_be_written_leaves_the_app(void) {
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    const file_t v1[] = {{"main.lua", "-- v1\n"}};
    apps_pkg_t *p = make_pkg("timer", "Timer", "1", 1, v1);
    bool unchanged;
    char err[256];
    CHECK(apps_store_commit(p, &unchanged, err, sizeof err), "%s", err);
    free_pkg(p);
    fat_free_bytes = 100;
    const file_t v2[] = {{"main.lua", "-- v2 is a little longer than there is room for in the fake's hundred bytes\n"}};
    p = make_pkg("timer", "Timer", "2", 1, v2);
    CHECK(!apps_store_commit(p, &unchanged, err, sizeof err) && strstr(err, "not enough room on the gadget for timer") != NULL,
          "refused: '%s'", err);
    CHECK(file_is("apps_fs/timer/main.lua", "-- v1\n") && !exists("apps_fs/.stage-timer"), "v1 stays, nothing written");
    fat_free_bytes = (uint64_t)6 << 20;
    CHECK(chmod("apps_fs", 0555) == 0, "the partition can't be written: %s", strerror(errno));
    bool failed = !apps_store_commit(p, &unchanged, err, sizeof err);
    CHECK(chmod("apps_fs", 0777) == 0, "%s", strerror(errno));
    CHECK(failed && strstr(err, "couldn't write timer to the gadget's storage") != NULL, "a failed write: '%s'", err);
    CHECK(file_is("apps_fs/timer/main.lua", "-- v1\n") && !exists("apps_fs/.old-timer") && !exists("apps_fs/.stage-timer"), "v1 stays");
    free_pkg(p);
}

/* A stored copy that has lost a file isn't loaded: the error says to install it again. */
static void a_damaged_copy_is_not_loaded(void) {
    spit("apps_fs/timer/manifest.json", "{\"id\":\"timer\",\"name\":\"Timer\",\"api\":1,\"files\":[\"util.lua\"]}");
    spit("apps_fs/timer/main.lua", "-- main\n");
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    apps_pkg_t *got = apps_port_realloc(NULL, sizeof *got);
    char err[256];
    long held = blocks_held;
    CHECK(!apps_store_load("timer", got, err, sizeof err) && strstr(err, "the stored copy of timer is damaged") != NULL &&
              strstr(err, "install it again") != NULL,
          "refused: '%s'", err);
    CHECK(blocks_held == held, "nothing held after a failed load");
    CHECK(!apps_store_load("nope", got, err, sizeof err) && strcmp(err, "no such app on the gadget") == 0, "'%s'", err);
    apps_port_free(got);
}

/* The ids of the list, joined by spaces. */
static const char *list_text(void) {
    static char text[256];
    char ids[APPS_STORE_APPS_MAX][APPS_ID_MAX + 1], names[APPS_STORE_APPS_MAX][25];
    int n = list_ids(ids, names, APPS_STORE_APPS_MAX);
    text[0] = '\0';
    for (int i = 0; i < n; i++) {
        snprintf(text + strlen(text), sizeof text - strlen(text), "%s%s", i > 0 ? " " : "", ids[i]);
    }
    return text;
}

/* The order blob as text (its NULs as spaces), or "(none)". */
static const char *order_text(void) {
    static char text[256];
    const nvs_item_t *it = nvs_find("apps_nvs", "apps", "order");
    if (it == NULL) {
        return "(none)";
    }
    size_t n = it->len < sizeof text - 1 ? it->len : sizeof text - 1;
    for (size_t i = 0; i < n; i++) {
        text[i] = it->blob[i] == '\0' ? ' ' : (char)it->blob[i];
    }
    text[n] = '\0';
    return text;
}

static void commit_app(const char *id) {
    const file_t files[] = {{"main.lua", "-- main\n"}};
    apps_pkg_t *p = make_pkg(id, id, "1", 1, files);
    bool unchanged;
    char err[256];
    CHECK(apps_store_commit(p, &unchanged, err, sizeof err), "commit %s: %s", id, err);
    free_pkg(p);
}

static void load_app(const char *id) {
    apps_pkg_t *got = apps_port_realloc(NULL, sizeof *got);
    char err[256];
    CHECK(apps_store_load(id, got, err, sizeof err), "load %s: %s", id, err);
    apps_pkg_free(got, apps_port_free);
    apps_port_free(got);
}

/* P2: the launcher's order is the store's, kept in apps_nvs ("order"), not the folders' times: an install (changed or unchanged) and a
 * launch (a load for a run) make the app the first, and each writes the order; a remount reads it, with no valid clock. An id the order
 * names with no folder is left out, and a folder it doesn't name goes after the rest, by id. */
static void order_follows_installs_and_launches(void) {
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    commit_app("aa");
    commit_app("bb");
    commit_app("cc");
    CHECK(strcmp(list_text(), "cc bb aa") == 0, "installs: '%s'", list_text());
    CHECK(strcmp(order_text(), "cc bb aa ") == 0, "the order is written: '%s'", order_text());
    load_app("aa");
    CHECK(strcmp(list_text(), "aa cc bb") == 0, "a launch makes it the first: '%s'", list_text());
    CHECK(listed_n == 3 && strcmp(listed[0].id, "aa") == 0 && strcmp(listed[2].id, "bb") == 0, "and the launcher has it");
    commit_app("bb");  /* the same package: unchanged */
    CHECK(strcmp(list_text(), "bb aa cc") == 0 && strcmp(order_text(), "bb aa cc ") == 0, "an unchanged reinstall too: '%s' '%s'",
          list_text(), order_text());
    CHECK(!clock_set, "no valid clock");
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "remount");
    CHECK(strcmp(list_text(), "bb aa cc") == 0, "a remount keeps it: '%s'", list_text());
    put_app("apps_fs/zz", "zz", "Zz", "-- z\n");
    put_app("apps_fs/ab", "ab", "Ab", "-- ab\n");
    char rm[] = "rm -rf apps_fs/aa";
    CHECK(system(rm) == 0, "aa's folder goes behind the store's back");
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "remount");
    CHECK(strcmp(list_text(), "bb cc ab zz") == 0, "aa left out; ab and zz after the rest, by id: '%s'", list_text());
    commit_app("zz");
    CHECK(strcmp(order_text(), "zz bb cc ab ") == 0, "the next write has the order as it is now: '%s'", order_text());
}

/* P3: the order's write takes its cost from the budget; when it doesn't fit, the list and the launcher change at once and the write
 * waits, and the app task's tick asks the worker for it once it fits. */
static void order_waits_for_the_budget(void) {
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    commit_app("aa");
    CHECK(strcmp(order_text(), "aa ") == 0, "written: '%s'", order_text());
    CHECK(budget_takes >= 1 && budget_bytes >= 3 + 32, "its cost taken: %u bytes", (unsigned)budget_bytes);
    budget_spent = true;
    commit_app("bb");
    CHECK(strcmp(list_text(), "bb aa") == 0 && listed_n == 2 && strcmp(listed[0].id, "bb") == 0, "the list has it at once");
    CHECK(strcmp(order_text(), "aa ") == 0, "the write waits: '%s'", order_text());
    apps_store_tick(mono_ms);
    CHECK(n_jobs == 0, "the tick asks for nothing while it doesn't fit");
    budget_spent = false;
    apps_store_tick(mono_ms + 1000);
    CHECK(n_jobs == 1, "it fits: the tick asks the worker for it");
    run_jobs();
    CHECK(strcmp(order_text(), "bb aa ") == 0, "written: '%s'", order_text());
    apps_store_tick(mono_ms + 5000);
    CHECK(n_jobs == 0, "once");
}

/* T: no memory for the stored manifest is no damage: the load says so, as it does for a file, and doesn't say to install it again. */
static void out_of_memory_reading_the_manifest(void) {
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    commit_app("timer");
    apps_pkg_t *got = apps_port_realloc(NULL, sizeof *got);
    char err[256];
    long held = blocks_held;
    alloc_fail_in = 0;
    CHECK(!apps_store_load("timer", got, err, sizeof err) && strcmp(err, "out of memory: no room to load the app") == 0, "'%s'", err);
    alloc_fail_in = -1;
    CHECK(blocks_held == held, "nothing held");
    apps_port_free(got);
}

/* C1: a load from the FAT is admitted only when free PSRAM holds the app's files (their sizes on the FAT) and still keeps its heap
 * (memory_kb) and APPS_RAM_RESERVE, the RAM store's formula with nothing to drop: short of that it is refused in the RAM store's
 * words and nothing is held. The app that runs makes no second heap (a launch of it only fronts it, and a run admits its heap again
 * once the old copy has gone: apps_task.h), so its own load needs room for its files and the reserve alone. */
static void a_load_is_admitted_against_free_psram(void) {
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    const file_t files[] = {{"main.lua", "-- main\nreturn {}\n"}, {"util.lua", "return 1\n"}};
    apps_pkg_t *p = make_pkg("timer", "Timer", "1.0.0", 2, files);
    bool unchanged;
    char err[256], want[256];
    CHECK(apps_store_commit(p, &unchanged, err, sizeof err), "commit: %s", err);
    CHECK(p->manifest.memory_kb == 256, "the default heap: %u KB", p->manifest.memory_kb);
    free_pkg(p);
    size_t bytes = strlen(files[0].text) + strlen(files[1].text);
    size_t need = bytes + 256 * 1024 + APPS_RAM_RESERVE;
    apps_pkg_t *got = apps_port_realloc(NULL, sizeof *got);
    long held = blocks_held;
    psram_free = need - 1;
    snprintf(want, sizeof want, "the gadget is short of memory for timer: %u KB free, %u KB needed; restart it or install a smaller app",
             (unsigned)((need - 1) / 1024), (unsigned)((need + 1023) / 1024));
    CHECK(!apps_store_load("timer", got, err, sizeof err) && strcmp(err, want) == 0, "short by a byte: '%s'", err);
    CHECK(blocks_held == held, "%ld blocks held after the refusal", blocks_held - held);
    psram_free = need;
    CHECK(apps_store_load("timer", got, err, sizeof err), "exactly the room it needs: %s", err);
    apps_pkg_free(got, apps_port_free);
    snprintf(running_id, sizeof running_id, "timer");
    psram_free = bytes + APPS_RAM_RESERVE;
    CHECK(apps_store_load("timer", got, err, sizeof err), "the app that runs, with room for its files and the reserve: %s", err);
    apps_pkg_free(got, apps_port_free);
    psram_free = bytes + APPS_RAM_RESERVE - 1;
    CHECK(!apps_store_load("timer", got, err, sizeof err) && strstr(err, "the gadget is short of memory for timer: ") == err,
          "the app that runs, short by a byte: '%s'", err);
    running_id[0] = '\0';
    psram_free = (size_t)3 << 20;
    apps_port_free(got);
    CHECK(blocks_held == held - 1, "%ld blocks held", blocks_held - held + 1);
}

/* N3: a load beside another app that is loaded is admitted twice, and the other is left alone: (a) free PSRAM now holds the files
 * and APPS_RAM_RESERVE; (b) free PSRAM now and what the other's end frees (its Lua heap and package: apps_task_footprint) hold the
 * files, the heap and the reserve. Short of (b) even its end leaves no room, and the refusal says so; short of (a) alone the other
 * must stop first, and the refusal says that. With no word from the app task, the load is admitted as it was (C1). The app's own
 * load (the same id) is unchanged. */
static void a_load_beside_another_app_counts_what_its_end_frees(void) {
    CHECK(apps_store_mount() == APPS_STORAGE_FLASH, "flash");
    const file_t files[] = {{"main.lua", "-- main\nreturn {}\n"}, {"util.lua", "return 1\n"}};
    apps_pkg_t *p = make_pkg("timer", "Timer", "1.0.0", 2, files);
    bool unchanged;
    char err[300], want[300];
    CHECK(apps_store_commit(p, &unchanged, err, sizeof err), "commit: %s", err);
    free_pkg(p);
    size_t bytes = strlen(files[0].text) + strlen(files[1].text), heap = 256 * 1024;
    size_t load = bytes + APPS_RAM_RESERVE, run = bytes + heap + APPS_RAM_RESERVE;
    apps_pkg_t *got = apps_port_realloc(NULL, sizeof *got);
    long held = blocks_held;
    snprintf(running_id, sizeof running_id, "big");
    snprintf(footprint_id, sizeof footprint_id, "big");
    /* Short of the heap now, which big's end frees. */
    footprint_bytes = (size_t)1300 * 1024;
    psram_free = load + 10 * 1024;
    CHECK(apps_store_load("timer", got, err, sizeof err), "beside big, which frees 1300 KB: %s", err);
    apps_pkg_free(got, apps_port_free);
    /* Exactly enough, both ways. */
    psram_free = load;
    footprint_bytes = heap;
    CHECK(apps_store_load("timer", got, err, sizeof err), "exactly the room, now and once big ends: %s", err);
    apps_pkg_free(got, apps_port_free);
    /* (b): a byte short once big ends. */
    footprint_bytes = heap - 1;
    snprintf(want, sizeof want, "the gadget is short of memory for timer: %u KB free once big ends, %u KB needed; restart the gadget or "
             "install a smaller app", (unsigned)((run - 1) / 1024), (unsigned)((run + 1023) / 1024));
    CHECK(!apps_store_load("timer", got, err, sizeof err) && strcmp(err, want) == 0, "short by a byte once big ends: '%s'", err);
    CHECK(blocks_held == held, "%ld blocks held after the refusal", blocks_held - held);
    /* (a): a byte short now, with room once big ends. */
    footprint_bytes = (size_t)2 << 20;
    psram_free = load - 1;
    snprintf(want, sizeof want, "the gadget is short of memory to load timer while big runs: %u KB free, %u KB needed; stop big, then "
             "launch timer", (unsigned)((load - 1) / 1024), (unsigned)((load + 1023) / 1024));
    CHECK(!apps_store_load("timer", got, err, sizeof err) && strcmp(err, want) == 0, "short by a byte now: '%s'", err);
    CHECK(blocks_held == held, "%ld blocks held after the refusal", blocks_held - held);
    /* Both short: (b) says it, as stopping big wouldn't help. */
    psram_free = load - 1;
    footprint_bytes = 0;
    CHECK(!apps_store_load("timer", got, err, sizeof err) && strstr(err, " KB free once big ends, ") != NULL, "both short: '%s'", err);
    /* No word from the app task: C1's formula, as before. */
    footprint_id[0] = '\0';
    psram_free = run - 1;
    CHECK(!apps_store_load("timer", got, err, sizeof err) && strstr(err, "the gadget is short of memory for timer: ") == err &&
              strstr(err, "once") == NULL, "nothing loaded: '%s'", err);
    psram_free = run;
    CHECK(apps_store_load("timer", got, err, sizeof err), "nothing loaded, the room: %s", err);
    apps_pkg_free(got, apps_port_free);
    /* The same id: its files and the reserve, whatever it frees. */
    snprintf(running_id, sizeof running_id, "timer");
    snprintf(footprint_id, sizeof footprint_id, "timer");
    footprint_bytes = (size_t)2 << 20;
    psram_free = load - 1;
    CHECK(!apps_store_load("timer", got, err, sizeof err) && strstr(err, "the gadget is short of memory for timer: ") == err,
          "the app that runs, short by a byte: '%s'", err);
    psram_free = load;
    CHECK(apps_store_load("timer", got, err, sizeof err), "the app that runs: %s", err);
    apps_pkg_free(got, apps_port_free);
    running_id[0] = footprint_id[0] = '\0';
    footprint_bytes = 0;
    psram_free = (size_t)3 << 20;
    apps_port_free(got);
    CHECK(blocks_held == held - 1, "%ld blocks held", blocks_held - held + 1);
}

/* ---- main ---- */

typedef struct {
    const char *name;
    void (*fn)(void);
} case_t;

static const case_t CASES[] = {
    {"commit_creates_and_replaces", commit_creates_and_replaces},
    {"recovery_restores_old_when_app_missing", recovery_restores_old_when_app_missing},
    {"recovery_deletes_stray_stage", recovery_deletes_stray_stage},
    {"old_layout_stage_is_deleted", old_layout_stage_is_deleted},
    {"remove", remove_app},
    {"list_reads_names", list_reads_names},
    {"list_carries_icon_paths", list_carries_icon_paths},
    {"list_has_every_app", list_has_every_app},
    {"read_file_reads_an_apps_file", read_file_reads_an_apps_file},
    {"read_file_needs_the_flash_store", read_file_needs_the_flash_store},
    {"a_refused_list_block_publishes_nothing", a_refused_list_block_publishes_nothing},
    {"unordered_tail_is_alphabetical_by_name", unordered_tail_is_alphabetical_by_name},
    {"unchanged_skips_commit", unchanged_skips_commit},
    {"values_roundtrip", values_roundtrip},
    {"values_that_cannot_be_read_are_not_empty", values_that_cannot_be_read_are_not_empty},
    {"keys_that_collide_stay_apart", keys_that_collide_stay_apart},
    {"full_nvs_erases_only_apps_nvs", full_nvs_erases_only_apps_nvs},
    {"new_nvs_version_erases_only_apps_nvs", new_nvs_version_erases_only_apps_nvs},
    {"ram_mode_when_partition_missing", ram_mode_when_partition_missing},
    {"marker_quarantines_after_two_resets", marker_quarantines_after_two_resets},
    {"list_before_mount_is_empty", list_before_mount_is_empty},
    {"a_refused_mount_is_asked_again_later", a_refused_mount_is_asked_again_later},
    {"quarantine_refuses_until_reinstalled", quarantine_refuses_until_reinstalled},
    {"boot_line_says_the_count", boot_line_says_the_count},
    {"records_are_handed_over_at_the_mount", records_are_handed_over_at_the_mount},
    {"a_record_that_cannot_be_read_now_is_kept", a_record_that_cannot_be_read_now_is_kept},
    {"records_survive_a_list_that_failed", records_survive_a_list_that_failed},
    {"alarm_records_round_trip", alarm_records_round_trip},
    {"sync_alarms_touches_only_the_names_it_is_handed", sync_alarms_touches_only_the_names_it_is_handed},
    {"a_record_that_must_wait_drops_the_old_one_at_once", a_record_that_must_wait_drops_the_old_one_at_once},
    {"a_sync_leaves_the_records_nobody_touched", a_sync_leaves_the_records_nobody_touched},
    {"first_mount_formats", first_mount_formats},
    {"a_second_mount_is_not_new", a_second_mount_is_not_new},
    {"an_unmountable_partition_is_ram_mode", an_unmountable_partition_is_ram_mode},
    {"a_commit_that_cant_be_written_leaves_the_app", a_commit_that_cant_be_written_leaves_the_app},
    {"a_damaged_copy_is_not_loaded", a_damaged_copy_is_not_loaded},
    {"order_follows_installs_and_launches", order_follows_installs_and_launches},
    {"order_waits_for_the_budget", order_waits_for_the_budget},
    {"out_of_memory_reading_the_manifest", out_of_memory_reading_the_manifest},
    {"a_load_is_admitted_against_free_psram", a_load_is_admitted_against_free_psram},
    {"a_load_beside_another_app_counts_what_its_end_frees", a_load_beside_another_app_counts_what_its_end_frees},
};

/* X: whatever a case leaves of the apps' heap besides the RAM store's copies, which the harness frees here, is a leak. */
static void ram_clear(void) {
    while (ram_n > 0) {
        free_pkg(ram_kept[--ram_n]);
    }
}

int main(int argc, char **argv) {
    if (argc == 2 && strcmp(argv[1], "--cases") == 0) {
        for (size_t i = 0; i < sizeof CASES / sizeof CASES[0]; i++) {
            printf("%s\n", CASES[i].name);
        }
        printf("reset_reason_counts\n");
        return 0;
    }
    if (argc < 3 || chdir(argv[2]) != 0) {
        fprintf(stderr, "usage: %s <case> <dir> [arg]\n", argv[0]);
        return 2;
    }
    long held = blocks_held;
    if (strcmp(argv[1], "reset_reason_counts") == 0 && argc == 4) {
        reset_reason_counts(atoi(argv[3]));
    } else {
        const case_t *c = NULL;
        for (size_t i = 0; c == NULL && i < sizeof CASES / sizeof CASES[0]; i++) {
            c = strcmp(CASES[i].name, argv[1]) == 0 ? &CASES[i] : NULL;
        }
        if (c == NULL) {
            fprintf(stderr, "no case %s\n", argv[1]);
            return 2;
        }
        c->fn();
    }
    ram_clear();
    CHECK(blocks_held == held, "%ld blocks of the apps' heap still held at the case's end", blocks_held - held);
    if (failures > 0) {
        dump_log();
        return 1;
    }
    return 0;
}
