# drive.sh: type at a machine's console when it is ready, not by the clock.
# shellcheck shell=sh
#
# A suite that boots QEMU itself, `-serial stdio < FIFO > LOG`, sources this
# file and drives the machine with `drive_expect` and `drive_type`: a line is
# typed once the output it answers has arrived, and a step that waits does so
# under a bound and fails with the log's tail when the bound passes. Every
# wait is for fresh output: a cursor, the byte position in the log of the
# last match or the last typed line, is where the next search starts, so a
# token the machine printed earlier, or the console's echo of a line just
# typed, never satisfies a later step.
#
# The feeder holds the fifo open while the machine lives and types the lines
# dropped into spool files, one per line and numbered, taken in order and
# by rename so a line being written is never read half and two typed back
# to back are both typed. The machine is started first and the feeder
# second: opening a fifo for reading waits for a writer, so QEMU sits in
# that open until the feeder arrives and neither side misses a line. The
# cursor starts at zero with each feeder, which is each boot.
#
#     DRIVE_LOG=$LOG; DRIVE_FIFO=$FEED
#     drive_feed "$QPID"
#     drive_expect 'rach2 login:' 90 || fail "no login prompt"
#     drive_type root
#     drive_expect 'Password:' 20 || fail "no password prompt"
#     drive_type "$PASSWORD"
#     drive_prompt 30 || fail "no shell prompt"
#     drive_type 'id; echo MARK""ER'
#     drive_expect 'MARKER' 30 || fail "the command did not answer"
#
# `drive_login NAME PASSWORD SECS` is that exchange in one call.

DRIVE_LOG=${DRIVE_LOG:-}
DRIVE_FIFO=${DRIVE_FIFO:-}
DRIVE_POS=0
DRIVE_FPID=""
DRIVE_SEQ=0
DRIVE_POLL=${DRIVE_POLL:-0.2}
# Every bound is elapsed seconds. Callers apply `scaled` once, if wanted;
# the driver never applies a factor, including in its compatibility aliases.
# shellcheck source=tests/system/deadline.sh
. "$REPO/tests/system/deadline.sh"

# The runner supplies a monotonic clock. Direct suite invocations can use
# the epoch format provided by both BSD date on macOS and GNU date on Linux.
drive_now() {
    if [ -n "${RACH2_RUN_UTIL:-}" ]; then "$RACH2_RUN_UTIL" now; else date +%s; fi
}

# The log's size in bytes, 0 before it exists.
drive_size() {
    if [ -f "$DRIVE_LOG" ]; then
        wc -c < "$DRIVE_LOG" | tr -d ' '
    else
        echo 0
    fi
}

# Hold the fifo open while process $1 lives, typing the spool files in
# their order. Runs in the background; `drive_stop` ends it.
drive_feed() {
    rm -f "$DRIVE_FIFO".type.*
    DRIVE_POS=0
    DRIVE_SEQ=0
    (
        while kill -0 "$1" 2>/dev/null; do
            for f in "$DRIVE_FIFO".type.[0-9]*; do
                [ -f "$f" ] || continue
                mv "$f" "$DRIVE_FIFO.taken" 2>/dev/null || continue
                cat "$DRIVE_FIFO.taken"
                rm -f "$DRIVE_FIFO.taken"
            done
            sleep "$DRIVE_POLL"
        done
    ) > "$DRIVE_FIFO" &
    DRIVE_FPID=$!
}

# Safe under `set -e` and when no feeder was started, which is how the
# suites' cleanup traps call it.
drive_stop() {
    if [ -n "$DRIVE_FPID" ]; then
        kill "$DRIVE_FPID" 2>/dev/null || true
        wait "$DRIVE_FPID" 2>/dev/null || true
        DRIVE_FPID=""
    fi
    [ -n "$DRIVE_FIFO" ] && rm -f "$DRIVE_FIFO".type.* "$DRIVE_FIFO.taken"
    return 0
}

# Type one line. The cursor moves to the log's end first, before the line is
# published, so nothing printed before it was typed, the prompt it answers
# included, can satisfy the step that waits for its answer, and an answer
# that comes at once is not skipped. A feeder that is gone is a failure
# here, not a silent line.
drive_type() {
    if [ -z "$DRIVE_FPID" ] || ! kill -0 "$DRIVE_FPID" 2>/dev/null; then
        echo "drive: the feeder is gone; nothing types at the machine" >&2
        return 1
    fi
    DRIVE_POS=$(drive_size)
    DRIVE_SEQ=$((DRIVE_SEQ + 1))
    printf '%s\n' "$1" > "$DRIVE_FIFO.type.new"
    mv "$DRIVE_FIFO.type.new" "$(printf '%s.type.%06d' "$DRIVE_FIFO" "$DRIVE_SEQ")"
}

# Wait up to $2 seconds for the extended regular expression $1 in the log
# after the cursor; on a match the cursor moves past it. The log is searched
# as it is, carriage returns included, so a pattern is one line's text and
# the offsets are the log's own. Fails past the bound with the fresh part's
# tail on standard error.
drive_expect() {
    drive_expect_raw "$1" "$2"
}

drive_expect_raw() {
    deadline=$(( $(drive_now) + $2 ))
    while :; do
        found=$(tail -c +"$((DRIVE_POS + 1))" "$DRIVE_LOG" 2>/dev/null |
            DRIVE_PATTERN="$1" LC_ALL=C awk '
                BEGIN { pattern = ENVIRON["DRIVE_PATTERN"] }
                match($0, pattern) { print offset + RSTART - 1, RLENGTH; exit }
                { offset += length($0) + 1 }
            ')
        if [ -n "$found" ]; then
            offset=${found%% *}
            length=${found#* }
            DRIVE_POS=$((DRIVE_POS + offset + length))
            return 0
        fi
        if [ "$(drive_now)" -ge "$deadline" ]; then
            echo "drive: no '$1' within $2 s; the console's last lines:" >&2
            tail -c +"$((DRIVE_POS + 1))" "$DRIVE_LOG" 2>/dev/null | tr -d '\r' | tail -5 >&2
            return 1
        fi
        sleep "$DRIVE_POLL"
    done
}

# A prompt begins a line, optionally after an absolute or home directory.
# A daemon can append output after the prompt without making it disappear.
# Advance only past the prompt, preserving any following output for a wait.
drive_prompt() {
    drive_expect_raw '^((/|~)[^ ]* )?[#$>] ' "$1"
}

drive_prompt_raw() {
    drive_prompt "$1"
}

# Log in as $1 with password $2, each step within $3 seconds, to a prompt.
drive_login() {
    drive_expect 'login:' "$3" || return 1
    drive_type "$1"
    drive_expect 'Password:' "$3" || return 1
    drive_type "$2"
    drive_prompt "$3"
}
