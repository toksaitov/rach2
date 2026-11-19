#!/bin/sh
# Read the Lenovo's existing eDP scanout without opening a DRM card.
# Run on Rach2 as root, with debugfs mounted and the panel gate off.
# Haswell may mode set pipe A to route it through the fitter at open and
# close. Capture scanout.sh and dmesg before opening this reader.
# The argument is the frame count. Output includes each distinct CRC and
# the number of CRC changes, with the raw readings before the summary.
set -eu

frames=${1:-3600}
case $frames in
    ''|*[!0-9]*|0) exit 2 ;;
esac

gpu=/sys/bus/pci/devices/0000:00:02.0
backlight=/sys/class/backlight/intel_backlight
[ "$(basename "$(readlink "$gpu/driver")")" = i915 ]
card=''
for path in "$gpu"/drm/card*; do
    if [ -d "$path" ]; then
        card=${path##*/}
        break
    fi
done

# Read the gate, since actual_brightness can show a duty with the gate off.
gate()
{
    intel_reg --pci-slot 0000:00:02.0 read 0xc7204 | awk 'NR == 1 { print $NF }'
}

control=$(gate)
[ -n "$card" ] && [ -n "$control" ] && [ "$((control & 4))" -eq 0 ] || {
    echo 'console-crc: i915 card missing or backlight lit' >&2
    exit 1
}

debug=/sys/kernel/debug/dri/${card#card}
# The Lenovo's internal panel is on pipe A in the console's configuration.
crc=$debug/crtc-0/crc
tmp=$(mktemp -d /dev/shm/console-crc.XXXXXX)
armed=no
cleanup()
{
    if [ "$armed" = yes ]; then
        printf 'none\n' > "$crc/control" || :
    fi
    for file in "$tmp/frames" "$tmp/text"; do
        if [ -e "$file" ]; then unlink "$file"; fi
    done
    rmdir "$tmp"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

printf 'console-crc: %s: uptime %s\n' "$card" "$(cat /proc/uptime)"
for path in /sys/class/drm/"$card"-*; do
    printf 'console-crc: %s: %s\n' "${path##*/}" "$(cat "$path/status")"
done
if [ -r "$debug/state" ]; then
    cat "$debug/state"
fi

printf 'auto\n' > "$crc/control"
armed=yes
status=0
# Allow a stalled CRC read to return its partial capture.
timeout 75 head -n "$frames" "$crc/data" > "$tmp/frames" || status=$?
tr -d '\000' < "$tmp/frames" > "$tmp/text"
cat "$tmp/text"
awk -v want="$frames" '
    NF >= 2 {
        value = $2
        for (i = 3; i <= NF; i++)
            value = value " " $i
        if (n && value != previous) {
            changes++
        }
        previous = value
        counts[value]++
        n++
    }
    END {
        for (value in counts)
            printf "console-crc: value %s: %d frames\n", value, counts[value]
        printf "console-crc: frames %d, changes %d\n", n, changes
        exit n != want
    }
' "$tmp/text" || [ "$status" -ne 0 ] || status=1
control=$(gate)
printf 'console-crc: uptime %s, reader status %s, pwm level %s, panel control %s\n' \
    "$(cat /proc/uptime)" "$status" "$(cat "$backlight/actual_brightness")" "$control"
[ -n "$control" ] && [ "$((control & 4))" -eq 0 ] || exit 1
exit "$status"
