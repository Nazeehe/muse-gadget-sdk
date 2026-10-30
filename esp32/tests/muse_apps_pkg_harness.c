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
 * Checks apps_pkg: ids, paths, file kinds, the manifest and LVGL images. Usage:
 *   muse_apps_pkg_harness list
 *   muse_apps_pkg_harness parse <file> <folder|-> <heap_kb> [err_len]
 *   muse_apps_pkg_harness check id|path|kind          (one text per line on stdin, one answer per line)
 *   muse_apps_pkg_harness image <file>
 *   muse_apps_pkg_harness <case>                      (a case listed by "list": one test of tests/test_muse_apps_pkg.py)
 * It exits 0 when everything it checked holds and prints what it got when something doesn't.
 *
 * parse and image print their verdict as one JSON line for the Python test to compare, so every message
 * is asserted exactly there; the harness itself checks what must hold of every result. A manifest that
 * parses: its id is valid and the folder's, every path is valid and typed, no file is listed twice, the
 * count and memory are in range, messages_json is a compact object of at most 16 strings, and no
 * warning is cut. One that doesn't: the message starts "manifest: ", fits err and ends on a character
 * boundary, and the manifest is all zero. Every text the module hands out must be one a JSON string can
 * carry (valid UTF-8), since it reaches Muse inside one.
 *
 * Every input goes in a heap block of exactly its length, and the error buffer is one of exactly err_len
 * bytes, so a read or write past either is an AddressSanitizer report (see muse_apps_json_harness.c:
 * a literal has a NUL after it, a stack buffer has room, and neither shows an overrun).
 *
 * apps_pkg.c is built with ESP_PLATFORM and a fake esp_heap_caps.h (tests/apps_pkg_fakes), so the
 * heap_caps_malloc below is the call it makes on the device: it records the capabilities it is asked
 * for, counts what it hands out (the leak check: LeakSanitizer doesn't run on macOS) and can refuse.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#include "apps_json.h"
#include "apps_pkg.h"
#include "esp_heap_caps.h"

#if defined(__SANITIZE_ADDRESS__)
#define HAVE_ASAN 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer)
#define HAVE_ASAN 1
#endif
#endif
#ifdef HAVE_ASAN
#include <sanitizer/asan_interface.h>
#endif

static int failures;

#define CHECK_AT(line, cond, ...)                                  \
    do {                                                           \
        if (!(cond)) {                                             \
            failures++;                                            \
            fprintf(stderr, "FAIL line %d: %s: ", (line), #cond);  \
            fprintf(stderr, __VA_ARGS__);                          \
            fputc('\n', stderr);                                   \
        }                                                          \
    } while (0)
#define CHECK(cond, ...) CHECK_AT(__LINE__, cond, __VA_ARGS__)

#define LIT(s) (s), (sizeof(s) - 1)

/* ---- Exact-size texts ---- */

typedef struct {
    void *block;
    bool mapped;  /* an mmap'd page pair, not a malloc block */
} exact_t;

static exact_t *exact_blocks;
static size_t exact_count, exact_room;

static void exact_track(void *block, bool mapped) {
    if (exact_count == exact_room) {
        exact_room = exact_room != 0 ? 2 * exact_room : 64;
        exact_blocks = realloc(exact_blocks, exact_room * sizeof *exact_blocks);
        if (exact_blocks == NULL) {
            fputs("out of memory in the harness\n", stderr);
            exit(2);
        }
    }
    exact_blocks[exact_count++] = (exact_t){block, mapped};
}

/* An address with no byte to read or write at it: the end of a mapped page, with an inaccessible one
 * after. malloc(0) won't do: ASan hands out a usable byte for it. */
static char *exact_nothing(void) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    char *map = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (map == MAP_FAILED || mprotect(map + page, page, PROT_NONE) != 0) {
        fputs("no pages for an empty text\n", stderr);
        exit(2);
    }
    exact_track(map, true);
    return map + page;
}

/* A block of exactly len bytes (filled from s when that isn't NULL, else with 'x'), so that a read or a
 * write at [len] is a heap-buffer-overflow. */
static char *exact_block(const char *s, size_t len) {
    if (len == 0) {
        return exact_nothing();
    }
    char *p = malloc(len);
    if (p == NULL) {
        fputs("out of memory in the harness\n", stderr);
        exit(2);
    }
    if (s != NULL) {
        memcpy(p, s, len);
    } else {
        memset(p, 'x', len);
    }
    exact_track(p, false);
    return p;
}

static char *exact_copy(const char *s, size_t len) {
    return exact_block(s, len);
}

/* Frees them all. Nothing may use one afterwards. */
static void exact_release(void) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    for (size_t i = 0; i < exact_count; i++) {
        if (exact_blocks[i].mapped) {
            munmap(exact_blocks[i].block, 2 * page);
        } else {
            free(exact_blocks[i].block);
        }
    }
    exact_count = 0;
}

/* ---- The heap apps_pkg.c allocates from on the device (esp_heap_caps.h) ---- */

typedef union {
    size_t size;
    max_align_t align;
} block_t;

static size_t held;         /* bytes handed out and not freed */
static unsigned calls;      /* heap_caps_malloc calls, refused ones too */
static uint32_t last_caps;  /* what the latest call asked for */
static bool refuse;         /* the next calls return NULL */
static bool plain_blocks;   /* hand out blocks that free() takes, uncounted */

void *heap_caps_malloc(size_t size, uint32_t caps) {
    calls++;
    last_caps = caps;
    if (refuse) {
        return NULL;
    }
    if (plain_blocks) {
        return malloc(size);
    }
    block_t *b = malloc(sizeof *b + size);
    if (b == NULL) {
        return NULL;
    }
    b->size = size;
    held += size;
    return b + 1;
}

static void count_free(void *ptr) {
    if (ptr != NULL) {
        block_t *b = (block_t *)ptr - 1;
        held -= b->size;
        free(b);
    }
}

static void reset_heap(void) {
    calls = 0;
    last_caps = 0;
    refuse = false;
    plain_blocks = false;
}

/* ---- Output ---- */

/* s as a JSON string. */
static void put_json_string(const char *s) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            putchar('\\');
            putchar(*p);
        } else if (*p < 0x20) {
            printf("\\u%04x", *p);
        } else {
            putchar(*p);
        }
    }
    putchar('"');
}

/* Whether s is text that a JSON string carries: valid UTF-8 (RFC 3629), by the scanner's own verdict on s
 * quoted. Control characters are fine: they are escaped. */
static bool json_text_ok(const char *s) {
    size_t n = strlen(s), k = 0;
    char *q = malloc(6 * n + 3);
    q[k++] = '"';
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        if (*p == '"' || *p == '\\') {
            q[k++] = '\\';
            q[k++] = (char)*p;
        } else if (*p < 0x20) {
            k += (size_t)snprintf(q + k, 7, "\\u%04x", *p);
        } else {
            q[k++] = (char)*p;
        }
    }
    q[k++] = '"';
    char err[96];
    bool ok = apps_json_validate(q, k, err, sizeof err);
    free(q);
    return ok;
}

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (f == NULL) {
        fprintf(stderr, "can't read %s\n", path);
        exit(2);
    }
    size_t cap = 4096, n = 0;
    char *buf = malloc(cap);
    for (;;) {
        size_t got = fread(buf + n, 1, cap - n, f);
        n += got;
        if (got == 0) {
            break;
        }
        if (n == cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
    }
    fclose(f);
    *len = n;
    return buf;
}

/* ---- What must hold of every result ---- */

static bool is_zero(const apps_manifest_t *m) {
    static const apps_manifest_t zero;
    return memcmp(m, &zero, sizeof zero) == 0;
}

/* messages_json: a compact object of at most 16 names and descriptions, each under 200 bytes. */
static void check_messages_json(const char *json) {
    size_t n = strlen(json);
    char err[96];
    CHECK(json[0] == '{', "messages_json '%s' isn't an object", json);
    CHECK(apps_json_validate(json, n, err, sizeof err), "messages_json '%s': %s", json, err);
    char *again = malloc(n + 1);
    CHECK(apps_json_compact(json, n, again, n + 1) == n && strcmp(again, json) == 0, "messages_json '%s' isn't compact", json);
    free(again);
    apps_jr_t r;
    char text[200];
    int count = 0;
    apps_jr_init(&r, json, n);
    CHECK(apps_jr_object(&r), "%s", r.err);
    while (apps_jr_key(&r, text, sizeof text) == 1) {
        count++;
        CHECK(apps_jr_string(&r, text, sizeof text), "messages_json '%s': a description that isn't text under 200 bytes: %s", json, r.err);
    }
    CHECK(count <= 16, "messages_json has %d entries", count);
    CHECK(r.err[0] == '\0' && apps_jr_end(&r), "messages_json '%s': %s", json, r.err);
}

/* Whether s holds a control character: C0 (under 0x20), DEL, or C1 (U+0080 to U+009F, C2 80 to C2 9F in UTF-8). */
static bool has_control(const char *s) {
    for (const unsigned char *p = (const unsigned char *)s; *p != '\0'; p++) {
        if (*p < 0x20 || *p == 0x7F || (p[0] == 0xC2 && p[1] >= 0x80 && p[1] <= 0x9F)) {
            return true;
        }
    }
    return false;
}

/* A manifest that parsed, for folder and heap_kb. */
static void check_manifest(const apps_manifest_t *m, const char *folder, int heap_kb) {
    CHECK(apps_id_valid(m->id) && strcmp(m->id, folder) == 0, "id '%s' for folder '%s'", m->id, folder);
    size_t name_len = strlen(m->name);
    CHECK(name_len >= 1 && name_len <= 24, "name of %zu bytes", name_len);
    CHECK(strlen(m->version) <= 23, "version of %zu bytes", strlen(m->version));
    /* Task 13's F7: what reaches the console's replies carries no control character. */
    CHECK(!has_control(m->name) && !has_control(m->version), "a control character in name '%s' or version '%s'", m->name,
          m->version);
    CHECK(apps_path_valid(m->main) && apps_file_kind(m->main) == APPS_FILE_LUA, "main '%s'", m->main);
    CHECK(m->file_count >= 1 && m->file_count <= APPS_MAX_FILES, "file_count %u", m->file_count);
    CHECK(strcmp(m->files[0], m->main) == 0, "files[0] '%s' isn't main '%s'", m->files[0], m->main);
    for (int i = 0; i < APPS_MAX_FILES; i++) {
        if (i < m->file_count) {
            CHECK(apps_path_valid(m->files[i]) && apps_file_kind(m->files[i]) != APPS_FILE_BAD, "files[%d] '%s'", i, m->files[i]);
            for (int j = 0; j < i; j++) {
                CHECK(strcmp(m->files[i], m->files[j]) != 0, "files[%d] and [%d] are both '%s'", j, i, m->files[i]);
            }
        } else {
            CHECK(m->files[i][0] == '\0', "files[%d] '%s' is past file_count %u", i, m->files[i], m->file_count);
        }
    }
    if (m->icon[0] != '\0') {
        bool listed = false;
        for (int i = 0; i < m->file_count; i++) {
            listed = listed || strcmp(m->files[i], m->icon) == 0;
        }
        CHECK(listed && apps_file_kind(m->icon) == APPS_FILE_IMAGE, "icon '%s' isn't a listed .bin", m->icon);
    }
    CHECK((m->perms & ~15u) == 0, "perms 0x%x", m->perms);
    int max_kb = heap_kb > 1024 ? 1024 : heap_kb;
    if (max_kb < 64) {
        max_kb = 64;
    }
    CHECK(m->memory_kb >= 64 && m->memory_kb <= max_kb, "memory_kb %u for a heap of %d KB", m->memory_kb, heap_kb);
    if (m->messages_json != NULL) {
        check_messages_json(m->messages_json);
    }
    size_t warn_len = strlen(m->warnings);
    CHECK(warn_len < sizeof m->warnings, "warnings aren't terminated");
    CHECK(json_text_ok(m->warnings), "warnings aren't valid text: '%s'", m->warnings);
    CHECK(!has_control(m->warnings), "a control character in the warnings: '%s'", m->warnings);
    if (warn_len > 0) {
        /* Whole warnings only: each is "unknown key '...' ignored", with " (did you mean '...'?)" at most. */
        static const char start[] = "unknown key '";
        CHECK(strncmp(m->warnings, start, sizeof start - 1) == 0, "warnings '%s'", m->warnings);
        CHECK((warn_len >= 9 && strcmp(m->warnings + warn_len - 9, "' ignored") == 0) ||
                  (warn_len >= 3 && strcmp(m->warnings + warn_len - 3, "'?)") == 0),
              "warnings end in a cut warning: '%s'", m->warnings);
    }
}

/* A manifest that didn't. */
static void check_failed(const apps_manifest_t *m, const char *err, size_t err_len) {
    static const char prefix[] = "manifest: ";
    size_t n = strlen(err);
    CHECK(n < err_len, "err isn't terminated within %zu bytes", err_len);
    CHECK(strncmp(err, prefix, n < sizeof prefix - 1 ? n : sizeof prefix - 1) == 0, "err '%s' doesn't start with 'manifest: '", err);
    CHECK(json_text_ok(err), "err isn't valid text: '%s'", err);
    CHECK(is_zero(m), "a manifest that failed isn't all zero (id '%s')", m->id);
}

/* ---- parse, check and image: driven by the Python test ---- */

static void print_manifest(const apps_manifest_t *m) {
    fputs("{\"ok\":true,\"id\":", stdout);
    put_json_string(m->id);
    fputs(",\"name\":", stdout);
    put_json_string(m->name);
    fputs(",\"version\":", stdout);
    put_json_string(m->version);
    fputs(",\"main\":", stdout);
    put_json_string(m->main);
    fputs(",\"files\":[", stdout);
    for (int i = 0; i < m->file_count; i++) {
        if (i > 0) {
            putchar(',');
        }
        put_json_string(m->files[i]);
    }
    printf("],\"perms\":%u,\"memory_kb\":%u,\"messages_json\":", (unsigned)m->perms, (unsigned)m->memory_kb);
    if (m->messages_json != NULL) {
        put_json_string(m->messages_json);
    } else {
        fputs("null", stdout);
    }
    fputs(",\"warnings\":", stdout);
    put_json_string(m->warnings);
    fputs(",\"icon\":", stdout);
    put_json_string(m->icon);
    puts("}");
}

/* parse <file> <folder|-> <heap_kb> [err_len] */
static void run_parse(int argc, char **argv) {
    if (argc < 5) {
        fputs("usage: parse <file> <folder|-> <heap_kb> [err_len]\n", stderr);
        exit(2);
    }
    size_t len, err_len = argc > 5 ? strtoul(argv[5], NULL, 10) : 1024;
    if (err_len == 0) {
        fputs("parse needs an err_len of at least 1\n", stderr);
        exit(2);
    }
    char *raw = read_file(argv[2], &len);
    const char *folder = strcmp(argv[3], "-") == 0 ? NULL : argv[3];
    int heap_kb = atoi(argv[4]);
    apps_manifest_t *m = malloc(sizeof *m);
    memset(m, 0xA5, sizeof *m);  /* parse overwrites all of it */
    char *err = exact_block(NULL, err_len);
    bool ok = apps_manifest_parse(exact_copy(raw, len), len, folder, heap_kb, m, err, err_len);
    free(raw);
    if (ok) {
        CHECK(err[0] == '\0', "err '%s' after a manifest that parsed", err);
        check_manifest(m, folder, heap_kb);
        print_manifest(m);
    } else {
        check_failed(m, err, err_len);
        fputs("{\"ok\":false,\"error\":", stdout);
        put_json_string(err);
        puts("}");
    }
    apps_manifest_clear(m, count_free);
    CHECK(is_zero(m), "clear left the manifest not zero");
    free(m);
}

/* check id|path|kind: a text per line on stdin, an answer per line. */
static void run_check(int argc, char **argv) {
    if (argc < 3) {
        fputs("usage: check id|path|kind\n", stderr);
        exit(2);
    }
    static const char *const kinds[] = {"lua", "json", "image", "bad"};
    char *line = NULL;
    size_t cap = 0;
    ssize_t n;
    while ((n = getline(&line, &cap, stdin)) >= 0) {
        if (n > 0 && line[n - 1] == '\n') {
            line[--n] = '\0';
        }
        const char *text = exact_copy(line, (size_t)n + 1);  /* with its NUL, and no byte after it */
        if (strcmp(argv[2], "id") == 0) {
            puts(apps_id_valid(text) ? "1" : "0");
        } else if (strcmp(argv[2], "path") == 0) {
            puts(apps_path_valid(text) ? "1" : "0");
        } else {
            apps_file_kind_t kind = apps_file_kind(text);
            CHECK((int)kind >= 0 && (int)kind <= (int)APPS_FILE_BAD, "kind %d", (int)kind);
            puts(kinds[kind]);
        }
    }
    free(line);
}

/* image <file> */
static void run_image(int argc, char **argv) {
    if (argc < 3) {
        fputs("usage: image <file>\n", stderr);
        exit(2);
    }
    size_t len;
    char *raw = read_file(argv[2], &len);
    const uint8_t *data = (const uint8_t *)exact_copy(raw, len);
    free(raw);
    uint16_t w = 0xAAAA, h = 0xAAAA;
    char err[256];
    memset(err, 'x', sizeof err);
    bool ok = apps_image_check(data, len, &w, &h, err, sizeof err);
    CHECK(apps_image_check(data, len, NULL, NULL, NULL, 0) == ok, "the verdict changes without out parameters");
    if (ok) {
        CHECK(err[0] == '\0', "err '%s' after an image that passed", err);
        printf("{\"ok\":true,\"w\":%u,\"h\":%u}\n", w, h);
    } else {
        CHECK(w == 0 && h == 0, "%ux%u after an image that failed", w, h);
        CHECK(strncmp(err, "image: ", 7) == 0, "err '%s'", err);
        CHECK(json_text_ok(err), "err isn't valid text");
        fputs("{\"ok\":false,\"error\":", stdout);
        put_json_string(err);
        puts("}");
    }
}

/* icon <file> */
static void run_icon(int argc, char **argv) {
    if (argc < 3) {
        fputs("usage: icon <file>\n", stderr);
        exit(2);
    }
    size_t len;
    char *raw = read_file(argv[2], &len);
    const uint8_t *data = (const uint8_t *)exact_copy(raw, len);
    free(raw);
    char err[256];
    memset(err, 'x', sizeof err);
    bool ok = apps_icon_check(data, len, err, sizeof err);
    CHECK(apps_icon_check(data, len, NULL, 0) == ok, "the verdict changes without an err");
    if (ok) {
        CHECK(err[0] == '\0', "err '%s' after an icon that passed", err);
        puts("{\"ok\":true}");
    } else {
        CHECK(json_text_ok(err), "err isn't valid text");
        fputs("{\"ok\":false,\"error\":", stdout);
        put_json_string(err);
        puts("}");
    }
}

/* ---- Cases ---- */

static const char timer_manifest[] =
    "{\n"
    "  \"id\": \"timer\",\n"
    "  \"name\": \"Timer\",\n"
    "  \"version\": \"1.0.0\",\n"
    "  \"api\": 1,\n"
    "  \"main\": \"main.lua\",\n"
    "  \"files\": [\"util.lua\", \"assets/bell.bin\"],\n"
    "  \"permissions\": [\"sound\", \"alarm\", \"store\"],\n"
    "  \"memory_kb\": 128,\n"
    "  \"messages\": {\n"
    "    \"start\": \"Start a countdown. data: {seconds: whole number 1..5999}. Replies {state, remaining}.\",\n"
    "    \"status\": \"Replies {state, remaining} in seconds.\"\n"
    "  }\n"
    "}";

static const char minimal_manifest[] = "{\"id\":\"timer\",\"name\":\"Timer\",\"api\":1}";

/* Parses text[0, len) (NULL: no text) from an exact copy, for folder, into m, with an err of exactly err_len
 * bytes whose message comes back in the caller's err (NULL: no err at all; err_len 0: one that mustn't be
 * written). Checks everything that must hold of the result. */
static bool parse_checked(const char *text, size_t len, const char *folder, int heap_kb, apps_manifest_t *m,
                          char *err, size_t err_len) {
    memset(m, 0xA5, sizeof *m);
    char *buffer = err != NULL ? exact_block(NULL, err_len) : NULL;
    bool ok = apps_manifest_parse(text != NULL ? exact_copy(text, len) : NULL, len, folder, heap_kb, m, buffer, err_len);
    if (err != NULL && err_len > 0) {
        memcpy(err, buffer, err_len);
    }
    if (ok) {
        check_manifest(m, folder != NULL ? folder : "", heap_kb);
    } else if (err != NULL && err_len > 0) {
        check_failed(m, err, err_len);
    } else {
        CHECK(is_zero(m), "a manifest that failed isn't all zero");
    }
    return ok;
}

/* The documents the contracts and cuts below parse, and the message each refuses with. */
static void contracts(void) {
    CHECK(!apps_id_valid(NULL) && !apps_path_valid(NULL) && apps_file_kind(NULL) == APPS_FILE_BAD, "NULL strings are valid");
    /* apps_pkg.h says so, and it is why they are allocated: on the host's 64-bit pointers, the larger. */
    CHECK(sizeof(apps_manifest_t) < 3 * 1024 && sizeof(apps_pkg_t) < 7 * 1024, "apps_manifest_t is %zu bytes and apps_pkg_t %zu",
          sizeof(apps_manifest_t), sizeof(apps_pkg_t));
    apps_manifest_t *m = malloc(sizeof *m);
    char err[512];

    /* NULL text of length 0 is an empty document. */
    CHECK(!parse_checked(NULL, 0, "timer", 1024, m, err, sizeof err), "NULL text parsed");
    CHECK(strcmp(err, "manifest: unexpected end of input at byte 1") == 0, "got '%s'", err);

    /* No folder: nothing matches, so the id message names no folder. */
    CHECK(!parse_checked(LIT("{\"id\":\"timer\",\"name\":\"Timer\",\"api\":1}"), NULL, 1024, m, err, sizeof err), "a NULL folder matched");
    CHECK(strcmp(err, "manifest: id 'timer' must match ^[a-z][a-z0-9_]{1,23}$ and the folder name ''") == 0, "got '%s'", err);

    /* err NULL, and err of no room, change nothing about the verdict and are never written. */
    CHECK(parse_checked(LIT("{\"id\":\"timer\",\"name\":\"Timer\",\"api\":1}"), "timer", 1024, m, NULL, 0), "no err: refused");
    apps_manifest_clear(m, count_free);
    CHECK(!parse_checked(LIT("{\"id\":1}"), "timer", 1024, m, NULL, 0), "no err: accepted");
    CHECK(!parse_checked(LIT("{\"id\":1}"), "timer", 1024, m, err, 0), "err_len 0: accepted");
    CHECK(!parse_checked(LIT("{\"id\":1}"), "timer", 1024, m, err, 1) && err[0] == '\0', "err_len 1: '%s'", err);
    static const char not_an_app[] = "{\"id\":1}";
    CHECK(!apps_manifest_parse(exact_copy(not_an_app, sizeof not_an_app - 1), sizeof not_an_app - 1, "timer", 1024, m, NULL, 64) &&
              is_zero(m),
          "err NULL with an err_len");
    CHECK(!apps_image_check(NULL, 0, NULL, NULL, NULL, 64), "err NULL with an err_len, for an image");

    /* err is cut: the message to err_len - 1 bytes, and on a character boundary. */
    static const char bad_id[] = "{\"id\":\"Timer\",\"name\":\"Timer\",\"api\":1}";
    CHECK(!parse_checked(LIT(bad_id), "timer", 1024, m, err, sizeof err), "bad id parsed");
    char full[512];
    snprintf(full, sizeof full, "%s", err);
    for (size_t cap = 1; cap <= strlen(full) + 3; cap++) {
        char cut[512];
        CHECK(!parse_checked(LIT(bad_id), "timer", 1024, m, cut, cap), "cut %zu: parsed", cap);
        size_t want = cap - 1 < strlen(full) ? cap - 1 : strlen(full);
        CHECK(strlen(cut) == want && memcmp(cut, full, want) == 0, "cap %zu: '%s'", cap, cut);
    }
    /* The id has a two-byte character at bytes 14 and 15 of the message: a cut after 15 keeps 14. */
    static const char accented[] = "{\"id\":\"\xc3\xa9\",\"name\":\"Timer\",\"api\":1}";
    CHECK(!parse_checked(LIT(accented), "timer", 1024, m, err, 16), "accented id parsed");
    CHECK(strcmp(err, "manifest: id '") == 0, "cut in a character: '%s'", err);
    CHECK(!parse_checked(LIT(accented), "timer", 1024, m, err, 17), "accented id parsed");
    CHECK(strcmp(err, "manifest: id '\xc3\xa9") == 0, "cut after a character: '%s'", err);

    /* A parse overwrites a manifest it is given, filled or not, and clear and free take NULL. */
    CHECK(parse_checked(LIT(minimal_manifest), "timer", 1024, m, err, sizeof err), "%s", err);
    apps_manifest_clear(NULL, count_free);
    apps_pkg_free(NULL, count_free);

    /* apps_file_max: the one table of the kinds' limits, the console's stage and the install pipeline both read it
     * (Task 13's F9). */
    CHECK(apps_file_max(APPS_FILE_LUA) == APPS_LUA_MAX && apps_file_max(APPS_FILE_JSON) == APPS_JSON_FILE_MAX &&
              apps_file_max(APPS_FILE_IMAGE) == APPS_IMAGE_MAX && apps_file_max(APPS_FILE_BAD) == 0,
          "apps_file_max");
    /* apps_text_shown: text from outside as a message shows it (Task 13's F7 and F9). */
    char out[80];
    static const char mixed[] = "a\x01\x7f\xff\xc3\xa9\xc2\x9b\xc2\xa0z";
    CHECK(strcmp(apps_text_shown(mixed, sizeof mixed - 1, 64, out), "a???\xc3\xa9?\xc2\xa0z") == 0, "mapped: '%s'", out);
    CHECK(strcmp(apps_text_shown("abcdef", 6, 4, out), "abcd...") == 0, "cut: '%s'", out);
    CHECK(strcmp(apps_text_shown("ab\xc3\xa9", 4, 3, out), "ab...") == 0, "cut between characters: '%s'", out);
    CHECK(strcmp(apps_text_shown(NULL, 0, 64, out), "") == 0 && apps_text_shown("x", 1, 64, out) == out, "NULL, and out");

    /* Images: NULL data, NULL out parameters, a cut error. */
    uint16_t w = 7, h = 7;
    CHECK(!apps_image_check(NULL, 0, &w, &h, err, sizeof err) && w == 0 && h == 0, "NULL image");
    CHECK(strcmp(err, "image: not an LVGL v9 image file (use LVGLImage.py --ofmt BIN)") == 0, "got '%s'", err);
    char small[8];
    CHECK(!apps_image_check(NULL, 0, NULL, NULL, small, sizeof small) && strcmp(small, "image: ") == 0, "cut '%s'", small);
    free(m);
}

/* messages_json is a block of PSRAM on the device, owned by the manifest, and nothing leaks. */
static void memory(void) {
    apps_manifest_t *m = malloc(sizeof *m);
    char err[256];

    /* One allocation, PSRAM and 8-bit capable, when there are messages; none when there aren't. */
    reset_heap();
    CHECK(parse_checked(LIT(timer_manifest), "timer", 1024, m, err, sizeof err), "%s", err);
    CHECK(calls == 1, "%u allocations", calls);
    CHECK(last_caps == (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT), "asked for caps 0x%x, wanted PSRAM and 8-bit", last_caps);
    CHECK(m->messages_json != NULL && held >= strlen(m->messages_json) + 1, "held %zu", held);
    apps_manifest_clear(m, count_free);
    CHECK(held == 0 && m->messages_json == NULL, "held %zu after clear", held);
    apps_manifest_clear(m, count_free);  /* again: nothing to free */
    CHECK(held == 0, "held %zu after a second clear", held);

    reset_heap();
    CHECK(parse_checked(LIT(minimal_manifest), "timer", 1024, m, err, sizeof err), "%s", err);
    CHECK(calls == 0 && m->messages_json == NULL, "%u allocations for a manifest without messages", calls);
    apps_manifest_clear(m, count_free);

    /* Nothing is allocated for a manifest that is refused: it needs the allocation last. */
    reset_heap();
    static const char refused[] = "{\"id\":\"timer\",\"name\":\"Timer\",\"api\":2,\"messages\":{\"a\":\"b\"}}";
    CHECK(!parse_checked(LIT(refused), "timer", 1024, m, err, sizeof err), "api 2 parsed");
    CHECK(calls == 0 && held == 0, "%u allocations (held %zu) for a manifest that fails", calls, held);

    /* The allocation refused: its own message, the manifest zero, nothing held. */
    reset_heap();
    refuse = true;
    CHECK(!parse_checked(LIT(timer_manifest), "timer", 1024, m, err, sizeof err), "parsed with no memory");
    CHECK(strcmp(err, "manifest: out of memory") == 0, "got '%s'", err);
    CHECK(calls == 1 && held == 0 && m->messages_json == NULL, "%u allocations, held %zu", calls, held);
    reset_heap();

    /* A free_fn of NULL is free. The block is plain here, and the free is ASan's to see. */
    plain_blocks = true;
    CHECK(parse_checked(LIT(timer_manifest), "timer", 1024, m, err, sizeof err), "%s", err);
    char *block = m->messages_json;
    CHECK(block != NULL, "no messages_json");
    apps_manifest_clear(m, NULL);
    CHECK(m->messages_json == NULL, "clear left messages_json");
#ifdef HAVE_ASAN
    CHECK(__asan_address_is_poisoned(block), "clear(NULL) didn't free the block");
#else
    (void)block;
#endif
    reset_heap();

    /* Many cycles, none leaks. */
    for (int i = 0; i < 200; i++) {
        CHECK(parse_checked(LIT(timer_manifest), "timer", 1024, m, err, sizeof err), "%s", err);
        apps_manifest_clear(m, count_free);
    }
    CHECK(held == 0, "held %zu after 200 cycles", held);
    free(m);
}

/* The lines of ctl.lua around line 2, as the message shows them. */
#define CTL_LINES "\n 1| -- ?]0;owned? ?2J \xc2\xa0\xc3\xa9\n>2| local b = = 2\n 3| \treturn ?{}?"

/* Whether s[0, n) is text for a terminal and for JSON: valid UTF-8, with no C0 byte but "\n" and tab, no DEL and no C1
 * character (U+0080 to U+009F). */
static bool is_clean_n(const char *s, size_t n) {
    const unsigned char *p = (const unsigned char *)s, *end = p + n;
    while (p < end) {
        if (*p < 0x80) {
            if ((*p < 0x20 && *p != '\n' && *p != '\t') || *p == 0x7F) {
                return false;
            }
            p++;
            continue;
        }
        int k = apps_json_utf8_len(p, end);
        if (k <= 0 || (k == 2 && p[0] == 0xC2 && p[1] <= 0x9F)) {
            return false;
        }
        p += k;
    }
    return true;
}

static bool is_clean(const char *s) {
    return is_clean_n(s, strlen(s));
}

/* apps_pkg_compile_message: "compile failed: ", Lua's message, and the lines of the file it names around its line. The
 * install pipeline and apphost's check both word a refusal with it. */
static void compile_message(void) {
    static const char main_src[] = "local a = 1\nlocal b = = 2\nreturn {}\n";
    static const char util_src[] = "return {\n  x = 1,\n  y = ,\n}\n";
    /* Bytes a terminal takes for more than text: ESC and BEL, a C1 character (U+009B, CSI), DEL, broken UTF-8 and a tab
     * in the lines; U+00A0 and an accented letter are text. (The literal is split where a hex escape would eat a digit.) */
    static const char ctl_src[] = "-- \x1b]0;owned\x07 \xc2\x9b" "2J \xc2\xa0\xc3\xa9\nlocal b = = 2\n\treturn \x7f{}\xff\n";
    static const char nul_src[] = "x = 1 \0 y\nz = = 2\n";
    apps_pkg_t *pkg = calloc(1, sizeof *pkg);
    static const struct {
        const char *path;
        const char *src;  /* NULL: an empty file with no block */
        size_t len;
    } files[] = {
        {"main.lua", main_src, sizeof main_src - 1},
        {"lib/util.lua", util_src, sizeof util_src - 1},
        {"ctl.lua", ctl_src, sizeof ctl_src - 1},
        {"nul.lua", nul_src, sizeof nul_src - 1},
        {"empty.lua", NULL, 0},
    };
    for (size_t i = 0; i < sizeof files / sizeof files[0]; i++) {
        apps_blob_t *b = &pkg->files[pkg->count++];
        snprintf(b->path, sizeof b->path, "%s", files[i].path);
        b->data = files[i].src != NULL ? (uint8_t *)exact_copy(files[i].src, files[i].len) : NULL;
        b->len = files[i].len;
    }

    static const struct {
        const char *raw, *want;
    } rows[] = {
        /* The file is what is before the first ":<digits>:", and the lines around that line show, the line marked. */
        {"main.lua:2: unexpected symbol near '='",
         "compile failed: main.lua:2: unexpected symbol near '='\n 1| local a = 1\n>2| local b = = 2\n 3| return {}"},
        {"lib/util.lua:3: unexpected symbol near ','",
         "compile failed: lib/util.lua:3: unexpected symbol near ','\n 2|   x = 1,\n>3|   y = ,\n 4| }"},
        {"main.lua:1: boom", "compile failed: main.lua:1: boom\n>1| local a = 1\n 2| local b = = 2"},
        {"main.lua:3: at the end", "compile failed: main.lua:3: at the end\n 2| local b = = 2\n>3| return {}"},
        /* Whatever isn't a file of the package, or has no line, or a line the file lacks, is shown as it is. */
        {"other.lua:2: boom", "compile failed: other.lua:2: boom"},
        {"xmain.lua:2: boom", "compile failed: xmain.lua:2: boom"},
        {"main.lu:2: boom", "compile failed: main.lu:2: boom"},
        {"main.lua.bak:2: boom", "compile failed: main.lua.bak:2: boom"},
        {"util.lua:3: boom", "compile failed: util.lua:3: boom"},
        {"C stack overflow", "compile failed: C stack overflow"},
        {"out of memory: this app may use 64 KB (memory_kb in manifest.json)",
         "compile failed: out of memory: this app may use 64 KB (memory_kb in manifest.json)"},
        {"main.lua:99: past the end", "compile failed: main.lua:99: past the end"},
        {"main.lua:0: no line", "compile failed: main.lua:0: no line"},
        {"empty.lua:1: unexpected symbol near <eof>", "compile failed: empty.lua:1: unexpected symbol near <eof>"},
        {"", "compile failed: "},
        /* Only the first ":<digits>:" can name the file: a later one in the message doesn't. */
        {"x 1: main.lua:2: y", "compile failed: x 1: main.lua:2: y"},
        {"near 3:4: main.lua:2: y", "compile failed: near 3:4: main.lua:2: y"},
        /* A colon that isn't followed by digits and a colon ends no file name, though a later line number shows. */
        {"main.lua: x:2: boom", "compile failed: main.lua: x:2: boom"},
        {"main.lua:: x:2: boom", "compile failed: main.lua:: x:2: boom"},
        /* What a terminal could take for more than text becomes '?' in Lua's message and in the lines of the file: each
         * C0 byte but the excerpt's own newlines and tab, DEL, each C1 character and each byte of broken UTF-8 (a
         * message is one line, though a token can hold a newline). Valid UTF-8 stays, U+00A0 and an accented letter. */
        {"ctl.lua:2: unexpected symbol near '='", "compile failed: ctl.lua:2: unexpected symbol near '='" CTL_LINES},
        {"ctl.lua:2: unfinished string near ''\x01\x1b[2J\xff'",
         "compile failed: ctl.lua:2: unfinished string near ''??[2J?'" CTL_LINES},
        {"ctl.lua:2: bad \xc2\x9b token\xc2\x85 \xc2\xa0 \xc3\xa9\x7f\xe2\x82",
         "compile failed: ctl.lua:2: bad ? token? \xc2\xa0 \xc3\xa9???" CTL_LINES},
        {"ctl.lua:2: hexadecimal digit expected near ''\\x\n'",
         "compile failed: ctl.lua:2: hexadecimal digit expected near ''\\x?'" CTL_LINES},
        {"ctl.lua:2: a\rb\tc\x7f" "d", "compile failed: ctl.lua:2: a?b\tc?d" CTL_LINES},
        {"\x1b[2J C stack overflow\xff", "compile failed: ?[2J C stack overflow?"},
        {"other.lua:2: \x1b]0;x\x07", "compile failed: other.lua:2: ?]0;x?"},
        /* A NUL in a line is a byte like the others: the lines after it show. */
        {"nul.lua:2: unexpected symbol near '='",
         "compile failed: nul.lua:2: unexpected symbol near '='\n 1| x = 1 ? y\n>2| z = = 2"},
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        char out[512];
        memset(out, 'x', sizeof out);
        apps_pkg_compile_message(pkg, exact_copy(rows[i].raw, strlen(rows[i].raw) + 1), out, sizeof out);
        CHECK(strcmp(out, rows[i].want) == 0, "row %zu '%s':\n  got  '%s'\n  want '%s'", i, rows[i].raw, out, rows[i].want);
    }

    /* The result is cut at cap - 1 bytes and terminated, whatever cap is; nothing is written past it (the buffer is an
     * exact block), and a cut keeps a character whole. */
    static const char raw[] = "lib/util.lua:3: caf\xc3\xa9 near ','";
    static const char head[] = "compile failed: lib/util.lua:3: caf\xc3\xa9";
    char full[512];
    apps_pkg_compile_message(pkg, exact_copy(raw, sizeof raw), full, sizeof full);
    CHECK(strncmp(full, head, sizeof head - 1) == 0 && strstr(full, "\n>3|   y = ,") != NULL, "full: '%s'", full);
    for (size_t cap = 1; cap <= strlen(full) + 2; cap++) {
        char *out = exact_copy(NULL, cap);
        apps_pkg_compile_message(pkg, exact_copy(raw, sizeof raw), out, cap);
        size_t want = cap - 1 < strlen(full) ? cap - 1 : strlen(full);
        while (want > 0 && want < strlen(full) && ((unsigned char)full[want] & 0xC0) == 0x80) {
            want--;  /* a cut inside a character goes back to where it starts */
        }
        CHECK(strlen(out) == want && memcmp(out, full, want) == 0, "cap %zu: '%s', wanted %zu bytes", cap, out, want);
    }
    /* Bytes to clean don't change that: whatever the cap, the result is text, ends at cap - 1 bytes at most, and is
     * the start of the whole one. A C1 character takes two bytes of the message and one '?' of the result. */
    static const char *const dirty[] = {
        "ctl.lua:2: unfinished string near ''\x01\x1b[2J\xff'",
        "ctl.lua:2: bad \xc2\x9b\xc2\x9b token \xe2\x82",
        "\xc2\x9b\x1b[2J ctl.lua:2: x\n",
    };
    for (size_t d = 0; d < sizeof dirty / sizeof dirty[0]; d++) {
        char whole[512];
        apps_pkg_compile_message(pkg, exact_copy(dirty[d], strlen(dirty[d]) + 1), whole, sizeof whole);
        CHECK(is_clean(whole) && strlen(whole) > 20, "dirty %zu: '%s'", d, whole);
        for (size_t cap = 1; cap <= strlen(whole) + 8; cap++) {
            char *out = exact_copy(NULL, cap);
            apps_pkg_compile_message(pkg, exact_copy(dirty[d], strlen(dirty[d]) + 1), out, cap);
            CHECK(strlen(out) < cap && is_clean(out) && strncmp(out, whole, strlen(out)) == 0,
                  "dirty %zu, cap %zu: '%s' (whole '%s')", d, cap, out, whole);
        }
    }
    apps_pkg_compile_message(pkg, raw, NULL, 10);  /* nowhere to write, or no room: no write */
    char guard[4] = "abc";
    apps_pkg_compile_message(pkg, raw, guard, 0);
    CHECK(strcmp(guard, "abc") == 0, "cap 0 wrote '%s'", guard);
    free(pkg);
}

/* apps_pkg_compile_order: main first, then the package's other .lua files as it has them, and nothing that isn't Lua; the one
 * order the app task's compile and apphost's check use (fix round 1, E). */
static void compile_order(void) {
    static const struct {
        const char *main;
        const char *paths[8];  /* NULL ends the list */
        const char *want;      /* the order, the paths joined with a space */
    } rows[] = {
        {"main.lua", {"main.lua", "a.lua", "b.lua"}, "main.lua a.lua b.lua"},
        {"main.lua", {"a.lua", "b.lua", "main.lua"}, "main.lua a.lua b.lua"},
        {"main.lua", {"a.lua", "main.lua", "b.lua"}, "main.lua a.lua b.lua"},
        {"main.lua", {"b.lua", "a.lua", "main.lua"}, "main.lua b.lua a.lua"},
        {"main.lua", {"d.json", "a.lua", "i.bin", "main.lua", "lib/x.lua"}, "main.lua a.lua lib/x.lua"},
        {"app.lua", {"main.lua", "app.lua"}, "app.lua main.lua"},
        /* A file is main by its whole path. */
        {"main.lua", {"xmain.lua", "main.lua.bak", "lib/main.lua", "main.lua"}, "main.lua xmain.lua lib/main.lua"},
        /* No main among the .lua files: the others alone, in the package's order. */
        {"main.lua", {"b.lua", "a.lua"}, "b.lua a.lua"},
        {"main.json", {"a.lua", "main.json"}, "a.lua"},
        {"main.lua", {"d.json", "i.bin"}, ""},
        {"main.lua", {NULL}, ""},
    };
    for (size_t r = 0; r < sizeof rows / sizeof rows[0]; r++) {
        apps_pkg_t *pkg = calloc(1, sizeof *pkg);
        snprintf(pkg->manifest.main, sizeof pkg->manifest.main, "%s", rows[r].main);
        for (int i = 0; i < 8 && rows[r].paths[i] != NULL; i++) {
            snprintf(pkg->files[pkg->count++].path, sizeof pkg->files[0].path, "%s", rows[r].paths[i]);
        }
        const apps_blob_t **order = (const apps_blob_t **)exact_copy(NULL, APPS_MAX_FILES * sizeof *order);
        int n = apps_pkg_compile_order(pkg, order);
        char got[256] = "";
        for (int i = 0; i < n; i++) {
            snprintf(got + strlen(got), sizeof got - strlen(got), "%s%s", i > 0 ? " " : "", order[i]->path);
        }
        CHECK(strcmp(got, rows[r].want) == 0, "row %zu: '%s' instead of '%s'", r, got, rows[r].want);
        free(pkg);
    }
    /* 32 files, main the last: all of them, main first, and not a pointer past the room for 32. */
    apps_pkg_t *pkg = calloc(1, sizeof *pkg);
    snprintf(pkg->manifest.main, sizeof pkg->manifest.main, "main.lua");
    for (int i = 0; i < APPS_MAX_FILES - 1; i++) {
        snprintf(pkg->files[pkg->count++].path, sizeof pkg->files[0].path, "m%02d.lua", i);
    }
    snprintf(pkg->files[pkg->count++].path, sizeof pkg->files[0].path, "main.lua");
    const apps_blob_t **order = (const apps_blob_t **)exact_copy(NULL, APPS_MAX_FILES * sizeof *order);
    int n = apps_pkg_compile_order(pkg, order);
    CHECK(n == APPS_MAX_FILES && strcmp(order[0]->path, "main.lua") == 0 && strcmp(order[1]->path, "m00.lua") == 0 &&
              strcmp(order[APPS_MAX_FILES - 1]->path, "m30.lua") == 0,
          "32 files: %d, first '%s', last '%s'", n, order[0]->path, order[n > 0 ? n - 1 : 0]->path);
    /* The pointers are into the package: the same blobs, not copies. */
    CHECK(order[0] == &pkg->files[APPS_MAX_FILES - 1] && order[1] == &pkg->files[0], "the blobs of the package");
    free(pkg);
}

/* apps_pkg_check_source: a .lua file that starts with Lua's signature is a compiled chunk, in the install's words; nothing
 * else is refused, and what is read is read in len bytes (fix round 1, F). */
static void check_source(void) {
    static const struct {
        int index;
        const char *path;
        const char *data;
        size_t len;
        const char *want;  /* NULL: accepted */
    } rows[] = {
        {0, "main.lua", LIT("\x1b" "LuaT\x00\x19\x93"), "files[0] main.lua is a compiled chunk: install Lua source text"},
        {3, "lib/util.lua", LIT("\x1b" "Lua"), "files[3] lib/util.lua is a compiled chunk: install Lua source text"},
        {31, "z.lua", LIT("\x1b" "Lua" "x"), "files[31] z.lua is a compiled chunk: install Lua source text"},
        /* Three bytes of the signature, or another signature, or a later one: not a chunk (the compile has its own say). */
        {0, "main.lua", LIT("\x1b" "Lu"), NULL},
        {0, "main.lua", LIT("\x1b" "LuA"), NULL},
        {0, "main.lua", LIT(" \x1b" "Lua"), NULL},
        {0, "main.lua", LIT("return {}"), NULL},
        {0, "main.lua", "", 0, NULL},
        /* Only a .lua file is a chunk. */
        {1, "d.json", LIT("\x1b" "Lua"), NULL},
        {1, "i.bin", LIT("\x1b" "Lua"), NULL},
        {1, "lua", LIT("\x1b" "Lua"), NULL},
        {1, "x.LUA", LIT("\x1b" "Lua"), NULL},
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        char err[160];
        snprintf(err, sizeof err, "stale");
        const uint8_t *data = (const uint8_t *)exact_copy(rows[i].data, rows[i].len);
        bool ok = apps_pkg_check_source(rows[i].index, rows[i].path, data, rows[i].len, err, sizeof err);
        CHECK(ok == (rows[i].want == NULL) && strcmp(err, rows[i].want != NULL ? rows[i].want : "") == 0, "row %zu: %d '%s'", i,
              ok, err);
    }
    /* The message is cut where every one is (a block of exactly err_len bytes), and err may be NULL. */
    static const char want[] = "files[2] main.lua is a compiled chunk: install Lua source text";
    for (size_t cap = 1; cap <= sizeof want + 1; cap++) {
        char *out = exact_copy(NULL, cap);
        CHECK(!apps_pkg_check_source(2, "main.lua", (const uint8_t *)exact_copy("\x1b" "Lua", 4), 4, out, cap),
              "cap %zu: accepted", cap);
        size_t n = cap - 1 < sizeof want - 1 ? cap - 1 : sizeof want - 1;
        CHECK(strlen(out) == n && memcmp(out, want, n) == 0, "cap %zu: '%s'", cap, out);
    }
    CHECK(!apps_pkg_check_source(2, "main.lua", (const uint8_t *)exact_copy("\x1b" "Lua", 4), 4, NULL, 0), "NULL err");
    CHECK(apps_pkg_check_source(2, "main.lua", NULL, 0, NULL, 0), "no data, no err");
}

/* apps_text_clean and apps_text_next: the one rule for text that leaves the runtime and for the compile message (fix round
 * 1, A). */
static void text_clean(void) {
    static const struct {
        const char *in;
        size_t len;
        bool newline;
        const char *want;
        size_t want_len;
    } rows[] = {
        {LIT("plain text, caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80"), false, "plain text, caf\xc3\xa9 \xe2\x82\xac \xf0\x9f\x98\x80", 0},
        /* C0: every byte but tab, and "\n" when it is kept; DEL */
        {LIT("a\x01\x1b[2J\x07z\x7f"), false, "a??[2J?z?", 0},
        {LIT("a\tb\nc\rd"), false, "a\tb?c?d", 0},
        {LIT("a\tb\nc\rd"), true, "a\tb\nc?d", 0},
        /* C1: the character, one '?' for its two bytes; U+00A0 and U+00A1 are text */
        {LIT("x\xc2\x80y\xc2\x9bz\xc2\x9f"), true, "x?y?z?", 0},
        {LIT("\xc2\xa0\xc2\xa1\xc2\xbf\xc3\x80"), true, "\xc2\xa0\xc2\xa1\xc2\xbf\xc3\x80", 0},
        /* broken UTF-8: each byte; a character cut short by the end too */
        {LIT("a\xff\xfe\x80" "b"), true, "a???b", 0},
        {LIT("a\xc3(b"), true, "a?(b", 0},
        {LIT("a\xe2\x82"), true, "a??", 0},
        {LIT("a\xc2"), true, "a?", 0},
        {LIT("\xed\xa0\x80\xc0\x80\xf4\x90\x80\x80"), true, "?????????", 0},  /* a surrogate, an overlong NUL, past U+10FFFF */
        /* a NUL inside the range is a byte like the others */
        {"a\0b\0", 4, true, "a?b?", 0},
        {"", 0, true, "", 0},
    };
    for (size_t i = 0; i < sizeof rows / sizeof rows[0]; i++) {
        size_t want_len = strlen(rows[i].want);
        char *text = exact_copy(rows[i].in, rows[i].len);
        size_t got = apps_text_clean(text, rows[i].len, rows[i].newline);
        CHECK(got == want_len && memcmp(text, rows[i].want, want_len) == 0, "row %zu: %zu bytes '%.*s', wanted %zu '%s'", i, got,
              (int)got, text, want_len, rows[i].want);
        CHECK(got <= rows[i].len, "row %zu: %zu bytes from %zu", i, got, rows[i].len);
        /* A text that shrank ends with a NUL (and one that didn't has none written: the block is exactly len bytes). */
        if (got < rows[i].len) {
            CHECK(text[got] == '\0', "row %zu: no NUL at %zu", i, got);
        }
        CHECK(is_clean_n(text, got), "row %zu: still not text", i);
    }
    /* Every byte, alone and between two letters: the result is text, and never longer. */
    for (int b = 0; b < 256; b++) {
        for (int newline = 0; newline < 2; newline++) {
            const char in[3] = {'a', (char)b, 'z'};
            char *text = exact_copy(in, 3);
            size_t got = apps_text_clean(text, 3, newline != 0);
            CHECK(got == 3 && text[0] == 'a' && text[2] == 'z', "byte %d: %zu bytes", b, got);
            bool kept = (b >= 0x20 && b < 0x7F) || b == '\t' || (b == '\n' && newline);
            CHECK(text[1] == (kept ? (char)b : '?'), "byte %d (newline %d) became %d", b, newline, text[1]);
        }
    }
    /* Every two-byte character: C1 is the one that goes, whatever follows. */
    for (int lead = 0xC2; lead < 0xE0; lead++) {
        for (int cont = 0x80; cont < 0xC0; cont++) {
            char in[4] = {'a', (char)lead, (char)cont, 'z'};
            char *text = exact_copy(in, 4);
            size_t got = apps_text_clean(text, 4, true);
            bool c1 = lead == 0xC2 && cont <= 0x9F;
            CHECK(got == (c1 ? 3u : 4u) && text[0] == 'a' && text[got - 1] == 'z', "U+%04x: %zu bytes", (lead & 0x1F) << 6 | (cont & 0x3F), got);
            CHECK(c1 ? text[1] == '?' : (text[1] == (char)lead && text[2] == (char)cont), "U+%04x: kept wrongly", (lead & 0x1F) << 6 | (cont & 0x3F));
        }
    }
    /* apps_text_next: the bytes a character takes, and whether it stays. */
    static const unsigned char sample[] = "\xc2\x9b\xc2\xa0\xe2\x82\xac\xff\x01\n";
    static const struct { size_t take; bool keep_plain, keep_newline; } steps[] = {
        {2, false, false}, {2, true, true}, {3, true, true}, {1, false, false}, {1, false, false}, {1, false, true},
    };
    const unsigned char *at = sample, *end = sample + sizeof sample - 1;
    for (size_t i = 0; i < sizeof steps / sizeof steps[0]; i++) {
        bool plain, newline;
        size_t a = apps_text_next(at, end, false, &plain), b = apps_text_next(at, end, true, &newline);
        CHECK(a == steps[i].take && b == steps[i].take && plain == steps[i].keep_plain && newline == steps[i].keep_newline,
              "step %zu: %zu/%zu bytes, keep %d/%d", i, a, b, plain, newline);
        at += a;
    }
    CHECK(at == end, "the steps end at the end");
}

/* apps_pkg_free frees every blob and the manifest's messages, and leaves a zero package. */
static void pkg_free(void) {
    apps_pkg_t *pkg = calloc(1, sizeof *pkg);
    char err[256];
    reset_heap();
    CHECK(apps_manifest_parse(exact_copy(LIT(timer_manifest)), sizeof timer_manifest - 1, "timer", 1024, &pkg->manifest, err, sizeof err),
          "%s", err);
    CHECK(pkg->manifest.messages_json != NULL, "no messages_json");
    for (int i = 0; i < 3; i++) {
        snprintf(pkg->files[i].path, sizeof pkg->files[i].path, "f%d.lua", i);
        pkg->files[i].data = heap_caps_malloc(100 + (size_t)i, MALLOC_CAP_SPIRAM);
        pkg->files[i].len = 100 + (size_t)i;
    }
    pkg->count = 2;  /* the third is a blob whose count never went up */
    CHECK(held > 0, "nothing held");
    apps_pkg_free(pkg, count_free);
    CHECK(held == 0, "held %zu after apps_pkg_free", held);
    static const apps_pkg_t zero;
    CHECK(memcmp(pkg, &zero, sizeof zero) == 0, "the package isn't zero");
    apps_pkg_free(pkg, count_free);  /* again: nothing to free */
    CHECK(held == 0, "held %zu after a second free", held);

    /* An empty package, and a free_fn of NULL on one that holds nothing. */
    apps_pkg_free(pkg, NULL);
    free(pkg);
}

/* Every strict prefix of a manifest is refused, whatever it cuts: nothing reads past len. */
static void truncations(void) {
    apps_manifest_t *m = malloc(sizeof *m);
    char err[256];
    static const char *const docs[] = {timer_manifest, "{\"id\":\"timer\",\"name\":\"Timer\",\"api\":1,\"x\":[1,{\"y\":\"\\u00e9\\ud83d\\ude00\"}],\"messages\":{\"a\":\"\xc3\xa9\"}}"};
    for (size_t d = 0; d < sizeof docs / sizeof docs[0]; d++) {
        size_t len = strlen(docs[d]);
        CHECK(parse_checked(docs[d], len, "timer", 1024, m, err, sizeof err), "doc %zu: %s", d, err);
        apps_manifest_clear(m, count_free);
        for (size_t cut = 0; cut < len; cut++) {
            if (parse_checked(docs[d], cut, "timer", 1024, m, err, sizeof err)) {
                CHECK(false, "doc %zu cut at %zu bytes parsed", d, cut);
                apps_manifest_clear(m, count_free);
            }
            exact_release();
        }
    }
    CHECK(held == 0, "held %zu", held);
    free(m);
}

/* ---- The same checks over many generated manifests ---- */

static uint64_t rng_state = 0x9E3779B97F4A7C15ull;

static uint64_t rng(void) {
    rng_state ^= rng_state >> 12;
    rng_state ^= rng_state << 25;
    rng_state ^= rng_state >> 27;
    return rng_state * 0x2545F4914F6CDD1Dull;
}

static unsigned rnd(unsigned n) {
    return (unsigned)(rng() % n);
}

/* A value a key may be given, and whether the manifest takes it (for folder "timer", a heap of 1024 KB). */
typedef struct {
    const char *json;
    bool valid;
} pool_t;

typedef struct {
    const char *key;
    const pool_t *pool;
    size_t len;
    bool required;
} field_t;

#define POOL_SIZE(p) (sizeof(p) / sizeof((p)[0]))

static const pool_t pool_id[] = {
    {"\"timer\"", true}, {"\"tim\\u0065r\"", true},
    {"\"Timer\"", false}, {"\"timers\"", false}, {"\"\"", false}, {"5", false}, {"null", false}, {"[\"timer\"]", false},
    {"\"timer\\u0000\"", false}, {"{}", false}, {"true", false},
};

static const pool_t pool_name[] = {
    {"\"Timer\"", true}, {"\"x\"", true}, {"\"Caf\\u00e9\"", true}, {"\"012345678901234567890123\"", true},
    {"\"\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\"", true},
    {"\"\"", false}, {"\"0123456789012345678901234\"", false}, {"5", false}, {"null", false}, {"[\"x\"]", false},
    {"\"a\\u0000b\"", false}, {"{}", false},
    {"\"a\\nb\"", false}, {"\"A\\u009b2J\"", false}, {"\"x\\u007f\"", false}, {"\"\\u0085\"", false},  /* F7 */
    {"\"\\u00a0Timer\"", true},
    {"\"\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\\u00e9\"", false},
};

static const pool_t pool_version[] = {
    {"\"1.0.0\"", true}, {"\"\"", true}, {"\"01234567890123456789012\"", true},
    {"\"012345678901234567890123\"", false}, {"1", false}, {"null", false}, {"[]", false}, {"\"\\u0000\"", false},
    {"\"1\\t0\"", false}, {"\"\\u009b31m\"", false}, {"\"1.0\\u00a0\"", true},  /* F7 */
};

static const pool_t pool_api[] = {
    {"1", true},
    {"2", false}, {"0", false}, {"-1", false}, {"\"1\"", false}, {"1.0", false}, {"1e0", false}, {"true", false},
    {"null", false}, {"[1]", false}, {"99999999999999999999", false}, {"{}", false},
};

static const pool_t pool_main[] = {
    {"\"main.lua\"", true}, {"\"lib/m.lua\"", true}, {"\"m\\u0061in.lua\"", true},
    {"\"main.json\"", false}, {"\"m.bin\"", false}, {"\"../a.lua\"", false}, {"\"\"", false}, {"5", false},
    {"null", false}, {"\"A.lua\"", false}, {"\"a//b.lua\"", false}, {"\"a.lua.\"", false}, {"\".a.lua\"", false},
    {"\"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa.lua\"", false}, {"[\"main.lua\"]", false},
};

static const pool_t pool_perms[] = {
    {"[]", true}, {"[\"sound\"]", true}, {"[\"sound\",\"alarm\",\"store\",\"keep_awake\"]", true}, {"[\"sound\",\"sound\"]", true},
    {"[\"net\"]", false}, {"[\"secrets\"]", false}, {"[\"muse\"]", false}, {"[\"bogus\"]", false}, {"\"sound\"", false},
    {"[7]", false}, {"{}", false}, {"[\"sound\",\"net\"]", false}, {"[null]", false}, {"null", false},
};

static const pool_t pool_memory[] = {
    {"64", true}, {"256", true}, {"1024", true},
    {"63", false}, {"1025", false}, {"\"128\"", false}, {"128.5", false}, {"null", false}, {"true", false},
    {"-1", false}, {"1e2", false}, {"0", false}, {"99999999999999999999", false}, {"[]", false},
};

/* files and messages have members that are built: a list of 31 and of 32 files (main makes it 32 and 33),
 * and maps of 16 and 17 events, of descriptions and of names under 200 bytes and of 200. */
static char list_31[512], list_32[512], map_16[512], map_17[512], desc_199[512], desc_200[512], key_199[512], key_200[512];

static pool_t pool_files[16], pool_messages[24];
static field_t fields[9];

static const pool_t pool_files_fixed[] = {
    {"[]", true}, {"[\"util.lua\"]", true}, {"[\"a.json\",\"assets/x.bin\",\"main.lua\",\"main.lua\",\"a.json\"]", true},
    {"{}", false}, {"\"util.lua\"", false}, {"[\"../x.lua\"]", false}, {"[\"x.txt\"]", false}, {"[5]", false},
    {"[null]", false}, {"[\"a.lua\",true]", false}, {"[[\"a.lua\"]]", false}, {"[\"a.lua\",\"a//b.lua\"]", false},
    {"null", false},
};

static const pool_t pool_messages_fixed[] = {
    {"{}", true}, {"{\"start\":\"go\"}", true}, {"{ \"a\" : \"b\" , \"c\" : \"d\" }", true}, {"{\"a\":\"\\u00e9\"}", true},
    {"[]", false}, {"{\"a\":1}", false}, {"{\"a\":null}", false}, {"\"x\"", false}, {"{\"a\":{\"b\":\"c\"}}", false},
    {"null", false}, {"{\"a\":\"b\",\"c\":[]}", false},
};

/* A list of n distinct .lua files, as JSON. */
static void make_list(char *out, size_t cap, int n) {
    size_t k = (size_t)snprintf(out, cap, "[");
    for (int i = 0; i < n; i++) {
        k += (size_t)snprintf(out + k, cap - k, "%s\"f%02d.lua\"", i > 0 ? "," : "", i);
    }
    snprintf(out + k, cap - k, "]");
}

/* A map of n events with a one-letter description. */
static void make_map(char *out, size_t cap, int n) {
    size_t k = (size_t)snprintf(out, cap, "{");
    for (int i = 0; i < n; i++) {
        k += (size_t)snprintf(out + k, cap - k, "%s\"e%02d\":\"d\"", i > 0 ? "," : "", i);
    }
    snprintf(out + k, cap - k, "}");
}

/* {"a":"<n x>"}, or {"<n x>":"d"} for as_key. */
static void make_text(char *out, size_t cap, int n, bool as_key) {
    char xs[256];
    memset(xs, 'x', (size_t)n);
    xs[n] = '\0';
    if (as_key) {
        snprintf(out, cap, "{\"%s\":\"d\"}", xs);
    } else {
        snprintf(out, cap, "{\"a\":\"%s\"}", xs);
    }
}

static void build_fields(void) {
    make_list(list_31, sizeof list_31, 31);
    make_list(list_32, sizeof list_32, 32);
    make_map(map_16, sizeof map_16, 16);
    make_map(map_17, sizeof map_17, 17);
    make_text(desc_199, sizeof desc_199, 199, false);
    make_text(desc_200, sizeof desc_200, 200, false);
    make_text(key_199, sizeof key_199, 199, true);
    make_text(key_200, sizeof key_200, 200, true);

    size_t n = POOL_SIZE(pool_files_fixed);
    memcpy(pool_files, pool_files_fixed, sizeof pool_files_fixed);
    pool_files[n++] = (pool_t){list_31, true};
    pool_files[n++] = (pool_t){list_32, false};
    size_t files_len = n;

    n = POOL_SIZE(pool_messages_fixed);
    memcpy(pool_messages, pool_messages_fixed, sizeof pool_messages_fixed);
    pool_messages[n++] = (pool_t){map_16, true};
    pool_messages[n++] = (pool_t){map_17, false};
    pool_messages[n++] = (pool_t){desc_199, true};
    pool_messages[n++] = (pool_t){desc_200, false};
    pool_messages[n++] = (pool_t){key_199, true};
    pool_messages[n++] = (pool_t){key_200, false};
    size_t messages_len = n;

    const field_t list[] = {
        {"id", pool_id, POOL_SIZE(pool_id), true},
        {"name", pool_name, POOL_SIZE(pool_name), true},
        {"version", pool_version, POOL_SIZE(pool_version), false},
        {"api", pool_api, POOL_SIZE(pool_api), true},
        {"main", pool_main, POOL_SIZE(pool_main), false},
        {"files", pool_files, files_len, false},
        {"permissions", pool_perms, POOL_SIZE(pool_perms), false},
        {"memory_kb", pool_memory, POOL_SIZE(pool_memory), false},
        {"messages", pool_messages, messages_len, false},
    };
    memcpy(fields, list, sizeof list);
}

/* A value of f's that is valid, or one that isn't. */
static const pool_t *pick(const field_t *f, bool valid) {
    for (;;) {
        const pool_t *p = &f->pool[rnd((unsigned)f->len)];
        if (p->valid == valid) {
            return p;
        }
    }
}

typedef struct {
    const char *key, *json;
    bool second;  /* the key's later value, which is the one that counts */
} member_t;

/* Appends s to out at *k. */
static void put(char *out, size_t cap, size_t *k, const char *s) {
    size_t n = strlen(s);
    if (*k + n + 1 > cap) {
        fputs("generated manifest too long\n", stderr);
        exit(2);
    }
    memcpy(out + *k, s, n + 1);
    *k += n;
}

static void put_space(char *out, size_t cap, size_t *k) {
    static const char *const gap[] = {"", "", " ", "\n  ", "\t"};
    put(out, cap, k, gap[rnd(5)]);
}

/*
 * A random manifest in out: each key absent, given, or given twice, with a value that is mostly valid
 * (seven in eight); unknown keys; whitespace; any order. Whether the manifest takes it follows from its
 * parts: the last value of each key is a valid one, and id, name and api are there. *valid says so.
 * Returns its length.
 */
static size_t make_manifest(char *out, size_t cap, bool *valid) {
    enum { FIELDS = sizeof fields / sizeof fields[0], EXTRA = 3 };
    member_t members[2 * FIELDS + EXTRA];
    size_t count = 0;
    *valid = true;
    for (size_t f = 0; f < FIELDS; f++) {
        if (rnd(16) == 0) {  /* absent */
            if (fields[f].required) {
                *valid = false;
            }
            continue;
        }
        if (rnd(10) == 0) {  /* given twice: the second counts */
            members[count++] = (member_t){fields[f].key, pick(&fields[f], rnd(2) == 0)->json, false};
        }
        const pool_t *last = pick(&fields[f], rnd(8) != 0);
        members[count++] = (member_t){fields[f].key, last->json, true};
        if (!last->valid) {
            *valid = false;
        }
    }
    static const char *const unknown_keys[] = {"x", "colour", "entry", "permission", "file", "Name", "zz", "i\\u0064s",
                                               "\\u009bk", "q\\nz"};
    static const char *const unknown_values[] = {"1", "\"s\"", "[1,{\"a\":null}]", "{\"k\":[]}", "null", "\"\\u00e9\""};
    for (unsigned i = rnd(EXTRA + 1); i > 0; i--) {
        members[count++] = (member_t){unknown_keys[rnd(sizeof unknown_keys / sizeof unknown_keys[0])],
                                      unknown_values[rnd(sizeof unknown_values / sizeof unknown_values[0])], true};
    }
    /* Any order, then each key's two values put back in theirs. */
    for (size_t i = count; i > 1; i--) {
        size_t j = rnd((unsigned)i);
        member_t t = members[i - 1];
        members[i - 1] = members[j];
        members[j] = t;
    }
    for (size_t i = 0; i < count; i++) {
        for (size_t j = i + 1; j < count; j++) {
            if (strcmp(members[i].key, members[j].key) == 0 && members[i].second && !members[j].second) {
                member_t t = members[i];
                members[i] = members[j];
                members[j] = t;
            }
        }
    }
    size_t k = 0;
    put(out, cap, &k, "{");
    put_space(out, cap, &k);
    for (size_t i = 0; i < count; i++) {
        if (i > 0) {
            put(out, cap, &k, ",");
            put_space(out, cap, &k);
        }
        put(out, cap, &k, "\"");
        put(out, cap, &k, members[i].key);
        put(out, cap, &k, "\"");
        put_space(out, cap, &k);
        put(out, cap, &k, ":");
        put_space(out, cap, &k);
        put(out, cap, &k, members[i].json);
    }
    put_space(out, cap, &k);
    put(out, cap, &k, "}");
    return k;
}

/* The verdict on a generated manifest is its parts': nothing invalid gets in, however it is combined with
 * what is valid, and nothing valid is turned away by what is beside it. The unknown key "i\u0064s" is the
 * key "ids" (it decodes), not "id". */
static void combos(void) {
    build_fields();
    apps_manifest_t *m = malloc(sizeof *m);
    char err[512];
    static char doc[8192];
    int valid_count = 0, invalid_count = 0;
    for (int i = 0; i < 20000 && failures == 0; i++) {
        bool valid;
        size_t len = make_manifest(doc, sizeof doc, &valid);
        bool ok = parse_checked(doc, len, "timer", 1024, m, err, sizeof err);
        if (ok != valid) {
            doc[len < 600 ? len : 600] = '\0';
            CHECK(ok == valid, "parse %s (%s), expected %s, for %s", ok ? "passed" : "failed", ok ? "" : err,
                  valid ? "to pass" : "to fail", doc);
        }
        valid_count += valid;
        invalid_count += !valid;
        apps_manifest_clear(m, count_free);
        exact_release();
    }
    fprintf(stderr, "combos: %d valid and %d invalid manifests\n", valid_count, invalid_count);
    CHECK(valid_count > 1000 && invalid_count > 1000, "%d valid and %d invalid manifests: the pools are lopsided", valid_count,
          invalid_count);
    CHECK(held == 0, "held %zu", held);
    free(m);
}

/* Random damage to a valid manifest: whatever comes out is refused cleanly or parses to something valid. */
static void fuzz(void) {
    apps_manifest_t *m = malloc(sizeof *m);
    char err[512];
    static char doc[4096];
    static const char *const bases[] = {
        timer_manifest,
        "{\"id\":\"timer\",\"name\":\"Timer\",\"version\":\"1.0.0\",\"api\":1,\"main\":\"main.lua\",\"files\":[\"util.lua\",\"a/b.json\"],"
        "\"permissions\":[\"sound\",\"alarm\"],\"memory_kb\":128,\"messages\":{\"start\":\"go\",\"stop\":\"\\u00e9\"},\"x\":[1,2.5e3,{\"y\":null}]}",
    };
    static const char *const tokens[] = {"\"id\"", "\"api\"", "\"files\"", "\"messages\"", "\"permissions\"", "\"main\"", "\"memory_kb\"", "\"name\"", "[", "]", "{", "}",
                                         ",", ":", "\"", "\\", "\\u", "\xc3", "\xe2\x82", "null", "true", "1e999", "-", "0", " ", "\n"};
    int ok_count = 0;
    for (int i = 0; i < 30000; i++) {
        const char *base = bases[rnd(sizeof bases / sizeof bases[0])];
        size_t len = strlen(base);
        memcpy(doc, base, len);
        for (unsigned edits = 1 + rnd(4); edits > 0; edits--) {
            unsigned kind = rnd(6);
            size_t at = len > 0 ? rnd((unsigned)len) : 0;
            if (kind == 0 && len > 0) {  /* a byte replaced by any byte */
                doc[at] = (char)rnd(256);
            } else if (kind == 1 && len > 0) {  /* a byte dropped */
                memmove(doc + at, doc + at + 1, len - at - 1);
                len--;
            } else if (kind == 2 && len + 1 < sizeof doc) {  /* a byte inserted */
                memmove(doc + at + 1, doc + at, len - at);
                doc[at] = (char)rnd(256);
                len++;
            } else if (kind == 3) {  /* cut short */
                len = at;
            } else if (kind == 4 && len + 40 < sizeof doc) {  /* a token inserted */
                const char *t = tokens[rnd(sizeof tokens / sizeof tokens[0])];
                size_t n = strlen(t);
                memmove(doc + at + n, doc + at, len - at);
                memcpy(doc + at, t, n);
                len += n;
            } else if (kind == 5 && len > 8 && len + 16 < sizeof doc) {  /* a stretch repeated */
                size_t n = 1 + rnd(8);
                if (at + n <= len) {
                    memmove(doc + at + n, doc + at, len - at);
                    len += n;
                }
            }
        }
        if (parse_checked(doc, len, "timer", 1024, m, err, sizeof err)) {
            ok_count++;
            apps_manifest_clear(m, count_free);
        }
        exact_release();
    }
    fprintf(stderr, "fuzz: %d of 30000 damaged manifests parsed\n", ok_count);
    CHECK(ok_count > 500, "only %d of 30000 mutations parsed", ok_count);
    CHECK(held == 0, "held %zu", held);
    free(m);
}

int main(int argc, char **argv) {
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"contracts", contracts},
        {"memory", memory},
        {"pkg_free", pkg_free},
        {"compile_message", compile_message},
        {"text_clean", text_clean},
        {"compile_order", compile_order},
        {"check_source", check_source},
        {"truncations", truncations},
        {"combos", combos},
        {"fuzz", fuzz},
    };
    if (argc < 2) {
        fputs("usage: muse_apps_pkg_harness <case>|list|parse|check|image\n", stderr);
        return 2;
    }
    if (strcmp(argv[1], "list") == 0) {
        for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
            puts(cases[i].name);
        }
        return 0;
    }
    if (strcmp(argv[1], "parse") == 0) {
        run_parse(argc, argv);
    } else if (strcmp(argv[1], "check") == 0) {
        run_check(argc, argv);
    } else if (strcmp(argv[1], "image") == 0) {
        run_image(argc, argv);
    } else if (strcmp(argv[1], "icon") == 0) {
        run_icon(argc, argv);
    } else {
        size_t i = 0;
        while (i < sizeof cases / sizeof cases[0] && strcmp(argv[1], cases[i].name) != 0) {
            i++;
        }
        if (i == sizeof cases / sizeof cases[0]) {
            fprintf(stderr, "unknown case %s\n", argv[1]);
            return 2;
        }
        cases[i].run();
    }
    exact_release();
    CHECK(held == 0, "%s: %zu bytes still held at the end", argv[1], held);
    return failures != 0;
}
