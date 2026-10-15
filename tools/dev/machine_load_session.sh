#!/bin/sh
#
# machine_load_session.sh: one session of machine_load.sh, which the machine's
# shell reads from SSH's stdin, once for each session's own time window.
#
# Usage: machine_load_session.sh SESSION SECONDS DIRECTORY
#
# The session copies /src/rach2 to DIRECTORY and builds it with guest.mk
# until SECONDS of guest time have passed since it started, then prints
# `SESSION N builds B failed F before-deadline W late L` for machine_load.sh;
# a build that finishes after the deadline counts as late.
set -eu
session=$1
seconds=$2
directory=$3
cp -r /src/rach2 "$directory"
cd "$directory"

# Print the whole seconds of /proc/uptime, or exit 1 when it does not read as
# a number. /proc/uptime counts the boot processor's 10 ms ticks, and the
# ticks missed while interrupts were off only on a processor with an invariant
# time-stamp counter. These are not host wall-clock seconds.
now()
{
    value=$(cut -d. -f1 /proc/uptime) || exit 1
    case "$value" in
        ''|*[!0-9]*) echo 'load-session: invalid uptime' >&2; exit 1 ;;
    esac
    printf '%s\n' "$value"
}

start=$(now)
end=$((start + seconds))
built=0
failed=0
within=0
late=0
attempt=0
previous=$start
printf 'load-session: window: %s start %s deadline %s clock guest-tick-seconds\n' \
    "$session" "$start" "$end"
echo 'load-session: clock: missed ticks are lost without an invariant TSC; each session has its own window'
while :; do
    began=$(now)
    [ "$began" -ge "$previous" ] || exit 1
    [ "$began" -lt "$end" ] || break
    attempt=$((attempt + 1))
    rc=0
    make -f guest.mk clean && make -f guest.mk || rc=$?
    finished=$(now)
    [ "$finished" -ge "$began" ] || exit 1
    previous=$finished
    printf 'load-session: build: %s start %s finish %s status %s clock guest-tick-seconds\n' \
        "$attempt" "$began" "$finished" "$rc"
    if [ "$rc" -ne 0 ]; then
        failed=$((failed + 1))
    else
        built=$((built + 1))
        if [ "$finished" -lt "$end" ]; then
            within=$((within + 1))
        else
            late=$((late + 1))
        fi
    fi
done
printf 'SESSION %s builds %s failed %s before-deadline %s late %s\n' \
    "$session" "$built" "$failed" "$within" "$late"
