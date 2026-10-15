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
# process group of the session that is running in `session`, the clone's path
# in `repo`, and in `copy` the copy on the testing host that
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
# and says so in its log, for the manager to see. A loop that starts while a
# session of the loop before it still runs, as one does after that loop's
# tmux session is killed, waits for the session to end before it does
# anything in the clone.
#
# While a `hold` issue is open, the loop starts no session on an issue whose
# work is committed on a branch named for it and waits only for the push,
# unless the issue is a `ci:` one, whose repair the hold lets through; it
# looks again every five minutes, and the next session after the hold pushes.

REPO=toksaitov/rach2
REASONING=high
CODEX_MODEL=gpt-6.1-sol
DEADLINE=21600

# claim: take an issue for this account, as the header says, and write its
# number to STATE/issue; write nothing when none is waiting, and fail, taking
# nothing, when the lock cannot be taken or the board cannot be read. The lock
# is perl's flock on ~/.rach2-claim.lock, which the redirection after the body
# opens on descriptor 9 for the length of the call. A flock belongs to the
# open file description, not to the process that took it, so the lock is held
# until the descriptor is closed: by the shell when the call ends, or by the
# kernel when the loop dies.
claim()
{
    if ! perl -MFcntl=:flock -e 'open(L, ">&=9") && flock(L, LOCK_EX) or
            die "worker: claim lock: $!\n"' 2>> "$state/loop.log"; then
        log "claim lock failed"
        return 1
    fi
    if ! listed=$(sh "$state/run/board.sh" next); then
        log "board unreachable"
        return 1
    fi
    for n in $(printf '%s\n' "$listed" | cut -f 1 | head -n 5); do
        if sh "$state/run/board.sh" take "$n" 2>> "$state/loop.log"; then
            echo "$n" > "$state/issue"
            log "took #$n"
            break
        fi
    done
} 9>> "$HOME/.rach2-claim.lock"

# waiting N: print the number of the open `hold` issue and succeed when the
# work on issue N is committed and only its push waits for that hold: a local
# branch named for N has commits that master lacks, and N is not a `ci:`
# issue, whose repair the hold lets through. Fail when nothing waits or
# GitHub cannot be reached, so the loop goes on to the session.
waiting()
{
    unpushed=
    for b in $(git for-each-ref --format='%(refname:short)' "refs/heads/$1-*"); do
        [ -z "$(git rev-list -n 1 "origin/master..$b" --)" ] || unpushed=$b
    done
    [ -n "$unpushed" ] || return 1
    hold=$(gh issue list --repo "$REPO" --label hold --state open \
        --json number --jq '.[0].number // empty') && [ -n "$hold" ] || return 1
    title=$(gh issue view "$1" --repo "$REPO" --json title --jq .title) || return 1
    case $title in
        ci:*) return 1 ;;
    esac
    echo "$hold"
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
    wait_session
    git config core.hooksPath tools/git-hooks
    if [ "$0" != "$state/run/worker.sh" ]; then
        refresh
        while update worker.sh 2>> "$state/loop.log"; [ $? -eq 2 ]; do
            log "update failed"
            sleep 60
        done
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
        update worker.sh 2>> "$state/loop.log"
        case $? in
            0)
                log "restarting as master's loop"
                exec sh "$state/run/worker.sh" -a "$agent" -s "$state" -r "$repo" ;;
            2)
                log "update failed"
                sleep 60
                continue ;;
        esac
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
            if ! claim; then
                sleep 60
                continue
            fi
            issue=$(cat "$state/issue" 2> /dev/null)
        fi
        if [ -z "$issue" ]; then
            log idle
            sleep 300
            continue
        fi
        if hold=$(waiting "$issue"); then
            log "#$issue is committed and waits for hold #$hold"
            sleep 300
            continue
        fi
        [ "$issue" = "$ran" ] || runs=0
        ran=$issue
        session "$issue"
        if why=$(limited "$last.jsonl" "$last.err"); then
            log "limit: $why"
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
