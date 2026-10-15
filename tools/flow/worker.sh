#!/bin/sh
#
# worker.sh: run one worker session after another until STATE/stop exists.
#
# Usage: worker.sh [-a claude|codex] [-s STATE]
#
# The loop works in the clone this script is in, as the account `gh` and git
# are signed in to there. Each session works on one issue, which the loop, not
# the session, claims: under a lock shared by the loops of this user on this
# machine, it takes the first issue `board.sh next` lists that `board.sh take`
# gives this account, and writes its number to STATE/issue. While that issue
# is open and assigned to this account, each new session resumes it. The
# number is kept in a file, not read from the board, because two loops of one
# account, such as the owner's Claude Code and Codex loops, cannot tell their
# issues apart by assignee. Write a number to STATE/issue to give a loop an
# issue to resume.
#
# AGENT is the program a session runs: Claude Code, `claude`, the default, or
# the Codex CLI, `codex`, at the model and effort below. A Claude Code session
# waits for its helpers however long they run, which print mode does not do
# unless CLAUDE_CODE_PRINT_BG_WAIT_CEILING_MS is 0, and cannot schedule a
# wakeup, since print mode keeps a finished session open until a pending
# wakeup fires. STATE, `~/flow` unless given, holds the issue file, the log
# `loop.log`, each session's output as `s-TIME.jsonl` and `s-TIME.err`, the
# clone's path in `repo`, and in `copy` the copy on the testing host that
# RACH2_TESTING_COPY names for testing.sh, if any; `touch STATE/stop` ends
# the loop after the session that is running.
#
# A session that ends on the account's usage limit makes the loop wait fifteen
# minutes. One shorter than two minutes for another reason, such as a lost
# network or a lapsed login, doubles the wait before the next, from one minute
# up to thirty. A session still running after six hours is stopped, with
# every process in its group; a long suite is not a stuck session, and the
# manager stops one that is stuck sooner. After three sessions on one issue
# that leave it open, the loop waits thirty minutes before each further one
# and says so in its log, for the manager to see.

REASONING=high
CODEX_MODEL=gpt-6.1-sol
DEADLINE=21600

# claim: take an issue for this account, as the header says, and write its
# number to STATE/issue; write nothing when none is waiting.
claim()
{
    lock=$HOME/.rach2-flow.lock
    until mkdir "$lock" 2> /dev/null; do
        holder=$(cat "$lock/pid" 2> /dev/null)
        if [ -n "$holder" ] && ! kill -0 "$holder" 2> /dev/null; then
            rm -rf "$lock"
        else
            sleep 5
        fi
    done
    echo $$ > "$lock/pid"
    for n in $(sh "$state/run/board.sh" next | cut -f 1 | head -n 5); do
        if sh "$state/run/board.sh" take "$n" 2>> "$state/loop.log"; then
            echo "$n" > "$state/issue"
            log "took #$n"
            break
        fi
    done
    rm -rf "$lock"
}

# session ISSUE: run one session on ISSUE with the agent's command.
session()
{
    t=$(date -u '+%m%d-%H%M%S')
    prompt=$(sed "s/@ISSUE@/$1/g" "$state/run/worker.txt")
    log "session $t on #$1 starts"
    started=$(date +%s)
    case $agent in
        claude)
            run_session "$state/s-$t.jsonl" "$state/s-$t.err" "$DEADLINE" \
                env CLAUDE_CODE_PRINT_BG_WAIT_CEILING_MS=0 \
                CLAUDE_CODE_EFFORT_LEVEL="$REASONING" \
                CLAUDE_CODE_AUTO_COMPACT_WINDOW=400000 \
                claude -p "$prompt" --permission-mode bypassPermissions \
                --disallowedTools ScheduleWakeup \
                --output-format stream-json --verbose ;;
        codex)
            run_session "$state/s-$t.jsonl" "$state/s-$t.err" "$DEADLINE" \
                codex exec -m "$CODEX_MODEL" \
                -c model_reasoning_effort="$REASONING" \
                --dangerously-bypass-approvals-and-sandbox --json \
                -C "$repo" "$prompt" ;;
    esac
    status=$?
    took=$(($(date +%s) - started))
    log "session $t ended $status after ${took}s"
    last=$state/s-$t
}

main()
{
    agent=claude state=$HOME/flow repo=
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
    mkdir -p "$state"
    state=$(cd "$state" && pwd)
    cd "$repo" || exit 1
    echo "$repo" > "$state/repo"
    echo "${RACH2_TESTING_COPY:-}" > "$state/copy"
    git config core.hooksPath tools/git-hooks
    if [ "$0" != "$state/run/worker.sh" ]; then
        refresh
        update worker.sh
        log "loop starts: $agent in $repo"
        exec sh "$state/run/worker.sh" -a "$agent" -s "$state" -r "$repo"
    fi
    fails=0 runs=0 ran=
    while [ ! -e "$state/stop" ]; do
        if ! refresh; then
            log "fetch failed"
            sleep 60
            continue
        fi
        if update worker.sh; then
            log "restarting as master's loop"
            exec sh "$state/run/worker.sh" -a "$agent" -s "$state" -r "$repo"
        fi
        issue=$(cat "$state/issue" 2> /dev/null)
        if [ -n "$issue" ]; then
            if ! held=$(sh "$state/run/board.sh" mine "$issue"); then
                log "board unreachable"
                sleep 60
                continue
            fi
            if [ "$held" != yes ]; then
                log "#$issue is closed or no longer this account's"
                rm -f "$state/issue"
                issue=
            fi
        fi
        if [ -z "$issue" ]; then
            claim
            issue=$(cat "$state/issue" 2> /dev/null)
        fi
        if [ -z "$issue" ]; then
            log idle
            sleep 300
            continue
        fi
        [ "$issue" = "$ran" ] || runs=0
        ran=$issue
        session "$issue"
        if tail -n 40 "$last.jsonl" "$last.err" | grep -q 'hit your session limit\|hit your usage limit'; then
            log "limit: $(grep -o 'resets [^"]*' "$last.jsonl" | head -n 1)"
            sleep 900
            continue
        fi
        if [ "$took" -lt 120 ]; then
            fails=$((fails + 1))
            wait=60
            i=1
            while [ "$i" -lt "$fails" ] && [ "$wait" -lt 1800 ]; do
                wait=$((wait * 2))
                i=$((i + 1))
            done
            [ "$wait" -le 1800 ] || wait=1800
            log "short session; waiting ${wait}s"
            sleep "$wait"
            continue
        fi
        fails=0
        runs=$((runs + 1))
        if [ "$runs" -ge 3 ] && [ "$(sh "$state/run/board.sh" mine "$issue")" = yes ]; then
            log "#$issue still open after $runs sessions; waiting 30 minutes"
            sleep 1800
        fi
        sleep 20
    done
    log stopped
    exit 0
}

. "$(dirname "$0")/lib.sh"
main "$@"
