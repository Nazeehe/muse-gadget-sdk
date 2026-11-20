# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Exercise production PSRAM reply handlers and turn completion with host-side event sinks."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

from host_cjson import cjson_source

ROOT = Path(__file__).resolve().parents[1]
JSON = Path(os.environ.get(
    'CJSON_SOURCE_DIR', ROOT / 'managed_components/espressif__cjson/cJSON'
))


class ChatSession(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        source = (ROOT / 'components/muse/muse_chat_session.cpp').read_text()
        constants = source[source.index('#define MIC_RATE'):source.index('/* ---- Voice task')]
        types = source[source.index('enum phase_t'):source.index('/* 10 KB')]
        handlers = source[source.index('static int find_msg('):source.index('static void on_chat_ack(')]
        reset = source[source.index('static bool turn_start('):source.index('static void turn_begin(')]
        done = source[source.index('static void turn_done('):source.index('/* Ends any turn')]
        check = source[source.index('static void check_turn('):source.index('/* ---- Inbound dispatch')]
        chat = source[source.index('static void send_chat('):source.index('static void on_dictation_line(')]
        kinds = source.index('enum kind_t')
        kinds = source[kinds:source.index('\n', kinds) + 1]
        tts = source.index('static void start_tts(void)')
        start_tts = source[tts:source.index('\n}\n', tts) + 3]
        code = r'''
#include <atomic>
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "host_compat.h"
#include "cJSON.h"
#include "muse_options.h"
#include "minimp3.h"
#include "muse_chat_priv.h"
#if CONFIG_MUSE_BOARD_SENSECAP_WATCHER || CONFIG_MUSE_LATEST_REPLY
#include "muse_review.h"
#include "muse_text.h"
#endif
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
''' + constants + types + r'''
static turn_t s_turn;
static pending_t s_pending;
static std::atomic<bool> s_incoming{false};
static std::atomic<bool> s_incoming_used{true};
static char s_reply_shown[EV_TEXT];
static int64_t s_last_seq, s_last_row_seq, s_marks[4];
static void *psram_alloc(size_t n) { return malloc(n); }
static int captions, console_events;
static int done_events;
static int64_t clock_us = 12345;
static char done_text[EV_TEXT], console_type[32], console_fields[128];
enum mark_t { M_TEXT, M_DONE, M_SENT };
static void mark(mark_t) {}
static int64_t now_us() { return clock_us; }
static void emit(muse_hatch_ev_t type, const char *text) {
    if (s_turn.text) return;
    if (type == MUSE_HATCH_EV_REPLY) captions++;
    if (type == MUSE_HATCH_EV_DONE) {
        done_events++;
        strlcpy(done_text, text ? text : "", sizeof(done_text));
    }
}
void muse_hatch_console(const char *type, const char *, const char *fields, ...) {
    console_events++;
    strlcpy(console_type, type, sizeof(console_type));
    va_list args;
    va_start(args, fields);
    if (fields) vsnprintf(console_fields, sizeof(console_fields), fields, args);
    else console_fields[0] = '\0';
    va_end(args);
}
void muse_hatch_tail_words(const char *text, char *out, size_t cap) { strlcpy(out, text, cap); }
bool muse_hatch_caption_at(const char *text, size_t, char *out, size_t cap) {
    strlcpy(out, text, cap); return text[0];
}
static void turn_finish() { s_turn.phase = P_IDLE; }
static int fail_events;
static char fail_text[EV_TEXT];
static void turn_fail(const char *text) {
    fail_events++;
    strlcpy(fail_text, text, sizeof(fail_text));
    turn_finish();
}
static bool ensure_connected() { return true; }
bool muse_hatch_configured() { return true; }
static void resampler_init(resampler_t *, int, int) {}
static void on_dictation_end(bool) {}
static void log_marks() {}
#if CONFIG_MUSE_BOARD_SENSECAP_WATCHER || CONFIG_MUSE_LATEST_REPLY
/* The real review store, read back whole: one page holds all of it here. */
int muse_chat_caption_page(const char *text, int, char *out, size_t cap) {
    strlcpy(out, text, cap); return 1;
}
void muse_text_to_ascii(char *, size_t) {}
void muse_text_to_caption(char *, size_t) {}
#endif
''' + kinds + r'''static char chat_body[256];
static int64_t open_stream(kind_t kind, const char *verb, const char *path, const char *, const char *,
                           const char *body, bool end_body) {
    assert(kind == K_CHAT && !strcmp(verb, "POST") && !strcmp(path, "/chat/stream") && body && end_body);
    strlcpy(chat_body, body, sizeof(chat_body));
    return 7;
}
static bool send_body(int64_t, const uint8_t *, size_t, bool) { return true; }
static void disconnect(const char *) {}
static bool send_reset(int64_t) { return true; }
''' + done + reset + handlers + start_tts + check + chat + r'''
static void begin(bool typed = false) {
    clock_us = 12345;
    assert(turn_start(s_turn.gen + 1, typed));
    s_turn.phase = P_WAIT_REPLY;
    s_turn.chat_us = clock_us;
    s_turn.acked = true;
    strlcpy(s_turn.user_ids[0], "note", sizeof(s_turn.user_ids[0]));
    strlcpy(s_turn.user_ids[1], "parent", sizeof(s_turn.user_ids[1]));
    captions = console_events = 0;
    done_events = fail_events = 0;
    done_text[0] = fail_text[0] = console_type[0] = console_fields[0] = '\0';
}
static void event(const char *kind, const char *id, const char *parent = "", const char *text = "") {
    cJSON *root = cJSON_CreateObject(), *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "event", kind);
    cJSON_AddItemToObject(root, "payload", payload);
    cJSON_AddStringToObject(payload, "message_id", id);
    if (parent[0]) cJSON_AddStringToObject(payload, "reply_to_message_id", parent);
    cJSON_AddStringToObject(payload, "text", text);
    cJSON_AddStringToObject(payload, "display_text", text);
    on_event(root);
    cJSON_Delete(root);
}
static void rejected_deltas() {
    for (bool typed : {false, true}) {
        begin(typed);
        event("delta.message_start", "other", "elsewhere");
        event("delta.text_append", "other", "", "Wrong reply");
        event("delta.message_done", "other");
        event("message.assistant", "other", "", "Wrong final");
        assert(!s_turn.nmsgs && !captions && !console_events);
        assert(!s_turn.last_content_us && !s_turn.last_event_us);
        event("delta.message_start", "reply", "note");
        event("delta.text_append", "reply", "", "Our reply");
        event("delta.message_done", "reply");
        assert(s_turn.nmsgs == 1 && s_turn.msgs[0].done && s_turn.msgs[0].len == 9);
        assert(typed ? console_events == 2 : captions == 1);
        begin(typed);
        event("message.assistant", "other", "", "New turn");
        assert(s_turn.nmsgs == 1 && s_turn.msgs[0].done);
    }
}
static void valid_parents() {
    begin();
    event("message.assistant", "first", "note", "First");
    event("message.assistant", "second", "parent", "Second");
    event("message.assistant", "third", "first", "Third");
    event("message.assistant", "fourth", "", "Live");
    assert(s_turn.nmsgs == 4);
    for (int i = 0; i < s_turn.nmsgs; i++) assert(s_turn.msgs[i].done);
    cJSON *payload = cJSON_Parse("{\"parent_message_id\":\"elsewhere\"}");
    assert(bind_msg("fallback", payload) == -1);
    cJSON_Delete(payload);
    event("message.assistant", "fallback", "", "Wrong final");
    assert(s_turn.nmsgs == 4);
}
static void bounded_rejections() {
    begin();
    event("delta.message_start", "reply", "note");
    char id[20];
    for (int i = 0; i < 9; i++) { /* exceed the eight rejected IDs retained per turn */
        snprintf(id, sizeof(id), "other%d", i);
        event("delta.message_start", id, "elsewhere");
    }
    event("message.assistant", "other0", "", "Wrong final");
    event("message.assistant", id, "", "Overflow final");
    assert(s_turn.nmsgs == 1 && !captions && !console_events);
    event("delta.text_append", "reply", "", "Our reply");
    event("delta.message_done", "reply");
    event("message.assistant", "second", "note", "Second");
    assert(s_turn.nmsgs == 2 && s_turn.msgs[0].done && s_turn.msgs[1].done);
}
static char s_texts[MAX_MSGS * TEXT_MAX];
/* A voice reply streamed as these pieces; returns the text kept for its captions. */
static const char *voice_reply(std::initializer_list<const char *> pieces) {
    s_turn.texts = s_texts;   /* turn_start() keeps it */
    begin();
    event("delta.message_start", "long", "note");
    for (const char *piece : pieces) event("delta.text_append", "long", "", piece);
    event("delta.message_done", "long");
    return s_texts;
}
static void cut_inside_a_character() {
    /* The cut falls inside a two-byte character: it's left out whole, and so is
     * everything after it, even what would fit in the byte that frees. */
    static char head[TEXT_MAX];
    memset(head, 'a', TEXT_MAX - 2);
    head[TEXT_MAX - 2] = '\0';
    const char *kept = voice_reply({head, "\xc3\xa9" "bbb", "z"});
    assert(strlen(kept) == TEXT_MAX - 2 && !strpbrk(kept, "\xc3" "bz"));
    assert(s_turn.msgs[0].len == TEXT_MAX - 2 + 5 + 1);
    s_turn.texts = nullptr;
}
static void character_split_across_pieces() {
    /* A piece ends with a character's first byte, exactly at the cut; its last
     * byte comes in the next piece and doesn't fit. */
    static char head[TEXT_MAX];
    memset(head, 'a', TEXT_MAX - 2);
    head[TEXT_MAX - 2] = '\xc3';
    head[TEXT_MAX - 1] = '\0';
    const char *kept = voice_reply({head, "\xa9" "bbb"});
    assert(strlen(kept) == TEXT_MAX - 2 && !strpbrk(kept, "\xc3" "b"));
    s_turn.texts = nullptr;
}
static void paced_to_the_kept_text() {
    /* Speaker off, a reply is shown at reading pace: for the text kept, not
     * for the whole reply, which would crawl the kept part across its time. */
    static char reply[TEXT_MAX + 1000];
    memset(reply, 'a', sizeof(reply) - 1);
    reply[sizeof(reply) - 1] = '\0';
    const char *kept = voice_reply({reply});
    start_tts();
    const msg_t &m = s_turn.msgs[0];
    assert(m.tts == TTS_ACTIVE && m.len == sizeof(reply) - 1 && strlen(kept) == TEXT_MAX - 1);
    assert(m.pcm_frames == (uint32_t)((TEXT_MAX - 1) * MIC_RATE / TEXT_CHARS_PER_S));
    voice_reply({"Short reply"});   /* nothing left out: paced as before */
    start_tts();
    assert(s_turn.msgs[0].pcm_frames == (uint32_t)(11 * MIC_RATE / TEXT_CHARS_PER_S));
    s_turn.texts = nullptr;
}
static void capped_voice(bool partial) {
    begin();
    s_turn.agent_busy = true;
    /* An acked request Muse is working on lets go at DETACH_US instead (detach()). */
    s_turn.acked = partial;
    if (partial) {
        event("delta.message_start", "reply", "note");
        event("delta.text_append", "reply", "", "Partial reply");
    }
    clock_us = s_turn.start_us + TURN_CAP_US;
    check_turn();
    assert(s_turn.phase == P_WAIT_REPLY && !done_events);
    clock_us++;
    check_turn();
    assert(s_turn.phase == P_IDLE && !console_events);
    if (!partial) { /* no reply at all: the turn fails instead of ending */
        assert(fail_events == 1 && !strcmp(fail_text, "NO REPLY FROM MUSE") && !done_events);
        return;
    }
    assert(done_events == 1 && !fail_events);
    assert(!strcmp(done_text, "REPLY INCOMPLETE"));
    assert(s_turn.msgs[0].len == 13 && !s_turn.msgs[0].done);
}
static void completed_voice() {
    begin();
    event("message.assistant", "reply", "note", "Complete reply");
    s_turn.msgs[0].tts = TTS_FINISHED;
    clock_us += SETTLE_US + 1;
    check_turn();
    assert(s_turn.phase == P_IDLE && done_events == 1 && !done_text[0]);
    assert(!console_events);
}
static void capped_typed() {
    begin(true);
    s_turn.agent_busy = true;
    clock_us = s_turn.start_us + TEXT_TURN_CAP_US;
    check_turn();
    assert(s_turn.phase == P_WAIT_REPLY && !console_events);
    clock_us++;
    check_turn();
    assert(s_turn.phase == P_IDLE && !done_events && console_events == 1);
    assert(!strcmp(console_type, "done"));
    assert(!strcmp(console_fields, "\"messages\":0,\"complete\":false"));
}
static void completed_voice_at_cap(bool busy, int64_t quiet_us) {
    begin();
    clock_us = s_turn.start_us + TURN_CAP_US - quiet_us;
    event("message.assistant", "reply", "note", "Complete reply");
    s_turn.msgs[0].tts = TTS_FINISHED;
    s_turn.agent_busy = busy;
    clock_us = s_turn.start_us + TURN_CAP_US;
    check_turn();
    assert(s_turn.phase == P_WAIT_REPLY && !done_events);
    clock_us++;
    check_turn();
    assert(s_turn.phase == P_IDLE && done_events == 1);
    /* A busy agent may still be replying: the cap cut it off. */
    assert(!strcmp(done_text, busy ? "REPLY INCOMPLETE" : ""));
    assert(!console_events);
}
static void capped_voice_with_pending_playback(tts_t state) {
    begin();
    event("message.assistant", "reply", "note", "Complete reply");
    assert(s_turn.msgs[0].done);
    s_turn.msgs[0].tts = state;
    clock_us = s_turn.start_us + TURN_CAP_US + 1;
    check_turn();
    assert(s_turn.phase == P_IDLE && done_events == 1);
    assert(!strcmp(done_text, "REPLY INCOMPLETE"));
    assert(!console_events);
}
static void typed_reply_at_cap(bool finished, bool busy) {
    /* A finished typed reply waiting out the settle ends complete at the cap;
     * one still streaming, or with the agent still working, ends incomplete. */
    begin(true);
    clock_us = s_turn.start_us + TEXT_TURN_CAP_US - 1000000;
    if (finished) {
        event("message.assistant", "reply", "note", "Complete reply");
        s_turn.msgs[0].tts = TTS_FINISHED;
    } else {
        event("delta.message_start", "reply", "note");
        event("delta.text_append", "reply", "", "Partial reply");
    }
    s_turn.agent_busy = busy;
    clock_us = s_turn.start_us + TEXT_TURN_CAP_US;
    check_turn();
    assert(s_turn.phase == P_WAIT_REPLY);
    console_events = 0;
    clock_us++;
    check_turn();
    assert(s_turn.phase == P_IDLE && !done_events && console_events == 1);
    assert(!strcmp(console_type, "done"));
    assert(!strcmp(console_fields, finished && !busy ? "\"messages\":1,\"complete\":true"
                                                     : "\"messages\":1,\"complete\":false"));
}
/* A dictated turn names the device, so Muse keeps its reply short and plain;
 * a typed one doesn't. Both still ask for text. */
static void chat_request(bool spoken) {
    chat_body[0] = '\0';
    if (spoken) {
        assert(turn_start(s_turn.gen + 1, false));
        post_chat(" Hi");
    } else {
        text_begin("Hi");
    }
    assert(s_turn.phase == P_WAIT_REPLY && s_turn.text == !spoken);
    assert(!strcmp(chat_body, spoken ? "{\"message\":\"Hi\",\"output_modality\":\"text\",\"device\":\"mcu-wearable\"}"
                                     : "{\"message\":\"Hi\",\"output_modality\":\"text\"}"));
}
#if CONFIG_MUSE_BOARD_SENSECAP_WATCHER || CONFIG_MUSE_LATEST_REPLY
static char review[40000];
static const char *reviewed() {
    int page = 0, pages = 0;
    review[0] = 0;
    muse_review_close();
    if (muse_review_step(1)) assert(muse_review_page(review, sizeof(review), &page, &pages));
    muse_review_close();
    return review;
}
/* Production typed turns: text_begin() starts each one with generation 0. */
static void typed(const char *text) {
    text_begin(text);
    assert(s_turn.phase == P_WAIT_REPLY && s_turn.text && s_turn.gen == 0);
    s_turn.acked = true;
    strlcpy(s_turn.user_ids[0], "note", sizeof(s_turn.user_ids[0]));
}
static void reply(const char *id, const char *text) {
    event("delta.message_start", id, "note");
    event("delta.text_append", id, "", text);
    event("delta.message_done", id);
}
static void typed_turns_replace_the_review() {
    assert(muse_review_init() == 0);
    s_turn.texts = static_cast<char *>(calloc(MAX_MSGS, TEXT_MAX));
    typed("one");
    reply("r1", "First reply");
    assert(!strcmp(reviewed(), "First reply"));
    typed("two");
    reply("r2", "Second reply");
    assert(!strcmp(reviewed(), "Second reply"));   /* not joined to the first turn's */
    typed("three");
    reply("a", "Part one");
    reply("b", "Part two");
    assert(!strcmp(reviewed(), "Part one\n\nPart two"));   /* one turn's messages stay together */
    typed("four");   /* no reply: the last one stays */
    assert(!strcmp(reviewed(), "Part one\n\nPart two"));
    for (int i = 0; i < 40; i++) {   /* many serial turns never fill the store */
        char text[32];
        snprintf(text, sizeof(text), "Reply %d", i);
        typed("again");
        reply("r", text);
        assert(!strcmp(reviewed(), text));
    }
    begin(false);   /* a voice turn replaces a typed one, and the other way round */
    reply("v", "Spoken reply");
    assert(!strcmp(reviewed(), "Spoken reply"));
    typed("five");
    reply("t", "Typed again");
    assert(!strcmp(reviewed(), "Typed again"));
    free(s_turn.texts);
    s_turn.texts = nullptr;
}
static void review_keeps_the_whole_reply() {
    assert(muse_review_init() == 0);
    s_turn.texts = static_cast<char *>(calloc(MAX_MSGS, TEXT_MAX));
    static char long_text[5000];
    memset(long_text, 'a', sizeof(long_text) - 1);
    for (bool typed_turn : {false, true}) {
        begin(typed_turn);
        reply("long", long_text);   /* past the 1 KB live-caption copy */
        assert(!strcmp(reviewed(), long_text));
        begin(typed_turn);
        event("delta.message_start", "reply", "note");
        event("delta.text_append", "reply", "", "short delta");
        event("message.assistant", "reply", "note", "Complete final reply");
        assert(!strcmp(reviewed(), "Complete final reply"));
        event("delta.message_done", "reply");
        assert(!strcmp(reviewed(), "Complete final reply"));   /* one completion, stored once */
    }
    free(s_turn.texts);
    s_turn.texts = nullptr;
}
/* Keeping the whole reply for review doesn't slow the automatic captions:
 * they pace the same first CAPTION_TEXT bytes as other boards, well inside
 * the turn's cap, so a long completed reply isn't cut off as incomplete. */
static void review_keeps_caption_pace() {
    assert(muse_review_init() == 0);
    static char reply[5001];
    memset(reply, 'a', sizeof(reply) - 1);
    const char *kept = voice_reply({reply});
    start_tts();
    const msg_t &m = s_turn.msgs[0];
    assert(m.tts == TTS_ACTIVE && strlen(kept) == sizeof(reply) - 1);
    assert(m.pcm_frames == (uint32_t)((CAPTION_TEXT - 1) * MIC_RATE / TEXT_CHARS_PER_S));
    assert((int64_t)m.pcm_frames * 1000000 / MIC_RATE < TURN_CAP_US / 2);
    assert(!strcmp(reviewed(), reply));
    s_turn.texts = nullptr;
}
#endif
static void status(const char *code) {
    cJSON *root = cJSON_CreateObject(), *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "event", "agent.status");
    cJSON_AddItemToObject(root, "payload", payload);
    cJSON_AddStringToObject(payload, "activity_code", code);
    on_event(root);
    cJSON_Delete(root);
}
/* Plays out the turn's messages and lets it settle, as check_turn sees it. */
static void settle() {
    for (int i = 0; i < s_turn.nmsgs; i++) s_turn.msgs[i].tts = TTS_FINISHED;
    clock_us += BUSY_HOLD_US + 1;
    check_turn();
}
/* A spoken turn Muse acked and is still working on, 21 s on: it says so and lets go. */
static void detach() {
    pending_clear();
    begin();
    status("working");
    clock_us += DETACH_US + 1;
    check_turn();
    assert(s_turn.detached && s_turn.nmsgs == 1 && s_turn.msgs[0].done && captions == 1);
    s_turn.msgs[0].tts = TTS_FINISHED;
    clock_us += SETTLE_US - 1000000;
    status("working");   /* Muse's status keeps coming: the notice still ends the turn */
    clock_us += 1000001;
    check_turn();
    assert(s_turn.phase == P_IDLE && s_pending.active && s_pending.busy && done_events == 1);
}
#if CONFIG_MUSE_BOARD_SENSECAP_WATCHER || CONFIG_MUSE_LATEST_REPLY
/* The detached turn's "working on it" notice never replaces the last answer. */
static void detach_notice_keeps_the_review() {
    assert(muse_review_init() == 0);
    s_turn.texts = static_cast<char *>(calloc(MAX_MSGS, TEXT_MAX));
    begin();
    reply("earlier", "Earlier answer");
    assert(!strcmp(reviewed(), "Earlier answer"));
    detach();
    assert(!strcmp(reviewed(), "Earlier answer"));
    free(s_turn.texts);
    s_turn.texts = nullptr;
}
#endif
static void detaches_only_while_muse_works() {
    begin();   /* Muse doesn't say it's working: the turn waits, then fails as before */
    clock_us += DETACH_US + 1;
    check_turn();
    assert(!s_turn.detached && !s_turn.nmsgs && s_turn.phase == P_WAIT_REPLY);
    clock_us = s_turn.chat_us + REPLY_TIMEOUT_US + 1;
    check_turn();
    assert(fail_events == 1 && !strcmp(fail_text, "NO REPLY FROM MUSE") && !s_pending.active);
    s_incoming_used = false;   /* no voice task plays late replies (Voice PE): it waits as before */
    begin();
    status("working");
    clock_us += DETACH_US + 1;
    check_turn();
    assert(!s_turn.detached && !s_turn.nmsgs && s_turn.phase == P_WAIT_REPLY);
    event("message.assistant", "reply", "note", "Answer");
    settle();   /* ends while Muse is still busy, but nothing would play the rest */
    assert(s_turn.phase == P_IDLE && done_events == 1 && !s_pending.active);
    s_incoming_used = true;
    detach();
}
static void late_reply_matched_by_its_request() {
    detach();
    s_incoming = false;
    /* Live deltas name no request; another chat's row, or one naming nothing, isn't it. */
    event("delta.message_start", "late");
    event("delta.text_append", "late", "", "Streaming");
    event("delta.message_done", "late");
    event("message.assistant", "phone", "elsewhere", "Another chat");
    event("message.assistant", "proactive", "", "Unprompted");
    event("message.assistant", "late", "note", "");   /* text not ready yet */
    assert(s_turn.phase == P_IDLE && !s_incoming && !s_pending.replies);
    event("message.assistant", "late", "note", "The document is ready");
    assert(s_incoming.exchange(false) && s_pending.replies && s_turn.phase == P_IDLE);
    event("message.assistant", "late", "note", "The document is ready");   /* sent again */
    event("message.assistant", "later", "note", "And a summary");   /* before the voice task takes them */
    assert(cJSON_GetArraySize(s_pending.replies) == 2);
    incoming_play(s_turn.gen + 1);
    assert(s_turn.phase == P_WAIT_REPLY && s_turn.incoming && s_turn.agent_busy);
    assert(s_turn.nmsgs == 2 && !strcmp(s_turn.msgs[0].id, "late") && !strcmp(s_turn.msgs[1].id, "later"));
    assert(s_turn.msgs[0].done && s_turn.msgs[1].done);
    assert(s_turn.msgs[0].len == 21 && !s_pending.active && !s_pending.replies);
    /* Delivered, and Muse is done: nothing more is waited for. */
    status("online");
    settle();
    assert(s_turn.phase == P_IDLE && !s_pending.active);
    event("message.assistant", "after", "note", "Too late");
    assert(!s_incoming && !s_pending.replies);
}
static void pending_is_forgotten() {
    detach();   /* Muse stops working and nothing comes: forgotten soon after */
    status("online");
    pending_expire(clock_us + PENDING_IDLE_US - 1);
    assert(s_pending.active);
    pending_expire(clock_us + PENDING_IDLE_US + 1);
    assert(!s_pending.active);
    event("message.assistant", "late", "note", "Too late");
    assert(!s_incoming && !s_pending.replies);
    detach();   /* still working, but not for ever, however often Muse says so */
    int64_t armed = clock_us;
    for (clock_us = armed; clock_us < armed + PENDING_US; clock_us += 60 * 1000000LL) status("working");
    pending_expire(armed + PENDING_US - 1);
    assert(s_pending.active);
    pending_expire(armed + PENDING_US + 1);
    assert(!s_pending.active);
    detach();   /* a reply held for a resting board waits out Muse going idle, then the cap */
    event("message.assistant", "late", "note", "Held");
    status("online");
    pending_expire(clock_us + PENDING_IDLE_US + 1);
    assert(s_pending.active && s_incoming);
    pending_expire(clock_us + PENDING_US + 1);
    assert(!s_pending.active && !s_pending.replies && !s_incoming);
}
static void played_messages_are_not_replayed() {
    pending_clear();
    begin();
    status("working");
    event("delta.message_start", "first");
    event("delta.text_append", "first", "", "On it");
    event("delta.message_done", "first");
    settle();   /* still working after the hold: the rest comes later */
    assert(s_turn.phase == P_IDLE && done_events == 1 && s_pending.active && s_pending.nplayed == 1);
    s_incoming = false;
    event("message.assistant", "first", "note", "On it");   /* its row, after it played */
    assert(!s_incoming && !s_pending.replies);
    event("message.assistant", "second", "note", "Done now");
    assert(s_incoming.exchange(false));
    incoming_play(s_turn.gen + 1);
    event("message.assistant", "first", "note", "On it");
    assert(s_turn.nmsgs == 1 && !strcmp(s_turn.msgs[0].id, "second"));
    event("message.assistant", "third", "first", "Following up on it");   /* answers what played before */
    assert(s_turn.nmsgs == 2 && !strcmp(s_turn.msgs[1].id, "third"));
    int64_t armed = s_pending.armed_us;
    settle();   /* still working: armed again for the same request, as of its first turn */
    assert(s_pending.active && s_pending.nplayed == 3 && s_pending.armed_us == armed);
    event("message.assistant", "first", "note", "On it");
    event("message.assistant", "second", "note", "Done now");
    assert(!s_incoming && !s_pending.replies);
}
static void status_event(const char *parent, const char *code) {
    cJSON *root = cJSON_CreateObject(), *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "event", "task.status");
    cJSON_AddItemToObject(root, "payload", payload);
    cJSON_AddStringToObject(payload, "status", code);
    cJSON_AddStringToObject(payload, "reply_to_message_id", parent);
    on_event(root);
    cJSON_Delete(root);
}
static void others_status_and_late_play() {
    detach();   /* another request finishing doesn't end the wait for this one */
    status_event("someone-else", "completed");
    assert(s_pending.busy);
    pending_expire(clock_us + PENDING_IDLE_US + 1);
    assert(s_pending.active);
    status_event("note", "completed");   /* its own does */
    assert(!s_pending.busy);
    detach();   /* a row held past the cap (a long rest, no connection) doesn't play */
    event("message.assistant", "late", "note", "Too old");
    clock_us += PENDING_US + 1;
    fail_events = 0;
    incoming_play(s_turn.gen + 1);
    assert(!s_turn.nmsgs && s_turn.phase == P_IDLE && fail_events == 1 && !fail_text[0]);
    assert(!s_pending.active && !s_pending.replies && !s_incoming);
}
static void chained_and_mid_stream_replies() {
    detach();   /* a reply to the request's reply belongs to it too */
    event("message.assistant", "a", "note", "First");
    event("message.assistant", "b", "a", "Then this");
    event("message.assistant", "c", "elsewhere", "Not this");
    assert(cJSON_GetArraySize(s_pending.replies) == 2);
    event("delta.message_start", "d");   /* starts streaming before the late turn does */
    event("delta.text_append", "d", "", "First ");
    incoming_play(s_turn.gen + 1);
    assert(s_turn.nmsgs == 2 && !strcmp(s_turn.msgs[1].id, "b"));
    event("delta.text_append", "d", "", "second");   /* only its end: wait for the whole row */
    event("delta.message_done", "d");
    assert(s_turn.nmsgs == 2);
    event("message.assistant", "d", "note", "First second");
    assert(s_turn.nmsgs == 3 && s_turn.msgs[2].done && s_turn.msgs[2].len == 12);
    event("delta.message_start", "e");   /* one that starts during the late turn streams as usual */
    event("delta.text_append", "e", "", "More");
    assert(s_turn.nmsgs == 4 && s_turn.msgs[3].len == 4);
}
static void held_rows_outlive_a_newer_request() {
    detach();   /* request A: its reply is held, then a press comes first */
    event("message.assistant", "a1", "note", "A's answer");
    assert(s_pending.replies);
    begin();   /* request B, which Muse is also still working on */
    strlcpy(s_turn.user_ids[0], "b", sizeof(s_turn.user_ids[0]));
    strlcpy(s_turn.user_ids[1], "b", sizeof(s_turn.user_ids[1]));
    status("working");
    clock_us += DETACH_US + 1;
    check_turn();
    settle();
    assert(s_pending.active && !strcmp(s_pending.ids[0], "b") && s_pending.replies);
    event("message.assistant", "b1", "b", "B's answer");
    event("message.assistant", "a2", "note", "Not A's any more");   /* A is no longer remembered */
    incoming_play(s_turn.gen + 1);
    assert(s_turn.nmsgs == 2 && !strcmp(s_turn.msgs[0].id, "a1") && !strcmp(s_turn.msgs[1].id, "b1"));
    assert(!strcmp(s_turn.user_ids[0], "b") && !strcmp(s_turn.user_ids[1], "b"));
}
/* A numbered event: message rows are numbered apart from live deltas. */
static void numbered(const char *kind, const char *id, const char *parent, int seq, const char *field,
                     const char *text, bool padded = false) {
    cJSON *root = cJSON_CreateObject(), *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "event", kind);
    cJSON_AddNumberToObject(root, "seq", seq);
    cJSON_AddItemToObject(root, "payload", payload);
    cJSON_AddStringToObject(payload, "message_id", id);
    if (parent[0]) cJSON_AddStringToObject(payload, "reply_to_message_id", parent);
    cJSON_AddStringToObject(payload, field, text);
    if (padded) cJSON_AddStringToObject(payload, "padding", "unused by playback");
    on_event(root);
    cJSON_Delete(root);
}
static void late_rows_by_their_own_order_and_content() {
    detach();
    s_incoming = false;
    numbered("delta.text_append", "live", "", 1150, "text", "Streaming elsewhere");
    /* The finished row, numbered in the chat's order (below the live one), with content only. */
    numbered("message.assistant", "late", "note", 977, "content", "Answer", true);
    assert(s_incoming.exchange(false) && cJSON_GetArraySize(s_pending.replies) == 1);
    cJSON *row = cJSON_GetArrayItem(s_pending.replies, 0);
    assert(cJSON_GetArraySize(row) == 3 && !cJSON_GetObjectItem(row, "padding"));
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(row, "display_text")), "Answer"));
    numbered("message.assistant", "again", "note", 977, "content", "A replayed row");   /* same number: dropped */
    assert(cJSON_GetArraySize(s_pending.replies) == 1);
    /* A long answer keeps only what a turn keeps. */
    static char longer[TEXT_MAX + 100];
    memset(longer, 'x', sizeof(longer) - 1);
    numbered("message.assistant", "long", "note", 978, "display_text", longer);
    row = cJSON_GetArrayItem(s_pending.replies, 1);
    assert(row && strlen(cJSON_GetStringValue(cJSON_GetObjectItem(row, "display_text"))) == TEXT_MAX - 1);
    /* The message id may come on the event rather than in its payload; one too long to keep is ignored. */
    cJSON *line = cJSON_Parse("{\"type\":\"event\",\"event\":\"message.assistant\",\"message_id\":\"outer\","
                              "\"payload\":{\"reply_to_message_id\":\"note\",\"display_text\":\"Finished\"}}");
    on_event(line);
    cJSON_Delete(line);
    char oversized[100];
    memset(oversized, 'i', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = '\0';
    numbered("message.assistant", oversized, "note", 979, "display_text", "Too long an id");
    assert(cJSON_GetArraySize(s_pending.replies) == 3);
    incoming_play(s_turn.gen + 1);
    assert(s_turn.nmsgs == 3 && !strcmp(s_turn.msgs[0].id, "late") && s_turn.msgs[0].done);
    assert(s_turn.msgs[0].len == 6 && s_turn.msgs[1].done && !strcmp(s_turn.msgs[2].id, "outer"));
}
static void canonical_reply(const char *id, const char *parent, const char *text) {
    cJSON *line = cJSON_Parse(R"({"type":"event","event":"message.assistant","payload":{
        "display_text":"","content":"","transcript":{"messages":[
            {"role":"user","content":[{"type":"text","text":"Private request"}]},
            {"role":"tool","content":[{"type":"text","text":"Private tool result"}]},
            {"role":"assistant","content":[{"type":"thinking","text":"Private reasoning"},
                {"type":"text","text":"First line"},{"type":"text","text":"Second line"}]}]}}})");
    assert(line);
    cJSON *payload = cJSON_GetObjectItem(line, "payload");
    cJSON_AddStringToObject(payload, "message_id", id);
    cJSON_AddStringToObject(payload, "reply_to_message_id", parent);
    if (text) {
        cJSON *rows = cJSON_GetObjectItem(cJSON_GetObjectItem(payload, "transcript"), "messages");
        cJSON *blocks = cJSON_GetObjectItem(cJSON_GetArrayItem(rows, 2), "content");
        cJSON_SetValuestring(cJSON_GetObjectItem(cJSON_GetArrayItem(blocks, 1), "text"), text);
        cJSON_DeleteItemFromArray(blocks, 2);
    }
    on_event(line);
    cJSON_Delete(line);
}
static void canonical_late_progress_and_completion() {
    detach();
    canonical_reply("foreign", "elsewhere", "Another request");
    assert(!s_incoming);
    canonical_reply("progress", "note", nullptr);
    assert(s_incoming && cJSON_GetArraySize(s_pending.replies) == 1);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(s_pending.replies, 0),
        "display_text")), "First line\nSecond line"));
    incoming_play(s_turn.gen + 1);
    assert(s_turn.nmsgs == 1 && s_turn.msgs[0].done && s_turn.msgs[0].tts == TTS_QUEUED);
    assert(s_turn.msgs[0].len == strlen("First line\nSecond line"));
    settle();
    assert(s_pending.active);
    canonical_reply("progress", "note", nullptr);
    assert(!s_incoming);
    status("online");
    canonical_reply("finished", "progress", "The task is complete.");
    assert(s_incoming);
    incoming_play(s_turn.gen + 1);
    assert(s_turn.nmsgs == 1 && !strcmp(s_turn.msgs[0].id, "finished"));
    assert(s_turn.msgs[0].tts == TTS_QUEUED && s_turn.msgs[0].len == 21);
    settle();
    assert(s_turn.phase == P_IDLE && !s_pending.active);
}
static void canonical_reply_during_notice() {
    begin();
    status("working");
    clock_us += DETACH_US + 1;
    check_turn();
    assert(s_turn.detached);
    canonical_reply("progress", "note", "I am checking now.");
    assert(s_turn.nmsgs == 2 && s_turn.msgs[1].tts == TTS_QUEUED && s_turn.msgs[1].len == 18);
    settle();
    assert(s_pending.active);
    canonical_reply("finished", "note", "All done.");
    assert(s_incoming);
}
static void canonical_reply_is_bounded_at_a_whole_character() {
    detach();
    static char long_text[TEXT_MAX + 100];
    memset(long_text, 'a', TEXT_MAX - 2);
    strcpy(long_text + TEXT_MAX - 2, "\xc3\xa9" "overflow");
    canonical_reply("long", "note", long_text);
    assert(s_incoming);
    cJSON *row = cJSON_GetArrayItem(s_pending.replies, 0);
    const char *text = cJSON_GetStringValue(cJSON_GetObjectItem(row, "display_text"));
    assert(text && strlen(text) == TEXT_MAX - 2 && !strchr(text, '\xc3'));
    incoming_play(s_turn.gen + 1);
    assert(s_turn.nmsgs == 1 && s_turn.msgs[0].tts == TTS_QUEUED && s_turn.msgs[0].len == TEXT_MAX - 2);
}
static void canonical_reply_reads_one_message_without_commentary() {
    detach();
    cJSON *line = cJSON_Parse(R"({"type":"event","event":"message.assistant","payload":{
        "message_id":"final","reply_to_message_id":"note","transcript":{"messages":[
            {"id":"assistant-msg-early","role":"assistant","content":[{"type":"text","text":"Already said"}]},
            {"id":"assistant-msg-final","role":"assistant","content":[
                {"type":"text","recipient":"commentary","text":"Hidden aside"},
                {"type":"text","text":"Final answer"}]},
            {"id":"assistant-msg-after","role":"assistant","content":[{"type":"text","text":"Not this row"}]}]}}})");
    assert(line);
    on_event(line);
    cJSON_Delete(line);
    assert(s_incoming && cJSON_GetArraySize(s_pending.replies) == 1);
    assert(!strcmp(cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetArrayItem(s_pending.replies, 0),
        "display_text")), "Final answer"));
}
static void canonical_reply_never_reads_another_message() {
    detach();
    cJSON *line = cJSON_Parse(R"({"type":"event","event":"message.assistant","payload":{
        "message_id":"tool","reply_to_message_id":"note","transcript":{"messages":[
            {"id":"assistant-msg-early","role":"assistant","content":[{"type":"text","text":"Already said"}]},
            {"id":"assistant-msg-tool","role":"assistant","content":[
                {"type":"text","recipient":"commentary","text":"Hidden aside"}]}]}}})");
    assert(line);
    on_event(line);
    cJSON_Delete(line);
    assert(!s_incoming && cJSON_GetArraySize(s_pending.replies) == 0);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 36: canonical_late_progress_and_completion(); break;
    case 37: canonical_reply_during_notice(); break;
    case 38: canonical_reply_is_bounded_at_a_whole_character(); break;
    case 39: canonical_reply_reads_one_message_without_commentary(); break;
    case 40: canonical_reply_never_reads_another_message(); break;
    case 0: rejected_deltas(); break;
    case 1: valid_parents(); break;
    case 2: bounded_rejections(); break;
    case 3: cut_inside_a_character(); break;
    case 4: character_split_across_pieces(); break;
    case 5: paced_to_the_kept_text(); break;
    case 6: capped_voice(false); break;
    case 7: capped_voice(true); break;
    case 8: completed_voice(); break;
    case 9: capped_typed(); break;
    case 10: completed_voice_at_cap(false, 1000000); break;
    case 11: completed_voice_at_cap(true, 1000000); break;
    case 12: completed_voice_at_cap(true, 4000000); break;
    case 13: capped_voice_with_pending_playback(TTS_QUEUED); break;
    case 14: capped_voice_with_pending_playback(TTS_ACTIVE); break;
    case 15: typed_reply_at_cap(true, false); break;
    case 16: typed_reply_at_cap(true, true); break;
    case 17: typed_reply_at_cap(false, false); break;
    case 21: chat_request(true); break;
    case 22: chat_request(false); break;
    case 23: detaches_only_while_muse_works(); break;
    case 24: late_reply_matched_by_its_request(); break;
    case 25: pending_is_forgotten(); break;
    case 26: played_messages_are_not_replayed(); break;
    case 27: held_rows_outlive_a_newer_request(); break;
    case 28: others_status_and_late_play(); break;
    case 29: chained_and_mid_stream_replies(); break;
    case 30: late_rows_by_their_own_order_and_content(); break;
    case 31: {
        muse_options_clear();
        s_turn.phase = P_IDLE;
        cJSON *line = cJSON_Parse(R"({"type":"event","event":"message.assistant","payload":{"message_id":"question","widgets":[{"kind":"option","widget_id":"choices","data":{"options":["Red","Green"]}}]}})");
        on_event(line); cJSON_Delete(line);
        muse_options_t q;
        assert(muse_options_snapshot(&q) && q.count == 2 && q.status == MUSE_OPTIONS_READY);
        assert(muse_options_request(q.generation,1));
        line = cJSON_Parse(R"({"type":"event","event":"widget.state_updated","payload":{"host_message_id":"question","widget_id":"choices","state":{"version":1,"data":{"selectedText":"Green"}}}})");
        on_event(line); cJSON_Delete(line);
        assert(muse_options_snapshot(&q) && q.selected == 1 && q.saved);
        break;
    }
    case 32: {
        s_turn.texts=static_cast<char *>(calloc(MAX_MSGS,TEXT_MAX));
        assert(s_turn.texts);
        for (bool typed : {false, true}) {
            muse_options_clear(); begin(typed);
            event("delta.message_start","lunch","note");
            event("delta.text_append","lunch","","What do you want for lunch?");
            event("delta.text_append","lunch","","\n\n[[hatch_widget:meal]]");
            cJSON *line=cJSON_Parse(R"({"type":"event","event":"delta.message_done","payload":{"message_id":"lunch","widgets":[{"kind":"option","widget_id":"meal","data":{"options":["Pasta","Salad"]}}]}})");
            on_event(line); cJSON_Delete(line);
            muse_options_t q;
            assert(muse_options_snapshot(&q) && !strcmp(q.question,"What do you want for lunch?"));
            /* Another message must not borrow the lunch question's streamed text. */
            line=cJSON_Parse(R"({"type":"event","event":"message.assistant","payload":{"message_id":"other-question","widgets":[{"kind":"option","widget_id":"other-choices","data":{"options":["Yes","No"]}}]}})");
            on_event(line); cJSON_Delete(line);
            assert(muse_options_snapshot(&q) && !q.question[0]);
        }
        free(s_turn.texts); s_turn.texts=nullptr;
        break;
    }
    case 33: {
        muse_options_clear(); s_turn.phase=P_IDLE;
        cJSON *line=cJSON_Parse(R"({"type":"event","event":"message.assistant","payload":{"message_id":"lunch","transcript":{"messages":[{"role":"assistant","content":[{"type":"text","text":"What do you want for lunch?\n[[hatch_widget:meal]]"}]}]},"widgets":[{"kind":"option","widget_id":"meal","data":{"options":["Pasta","Salad"]}}]}})");
        on_event(line); cJSON_Delete(line);
        muse_options_t q;
        assert(muse_options_snapshot(&q) && !strcmp(q.question,"What do you want for lunch?"));
        break;
    }
#if CONFIG_MUSE_BOARD_SENSECAP_WATCHER || CONFIG_MUSE_LATEST_REPLY
    case 18: typed_turns_replace_the_review(); break;
    case 19: review_keeps_the_whole_reply(); break;
    case 20: review_keeps_caption_pace(); break;
    case 35: detach_notice_keeps_the_review(); break;
#endif
    default: return 2;
    }
}
'''
        (out / 'session.cpp').write_text(code)
        flags = ['-Wall', '-Wextra', '-Werror', '-I', str(JSON),
                 '-I', str(ROOT / 'tests'), '-I', str(ROOT / 'components/muse'),
                 '-I', str(ROOT / 'components/minimp3/include')]
        commands = [
            [*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', *flags, '-pthread',
             '-I', str(ROOT / 'simulator/compat'), '-c', str(ROOT / 'components/muse/muse_options.c'), '-o', str(out / 'options.o')],
            [*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', *flags,
             '-c', str(ROOT / 'components/muse/muse_options_json.c'), '-o', str(out / 'options-json.o')],
            [*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', *flags,
             '-c', *cjson_source(JSON), '-o', str(out / 'cjson.o')],
            [*shlex.split(os.environ.get('CXX', 'c++')), '-std=gnu++17', *flags,
             str(out / 'session.cpp'), str(out / 'options.o'), str(out / 'options-json.o'), str(out / 'cjson.o'), '-o', str(out / 'session')],
        ]
        # The Watcher keeps whole replies for review: the same handlers, with the
        # real review store and fake FreeRTOS/heap headers.
        stubs = out / 'watcher'
        (stubs / 'freertos').mkdir(parents=True)
        (stubs / 'esp_err.h').write_text('typedef int esp_err_t;\n#define ESP_OK 0\n#define ESP_ERR_NO_MEM 1\n')
        (stubs / 'esp_heap_caps.h').write_text(
            '#include <stdlib.h>\n#define MALLOC_CAP_SPIRAM 1\n#define MALLOC_CAP_8BIT 2\n'
            '#define heap_caps_malloc(n, caps) malloc(n)\n')
        (stubs / 'freertos/FreeRTOS.h').write_text('#define portMAX_DELAY 0\n')
        (stubs / 'freertos/semphr.h').write_text(
            'typedef int SemaphoreHandle_t;\n#define xSemaphoreCreateMutex() 1\n'
            '#define xSemaphoreTake(lock, wait) ((void)0)\n#define xSemaphoreGive(lock) ((void)0)\n')
        watcher = ['-DCONFIG_MUSE_BOARD_SENSECAP_WATCHER=1', '-I', str(stubs), '-include', str(ROOT / 'tests/host_compat.h')]
        commands += [
            [*shlex.split(os.environ.get('CC', 'cc')), '-std=gnu11', *flags, *watcher,
             '-c', str(ROOT / 'components/muse/muse_review.c'), '-o', str(out / 'review.o')],
            [*shlex.split(os.environ.get('CXX', 'c++')), '-std=gnu++17', *flags, *watcher,
             str(out / 'session.cpp'), str(out / 'options.o'), str(out / 'options-json.o'), str(out / 'cjson.o'), str(out / 'review.o'), '-o', str(out / 'session-watcher')],
        ]
        latest = [flag.replace('CONFIG_MUSE_BOARD_SENSECAP_WATCHER', 'CONFIG_MUSE_LATEST_REPLY') for flag in watcher]
        commands += [
            [*shlex.split(os.environ.get('CXX', 'c++')), '-std=gnu++17', *flags, *latest,
             str(out / 'session.cpp'), str(out / 'options.o'), str(out / 'options-json.o'),
             str(out / 'cjson.o'), str(out / 'review.o'), '-o', str(out / 'session-latest')],
        ]
        for command in commands:
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
        cls.binary = out / 'session'
        cls.watcher_binary = out / 'session-watcher'

    def run_case(self, case, watcher=False):
        binary = self.watcher_binary if watcher else self.binary
        result = subprocess.run([str(binary), str(case)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_idle_widget_and_phone_selection_use_the_real_adapter(self):
        self.run_case(31)

    def test_widget_done_uses_only_its_own_streamed_question(self):
        self.run_case(32)

    def test_idle_canonical_transcript_supplies_the_picker_question(self):
        self.run_case(33)

    def test_rejected_deltas_do_not_emit_or_complete_voice_or_typed_replies(self):
        self.run_case(0)

    def test_ack_parent_reply_chain_and_parentless_messages_still_work(self):
        self.run_case(1)

    def test_rejection_capacity_preserves_correlation(self):
        self.run_case(2)

    def test_long_reply_cut_inside_a_character_keeps_whole_characters(self):
        self.run_case(3)

    def test_long_reply_character_split_across_pieces_is_left_out(self):
        self.run_case(4)

    def test_long_reply_is_paced_to_the_text_kept(self):
        self.run_case(5)

    def test_busy_voice_without_messages_fails_at_cap(self):
        self.run_case(6)

    def test_partial_voice_reply_is_incomplete_at_cap(self):
        self.run_case(7)

    def test_completed_voice_reply_has_empty_done_text(self):
        self.run_case(8)

    def test_typed_cap_keeps_complete_false(self):
        self.run_case(9)

    def test_completed_voice_at_cap_is_complete_only_when_the_agent_is_idle(self):
        for name, case in [('idle settle', 10), ('busy settle', 11), ('busy hold', 12)]:
            with self.subTest(window=name):
                self.run_case(case)

    def test_capped_voice_with_queued_or_active_playback_is_incomplete(self):
        for name, case in [('queued', 13), ('active', 14)]:
            with self.subTest(playback=name):
                self.run_case(case)

    def test_typed_reply_at_cap_is_complete_only_when_finished_and_idle(self):
        for name, case in [('finished idle', 15), ('finished busy', 16), ('streaming', 17)]:
            with self.subTest(reply=name):
                self.run_case(case)

    def test_watcher_rejected_deltas_and_parents_are_unchanged(self):
        for case in (0, 1, 2):
            with self.subTest(case=case):
                self.run_case(case, watcher=True)

    def test_watcher_review_replaces_each_typed_or_spoken_turn(self):
        self.run_case(18, watcher=True)

    def test_watcher_review_keeps_full_final_and_long_streamed_reply(self):
        self.run_case(19, watcher=True)

    def test_watcher_review_keeps_the_caption_pace_inside_the_turn_cap(self):
        self.run_case(20, watcher=True)

    def test_review_keeps_the_last_answer_through_a_detach_notice(self):
        self.run_case(35, watcher=True)

    def test_dictated_turns_name_the_device_and_typed_turns_do_not(self):
        for watcher in (False, True):
            for name, case in [('dictated', 21), ('typed', 22)]:
                with self.subTest(turn=name, watcher=watcher):
                    self.run_case(case, watcher=watcher)

    def test_slow_reply_detaches_only_while_muse_says_it_is_working(self):
        self.run_case(23)

    def test_transcript_only_progress_and_final_reply_play_after_detaching(self):
        self.run_case(36)

    def test_transcript_only_reply_arriving_during_the_notice_is_spoken(self):
        self.run_case(37)

    def test_long_transcript_only_late_reply_keeps_whole_utf8_characters(self):
        self.run_case(38)

    def test_transcript_only_reply_reads_its_own_message_without_commentary(self):
        self.run_case(39)

    def test_transcript_only_reply_never_reads_another_message_in_its_place(self):
        self.run_case(40)

    def test_late_reply_is_the_row_naming_the_request_and_plays_once(self):
        self.run_case(24)

    def test_pending_request_is_forgotten_when_muse_stops_or_after_the_cap(self):
        self.run_case(25)

    def test_messages_the_turn_played_are_not_announced_again(self):
        self.run_case(26)

    def test_held_rows_still_play_after_a_newer_request_is_remembered(self):
        self.run_case(27)

    def test_other_requests_status_and_overdue_rows_do_not_count(self):
        self.run_case(28)

    def test_late_reply_row_numbered_below_live_events_content_only_or_envelope_id_is_held(self):
        self.run_case(30)

    def test_late_turn_takes_chained_rows_and_never_half_a_message(self):
        self.run_case(29)

    def test_latest_reply_capture_and_caption_pace(self):
        for case in (0, 1, 2, 18, 19, 20, 35):
            with self.subTest(case=case):
                result = subprocess.run([str(self.binary.parent / 'session-latest'), str(case)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

# The production typed-turn code (turn_start, turn_fail, turn_done, text_begin, the reply's handlers and, with the apps, the app's sink
# and muse_hatch_text_turn_to), built with CONFIG_MUSE_APPS off and on, over host stubs of the rest of the session. The console's
# muse_hatch_console records every line it is given.
TYPED_TURNS = r'''
#include <cassert>
#include <cstdarg>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "host_compat.h"
#include "cJSON.h"
#include "minimp3.h"
#include "muse_chat_priv.h"
#include "muse_options.h"
#include <atomic>
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
@CONSTANTS@
@COMMANDS@
@TYPES@
static turn_t s_turn;
static pending_t s_pending;   /* a request held past its turn: never armed here */
static std::atomic<bool> s_incoming{false};
static muse_options_t *s_option_send;   /* a chosen option's turn: none here */
static bool s_option_sending;
void muse_options_result(uint32_t, muse_options_status_t, bool, const char *) {}
bool muse_options_receive(const cJSON *) { return false; }   /* no reply here carries options */
void muse_options_clear() {}
bool muse_options_receive_message(const cJSON *, const char *, const char *) { return false; }
static void *psram_alloc(size_t n) { return calloc(1, n); }
static char s_reply_shown[EV_TEXT];
static int64_t s_last_seq, s_last_row_seq, s_marks[4];
enum mark_t { M_SENT, M_TEXT, M_DONE };
static void mark(mark_t) {}
static int64_t now_us() { return 12345; }
static std::vector<std::string> said;   /* what the console printed, and what an app's sink heard */
static void emit(muse_hatch_ev_t, const char *) {}
void muse_hatch_console(const char *type, const char *text, const char *fields, ...) {
    char f[256] = "";
    if (fields) {
        va_list ap;
        va_start(ap, fields);
        vsnprintf(f, sizeof f, fields, ap);
        va_end(ap);
    }
    said.push_back(std::string("console ") + type + " [" + (text ? text : "") + "] " + f);
}
void muse_hatch_tail_words(const char *text, char *out, size_t cap) { strlcpy(out, text, cap); }
bool muse_hatch_caption_at(const char *text, size_t, char *out, size_t cap) { strlcpy(out, text, cap); return text[0]; }
static void turn_finish() { s_turn.phase = P_IDLE; }
static bool connects = true, configured = true, chat_fails;
static bool ensure_connected() { return connects; }
bool muse_hatch_configured() { return configured; }
static void resampler_init(resampler_t *, int, int) {}
@TURNS@
static void send_chat(const char *, const char *, bool, cJSON * = nullptr) {
    s_turn.chat_posted = true;
    if (chat_fails) {
        turn_fail("CAN'T REACH MUSE");
        return;
    }
    s_turn.phase = P_WAIT_REPLY;
}
@TEXT_BEGIN@
@HANDLERS@
static void acked() {
    s_turn.acked = true;
    strlcpy(s_turn.user_ids[0], "note", sizeof(s_turn.user_ids[0]));
}
static void event(const char *kind, const char *id, const char *parent = "", const char *text = "") {
    cJSON *root = cJSON_CreateObject(), *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "event", kind);
    cJSON_AddItemToObject(root, "payload", payload);
    cJSON_AddStringToObject(payload, "message_id", id);
    if (parent[0]) cJSON_AddStringToObject(payload, "reply_to_message_id", parent);
    cJSON_AddStringToObject(payload, "text", text);
    cJSON_AddStringToObject(payload, "display_text", text);
    on_event(root);
    cJSON_Delete(root);
}
static void status(const char *code) {
    cJSON *root = cJSON_CreateObject(), *payload = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "event");
    cJSON_AddStringToObject(root, "event", "agent.status");
    cJSON_AddItemToObject(root, "payload", payload);
    cJSON_AddStringToObject(payload, "activity_code", code);
    on_event(root);
    cJSON_Delete(root);
}
/* A reply of two messages: pieces that fall short of the first one's whole text, and a second that comes whole, with the agent busy
 * between. */
static void reply() {
    acked();
    status("thinking");
    event("delta.message_start", "reply", "note");
    event("delta.text_append", "reply", "", "Hi ");
    event("delta.text_append", "reply", "", "there");
    event("delta.message_done", "reply", "", "Hi there!");
    status("idle");
    event("message.assistant", "second", "note", "Bye");
    turn_done(true);
}
/* The console's typed turn, start to end. */
static void console_turn() {
    text_begin("hello");
    reply();
    text_begin("again");
    turn_fail("NO REPLY FROM MUSE");
}
@APPS@
int main(int argc, char **argv) {
    assert(argc == 2);
    if (!strcmp(argv[1], "console")) {
        console_turn();
    } else if (!run_apps(argv[1])) {
        return 2;
    }
    for (const std::string &line : said) {
        printf("%s\n", line.c_str());
    }
}
'''

# The app's side, in the build with the apps: its sink records what it hears, the chat's queue is a stub that takes one command, and
# each case prints what was said.
APPS_CASES = r'''
#include <atomic>
static void piece(void *user, const char *text, size_t len) {
    std::string shown;
    for (size_t i = 0; i < len; i++) {
        shown += text[i] == '\n' ? std::string("\\n") : std::string(1, text[i]);
    }
    said.push_back(std::string("piece ") + (const char *)user + " [" + shown + "]");
}
static void done(void *user, const char *err) {
    said.push_back(std::string("done ") + (const char *)user + " " + (err ? err : "(null)"));
}
static void final_text(void *user, const char *text, size_t len, size_t given) {
    said.push_back(std::string("final ") + (const char *)user + " [" + std::string(text, len) + "] " + std::to_string(given));
}
static char app_name[] = "app";
static const muse_hatch_text_sink_t SINK = {piece, done, app_name, final_text};
static void app_turn() {
    text_to("q", SINK);
}
/* muse_hatch_text_turn_to over a stub of the chat's queue. */
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
static int queue_room = 1;
static cmd_t queued;
static void *s_cmds = &queue_room;
static BaseType_t xQueueSend(void *, const cmd_t *cmd, int wait) {
    assert(wait == 0);   /* it never waits: the app task calls it inside a call */
    if (!queue_room) return pdFALSE;
    queue_room--;
    queued = *cmd;
    return pdTRUE;
}
@TURN_TO@
static char *dup(const char *s) {
    char *p = (char *)malloc(strlen(s) + 1);
    strcpy(p, s);
    return p;
}
/* The session's real handle(), with stubs for what its other commands use. */
static std::atomic<uint32_t> s_gen{0};
static int64_t s_auto_next_us, s_auto_backoff_us = AUTO_RETRY_MIN_US;
static bool call_busy() { return false; }
static void disconnect(const char *) {}
static void forget_vm() {}
static void turn_begin(uint32_t gen) {
    if (turn_start(gen, false)) {
        s_turn.phase = P_LISTEN;
    }
}
@HANDLE@
/* M56: a cancel the full queue drops is a warning in the log. */
#undef ESP_LOGW
#define ESP_LOGW(tag, fmt, ...) said.push_back(std::string("warn ") + (fmt))
@CANCEL_TO@
#undef ESP_LOGW
#define ESP_LOGW(...) ((void)0)
/* A command as the chat's queue carries it: what muse_hatch_text_turn_to or muse_hatch_text_cancel_to queued, then handled. */
static void queue_and_handle_text(const muse_hatch_text_sink_t *sink) {
    queue_room = 1;
    if (sink) {
        muse_hatch_text_turn_to(dup("hello"), sink);
    } else {
        cmd_t cmd{ CMD_TEXT, 0, dup("hello") };   /* the console's (muse_hatch_text_turn) */
        queued = cmd;
    }
    handle(queued);
}
static void queue_and_handle_cancel(void *user) {
    queue_room = 1;
    muse_hatch_text_cancel_to(user);
    assert(queued.type == CMD_SINK_CANCEL && queued.sink.user == user && !queued.text);
    handle(queued);
}
static bool run_apps(const char *name) {
    std::string n = name;
    if (n == "app_reply") {            /* the pieces and the end go to the sink, and nothing to the console */
        app_turn();
        reply();
    } else if (n == "voice_press") {   /* a voice press ends an app's turn: "interrupted" */
        app_turn();
        acked();
        event("delta.text_append", "reply", "note", "Sure");
        turn_start(7, false);
        assert(!s_turn.text && s_turn.gen == 7 && !s_turn.sink.done);
    } else if (n == "voice_turn") {    /* a voice turn refuses an app's turn: "busy with a voice turn", and goes on */
        turn_start(3, false);
        s_turn.phase = P_LISTEN;
        app_turn();
        assert(s_turn.phase == P_LISTEN && !s_turn.text && s_turn.gen == 3);
        said.push_back("voice turn goes on");
    } else if (n == "console_over_app") {  /* a console turn ends an app's ("interrupted"), and prints as ever */
        app_turn();
        console_turn();
    } else if (n == "app_over_console") {  /* an app's turn ends a console turn, which says so as ever */
        text_begin("hello");
        app_turn();
        reply();
    } else if (n == "unreachable") {   /* the turn's own errors, and "not set up" */
        connects = false;
        app_turn();
        configured = false;
        app_turn();
        connects = configured = true;
        chat_fails = true;
        app_turn();
    } else if (n == "sink_cancel") {   /* P30: an ask's cancel ends that ask's turn, and nothing else */
        static char other[] = "other";
        queue_and_handle_text(&SINK);       /* its own: ended with the console's word, once */
        queue_and_handle_cancel(app_name);
        assert(s_turn.phase == P_IDLE && !s_turn.sink.done);
        queue_and_handle_cancel(app_name);  /* stale: its turn has ended already */
        queue_and_handle_text(&SINK);       /* another ask's generation: this turn goes on, then its own ends it */
        queue_and_handle_cancel(other);
        if (s_turn.phase == P_WAIT_REPLY && s_turn.sink.user == app_name) said.push_back("app turn goes on");
        queue_and_handle_cancel(app_name);
        queue_and_handle_text(nullptr);     /* a console turn: untouched */
        queue_and_handle_cancel(app_name);
        if (s_turn.phase == P_WAIT_REPLY && s_turn.text && !s_turn.sink.done) said.push_back("console turn goes on");
        turn_fail("NO REPLY FROM MUSE");
        s_gen = 5;                          /* a voice turn: untouched */
        cmd_t press{ CMD_BEGIN, 5, nullptr };
        handle(press);
        queue_and_handle_cancel(app_name);
        if (s_turn.phase == P_LISTEN && !s_turn.text && s_turn.gen == 5) said.push_back("voice turn goes on");
        queue_room = 0;                     /* a full queue: dropped, never waited for, and said */
        queued = cmd_t{ CMD_WAKE, 0, nullptr };
        muse_hatch_text_cancel_to(app_name);
        assert(queued.type == CMD_WAKE);
    } else if (n == "final_text") {    /* I7: a message whose pieces fell short gets its whole text, as the console's reader does */
        app_turn();
        acked();
        event("delta.message_start", "one", "note");
        event("delta.text_append", "one", "", "Hi ");
        event("delta.text_append", "one", "", "there");
        event("delta.message_done", "one", "", "Hi there");     /* the pieces add up: nothing more */
        event("delta.message_start", "two", "note");
        event("delta.text_append", "two", "", "a");
        event("delta.text_append", "two", "", "c");
        event("delta.message_done", "two", "", "abc");          /* a piece skipped: its whole text, for its 2 bytes */
        event("delta.message_start", "three", "note");
        event("delta.text_append", "three", "", "long piece");
        event("delta.message_done", "three", "", "short");      /* shorter: the whole text all the same */
        event("delta.message_start", "four", "note");
        event("delta.text_append", "four", "", "x");
        event("delta.message_start", "five", "note");
        event("delta.text_append", "five", "", "y");
        event("delta.message_done", "four", "", "xyz");         /* not the message whose pieces came last: its pieces stand */
        event("delta.message_done", "five", "", "y");
        turn_done(true);
    } else if (n == "turn_to") {       /* refused at once on the caller's task, or queued with a copy of the sink */
        configured = false;
        muse_hatch_text_turn_to(dup("a"), &SINK);
        configured = true;
        muse_hatch_text_turn_to(dup("b"), &SINK);
        assert(queued.type == CMD_TEXT && !strcmp(queued.text, "b"));
        free(queued.text);
        muse_hatch_text_turn_to(dup("c"), &SINK);   /* the queue is full now */
        muse_hatch_text_sink_t mine = SINK;
        queue_room = 1;
        static char other[] = "other";
        mine.user = other;
        muse_hatch_text_turn_to(dup("d"), &mine);
        mine.user = app_name;                         /* the queued command kept its own copy */
        assert(queued.type == CMD_TEXT && !strcmp(queued.text, "d") && queued.sink.user == other);
        assert(queued.sink.piece == piece && queued.sink.done == done);
        free(queued.text);
    } else {
        return false;
    }
    return true;
}
'''


class AppTypedTurns(unittest.TestCase):
    """Review Focus 5, the session's side: an app's typed turn (muse.ask, CONFIG_MUSE_APPS) and voice share the session. A voice press
    ends it ("interrupted"), a voice turn refuses it ("busy with a voice turn"), its reply goes to its sink and none of it to the
    console, and the console's typed turns print what they always printed: the same lines, byte for byte, built with the apps or
    without."""

    @classmethod
    def setUpClass(cls):
        cls.tmp = tempfile.TemporaryDirectory()
        cls.addClassCleanup(cls.tmp.cleanup)
        out = Path(cls.tmp.name)
        source = (ROOT / 'components/muse/muse_chat_session.cpp').read_text()

        def between(start, end):
            return source[source.index(start):source.index(end, source.index(start))]

        parts = {
            '@CONSTANTS@': between('#define MIC_RATE', '/* ---- Voice task'),
            '@COMMANDS@': between('enum cmd_type_t', 'struct ev_t'),
            '@TYPES@': between('enum phase_t', '/* 10 KB'),
            '@TURNS@': between('static void turn_fail(', 'static void turn_begin('),
            '@TEXT_BEGIN@': between('static void text_begin(', 'static void on_dictation_line('),
            '@HANDLERS@': between('static int find_msg(', 'static void on_chat_ack('),
        }
        cjson = out / 'cjson.o'
        # Unused: the session's held-reply helpers (turn_detach, pending_arm, incoming_play), which no typed turn here reaches.
        flags = ['-Wall', '-Wextra', '-Werror', '-Wno-unused-function', '-I', str(JSON), '-I', str(ROOT / 'tests'),
                 '-I', str(ROOT / 'components/muse'), '-I', str(ROOT / 'components/minimp3/include')]
        cls.compile([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', *flags, '-c', str(JSON / 'cJSON.c'), '-o', str(cjson)])
        cls.binaries = {}
        # With the apps, the app's sink (its helpers come just before turn_fail) and muse_hatch_text_turn_to.
        helpers = between('#if CONFIG_MUSE_APPS\n/* ---- An app\'s typed turn', 'static void turn_fail(')
        turn_to = between('extern "C" void muse_hatch_text_turn_to(', '\n}\n') + '\n}\n'
        # P30: the sink's own cancel, through the session's real handle() (its other commands' helpers are stubs in APPS_CASES).
        cancel_to = between('extern "C" void muse_hatch_text_cancel_to(', '\n}\n') + '\n}\n'
        handle = between('static void handle(const cmd_t &cmd)', 'static void hatch_task(')
        # Less a held reply's turn and a chosen option's (CMD_INCOMING, CMD_OPTION), which no typed turn here sends.
        start = handle.index('    case CMD_INCOMING:')
        end = handle.index('\n    }\n', handle.index('    case CMD_OPTION: {', start)) + len('\n    }\n')
        handle = handle[:start] + '    case CMD_INCOMING:\n    case CMD_OPTION:\n        break;\n' + handle[end:]
        for apps in (False, True):
            code = TYPED_TURNS
            for marker, text in parts.items():
                code = code.replace(marker, helpers + text if apps and marker == '@TURNS@' else text)
            if apps:
                code = '#define CONFIG_MUSE_APPS 1\n' + code.replace(
                    '@APPS@', APPS_CASES.replace('@TURN_TO@', turn_to).replace('@CANCEL_TO@', cancel_to).replace('@HANDLE@', handle))
            else:
                code = code.replace('@APPS@', 'static bool run_apps(const char *) { return false; }\n')
            name = 'apps' if apps else 'plain'
            (out / f'{name}.cpp').write_text(code)
            cls.compile([*shlex.split(os.environ.get('CXX', 'c++')), '-std=gnu++17', *flags, str(out / f'{name}.cpp'), str(cjson),
                         '-o', str(out / name)])
            cls.binaries[apps] = out / name

    @staticmethod
    def compile(command):
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise AssertionError(result.stdout + result.stderr)

    def said(self, case, apps=True):
        result = subprocess.run([str(self.binaries[apps]), case], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout.splitlines()

    CONSOLE = [
        'console sent [] "bytes":5',
        'console busy [] "on":true',
        'console text [Hi ] "msg":0',
        'console text [there] "msg":0',
        'console final [Hi there!] "msg":0',
        'console message_done [] "msg":0,"bytes":9',
        'console busy [] "on":false',
        'console final [Bye] "msg":1',
        'console message_done [] "msg":1,"bytes":3',
        'console done [] "messages":2,"complete":true',
        'console sent [] "bytes":5',
        'console error [NO REPLY FROM MUSE] ',
    ]

    def test_the_console_path_is_unchanged(self):
        # The console's typed turn prints the same lines with the apps built in as without, and they are the lines it printed before.
        self.assertEqual(self.said('console', apps=False), self.CONSOLE)
        self.assertEqual(self.said('console', apps=True), self.CONSOLE)

    def test_ask_and_voice_share_the_session(self):
        # An app's reply: its pieces (a second message after a blank line, a message that came whole as one piece) and its end go to
        # the sink, and the console hears nothing of it.
        self.assertEqual(self.said('app_reply'), ['piece app [Hi ]', 'piece app [there]', 'final app [Hi there!] 8',
                                                  'piece app [\\n\\n]', 'piece app [Bye]', 'done app (null)'])
        # A voice press ends an app's turn, which hears "interrupted"; a voice turn refuses one, "busy with a voice turn", and goes on.
        self.assertEqual(self.said('voice_press'), ['piece app [Sure]', 'done app interrupted'])
        self.assertEqual(self.said('voice_turn'), ['done app busy with a voice turn', 'voice turn goes on'])
        # A console turn ends an app's turn, and prints its own lines as ever; an app's turn ends a console turn, which says so as ever.
        self.assertEqual(self.said('console_over_app'), ['done app interrupted', *self.CONSOLE])
        self.assertEqual(self.said('app_over_console'), ['console sent [] "bytes":5', 'console error [INTERRUPTED] ',
                                                         'piece app [Hi ]', 'piece app [there]', 'final app [Hi there!] 8',
                                                         'piece app [\\n\\n]', 'piece app [Bye]', 'done app (null)'])

    def test_an_apps_turn_fails_in_the_chats_words(self):
        self.assertEqual(self.said('unreachable'), ["done app CAN'T REACH MUSE", 'done app not set up', "done app CAN'T REACH MUSE"])

    def test_a_sink_cancel_ends_only_its_own_turn(self):
        # P30: muse_hatch_text_cancel_to queues the session's CMD_SINK_CANCEL (never waiting, dropped when the queue is full), and the
        # real handle() ends the turn in progress only when it is that ask's sink turn (its sink's user): once, with the console's word.
        # A stale cancel (its turn over, or another ask's generation), a console turn and a voice turn are left as they were.
        self.assertEqual(self.said('sink_cancel'), ['done app CANCELLED', 'app turn goes on', 'done app CANCELLED',
                                                    'console sent [] "bytes":5', 'console turn goes on',
                                                    'console error [NO REPLY FROM MUSE] ', 'voice turn goes on',
                                                    "warn an app's ask cancel was dropped: the chat's queue is full"])

    def test_a_message_whose_pieces_fell_short_gets_its_whole_text(self):
        # I7 (M47): at a message's end, when the pieces the sink was given don't add up to the message's whole text, the sink gets
        # the whole text with the count of those pieces' bytes (final), and replaces them; when they add up, nothing more. Only for
        # the message whose pieces came last: an earlier one's pieces stand.
        self.assertEqual(self.said('final_text'), [
            'piece app [Hi ]', 'piece app [there]',
            'piece app [\\n\\n]', 'piece app [a]', 'piece app [c]', 'final app [abc] 2',
            'piece app [\\n\\n]', 'piece app [long piece]', 'final app [short] 10',
            'piece app [\\n\\n]', 'piece app [x]', 'piece app [\\n\\n]', 'piece app [y]',
            'done app (null)'])

    def test_muse_hatch_text_turn_to_never_waits(self):
        # Refused at once, on the caller's task, when Muse isn't set up or the chat's queue is full; queued with its own copy of the sink.
        self.assertEqual(self.said('turn_to'), ['done app not set up', 'done app BUSY'])
