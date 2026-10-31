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
 * Runs apps_jpeg_decode, the apps' JPEG decoder, over the fake TJpgDec (tests/fake_tjpgd.h has its test format), for
 * tests/test_muse_apps_jpeg.py, which reads what this prints and checks it.
 *
 *   muse_apps_jpeg_harness decode MAX_SIDE CIRCLE REFUSE_FROM < picture
 *     decodes stdin (an exact copy of it in the heap, so a read past its end is AddressSanitizer's) with max_side MAX_SIDE and
 *     circle CIRCLE (0 or 1); the heap refuses every growth from the REFUSE_FROM'th on (0: none). Prints
 *       ok W H LEN SCALE RECTS          the decode, the scale tjpgd was given, the rectangles it gave out
 *       check ok                        apps_image_check of the block (or its message in place of "ok")
 *       block HEX                       the block, every byte
 *     or
 *       error MESSAGE
 *       out NULL|SET LEN W H            what the outputs were left at
 *     and last, after the block is freed: held BYTES (what the counting heap still holds: 0, or something leaked).
 *   muse_apps_jpeg_harness nulls
 *     w, h and err NULL: the decode still works, and fails cleanly. Exits 0 when it holds.
 *
 * The port is a counting allocator: LeakSanitizer doesn't run on macOS (X13), so "held" is the leak check.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps_jpeg.h"
#include "apps_pkg.h"
#include "apps_port.h"
#include "fake_tjpgd.h"

typedef union {
    size_t size;
    max_align_t align;
} block_t;

static size_t held;            /* bytes handed out and not freed */
static long growths;           /* growth requests so far */
static long refuse_from = -1;  /* refuse every growth request from this one on (counted in growths); -1: none */

void *apps_port_realloc(void *ptr, size_t n) {
    block_t *b = ptr != NULL ? (block_t *)ptr - 1 : NULL;
    size_t old = b != NULL ? b->size : 0;
    if (n > old) {
        growths++;
        if (refuse_from >= 0 && growths >= refuse_from) {
            return NULL;
        }
    }
    block_t *nb = realloc(b, sizeof *nb + n);
    if (nb == NULL) {
        return NULL;
    }
    nb->size = n;
    held = held - old + n;
    return nb + 1;
}

void apps_port_free(void *ptr) {
    if (ptr != NULL) {
        block_t *b = (block_t *)ptr - 1;
        held -= b->size;
        free(b);
    }
}

/* stdin, whole, in a block of exactly its size (malloc's, not the counted heap). */
static uint8_t *read_stdin(size_t *len) {
    size_t cap = 4096, n = 0;
    uint8_t *buf = malloc(cap);
    for (size_t got; buf != NULL && (got = fread(buf + n, 1, cap - n, stdin)) > 0;) {
        n += got;
        if (n == cap) {
            cap *= 2;
            buf = realloc(buf, cap);
        }
    }
    if (buf == NULL) {
        fputs("out of memory in the harness\n", stderr);
        exit(2);
    }
    uint8_t *exact = malloc(n != 0 ? n : 1);
    if (exact == NULL) {
        fputs("out of memory in the harness\n", stderr);
        exit(2);
    }
    memcpy(exact, buf, n);
    free(buf);
    *len = n;
    return exact;
}

static int decode(int max_side, bool circle, long refuse) {
    size_t len;
    uint8_t *jpg = read_stdin(&len);
    refuse_from = refuse > 0 ? refuse : -1;
    uint8_t *out = (uint8_t *)&out;  /* not NULL, so a failure has to set it */
    size_t out_len = 12345;
    int w = -1, h = -1;
    char err[160] = "untouched";
    bool ok = apps_jpeg_decode(len > 0 ? jpg : NULL, len, max_side, circle, &out, &out_len, &w, &h, err, sizeof err);
    refuse_from = -1;
    free(jpg);
    if (ok) {
        printf("ok %d %d %zu %d %d\n", w, h, out_len, fake_tjpgd_scale, fake_tjpgd_rects);
        char why[160];
        uint16_t cw, ch;
        bool checked = apps_image_check(out, out_len, &cw, &ch, why, sizeof why);
        printf("check %s\n", checked && cw == w && ch == h ? "ok" : checked ? "size differs" : why);
        if (err[0] != '\0') {
            printf("err on success: %s\n", err);
        }
        fputs("block ", stdout);
        for (size_t i = 0; i < out_len; i++) {
            printf("%02x", out[i]);
        }
        putchar('\n');
        apps_port_free(out);
    } else {
        printf("error %s\n", err);
        printf("out %s %zu %d %d\n", out == NULL ? "NULL" : "SET", out_len, w, h);
    }
    printf("held %zu\n", held);
    return 0;
}

static int failures;
#define CHECK(cond, ...)                                          \
    do {                                                          \
        if (!(cond)) {                                            \
            failures++;                                           \
            fprintf(stderr, "FAIL line %d: %s: ", __LINE__, #cond); \
            fprintf(stderr, __VA_ARGS__);                         \
            fputc('\n', stderr);                                  \
        }                                                         \
    } while (0)

/* A fake picture of w x h, baseline, MCUs of 8 x 8. */
static size_t fake_picture(uint8_t *buf, unsigned w, unsigned h, char mode) {
    const uint8_t head[14] = {'F', 'J', 'P', 'G', (uint8_t)mode, 1, 1, 0, (uint8_t)w, (uint8_t)(w >> 8), (uint8_t)h, (uint8_t)(h >> 8),
                              0, 0};
    memcpy(buf, head, sizeof head);
    memcpy(buf + sizeof head, "END!", 4);
    return sizeof head + 4;
}

static void nulls(void) {
    uint8_t jpg[32];
    size_t len = fake_picture(jpg, 20, 10, 'B');
    uint8_t *out = NULL;
    size_t out_len = 0;
    CHECK(apps_jpeg_decode(jpg, len, 466, true, &out, &out_len, NULL, NULL, NULL, 0), "w, h and err may be NULL");
    CHECK(out != NULL && out_len == 12 + 20 * 10 * 3, "the block: %zu bytes", out_len);
    apps_port_free(out);
    len = fake_picture(jpg, 20, 10, 'P');
    out = (uint8_t *)&out;
    CHECK(!apps_jpeg_decode(jpg, len, 466, false, &out, &out_len, NULL, NULL, NULL, 0), "a failure with nowhere to say why");
    CHECK(out == NULL && out_len == 0, "the outputs are cleared");
    char err[8];
    len = fake_picture(jpg, 20, 10, 'P');
    CHECK(!apps_jpeg_decode(jpg, len, 466, false, &out, &out_len, NULL, NULL, err, sizeof err), "a short err");
    CHECK(strcmp(err, "unsuppo") == 0, "err is cut to its room: '%s'", err);
    CHECK(held == 0, "%zu bytes held", held);
}

int main(int argc, char **argv) {
    if (argc == 5 && strcmp(argv[1], "decode") == 0) {
        return decode(atoi(argv[2]), atoi(argv[3]) != 0, atol(argv[4]));
    }
    if (argc == 2 && strcmp(argv[1], "nulls") == 0) {
        nulls();
        return failures != 0 ? 1 : 0;
    }
    fprintf(stderr, "usage: %s decode MAX_SIDE CIRCLE REFUSE_FROM < picture | %s nulls\n", argv[0], argv[0]);
    return 2;
}
