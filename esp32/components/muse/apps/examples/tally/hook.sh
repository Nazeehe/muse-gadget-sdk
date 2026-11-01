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
# the data source within the call budget, and publishes the app's state (state/count.json, then state/rev) when it
# changes. To adapt it: replace fetch_state (the one call to the data source) and run_command, and set STATE_FILE.
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

# The call budget: a token bucket in hook_state, one token for each call to the data source.
TOKENS_MAX=100                # the bucket holds at most this many
TOKEN_EVERY_S=36              # and gets one back this often: 100 an hour
FAST_S=5                      # the gap between reads with EASE_AT tokens or more
SLOW_S=30                     # growing evenly to this at LOW_AT; below LOW_AT, this or the time to the next token
EASE_AT=50
LOW_AT=10
TOKENS_FLOOR=-20              # commands are served down to this: headroom under the 120 calls an hour a host allows

dry_run() {
    [[ "${HATCH_HOOK_DRY_RUN:-0}" == "1" ]]
}

# ---- The data source: the one function to replace -------------------------------------------------------------------
# fetch_state prints the app's state as one JSON object on stdout, or returns non-zero when it can't read it (3: not
# authorized). It is the only function that calls the data source. For a connector app, call the connector's CLI here
# by its full path, with its stderr to /dev/null: keep its exit status (3 means not authorized), check .ok in what it
# prints (a connector-side error exits 0 with {"ok":false,...}: return 2), and print only what the app needs. Tally has
# no connector: its source is its own state file.
fetch_state() {
    if [[ -f "$STATE_FILE" ]]; then
        jq -c '{count: (if (.count | type) == "number" then (.count | floor) else 0 end)}' "$STATE_FILE"
    else
        printf '{"count":0}\n'
    fi
}

# run_command NAME ARGS carries out one of the app's commands and sets RESULT, the ack's result: {"ok":true,...}, or
# {"ok":false,"error":"..."} for one it can't carry out. A connector app calls the connector's write here, after
# take_command_token for each call (refused at the floor: ack that). Tally's commands change its own state file.
run_command() {
    local name=$1 args=$2 current='{"count":0}' count n
    if [[ -n "${PUBLISHED:-}" ]]; then current=$PUBLISHED; fi
    count=$(jq -r '(.count // 0) | if type == "number" then floor else 0 end' <<<"$current")
    case "$name" in
        add)
            if ! n=$(jq -er '(.n // 1) as $n | if ($n | type) == "number" and $n == ($n | floor) and ($n | fabs) <= 1000000
                             then $n else error("bad n") end' <<<"$args" 2>/dev/null); then
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

# ---- Files: every write goes through these, and a dry run writes nothing --------------------------------------------
write_file() {   # PATH TEXT: replaced whole (a reader sees the old file or the new one)
    if dry_run; then return 0; fi
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

# ---- The budget ---------------------------------------------------------------------------------------------------
load_state() {   # TOKENS AT LAST_POLL LAST_SEQ from hook_state, a full bucket when there is none
    local state
    state=$(hook_state_get)
    read -r TOKENS AT LAST_POLL LAST_SEQ < <(jq -r --argjson max "$TOKENS_MAX" \
        '[(.tokens // $max), (.at // 0), (.last_poll // 0), (.last_seq // 0)]
         | map(if type == "number" then floor else 0 end) | @tsv' <<<"$state")
}

save_state() {
    hook_state_set "{\"tokens\":$TOKENS,\"at\":$AT,\"last_poll\":$LAST_POLL,\"last_seq\":$LAST_SEQ}"
}

refill() {   # NOW: a token back for each TOKEN_EVERY_S since AT, up to TOKENS_MAX
    local now=$1 gained
    if ((now < AT)); then AT=$now; fi
    if ((TOKENS >= TOKENS_MAX)); then
        TOKENS=$TOKENS_MAX
        AT=$now
        return 0
    fi
    gained=$(((now - AT) / TOKEN_EVERY_S))
    if ((gained <= 0)); then return 0; fi
    TOKENS=$((TOKENS + gained))
    if ((TOKENS >= TOKENS_MAX)); then
        TOKENS=$TOKENS_MAX
        AT=$now
    else
        AT=$((AT + gained * TOKEN_EVERY_S))
    fi
}

read_due() {   # NOW: whether the gap for the tokens left has passed (none left: no read until one is back)
    local now=$1 gap
    if ((TOKENS <= 0)); then return 1; fi
    if ((TOKENS >= EASE_AT)); then
        gap=$FAST_S
    elif ((TOKENS >= LOW_AT)); then
        gap=$((FAST_S + (EASE_AT - TOKENS) * (SLOW_S - FAST_S) / (EASE_AT - LOW_AT)))
    else
        gap=$((AT + TOKEN_EVERY_S - now))
        if ((gap < SLOW_S)); then gap=$SLOW_S; fi
    fi
    ((now - LAST_POLL >= gap))
}

take_command_token() {   # for each call of a command: refused at the floor
    if ((TOKENS <= TOKENS_FLOOR)); then return 1; fi
    TOKENS=$((TOKENS - 1))
}

# read_now NOW: one read of the data source (its token already taken), published. Sets STOP when the source says not
# authorized.
read_now() {
    local state rc=0
    LAST_POLL=$1
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

# serve_commands NOW: the waiting commands in seq order, as numbers, each once, with one read after each.
serve_commands() {
    local now=$1 file seq name args files=(outbox/cmd-*.json)
    if ((${#files[@]} > 1)); then mapfile -t files < <(printf '%s\n' "${files[@]}" | sort -t- -k2,2n); fi
    for file in ${files[@]+"${files[@]}"}; do
        seq=${file#outbox/cmd-}
        seq=${seq%.json}
        if ! [[ "$seq" =~ ^[0-9]+$ ]] || ((seq <= LAST_SEQ)); then
            remove_file "$file"   # applied before (or not a command): never twice
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
        if [[ -n "$name" ]]; then run_command "$name" "$args"; fi
        LAST_SEQ=$seq
        COMMANDS=$((COMMANDS + 1))
        if take_command_token; then read_now "$now"; else save_state; fi
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
    refill "$now"
    serve_commands "$now"
    if [[ -z "$STOP" ]] && read_due "$now"; then
        TOKENS=$((TOKENS - 1))
        read_now "$now"
    fi
    if [[ -n "$STOP" ]]; then
        why=$STOP
        break
    fi
    sleep "$TICK_S"
done

save_state
silent "$why" "{\"reads\":$READS,\"commands\":$COMMANDS,\"tokens\":$TOKENS}"
