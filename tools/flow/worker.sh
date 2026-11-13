#!/bin/sh
#
# worker.sh: run one worker session after another until STATE/stop exists.
#
# Usage: worker.sh [-a claude|codex] [-s STATE]
#
# The loop works in the clone this script is in, as the account `gh` and git
# are signed in to there. Each session works on one task, which the loop, not
# the session, claims: under a lock shared by the loops of this user on this
# machine, it takes the first task `board.sh next` lists that `board.sh take`
# gives this account, and writes its number to STATE/issue. While that task
# is open and assigned to this account, each new session resumes it; when it
# is closed or no longer this account's, as when the manager gave it to
# another account while this one waited for its usage limit, the loop drops
# it and claims another. The number is kept in a file, not read from the
# board, because two loops of one account cannot tell their tasks apart by
# assignee. Write a number to STATE/issue to give a loop a task to resume,
# with the take before it and under the claim lock, as claim does:
# `perl -MFcntl=:flock -e 'open(L, ">>", shift) && flock(L, LOCK_EX) or die;
# system(@ARGV) == 0 or exit 1' build/flow/claim.lock sh -c 'CMDS'`. Outside
# it, a loop of the same account can take the card meanwhile, since `take`
# answers yes to a card this account already holds.
#
# The session's prompt is `worker.txt`, with @ISSUE@ replaced by the task's
# number, @BRANCH@ by its milestone's branch, which `board.sh branch` reads
# before each session, and @BOARD@ by STATE/run/board.sh, master's copy.
#
# AGENT is the program a session runs: Claude Code, `claude`, the default, or
# the Codex CLI, `codex`, at the model and effort below. A Claude Code session
# waits for its helpers however long they run, which print mode does not do
# unless CLAUDE_CODE_PRINT_BG_WAIT_CEILING_MS is 0, and cannot schedule a wakeup
# or start a Monitor, since print mode keeps a finished session open until a
# pending wakeup fires or a monitor's command ends; nor can it start a subagent,
# which the rules keep from workers. STATE, `build/flow/AGENT` of
# the clone's main working tree unless given, holds the issue file, the log
# `loop.log`, each session's output as `s-TIME.jsonl` and `s-TIME.err`, the
# process group of the session that is running in `session`, the clone's path in
# `repo`, and in `copy` the copy on the testing host that RACH2_TESTING_COPY
# names for testing.sh, if any; `touch STATE/stop` ends the loop after the
# session that is running, and before it takes or starts a task, within five
# seconds of any wait; tools/flow/stop.sh also leaves STATE/session.stop, on
# which session.pl stops the session, and which the loop removes before each
# session. The loop holds STATE/loop.live, a lock on descriptor 7, for its
# whole life, through its exec of master's copy, and closes it for each
# session, whose supervisor holds STATE/session.live; stop.sh and status.sh
# probe the two locks to tell whether the loop and its session run. A loop
# that finds STATE/loop.live held exits, since another loop has STATE.
# Sessions run with RACH2_MIN_FREE_GIB at 40 unless the environment gives
# another, so that a build they start stops before it fills the disk, and
# with RACH2_WORKER_STATE set to STATE, so that `board.sh release` of the
# loop's task empties STATE/issue under the claim lock and another loop of
# the same account can take the task without this one going on with it. Codex
# reads at most DOC_BYTES of AGENTS.md, since its own limit, 32 KiB, cuts the
# file short.
#
# STATE/model, if it exists, sets the model and effort of this loop's sessions
# in place of the account's model, or CODEX_MODEL, and REASONING: one line,
# `MODEL EFFORT [UNTIL]`, with UNTIL in seconds since the epoch. The loop reads
# it before each session, so it takes effect at the next one, and removes it
# once UNTIL has passed, which puts the loop back on its defaults by itself.
#
# A session that ends on the account's usage limit makes the loop wait fifteen
# minutes, and STATE/limit keeps the time the limit's message gives for its
# reset: until then a loop whose task went to another account claims no new
# one, which it could not work. One shorter than two minutes for another
# reason, such as a lost network or a lapsed login, doubles the wait before
# the next, from one minute up to thirty. A session still running after six
# hours is stopped, with every process in its group. At its start and before
# each turn, before it touches the clone, drops a task or claims one, the loop
# waits until `session.pl -q` says that no session of it may still run, as
# `wait_session` in lib.sh says: a session of the loop before it that still
# runs, as one does after that loop's tmux session is killed, or one whose
# supervisor was killed, until its boot has passed or a person has removed
# its record; and `refresh` changes nothing in the clone until then, whoever
# calls it.
#
# While it holds no task, before it claims one, the loop removes what
# finished work left in its clone, as `tidy` in lib.sh says, and prunes its
# sessions' output, as `prune_logs` says.

REPO=toksaitov/rach2
REASONING=high
CODEX_MODEL=gpt-6.1-sol
DOC_BYTES=65536
DEADLINE=21600

# claim: take a task for this account, as the header says, and write its
# number to STATE/issue. Write nothing when none is waiting, and fail, taking
# nothing, when the lock cannot be taken or the board cannot be read. The lock
# is perl's flock on `build/flow/claim.lock` of the clone's main working tree,
# which the loops of its worktrees share and which the redirection after the
# body opens on descriptor 9 for the length of the call. A flock belongs to
# the open file description, not to the process that took it, so the lock is
# held until the descriptor is closed: by the shell when the call ends, or by
# the kernel when the loop dies.
claim()
{
    if ! perl -MFcntl=:flock -e 'open(L, ">&=9") && flock(L, LOCK_EX) or
            die "worker: claim lock: $!\n"' 2>> "$state/loop.log"; then
        log "claim lock failed"
        return 1
    fi
    # A task handed to this loop under the lock since its last one ended.
    [ -s "$state/issue" ] && return 0
    if ! listed=$(sh "$state/run/board.sh" next); then
        log "board unreachable"
        return 1
    fi
    tries=0
    for n in $(printf '%s\n' "$listed" | cut -f 1); do
        tries=$((tries + 1))
        [ "$tries" -le 5 ] || break
        if sh "$state/run/board.sh" take "$n" 2>> "$state/loop.log"; then
            echo "$n" > "$state/issue"
            log "took #$n"
            break
        fi
    done
} 9>> "$flowdir/claim.lock"

# choose: set `model` and `effort` for the next session: empty, for the
# agent's default model, and REASONING, unless STATE/model names others, as
# the header says. A line whose model or effort has a character outside
# letters, digits, `.`, `_` and `-`, or whose UNTIL is not a number, is
# ignored and logged.
choose()
{
    model='' effort=$REASONING
    [ -r "$state/model" ] || return 0
    read -r m e until < "$state/model"
    case "$m$e" in
        ''|*[!A-Za-z0-9._-]*)
            log "model: unusable line in $state/model"
            return 0 ;;
    esac
    case $until in
        *[!0-9]*)
            log "model: unusable line in $state/model"
            return 0 ;;
    esac
    if [ -n "$until" ] && [ "$(date +%s)" -ge "$until" ]; then
        rm -f "$state/model"
        log "model: $m ended; back to the default at $REASONING"
        return 0
    fi
    model=$m effort=${e:-$REASONING}
    log "model: $model at $effort"
}

# session ISSUE BRANCH: run one session on task ISSUE, whose milestone's
# branch is BRANCH, with the agent's command.
session()
{
    t=$(date -u '+%m%d-%H%M%S')
    prompt=$(sed "s|@ISSUE@|$1|g; s|@BRANCH@|$2|g; s|@BOARD@|$state/run/board.sh|g" \
        "$state/run/worker.txt")
    choose
    log "session $t on #$1 starts"
    started=$(date +%s)
    case $agent in
        claude)
            run_session "$state/s-$t.jsonl" "$state/s-$t.err" "$DEADLINE" \
                env CLAUDE_CODE_PRINT_BG_WAIT_CEILING_MS=0 \
                CLAUDE_CODE_EFFORT_LEVEL="$effort" \
                CLAUDE_CODE_AUTO_COMPACT_WINDOW=400000 \
                claude ${model:+--model "$model"} \
                -p "$prompt" --permission-mode bypassPermissions \
                --disallowedTools ScheduleWakeup,Monitor,Agent \
                --output-format stream-json --verbose ;;
        codex)
            run_session "$state/s-$t.jsonl" "$state/s-$t.err" "$DEADLINE" \
                codex exec -m "${model:-$CODEX_MODEL}" \
                -c model_reasoning_effort="$effort" \
                -c project_doc_max_bytes="$DOC_BYTES" \
                --dangerously-bypass-approvals-and-sandbox --json \
                -C "$repo" "$prompt" ;;
    esac
    status=$?
    took=$(($(date +%s) - started))
    log "session $t ended $status after ${took}s"
    last=$state/s-$t
}

# nap SECONDS: wait SECONDS, in steps of five, or until STATE/stop exists.
nap()
{
    np_left=$1
    while [ "$np_left" -gt 0 ] && [ ! -e "$state/stop" ]; do
        sleep 5
        np_left=$((np_left - 5))
    done
}

main()
{
    agent=claude state='' repo=
    while getopts a:s:r: o; do
        case $o in
            a) agent=$OPTARG ;;
            s) state=$OPTARG ;;
            r) repo=$OPTARG ;;
            *) echo "usage: worker.sh [-a claude|codex] [-s STATE]" >&2; exit 2 ;;
        esac
    done
    case $agent in
        claude|codex) ;;
        *) echo "worker: no agent named $agent" >&2; exit 2 ;;
    esac
    [ -n "$repo" ] || repo=$(cd "$(dirname "$0")/../.." && pwd)
    flowdir=$(primary "$repo")/build/flow || exit 1
    [ -n "$state" ] || state=$flowdir/$agent
    mkdir -p "$state" "$flowdir"
    state=$(cd "$state" && pwd)
    # The lock passes to master's copy on the exec below, with
    # RACH2_LOOP_LIVE naming the file it holds.
    if [ "${RACH2_LOOP_LIVE-}" != "$state/loop.live" ]; then
        exec 7>> "$state/loop.live"
        perl -MFcntl=:flock -e 'open(L, ">&=7") && flock(L, LOCK_EX | LOCK_NB) or exit 1' || {
            echo "worker: another loop holds $state/loop.live" >&2
            exit 1
        }
        RACH2_LOOP_LIVE=$state/loop.live
        export RACH2_LOOP_LIVE
    fi
    cd "$repo" || exit 1
    echo "$repo" > "$state/repo"
    echo "${RACH2_TESTING_COPY:-}" > "$state/copy"
    RACH2_MIN_FREE_GIB=${RACH2_MIN_FREE_GIB:-40}
    RACH2_WORKER_STATE=$state
    export RACH2_MIN_FREE_GIB RACH2_WORKER_STATE
    if ! wait_session; then
        log stopped
        exit 0
    fi
    git config core.hooksPath tools/git-hooks
    if [ "$0" != "$state/run/worker.sh" ]; then
        refresh
        while update worker.sh 2>> "$state/loop.log"; [ $? -eq 2 ]; do
            log "update failed"
            nap 60
            if [ -e "$state/stop" ]; then
                log stopped
                exit 0
            fi
        done
        log "loop starts: $agent in $repo"
        exec sh "$state/run/worker.sh" -a "$agent" -s "$state" -r "$repo"
    fi
    fails=0
    while [ ! -e "$state/stop" ]; do
        wait_session || break
        if ! refresh; then
            log "fetch failed"
            nap 60
            continue
        fi
        update worker.sh 2>> "$state/loop.log"
        case $? in
            0)
                log "restarting as master's loop"
                exec sh "$state/run/worker.sh" -a "$agent" -s "$state" -r "$repo" ;;
            2)
                log "update failed"
                nap 60
                continue ;;
        esac
        issue=$(cat "$state/issue" 2> /dev/null)
        if [ -n "$issue" ]; then
            if ! held=$(sh "$state/run/board.sh" mine "$issue"); then
                log "board unreachable"
                nap 60
                continue
            fi
            if [ "$held" != yes ]; then
                log "#$issue is closed or no longer this account's"
                rm -f "$state/issue"
                issue=
            fi
        fi
        if [ -z "$issue" ]; then
            until=$(cat "$state/limit" 2> /dev/null)
            if [ "$(date +%s)" -lt "${until:-0}" ]; then
                nap 300
                continue
            fi
            tidy
            prune_logs 's-*'
            [ -e "$state/stop" ] && break
            if ! claim; then
                nap 60
                continue
            fi
            issue=$(cat "$state/issue" 2> /dev/null)
        fi
        if [ -z "$issue" ]; then
            log idle
            nap 300
            continue
        fi
        branch=$(sh "$state/run/board.sh" branch "$issue" 2>> "$state/loop.log")
        case $branch in
            ''|*[!A-Za-z0-9._/-]*)
                log "#$issue: no usable milestone branch"
                nap 300
                continue ;;
        esac
        rm -f "$state/session.stop"
        [ -e "$state/stop" ] && break
        session "$issue" "$branch"
        if why=$(limited "$last.jsonl" "$last.err"); then
            log "limit: $why"
            limit_end "$why" > "$state/limit"
            nap 900
            continue
        fi
        rm -f "$state/limit"
        if [ "$took" -lt 120 ] && [ "$(gh issue view "$issue" --repo "$REPO" \
            --json state --jq .state)" != CLOSED ]; then
            fails=$((fails + 1))
            wait=60
            i=1
            while [ "$i" -lt "$fails" ] && [ "$wait" -lt 1800 ]; do
                wait=$((wait * 2))
                i=$((i + 1))
            done
            [ "$wait" -le 1800 ] || wait=1800
            log "short session; waiting ${wait}s"
            nap "$wait"
            continue
        fi
        fails=0
        nap 20
    done
    log stopped
    exit 0
}

. "$(dirname "$0")/lib.sh"
main "$@"
