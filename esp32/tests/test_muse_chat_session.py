# Copyright (c) Meta Platforms, Inc. and affiliates.
# SPDX-License-Identifier: Apache-2.0
"""Exercise production PSRAM reply handlers with host-side event sinks."""
import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest

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
        code = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <initializer_list>
#include "host_compat.h"
#include "cJSON.h"
#include "minimp3.h"
#include "muse_chat_priv.h"
#define ESP_LOGI(...) ((void)0)
''' + constants + types + r'''
static turn_t s_turn;
static char s_reply_shown[EV_TEXT];
static int64_t s_last_seq, s_marks[4];
static int captions, console_events;
enum mark_t { M_TEXT, M_DONE };
static void mark(mark_t) {}
static int64_t now_us() { return 12345; }
static void emit(muse_hatch_ev_t type, const char *) {
    if (type == MUSE_HATCH_EV_REPLY) captions++;
}
void muse_hatch_console(const char *, const char *, const char *, ...) { console_events++; }
void muse_hatch_tail_words(const char *text, char *out, size_t cap) { strlcpy(out, text, cap); }
bool muse_hatch_caption_at(const char *text, size_t, char *out, size_t cap) {
    strlcpy(out, text, cap); return text[0];
}
static void turn_finish() { s_turn.phase = P_IDLE; }
static void turn_fail(const char *) { turn_finish(); }
static bool ensure_connected() { return true; }
bool muse_hatch_configured() { return true; }
static void resampler_init(resampler_t *, int, int) {}
''' + reset + handlers + r'''
static void begin(bool typed = false) {
    assert(turn_start(s_turn.gen + 1, typed));
    s_turn.phase = P_WAIT_REPLY;
    s_turn.acked = true;
    strlcpy(s_turn.user_ids[0], "note", sizeof(s_turn.user_ids[0]));
    strlcpy(s_turn.user_ids[1], "parent", sizeof(s_turn.user_ids[1]));
    captions = console_events = 0;
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
int main(int argc, char **argv) {
    assert(argc == 2);
    switch (atoi(argv[1])) {
    case 0: rejected_deltas(); break;
    case 1: valid_parents(); break;
    case 2: bounded_rejections(); break;
    default: return 2;
    }
}
'''
        (out / 'session.cpp').write_text(code)
        flags = ['-Wall', '-Wextra', '-Werror', '-I', str(JSON),
                 '-I', str(ROOT / 'tests'), '-I', str(ROOT / 'components/muse'),
                 '-I', str(ROOT / 'components/minimp3/include')]
        commands = [
            [*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', *flags,
             '-c', str(JSON / 'cJSON.c'), '-o', str(out / 'cjson.o')],
            [*shlex.split(os.environ.get('CXX', 'c++')), '-std=gnu++17', *flags,
             str(out / 'session.cpp'), str(out / 'cjson.o'), '-o', str(out / 'session')],
        ]
        for command in commands:
            result = subprocess.run(command, capture_output=True, text=True)
            if result.returncode:
                raise AssertionError(result.stdout + result.stderr)
        cls.binary = out / 'session'

    def run_case(self, case):
        result = subprocess.run([str(self.binary), str(case)], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_rejected_deltas_do_not_emit_or_complete_voice_or_typed_replies(self):
        self.run_case(0)

    def test_ack_parent_reply_chain_and_parentless_messages_still_work(self):
        self.run_case(1)

    def test_rejection_capacity_preserves_correlation(self):
        self.run_case(2)


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
#define ESP_LOGI(...) ((void)0)
#define ESP_LOGW(...) ((void)0)
@CONSTANTS@
@COMMANDS@
@TYPES@
static turn_t s_turn;
static char s_reply_shown[EV_TEXT];
static int64_t s_last_seq, s_marks[4];
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
static void send_chat(const char *, const char *) {
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
        flags = ['-Wall', '-Wextra', '-Werror', '-I', str(JSON), '-I', str(ROOT / 'tests'), '-I', str(ROOT / 'components/muse'),
                 '-I', str(ROOT / 'components/minimp3/include')]
        cls.compile([*shlex.split(os.environ.get('CC', 'cc')), '-std=c11', *flags, '-c', str(JSON / 'cJSON.c'), '-o', str(cjson)])
        cls.binaries = {}
        # With the apps, the app's sink (its helpers come just before turn_fail) and muse_hatch_text_turn_to.
        helpers = between('#if CONFIG_MUSE_APPS\n/* ---- An app\'s typed turn', 'static void turn_fail(')
        turn_to = between('extern "C" void muse_hatch_text_turn_to(', '\n}\n') + '\n}\n'
        # P30: the sink's own cancel, through the session's real handle() (its other commands' helpers are stubs in APPS_CASES).
        cancel_to = between('extern "C" void muse_hatch_text_cancel_to(', '\n}\n') + '\n}\n'
        handle = between('static void handle(const cmd_t &cmd)', 'static void hatch_task(')
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
