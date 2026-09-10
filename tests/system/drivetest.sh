#!/usr/bin/env sh
#
# drivetest.sh: the drivers type when the machine is ready, not by the clock.
#
# Two fake machines stand in for QEMU, each a shell script that reads what is
# typed and writes a console: one behind a fifo and a log for `drive.sh`, one
# on PATH as `qemu-system-i386` for the harness. Each row chooses when the
# fake prints its prompt or its token, and the checks are about time: a
# machine ready later than a clock-typed feeder would have allowed still
# gets its line; one that never becomes ready fails the step within its
# bound; a token printed before the step began does not satisfy it; and the
# harness's `until=` ends when the run's token arrives while `wait=` does not.
#
# `tests/run drive` runs this. It boots nothing.
#
set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rach2-drive.XXXXXX")
[ -d "$WORK" ] || { echo "drivetest: no scratch directory" >&2; exit 1; }
HARNESS="$REPO/build/tests/system/harness"
PIDS=""
cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null; done
    rm -rf "$WORK"
}
trap 'cleanup' EXIT
trap 'cleanup; exit 130' INT
trap 'cleanup; exit 143' TERM
trap 'cleanup; exit 129' HUP

fails=0
total=0
check() {
    total=$((total + 1))
    if [ "$2" = "$3" ]; then
        echo "  PASS: $1"
    else
        echo "  FAIL: $1: expected [$2], got [$3]"
        fails=$((fails + 1))
    fi
}

# A console: after $1 seconds it prints a login prompt, then answers what is
# typed the way os-real does, echoing each line first. `MARKER` is printed
# once at the start, before anything was typed, for the stale-token row.
cat > "$WORK/console.sh" <<'EOF'
#!/bin/sh
delay=$1
printf 'MARKER\r\n'
sleep "$delay"
printf 'rach2 login: '
while IFS= read -r line; do
    case "$line" in
        'printf x > /tmp/a')
            # The echo pauses at the redirection, as a slow console can.
            printf 'printf x > '; sleep 2; printf '/tmp/a\r\n# ' ;;
        *) printf '%s\r\n' "$line" ;;
    esac
    case "$line" in
        'printf x > /tmp/a') ;;
        root) printf 'Password: ' ;;
        secret) printf '# ' ;;
        'echo MARK""ER') printf 'MARKER\r\n# ' ;;
        *) printf '# ' ;;
    esac
done
EOF
chmod +x "$WORK/console.sh"

# The harness's machine: it prints the debug shell's prompt at once and, when
# told to run a program, the run's token after $QEMU_TOKEN_AT seconds. It
# ignores its arguments, which are QEMU's.
mkdir -p "$WORK/bin"
cat > "$WORK/bin/qemu-system-i386" <<'EOF'
#!/bin/sh
printf "Type 'help' for available commands.\r\n> "
while IFS= read -r line; do
    printf '%s\r\n' "$line"
    case "$line" in
        exec*) sleep "${QEMU_TOKEN_AT:-0}"; printf 'FAKE OK\r\n> ' ;;
        *) printf '> ' ;;
    esac
done
sleep 30
EOF
chmod +x "$WORK/bin/qemu-system-i386"

# boot DELAY: the console behind its fifo and log, driven through drive.sh.
boot() {
    rm -f "$WORK/fifo" "$WORK/log"
    mkfifo "$WORK/fifo"
    "$WORK/console.sh" "$1" < "$WORK/fifo" > "$WORK/log" 2>&1 &
    CPID=$!
    PIDS="$PIDS $CPID"
    DRIVE_LOG="$WORK/log"
    DRIVE_FIFO="$WORK/fifo"
    drive_feed "$CPID"
    PIDS="$PIDS $DRIVE_FPID"
}
stop() {
    drive_stop
    kill "$CPID" 2>/dev/null
    wait "$CPID" 2>/dev/null
}

# shellcheck source=tests/system/drive.sh
. "$REPO/tests/system/drive.sh"

# A machine ready at 4 s, past a feeder that would have typed at 2, still
# logs in: the line is typed when the prompt has arrived.
boot 4
t0=$(date +%s)
drive_login root secret 20; rc=$?
took=$(( $(date +%s) - t0 ))
check "a machine ready later than a clock feeder would allow logs in" 0 "$rc"
check "and the login waited for the prompt" 1 "$([ "$took" -ge 4 ] && echo 1 || echo 0)"
drive_type 'echo MARK""ER'
drive_expect 'MARKER' 10; rc=$?
check "a marker printed for a typed line is found" 0 "$rc"
check "and the prompt printed with it in one write is still there for the next step" 0 "$(drive_prompt 5; echo $?)"
drive_type 'echo MARK""ER'
drive_type 'echo MARK""ER'
drive_expect 'MARKER' 10 && drive_expect 'MARKER' 10; rc=$?
check "two lines typed back to back are both typed, in order" 0 "$rc"
drive_prompt 5 > /dev/null 2>&1
t0=$(date +%s)
drive_type 'printf x > /tmp/a'
drive_prompt 10; rc=$?
took=$(( $(date +%s) - t0 ))
check "a command's echo paused at a redirection is not taken for a prompt" 0 "$rc"
check "and the prompt after it is waited for" 1 "$([ "$took" -ge 2 ] && echo 1 || echo 0)"
stop

# A machine ready at once: the answer that comes before the step looks is
# not skipped, since the cursor was set before the line was published.
boot 0
drive_login root secret 20; rc=$?
check "a machine ready at once logs in" 0 "$rc"
stop

# A machine that never becomes ready fails the step within its bound.
boot 60
t0=$(date +%s)
drive_expect 'login:' 3 2>/dev/null; rc=$?
took=$(( $(date +%s) - t0 ))
check "a prompt that never comes fails the step" 1 "$rc"
check "within its bound" 1 "$([ "$took" -ge 3 ] && [ "$took" -le 5 ] && echo 1 || echo 0)"
stop

# A token the machine printed before the step began does not satisfy it: the
# console prints MARKER once at its start, and the search begins after the
# line typed next.
boot 1
drive_login root secret 20 > /dev/null 2>&1
drive_type 'true'
drive_expect 'MARKER' 2 2>/dev/null; rc=$?
check "a token printed before the step is not accepted" 1 "$rc"
drive_type 'echo MARK""ER'
drive_expect 'MARKER' 10; rc=$?
check "and the same token printed after it is" 0 "$rc"
stop

# The harness: `until=60` ends when the run's token arrives; `wait=4` does
# not; a token printed before the until began does not end it; and an until
# never outlives the run's timeout.
harness() {
    ( cd "$WORK" && PATH="$WORK/bin:$PATH" "$HARNESS" --kernel none --timeout 90 --grace 0.2 --ok "FAKE OK" "$@" > /dev/null 2>&1 )
}
t0=$(date +%s)
QEMU_TOKEN_AT=2 harness "expect=Type 'help'" "send=exec /fake" until=60; rc=$?
took=$(( $(date +%s) - t0 ))
check "an until ends when the run's token arrives" 0 "$rc"
check "well before its time" 1 "$([ "$took" -lt 8 ] && echo 1 || echo 0)"
t0=$(date +%s)
QEMU_TOKEN_AT=1 harness "expect=Type 'help'" "send=exec /fake" wait=4; rc=$?
took=$(( $(date +%s) - t0 ))
check "a wait runs its time whatever arrives" 1 "$([ "$took" -ge 4 ] && echo 1 || echo 0)"
t0=$(date +%s)
QEMU_TOKEN_AT=0 harness "expect=Type 'help'" "send=exec /fake" "expect=FAKE OK" until=4; rc=$?
took=$(( $(date +%s) - t0 ))
check "a token printed before the until began does not end it" 1 "$([ "$took" -ge 4 ] && echo 1 || echo 0)"
t0=$(date +%s)
( cd "$WORK" && PATH="$WORK/bin:$PATH" QEMU_TOKEN_AT=60 "$HARNESS" --kernel none --timeout 3 --grace 0.2 --ok "FAKE OK" "expect=Type 'help'" "send=exec /fake" until=60 > /dev/null 2>&1 ); rc=$?
took=$(( $(date +%s) - t0 ))
check "an until never runs past the run's timeout" 1 "$([ "$took" -le 6 ] && echo 1 || echo 0)"

echo "drivetest: $((total - fails)) of $total checks passed"
[ "$fails" -eq 0 ] || exit 1
