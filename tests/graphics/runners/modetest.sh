#!/bin/sh
# modetest.sh: every mode of a card's connected connectors, set by libdrm's
# modetest, read back and checksummed by drmcapture, inside a graphics
# guest, Rach2's or Debian's, as root.
#
#   modetest.sh [-M DRIVER] [-D CARD] [-c CAPTURE] [-o OUT]
#
# Lists the connectors of the card of DRIVER with `modetest -M DRIVER -c` and,
# for each mode of each connected one in the order the kernel lists them,
# runs `modetest -s CONNECTOR:#INDEX`, which makes a dumb buffer at the mode's
# size, draws its test pattern there, adds it as a framebuffer, sets it on a
# CRTC of the connector and then waits for a line on its standard input.
# While it waits, CAPTURE (drmcapture) reads the frame of CARD
# (/dev/dri/card0) back through GETFB2 and a mapping of its buffer, until it
# finds a framebuffer on the CRTC other than the one there before; the line
# then ends modetest. A mode that modetest reports it cannot set, with a
# line `failed ...` on its standard error, is `failed`. modetest.sh sets
# no time limit of its own. modetest opens a card by its DRM driver's name,
# never by its path; DRIVER is by default the first of virtio_gpu,
# bochs-drm, simpledrm and vkms that modetest opens.
#
# OUT (.) gets the record, `connectors`, modetest's listing, and `modes`,
# one line a mode: `CONNECTOR INDEX NAME REFRESH set WxH CRC` or
# `CONNECTOR INDEX NAME REFRESH failed`, CONNECTOR being the connector's name,
# NAME and REFRESH the mode's as modetest lists them, WxH the size of the
# framebuffer read back and CRC drmcapture's CRC-32 of its pixels. It prints
# one line:
#
#   modetest: result: MODES modes on CONNECTORS connectors: set S failed F
#
# The status is 0 when the card was listed and every mode tried, and 1
# otherwise; a mode that fails to set is a result, which judge.py compares
# with the reference's.
set -eu

RUNNER_DIR=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=common.sh
. "$RUNNER_DIR/common.sh"

driver='' card=/dev/dri/card0 capture=drmcapture out=.
while getopts M:D:c:o: o; do
    case $o in
        M) driver=$OPTARG ;;
        D) card=$OPTARG ;;
        c) capture=$OPTARG ;;
        o) out=$OPTARG ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 0 ] || { echo "usage: modetest.sh [-M DRIVER] [-D CARD] [-c CAPTURE] [-o OUT]" >&2; exit 2; }
command -v modetest > /dev/null || { echo "modetest: no modetest" >&2; exit 1; }
[ "$(id -u)" = 0 ] || { echo "modetest: setting a mode takes the card's master, which needs root here" >&2; exit 1; }

if [ -z "$driver" ]; then
    for driver in virtio_gpu bochs-drm simpledrm vkms ''; do
        [ -z "$driver" ] || ! modetest -M "$driver" -c > /dev/null 2>&1 || break
    done
    [ -n "$driver" ] || { echo "modetest: modetest opens no card of virtio_gpu, bochs-drm, simpledrm or vkms" >&2; exit 1; }
fi

runner_init modetest "$out"
runner_start
runner_record "case modetest every mode of $driver, read back from $card"
runner_record "warmup 0"
runner_record "repetitions 1"
runner_record "modetest $(dpkg-query -W -f '${Version}' libdrm-tests 2>/dev/null || echo unknown)"
modetest -M "$driver" -c > "$out/connectors" 2>&1 || runner_die "modetest -c failed: $(tail -n 3 "$out/connectors")"

# The connected connectors' modes, `ID NAME INDEX MODE REFRESH` a line.
awk -F '\t' '
    /^[0-9]+\t/ { id = $1; status = $3; name = $4; sub(/ +$/, "", name); next }
    /^  #[0-9]+ / && status == "connected" {
        split($0, f, " ")
        print id, name, substr(f[1], 2), f[2], f[3]
    }
' "$out/connectors" > "$RUNNER_WORK/modes"
[ -s "$RUNNER_WORK/modes" ] || runner_die "no connected connector of $driver has a mode"
runner_record "modes $(wc -l < "$RUNNER_WORK/modes")"

# fb: the framebuffer that the first CRTC scanning one out shows, or none.
fb()
{
    "$capture" "$card" < /dev/null 2> /dev/null | awk '{ print $5 }' | grep . || echo none
}

fifo=$RUNNER_WORK/stdin
: > "$out/modes"
while read -r id name index mode refresh; do
    before=$(fb)
    rm -f "$fifo"
    mkfifo "$fifo"
    modetest -M "$driver" -s "$id:#$index" < "$fifo" > "$RUNNER_WORK/log" 2>&1 &
    pid=$!
    exec 3> "$fifo"
    line=
    while kill -0 "$pid" 2> /dev/null && ! grep -q '^failed' "$RUNNER_WORK/log"; do
        line=$("$capture" "$card" < /dev/null 2> /dev/null || true)
        # shellcheck disable=SC2086 # drmcapture's words
        set -- $line
        if [ $# -ge 9 ] && [ "$5" != "$before" ]; then
            break
        fi
        line=
        sleep 0.1
    done
    echo >&3
    exec 3>&-
    wait "$pid" 2> /dev/null || true
    sed "s/^/$name $index: /" "$RUNNER_WORK/log" >> "$out/modetest.log"
    if [ -n "$line" ]; then
        # shellcheck disable=SC2086 # drmcapture's words
        set -- $line
        echo "$name $index $mode $refresh set $6 $9"
    else
        echo "$name $index $mode $refresh failed"
    fi | tee -a "$out/modes" | sed 's/^/modetest: mode: /'

done < "$RUNNER_WORK/modes"
runner_finish

set=$(grep -c ' set ' "$out/modes" || true)
failed=$(grep -c ' failed$' "$out/modes" || true)
echo "modetest: result: $(wc -l < "$out/modes") modes on $(cut -d' ' -f1 "$RUNNER_WORK/modes" | sort -u | wc -l) connectors: set $set failed $failed" |
    tee -a "$out/results"
