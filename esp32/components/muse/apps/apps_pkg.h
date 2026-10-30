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
 * What a Muse app's package has to be: ids, paths, the manifest, typed files and LVGL images. Every install
 * validates its package with this module first, and the app list reads manifests through it. Portable C
 * (but for one allocation, below): the manifest is read with the pull reader (apps_json.h), so no tree is
 * built, and nothing here calls Lua.
 *
 * Ids and paths. An id is ^[a-z][a-z0-9_]{1,23}$, 2 to APPS_ID_MAX characters, and equals its folder's name.
 * A path is what the manifest lists, relative to the app's folder, and it is checked before it is read or
 * written: [a-z0-9_./-], 1 to APPS_PATH_MAX characters, and
 * - no empty segment (so no leading or trailing '/' and no "//") and no segment that starts with '.' (so no
 *   ".", ".." or hidden file); no ".." anywhere and no segment that ends in '.';
 * - why so strict: FAT reads "a/./b.lua", "a//b.lua" and "a./b.lua" as the file "a/b.lua" (it skips doubled
 *   separators and trailing dots), which comparing strings can't see, so two entries of the manifest could
 *   be one file. The letters are lower case for the same reason.
 * Files are typed by their end: .lua (Lua source text), .json (must parse) or .bin (an LVGL image, below).
 * Anything else is refused: an app has no way to read raw bytes.
 *
 * The manifest is a JSON object of at most APPS_MANIFEST_MAX bytes, read by apps_manifest_parse:
 *   id           required text: a valid id, the folder's name
 *   name         required text, 1 to 24 bytes
 *   version      optional text, 0 to 23 bytes, informational
 *   api          required whole number: 1
 *   main         optional path of a .lua file; "main.lua" if left out
 *   files        optional list of paths (.lua, .json, .bin): every file but main
 *   permissions  optional list of sound alarm store keep_awake (net, secrets and muse aren't there yet)
 *   memory_kb    optional whole number, 64 to min(1024, heap_max_kb); 256 (or heap_max_kb, if less) if left out
 *   messages     optional object of at most 16 event names to descriptions, each under 200 bytes
 * Any other key is ignored, with a warning. A key that appears twice has its last value, as in json.decode.
 * Characters are bytes throughout (a 24-byte name may be 8 characters of Chinese). A whole number is a JSON
 * integer: "api": 1.0 and 1e0 aren't one.
 *
 * Messages. A manifest that fails puts one message in err, cut to err_len on a character boundary, and each
 * starts "manifest: ". A string of the right type is quoted as it was read ('%s'); a value of the wrong type,
 * or a string too long to read (over 255 bytes, or with \u0000 in it), as its JSON text (the inside of a
 * string), cut to 32 bytes on a character boundary with "..." after the cut: "id": 5 reads id '5', and
 * "files": ["a.lua", true] reads files[1] 'true'.
 *   manifest: <the reader's message>        malformed JSON, as apps_json.h lists them, with the position:
 *                                           "manifest: unexpected '}' at byte 41". A document that isn't
 *                                           an object fails the same way, at byte 1.
 *   manifest: manifest.json is larger than 8 KB
 *   manifest: id '%s' must match ^[a-z][a-z0-9_]{1,23}$ and the folder name '%s'     (a missing id is '')
 *   manifest: name must be 1 to 24 characters                              (also when missing or not text)
 *   manifest: name has a control character                                 (C0, DEL or C1: U+0000 to U+001F,
 *                                                                          U+007F, U+0080 to U+009F)
 *   manifest: version must be text of at most 23 characters, like "1.0.0"
 *   manifest: version has a control character
 *   manifest: this gadget supports api 1 (manifest says api %s)           (the number, or the JSON text)
 *   manifest: api is missing (this gadget supports api 1)
 *   manifest: main '%s' is not an allowed path (a-z 0-9 _ . / -, no '..', at most 64 characters)
 *   manifest: main must name a .lua file
 *   manifest: files must be a list of paths, like ["util.lua"]
 *   manifest: files[%d] '%s' is not an allowed path (a-z 0-9 _ . / -, no '..', at most 64 characters)
 *   manifest: files[%d] '%s' has an unsupported type (use .lua, .json or .bin)
 *   manifest: more than 32 files                                           (main and the rest, no repeats)
 *   manifest: permissions must be a list, like ["sound"] (valid: sound, alarm, store, keep_awake)
 *   manifest: permission '%s' is not available in this firmware yet (api 1)       (net, secrets, muse)
 *   manifest: unknown permission '%s' (valid: sound, alarm, store, keep_awake)
 *   manifest: memory_kb must be a whole number from 64 to %d
 *   manifest: messages must map event names to descriptions (at most 16, each under 200 characters)
 *   manifest: out of memory                                                (messages_json didn't fit)
 * files[%d] counts the manifest's own list from 0. The first problem in the order id, name, version, api,
 * main, files, permissions, memory_kb, messages is the one reported, and a required key that is missing
 * only after every key that is there has passed.
 * A warning, for a key that isn't one of those: "unknown key '%s' ignored", with " (did you mean '%s'?)" when
 * a manifest key is near (apps_suggest), the key shown as apps_text_shown shows text from outside (a control
 * character as '?') and never cut. They are joined with "; " in apps_manifest_t's warnings, one a key; one that
 * doesn't fit whole in what is left is dropped, never cut.
 * An image's: "image: not an LVGL v9 image file (use LVGLImage.py --ofmt BIN)", "image: compressed images
 * aren't supported", "image: flags 0x%04x aren't supported (only premultiplied alpha)", "image: %ux%u has no
 * pixels", "image: %ux%u is larger than the 466x466 screen", "image: colour format 0x%02x isn't RGB565,
 * RGB565A8 or A8", "image: stride %u is too small for a row of %u pixels (%u bytes needed)" and "image: size %u
 * doesn't match its header (%u expected)".
 *
 * Memory. A parsed manifest owns one block, messages_json: the "messages" object as written, less the
 * whitespace outside its strings. It is in PSRAM on the device (the muse overlay keeps small mallocs in
 * internal RAM, which is scarce, and this lives as long as the app) and malloc'd on the host. A manifest
 * is cleared with apps_manifest_clear, which frees it with free_fn: free, or something that wraps it.
 * apps_manifest_t is under 3 KB and apps_pkg_t under 7 KB: allocate them, never put them on a stack.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define APPS_ID_MAX 24
#define APPS_PATH_MAX 64
#define APPS_MAX_FILES 32
#define APPS_MANIFEST_MAX (8 * 1024)
#define APPS_LUA_MAX (256 * 1024)
#define APPS_JSON_FILE_MAX (32 * 1024)
#define APPS_IMAGE_MAX (512 * 1024)
#define APPS_IMAGES_TOTAL_MAX (1536 * 1024)
#define APPS_APP_MAX (2 * 1024 * 1024)

/*
 * Room for an app (C1). An app's files and its Lua heap (memory_kb) come out of the gadget's free PSRAM, and the apps always
 * leave APPS_RAM_RESERVE of it to the rest of the gadget: Muse's session (about 430 KB while it is up, about 630 KB at the
 * peak of a turn), LVGL's working memory, Wi-Fi's and TLS's buffers (PSRAM on the 1.75C) and a snapshot's encoder (about
 * 460 KB that apps_port_realloc doesn't hand out). LVGL asserts, and so aborts, when it can't allocate. So:
 * - apps_port_realloc refuses growth that would leave less than APPS_RAM_RESERVE free (apps_port.h);
 * - an app is admitted only when free PSRAM holds the files still to be read, its heap and the reserve: the store's load
 *   (apps_store.h), the RAM store's copies (apps_install.h), and every run once the app it replaces has gone (apps_task.h),
 *   when its files are in PSRAM already.
 */
#define APPS_RAM_RESERVE (1024 * 1024)
#define APPS_MSG_SHORT_OF_MEMORY \
    "the gadget is short of memory for %s: %u KB free, %u KB needed; restart it or install a smaller app"

/* Whether free_bytes of PSRAM hold `bytes` more for app id and still keep its heap (memory_kb) and APPS_RAM_RESERVE. False,
 * with APPS_MSG_SHORT_OF_MEMORY in err (free_bytes and the need in KB, the need rounded up; err may be NULL), when they
 * don't. */
bool apps_room(size_t free_bytes, const char *id, size_t bytes, unsigned memory_kb, char *err, size_t err_len);

/* What a message says of an id that isn't one, with the id as shown (apps_text_shown): the USB console's and the
 * install pipeline's. */
#define APPS_MSG_BAD_ID "app id '%s' must match ^[a-z][a-z0-9_]{1,23}$"

enum { APPS_PERM_SOUND = 1, APPS_PERM_ALARM = 2, APPS_PERM_STORE = 4, APPS_PERM_KEEP_AWAKE = 8 };

typedef enum { APPS_FILE_LUA, APPS_FILE_JSON, APPS_FILE_IMAGE, APPS_FILE_BAD } apps_file_kind_t;

typedef struct {
    char id[APPS_ID_MAX + 1], name[25], version[24], main[APPS_PATH_MAX + 1];
    char files[APPS_MAX_FILES][APPS_PATH_MAX + 1];  /* main first, then "files" in order, de-duplicated */
    uint8_t file_count, perms;
    uint16_t memory_kb;
    char *messages_json;  /* compact "messages" object or NULL; apps_manifest_clear frees */
    char warnings[512];   /* "; "-joined */
} apps_manifest_t;

/* One file of a package: its path, its bytes (a block that apps_pkg_free frees with the same free_fn as the
 * manifest's) and their SHA-256. */
typedef struct {
    char path[APPS_PATH_MAX + 1];
    uint8_t *data;
    size_t len;
    uint8_t sha256[32];
} apps_blob_t;

/* A package: the manifest, and the bytes of the files it lists. Start it zeroed. manifest_json is the manifest.json the
 * package was read from, manifest_len bytes, kept by the install pipeline and the flash store's load so that the store
 * writes the manifest as it came (and sees a reinstall that changed only it): a block like the files', which
 * apps_pkg_free frees, or NULL (the RAM store's copies don't keep it). */
typedef struct {
    apps_manifest_t manifest;
    apps_blob_t files[APPS_MAX_FILES];
    int count;
    char *manifest_json;
    size_t manifest_len;
} apps_pkg_t;

/* Whether id is ^[a-z][a-z0-9_]{1,23}$. NULL is not. */
bool apps_id_valid(const char *id);

/* Whether path is one a manifest may list (Ids and paths, above). NULL is not. */
bool apps_path_valid(const char *path);

/* How a file is read, by the end of its path: .lua, .json or .bin (case matters); APPS_FILE_BAD for anything
 * else. It doesn't check that the path is valid. */
apps_file_kind_t apps_file_kind(const char *path);

/* The most bytes a file of that kind may have: APPS_LUA_MAX, APPS_JSON_FILE_MAX or APPS_IMAGE_MAX, and 0 for
 * APPS_FILE_BAD (manifest.json's own is APPS_MANIFEST_MAX). The one table of them: the USB console's stage and the
 * install pipeline both read it. */
size_t apps_file_max(apps_file_kind_t kind);

/*
 * s[0, n), text from outside (what someone typed, a manifest's key), as a message shows it: valid UTF-8 as it is, each
 * byte of a control character (under 0x20, 0x7F) or of broken UTF-8 as '?', and a C1 control character (U+0080 to
 * U+009F, which a terminal reads as the start of an escape) as one '?'; cut at max bytes on a character boundary, with
 * "..." after the cut. out has room for max + 4 bytes; it returns out. s may be NULL when n is 0.
 */
char *apps_text_shown(const char *s, size_t n, size_t max, char *out);

/*
 * Text that leaves the runtime, and the compile message the install and apphost share: what an app prints or raises, and
 * what a package's source says, are bytes from outside, and they reach the serial console, Muse and a developer's terminal
 * inside JSON. So text handed out is valid UTF-8 (RFC 3629) with no control byte but, where a caller says so, "\n" and
 * tab; anything else is one '?': each byte of a C0 control (under 0x20), of DEL (0x7F) and of broken UTF-8, and, for a C1
 * control character (U+0080 to U+009F, which a terminal can take for an escape sequence: U+009B is CSI), the character.
 * The mapping never lengthens a text.
 *
 * apps_text_next reads the character at p (end is the end of the text) and gives the bytes it takes, at least one;
 * *keep says whether it passes as it is, or becomes one '?'. apps_text_clean makes s[0, n) clean in place, and returns
 * its new length. A NUL ends a text that shrank (s[new length]), and s[n] is never written: s[0, n) may hold NULs, each
 * of which is one more byte to map.
 */
size_t apps_text_next(const unsigned char *p, const unsigned char *end, bool keep_newline, bool *keep);
size_t apps_text_clean(char *s, size_t n, bool keep_newline);

/*
 * Reads json[0, len), a manifest.json (json may be NULL when len is 0), into m (never NULL), which this
 * overwrites (it doesn't free what m held): on success m is the manifest and err is "", and on failure m is
 * all zero and err has the message (Messages, above). err may be NULL, and nothing is written when err_len
 * is 0.
 *
 * expected_id is the app's folder name, which id must equal (NULL: no folder, so nothing matches).
 * heap_max_kb is the device's CONFIG_MUSE_APPS_HEAP_MAX_KB: a memory_kb above it, or above 1024, is
 * refused, and a manifest that leaves it out gets 256, or heap_max_kb if that is less (under 64 counts as
 * 64).
 *
 * It reads only json[0, len) and writes only m and err. The one thing it allocates is messages_json, last of
 * all, once the manifest has passed, so a failure leaves nothing to free (Memory, above). It doesn't recurse
 * itself, and its stack stays under 2 KB with the reader's own recursion in it (16 levels, 48 bytes each).
 */
bool apps_manifest_parse(const char *json, size_t len, const char *expected_id, int heap_max_kb, apps_manifest_t *m,
                         char *err, size_t err_len);

/* Frees messages_json with free_fn (NULL: free) and zeroes m. NULL is ignored. Safe after any parse. */
void apps_manifest_clear(apps_manifest_t *m, void (*free_fn)(void *));

/* Frees every blob's data, manifest_json and the manifest's messages_json with free_fn (NULL: free), and zeroes pkg.
 * It looks at every file slot, not just the first count, so a blob that was read but never counted is freed too.
 * NULL is ignored. */
void apps_pkg_free(apps_pkg_t *pkg, void (*free_fn)(void *));

/*
 * The .lua files of pkg in the order the install compiles them (apps_rt_compile): main first, then the others in the
 * package's order. order has room for APPS_MAX_FILES pointers into pkg; it returns how many it set. main is order[0]
 * when it is one of pkg's .lua files, which a package that passed the manifest's parse and was read always has; with
 * no such file the others come alone. One order for two callers, the app task's compile and apphost's check: where the
 * first error is found depends on it.
 */
int apps_pkg_compile_order(const apps_pkg_t *pkg, const apps_blob_t *order[APPS_MAX_FILES]);

/*
 * The message for a package whose Lua didn't compile, from raw, what apps_vm_load said of one of its files
 * ("util.lua:7: unexpected symbol near '='"): "compile failed: ", raw, and the lines of the file it names around the
 * line it gives (apps_vm_excerpt's text: " 6| ...\n>7| ...\n 8| ..."). A raw that names no file of the package, or
 * names no line, is shown as it is. The file is what comes before raw's first ":<digits>:", matched against the
 * paths of pkg's files whole. out is always terminated when cap is above 0, and cut at cap - 1 bytes on a character
 * boundary; raw must not be inside out. One wording for two callers: the install pipeline's refusal (apps_install.c,
 * step 4) and apphost's, whose check compiles what the install compiles.
 * The text is clean (apps_text_clean): Lua's message quotes the token it stopped at and the lines are the package's own
 * bytes, so a '?' stands for each control byte, C1 character and byte that isn't UTF-8, in a message of one line (a token
 * can hold a newline) and in the lines (the excerpt's own newlines stay, and tab).
 */
void apps_pkg_compile_message(const apps_pkg_t *pkg, const char *raw, char *out, size_t cap);

/*
 * Whether `path`, the manifest's entry number `index` (counting main as 0), whose bytes are data[0, len), is source the
 * install takes. A .lua file that starts with Lua's signature, "\x1bLua", is a compiled chunk, and no package may hold
 * one: err has "files[%d] %s is a compiled chunk: install Lua source text", and it returns false. Any other file, of any
 * kind, passes. (apps_vm_load refuses a chunk too, but later and in its own words.) One wording for two callers, the
 * install pipeline's step 3 and apphost's reading of a package, so check says what the install says.
 */
bool apps_pkg_check_source(int index, const char *path, const uint8_t *data, size_t len, char *err, size_t err_len);

/*
 * Whether data[0, len) is an LVGL v9 .bin image the gadget can draw: magic 0x19; colour format RGB565 (0x12),
 * RGB565A8 (0x14) or A8 (0x0E); not compressed (flag 0x0008) and with no flag but premultiplied alpha (0x0001,
 * which LVGLImage.py sets for a premultiplied image; the others, allocated and modifiable, say what an image's
 * memory is, and the file is drawn in place); 1 to 466 pixels each way; a stride (bytes a row) of at least a
 * row's worth; and a size of exactly 12 + stride * h bytes (RGB565A8: and (stride / 2) * h more, for the alpha
 * plane). On success *w and *h are its size (either may be NULL); otherwise they are 0 and err has the message
 * (Messages, above). The header's fields are read from the bytes, so data needs no alignment, and nothing past
 * data[len] is read.
 */
bool apps_image_check(const uint8_t *data, size_t len, uint16_t *w, uint16_t *h, char *err, size_t err_len);
