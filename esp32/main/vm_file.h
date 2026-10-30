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

// Files on the user's VM, over the daemon requests of the Noise session
// (CONFIG_HOMEHUB_DAEMON_REQUESTS): GET /fs/raw to read one, POST /fs/upload to
// write one. display.draw_file and app installs read through the same reader.
// (A gadget's credential reaches those two routes; /fs/read and /fs/write are
// the owner's, and answer a gadget with HTTP 403.)
//
//   const char *code, *message;
//   vm_file_t *f = vm_file_open(path, 0, VM_FILE_OWNER_INSTALL, &code, &message);
//   if (!f) return fail(code, message);
//   if (!vm_file_read_all(f, max, sink, user, deadline_us)) text = vm_file_error(f, msg, sizeof(msg));
//   vm_file_close(f);

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// A file on the VM being read, a range at a time. There is one reader across
// the firmware: while one is open, another vm_file_open is refused.
typedef struct vm_file vm_file_t;

// What an owner calls itself in vm_file_open, and so what a refused opener is
// told holds the reader. Another tag works, and is reported without its name.
#define VM_FILE_OWNER_IMAGE   "an image download"
#define VM_FILE_OWNER_INSTALL "an app install"

// Gets each range of the file: data is valid only during the call. Returns
// false to stop reading.
typedef bool (*vm_file_sink_t)(void *user, const uint8_t *data, size_t len);

// Opens the reader for `path` (relative to the user's home on the VM; leading
// slashes are ignored): allocates the range buffer and checks that there is
// memory for it. Sends nothing: the first read asks for the first range.
//
// `keep` is the internal and DMA RAM, in bytes, that the caller's own work
// still needs beyond what the Noise session and this request reserve:
// image_fetch passes its task's stack and the memory it draws with, an install
// on a PSRAM task passes 0. It is one sum; a caller whose allocations each need
// a block of their own checks heap_caps_get_largest_free_block() after the open.
//
// Returns NULL with a code and a message (both literals) if it can't:
//   busy            another reader is open: "the VM file reader is busy (<owner>)"
//   out_of_memory   "failed to allocate"
//   invalid_params  "path names no file"
// `owner` is one of the VM_FILE_OWNER_ tags (a literal: it is kept, not copied,
// until the reader closes).
vm_file_t *vm_file_open(const char *path, size_t keep, const char *owner,
                        const char **code, const char **message);

// Reads up to len bytes (at most INT_MAX), asking for the next range when the
// buffer is used up, and waits for all of them unless the file ends first.
// Returns the count, 0 once the file is over, or -1 if a range can't be had
// (vm_file_error says why; close the reader then). deadline_us is absolute, in
// esp_timer_get_time() microseconds, and bounds every request this call makes.
int vm_file_read(vm_file_t *f, uint8_t *buf, size_t len, int64_t deadline_us);

// Reads the rest of the file, handing each range to `sink` as it arrives and
// asking for the next one only after the sink returns, so a slow sink holds the
// session still. Returns true once the whole file has gone to the sink. Returns
// false if a range can't be had, or the file is larger than `max` bytes (no
// range that would take it past `max` reaches the sink), or the sink returns
// false: the last two read "larger than <max> bytes" in vm_file_error.
bool vm_file_read_all(vm_file_t *f, size_t max, vm_file_sink_t sink, void *user,
                      int64_t deadline_us);

// Why the last read failed, in words the user can read: "could not send the
// request to the VM", "download timed out", "the VM ignored Range and sent the
// whole file", "the VM sent a frame too large for this board", "the VM
// connection dropped", "VM answered HTTP <status>", "larger than <max> bytes".
// A text that has a number in it is written into msg; the rest are literals.
// Never NULL.
const char *vm_file_error(vm_file_t *f, char *msg, size_t msg_len);

// Bytes the VM has sent so far, and the size of each request for a range.
size_t vm_file_bytes(const vm_file_t *f);
size_t vm_file_chunk(const vm_file_t *f);

// True once a request of this reader ended because its deadline came.
bool vm_file_timed_out(const vm_file_t *f);

// Releases the buffer and the reader. NULL is fine. The handle is not valid
// afterwards.
void vm_file_close(vm_file_t *f);

// POST /fs/upload takes a file as multipart/form-data, sent as one streamed
// request: the text parts path, overwrite and create_parent, then the file's
// bytes, which go out in sends of at most this many bytes (the head of the form
// is one send too). It is what a Muse board's session takes in one send even
// without PSRAM; the Cardputer ADV and AI Passport take 2048, and run no apps.
#define VM_FILE_WRITE_PIECE_BYTES 4096

// Writes data to `path` on the VM (UTF-8, relative to the user's home: not
// absolute, no ".." folder), making its parent folders. One request per file:
// the VM writes a temporary file and renames it into place, so the file is
// replaced whole, or left as it was if the write fails, and there is no append.
// No bytes make an empty file. timeout_ms bounds the whole call. Nothing is sent
// again.
//
// Returns true once the VM answers 2xx and says it wrote all len bytes (the
// bytes_written of its JSON). Otherwise false, with `err` (if there is room)
// saying why:
//   "VM answered HTTP <status>", and after it, if it fits, ": " and the VM's
//     own words (the text of its reply, which for a JSON error is the JSON)
//   "the VM wrote <n> of <len> bytes", "the VM didn't say how many bytes it wrote"
//   "write timed out", "the VM connection dropped",
//   "the VM sent a frame too large for this board",
//   "could not send the request to the VM"
//   "the VM file writer is busy" (one write at a time)
//   "vm_file_write: bad path" (absolute, or with a ".." folder),
//   "path names no file" (empty, or ending in a slash), "path is too long"
//   "no data to write" (len but no data), "out of memory"
//   "couldn't pick a boundary the data doesn't contain"
//
// Blocks until the VM answers, so it must not run on the Noise session's task,
// which is what delivers the answers.
bool vm_file_write(const char *path, const void *data, size_t len, int timeout_ms,
                   char *err, size_t err_len);

#ifdef __cplusplus
}
#endif
