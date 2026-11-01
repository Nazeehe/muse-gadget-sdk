#!/usr/bin/env bash
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

# The tally example's hook: the template of a connected app's hook (the guide's Connected apps). The VM's hook runtime
# runs it every 5 s through the wrapper ~/hooks/scripts/tally.sh, with no model in the loop. While the app is in front
# (its heartbeat, outbox/active.json, written 15 s ago or less) one run checks outbox/ every 1 s for at most 50 s:
# it applies the app's commands (outbox/cmd-<seq>.json) in seq order, each once, acks them in state/acks.json, reads
# the data source within the call budget (at most 100 calls in any rolling hour), and publishes the app's state
# (state/count.json, then state/rev) when it changes. To adapt it: replace fetch_state (the one read of the data
# source) and run_command (whose connector calls go through connector_call), and set STATE_FILE.
set -euo pipefail
source "$HATCH_HOOK_RUNTIME"

APP_DIR=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
cd "$APP_DIR"
shopt -s nullglob

STATE_FILE=state/count.json   # what the app reads: the state fetch_state prints
REV_FILE=state/rev            # changed after every change to the state, so the watch of the app sees it
ACKS_FILE=state/acks.json
HEARTBEAT=outbox/active.json

HEARTBEAT_MAX_AGE_S=15        # the in-front gate: the gadget writes the heartbeat every 10 s while the app watches
TICK_S=1                      # how often a run checks outbox/
LOOP_S=50                     # how long one run goes on (script_timeout_secs is 60)
ACKS_KEEP=16                  # the acks state/acks.json keeps, newest last
CMD_MAX_AGE_S=60              # a command older than this (its seq: the gadget clock, in ms) is acked "expired", not run

# The call budget: at most CALLS_MAX calls to the data source in any rolling WINDOW_S, from a log of their times in
# hook_state. The wait after a read depends on how many calls remain in the window.
CALLS_MAX=100
WINDOW_S=3600
FAST_S=5                      # the wait after a read while EASE_AT or more calls remain
EASE_AT=50
SPREAD_MIN_S=30               # below EASE_AT, the least wait: the calls left spread evenly until the oldest leaves
LOG_KEEP=1000                 # the log keeps at most this many times, so hook_state stays under 16 KB

dry_run() {
    [[ "${HATCH_HOOK_DRY_RUN:-0}" == "1" ]]
}

# ---- The data source: fetch_state is the one read to replace, run_command the commands; connector_call is plumbing ----
# fetch_state prints the app's state as one JSON object on stdout, or returns non-zero when it can't read it (3: not
# authorized). It is the only function that reads the data source. For a connector app, call the connector's CLI here
# by its full path, with its stderr to /dev/null: keep its exit status (3 means not authorized), check .ok in what it
# prints (a connector-side error exits 0 with {"ok":false,...}: return 2), and print only what the app needs. Tally has
# no connector: its source is its own state file.
fetch_state() {
    if [[ -f "$STATE_FILE" ]]; then
        jq -c '{count: (if (.count | type) == "number" then (.count | floor) else 0 end)}' "$STATE_FILE" 2>/dev/null
    else
        printf '{"count":0}\n'
    fi
}

# run_command NAME ARGS carries out one of the app's commands and sets RESULT, the ack's result: {"ok":true,...}, or
# {"ok":false,"error":"..."} for one it can't carry out. It must never end the run (set -e): the command's seq is saved
# before it runs, so a run that dies here never repeats it, but the app hears nothing. A connector app makes each call to
# the connector's CLI through connector_call, by its full path, always with "|| return 0":
#     connector_call /full/path/to/cli <args...> || return 0     # its RESULT is set, and on exit 3 the run ends
#     RESULT='{"ok":true}'                                         # or what OUT, the CLI's output, says
# What a connector returns stays out of files its rules forbid (the guide's Rules: "Connector rules are binding").
# Tally's commands change its own state file.
run_command() {
    local name=$1 args=$2 current='{"count":0}' count n
    if [[ -n "${PUBLISHED:-}" ]]; then current=$PUBLISHED; fi
    count=$(jq -r '(.count // 0) | if type == "number" then floor else 0 end' <<<"$current")
    case "$name" in
        add)
            # A whole number in any form (1, 1.0, 1e3) as digits: bash can't add 1.0.
            if ! n=$(jq -er '(.n // 1) as $n | if ($n | type) == "number" and $n == ($n | floor) and ($n | fabs) <= 1000000
                             then $n | floor | tostring else error("bad n") end' <<<"$args" 2>/dev/null) ||
               ! [[ "$n" =~ ^-?[0-9]+$ ]]; then
                RESULT='{"ok":false,"error":"add takes {n = a whole number}"}'
                return 0
            fi
            count=$((count + n))
            ;;
        reset)
            count=0
            ;;
        *)
            RESULT='{"ok":false,"error":"unknown command"}'
            return 0
            ;;
    esac
    publish "{\"count\":$count}"
    RESULT="{\"ok\":true,\"count\":$count}"
}

# connector_call CLI ARGS...: one call to a connector's CLI for a command (run_command's), logged and saved in the budget
# first, its stderr to /dev/null. Sets OUT to what it printed and returns 0 when it says {"ok":true,...}; else sets RESULT
# to the ack's {"ok":false,...} and returns 1, and on exit 3 (not authorized) also sets STOP. Call it as
# "connector_call CLI ARGS... || return 0", never bare: under set -e a bare call that fails ends the run (the call stays
# counted, but the command is never acked).
connector_call() {
    local rc=0
    note_call "$now"   # the now of serve_commands
    save_state
    OUT=$("$@" 2>/dev/null) || rc=$?
    if ((rc == 3)); then
        log "a connector says not authorized: re-approve with hooks.run" '{"exit":3}'
        RESULT='{"ok":false,"error":"not authorized"}'
        STOP="not authorized"
        return 1
    fi
    if ((rc != 0)) || ! jq -e '.ok == true' >/dev/null 2>&1 <<<"$OUT"; then
        log "a connector call failed" "{\"exit\":$rc}"
        RESULT='{"ok":false,"error":"the connector failed"}'
        return 1
    fi
}

# ---- Files: every write goes through these, and a dry run writes nothing --------------------------------------------
write_file() {   # PATH TEXT: replaced whole (a reader sees the old file or the new one), its folder made when missing
    if dry_run; then return 0; fi
    [[ "$1" != */* || -d "${1%/*}" ]] || mkdir -p "${1%/*}"
    printf '%s\n' "$2" > "$1.tmp"
    mv "$1.tmp" "$1"
}

remove_file() {
    if dry_run; then return 0; fi
    rm -f "$1"
}

# publish STATE: when it differs from what the app has, write the state file, then a new rev.
publish() {
    local state rev
    state=$(jq -cS . <<<"$1")
    if [[ "$state" == "${PUBLISHED:-}" ]]; then return 0; fi
    write_file "$STATE_FILE" "$state"
    rev=$(cat "$REV_FILE" 2>/dev/null || true)
    [[ "$rev" =~ ^[0-9]+$ ]] || rev=0
    write_file "$REV_FILE" "$((rev + 1))"
    PUBLISHED=$state
}

ack() {   # SEQ RESULT
    local acks
    acks=$(jq -c --argjson seq "$1" --argjson result "$2" --argjson keep "$ACKS_KEEP" \
        '((.acks // []) | map(select(type == "object"))) + [{seq: $seq, result: $result}]
         | {acks: .[(if length > $keep then length - $keep else 0 end):]}' "$ACKS_FILE" 2>/dev/null) ||
        acks=$(jq -cn --argjson seq "$1" --argjson result "$2" '{acks: [{seq: $seq, result: $result}]}')
    write_file "$ACKS_FILE" "$acks"
}

# ---- The budget: bash alone, no other process ----------------------------------------------------------------------
# CALLS holds the times of the calls to the data source in the last WINDOW_S, oldest first; NEXT_POLL is when the next
# read may go.

# prune NOW: the calls that have left the window go. After the clock steps back, so do the calls logged after NOW, and the
# next read, scheduled on the old clock, is due (as it is when NEXT_POLL is more than WINDOW_S on, which no schedule gives).
prune() {
    local k=0 n=${#CALLS[@]}
    while ((n > 0)) && ((CALLS[n - 1] > $1)); do n=$((n - 1)); done
    if ((n < ${#CALLS[@]})); then
        CALLS=("${CALLS[@]:0:n}")
        NEXT_POLL=$1
    fi
    if ((NEXT_POLL > $1 + WINDOW_S)); then NEXT_POLL=$1; fi
    while ((k < ${#CALLS[@]})) && ((CALLS[k] <= $1 - WINDOW_S)); do k=$((k + 1)); done
    if ((k > 0)); then CALLS=("${CALLS[@]:k}"); fi
}

note_call() {   # NOW: one call to the data source, logged
    CALLS+=("$1")
    if ((${#CALLS[@]} > LOG_KEEP)); then CALLS=("${CALLS[@]:${#CALLS[@]}-LOG_KEEP}"); fi
}

# schedule NOW: after a read at NOW, when the next may go. While EASE_AT or more calls remain in the window, FAST_S on.
# Below that, the calls left spread evenly over the time until the oldest call leaves, at least SPREAD_MIN_S apart, so the
# app never goes long without a read. With none left, when the log is back under CALLS_MAX (when the oldest leaves, unless
# commands took it past), and at least SPREAD_MIN_S on, so that a burst of calls an hour ago does not come back as a burst.
schedule() {
    local now=$1 remaining=$((CALLS_MAX - ${#CALLS[@]})) wait
    if ((remaining >= EASE_AT)); then
        NEXT_POLL=$((now + FAST_S))
    elif ((remaining > 0)); then
        wait=$(((CALLS[0] + WINDOW_S - now) / remaining))
        if ((wait < SPREAD_MIN_S)); then wait=$SPREAD_MIN_S; fi
        NEXT_POLL=$((now + wait))
    else
        NEXT_POLL=$((CALLS[${#CALLS[@]} - CALLS_MAX] + WINDOW_S))
        if ((NEXT_POLL < now + SPREAD_MIN_S)); then NEXT_POLL=$((now + SPREAD_MIN_S)); fi
    fi
}

take_read() {   # NOW: a read of the data source, logged and the next one scheduled
    note_call "$1"
    schedule "$1"
}

read_due() {   # NOW: whether a poll may read now: its time has come and the window has room
    ((${#CALLS[@]} < CALLS_MAX && $1 >= NEXT_POLL))
}

# ---- The hook's state -----------------------------------------------------------------------------------------------
load_state() {   # CALLS, NEXT_POLL and LAST_SEQ from hook_state (none: an empty log)
    local state calls
    state=$(hook_state_get)
    { read -r NEXT_POLL LAST_SEQ; read -r calls; } < <(jq -r '
        ([(.next_poll // 0), (.last_seq // 0)] | map(if type == "number" then floor else 0 end) | @tsv),
        ((.calls // []) | map(select(type == "number") | floor) | sort | map(tostring) | join(" "))' <<<"$state")
    read -ra CALLS <<<"$calls"
}

save_state() {
    local IFS=,
    hook_state_set "{\"calls\":[${CALLS[*]-}],\"next_poll\":$NEXT_POLL,\"last_seq\":$LAST_SEQ}"
}

# read_now NOW: one read of the data source, logged and published. Sets STOP when the source says not authorized.
read_now() {
    local state rc=0
    take_read "$1"
    READS=$((READS + 1))
    state=$(fetch_state) || rc=$?
    save_state
    if ((rc == 3)); then
        log "the data source says not authorized: re-approve with hooks.run" '{"exit":3}'
        STOP="not authorized"
    elif ((rc != 0)); then
        log "the data source failed" "{\"exit\":$rc}"
    elif jq -e 'type == "object"' >/dev/null 2>&1 <<<"$state"; then
        publish "$state"
    else
        log "the data source printed no JSON object" '{}'
    fi
}

heartbeat_fresh() {   # NOW
    local mtime
    mtime=$(stat -c %Y "$HEARTBEAT" 2>/dev/null) || return 1
    (($1 - mtime <= HEARTBEAT_MAX_AGE_S))
}

# serve_commands NOW: the waiting commands in seq order, as numbers, each once (its seq saved before it runs), with one
# read after each. Commands are always served, and their calls logged, while the run is inside LOOP_S: the rest wait for
# the next run. A command older than CMD_MAX_AGE_S is acked {"ok":false,"error":"expired"} and removed without running:
# the app said "timed out" long before (the hook was off or failing, or the VM asleep), and a tap from then must not act now.
serve_commands() {
    local now=$1 file seq name args files=(outbox/cmd-*.json)
    if ((${#files[@]} > 1)); then mapfile -t files < <(printf '%s\n' "${files[@]}" | sort -t- -k2,2n); fi
    for file in ${files[@]+"${files[@]}"}; do
        if ((${#files[@]} > 1)); then
            now=$(date +%s)
            if ((now - start >= LOOP_S)); then return 0; fi
        fi
        seq=${file#outbox/cmd-}
        seq=${seq%.json}
        if ! [[ "$seq" =~ ^[0-9]+$ ]] || ((seq <= LAST_SEQ)); then
            remove_file "$file"   # applied before (or not a command): never twice
            continue
        fi
        if ((seq / 1000 < now - CMD_MAX_AGE_S)); then
            LAST_SEQ=$seq
            save_state
            ack "$seq" '{"ok":false,"error":"expired"}'
            remove_file "$file"
            log "expired a command" "{\"seq\":$seq}"
            continue
        fi
        # NAME then ARGS (compact JSON, the rest of the line); nothing for a file that isn't this seq's command.
        if ! read -r name args < <(jq -r --argjson seq "$seq" \
            'select(.seq == $seq and (.name | type) == "string" and (.name | test("^[a-z0-9_]{1,32}$")))
             | "\(.name) \(.args | tojson)"' "$file" 2>/dev/null); then
            name=""
            args=null
        fi
        RESULT='{"ok":false,"error":"not a command"}'
        LAST_SEQ=$seq
        save_state   # before it runs: at most once, even if the run dies in it
        if [[ -n "$name" ]]; then run_command "$name" "$args"; fi
        COMMANDS=$((COMMANDS + 1))
        if [[ -z "$STOP" ]]; then read_now "$now"; fi
        ack "$seq" "$RESULT"
        remove_file "$file"
        log "applied a command" "{\"seq\":$seq}"
        if [[ -n "$STOP" ]]; then return 0; fi
    done
}

# ---- The run ------------------------------------------------------------------------------------------------------
PUBLISHED=$(jq -cS . "$STATE_FILE" 2>/dev/null || true)
READS=0
COMMANDS=0
STOP=""

if dry_run; then
    # A dry run reads once (a connector's approval card comes up now, even with the app closed) and writes nothing.
    rc=0
    fetch_state >/dev/null || rc=$?
    log "dry run: read the data source once, wrote nothing" "{\"exit\":$rc}"
    silent "dry run" "{\"exit\":$rc}"
fi

start=$(date +%s)
if ! heartbeat_fresh "$start"; then
    silent "idle: the app is not in front"
fi

load_state
why="loop done"
while :; do
    now=$(date +%s)
    if ((now - start >= LOOP_S)); then break; fi
    if ! heartbeat_fresh "$now"; then
        why="the app went behind"
        break
    fi
    prune "$now"
    serve_commands "$now"
    if [[ -z "$STOP" ]] && read_due "$now"; then read_now "$now"; fi
    if [[ -n "$STOP" ]]; then
        why=$STOP
        break
    fi
    sleep "$TICK_S"
done

save_state
silent "$why" "{\"reads\":$READS,\"commands\":$COMMANDS,\"calls_in_window\":${#CALLS[@]}}"
