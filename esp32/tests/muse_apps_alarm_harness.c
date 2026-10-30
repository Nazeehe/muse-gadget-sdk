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
 * The system's alarms of Muse apps (apps_alarm.c), run for real on the host. Usage: muse_apps_alarm_harness <case>, one case per
 * test in tests/test_muse_apps_alarm.py; it exits 0 when the case holds, and otherwise prints why and every event.
 *
 * The esp_timer is the fake of tests/muse_apps_fake_esp_timer.c: one thread runs the callbacks, as the esp_timer task does, and
 * its clock is this harness's, a number a case moves. advance() moves it timer by timer: each one that comes due runs its
 * callback at its own time (an alarm, a retry 10 ms on, a chirp of the ring every 1.5 s), and the call returns once the thread
 * is idle again, so what a case looks at has all happened, in an order the clock decides and not the scheduler. What the alarms
 * ask of the rest of the gadget is an event each, in one log: the chirp asked of the voice task, the wake of the screen, the
 * fired callback (which asks the table a question, as the app task's post doesn't, to show that it runs outside the lock), and
 * the log lines. The esp_timer task never logs (a line from it is an error, and fails the case): what it records, the harness
 * prints as the app task does, by apps_alarm_log_drain, after each step of the clock, so the lines come in the order of the steps.
 * Most cases name the whole list of events a step made; the thread that hammers the table at once is the last.
 * The lock is a mutex of pthreads that checks its contract (taken again by its holder is an error), and the esp_timer task's
 * waits for it are recorded: a short try, never a wait without an end. App ids are as apps_id_valid has them (two letters at
 * least): "aa", "bb", "cc", and "app0" to "app2" in the stress case.
 */

#include <errno.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "apps_alarm.h"
#include "muse_apps_task_fakes.h"
#include "muse_input.h"
#include "muse_voice.h"

#define ARRAY_LEN(a) (sizeof(a) / sizeof((a)[0]))

/* ---- The clock ---- */

#define START_MS 100000  /* the clock's first reading: a while in, never 0 */

static _Atomic int64_t now_us = (int64_t)START_MS * 1000;

int64_t esp_timer_get_time(void) {
    return atomic_load(&now_us);
}

/* The clock moves on ms; each timer that comes due on the way runs at its own time. */
static void advance(int64_t ms) {
    int64_t target = atomic_load(&now_us) + ms * 1000;
    for (int rounds = 0;; rounds++) {
        if (rounds > 200000) {
            fprintf(stderr, "FAIL advance: timers keep coming due\n");
            _exit(1);
        }
        int64_t next = fake_esp_timer_next_due_us();
        if (next > target) {
            break;
        }
        if (next > atomic_load(&now_us)) {
            atomic_store(&now_us, next);
        }
        fake_esp_timer_sync();
        apps_alarm_log_drain();  /* the app task's next round: what the callbacks recorded is printed */
    }
    atomic_store(&now_us, target);
    fake_esp_timer_sync();
    apps_alarm_log_drain();
}

/* The clock jumps ms in one go, and every timer that came due in it runs, in the order they were due, at the time it landed (as the esp_timer task
 * would after a long stall, and as a thread that arms at random needs, which no step of timer by timer could keep up with). */
static void jump(int64_t ms) {
    atomic_fetch_add(&now_us, ms * 1000);
    fake_esp_timer_sync();
    apps_alarm_log_drain();
}

/* Milliseconds since the start (START_MS), and the clock moved to such a time: a case says when things happen, not how long since. */
static int64_t at_ms(void) {
    return atomic_load(&now_us) / 1000 - START_MS;
}

static void advance_to(int64_t ms) {
    advance(ms - at_ms());
}

/* ---- The events ---- */

#define EVENTS_MAX 4096
static pthread_mutex_t events_mu = PTHREAD_MUTEX_INITIALIZER;
static char *events[EVENTS_MAX];
static int n_events;

static void event(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void event(const char *fmt, ...) {
    char text[300];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(text, sizeof text, fmt, ap);
    va_end(ap);
    pthread_mutex_lock(&events_mu);
    if (n_events < EVENTS_MAX) {
        events[n_events++] = strdup(text);
    }
    pthread_mutex_unlock(&events_mu);
}

static int mark(void) {
    pthread_mutex_lock(&events_mu);
    int n = n_events;
    pthread_mutex_unlock(&events_mu);
    return n;
}

static int count_from(int from, const char *prefix) {
    size_t len = strlen(prefix);
    int n = 0;
    pthread_mutex_lock(&events_mu);
    for (int i = from; i < n_events; i++) {
        n += strncmp(events[i], prefix, len) == 0 ? 1 : 0;
    }
    pthread_mutex_unlock(&events_mu);
    return n;
}

static void dump_events(void) {
    pthread_mutex_lock(&events_mu);
    fprintf(stderr, "---- %d events ----\n", n_events);
    for (int i = 0; i < n_events; i++) {
        fprintf(stderr, "%5d %s\n", i, events[i]);
    }
    pthread_mutex_unlock(&events_mu);
}

static void fail_at(int line, const char *fmt, ...) __attribute__((format(printf, 2, 3), noreturn));
static void fail_at(int line, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    fprintf(stderr, "FAIL line %d: ", line);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
    dump_events();
    fflush(stderr);
    _exit(1);
}

#define CHECK(cond, ...)                   \
    do {                                   \
        if (!(cond)) {                     \
            fail_at(__LINE__, __VA_ARGS__); \
        }                                  \
    } while (0)

/* The events since `from` are these, exactly, in this order. */
static void expect_at(int line, int from, int n, const char *const *want) {
    pthread_mutex_lock(&events_mu);
    bool same = n_events - from == n;
    for (int i = 0; same && i < n; i++) {
        same = strcmp(events[from + i], want[i]) == 0;
    }
    pthread_mutex_unlock(&events_mu);
    if (!same) {
        fprintf(stderr, "the events since %d should be:\n", from);
        for (int i = 0; i < n; i++) {
            fprintf(stderr, "      %s\n", want[i]);
        }
        fail_at(line, "the events since %d are not those", from);
    }
}
#define EXPECT(from, ...)                                                           \
    do {                                                                            \
        const char *const want_[] = {__VA_ARGS__};                                  \
        expect_at(__LINE__, (from), (int)ARRAY_LEN(want_), want_);                  \
    } while (0)
#define EXPECT_NONE(from) expect_at(__LINE__, (from), 0, NULL)

/* What another thread got wrong: the case fails at its end. */
static atomic_int errors;
static char error_note[200];
static pthread_mutex_t error_mu = PTHREAD_MUTEX_INITIALIZER;

static void error(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    pthread_mutex_lock(&error_mu);
    if (atomic_fetch_add(&errors, 1) == 0) {
        vsnprintf(error_note, sizeof error_note, fmt, ap);
    }
    pthread_mutex_unlock(&error_mu);
    va_end(ap);
}

/* ---- What the alarms call ---- */

void fake_esp_log(char level, const char *tag, const char *fmt, ...) {
    char msg[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    if (strcmp(tag, "muse_apps") != 0) {
        error("a log line with the tag '%s'", tag);
    }
    if (fake_esp_timer_on_task()) {
        error("a log line from the esp_timer task, which serves every timer of the gadget and never waits for the console: %s", msg);
    }
    event("log %c %s", level, msg);
}

void muse_input_wake(const char *why) {
    event("wake %s", why != NULL ? why : "(null)");
}

void muse_voice_request_chirp(void) {
    event("chirp");
}

void vTaskDelay(const TickType_t ticks) {
    struct timespec ts = {0, (long)ticks * 1000000L};
    nanosleep(&ts, NULL);
}

/* Set while a fired callback of this harness runs: it asks the table, whose lock it takes as any caller does (not apps_alarm.c's own take). */
static _Thread_local bool in_callback;

/* The fired callback, as apps_task.c's is: it comes from the esp_timer task, and asks the table what the app task would, which
 * it can only if the table is free (the fake lock fails a take by its holder). */
static void on_fired(const char *app, const char *name) {
    in_callback = true;
    event("cb %s/%s left %lld", app, name, (long long)apps_alarm_remaining(app, name));
    in_callback = false;
}

/* The lock: a mutex of pthreads that fails a take by its holder, with a count of the takes the esp_timer task made and the longest
 * wait it asked for, and a way to make its next takes fail, as a table another task holds too long would. */
struct fake_mutex {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    bool held;
    pthread_t holder;
};
_Static_assert(sizeof(struct fake_mutex) <= sizeof(StaticSemaphore_t), "a fake mutex fits in its buffer");

static atomic_int busy_takes;                /* the esp_timer task's next takes (with a wait that ends) fail */
static _Atomic(TickType_t) timer_longest_take;

SemaphoreHandle_t xSemaphoreCreateMutexStatic(StaticSemaphore_t *buffer) {
    struct fake_mutex *m = (struct fake_mutex *)(void *)buffer;
    pthread_mutex_init(&m->mu, NULL);
    pthread_cond_init(&m->cv, NULL);
    m->held = false;
    return m;
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t m, TickType_t wait) {
    if (fake_esp_timer_on_task() && !in_callback) {
        if (wait > atomic_load(&timer_longest_take)) {
            atomic_store(&timer_longest_take, wait);
        }
        if (wait != portMAX_DELAY && atomic_load(&busy_takes) > 0) {
            atomic_fetch_sub(&busy_takes, 1);
            return pdFALSE;
        }
    }
    /* A wait that ends ends: after its time, with no lock (the esp_timer task's short try); one without an end is a failure at 5 s. */
    int64_t wait_ms = wait == portMAX_DELAY ? 5000 : (int64_t)wait * 1000 / configTICK_RATE_HZ;
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += (time_t)(wait_ms / 1000);
    deadline.tv_nsec += (long)(wait_ms % 1000) * 1000000L;
    if (deadline.tv_nsec >= 1000000000L) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&m->mu);
    if (m->held && pthread_equal(m->holder, pthread_self())) {
        pthread_mutex_unlock(&m->mu);
        error("the table's lock taken again by its holder");
        return pdFALSE;
    }
    while (m->held) {
        if (pthread_cond_timedwait(&m->cv, &m->mu, &deadline) == ETIMEDOUT && m->held) {
            pthread_mutex_unlock(&m->mu);
            if (wait == portMAX_DELAY) {
                error("the table's lock held for 5 s");
            }
            return pdFALSE;
        }
    }
    m->held = true;
    m->holder = pthread_self();
    pthread_mutex_unlock(&m->mu);
    return pdTRUE;
}

BaseType_t xSemaphoreGive(SemaphoreHandle_t m) {
    pthread_mutex_lock(&m->mu);
    bool mine = m->held && pthread_equal(m->holder, pthread_self());
    if (mine) {
        m->held = false;
        pthread_cond_signal(&m->cv);
    }
    pthread_mutex_unlock(&m->mu);
    if (!mine) {
        error("the table's lock given by a thread that doesn't hold it");
        return pdFALSE;
    }
    return pdTRUE;
}

/* ---- Helpers of the cases ---- */

static bool arm(const char *app, const char *name, int64_t ms, bool sound, bool wake) {
    return apps_alarm_arm(app, name, ms, sound, wake) == APPS_ALARM_ARMED;
}

static int64_t left(const char *app, const char *name) {
    return apps_alarm_remaining(app, name);
}

/* How many alarms of app the list has, and their names with the time left, joined with spaces ("u:1000 t:4999"). */
static int listing(const char *app, char *text, size_t cap) {
    char names[4][24];
    int64_t remaining[4];
    int n = apps_alarm_list(app, names, remaining);
    int used = snprintf(text, cap, "%s", "");
    for (int i = 0; i < n && used < (int)cap; i++) {
        used += snprintf(text + used, cap - (size_t)used, "%s%s:%lld", i > 0 ? " " : "", names[i], (long long)remaining[i]);
    }
    return n;
}

#define LISTING_IS(app, want)                                                                         \
    do {                                                                                              \
        char got_[200];                                                                               \
        listing((app), got_, sizeof got_);                                                            \
        CHECK(strcmp(got_, (want)) == 0, "the alarms of %s are '%s', not '%s'", (app), got_, (want)); \
    } while (0)

/* Takes every firing, as the app task does once it has told the app: how many were this call's. */
static int take_all(bool delivered) {
    apps_alarm_fired_t due[APPS_ALARMS_MAX];
    int n = apps_alarm_fired(due, APPS_ALARMS_MAX);
    int took = 0;
    for (int i = 0; i < n; i++) {
        took += apps_alarm_take(&due[i], delivered) ? 1 : 0;
    }
    return took;
}

/* Everything of these apps cancelled, firings too. */
static void clear_all(void) {
    static const char *const apps[] = {"aa", "bb", "cc"};
    for (size_t i = 0; i < ARRAY_LEN(apps); i++) {
        char names[4][24];
        int64_t remaining[4];
        int n = apps_alarm_list(apps[i], names, remaining);
        for (int k = 0; k < n; k++) {
            apps_alarm_cancel(apps[i], names[k]);
        }
    }
}

/* What a sound-and-wake alarm "app/name" does when it fires and the ring begins, in order: the chirp (under the lock), the wake
 * and the callback, then the log. */
#define FIRES_RINGING(who) \
    "chirp", "wake alarm", "cb " who " left 0", "log I alarm " who " fired (sound 1, wake 1)", "log I ring " who " chirp 1"

static void boot(void) {
    apps_alarm_set_fired_cb(on_fired);
}

static void finish(void) {
    CHECK(atomic_load(&errors) == 0, "%d errors, the first: %s", atomic_load(&errors), error_note);
    CHECK(atomic_load(&timer_longest_take) <= pdMS_TO_TICKS(20), "the esp_timer task waited %u ticks for the table: 20 ms at most",
          (unsigned)atomic_load(&timer_longest_take));
}

/* ---- The cases ---- */

/* An alarm is armed for a time, counts down (rounded up: the clock goes in microseconds), is listed earliest first with the time it
 * has left, is replaced by the next arming of its name, and is cancelled; a cancelled one never fires, and nothing was asked of
 * anyone meanwhile. */
static void arm_remaining_list_cancel(void) {
    int from = mark();
    CHECK(left("aa", "t") == -1, "nothing armed yet");
    CHECK(arm("aa", "t", 5000, true, true), "arm");
    CHECK(left("aa", "t") == 5000, "left %lld", (long long)left("aa", "t"));
    atomic_fetch_add(&now_us, 500);  /* half a millisecond on: 4999.5 ms left, which rounds up to 5000 */
    CHECK(left("aa", "t") == 5000, "rounded up: %lld", (long long)left("aa", "t"));
    advance(1);
    CHECK(left("aa", "t") == 4999, "4998.5 ms left rounds up to 4999, not %lld", (long long)left("aa", "t"));
    CHECK(left("aa", "u") == -1 && left("bb", "t") == -1, "another name or app has none");
    CHECK(arm("aa", "u", 1000, false, false) && arm("bb", "t", 90000, true, false), "arm two more");
    LISTING_IS("aa", "u:1000 t:4999");  /* earliest first, whatever order they were armed in */
    LISTING_IS("bb", "t:90000");
    LISTING_IS("cc", "");
    CHECK(arm("aa", "t", 20000, true, true), "replace");
    CHECK(left("aa", "t") == 20000, "the replacement's time: %lld", (long long)left("aa", "t"));
    LISTING_IS("aa", "u:1000 t:20000");
    apps_alarm_cancel("aa", "u");
    CHECK(left("aa", "u") == -1, "cancelled");
    LISTING_IS("aa", "t:20000");
    apps_alarm_cancel("aa", "u");  /* again, and ones that never were: nothing */
    apps_alarm_cancel("aa", "zz");
    apps_alarm_cancel("nobody", "t");
    CHECK(!apps_alarm_pending(), "nothing fired");
    advance(19999);
    EXPECT_NONE(from);
    apps_alarm_cancel("aa", "t");
    apps_alarm_cancel("bb", "t");
    advance(100000);
    EXPECT_NONE(from);  /* a cancelled alarm never fires */
}

/* Four to an app, eight in all, and a firing that waits holds its slot. A name is 1 to 23 of a-z, 0-9 and _; the time 1 ms to a
 * week; an app id 1 to 24 characters. An argument that isn't changes nothing. */
static void limits_and_names(void) {
    static const char *const names[] = {"n1", "n2", "n3", "n4"};
    for (int i = 0; i < 4; i++) {
        CHECK(arm("aa", names[i], 10000, true, true), "a/%s", names[i]);
    }
    CHECK(!arm("aa", "n5", 10000, true, true), "a fifth name of one app is refused");
    CHECK(arm("aa", "n1", 20000, true, true), "a name that is armed is replaced at the limit");
    CHECK(arm("bb", "n1", 10000, true, true), "another app has its own four");
    apps_alarm_cancel("aa", "n2");
    CHECK(arm("aa", "n5", 10000, true, true), "a cancelled one's place is free");
    CHECK(arm("bb", "n2", 10000, true, true) && arm("bb", "n3", 10000, true, true) && arm("bb", "n4", 10000, true, true), "b to four");
    CHECK(!arm("cc", "n1", 10000, true, true), "the table has eight: a ninth is refused");
    CHECK(arm("aa", "n1", 30000, true, true) && arm("bb", "n4", 30000, true, true), "replacing works with the table full");
    apps_alarm_cancel("bb", "n1");
    CHECK(arm("cc", "n1", 5000, true, false), "a free slot, for another app");
    CHECK(!arm("bb", "n5", 1, true, true), "b has three, but the table is full again");
    /* A firing that waits holds its slot: it counts for both limits, and shows in the list with nothing left. */
    int from = mark();
    advance(5000);
    EXPECT(from, "chirp", "cb cc/n1 left 0", "log I alarm cc/n1 fired (sound 1, wake 0)", "log I ring cc/n1 chirp 1");
    CHECK(apps_alarm_pending() && left("cc", "n1") == 0, "cc/n1 fired and waits");
    CHECK(!arm("bb", "n5", 1, true, true), "the waiting firing still holds its slot");
    LISTING_IS("cc", "n1:0");
    CHECK(take_all(true) == 1, "told");
    CHECK(arm("bb", "n5", 20000, true, true), "its slot is free once it is told");
    CHECK(left("cc", "n1") == -1, "gone from c's list");
    apps_alarm_ring_stop();
    clear_all();
    /* Names. */
    static const char *const good[] = {"a", "a_1", "0", "z9_", "abcdefghijklmnopqrstuvw"};  /* the last has 23 */
    static const char *const bad[] = {"", "abcdefghijklmnopqrstuvwx" /* 24 */, "A", "a-b", "a b", "a.b", "\xc3\xa9", "a\n"};
    for (size_t i = 0; i < ARRAY_LEN(good); i++) {
        CHECK(arm("aa", good[i], 1000, true, true), "'%s' is a name", good[i]);
        CHECK(left("aa", good[i]) == 1000, "armed");
        apps_alarm_cancel("aa", good[i]);
        CHECK(left("aa", good[i]) == -1, "cancelled");
    }
    for (size_t i = 0; i < ARRAY_LEN(bad); i++) {
        CHECK(!arm("aa", bad[i], 1000, true, true), "'%s' is not a name", bad[i]);
    }
    CHECK(!arm("aa", NULL, 1000, true, true), "no name");
    CHECK(!arm("aa", "x", 0, true, true) && !arm("aa", "x", -5, true, true) && !arm("aa", "x", 604800001LL, true, true),
          "times out of range");
    CHECK(arm("aa", "x", 1, true, true) && arm("aa", "y", 604800000, true, true), "1 ms and a week are in");
    CHECK(left("aa", "x") == 1 && left("aa", "y") == 604800000, "their times");
    char id24[25], id25[26];
    memset(id24, 'q', 24);
    id24[24] = '\0';
    memset(id25, 'q', 25);
    id25[25] = '\0';
    apps_alarm_cancel("aa", "x");
    CHECK(arm(id24, "x", 1000, true, true), "an id of 24 characters");
    CHECK(!arm(id25, "x", 1000, true, true) && !arm("", "x", 1000, true, true) && !arm(NULL, "x", 1000, true, true),
          "ids that aren't");
    CHECK(left(id24, "x") == 1000 && left(id25, "x") == -1 && left(NULL, "x") == -1 && left("aa", NULL) == -1, "asking about them");
    char names_out[4][24];
    int64_t remaining[4];
    CHECK(apps_alarm_list(NULL, names_out, remaining) == 0 && apps_alarm_list("", names_out, remaining) == 0, "listing no app");
    apps_alarm_cancel(id24, "x");
    apps_alarm_cancel("aa", "y");
}

/* An alarm that comes due is marked, rings, wakes the screen and tells the app task, in that order (the logs last), and keeps its
 * slot, with nothing left and on the pending word, until it is taken: once. The callback ran outside the table's lock. */
static void a_fired_alarm_waits_in_its_slot(void) {
    CHECK(arm("aa", "t", 5000, true, true), "arm");
    int from = mark();
    advance(4999);
    EXPECT_NONE(from);
    CHECK(!apps_alarm_pending() && left("aa", "t") == 1, "not yet");
    advance(1);
    EXPECT(from, FIRES_RINGING("aa/t"));
    CHECK(apps_alarm_pending(), "it is pending");
    CHECK(left("aa", "t") == 0, "nothing left");
    LISTING_IS("aa", "t:0");
    apps_alarm_fired_t due[4];
    CHECK(apps_alarm_fired(due, 4) == 1, "one firing");
    CHECK(strcmp(due[0].app, "aa") == 0 && strcmp(due[0].name, "t") == 0 && due[0].sound && due[0].wake &&
              due[0].fired_ms == START_MS + 5000,
          "%s/%s sound %d wake %d at %lld", due[0].app, due[0].name, due[0].sound, due[0].wake, (long long)due[0].fired_ms);
    CHECK(apps_alarm_pending() && apps_alarm_fired(due, 4) == 1, "copying changes nothing");
    CHECK(apps_alarm_fired(NULL, 4) == 0 && apps_alarm_fired(due, 0) == 0, "no room, no copies");
    int after = mark();
    CHECK(apps_alarm_take(&due[0], true), "taken");
    CHECK(!apps_alarm_take(&due[0], true) && !apps_alarm_take(&due[0], false), "a firing is told once");
    CHECK(!apps_alarm_pending() && left("aa", "t") == -1, "and its slot is free");
    LISTING_IS("aa", "");
    EXPECT_NONE(after);  /* told: the ring goes on */
    advance(1500);
    EXPECT(after, "chirp", "log I ring aa/t chirp 2");
}

/* sound and wake are each the alarm's own: no chirp without sound, no wake without wake, and the log says what it did. */
static void sound_and_wake_options(void) {
    CHECK(arm("aa", "both", 100, true, true) && arm("aa", "wake", 200, false, true) && arm("bb", "sound", 300, true, false) &&
              arm("bb", "none", 400, false, false),
          "arm four");
    int from = mark();
    advance(100);
    EXPECT(from, FIRES_RINGING("aa/both"));
    from = mark();
    advance(100);
    EXPECT(from, "wake alarm", "cb aa/wake left 0", "log I alarm aa/wake fired (sound 0, wake 1)");
    from = mark();
    advance(100);  /* the ring goes (the first alarm's): the minute is made longer, and that is all: no chirp, and no line for one */
    EXPECT(from, "cb bb/sound left 0", "log I alarm bb/sound fired (sound 1, wake 0)");
    from = mark();
    advance(100);
    EXPECT(from, "cb bb/none left 0", "log I alarm bb/none fired (sound 0, wake 0)");
    CHECK(take_all(true) == 4, "four firings, in the table until told");
    apps_alarm_ring_stop();
}

/* The ring: a chirp when it starts and one every 1.5 s, 40 in the minute, then it ends by itself and says so. */
static void the_ring_chirps_until_its_minute_is_over(void) {
    CHECK(arm("aa", "t", 1000, true, false), "arm");
    int from = mark();
    advance(1000);
    EXPECT(from, "chirp", "cb aa/t left 0", "log I alarm aa/t fired (sound 1, wake 0)", "log I ring aa/t chirp 1");
    CHECK(take_all(true) == 1, "told: the ring is the system's, and goes on");
    for (int k = 2; k <= 40; k++) {
        from = mark();
        advance(APPS_RING_PERIOD_MS - 1);
        EXPECT_NONE(from);
        advance(1);
        char line[40];
        snprintf(line, sizeof line, "log I ring aa/t chirp %d", k);
        EXPECT(from, "chirp", line);
    }
    from = mark();
    advance(APPS_RING_PERIOD_MS - 1);
    EXPECT_NONE(from);
    advance(1);  /* 60 s after it began */
    EXPECT(from, "log I ring stopped (timeout)");
    from = mark();
    advance(3 * APPS_RING_MAX_MS);
    EXPECT_NONE(from);
    CHECK(count_from(0, "chirp") == 40, "%d chirps in all", count_from(0, "chirp"));
    CHECK(fake_esp_timer_next_due_us() == INT64_MAX, "no timer is left running");
}

/* sound.stop ends the ring, and says so once; with nothing ringing it does nothing; another reason reads as given. */
static void stopping_the_ring(void) {
    int from = mark();
    apps_alarm_ring_stop();
    apps_alarm_app_ended("aa", "app stopped");
    EXPECT_NONE(from);  /* nothing rings */
    CHECK(arm("aa", "t", 100, true, true), "arm");
    advance(100);
    CHECK(take_all(true) == 1, "told");
    advance(1500);
    from = mark();
    apps_alarm_ring_stop();
    EXPECT(from, "log I ring stopped (sound.stop)");
    from = mark();
    apps_alarm_ring_stop();
    advance(10 * APPS_RING_PERIOD_MS);
    EXPECT_NONE(from);  /* once, and no chirp after it */
    CHECK(count_from(0, "chirp") == 2, "two chirps before it");
    /* Another alarm rings again, and its end has its own words. */
    CHECK(arm("aa", "u", 100, true, true), "arm again");
    advance(100);
    CHECK(take_all(true) == 1, "told");
    from = mark();
    apps_alarm_app_ended("aa", "app crashed");  /* the app that was told it is gone: nothing could stop the ring but the minute */
    EXPECT(from, "log I ring stopped (app crashed)");
    advance(5000);
    CHECK(count_from(0, "chirp") == 3, "no chirp after it");
}

/* A second alarm that rings while the ring goes makes it a minute longer from then: a chirp at once, the period unchanged, the log
 * naming the alarm that rang last. The first alarm fires at 1000 ms, and the ring chirps at 1000 + 1500 k; the second fires at 30500,
 * a minute after which is 90500: the tick that ends it is the first one past that, at 91000. */
static void a_second_alarm_makes_the_ring_a_minute_longer(void) {
    CHECK(arm("aa", "one", 1000, true, true) && arm("bb", "two", 30500, true, true), "arm two");
    advance_to(1000);
    CHECK(take_all(true) == 1, "told");
    int from = mark();
    advance_to(30499);  /* the ticks of 2500 to 29500: 19 chirps, and nothing yet of the second */
    CHECK(count_from(from, "chirp") == 19 && count_from(from, "cb ") == 0, "%d chirps", count_from(from, "chirp"));
    from = mark();
    advance_to(30500);  /* the minute is made longer: no chirp of its own, and no line for one */
    EXPECT(from, "wake alarm", "cb bb/two left 0", "log I alarm bb/two fired (sound 1, wake 1)");
    CHECK(take_all(true) == 1, "told");
    from = mark();
    advance_to(31000);  /* the next tick of the period (the 21st chirp), named for the alarm that rang last */
    EXPECT(from, "chirp", "log I ring bb/two chirp 21");
    advance_to(60999);
    advance_to(61000);  /* the first alarm's minute is over */
    from = mark();
    advance_to(90999);
    CHECK(count_from(from, "log I ring stopped") == 0, "it goes on past the first alarm's minute");
    from = mark();
    advance_to(91000);
    EXPECT(from, "log I ring stopped (timeout)");
    CHECK(fake_esp_timer_next_due_us() == INT64_MAX, "and nothing runs after it");
}

/* An alarm without sound doesn't ring, doesn't make a ring that goes any longer, and doesn't end it: the minute is the first alarm's,
 * 1000 to 61000 ms. */
static void an_alarm_without_sound_leaves_the_ring_alone(void) {
    CHECK(arm("aa", "loud", 1000, true, true) && arm("aa", "quiet", 10000, false, true), "arm two");
    advance_to(1000);
    CHECK(take_all(true) == 1, "told");
    int from = mark();
    advance_to(9999);
    int chirps = count_from(from, "chirp");
    CHECK(chirps == 5, "%d chirps, at 2500 to 8500", chirps);
    from = mark();
    advance_to(10000);  /* the quiet one: the screen wakes and the app is told, and it doesn't chirp (the tick of 10000 does, after it) */
    EXPECT(from, "wake alarm", "cb aa/quiet left 0", "chirp", "log I alarm aa/quiet fired (sound 0, wake 1)", "log I ring aa/loud chirp 7");
    CHECK(take_all(true) == 1, "told");
    from = mark();
    advance_to(60999);
    CHECK(count_from(from, "log I ring stopped") == 0, "the minute isn't over");
    from = mark();
    advance_to(61000);
    EXPECT(from, "log I ring stopped (timeout)");
}

/* A firing given up on (its app could not be started) ends the ring it started, as nothing would tell the user to stop it; one
 * that was told leaves it; one without sound never had one. */
static void a_firing_given_up_on_ends_its_ring(void) {
    CHECK(arm("aa", "t", 100, true, true), "arm");
    advance_to(100);
    apps_alarm_fired_t one;
    CHECK(apps_alarm_fired(&one, 1) == 1 && strcmp(one.name, "t") == 0 && one.sound, "t fired");
    int from = mark();
    CHECK(apps_alarm_take(&one, false), "given up on");
    EXPECT(from, "log I ring stopped (alarm dropped)");
    CHECK(!apps_alarm_pending() && left("aa", "t") == -1, "and gone");
    advance_to(5000);
    EXPECT(from, "log I ring stopped (alarm dropped)");  /* and no chirp since */
    /* One that was told leaves the ring going (its app has the screen, and sound.stop). */
    CHECK(arm("aa", "u", 100, true, true), "arm");
    advance_to(5100);
    CHECK(take_all(true) == 1, "told");
    from = mark();
    advance_to(6600);
    EXPECT(from, "chirp", "log I ring aa/u chirp 2");
    /* One without sound that is given up on, while a ring goes, leaves it. */
    CHECK(arm("aa", "w", 100, false, false), "arm");
    advance_to(6700);
    apps_alarm_fired_t quiet;
    CHECK(apps_alarm_fired(&quiet, 1) == 1 && strcmp(quiet.name, "w") == 0 && !quiet.sound, "w fired, without sound");
    from = mark();
    CHECK(apps_alarm_take(&quiet, false), "given up on");
    EXPECT_NONE(from);
    advance_to(8100);
    EXPECT(from, "chirp", "log I ring aa/u chirp 3");
    apps_alarm_ring_stop();
}

/* A firing that is cancelled before it is told is never told, and ends the ring it started; a stale copy of it isn't taken. */
static void cancelling_a_firing_ends_its_ring(void) {
    CHECK(arm("aa", "t", 100, true, true), "arm");
    advance(100);
    apps_alarm_fired_t f;
    CHECK(apps_alarm_fired(&f, 1) == 1, "fired");
    int from = mark();
    apps_alarm_cancel("aa", "t");
    EXPECT(from, "log I ring stopped (alarm cancelled)");
    CHECK(!apps_alarm_pending() && left("aa", "t") == -1, "dropped");
    CHECK(!apps_alarm_take(&f, true), "the copy of a cancelled firing isn't taken");
    advance(5000);
    EXPECT(from, "log I ring stopped (alarm cancelled)");  /* no chirp since */
    /* An alarm that hasn't fired, cancelled while a ring of another goes, leaves the ring alone. */
    CHECK(arm("aa", "u", 100, true, true) && arm("aa", "v", 5000, true, true), "arm two");
    advance(100);
    CHECK(take_all(true) == 1, "told");
    from = mark();
    apps_alarm_cancel("aa", "v");
    EXPECT_NONE(from);
    advance(1500);
    EXPECT(from, "chirp", "log I ring aa/u chirp 2");
    apps_alarm_ring_stop();
}

/* An app that arms the name its alarm started it for (an alarm clock that sets its next one in start()) still has that alarm
 * told: the firing waits beside the new arming, which is armed as asked, and neither takes the other's place or slot. */
static void arming_a_name_that_fired_keeps_its_firing(void) {
    CHECK(arm("aa", "t", 1000, true, true), "arm");
    advance(1000);
    apps_alarm_fired_t f;
    CHECK(apps_alarm_fired(&f, 1) == 1, "fired");
    int from = mark();
    CHECK(arm("aa", "t", 5000, true, true), "armed again before it was told");
    EXPECT_NONE(from);  /* the ring goes on: nothing was dropped */
    CHECK(left("aa", "t") == 5000, "the new arming's time: %lld", (long long)left("aa", "t"));
    LISTING_IS("aa", "t:5000");
    apps_alarm_fired_t again;
    CHECK(apps_alarm_pending() && apps_alarm_fired(&again, 1) == 1 && again.gen == f.gen && strcmp(again.name, "t") == 0,
          "the firing is still there, as it was");
    CHECK(apps_alarm_take(&f, true), "and is told");
    CHECK(!apps_alarm_pending() && left("aa", "t") == 5000, "the new arming is armed still");
    LISTING_IS("aa", "t:5000");
    from = mark();
    advance(5000);
    CHECK(count_from(from, "cb aa/t left 0") == 1 && apps_alarm_pending(), "the new alarm fires in its time");
    CHECK(take_all(true) == 1, "told");
    apps_alarm_ring_stop();
    /* The limits count slots, and a name already in one needs no other. */
    CHECK(arm("bb", "p1", 100, false, false) && arm("bb", "p2", 20000, false, false) && arm("bb", "p3", 20000, false, false) &&
              arm("bb", "p4", 20000, false, false),
          "b has four");
    advance(100);
    CHECK(arm("bb", "p1", 9000, false, false), "p1 fired and waits: armed again, in its own slot");
    CHECK(!arm("bb", "p5", 9000, false, false), "but a fifth name is refused");
    CHECK(take_all(true) == 1 && left("bb", "p1") == 9000, "p1 told, and armed");
    apps_alarm_cancel("bb", "p1");
    apps_alarm_cancel("bb", "p2");
    apps_alarm_cancel("bb", "p3");
    apps_alarm_cancel("bb", "p4");
    /* Cancelling a name drops both halves, and a ring the waiting one started. */
    CHECK(arm("cc", "c1", 1000, true, true), "arm");
    advance(1000);
    apps_alarm_fired_t g;
    CHECK(apps_alarm_fired(&g, 1) == 1, "fired");
    CHECK(arm("cc", "c1", 9000, true, true), "armed again");
    from = mark();
    apps_alarm_cancel("cc", "c1");
    EXPECT(from, "log I ring stopped (alarm cancelled)");
    CHECK(left("cc", "c1") == -1 && !apps_alarm_pending() && !apps_alarm_take(&g, true), "both halves are gone");
    advance(9000);
    EXPECT(from, "log I ring stopped (alarm cancelled)");  /* the new arming never fires */
    /* A firing that waits when the new arming fires too is told once: they are one name, with what each asked for (the first's sound, the second's wake). */
    CHECK(arm("cc", "m", 1000, true, false), "arm");
    advance(1000);
    apps_alarm_fired_t first;
    CHECK(apps_alarm_fired(&first, 1) == 1 && first.sound && !first.wake, "fired, with sound and no wake");
    CHECK(arm("cc", "m", 500, false, true), "armed again, without sound and with wake");
    from = mark();
    advance(500);
    EXPECT(from, "wake alarm", "cb cc/m left 0", "log I alarm cc/m fired (sound 0, wake 1)");
    apps_alarm_fired_t merged[2];
    CHECK(apps_alarm_fired(merged, 2) == 1, "one firing for both");
    CHECK(merged[0].gen == first.gen && merged[0].fired_ms == first.fired_ms && merged[0].sound && merged[0].wake,
          "the first's time, its sound and the second's wake");
    CHECK(take_all(true) == 1 && !apps_alarm_pending() && left("cc", "m") == -1, "told once");
    apps_alarm_ring_stop();
    /* The other way round, and the firing's own kept apart from the arming's: a firing with wake and no sound waits, and the name is armed again with
     * sound and no wake. Until the new arming fires the waiting firing is still what it was (wake, no sound); then it has the second's sound, and
     * keeps the first's wake. */
    CHECK(arm("cc", "n", 1000, false, true), "arm");
    advance(1000);
    CHECK(arm("cc", "n", 500, true, false), "armed again, with sound and without wake");
    apps_alarm_fired_t waiting;
    CHECK(apps_alarm_fired(&waiting, 1) == 1 && !waiting.sound && waiting.wake,
          "the firing is as it fired (wake, no sound), not as the arming beside it asks (sound, no wake)");
    from = mark();
    advance(500);
    EXPECT(from, "chirp", "cb cc/n left 0", "log I alarm cc/n fired (sound 1, wake 0)", "log I ring cc/n chirp 1");
    apps_alarm_fired_t both;
    CHECK(apps_alarm_fired(&both, 1) == 1 && both.gen == waiting.gen && both.sound && both.wake,
          "one firing, with the first's wake and the second's sound");
    CHECK(take_all(true) == 1 && !apps_alarm_pending(), "told once");
    apps_alarm_ring_stop();
}

/* Re-arming a name moves its time: the first time passes with nothing, the second fires once. */
static void a_replaced_alarm_fires_once_at_its_new_time(void) {
    CHECK(arm("aa", "t", 5000, true, true), "arm");
    advance(1000);
    CHECK(arm("aa", "t", 5000, true, true), "arm again, 4000 ms later than the first time");
    int from = mark();
    advance(4999);  /* 5999 ms in: the first arming's 5000 has passed */
    EXPECT_NONE(from);
    advance(1);
    EXPECT(from, FIRES_RINGING("aa/t"));
    CHECK(take_all(true) == 1, "told");
    advance(30000);
    CHECK(count_from(from, "cb aa/t") == 1, "once");
    apps_alarm_ring_stop();
}

/* A callback that was on its way when its alarm was cancelled or re-armed, or that comes early, finds nothing due and does
 * nothing: it looks at the slot, which says whether an alarm is armed and its time reached. */
static void a_stale_call_does_nothing(void) {
    int from = mark();
    CHECK(arm("aa", "t", 5000, true, true), "arm");  /* slot 0 */
    apps_alarm_cancel("aa", "t");
    fake_esp_timer_call("muse_alarm", 0);  /* the call that was on its way */
    EXPECT_NONE(from);
    CHECK(!apps_alarm_pending(), "nothing fired");
    CHECK(arm("aa", "t", 5000, true, true), "arm again");
    fake_esp_timer_call("muse_alarm", 0);  /* early */
    EXPECT_NONE(from);
    CHECK(left("aa", "t") == 5000, "still armed");
    advance(5000);
    EXPECT(from, FIRES_RINGING("aa/t"));
    CHECK(take_all(true) == 1, "told");
    apps_alarm_ring_stop();
    /* The same slot, for another alarm: the old call finds that alarm's time not reached. */
    from = mark();
    CHECK(arm("bb", "x", 9000, false, false), "bb/x takes slot 0");
    fake_esp_timer_call("muse_alarm", 0);
    EXPECT_NONE(from);
    advance(9000);
    EXPECT(from, "cb bb/x left 0", "log I alarm bb/x fired (sound 0, wake 0)");
    /* An alarm that has fired is spent: a late call is not a second firing. */
    from = mark();
    fake_esp_timer_call("muse_alarm", 0);
    fake_esp_timer_call("muse_alarm", 0);
    EXPECT_NONE(from);
    CHECK(take_all(true) == 1, "one firing, told once");
    /* The ring's tick after its end finds nothing to do. */
    CHECK(arm("aa", "r", 100, true, true), "arm");
    advance(100);
    CHECK(take_all(true) == 1, "told");
    apps_alarm_ring_stop();
    from = mark();
    fake_esp_timer_call("muse_ring", 0);
    EXPECT_NONE(from);
    CHECK(fake_esp_timer_next_due_us() == INT64_MAX, "no timer is left running");
}

/* The esp_timer task never waits for the table for long: a try that fails is made again 10 ms later, for an alarm and for a tick
 * of the ring (whose chirp is lost, and the next comes 1.5 s on). An alarm cancelled while its retry waits is not fired by it. */
static void a_busy_table_is_tried_again(void) {
    CHECK(arm("aa", "t", 5000, true, true), "arm");
    int from = mark();
    atomic_store(&busy_takes, 1);
    advance_to(5000);
    EXPECT_NONE(from);  /* the table was busy: nothing fired yet */
    CHECK(!apps_alarm_pending(), "not marked");
    advance_to(5009);
    EXPECT_NONE(from);
    advance_to(5010);  /* 10 ms later */
    EXPECT(from, FIRES_RINGING("aa/t"));
    CHECK(apps_alarm_pending(), "marked now");
    CHECK(take_all(true) == 1, "told");
    /* Three tries that fail: 30 ms. */
    CHECK(arm("aa", "u", 1000, false, false), "arm");  /* due at 6010 */
    from = mark();
    atomic_store(&busy_takes, 3);
    advance_to(6039);
    EXPECT_NONE(from);
    advance_to(6040);
    EXPECT(from, "cb aa/u left 0", "log I alarm aa/u fired (sound 0, wake 0)");
    CHECK(take_all(true) == 1, "told");
    /* A tick of the ring that finds the table busy is lost; the next is as it was (the ring began at 5010: ticks at 6510, 8010). */
    from = mark();
    atomic_store(&busy_takes, 1);
    advance_to(6509);
    EXPECT_NONE(from);
    advance_to(6510);
    EXPECT_NONE(from);
    advance_to(8010);
    EXPECT(from, "chirp", "log I ring aa/t chirp 2");
    apps_alarm_ring_stop();
    /* Cancelled while its retry waits: the retry finds nothing armed. */
    CHECK(arm("aa", "v", 1000, true, true), "arm");  /* due at 9010 */
    from = mark();
    atomic_store(&busy_takes, 1);
    advance_to(9010);
    apps_alarm_cancel("aa", "v");
    advance_to(9100);
    EXPECT_NONE(from);
    /* What fired first is told first, not what was due first: "late" is due at 9200 and finds the table busy (it fires at 9210), "early"
     * is due at 9205 and fires then. */
    CHECK(arm("aa", "late", 100, false, false) && arm("aa", "early", 105, false, false), "arm two");
    from = mark();
    atomic_store(&busy_takes, 1);
    advance_to(9300);
    EXPECT(from, "cb aa/early left 0", "log I alarm aa/early fired (sound 0, wake 0)", "cb aa/late left 0",
           "log I alarm aa/late fired (sound 0, wake 0)");
    apps_alarm_fired_t due[2];
    CHECK(apps_alarm_fired(due, 2) == 2 && strcmp(due[0].name, "early") == 0 && strcmp(due[1].name, "late") == 0 &&
              due[0].fired_ms == START_MS + 9205 && due[1].fired_ms == START_MS + 9210,
          "told in the order they fired: %s, %s", due[0].name, due[1].name);
    CHECK(take_all(true) == 2, "told");
}

/* A retry that started the alarm's timer between an arming's stop and its start (it could not take the table to be told otherwise)
 * does not lose the arming: it is the arming's timer, restarted for its time, and the early retry does not fire it. */
static void a_retry_started_behind_an_arming_does_not_lose_it(void) {
    CHECK(arm("aa", "t", 5000, true, true), "arm");
    int from = mark();
    fake_esp_timer_start_behind_next_stop();
    CHECK(arm("aa", "t", 9000, true, true), "armed again: not refused for a timer that was started behind it");
    CHECK(left("aa", "t") == 9000, "its time: %lld", (long long)left("aa", "t"));
    advance(8999);
    EXPECT_NONE(from);  /* not at the stray retry's 10 ms, nor at 5000 */
    advance(1);
    EXPECT(from, FIRES_RINGING("aa/t"));
    apps_alarm_ring_stop();
}

/* Firings are told in the order they came due, whatever order they were armed in; of two due together, the one armed first, and not the
 * one in the lower slot: "junk" takes a slot and gives it back, so that "q", armed after "p", lies in a lower slot than it. */
static void firings_are_told_in_the_order_they_came_due(void) {
    CHECK(arm("bb", "x", 300, false, false) && arm("aa", "y", 100, false, false) && arm("aa", "z", 200, false, false), "arm three");
    CHECK(arm("cc", "junk", 9000, false, false), "a throwaway takes the next slot");
    CHECK(arm("cc", "p", 500, false, false), "p, armed first, is in the slot after it");
    apps_alarm_cancel("cc", "junk");
    CHECK(arm("cc", "q", 500, false, false), "q, armed after p, takes the slot the throwaway left, below p's");
    LISTING_IS("aa", "y:100 z:200");
    LISTING_IS("cc", "p:500 q:500");  /* the listing breaks the tie by arming order too */
    int from = mark();
    advance(1000);
    EXPECT(from, "cb aa/y left 0", "log I alarm aa/y fired (sound 0, wake 0)", "cb aa/z left 0", "log I alarm aa/z fired (sound 0, wake 0)",
           "cb bb/x left 0", "log I alarm bb/x fired (sound 0, wake 0)", "cb cc/p left 0", "cb cc/q left 0",
           "log I alarm cc/p fired (sound 0, wake 0)", "log I alarm cc/q fired (sound 0, wake 0)");
    apps_alarm_fired_t due[APPS_ALARMS_MAX];
    int n = apps_alarm_fired(due, APPS_ALARMS_MAX);
    static const char *const order[] = {"y", "z", "x", "p", "q"};
    CHECK(n == 5, "%d firings", n);
    for (int i = 0; i < n; i++) {
        CHECK(strcmp(due[i].name, order[i]) == 0, "firing %d is %s, not %s", i, due[i].name, order[i]);
    }
    CHECK(due[3].slot > due[4].slot, "p (slot %d) is above q (slot %d): the order is not the slots'", due[3].slot, due[4].slot);
    apps_alarm_fired_t two[2];
    CHECK(apps_alarm_fired(two, 2) == 2 && strcmp(two[0].name, "y") == 0 && strcmp(two[1].name, "z") == 0, "the first two");
    CHECK(apps_alarm_take(&due[2], true), "x told");
    CHECK(apps_alarm_pending(), "the others wait");
    CHECK(take_all(true) == 4 && !apps_alarm_pending(), "and are told");
}

/* A copy of a firing is not taken once its slot has another: cancelled and armed again, the old copy's gen is not the new one's. */
static void a_stale_copy_is_not_taken_twice(void) {
    CHECK(arm("aa", "t", 100, false, false), "arm");
    advance(100);
    apps_alarm_fired_t old;
    CHECK(apps_alarm_fired(&old, 1) == 1, "fired");
    apps_alarm_cancel("aa", "t");
    CHECK(arm("aa", "t", 100, false, false), "arm again");
    advance(100);
    apps_alarm_fired_t fresh;
    CHECK(apps_alarm_fired(&fresh, 1) == 1 && fresh.slot == old.slot && fresh.gen != old.gen, "the same slot, another firing");
    CHECK(!apps_alarm_take(&old, true), "the old copy is not taken");
    CHECK(apps_alarm_pending(), "the new firing waits");
    CHECK(apps_alarm_take(&fresh, true) && !apps_alarm_pending(), "and is told");
    apps_alarm_fired_t none = {.slot = APPS_ALARMS_MAX};
    CHECK(!apps_alarm_take(&none, true) && !apps_alarm_take(NULL, true), "a copy that names no slot");
}

/* The pending word is set while any firing waits, whichever is told first. */
static void the_pending_word_follows_the_firings(void) {
    CHECK(!apps_alarm_pending(), "none");
    CHECK(arm("aa", "p", 100, false, false) && arm("aa", "q", 100, false, false), "arm two");
    CHECK(!apps_alarm_pending(), "not yet");
    advance(100);
    apps_alarm_fired_t due[2];
    CHECK(apps_alarm_fired(due, 2) == 2 && apps_alarm_pending(), "two wait");
    CHECK(apps_alarm_take(&due[1], true) && apps_alarm_pending(), "one told, one waits");
    CHECK(apps_alarm_take(&due[0], true) && !apps_alarm_pending(), "both told");
}

/* ---- The ring's rule (B): never silenced while something could still be stopped by it, or told of it ---- */

/* Two firings with sound wait, and one is dropped (its app can't be started, or it is cancelled): the ring goes on for the other,
 * which still has to be told: it stops when that one is dropped as well, and says why. */
static void a_dropped_firing_leaves_the_ring_to_the_one_still_waiting(void) {
    CHECK(arm("aa", "t", 100, true, true) && arm("bb", "u", 200, true, true), "arm two");
    advance_to(100);
    int from = mark();
    advance_to(200);  /* bb/u fires into the ring that goes: the minute is made longer, with no chirp and no line of its own */
    EXPECT(from, "wake alarm", "cb bb/u left 0", "log I alarm bb/u fired (sound 1, wake 1)");
    apps_alarm_fired_t due[2];
    CHECK(apps_alarm_fired(due, 2) == 2 && strcmp(due[0].app, "aa") == 0 && strcmp(due[1].app, "bb") == 0, "two firings wait");
    from = mark();
    CHECK(apps_alarm_take(&due[0], false), "aa's is given up on");
    EXPECT_NONE(from);  /* bb's still waits to be told: the ring goes on */
    CHECK(apps_alarm_ringing(), "it rings");
    advance_to(1600);
    EXPECT(from, "chirp", "log I ring bb/u chirp 2");  /* naming the alarm that rang last */
    from = mark();
    CHECK(apps_alarm_take(&due[1], false), "bb's too");
    EXPECT(from, "log I ring stopped (alarm dropped)");  /* now nothing waits and no app was told */
    CHECK(!apps_alarm_ringing(), "it doesn't");
    advance_to(20000);
    EXPECT(from, "log I ring stopped (alarm dropped)");  /* and no chirp since */
    /* The same when the firing is cancelled and not given up on. */
    CHECK(arm("aa", "v", 100, true, true) && arm("bb", "w", 200, true, true), "arm two more");
    advance_to(20200);
    from = mark();
    apps_alarm_cancel("aa", "v");
    EXPECT_NONE(from);
    CHECK(apps_alarm_ringing(), "bb/w's firing waits: the ring goes on");
    apps_alarm_cancel("bb", "w");
    EXPECT(from, "log I ring stopped (alarm cancelled)");
}

/* The app that was told a ring crashed (in alarm(), or after) while another app's firing with sound waits: the ring goes on for that
 * one, and ends with the app that is told it. */
static void a_crash_while_another_firing_waits_leaves_the_ring_going(void) {
    CHECK(arm("aa", "t", 100, true, true) && arm("bb", "u", 200, true, true), "arm two");
    advance_to(200);
    apps_alarm_fired_t due[2];
    CHECK(apps_alarm_fired(due, 2) == 2, "both fired");
    CHECK(apps_alarm_take(&due[0], true), "aa is told");
    int from = mark();
    apps_alarm_app_ended("aa", "app crashed");  /* it crashed in alarm(): bb's firing still waits, so the ring stays */
    EXPECT_NONE(from);
    CHECK(apps_alarm_ringing(), "it rings");
    advance_to(1600);
    EXPECT(from, "chirp", "log I ring bb/u chirp 2");
    from = mark();
    CHECK(apps_alarm_take(&due[1], true), "bb is told");
    apps_alarm_app_ended("bb", "app crashed");  /* and now there is nothing left that could stop it */
    EXPECT(from, "log I ring stopped (app crashed)");
    CHECK(!apps_alarm_ringing(), "it doesn't");
}

/* The app that was last told an alarm with sound ends, with nothing waiting: the ring ends, with how the app ended. Another app's end,
 * an app that was told an alarm without sound, and an end when nothing rings, are nothing to it. */
static void the_told_app_ending_with_nothing_waiting_stops_the_ring(void) {
    static const char *const reasons[] = {"app crashed", "app stopped", "app replaced", "app exited", "app hung"};
    for (size_t i = 0; i < ARRAY_LEN(reasons); i++) {
        CHECK(arm("aa", "t", 100, true, true), "arm");
        advance(100);
        CHECK(take_all(true) == 1, "told");
        int from = mark();
        apps_alarm_app_ended("zz", reasons[i]);  /* another app's end is nothing to this ring */
        EXPECT_NONE(from);
        apps_alarm_app_ended("aa", reasons[i]);
        char line[60];
        snprintf(line, sizeof line, "log I ring stopped (%s)", reasons[i]);
        EXPECT(from, line);
        advance(5000);
        EXPECT(from, line);  /* once, and no chirp after it */
        apps_alarm_app_ended("aa", reasons[i]);
        EXPECT(from, line);
    }
    /* An alarm without sound that is told doesn't make its app the one that could stop the ring. (Other apps for each: an app's own
     * budget is six firings in a minute.) */
    CHECK(arm("bb", "loud", 100, true, true) && arm("cc", "quiet", 200, false, true), "arm two");
    advance(100);
    CHECK(take_all(true) == 1, "bb told");
    advance(100);
    CHECK(take_all(true) == 1, "cc told, of an alarm without sound");
    int from = mark();
    apps_alarm_app_ended("cc", "app stopped");
    EXPECT_NONE(from);
    CHECK(apps_alarm_ringing(), "cc could not stop it, so its end doesn't");
    apps_alarm_app_ended("bb", "app stopped");
    EXPECT(from, "log I ring stopped (app stopped)");
    /* A ring that sound.stop() ended has no one left to wait for: a later end says nothing. */
    CHECK(arm("dd", "t", 100, true, true), "arm");
    advance(100);
    CHECK(take_all(true) == 1, "told");
    apps_alarm_ring_stop();
    from = mark();
    apps_alarm_app_ended("dd", "app stopped");
    EXPECT_NONE(from);
    /* A firing told after the ring ended is not the one that stops anything. */
    CHECK(arm("ee", "t", 100, true, false), "arm");
    advance(100);
    apps_alarm_fired_t f;
    CHECK(apps_alarm_fired(&f, 1) == 1, "fired");
    apps_alarm_ring_stop();
    CHECK(apps_alarm_take(&f, true), "told");
    from = mark();
    apps_alarm_app_ended("ee", "app stopped");
    EXPECT_NONE(from);
    advance(5000);
    CHECK(!apps_alarm_ringing(), "and the ring is over");
}

/* The id-cancel: every alarm of one app goes, armings and firings, with a line that says why; the rest of the table is as it was; the ring
 * ends if that leaves nothing to stop it, and stays while the app that was told it runs. An app with none says nothing. */
static void cancelling_an_apps_alarms_by_its_id(void) {
    CHECK(arm("aa", "t", 100, true, true) && arm("aa", "u", 5000, true, true) && arm("aa", "v", 6000, false, false) &&
              arm("bb", "t", 5000, true, true),
          "arm four");
    advance_to(100);
    apps_alarm_fired_t f;
    CHECK(apps_alarm_fired(&f, 1) == 1, "aa/t fired");
    int from = mark();
    apps_alarm_cancel_app("aa", "crashed when its alarm launched it");
    EXPECT(from, "log W alarm: aa crashed when its alarm launched it; its alarms are cancelled", "log I ring stopped (alarms cancelled)");
    LISTING_IS("aa", "");
    LISTING_IS("bb", "t:4900");
    CHECK(!apps_alarm_pending() && !apps_alarm_take(&f, true), "the firing went with them");
    from = mark();
    apps_alarm_cancel_app("aa", "again");
    apps_alarm_cancel_app("zz", "nobody's");
    apps_alarm_cancel_app(NULL, "none");
    apps_alarm_cancel_app("aa", NULL);
    EXPECT_NONE(from);  /* nothing to cancel, nothing to say */
    advance_to(5000);
    EXPECT(from, FIRES_RINGING("bb/t"));  /* the others' go on: only bb's fires */
    CHECK(take_all(true) == 1, "bb told");
    /* The app that was told the ring is running, and can stop it: its alarms go, and the ring stays until the app ends. */
    CHECK(arm("bb", "x", 9000, false, false), "arm");
    from = mark();
    apps_alarm_cancel_app("bb", "fired more than 6 alarms in a minute");
    EXPECT(from, "log W alarm: bb fired more than 6 alarms in a minute; its alarms are cancelled");
    CHECK(apps_alarm_ringing(), "bb runs and was told the ring");
    apps_alarm_app_ended("bb", "app stopped");
    EXPECT(from, "log W alarm: bb fired more than 6 alarms in a minute; its alarms are cancelled", "log I ring stopped (app stopped)");
}

/* ---- The budget (A): six firings of an app in a minute, and its alarms are gone with the seventh ---- */

/* Exactly the bound: the 7th firing of an app less than a minute after the 1st cancels all its alarms; at a minute it doesn't. Another app has
 * its own count. */
static void the_budget_is_six_firings_in_less_than_a_minute(void) {
    for (int k = 1; k <= 6; k++) {  /* both apps fire at 1, 2, ... 6 ms */
        CHECK(arm("aa", "t", 1, false, false) && arm("bb", "t", 1, false, false), "arm %d", k);
        advance(1);
        CHECK(take_all(true) == 2, "told");
    }
    CHECK(count_from(0, "cb ") == 12, "six firings of each: %d", count_from(0, "cb "));
    advance_to(59998);
    CHECK(arm("aa", "t", 1, false, false) && arm("aa", "keep", 5000, false, false), "aa's 7th, due at 59999: 59998 ms after its 1st");
    int from = mark();
    advance_to(59999);
    EXPECT(from, "log W alarm: aa fired more than 6 alarms in a minute; its alarms are cancelled");  /* not told, and "keep" went too */
    CHECK(!apps_alarm_pending(), "nothing waits");
    LISTING_IS("aa", "");
    advance_to(60000);
    /* Both at once, due at 60001: bb's 7th, 60000 ms after its 1st, which fits a whole minute; and aa's firing after the one that was cut off. The
     * cut-off one was not counted, so aa's window is still the six of 1 to 6 ms, and this is 60000 ms after the 1st of them: it fits too (counted in
     * the window, the cut-off firing at 59999 would have pushed the 1st out, and this one, 59999 ms after the 2nd, would have been the 7th). */
    CHECK(arm("aa", "t", 1, false, false) && arm("bb", "t", 1, false, false), "aa arms again, and bb's 7th, due at 60001");
    from = mark();
    advance_to(60001);
    EXPECT(from, "cb aa/t left 0", "cb bb/t left 0", "log I alarm aa/t fired (sound 0, wake 0)", "log I alarm bb/t fired (sound 0, wake 0)");
    CHECK(take_all(true) == 2, "both told");
    /* The window slides: the next is the 8th, 60001 ms after the 2nd of the last six (which fired at 2 ms): it fits too, for each. */
    from = mark();
    advance_to(60002);
    CHECK(arm("aa", "t", 1, false, false) && arm("bb", "t", 1, false, false), "the 8th of each");
    advance_to(60003);
    EXPECT(from, "cb aa/t left 0", "cb bb/t left 0", "log I alarm aa/t fired (sound 0, wake 0)", "log I alarm bb/t fired (sound 0, wake 0)");
    CHECK(take_all(true) == 2, "both told");
    /* A minute on, it has its six again. */
    advance_to(120000);
    for (int k = 1; k <= 6; k++) {
        CHECK(arm("aa", "t", 1, false, false), "arm %d", k);
        from = mark();
        advance(1);
        EXPECT(from, "cb aa/t left 0", "log I alarm aa/t fired (sound 0, wake 0)");
        CHECK(take_all(true) == 1, "told");
    }
}

/* The reviewers' app: it arms four alarms of 1 ms every 40 ms for ten seconds of the clock, and the app task tells each at once. It gets six
 * firings, and the rest of its alarms are cancelled as they come due; what it makes of the console and the voice task is bounded: the lines
 * of the bucket, and one chirp for the ring's period. */
static void an_app_that_arms_alarms_all_the_time_is_cut_off_after_six(void) {
    static const char *const names[] = {"a1", "a2", "a3", "a4"};
    int from = mark();
    for (int round = 0; round < 250; round++) {
        for (int k = 0; k < 4; k++) {
            (void)apps_alarm_arm("aa", names[k], 1, true, true);
        }
        advance(40);
        (void)take_all(true);
    }
    CHECK(count_from(from, "cb ") == 6, "%d firings reached the app task", count_from(from, "cb "));
    CHECK(count_from(from, "log I alarm aa/") == 6, "six firings, six lines (the bucket holds eight)");
    int cancels = count_from(from, "log W alarm: aa fired more than 6 alarms in a minute; its alarms are cancelled");
    CHECK(cancels >= 1 && cancels <= 8 + 4 * 10, "%d cancel lines in 10 s, at most the bucket's 8 and four a second", cancels);
    CHECK(count_from(from, "log ") <= 6 + 1 + (8 + 4 * 10) * 2 + 8, "%d log lines in all", count_from(from, "log "));
    int chirps = count_from(from, "chirp");
    CHECK(chirps >= 2 && chirps <= 1 + 10000 / APPS_RING_PERIOD_MS, "%d chirps in 10 s: one for the ring's start and one for each of its periods", chirps);
    CHECK(count_from(from, "wake alarm") == 6, "the screen was woken six times, not at every arming: %d", count_from(from, "wake alarm"));
    advance(10);
    CHECK(!apps_alarm_pending(), "nothing waits");
    LISTING_IS("aa", "");
    CHECK(apps_alarm_ringing(), "the app was told the ring and runs: it goes on until it ends");
    apps_alarm_app_ended("aa", "app stopped");
    CHECK(!apps_alarm_ringing(), "and the stop of the app ends it");
}

/* A flood: eight apps fire six alarms each within a few milliseconds. Every firing reaches the app task, the lines are the bucket's (eight, and
 * no more however many come), the count of those left out is printed once the lines have been quiet for a second, and there is one chirp. */
static void a_flood_of_firings_is_logged_at_the_rate_of_the_bucket(void) {
    static const char *const apps[] = {"aa", "bb", "cc", "dd", "ee", "ff", "gg", "hh"};
    int from = mark();
    for (int round = 0; round < 6; round++) {
        for (size_t k = 0; k < ARRAY_LEN(apps); k++) {
            CHECK(arm(apps[k], "t", 1, true, true), "arm %s", apps[k]);
        }
        advance(1);
        CHECK(take_all(true) == 8, "eight told");
    }
    CHECK(count_from(from, "cb ") == 48, "all 48 firings reached the app task: %d", count_from(from, "cb "));
    CHECK(count_from(from, "chirp") == 1, "one request for the whole flood: %d", count_from(from, "chirp"));
    CHECK(count_from(from, "log I alarm ") == APPS_ALARM_LOG_BURST, "%d firing lines, the bucket's %d", count_from(from, "log I alarm "),
          APPS_ALARM_LOG_BURST);
    CHECK(count_from(from, "log I ring ") == 1, "and the ring's first chirp");
    CHECK(count_from(from, "log I (") == 0, "the count of the lines left out waits for the lines to be quiet");
    advance(900);
    CHECK(count_from(from, "log I (") == 0, "it is not a second yet");
    advance(700);  /* 1.6 s after the last of them: the ring's tick comes, and the lines have been quiet for a second */
    CHECK(count_from(from, "log I (40 alarm lines not logged)") == 1, "the 40 left out are counted, once");
    advance(60000);
    CHECK(count_from(from, "log I (") == 1, "once");
    /* After a quiet second the bucket is full again: a firing is logged. */
    from = mark();
    CHECK(arm("aa", "t", 1, false, false), "arm");
    advance(1);
    EXPECT(from, "cb aa/t left 0", "log I alarm aa/t fired (sound 0, wake 0)");
}

/* The esp_timer task asks for a chirp once for a ring and once for each period of it, whatever the firings do. */
static void firings_in_a_ring_ask_for_no_chirp_of_their_own(void) {
    CHECK(arm("aa", "t", 100, true, true), "arm");
    advance_to(100);
    CHECK(take_all(true) == 1, "told");
    int from = mark();
    for (int k = 0; k < 4; k++) {
        CHECK(arm("aa", "again", 1, true, true), "arm");
        advance(1);
        CHECK(take_all(true) == 1, "told");
    }
    CHECK(count_from(from, "chirp") == 0, "four firings into a ring that goes: no chirp");
    CHECK(count_from(from, "log I ring ") == 0, "and no line for one");
    from = mark();
    advance_to(1600);
    EXPECT(from, "chirp", "log I ring aa/again chirp 2");  /* the period's: one */
}

/* ---- Memory: the lock, the ring's timer and each alarm's timer can fail to be made, and the next try works (G, H) ---- */

static void no_memory_for_a_timer_is_told_and_tried_again(void) {
    fake_esp_timer_set_limit(0);  /* the ring's timer, made on the first use, can't be made */
    int from = mark();
    CHECK(apps_alarm_arm("aa", "t", 1000, true, true) == APPS_ALARM_NO_MEMORY, "no memory for the ring's timer");
    EXPECT(from, "log E alarms: no memory for the ring's timer: the next alarm.after tries again");
    CHECK(left("aa", "t") == -1 && !apps_alarm_pending() && !apps_alarm_ringing(), "nothing is armed");
    from = mark();
    /* Everything else is harmless with that: asking, listing, cancelling, ending, draining, taking. */
    LISTING_IS("aa", "");
    apps_alarm_cancel("aa", "t");
    apps_alarm_cancel_app("aa", "x");
    apps_alarm_ring_stop();
    apps_alarm_app_ended("aa", "app stopped");
    apps_alarm_log_drain();
    apps_alarm_fired_t f;
    CHECK(apps_alarm_fired(&f, 1) == 0 && !apps_alarm_take(&f, true) && !apps_alarm_wanted("aa", 1), "nothing to take");
    EXPECT_NONE(from);
    advance(5000);
    EXPECT_NONE(from);
    /* One timer is allowed: the ring's is made, and the alarm's own can't be: the slot stays free. */
    fake_esp_timer_set_limit(1);
    CHECK(apps_alarm_arm("aa", "t", 1000, true, true) == APPS_ALARM_NO_MEMORY, "no memory for the alarm's timer");
    CHECK(left("aa", "t") == -1 && !apps_alarm_pending(), "the slot is as it was");
    LISTING_IS("aa", "");
    advance(5000);
    EXPECT_NONE(from);  /* nothing fired */
    /* Two: it works, and the alarm fires. */
    fake_esp_timer_set_limit(2);
    CHECK(apps_alarm_arm("aa", "t", 1000, true, true) == APPS_ALARM_ARMED, "armed once there is memory");
    CHECK(left("aa", "t") == 1000, "its time");
    /* A second slot needs a timer of its own: refused for memory, and the first alarm is as it was. */
    CHECK(apps_alarm_arm("bb", "u", 500, false, false) == APPS_ALARM_NO_MEMORY, "no memory for another");
    CHECK(left("bb", "u") == -1 && left("aa", "t") == 1000, "the table is as it was");
    from = mark();
    advance(1000);
    EXPECT(from, FIRES_RINGING("aa/t"));
    CHECK(take_all(true) == 1, "told");
    /* The slot that has its timer is armed again, and refused arguments are refusals, not a want of memory. */
    CHECK(apps_alarm_arm("aa", "t", 1000, true, true) == APPS_ALARM_ARMED, "the slot of the alarm that fired is its timer's again");
    CHECK(apps_alarm_arm("aa", "BAD", 1000, true, true) == APPS_ALARM_REFUSED, "a name that isn't one");
    fake_esp_timer_set_limit(32);
    CHECK(apps_alarm_arm("bb", "u", 500, false, false) == APPS_ALARM_ARMED, "memory again: bb arms");
    apps_alarm_ring_stop();
}

/* ---- Ids (S) ---- */

/* An id is an app's id as apps_id_valid says (apps_pkg.c: ^[a-z][a-z0-9_]{1,23}$): a table that is told to hold one that isn't, from
 * stored data later, is not made to print a console line of its own. */
static void ids_follow_the_apps_rule(void) {
    static const char *const valid[] = {"ab", "timer", "a1", "a_", "a_b_9", "z9_", "aaaaaaaaaaaaaaaaaaaaaaaa"};
    static const char *const invalid[] = {"",     "a",    "aaaaaaaaaaaaaaaaaaaaaaaaa", "Timer", "1a",   "_a",   "a-b",  "a.b",
                                          "a b",  "ab ",  " ab",                       "aB",    "a/b",  "a\tb", "\xc3\xa9\x61",
                                          "t\xc3\xaf\x6d\x65r", "ab\n@app {\"op\":1}"};
    for (size_t i = 0; i < ARRAY_LEN(valid); i++) {
        CHECK(arm(valid[i], "t", 1000, false, false), "'%s' is an id", valid[i]);
        CHECK(left(valid[i], "t") == 1000, "armed");
        apps_alarm_cancel(valid[i], "t");
        CHECK(left(valid[i], "t") == -1, "cancelled");
    }
    for (size_t i = 0; i < ARRAY_LEN(invalid); i++) {
        CHECK(!arm(invalid[i], "t", 1000, false, false), "'%s' is not an id", invalid[i]);
        CHECK(left(invalid[i], "t") == -1, "and nothing is armed for it");
        char names[4][24];
        int64_t remaining[4];
        CHECK(apps_alarm_list(invalid[i], names, remaining) == 0, "or listed");
        CHECK(!apps_alarm_wanted(invalid[i], 1), "or wanted");
    }
    CHECK(!arm(NULL, "t", 1000, false, false), "no id");
}

/* ---- A launch for a firing (apps_alarm_wanted) ---- */

static void a_launch_is_wanted_while_a_firing_it_was_asked_for_waits(void) {
    CHECK(arm("aa", "t", 100, false, false) && arm("aa", "u", 300, false, false), "arm two");
    advance_to(100);
    apps_alarm_fired_t f1;
    CHECK(apps_alarm_fired(&f1, 1) == 1 && strcmp(f1.name, "t") == 0, "t fired");
    CHECK(apps_alarm_wanted("aa", f1.gen), "the launch asked for it is wanted");
    CHECK(!apps_alarm_wanted("bb", f1.gen) && !apps_alarm_wanted("zz", f1.gen) && !apps_alarm_wanted(NULL, f1.gen), "not another app's");
    CHECK(!apps_alarm_wanted("aa", 0), "a launch with no identity wants nothing");
    CHECK(!apps_alarm_wanted("aa", f1.gen - 1), "nor one asked for firings before it");
    advance_to(300);
    apps_alarm_fired_t two[2];
    CHECK(apps_alarm_fired(two, 2) == 2 && two[1].gen != f1.gen, "u fired too");
    CHECK((int32_t)(two[1].gen - f1.gen) > 0, "later firings have later gens");
    CHECK(apps_alarm_wanted("aa", two[1].gen) && apps_alarm_wanted("aa", f1.gen), "both wait");
    CHECK(apps_alarm_take(&two[0], true), "t is told");
    CHECK(!apps_alarm_wanted("aa", f1.gen), "nothing at or below t's gen waits now (u's is above)");
    CHECK(apps_alarm_wanted("aa", two[1].gen), "the launch asked for both still has u to tell");
    CHECK(apps_alarm_take(&two[1], false), "u is given up on");
    CHECK(!apps_alarm_wanted("aa", two[1].gen), "nothing waits");
    /* A firing cancelled while its launch is queued: the launch is not wanted. */
    CHECK(arm("aa", "v", 100, false, false), "arm");
    advance(100);
    apps_alarm_fired_t f3;
    CHECK(apps_alarm_fired(&f3, 1) == 1 && apps_alarm_wanted("aa", f3.gen), "v fired and is wanted");
    apps_alarm_cancel("aa", "v");
    CHECK(!apps_alarm_wanted("aa", f3.gen), "cancelled: the launch has nothing to run for");
}

/* ---- Three tasks and the esp_timer task at once ---- */

static atomic_bool go, stop_workers;
static atomic_ullong ops, takes_ok;
static atomic_uint cb_calls;

#define STRESS_MAX_MS 60

/* Every successful take of the workers: a firing is taken once, whoever asks (I). A repeat is a firing told twice. */
static pthread_mutex_t taken_mu = PTHREAD_MUTEX_INITIALIZER;
static struct {
    uint8_t slot;
    uint32_t gen;
} taken[16384];
static int n_taken;

static void note_taken(const apps_alarm_fired_t *f) {
    pthread_mutex_lock(&taken_mu);
    for (int i = 0; i < n_taken; i++) {
        if (taken[i].slot == f->slot && taken[i].gen == f->gen) {
            pthread_mutex_unlock(&taken_mu);
            error("the firing %u of slot %d was taken twice (%s/%s)", (unsigned)f->gen, f->slot, f->app, f->name);
            return;
        }
    }
    if (n_taken < (int)ARRAY_LEN(taken)) {
        taken[n_taken].slot = f->slot;
        taken[n_taken].gen = f->gen;
        n_taken++;
    }
    pthread_mutex_unlock(&taken_mu);
}

/* What an app thread does: arms (ms to STRESS_MAX_MS), cancels, asks, lists, and takes what has fired as the app task does, at random, and
 * checks what it is given: a name and an app that are a name and an app, a count within the limits, a time in range, firings in
 * order. */
static void *worker(void *arg) {
    int id = (int)(intptr_t)arg;
    char app[8];
    snprintf(app, sizeof app, "app%d", id);
    unsigned seed = 1234u + (unsigned)id * 77u;
    while (!atomic_load(&go)) {
        usleep(50);
    }
    while (!atomic_load(&stop_workers)) {
        char name[8];
        snprintf(name, sizeof name, "n%u", rand_r(&seed) % 6);
        switch (rand_r(&seed) % 9) {
        case 0:
        case 1:
        case 2:
            (void)apps_alarm_arm(app, name, 1 + (int64_t)(rand_r(&seed) % STRESS_MAX_MS), (rand_r(&seed) & 1) != 0,
                                 (rand_r(&seed) & 1) != 0);
            break;
        case 3:
            if ((rand_r(&seed) & 3) == 0) {
                apps_alarm_cancel(app, name);
            }
            break;
        case 4: {
            int64_t r = apps_alarm_remaining(app, name);
            if (r < -1 || r > STRESS_MAX_MS) {
                error("remaining %lld", (long long)r);
            }
            break;
        }
        case 5: {
            char names[4][24];
            int64_t remaining[4];
            int n = apps_alarm_list(app, names, remaining);
            if (n < 0 || n > 4) {
                error("%d alarms listed", n);
            }
            for (int i = 0; i < n; i++) {
                if (remaining[i] < 0 || remaining[i] > STRESS_MAX_MS || strncmp(names[i], "n", 1) != 0 || (i > 0 && strcmp(names[i], names[i - 1]) == 0)) {
                    error("listed %s:%lld", names[i], (long long)remaining[i]);
                }
            }
            break;
        }
        case 6:
            if ((rand_r(&seed) & 15) == 0) {
                apps_alarm_cancel_app(app, "stress");
            } else if ((rand_r(&seed) & 15) == 0) {
                apps_alarm_app_ended(app, "app stopped");
            } else {
                apps_alarm_ring_stop();
            }
            break;
        default: {
            apps_alarm_fired_t due[APPS_ALARMS_MAX];
            int n = apps_alarm_fired(due, APPS_ALARMS_MAX);
            int64_t now_ms = atomic_load(&now_us) / 1000;
            for (int i = 0; i < n; i++) {
                if (strncmp(due[i].app, "app", 3) != 0 || due[i].name[0] != 'n' || due[i].fired_ms > now_ms ||
                    (i > 0 && due[i].fired_ms < due[i - 1].fired_ms)) {
                    error("a firing %s/%s at %lld (now %lld) after %lld", due[i].app, due[i].name, (long long)due[i].fired_ms,
                          (long long)now_ms, (long long)(i > 0 ? due[i - 1].fired_ms : 0));
                }
                if ((rand_r(&seed) & 1) != 0 && apps_alarm_take(&due[i], (rand_r(&seed) % 3) != 0)) {
                    atomic_fetch_add(&takes_ok, 1);
                    note_taken(&due[i]);
                }
            }
            break;
        }
        }
        atomic_fetch_add(&ops, 1);
        usleep(20 + rand_r(&seed) % 100);  /* so that an alarm lives long enough to fire before its name is armed again */
    }
    return NULL;
}

static void concurrent_cb(const char *app, const char *name) {
    atomic_fetch_add(&cb_calls, 1);
    in_callback = true;
    (void)apps_alarm_remaining(app, name);  /* the table is free to ask: this is outside its lock */
    in_callback = false;
}

/* ---- The records' side (Task 20): what apps_values reads of an app's alarms, and the apps whose alarms were all cancelled ---- */

/* apps_alarm_list_info, which an alarm's record is made from (its time is the clock's now plus remaining_ms): an app's alarms earliest
 * first, each with the time it has left and the sound and wake it asked for; a firing that waits to be told is there too, with nothing
 * left and the sound and wake it fired with. Another app's aren't, and an id that isn't one has none. */
static void list_info_gives_each_alarm_its_time_and_options(void) {
    CHECK(arm("aa", "t", 5000, true, false) && arm("aa", "u", 1000, false, true) && arm("bb", "x", 3000, true, true), "arm three");
    apps_alarm_info_t info[APPS_ALARMS_PER_APP];
    memset(info, 0, sizeof info);
    int n = apps_alarm_list_info("aa", info);
    CHECK(n == 2 && strcmp(info[0].name, "u") == 0 && info[0].remaining_ms == 1000 && !info[0].sound && info[0].wake &&
              strcmp(info[1].name, "t") == 0 && info[1].remaining_ms == 5000 && info[1].sound && !info[1].wake,
          "armed: %d, %s %lld %d %d, %s %lld %d %d", n, info[0].name, (long long)info[0].remaining_ms, info[0].sound, info[0].wake,
          info[1].name, (long long)info[1].remaining_ms, info[1].sound, info[1].wake);
    advance(1000);  /* u fires, and waits to be told */
    CHECK(apps_alarm_pending(), "u waits");
    memset(info, 0, sizeof info);
    n = apps_alarm_list_info("aa", info);
    CHECK(n == 2 && strcmp(info[0].name, "u") == 0 && info[0].remaining_ms == 0 && !info[0].sound && info[0].wake &&
              strcmp(info[1].name, "t") == 0 && info[1].remaining_ms == 4000 && info[1].sound && !info[1].wake,
          "a firing that waits: %d, %s %lld %d %d, %s %lld %d %d", n, info[0].name, (long long)info[0].remaining_ms, info[0].sound,
          info[0].wake, info[1].name, (long long)info[1].remaining_ms, info[1].sound, info[1].wake);
    CHECK(take_all(true) == 1, "u told");
    n = apps_alarm_list_info("aa", info);
    CHECK(n == 1 && strcmp(info[0].name, "t") == 0 && info[0].remaining_ms == 4000, "told, it is gone: %d", n);
    n = apps_alarm_list_info("bb", info);
    CHECK(n == 1 && strcmp(info[0].name, "x") == 0 && info[0].remaining_ms == 2000 && info[0].sound && info[0].wake, "bb's own: %d", n);
    CHECK(apps_alarm_list_info("cc", info) == 0 && apps_alarm_list_info("Not an id", info) == 0, "none for another app or a bad id");
    clear_all();
}

/* apps_alarm_take_cancelled, which the app task's records follow: the apps whose alarms a cancel of all of them (apps_alarm_cancel_app)
 * or the budget emptied, each once, in the order they came; a take of fewer leaves the rest for the next; a cancel that found nothing notes
 * nothing; and between two takes the first APPS_ALARMS_MAX apps are kept. */
static void the_cancelled_apps_are_handed_over_once(void) {
    char ids[APPS_ALARMS_MAX][APPS_ID_MAX + 1];
    CHECK(apps_alarm_take_cancelled(ids, APPS_ALARMS_MAX) == 0, "none yet");
    CHECK(arm("aa", "t", 5000, false, false) && arm("bb", "t", 5000, false, false) && arm("bb", "u", 6000, false, false), "arm");
    apps_alarm_cancel_app("bb", "crashed when its alarm launched it");
    apps_alarm_cancel_app("aa", "was removed");
    apps_alarm_cancel_app("bb", "again");          /* nothing left to cancel */
    apps_alarm_cancel_app("zz", "has no alarms");  /* nor here */
    int n = apps_alarm_take_cancelled(ids, 1);
    CHECK(n == 1 && strcmp(ids[0], "bb") == 0, "the first, alone: %d %s", n, n > 0 ? ids[0] : "");
    n = apps_alarm_take_cancelled(ids, APPS_ALARMS_MAX);
    CHECK(n == 1 && strcmp(ids[0], "aa") == 0, "then the rest: %d %s", n, n > 0 ? ids[0] : "");
    CHECK(apps_alarm_take_cancelled(ids, APPS_ALARMS_MAX) == 0, "each once");
    /* The budget's cut-off on the firing path (the esp_timer task) notes the app too. */
    advance(10000);  /* the log's bucket fills again: the cut-off's line is one of its eight */
    for (int k = 1; k <= 7; k++) {
        CHECK(arm("dd", "t", 1, false, false), "arm %d", k);
        advance(1);
        (void)take_all(true);
    }
    CHECK(count_from(0, "log W alarm: dd fired more than 6 alarms in a minute; its alarms are cancelled") == 1, "the 7th was cut off");
    n = apps_alarm_take_cancelled(ids, APPS_ALARMS_MAX);
    CHECK(n == 1 && strcmp(ids[0], "dd") == 0, "the budget's: %d %s", n, n > 0 ? ids[0] : "");
    /* More apps than the bound between two takes: the first APPS_ALARMS_MAX. */
    static const char *const apps[] = {"a1", "a2", "a3", "a4", "a5", "a6", "a7", "a8", "a9"};
    _Static_assert(ARRAY_LEN(apps) == APPS_ALARMS_MAX + 1, "one more than the bound");
    for (size_t i = 0; i < ARRAY_LEN(apps); i++) {
        CHECK(arm(apps[i], "t", 5000, false, false), "arm %s", apps[i]);
        apps_alarm_cancel_app(apps[i], "was removed");
    }
    n = apps_alarm_take_cancelled(ids, APPS_ALARMS_MAX);
    CHECK(n == APPS_ALARMS_MAX && strcmp(ids[0], "a1") == 0 && strcmp(ids[APPS_ALARMS_MAX - 1], "a8") == 0, "the first %d: %d",
          APPS_ALARMS_MAX, n);
    CHECK(apps_alarm_take_cancelled(ids, APPS_ALARMS_MAX) == 0, "and no more");
}

/* Three threads arm, cancel, ask, cancel by id, end apps and take at random while the clock jumps on (400 jumps of three seconds, so that the
 * alarms are due in fake time, and each app's budget is a minute of it) and the esp_timer task fires what comes due: no check of what they
 * are given fails (and under AddressSanitizer and UBSan no access is bad), the callback never finds the lock held, no firing is taken twice
 * by anyone (each success is recorded by its slot and gen, and a repeat is an error), and when they have stopped and everything is told or
 * cancelled the table is empty. The count of firings is a statistic (it depends on how the threads were scheduled); a last, quiet phase shows
 * the table fires and tells as it should. */
static void the_table_holds_up_to_three_tasks_and_the_esp_timer_task(void) {
    apps_alarm_set_fired_cb(concurrent_cb);
    pthread_t threads[3];
    for (intptr_t i = 0; i < 3; i++) {
        CHECK(pthread_create(&threads[i], NULL, worker, (void *)i) == 0, "no thread");
    }
    atomic_store(&go, true);
    for (int round = 0; round < 400; round++) {
        jump(3000);
        usleep(300);
    }
    atomic_store(&stop_workers, true);
    for (int i = 0; i < 3; i++) {
        pthread_join(threads[i], NULL);
    }
    advance(1000);
    event("stress: %llu operations, %u firings, %llu of them taken as told, %d distinct takes recorded", (unsigned long long)atomic_load(&ops),
          atomic_load(&cb_calls), (unsigned long long)atomic_load(&takes_ok), n_taken);
    printf("stress: %llu operations, %u firings, %llu takes of them as told, %d distinct takes recorded\n", (unsigned long long)atomic_load(&ops),
           atomic_load(&cb_calls), (unsigned long long)atomic_load(&takes_ok), n_taken);
    CHECK(atomic_load(&ops) > 500, "only %llu operations", (unsigned long long)atomic_load(&ops));
    CHECK((unsigned long long)n_taken == atomic_load(&takes_ok) || n_taken == (int)ARRAY_LEN(taken), "%d takes recorded for %llu successes", n_taken,
          (unsigned long long)atomic_load(&takes_ok));
    (void)take_all(true);
    for (int id = 0; id < 3; id++) {
        char app[8];
        snprintf(app, sizeof app, "app%d", id);
        for (int k = 0; k < 6; k++) {
            char name[8];
            snprintf(name, sizeof name, "n%d", k);
            apps_alarm_cancel(app, name);
        }
    }
    advance(10000);
    CHECK(!apps_alarm_pending(), "a firing is left");
    for (int id = 0; id < 3; id++) {
        char app[8], text[100];
        snprintf(app, sizeof app, "app%d", id);
        CHECK(listing(app, text, sizeof text) == 0, "%s still has %s", app, text);
    }
    apps_alarm_ring_stop();
    advance(5000);
    CHECK(fake_esp_timer_next_due_us() == INT64_MAX, "a timer is left running");
    /* A quiet phase, with nothing else going on: a firing is made, told and told once, and the callback was called for it. */
    unsigned before = atomic_load(&cb_calls);
    CHECK(arm("quiet", "t", 5, false, false), "arm");
    advance(5);
    CHECK(atomic_load(&cb_calls) == before + 1 && apps_alarm_pending(), "the firing reached the callback");
    CHECK(take_all(true) == 1 && !apps_alarm_pending(), "and was told, once");
}

int main(int argc, char **argv) {
    static const struct {
        const char *name;
        void (*run)(void);
    } cases[] = {
        {"arm_remaining_list_cancel", arm_remaining_list_cancel},
        {"limits_and_names", limits_and_names},
        {"ids_follow_the_apps_rule", ids_follow_the_apps_rule},
        {"a_fired_alarm_waits_in_its_slot", a_fired_alarm_waits_in_its_slot},
        {"sound_and_wake_options", sound_and_wake_options},
        {"the_ring_chirps_until_its_minute_is_over", the_ring_chirps_until_its_minute_is_over},
        {"stopping_the_ring", stopping_the_ring},
        {"a_second_alarm_makes_the_ring_a_minute_longer", a_second_alarm_makes_the_ring_a_minute_longer},
        {"an_alarm_without_sound_leaves_the_ring_alone", an_alarm_without_sound_leaves_the_ring_alone},
        {"firings_in_a_ring_ask_for_no_chirp_of_their_own", firings_in_a_ring_ask_for_no_chirp_of_their_own},
        {"a_firing_given_up_on_ends_its_ring", a_firing_given_up_on_ends_its_ring},
        {"cancelling_a_firing_ends_its_ring", cancelling_a_firing_ends_its_ring},
        {"a_dropped_firing_leaves_the_ring_to_the_one_still_waiting", a_dropped_firing_leaves_the_ring_to_the_one_still_waiting},
        {"a_crash_while_another_firing_waits_leaves_the_ring_going", a_crash_while_another_firing_waits_leaves_the_ring_going},
        {"the_told_app_ending_with_nothing_waiting_stops_the_ring", the_told_app_ending_with_nothing_waiting_stops_the_ring},
        {"cancelling_an_apps_alarms_by_its_id", cancelling_an_apps_alarms_by_its_id},
        {"the_budget_is_six_firings_in_less_than_a_minute", the_budget_is_six_firings_in_less_than_a_minute},
        {"an_app_that_arms_alarms_all_the_time_is_cut_off_after_six", an_app_that_arms_alarms_all_the_time_is_cut_off_after_six},
        {"a_flood_of_firings_is_logged_at_the_rate_of_the_bucket", a_flood_of_firings_is_logged_at_the_rate_of_the_bucket},
        {"arming_a_name_that_fired_keeps_its_firing", arming_a_name_that_fired_keeps_its_firing},
        {"a_replaced_alarm_fires_once_at_its_new_time", a_replaced_alarm_fires_once_at_its_new_time},
        {"a_stale_call_does_nothing", a_stale_call_does_nothing},
        {"a_busy_table_is_tried_again", a_busy_table_is_tried_again},
        {"a_retry_started_behind_an_arming_does_not_lose_it", a_retry_started_behind_an_arming_does_not_lose_it},
        {"firings_are_told_in_the_order_they_came_due", firings_are_told_in_the_order_they_came_due},
        {"a_stale_copy_is_not_taken_twice", a_stale_copy_is_not_taken_twice},
        {"the_pending_word_follows_the_firings", the_pending_word_follows_the_firings},
        {"a_launch_is_wanted_while_a_firing_it_was_asked_for_waits", a_launch_is_wanted_while_a_firing_it_was_asked_for_waits},
        {"no_memory_for_a_timer_is_told_and_tried_again", no_memory_for_a_timer_is_told_and_tried_again},
        {"list_info_gives_each_alarm_its_time_and_options", list_info_gives_each_alarm_its_time_and_options},
        {"the_cancelled_apps_are_handed_over_once", the_cancelled_apps_are_handed_over_once},
        {"the_table_holds_up_to_three_tasks_and_the_esp_timer_task", the_table_holds_up_to_three_tasks_and_the_esp_timer_task},
    };
    if (argc != 2) {
        fprintf(stderr, "usage: %s <case>\n", argv[0]);
        return 2;
    }
    for (size_t i = 0; i < ARRAY_LEN(cases); i++) {
        if (strcmp(argv[1], cases[i].name) == 0) {
            boot();
            cases[i].run();
            finish();
            fflush(stdout);
            fflush(stderr);
            _exit(0);
        }
    }
    fprintf(stderr, "no case '%s'\n", argv[1]);
    return 2;
}
