#!/bin/sh
# Run through machine_load.sh's SSH stdin, once for each independent window.
set -eu
session=$1
seconds=$2
directory=$3
cp -r /src/rach2 "$directory"
cd "$directory"

# /proc/uptime counts delivered ticks. These are not host wall-clock seconds.
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
echo 'load-session: clock: delivered ticks can be lost; each session has its own window'
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
