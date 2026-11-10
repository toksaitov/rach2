#!/bin/sh
#
# vtest-rach2.sh: draw the triangle on Rach2 with Mesa's virpipe through the
# vtest relay to the vtest server guest, booted in the GNU root's QEMU, and
# check it against the triangle Rach2's llvmpipe draws.
#
# Usage: vtest-rach2.sh [-a ACCEL] [-s CPUS] [-m MIB] [-p PORT] [-w SECONDS]
#                       DIR
#
# DIR holds what `vtest-guest.sh stage` puts on the bed-vtest data disk: the
# prepared image, prepared.qcow2, its backing file, the cloud image, by the
# name the prepared image gives it, and triangle.c. Nothing in DIR is
# written, so it may be read-only.
#
# The runner compiles triangle.c with the root's cc and draws it on llvmpipe.
# It then boots a new overlay of the prepared image, in a scratch directory
# under TMPDIR, with CPUS processors (2) and MIB of memory (2048), under ACCEL:
# `tcg`, the default, QEMU's emulator, or `kvm`, Rach2's KVM with QEMU's split
# interrupt controller, the one Rach2's KVM has. QEMU's user network forwards
# PORT (5555) on Rach2's loopback address to the guest's vtest port. Once the
# guest reports its server ready on the serial console, the runner starts
# vtestrelay for one client and draws the triangle with virpipe through it.
# It passes when both pictures check and their checksums are equal.
#
# The guest's console is read once a second; QEMU's end before the server is
# ready, or SECONDS without a byte on it (600), fails the run. That bound is
# on silence, not on the boot's length. Progress goes to standard output as
# `vtest-rach2: ACTION` lines, with the triangle's and the relay's lines and
# the guest's record, and the last line is `vtest-rach2: result: pass:
# CHECKSUM` or `vtest-rach2: result: fail: REASON`. On a failure the last 30
# lines of the console and QEMU's messages come before it. The exit status is
# 0 on a pass, 1 on a failure and 2 on a usage error.

accel=tcg cpus=2 mib=2048 port=5555 silence=600
usage()
{
    echo "usage: vtest-rach2.sh [-a ACCEL] [-s CPUS] [-m MIB] [-p PORT] [-w SECONDS] DIR" >&2
    exit 2
}
while getopts a:s:m:p:w: o; do
    case $o in
        a) accel=$OPTARG ;;
        s) cpus=$OPTARG ;;
        m) mib=$OPTARG ;;
        p) port=$OPTARG ;;
        w) silence=$OPTARG ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 1 ] || usage
dir=$(cd "$1" 2>/dev/null && pwd) || {
    echo "vtest-rach2: result: fail: no directory $1"
    exit 1
}
for n in "$cpus" "$mib" "$port" "$silence"; do
    case $n in
        '' | *[!0-9]*) usage ;;
    esac
done
case $accel in
    kvm) accel_option=kvm,kernel-irqchip=split cpu=host ;;
    tcg) accel_option=tcg cpu=max ;;
    *) usage ;;
esac

work='' qemu='' relay=''
# shellcheck disable=SC2317 # the traps call it
cleanup()
{
    [ -z "$relay" ] || kill "$relay" 2>/dev/null
    [ -z "$qemu" ] || kill "$qemu" 2>/dev/null
    [ -z "$relay" ] || wait "$relay" 2>/dev/null
    [ -z "$qemu" ] || wait "$qemu" 2>/dev/null
    [ -z "$work" ] || rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# fail REASON: print the console's tail, QEMU's messages and the failure.
fail()
{
    if [ -n "$work" ] && [ -f "$work/console.log" ]; then
        tr -d '\r' < "$work/console.log" | tail -n 30 | sed 's/^/vtest-rach2: console: /'
        sed 's/^/vtest-rach2: qemu: /' "$work/qemu.err" 2>/dev/null
    fi
    echo "vtest-rach2: result: fail: $*"
    exit 1
}

# draw NAME DRIVER: draw the triangle with Mesa's DRIVER, its picture into
# $work/NAME.ppm and its lines into $work/NAME.out, which are printed too.
# Return the triangle's status.
draw()
{
    status=0
    LIBGL_ALWAYS_SOFTWARE=1 GALLIUM_DRIVER=$2 "$work/triangle" -o "$work/$1.ppm" \
        > "$work/$1.out" 2>&1 || status=$?
    sed "s/^/vtest-rach2: $1: /" "$work/$1.out"
    return "$status"
}

# checksum NAME: print the checksum of the triangle NAME drew.
checksum()
{
    sed -n 's/^triangle: checksum: //p' "$work/$1.out"
}

for f in prepared.qcow2 triangle.c; do
    [ -f "$dir/$f" ] || fail "$dir has no $f"
done
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-vtest.XXXXXX") || fail "no scratch directory"

echo "vtest-rach2: compile triangle.c"
cc -O2 -o "$work/triangle" "$dir/triangle.c" -ldl 2>&1 | sed 's/^/vtest-rach2: cc: /'
[ -x "$work/triangle" ] || fail "triangle.c did not compile"
echo "vtest-rach2: draw on llvmpipe"
draw llvmpipe llvmpipe || fail "the llvmpipe triangle failed"
want=$(checksum llvmpipe)

echo "vtest-rach2: boot the guest under $accel_option, $cpus CPUs, $mib MiB"
qemu-img create -q -f qcow2 -F qcow2 -b "$dir/prepared.qcow2" "$work/run.qcow2" ||
    fail "the guest's overlay was not made"
: > "$work/console.log"
# The cloud image boots only with a display adapter: without one, QEMU ends
# after GRUB's `Booting`. QEMU's standard VGA is the one vtest-guest.sh gives.
qemu-system-x86_64 -nodefaults -no-reboot -display none -machine q35 \
    -accel "$accel_option" -cpu "$cpu" -smp "$cpus" -m "$mib" \
    -drive "file=$work/run.qcow2,if=virtio,format=qcow2" \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$port-:5555" \
    -device virtio-net-pci,netdev=net0 -device virtio-rng-pci -device VGA \
    -serial "file:$work/console.log" -monitor none 2> "$work/qemu.err" < /dev/null &
qemu=$!

# Read the console until the server is ready, the guest fails, QEMU ends or
# the console is silent too long.
quiet=0 waited=0 last=''
while :; do
    if grep -a -q 'vtest-guest: ready' "$work/console.log"; then
        break
    fi
    if grep -a -q 'vtest-guest: failed' "$work/console.log"; then
        fail "the guest reported $(grep -a 'vtest-guest: failed' "$work/console.log" | tr -d '\r' | tail -n 1)"
    fi
    if ! kill -0 "$qemu" 2>/dev/null; then
        wait "$qemu"
        status=$?
        qemu=''
        fail "QEMU exited with $status before the server was ready"
    fi
    size=$(wc -c < "$work/console.log")
    if [ "$size" != "$last" ]; then
        last=$size quiet=0
    else
        quiet=$((quiet + 1))
    fi
    [ "$quiet" -lt "$silence" ] || fail "the console was silent for $silence s"
    sleep 1
    waited=$((waited + 1))
done
echo "vtest-rach2: the guest's server is ready after $waited s"
{
    grep -a 'vtest-guest: version: ' "$work/console.log" | tr -d '\r' |
        sed 's/^.*vtest-guest: version: /guest /'
    echo "accelerator $accel_option"
    echo "cpus $cpus"
    echo "memory_mib $mib"
    echo "qemu $(qemu-system-x86_64 --version | head -n 1)"
    echo "relay $(vtestrelay --version)"
} | sed 's/^/vtest-rach2: record: /'

sock=$work/vtest.sock
vtestrelay -1 -v -s "$sock" 127.0.0.1 "$port" 2> "$work/relay.out" &
relay=$!
tries=0
until [ -S "$sock" ]; do
    tries=$((tries + 1))
    kill -0 "$relay" 2>/dev/null && [ "$tries" -lt 30 ] || {
        sed 's/^/vtest-rach2: /' "$work/relay.out"
        fail "the relay did not listen on $sock"
    }
    sleep 1
done
echo "vtest-rach2: draw on virpipe through the relay"
drew=0
VTEST_SOCKET_NAME=$sock
export VTEST_SOCKET_NAME
draw virpipe virpipe || drew=$?
relayed=0
wait "$relay" || relayed=$?
relay=''
sed 's/^/vtest-rach2: /' "$work/relay.out"
[ "$drew" -eq 0 ] && [ "$relayed" -eq 0 ] ||
    fail "through the relay, the triangle exited $drew and the relay $relayed"
grep -q '^triangle: renderer: virgl' "$work/virpipe.out" ||
    fail "the virpipe triangle did not draw on virgl"
got=$(checksum virpipe)
[ -n "$got" ] && [ "$got" = "$want" ] ||
    fail "the checksums differ: llvmpipe $want, through the relay $got"
echo "vtest-rach2: result: pass: the triangle through the relay equals llvmpipe's, $got"
