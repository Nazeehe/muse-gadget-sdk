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

/*
 * Runs a function on a host thread with exactly `size` bytes of stack below its first frame, for the stack tests of code
 * that runs on the gadget's small internal stacks (R13.1: the console's 3,584 bytes; R13.7: the I/O worker's 6,144).
 * Task 5's tests give pthread_attr_setstack the exact region, but macOS and glibc refuse a stack under
 * PTHREAD_STACK_MIN (16 KB). So the thread gets a larger region above a PROT_NONE page, and calls the function with
 * all but `size` bytes of that region taken by an alloca: a frame that goes past `size` lands in the guard page, and
 * the harness dies (SIGSEGV or SIGBUS) rather than passing quietly. The alloca is calibrated once, with a call that does
 * nothing, so that the function's caller has its frame exactly `size` bytes above the guard page. The region is painted
 * first, and the deepest byte the function changed gives its high-water mark.
 *
 * Build such a harness without the sanitizers (apps_host.build(..., sanitize=False)), which fatten frames. Host frames
 * are bigger than the S3's (Task 5 measured 1.5 to 1.7 times on Xtensa's), so a pass here is a pass with room to spare
 * there, and the device's own high-water mark (uxTaskGetStackHighWaterMark) is the authority.
 */

#include <alloca.h>
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define EXACT_PAINT 0xA5
#define EXACT_SLACK (64 * 1024) /* the region's room above `size`: the thread's own start, and the alloca */

typedef struct {
    void (*fn)(void *arg);
    void *arg;
    uintptr_t want;  /* where exact_call's frame should be: lo + size */
    size_t shift;    /* the calibration: how far below `want` it lands with no correction */
    uintptr_t entry; /* where it was */
} exact_run_t;

static size_t exact_shift = SIZE_MAX; /* calibrated once: a constant of this binary's code */

static void exact_nothing(void *arg) {
    (void)arg;
}

static __attribute__((noinline)) void exact_call(exact_run_t *r) {
    r->entry = (uintptr_t)__builtin_frame_address(0);
    r->fn(r->arg);
    __asm__ volatile("" ::: "memory"); /* no tail call: the function runs below this frame */
}

static void *exact_thread(void *arg) {
    exact_run_t *r = arg;
    uintptr_t here = (uintptr_t)__builtin_frame_address(0);
    size_t pad = here > r->want + r->shift ? (size_t)(here - r->want - r->shift) : 0;
    void *taken = alloca(pad);
    __asm__ volatile("" : : "r"(taken) : "memory"); /* keep the alloca */
    exact_call(r);
    return NULL;
}

/* One run on a fresh region. Returns 0, with *entry_off (exact_call's frame above the guard) and *used (bytes below it
 * that changed), or an errno-like code when the thread couldn't be made. */
static int exact_once(void (*fn)(void *), void *arg, size_t size, size_t shift, size_t *entry_off, size_t *used) {
    const size_t page = (size_t)sysconf(_SC_PAGESIZE);
    const size_t total = (size + EXACT_SLACK + page - 1) / page * page;
    unsigned char *map = mmap(NULL, page + total, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (map == MAP_FAILED) {
        return 1;
    }
    unsigned char *lo = map + page;
    int rc = mprotect(map, page, PROT_NONE);
    memset(lo, EXACT_PAINT, total);
    exact_run_t r = {fn, arg, (uintptr_t)lo + size, shift, 0};
    pthread_attr_t attr;
    pthread_t thread;
    pthread_attr_init(&attr);
    if (rc == 0) {
        rc = pthread_attr_setstack(&attr, lo, total);
    }
    if (rc == 0) {
        rc = pthread_create(&thread, &attr, exact_thread, &r);
    }
    if (rc == 0) {
        pthread_join(thread, NULL);
    }
    pthread_attr_destroy(&attr);
    size_t untouched = 0;
    while (untouched < total && lo[untouched] == EXACT_PAINT) {
        untouched++;
    }
    *entry_off = r.entry > (uintptr_t)lo ? (size_t)(r.entry - (uintptr_t)lo) : 0;
    *used = *entry_off > untouched ? *entry_off - untouched : 0;
    munmap(map, page + total);
    return rc;
}

/*
 * Runs fn(arg) with exactly size bytes (a multiple of 16) of stack below its caller's frame, and returns the bytes it
 * used; *exact (may be NULL) says whether the frame landed exactly there. A fn that needs more dies on the guard page.
 * Returns 0 when no thread could be made.
 */
static size_t on_exact_stack(void (*fn)(void *), void *arg, size_t size, int *exact) {
    size_t entry_off = 0, used = 0;
    if (exact_shift == SIZE_MAX) {
        if (exact_once(exact_nothing, NULL, size, 0, &entry_off, &used) != 0 || entry_off > size) {
            return 0;
        }
        exact_shift = size - entry_off;
    }
    if (exact_once(fn, arg, size, exact_shift, &entry_off, &used) != 0) {
        return 0;
    }
    if (exact != NULL) {
        *exact = entry_off == size;
    }
    return used;
}
