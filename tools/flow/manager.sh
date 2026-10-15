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
# from its end, so two checks never overlap. A check still running after
# forty minutes, which leaves room for the ten minutes an incident waits, is
# stopped with every process in its group, and the log says so.
#
# STATE, `~/flow/manager` unless given, holds the log `loop.log`, the
# manager's log `manager.log`, with the time and the last line of each check,
# each check's output as `c-TIME.jsonl` and `c-TIME.err`, and `last`, the time
# of the last check that ended by itself. `touch STATE/stop` ends the loop
# after the check that is running. The checks run at high effort, as a
# worker's sessions do.

PERIOD=1200
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
    if [ "$0" != "$state/run/manager.sh" ]; then
        refresh
        update manager.sh
        log "loop starts in $repo"
        exec sh "$state/run/manager.sh" -s "$state" -r "$repo"
    fi
    while [ ! -e "$state/stop" ]; do
        if ! refresh; then
            log "fetch failed"
            sleep 60
            continue
        fi
        if update manager.sh; then
            log "restarting as master's loop"
            exec sh "$state/run/manager.sh" -s "$state" -r "$repo"
        fi
        t=$(date -u '+%m%d-%H%M%S')
        prompt="$(cat "$state/run/manager.txt")

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
