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

// Drives vm_file.c against a fake daemon on the Noise session, a clock and a
// heap that the harness controls, in the pattern of link_image_fetch_harness.c.
//
// The daemon serves one file, whose byte at offset i is pat(i), in Range
// requests, and takes POST /fs/upload the way the VM does: it reads the form as
// the sends arrive, and once it has the whole of it answers with the number of
// bytes it wrote, keeping what it saw for the case to print. It answers on
// "turns of the session task": each vTaskDelay the code makes is one, which
// delivers the next frame of the request in flight. A case can add work to a
// turn (g_turn_hook) to do something while a request is out, as another task
// would.
//
// Built with WITH_IMAGE_FETCH it also runs image_fetch.c, over the same daemon
// and the fakes of link_image_fetch_harness.c, for the cases where the two
// meet: who holds the reader, and what a download reads when its range fails.
//
// The harness takes a case name, and for the first write case a path. A case
// prints @-lines for tests/test_vm_file.py to check what it can't from here, and
// exits 0, or says what failed and exits 1. vm_file.c is built with its
// allocations going through the counting allocator below (image_fetch_alloc.h),
// so every case ends by checking that nothing is left.

#include <stdarg.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "noise_control.h"
#include "noise_tunnel_internal.h"
#include "vm_file.h"

#if WITH_IMAGE_FETCH
#include "esp_http_client.h"
#include "image_fetch.h"
#include "led_status.h"
#include "rom/tjpgd.h"
#endif

// The largest reply body this board takes in one frame, noise_ctrl_req_max_body().
#define MAX_BODY (16 * 1024)
#define MAX_REQUESTS 64
#define MAX_POSTS 16
#define MAX_SENDS 64
#define MAX_PARTS 6
#define MAX_ALLOCATIONS 16
// The most one send can carry: a larger one can't be framed, and ends the request
// on the real session (4 KB where Muse has no PSRAM, 8 KB with it).
#define SEND_LIMIT (CONFIG_SPIRAM ? 8192 : 4096)
// What the VM says when it turns an upload away.
#define REPLY_REFUSED "route not allowed for this credential's scopes"
#define REPLY_NOT_FOUND "{\"ok\":false,\"error\":{\"code\":\"not_found\",\"message\":\"path not found\"}}"
#define REPLY_BAD_FORM "{\"ok\":false,\"error\":{\"code\":\"bad_request\",\"message\":\"not a form\"}}"

static void fail_at(const char *file, int line, const char *fmt, ...) {
    va_list ap;
    fflush(stdout);
    fprintf(stderr, "%s:%d: ", file, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    exit(1);
}

#define CHECK(cond, ...) \
    do { \
        if (!(cond)) fail_at(__FILE__, __LINE__, __VA_ARGS__); \
    } while (0)

// The file the daemon serves, and the data a case writes: a pattern that
// repeats neither every 256 bytes nor in every block, so a range read twice, a
// range lost, or a piece out of place all show.
static uint8_t pat(size_t i) {
    return (uint8_t)(i * 167u + (i >> 8) * 13u + 11u);
}

// ---- Heap --------------------------------------------------------------------

// What vm_file.c allocates, with the cost the real allocator adds, and what is
// free for a request's own copy: the harness models the Noise session's DMA
// margin too.
static struct {
    void *ptr;
    size_t size;
    bool external;
} g_allocations[MAX_ALLOCATIONS];
static size_t g_internal_used, g_external_used;
static size_t g_internal_free, g_dma_free, g_largest_block;
static size_t g_min_dma_free;
static unsigned g_alloc_calls, g_fail_alloc;
static size_t g_alloc_limit;            // larger heap_caps_malloc calls fail; 0 for none
static size_t g_pressure_after_body;    // DMA RAM another task takes once the buffer is allocated
static size_t g_internal_pressure_after_body;   // and internal RAM that is not DMA's
static size_t g_body_bytes;             // the last heap_caps_malloc
static bool g_fail_mutex;
static unsigned g_mutexes;
static int g_lock_depth;
static unsigned g_random_calls;         // words esp_random has given

static void record_dma(void) {
    size_t free = g_dma_free > g_internal_used ? g_dma_free - g_internal_used : 0;
    if (free < g_min_dma_free) g_min_dma_free = free;
}

static void *image_alloc(size_t size, bool external) {
    if (++g_alloc_calls == g_fail_alloc) return NULL;
    size_t cost = size + 32;
    if (!external && cost > heap_caps_get_free_size(MALLOC_CAP_DMA)) return NULL;
    void *ptr = malloc(size ? size : 1);
    if (!ptr) return NULL;
    for (size_t i = 0; i < MAX_ALLOCATIONS; i++) {
        if (g_allocations[i].ptr) continue;
        g_allocations[i].ptr = ptr;
        g_allocations[i].size = cost;
        g_allocations[i].external = external;
        if (external) g_external_used += cost;
        else g_internal_used += cost;
        record_dma();
        return ptr;
    }
    CHECK(false, "too many live allocations");
    return NULL;
}

void *image_test_malloc(size_t size) {
    return image_alloc(size, false);
}

void *image_test_calloc(size_t count, size_t size) {
    void *ptr = image_test_malloc(count * size);
    if (ptr) memset(ptr, 0, count * size);
    return ptr;
}

char *image_test_strdup(const char *str) {
    size_t size = strlen(str) + 1;
    char *ptr = image_test_malloc(size);
    if (ptr) memcpy(ptr, str, size);
    return ptr;
}

void image_test_free(void *ptr) {
    if (!ptr) return;
    for (size_t i = 0; i < MAX_ALLOCATIONS; i++) {
        if (g_allocations[i].ptr != ptr) continue;
        if (g_allocations[i].external) g_external_used -= g_allocations[i].size;
        else g_internal_used -= g_allocations[i].size;
        g_allocations[i].ptr = NULL;
        free(ptr);
        return;
    }
    CHECK(false, "free of an allocation vm_file.c didn't make");
}

size_t heap_caps_get_free_size(int caps) {
    size_t capacity = caps == MALLOC_CAP_DMA ? g_dma_free : g_internal_free;
    return capacity > g_internal_used ? capacity - g_internal_used : 0;
}

size_t heap_caps_get_minimum_free_size(int caps) {
    (void)caps;
    return 200000;
}

size_t heap_caps_get_largest_free_block(int caps) {
    size_t block = g_largest_block;
    if (caps == MALLOC_CAP_8BIT && g_alloc_limit && block > g_alloc_limit) block = g_alloc_limit;
    size_t free = heap_caps_get_free_size(caps);
    return block < free ? block : free;
}

void *heap_caps_malloc(size_t size, int caps) {
    if (g_alloc_limit && size > g_alloc_limit) return NULL;
    void *ptr = image_alloc(size, (caps & MALLOC_CAP_SPIRAM) != 0);
    if (ptr) {
        g_body_bytes = size;
        g_dma_free -= g_pressure_after_body;
        g_internal_free -= g_internal_pressure_after_body;
        record_dma();
    }
    return ptr;
}

// What the allocator is told on the way in, so a case that fails on purpose
// can check that it was told the truth.
SemaphoreHandle_t xSemaphoreCreateMutex(void) {
    if (g_fail_mutex) return NULL;
    g_mutexes++;
    return (SemaphoreHandle_t)1;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t wait_ticks) {
    (void)semaphore;
    (void)wait_ticks;
    CHECK(g_lock_depth == 0, "a reply lock was taken twice: the session task would wait on itself");
    g_lock_depth++;
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore) {
    (void)semaphore;
    CHECK(g_lock_depth == 1, "a reply lock was given that wasn't taken");
    g_lock_depth--;
    return pdTRUE;
}

// ---- Clock and log -----------------------------------------------------------

static int64_t g_now_us;

int64_t esp_timer_get_time(void) {
    return g_now_us;
}

static char g_log[32][192];
static unsigned g_log_lines;

void fake_esp_log(char level, const char *tag, const char *fmt, ...) {
    char *line = g_log[g_log_lines++ % 32];
    int n = snprintf(line, sizeof(g_log[0]), "%c %s: ", level, tag);
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line + n, sizeof(g_log[0]) - (size_t)n, fmt, ap);
    va_end(ap);
}

static bool log_has(const char *text) {
    for (unsigned i = 0; i < 32 && i < g_log_lines; i++) {
        if (strstr(g_log[i], text)) return true;
    }
    return false;
}

// The writer's boundary comes from four words of this: a sequence, so a case knows the boundaries to come.
uint32_t esp_random(void) {
    return 0x0A0B0C0Du + 0x01010101u * g_random_calls++;
}

// ---- Fake daemon -------------------------------------------------------------

typedef struct {
    bool active;                // its reply is still to come
    int64_t id;
    noise_ctrl_req_cb cb;
    void *ctx;
} request_t;

static request_t g_get, g_post;
static int64_t g_next_id;
static bool g_connected;        // false: the session refuses requests
static size_t g_max_body;       // noise_ctrl_req_max_body()
static int g_cancels;
static void (*g_turn_hook)(void);

// The file, and how the daemon serves it.
static size_t g_file_len;
static size_t g_frame_bytes;    // reply bytes in one frame
static int g_error_status;      // 0, or the status of every reply
static int g_ignore_range_from; // the daemon ignores Range from this request on; -1 never
static size_t g_cap_range;      // the daemon sends no more than this of a range; 0 for no cap
static size_t g_drop_at;        // the session ends once this many bytes are sent
static int g_stall_after;       // frames sent before the daemon goes quiet
static int g_drop_status;       // what the callback gets when the session ends: -1 or TOO_LARGE
static char g_ranges[MAX_REQUESTS][48];
static char g_get_path[256];
static int g_gets, g_frames_sent;
static size_t g_bytes_sent;
static bool g_head_sent;
static int g_status;
static size_t g_pos, g_end;

// The writes: what the daemon does with each POST /fs/upload, and what it saw of
// the one in flight (the last one, once it is over).
typedef enum { POST_ANSWER, POST_DROP, POST_STALL, POST_HEAD_ONLY } post_action_t;
// How the daemon words its 200.
typedef enum {
    REPLY_COUNT,            // {"ok":true,"bytes_written":N}
    REPLY_BUSY,             // other fields around the count, white space, the key's text in a string
    REPLY_NO_COUNT,         // {"ok":true}
    REPLY_COUNT_STRING,     // "bytes_written":"N"
    REPLY_COUNT_LAST,       // the count is the last thing in the reply
    REPLY_STUTTER,          // the key's start twice over, as a stream that isn't JSON has it
    REPLY_HUGE,             // a count of more digits than a number holds
} reply_style_t;

static int g_posts;                     // POSTs opened
static struct {
    int status[MAX_POSTS];              // of the nth POST's answer; 0 for a 200 with the count
    post_action_t action[MAX_POSTS];
    const char *reply;                  // the body of an answer that has a status
    reply_style_t style;                // of a 200
    long delta;                         // added to the count a 200 reports
    size_t frame_bytes;                 // reply bytes in one frame; 0 for all of them
    int delay_turns;                    // turns the daemon takes to answer
    int early_after_sends;              // it answers with early_status after this many sends; 0 never
    int early_status;
    int drop_after_sends;               // the session ends after this many sends; 0 never
    int send_fails_at;                  // this send, counted from the first of the write, won't queue; -1 never
    int64_t send_cost_us;               // what each send takes of the clock
} g_cfg;

typedef struct {
    char name[24];
    char filename[96];
    char ctype[48];
    bool has_filename, has_ctype;
    uint8_t *body;
    size_t len;
} part_t;

static struct {
    char path[32];
    char ctype[160];                    // its Content-Type header
    char boundary[96];                  // and the boundary in it
    uint8_t *body;                      // every byte sent
    size_t len;
    size_t send_len[MAX_SENDS];
    int wait_ms[MAX_SENDS];             // how long each send was told it could wait for room
    int sends;                          // taken
    int attempts;                       // asked for
    int end_at;                         // the send that had end_body; -1 for none
    bool ended;
    bool answered;                      // answered, cancelled or dropped by the session
    int late_sends;                     // sends after that, which the session throws away
    int delay;
    bool head_sent;
    char malformed[96];                 // why the body isn't a form; "" if it is
    part_t parts[MAX_PARTS];
    int nparts;
} g_up;

static void clear_upload(void) {
    for (int i = 0; i < MAX_PARTS; i++) free(g_up.parts[i].body);
    free(g_up.body);
    memset(&g_up, 0, sizeof(g_up));
    g_up.end_at = -1;
}

static void reset_cfg(void) {
    memset(&g_cfg, 0, sizeof(g_cfg));
    g_cfg.reply = "";
    g_cfg.early_status = 403;
    g_cfg.send_fails_at = -1;
}

static const uint8_t *find(const uint8_t *hay, size_t hay_len, const char *needle, size_t needle_len) {
    for (size_t i = 0; i + needle_len <= hay_len; i++) {
        if (hay[i] == (uint8_t)needle[0] && memcmp(hay + i, needle, needle_len) == 0) return hay + i;
    }
    return NULL;
}

static void malformed(const char *why) {
    if (!g_up.malformed[0]) snprintf(g_up.malformed, sizeof(g_up.malformed), "%s", why);
}

// The text after `key` in a header line, up to the next quote.
static bool header_value(const char *line, const char *key, char *out, size_t out_len) {
    const char *at = strstr(line, key);
    if (!at) return false;
    at += strlen(key);
    const char *quote = strchr(at, '"');
    if (!quote || (size_t)(quote - at) >= out_len) return false;
    memcpy(out, at, (size_t)(quote - at));
    out[quote - at] = '\0';
    return true;
}

// Reads what the writer sent as the VM does, a form: the first boundary at the very
// start, each part's headers, a blank line, its bytes up to the next boundary, and the
// closing boundary last with nothing but a line break after it. Fills g_up.parts, or
// says in g_up.malformed what is wrong.
static void parse_upload(void) {
    g_up.nparts = 0;
    g_up.malformed[0] = '\0';
    char delimiter[128];
    snprintf(delimiter, sizeof(delimiter), "\r\n--%s", g_up.boundary);
    size_t delimiter_len = strlen(delimiter);
    const uint8_t *p = g_up.body, *end = g_up.body + g_up.len;
    if ((size_t)(end - p) < delimiter_len - 2 || memcmp(p, delimiter + 2, delimiter_len - 2) != 0) {
        malformed("the body doesn't start with the boundary");
        return;
    }
    p += delimiter_len - 2;
    for (;;) {
        if (end - p >= 2 && p[0] == '-' && p[1] == '-') {
            p += 2;
            if (end - p != 2 || memcmp(p, "\r\n", 2) != 0) malformed("something follows the closing boundary");
            else if (!g_up.nparts) malformed("the form has no parts");
            return;
        }
        if (end - p < 2 || memcmp(p, "\r\n", 2) != 0) {
            malformed("a boundary isn't followed by a line break");
            return;
        }
        p += 2;
        if (g_up.nparts == MAX_PARTS) {
            malformed("too many parts");
            return;
        }
        const uint8_t *blank = find(p, (size_t)(end - p), "\r\n\r\n", 4);
        if (!blank) {
            malformed("a part has no blank line after its headers");
            return;
        }
        part_t *part = &g_up.parts[g_up.nparts];
        size_t headers_len = (size_t)(blank - p);
        char *headers = malloc(headers_len + 1);
        memcpy(headers, p, headers_len);
        headers[headers_len] = '\0';
        for (char *line = headers; line;) {
            char *next = strstr(line, "\r\n");
            if (next) {
                *next = '\0';
                next += 2;
            }
            if (strncmp(line, "Content-Disposition: form-data; name=\"", 38) == 0) {
                if (!header_value(line, "name=\"", part->name, sizeof(part->name))) malformed("a name too long to read");
                part->has_filename = header_value(line, "filename=\"", part->filename, sizeof(part->filename));
            } else if (strncmp(line, "Content-Type: ", 14) == 0) {
                snprintf(part->ctype, sizeof(part->ctype), "%s", line + 14);
                part->has_ctype = true;
            } else {
                malformed("a header the form doesn't have");
            }
            line = next;
        }
        free(headers);
        if (!part->name[0]) malformed("a part without a name");
        const uint8_t *body = blank + 4;
        const uint8_t *next = find(body, (size_t)(end - body), delimiter, delimiter_len);
        if (!next) {
            malformed("a part isn't closed by a boundary");
            return;
        }
        part->len = (size_t)(next - body);
        part->body = malloc(part->len + 1);
        memcpy(part->body, body, part->len);
        part->body[part->len] = '\0';
        g_up.nparts++;
        p = next + delimiter_len;
    }
}

// What a 200 says: the number of bytes the daemon wrote, worded as the case wants.
static void make_reply(char *out, size_t out_len, size_t count) {
    switch (g_cfg.style) {
    case REPLY_COUNT:
        snprintf(out, out_len, "{\"ok\":true,\"bytes_written\":%zu}", count);
        break;
    case REPLY_BUSY:
        snprintf(out, out_len,
                 "{\"ok\": true, \"path\": \"a.txt\", \"note\": \"\\\"bytes_written\\\": 7\", "
                 "\"bytes_written\" : %zu , \"mode\": \"replace\"}",
                 count);
        break;
    case REPLY_NO_COUNT:
        snprintf(out, out_len, "{\"ok\":true}");
        break;
    case REPLY_COUNT_STRING:
        snprintf(out, out_len, "{\"ok\":true,\"bytes_written\":\"%zu\"}", count);
        break;
    case REPLY_COUNT_LAST:
        snprintf(out, out_len, "{\"bytes_written\":%zu", count);
        break;
    case REPLY_STUTTER:
        snprintf(out, out_len, "{\"bytes\"bytes_written\":%zu}", count);
        break;
    case REPLY_HUGE:
        snprintf(out, out_len, "{\"bytes_written\":%zu99999999999999999999999}", count);
        break;
    }
}

// The answer to the request in flight: its status and body, in frames of frame_bytes.
static void deliver(int status, const char *body) {
    size_t len = strlen(body), at = 0;
    size_t step = g_cfg.frame_bytes ? g_cfg.frame_bytes : (len ? len : 1);
    bool first = true;
    do {
        size_t n = len - at < step ? len - at : step;
        g_post.cb(g_post.ctx, first ? status : 0, (const uint8_t *)body + at, n, at + n == len);
        first = false;
        at += n;
    } while (at < len);
}

// The daemon has the whole form: it reads it and answers.
static void answer_upload(void) {
    int index = g_posts - 1;
    g_post.active = false;
    g_up.answered = true;
    parse_upload();
    if (g_cfg.action[index] == POST_DROP) {
        g_post.cb(g_post.ctx, g_drop_status, NULL, 0, true);
        return;
    }
    if (g_cfg.status[index]) {
        deliver(g_cfg.status[index], g_cfg.reply);
        return;
    }
    if (g_up.malformed[0]) {
        deliver(400, REPLY_BAD_FORM);
        return;
    }
    size_t count = 0;
    for (int i = 0; i < g_up.nparts; i++) {
        if (strcmp(g_up.parts[i].name, "file") == 0) count = g_up.parts[i].len;
    }
    char reply[256];
    make_reply(reply, sizeof(reply), (size_t)((long)count + g_cfg.delta));
    deliver(200, reply);
}

// What the daemon or the session does while the body is still coming: the session
// ends, or the daemon turns the request away without waiting for the rest. True if
// that ended the request.
static bool stream_turn(void) {
    if (!g_post.active || g_up.ended) return false;
    if (g_cfg.drop_after_sends && g_up.sends >= g_cfg.drop_after_sends) {
        g_post.active = false;
        g_up.answered = true;
        g_post.cb(g_post.ctx, g_drop_status, NULL, 0, true);
        return true;
    }
    if (g_cfg.early_after_sends && g_up.sends >= g_cfg.early_after_sends) {
        g_post.active = false;
        g_up.answered = true;
        deliver(g_cfg.early_status, g_cfg.reply);
        return true;
    }
    return false;
}

static void reset_world(void) {
    CHECK(g_lock_depth == 0, "a reply lock is still held");
    CHECK(!g_internal_used && !g_external_used, "leaked %zu internal / %zu external bytes",
          g_internal_used, g_external_used);
    g_internal_free = g_dma_free = 200000;
    g_largest_block = 100000;
    g_min_dma_free = 200000;
    g_alloc_calls = g_fail_alloc = 0;
    g_alloc_limit = 0;
    g_pressure_after_body = g_internal_pressure_after_body = g_body_bytes = 0;
    g_fail_mutex = false;
    g_now_us = 1000000;
    g_connected = true;
    g_max_body = MAX_BODY;
    g_cancels = 0;
    g_turn_hook = NULL;
    memset(&g_get, 0, sizeof(g_get));
    memset(&g_post, 0, sizeof(g_post));
    g_file_len = 0;
    g_frame_bytes = 64 * 1024;
    g_error_status = 0;
    g_ignore_range_from = -1;
    g_cap_range = 0;
    g_drop_at = 0;
    g_stall_after = 1 << 30;
    g_drop_status = -1;
    memset(g_ranges, 0, sizeof(g_ranges));
    g_get_path[0] = '\0';
    g_gets = g_frames_sent = 0;
    g_bytes_sent = 0;
    g_posts = 0;
    g_random_calls = 0;
    reset_cfg();
    clear_upload();
}

static void reset_vm(size_t file_len) {
    reset_world();
    g_file_len = file_len;
}

size_t noise_ctrl_req_max_body(void) {
    return g_max_body;
}

static int64_t open_get(const char *path, const char *const *headers, bool end_body,
                        noise_ctrl_req_cb cb, void *ctx) {
    CHECK(end_body, "a GET has no body");
    CHECK(!g_get.active, "a second request for a range while one is in flight");
    CHECK(g_gets < MAX_REQUESTS, "too many requests");
    const char *range = NULL;
    for (const char *const *h = headers; h && *h; h += 2) {
        CHECK(strcmp(h[0], "Range") == 0, "unexpected header %s", h[0]);
        range = h[1];
    }
    // The real session holds a copy of the request until TX dequeues it. Even
    // while it is queued, the reader must leave enough DMA RAM for control results.
    size_t request_bytes = strlen("GET") + strlen(path) + 3;
    for (const char *const *h = headers; h && *h; h++) request_bytes += strlen(*h) + 1;
    void *request = image_test_malloc(request_bytes);
    if (!request) return 0;
    CHECK(heap_caps_get_free_size(MALLOC_CAP_DMA) >= NOISE_TX_DMA_RESERVE_BYTES,
          "the reader blocked Noise TX with only %zu DMA bytes free",
          heap_caps_get_free_size(MALLOC_CAP_DMA));
    image_test_free(request);

    snprintf(g_ranges[g_gets], sizeof(g_ranges[0]), "%s", range ? range : "");
    snprintf(g_get_path, sizeof(g_get_path), "%s", path);
    bool honours = range && !(g_ignore_range_from >= 0 && g_gets >= g_ignore_range_from);
    g_gets++;
    g_head_sent = false;
    unsigned from = 0, to = 0;
    if (g_error_status) {
        g_status = g_error_status;
        g_pos = g_end = 0;
    } else if (honours) {
        CHECK(sscanf(range, "bytes=%u-%u", &from, &to) == 2, "bad range %s", range);
        g_status = from < g_file_len ? 206 : 416;
        g_pos = from < g_file_len ? from : 0;
        g_end = from < g_file_len ? (to + 1 < g_file_len ? to + 1 : g_file_len) : 0;
        if (g_cap_range && g_end - g_pos > g_cap_range) g_end = g_pos + g_cap_range;
    } else {
        g_status = 200;
        g_pos = 0;
        g_end = g_file_len;
    }
    g_get = (request_t){true, ++g_next_id, cb, ctx};
    return g_get.id;
}

static int64_t open_post(const char *path, const char *const *headers, bool end_body,
                         noise_ctrl_req_cb cb, void *ctx) {
    CHECK(strcmp(path, "/fs/upload") == 0, "a write went to %s", path);
    CHECK(!end_body, "the body of a POST follows its request");
    static const char form[] = "multipart/form-data; boundary=";
    CHECK(headers && headers[0] && strcmp(headers[0], "Content-Type") == 0 && headers[1] && !headers[2],
          "an upload needs exactly one header, Content-Type");
    CHECK(strncmp(headers[1], form, strlen(form)) == 0, "Content-Type is '%s'", headers[1]);
    const char *boundary = headers[1] + strlen(form);
    size_t boundary_len = strlen(boundary);
    CHECK(boundary_len >= 1 && boundary_len <= 70
              && strspn(boundary, "0123456789abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ'()+_,-./:=?")
                     == boundary_len,
          "'%s' is not a boundary RFC 2046 allows", boundary);
    CHECK(!g_post.active, "a second write request while one is in flight");
    CHECK(g_posts < MAX_POSTS, "too many writes");
    clear_upload();
    snprintf(g_up.path, sizeof(g_up.path), "%s", path);
    snprintf(g_up.ctype, sizeof(g_up.ctype), "%s", headers[1]);
    snprintf(g_up.boundary, sizeof(g_up.boundary), "%s", boundary);
    g_up.delay = g_cfg.delay_turns;
    g_posts++;
    g_post = (request_t){true, ++g_next_id, cb, ctx};
    return g_post.id;
}

int64_t noise_ctrl_req_open(const char *verb, const char *path, const char *const *headers,
                            bool end_body, noise_ctrl_req_cb cb, void *ctx) {
    CHECK(g_lock_depth == 0, "opened a request while holding a reply lock");
    if (!g_connected) return 0;
    if (strcmp(verb, "GET") == 0) return open_get(path, headers, end_body, cb, ctx);
    CHECK(strcmp(verb, "POST") == 0, "unexpected verb %s", verb);
    return open_post(path, headers, end_body, cb, ctx);
}

bool noise_ctrl_req_send(int64_t id, const void *data, size_t len, bool end_body, int wait_ms) {
    CHECK(g_lock_depth == 0, "sent a body while holding a reply lock");
    CHECK(id == g_post.id, "body for stream %lld, not the last write", (long long)id);
    CHECK(!g_up.ended, "body sent after its end");
    CHECK(wait_ms > 0, "a body wait of %d ms", wait_ms);
    // A send larger than this ends the request on the real session.
    CHECK(len <= SEND_LIMIT, "a %zu-byte send would end the request", len);
    g_now_us += g_cfg.send_cost_us;
    if (g_cfg.send_fails_at == g_up.attempts++) return false;
    if (!g_post.active) {       // answered or cancelled: the session throws away what is sent for it
        g_up.late_sends++;
        return true;
    }
    CHECK(g_up.sends < MAX_SENDS, "too many sends");
    g_up.body = realloc(g_up.body, g_up.len + len + 1);
    CHECK(g_up.body, "realloc");
    memcpy(g_up.body + g_up.len, data, len);
    g_up.len += len;
    g_up.body[g_up.len] = '\0';
    g_up.send_len[g_up.sends] = len;
    g_up.wait_ms[g_up.sends] = wait_ms;
    if (end_body) {
        g_up.end_at = g_up.sends;
        g_up.ended = true;
    }
    g_up.sends++;
    stream_turn();              // the session task runs while the writer goes on sending
    return true;
}

void noise_ctrl_req_cancel(int64_t id) {
    CHECK(g_lock_depth == 0, "cancelled a request while holding a reply lock");
    CHECK(id == g_get.id || id == g_post.id, "cancelled stream %lld, which isn't open", (long long)id);
    g_cancels++;
    if (id == g_get.id) g_get.active = false;
    if (id == g_post.id) g_post.active = false;
}

static void get_turn(void) {
    if (!g_get.active || g_frames_sent >= g_stall_after) return;
    size_t n = g_end - g_pos;
    if (n > g_frame_bytes) n = g_frame_bytes;
    if (g_drop_at && g_bytes_sent + n > g_drop_at) {
        g_get.active = false;
        g_get.cb(g_get.ctx, g_drop_status, NULL, 0, true);
        return;
    }
    CHECK(n <= g_max_body, "a %u-byte frame would end the session", (unsigned)n);
    uint8_t *data = n ? malloc(n) : NULL;
    for (size_t i = 0; i < n; i++) data[i] = pat(g_pos + i);
    int status = g_head_sent ? 0 : g_status;
    g_head_sent = true;
    g_pos += n;
    g_bytes_sent += n;
    g_frames_sent++;
    bool end = g_pos == g_end;
    if (end) g_get.active = false;
    g_get.cb(g_get.ctx, status, data, n, end);
    free(data);
}

static void post_turn(void) {
    if (!g_post.active) return;
    if (stream_turn() || !g_up.ended) return;       // the daemon waits for the whole form
    int index = g_posts - 1;
    if (g_cfg.action[index] == POST_STALL) return;
    if (g_cfg.action[index] == POST_HEAD_ONLY) {    // the head of the reply, and no more
        if (!g_up.head_sent) {
            g_up.head_sent = true;
            g_post.cb(g_post.ctx, 200, (const uint8_t *)"{", 1, false);
        }
        return;
    }
    if (g_up.delay > 0) {
        g_up.delay--;
        return;
    }
    answer_upload();
}

// One turn of the session task: what a case wants done meanwhile, then the next
// frame of each request in flight.
static void session_turn(void) {
    void (*hook)(void) = g_turn_hook;
    g_turn_hook = NULL;
    if (hook) hook();
    get_turn();
    post_turn();
}

void vTaskDelay(int ticks) {
    CHECK(g_lock_depth == 0, "slept while holding a reply lock");
    CHECK(ticks > 0, "a delay of no ticks is a spin");
    g_now_us += (int64_t)ticks * 1000;
    session_turn();
}

// ---- Helpers -----------------------------------------------------------------

static int64_t deadline(void) {
    return g_now_us + 60 * 1000000LL;
}

static char g_msg[96];

static const char *error_of(vm_file_t *f) {
    return vm_file_error(f, g_msg, sizeof(g_msg));
}

static vm_file_t *open_ok(const char *path, size_t keep, const char *owner) {
    const char *code = NULL, *message = NULL;
    vm_file_t *f = vm_file_open(path, keep, owner, &code, &message);
    CHECK(f, "open %s refused: %s (%s)", path, message ? message : "", code ? code : "");
    return f;
}

static void expect_refusal(const char *path, size_t keep, const char *owner, const char *want_code,
                           const char *want_message) {
    const char *code = NULL, *message = NULL;
    vm_file_t *f = vm_file_open(path, keep, owner, &code, &message);
    CHECK(!f, "open %s should be refused", path ? path : "(null)");
    CHECK(code && strcmp(code, want_code) == 0, "code %s, wanted %s", code ? code : "(null)",
          want_code);
    CHECK(message && strcmp(message, want_message) == 0, "message '%s', wanted '%s'",
          message ? message : "(null)", want_message);
}

// What a case's end checks: nothing is left of the reader, in the heap or in
// the lock, and the writer is free too.
static void finish(void) {
    g_fail_alloc = 0;
    g_alloc_limit = 0;
    g_fail_mutex = false;
    g_dma_free = g_internal_free = 200000;
    g_largest_block = 100000;
    g_pressure_after_body = g_internal_pressure_after_body = 0;
    CHECK(g_lock_depth == 0, "a reply lock is still held");
    CHECK(!g_internal_used && !g_external_used, "left %zu internal / %zu external bytes",
          g_internal_used, g_external_used);
    vm_file_t *f = open_ok("after.bin", 0, VM_FILE_OWNER_IMAGE);   // the reader is free
    vm_file_close(f);
    char err[96];
    g_connected = true;
    reset_cfg();
    g_turn_hook = NULL;
    int posts = g_posts;
    CHECK(vm_file_write("after.bin", "x", 1, 1000, err, sizeof(err)), "the writer is stuck: %s", err);
    CHECK(g_posts == posts + 1, "a write of one byte took %d requests", g_posts - posts);
    CHECK(!g_internal_used && !g_external_used, "the writer left %zu bytes", g_internal_used);
}

typedef struct {
    size_t calls, total, lens[16];
    size_t abort_at;            // the call that returns false; 0 for none
} sink_t;

static bool sink_cb(void *user, const uint8_t *data, size_t len) {
    sink_t *s = user;
    CHECK(s->calls < 16, "too many sink calls");
    for (size_t i = 0; i < len; i++) {
        CHECK(data[i] == pat(s->total + i), "byte %zu arrived as %u", s->total + i, data[i]);
    }
    s->lens[s->calls++] = len;
    s->total += len;
    return !(s->abort_at && s->calls == s->abort_at);
}

static void print_sink(const sink_t *s) {
    for (size_t i = 0; i < s->calls; i++) printf("@sink %zu\n", s->lens[i]);
}

static void print_requests(void) {
    for (int i = 0; i < g_gets; i++) printf("@get %s\n", g_ranges[i]);
}

static void put_escaped(const uint8_t *s, size_t n) {
    for (size_t i = 0; i < n; i++) {
        uint8_t c = s[i];
        if (c == '\\') fputs("\\\\", stdout);
        else if (c == '\t') fputs("\\t", stdout);
        else if (c == '\r') fputs("\\r", stdout);
        else if (c == '\n') fputs("\\n", stdout);
        else if (c < 0x20 || c == 0x7f) printf("\\x%02x", c);
        else putchar(c);
    }
}

// What the daemon saw of the last upload, one line a fact, for the test to check: the
// request, each send's size and which one ended the body, the first and the last send
// as the daemon got them, and each part as it read them.
static void print_upload(const char *label) {
    printf("@up %s\topen\t%s\t%s\n", label, g_up.path, g_up.ctype);
    printf("@up %s\tsends\t%d\t%d\t", label, g_up.end_at, g_up.late_sends);
    for (int i = 0; i < g_up.sends; i++) printf(i ? ",%zu" : "%zu", g_up.send_len[i]);
    putchar('\n');
    if (g_up.sends) {
        printf("@up %s\thead\t", label);
        put_escaped(g_up.body, g_up.send_len[0]);
        size_t last = g_up.send_len[g_up.sends - 1];
        printf("\n@up %s\ttail\t", label);
        put_escaped(g_up.body + g_up.len - last, last);
        putchar('\n');
    }
    for (int i = 0; i < g_up.nparts; i++) {
        const part_t *part = &g_up.parts[i];
        printf("@up %s\tpart\t%s\t%s\t%s\t%zu\t", label, part->name, part->has_filename ? part->filename : "-",
               part->has_ctype ? part->ctype : "-", part->len);
        if (strcmp(part->name, "file") != 0) put_escaped(part->body, part->len);
        putchar('\n');
    }
    if (g_up.malformed[0]) printf("@up %s\tmalformed\t%s\n", label, g_up.malformed);
}

static uint8_t g_data[20 * 1024];

static void fill(size_t n) {
    CHECK(n <= sizeof(g_data), "too much data");
    for (size_t i = 0; i < n; i++) g_data[i] = pat(i);
}

// ---- Reading cases -----------------------------------------------------------

static void case_read_all_streams_ranges_to_sink(void) {
    reset_vm(40 * 1024);
    vm_file_t *f = open_ok("/pics/big.bin", 0, VM_FILE_OWNER_IMAGE);
    CHECK(g_gets == 0, "opening a reader must send nothing: %d requests", g_gets);
    CHECK(vm_file_chunk(f) == MAX_BODY, "chunk %zu", vm_file_chunk(f));
    sink_t s = {0};
    CHECK(vm_file_read_all(f, 1 << 20, sink_cb, &s, deadline()), "read_all failed: %s", error_of(f));
    print_sink(&s);
    print_requests();
    printf("@bytes %zu\n", vm_file_bytes(f));
    CHECK(s.total == 40 * 1024, "the sink got %zu bytes", s.total);
    CHECK(!g_cancels, "a finished request needs no cancel");
    CHECK(log_has("GET /fs/raw/pics/big.bin, 16384 bytes at a time"), "the first range isn't logged");
    // What the sink was given is used up: the file is over for a read, and nothing is asked for.
    uint8_t after[10];
    CHECK(vm_file_read(f, after, sizeof(after), deadline()) == 0, "a read after the whole file went to the sink");
    CHECK(g_gets == 3, "%d requests", g_gets);
    vm_file_close(f);
    finish();
}

static void case_sink_abort_cancels(void) {
    reset_vm(40 * 1024);
    vm_file_t *f = open_ok("big.bin", 0, VM_FILE_OWNER_INSTALL);
    sink_t s = {.abort_at = 2};
    CHECK(!vm_file_read_all(f, 1 << 20, sink_cb, &s, deadline()), "an aborting sink must fail the read");
    CHECK(s.calls == 2, "the sink was called %zu times", s.calls);
    printf("@error %s\n", error_of(f));
    print_sink(&s);
    print_requests();
    CHECK(!g_cancels, "both requests had finished: nothing to cancel");
    CHECK(!g_get.active, "a request is still out");
    vm_file_close(f);
    finish();
}

static void case_max_exceeded(void) {
    // 40 KB with a limit of 20000: the second range would pass it, so it is not given.
    reset_vm(40 * 1024);
    vm_file_t *f = open_ok("big.bin", 0, VM_FILE_OWNER_INSTALL);
    sink_t s = {0};
    CHECK(!vm_file_read_all(f, 20000, sink_cb, &s, deadline()), "a file over max must fail");
    printf("@error %s\n", error_of(f));
    print_sink(&s);
    print_requests();
    CHECK(s.total <= 20000, "the sink got %zu bytes of a 20000-byte limit", s.total);
    vm_file_close(f);
    finish();

    // Exactly max is fine; one byte more is not.
    for (int over = 0; over <= 1; over++) {
        reset_vm(40 * 1024);
        f = open_ok("big.bin", 0, VM_FILE_OWNER_INSTALL);
        sink_t t = {0};
        bool ok = vm_file_read_all(f, 40 * 1024 - over, sink_cb, &t, deadline());
        printf("@limit %d %d %s\n", over, ok, ok ? "" : error_of(f));
        vm_file_close(f);
        finish();
    }

    // A limit of nothing takes an empty file and refuses a byte.
    for (size_t len = 0; len <= 1; len++) {
        reset_vm(len);
        f = open_ok("tiny.bin", 0, VM_FILE_OWNER_INSTALL);
        sink_t t = {0};
        bool ok = vm_file_read_all(f, 0, sink_cb, &t, deadline());
        printf("@zero %zu %d %zu %s\n", len, ok, t.calls, ok ? "" : error_of(f));
        vm_file_close(f);
        finish();
    }
}

static void case_second_reader_is_busy(void) {
    reset_vm(100);
    unsigned allocs;
    vm_file_t *f = open_ok("a.bin", 0, VM_FILE_OWNER_IMAGE);
    allocs = g_alloc_calls;
    expect_refusal("b.bin", 0, VM_FILE_OWNER_INSTALL, "busy",
                   "the VM file reader is busy (an image download)");
    expect_refusal("b.bin", 0, VM_FILE_OWNER_IMAGE, "busy",
                   "the VM file reader is busy (an image download)");
    CHECK(g_alloc_calls == allocs, "a refused open allocated");
    // The holder is not disturbed by them.
    uint8_t buf[100];
    CHECK(vm_file_read(f, buf, sizeof(buf), deadline()) == 100, "the holder can't read: %s", error_of(f));
    vm_file_close(f);

    f = open_ok("a.bin", 0, VM_FILE_OWNER_INSTALL);
    expect_refusal("b.bin", 0, VM_FILE_OWNER_IMAGE, "busy",
                   "the VM file reader is busy (an app install)");
    expect_refusal("/", 0, VM_FILE_OWNER_IMAGE, "invalid_params", "path names no file");
    // Another owner is reported without a name, and nothing else is different.
    vm_file_close(f);
    f = open_ok("a.bin", 0, "something else");
    expect_refusal("b.bin", 0, VM_FILE_OWNER_INSTALL, "busy", "the VM file reader is busy");
    vm_file_close(f);
    f = open_ok("a.bin", 0, NULL);
    expect_refusal("b.bin", 0, VM_FILE_OWNER_INSTALL, "busy", "the VM file reader is busy");
    vm_file_close(f);

    // Closed, it is free for either.
    f = open_ok("a.bin", 0, VM_FILE_OWNER_IMAGE);
    vm_file_close(f);
    vm_file_close(NULL);
    f = open_ok("a.bin", 0, VM_FILE_OWNER_INSTALL);
    vm_file_close(f);
    finish();
}

// A 206 shorter than was asked for is the end of the file: that is what the
// last range of every file looks like, and a reply carries no length of the file.
static void case_short_206_is_the_end(void) {
    reset_vm(40 * 1024);
    g_cap_range = 10000;    // a daemon that sends no more than this of a range
    vm_file_t *f = open_ok("big.bin", 0, VM_FILE_OWNER_INSTALL);
    sink_t s = {0};
    CHECK(vm_file_read_all(f, 1 << 20, sink_cb, &s, deadline()), "read_all: %s", error_of(f));
    print_sink(&s);
    print_requests();
    printf("@bytes %zu\n", vm_file_bytes(f));
    vm_file_close(f);
    finish();
}

static void case_416_at_the_end(void) {
    // A file of whole ranges: the range after the last is past the end.
    reset_vm(2 * MAX_BODY);
    vm_file_t *f = open_ok("whole.bin", 0, VM_FILE_OWNER_INSTALL);
    sink_t s = {0};
    CHECK(vm_file_read_all(f, 1 << 20, sink_cb, &s, deadline()), "read_all: %s", error_of(f));
    print_sink(&s);
    print_requests();
    vm_file_close(f);
    finish();

    // An empty file: the first range is past the end.
    reset_vm(0);
    f = open_ok("empty.bin", 0, VM_FILE_OWNER_INSTALL);
    sink_t t = {0};
    CHECK(vm_file_read_all(f, 1 << 20, sink_cb, &t, deadline()), "empty file: %s", error_of(f));
    printf("@empty %zu %d\n", t.calls, g_gets);
    uint8_t buf[8];
    CHECK(vm_file_read(f, buf, sizeof(buf), deadline()) == 0, "a read at the end must give 0");
    CHECK(vm_file_bytes(f) == 0, "bytes %zu", vm_file_bytes(f));
    vm_file_close(f);
    finish();
}

static void case_200_to_a_ranged_get(void) {
    // A daemon that ignores Range is fine for a file that fits one range.
    reset_vm(5000);
    g_ignore_range_from = 0;
    vm_file_t *f = open_ok("small.bin", 0, VM_FILE_OWNER_INSTALL);
    sink_t s = {0};
    CHECK(vm_file_read_all(f, 1 << 20, sink_cb, &s, deadline()), "small file without Range: %s",
          error_of(f));
    print_sink(&s);
    printf("@requests %d\n", g_gets);
    vm_file_close(f);
    finish();

    // But not for a larger one, which it sends in frames that overflow the buffer.
    reset_vm(40 * 1024);
    g_ignore_range_from = 0;
    g_frame_bytes = 4096;
    f = open_ok("big.bin", 0, VM_FILE_OWNER_INSTALL);
    sink_t t = {0};
    bool ok = vm_file_read_all(f, 1 << 20, sink_cb, &t, deadline());
    CHECK(!ok, "a file larger than a range can't come from a daemon that ignores Range");
    printf("@big %zu %d %s\n", t.calls, g_cancels, error_of(f));
    vm_file_close(f);
    finish();

    // One that starts ignoring it after the first range sends a 200 at an offset: not the rest of
    // the file. (Its whole file has to fit the buffer to get that far: this one is one range exactly.)
    reset_vm(MAX_BODY);
    g_ignore_range_from = 1;
    f = open_ok("one.bin", 0, VM_FILE_OWNER_INSTALL);
    sink_t u = {0};
    ok = vm_file_read_all(f, 1 << 20, sink_cb, &u, deadline());
    CHECK(!ok, "a 200 at a later offset is not the rest of the file");
    printf("@late %zu %s\n", u.calls, error_of(f));
    vm_file_close(f);
    finish();
}

static void case_session_drop_mid_read(void) {
    reset_vm(40 * 1024);
    g_drop_at = 20000;      // the second range's frame doesn't make it
    vm_file_t *f = open_ok("big.bin", 0, VM_FILE_OWNER_INSTALL);
    sink_t s = {0};
    CHECK(!vm_file_read_all(f, 1 << 20, sink_cb, &s, deadline()), "a dropped session must fail the read");
    print_sink(&s);
    printf("@error %s\n", error_of(f));
    CHECK(!vm_file_timed_out(f), "a drop is not a timeout");
    vm_file_close(f);
    finish();

    // The same through vm_file_read: the bytes of the first range, then -1.
    reset_vm(40 * 1024);
    g_drop_at = 20000;
    f = open_ok("big.bin", 0, VM_FILE_OWNER_INSTALL);
    static uint8_t buf[40 * 1024];
    int n = vm_file_read(f, buf, 16 * 1024, deadline());
    CHECK(n == 16 * 1024, "the first range is whole: %d", n);
    n = vm_file_read(f, buf, 16 * 1024, deadline());
    printf("@read %d %s\n", n, error_of(f));
    vm_file_close(f);
    finish();

    // A drop in the first range.
    reset_vm(40 * 1024);
    g_drop_at = 100;
    f = open_ok("big.bin", 0, VM_FILE_OWNER_INSTALL);
    CHECK(vm_file_read(f, buf, 10, deadline()) < 0, "a drop in the first range");
    printf("@first %s\n", error_of(f));
    vm_file_close(f);
    finish();

    // The session ends because a frame was larger than the board's buffers.
    reset_vm(40 * 1024);
    g_drop_at = 100;
    g_drop_status = NOISE_CTRL_REQ_TOO_LARGE;
    f = open_ok("big.bin", 0, VM_FILE_OWNER_INSTALL);
    CHECK(vm_file_read(f, buf, 10, deadline()) < 0, "a frame too large");
    printf("@large %s\n", error_of(f));
    vm_file_close(f);
    finish();

    // The session is down: nothing can be sent, and nothing needs cancelling.
    reset_vm(40 * 1024);
    g_connected = false;
    f = open_ok("big.bin", 0, VM_FILE_OWNER_INSTALL);
    sink_t s2 = {0};
    CHECK(!vm_file_read_all(f, 1 << 20, sink_cb, &s2, deadline()), "no session");
    printf("@down %s %d %d\n", error_of(f), g_gets, g_cancels);
    vm_file_close(f);
    finish();
}

static void case_http_error(void) {
    reset_vm(100);
    g_error_status = 404;
    vm_file_t *f = open_ok("nope.bin", 0, VM_FILE_OWNER_INSTALL);
    uint8_t buf[10];
    CHECK(vm_file_read(f, buf, sizeof(buf), deadline()) < 0, "a 404 is not a file");
    printf("@error %s\n", error_of(f));
    CHECK(strcmp(g_get_path, "/fs/raw/nope.bin") == 0, "path %s", g_get_path);
    sink_t s = {0};
    CHECK(!vm_file_read_all(f, 100, sink_cb, &s, deadline()), "read_all of a 404");
    printf("@error %s\n", error_of(f));
    CHECK(!vm_file_timed_out(f), "a 404 is not a timeout");
    vm_file_close(f);
    finish();
}

static void case_read_timeout(void) {
    reset_vm(40 * 1024);
    g_stall_after = 0;      // the daemon takes the request and goes quiet
    vm_file_t *f = open_ok("slow.bin", 0, VM_FILE_OWNER_INSTALL);
    int64_t start = g_now_us;
    sink_t s = {0};
    CHECK(!vm_file_read_all(f, 1 << 20, sink_cb, &s, start + 1000000), "a silent daemon must time out");
    printf("@timeout %d %s %lld\n", vm_file_timed_out(f), error_of(f), (long long)(g_now_us - start));
    printf("@cancels %d\n", g_cancels);
    vm_file_close(f);
    finish();

    // The deadline is the caller's and absolute: one in the past sends one request and gives up.
    reset_vm(40 * 1024);
    g_stall_after = 0;
    f = open_ok("slow.bin", 0, VM_FILE_OWNER_INSTALL);
    uint8_t buf[10];
    CHECK(vm_file_read(f, buf, sizeof(buf), g_now_us - 1) < 0, "a deadline in the past");
    printf("@past %d %d\n", vm_file_timed_out(f), g_cancels);
    vm_file_close(f);
    finish();

    // A reader that timed out remembers it. If the next request can't even be sent, that is what
    // it says, not the old timeout.
    reset_vm(40 * 1024);
    g_stall_after = 0;
    f = open_ok("slow.bin", 0, VM_FILE_OWNER_INSTALL);
    CHECK(vm_file_read(f, buf, sizeof(buf), g_now_us + 100000) < 0, "a silent daemon must time out");
    CHECK(vm_file_timed_out(f), "the timeout isn't kept");
    g_connected = false;
    CHECK(vm_file_read(f, buf, sizeof(buf), g_now_us + 100000) < 0, "no session");
    printf("@gone %d %s\n", vm_file_timed_out(f), error_of(f));
    vm_file_close(f);
    finish();
}

// The frames of an earlier request, cancelled or timed out, carry an older
// token and are dropped, whenever they come.
static noise_ctrl_req_cb g_late_cb;
static void *g_late_ctx;

static void late_frame(void) {
    g_late_cb(g_late_ctx, 200, (const uint8_t *)"xx", 2, true);
}

static void case_late_frames_are_dropped(void) {
    reset_vm(40 * 1024);
    g_stall_after = 0;
    vm_file_t *f = open_ok("slow.bin", 0, VM_FILE_OWNER_INSTALL);
    uint8_t buf[100];
    CHECK(vm_file_read(f, buf, sizeof(buf), g_now_us + 100000) < 0, "timeout");
    g_late_cb = g_get.cb;
    g_late_ctx = g_get.ctx;
    vm_file_close(f);
    finish();

    // After the timeout, before the next request.
    reset_vm(100);
    g_late_cb(g_late_ctx, 200, (const uint8_t *)"xx", 2, true);
    f = open_ok("next.bin", 0, VM_FILE_OWNER_INSTALL);
    CHECK(vm_file_read(f, buf, sizeof(buf), deadline()) == 100, "next read: %s", error_of(f));
    for (size_t i = 0; i < 100; i++) CHECK(buf[i] == pat(i), "byte %zu was %u", i, buf[i]);
    vm_file_close(f);
    finish();

    // While the next request is in flight.
    reset_vm(100);
    f = open_ok("next.bin", 0, VM_FILE_OWNER_INSTALL);
    g_turn_hook = late_frame;
    CHECK(vm_file_read(f, buf, sizeof(buf), deadline()) == 100, "read: %s", error_of(f));
    for (size_t i = 0; i < 100; i++) CHECK(buf[i] == pat(i), "byte %zu was %u", i, buf[i]);
    vm_file_close(f);
    finish();
}

static void case_read_in_small_pieces(void) {
    reset_vm(40 * 1024);
    vm_file_t *f = open_ok("big.bin", 0, VM_FILE_OWNER_INSTALL);
    static uint8_t buf[5000];
    size_t total = 0;
    int calls = 0;
    for (;;) {
        int n = vm_file_read(f, buf, sizeof(buf), deadline());
        CHECK(n >= 0, "read: %s", error_of(f));
        if (n == 0) break;
        for (int i = 0; i < n; i++) CHECK(buf[i] == pat(total + (size_t)i), "byte %zu", total + (size_t)i);
        total += (size_t)n;
        calls++;
    }
    printf("@read %zu %d %d\n", total, calls, g_gets);
    CHECK(vm_file_read(f, buf, sizeof(buf), deadline()) == 0, "reads at the end stay 0");
    CHECK(vm_file_read(f, buf, 0, deadline()) == 0, "a read of nothing");
    CHECK(vm_file_bytes(f) == 40 * 1024, "bytes %zu", vm_file_bytes(f));
    print_requests();
    vm_file_close(f);
    finish();
}

static void case_read_then_read_all(void) {
    reset_vm(40 * 1024);
    vm_file_t *f = open_ok("big.bin", 0, VM_FILE_OWNER_INSTALL);
    uint8_t head[100];
    CHECK(vm_file_read(f, head, sizeof(head), deadline()) == 100, "head");
    for (size_t i = 0; i < 100; i++) CHECK(head[i] == pat(i), "head byte %zu", i);
    // The rest of the first range goes to the sink first, and offset 100 is where it starts.
    sink_t s = {0};
    s.total = 100;
    CHECK(vm_file_read_all(f, 1 << 20, sink_cb, &s, deadline()), "read_all: %s", error_of(f));
    print_sink(&s);
    print_requests();
    vm_file_close(f);
    finish();
}

// ---- Opening cases -----------------------------------------------------------

static void case_open_refusals(void) {
    // The lock for the reply can't be made (it is made the first time, so this goes first):
    // refused, and the reader is not held.
    reset_vm(100);
    g_fail_mutex = true;
    expect_refusal("a.bin", 0, VM_FILE_OWNER_IMAGE, "out_of_memory", "failed to allocate");
    CHECK(!g_alloc_calls, "allocated without a lock");
    finish();

    // An allocation that fails: the path (1st), the buffer (2nd).
    for (unsigned fail = 1; fail <= 2; fail++) {
        reset_vm(100);
        g_fail_alloc = fail;
        expect_refusal("a.bin", 0, VM_FILE_OWNER_IMAGE, "out_of_memory", "failed to allocate");
        CHECK(!g_internal_used && !g_external_used, "allocation %u leaked", fail);
        CHECK(!g_gets, "nothing should be requested");
        finish();
    }

    // No room for the Noise session's margin, the request and the caller's own work: a task
    // stack of 6144 bytes and a decoder's 3100, as image_fetch counts them.
    reset_vm(100);
    g_dma_free = 26 * 1024;
    expect_refusal("big.raw", 6144 + 3100, VM_FILE_OWNER_IMAGE, "out_of_memory", "failed to allocate");
    CHECK(g_alloc_calls == 1, "a refusal before allocating the buffer: %u allocations", g_alloc_calls);
    finish();

    // Another task takes the DMA RAM between the first check and the buffer.
    reset_vm(100);
    g_pressure_after_body = 180000;
    expect_refusal("big.raw", 0, VM_FILE_OWNER_IMAGE, "out_of_memory", "failed to allocate");
    CHECK(g_body_bytes && !g_gets, "expected a refusal after the buffer was allocated");
    finish();

    // Another task takes internal RAM that isn't the DMA's, between the first check and the buffer.
    reset_vm(100);
    g_internal_pressure_after_body = 180000;
    expect_refusal("big.raw", 0, VM_FILE_OWNER_IMAGE, "out_of_memory", "failed to allocate");
    CHECK(g_body_bytes && !g_gets, "expected a refusal after the buffer was allocated");
    finish();

#if CONFIG_SPIRAM
    // The queued request needs a block of its own once the buffer is allocated. With PSRAM the
    // buffer takes none, so the request's own size decides: its path and the 64 bytes.
    const size_t request = strlen("/fs/raw/a.bin") + 64;
    reset_vm(100);
    g_largest_block = request - 1;
    expect_refusal("a.bin", 0, VM_FILE_OWNER_IMAGE, "out_of_memory", "failed to allocate");
    CHECK(g_body_bytes && !g_gets, "expected a refusal after the buffer was allocated");
    finish();
    reset_vm(100);
    g_largest_block = request;
    vm_file_close(open_ok("a.bin", 0, VM_FILE_OWNER_IMAGE));
    finish();
#endif

    // An empty path, or one of slashes, names no file, and holds nothing.
    reset_vm(100);
    expect_refusal("", 0, VM_FILE_OWNER_IMAGE, "invalid_params", "path names no file");
    expect_refusal("///", 0, VM_FILE_OWNER_IMAGE, "invalid_params", "path names no file");
    expect_refusal(NULL, 0, VM_FILE_OWNER_IMAGE, "invalid_params", "path names no file");
    CHECK(!g_alloc_calls, "an empty path allocated");
    finish();
}

// What `keep` is for: the caller's own memory is counted with the session's
// margin, the request and the 4 KB reserve, as one sum.
static void case_open_counts_keep(void) {
    // The same memory takes a reader that keeps nothing, and not one that keeps more than is left.
    reset_vm(100);
    g_dma_free = 40000;
    vm_file_t *f = open_ok("f.bin", 0, VM_FILE_OWNER_IMAGE);
    vm_file_close(f);
    finish();
    reset_vm(100);
    g_dma_free = 40000;
    expect_refusal("f.bin", 20000, VM_FILE_OWNER_IMAGE, "out_of_memory", "failed to allocate");
    finish();
#if CONFIG_SPIRAM
    // With PSRAM the buffer takes none of it, so the sum is exact: the margin, the keep, the
    // request (its path and the 64 bytes of verb, Range header and separators), the reserve, and
    // the copy of the path the harness's heap charges.
    const size_t path_alloc = strlen("/fs/raw/") + strlen("f.bin") * 3 + 1 + 32;
    const size_t request = strlen("/fs/raw/f.bin") + 64;
    for (size_t keep = 0; keep <= 9000; keep += 9000) {
        size_t exactly = NOISE_TX_DMA_RESERVE_BYTES + keep + request + 4096 + path_alloc;
        reset_vm(100);
        g_dma_free = exactly;
        f = open_ok("f.bin", keep, VM_FILE_OWNER_IMAGE);
        vm_file_close(f);
        finish();

        reset_vm(100);
        g_dma_free = exactly - 1;
        expect_refusal("f.bin", keep, VM_FILE_OWNER_IMAGE, "out_of_memory", "failed to allocate");
        finish();
    }
#endif
}

static void case_open_chunk_under_pressure(void) {
    // A board with PSRAM keeps the largest range; without it the range shrinks to what is left
    // after the margin, the request and the reserve (and the allocator's 32 bytes).
    reset_vm(100);
    g_max_body = 10 * 1024;
    g_dma_free = 28 * 1024;
    vm_file_t *f = open_ok("big.bin", 0, VM_FILE_OWNER_INSTALL);
#if CONFIG_SPIRAM
    CHECK(vm_file_chunk(f) == g_max_body && g_body_bytes == g_max_body, "PSRAM keeps the maximum: %zu",
          vm_file_chunk(f));
#else
    const size_t path_alloc = strlen("/fs/raw/") + strlen("big.bin") * 3 + 1 + 32;
    const size_t keep = NOISE_TX_DMA_RESERVE_BYTES + strlen("/fs/raw/big.bin") + 64 + 4096;
    CHECK(vm_file_chunk(f) == g_dma_free - path_alloc - keep - 32, "the range that is left: %zu",
          vm_file_chunk(f));
    CHECK(vm_file_chunk(f) < g_max_body, "the range should have shrunk: %zu", vm_file_chunk(f));
#endif
    CHECK(g_min_dma_free >= NOISE_TX_DMA_RESERVE_BYTES, "the reserve was used: %zu", g_min_dma_free);
    printf("@chunk %zu\n", vm_file_chunk(f));
    vm_file_close(f);
    finish();

    // Ample memory takes the maximum.
    reset_vm(100);
    g_max_body = 10 * 1024;
    f = open_ok("big.bin", 0, VM_FILE_OWNER_INSTALL);
    CHECK(vm_file_chunk(f) == g_max_body, "ample memory must use the maximum: %zu", vm_file_chunk(f));
    vm_file_close(f);
    finish();

    // The largest block the heap can give caps the buffer without PSRAM; a PSRAM board refuses.
    reset_vm(100);
    g_max_body = 10 * 1024;
    g_alloc_limit = 9 * 1024;
#if CONFIG_SPIRAM
    expect_refusal("big.bin", 0, VM_FILE_OWNER_INSTALL, "out_of_memory", "failed to allocate");
#else
    f = open_ok("big.bin", 0, VM_FILE_OWNER_INSTALL);
    CHECK(vm_file_chunk(f) == g_alloc_limit, "the largest allocatable range: %zu", vm_file_chunk(f));
    vm_file_close(f);
#endif
    finish();
}

static void case_request_path_is_encoded(void) {
    static const struct {
        const char *path;
        const char *request;
    } cases[] = {
        {"/home/me/pics/a b%.raw", "/fs/raw/home/me/pics/a%20b%25.raw"},
        {"workspace/apps/hello/main.lua", "/fs/raw/workspace/apps/hello/main.lua"},
        {"///x/y", "/fs/raw/x/y"},
        {"a-b_c.d~e/f", "/fs/raw/a-b_c.d~e/f"},
        {"q?x=1&y#z", "/fs/raw/q%3Fx%3D1%26y%23z"},
        {"caf\xc3\xa9", "/fs/raw/caf%C3%A9"},
        {"a\"b\\c\n", "/fs/raw/a%22b%5Cc%0A"},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        reset_vm(10);
        vm_file_t *f = open_ok(cases[i].path, 0, VM_FILE_OWNER_INSTALL);
        uint8_t buf[10];
        CHECK(vm_file_read(f, buf, sizeof(buf), deadline()) == 10, "read: %s", error_of(f));
        CHECK(strcmp(g_get_path, cases[i].request) == 0, "'%s' asked for %s, wanted %s", cases[i].path,
              g_get_path, cases[i].request);
        vm_file_close(f);
        finish();
    }
    // A long path costs nothing it wasn't counted for.
    reset_vm(10);
    char path[1025];
    memset(path, 'a', sizeof(path) - 1);
    path[sizeof(path) - 1] = '\0';
    vm_file_t *f = open_ok(path, 0, VM_FILE_OWNER_INSTALL);
    uint8_t buf[10];
    CHECK(vm_file_read(f, buf, sizeof(buf), deadline()) == 10, "long path: %s", error_of(f));
    vm_file_close(f);
    finish();
}

// ---- Writing cases -----------------------------------------------------------

#define FILE_PATH "workspace/apps/x/f.bin"

static const char *path_arg(int argc, char **argv) {
    return argc > 2 ? argv[2] : "workspace/apps/x/main.lua";
}

// The file part of the last upload as the daemon read it: n bytes, those of `want`.
static void check_file(const uint8_t *want, size_t n) {
    CHECK(!g_up.malformed[0], "the daemon could not read the body as a form: %s", g_up.malformed);
    const part_t *file = NULL;
    for (int i = 0; i < g_up.nparts; i++) {
        if (strcmp(g_up.parts[i].name, "file") == 0) file = &g_up.parts[i];
    }
    CHECK(file, "the form has no file part");
    CHECK(file->len == n, "the file part has %zu bytes, not %zu", file->len, n);
    CHECK(n == 0 || memcmp(file->body, want, n) == 0, "the file part's bytes are not the ones written");
}

// Data a form could trip on: a zero, a line break and dashes across two pieces, what a
// boundary starts with (which isn't one), and the start of a delimiter as its last bytes.
static void fill_form_data(size_t n) {
    fill(n);
    g_data[7] = 0;
    memcpy(g_data + 4094, "\r\n--", 4);
    memcpy(g_data + 6000, "\r\n--muse", 8);
    memcpy(g_data + n - 4, "\r\n--", 4);
}

static bool write_ten_thousand(char *err, size_t err_len) {
    fill(10000);
    return vm_file_write(FILE_PATH, g_data, 10000, 10000, err, err_len);
}

// How a failed write ended: what it said, the sends the daemon took, those it was
// sent after it had answered, and the cancels.
#define SUMMARY(tag, err) printf("@" tag " %s|%d|%d|%d\n", err, g_up.sends, g_up.late_sends, g_cancels)

static void case_write_upload_multipart(int argc, char **argv) {
    reset_vm(0);
    fill_form_data(10000);
    char err[96] = "";
    CHECK(vm_file_write(path_arg(argc, argv), g_data, 10000, 10000, err, sizeof(err)), "write failed: %s", err);
    CHECK(g_posts == 1, "%d requests", g_posts);
    CHECK(g_cancels == 0, "a finished write needs no cancel");
    check_file(g_data, 10000);
    print_upload("up");
    finish();
}

static void case_write_zero_4096_4097(void) {
    static const size_t sizes[] = {0, 1, 4095, 4096, 4097, 8192, 8193};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
        reset_vm(0);
        fill(sizes[i]);
        char err[96] = "";
        CHECK(vm_file_write(FILE_PATH, sizes[i] ? g_data : NULL, sizes[i], 5000, err, sizeof(err)),
              "write of %zu: %s", sizes[i], err);
        check_file(g_data, sizes[i]);
        char label[16];
        snprintf(label, sizeof(label), "%zu", sizes[i]);
        print_upload(label);
        finish();
    }
}

static void case_write_http_error(void) {
    char err[96];
    // The VM's words follow the status when they fit. The whole upload went out first.
    reset_vm(0);
    g_cfg.status[0] = 500;
    g_cfg.reply = "boom";
    CHECK(!write_ten_thousand(err, sizeof(err)), "an HTTP 500 must fail the write");
    SUMMARY("e500", err);
    finish();

    // Turned away after the first send: the rest is not sent.
    reset_vm(0);
    g_cfg.early_after_sends = 1;
    g_cfg.early_status = 403;
    g_cfg.reply = REPLY_REFUSED;
    CHECK(!write_ten_thousand(err, sizeof(err)), "an HTTP 403 must fail the write");
    SUMMARY("e403", err);
    CHECK(log_has("403"), "the refusal isn't logged");
    CHECK(log_has(REPLY_REFUSED), "the VM's own words aren't logged");
    finish();

    // The VM's error as it words it for a missing folder.
    reset_vm(0);
    g_cfg.status[0] = 404;
    g_cfg.reply = REPLY_NOT_FOUND;
    CHECK(!write_ten_thousand(err, sizeof(err)), "an HTTP 404 must fail the write");
    SUMMARY("e404", err);
    finish();

    // Words that don't fit are left out, and so are words the writer kept only the start of.
    reset_vm(0);
    g_cfg.status[0] = 500;
    g_cfg.reply = "0123456789012345678901234567890123456789012345678901234567890123456789012345678901234567890123456789";
    CHECK(!write_ten_thousand(err, sizeof(err)), "an HTTP 500 must fail the write");
    SUMMARY("e_long", err);
    finish();
    {
        char wide[256];
        reset_vm(0);
        g_cfg.status[0] = 500;
        g_cfg.reply = "01234567890123456789012345678901234567890123456789012345678901234567890123456789"
                      "01234567890123456789012345678901234567890123456789012345678901234567890123456789";
        CHECK(!write_ten_thousand(wide, sizeof(wide)), "an HTTP 500 must fail the write");
        printf("@e_kept %s\n", wide);    // 160 bytes of words, of which the writer keeps 127
        finish();
    }

    // What isn't printable is not passed on: white space is a space, the rest a question mark, and the
    // ends are trimmed. The path is not in the log.
    reset_vm(0);
    g_cfg.status[0] = 500;
    g_cfg.reply = "\r\n bad\tpath \x01 \xc3\xa9 \r\n";
    CHECK(!write_ten_thousand(err, sizeof(err)), "an HTTP 500 must fail the write");
    SUMMARY("e_clean", err);
    CHECK(!log_has("workspace/apps"), "the path is in the log");
    finish();

    // No words: the status alone. A redirect isn't success.
    reset_vm(0);
    g_cfg.status[0] = 500;
    CHECK(!write_ten_thousand(err, sizeof(err)), "an HTTP 500 must fail the write");
    SUMMARY("e_empty", err);
    finish();
    reset_vm(0);
    g_cfg.status[0] = 302;
    CHECK(!write_ten_thousand(err, sizeof(err)), "a 302 must fail the write");
    SUMMARY("e302", err);
    finish();

    // A 2xx with no count is not a write that was counted.
    reset_vm(0);
    g_cfg.status[0] = 204;
    CHECK(!vm_file_write(FILE_PATH, "x", 1, 1000, err, sizeof(err)), "a 204 says nothing of the count");
    SUMMARY("e204", err);
    finish();
}

static void case_write_session_drop(void) {
    char err[96];
    // The session ends while the body is going out: the writer stops sending.
    reset_vm(0);
    g_cfg.drop_after_sends = 2;
    CHECK(!write_ten_thousand(err, sizeof(err)), "a dropped session must fail the write");
    SUMMARY("drop", err);
    finish();

    // It ends once the whole body is out, before the answer.
    reset_vm(0);
    g_cfg.action[0] = POST_DROP;
    CHECK(!write_ten_thousand(err, sizeof(err)), "a dropped session must fail the write");
    SUMMARY("drop_late", err);
    finish();

    // The session is gone when the write starts, and when a send can't be queued.
    reset_vm(0);
    g_connected = false;
    CHECK(!write_ten_thousand(err, sizeof(err)), "no session");
    printf("@down %s %d %d\n", err, g_posts, g_cancels);
    finish();

    reset_vm(0);
    g_cfg.send_fails_at = 2;    // the third: the first piece of the file
    CHECK(!write_ten_thousand(err, sizeof(err)), "a send that can't be queued");
    SUMMARY("queue", err);
    finish();

    // The head of the form can't be queued either.
    reset_vm(0);
    g_cfg.send_fails_at = 0;
    CHECK(!write_ten_thousand(err, sizeof(err)), "a send that can't be queued");
    SUMMARY("queue_first", err);
    finish();

    // A reply the board can't take ends the session: the VM's frame was too large.
    reset_vm(0);
    g_cfg.status[0] = NOISE_CTRL_REQ_TOO_LARGE;
    CHECK(!vm_file_write(FILE_PATH, "x", 1, 1000, err, sizeof(err)), "too large");
    printf("@large %s\n", err);
    finish();
}

static void case_write_timeout(void) {
    char err[96];
    // A daemon that takes the form and never answers.
    reset_vm(0);
    g_cfg.action[0] = POST_STALL;
    int64_t start = g_now_us;
    CHECK(!vm_file_write(FILE_PATH, "x", 1, 1000, err, sizeof(err)), "must time out");
    printf("@stall %s %d %d %lld\n", err, g_posts, g_cancels, (long long)(g_now_us - start));
    finish();

    // The time is for the whole call, sending included: a session that takes 400 ms a send
    // has taken three of them when the second piece is out, and the third is never sent.
    reset_vm(0);
    fill(10000);
    g_cfg.send_cost_us = 400000;
    start = g_now_us;
    CHECK(!vm_file_write(FILE_PATH, g_data, 10000, 1000, err, sizeof(err)), "the sends can't all fit");
    printf("@slow %s|%d|%d|%d|%lld|", err, g_up.sends, g_up.late_sends, g_cancels, (long long)(g_now_us - start));
    for (int i = 0; i < g_up.sends; i++) printf(i ? ",%d" : "%d", g_up.wait_ms[i]);   // the time left for each
    putchar('\n');
    finish();

    // A send that waited for room until the time ran out is a timeout, not a refusal; one that
    // failed at once is a refusal.
    reset_vm(0);
    g_cfg.send_cost_us = 1500000;
    g_cfg.send_fails_at = 0;
    CHECK(!vm_file_write(FILE_PATH, "x", 1, 1000, err, sizeof(err)), "the head is never queued");
    printf("@waited %s|%d|%d\n", err, g_up.sends, g_cancels);
    finish();
    reset_vm(0);
    g_cfg.send_cost_us = 1000;
    g_cfg.send_fails_at = 0;
    CHECK(!vm_file_write(FILE_PATH, "x", 1, 1000, err, sizeof(err)), "the head is refused");
    printf("@quick %s|%d|%d\n", err, g_up.sends, g_cancels);
    finish();

    // An answer that takes 400 ms, in time and not.
    reset_vm(0);
    g_cfg.delay_turns = 20;     // 20 turns of 20 ms
    fill(10000);
    CHECK(vm_file_write(FILE_PATH, g_data, 10000, 2000, err, sizeof(err)), "in time: %s", err);
    finish();
    reset_vm(0);
    g_cfg.delay_turns = 60;
    fill(10000);
    start = g_now_us;
    CHECK(!vm_file_write(FILE_PATH, g_data, 10000, 1000, err, sizeof(err)), "the answer is too late");
    printf("@total %s %d %d %lld\n", err, g_posts, g_cancels, (long long)(g_now_us - start));
    finish();

    // No time at all: nothing is sent.
    reset_vm(0);
    CHECK(!vm_file_write(FILE_PATH, "x", 1, 0, err, sizeof(err)), "no time");
    printf("@none %s %d\n", err, g_posts);
    reset_vm(0);
    CHECK(!vm_file_write(FILE_PATH, "x", 1, -5, err, sizeof(err)), "negative time");
    printf("@negative %s %d\n", err, g_posts);
    finish();
}

// The VM's answer has to be whole: a head that is never followed by the rest
// leaves the request open, and the session only takes three at a time.
static void case_write_waits_for_the_whole_reply(void) {
    char err[96];
    reset_vm(0);
    g_cfg.action[0] = POST_HEAD_ONLY;
    CHECK(!vm_file_write(FILE_PATH, "x", 1, 1000, err, sizeof(err)), "must wait for the end");
    printf("@head %s %d %d\n", err, g_posts, g_cancels);
    finish();
}

static void write_again(void) {
    char err[96];
    bool ok = vm_file_write("workspace/apps/x/other.bin", "y", 1, 1000, err, sizeof(err));
    printf("@second %d %s\n", ok, err);
}

static void case_write_one_at_a_time(void) {
    char err[96];
    reset_vm(0);
    g_turn_hook = write_again;      // while the first write waits for its answer
    g_cfg.delay_turns = 2;
    CHECK(vm_file_write(FILE_PATH, "x", 1, 1000, err, sizeof(err)), "first: %s", err);
    CHECK(g_posts == 1, "the second write sent %d requests", g_posts - 1);
    printf("@first %d\n", g_posts);
    finish();
}

static void write_during_read(void) {
    // A write from another task while a range of a file is out.
    char err[96];
    bool ok = vm_file_write(FILE_PATH, "zzz", 3, 1000, err, sizeof(err));
    printf("@write %d %s\n", ok, err);
}

// A reader and a writer don't share a reply: the write completes while the
// range is in flight, and neither takes the other's frames.
static void case_reader_and_writer_are_independent(void) {
    reset_vm(40 * 1024);
    vm_file_t *f = open_ok("big.bin", 0, VM_FILE_OWNER_IMAGE);
    g_turn_hook = write_during_read;
    sink_t s = {0};
    CHECK(vm_file_read_all(f, 1 << 20, sink_cb, &s, deadline()), "read_all: %s", error_of(f));
    print_sink(&s);
    printf("@posts %d\n", g_posts);
    vm_file_close(f);
    finish();
}

// The answer of a request the writer gave up on, which says the next write worked.
#define LATE_REPLY "{\"ok\":true,\"bytes_written\":1}"

static void late_write_frame(void) {
    g_late_cb(g_late_ctx, 200, (const uint8_t *)LATE_REPLY, strlen(LATE_REPLY), true);
}

static void case_write_late_frames_are_dropped(void) {
    char err[96];
    // The first write times out; its answer comes while the second is waiting for its own.
    reset_vm(0);
    g_cfg.action[0] = POST_STALL;
    CHECK(!vm_file_write(FILE_PATH, "x", 1, 200, err, sizeof(err)), "must time out");
    g_late_cb = g_post.cb;
    g_late_ctx = g_post.ctx;
    g_cfg.action[1] = POST_STALL;
    g_turn_hook = late_write_frame;
    CHECK(!vm_file_write(FILE_PATH, "x", 1, 200, err, sizeof(err)),
          "a frame of the first request must not answer the second");
    printf("@late %s\n", err);
    // And before the next request starts.
    g_late_cb(g_late_ctx, 200, (const uint8_t *)LATE_REPLY, strlen(LATE_REPLY), true);
    g_cfg.action[2] = POST_STALL;
    CHECK(!vm_file_write(FILE_PATH, "x", 1, 200, err, sizeof(err)), "must time out");
    finish();
}

// The head of the form is one send, so it has to fit in one: 4096 bytes, which every board's
// session takes. The path is in it twice when it is a bare name, and once, with the name,
// when it has folders.
static void case_write_path_limits(void) {
    char err[96];
    reset_vm(0);
    CHECK(vm_file_write("a", "x", 1, 5000, err, sizeof(err)), "calibration: %s", err);
    size_t overhead = g_up.send_len[0] - 2;     // the head without the path, and the name, which are one byte each
    finish();
    static char path[5000];
    for (size_t head = VM_FILE_WRITE_PIECE_BYTES; head <= VM_FILE_WRITE_PIECE_BYTES + 1; head++) {
        // folders, a slash, and a file name of 100 characters, which the head holds twice
        size_t folders = head - overhead - 1 - 200;
        memset(path, 'd', folders);
        path[folders] = '/';
        memset(path + folders + 1, 'f', 100);
        path[folders + 101] = '\0';
        reset_vm(0);
        bool ok = vm_file_write(path, "x", 1, 5000, err, sizeof(err));
        printf("@limit %zu %d %s %d %zu\n", head, ok, ok ? "-" : err, g_posts, g_up.sends ? g_up.send_len[0] : 0);
        if (ok) check_file((const uint8_t *)"x", 1);
        finish();
    }
}

static void case_write_filename_is_escaped(void) {
    char err[96];
    // The path goes as it is. In the file's name, as in a browser's form, a quote and a
    // line break are written as %22, %0D and %0A; the rest, a % and UTF-8 included, is not.
    static const char path[] = "dir/a\"b\r\nc d%41\xc3\xa9.txt";
    reset_vm(0);
    CHECK(vm_file_write(path, "x", 1, 5000, err, sizeof(err)), "write: %s", err);
    check_file((const uint8_t *)"x", 1);
    print_upload("name");
    finish();
}

static void case_write_bad_path(void) {
    char err[96];
    static const char *const refused[] = {"/etc/passwd", "/", "/workspace/a.txt", "..", "../x", "a/..", "a/../b",
                                          "a/b/../../c", "x/y/.."};
    static const char *const allowed[] = {"a..b", "..a", "a/..b/c", "a/b..", ".hidden", "a/./b", "a//b", "...",
                                          "a/.../b"};
    static const char *const nameless[] = {"dir/", "a/b/"};
    reset_vm(0);
    for (size_t i = 0; i < sizeof(refused) / sizeof(refused[0]); i++) {
        CHECK(!vm_file_write(refused[i], "x", 1, 1000, err, sizeof(err)), "%s must be refused", refused[i]);
        printf("@refused %s\t%s\n", refused[i], err);
    }
    for (size_t i = 0; i < sizeof(nameless) / sizeof(nameless[0]); i++) {
        CHECK(!vm_file_write(nameless[i], "x", 1, 1000, err, sizeof(err)), "%s names no file", nameless[i]);
        printf("@nameless %s\t%s\n", nameless[i], err);
    }
    CHECK(g_posts == 0, "a refused path sent %d requests", g_posts);
    for (size_t i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        CHECK(vm_file_write(allowed[i], "x", 1, 1000, err, sizeof(err)), "%s must be written: %s", allowed[i], err);
        printf("@allowed %s\n", allowed[i]);
    }
    CHECK(g_posts == (int)(sizeof(allowed) / sizeof(allowed[0])), "%d requests", g_posts);
    finish();
}

static void case_write_short_count(void) {
    char err[96];
    // The VM says it wrote one byte too few, and one too many.
    reset_vm(0);
    g_cfg.delta = -1;
    CHECK(!write_ten_thousand(err, sizeof(err)), "a short count must fail the write");
    SUMMARY("short", err);
    finish();
    reset_vm(0);
    g_cfg.delta = 1;
    CHECK(!write_ten_thousand(err, sizeof(err)), "a long count must fail the write");
    SUMMARY("long", err);
    finish();

    // A 200 that doesn't say, or says it in a string, has not said.
    reset_vm(0);
    g_cfg.style = REPLY_NO_COUNT;
    CHECK(!write_ten_thousand(err, sizeof(err)), "no count must fail the write");
    SUMMARY("none", err);
    finish();
    reset_vm(0);
    g_cfg.style = REPLY_COUNT_STRING;
    CHECK(!write_ten_thousand(err, sizeof(err)), "a count in a string must fail the write");
    SUMMARY("string", err);
    finish();

    // The count is found wherever the reply's frames break it, among other fields, with
    // white space around it, after the key's text in a string, and as the end of the reply.
    static const reply_style_t styles[] = {REPLY_COUNT, REPLY_BUSY, REPLY_COUNT_LAST};
    static const size_t frames[] = {0, 1, 3, 7};
    for (size_t s = 0; s < sizeof(styles) / sizeof(styles[0]); s++) {
        for (size_t i = 0; i < sizeof(frames) / sizeof(frames[0]); i++) {
            reset_vm(0);
            g_cfg.style = styles[s];
            g_cfg.frame_bytes = frames[i];
            CHECK(write_ten_thousand(err, sizeof(err)), "reply style %d in frames of %zu: %s", (int)styles[s],
                  frames[i], err);
            finish();
        }
    }
    printf("@counted %zu\n", sizeof(styles) / sizeof(styles[0]) * sizeof(frames) / sizeof(frames[0]));

    // The key's start twice over is still the key; a count of more digits than a number holds is a
    // count of more than was written.
    reset_vm(0);
    g_cfg.style = REPLY_STUTTER;
    CHECK(write_ten_thousand(err, sizeof(err)), "the key after a false start: %s", err);
    finish();
    reset_vm(0);
    g_cfg.style = REPLY_HUGE;
    CHECK(!write_ten_thousand(err, sizeof(err)), "a count of 28 digits is not 10000");
    SUMMARY("huge", err);
    finish();

    // And a wrong count is wrong in any of them.
    reset_vm(0);
    g_cfg.style = REPLY_BUSY;
    g_cfg.frame_bytes = 1;
    g_cfg.delta = -3;
    CHECK(!write_ten_thousand(err, sizeof(err)), "a short count in pieces");
    SUMMARY("short_busy", err);
    finish();

    // No bytes is a count of none.
    reset_vm(0);
    CHECK(vm_file_write(FILE_PATH, NULL, 0, 1000, err, sizeof(err)), "an empty file: %s", err);
    finish();
}

static void case_write_boundary_collision(void) {
    char err[96];
    // The writer makes its boundary from four words of the random number generator, and the
    // harness's are a sequence: four small writes show the boundaries it will make, in order.
    reset_vm(0);
    char learned[4][96];
    for (int i = 0; i < 4; i++) {
        CHECK(vm_file_write("a.txt", "x", 1, 5000, err, sizeof(err)), "learning %d: %s", i, err);
        snprintf(learned[i], sizeof(learned[i]), "%s", g_up.boundary);
        for (int j = 0; j < i; j++) CHECK(strcmp(learned[i], learned[j]) != 0, "the boundaries repeat");
    }
    int learned_posts = g_posts;

    // The data holds the first one: the writer makes another, and the file's bytes still go through.
    static uint8_t data[600];
    for (size_t i = 0; i < sizeof(data); i++) data[i] = pat(i);
    memcpy(data + 100, learned[0], strlen(learned[0]));
    g_random_calls = 0;
    CHECK(vm_file_write("a.txt", data, sizeof(data), 5000, err, sizeof(err)), "collision in the data: %s", err);
    CHECK(strcmp(g_up.boundary, learned[1]) == 0, "the boundary is %s, not the second", g_up.boundary);
    CHECK(g_random_calls == 8, "%u words of randomness", g_random_calls);
    check_file(data, sizeof(data));
    printf("@second %d\n", g_posts - learned_posts);

    // Or the path does.
    char path[160];
    snprintf(path, sizeof(path), "dir/%s.txt", learned[0]);
    g_random_calls = 0;
    CHECK(vm_file_write(path, "x", 1, 5000, err, sizeof(err)), "collision in the path: %s", err);
    CHECK(strcmp(g_up.boundary, learned[1]) == 0, "the boundary is %s, not the second", g_up.boundary);
    check_file((const uint8_t *)"x", 1);

    // The first two are in the data: the third.
    memcpy(data + 200, learned[1], strlen(learned[1]));
    g_random_calls = 0;
    CHECK(vm_file_write("a.txt", data, sizeof(data), 5000, err, sizeof(err)), "two collisions: %s", err);
    CHECK(strcmp(g_up.boundary, learned[2]) == 0, "the boundary is %s, not the third", g_up.boundary);
    check_file(data, sizeof(data));

    // All four are: no request is made.
    memcpy(data + 300, learned[2], strlen(learned[2]));
    memcpy(data + 400, learned[3], strlen(learned[3]));
    g_random_calls = 0;
    int posts = g_posts;
    CHECK(!vm_file_write("a.txt", data, sizeof(data), 5000, err, sizeof(err)), "four collisions");
    printf("@exhausted %s|%d|%u\n", err, g_posts - posts, g_random_calls);
    finish();
}

static void case_write_arguments(void) {
    char err[96];
    reset_vm(0);
    CHECK(!vm_file_write(NULL, "x", 1, 1000, err, sizeof(err)), "no path");
    printf("@null %s\n", err);
    CHECK(!vm_file_write("", "x", 1, 1000, err, sizeof(err)), "empty path");
    printf("@empty %s\n", err);
    CHECK(!vm_file_write("dir/", "x", 1, 1000, err, sizeof(err)), "a folder");
    printf("@folder %s\n", err);
    CHECK(!vm_file_write("f", NULL, 5, 1000, err, sizeof(err)), "bytes from nowhere");
    printf("@nodata %s\n", err);
    CHECK(g_posts == 0, "a refused call sent %d requests", g_posts);
    // A write that works leaves nothing in err of what was there.
    char stale[96] = "left from before";
    CHECK(vm_file_write("f", "x", 1, 1000, stale, sizeof(stale)), "write: %s", stale);
    printf("@cleared [%s]\n", stale);
    // No room for the reason is no reason to crash.
    CHECK(!vm_file_write("f", NULL, 5, 1000, NULL, 0), "no err");
    CHECK(!vm_file_write("f", NULL, 5, 1000, err, 0), "err_len 0");
    char tiny[6];
    CHECK(!vm_file_write("f", NULL, 5, 1000, tiny, sizeof(tiny)), "tiny err");
    printf("@tiny %s\n", tiny);
    g_cfg.status[1] = 500;
    g_cfg.reply = "boom";
    CHECK(!vm_file_write("f", "x", 1, 1000, NULL, 0), "an error with nowhere to say it");
    g_cfg.status[2] = 500;
    g_cfg.reply = "boom";
    char small[8];
    CHECK(!vm_file_write("f", "x", 1, 1000, small, sizeof(small)), "an error with little room to say it");
    printf("@small %s\n", small);
    finish();
}

static void case_write_memory(void) {
    char err[96];
    // The lock for the reply (made the first time a write is made, so this goes first).
    reset_vm(0);
    g_fail_mutex = true;
    CHECK(!vm_file_write(FILE_PATH, "x", 1, 1000, err, sizeof(err)), "no lock");
    printf("@lock %s %d\n", err, g_posts);
    finish();
    // The buffer for the head of the form.
    reset_vm(0);
    g_fail_alloc = 1;
    CHECK(!vm_file_write(FILE_PATH, "x", 1, 1000, err, sizeof(err)), "no buffer");
    printf("@alloc %s %d\n", err, g_posts);
    finish();
    // The buffer is in PSRAM where there is some: it takes nothing the session needs.
    reset_vm(0);
    fill(10000);
    CHECK(vm_file_write(FILE_PATH, g_data, 10000, 10000, err, sizeof(err)), "write: %s", err);
#if CONFIG_SPIRAM
    CHECK(g_min_dma_free == 200000, "the writer used %zu bytes of DMA RAM", 200000 - g_min_dma_free);
#endif
    finish();
}

#if WITH_IMAGE_FETCH
// ---- Cases with image_fetch.c ------------------------------------------------

// The screen is 8 pixels wide and 4096 tall, as in link_image_fetch_harness.c:
// the daemon's file is raw rows of 16 bytes, which the screen must get in
// order, none lost or twice.
#define SCREEN_WIDTH 8
#define SCREEN_HEIGHT 4096
#define ROW_BYTES (SCREEN_WIDTH * 2)

void *esp_crt_bundle_attach = NULL;

// The HTTP client is never used: these files are on the VM.
#define NO_HTTP() CHECK(false, "a file from the VM used the HTTP client")
esp_http_client_handle_t esp_http_client_init(const esp_http_client_config_t *config) {
    (void)config;
    NO_HTTP();
    return NULL;
}
esp_err_t esp_http_client_open(esp_http_client_handle_t c, int write_len) {
    (void)c; (void)write_len;
    NO_HTTP();
    return ESP_FAIL;
}
long long esp_http_client_fetch_headers(esp_http_client_handle_t c) {
    (void)c;
    NO_HTTP();
    return 0;
}
int esp_http_client_get_status_code(esp_http_client_handle_t c) {
    (void)c;
    NO_HTTP();
    return 0;
}
esp_err_t esp_http_client_set_redirection(esp_http_client_handle_t c) {
    (void)c;
    NO_HTTP();
    return ESP_FAIL;
}
esp_err_t esp_http_client_get_url(esp_http_client_handle_t c, char *url, int len) {
    (void)c; (void)url; (void)len;
    NO_HTTP();
    return ESP_FAIL;
}
esp_err_t esp_http_client_set_timeout_ms(esp_http_client_handle_t c, int timeout_ms) {
    (void)c; (void)timeout_ms;
    NO_HTTP();
    return ESP_FAIL;
}
int esp_http_client_read(esp_http_client_handle_t c, char *buffer, int len) {
    (void)c; (void)buffer; (void)len;
    NO_HTTP();
    return -1;
}
bool esp_http_client_is_complete_data_received(esp_http_client_handle_t c) {
    (void)c;
    NO_HTTP();
    return false;
}
esp_err_t esp_http_client_close(esp_http_client_handle_t c) {
    (void)c;
    NO_HTTP();
    return ESP_FAIL;
}
esp_err_t esp_http_client_cleanup(esp_http_client_handle_t c) {
    (void)c;
    NO_HTTP();
    return ESP_FAIL;
}
const char *esp_err_to_name(esp_err_t err) {
    (void)err;
    return "ESP_FAIL";
}

JRESULT jd_prepare(JDEC *jd, UINT (*infunc)(JDEC *, BYTE *, UINT), void *pool, UINT sz_pool,
                   void *dev) {
    (void)jd; (void)infunc; (void)pool; (void)sz_pool; (void)dev;
    return JDR_FMT1;
}
JRESULT jd_decomp(JDEC *jd, UINT (*outfunc)(JDEC *, void *, JRECT *), BYTE scale) {
    (void)jd; (void)outfunc; (void)scale;
    return JDR_FMT1;
}

static bool g_fail_task;
static unsigned g_stack;
static int g_rows_drawn, g_draw_dones;
static size_t g_next_byte;

static int g_screen_width = SCREEN_WIDTH;

bool led_status_display_info(int *width, int *height) {
    *width = g_screen_width;
    *height = SCREEN_HEIGHT;
    return true;
}

bool led_status_draw_rect(int x, int y, int w, int h, const uint16_t *pixels) {
    (void)x;
    (void)y;
    const uint8_t *b = (const uint8_t *)pixels;
    for (int i = 0; i < w * h * 2; i++, g_next_byte++) {
        CHECK(b[i] == pat(g_next_byte), "byte %zu drawn as %u", g_next_byte, b[i]);
    }
    g_rows_drawn += h;
    return true;
}

void led_status_draw_done(void) {
    g_draw_dones++;
}

unsigned uxTaskGetStackHighWaterMark(TaskHandle_t task) {
    (void)task;
    return 1234;
}

// The download task runs to completion where it is made, and holds its stack.
BaseType_t xTaskCreate(TaskFunction_t task, const char *name, unsigned stack_depth, void *params,
                       unsigned priority, TaskHandle_t *out_handle) {
    (void)name;
    (void)priority;
    (void)out_handle;
    if (g_fail_task) return pdFALSE;
    g_stack = stack_depth;
    g_internal_used += stack_depth;
    record_dma();
    task(params);
    g_internal_used -= stack_depth;
    return pdPASS;
}

void vTaskDelete(TaskHandle_t task) {
    (void)task;
}

static image_fetch_result_t g_result;
static char g_message[128];
static bool g_done;

static void on_done(const image_fetch_result_t *result, void *user) {
    (void)user;
    g_result = *result;
    snprintf(g_message, sizeof(g_message), "%s", result->message ? result->message : "");
    g_result.message = g_message;
    g_done = true;
}

// Starts a download of a file on the VM; when it starts, it has finished too.
static bool draw_vm(const char *path, const char **code, const char **message) {
    g_done = false;
    g_rows_drawn = g_draw_dones = 0;
    g_next_byte = 0;
    bool started = image_fetch_start_vm(path, 0, on_done, NULL, code, message);
    if (started) CHECK(g_done, "the download did not finish");
    return started;
}

static void reset_image(size_t file_len) {
    reset_vm(file_len);
    g_fail_task = false;
    g_screen_width = SCREEN_WIDTH;
}

static void case_image_refused_while_an_install_reads(void) {
    reset_image(5 * ROW_BYTES);
    vm_file_t *f = open_ok("workspace/apps/x/main.lua", 0, VM_FILE_OWNER_INSTALL);
    const char *code = NULL, *message = NULL;
    CHECK(!draw_vm("pic.raw", &code, &message), "a download started while an install was reading");
    printf("@refused %s | %s\n", code, message);
    CHECK(!g_gets && !g_posts, "a refused download sent a request");
    vm_file_close(f);

    // Refused, it left nothing held: the same download goes through.
    CHECK(draw_vm("pic.raw", &code, &message), "refused after the install: %s", message);
    CHECK(g_result.ok && g_rows_drawn == 5, "the picture: %s", g_message);
    printf("@drawn %d %zu %d\n", g_rows_drawn, g_result.bytes, g_draw_dones);
    finish();
}

// While a download reads, an install is told so, and a second download is
// stopped by the download's own flag, which the HTTP downloads share.
static void during_the_download(void) {
    const char *code = NULL, *message = NULL;
    vm_file_t *f = vm_file_open("workspace/apps/x/main.lua", 0, VM_FILE_OWNER_INSTALL, &code, &message);
    printf("@install %d %s | %s\n", f != NULL, code ? code : "", message ? message : "");
    if (f) vm_file_close(f);
    bool started = image_fetch_start_vm("other.raw", 0, on_done, NULL, &code, &message);
    printf("@again %d %s | %s\n", started, code ? code : "", message ? message : "");
    started = image_fetch_start("http://host/x", 0, on_done, NULL, &code, &message);
    printf("@http %d %s | %s\n", started, code ? code : "", message ? message : "");
}

static void case_install_refused_while_an_image_downloads(void) {
    reset_image(5 * ROW_BYTES);
    g_turn_hook = during_the_download;
    const char *code = NULL, *message = NULL;
    CHECK(draw_vm("pic.raw", &code, &message), "the download: %s", message);
    CHECK(g_result.ok && g_rows_drawn == 5, "the picture: %s", g_message);
    // Done, it is the install's.
    vm_file_t *f = open_ok("workspace/apps/x/main.lua", 0, VM_FILE_OWNER_INSTALL);
    vm_file_close(f);
    finish();
}

static void case_image_stalls_in_a_later_range(void) {
    // Two ranges, the second never answered: the download that had drawn the first
    // says it timed out, not that the image is bad.
    reset_image(2 * MAX_BODY);
    g_stall_after = 1;
    const char *code = NULL, *message = NULL;
    CHECK(draw_vm("pic.raw", &code, &message), "start: %s", message);
    CHECK(!g_result.ok, "the second range never came");
    printf("@late %s | %s | rows %d | cancels %d | bytes %zu | done %d\n", g_result.code, g_message,
           g_rows_drawn, g_cancels, g_result.bytes, g_draw_dones);
    finish();

    // The first range failing is told by the reader, and draws nothing, not even the screen's end.
    reset_image(5 * ROW_BYTES);
    g_error_status = 404;
    CHECK(draw_vm("nope.raw", &code, &message), "start: %s", message);
    printf("@first %s | %s | rows %d | done %d | bytes %zu\n", g_result.code, g_message, g_rows_drawn,
           g_draw_dones, g_result.bytes);
    finish();

    reset_image(5 * ROW_BYTES);
    g_stall_after = 0;
    CHECK(draw_vm("silent.raw", &code, &message), "start: %s", message);
    printf("@silent %s | %s | cancels %d\n", g_result.code, g_message, g_cancels);
    finish();
}

// The task's stack and the decoder's pool each need a block of their own, and
// the reader counts only their sum.
static void case_image_needs_a_block_for_its_stack(void) {
    const char *code = NULL, *message = NULL;
    for (size_t block = 6143; block <= 6144; block++) {
        reset_image(5 * ROW_BYTES);
        g_largest_block = block;
        bool started = draw_vm("pic.raw", &code, &message);
        printf("@block %zu %d %s | %s | %d\n", block, started, started ? "" : code,
               started ? "" : message, g_gets);
        finish();
    }
}

// The decoder's pool, or four rows of the screen, need a block of their own too, and on a wide
// screen the rows are the larger: 1000 pixels are 8000 bytes, over the stack's 6144.
static void case_image_needs_a_block_for_its_drawing(void) {
    const char *code = NULL, *message = NULL;
    for (size_t block = 7999; block <= 8000; block++) {
        reset_image(0);
        g_screen_width = 1000;
        g_error_status = 404;       // the file isn't there: nothing is drawn, the download only ends
        g_largest_block = block;
        bool started = draw_vm("pic.raw", &code, &message);
        printf("@wide %zu %d %s | %s\n", block, started, started ? "" : code, started ? "" : message);
        finish();
    }
}

static void case_image_task_that_will_not_start(void) {
    reset_image(5 * ROW_BYTES);
    g_fail_task = true;
    const char *code = NULL, *message = NULL;
    CHECK(!draw_vm("pic.raw", &code, &message), "no task");
    printf("@task %s | %s | %d\n", code, message, g_gets);
    g_fail_task = false;
    CHECK(draw_vm("pic.raw", &code, &message), "the reader and the flag are free: %s", message);
    finish();
}
#endif

// ---- Main --------------------------------------------------------------------

typedef struct {
    const char *name;
    void (*run)(void);
    void (*run_with_path)(int argc, char **argv);
} test_case_t;

#define CASE(name) {#name, case_##name, NULL}
#define CASE_PATH(name) {#name, NULL, case_##name}

static const test_case_t cases[] = {
    CASE(read_all_streams_ranges_to_sink),
    CASE(sink_abort_cancels),
    CASE(max_exceeded),
    CASE(second_reader_is_busy),
    CASE(short_206_is_the_end),
    CASE(416_at_the_end),
    CASE(200_to_a_ranged_get),
    CASE(session_drop_mid_read),
    CASE(http_error),
    CASE(read_timeout),
    CASE(late_frames_are_dropped),
    CASE(read_in_small_pieces),
    CASE(read_then_read_all),
    CASE(open_refusals),
    CASE(open_counts_keep),
    CASE(open_chunk_under_pressure),
    CASE(request_path_is_encoded),
    CASE_PATH(write_upload_multipart),
    CASE(write_zero_4096_4097),
    CASE(write_http_error),
    CASE(write_session_drop),
    CASE(write_timeout),
    CASE(write_waits_for_the_whole_reply),
    CASE(write_one_at_a_time),
    CASE(reader_and_writer_are_independent),
    CASE(write_late_frames_are_dropped),
    CASE(write_path_limits),
    CASE(write_filename_is_escaped),
    CASE(write_bad_path),
    CASE(write_short_count),
    CASE(write_boundary_collision),
    CASE(write_arguments),
    CASE(write_memory),
#if WITH_IMAGE_FETCH
    CASE(image_refused_while_an_install_reads),
    CASE(install_refused_while_an_image_downloads),
    CASE(image_stalls_in_a_later_range),
    CASE(image_needs_a_block_for_its_stack),
    CASE(image_needs_a_block_for_its_drawing),
    CASE(image_task_that_will_not_start),
#endif
};

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <case> [path]\n", argv[0]);
        return 2;
    }
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        if (strcmp(argv[1], cases[i].name) != 0) continue;
        if (cases[i].run) cases[i].run();
        else cases[i].run_with_path(argc, argv);
        puts("ok");
        return 0;
    }
    fprintf(stderr, "no case named %s\n", argv[1]);
    return 2;
}
