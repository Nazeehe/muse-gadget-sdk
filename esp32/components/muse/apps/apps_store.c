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

#include "apps_store.h"

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"

#include "apps_install.h"
#include "apps_io.h"
#include "apps_port.h"
#include "apps_rt.h"
#include "apps_task.h"
#include "apps_values.h"

/*
 * The store of packages (apps_store.h has the contract). The I/O worker is the only task that touches the file system and apps_nvs,
 * so neither needs a lock of its own; what other tasks read (the list, the room) is a cache the worker writes under s_lock, held for
 * copies only. The mode is two: s_kind, the worker's own, set as the mount goes, and s_mode, the one other tasks read, an atomic set
 * at the mount's end (after the lock is made and the list read, before the launcher is given it).
 *
 * Nothing here recurses and no frame holds a large array (the worker's stack is 6 KB, and the values' ops can run inside a job's wait):
 * paths are PATH_CAP bytes, everything bigger (a file's bytes, a manifest, the list being built) is on the apps' heap.
 */

static const char *TAG = "muse_apps";

#define PATH_CAP 192                 /* APPS_STORE_BASE, ".stage/", an id and a path, with room to spare */
#define NAME_CAP 72                  /* a directory entry this store reads: an id, ".old-" and an id, a path's segment */
#define TREE_STEPS 8192              /* a removal's steps at most: far more than an app's files and folders */
#define VALUES_BLOB_MAX (APPS_ID_MAX + 1 + APPS_STORE_MAX_BYTES)
#define RECORD_LEN (APPS_ID_MAX + 1 + APPS_ALARM_NAME_MAX + 1 + 1 + 8)  /* id, name (each NUL-padded), flags, epoch ms (LE) */
#define RECORD_FLAGS (APPS_ID_MAX + 1 + APPS_ALARM_NAME_MAX + 1)
#define RECORD_EPOCH (RECORD_FLAGS + 1)
#define EPOCH_SLACK_MS 1000          /* a record whose time is this near the alarm's isn't written again */
#define KEYS_AT_ONCE 8               /* keys an iteration collects before it erases them (the iterator can't see a change) */
#define STALE_MAX 32                 /* leftovers recovery deals with in one mount */
#define QUARANTINE_FMT "%s was running during two restarts in a row; reinstall it to run it again"

typedef struct {
    char id[APPS_ID_MAX + 1], name[25];
    int64_t mtime;
} entry_t;

static apps_storage_t s_kind;        /* the worker's: what the mount has found so far */
static atomic_int s_mode;            /* apps_storage_t, for any task: NONE until the mount has run */
static StaticSemaphore_t s_lock_buf;
static SemaphoreHandle_t s_lock;     /* made by the mount, before s_mode */
static EXT_RAM_BSS_ATTR entry_t s_list[APPS_STORE_APPS_MAX];  /* under s_lock: most recently installed first */
static int s_list_n;
static uint32_t s_free_kb;           /* under s_lock */

static nvs_handle_t s_nvs;           /* the worker's */
static bool s_nvs_ok;

static atomic_bool s_mount_taken;    /* the mount job is the worker's */
static int64_t s_mount_retry_at;     /* the app task's */
static atomic_bool s_rearm_wanted;   /* the mount left records for a valid clock */
static int64_t s_rearm_retry_at;     /* the app task's */

static RTC_NOINIT_ATTR apps_marker_t s_marker;

const char *apps_storage_name(apps_storage_t s) {
    return s == APPS_STORAGE_FLASH ? "flash" : s == APPS_STORAGE_RAM ? "ram" : "none";
}

apps_storage_t apps_store_mode(void) {
    return (apps_storage_t)atomic_load(&s_mode);
}

apps_marker_t *apps_store_marker(void) {
    return &s_marker;
}

/* ---- Small things ---- */

static void put_err(char *err, size_t n, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
static void put_err(char *err, size_t n, const char *fmt, ...) {
    if (err == NULL || n == 0) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(err, n, fmt, ap);
    va_end(ap);
}

/* The CRC-32 of s, zlib's (the keys of apps_nvs). */
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

/* kind (s, a or q) and the CRC-32 of id, or of "id/name". */
static void key_of(char out[NVS_KEY_NAME_MAX_SIZE], char kind, const char *id, const char *name) {
    char what[APPS_ID_MAX + 1 + APPS_ALARM_NAME_MAX + 1];
    if (name != NULL) {
        snprintf(what, sizeof what, "%s/%s", id, name);
    } else {
        snprintf(what, sizeof what, "%s", id);
    }
    snprintf(out, NVS_KEY_NAME_MAX_SIZE, "%c%08x", kind, (unsigned)crc32_of(what));
}

/* src into dst (cap bytes), cut at cap - 1 bytes: for a field whose end the compiler can't see (a struct's array, RTC memory). */
static void copy_text(char *dst, size_t cap, const char *src) {
    size_t n = strnlen(src, cap - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* A block of the apps' heap holding data[0, n) (one byte for nothing). */
static void *dup_block(const void *data, size_t n) {
    void *copy = apps_port_realloc(NULL, n > 0 ? n : 1);
    if (copy != NULL && n > 0) {
        memcpy(copy, data, n);
    }
    return copy;
}

/* ---- Paths and files ---- */

static void path_of(char *out, const char *a, const char *b) {
    if (b != NULL) {
        snprintf(out, PATH_CAP, "%s/%s/%s", APPS_STORE_BASE, a, b);
    } else {
        snprintf(out, PATH_CAP, "%s/%s", APPS_STORE_BASE, a);
    }
}

/* out = dir/rel (PATH_CAP bytes): false, with errno ENAMETOOLONG, when it doesn't fit, which an id and a path the store takes never
 * make. */
static bool join(char *out, const char *dir, const char *rel) {
    int n = snprintf(out, PATH_CAP, "%s/%s", dir, rel);
    if (n > 0 && n < PATH_CAP) {
        return true;
    }
    errno = ENAMETOOLONG;
    return false;
}

static void stage_of(char *out, const char *id) {
    snprintf(out, PATH_CAP, "%s/.stage/%s", APPS_STORE_BASE, id);
}

static void old_of(char *out, const char *id) {
    snprintf(out, PATH_CAP, "%s/.old-%s", APPS_STORE_BASE, id);
}

static bool is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

static bool exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0;
}

/* The first entry of dir (not "." or ".."), into name: false when it has none or can't be read (*readable says which). */
static bool first_entry(const char *dir, char name[NAME_CAP], bool *readable) {
    DIR *d = opendir(dir);
    *readable = d != NULL;
    if (d == NULL) {
        return false;
    }
    bool found = false;
    struct dirent *e;
    while (!found && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) {
            continue;
        }
        found = strlen(e->d_name) < NAME_CAP;
        if (found) {
            snprintf(name, NAME_CAP, "%s", e->d_name);
        } else {
            *readable = false;  /* a name longer than this store ever writes: left as it is */
            break;
        }
    }
    closedir(d);
    return found;
}

/* Removes path and everything under it, without recursion: each step opens the folder it is in again (a folder that changes under
 * readdir is read afresh), takes its first entry, and goes into it or deletes it; an empty folder goes and the walk climbs. True when
 * nothing is left. */
static bool remove_tree(const char *path) {
    char at[PATH_CAP];
    snprintf(at, sizeof at, "%s", path);
    struct stat st;
    if (stat(at, &st) != 0) {
        return errno == ENOENT;
    }
    if (!S_ISDIR(st.st_mode)) {
        return unlink(at) == 0;
    }
    const size_t root = strlen(at);
    for (int step = 0; step < TREE_STEPS; step++) {
        char name[NAME_CAP];
        bool readable;
        if (!first_entry(at, name, &readable)) {
            if (!readable || rmdir(at) != 0) {
                return false;
            }
            if (strlen(at) == root) {
                return true;
            }
            *strrchr(at, '/') = '\0';  /* up to the folder it was in */
            continue;
        }
        size_t len = strlen(at);
        if (len + 1 + strlen(name) >= sizeof at) {
            return false;
        }
        at[len] = '/';
        snprintf(at + len + 1, sizeof at - len - 1, "%s", name);
        if (stat(at, &st) != 0) {
            return false;
        }
        if (!S_ISDIR(st.st_mode)) {
            if (unlink(at) != 0) {
                return false;
            }
            at[len] = '\0';
        }
    }
    return false;
}

/* The folders of path after its first `from` bytes, made where they aren't (a file's under an app's folder). */
static bool make_parents(char *path, size_t from) {
    for (char *p = path + from; *p != '\0'; p++) {
        if (*p != '/') {
            continue;
        }
        *p = '\0';
        bool ok = mkdir(path, 0777) == 0 || errno == EEXIST;
        *p = '/';
        if (!ok) {
            return false;
        }
    }
    return true;
}

/* data[0, len) as the file path, made or emptied first. */
static bool write_file(const char *path, const void *data, size_t len) {
    int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        return false;
    }
    const uint8_t *p = data;
    size_t left = len;
    while (left > 0) {
        ssize_t put = write(fd, p, left);
        if (put <= 0) {
            int saved = errno;
            close(fd);
            errno = put == 0 ? ENOSPC : saved;
            return false;
        }
        p += put;
        left -= (size_t)put;
    }
    return close(fd) == 0;
}

/* The file path into a block of the apps' heap with a NUL after its *len bytes: false, with why (cap bytes), when it can't be read, is
 * larger than max, or there is no memory. */
static bool read_file(const char *path, size_t max, char **out, size_t *len, char *why, size_t cap) {
    *out = NULL;
    *len = 0;
    int fd = open(path, O_RDONLY);
    struct stat st;
    if (fd < 0 || fstat(fd, &st) != 0) {
        put_err(why, cap, "%s", strerror(errno));
        if (fd >= 0) {
            close(fd);
        }
        return false;
    }
    size_t size = (size_t)st.st_size;
    if (st.st_size < 0 || size > max) {
        close(fd);
        put_err(why, cap, "larger than %u KB", (unsigned)(max / 1024));
        return false;
    }
    char *buf = apps_port_realloc(NULL, size + 1);
    if (buf == NULL) {
        close(fd);
        put_err(why, cap, "out of memory");
        return false;
    }
    size_t got = 0;
    while (got < size) {
        ssize_t n = read(fd, buf + got, size - got);
        if (n <= 0) {
            break;
        }
        got += (size_t)n;
    }
    close(fd);
    if (got != size) {
        apps_port_free(buf);
        put_err(why, cap, "it ends early");
        return false;
    }
    buf[size] = '\0';
    *out = buf;
    *len = size;
    return true;
}

/* ---- The cache (any task reads it) ---- */

static void lock(void) {
    xSemaphoreTake(s_lock, portMAX_DELAY);
}

static void unlock(void) {
    xSemaphoreGive(s_lock);
}

/* Whether the cache lists id: the worker's question (the cache is empty until the mount reads it). */
static bool installed(const char *id) {
    if (s_lock == NULL || id == NULL) {
        return false;
    }
    lock();
    bool found = false;
    for (int i = 0; i < s_list_n && !found; i++) {
        found = strcmp(s_list[i].id, id) == 0;
    }
    unlock();
    return found;
}

int apps_store_list(char ids[][APPS_ID_MAX + 1], char names[][25], int max) {
    if (apps_store_mode() == APPS_STORAGE_NONE || max <= 0) {
        return 0;
    }
    lock();
    int n = s_list_n < max ? s_list_n : max;
    for (int i = 0; i < n; i++) {
        copy_text(ids[i], APPS_ID_MAX + 1, s_list[i].id);
        copy_text(names[i], 25, s_list[i].name);
    }
    unlock();
    return n;
}

bool apps_store_has(const char *id) {
    return apps_store_mode() != APPS_STORAGE_NONE && installed(id);
}

uint32_t apps_store_free_kb(void) {
    switch (apps_store_mode()) {
    case APPS_STORAGE_RAM:
        return (uint32_t)(apps_port_psram_free() / 1024);
    case APPS_STORAGE_FLASH: {
        lock();
        uint32_t kb = s_free_kb;
        unlock();
        return kb;
    }
    default:
        return 0;
    }
}

/* The launcher's list (apps_task.h): the first APPS_TASK_LIST_MAX of the cache. */
static void publish(void) {
    apps_listed_t listed[APPS_TASK_LIST_MAX];
    lock();
    int n = s_list_n < APPS_TASK_LIST_MAX ? s_list_n : APPS_TASK_LIST_MAX;
    for (int i = 0; i < n; i++) {
        copy_text(listed[i].id, sizeof listed[i].id, s_list[i].id);
        copy_text(listed[i].name, sizeof listed[i].name, s_list[i].name[0] != '\0' ? s_list[i].name : s_list[i].id);
    }
    unlock();
    apps_task_set_list(listed, n);
}

/* RAM mode: the cache is the RAM store's list, read again after each of its changes (the RAM store gives the launcher its own). */
static void cache_ram(void) {
    apps_ram_entry_t *e = apps_port_realloc(NULL, APPS_RAM_APPS * sizeof *e);
    int n = e != NULL ? apps_ram_list(e, APPS_RAM_APPS) : 0;
    lock();
    s_list_n = 0;
    for (int i = 0; i < n && i < APPS_STORE_APPS_MAX; i++) {
        copy_text(s_list[i].id, sizeof s_list[i].id, e[i].id);
        copy_text(s_list[i].name, sizeof s_list[i].name, e[i].name);
        s_list[i].mtime = 0;
        s_list_n++;
    }
    unlock();
    apps_port_free(e);
}

/* The room the FAT has, into the cache. */
static void cache_free(void) {
    uint64_t total = 0, free_bytes = 0;
    if (esp_vfs_fat_info(APPS_STORE_BASE, &total, &free_bytes) != ESP_OK) {
        return;
    }
    lock();
    s_free_kb = (uint32_t)(free_bytes / 1024);
    unlock();
}

/* id's manifest.json, read and parsed for the folder id, into m (zeroed on failure, with why). messages_json is apps_pkg.c's own
 * block (free). The bytes into *json (a block of the apps' heap) when json isn't NULL. */
static bool read_manifest(const char *id, apps_manifest_t *m, char **json, size_t *json_len, char *why, size_t cap) {
    char path[PATH_CAP];
    path_of(path, id, "manifest.json");
    char *text;
    size_t len;
    memset(m, 0, sizeof *m);
    if (!read_file(path, APPS_MANIFEST_MAX, &text, &len, why, cap)) {
        return false;
    }
    bool ok = apps_manifest_parse(text, len, id, CONFIG_MUSE_APPS_HEAP_MAX_KB, m, why, cap);
    if (ok && json != NULL) {
        *json = text;
        *json_len = len;
    } else {
        apps_port_free(text);
    }
    return ok;
}

/* Whether a comes before b in the list: the newer first, then by id. */
static bool newer(const entry_t *a, const entry_t *b) {
    return a->mtime != b->mtime ? a->mtime > b->mtime : strcmp(a->id, b->id) < 0;
}

/* The cache, read again from the folders: every one whose manifest parses for its name, the newest APPS_STORE_APPS_MAX. */
static void scan(void) {
    entry_t *found = apps_port_realloc(NULL, (APPS_STORE_APPS_MAX + 1) * sizeof *found);
    apps_manifest_t *m = apps_port_realloc(NULL, sizeof *m);
    DIR *d = found != NULL && m != NULL ? opendir(APPS_STORE_BASE) : NULL;
    int n = 0, left_out = 0;
    struct dirent *e;
    while (d != NULL && (e = readdir(d)) != NULL) {
        char path[PATH_CAP];
        struct stat st;
        if (strlen(e->d_name) > APPS_ID_MAX || !apps_id_valid(e->d_name)) {
            continue;  /* ".stage", ".store", ".old-<id>", and what the store never makes */
        }
        path_of(path, e->d_name, NULL);
        char why[96];
        if (stat(path, &st) != 0 || !S_ISDIR(st.st_mode) || !read_manifest(e->d_name, m, NULL, NULL, why, sizeof why)) {
            continue;
        }
        entry_t item;
        snprintf(item.id, sizeof item.id, "%s", e->d_name);
        snprintf(item.name, sizeof item.name, "%s", m->name);
        item.mtime = (int64_t)st.st_mtime;
        apps_manifest_clear(m, NULL);
        int at = n;
        for (; at > 0 && newer(&item, &found[at - 1]); at--) {
            found[at] = found[at - 1];
        }
        found[at] = item;
        if (n < APPS_STORE_APPS_MAX) {
            n++;
        } else {
            left_out++;  /* the oldest, in found[APPS_STORE_APPS_MAX], goes */
        }
    }
    if (d != NULL) {
        closedir(d);
    }
    if (found == NULL || m == NULL) {
        ESP_LOGE(TAG, "store: out of memory: the list of apps wasn't read");
    }
    if (left_out > 0) {
        ESP_LOGW(TAG, "store: %d more apps than the %d the gadget lists", left_out, APPS_STORE_APPS_MAX);
    }
    if (found != NULL && m != NULL) {
        lock();
        memcpy(s_list, found, (size_t)n * sizeof *found);
        s_list_n = n;
        unlock();
    }
    apps_port_free(found);
    apps_port_free(m);
}

/* ---- apps_nvs ---- */

static bool nvs_start(void) {
    esp_err_t err = nvs_flash_init_partition(APPS_STORE_NVS);
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "store: apps_nvs: %s: erasing it (the pairing's nvs is not touched)", esp_err_to_name(err));
        err = nvs_flash_erase_partition(APPS_STORE_NVS);
        if (err == ESP_OK) {
            err = nvs_flash_init_partition(APPS_STORE_NVS);
        }
    }
    if (err == ESP_OK) {
        err = nvs_open_from_partition(APPS_STORE_NVS, APPS_STORE_NAMESPACE, NVS_READWRITE, &s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "store: apps_nvs can't be used (%s): values, alarms and quarantines aren't kept", esp_err_to_name(err));
        return false;
    }
    return true;
}

/* The blob under key into a block of the apps' heap (one byte more, for a NUL), its length in *len: NULL when there is none, or it is
 * longer than max, or there is no memory (*missing says the first). */
static uint8_t *blob_get(const char *key, size_t max, size_t *len, bool *missing) {
    *len = 0;
    *missing = false;
    size_t size = 0;
    esp_err_t err = nvs_get_blob(s_nvs, key, NULL, &size);
    if (err != ESP_OK) {
        *missing = err == ESP_ERR_NVS_NOT_FOUND;
        return NULL;
    }
    if (size > max) {
        return NULL;
    }
    uint8_t *buf = apps_port_realloc(NULL, size + 1);
    if (buf == NULL) {
        return NULL;
    }
    if (nvs_get_blob(s_nvs, key, buf, &size) != ESP_OK) {
        apps_port_free(buf);
        return NULL;
    }
    buf[size] = '\0';
    *len = size;
    return buf;
}

static bool blob_set(const char *key, const void *data, size_t len) {
    esp_err_t err = nvs_set_blob(s_nvs, key, data, len);
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "store: apps_nvs: %s", esp_err_to_name(err));
    }
    return err == ESP_OK;
}

static void blob_erase(const char *key) {
    if (nvs_erase_key(s_nvs, key) == ESP_OK) {
        (void)nvs_commit(s_nvs);
    }
}

/* Whether the blob of key starts with id and a NUL: a key's own id, checked (the keys are CRC-32s). */
static bool blob_is(const uint8_t *blob, size_t len, const char *id) {
    size_t n = strlen(id);
    return len > n && memcmp(blob, id, n) == 0 && blob[n] == '\0';
}

static bool quarantined(const char *id) {
    if (!s_nvs_ok) {
        return false;
    }
    char key[NVS_KEY_NAME_MAX_SIZE];
    key_of(key, 'q', id, NULL);
    size_t len;
    bool missing;
    uint8_t *blob = blob_get(key, APPS_ID_MAX + 1, &len, &missing);
    bool mine = blob != NULL && blob_is(blob, len, id);
    apps_port_free(blob);
    return mine;
}

static void quarantine_clear(const char *id) {
    if (s_nvs_ok && quarantined(id)) {
        char key[NVS_KEY_NAME_MAX_SIZE];
        key_of(key, 'q', id, NULL);
        blob_erase(key);
        ESP_LOGI(TAG, "store: %s was reinstalled: its quarantine is over", id);
    }
}

/* An alarm's record, read. */
typedef struct {
    char id[APPS_ID_MAX + 1], name[APPS_ALARM_NAME_MAX + 1];
    int64_t epoch_ms;
    bool sound, wake;
} record_t;

static void record_pack(uint8_t out[RECORD_LEN], const char *id, const char *name, int64_t epoch_ms, bool sound, bool wake) {
    memset(out, 0, RECORD_LEN);
    memcpy(out, id, strlen(id));
    memcpy(out + APPS_ID_MAX + 1, name, strlen(name));
    out[RECORD_FLAGS] = (uint8_t)((sound ? 1 : 0) | (wake ? 2 : 0));
    for (int i = 0; i < 8; i++) {
        out[RECORD_EPOCH + i] = (uint8_t)((uint64_t)epoch_ms >> (8 * i));
    }
}

static bool record_unpack(const uint8_t *blob, size_t len, record_t *r) {
    if (len != RECORD_LEN || blob[APPS_ID_MAX] != '\0' || blob[RECORD_FLAGS - 1] != '\0') {
        return false;
    }
    snprintf(r->id, sizeof r->id, "%s", (const char *)blob);
    snprintf(r->name, sizeof r->name, "%s", (const char *)blob + APPS_ID_MAX + 1);
    r->sound = (blob[RECORD_FLAGS] & 1) != 0;
    r->wake = (blob[RECORD_FLAGS] & 2) != 0;
    uint64_t epoch = 0;
    for (int i = 0; i < 8; i++) {
        epoch |= (uint64_t)blob[RECORD_EPOCH + i] << (8 * i);
    }
    r->epoch_ms = (int64_t)epoch;
    return apps_id_valid(r->id) && apps_alarm_name_valid(r->name);
}

/*
 * Every record (the a keys of the namespace) to visit(r, ctx), which says whether the key is to go; the keys that are go once the
 * iteration has let go of them, KEYS_AT_ONCE at a time (NVS's iterator can't see a change made under it), and a record that can't be
 * read goes too. More to go than a pass takes is another pass, which visits what is left again: each visit is one that can be made
 * twice (a record armed again in the first pass is one the app has armed by the second). Returns how many records the first pass read.
 */
typedef bool (*visit_fn)(const record_t *r, void *ctx);

static int each_record(visit_fn visit, void *ctx) {
    int visited = 0;
    for (int pass = 0; pass < 16; pass++) {
        char doomed[KEYS_AT_ONCE][NVS_KEY_NAME_MAX_SIZE];
        int n_doomed = 0;
        bool more = false;
        nvs_iterator_t it = NULL;
        esp_err_t err = nvs_entry_find(APPS_STORE_NVS, APPS_STORE_NAMESPACE, NVS_TYPE_BLOB, &it);
        while (err == ESP_OK) {
            nvs_entry_info_t info;
            nvs_entry_info(it, &info);
            if (info.key[0] == 'a') {
                size_t len;
                bool missing;
                uint8_t *blob = blob_get(info.key, RECORD_LEN, &len, &missing);
                record_t r;
                bool readable = blob != NULL && record_unpack(blob, len, &r);
                apps_port_free(blob);
                visited += pass == 0 && readable ? 1 : 0;
                if (!readable || visit(&r, ctx)) {
                    if (n_doomed < KEYS_AT_ONCE) {
                        snprintf(doomed[n_doomed++], NVS_KEY_NAME_MAX_SIZE, "%s", info.key);
                    } else {
                        more = true;
                    }
                }
            }
            err = nvs_entry_next(&it);
        }
        nvs_release_iterator(it);
        for (int i = 0; i < n_doomed; i++) {
            blob_erase(doomed[i]);
        }
        if (!more) {
            break;
        }
    }
    return visited;
}

/* ---- Values and alarms (the worker's) ---- */

bool apps_store_save_values(const char *id, const char *json, size_t len) {
    if (s_kind != APPS_STORAGE_FLASH || !s_nvs_ok || !apps_id_valid(id) || len > APPS_STORE_MAX_BYTES) {
        return false;
    }
    if (!installed(id)) {
        ESP_LOGW(TAG, "store: values for %s dropped: it isn't installed", id);
        return true;
    }
    char key[NVS_KEY_NAME_MAX_SIZE];
    key_of(key, 's', id, NULL);
    if (len == 2 && memcmp(json, "{}", 2) == 0) {
        blob_erase(key);  /* an empty store keeps no key */
        return true;
    }
    size_t idlen = strlen(id), n = idlen + 1 + len;
    uint8_t *blob = apps_port_realloc(NULL, n);
    if (blob == NULL) {
        return false;
    }
    memcpy(blob, id, idlen + 1);
    memcpy(blob + idlen + 1, json, len);
    bool ok = blob_set(key, blob, n);
    apps_port_free(blob);
    return ok;
}

char *apps_store_load_values(const char *id, size_t *len) {
    *len = 0;
    char *empty = dup_block("{}", 3);
    if (empty == NULL) {
        return NULL;
    }
    *len = 2;
    if (s_kind != APPS_STORAGE_FLASH || !s_nvs_ok || !apps_id_valid(id)) {
        return empty;
    }
    char key[NVS_KEY_NAME_MAX_SIZE];
    key_of(key, 's', id, NULL);
    size_t size;
    bool missing;
    uint8_t *blob = blob_get(key, VALUES_BLOB_MAX, &size, &missing);
    if (blob == NULL) {
        if (!missing) {
            ESP_LOGW(TAG, "store: the values of %s can't be read: it starts with none", id);
        }
        return empty;
    }
    if (!blob_is(blob, size, id)) {
        ESP_LOGW(TAG, "store: the values under %s's key are another app's: it starts with none", id);
        apps_port_free(blob);
        return empty;
    }
    apps_port_free(empty);
    size_t skip = strlen(id) + 1;
    memmove(blob, blob + skip, size - skip);
    blob[size - skip] = '\0';
    *len = size - skip;
    return (char *)blob;
}

bool apps_store_save_alarm(const char *id, const char *name, int64_t epoch_ms, bool sound, bool wake) {
    if (s_kind != APPS_STORAGE_FLASH || !s_nvs_ok || !apps_id_valid(id) || !apps_alarm_name_valid(name)) {
        return false;
    }
    if (!installed(id)) {
        return true;  /* moot: it was removed */
    }
    uint8_t rec[RECORD_LEN];
    record_pack(rec, id, name, epoch_ms, sound, wake);
    char key[NVS_KEY_NAME_MAX_SIZE];
    key_of(key, 'a', id, name);
    return blob_set(key, rec, sizeof rec);
}

static bool visit_app(const record_t *r, void *ctx) {
    return strcmp(r->id, (const char *)ctx) == 0;
}

void apps_store_drop_alarm(const char *id, const char *name) {
    if (s_kind != APPS_STORAGE_FLASH || !s_nvs_ok || !apps_id_valid(id)) {
        return;
    }
    if (name == NULL) {
        (void)each_record(visit_app, (void *)id);
        return;
    }
    char key[NVS_KEY_NAME_MAX_SIZE];
    key_of(key, 'a', id, name);
    blob_erase(key);
}

typedef struct {
    const char *id;
    const apps_store_alarm_t *alarms;
    int n;
    bool epochs;
    unsigned seen;    /* bit i: alarms[i] has its record already */
} sync_ctx_t;

static bool visit_sync(const record_t *r, void *arg) {
    sync_ctx_t *s = arg;
    if (strcmp(r->id, s->id) != 0) {
        return false;
    }
    for (int i = 0; i < s->n; i++) {
        const apps_store_alarm_t *a = &s->alarms[i];
        if (strcmp(a->name, r->name) == 0) {
            int64_t off = r->epoch_ms - a->epoch_ms;
            bool same = !s->epochs || ((off < 0 ? -off : off) <= EPOCH_SLACK_MS && r->sound == a->sound && r->wake == a->wake);
            s->seen |= same ? 1u << i : 0u;
            return false;
        }
    }
    return true;  /* no alarm of that name now */
}

void apps_store_sync_alarms(const char *id, const apps_store_alarm_t *alarms, int n, bool epochs) {
    if (s_kind != APPS_STORAGE_FLASH || !s_nvs_ok || !apps_id_valid(id)) {
        return;
    }
    sync_ctx_t s = {id, alarms, n < APPS_ALARMS_PER_APP ? n : APPS_ALARMS_PER_APP, epochs, 0};
    (void)each_record(visit_sync, &s);
    for (int i = 0; epochs && i < s.n; i++) {
        if ((s.seen & (1u << i)) == 0 &&
            !apps_store_save_alarm(id, alarms[i].name, alarms[i].epoch_ms, alarms[i].sound, alarms[i].wake)) {
            ESP_LOGW(TAG, "alarm %s/%s: no record was kept: it won't outlive a restart", id, alarms[i].name);
        }
    }
}

/* ---- Re-arming ---- */

typedef struct {
    int64_t now_epoch;
    int armed, dropped;
} rearm_ctx_t;

static bool visit_rearm(const record_t *r, void *arg) {
    rearm_ctx_t *c = arg;
    if (!installed(r->id) || quarantined(r->id)) {
        c->dropped++;
        return true;
    }
    if (apps_alarm_remaining(r->id, r->name) >= 0) {
        return false;  /* the app armed it itself since the boot: its record follows that one */
    }
    int64_t ms = r->epoch_ms - c->now_epoch;
    ms = ms < 1 ? 1 : ms > APPS_ALARM_MAX_MS ? APPS_ALARM_MAX_MS : ms;
    apps_alarm_result_t got = apps_alarm_arm(r->id, r->name, ms, r->sound, r->wake);
    if (got == APPS_ALARM_ARMED) {
        c->armed++;
        ESP_LOGI(TAG, "store: alarm %s/%s armed again: %lld ms left", r->id, r->name, (long long)ms);
        return false;
    }
    if (got == APPS_ALARM_NO_MEMORY) {
        ESP_LOGW(TAG, "store: alarm %s/%s: no memory to arm it again: asked again in %d s", r->id, r->name,
                 APPS_STORE_RETRY_MS / 1000);
        atomic_store(&s_rearm_wanted, true);
        return false;
    }
    ESP_LOGW(TAG, "store: alarm %s/%s couldn't be armed again (the gadget has no slot for it): dropped", r->id, r->name);
    return true;
}

static int count_records(void) {
    int n = 0;
    nvs_iterator_t it = NULL;
    esp_err_t err = nvs_entry_find(APPS_STORE_NVS, APPS_STORE_NAMESPACE, NVS_TYPE_BLOB, &it);
    while (err == ESP_OK) {
        nvs_entry_info_t info;
        nvs_entry_info(it, &info);
        n += info.key[0] == 'a' ? 1 : 0;
        err = nvs_entry_next(&it);
    }
    nvs_release_iterator(it);
    return n;
}

/* The records armed again, if the clock is valid; otherwise they wait for it (apps_store_tick). */
static void rearm(void) {
    if (s_kind != APPS_STORAGE_FLASH || !s_nvs_ok) {
        return;
    }
    rearm_ctx_t c = {0};
    if (!apps_port_epoch_ms(&c.now_epoch)) {
        int n = count_records();
        if (n > 0) {
            ESP_LOGI(TAG, "store: alarms wait for the clock (%d records)", n);
            atomic_store(&s_rearm_wanted, true);
        }
        return;
    }
    (void)each_record(visit_rearm, &c);
    if (c.dropped > 0) {
        ESP_LOGI(TAG, "store: %d alarm records of apps that aren't installed or are quarantined dropped", c.dropped);
    }
}

static void rearm_job(void *arg) {
    (void)arg;
    rearm();
}

/* ---- The quarantine ---- */

void apps_marker_launch(apps_marker_t *m, const char *id) {
    if (m->magic != APPS_MARKER_MAGIC) {
        memset(m, 0, sizeof *m);
        m->magic = APPS_MARKER_MAGIC;
    }
    if (strncmp(m->id, id, sizeof m->id) != 0) {
        snprintf(m->id, sizeof m->id, "%s", id);
        m->resets = 0;
    }
    m->running = 1;
}

void apps_marker_clean_stop(apps_marker_t *m) {
    m->running = 0;
    m->resets = 0;
}

bool apps_marker_boot(apps_marker_t *m, char *quarantine_id) {
    if (m->magic != APPS_MARKER_MAGIC) {
        memset(m, 0, sizeof *m);
        m->magic = APPS_MARKER_MAGIC;
        return false;
    }
    m->id[APPS_ID_MAX] = '\0';  /* RTC memory: never trust it to end */
    if (!m->running) {
        return false;
    }
    m->running = 0;
    if (++m->resets < 2) {
        return false;
    }
    snprintf(quarantine_id, APPS_ID_MAX + 1, "%s", m->id);
    m->resets = 0;
    return true;
}

static bool crash_reset(esp_reset_reason_t why) {
    return why == ESP_RST_PANIC || why == ESP_RST_INT_WDT || why == ESP_RST_TASK_WDT || why == ESP_RST_WDT ||
           why == ESP_RST_BROWNOUT || why == ESP_RST_CPU_LOCKUP;
}

static const char *reset_name(esp_reset_reason_t why) {
    switch (why) {
    case ESP_RST_POWERON: return "poweron";
    case ESP_RST_EXT: return "ext";
    case ESP_RST_SW: return "sw";
    case ESP_RST_PANIC: return "panic";
    case ESP_RST_INT_WDT: return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT: return "wdt";
    case ESP_RST_DEEPSLEEP: return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO: return "sdio";
    case ESP_RST_USB: return "usb";
    case ESP_RST_JTAG: return "jtag";
    case ESP_RST_EFUSE: return "efuse";
    case ESP_RST_PWR_GLITCH: return "pwr_glitch";
    case ESP_RST_CPU_LOCKUP: return "cpu_lockup";
    default: return "unknown";
    }
}

/* id is quarantined: its key, and its alarms gone (records and armed ones), so that no re-arm can launch it. */
static void quarantine(const char *id) {
    if (s_nvs_ok) {
        char key[NVS_KEY_NAME_MAX_SIZE];
        key_of(key, 'q', id, NULL);
        if (!blob_set(key, id, strlen(id) + 1)) {
            ESP_LOGW(TAG, "store: the quarantine of %s couldn't be kept", id);
        }
    }
    apps_store_drop_alarm(id, NULL);
    apps_alarm_cancel_app(id, "was running during two restarts in a row");
}

/* The boot's look at the marker, and its one line. RAM mode keeps nothing across a reset: the marker is cleared, and nothing counted. */
static void boot_check(void) {
    apps_marker_t *m = &s_marker;
    esp_reset_reason_t why = esp_reset_reason();
    bool valid = m->magic == APPS_MARKER_MAGIC, ran = valid && m->running != 0;
    char id[APPS_ID_MAX + 1] = "-";
    if (valid) {
        m->id[APPS_ID_MAX] = '\0';
        copy_text(id, sizeof id, m->id[0] != '\0' ? m->id : "-");
    }
    bool counts = crash_reset(why) && s_kind == APPS_STORAGE_FLASH;
    if (!counts) {
        apps_marker_clean_stop(m);
    }
    char q[APPS_ID_MAX + 1] = "";
    bool quarantined_now = apps_marker_boot(m, q) && apps_id_valid(q);
    if (quarantined_now) {
        quarantine(q);
    }
    const char *what = !valid ? "none (cleared)"
                       : !ran ? "nothing was running"
                       : !crash_reset(why) ? "was running, not a crash: cleared"
                       : !counts ? "was running (RAM mode keeps nothing across a reset): cleared"
                       : quarantined_now ? "was running during 2 crash resets in a row: quarantined"
                                         : "was running during a crash reset";
    ESP_LOGI(TAG, "boot: reset reason %s; app marker %s, %s", reset_name(why), id, what);
}

/* ---- The mount ---- */

static void recover(void) {
    char (*stale)[NAME_CAP] = apps_port_realloc(NULL, STALE_MAX * sizeof *stale);
    if (stale == NULL) {
        ESP_LOGE(TAG, "store: out of memory: no recovery this time");
        return;
    }
    int n = 0;
    DIR *d = opendir(APPS_STORE_BASE);
    struct dirent *e;
    while (d != NULL && n < STALE_MAX && (e = readdir(d)) != NULL) {
        if (strncmp(e->d_name, ".old-", 5) == 0 && strlen(e->d_name) < NAME_CAP) {
            snprintf(stale[n++], NAME_CAP, "%s", e->d_name);
        }
    }
    if (d != NULL) {
        closedir(d);
    }
    for (int i = 0; i < n; i++) {
        const char *id = stale[i] + 5;
        char old[PATH_CAP], app[PATH_CAP];
        snprintf(old, sizeof old, "%s/%s", APPS_STORE_BASE, stale[i]);
        path_of(app, id, NULL);
        if (apps_id_valid(id) && !exists(app) && rename(old, app) == 0) {
            ESP_LOGW(TAG, "store: restored %s from an interrupted install", id);
        } else if (remove_tree(old)) {
            ESP_LOGW(TAG, "store: removed a stray copy of %s", id);
        } else {
            ESP_LOGE(TAG, "store: couldn't remove the stray copy of %s", id);
        }
    }
    char stage[PATH_CAP];
    snprintf(stage, sizeof stage, "%s/.stage", APPS_STORE_BASE);
    n = 0;
    d = is_dir(stage) ? opendir(stage) : NULL;
    while (d != NULL && n < STALE_MAX && (e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0 && strlen(e->d_name) < NAME_CAP) {
            snprintf(stale[n++], NAME_CAP, "%s", e->d_name);
        }
    }
    if (d != NULL) {
        closedir(d);
    }
    if (exists(stage) && !remove_tree(stage)) {
        ESP_LOGE(TAG, "store: couldn't remove the unfinished installs");
    } else {
        for (int i = 0; i < n; i++) {
            ESP_LOGW(TAG, "store: removed the unfinished install of %s", stale[i]);
        }
    }
    apps_port_free(stale);
}

static apps_storage_t mount_ram(const char *why) {
    ESP_LOGW(TAG, "store: %s: apps are kept in RAM until the gadget restarts; " APPS_RAM_HINT, why);
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    s_kind = APPS_STORAGE_RAM;
    cache_ram();
    boot_check();
    atomic_store(&s_mode, APPS_STORAGE_RAM);
    apps_task_set_list(NULL, 0);  /* the launcher: no apps yet (it showed "Loading apps...") */
    return APPS_STORAGE_RAM;
}

apps_storage_t apps_store_mount(void) {
    int64_t began = apps_port_mono_ms();
    atomic_store(&s_mount_taken, true);
    if (esp_partition_find_first(ESP_PARTITION_TYPE_DATA, ESP_PARTITION_SUBTYPE_DATA_FAT, APPS_STORE_PARTITION) == NULL) {
        return mount_ram("no apps partition");
    }
    esp_vfs_fat_mount_config_t cfg = VFS_FAT_MOUNT_DEFAULT_CONFIG();
    cfg.format_if_mount_failed = true;
    cfg.max_files = 4;
    cfg.allocation_unit_size = APPS_STORE_UNIT;
    wl_handle_t wl = WL_INVALID_HANDLE;
    esp_err_t err = esp_vfs_fat_spiflash_mount_rw_wl(APPS_STORE_BASE, APPS_STORE_PARTITION, &cfg, &wl);
    if (err != ESP_OK) {
        char why[96];
        snprintf(why, sizeof why, "the apps partition can't be mounted (%s)", esp_err_to_name(err));
        return mount_ram(why);
    }
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutexStatic(&s_lock_buf);
    }
    char marker[PATH_CAP];
    path_of(marker, ".store", NULL);
    bool fresh = !exists(marker);
    if (fresh && !write_file(marker, "", 0)) {
        ESP_LOGW(TAG, "store: couldn't mark the file system as the store's");
    }
    s_kind = APPS_STORAGE_FLASH;
    s_nvs_ok = nvs_start();
    recover();
    scan();
    cache_free();
    boot_check();
    rearm();
    nvs_stats_t st = {0};
    if (!s_nvs_ok || nvs_get_stats(APPS_STORE_NVS, &st) != ESP_OK) {
        memset(&st, 0, sizeof st);
    }
    ESP_LOGI(TAG, "store: flash: mounted in %d ms%s, %u KB free; apps_nvs %u of %u entries used, %d apps",
             (int)(apps_port_mono_ms() - began), fresh ? ", a new file system" : "", (unsigned)s_free_kb,
             (unsigned)st.used_entries, (unsigned)st.total_entries, s_list_n);
    atomic_store(&s_mode, APPS_STORAGE_FLASH);
    publish();
    return APPS_STORAGE_FLASH;
}

void apps_store_mount_once(void) {
    if (s_kind == APPS_STORAGE_NONE) {
        (void)apps_store_mount();
    }
}

static void mount_job(void *arg) {
    (void)arg;
    apps_store_mount_once();
}

void apps_store_tick(int64_t now_ms) {
    if (apps_store_mode() == APPS_STORAGE_NONE) {
        if (!atomic_load(&s_mount_taken) && now_ms >= s_mount_retry_at) {
            if (apps_io_submit(mount_job, NULL)) {
                atomic_store(&s_mount_taken, true);
            } else {
                s_mount_retry_at = now_ms + APPS_STORE_RETRY_MS;  /* apps_io_submit said why */
            }
        }
        return;
    }
    int64_t epoch;
    if (atomic_load(&s_rearm_wanted) && now_ms >= s_rearm_retry_at && apps_port_epoch_ms(&epoch)) {
        if (apps_io_submit(rearm_job, NULL)) {
            atomic_store(&s_rearm_wanted, false);
        } else {
            s_rearm_retry_at = now_ms + APPS_STORE_RETRY_MS;
        }
    }
}

/* ---- Commit, remove, load (the worker's) ---- */

static uint64_t units(size_t bytes) {
    return ((uint64_t)bytes + APPS_STORE_UNIT - 1) / APPS_STORE_UNIT * APPS_STORE_UNIT;
}

/* What pkg takes on the FAT: each file's units, a unit for each folder (the app's own, each of its paths', the stage's), and a unit
 * of slack. */
static uint64_t room_needed(const apps_pkg_t *pkg) {
    uint64_t need = units(pkg->manifest_len) + 3 * APPS_STORE_UNIT;
    for (int i = 0; i < pkg->count; i++) {
        need += units(pkg->files[i].len);
        const char *path = pkg->files[i].path;
        for (const char *slash = strchr(path, '/'); slash != NULL; slash = strchr(slash + 1, '/')) {
            size_t prefix = (size_t)(slash - path);
            bool seen = false;
            for (int j = 0; j < i && !seen; j++) {
                seen = strncmp(pkg->files[j].path, path, prefix) == 0 && pkg->files[j].path[prefix] == '/';
            }
            need += seen ? 0 : APPS_STORE_UNIT;
        }
    }
    return need;
}

/* Whether the stored copy of pkg's id is pkg: manifest.json byte for byte, and each file by its length and SHA-256. */
static bool same_as_stored(const apps_pkg_t *pkg) {
    const char *id = pkg->manifest.id;
    char path[PATH_CAP], why[64];
    path_of(path, id, "manifest.json");
    char *data;
    size_t len;
    if (!read_file(path, APPS_MANIFEST_MAX, &data, &len, why, sizeof why)) {
        return false;
    }
    bool same = len == pkg->manifest_len && memcmp(data, pkg->manifest_json, len) == 0;
    apps_port_free(data);
    char dir[PATH_CAP];
    path_of(dir, id, NULL);
    for (int i = 0; same && i < pkg->count; i++) {  /* the lengths first: no file is read for a package that grew */
        struct stat st;
        same = join(path, dir, pkg->files[i].path) && stat(path, &st) == 0 && (size_t)st.st_size == pkg->files[i].len;
    }
    for (int i = 0; same && i < pkg->count; i++) {
        if (!join(path, dir, pkg->files[i].path) || !read_file(path, pkg->files[i].len, &data, &len, why, sizeof why)) {
            return false;
        }
        uint8_t sha[32];
        apps_port_sha256(data, len, sha);
        apps_port_free(data);
        same = memcmp(sha, pkg->files[i].sha256, sizeof sha) == 0;
    }
    return same;
}

/* pkg into the stage: false, with what failed in err. */
static bool write_stage(const apps_pkg_t *pkg, const char *stage, char *err, size_t n) {
    const char *id = pkg->manifest.id;
    char path[PATH_CAP];
    snprintf(path, sizeof path, "%s/.stage", APPS_STORE_BASE);
    (void)remove_tree(stage);  /* a leftover of this id's: recovery takes them at the mount, so this is rare */
    if ((mkdir(path, 0777) != 0 && errno != EEXIST) || mkdir(stage, 0777) != 0) {
        put_err(err, n, "couldn't write %s to the gadget's storage: its folder: %s", id, strerror(errno));
        return false;
    }
    if (!join(path, stage, "manifest.json") || !write_file(path, pkg->manifest_json, pkg->manifest_len)) {
        put_err(err, n, "couldn't write %s to the gadget's storage: manifest.json: %s", id, strerror(errno));
        return false;
    }
    for (int i = 0; i < pkg->count; i++) {
        const apps_blob_t *b = &pkg->files[i];
        if (!join(path, stage, b->path) || !make_parents(path, strlen(stage) + 1) || !write_file(path, b->data, b->len)) {
            put_err(err, n, "couldn't write %s to the gadget's storage: %s: %s", id, b->path, strerror(errno));
            return false;
        }
    }
    return true;
}

static bool commit_flash(const apps_pkg_t *pkg, bool *unchanged, char *err, size_t n) {
    const char *id = pkg->manifest.id;
    if (!apps_id_valid(id) || pkg->manifest_json == NULL) {
        put_err(err, n, "couldn't write %s to the gadget's storage: the package has no manifest.json", id);
        return false;
    }
    bool had = installed(id);
    if (had && same_as_stored(pkg)) {
        *unchanged = true;
        quarantine_clear(id);
        return true;
    }
    if (!had && s_list_n >= APPS_STORE_APPS_MAX) {
        put_err(err, n, "the gadget keeps at most %d apps; remove one first", APPS_STORE_APPS_MAX);
        return false;
    }
    uint64_t total = 0, free_bytes = 0, need = room_needed(pkg);
    if (esp_vfs_fat_info(APPS_STORE_BASE, &total, &free_bytes) == ESP_OK && need > free_bytes) {
        put_err(err, n, "not enough room on the gadget for %s: %u KB free, %u KB needed; remove an app you don't use", id,
                (unsigned)(free_bytes / 1024), (unsigned)((need + 1023) / 1024));
        return false;
    }
    char stage[PATH_CAP], app[PATH_CAP], old[PATH_CAP];
    stage_of(stage, id);
    path_of(app, id, NULL);
    old_of(old, id);
    if (!write_stage(pkg, stage, err, n)) {
        (void)remove_tree(stage);
        cache_free();
        return false;
    }
    bool replace = exists(app);
    if (exists(old)) {
        (void)remove_tree(old);
    }
    if (replace && rename(app, old) != 0) {
        put_err(err, n, "couldn't write %s to the gadget's storage: moving the old copy aside: %s", id, strerror(errno));
        (void)remove_tree(stage);
        cache_free();
        return false;
    }
    if (rename(stage, app) != 0) {
        put_err(err, n, "couldn't write %s to the gadget's storage: putting it in place: %s", id, strerror(errno));
        if (replace && rename(old, app) != 0) {
            ESP_LOGE(TAG, "store: the old copy of %s couldn't be put back: the next mount does", id);
        }
        (void)remove_tree(stage);
        cache_free();
        return false;
    }
    if (replace && !remove_tree(old)) {
        ESP_LOGW(TAG, "store: the old copy of %s couldn't be deleted: the next mount does", id);
    }
    quarantine_clear(id);
    scan();
    cache_free();
    publish();
    return true;
}

bool apps_store_commit(const apps_pkg_t *pkg, bool *unchanged, char *err, size_t n) {
    *unchanged = false;
    put_err(err, n, "%s", "");
    switch (s_kind) {
    case APPS_STORAGE_RAM: {
        bool ok = apps_ram_commit(pkg, unchanged, err, n);
        cache_ram();
        return ok;
    }
    case APPS_STORAGE_FLASH:
        return commit_flash(pkg, unchanged, err, n);
    default:
        put_err(err, n, "storage not ready");
        return false;
    }
}

bool apps_store_remove(const char *id, char *err, size_t n) {
    put_err(err, n, "%s", "");
    apps_storage_t mode = s_kind;
    if (mode == APPS_STORAGE_NONE) {
        put_err(err, n, "storage not ready");
        return false;
    }
    char running[APPS_ID_MAX + 1];
    if (id != NULL && apps_task_running(running, sizeof running) && strcmp(running, id) == 0) {
        put_err(err, n, "%s is running; stop it first", id);
        return false;
    }
    char app[PATH_CAP];
    if (id != NULL && apps_id_valid(id)) {
        path_of(app, id, NULL);
    }
    /* A folder that isn't listed (its manifest.json is damaged) can go too: it is the store's, and it takes room. */
    if (id == NULL || !apps_id_valid(id) || !(installed(id) || (mode == APPS_STORAGE_FLASH && is_dir(app)))) {
        put_err(err, n, "no such app on the gadget");
        return false;
    }
    if (mode == APPS_STORAGE_RAM) {
        bool ok = apps_ram_remove(id);
        apps_values_forget(id);
        apps_alarm_cancel_app(id, "was removed");
        cache_ram();
        if (!ok) {
            put_err(err, n, "no such app on the gadget");
        }
        return ok;
    }
    bool gone = remove_tree(app);
    if (s_nvs_ok) {
        char key[NVS_KEY_NAME_MAX_SIZE];
        key_of(key, 's', id, NULL);
        blob_erase(key);
        key_of(key, 'q', id, NULL);
        blob_erase(key);
        apps_store_drop_alarm(id, NULL);
    }
    apps_alarm_cancel_app(id, "was removed");
    scan();
    cache_free();
    publish();
    if (!gone) {
        put_err(err, n, "couldn't remove %s: %s", id, strerror(errno));
    }
    return gone;
}

/* messages_json into a block of the apps' heap (apps_pkg.c's own is malloc's): false without memory. */
static bool rehome_messages(apps_manifest_t *m) {
    if (m->messages_json == NULL) {
        return true;
    }
    char *copy = dup_block(m->messages_json, strlen(m->messages_json) + 1);
    free(m->messages_json);
    m->messages_json = copy;
    return copy != NULL;
}

static bool load_flash(const char *id, apps_pkg_t *pkg, char *err, size_t n) {
    if (!apps_id_valid(id) || !installed(id)) {
        put_err(err, n, "no such app on the gadget");
        return false;
    }
    if (quarantined(id)) {
        put_err(err, n, QUARANTINE_FMT, id);
        return false;
    }
    char why[160];
    if (!read_manifest(id, &pkg->manifest, &pkg->manifest_json, &pkg->manifest_len, why, sizeof why)) {
        put_err(err, n, "the stored copy of %s is damaged (manifest.json: %s); install it again", id, why);
        return false;
    }
    if (!rehome_messages(&pkg->manifest)) {
        put_err(err, n, "out of memory: no room to load the app");
        return false;
    }
    char dir[PATH_CAP], path[PATH_CAP];
    path_of(dir, id, NULL);
    const apps_manifest_t *m = &pkg->manifest;
    for (int i = 0; i < m->file_count; i++) {
        const char *rel = m->files[i];
        char *data;
        size_t len;
        if (!join(path, dir, rel)) {
            put_err(err, n, "the stored copy of %s is damaged (%s: its path is too long); install it again", id, rel);
            return false;
        }
        if (!read_file(path, apps_file_max(apps_file_kind(rel)), &data, &len, why, sizeof why)) {
            if (strcmp(why, "out of memory") == 0) {
                put_err(err, n, "out of memory: no room to load the app");
            } else {
                put_err(err, n, "the stored copy of %s is damaged (%s: %s); install it again", id, rel, why);
            }
            return false;
        }
        apps_blob_t *b = &pkg->files[pkg->count++];
        snprintf(b->path, sizeof b->path, "%s", rel);
        b->data = (uint8_t *)data;
        b->len = len;
        apps_port_sha256(b->data, b->len, b->sha256);
    }
    return true;
}

bool apps_store_load(const char *id, apps_pkg_t *pkg, char *err, size_t n) {
    if (pkg == NULL) {
        return false;
    }
    memset(pkg, 0, sizeof *pkg);
    put_err(err, n, "%s", "");
    switch (s_kind) {
    case APPS_STORAGE_RAM: {
        bool ok = apps_ram_load(id, pkg, err, n);
        cache_ram();  /* a load makes it the most recent */
        return ok;
    }
    case APPS_STORAGE_FLASH:
        if (load_flash(id, pkg, err, n)) {
            return true;
        }
        apps_pkg_free(pkg, apps_port_free);
        return false;
    default:
        put_err(err, n, "storage not ready");
        return false;
    }
}

bool apps_store_manifest(const char *id, apps_manifest_t *m) {
    memset(m, 0, sizeof *m);
    switch (s_kind) {
    case APPS_STORAGE_RAM:
        return apps_ram_manifest(id, m);
    case APPS_STORAGE_FLASH: {
        char why[96];
        if (!apps_id_valid(id) || !installed(id) || !read_manifest(id, m, NULL, NULL, why, sizeof why)) {
            return false;
        }
        if (!rehome_messages(m)) {
            apps_manifest_clear(m, apps_port_free);
            return false;
        }
        return true;
    }
    default:
        return false;
    }
}
