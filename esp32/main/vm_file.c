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

#include "vm_file.h"
#include "noise_control.h"
#include "noise_tunnel_internal.h"
#include "sdkconfig.h"

#if CONFIG_HOMEHUB_DAEMON_REQUESTS

#include <ctype.h>
#include <limits.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

static const char *TAG = "link.vm_file";

// A file from the VM is read in Range requests of at most
// noise_ctrl_req_max_body() bytes into one buffer, each sent once the last has
// been taken: the daemon may send a reply in one frame, which must fit the
// session's receive buffers, and the session can't be paused while a slow
// reader catches up. The buffer goes in PSRAM where there is some.
#if CONFIG_SPIRAM
#define VM_CAPS             (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#else
#define VM_CAPS             MALLOC_CAP_8BIT
#endif
#define VM_POLL_MS          20
#define VM_PATH_PREFIX      "/fs/raw/"
// Smaller ranges cost round trips: only shrink without PSRAM when the maximum
// will not fit, and refuse rather than going below this.
#define VM_MIN_CHUNK        (2 * 1024)
// Leave room for the range allocation's heap header, alignment and optional
// poisoning canaries; otherwise a range sized to all available bytes fails
// the post-allocation check by its own allocator overhead.
#define VM_CHUNK_ALLOC_BYTES 32
// The GET verb, Range header and separators in noise_ctrl_req_open's copy,
// beyond the path. Enough for two unsigned 32-bit offsets in the header.
#define VM_REQUEST_BYTES    64
// Left for Wi-Fi, lwIP and the Noise session: never open a reader that would
// go below it.
#define VM_RESERVE_BYTES    (4 * 1024)

// Byte-addressable internal RAM. MALLOC_CAP_INTERNAL alone also counts
// IRAM that only takes 32-bit accesses, which malloc() never hands out.
#define BYTE_CAPS (MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)

static size_t internal_free(void) {
    return heap_caps_get_free_size(BYTE_CAPS);
}

// ---- Reading a file ----------------------------------------------------------

// The reply to the one GET /fs/raw in flight, written into the reader's
// buffer on the Noise session task. Frames of an earlier request, cancelled
// or timed out, carry an older generation and are dropped, so the reading
// task never waits on them and they never touch the buffer.
static struct {
    SemaphoreHandle_t lock;
    uint32_t gen;
    uint8_t *buf;        // set only while a request is in flight
    size_t cap, len;
    int status;          // HTTP status, or a noise_ctrl_req_cb failure below 0
    bool done;
    bool complete;       // the whole body arrived
    bool too_large;      // more than was asked for
} s_vm;

// How one request for a range ended.
typedef struct {
    size_t len;
    int status;
    bool complete, too_large, not_sent;
} vm_reply_t;

struct vm_file {
    char *url;           // the request path, VM_PATH_PREFIX and the file's
    int64_t deadline_us;
    // The buffer for one range, what is in it, and where the next range starts.
    uint8_t *body;
    size_t chunk, body_len, body_pos;
    size_t next_offset;
    size_t bytes;        // what the VM has sent
    bool asked;          // the first range has been requested
    bool eof;
    bool timed_out;
    // Why the last read failed, for vm_file_error: the reply of the range that
    // couldn't be had, or a limit (max) that ended it.
    bool failed, limited;
    vm_reply_t last;
    size_t max;
};

// The one reader. s_owner is who has it, the owner's tag, and NULL while it is
// free: taking it is a compare and swap, so two tasks can't both have it, and
// a task that loses learns the winner's tag in the same step.
static struct vm_file s_file;
static _Atomic(const char *) s_owner;

static void vm_on_frame(void *ctx, int status, const uint8_t *data, size_t len, bool end) {
    xSemaphoreTake(s_vm.lock, portMAX_DELAY);
    if ((uint32_t)(uintptr_t)ctx == s_vm.gen && !s_vm.done) {
        if (status) s_vm.status = status;
        // Only a 206's body is the range, or a 200's the whole file.
        bool ok = true;
        if (len && (s_vm.status == 200 || s_vm.status == 206)) {
            if (len > s_vm.cap - s_vm.len) {
                s_vm.too_large = true;
                ok = false;
            } else {
                memcpy(s_vm.buf + s_vm.len, data, len);
                s_vm.len += len;
            }
        }
        s_vm.complete = end && status >= 0 && ok;
        s_vm.done = end || status < 0 || !ok;
    }
    xSemaphoreGive(s_vm.lock);
}

static bool vm_done(void) {
    xSemaphoreTake(s_vm.lock, portMAX_DELAY);
    bool done = s_vm.done;
    xSemaphoreGive(s_vm.lock);
    return done;
}

// GETs the next f->chunk bytes into f->body and waits for the whole reply or
// the deadline.
static void vm_get_range(vm_file_t *f, vm_reply_t *out) {
    memset(out, 0, sizeof(*out));
    char range[48];
    snprintf(range, sizeof(range), "bytes=%u-%u", (unsigned)f->next_offset,
             (unsigned)(f->next_offset + f->chunk - 1));
    const char *headers[] = {"Range", range, NULL};
    xSemaphoreTake(s_vm.lock, portMAX_DELAY);
    uint32_t token = ++s_vm.gen;
    s_vm.buf = f->body;
    s_vm.cap = f->chunk;
    s_vm.len = 0;
    s_vm.status = 0;
    s_vm.done = s_vm.complete = s_vm.too_large = false;
    xSemaphoreGive(s_vm.lock);

    int64_t id = noise_ctrl_req_open("GET", f->url, headers, true, vm_on_frame,
                                     (void *)(uintptr_t)token);
    while (id && !vm_done()) {
        if (esp_timer_get_time() >= f->deadline_us) {
            f->timed_out = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(VM_POLL_MS));
    }

    xSemaphoreTake(s_vm.lock, portMAX_DELAY);
    s_vm.gen++;  // drop anything still to come for this request
    s_vm.buf = NULL;
    out->len = s_vm.len;
    out->status = s_vm.status;
    out->complete = s_vm.complete;
    out->too_large = s_vm.too_large;
    xSemaphoreGive(s_vm.lock);
    out->not_sent = !id;
    if (id && !out->complete) noise_ctrl_req_cancel(id);
}

// Reads the next range into f->body. False if it can't.
static bool vm_next_range(vm_file_t *f, vm_reply_t *out) {
    if (!f->asked) {
        f->asked = true;
        ESP_LOGI(TAG, "GET %s, %u bytes at a time", f->url, (unsigned)f->chunk);
    }
    f->body_len = f->body_pos = 0;
    vm_get_range(f, out);
    if (!out->complete) return false;
    if (out->status == 416) {
        // Starts at the end: an empty file, or the last range was full.
        f->eof = true;
        return true;
    }
    // A daemon ignoring Range sends the whole file, fine if it fits.
    if (out->status != 206 && !(out->status == 200 && f->next_offset == 0)) return false;
    f->body_len = out->len;
    f->next_offset += out->len;
    f->bytes += out->len;
    if (out->status == 200 || out->len < f->chunk) f->eof = true;
    return true;
}

static void vm_range_failed(vm_file_t *f, const vm_reply_t *r) {
    ESP_LOGW(TAG, "%s at %u: status %d, %u bytes%s", f->url, (unsigned)f->next_offset,
             r->status, (unsigned)r->len, r->complete ? "" : ", incomplete");
    f->last = *r;
    f->failed = true;
}

// GET /fs/raw/ and the path, without its leading slashes, percent-encoded
// but for its slashes. NULL if out of memory.
static char *vm_request_path(const char *path) {
    while (*path == '/') path++;
    size_t prefix = strlen(VM_PATH_PREFIX);
    char *out = malloc(prefix + strlen(path) * 3 + 1);
    if (!out) return NULL;
    memcpy(out, VM_PATH_PREFIX, prefix);
    char *p = out + prefix;
    for (const unsigned char *c = (const unsigned char *)path; *c; c++) {
        if (isalnum(*c) || strchr("-._~/", *c)) {
            *p++ = (char)*c;
        } else {
            *p++ = '%';
            *p++ = "0123456789ABCDEF"[*c >> 4];
            *p++ = "0123456789ABCDEF"[*c & 15];
        }
    }
    *p = '\0';
    return out;
}

// Allocate the largest safe range once. Future allocations must leave Noise's
// TX margin: the caller's own (`caller_keep`: for a download, the task stack
// and either decoder or raw rows) and the queued GET.
static bool vm_allocate_body(vm_file_t *f, size_t caller_keep) {
    size_t request_bytes = strlen(f->url) + VM_REQUEST_BYTES;
    size_t keep = NOISE_TX_DMA_RESERVE_BYTES + caller_keep
                  + request_bytes + VM_RESERVE_BYTES;
    size_t available = internal_free();
    size_t dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    if (available > dma_free) available = dma_free;
    if (available < keep) return false;

    size_t chunk = noise_ctrl_req_max_body();
#if !CONFIG_SPIRAM
    if (available - keep < VM_CHUNK_ALLOC_BYTES) return false;
    size_t room = available - keep - VM_CHUNK_ALLOC_BYTES;
    if (chunk > room) chunk = room;
    size_t block = heap_caps_get_largest_free_block(VM_CAPS);
    if (chunk > block) chunk = block;
    if (chunk < VM_MIN_CHUNK) return false;
#endif
    f->body = heap_caps_malloc(chunk, VM_CAPS);
    if (!f->body) return false;
    f->chunk = chunk;

    // Recheck after the actual allocations, including their heap overhead.
    // A concurrent allocation may also have consumed the sampled headroom.
    return internal_free() >= keep
        && heap_caps_get_free_size(MALLOC_CAP_DMA) >= keep
        && heap_caps_get_largest_free_block(BYTE_CAPS) >= request_bytes;
}

// What the tag of the reader's owner reads as to the opener it refused. The
// texts are literals, so a refused caller can keep the pointer.
static const char *busy_message(const char *owner) {
    if (strcmp(owner, VM_FILE_OWNER_IMAGE) == 0) {
        return "the VM file reader is busy (" VM_FILE_OWNER_IMAGE ")";
    }
    if (strcmp(owner, VM_FILE_OWNER_INSTALL) == 0) {
        return "the VM file reader is busy (" VM_FILE_OWNER_INSTALL ")";
    }
    return "the VM file reader is busy";
}

// The handle of the open reader, or false for NULL, a closed reader and
// anything else.
static bool is_open(const vm_file_t *f) {
    return f == &s_file && atomic_load(&s_owner) != NULL;
}

vm_file_t *vm_file_open(const char *path, size_t keep, const char *owner,
                        const char **code, const char **message) {
    const char *unused;
    if (!code) code = &unused;
    if (!message) message = &unused;
    if (!path || !path[strspn(path, "/")]) {
        *code = "invalid_params";
        *message = "path names no file";
        return NULL;
    }
    if (!owner) owner = "";
    const char *held_by = NULL;
    if (!atomic_compare_exchange_strong(&s_owner, &held_by, owner)) {
        *code = "busy";
        *message = busy_message(held_by);
        return NULL;
    }

    // The reader is ours; only its owner touches what it holds.
    vm_file_t *f = &s_file;
    memset(f, 0, sizeof(*f));
    if ((!s_vm.lock && !(s_vm.lock = xSemaphoreCreateMutex()))
        || !(f->url = vm_request_path(path))
        || !vm_allocate_body(f, keep)) {
        free(f->body);
        free(f->url);
        memset(f, 0, sizeof(*f));
        atomic_store(&s_owner, NULL);
        *code = "out_of_memory";
        *message = "failed to allocate";
        return NULL;
    }
    return f;
}

// Read up to `len` bytes, waiting for all of them unless the file ends.
// Returns the count read, or -1 if a range can't be had.
int vm_file_read(vm_file_t *f, uint8_t *buf, size_t len, int64_t deadline_us) {
    if (!is_open(f)) return -1;
    if (len > INT_MAX) len = INT_MAX;  // the count comes back as an int
    f->deadline_us = deadline_us;
    f->failed = f->limited = false;
    size_t have = 0;
    while (have < len) {
        if (f->body_pos == f->body_len) {
            if (f->eof) break;
            vm_reply_t r;
            if (!vm_next_range(f, &r)) {
                vm_range_failed(f, &r);
                return -1;
            }
            continue;
        }
        size_t n = f->body_len - f->body_pos;
        if (n > len - have) n = len - have;
        memcpy(buf + have, f->body + f->body_pos, n);
        f->body_pos += n;
        have += n;
    }
    return (int)have;
}

bool vm_file_read_all(vm_file_t *f, size_t max, vm_file_sink_t sink, void *user,
                      int64_t deadline_us) {
    if (!is_open(f) || !sink) return false;
    f->deadline_us = deadline_us;
    f->failed = f->limited = false;
    for (;;) {
        // Checked as each range arrives, before the sink sees it: the sink is
        // given no more than max bytes of the file in all.
        if (f->bytes > max) {
            f->limited = true;
            f->max = max;
            return false;
        }
        size_t n = f->body_len - f->body_pos;
        if (n) {
            const uint8_t *data = f->body + f->body_pos;
            f->body_pos = f->body_len;
            if (!sink(user, data, n)) {
                f->limited = true;
                f->max = max;
                return false;
            }
        }
        if (f->eof) return true;
        vm_reply_t r;
        if (!vm_next_range(f, &r)) {
            vm_range_failed(f, &r);
            return false;
        }
    }
}

// What a request that didn't end in a usable reply tells the user.
// `timeout_text` is what the deadline reads as, for the caller's kind of work, and `words`
// (or NULL) the VM's own for an HTTP status, which follow it if they fit. msg is written
// for the one text that has a number in it.
static const char *reply_failure(bool not_sent, bool timed_out, bool too_large, int status,
                                 const char *timeout_text, const char *words, char *msg,
                                 size_t msg_len) {
    if (not_sent) return "could not send the request to the VM";
    if (timed_out) return timeout_text;
    if (too_large) return "the VM ignored Range and sent the whole file";
    if (status == NOISE_CTRL_REQ_TOO_LARGE) return "the VM sent a frame too large for this board";
    if (status < 0) return "the VM connection dropped";
    if (!msg || !msg_len) return "the VM answered with an error";
    int n = words ? snprintf(msg, msg_len, "VM answered HTTP %d: %s", status, words) : -1;
    if (n < 0 || (size_t)n >= msg_len) snprintf(msg, msg_len, "VM answered HTTP %d", status);
    return msg;
}

const char *vm_file_error(vm_file_t *f, char *msg, size_t msg_len) {
    if (!is_open(f)) return "download failed";
    if (f->limited) {
        if (!msg || !msg_len) return "larger than the limit";
        snprintf(msg, msg_len, "larger than %u bytes", (unsigned)f->max);
        return msg;
    }
    if (f->failed) {
        const vm_reply_t *r = &f->last;
        return reply_failure(r->not_sent, f->timed_out, r->too_large, r->status,
                             "download timed out", NULL, msg, msg_len);
    }
    return "download failed";
}

size_t vm_file_bytes(const vm_file_t *f) {
    return is_open(f) ? f->bytes : 0;
}

size_t vm_file_chunk(const vm_file_t *f) {
    return is_open(f) ? f->chunk : 0;
}

bool vm_file_timed_out(const vm_file_t *f) {
    return is_open(f) && f->timed_out;
}

void vm_file_close(vm_file_t *f) {
    if (!is_open(f)) return;
    free(f->body);
    free(f->url);
    memset(f, 0, sizeof(*f));
    atomic_store(&s_owner, NULL);
}

// ---- Writing a file ----------------------------------------------------------

#define WR_PATH "/fs/upload"
// The boundary of a form: "muse" and 128 random bits in hex. The path and the data
// are checked for it, and if either holds it another is made.
#define WR_BOUNDARY_PREFIX "muse"
#define WR_BOUNDARY_CHARS  (sizeof(WR_BOUNDARY_PREFIX) - 1 + 32)
#define WR_BOUNDARY_TRIES  4

// The reply to the one POST /fs/upload in flight, kept as s_vm keeps the GET's: by
// generation, so the frames of a request that timed out or was cancelled are dropped
// whenever they come. Only what the writer needs is kept: the status, the start of the
// body for the log and the error, and the number after "bytes_written", found as the
// body goes by.
static struct {
    SemaphoreHandle_t lock;
    uint32_t gen;
    int status;          // HTTP status, or a noise_ctrl_req_cb failure below 0
    size_t seen;         // reply body bytes
    char text[128];      // the start of them, the printable part
    size_t kept;
    uint64_t written;    // the number after "bytes_written":
    uint8_t scan;        // where wr_scan is in finding it
    uint8_t key_at;
    bool have_written;
    bool done;
    bool complete;       // the whole reply arrived
} s_wr;
static atomic_bool s_writing;

enum { SCAN_KEY, SCAN_COLON, SCAN_NUMBER, SCAN_DIGITS, SCAN_FOUND };
#define WR_KEY "\"bytes_written\""

static bool json_space(uint8_t c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Finds "bytes_written": and its number in the reply, a byte at a time, wherever its
// frames break it. The key has no quote but its first and last, so after a mismatch
// only a quote can start it again.
static void wr_scan(uint8_t c) {
    switch (s_wr.scan) {
    case SCAN_KEY:
        if (c == (uint8_t)WR_KEY[s_wr.key_at]) {
            if (++s_wr.key_at == sizeof(WR_KEY) - 1) s_wr.scan = SCAN_COLON;
            return;
        }
        break;
    case SCAN_COLON:
        if (json_space(c)) return;
        if (c == ':') {
            s_wr.scan = SCAN_NUMBER;
            return;
        }
        break;
    case SCAN_NUMBER:
        if (json_space(c)) return;
        if (c >= '0' && c <= '9') {
            s_wr.written = (uint64_t)(c - '0');
            s_wr.scan = SCAN_DIGITS;
            return;
        }
        break;
    case SCAN_DIGITS:
        if (c >= '0' && c <= '9') {
            uint64_t digit = (uint64_t)(c - '0');
            s_wr.written = s_wr.written > (UINT64_MAX - digit) / 10 ? UINT64_MAX : s_wr.written * 10 + digit;
        } else {
            s_wr.have_written = true;
            s_wr.scan = SCAN_FOUND;
        }
        return;
    default:
        return;
    }
    // Not the key, or not a number after it: look on from here.
    s_wr.scan = SCAN_KEY;
    s_wr.key_at = c == '"' ? 1 : 0;
}

// How the writer's request ended, and what the VM said.
typedef struct {
    int status;
    bool complete, not_sent, timed_out, have_written;
    uint64_t written;
    unsigned sends;
    size_t seen, kept;
    char text[sizeof(s_wr.text)];
} wr_reply_t;

static void wr_on_frame(void *ctx, int status, const uint8_t *data, size_t len, bool end) {
    xSemaphoreTake(s_wr.lock, portMAX_DELAY);
    if ((uint32_t)(uintptr_t)ctx == s_wr.gen && !s_wr.done) {
        if (status) s_wr.status = status;
        for (size_t i = 0; i < len; i++) {
            // The VM's words reach the log and the error: only the printable ones,
            // white space as a space.
            uint8_t c = data[i];
            if (s_wr.kept < sizeof(s_wr.text) - 1) {
                s_wr.text[s_wr.kept++] = json_space(c) ? ' ' : c >= 0x20 && c < 0x7f ? (char)c : '?';
            }
            wr_scan(c);
        }
        s_wr.text[s_wr.kept] = '\0';
        s_wr.seen += len;
        if (end && s_wr.scan == SCAN_DIGITS) {  // the number is the end of the reply
            s_wr.have_written = true;
            s_wr.scan = SCAN_FOUND;
        }
        s_wr.complete = end && status >= 0;
        s_wr.done = end || status < 0;
    }
    xSemaphoreGive(s_wr.lock);
}

static bool wr_done(void) {
    xSemaphoreTake(s_wr.lock, portMAX_DELAY);
    bool done = s_wr.done;
    xSemaphoreGive(s_wr.lock);
    return done;
}

// Queues one send of the request. False if it can't, with why in `out`: the time is out,
// the VM has answered already (the rest is not wanted), or the session won't take it.
static bool wr_send(int64_t id, const void *bytes, size_t len, bool last, int64_t deadline_us,
                    wr_reply_t *out) {
    if (wr_done()) return false;
    int64_t left_ms = (deadline_us - esp_timer_get_time()) / 1000;
    if (left_ms < 1) {
        out->timed_out = true;
        return false;
    }
    if (!noise_ctrl_req_send(id, bytes, len, last, (int)left_ms)) {
        // A send that waited for room until the time ran out is a timeout, not a refusal.
        if (esp_timer_get_time() >= deadline_us) out->timed_out = true;
        else out->not_sent = true;
        return false;
    }
    out->sends++;
    return true;
}

// POSTs the form as one request and waits for the whole reply or the deadline. The request
// is a stream: `head`, the file in pieces of VM_FILE_WRITE_PIECE_BYTES, then `tail`. Each
// send waits for room in the session's queue, within the call's time; if the VM answers
// before the end (it turns the path away, say) the rest is not sent.
static void wr_upload(const char *content_type, const char *head, size_t head_len,
                      const uint8_t *data, size_t len, const char *tail, size_t tail_len,
                      int64_t deadline_us, wr_reply_t *out) {
    memset(out, 0, sizeof(*out));
    const char *headers[] = {"Content-Type", content_type, NULL};
    xSemaphoreTake(s_wr.lock, portMAX_DELAY);
    uint32_t token = ++s_wr.gen;
    s_wr.status = 0;
    s_wr.seen = s_wr.kept = 0;
    s_wr.text[0] = '\0';
    s_wr.written = 0;
    s_wr.scan = SCAN_KEY;
    s_wr.key_at = 0;
    s_wr.have_written = false;
    s_wr.done = s_wr.complete = false;
    xSemaphoreGive(s_wr.lock);

    int64_t id = noise_ctrl_req_open("POST", WR_PATH, headers, false, wr_on_frame,
                                     (void *)(uintptr_t)token);
    if (id) {
        bool more = wr_send(id, head, head_len, false, deadline_us, out);
        for (size_t offset = 0; more && offset < len;) {
            size_t n = len - offset < VM_FILE_WRITE_PIECE_BYTES ? len - offset : VM_FILE_WRITE_PIECE_BYTES;
            more = wr_send(id, data + offset, n, false, deadline_us, out);
            offset += n;
        }
        if (more) wr_send(id, tail, tail_len, true, deadline_us, out);
    }
    while (id && !out->not_sent && !out->timed_out && !wr_done()) {
        if (esp_timer_get_time() >= deadline_us) {
            out->timed_out = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(VM_POLL_MS));
    }

    xSemaphoreTake(s_wr.lock, portMAX_DELAY);
    s_wr.gen++;  // drop anything still to come for this request
    out->status = s_wr.status;
    out->complete = s_wr.complete;
    out->have_written = s_wr.have_written;
    out->written = s_wr.written;
    out->seen = s_wr.seen;
    out->kept = s_wr.kept;
    memcpy(out->text, s_wr.text, sizeof(out->text));
    xSemaphoreGive(s_wr.lock);
    out->not_sent = out->not_sent || !id;
    if (id && !out->complete) noise_ctrl_req_cancel(id);
}

// ---- The form

// Appends to a buffer, or only counts when there is none.
typedef struct {
    char *out;
    size_t n;
} emit_t;

static void emit(emit_t *e, const char *s, size_t len) {
    if (e->out) memcpy(e->out + e->n, s, len);
    e->n += len;
}

static void emit_str(emit_t *e, const char *s) {
    emit(e, s, strlen(s));
}

// A text part of the form: its delimiter, its name, a blank line and its value.
static void emit_field(emit_t *e, const char *boundary, const char *name, const char *value) {
    emit_str(e, "--");
    emit_str(e, boundary);
    emit_str(e, "\r\nContent-Disposition: form-data; name=\"");
    emit_str(e, name);
    emit_str(e, "\"\r\n\r\n");
    emit_str(e, value);
    emit_str(e, "\r\n");
}

// Everything before the file's bytes: the text parts the VM needs first, then the headers
// of the file part. The VM goes by `path`; the file's name is only a name, and as in a
// browser's form a quote or a line break in it is written as %22, %0D or %0A.
static void emit_head(emit_t *e, const char *boundary, const char *path, const char *name) {
    emit_field(e, boundary, "path", path);
    emit_field(e, boundary, "overwrite", "true");
    emit_field(e, boundary, "create_parent", "true");
    emit_str(e, "--");
    emit_str(e, boundary);
    emit_str(e, "\r\nContent-Disposition: form-data; name=\"file\"; filename=\"");
    for (const char *c = name; *c; c++) {
        if (*c == '"') emit_str(e, "%22");
        else if (*c == '\r') emit_str(e, "%0D");
        else if (*c == '\n') emit_str(e, "%0A");
        else emit(e, c, 1);
    }
    emit_str(e, "\"\r\nContent-Type: application/octet-stream\r\n\r\n");
}

static bool contains(const void *hay, size_t hay_len, const char *needle) {
    size_t n = strlen(needle);
    const uint8_t *p = hay;
    for (size_t i = 0; i + n <= hay_len; i++) {
        if (p[i] == (uint8_t)needle[0] && memcmp(p + i, needle, n) == 0) return true;
    }
    return false;
}

static void make_boundary(char *out) {
    uint32_t a = esp_random(), b = esp_random(), c = esp_random(), d = esp_random();
    snprintf(out, WR_BOUNDARY_CHARS + 1, WR_BOUNDARY_PREFIX "%08x%08x%08x%08x", (unsigned)a, (unsigned)b,
             (unsigned)c, (unsigned)d);
}

// Not absolute, and no ".." folder in it: the VM may allow more, this is for the caller's
// mistakes. A ".." inside a name is a name.
static bool path_stays_inside(const char *path) {
    if (path[0] == '/') return false;
    for (const char *segment = path;;) {
        size_t n = strcspn(segment, "/");
        if (n == 2 && segment[0] == '.' && segment[1] == '.') return false;
        if (!segment[n]) return true;
        segment += n + 1;
    }
}

static bool write_failed(char *err, size_t err_len, const char *why) {
    if (err && err_len) snprintf(err, err_len, "%s", why);
    return false;
}

// The VM's own words after a refusal, if the whole reply was kept: its text with the
// white space at the ends cut off. NULL if there is none, or only the start of it.
static const char *reply_words(wr_reply_t *r) {
    if (r->seen != r->kept) return NULL;
    char *t = r->text;
    while (*t == ' ') t++;
    size_t n = strlen(t);
    while (n && t[n - 1] == ' ') t[--n] = '\0';
    return n ? t : NULL;
}

static bool write_all(const char *path, const uint8_t *data, size_t len, int timeout_ms,
                      char *err, size_t err_len) {
    int64_t deadline_us = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    const char *name = strrchr(path, '/');
    name = name ? name + 1 : path;

    // The head of the form goes in one send, so it has to fit in one: 4096 bytes, which a
    // Muse board's session takes. A boundary of the right length stands in for the real one.
    char boundary[WR_BOUNDARY_CHARS + 1];
    memset(boundary, 'x', WR_BOUNDARY_CHARS);
    boundary[WR_BOUNDARY_CHARS] = '\0';
    emit_t size = {NULL, 0};
    emit_head(&size, boundary, path, name);
    if (size.n > VM_FILE_WRITE_PIECE_BYTES) return write_failed(err, err_len, "path is too long");
    if (esp_timer_get_time() >= deadline_us) return write_failed(err, err_len, "write timed out");
    if (!s_wr.lock && !(s_wr.lock = xSemaphoreCreateMutex())) {
        return write_failed(err, err_len, "out of memory");
    }

    // A boundary that is nowhere in the path or the data: with 128 random bits one is the
    // first, unless the data was made to hold it.
    bool unique = false;
    for (unsigned tries = 0; tries < WR_BOUNDARY_TRIES && !unique; tries++) {
        make_boundary(boundary);
        unique = !contains(path, strlen(path), boundary) && !contains(data, len, boundary);
    }
    if (!unique) return write_failed(err, err_len, "couldn't pick a boundary the data doesn't contain");

    char *head = heap_caps_malloc(size.n, VM_CAPS);
    if (!head) return write_failed(err, err_len, "out of memory");
    emit_t put = {head, 0};
    emit_head(&put, boundary, path, name);
    char tail[48];
    int tail_len = snprintf(tail, sizeof(tail), "\r\n--%s--\r\n", boundary);
    char content_type[96];
    snprintf(content_type, sizeof(content_type), "multipart/form-data; boundary=%s", boundary);

    wr_reply_t reply;
    wr_upload(content_type, head, size.n, data, len, tail, (size_t)tail_len, deadline_us, &reply);
    free(head);

    // Done when the VM answers 2xx and says it wrote every byte.
    bool answered = reply.complete && reply.status >= 200 && reply.status < 300;
    bool ok = false;
    if (!answered) {
        ESP_LOGW(TAG, "write: status %d, %u reply bytes: %s", reply.status, (unsigned)reply.seen, reply.text);
        const char *why = reply_failure(reply.not_sent, reply.timed_out, false, reply.status,
                                        "write timed out", reply_words(&reply), err, err_len);
        if (why != err) write_failed(err, err_len, why);
    } else if (!reply.have_written) {
        write_failed(err, err_len, "the VM didn't say how many bytes it wrote");
    } else if (reply.written != len) {
        if (err && err_len) {
            snprintf(err, err_len, "the VM wrote %u of %u bytes", (unsigned)reply.written, (unsigned)len);
        }
    } else {
        ok = true;
        ESP_LOGI(TAG, "wrote %u bytes in %u sends", (unsigned)len, reply.sends);
    }
    return ok;
}

bool vm_file_write(const char *path, const void *data, size_t len, int timeout_ms,
                   char *err, size_t err_len) {
    if (err && err_len) err[0] = '\0';
    if (!path || !*path) return write_failed(err, err_len, "path names no file");
    if (!path_stays_inside(path)) return write_failed(err, err_len, "vm_file_write: bad path");
    if (path[strlen(path) - 1] == '/') return write_failed(err, err_len, "path names no file");
    if (len && !data) return write_failed(err, err_len, "no data to write");
    bool expected = false;
    if (!atomic_compare_exchange_strong(&s_writing, &expected, true)) {
        return write_failed(err, err_len, "the VM file writer is busy");
    }
    bool ok = write_all(path, data, len, timeout_ms, err, err_len);
    atomic_store(&s_writing, false);
    return ok;
}

#endif  // CONFIG_HOMEHUB_DAEMON_REQUESTS
