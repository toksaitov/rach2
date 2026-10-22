#!/bin/sh
#
# manager.sh: run the manager's check, then the next twenty minutes after it
# ends, until STATE/stop exists.
#
# Usage: manager.sh [-s STATE]
#
# A check is one headless Claude Code session in the clone this script is in,
# with the prompt `manager.txt` followed by the last ten lines of the
# manager's log, so that each check knows what the ones before it found and
# started. The loop starts one check at a time and counts the twenty minutes
# from its end, and a loop that starts while a check of the loop before it
# still runs waits for that check to end, so two checks never overlap. A check
# still running after forty minutes, which leaves room for the ten minutes an
# incident waits, is stopped with every process in its group, and the log
# says so.
#
# STATE, `~/flow/manager` unless given, holds the log `loop.log`, the
# manager's log `manager.log`, with the time and the last line of each check,
# each check's output as `c-TIME.jsonl` and `c-TIME.err`, the process group of
# the check that is running in `session`, and `last`, the time of the last
# check that ended by itself. `touch STATE/stop` ends the loop after the check
# that is running. The checks run at high effort, as a worker's sessions do.
#
# Before each check the loop prunes the checks' output, as `prune_logs` in
# lib.sh says, and runs `scratch.pl` on the testing host with the stems of
# master's scratch names, which removes from its /tmp the scratch of tests
# that were killed before their traps ran. The sweep is stopped after ten
# minutes, and its result is one line of loop.log. It then runs
# `leftovers.pl`, through `leftovers` in lib.sh, on the testing host and
# here, which removes the failed tests' directories, loose files in the home
# directories and copies of the repository that finished work left; loop.log
# has a line for each entry it removes and one for each run's total.

PERIOD=1200
TESTING=testing
DEADLINE=2400

main()
{
    state=$HOME/flow/manager repo=
    while getopts s:r: o; do
        case $o in
            s) state=$OPTARG ;;
            r) repo=$OPTARG ;;
            *) echo "usage: manager.sh [-s STATE]" >&2; exit 2 ;;
        esac
    done
    [ -n "$repo" ] || repo=$(cd "$(dirname "$0")/../.." && pwd)
    mkdir -p "$state"
    state=$(cd "$state" && pwd)
    cd "$repo" || exit 1
    echo "$repo" > "$state/repo"
    wait_session
    if [ "$0" != "$state/run/manager.sh" ]; then
        refresh
        while update manager.sh 2>> "$state/loop.log"; [ $? -eq 2 ]; do
            log "update failed"
            sleep 60
        done
        log "loop starts in $repo"
        exec sh "$state/run/manager.sh" -s "$state" -r "$repo"
    fi
    while [ ! -e "$state/stop" ]; do
        if ! refresh; then
            log "fetch failed"
            sleep 60
            continue
        fi
        update manager.sh 2>> "$state/loop.log"
        case $? in
            0)
                log "restarting as master's loop"
                exec sh "$state/run/manager.sh" -s "$state" -r "$repo" ;;
            2)
                log "update failed"
                sleep 60
                continue ;;
        esac
        prune_logs 'c-*'
        swept=$(perl -e 'alarm shift; exec @ARGV' 600 \
            ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" \
            "MSYS_NO_PATHCONV=1 wsl.exe -u root -- perl - /tmp 86400 $(stems origin/master | tr '\n' ' ')" \
            < "$state/run/scratch.pl" 2>&1)
        swept_rc=$?
        swept=$(printf '%s\n' "$swept" | tail -n 1)
        [ "$swept_rc" -eq 0 ] || swept="scratch: failed on $TESTING: $swept"
        log "$swept"
        leftovers 2>&1 | grep -v ': leftovers: keeps ' | while IFS= read -r line; do
            log "$line"
        done
        t=$(date -u '+%m%d-%H%M%S')
        prompt="$(cat "$state/run/manager.txt")

Your persistent progress record is $state/progress. Read it before checking the
phase, and replace it before ending, as manager.txt says.

The manager's log ends:
$(tail -n 10 "$state/manager.log" 2> /dev/null)"
        log "check $t starts"
        run_session "$state/c-$t.jsonl" "$state/c-$t.err" "$DEADLINE" \
            env CLAUDE_CODE_EFFORT_LEVEL=high CLAUDE_CODE_AUTO_COMPACT_WINDOW=400000 \
            claude -p "$prompt" --permission-mode bypassPermissions \
            --disallowedTools ScheduleWakeup --output-format stream-json --verbose
        status=$?
        log "check $t ended $status"
        line=$(jq -r 'select(.type == "result") | .result' "$state/c-$t.jsonl" 2> /dev/null |
            awk 'NF { l = $0 } END { print l }')
        case $status in
            0) date -u '+%F %T' > "$state/last" ;;
            124) line="check stopped after $DEADLINE seconds: $line" ;;
            *) line="check failed with status $status: $line" ;;
        esac
        echo "$(date -u '+%F %T') $line" >> "$state/manager.log"
        if why=$(limited "$state/c-$t.jsonl" "$state/c-$t.err"); then
            log "limit: $why"
        fi
        sleep "$PERIOD"
    done
    log stopped
    exit 0
}

. "$(dirname "$0")/lib.sh"
main "$@"
