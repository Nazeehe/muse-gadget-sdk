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
 * The package rules for Muse apps (apps_pkg.h).
 *
 * The manifest is read in two passes with the pull reader, and no tree is built:
 * 1. The whole text, strictly (the reader checks what it skips as the decoder does, so any syntax error is
 *    found here, before a value is judged). Each key the manifest reads is kept as the place its value
 *    has in the text, the last one when a key comes twice; any other key is a warning.
 * 2. Each kept value in turn, in the order apps_pkg.h gives, by a reader of its own over just that value.
 * Why two: "the last value counts" (json.decode) can't be told until the text has been read to its end,
 * and a syntax error anywhere should win over a wrong value early on.
 *
 * A reader error is final, so a value this file may refuse with a message of its own (an over-long path,
 * the wrong type, a \u0000) is never read by a call that could put the reader that is walking the text
 * in error. The first pass only skips it (apps_jr_raw_value), and the second decodes it from its place
 * with a small reader of its own, whose failure just means "this isn't what is wanted".
 *
 * Stack: nothing here recurses. The biggest frame is 576 bytes on the device (check_files: a reader and a string
 * read into TEXT_MAX), the checks run one after another, and the deepest chain, a check or the scan over the
 * reader's skip of a value nested 14 or 15 levels (48 bytes a level), is under 1.6 KB.
 */

#include "apps_pkg.h"

#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef ESP_PLATFORM
#include "esp_heap_caps.h"
#endif

#include "apps_json.h"
#include "apps_suggest.h"
#include "apps_vm.h"

/* The functions that hold a buffer or a reader stay out of one another's frames: they run one after the
 * other, so the deepest the stack goes is the biggest of them, not the sum. */
#define NOINLINE __attribute__((noinline))

/* A string read from the manifest goes into this many bytes (B14), so that a path past APPS_PATH_MAX
 * is still read, and refused by name. One longer than TEXT_MAX - 1 isn't, and is quoted from its JSON. */
#define TEXT_MAX 256

/* A value that is the wrong type, or can't be read, is quoted to this many bytes (R7.2). */
#define ECHO_MAX 32

#define DEFAULT_MEMORY_KB 256
#define MIN_MEMORY_KB 64
#define MAX_MEMORY_KB 1024

#define MESSAGES_MAX 16
#define MESSAGE_TEXT_MAX 200  /* each name and description is under this: read into this many bytes, NUL included */

#define PATH_RULE "(a-z 0-9 _ . / -, no '..', at most 64 characters)"
#define PERMISSIONS_VALID "(valid: sound, alarm, store, keep_awake)"

/* LVGL v9's image header: magic, colour format, flags (16 bits), w, h, stride and 16 reserved bits, all
 * little-endian, in 12 bytes. */
#define IMAGE_HEADER 12
#define IMAGE_MAGIC 0x19
#define IMAGE_FLAG_PREMULTIPLIED 0x0001
#define IMAGE_FLAG_COMPRESSED 0x0008
#define IMAGE_CF_A8 0x0E
#define IMAGE_CF_RGB565 0x12
#define IMAGE_CF_RGB565A8 0x14
#define IMAGE_SCREEN 466

/* ==== Text ==== */

/* s[0, n) is the start of valid UTF-8 text that goes on past n: how much of it to keep so as not to end in the middle
 * of a character, which is n, or n less the bytes of the character that n cuts. Everything cut here is text the
 * JSON reader has already checked, or this file's own. */
static size_t utf8_whole(const char *s, size_t n) {
    for (size_t back = 1; back <= 3 && back <= n; back++) {
        unsigned char c = (unsigned char)s[n - back];
        if ((c & 0xC0) != 0x80) {  /* a lead byte, or ASCII: does its character end by n? */
            size_t length = c >= 0xF0 ? 4 : c >= 0xE0 ? 3 : c >= 0xC0 ? 2 : 1;
            return length > back ? n - back : n;
        }
    }
    return n;
}

/* src into dst (cap bytes with the NUL), cut if it must be. */
static void copy_text(char *dst, size_t cap, const char *src) {
    size_t n = strlen(src);
    if (n >= cap) {
        n = cap - 1;
    }
    memcpy(dst, src, n);
    dst[n] = '\0';
}

/* The message, in err (err_len bytes with the NUL) cut on a character boundary, and false. */
static bool failf(char *err, size_t err_len, const char *fmt, ...) __attribute__((format(printf, 3, 4)));

static bool failf(char *err, size_t err_len, const char *fmt, ...) {
    if (err == NULL || err_len == 0) {
        return false;
    }
    va_list args;
    va_start(args, fmt);
    int n = vsnprintf(err, err_len, fmt, args);
    va_end(args);
    if (n >= 0 && (size_t)n >= err_len) {  /* cut: err_len - 1 bytes are there */
        err[utf8_whole(err, err_len - 1)] = '\0';
    }
    return false;
}

/* What a message quotes of a value that isn't what it should be: s[0, n) cut to ECHO_MAX bytes on a
 * character boundary, with "..." after a cut. */
typedef struct {
    char text[ECHO_MAX + 4];
} echo_t;

static const char *echo(echo_t *e, const char *s, size_t n) {
    bool cut = n > ECHO_MAX;
    if (cut) {
        n = utf8_whole(s, ECHO_MAX);
    }
    memcpy(e->text, s, n);
    if (cut) {
        memcpy(e->text + n, "...", 3);
        n += 3;
    }
    e->text[n] = '\0';
    return e->text;
}

/* ==== Ids, paths, kinds ==== */

bool apps_id_valid(const char *id) {
    if (id == NULL) {
        return false;
    }
    size_t n = 0;
    for (; id[n] != '\0'; n++) {
        char c = id[n];
        bool lower = c >= 'a' && c <= 'z';
        if (n >= APPS_ID_MAX || !(lower || (n > 0 && ((c >= '0' && c <= '9') || c == '_')))) {
            return false;
        }
    }
    return n >= 2;
}

bool apps_path_valid(const char *path) {
    if (path == NULL) {
        return false;
    }
    size_t n = 0;
    char prev = '/';  /* the start of a path is the start of a segment */
    for (; path[n] != '\0'; n++) {
        char c = path[n];
        if (n >= APPS_PATH_MAX) {
            return false;
        }
        if (c == '/' || c == '.') {
            /* A '/' or '.' straight after a '/' (or at the start) is an empty segment or one that starts with '.'.
             * Straight after a '.', it is a segment that ends in one (FAT drops it) or a "..". */
            if (prev == '/' || prev == '.') {
                return false;
            }
        } else if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) {
            return false;
        }
        prev = c;
    }
    return prev != '/' && prev != '.';  /* so an empty path, which ends where it began, is no path */
}

/* Whether s ends with end. */
static bool ends_with(const char *s, const char *end) {
    size_t n = strlen(s), m = strlen(end);
    return n >= m && memcmp(s + n - m, end, m) == 0;
}

apps_file_kind_t apps_file_kind(const char *path) {
    if (path == NULL) {
        return APPS_FILE_BAD;
    }
    if (ends_with(path, ".lua")) {
        return APPS_FILE_LUA;
    }
    if (ends_with(path, ".json")) {
        return APPS_FILE_JSON;
    }
    if (ends_with(path, ".bin")) {
        return APPS_FILE_IMAGE;
    }
    return APPS_FILE_BAD;
}

bool apps_room(size_t free_bytes, const char *id, size_t bytes, unsigned memory_kb, char *err, size_t err_len) {
    size_t need = bytes + (size_t)memory_kb * 1024 + APPS_RAM_RESERVE;
    if (free_bytes >= need) {
        return true;
    }
    if (err != NULL && err_len > 0) {
        snprintf(err, err_len, APPS_MSG_SHORT_OF_MEMORY, id != NULL ? id : "", (unsigned)(free_bytes / 1024),
                 (unsigned)((need + 1023) / 1024));
    }
    return false;
}

bool apps_room_beside(size_t free_bytes, const char *id, size_t bytes, unsigned memory_kb, const char *other, size_t other_frees,
                      char *err, size_t err_len) {
    if (other == NULL || other[0] == '\0') {
        return apps_room(free_bytes, id, bytes, memory_kb, err, err_len);
    }
    id = id != NULL ? id : "";
    size_t load = bytes + APPS_RAM_RESERVE, run = load + (size_t)memory_kb * 1024;
    size_t after = other_frees > SIZE_MAX - free_bytes ? SIZE_MAX : free_bytes + other_frees;
    bool fits_after = after >= run, fits_now = free_bytes >= load;
    if (fits_after && fits_now) {
        return true;
    }
    if (err != NULL && err_len > 0) {
        if (!fits_after) {
            snprintf(err, err_len, APPS_MSG_SHORT_ONCE_ENDED, id, (unsigned)(after / 1024), other, (unsigned)((run + 1023) / 1024));
        } else {
            snprintf(err, err_len, APPS_MSG_SHORT_BESIDE, id, other, (unsigned)(free_bytes / 1024), (unsigned)((load + 1023) / 1024),
                     other, id);
        }
    }
    return false;
}

size_t apps_file_max(apps_file_kind_t kind) {
    switch (kind) {
    case APPS_FILE_LUA:
        return APPS_LUA_MAX;
    case APPS_FILE_JSON:
        return APPS_JSON_FILE_MAX;
    case APPS_FILE_IMAGE:
        return APPS_IMAGE_MAX;
    default:
        return 0;
    }
}

char *apps_text_shown(const char *s, size_t n, size_t max, char *out) {
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
        if (used + put > max) {
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
    return out;
}

size_t apps_text_next(const unsigned char *p, const unsigned char *end, bool keep_newline, bool *keep) {
    unsigned char c = *p;
    if (c < 0x80) {
        *keep = (c >= 0x20 && c != 0x7F) || c == '\t' || (keep_newline && c == '\n');
        return 1;
    }
    int n = apps_json_utf8_len(p, end);
    if (n <= 0) {  /* not UTF-8, or cut short by the end: this byte alone */
        *keep = false;
        return 1;
    }
    *keep = !(n == 2 && p[0] == 0xC2 && p[1] <= 0x9F);
    return (size_t)n;
}

size_t apps_text_clean(char *s, size_t n, bool keep_newline) {
    unsigned char *r = (unsigned char *)s, *w = r;
    const unsigned char *end = r + n;
    while (r < end) {
        bool keep;
        size_t k = apps_text_next(r, end, keep_newline, &keep);
        if (keep) {
            memmove(w, r, k);
            w += k;
        } else {
            *w++ = '?';
        }
        r += k;
    }
    size_t len = (size_t)(w - (unsigned char *)s);
    if (len < n) {
        s[len] = '\0';
    }
    return len;
}

/* ==== The manifest ==== */

/* The keys the manifest reads, in the order apps_pkg.h checks them (and the candidates of "did you mean"). */
enum { K_ID, K_NAME, K_VERSION, K_API, K_MAIN, K_FILES, K_PERMISSIONS, K_MEMORY_KB, K_MESSAGES, K_ICON, K_COUNT };

static const char *const k_names[K_COUNT] = {"id",   "name",        "version",   "api",     "main",
                                             "files", "permissions", "memory_kb", "messages", "icon"};

static const struct {
    const char *name;
    uint8_t bit;
} k_permissions[] = {
    {"sound", APPS_PERM_SOUND},
    {"alarm", APPS_PERM_ALARM},
    {"store", APPS_PERM_STORE},
    {"keep_awake", APPS_PERM_KEEP_AWAKE},
};

/* Designed for api 2 and refused in api 1. */
static const char *const k_future_permissions[] = {"net", "secrets", "muse"};

/* A value's JSON text, where it sits in the manifest. */
typedef struct {
    const char *p;  /* NULL: the key isn't there */
    size_t n;
} slice_t;

typedef struct {
    apps_manifest_t *m;
    const char *folder;
    int max_kb;  /* the most a memory_kb may be */
    char *err;
    size_t err_len;
    char *dropped;       /* a stored manifest's (apps_manifest_parse_stored): where an icon left out says why; NULL: the install's */
    size_t dropped_len;
    bool stored;         /* the store's read: an icon that breaks its rules is left out, not a failure */
    slice_t val[K_COUNT];
} parse_t;

#define fail(p, ...) failf((p)->err, (p)->err_len, __VA_ARGS__)

static int key_index(const char *key) {
    for (int i = 0; i < K_COUNT; i++) {
        if (strcmp(key, k_names[i]) == 0) {
            return i;
        }
    }
    return -1;
}

/* Whether warnings holds the warning w whole, between its separators. */
static bool warned(const char *warnings, const char *w) {
    size_t n = strlen(w);
    for (const char *at = strstr(warnings, w); at != NULL; at = strstr(at + 1, w)) {
        bool starts = at == warnings || (at - warnings >= 2 && at[-2] == ';' && at[-1] == ' ');
        bool ends = at[n] == '\0' || (at[n] == ';' && at[n + 1] == ' ');
        if (starts && ends) {
            return true;
        }
    }
    return false;
}

/* The warning for a key the manifest doesn't read, once, if it fits whole in what is left (R7.7). The key is shown as
 * apps_text_shown shows text from outside (a control character as '?': the warnings reach the console's replies), never
 * cut: it has at most TEXT_MAX - 1 bytes, and the mapping never lengthens it. */
static NOINLINE void warn_unknown(apps_manifest_t *m, const char *key) {
    char w[320];  /* a key of 255 bytes, a suggestion of 11 and the words */
    const char *suggestion = apps_suggest(key, k_names, K_COUNT);
    size_t at = (size_t)snprintf(w, sizeof w, "unknown key '");
    apps_text_shown(key, strlen(key), TEXT_MAX - 1, w + at);
    at += strlen(w + at);
    if (suggestion != NULL) {
        snprintf(w + at, sizeof w - at, "' ignored (did you mean '%s'?)", suggestion);
    } else {
        snprintf(w + at, sizeof w - at, "' ignored");
    }
    size_t have = strlen(m->warnings), n = strlen(w), sep = have > 0 ? 2 : 0;
    if (have + sep + n + 1 > sizeof m->warnings || warned(m->warnings, w)) {
        return;
    }
    memcpy(m->warnings + have, "; ", sep);
    memcpy(m->warnings + have + sep, w, n + 1);
}

static bool reader_error(parse_t *p, const apps_jr_t *r) {
    return fail(p, "manifest: %s", r->err[0] != '\0' ? r->err : "unreadable");
}

/* Pass 1: the text, checked to its end, each key the manifest reads kept as the place of its value. */
static NOINLINE bool scan(parse_t *p, const char *json, size_t len) {
    apps_jr_t r;
    char key[TEXT_MAX];
    apps_jr_init(&r, json, len);
    if (!apps_jr_object(&r)) {
        return reader_error(p, &r);
    }
    for (;;) {
        int more = apps_jr_key(&r, key, sizeof key);
        if (more == 0) {
            break;
        }
        size_t n = 0;
        const char *raw = more > 0 ? apps_jr_raw_value(&r, &n) : NULL;
        if (raw == NULL) {
            return reader_error(p, &r);
        }
        int k = key_index(key);
        if (k >= 0) {
            p->val[k] = (slice_t){raw, n};
        } else {
            warn_unknown(p->m, key);
        }
    }
    return apps_jr_end(&r) || reader_error(p, &r);
}

/* v, a string, decoded into out (cap bytes with the NUL). False when v isn't a string, or when the reader
 * refuses it: it doesn't fit, or it holds \u0000, which a C string can't. */
static bool decode_string(slice_t v, char *out, size_t cap) {
    out[0] = '\0';
    if (v.p == NULL || v.n < 2 || v.p[0] != '"') {
        return false;
    }
    apps_jr_t r;
    apps_jr_init(&r, v.p, v.n);
    return apps_jr_string(&r, out, cap);
}

/* v as a whole number (a JSON integer in int64), if it is one. */
static bool read_integer(slice_t v, int64_t *out) {
    if (v.p == NULL) {
        return false;
    }
    apps_jr_t r;
    apps_jr_init(&r, v.p, v.n);
    return apps_jr_integer(&r, out);
}

/* What to quote of v when `read` says text is its string: that; otherwise its JSON text, the inside of a
 * string that couldn't be read, cut by echo(). A key that isn't there is quoted as nothing. */
static const char *quote(echo_t *e, slice_t v, const char *text, bool read) {
    if (read) {
        return text;
    }
    if (v.p == NULL) {
        return "";
    }
    if (v.n >= 2 && v.p[0] == '"') {
        return echo(e, v.p + 1, v.n - 2);
    }
    return echo(e, v.p, v.n);
}

/* Pass 2: each key, in the order of apps_pkg.h. A check returns true, or false with the message in err. */

static NOINLINE bool check_id(parse_t *p) {
    slice_t v = p->val[K_ID];
    char text[TEXT_MAX];
    bool read = decode_string(v, text, sizeof text);
    if (read && apps_id_valid(text) && p->folder != NULL && strcmp(text, p->folder) == 0) {
        copy_text(p->m->id, sizeof p->m->id, text);
        return true;
    }
    echo_t shown, folder;
    return fail(p, "manifest: id '%s' must match ^[a-z][a-z0-9_]{1,23}$ and the folder name '%s'",
                quote(&shown, v, text, read), p->folder != NULL ? echo(&folder, p->folder, strlen(p->folder)) : "");
}

/* Whether s, valid UTF-8, holds a control character: C0 (under 0x20: a decoded \n too), DEL, or C1 (U+0080 to U+009F,
 * C2 80 to C2 9F), which a terminal reads as the start of an escape (U+009B is CSI). */
static bool has_control(const char *s) {
    for (const unsigned char *c = (const unsigned char *)s; *c != '\0'; c++) {
        if (*c < 0x20 || *c == 0x7F || (c[0] == 0xC2 && c[1] >= 0x80 && c[1] <= 0x9F)) {
            return true;
        }
    }
    return false;
}

/* The name and the version reach the console's replies and log lines as they are: no control character in them. */
static NOINLINE bool check_name(parse_t *p) {
    char text[TEXT_MAX];
    if (!decode_string(p->val[K_NAME], text, sizeof text) || text[0] == '\0' || strlen(text) >= sizeof p->m->name) {
        return fail(p, "manifest: name must be 1 to 24 characters");
    }
    if (has_control(text)) {
        return fail(p, "manifest: name has a control character");
    }
    copy_text(p->m->name, sizeof p->m->name, text);
    return true;
}

static NOINLINE bool check_version(parse_t *p) {
    slice_t v = p->val[K_VERSION];
    if (v.p != NULL && !decode_string(v, p->m->version, sizeof p->m->version)) {
        return fail(p, "manifest: version must be text of at most 23 characters, like \"1.0.0\"");
    }
    if (has_control(p->m->version)) {
        return fail(p, "manifest: version has a control character");
    }
    return true;
}

static NOINLINE bool check_api(parse_t *p) {
    slice_t v = p->val[K_API];
    int64_t api;
    if (read_integer(v, &api) && api == 1) {
        return true;
    }
    echo_t shown;
    return fail(p, "manifest: this gadget supports api 1 (manifest says api %s)", echo(&shown, v.p, v.n));
}

/* main, and with it the first of the files. */
static NOINLINE bool check_main(parse_t *p) {
    apps_manifest_t *m = p->m;
    slice_t v = p->val[K_MAIN];
    char text[TEXT_MAX] = "main.lua";
    if (v.p != NULL) {
        bool read = decode_string(v, text, sizeof text);
        if (!read || !apps_path_valid(text)) {
            echo_t shown;
            return fail(p, "manifest: main '%s' is not an allowed path " PATH_RULE, quote(&shown, v, text, read));
        }
        if (apps_file_kind(text) != APPS_FILE_LUA) {
            return fail(p, "manifest: main must name a .lua file");
        }
    }
    copy_text(m->main, sizeof m->main, text);
    copy_text(m->files[0], sizeof m->files[0], text);
    m->file_count = 1;
    return true;
}

static bool is_listed(const apps_manifest_t *m, const char *path) {
    for (int i = 0; i < m->file_count; i++) {
        if (strcmp(m->files[i], path) == 0) {
            return true;
        }
    }
    return false;
}

/* The rest of the files: each valid and typed, none twice, and with main at most APPS_MAX_FILES. */
static NOINLINE bool check_files(parse_t *p) {
    apps_manifest_t *m = p->m;
    slice_t v = p->val[K_FILES];
    if (v.p == NULL) {
        return true;
    }
    apps_jr_t r;
    char text[TEXT_MAX];
    apps_jr_init(&r, v.p, v.n);
    if (!apps_jr_array(&r)) {
        return fail(p, "manifest: files must be a list of paths, like [\"util.lua\"]");
    }
    for (int index = 0; apps_jr_next(&r) == 1; index++) {
        slice_t e = {NULL, 0};
        e.p = apps_jr_raw_value(&r, &e.n);
        bool read = decode_string(e, text, sizeof text);
        if (!read || !apps_path_valid(text)) {
            echo_t shown;
            return fail(p, "manifest: files[%d] '%s' is not an allowed path " PATH_RULE, index,
                        quote(&shown, e, text, read));
        }
        if (apps_file_kind(text) == APPS_FILE_BAD) {
            return fail(p, "manifest: files[%d] '%s' has an unsupported type (use .lua, .json or .bin)", index, text);
        }
        if (strcmp(text, "manifest.json") == 0) {  /* the store keeps it beside the files: a file of that path would replace it */
            return fail(p, "manifest: files[%d] 'manifest.json' is the manifest itself; don't list it", index);
        }
        if (is_listed(m, text)) {
            continue;
        }
        if (m->file_count >= APPS_MAX_FILES) {
            return fail(p, "manifest: more than 32 files");
        }
        copy_text(m->files[m->file_count++], sizeof m->files[0], text);
    }
    return r.err[0] == '\0' || reader_error(p, &r);
}

/* An icon that breaks a rule: the install's manifest fails; a stored one is read without its icon (m->icon stays ""), and dropped says
 * why (the final review's m3: an app installed before the rules were this strict keeps its place, with its tile). */
#define icon_fail(p, ...) ((p)->stored ? ((void)failf((p)->dropped, (p)->dropped_len, __VA_ARGS__), true) : fail(p, __VA_ARGS__))

/* icon: one of the files, and a .bin. The image itself is checked with the package's files (apps_icon_check). */
static NOINLINE bool check_icon(parse_t *p) {
    slice_t v = p->val[K_ICON];
    if (v.p == NULL) {
        return true;
    }
    char text[TEXT_MAX];
    bool read = decode_string(v, text, sizeof text);
    echo_t shown;
    if (!read || !apps_path_valid(text)) {
        return icon_fail(p, "manifest: icon '%s' is not an allowed path " PATH_RULE, quote(&shown, v, text, read));
    }
    if (apps_file_kind(text) != APPS_FILE_IMAGE) {
        return icon_fail(p, APPS_MSG_ICON_NOT_IMAGE, text);
    }
    if (!is_listed(p->m, text)) {
        return icon_fail(p, APPS_MSG_ICON_NOT_LISTED, text);
    }
    copy_text(p->m->icon, sizeof p->m->icon, text);
    return true;
}

static NOINLINE bool check_permissions(parse_t *p) {
    slice_t v = p->val[K_PERMISSIONS];
    if (v.p == NULL) {
        return true;
    }
    apps_jr_t r;
    char text[TEXT_MAX];
    apps_jr_init(&r, v.p, v.n);
    if (!apps_jr_array(&r)) {
        return fail(p, "manifest: permissions must be a list, like [\"sound\"] " PERMISSIONS_VALID);
    }
    while (apps_jr_next(&r) == 1) {
        slice_t e = {NULL, 0};
        e.p = apps_jr_raw_value(&r, &e.n);
        bool read = decode_string(e, text, sizeof text);
        bool known = false;
        for (size_t i = 0; read && i < sizeof k_permissions / sizeof k_permissions[0]; i++) {
            if (strcmp(text, k_permissions[i].name) == 0) {
                p->m->perms = (uint8_t)(p->m->perms | k_permissions[i].bit);
                known = true;
            }
        }
        if (known) {
            continue;
        }
        for (size_t i = 0; read && i < sizeof k_future_permissions / sizeof k_future_permissions[0]; i++) {
            if (strcmp(text, k_future_permissions[i]) == 0) {
                return fail(p, "manifest: permission '%s' is not available in this firmware yet (api 1)", text);
            }
        }
        echo_t shown;
        return fail(p, "manifest: unknown permission '%s' " PERMISSIONS_VALID, quote(&shown, e, text, read));
    }
    return r.err[0] == '\0' || reader_error(p, &r);
}

static NOINLINE bool check_memory(parse_t *p) {
    slice_t v = p->val[K_MEMORY_KB];
    int64_t kb = p->max_kb < DEFAULT_MEMORY_KB ? p->max_kb : DEFAULT_MEMORY_KB;
    if (v.p != NULL && (!read_integer(v, &kb) || kb < MIN_MEMORY_KB || kb > p->max_kb)) {
        return fail(p, "manifest: memory_kb must be a whole number from 64 to %d", p->max_kb);
    }
    p->m->memory_kb = (uint16_t)kb;
    return true;
}

/* messages: event names to descriptions, MESSAGES_MAX at most, each name and description under
 * MESSAGE_TEXT_MAX bytes. Nothing is kept here: see keep_messages. */
static NOINLINE bool check_messages(parse_t *p) {
    slice_t v = p->val[K_MESSAGES];
    if (v.p == NULL) {
        return true;
    }
    static const char refused[] =
        "manifest: messages must map event names to descriptions (at most 16, each under 200 characters)";
    apps_jr_t r;
    char text[MESSAGE_TEXT_MAX];
    apps_jr_init(&r, v.p, v.n);
    apps_jr_object(&r);  /* if v isn't an object this fails, and so does the first apps_jr_key below */
    for (int events = 0;; events++) {
        int more = apps_jr_key(&r, text, sizeof text);  /* the name, in text, which the description then reuses */
        if (more == 0) {
            return true;
        }
        slice_t d = {NULL, 0};
        if (more > 0 && events < MESSAGES_MAX) {
            d.p = apps_jr_raw_value(&r, &d.n);
        }
        if (!decode_string(d, text, sizeof text)) {
            return fail(p, "%s", refused);
        }
    }
}

/* Where the manifest's one block of memory comes from (R7.8): PSRAM on the device, since a block of 4 KB or
 * less would come from internal RAM, which is scarce and where this one lives as long as the app. */
static char *alloc_block(size_t n) {
#ifdef ESP_PLATFORM
    return heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
#else
    return malloc(n);
#endif
}

/* messages_json: last of all, so that nothing is left to free when anything above fails. */
static bool keep_messages(parse_t *p) {
    slice_t v = p->val[K_MESSAGES];
    if (v.p == NULL) {
        return true;
    }
    char *json = alloc_block(v.n + 1);
    if (json == NULL) {
        return fail(p, "manifest: out of memory");
    }
    apps_json_compact(v.p, v.n, json, v.n + 1);
    p->m->messages_json = json;
    return true;
}

/* The keys that must be there, found missing once every key that is there has passed. id and name fail their
 * own checks, which then find nothing to read. */
static bool check_required(parse_t *p) {
    if (p->val[K_ID].p == NULL) {
        return check_id(p);
    }
    if (p->val[K_NAME].p == NULL) {
        return check_name(p);
    }
    return p->val[K_API].p != NULL || fail(p, "manifest: api is missing (this gadget supports api 1)");
}

/* Everything pass 1 found, in the order of apps_pkg.h. */
static bool validate(parse_t *p) {
    const slice_t *v = p->val;
    if (v[K_ID].p != NULL && !check_id(p)) {
        return false;
    }
    if (v[K_NAME].p != NULL && !check_name(p)) {
        return false;
    }
    if (!check_version(p)) {
        return false;
    }
    if (v[K_API].p != NULL && !check_api(p)) {
        return false;
    }
    return check_main(p) && check_files(p) && check_icon(p) && check_permissions(p) && check_memory(p) && check_messages(p) &&
           check_required(p);
}

static bool parse_manifest(parse_t *p, const char *json, size_t len, int heap_max_kb) {
    p->max_kb = heap_max_kb > MAX_MEMORY_KB ? MAX_MEMORY_KB : heap_max_kb;
    if (p->max_kb < MIN_MEMORY_KB) {
        p->max_kb = MIN_MEMORY_KB;
    }
    memset(p->m, 0, sizeof *p->m);
    if (p->err != NULL && p->err_len > 0) {
        p->err[0] = '\0';
    }
    if (p->dropped != NULL && p->dropped_len > 0) {
        p->dropped[0] = '\0';
    }
    bool ok = len > APPS_MANIFEST_MAX ? fail(p, "manifest: manifest.json is larger than 8 KB")
                                      : scan(p, json, len) && validate(p) && keep_messages(p);
    if (!ok) {
        memset(p->m, 0, sizeof *p->m);
        if (p->dropped != NULL && p->dropped_len > 0) {
            p->dropped[0] = '\0';  /* a manifest that failed left nothing out: it isn't read at all */
        }
    }
    return ok;
}

bool apps_manifest_parse(const char *json, size_t len, const char *expected_id, int heap_max_kb, apps_manifest_t *m,
                         char *err, size_t err_len) {
    parse_t p = {.m = m, .folder = expected_id, .err = err, .err_len = err_len};
    return parse_manifest(&p, json, len, heap_max_kb);
}

bool apps_manifest_parse_stored(const char *json, size_t len, const char *expected_id, int heap_max_kb, apps_manifest_t *m,
                                char *err, size_t err_len, char *dropped, size_t dropped_len) {
    parse_t p = {.m = m, .folder = expected_id, .err = err, .err_len = err_len, .dropped = dropped, .dropped_len = dropped_len,
                 .stored = true};
    return parse_manifest(&p, json, len, heap_max_kb);
}

/* free_fn, or free. */
static void release(void *block, void (*free_fn)(void *)) {
    if (free_fn != NULL) {
        free_fn(block);
    } else {
        free(block);
    }
}

void apps_manifest_clear(apps_manifest_t *m, void (*free_fn)(void *)) {
    if (m == NULL) {
        return;
    }
    if (m->messages_json != NULL) {
        release(m->messages_json, free_fn);
    }
    memset(m, 0, sizeof *m);
}

void apps_pkg_free(apps_pkg_t *pkg, void (*free_fn)(void *)) {
    if (pkg == NULL) {
        return;
    }
    for (int i = 0; i < APPS_MAX_FILES; i++) {
        if (pkg->files[i].data != NULL) {
            release(pkg->files[i].data, free_fn);
        }
    }
    if (pkg->manifest_json != NULL) {
        release(pkg->manifest_json, free_fn);
    }
    apps_manifest_clear(&pkg->manifest, free_fn);
    memset(pkg, 0, sizeof *pkg);
}

/* ==== Compile messages ==== */

int apps_pkg_compile_order(const apps_pkg_t *pkg, const apps_blob_t *order[APPS_MAX_FILES]) {
    int n = 0;
    for (int pass = 0; pass < 2; pass++) {  /* pass 0 is main alone, pass 1 the rest */
        for (int i = 0; i < pkg->count && i < APPS_MAX_FILES; i++) {
            const apps_blob_t *b = &pkg->files[i];
            if ((strcmp(b->path, pkg->manifest.main) == 0) == (pass == 0) && apps_file_kind(b->path) == APPS_FILE_LUA) {
                order[n++] = b;
            }
        }
    }
    return n;
}

void apps_pkg_compile_message(const apps_pkg_t *pkg, const char *raw, char *out, size_t cap) {
    if (out == NULL || cap == 0) {
        return;
    }
    int n = snprintf(out, cap, "compile failed: ");
    size_t at = n > 0 && (size_t)n < cap ? (size_t)n : cap - 1;
    const char *src = NULL;
    size_t len = 0;
    /* "<file>:<line>: ...": the file is what comes before the first ":<digits>:". */
    for (const char *colon = strchr(raw, ':'); colon != NULL && src == NULL; colon = strchr(colon + 1, ':')) {
        const char *d = colon + 1;
        while (*d >= '0' && *d <= '9') {
            d++;
        }
        if (d == colon + 1 || *d != ':') {
            continue;
        }
        size_t name = (size_t)(colon - raw);
        for (int i = 0; i < pkg->count && i < APPS_MAX_FILES; i++) {
            if (strlen(pkg->files[i].path) == name && memcmp(pkg->files[i].path, raw, name) == 0) {
                src = (const char *)pkg->files[i].data;
                len = pkg->files[i].len;
            }
        }
        break;
    }
    size_t used = apps_vm_excerpt(raw, src, len, out + at, cap - at);
    /* Lua's message and the lines are bytes from outside (apps_text_clean). The message is one line, though a token can
     * hold a newline: it is what the excerpt wrote first, and the newlines after it are the excerpt's own. */
    size_t head = strlen(raw) < used ? strlen(raw) : used;
    for (size_t i = 0; i < head; i++) {
        if (out[at + i] == '\n') {
            out[at + i] = '?';
        }
    }
    apps_text_clean(out + at, used, true);
}

bool apps_pkg_check_source(int index, const char *path, const uint8_t *data, size_t len, char *err, size_t err_len) {
    if (err != NULL && err_len > 0) {
        err[0] = '\0';
    }
    if (apps_file_kind(path) == APPS_FILE_LUA && len >= 4 && memcmp(data, "\x1bLua", 4) == 0) {
        return failf(err, err_len, "files[%d] %s is a compiled chunk: install Lua source text", index, path);
    }
    return true;
}

/* ==== Images ==== */

static unsigned le16(const uint8_t *p) {
    return (unsigned)p[0] | ((unsigned)p[1] << 8);
}

bool apps_image_check(const uint8_t *data, size_t len, uint16_t *w, uint16_t *h, char *err, size_t err_len) {
    if (w != NULL) {
        *w = 0;
    }
    if (h != NULL) {
        *h = 0;
    }
    if (err != NULL && err_len > 0) {
        err[0] = '\0';
    }
    if (data == NULL || len < IMAGE_HEADER || data[0] != IMAGE_MAGIC) {
        return failf(err, err_len, "image: not an LVGL v9 image file (use LVGLImage.py --ofmt BIN)");
    }
    unsigned cf = data[1], flags = le16(data + 2), iw = le16(data + 4), ih = le16(data + 6), stride = le16(data + 8);
    if ((flags & IMAGE_FLAG_COMPRESSED) != 0) {
        return failf(err, err_len, "image: compressed images aren't supported");
    }
    /* The others (allocated, modifiable...) say what the image's memory is, and apps_lv draws the file in place: LVGL
     * would take a descriptor flagged allocated for one of its draw buffers, refuse it, and never show the image. */
    if ((flags & ~(unsigned)IMAGE_FLAG_PREMULTIPLIED) != 0) {
        return failf(err, err_len, "image: flags 0x%04x aren't supported (only premultiplied alpha)", flags);
    }
    if (iw == 0 || ih == 0) {
        return failf(err, err_len, "image: %ux%u has no pixels", iw, ih);
    }
    if (iw > IMAGE_SCREEN || ih > IMAGE_SCREEN) {
        return failf(err, err_len, "image: %ux%u is larger than the %dx%d screen", iw, ih, IMAGE_SCREEN, IMAGE_SCREEN);
    }
    unsigned row;       /* the bytes a row of pixels takes: all the stride has to hold */
    bool alpha = false;  /* RGB565A8: a plane of alpha follows the colour, (stride / 2) a row */
    switch (cf) {
        case IMAGE_CF_RGB565:
            row = iw * 2;
            break;
        case IMAGE_CF_RGB565A8:
            row = iw * 2;
            alpha = true;
            break;
        case IMAGE_CF_A8:
            row = iw;
            break;
        default:
            return failf(err, err_len, "image: colour format 0x%02x isn't RGB565, RGB565A8 or A8", cf);
    }
    if (stride < row) {
        return failf(err, err_len, "image: stride %u is too small for a row of %u pixels (%u bytes needed)", stride, iw,
                     row);
    }
    unsigned long long colour = (unsigned long long)stride * ih;
    unsigned long long want = IMAGE_HEADER + colour + (alpha ? (unsigned long long)(stride / 2) * ih : 0);
    if ((unsigned long long)len != want) {
        return failf(err, err_len, "image: size %llu doesn't match its header (%llu expected)", (unsigned long long)len,
                     want);
    }
    if (w != NULL) {
        *w = (uint16_t)iw;
    }
    if (h != NULL) {
        *h = (uint16_t)ih;
    }
    return true;
}

bool apps_icon_check(const uint8_t *data, size_t len, char *err, size_t err_len) {
    uint16_t w, h;
    if (!apps_image_check(data, len, &w, &h, err, err_len)) {
        return false;
    }
    if (w != h || w > APPS_ICON_MAX_PX) {
        return failf(err, err_len, APPS_MSG_ICON_SIZE, (unsigned)w, (unsigned)h, APPS_ICON_MAX_PX, APPS_ICON_MAX_PX);
    }
    /* The launcher reads at most this (apps_icons.c): a padded file past it would install and show as its tile at every load. */
    if (len > APPS_ICON_MAX_BYTES) {
        return failf(err, err_len, APPS_MSG_ICON_BYTES, (unsigned)len, (unsigned)APPS_ICON_MAX_BYTES);
    }
    return true;
}
