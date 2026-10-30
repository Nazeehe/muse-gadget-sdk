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
 * JSON for Muse apps (apps_json.h): one scanner under the decoder, the validator and the pull
 * reader; one encoder that builds its output in the app's own heap.
 *
 * Rules this file keeps, since the app task's 56 KB stack also carries Lua's own C recursion and
 * no count hook interrupts C:
 * - Work is bounded. Decoding is bounded by its 32 KB input. Encoding charges each table it visits
 *   its slots, empty ones too, to the call's instruction budget (a table that appears many times,
 *   or one emptied after it grew, costs every walk), and its output grows in the app's capped heap.
 *   Both read the clock every 256 values (CLOCK_EVERY_MASK), so APPS_WALL_MS holds inside one call.
 * - Stacks are small. The recursive functions (value, object, array, encode_at, encode_table) keep
 *   a few scalars each; every buffer lives in a NOINLINE leaf the compiler can't fold into their
 *   frames (push_string's luaL_Buffer, push_number's strtod copy, put_float's digits). Both
 *   directions call luaL_checkstack at every level: Lua guarantees a C function 20 slots, and 16
 *   levels of table, key and value want more.
 * - Nothing the app owns runs. Tables are read with lua_next and raw gets and built with raw sets,
 *   so no metamethod is called.
 * - Everything that allocates keeps it in the Lua heap (a userdata, a luaL_Buffer's box), so it
 *   counts against the cap and an error raised halfway leaks nothing.
 */

#include "apps_json.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "apps_vm.h"
#include "lauxlib.h"
#include "lua.h"

#define NOINLINE __attribute__((noinline))

/* The longest number token. A double's shortest form is under 25 characters; this leaves room for
 * anyone's long form and bounds both strtod's work and push_number's buffer. */
#define NUMBER_MAX 200

/* The Lua stack slots reserved with luaL_checkstack. Lua gives a C function 20, and 16 levels of table
 * and key want more, so each level reserves for itself, and each push below falls inside the latest
 * reservation (a build with LUA_USE_APICHECK aborts on one that doesn't; the tests make one).
 * Decoding reserves on entering a container, and at the start for a value that isn't one: the
 * container's table and key (2), and the 5 that a luaL_Buffer's first box takes under them (the box,
 * a nil from looking its metatable up, the metatable, a metamethod on its way into it, and the
 * field's name while lua_setfield stores that). Encoding reserves on entering a table: lua_next's
 * key and value (2) and the larger output userdata out_grow makes before it replaces the old (1). */
#define DECODE_SLOTS 7
#define ENCODE_SLOTS 3

/* Decoding and encoding call apps_vm_charge(L, 0) after every 256 values they build or write, and encoding after
 * every 256 keys of the walk that tells an array from an object: it reads the clock, which kills a call past
 * APPS_WALL_MS and raises an external kill, inside one big table. A float costs the S3 7 to 11 us to decode and, at
 * the most, 109 us to encode (put_float's three tries), so 256 of them are under 30 ms. */
#define CLOCK_EVERY_MASK 0xFFu

/* json.null is this address. */
static char null_tag;

/* ==== Scanning ==== */

static unsigned position(const apps_jr_t *r, const char *p) {
    return (unsigned)(p - r->start) + 1u;
}

/* The errors. Each records the reader's first error and keeps it, and returns false. */

/* "<what> at byte N". */
static bool fail_at(apps_jr_t *r, const char *what, const char *p) {
    if (r->err[0] == '\0') {
        snprintf(r->err, sizeof r->err, "%s at byte %u", what, position(r, p));
    }
    return false;
}

/* The byte p points at can't be there; at the end of the input, there is none. */
static bool fail_unexpected(apps_jr_t *r, const char *p) {
    if (r->err[0] != '\0') {
        return false;
    }
    if (p >= r->end) {
        return fail_at(r, "unexpected end of input", r->end);
    }
    unsigned char c = (unsigned char)*p;
    if (c >= 0x21 && c <= 0x7E) {
        snprintf(r->err, sizeof r->err, "unexpected '%c' at byte %u", c, position(r, p));
    } else {
        snprintf(r->err, sizeof r->err, "unexpected byte 0x%02X at byte %u", c, position(r, p));
    }
    return false;
}

/* The levels the `started` bits have room for; max_depth is held to it. */
#define JR_LEVELS 32

static bool fail_depth(apps_jr_t *r) {
    if (r->err[0] == '\0') {
        snprintf(r->err, sizeof r->err, "nested deeper than %d", APPS_JSON_MAX_DEPTH);
    }
    return false;
}

static void skip_ws(apps_jr_t *r) {
    while (r->p < r->end && (*r->p == ' ' || *r->p == '\t' || *r->p == '\n' || *r->p == '\r')) {
        r->p++;
    }
}

/* The next non-whitespace byte is c: consumed. */
static bool expect(apps_jr_t *r, char c) {
    skip_ws(r);
    if (r->p >= r->end || *r->p != c) {
        return fail_unexpected(r, r->p);
    }
    r->p++;
    return true;
}

/* UTF-8 (RFC 3629) at p, in the text that ends at end, whose first byte is 0x80 or more: the length of the
 * sequence; or APPS_UTF8_INVALID when it isn't one: a stray continuation byte, an overlong form, a surrogate
 * (U+D800-DFFF), a code point past U+10FFFF, or a byte that isn't a continuation byte where one is due; or
 * APPS_UTF8_CUT when the text ends inside it and the bytes there are valid so far. Shared by the decoder,
 * which reports a cut sequence as the end of the input like any other cut, and the encoder, which refuses
 * both; public (apps_json.h) so that no other module needs a second definition of valid text. */
int apps_json_utf8_len(const unsigned char *p, const unsigned char *end) {
    unsigned char first = p[0], lo = 0x80, hi = 0xBF;  /* the range of the second byte */
    int n;
    if (first >= 0xC2 && first <= 0xDF) {
        n = 2;
    } else if (first >= 0xE0 && first <= 0xEF) {
        n = 3;
        if (first == 0xE0) {
            lo = 0xA0;  /* no overlong 3-byte form */
        } else if (first == 0xED) {
            hi = 0x9F;  /* no surrogates */
        }
    } else if (first >= 0xF0 && first <= 0xF4) {
        n = 4;
        if (first == 0xF0) {
            lo = 0x90;  /* no overlong 4-byte form */
        } else if (first == 0xF4) {
            hi = 0x8F;  /* nothing past U+10FFFF */
        }
    } else {
        return APPS_UTF8_INVALID;  /* a continuation byte, an overlong 2-byte lead (C0, C1), or past F4 */
    }
    for (int i = 1; i < n; i++) {
        if (p + i >= end) {
            return APPS_UTF8_CUT;
        }
        unsigned char b = p[i];
        if (i == 1 ? (b < lo || b > hi) : (b & 0xC0) != 0x80) {
            return APPS_UTF8_INVALID;
        }
    }
    return n;
}

/* The UTF-8 bytes of the code point cp (at most U+10FFFF, no surrogate) into out; how many. */
static size_t utf8_encode(unsigned cp, char *out) {
    if (cp < 0x80) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp < 0x800) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static int hex_digit(char c) {
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

/* A string being scanned: where its decoded bytes go, and what the scan learned. */
typedef struct {
    char *out;      /* the decoded bytes go here; NULL: only count them */
    size_t cap;     /* the room in out, which a longer string fails to fit */
    size_t len;     /* the decoded length so far, counted with or without out */
    bool escaped;   /* the string has an escape, so its text isn't its value */
    bool no_nul;    /* refuse \u0000: the string is for C code */
    bool lenient;   /* a string that outgrows out, or has a \u0000 that no_nul refuses, is still scanned to its end and checked, with
                     * lost set, instead of failing (apps_jr_key_any) */
    bool lost;      /* lenient, and out doesn't hold the string: only len is counted from here on */
} jstr_t;

static bool jstr_put(apps_jr_t *r, jstr_t *s, const char *open, const char *bytes, size_t n) {
    if (s->out != NULL && !s->lost) {
        if (n > s->cap - s->len) {
            if (s->lenient) {
                s->lost = true;
                s->len += n;
                return true;
            }
            if (r->err[0] == '\0') {
                snprintf(r->err, sizeof r->err, "string longer than %u bytes at byte %u", (unsigned)s->cap, position(r, open));
            }
            return false;
        }
        memcpy(s->out + s->len, bytes, n);
    }
    s->len += n;
    return true;
}

/* Four hex digits at p, within the input. The escape's backslash is bs: bad digits are its fault, a
 * text that ends first is the input's. */
static bool hex4(apps_jr_t *r, const char *bs, const char *p, unsigned *out) {
    unsigned v = 0;
    for (int i = 0; i < 4; i++) {
        if (p + i >= r->end) {
            return fail_unexpected(r, r->end);
        }
        int d = hex_digit(p[i]);
        if (d < 0) {
            return fail_at(r, "invalid \\u escape", bs);
        }
        v = (v << 4) | (unsigned)d;
    }
    *out = v;
    return true;
}

/* \uXXXX at *pp, with a low surrogate's escape after a high one. A lone or unpaired surrogate is
 * an error at the first escape's backslash; bad digits are one at the backslash of the escape that
 * has them. */
static bool scan_unicode(apps_jr_t *r, jstr_t *s, const char *open, const char **pp) {
    const char *bs = *pp;
    unsigned cp;
    if (!hex4(r, bs, bs + 2, &cp)) {
        return false;
    }
    const char *next = bs + 6;
    if (cp >= 0xD800 && cp <= 0xDBFF) {
        unsigned low;
        if (next >= r->end) {
            return fail_unexpected(r, r->end);
        }
        if (next[0] != '\\') {
            return fail_at(r, "invalid \\u escape", bs);
        }
        if (next + 1 >= r->end) {
            return fail_unexpected(r, r->end);
        }
        if (next[1] != 'u') {
            return fail_at(r, "invalid \\u escape", bs);
        }
        if (!hex4(r, next, next + 2, &low)) {
            return false;
        }
        if (low < 0xDC00 || low > 0xDFFF) {
            return fail_at(r, "invalid \\u escape", bs);
        }
        cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
        next += 6;
    } else if (cp >= 0xDC00 && cp <= 0xDFFF) {
        return fail_at(r, "invalid \\u escape", bs);
    }
    if (cp == 0 && s->no_nul) {
        if (!s->lenient) {
            return fail_at(r, "\\u0000 isn't allowed in this string", bs);
        }
        s->lost = true;
    }
    char utf8[4];
    size_t n = utf8_encode(cp, utf8);
    if (!jstr_put(r, s, open, utf8, n)) {
        return false;
    }
    s->escaped = true;
    *pp = next;
    return true;
}

/* The escape at *pp, which starts with a backslash. Any other letter after it is unexpected. */
static bool scan_escape(apps_jr_t *r, jstr_t *s, const char *open, const char **pp) {
    const char *bs = *pp;
    if (bs + 1 >= r->end) {
        return fail_unexpected(r, r->end);
    }
    char c = bs[1], out;
    switch (c) {
        case '"':
        case '\\':
        case '/': out = c; break;
        case 'b': out = '\b'; break;
        case 'f': out = '\f'; break;
        case 'n': out = '\n'; break;
        case 'r': out = '\r'; break;
        case 't': out = '\t'; break;
        case 'u': return scan_unicode(r, s, open, pp);
        default: return fail_unexpected(r, bs + 1);
    }
    if (!jstr_put(r, s, open, &out, 1)) {
        return false;
    }
    s->escaped = true;
    *pp = bs + 2;
    return true;
}

/* The string at r->p, which starts with its opening quote: validated, and decoded into s. On
 * success r->p is past the closing quote. Every string any reader sees goes through here. */
static bool scan_string(apps_jr_t *r, jstr_t *s) {
    const char *open = r->p, *end = r->end, *p = open + 1, *run = p;
    for (;;) {
        if (p >= end) {
            return fail_unexpected(r, end);
        }
        unsigned char c = (unsigned char)*p;
        if (c == '"') {
            break;
        }
        if (c < 0x20) {
            return fail_unexpected(r, p);
        }
        if (c == '\\') {
            if (!jstr_put(r, s, open, run, (size_t)(p - run)) || !scan_escape(r, s, open, &p)) {
                return false;
            }
            run = p;
        } else if (c >= 0x80) {
            int n = apps_json_utf8_len((const unsigned char *)p, (const unsigned char *)end);
            if (n == APPS_UTF8_CUT) {
                return fail_unexpected(r, end);
            }
            if (n == APPS_UTF8_INVALID) {
                return fail_at(r, "invalid UTF-8", p);
            }
            p += n;
        } else {
            p++;
        }
    }
    if (!jstr_put(r, s, open, run, (size_t)(p - run))) {
        return false;
    }
    r->p = p + 1;
    return true;
}

static bool is_digit(char c) {
    return c >= '0' && c <= '9';
}

/* The number at r->p (RFC 8259: -? int frac? exp?), at most NUMBER_MAX characters. is_float is
 * whether it has a '.', 'e' or 'E'. */
static bool scan_number(apps_jr_t *r, bool *is_float) {
    const char *start = r->p, *p = start, *end = r->end;
    *is_float = false;
    if (*p == '-') {
        p++;
    }
    if (p >= end) {
        return fail_unexpected(r, end);
    }
    if (*p == '0') {
        p++;
    } else if (*p >= '1' && *p <= '9') {
        while (p < end && is_digit(*p)) {
            p++;
        }
    } else {
        return fail_unexpected(r, p);
    }
    if (p < end && *p == '.') {
        *is_float = true;
        p++;
        if (p >= end || !is_digit(*p)) {
            return fail_unexpected(r, p);
        }
        while (p < end && is_digit(*p)) {
            p++;
        }
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        *is_float = true;
        p++;
        if (p < end && (*p == '+' || *p == '-')) {
            p++;
        }
        if (p >= end || !is_digit(*p)) {
            return fail_unexpected(r, p);
        }
        while (p < end && is_digit(*p)) {
            p++;
        }
    }
    if ((size_t)(p - start) > NUMBER_MAX) {
        if (r->err[0] == '\0') {
            snprintf(r->err, sizeof r->err, "number longer than %d characters at byte %u", NUMBER_MAX, position(r, start));
        }
        return false;
    }
    r->p = p;
    return true;
}

/* The integer s[0, n) (an optional '-' and digits), when it fits int64. */
static bool parse_int64(const char *s, size_t n, int64_t *out) {
    bool negative = n > 0 && s[0] == '-';
    uint64_t magnitude = 0;
    for (size_t i = negative ? 1 : 0; i < n; i++) {
        unsigned digit = (unsigned)(s[i] - '0');
        if (magnitude > (UINT64_MAX - digit) / 10) {
            return false;
        }
        magnitude = magnitude * 10 + digit;
    }
    if (negative) {
        if (magnitude > (uint64_t)INT64_MAX + 1) {
            return false;
        }
        *out = magnitude == (uint64_t)INT64_MAX + 1 ? INT64_MIN : -(int64_t)magnitude;
    } else {
        if (magnitude > (uint64_t)INT64_MAX) {
            return false;
        }
        *out = (int64_t)magnitude;
    }
    return true;
}

/* ==== Decoding ==== */

/* A string at r->p, checked and not kept. A leaf, so its jstr_t stays out of value()'s frame. */
static NOINLINE bool skip_string(apps_jr_t *r) {
    jstr_t s = {0};
    return scan_string(r, &s);
}

/* A string at r->p, pushed. A leaf: the luaL_Buffer here holds LUAL_BUFFERSIZE bytes inline, which
 * must not be folded into the recursive functions' frames. A string without an escape is its own
 * text. One with escapes is scanned twice: the first pass checks it and measures what it decodes
 * to, the second fills a buffer of exactly that size, so the box (a userdata, past the inline
 * buffer) is made once and nothing else is pushed while it is open. */
static NOINLINE bool push_string(apps_jr_t *r, lua_State *L) {
    const char *open = r->p;
    jstr_t s = {0};
    if (!scan_string(r, &s)) {
        return false;
    }
    if (!s.escaped) {
        lua_pushlstring(L, open + 1, s.len);
        return true;
    }
    luaL_Buffer b;
    jstr_t again = {.out = luaL_buffinitsize(L, &b, s.len), .cap = s.len};
    const char *after = r->p;
    r->p = open;
    bool ok = scan_string(r, &again);  /* the same, valid text: it can't fail, and it fills s.len bytes */
    r->p = after;
    luaL_pushresultsize(&b, again.len);
    return ok;
}

/* The number at r->p, pushed: an integer when it has no '.', 'e' or 'E' and fits int64, else a
 * float. A leaf, for the copy strtod needs (the text isn't NUL-terminated). */
static NOINLINE void push_number(lua_State *L, const char *s, size_t n, bool is_float) {
    int64_t i;
    if (!is_float && parse_int64(s, n, &i)) {
        lua_pushinteger(L, (lua_Integer)i);
        return;
    }
    char buf[NUMBER_MAX + 1];
    memcpy(buf, s, n);
    buf[n] = '\0';
    lua_pushnumber(L, (lua_Number)strtod(buf, NULL));
}

static bool literal(apps_jr_t *r, const char *word) {
    const char *p = r->p;
    for (; *word != '\0'; word++, p++) {
        if (p >= r->end || *p != *word) {
            return fail_unexpected(r, p);
        }
    }
    r->p = p;
    return true;
}

/* At the '{' or '[' at r->p: consumed, one level deeper. L (or NULL) is where the decoder is
 * building, which reserves the stack slots this level and its leaves use. */
static bool enter(apps_jr_t *r, lua_State *L) {
    if (r->depth >= r->max_depth || r->depth >= JR_LEVELS) {
        return fail_depth(r);
    }
    r->depth++;
    r->started &= ~(1u << (r->depth - 1));
    r->p++;
    if (L != NULL) {
        luaL_checkstack(L, DECODE_SLOTS, "json");
    }
    return true;
}

/* In a container closed by close, whose elements so far are started: 1 when another follows (its
 * comma consumed, nothing else), 0 at the closing bracket (consumed, one level up), -1 on an error.
 * The comma rules live here: one before every element but the first, none after the last. */
static int more(apps_jr_t *r, char close, bool *started) {
    skip_ws(r);
    if (r->p >= r->end) {
        fail_unexpected(r, r->p);
        return -1;
    }
    if (*r->p == close) {
        r->p++;
        r->depth--;
        return 0;
    }
    if (*started) {
        if (*r->p != ',') {
            fail_unexpected(r, r->p);
            return -1;
        }
        r->p++;
    }
    *started = true;
    return 1;
}

static bool value(apps_jr_t *r, lua_State *L);

/* An object's key and its colon, the key pushed when building. */
static bool key(apps_jr_t *r, lua_State *L) {
    skip_ws(r);
    if (r->p >= r->end || *r->p != '"') {
        return fail_unexpected(r, r->p);
    }
    if (!(L != NULL ? push_string(r, L) : skip_string(r))) {
        return false;
    }
    return expect(r, ':');
}

static bool object(apps_jr_t *r, lua_State *L) {
    if (!enter(r, L)) {
        return false;
    }
    int table = 0;
    if (L != NULL) {
        lua_createtable(L, 0, 0);
        table = lua_gettop(L);
    }
    bool started = false;
    for (;;) {
        int next = more(r, '}', &started);
        if (next <= 0) {
            return next == 0;
        }
        if (!key(r, L) || !value(r, L)) {
            return false;
        }
        if (L != NULL) {
            lua_rawset(L, table);  /* the key and the value */
        }
    }
}

static bool array(apps_jr_t *r, lua_State *L) {
    if (!enter(r, L)) {
        return false;
    }
    int table = 0;
    if (L != NULL) {
        lua_createtable(L, 0, 0);
        table = lua_gettop(L);
    }
    bool started = false;
    for (lua_Integer n = 1;; n++) {
        int next = more(r, ']', &started);
        if (next <= 0) {
            return next == 0;
        }
        if (!value(r, L)) {
            return false;
        }
        if (L != NULL) {
            lua_rawseti(L, table, n);
        }
    }
}

/* One value at r->p, pushed when L isn't NULL. */
static bool value(apps_jr_t *r, lua_State *L) {
    if (L != NULL && (++r->values & CLOCK_EVERY_MASK) == 0) {
        apps_vm_charge(L, 0);
    }
    skip_ws(r);
    if (r->p >= r->end) {
        return fail_unexpected(r, r->p);
    }
    switch (*r->p) {
        case '{': return object(r, L);
        case '[': return array(r, L);
        case '"': return L != NULL ? push_string(r, L) : skip_string(r);
        case 't':
            if (!literal(r, "true")) {
                return false;
            }
            if (L != NULL) {
                lua_pushboolean(L, 1);
            }
            return true;
        case 'f':
            if (!literal(r, "false")) {
                return false;
            }
            if (L != NULL) {
                lua_pushboolean(L, 0);
            }
            return true;
        case 'n':
            if (!literal(r, "null")) {
                return false;
            }
            if (L != NULL) {
                lua_pushlightuserdata(L, &null_tag);
            }
            return true;
        case '-':
        case '0': case '1': case '2': case '3': case '4': case '5': case '6': case '7': case '8': case '9': {
            const char *start = r->p;
            bool is_float;
            if (!scan_number(r, &is_float)) {
                return false;
            }
            if (L != NULL) {
                push_number(L, start, (size_t)(r->p - start), is_float);
            }
            return true;
        }
        default: return fail_unexpected(r, r->p);
    }
}

/* One value and nothing after it but whitespace. */
static bool document(apps_jr_t *r, lua_State *L) {
    if (!value(r, L)) {
        return false;
    }
    skip_ws(r);
    return r->p >= r->end || fail_unexpected(r, r->p);
}

/* msg into err, cut to err_len (it is ASCII, so any cut is a whole character). */
static void copy_err(char *err, size_t err_len, const char *msg) {
    if (err == NULL || err_len == 0) {
        return;
    }
    size_t n = strlen(msg);
    if (n >= err_len) {
        n = err_len - 1;
    }
    memcpy(err, msg, n);
    err[n] = '\0';
}

bool apps_json_validate(const char *s, size_t len, char *err, size_t err_len) {
    apps_jr_t r;
    apps_jr_init(&r, s, len);
    bool ok = r.err[0] == '\0' && document(&r, NULL);
    copy_err(err, err_len, ok ? "" : r.err);
    return ok;
}

bool apps_json_push(lua_State *L, const char *s, size_t len, char *err, size_t err_len) {
    int top = lua_gettop(L);
    apps_jr_t r;
    apps_jr_init(&r, s, len);
    luaL_checkstack(L, DECODE_SLOTS, "json");
    if (r.err[0] == '\0' && document(&r, L)) {
        copy_err(err, err_len, "");
        return true;
    }
    lua_settop(L, top);  /* the tables and keys built so far */
    copy_err(err, err_len, r.err);
    return false;
}

/* ==== The pull reader ==== */

void apps_jr_init(apps_jr_t *r, const char *s, size_t len) {
    static const char empty[] = "";
    if (s == NULL) {
        s = empty;
        len = 0;
    }
    memset(r, 0, sizeof *r);
    r->max_depth = APPS_JSON_MAX_DEPTH;
    r->start = r->p = s;
    r->end = s + len;
    if (len > APPS_JSON_MAX_INPUT) {
        r->p = r->end;
        snprintf(r->err, sizeof r->err, "input larger than %d KB", APPS_JSON_MAX_INPUT / 1024);
    }
}

/* Whether the container the reader is in has begun its elements. Needs depth > 0. */
static bool level_started(const apps_jr_t *r) {
    return ((r->started >> (r->depth - 1)) & 1u) != 0;
}

static void mark_started(apps_jr_t *r) {
    r->started |= 1u << (r->depth - 1);
}

bool apps_jr_object(apps_jr_t *r) {
    if (r->err[0] != '\0') {
        return false;
    }
    skip_ws(r);
    if (r->p >= r->end || *r->p != '{') {
        return fail_unexpected(r, r->p);
    }
    return enter(r, NULL);
}

bool apps_jr_array(apps_jr_t *r) {
    if (r->err[0] != '\0') {
        return false;
    }
    skip_ws(r);
    if (r->p >= r->end || *r->p != '[') {
        return fail_unexpected(r, r->p);
    }
    return enter(r, NULL);
}

/* Takes the next element of the container the reader is in, closed by close: more() on the level's
 * own comma state. */
static int next_element(apps_jr_t *r, char close, const char *caller) {
    if (r->err[0] != '\0') {
        return -1;
    }
    if (r->depth == 0) {
        char what[40];
        snprintf(what, sizeof what, "%s outside a container", caller);
        fail_at(r, what, r->p);
        return -1;
    }
    bool started = level_started(r);
    int next = more(r, close, &started);
    if (next == 1) {
        mark_started(r);
    }
    return next;
}

/* The next key and its colon: 1 with the key in key_out, 0 at the '}', -1 on an error; and, when lenient, 2 for a key that didn't fit or
 * had a \u0000, which is in key_out as "". */
static int read_key(apps_jr_t *r, char *key_out, size_t cap, bool lenient, const char *caller) {
    if (key_out != NULL && cap > 0) {
        key_out[0] = '\0';  /* so that whatever goes wrong leaves an empty string, not stale text */
    }
    int next = next_element(r, '}', caller);
    if (next != 1) {
        return next;
    }
    skip_ws(r);
    if (r->p >= r->end || *r->p != '"') {
        fail_unexpected(r, r->p);
        return -1;
    }
    if (key_out == NULL || cap == 0) {
        fail_at(r, "no room for a string", r->p);
        return -1;
    }
    jstr_t s = {.out = key_out, .cap = cap - 1, .no_nul = true, .lenient = lenient};
    if (!scan_string(r, &s)) {
        key_out[0] = '\0';
        return -1;
    }
    if (s.lost) {
        key_out[0] = '\0';
        return expect(r, ':') ? 2 : -1;
    }
    key_out[s.len] = '\0';
    return expect(r, ':') ? 1 : -1;
}

int apps_jr_key(apps_jr_t *r, char *key_out, size_t cap) {
    return read_key(r, key_out, cap, false, "apps_jr_key");
}

int apps_jr_key_any(apps_jr_t *r, char *key_out, size_t cap) {
    return read_key(r, key_out, cap, true, "apps_jr_key_any");
}

int apps_jr_next(apps_jr_t *r) {
    return next_element(r, ']', "apps_jr_next");
}

bool apps_jr_string(apps_jr_t *r, char *out, size_t cap) {
    if (out != NULL && cap > 0) {
        out[0] = '\0';  /* so that whatever goes wrong leaves an empty string, not stale text */
    }
    if (r->err[0] != '\0') {
        return false;
    }
    skip_ws(r);
    if (r->p >= r->end || *r->p != '"') {
        return fail_unexpected(r, r->p);
    }
    if (out == NULL || cap == 0) {
        return fail_at(r, "no room for a string", r->p);
    }
    jstr_t s = {.out = out, .cap = cap - 1, .no_nul = true};
    if (!scan_string(r, &s)) {
        out[0] = '\0';
        return false;
    }
    out[s.len] = '\0';
    return true;
}

bool apps_jr_integer(apps_jr_t *r, int64_t *out) {
    if (r->err[0] != '\0') {
        return false;
    }
    skip_ws(r);
    if (r->p >= r->end) {
        return fail_unexpected(r, r->p);
    }
    const char *start = r->p;
    if (*start != '-' && !is_digit(*start)) {
        return fail_at(r, "expected an integer", start);
    }
    bool is_float;
    if (!scan_number(r, &is_float)) {
        return false;
    }
    if (is_float) {
        return fail_at(r, "expected an integer", start);
    }
    int64_t v;
    if (!parse_int64(start, (size_t)(r->p - start), &v)) {
        return fail_at(r, "integer out of range", start);
    }
    *out = v;
    return true;
}

bool apps_jr_skip(apps_jr_t *r) {
    return r->err[0] == '\0' && value(r, NULL);
}

bool apps_jr_end(apps_jr_t *r) {
    if (r->err[0] != '\0') {
        return false;
    }
    skip_ws(r);
    if (r->p < r->end) {
        return fail_unexpected(r, r->p);
    }
    return r->depth == 0 || fail_unexpected(r, r->end);
}

const char *apps_jr_raw_value(apps_jr_t *r, size_t *len) {
    if (r->err[0] != '\0') {
        return NULL;
    }
    skip_ws(r);
    const char *start = r->p;
    if (!value(r, NULL)) {
        return NULL;
    }
    if (len != NULL) {
        *len = (size_t)(r->p - start);
    }
    return start;
}

size_t apps_json_compact(const char *s, size_t len, char *out, size_t cap) {
    if (cap == 0) {
        return 0;
    }
    size_t n = 0;
    bool in_string = false;
    for (size_t i = 0; i < len; i++) {
        char c = s[i];
        if (!in_string && (c == ' ' || c == '\t' || c == '\n' || c == '\r')) {
            continue;
        }
        if (n + 1 >= cap) {  /* this byte and the NUL won't fit */
            out[0] = '\0';
            return 0;
        }
        out[n++] = c;
        if (in_string && c == '\\' && i + 1 < len) {  /* the escaped byte is part of the string whatever it is */
            if (n + 1 >= cap) {
                out[0] = '\0';
                return 0;
            }
            out[n++] = s[++i];
        } else if (c == '"') {
            in_string = !in_string;
        }
    }
    out[n] = '\0';
    return n;
}

/* ==== Encoding ==== */

/* The output is a userdata at a fixed absolute stack index, grown by making a bigger one, copying and
 * replacing it at that index. A luaL_Buffer can't do this job: its box must be at the top of the stack
 * whenever it grows, and the encoder keeps lua_next's key there. */
typedef struct {
    lua_State *L;
    int box;          /* the stack index of the userdata holding the output */
    char *buf;        /* that userdata's memory */
    size_t len, cap;  /* bytes written, bytes it holds */
    size_t max;       /* the most output there may be, which the userdata never outgrows; 0 for no limit */
    unsigned values;  /* values visited, for the clock check */
    char err[96];     /* "" until the first error */
} enc_t;

#define ENC_FIRST_CAP 256

static bool enc_fail(enc_t *e, const char *msg) {
    if (e->err[0] == '\0') {
        snprintf(e->err, sizeof e->err, "%s", msg);
    }
    return false;
}

/* "can't encode a <type>" for the value at idx, or "... a <type> key". */
static bool enc_fail_type(enc_t *e, int idx, bool key) {
    if (e->err[0] == '\0') {
        if (key) {
            snprintf(e->err, sizeof e->err, "can't encode a %s key", luaL_typename(e->L, idx));
        } else {
            snprintf(e->err, sizeof e->err, "can't encode a %s", luaL_typename(e->L, idx));
        }
    }
    return false;
}

/* Room for the output to reach need bytes: a userdata twice the size (or enough), the old text copied
 * in, replacing the old one at e->box. The capped heap counts both until the old one is collected,
 * and a refusal raises (apps_json.h). */
static void out_grow(enc_t *e, size_t need) {
    size_t cap = e->cap;
    while (cap < need) {
        if (cap > SIZE_MAX / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    if (e->max != 0 && cap > e->max) {
        cap = e->max;  /* need is within it: out_put refused anything past */
    }
    char *grown = lua_newuserdatauv(e->L, cap, 0);
    memcpy(grown, e->buf, e->len);
    lua_replace(e->L, e->box);
    e->buf = grown;
    e->cap = cap;
}

/* A failure is sticky: after a refusal nothing more is written, and encode_at, which every value passes through, ends the walk, so the
 * callers that only write needn't check. That a text would outgrow the limit is a failure like any other: no memory is asked for it. */
static void out_put(enc_t *e, const char *s, size_t n) {
    if (n == 0 || e->err[0] != '\0') {
        return;
    }
    if (e->max != 0 && n > e->max - e->len) {
        enc_fail(e, APPS_JSON_TOO_LARGE);
        return;
    }
    if (n > e->cap - e->len) {
        out_grow(e, e->len + n);
    }
    memcpy(e->buf + e->len, s, n);
    e->len += n;
}

/* The escape table of a JSON string, the only copy: the two-byte forms for '"' '\' and \b \f \n \r \t, \u00xx (lowercase hex) for every
 * other byte under 0x20, and every other byte as it is. */
size_t apps_json_escape(char *out, const char *s, size_t n) {
    static const char hex[] = "0123456789abcdef";
    size_t len = 0;
    for (size_t i = 0; i < n; i++) {
        unsigned char c = (unsigned char)s[i];
        char esc[6] = {'\\', 0, 0, 0, 0, 0};
        size_t k = 2;
        switch (c) {
            case '"': esc[1] = '"'; break;
            case '\\': esc[1] = '\\'; break;
            case '\b': esc[1] = 'b'; break;
            case '\f': esc[1] = 'f'; break;
            case '\n': esc[1] = 'n'; break;
            case '\r': esc[1] = 'r'; break;
            case '\t': esc[1] = 't'; break;
            default:
                if (c >= 0x20) {
                    esc[0] = (char)c;
                    k = 1;
                } else {
                    memcpy(esc, "\\u00", 4);
                    esc[4] = hex[c >> 4];
                    esc[5] = hex[c & 0xF];
                    k = 6;
                }
                break;
        }
        if (out != NULL) {
            memcpy(out + len, esc, k);
        }
        len += k;
    }
    return len;
}

/* A string: quoted, '"', '\' and the bytes under 0x20 escaped (apps_json_escape), valid UTF-8 or an error. */
static bool put_string(enc_t *e, const char *s, size_t n) {
    const unsigned char *p = (const unsigned char *)s, *end = p + n, *run = p;
    if (e->err[0] != '\0') {
        return false;
    }
    /* Its text is at least n bytes and the two quotes: a string that can't fit is refused without a scan, whatever its length. */
    if (e->max != 0 && (e->max - e->len < 2 || n > e->max - e->len - 2)) {
        return enc_fail(e, APPS_JSON_TOO_LARGE);
    }
    out_put(e, "\"", 1);
    while (p < end) {
        unsigned char c = *p;
        if (c >= 0x80) {
            int k = apps_json_utf8_len(p, end);
            if (k <= 0) {  /* invalid, or cut short by the end of the string */
                return enc_fail(e, "can't encode a string that isn't valid UTF-8");
            }
            p += k;
        } else if (c >= 0x20 && c != '"' && c != '\\') {
            p++;
        } else {
            out_put(e, (const char *)run, (size_t)(p - run));
            char esc[6];
            out_put(e, esc, apps_json_escape(esc, (const char *)p, 1));
            p++;
            run = p;
        }
    }
    out_put(e, (const char *)run, (size_t)(end - run));
    out_put(e, "\"", 1);
    return true;
}

static NOINLINE void put_integer(enc_t *e, lua_Integer v) {
    char buf[24];  /* a sign and up to 19 digits */
    char *end = buf + sizeof buf, *p = end;
    lua_Unsigned u = v < 0 ? 0u - (lua_Unsigned)v : (lua_Unsigned)v;
    do {
        *--p = (char)('0' + u % 10);
        u /= 10;
    } while (u != 0);
    if (v < 0) {
        *--p = '-';
    }
    out_put(e, p, (size_t)(end - p));
}

/* The shortest of "%.15g", "%.16g" and "%.17g" that strtod reads back as the same double, so 0.1 is "0.1" and not the
 * "0.10000000000000001" that "%.17g" gives on a host. 17 digits always read back, so that is the last resort and isn't
 * checked. A text with no '.', 'e' or 'E' (3, -0, 2^53) gets ".0", so it decodes as a float again.
 *
 * Why not "%.17g" alone: a host's printf prints a double's exact digits and picolibc's its shortest ones, padded or cut
 * to the precision, so one double came out as two texts (0.1 on the board). Under this rule they agree for every
 * double but the subnormals, where 15 exact digits already read back: 5e-324 is 4.94065645841247e-324 on a host and
 * 5e-324 on the board. The two texts decode to the same double.
 *
 * Cost, on the S3: 32 us for a float that reads back at 15 digits (one printf, one strtod), 109 us for one that needs
 * all 17 (three printf, two strtod), whatever its exponent (Task 15's torture). The buffer is this leaf's. */
static NOINLINE bool put_float(enc_t *e, lua_Number n) {
    if (!isfinite(n)) {
        return enc_fail(e, "can't encode NaN or infinity");
    }
    const double d = (double)n;
    char buf[32];  /* the longest, -1.2345678901234567e-308, has 24 characters, and ".0" adds 2 */
    int len = 0;
    for (int digits = 15; digits <= 17; digits++) {
        len = snprintf(buf, sizeof buf - 2, "%.*g", digits, d);
        if (len < 0 || (size_t)len >= sizeof buf - 2) {
            return enc_fail(e, "can't encode a number");  /* not reachable: %.17g of a double fits */
        }
        if (digits < 17 && strtod(buf, NULL) == d) {
            break;
        }
    }
    if (strpbrk(buf, ".eE") == NULL) {
        memcpy(buf + len, ".0", 3);
        len += 2;
    }
    out_put(e, buf, (size_t)len);
    return true;
}

static bool encode_at(enc_t *e, int idx, int depth);

/* The table at idx, which is the level-th container open. An array when its keys are exactly
 * 1..n (n >= 1), else an object. One lua_next pass tells which (it stops at the first key that
 * can't be an array's): the keys are distinct, so all of them integers from 1 with the largest
 * equal to their count means 1..n. No metamethod runs: lua_next and lua_rawgeti are raw. */
static bool encode_table(enc_t *e, int idx, int level) {
    lua_State *L = e->L;
    if (level > APPS_JSON_MAX_DEPTH) {
        return enc_fail(e, "nested deeper than 16");
    }
    luaL_checkstack(L, ENCODE_SLOTS, "json");
    /* lua_next walks every slot the table was ever grown to, empty or not, once to decide array or object and
     * again to write it, and no count hook fires in C: charge the walk to the call's budget (a table emptied
     * after it grew costs its size, not what it holds), so a table shared many times is killed, not a spin. */
    apps_vm_charge(L, apps_vm_table_slots(L, idx) + 1);
    lua_Integer count = 0, largest = 0;
    bool sequence = true;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {
        lua_pop(L, 1);  /* the value; lua_next wants the key */
        if ((++count & CLOCK_EVERY_MASK) == 0) {
            apps_vm_charge(L, 0);  /* the clock: this walk writes nothing, so encode_at's checks don't run */
        }
        if (lua_isinteger(L, -1) && lua_tointeger(L, -1) >= 1) {
            if (lua_tointeger(L, -1) > largest) {
                largest = lua_tointeger(L, -1);
            }
        } else {
            sequence = false;
            lua_pop(L, 1);  /* the key lua_next won't finish with */
            break;
        }
    }
    if (sequence && count > 0 && largest == count) {
        out_put(e, "[", 1);
        for (lua_Integer i = 1; i <= count; i++) {
            if (i > 1) {
                out_put(e, ",", 1);
            }
            lua_rawgeti(L, idx, i);
            if (!encode_at(e, lua_gettop(L), level)) {
                return false;
            }
            lua_pop(L, 1);
        }
        out_put(e, "]", 1);
        return true;
    }
    out_put(e, "{", 1);
    bool first = true;
    lua_pushnil(L);
    while (lua_next(L, idx) != 0) {  /* the key at -2, the value at -1 */
        if (!first) {
            out_put(e, ",", 1);
        }
        first = false;
        if (lua_type(L, -2) == LUA_TSTRING) {
            size_t n;
            const char *s = lua_tolstring(L, -2, &n);  /* a string key: not converted in place */
            if (!put_string(e, s, n)) {
                return false;
            }
        } else if (lua_isinteger(L, -2)) {
            out_put(e, "\"", 1);
            put_integer(e, lua_tointeger(L, -2));
            out_put(e, "\"", 1);
        } else {
            return enc_fail_type(e, -2, true);
        }
        out_put(e, ":", 1);
        if (!encode_at(e, lua_gettop(L), level)) {
            return false;
        }
        lua_pop(L, 1);  /* the value */
    }
    out_put(e, "}", 1);
    return true;
}

/* The value at idx, depth containers deep. */
static bool encode_at(enc_t *e, int idx, int depth) {
    lua_State *L = e->L;
    if (e->err[0] != '\0') {
        return false;  /* an earlier write was refused */
    }
    if ((++e->values & CLOCK_EVERY_MASK) == 0) {
        apps_vm_charge(L, 0);  /* a float costs up to 109 us on the S3: the clock, inside one big table */
    }
    switch (lua_type(L, idx)) {
        case LUA_TNIL:
            out_put(e, "null", 4);
            return true;
        case LUA_TBOOLEAN:
            if (lua_toboolean(L, idx)) {
                out_put(e, "true", 4);
            } else {
                out_put(e, "false", 5);
            }
            return true;
        case LUA_TNUMBER:
            if (lua_isinteger(L, idx)) {
                put_integer(e, lua_tointeger(L, idx));
                return true;
            }
            return put_float(e, lua_tonumber(L, idx));
        case LUA_TSTRING: {
            size_t n;
            const char *s = lua_tolstring(L, idx, &n);
            return put_string(e, s, n);
        }
        case LUA_TLIGHTUSERDATA:
            if (lua_touserdata(L, idx) == &null_tag) {
                out_put(e, "null", 4);
                return true;
            }
            return enc_fail_type(e, idx, false);
        case LUA_TTABLE: return encode_table(e, idx, depth + 1);
        default: return enc_fail_type(e, idx, false);
    }
}

/* Encodes the value at idx into a new userdata, left on top of the stack, with the text in e. False
 * with e->err set for a value that can't be encoded (the stack then holds whatever the walk left). */
static bool encode_into_box(enc_t *e, lua_State *L, int idx, size_t max) {
    idx = lua_absindex(L, idx);
    luaL_checkstack(L, ENCODE_SLOTS, "json");
    e->L = L;
    e->len = 0;
    e->cap = ENC_FIRST_CAP;
    e->max = max;
    e->values = 0;
    e->err[0] = '\0';
    e->buf = lua_newuserdatauv(L, e->cap, 0);
    e->box = lua_gettop(L);
    return encode_at(e, idx, 0) && e->err[0] == '\0';
}

char *apps_json_encode_value(lua_State *L, int idx, void *(*realloc_fn)(void *, size_t), size_t *len, char *err,
                             size_t err_len) {
    return apps_json_encode_value_max(L, idx, realloc_fn, 0, len, err, err_len);
}

char *apps_json_encode_value_max(lua_State *L, int idx, void *(*realloc_fn)(void *, size_t), size_t max, size_t *len, char *err,
                                 size_t err_len) {
    int top = lua_gettop(L);
    enc_t e;
    char *text = NULL;
    const char *message = "";
    if (len != NULL) {
        *len = 0;
    }
    if (encode_into_box(&e, L, idx, max)) {
        text = realloc_fn != NULL ? realloc_fn(NULL, e.len + 1) : NULL;
        if (text != NULL) {
            memcpy(text, e.buf, e.len);
            text[e.len] = '\0';
            if (len != NULL) {
                *len = e.len;
            }
        } else {
            message = "out of memory";
        }
    } else {
        message = e.err;
    }
    lua_settop(L, top);
    copy_err(err, err_len, message);
    return text;
}

/* ==== The module ==== */

static int json_decode(lua_State *L) {
    apps_vm_check(L);
    size_t len;
    const char *s = luaL_checklstring(L, 1, &len);
    char err[96];
    if (!apps_json_push(L, s, len, err, sizeof err)) {
        return luaL_error(L, "json.decode: %s", err);
    }
    return 1;
}

static int json_encode(lua_State *L) {
    apps_vm_check(L);
    luaL_checkany(L, 1);
    enc_t e;
    if (!encode_into_box(&e, L, 1, 0)) {
        return luaL_error(L, "json.encode: %s", e.err);
    }
    lua_pushlstring(L, e.buf, e.len);
    return 1;
}

static const luaL_Reg apps_json_funcs[] = {
    {"encode", json_encode},
    {"decode", json_decode},
    {NULL, NULL},
};

static int open_json(lua_State *L) {
    lua_createtable(L, 0, 3);
    luaL_setfuncs(L, apps_json_funcs, 0);
    lua_pushlightuserdata(L, &null_tag);
    lua_setfield(L, -2, "null");
    lua_setglobal(L, "json");
    return 0;
}

void apps_json_open(lua_State *L) {
    int top = lua_gettop(L);
    if (lua_checkstack(L, 2)) {
        lua_pushcfunction(L, open_json);
        /* Protected, so a heap that refuses doesn't abort: the VM is then out of memory and the next
         * apps_vm_call says so. */
        (void)lua_pcall(L, 0, 0, 0);
    }
    lua_settop(L, top);
}
