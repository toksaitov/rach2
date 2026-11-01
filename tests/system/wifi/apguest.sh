#!/bin/sh
#
# apguest.sh: start the Linux AP guest in QEMU, alone, as a peer of a suite,
# or beside a Rach2 machine whose relay card reaches its radios.
#
# Usage: apguest.sh [-a ACCEL] [-b BASE] [-p PROFILE] [-m MODE] [-o DIR]
#                   [-d | -R IMAGE]
#
#   -a ACCEL    QEMU's accelerator: RACH2_QEMU_ACCEL, or kvm where /dev/kvm
#               opens, whpx on Windows and tcg elsewhere
#   -b BASE     the first of the five host ports, 15550 by default:
#                 BASE    UDP, the AP guest's relay, its port 5550
#                 BASE+1  UDP, the Rach2 machine's relay, its port 5550
#                 BASE+2  TCP, the SSH server of the station on rach2-net
#                 BASE+3  TCP, the SSH server of the station on rach2-other
#                 BASE+4  TCP, the AP guest's console, with -d or -R
#   -p PROFILE  network A's security, as wifiap(8) of the guest lists: open,
#               tkip, wpa2 (the default), sae, transition, enterprise or
#               enterprise2
#   -m MODE     shell (the default), check or trace, as wifiap.run
#   -o DIR      with -m trace, write the traces into DIR: NAME.pcap,
#               NAME.pids and NAME.log for wpa2, sae and peap
#   -d          run as a suite's peer: the console goes to the port BASE+4
#               and to apguest.log in TMPDIR's scratch, the script prints
#               `apguest: ready: PROFILE` once the guest's access points
#               beacon, and it serves until SIGTERM, SIGINT or SIGHUP, or
#               until the guest ends
#   -R IMAGE    as -d, and then boot IMAGE (os-real) of ARCH, i686 by
#               default, with its console on the terminal and a second e1000
#               on 10.0.4.0/24 whose UDP port 5550 is the host's BASE+1. Its
#               relay sends to 10.0.4.2 port BASE, the AP guest's, which
#               sends to 10.0.3.2 port BASE+1. The guest stops when the
#               machine ends.
#
# Without -d or -R, the guest's console is the terminal, and the script ends
# when the guest powers off. The guest has two e1000 cards on QEMU's user
# network: eth0 on 10.0.2.0/24, whose host 10.0.2.2 is the stations' traffic
# endpoint and which forwards BASE+2 and BASE+3 to their SSH servers, and
# eth1 on 10.0.3.0/24, the relay's card. The guest is built by
# `make wifi-guest`; RACH2_QEMU names QEMU, qemu-system-x86_64 by default.
# The status is the guest's QEMU's, 0 when it powered off, or the Rach2
# machine's with -R; 1 when the guest did not become ready or a trace did not
# check out, and 2 for a wrong usage.

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../../.." && pwd)"
GUEST="$REPO/build/wifi/guest"
QEMU="${RACH2_QEMU:-qemu-system-x86_64}"
ACCEL="${RACH2_QEMU_ACCEL:-}"
BASE=15550
PROFILE=wpa2
MODE=shell
OUTDIR=
PEER=
IMAGE=

usage()
{
    echo "usage: apguest.sh [-a ACCEL] [-b BASE] [-p PROFILE] [-m MODE] [-o DIR] [-d | -R IMAGE]" >&2
    exit 2
}

while getopts a:b:p:m:o:dR: opt; do
    case $opt in
        a) ACCEL=$OPTARG ;;
        b) BASE=$OPTARG ;;
        p) PROFILE=$OPTARG ;;
        m) MODE=$OPTARG ;;
        o) OUTDIR=$OPTARG ;;
        d) PEER=1 ;;
        R) PEER=1; IMAGE=$OPTARG ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 0 ] || usage
case $BASE in
    '' | *[!0-9]*) usage ;;
esac
[ "$BASE" -ge 1024 ] && [ "$BASE" -le 65531 ] || usage
case $MODE in
    shell | check | trace) ;;
    *) usage ;;
esac
[ -z "$OUTDIR" ] || [ "$MODE" = trace ] || usage
[ -z "$PEER" ] || [ "$MODE" = shell ] || usage
if [ ! -f "$GUEST/vmlinuz" ] || [ ! -f "$GUEST/initrd" ]; then
    echo "apguest: no guest in $GUEST; build it with 'make wifi-guest'" >&2
    exit 1
fi
if [ -z "$ACCEL" ]; then
    case "$(uname -s)" in
        MINGW* | MSYS*) ACCEL=whpx ;;
        Linux) if [ -w /dev/kvm ]; then ACCEL=kvm; else ACCEL=tcg; fi ;;
        *) ACCEL=tcg ;;
    esac
fi

# native PATH: PATH as the native QEMU of Windows reads it.
native()
{
    if command -v cygpath > /dev/null 2>&1; then
        cygpath -m "$1"
    else
        echo "$1"
    fi
}

work=
guest=
trap 'stop' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

stop()
{
    status=$?
    if [ -n "$guest" ]; then
        kill "$guest" 2> /dev/null || true
        wait "$guest" 2> /dev/null || true
    fi
    [ -z "$work" ] || rm -rf "$work"
    exit "$status"
}

mkdir -p "${TMPDIR:-/tmp}"
work="$(mktemp -d "${TMPDIR:-/tmp}/rach2-apguest.XXXXXX")"
log="$work/apguest.log"

append="console=ttyS0 panic=-1 quiet wifiap.profile=$PROFILE wifiap.run=$MODE"
[ -z "$PEER" ] || append="$append wifiap.peer=$((BASE + 1))"
set -- -accel "$ACCEL" -cpu max -m 512 -smp 2 -no-reboot -display none \
    -kernel "$(native "$GUEST/vmlinuz")" -initrd "$(native "$GUEST/initrd")" \
    -append "$append" \
    -netdev "user,id=n0,hostfwd=tcp:127.0.0.1:$((BASE + 2))-:2222,hostfwd=tcp:127.0.0.1:$((BASE + 3))-:2223" \
    -device e1000,netdev=n0 \
    -netdev "user,id=n1,net=10.0.3.0/24,hostfwd=udp:127.0.0.1:$BASE-:5550" \
    -device e1000,netdev=n1

if [ -z "$PEER" ] && [ -z "$OUTDIR" ]; then
    "$QEMU" "$@" -serial stdio
    exit $?
fi

if [ -n "$OUTDIR" ]; then
    "$QEMU" "$@" -serial stdio < /dev/null > "$log" 2>&1 || {
        echo "apguest: the guest's QEMU failed:" >&2
        cat "$log" >&2
        exit 1
    }
    mkdir -p "$OUTDIR"
    for name in wpa2 sae peap; do
        for kind in pcap pids log; do
            file="$name.$kind"
            awk -v b="wifiap: trace: $file: begin" -v e="wifiap: trace: $file: end" '
                { sub(/\r$/, "") }
                index($0, e) == 1 { exit }
                on { print }
                $0 == b { on = 1 }' "$log" > "$work/$file.b64"
            want="$(sed -n "s/^wifiap: trace: $file: end \([0-9a-f]*\).*/\1/p" "$log")"
            base64 -d < "$work/$file.b64" > "$work/$file"
            got="$(make -s -C "$REPO" build/tools/verify/sha256 > /dev/null &&
                "$REPO/build/tools/verify/sha256" "$work/$file" | awk '{print $1}')"
            if [ -z "$want" ] || [ "$got" != "$want" ]; then
                echo "apguest: trace: $file: sha256 ${got:-none}, the guest said ${want:-nothing}" >&2
                grep -a '^wifiap: ' "$log" >&2 || true
                exit 1
            fi
            mv "$work/$file" "$OUTDIR/$file"
        done
    done
    echo "apguest: result: traces in $OUTDIR"
    exit 0
fi

"$QEMU" "$@" -chardev "socket,id=c0,host=127.0.0.1,port=$((BASE + 4)),server=on,wait=off,logfile=$(native "$log")" \
    -serial chardev:c0 < /dev/null > "$work/qemu.out" 2>&1 &
guest=$!
# The console's log grows as the guest writes; it ends with a line of the
# boot's result, or QEMU ends first.
while :; do
    if grep -q '^wifiap: ready: ' "$log" 2> /dev/null; then
        break
    fi
    if grep -q '^wifiap: failed: ' "$log" 2> /dev/null || ! kill -0 "$guest" 2> /dev/null; then
        echo "apguest: the guest did not become ready:" >&2
        cat "$work/qemu.out" "$log" >&2 2> /dev/null || true
        exit 1
    fi
    sleep 1
done
echo "apguest: ready: $PROFILE"

if [ -z "$IMAGE" ]; then
    wait "$guest"
    status=$?
    guest=
    exit $status
fi

ARCH="${ARCH:-i686}"
case $ARCH in
    i686) build="$REPO/build"; machine=qemu-system-i386 ;;
    x86_64) build="$REPO/build/x86_64"; machine=qemu-system-x86_64 ;;
    *) echo "apguest: ARCH is i686 or x86_64, not $ARCH" >&2; exit 2 ;;
esac
initrd="$build/images/$IMAGE.krffs"
if [ ! -f "$initrd" ]; then
    echo "apguest: no image $initrd; build it with make" >&2
    exit 1
fi
status=0
"$machine" -accel "$ACCEL" -cpu max -display none -no-reboot \
    -kernel "$(native "$build/rach2/kernel")" -initrd "$(native "$initrd")" \
    -netdev user,id=n0 -device e1000,netdev=n0 \
    -netdev "user,id=n1,net=10.0.4.0/24,hostfwd=udp:127.0.0.1:$((BASE + 1))-:5550" \
    -device e1000,netdev=n1 -serial stdio || status=$?
exit $status
