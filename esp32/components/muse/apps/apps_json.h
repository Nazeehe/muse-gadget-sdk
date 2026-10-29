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
 * JSON for Muse apps: the `json` Lua module, a decoder and an encoder the firmware uses itself,
 * and a pull reader for the manifest. Portable C: nothing here calls the platform.
 *
 * One decoder. json.decode, apps_json_validate, apps_json_push and the reader's apps_jr_skip
 * run the same scanner, so they accept exactly the same text: RFC 8259, strictly (no comments,
 * no trailing commas, no NaN, no BOM), with
 * - nesting up to APPS_JSON_MAX_DEPTH levels and input up to APPS_JSON_MAX_INPUT bytes;
 * - strings as the RFC has them: no raw control character; the escapes \" \\ \/ \b \f \n \r \t and
 *   \uXXXX, where a surrogate pair is one character and a lone or unpaired surrogate is refused;
 *   and UTF-8 as RFC 3629 defines it: no overlong form, no surrogate, nothing above U+10FFFF;
 * - numbers by the RFC's grammar, in at most 200 characters.
 *
 * Decoding: objects become tables and arrays become sequences, built with raw sets. A number with
 * no '.', 'e' or 'E' that fits in int64 is a Lua integer; any other number is a float (1e999 is
 * inf). null is json.null, a unique light userdata, so a decoded array never has holes.
 *
 * Encoding: a table with the keys 1..n (n >= 1) and no others is an array; any other table is an
 * object, so {} is {}, and an integer key becomes its decimal text. An object's keys come in
 * lua_next's order, not the order they were set in. Integers print in decimal;
 * floats in the fewest of 15, 16 and 17 significant digits that strtod reads back as the same
 * double (0.1 is 0.1, not 0.10000000000000001), and ".0" is added to one that prints without a '.',
 * 'e' or 'E' (3.0, -0.0), so it decodes as a float again. A host's libc and picolibc's give the
 * same text for every double but the subnormals: 5e-324 is 4.94065645841247e-324 on a host and
 * 5e-324 on the board, and both decode to the same double. A string is escaped as RFC 8259 asks:
 * '"', '\' and every byte under 0x20 (\n \r \t \b \f, the rest as \u00xx); every other byte,
 * '/' included, passes through (apps_json_escape is that, for the firmware's own use). nil and
 * json.null are null. Encoding reads tables raw (lua_next,
 * lua_rawgeti) and never calls a metamethod, so no app code runs inside it. It refuses: a
 * function, a thread or any userdata but json.null; NaN and infinity; a string that isn't valid
 * UTF-8 (value or key), which json.decode would refuse after a reboot; a key that is neither a
 * string nor an integer; and nesting deeper than APPS_JSON_MAX_DEPTH. Every value it visits
 * writes at least one byte, so a table that holds the same subtable many times runs into the
 * app's memory cap, not into a long spin. That alone doesn't bound the walk: lua_next visits
 * every slot a table was ever grown to, empty or not, and a table emptied after it grew writes
 * little for what it costs. So each table visit is charged its slots plus one to the call's
 * instruction budget (apps_vm_charge, apps_vm_table_slots), and a table shared many times ends
 * in the budget's kill. Decoding and encoding both also check the clock every 256 values.
 *
 * Messages. The C functions write the bare message into err, ASCII only (it reaches Muse inside a
 * JSON string), and their callers add their own context ("manifest: ", "store.set: ", a file's
 * name). json.decode and json.encode add "json.decode: " and "json.encode: " themselves. A
 * position is a 1-based byte offset:
 *   unexpected '%c' at byte %u              a printable ASCII byte (0x21-0x7E) that can't be there
 *   unexpected byte 0x%02X at byte %u       any other byte, a raw control character in a string too
 *   unexpected end of input at byte %u      the text stops early: one past its last byte, inside a
 *                                           string or a UTF-8 sequence that is valid so far included
 *   invalid UTF-8 at byte %u                at the sequence's first byte
 *   invalid \u escape at byte %u            at the backslash: bad digits, a lone or unpaired surrogate
 *   number longer than 200 characters at byte %u
 *   nested deeper than 16                   (decode and encode)
 *   input larger than 32 KB
 * from the encoder:
 *   can't encode a %s                       a Lua type name: function, thread, userdata
 *   can't encode a %s key                   a key that isn't a string or an integer, "number" for a float
 *   can't encode a string that isn't valid UTF-8
 *   can't encode NaN or infinity
 *   out of memory                           the caller's realloc_fn returned NULL
 *   output larger than the limit            APPS_JSON_TOO_LARGE: apps_json_encode_value_max's max
 * and from the pull reader, besides the decoder's:
 *   expected an integer at byte %u          apps_jr_integer on anything but an integer: a fraction, an exponent
 *   integer out of range at byte %u         outside int64
 *   string longer than %u bytes at byte %u  apps_jr_string and apps_jr_key, which never cut a string
 *                                           (apps_jr_key_any reports a key that is too long instead)
 *   \u0000 isn't allowed in this string at byte %u
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* lua.h declares this too; the firmware outside apps/ includes this header, not Lua's. */
typedef struct lua_State lua_State;

#define APPS_JSON_MAX_DEPTH 16
#define APPS_JSON_MAX_INPUT (32 * 1024)

/* The message of an encoding that outgrew its limit (apps_json_encode_value_max), for a caller that says it its own way. */
#define APPS_JSON_TOO_LARGE "output larger than the limit"

/*
 * The text a JSON string holds between its quotes, for the firmware's own counting and writing of text the encoder would write (the
 * store sizes its saved keys with it): '"' and '\' as \" and \\, \b \f \n \r \t as their two-byte forms, every other byte under 0x20
 * as \u00xx in lowercase hex, and every other byte, 0x7F, '/' and the bytes of UTF-8 included, as it is. The encoder's put_string is
 * built on this, so the two can't differ. Writes into out, which needs the room the count says (at most 6 * n bytes, no NUL added), and
 * returns the count; out may be NULL to only count. It doesn't check that the bytes are UTF-8.
 */
size_t apps_json_escape(char *out, const char *s, size_t n);

/*
 * UTF-8 as the decoder reads it (RFC 3629, above), for the firmware's own checks of text that reaches Muse
 * inside JSON: the character at p, whose first byte is 0x80 or more, in the text that ends at end (p < end).
 * Its length in bytes (2 to 4); APPS_UTF8_INVALID when the bytes there aren't one (a stray continuation
 * byte, an overlong form, a surrogate, past U+10FFFF, or a byte that isn't a continuation byte where one is
 * due); APPS_UTF8_CUT when the text ends inside it and the bytes so far are valid. One definition of valid
 * text, so that a string the decoder accepts and one another module calls clean are the same.
 */
#define APPS_UTF8_INVALID 0
#define APPS_UTF8_CUT (-1)
int apps_json_utf8_len(const unsigned char *p, const unsigned char *end);

/*
 * Installs the global `json`: encode, decode and null. It allocates, but it is safe to call
 * outside a protected call: if the heap refuses, `json` isn't installed and the VM is out of
 * memory (apps_vm.h), so the next apps_vm_call ends NOMEM. The stack is as it was.
 *
 * json.decode(s) and json.encode(v) call apps_vm_check first, like every binding, and raise
 * "json.decode: <message>" and "json.encode: <message>". json.encode(nil) is "null";
 * json.encode() with no argument is an error.
 */
void apps_json_open(lua_State *L);

/* Whether s[0, len) is one JSON value, with only whitespace around it; else the bare message in
 * err (cut to err_len). Allocates nothing and never raises. */
bool apps_json_validate(const char *s, size_t len, char *err, size_t err_len);

/*
 * Decodes s[0, len) and pushes the one value. On a syntax error it returns false with the bare
 * message in err (cut to err_len) and leaves the stack as it was. ONLY INSIDE A PROTECTED CALL
 * (apps_vm_call or lua_pcall): it raises when the app's capped heap refuses memory, and when the
 * call is killed (over its budget or APPS_WALL_MS, which it checks every 256 values, or by an
 * external kill), which a caller outside a protected call turns into a panic. A syntax error never
 * raises. It reserves the Lua stack slots it uses (luaL_checkstack), so the caller needs none to
 * spare.
 */
bool apps_json_push(lua_State *L, const char *s, size_t len, char *err, size_t err_len);

/*
 * Encodes the value at idx (a negative index counts from the top at the call) as JSON text, and
 * returns it in a block from realloc_fn(NULL, n), NUL-terminated, with its length (without the NUL)
 * in *len. The caller frees the block with the free that pairs with realloc_fn (apps_vm_config_t's
 * free_fn, for the VM's own allocator), never with Lua's. The stack is as it was on return, in every
 * case.
 *
 * NULL means failure: the bare message in err (cut to err_len) and *len set to 0. An encoding error
 * doesn't raise, and neither does realloc_fn returning NULL ("out of memory"). len and err may be
 * NULL.
 *
 * ONLY INSIDE A PROTECTED CALL, as apps_json_push: the output is built in the app's capped heap, in a
 * userdata that grows by copying (never in a luaL_Buffer, whose box must be at the top of the stack
 * when it grows, where lua_next's key is), and when that heap refuses it raises. It raises as well
 * when the call is killed: it charges each table's slots to the call's budget and checks the clock
 * every 256 values (see Encoding above), so a hostile table costs the app its call, not the
 * gadget its time. Nothing it holds is outside the Lua heap before it returns, so a raise leaks
 * nothing. It reserves the Lua stack slots it uses, as apps_json_push does.
 */
char *apps_json_encode_value(lua_State *L, int idx, void *(*realloc_fn)(void *, size_t), size_t *len, char *err,
                             size_t err_len);

/*
 * apps_json_encode_value with a limit: a text longer than max bytes (without the NUL; 0 is no limit) fails with APPS_JSON_TOO_LARGE as
 * its message, before the memory for it is asked for. The output userdata never grows past max, the walk ends at the first byte over,
 * and a string whose own length can't fit is refused without being read, so a value that can never fit costs the app a few hundred
 * bytes and no scan, however large it is. (json.encode has no limit but the app's memory.) realloc_fn is called once, with the final
 * length, and not at all on a failure.
 */
char *apps_json_encode_value_max(lua_State *L, int idx, void *(*realloc_fn)(void *, size_t), size_t max, size_t *len, char *err,
                                 size_t err_len);

/*
 * Drops the whitespace outside strings from s[0, len) into out, NUL-terminated, and returns the
 * length written; 0 if cap is too small (out then holds ""). It never grows the text, so
 * cap >= len + 1 always suffices. The input is JSON the reader already accepted.
 */
size_t apps_json_compact(const char *s, size_t len, char *out, size_t cap);

/*
 * A pull reader: the caller walks a document and takes what it needs, with no tree built. For the
 * manifest, whose size is bounded by its caller (Task 7: 8 KB), and for nothing that allocates.
 *
 * It reads the same JSON the decoder accepts: skipping a value checks it, strings and numbers
 * included, to the same depth limit, which counts the containers the caller opened too. A
 * separator is checked as well, so a leading, doubled or trailing comma fails. Every error is
 * final: the first message stays in err, and every later call fails at once. err is "" until then.
 * The calls below all return false, -1 or NULL on an error, with the bare message in r->err.
 */
typedef struct {
    const char *p, *end;
    int depth;     /* containers open, counting the ones the caller opened with apps_jr_object/array */
    char err[96];  /* "" while it is well; else "<what> at byte N" (see the messages above) */
    /* The most containers open at once: APPS_JSON_MAX_DEPTH after apps_jr_init, the decoder's. A caller reading a document whose own
     * container wraps values that json.encode wrote (the store's saved object) sets it one higher, so that the wrapped values keep all
     * of theirs; the message stays "nested deeper than 16". At most 32. */
    int max_depth;
    /* Private. */
    const char *start;  /* the first byte: positions count from it */
    uint32_t started;   /* bit d-1: the container at depth d has begun its elements (comma rules) */
    uint32_t values;    /* values the decoder has built, for its clock check */
} apps_jr_t;

/* Reads s[0, len). Input over APPS_JSON_MAX_INPUT is an error from the first call. */
void apps_jr_init(apps_jr_t *r, const char *s, size_t len);
bool apps_jr_object(apps_jr_t *r);  /* '{' */
bool apps_jr_array(apps_jr_t *r);   /* '[' */
/* In an object: 1 with the next key in key (':' consumed), 0 at the '}' (consumed), -1 on an error.
 * The key is read as apps_jr_string reads a string. */
int apps_jr_key(apps_jr_t *r, char *key, size_t cap);
/* As apps_jr_key, for a caller that can do without a key it can't hold: one with more than cap - 1 bytes, or a \u0000 in it, is read to
 * its end, checked like every string, and reported as 2 with key "" (its colon consumed and its value still to come, which the caller
 * skips). An empty key is 1, with key "". Anything that isn't valid JSON is -1, as ever. */
int apps_jr_key_any(apps_jr_t *r, char *key, size_t cap);
/* In an array: 1 when another element follows (positioned at it), 0 at the ']' (consumed), -1 on an
 * error. Read the element with one of the calls below. */
int apps_jr_next(apps_jr_t *r);
/* A string into out, NUL-terminated, cap bytes with the NUL. A string that doesn't fit is an error
 * ("string longer than N bytes at byte N"): it is never cut. \u0000 is refused, since out is a C
 * string; json.decode allows it. */
bool apps_jr_string(apps_jr_t *r, char *out, size_t cap);
/* An integer: no fraction, no exponent, within int64. Anything else is an error. */
bool apps_jr_integer(apps_jr_t *r, int64_t *out);
bool apps_jr_skip(apps_jr_t *r);  /* any one value, checked */
/* Only whitespace is left, and no container is open. */
bool apps_jr_end(apps_jr_t *r);
/* Skips one value and returns its text, in the input (not copied, not terminated), with its length
 * in *len; NULL on an error. */
const char *apps_jr_raw_value(apps_jr_t *r, size_t *len);
