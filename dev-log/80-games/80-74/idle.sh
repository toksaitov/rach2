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
# show FILE [MAX [irq]]: print FILE, only its first MAX lines when MAX is not
# 0, and with irq only the lines that hold "hda" or begin with "48:" after
# blanks. It uses the shell's own read and printf, so that sleep is the only
# program started between the two reads of /proc/stat.
show() {
    n=0
    while IFS= read -r l || [ -n "$l" ]; do
        if [ "${3:-}" = irq ]; then
            case "${l#"${l%%[! ]*}"}" in
                *hda* | 48:*) ;;
                *) continue ;;
            esac
        fi
        printf '%s\n' "$l"
        n=$((n + 1))
        [ "${2:-0}" -eq 0 ] || [ "$n" -lt "$2" ] || break
    done < "$1"
}
# The /proc counters the ten seconds are judged by.
counters() {
    echo "idle: sample $1"
    show /proc/uptime
    for f in /proc/"$pa"/task/*/stat /proc/"$khda"/task/*/stat; do
        echo "idle: $f"
        show "$f"
    done
    echo "idle: khda schedstat"
    show /proc/"$khda"/schedstat
    show /proc/interrupts 0 irq
    show /proc/stat 5
    show /proc/audio
}
# The listings come before the first counters and after the second, so that
# no pactl client of this script runs inside the window.
listings before
counters before
sleep 10
counters after
listings after
