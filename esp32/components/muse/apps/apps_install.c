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

#include "apps_install.h"

#include <limits.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps_json.h"
#include "apps_port.h"
#include "apps_vm.h"
#include "sdkconfig.h"

/*
 * The install pipeline, the result and the status as JSON, and the RAM store (apps_install.h has the contract).
 *
 * Memory. An install's state (install_t: the messages, what the result lists of the files) and the package are blocks
 * of apps_port_realloc, PSRAM on the gadget, since the I/O worker's stack is 6 KB; the status record, several KB, is
 * one too (X12). Each has one owner: the install frees its state and the package on every way out, but for the package
 * once ops->run has taken it. The status runs on the console's task (3,584 bytes of stack): nothing here keeps more
 * than a few dozen bytes in a frame, and numbers are printed by this file's own digits, so that no printf runs below
 * cJSON's print.
 *
 * The trim. A result or a status is built whole, printed to measure it, and built again with less while it is over
 * APPS_RESULT_MAX: the log's oldest lines, then the dump's last entries, then the last files, the last warnings and
 * (a status) the end of last_error. Each round drops at least what its excess costs, as cJSON prints it, so it takes a
 * round or two, and at least one item a round, so it ends.
 */

#define MSG_MAX 4096     /* a result's message: a crash's error and traceback with the last lines of its log */
#define RAW_MAX 768      /* what the source, compile, commit and run say, and a status record's last_error */
#define CRASH_LINES 10   /* the log lines a crash's message shows */

#define MSG_BAD_ID "app id '%s' must match ^[a-z][a-z0-9_]{1,23}$"
#define MSG_NO_MEMORY "out of memory: no room for the install"

/* ---- Small things ---- */

static atomic_bool s_installing;

/* What cJSON prints for the byte c of a string: its escape, or itself. */
static size_t json_char_len(unsigned char c) {
    switch (c) {
    case '"':
    case '\\':
    case '\b':
    case '\f':
    case '\n':
    case '\r':
    case '\t':
        return 2;
    default:
        return c < 0x20 ? 6 : 1;
    }
}

/* What cJSON prints for s[0, n), without the quotes. */
static size_t json_len(const char *s, size_t n) {
    size_t len = 0;
    for (size_t i = 0; i < n; i++) {
        len += json_char_len((unsigned char)s[i]);
    }
    return len;
}

/* The bytes of s, valid UTF-8, whose escaped length is at most room: whole characters only. */
static size_t fit_len(const char *s, size_t room) {
    const unsigned char *p = (const unsigned char *)s, *end = p + strlen(s);
    size_t used = 0, at = 0;
    while (p + at < end) {
        size_t k = 1;
        if (p[at] >= 0x80) {
            int n = apps_json_utf8_len(p + at, end);
            k = n > 0 ? (size_t)n : 1;
        }
        size_t cost = 0;
        for (size_t i = 0; i < k; i++) {
            cost += json_char_len(p[at + i]);
        }
        if (used + cost > room) {
            break;
        }
        used += cost;
        at += k;
    }
    return at;
}

/* Every byte of s that isn't part of valid UTF-8 becomes '?': JSON is text. */
static void clean_utf8(char *s) {
    unsigned char *p = (unsigned char *)s, *end = p + strlen(s);
    while (p < end) {
        if (*p < 0x80) {
            p++;
            continue;
        }
        int n = apps_json_utf8_len(p, end);
        if (n > 0) {
            p += n;
        } else {
            *p++ = '?';
        }
    }
}

void apps_install_shown(const char *s, size_t n, char *out) {
    const unsigned char *p = (const unsigned char *)(s != NULL ? s : ""), *end = p + (s != NULL ? n : 0);
    size_t used = 0;
    while (p < end) {
        size_t take = 1, put = 1;
        bool keep = *p >= 0x20 && *p != 0x7F;
        if (*p >= 0x80) {
            int k = apps_json_utf8_len(p, end);
            keep = k > 0 && !(k == 2 && p[0] == 0xC2 && p[1] < 0xA0);  /* not broken, not C1 */
            take = k > 0 ? (size_t)k : 1;
            put = keep ? take : 1;
        }
        if (used + put > APPS_SHOWN_MAX) {
            memcpy(out + used, "...", 3);
            used += 3;
            break;
        }
        if (keep) {
            memcpy(out + used, p, take);
        } else {
            out[used] = '?';
        }
        used += put;
        p += take;
    }
    out[used] = '\0';
}

/* text into out (cap bytes with its NUL), cut on a character boundary. */
static void put_text(char *out, size_t cap, const char *text) {
    size_t n = strlen(text);
    if (n >= cap) {
        n = cap - 1;
        while (n > 0 && ((unsigned char)text[n] & 0xC0) == 0x80) {
            n--;
        }
    }
    memcpy(out, text, n);
    out[n] = '\0';
}

static void free_pkg(apps_pkg_t *pkg) {
    if (pkg != NULL) {
        apps_pkg_free(pkg, apps_port_free);
        apps_port_free(pkg);
    }
}

/* A block of apps_port_realloc holding data[0, n) (one byte for nothing). */
static void *dup_block(const void *data, size_t n) {
    void *copy = apps_port_realloc(NULL, n > 0 ? n : 1);
    if (copy != NULL && n > 0) {
        memcpy(copy, data, n);
    }
    return copy;
}

/* v as a raw JSON number: the integer it is, with no printf. False without memory. */
static bool add_uint(cJSON *obj, const char *key, uint64_t v) {
    char digits[24];
    char *p = digits + sizeof digits;
    *--p = '\0';
    do {
        *--p = (char)('0' + v % 10);
        v /= 10;
    } while (v > 0);
    return cJSON_AddRawToObject(obj, key, p) != NULL;
}

/* ---- Failures ---- */

/*
 * {"ok":false,"error":{"code":code,"message":message}}: the message as valid UTF-8, cut at its end on a character
 * boundary so that the whole prints in at most APPS_RESULT_MAX bytes. NULL without memory.
 */
static cJSON *failure(const char *code, const char *message) {
    char *text = dup_block(message, strlen(message) + 1);
    cJSON *r = text != NULL ? cJSON_CreateObject() : NULL;
    cJSON *e = NULL;
    bool ok = r != NULL && cJSON_AddFalseToObject(r, "ok") != NULL && (e = cJSON_AddObjectToObject(r, "error")) != NULL;
    if (ok) {
        clean_utf8(text);
        size_t skeleton = strlen("{\"ok\":false,\"error\":{\"code\":\"\",\"message\":\"\"}}") + json_len(code, strlen(code));
        text[fit_len(text, APPS_RESULT_MAX - skeleton)] = '\0';
        ok = cJSON_AddStringToObject(e, "code", code) != NULL && cJSON_AddStringToObject(e, "message", text) != NULL;
    }
    apps_port_free(text);
    if (!ok) {
        cJSON_Delete(r);
        return NULL;
    }
    return r;
}

/* failure() with a message made from fmt, in buf (cap bytes). */
static cJSON *failuref(char *buf, size_t cap, const char *code, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static cJSON *failuref(char *buf, size_t cap, const char *code, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, cap, fmt, ap);
    va_end(ap);
    return failure(code, buf);
}

/* ---- The JSON of a dump (apps_ui_dump's array, valid JSON) ---- */

/* From a[at], the first byte of an element of the array, the offset just past it. */
static size_t element_end(const char *a, size_t at) {
    int depth = 0;
    bool in_string = false;
    for (; a[at] != '\0'; at++) {
        char c = a[at];
        if (in_string) {
            if (c == '\\' && a[at + 1] != '\0') {
                at++;
            } else if (c == '"') {
                in_string = false;
                if (depth == 0) {
                    return at + 1;
                }
            }
        } else if (c == '"') {
            in_string = true;
        } else if (c == '{' || c == '[') {
            depth++;
        } else if (c == '}' || c == ']') {
            if (depth == 0) {
                return at;  /* the array's own end */
            }
            if (--depth == 0) {
                return at + 1;
            }
        } else if (c == ',' && depth == 0) {
            return at;
        }
    }
    return at;
}

static size_t skip_space(const char *a, size_t at) {
    while (a[at] == ' ' || a[at] == '\n' || a[at] == '\r' || a[at] == '\t') {
        at++;
    }
    return at;
}

/* The offset just past the dump's first `want` entries (just past its '[' for none), and in *count (may be NULL) the
 * entries it has, when it has fewer than want. */
static size_t dump_prefix(const char *dump, int want, int *count) {
    size_t at = 1, cut = 1;
    int n = 0;
    at = skip_space(dump, at);
    while (dump[at] != '\0' && dump[at] != ']' && n < want) {
        at = element_end(dump, at);
        cut = at;
        n++;
        at = skip_space(dump, at);
        if (dump[at] != ',') {
            break;
        }
        at = skip_space(dump, at + 1);
    }
    if (count != NULL) {
        *count = n;
    }
    return cut;
}

/* The most entries, fewer than below, whose prefix (dump_prefix) ends at target or before: 0 when none do. */
static int dump_fit(const char *dump, int below, size_t target) {
    size_t at = skip_space(dump, 1);
    int n = 0;
    while (n + 1 < below && dump[at] != '\0' && dump[at] != ']') {
        size_t end = element_end(dump, at);
        if (end > target) {
            break;
        }
        n++;
        at = skip_space(dump, end);
        if (dump[at] != ',') {
            break;
        }
        at = skip_space(dump, at + 1);
    }
    return n;
}

/* A dump as the record has it, or "[]" for none or anything that isn't an array. */
static const char *dump_or_empty(const char *dump) {
    size_t n = dump != NULL ? strlen(dump) : 0;
    return n >= 2 && dump[0] == '[' && dump[n - 1] == ']' ? dump : "[]";
}

/* ---- Results and the status: built, measured and trimmed ---- */

typedef struct {
    char path[APPS_PATH_MAX + 1];
    size_t bytes;
    uint8_t sha256[32];
} file_info_t;

/* What a result's payload or a status shows, and what the trim has left of it. */
typedef struct {
    bool status;                 /* the console's status, else a result */
    const apps_status_t *st;     /* the record (a result without run: NULL) */
    /* a result's */
    const char *id, *version, *storage;
    const file_info_t *files;
    int n_files;
    bool unchanged;
    /* both */
    const char *warnings;        /* "; "-joined */
    const char *log;             /* "\n"-joined, oldest first */
    const char *dump;            /* a JSON array */
    /* what is shown: the lines from log_from, the first ui_kept entries, files and warnings, error_len bytes */
    int log_from, ui_kept, files_kept, warnings_kept;
    size_t error_len;
    int n_lines, n_entries, n_warnings;
} view_t;

/* Pieces of text split at sep: how many. */
static int count_pieces(const char *text, const char *sep) {
    if (text[0] == '\0') {
        return 0;
    }
    int n = 1;
    for (const char *p = strstr(text, sep); p != NULL; p = strstr(p + strlen(sep), sep)) {
        n++;
    }
    return n;
}

/* Piece i of text split at sep: where it starts, and its length. */
static const char *piece(const char *text, const char *sep, int i, size_t *len) {
    const char *p = text;
    for (; i > 0; i--) {
        const char *next = strstr(p, sep);
        if (next == NULL) {
            *len = 0;
            return p + strlen(p);
        }
        p = next + strlen(sep);
    }
    const char *next = strstr(p, sep);
    *len = next != NULL ? (size_t)(next - p) : strlen(p);
    return p;
}

/* The array of pieces [from, upto) of text split at sep, under key. False without memory. */
static bool add_pieces(cJSON *obj, const char *key, const char *text, const char *sep, int from, int upto) {
    cJSON *array = cJSON_AddArrayToObject(obj, key);
    if (array == NULL) {
        return false;
    }
    if (from >= upto) {
        return true;
    }
    char *copy = dup_block(text, strlen(text) + 1);
    if (copy == NULL) {
        return false;
    }
    size_t sep_len = strlen(sep);
    bool ok = true;
    char *p = copy;
    for (int i = 0; ok && i < upto; i++) {
        char *next = strstr(p, sep);
        if (next != NULL) {
            *next = '\0';
        }
        if (i >= from) {
            cJSON *item = cJSON_CreateString(p);
            ok = item != NULL && cJSON_AddItemToArray(array, item);
            if (!ok) {
                cJSON_Delete(item);
            }
        }
        if (next == NULL) {
            break;
        }
        p = next + sep_len;
    }
    apps_port_free(copy);
    return ok;
}

/* "ui": the dump's first ui_kept entries, raw, and "ui_truncated" when any aren't there. */
static bool add_ui(cJSON *obj, const view_t *v) {
    cJSON *ui = cJSON_CreateRaw(v->dump);
    if (ui == NULL || !cJSON_AddItemToObject(obj, "ui", ui)) {
        cJSON_Delete(ui);
        return false;
    }
    if (v->ui_kept < v->n_entries) {
        size_t cut = dump_prefix(v->dump, v->ui_kept, NULL);
        ui->valuestring[cut] = ']';  /* the copy is the item's own, and longer than this */
        ui->valuestring[cut + 1] = '\0';
    }
    int dropped = (v->st != NULL ? v->st->ui_truncated : 0) + (v->n_entries - v->ui_kept);
    return dropped <= 0 || add_uint(obj, "ui_truncated", (uint64_t)dropped);
}

static bool add_file(cJSON *files, const file_info_t *f) {
    static const char HEX[] = "0123456789abcdef";
    char hex[65];
    for (int i = 0; i < 32; i++) {
        hex[2 * i] = HEX[f->sha256[i] >> 4];
        hex[2 * i + 1] = HEX[f->sha256[i] & 15];
    }
    hex[64] = '\0';
    cJSON *o = cJSON_CreateObject();
    bool ok = o != NULL && cJSON_AddItemToArray(files, o);
    if (!ok) {
        cJSON_Delete(o);
        return false;
    }
    return cJSON_AddStringToObject(o, "path", f->path) != NULL && add_uint(o, "bytes", f->bytes) &&
           cJSON_AddStringToObject(o, "sha256", hex) != NULL;
}

static bool add_payload(cJSON *p, const view_t *v) {
    const apps_status_t *st = v->st;
    bool ok = cJSON_AddStringToObject(p, "id", v->id) != NULL && cJSON_AddStringToObject(p, "version", v->version) != NULL &&
              cJSON_AddStringToObject(p, "storage", v->storage) != NULL;
    cJSON *files = ok ? cJSON_AddArrayToObject(p, "files") : NULL;
    ok = files != NULL;
    for (int i = 0; ok && i < v->files_kept; i++) {
        ok = add_file(files, &v->files[i]);
    }
    if (ok && v->files_kept < v->n_files) {
        ok = add_uint(p, "files_truncated", (uint64_t)(v->n_files - v->files_kept));
    }
    ok = ok && cJSON_AddStringToObject(p, "state", st != NULL ? apps_state_name(st->state) : "installed") != NULL &&
         add_pieces(p, "warnings", v->warnings, "; ", 0, v->warnings_kept) &&
         add_pieces(p, "log", v->log, "\n", v->log_from, v->n_lines) && add_ui(p, v) &&
         add_uint(p, "mem_kb", st != NULL ? st->mem_kb : 0) && add_uint(p, "peak_kb", st != NULL ? st->peak_kb : 0);
    if (ok && v->unchanged) {
        ok = cJSON_AddTrueToObject(p, "unchanged") != NULL;
    }
    return ok;
}

static bool add_status(cJSON *s, const view_t *v) {
    const apps_status_t *st = v->st;
    bool ok = cJSON_AddStringToObject(s, "state", apps_state_name(st->state)) != NULL &&
              cJSON_AddStringToObject(s, "id", st->id) != NULL && cJSON_AddStringToObject(s, "name", st->name) != NULL &&
              cJSON_AddStringToObject(s, "version", st->version) != NULL &&
              cJSON_AddBoolToObject(s, "front", st->front) != NULL;
    if (ok) {
        char *error = dup_block(st->last_error, v->error_len + 1);
        ok = error != NULL;
        if (ok) {
            error[v->error_len] = '\0';
            ok = cJSON_AddStringToObject(s, "last_error", error) != NULL;
        }
        apps_port_free(error);
    }
    return ok && add_pieces(s, "warnings", v->warnings, "; ", 0, v->warnings_kept) &&
           add_pieces(s, "log", v->log, "\n", v->log_from, v->n_lines) && add_ui(s, v) &&
           add_uint(s, "instructions", st->instructions) && add_uint(s, "mem_kb", st->mem_kb) &&
           add_uint(s, "peak_kb", st->peak_kb) && add_uint(s, "limit_kb", st->limit_kb) &&
           add_uint(s, "worst_dispatch_ms", st->worst_dispatch_ms) && add_uint(s, "cpu_pct", st->cpu_pct) &&
           add_uint(s, "dropped_events", st->dropped_events) && add_uint(s, "stack_free", st->stack_free) &&
           add_uint(s, "widgets", st->widgets > 0 ? (uint64_t)st->widgets : 0);
}

/* The result ({"ok":true,"payload":{...}}) or the status of v, as it stands. NULL without memory. */
static cJSON *build(const view_t *v) {
    cJSON *root = cJSON_CreateObject();
    bool ok = root != NULL;
    if (ok && v->status) {
        ok = add_status(root, v);
    } else if (ok) {
        cJSON *payload = cJSON_CreateObject();
        ok = cJSON_AddTrueToObject(root, "ok") != NULL && payload != NULL && cJSON_AddItemToObject(root, "payload", payload);
        if (!ok) {
            cJSON_Delete(payload);
        }
        ok = ok && add_payload(payload, v);
    }
    if (!ok) {
        cJSON_Delete(root);
        return NULL;
    }
    return root;
}

/* How many bytes item prints in; 0 without memory. */
static size_t printed_len(const cJSON *item) {
    char *text = cJSON_PrintUnformatted(item);
    size_t n = text != NULL ? strlen(text) : 0;
    cJSON_free(text);
    return n;
}

/* Digits of n. */
static size_t digits(int n) {
    size_t d = 1;
    for (; n >= 10; n /= 10) {
        d++;
    }
    return d;
}

/* Shows less of v, at least excess bytes of it as printed, if there is anything left to drop. */
static bool shrink(view_t *v, size_t excess) {
    size_t gone = 0;
    if (v->log_from < v->n_lines) {
        size_t len;
        const char *line = piece(v->log, "\n", v->log_from, &len);
        while (v->log_from < v->n_lines && gone < excess) {
            gone += json_len(line, len) + 3;  /* its quotes and its comma */
            if (++v->log_from < v->n_lines) {
                line += len + 1;  /* past its '\n' */
                const char *end = strchr(line, '\n');
                len = end != NULL ? (size_t)(end - line) : strlen(line);
            }
        }
        return true;
    }
    if (v->ui_kept > 0) {
        size_t now = dump_prefix(v->dump, v->ui_kept, NULL);
        int dropped = (v->st != NULL ? v->st->ui_truncated : 0) + (v->n_entries - v->ui_kept);
        size_t more = dropped > 0 ? 1 : strlen(",\"ui_truncated\":") + digits(v->n_entries);  /* what saying so costs */
        size_t target = now > excess + more ? now - excess - more : 0;
        v->ui_kept = dump_fit(v->dump, v->ui_kept, target);
        return true;
    }
    if (v->files_kept > 0) {
        while (v->files_kept > 0 && gone < excess) {
            const file_info_t *f = &v->files[--v->files_kept];
            gone += strlen("{\"path\":\"\",\"bytes\":,\"sha256\":\"\"},") + json_len(f->path, strlen(f->path)) +
                    digits((int)f->bytes) + 64;
        }
        return true;
    }
    if (v->warnings_kept > 0) {
        while (v->warnings_kept > 0 && gone < excess) {
            size_t len;
            const char *w = piece(v->warnings, "; ", --v->warnings_kept, &len);
            gone += json_len(w, len) + 3;
        }
        return true;
    }
    if (v->status && v->error_len > 0) {
        size_t now = json_len(v->st->last_error, v->error_len);
        size_t room = now > excess ? now - excess : 0;
        size_t keep = fit_len(v->st->last_error, room);
        v->error_len = keep < v->error_len ? keep : v->error_len - 1;
        while (v->error_len > 0 && ((unsigned char)v->st->last_error[v->error_len] & 0xC0) == 0x80) {
            v->error_len--;  /* never inside a character */
        }
        return true;
    }
    return false;
}

/* v built and trimmed until it prints in APPS_RESULT_MAX bytes. NULL without memory (or, which can't be, when nothing
 * is left to drop). */
static cJSON *fit(view_t *v) {
    v->dump = dump_or_empty(v->dump);
    v->n_lines = count_pieces(v->log, "\n");
    v->n_warnings = count_pieces(v->warnings, "; ");
    (void)dump_prefix(v->dump, INT_MAX, &v->n_entries);
    v->log_from = 0;
    v->ui_kept = v->n_entries;
    v->files_kept = v->n_files;
    v->warnings_kept = v->n_warnings;
    v->error_len = v->status ? strlen(v->st->last_error) : 0;
    for (;;) {
        cJSON *doc = build(v);
        size_t len = doc != NULL ? printed_len(doc) : 0;
        if (len == 0) {
            cJSON_Delete(doc);
            return NULL;
        }
        if (len <= APPS_RESULT_MAX) {
            return doc;
        }
        cJSON_Delete(doc);
        if (!shrink(v, len - APPS_RESULT_MAX)) {
            return NULL;
        }
    }
}

cJSON *apps_install_status(const apps_status_t *st) {
    if (st == NULL) {
        return NULL;
    }
    view_t v = {.status = true, .st = st, .warnings = st->warnings, .log = st->log_tail, .dump = st->ui_dump};
    return fit(&v);
}

/* ---- The pipeline ---- */

/* An install's own state (or a launch's): on the heap, as the I/O worker's stack is small. */
typedef struct {
    apps_pkg_t *pkg;                   /* the install's until ops->run takes it */
    char id[APPS_ID_MAX + 1], version[24], main[APPS_PATH_MAX + 1];
    char warnings[sizeof ((apps_manifest_t *)0)->warnings];
    int n_files;
    file_info_t files[APPS_MAX_FILES];
    char raw[RAW_MAX];                 /* what the source, compile, commit or run said */
    char msg[MSG_MAX];                 /* the result's message */
} install_t;

static size_t kind_max(apps_file_kind_t kind) {
    switch (kind) {
    case APPS_FILE_LUA:
        return APPS_LUA_MAX;
    case APPS_FILE_JSON:
        return APPS_JSON_FILE_MAX;
    default:
        return APPS_IMAGE_MAX;
    }
}

/* Where the package's blocks come from: apps_port_realloc's, every one, so that apps_task_run can free them. The
 * manifest's messages_json comes from apps_pkg.c's own allocation, malloc on the host: it moves into one. */
static bool rehome_messages(apps_manifest_t *m) {
    if (m->messages_json == NULL) {
        return true;
    }
    char *copy = dup_block(m->messages_json, strlen(m->messages_json) + 1);
    free(m->messages_json);
    m->messages_json = copy;
    return copy != NULL;
}

/* What the result lists of the package, kept before ops->run takes it. */
static void keep_infos(install_t *in, const apps_pkg_t *pkg) {
    put_text(in->id, sizeof in->id, pkg->manifest.id);
    put_text(in->version, sizeof in->version, pkg->manifest.version);
    put_text(in->main, sizeof in->main, pkg->manifest.main);
    put_text(in->warnings, sizeof in->warnings, pkg->manifest.warnings);
    in->n_files = 0;
    for (int i = 0; i < pkg->count && i < APPS_MAX_FILES; i++) {
        file_info_t *f = &in->files[in->n_files++];
        put_text(f->path, sizeof f->path, pkg->files[i].path);
        f->bytes = pkg->files[i].len;
        memcpy(f->sha256, pkg->files[i].sha256, sizeof f->sha256);
    }
}

/* Step 4's message: "compile failed: " and Lua's message with the excerpt of the file it names, if it names one. */
static void compile_message(install_t *in, const apps_pkg_t *pkg) {
    size_t at = (size_t)snprintf(in->msg, sizeof in->msg, "compile failed: ");
    const char *src = NULL;
    size_t len = 0;
    /* "<file>:<line>: ...": the file is what comes before the first ":<digits>:". */
    for (const char *colon = strchr(in->raw, ':'); colon != NULL && src == NULL; colon = strchr(colon + 1, ':')) {
        const char *d = colon + 1;
        while (*d >= '0' && *d <= '9') {
            d++;
        }
        if (d == colon + 1 || *d != ':') {
            continue;
        }
        size_t name = (size_t)(colon - in->raw);
        for (int i = 0; i < pkg->count; i++) {
            if (strlen(pkg->files[i].path) == name && memcmp(pkg->files[i].path, in->raw, name) == 0) {
                src = (const char *)pkg->files[i].data;
                len = pkg->files[i].len;
            }
        }
        break;
    }
    apps_vm_excerpt(in->raw, src, len, in->msg + at, sizeof in->msg - at);  /* raw is its own buffer: never in place */
}

/*
 * A run that failed: a crash ("<prefix>crashed in start(): <error>\n<traceback>\nlog:\n<the last 10 lines>", or
 * crashed while loading <main>, or crashed in the settle), or anything else ("<prefix>it didn't run: <why>"; a launch,
 * with no prefix, says why alone).
 */
static cJSON *run_failure(install_t *in, const apps_status_t *st, const char *prefix) {
    if (st->state != APPS_STATE_CRASHED) {
        snprintf(in->msg, sizeof in->msg, "%s%s%s", prefix, prefix[0] != '\0' ? "it didn't run: " : "", in->raw);
        return failure(APPS_ERR_NOT_RUNNING, in->msg);
    }
    char phrase[APPS_PATH_MAX + 40];
    if (strncmp(in->raw, "crashed in start()", 18) == 0) {
        snprintf(phrase, sizeof phrase, "crashed in start()");
    } else if (strncmp(in->raw, "crashed loading ", 16) == 0) {
        snprintf(phrase, sizeof phrase, "crashed while loading %s", in->main);
    } else {
        snprintf(phrase, sizeof phrase, "crashed");
    }
    const char *error = st->last_error;
    if (error[0] == '\0') {
        const char *after = strstr(in->raw, ": ");
        error = after != NULL ? after + 2 : in->raw;
    }
    /* The last CRASH_LINES lines of the log. */
    const char *log = st->log_tail, *from = log + strlen(log);
    for (int seen = 0; from > log; from--) {
        if (from[-1] == '\n' && ++seen == CRASH_LINES) {
            break;
        }
    }
    snprintf(in->msg, sizeof in->msg, "%s%s: %s\nlog:\n%s", prefix, phrase, error, from);
    return failure(APPS_ERR_CRASHED, in->msg);
}

/* The success: the package's files, and the run's record (st) or, without run, the manifest's warnings. */
static cJSON *success(const install_t *in, const apps_install_ops_t *ops, const apps_status_t *st, bool unchanged) {
    view_t v = {
        .st = st,
        .id = in->id,
        .version = in->version,
        .storage = ops->storage != NULL ? ops->storage() : "ram",
        .files = in->files,
        .n_files = in->n_files,
        .unchanged = unchanged,
        .warnings = st != NULL ? st->warnings : in->warnings,
        .log = st != NULL ? st->log_tail : "",
        .dump = st != NULL ? st->ui_dump : "[]",
    };
    cJSON *r = fit(&v);
    return r != NULL ? r : failure(APPS_ERR_OUT_OF_MEMORY, "out of memory: no room for the result");
}

/* Step 6 and the result: ops->run takes in->pkg. */
static cJSON *run_pkg(install_t *in, const apps_install_ops_t *ops, const char *prefix) {
    apps_status_t *st = apps_port_realloc(NULL, sizeof *st);
    if (st == NULL) {
        free_pkg(in->pkg);
        in->pkg = NULL;
        snprintf(in->msg, sizeof in->msg, "%s%sout of memory: no room for the app's status", prefix,
                 prefix[0] != '\0' ? "it didn't run: " : "");
        return failure(APPS_ERR_OUT_OF_MEMORY, in->msg);
    }
    memset(st, 0, sizeof *st);
    apps_pkg_t *pkg = in->pkg;
    in->pkg = NULL;
    in->raw[0] = '\0';
    bool ran = ops->run(pkg, APPS_SETTLE_MS, st, in->raw, sizeof in->raw);
    cJSON *r = ran ? success(in, ops, st, false) : run_failure(in, st, prefix);
    apps_port_free(st);
    return r;
}

/* Steps 2 to 7, for apps_install_run. */
static cJSON *install(install_t *in, const char *id, bool run, const apps_source_t *src, const apps_install_ops_t *ops) {
    const int64_t deadline_us = (apps_port_mono_ms() + APPS_INSTALL_DEADLINE_MS) * 1000;
    apps_pkg_t *pkg = in->pkg;
    uint8_t *data = NULL;
    size_t len = 0;
    if (!src->read(src->ctx, "manifest.json", APPS_MANIFEST_MAX, deadline_us, &data, &len, in->raw, sizeof in->raw)) {
        return failure(APPS_ERR_READ_FAILED, in->raw);
    }
    if (len > APPS_MANIFEST_MAX) {
        apps_port_free(data);
        return failuref(in->msg, sizeof in->msg, APPS_ERR_INVALID_PACKAGE, "manifest.json is larger than %u KB",
                        (unsigned)(APPS_MANIFEST_MAX / 1024));
    }
    bool ok = apps_manifest_parse((const char *)data, len, id, CONFIG_MUSE_APPS_HEAP_MAX_KB, &pkg->manifest, in->raw,
                                  sizeof in->raw);
    apps_port_free(data);
    if (!ok) {
        return failure(APPS_ERR_INVALID_PACKAGE, in->raw);
    }
    if (!rehome_messages(&pkg->manifest)) {
        return failure(APPS_ERR_OUT_OF_MEMORY, MSG_NO_MEMORY);
    }
    const apps_manifest_t *m = &pkg->manifest;
    size_t app_bytes = len, image_bytes = 0;
    for (int i = 0; i < m->file_count; i++) {
        const char *rel = m->files[i];
        if (!apps_path_valid(rel)) {  /* the manifest's parse checked it: never read a path that isn't one */
            return failuref(in->msg, sizeof in->msg, APPS_ERR_INVALID_PACKAGE, "files[%d] is not an allowed path", i);
        }
        apps_file_kind_t kind = apps_file_kind(rel);
        size_t max = kind_max(kind);
        if (!src->read(src->ctx, rel, max, deadline_us, &data, &len, in->raw, sizeof in->raw)) {
            return failure(APPS_ERR_READ_FAILED, in->raw);
        }
        apps_blob_t *b = &pkg->files[pkg->count++];  /* the package's from here: apps_pkg_free frees it */
        put_text(b->path, sizeof b->path, rel);
        b->data = data;
        b->len = len;
        if (len > max) {
            return failuref(in->msg, sizeof in->msg, APPS_ERR_INVALID_PACKAGE, "%s is larger than %u KB", rel,
                            (unsigned)(max / 1024));
        }
        app_bytes += len;
        if (app_bytes > APPS_APP_MAX) {
            return failure(APPS_ERR_INVALID_PACKAGE, "the app is larger than 2 MB");
        }
        if (kind == APPS_FILE_LUA && len >= 4 && memcmp(data, "\x1bLua", 4) == 0) {
            return failuref(in->msg, sizeof in->msg, APPS_ERR_INVALID_PACKAGE,
                            "files[%d] %s is a compiled chunk: install Lua source text", i, rel);
        }
        if (kind == APPS_FILE_JSON && !apps_json_validate((const char *)data, len, in->raw, sizeof in->raw)) {
            return failuref(in->msg, sizeof in->msg, APPS_ERR_INVALID_PACKAGE, "%s: %s", rel, in->raw);
        }
        if (kind == APPS_FILE_IMAGE) {
            image_bytes += len;
            if (image_bytes > APPS_IMAGES_TOTAL_MAX) {
                return failure(APPS_ERR_INVALID_PACKAGE, "images add up to more than 1.5 MB");
            }
            if (!apps_image_check(data, len, NULL, NULL, in->raw, sizeof in->raw)) {
                return failuref(in->msg, sizeof in->msg, APPS_ERR_INVALID_PACKAGE, "%s: %s", rel, in->raw);
            }
        }
        apps_port_sha256(data, len, b->sha256);
    }
    in->raw[0] = '\0';
    if (!ops->compile(pkg, in->raw, sizeof in->raw)) {
        compile_message(in, pkg);
        return failure(APPS_ERR_COMPILE_FAILED, in->msg);
    }
    bool unchanged = false;
    in->raw[0] = '\0';
    if (!ops->commit(pkg, &unchanged, in->raw, sizeof in->raw)) {
        return failure(APPS_ERR_COMMIT_FAILED, in->raw);
    }
    keep_infos(in, pkg);
    return run ? run_pkg(in, ops, "installed but ") : success(in, ops, NULL, unchanged);
}

cJSON *apps_install_run(const char *id, bool run, const apps_source_t *src, const apps_install_ops_t *ops) {
    id = id != NULL ? id : "";
    if (!apps_id_valid(id)) {
        char shown[APPS_SHOWN_MAX + 4], msg[sizeof MSG_BAD_ID + APPS_SHOWN_MAX + 4];
        apps_install_shown(id, strlen(id), shown);
        return failuref(msg, sizeof msg, APPS_ERR_INVALID_PARAMS, MSG_BAD_ID, shown);
    }
    install_t *in = apps_port_realloc(NULL, sizeof *in);
    apps_pkg_t *pkg = in != NULL ? apps_port_realloc(NULL, sizeof *pkg) : NULL;
    if (pkg == NULL) {
        apps_port_free(in);
        return failure(APPS_ERR_OUT_OF_MEMORY, MSG_NO_MEMORY);
    }
    memset(in, 0, sizeof *in);
    memset(pkg, 0, sizeof *pkg);
    in->pkg = pkg;
    cJSON *r = install(in, id, run, src, ops);
    free_pkg(in->pkg);  /* NULL once ops->run has taken it */
    apps_port_free(in);
    return r;
}

cJSON *apps_install_launch(apps_pkg_t *pkg, const apps_install_ops_t *ops) {
    install_t *in = pkg != NULL ? apps_port_realloc(NULL, sizeof *in) : NULL;
    if (in == NULL) {
        free_pkg(pkg);
        return failure(APPS_ERR_OUT_OF_MEMORY, "out of memory: no room for the launch");
    }
    memset(in, 0, sizeof *in);
    keep_infos(in, pkg);
    in->pkg = pkg;
    cJSON *r = run_pkg(in, ops, "");
    apps_port_free(in);
    return r;
}

bool apps_install_try_begin(void) {
    return !atomic_exchange(&s_installing, true);
}

void apps_install_end(void) {
    atomic_store(&s_installing, false);
}

/* ---- The RAM store: the I/O worker's alone ---- */

static apps_pkg_t *s_ram[APPS_RAM_APPS];  /* in the order first committed */
static int s_ram_count;

static int ram_find(const char *id) {
    for (int i = 0; id != NULL && i < s_ram_count; i++) {
        if (strcmp(s_ram[i]->manifest.id, id) == 0) {
            return i;
        }
    }
    return -1;
}

/* A deep copy of src into out (overwritten), every block from apps_port_realloc; out is zeroed when one is refused. */
static bool pkg_copy(const apps_pkg_t *src, apps_pkg_t *out) {
    memcpy(out, src, sizeof *out);
    out->manifest.messages_json = NULL;
    for (int i = 0; i < APPS_MAX_FILES; i++) {
        out->files[i].data = NULL;
    }
    bool ok = true;
    if (src->manifest.messages_json != NULL) {
        out->manifest.messages_json = dup_block(src->manifest.messages_json, strlen(src->manifest.messages_json) + 1);
        ok = out->manifest.messages_json != NULL;
    }
    for (int i = 0; ok && i < src->count && i < APPS_MAX_FILES; i++) {
        out->files[i].data = dup_block(src->files[i].data, src->files[i].len);
        ok = out->files[i].data != NULL;
    }
    if (!ok) {
        apps_pkg_free(out, apps_port_free);
    }
    return ok;
}

bool apps_ram_put(apps_pkg_t *pkg) {
    if (pkg == NULL) {
        return false;
    }
    int i = ram_find(pkg->manifest.id);
    if (i >= 0) {
        free_pkg(s_ram[i]);
        s_ram[i] = pkg;
        return true;
    }
    if (s_ram_count == APPS_RAM_APPS) {
        free_pkg(pkg);
        return false;
    }
    s_ram[s_ram_count++] = pkg;
    return true;
}

bool apps_ram_load(const char *id, apps_pkg_t *out) {
    if (out == NULL) {
        return false;
    }
    int i = ram_find(id);
    if (i < 0) {
        memset(out, 0, sizeof *out);
        return false;
    }
    return pkg_copy(s_ram[i], out);
}

bool apps_ram_has(const char *id) {
    return ram_find(id) >= 0;
}

bool apps_ram_remove(const char *id) {
    int i = ram_find(id);
    if (i < 0) {
        return false;
    }
    free_pkg(s_ram[i]);
    memmove(&s_ram[i], &s_ram[i + 1], (size_t)(s_ram_count - i - 1) * sizeof s_ram[0]);
    s_ram[--s_ram_count] = NULL;
    return true;
}

int apps_ram_list(apps_ram_entry_t *out, int max) {
    int n = 0;
    for (; out != NULL && n < s_ram_count && n < max; n++) {
        const apps_manifest_t *m = &s_ram[n]->manifest;
        put_text(out[n].id, sizeof out[n].id, m->id);
        put_text(out[n].name, sizeof out[n].name, m->name);
        put_text(out[n].version, sizeof out[n].version, m->version);
    }
    return n;
}

bool apps_ram_commit(const apps_pkg_t *pkg, bool *unchanged, char *err, size_t err_len) {
    *unchanged = false;
    if (ram_find(pkg->manifest.id) < 0 && s_ram_count == APPS_RAM_APPS) {
        snprintf(err, err_len, "the gadget keeps at most %d apps in RAM; restart it to make room", APPS_RAM_APPS);
        return false;
    }
    apps_pkg_t *copy = apps_port_realloc(NULL, sizeof *copy);
    if (copy == NULL || !pkg_copy(pkg, copy)) {
        apps_port_free(copy);
        snprintf(err, err_len, "out of memory: no room to keep the app in RAM");
        return false;
    }
    (void)apps_ram_put(copy);  /* there is room: its id's place, or a free one */
    snprintf(err, err_len, "%s", "");
    return true;
}
