# drive.sh: type at a machine's console when it is ready, not by the clock.
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
    deadline=$(( $(date +%s) + $2 ))
    while :; do
        found=$(tail -c +"$((DRIVE_POS + 1))" "$DRIVE_LOG" 2>/dev/null | grep -a -b -o -E "$1" 2>/dev/null | head -1)
        if [ -n "$found" ]; then
            offset=${found%%:*}
            match=${found#*:}
            DRIVE_POS=$((DRIVE_POS + offset + ${#match}))
            return 0
        fi
        if [ "$(date +%s)" -ge "$deadline" ]; then
            echo "drive: no '$1' within $2 s; the console's last lines:" >&2
            tail -c +"$((DRIVE_POS + 1))" "$DRIVE_LOG" 2>/dev/null | tr -d '\r' | tail -5 >&2
            return 1
        fi
        sleep "$DRIVE_POLL"
    done
}

# Wait up to $1 seconds for a shell prompt to be the last thing printed
# after the cursor: root's `# `, a user's `$ `, or the debug shell's `> `,
# with at most the directory before it, on a line of its own. A typed
# command's echo that stops for a moment at a redirection, `printf x > `,
# ends the same way and is not a prompt; the word before the prompt's
# character is what tells them apart.
drive_prompt() {
    deadline=$(( $(date +%s) + $1 ))
    while :; do
        last=$(tail -c +"$((DRIVE_POS + 1))" "$DRIVE_LOG" 2>/dev/null | tr -d '\r' | tail -1)
        if printf '%s\n' "$last" | grep -q -E '^([^ ]* )?[#$>] ?$'; then
            DRIVE_POS=$(drive_size)
            return 0
        fi
        if [ "$(date +%s)" -ge "$deadline" ]; then
            echo "drive: no prompt within $1 s; the console's last lines:" >&2
            tail -c +"$((DRIVE_POS + 1))" "$DRIVE_LOG" 2>/dev/null | tr -d '\r' | tail -5 >&2
            return 1
        fi
        sleep "$DRIVE_POLL"
    done
}

# Log in as $1 with password $2, each step within $3 seconds, to a prompt.
drive_login() {
    drive_expect 'login:' "$3" || return 1
    drive_type "$1"
    drive_expect 'Password:' "$3" || return 1
    drive_type "$2"
    drive_prompt "$3"
}
