# Copyright (c) Meta Platforms, Inc. and affiliates.
#
# Licensed under the Apache License, Version 2.0 (the "License");
# you may not use this file except in compliance with the License.
# You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""The tally example's hook (components/muse/apps/examples/tally/hook.sh): the connected-apps pattern the guide teaches,
run under this machine's bash in a temporary VM workspace.

On Muse's VM the hook runtime runs the hook as `/bin/bash` (bash 5, GNU coreutils) with the cwd at ~/workspace, through
the wrapper in ~/hooks/scripts/, and the hook sources $HATCH_HOOK_RUNTIME for silent, wake, log, disable_after_run,
hook_state_get and hook_state_set (the connected apps plan's hook contract, P23). Here:
- bash is the one on PATH, and the tests skip when it is older than 4 (macOS's /bin/bash is 3.2);
- a shim directory goes first on PATH: GNU `stat` and `date` through gstat and gdate, a fake clock (`date +%s` reads it)
  and a fake `sleep` that moves it on, and that also refreshes outbox/active.json as the gadget's heartbeat does while
  the app is in front (when the test says so); `mv` is logged, so the order of the hook's writes can be read;
- $HATCH_HOOK_RUNTIME is a file that defines the runtime's functions as the contract has them: one result line on stdout
  and exit 0 for silent and wake (a payload that isn't JSON fails the call), a structured line on stderr for log (the
  fields a JSON object), and hook_state_set that validates and, on a dry run, skips the write.

Every run must end the way the runtime requires: exit 0, exactly one result line on stdout and nothing else there, and
only the runtime's log lines on stderr (no content, no stray errors). The call budget's timing is checked against a model
of the budget's rules (ruling P21) on a copy of the hook whose one data-source function (P22) is a stub that counts its
calls; the hook itself has no test seam.
"""

import json
import os
import re
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
TALLY = ROOT / "components/muse/apps/examples/tally"
HOOK = TALLY / "hook.sh"

T0 = 1_800_000_000  # the fake clock's start, epoch seconds

# The budget and the loop (ruling P21, the guide's Connected apps): a model the hook's timing is checked against.
TOKENS_MAX = 100       # the bucket holds at most this many calls
TOKEN_EVERY_S = 36     # and gets one back this often: 100 an hour
FAST_S = 5             # the gap between reads with EASE_AT tokens or more
SLOW_S = 30            # growing evenly to this at LOW_AT
EASE_AT = 50
LOW_AT = 10
FLOOR = -20            # commands may take the bucket this low
LOOP_S = 50            # a run checks outbox/ for this long at most
TICK_S = 1             # this often
HEARTBEAT_MAX_AGE_S = 15


def refill(tokens, at, now):
    """The bucket at now: one token back for every TOKEN_EVERY_S since at, up to TOKENS_MAX (a full bucket's at is now)."""
    at = min(at, now)
    if tokens >= TOKENS_MAX:
        return TOKENS_MAX, now
    gained = (now - at) // TOKEN_EVERY_S
    if gained <= 0:
        return tokens, at
    tokens += gained
    if tokens >= TOKENS_MAX:
        return TOKENS_MAX, now
    return tokens, at + gained * TOKEN_EVERY_S


def gap(tokens, now, at):
    """The least time between two reads of the data source with this many tokens (P21)."""
    if tokens >= EASE_AT:
        return FAST_S
    if tokens >= LOW_AT:
        return FAST_S + (EASE_AT - tokens) * (SLOW_S - FAST_S) // (EASE_AT - LOW_AT)
    return max(SLOW_S, at + TOKEN_EVERY_S - now)


def model_reads(start, tokens, at, last_poll, ticks=LOOP_S // TICK_S):
    """The times one run reads the data source, from start with that bucket, with no commands, for that many ticks (all of
    LOOP_S while the heartbeat stays fresh; HEARTBEAT_MAX_AGE_S + 1 when it was fresh at the start and ages): each tick, the
    bucket is refilled, and a read goes when a token is left and its gap has passed. Returns the times and the bucket after
    the run (tokens, at, last_poll)."""
    reads = []
    for now in range(start, start + ticks * TICK_S, TICK_S):
        tokens, at = refill(tokens, at, now)
        if tokens > 0 and now - last_poll >= gap(tokens, now, at):
            tokens -= 1
            last_poll = now
            reads.append(now)
    return reads, tokens, at, last_poll


def find_bash():
    """bash 4 or later from PATH, or None."""
    bash = shutil.which("bash")
    if bash is None:
        return None
    done = subprocess.run([bash, "-c", "echo ${BASH_VERSINFO[0]}"], capture_output=True, text=True)
    return bash if done.returncode == 0 and done.stdout.strip().isdigit() and int(done.stdout) >= 4 else None


BASH = find_bash()
GNU = {name: shutil.which(f"g{name}") or (shutil.which(name) if sys.platform.startswith("linux") else None)
       for name in ("stat", "date", "touch")}
JQ = shutil.which("jq") or "/usr/bin/jq"
MISSING = ("no bash 4 or later on PATH (the VM's is 5; macOS's /bin/bash is 3.2)" if BASH is None else
           "no GNU coreutils (gstat, gdate, gtouch) for the VM's stat -c %Y, date +%s and touch -d" if None in GNU.values() else
           "no jq" if not os.access(JQ, os.X_OK) else None)

# The runtime's functions, as the hook contract defines them (Muse's VM: hatch-cron's hooks/storage.rs).
RUNTIME = r"""# The hook runtime's functions, as Muse's VM defines them (tests/test_muse_apps_hook.py's stand-in).
_hook_result() {
    local action=$1 reason=${2:-} payload=${3:-null}
    printf '%s' "$payload" | "$JQ" . >/dev/null 2>&1 || return 1
    printf 'HATCH_HOOK_RESULT:%s\n' "$("$JQ" -cn --arg a "$action" --arg r "$reason" --argjson p "$payload" \
        '{action: $a, reason: $r, payload: $p}')"
    exit 0
}
silent() { _hook_result silent "$@"; }
wake() { _hook_result wake "$@"; }
log() {
    local fields=${2:-'{}'}
    printf '%s' "$fields" | "$JQ" -e 'type == "object"' >/dev/null 2>&1 || return 1
    printf 'HATCH_HOOK_LOG:%s\n' "$("$JQ" -cn --arg m "$1" --argjson f "$fields" '{message: $m, fields: $f}')" >&2
}
disable_after_run() { :; }
hook_state_get() {
    local file="$HATCH_HOOK_STATE_DIR/$HATCH_HOOK_ID.json"
    if [[ -f "$file" ]]; then cat "$file"; else printf '{}\n'; fi
}
hook_state_set() {
    printf '%s' "$1" | "$JQ" -e 'type == "object"' >/dev/null 2>&1 || return 1
    if [[ "$HATCH_HOOK_DRY_RUN" == "1" ]]; then return 0; fi
    local file="$HATCH_HOOK_STATE_DIR/$HATCH_HOOK_ID.json"
    printf '%s' "$1" > "$file.tmp" && /bin/mv "$file.tmp" "$file"
}
"""


def stub_fetch_state(text, calls):
    """hook.sh's text with the body of its one data-source function, fetch_state (P22), replaced by a stub that writes the
    fake clock's time to calls for each call, then prints the state the app's file holds, as tally's own does."""
    start = text.index("\nfetch_state() {\n") + 1
    end = text.index("\n}\n", start) + 3
    stub = (f'fetch_state() {{\n    date +%s >> "{calls}"\n'
            '    if [[ -f "$STATE_FILE" ]]; then cat "$STATE_FILE"; else printf \'{"count":0}\\n\'; fi\n}\n')
    return text[:start] + stub + text[end:]


class Result:
    def __init__(self, test, done):
        test.assertEqual(done.returncode, 0, f"the run failed: {done.stdout}\n{done.stderr}")
        lines = done.stdout.splitlines()
        test.assertEqual(len(lines), 1, f"one result line and nothing else on stdout: {done.stdout!r}")
        test.assertTrue(lines[0].startswith("HATCH_HOOK_RESULT:"), lines[0])
        self.result = json.loads(lines[0][len("HATCH_HOOK_RESULT:"):])
        self.logs = []
        for line in done.stderr.splitlines():
            test.assertTrue(line.startswith("HATCH_HOOK_LOG:"), f"stderr has only the runtime's log lines: {line!r}")
            self.logs.append(json.loads(line[len("HATCH_HOOK_LOG:"):]))


@unittest.skipIf(MISSING, MISSING)
class HookTestCase(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.home = self.root / "home"
        self.workspace = self.home / "workspace"
        self.app = self.workspace / "apps/tally"
        self.app.mkdir(parents=True)
        (self.app / "state").mkdir()
        (self.app / "outbox").mkdir()
        self.state_dir = self.home / "hooks/state"
        self.state_dir.mkdir(parents=True)
        self.shim = self.root / "shim"
        self.shim.mkdir()
        self.clock = self.root / "clock"
        self.sleeps = self.root / "sleeps"
        self.moves = self.root / "moves"
        self.beat = self.root / "beat"
        self.calls = self.root / "calls"
        self.runtime = self.root / "runtime.sh"
        self.runtime.write_text(f'JQ="{JQ}"\n' + RUNTIME)
        self.set_clock(T0)
        self.write_shim()
        self.install(HOOK.read_text())

    def write_shim(self):
        scripts = {
            "date": (f'if [[ $# -eq 1 && "$1" == "+%s" ]]; then read -r t < "{self.clock}"; echo "$t"; '
                     f'else exec "{GNU["date"]}" "$@"; fi\n'),
            # GNU stat; on macOS its -c %Y (the hook's) as the system's stat -f %m, the same number, which starts faster.
            "stat": ((f'if [[ $# -eq 3 && "$1" == "-c" && "$2" == "%Y" ]]; then exec /usr/bin/stat -f %m "$3"; fi\n'
                      if sys.platform == "darwin" else "") + f'exec "{GNU["stat"]}" "$@"\n'),
            # The fake sleep: the clock moves on, and while the test has the app in front the gadget's heartbeat comes with it.
            "sleep": (f'read -r t < "{self.clock}"\nt=$(( t + ${{1%.*}} ))\necho "$t" > "{self.clock}"\necho "$1" >> "{self.sleeps}"\n'
                      f'if [[ -f "{self.beat}" ]]; then "{GNU["touch"]}" -d "@$t" "{self.app}/outbox/active.json"; fi\n'),
            "mv": f'echo "${{@: -1}}" >> "{self.moves}"\nexec /bin/mv "$@"\n',
        }
        for name, body in scripts.items():
            path = self.shim / name
            path.write_text("#!/bin/bash\n" + body)
            path.chmod(0o755)

    def install(self, text):
        (self.app / "hook.sh").write_text(text)

    def set_clock(self, t):
        self.clock.write_text(f"{t}\n")

    def now(self):
        return int(self.clock.read_text())

    def last_tick(self):
        """A heartbeat 15 s old: in front for one tick, after which it is too old and the run ends."""
        self.heartbeat(HEARTBEAT_MAX_AGE_S)

    def heartbeat(self, age=0, *, kept=False):
        """outbox/active.json written age s ago, as the gadget writes it; kept: the gadget goes on writing it as time passes
        (the app stays in front), else it ages."""
        path = self.app / "outbox/active.json"
        path.write_text('{"app":"tally","n":1}')
        t = self.now() - age
        os.utime(path, (t, t))
        if kept:
            self.beat.write_text("")
        elif self.beat.exists():
            self.beat.unlink()

    def command(self, seq, name, args=None):
        (self.app / f"outbox/cmd-{seq}.json").write_text(json.dumps({"seq": seq, "name": name, "args": args},
                                                                    separators=(",", ":")))

    def hook_state(self, value=None):
        path = self.state_dir / "tally.json"
        if value is not None:
            path.write_text(json.dumps(value))
        return json.loads(path.read_text()) if path.exists() else None

    def run_hook(self, *, dry=False):
        env = {"PATH": f"{self.shim}:/usr/bin:/bin", "HOME": str(self.home), "LANG": "C",
               "HATCH_HOOK_RUNTIME": str(self.runtime), "HATCH_HOOK_ID": "tally", "HATCH_HOOK_INVOCATION_ID": "test-1",
               "HATCH_HOOK_SCRIPT_PATH": str(self.home / "hooks/scripts/tally.sh"),
               "HATCH_HOOK_STATE_DIR": str(self.state_dir), "HATCH_HOOK_DRY_RUN": "1" if dry else "0"}
        done = subprocess.run([BASH, str(self.app / "hook.sh")], cwd=self.workspace, env=env, capture_output=True,
                              text=True, timeout=120)
        return Result(self, done)

    def read(self, rel):
        path = self.app / rel
        return path.read_text() if path.exists() else None

    def count(self):
        return json.loads(self.read("state/count.json"))["count"]

    def acks(self):
        return json.loads(self.read("state/acks.json"))

    def tree(self):
        """Every file under the app's folder and the hook's state, with its bytes and mtime."""
        files = {}
        for base in (self.app, self.state_dir):
            for path in sorted(base.rglob("*")):
                if path.is_file():
                    files[str(path)] = (path.read_bytes(), path.stat().st_mtime_ns)
        return files

    def lines(self, path):
        return path.read_text().split() if path.exists() else []


class TallyHook(HookTestCase):
    def test_idle_when_the_heartbeat_is_stale(self):
        # The in-front gate: no heartbeat, or one older than 15 s, and the run ends at once (no sleep, no time passing) with
        # nothing written: no state, no acks, no hook state.
        self.command(1, "add", {"n": 1})
        for age in (None, HEARTBEAT_MAX_AGE_S + 1, 3600):
            with self.subTest(age=age):
                if age is not None:
                    self.heartbeat(age)
                run = self.run_hook()
                self.assertEqual(run.result["action"], "silent")
                self.assertEqual(sorted(p.name for p in (self.app / "state").iterdir()), [])
                self.assertEqual(sorted(p.name for p in (self.app / "outbox").iterdir()),
                                 ([] if age is None else ["active.json"]) + ["cmd-1.json"])
                self.assertIsNone(self.hook_state())
                self.assertEqual(self.now(), T0, "no time passed")
                self.assertEqual(self.lines(self.sleeps), [])
        # 15 s old is still in front: the run serves the command, then ends once the heartbeat is past 15 s.
        self.heartbeat(HEARTBEAT_MAX_AGE_S)
        self.run_hook()
        self.assertEqual(self.count(), 1)
        self.assertEqual(self.now(), T0 + 1, "one tick, after which the heartbeat is 16 s old")

    def test_applies_commands_in_seq_order_once(self):
        # Commands go in seq order, as numbers (cmd-10 after cmd-9), each once: applied, its seq kept in the hook's state,
        # acked, and its file removed. The same files again (a re-read) change nothing; a new seq still goes.
        commands = [(9, "add", {"n": 3}), (10, "reset", None), (11, "add", {"n": 4})]
        for seq, name, args in commands:
            self.command(seq, name, args)
        self.last_tick()
        self.run_hook()
        self.assertEqual(self.count(), 4, "add 3, reset, add 4: in seq order")
        acks = self.acks()
        self.assertEqual([a["seq"] for a in acks["acks"]], [9, 10, 11])
        self.assertEqual([a["result"] for a in acks["acks"]],
                         [{"ok": True, "count": 3}, {"ok": True, "count": 0}, {"ok": True, "count": 4}])
        self.assertEqual(self.hook_state()["last_seq"], 11)
        self.assertEqual(sorted(p.name for p in (self.app / "outbox").iterdir()), ["active.json"])

        for seq, name, args in commands:
            self.command(seq, name, args)
        self.command(12, "add", {"n": 1})
        self.last_tick()
        self.run_hook()
        self.assertEqual(self.count(), 5, "9-11 aren't applied again; 12 is")
        self.assertEqual([a["seq"] for a in self.acks()["acks"]], [9, 10, 11, 12])
        self.assertEqual(sorted(p.name for p in (self.app / "outbox").iterdir()), ["active.json"])

    def test_unknown_or_bad_commands_are_acked_with_an_error(self):
        # A command the hook doesn't know, or bad args, is acked with ok false and why, so the app's callback hears at once.
        self.command(1, "jump", None)
        self.command(2, "add", {"n": "five"})
        self.last_tick()
        self.run_hook()
        self.assertEqual([(a["seq"], a["result"]["ok"]) for a in self.acks()["acks"]], [(1, False), (2, False)])
        self.assertEqual(self.count(), 0)

    def test_writes_rev_after_every_state_change(self):
        # Every change to the app's state file is followed by a new state/rev (after the file, so an app that sees the new
        # rev reads the new state), and a run that changes nothing writes no rev.
        self.command(1, "add", {"n": 1})
        self.command(2, "add", {"n": 2})
        self.last_tick()
        self.run_hook()
        moved = self.lines(self.moves)  # the hook's paths, relative to its folder
        state = [p for p in moved if p in ("state/count.json", "state/rev")]
        self.assertEqual(state, ["state/count.json", "state/rev"] * 2, "1, then 3: each followed by its rev")
        self.assertEqual(self.read("state/rev").strip(), "2")
        self.assertLess(len(self.read("state/rev").encode()), 64)
        self.assertEqual(self.count(), 3)

        self.moves.unlink()
        self.set_clock(T0 + 10)  # a read is due: it finds what the app has
        self.last_tick()
        self.calls.write_text("")
        self.install(stub_fetch_state(HOOK.read_text(), self.calls))
        self.run_hook()
        self.assertEqual(self.lines(self.calls), [str(T0 + 10)])
        moved = self.lines(self.moves)
        self.assertEqual([p for p in moved if p.startswith("state/")], [], "nothing changed: no write")
        self.assertEqual(self.read("state/rev").strip(), "2")

        # A first state the app has never seen is published too, with its rev.
        for f in (self.app / "state").iterdir():
            f.unlink()
        self.set_clock(T0 + 20)
        self.last_tick()
        self.run_hook()
        self.assertEqual((self.count(), self.read("state/rev").strip()), (0, "1"))

    def test_acks_list_applied_seqs(self):
        # state/acks.json is {"acks":[{"seq":<int>,"result":<any JSON>}, ...]}, newest last, the last 16 applied (P3).
        for seq in range(101, 119):
            self.command(seq, "add", {"n": 1})
        self.last_tick()
        self.run_hook()
        acks = self.acks()
        self.assertEqual(list(acks), ["acks"])
        self.assertEqual([a["seq"] for a in acks["acks"]], list(range(103, 119)))
        for a in acks["acks"]:
            self.assertEqual(sorted(a), ["result", "seq"])
            self.assertEqual(a["result"], {"ok": True, "count": a["seq"] - 100})

    def test_dry_run_has_no_side_effects(self):
        # A dry run writes nothing (no state, no acks, no removed commands, no hook state) and applies no command, but its
        # read of the data source still runs, so a connector's approval card comes up in that turn, even with the app closed.
        (self.app / "state/count.json").write_text('{"count":7}')
        (self.app / "state/rev").write_text("4\n")
        (self.app / "state/acks.json").write_text('{"acks":[{"seq":1,"result":{"ok":true,"count":7}}]}')
        self.hook_state({"tokens": 40, "at": T0 - 10, "last_poll": T0 - 100, "last_seq": 1})
        self.command(2, "add", {"n": 1})
        self.command(3, "reset", None)
        self.heartbeat(kept=True)  # the app in front
        before = self.tree()
        self.assertEqual(self.run_hook(dry=True).result["action"], "silent")
        self.assertEqual(self.tree(), before)
        (self.app / "outbox/active.json").unlink()  # the app closed, as during its creation
        self.beat.unlink()
        before = self.tree()
        self.assertEqual(self.run_hook(dry=True).result["action"], "silent")
        self.assertEqual(self.tree(), before)
        self.assertEqual(self.now(), T0, "no loop: one pass")
        self.install(stub_fetch_state(HOOK.read_text(), self.calls))
        self.run_hook(dry=True)
        self.assertEqual(len(self.lines(self.calls)), 1, "the read runs once, with the app closed")

    def test_the_loop_ends_by_50_s(self):
        # While the app stays in front, one run checks outbox/ every 1 s, for 50 s at most, then ends (the next poll starts
        # another), well inside its 60 s timeout.
        self.heartbeat(kept=True)
        run = self.run_hook()
        self.assertEqual(run.result["action"], "silent")
        self.assertEqual(self.now(), T0 + LOOP_S)
        self.assertEqual(self.lines(self.sleeps), ["1"] * (LOOP_S // TICK_S))
        # A command that comes while the run waits is served at the next tick, with its read at once.
        self.set_clock(T0 + 1000)
        self.heartbeat()
        self.calls.write_text("")
        self.install(stub_fetch_state(HOOK.read_text(), self.calls))
        scripts = self.shim / "sleep"
        scripts.write_text(scripts.read_text() + f'if [[ $t -eq {T0 + 1002} ]]; then echo \'{{"seq":5,"name":"add","args":{{"n":2}}}}\' '
                                                 f'> "{self.app}/outbox/cmd-5.json"; fi\n')
        self.run_hook()
        self.assertEqual(self.count(), 2)
        self.assertEqual([int(t) for t in self.lines(self.calls)], [T0 + 1000, T0 + 1002, T0 + 1007, T0 + 1012],
                         "the read after the command at once, then the polls 5 s on")

    def test_budget_backs_off(self):
        # The call budget (P21): a bucket of 100 calls that gets one back every 36 s, in the hook's state. With 50 or more
        # tokens the hook reads every 5 s; from 50 down to 10 the gap grows evenly to 30 s; below 10 it is 30 s or the time
        # to the next token, if longer; with none left reads wait for a token. Checked against the model above, on a copy
        # whose fetch_state counts its calls: a run whose heartbeat ages out (16 ticks), and one the app stays in front for.
        self.assertEqual([gap(t, 0, 0) for t in (100, 50, 30, 10)], [5, 5, 17, 30])
        self.assertEqual(gap(9, 0, 0), 36, "below 10: the time to the next token, when longer than 30 s")
        self.assertEqual(gap(5, 20, 0), 30)
        short = HEARTBEAT_MAX_AGE_S + 1
        cases = {
            "full": ({}, short),  # a new hook: a full bucket
            "easing": ({"tokens": 46, "at": T0, "last_poll": 0}, short),
            "low": ({"tokens": 8, "at": T0, "last_poll": T0 - 25}, short),
            "refilled": ({"tokens": 90, "at": T0 - 3600, "last_poll": 0}, short),  # an hour away refills it, to 100 at most
            "empty": ({"tokens": 0, "at": T0 - 30, "last_poll": T0 - 60}, LOOP_S // TICK_S),
        }
        for name, (seed, ticks) in cases.items():
            with self.subTest(name):
                self.set_clock(T0)
                self.calls.write_text("")
                if seed:
                    self.hook_state({**seed, "last_seq": 0})
                elif (self.state_dir / "tally.json").exists():
                    (self.state_dir / "tally.json").unlink()
                self.install(stub_fetch_state(HOOK.read_text(), self.calls))
                self.heartbeat(kept=ticks > short)
                self.run_hook()
                self.assertEqual(self.now(), T0 + ticks * TICK_S)
                reads, tokens, at, last_poll = model_reads(T0, seed.get("tokens", TOKENS_MAX), seed.get("at", 0),
                                                           seed.get("last_poll", 0), ticks)
                self.assertEqual([int(t) for t in self.lines(self.calls)], reads)
                state = self.hook_state()
                self.assertEqual((state["tokens"], state["at"], state["last_poll"]), (tokens, at, last_poll))
                self.assertLessEqual(len(json.dumps(state)), 16 * 1024)
        self.assertEqual(model_reads(T0, TOKENS_MAX, 0, 0)[0], list(range(T0, T0 + LOOP_S, FAST_S)))
        self.assertEqual(model_reads(T0, 46, T0, 0, short)[0], [T0, T0 + 8], "8 s at 45 tokens: the gap grows as they go")
        self.assertEqual(model_reads(T0, 8, T0, T0 - 25, short)[0], [T0 + 6], "30 s once the next token is 30 s away")
        self.assertEqual(model_reads(T0, 0, T0 - 30, T0 - 60)[0], [T0 + 6, T0 + 42], "each read waits for its token")

    def test_commands_are_served_down_to_the_floor(self):
        # Commands are always served first, with one read of the data source after each, and their reads may take the bucket
        # down to -20 (headroom under the host's 120 an hour). At -20 the command is still applied (tally's need no call)
        # but its read waits; the polls wait for tokens.
        # tokens before -> (reads, tokens after): the command's read takes one unless at the floor.
        for tokens, (reads, after) in {-19: (1, FLOOR), FLOOR: (0, FLOOR), 0: (1, -1)}.items():
            with self.subTest(tokens=tokens):
                self.set_clock(T0)
                self.calls.write_text("")
                self.hook_state({"tokens": tokens, "at": T0, "last_poll": T0, "last_seq": 0})
                for f in (self.app / "state").iterdir():
                    f.unlink()
                self.install(stub_fetch_state(HOOK.read_text(), self.calls))
                self.command(1, "add", {"n": 1})
                self.last_tick()
                self.run_hook()
                self.assertEqual(self.acks()["acks"], [{"seq": 1, "result": {"ok": True, "count": 1}}])
                self.assertEqual(len(self.lines(self.calls)), reads, "and no poll: no token")
                self.assertEqual(self.hook_state()["tokens"], after)


class HookText(unittest.TestCase):
    def test_the_preamble_and_one_data_source(self):
        # The runtime's preamble (the shebang, then set -euo pipefail, then the runtime sourced), and exactly one function that
        # calls the data source, marked as the one to replace (P22).
        text = HOOK.read_text()
        code = [line for line in text.splitlines() if line and not line.startswith("#")]
        self.assertTrue(text.startswith("#!/usr/bin/env bash\n"))
        self.assertEqual(code[:2], ["set -euo pipefail", 'source "$HATCH_HOOK_RUNTIME"'])
        self.assertEqual(text.count("\nfetch_state() {\n"), 1)
        marker = text.index("# ---- The data source")
        self.assertLess(marker, text.index("\nfetch_state() {\n"))
        self.assertNotIn("fetch_state() {", text[:marker])

    def test_no_test_seams_and_one_result_per_run(self):
        # P22: the hook reads no environment variable a test could set, only the runtime's; and silent or wake only at the
        # top level (never in a function or $(...)), so that a run prints exactly one result.
        text = HOOK.read_text()
        code = "\n".join(line for line in text.splitlines() if not line.lstrip().startswith("#"))
        shell = re.sub(r"'[^']*'", "''", code)  # jq's programs (single-quoted, over lines too) have $names of their own
        assigned = set(re.findall(r"\b([A-Za-z_]\w*)=", shell)) | set(re.findall(r"\bfor ([A-Za-z_]\w*) in\b", shell))
        for names in re.findall(r"\b(?:local|read -r|mapfile -t)\s+([^;\n|<]+)", shell):
            assigned |= {word.split("=")[0] for word in names.split() if re.fullmatch(r"[A-Za-z_]\w*(=.*)?", word)}
        used = set(re.findall(r"\$\{?([A-Za-z_]\w*)", shell))
        self.assertIn("HATCH_HOOK_DRY_RUN", used)
        self.assertEqual(sorted(used - assigned - {"HATCH_HOOK_RUNTIME", "HATCH_HOOK_DRY_RUN", "BASH_SOURCE"}), [])
        for line in code.splitlines():
            if re.search(r"\b(silent|wake)\b", line):
                self.assertRegex(line, r"^(    )?(silent|wake) ", f"silent or wake off the top level: {line!r}")
        functions = re.findall(r"(?ms)^(\w+)\(\) \{\n(.*?)^\}\n", code)
        self.assertGreater(len(functions), 5)
        for name, body in functions:
            self.assertNotRegex(body, r"\b(silent|wake)\b", f"{name} ends the run")

if __name__ == "__main__":
    unittest.main()
