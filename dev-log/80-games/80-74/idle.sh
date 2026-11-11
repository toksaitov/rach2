#!/bin/sh
# Read the desktop's idle audio counters over ten seconds; keep the output.
set -eu
pa=
khda=
for d in /proc/[0-9]*; do
    c=$(cat "$d/comm" 2>/dev/null) || continue
    case "$c" in
        pulseaudio) pa=${d#/proc/} ;;
        khda) khda=${d#/proc/} ;;
    esac
done
[ -n "$pa" ] && [ -n "$khda" ] || { echo 'idle: missing pulseaudio or khda' >&2; exit 1; }
echo "idle: pulseaudio $pa khda $khda ticks $(getconf CLK_TCK)"
pulse() {
    su -s /bin/sh desktop -c "XDG_RUNTIME_DIR=/run/user/1001 PULSE_SERVER=unix:/run/user/1001/pulse/native pactl list short $1"
}
pulse modules
pulse sink-inputs
pulse source-outputs
# The sink and source states, each a pactl client of PulseAudio.
listings() {
    echo "idle: listings $1"
    pulse sinks
    pulse sources
}
# The /proc counters the ten seconds are judged by.
counters() {
    echo "idle: sample $1"
    cat /proc/uptime
    for f in /proc/"$pa"/task/*/stat /proc/"$khda"/task/*/stat; do
        echo "idle: $f"
        cat "$f"
    done
    echo "idle: khda schedstat"
    cat /proc/"$khda"/schedstat
    grep -E 'hda|^ *48:' /proc/interrupts
    head -n 5 /proc/stat
    cat /proc/audio
}
# The listings come before the first counters and after the second, so that
# no pactl client of this script runs inside the window.
listings before
counters before
sleep 10
counters after
listings after
