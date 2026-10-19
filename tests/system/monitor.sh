# monitor.sh: ask a running QEMU, through its monitor, whether it has halted.
# shellcheck shell=sh
#
# A machine that has stopped for good, after a halt or a refusal to boot,
# prints nothing more, so its console cannot show that it stopped; only the
# emulator can. A suite sources this file with RUNUTIL set to the runner
# helper, calls `monitor_open PATH` before it starts QEMU with
# `-monitor pipe:PATH`, and `monitor_close` when the machine is gone. The
# monitor is a pair of FIFOs, PATH.in and PATH.out, and a reader copies what
# the monitor says to PATH.log, which stays for a failed run to show.
#
#     monitor_open "$WORK.monitor"
#     "$QEMU" ... -monitor "pipe:$WORK.monitor" &
#     monitor_wait_halted 30 || fail "the machine did not stop: $MONITOR_WHY"
#
# A processor that `hlt` stopped with its interrupts off stays stopped, so once
# a look finds every processor so, nothing the machine could still print is
# left to wait for.

MONITOR_PATH=""
MONITOR_PID=""
# The looks at the monitor since `monitor_open`, each of which ends with one
# line `VM status:` in the monitor's log.
MONITOR_LOOKS=0
# Why the last look did not find the machine halted.
MONITOR_WHY=""
# The processors whose registers the last look read.
MONITOR_CPUS=0

# monitor_open PATH: make the FIFOs that `-monitor pipe:PATH` opens and start
# the reader of the monitor's output.
monitor_open() {
    MONITOR_PATH=$1
    MONITOR_LOOKS=0
    rm -f "$1.in" "$1.out" "$1.log"
    mkfifo "$1.in" "$1.out"
    cat "$1.out" > "$1.log" &
    MONITOR_PID=$!
}

# monitor_close: stop the reader and remove the FIFOs. It is safe to call
# twice, and when no monitor was opened.
monitor_close() {
    mpid=$MONITOR_PID
    MONITOR_PID=""
    if [ -n "$mpid" ]; then
        kill -9 "$mpid" 2>/dev/null || true
        wait "$mpid" 2>/dev/null || true
    fi
    if [ -n "$MONITOR_PATH" ]; then
        rm -f "$MONITOR_PATH.in" "$MONITOR_PATH.out" "$MONITOR_PATH.log.cpus"
    fi
}

# monitor_halted: ask the monitor for the registers of every processor and
# the machine's status. Return 0 when the machine is running and every
# processor is halted with its interrupts off; 1, with MONITOR_WHY set, when
# it is not; and 2 when the monitor does not answer within 30 seconds, which
# is what a QEMU that has ended or hung gives. The monitor's input is opened
# for reading and writing, so that the write does not wait for a reader when
# QEMU has ended.
monitor_halted() {
    MONITOR_LOOKS=$((MONITOR_LOOKS + 1))
    exec 7<> "$MONITOR_PATH.in"
    printf 'info registers -a\ninfo status\n' >&7
    exec 7>&-
    if ! "$RUNUTIL" until "$MONITOR_PATH.log" "VM status: " 30 "$MONITOR_LOOKS"; then
        MONITOR_WHY="the monitor did not answer, so QEMU ended or does not respond"
        return 2
    fi
    status=$(grep -a "VM status: " "$MONITOR_PATH.log" | sed -n "${MONITOR_LOOKS}p" | tr -d '\r')
    if [ "$status" != "VM status: running" ]; then
        MONITOR_WHY="the monitor says '$status', not that the machine is running"
        return 1
    fi
    # Each processor has one line with its flags, EFL or RFL, and HLT.
    awk -v n="$MONITOR_LOOKS" '/VM status: /{ seen++; next } seen == n - 1 && /FL=/' \
        "$MONITOR_PATH.log" |
        sed 's/.*FL=\([0-9a-f]*\) .* HLT=\([01]\).*/\1 \2/' > "$MONITOR_PATH.log.cpus"
    cpus=0
    MONITOR_WHY=""
    while read -r flags halted; do
        cpus=$((cpus + 1))
        case "$flags" in
            "" | *[!0-9a-f]*)
                MONITOR_WHY="the registers of processor $cpus are not readable: $flags $halted"
                break
                ;;
        esac
        if [ "$halted" != 1 ]; then
            MONITOR_WHY="processor $cpus of the machine is not halted"
            break
        fi
        if [ $((0x$flags & 0x200)) -ne 0 ]; then
            MONITOR_WHY="processor $cpus is halted with its interrupts on (flags $flags)"
            break
        fi
    done < "$MONITOR_PATH.log.cpus"
    rm -f "$MONITOR_PATH.log.cpus"
    # shellcheck disable=SC2034 # the suites that source this file read it
    MONITOR_CPUS=$cpus
    if [ -z "$MONITOR_WHY" ] && [ "$cpus" -eq 0 ]; then
        MONITOR_WHY="the monitor gave the registers of no processor"
    fi
    [ -z "$MONITOR_WHY" ]
}

# monitor_wait_halted SECS: look at the machine until it is halted, for at
# most SECS seconds. Return 0 once a look finds it halted, and 1, with
# MONITOR_WHY from the last look, when the time passes or the monitor stops
# answering.
monitor_wait_halted() {
    deadline=$(( $("$RUNUTIL" now) + $1 ))
    while :; do
        look=0
        monitor_halted || look=$?
        case $look in
            0) return 0 ;;
            2) return 1 ;;
        esac
        [ "$("$RUNUTIL" now)" -lt "$deadline" ] || return 1
        "$RUNUTIL" sleep 0.2
    done
}
