#!/bin/sh
#
# vtest-rach2.sh: draw the triangle on Rach2 with Mesa's virpipe through the
# vtest relay to the vtest server guest, booted in the GNU root's QEMU, and
# check it against the triangle Rach2's llvmpipe draws.
#
# Usage: vtest-rach2.sh [-a ACCEL] [-s CPUS] [-m MIB] [-p PORT] [-w SECONDS]
#                       [-g BDF] [-x MEASURE -o OUT [-S SCENES] [-t SECONDS]]
#                       DIR
#
# DIR holds what `vtest-guest.sh stage` puts on the bed-vtest data disk: the
# prepared image, prepared.qcow2, its backing file, the cloud image, by the
# name the prepared image gives it, and triangle.c. Nothing in DIR is
# written, so it may be read-only.
#
# The runner compiles triangle.c with the root's GCC, since the cc first on
# root's path is TinyCC, which links only static programs, and draws it on
# llvmpipe. It then boots a new overlay of the prepared image, in a scratch
# directory under TMPDIR, with CPUS processors (2) and MIB of memory (2048),
# under ACCEL: `tcg`, the default, QEMU's emulator, or `kvm`, Rach2's KVM
# with the PIC, the I/O APIC and the PIT in Rach2's kernel, QEMU's default.
# QEMU's user network forwards PORT (5555) on Rach2's loopback address to the
# guest's vtest port, and QEMU's monitor, QMP, listens on PORT + 1 there.
# Once the guest reports its server ready on the serial console, the runner
# asks QMP for the guest's status over TCP with socat, which must answer
# `running`, then starts vtestrelay for one client and draws the triangle
# with virpipe through it. When the triangle fails or draws elsewhere, it may
# never have connected, so the runner stops the relay instead of waiting for
# it. It passes when both pictures check and their checksums are equal.
#
# The guest's console is read once a second, and each 60 s its size and last
# line are printed; QEMU's end before the server is ready, or SECONDS without
# a byte on it (600), fails the run. That bound is on silence, not on the
# boot's length, and it bounds each question to QMP the same way: QMP silent
# for SECONDS before it has answered ends the question. At each 120 s of silence, and before that failure, the
# runner prints what tells where the guest waits: the KVM lines of
# /proc/stat, the interrupt counts in the fdinfo of QEMU's VM descriptor,
# and over QMP QEMU's interrupt controllers, each vCPU's local APIC and the
# vCPUs' registers. Once the guest is ready it prints the counts and QEMU's
# interrupt controllers alone. Progress goes to standard output as
# `vtest-rach2: ACTION` lines, with the triangle's and the relay's lines and
# the guest's record, and the last line is `vtest-rach2: result: pass: the
# triangle through the relay equals llvmpipe's, CHECKSUM` or
# `vtest-rach2: result: fail: REASON`. On a failure the last 30 lines of the
# console and QEMU's messages come before it. The exit status is
# 0 on a pass, 1 on a failure and 2 on a usage error.
#
# With -g, the guest has the function BDF, which Rach2's Intel driver has,
# through gpu.sh (tests/assign/Readme.md), beside this script or in the
# tree's tests/assign: gpu.sh takes it from the driver, boots the guest with
# it on vfio-pci from a raw copy of the overlay, and gives it back when
# QEMU ends. A guest without a display function of its own has QEMU's VGA
# beside it, since the cloud image boots only with one. When BDF is a
# display function, the guest's server renders on its render node, which
# the guest reports as `vtest-guest: renderer: gpu ...`, and the run passes
# when the triangle through the relay checks and draws on no llvmpipe: the
# GPU's picture may differ from llvmpipe's in its edges, so the checksums
# are printed and not compared. At the end the runner sends gpu.sh SIGUSR1,
# which kills QEMU, and requires gpu.sh's pass, the function back with
# Rach2's driver, as `vtest-rach2: handoff: pass: ...`.
#
# With -x, once the triangle has passed, the runner measures the rung: a
# relay of its own for any number of clients, its log by `-v`, and for each
# scene of SCENES (`glmark2 piglit`) the harness MEASURE, measure.sh of
# bed/measure, with `-r vtest -p -v` on Mesa's virpipe, each scene for
# SECONDS as measure.sh takes them, into OUT/SCENE. Weston draws with
# Pixman, so that only the scene's client goes through the relay. Each
# scene's last line is printed as `vtest-rach2: measure: ...`; a scene that
# fails fails the run.

accel=tcg cpus=2 mib=2048 port=5555 silence=600
bdf='' measure='' results='' scenes='glmark2 piglit' seconds=''
usage()
{
    echo "usage: vtest-rach2.sh [-a ACCEL] [-s CPUS] [-m MIB] [-p PORT] [-w SECONDS]" >&2
    echo "                      [-g BDF] [-x MEASURE -o OUT [-S SCENES] [-t SECONDS]] DIR" >&2
    exit 2
}
while getopts a:s:m:p:w:g:x:o:S:t: o; do
    case $o in
        a) accel=$OPTARG ;;
        s) cpus=$OPTARG ;;
        m) mib=$OPTARG ;;
        p) port=$OPTARG ;;
        w) silence=$OPTARG ;;
        g) bdf=$OPTARG ;;
        x) measure=$OPTARG ;;
        o) results=$OPTARG ;;
        S) scenes=$OPTARG ;;
        t) seconds=$OPTARG ;;
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
    kvm) accel_option=kvm cpu=host ;;
    tcg) accel_option=tcg cpu=max ;;
    *) usage ;;
esac
if [ -n "$measure" ]; then
    [ -f "$measure" ] && [ -n "$results" ] || usage
    measure=$(cd "$(dirname "$measure")" && pwd)/${measure##*/}
    mkdir -p "$results"
    results=$(cd "$results" && pwd)
fi
gpush='' display=0
if [ -n "$bdf" ]; then
    here=$(cd "$(dirname "$0")" && pwd)
    for gpush in "$here/gpu.sh" "$here/../../tests/assign/gpu.sh" ''; do
        [ ! -f "$gpush" ] || break
    done
    [ -n "$gpush" ] || {
        echo "vtest-rach2: no gpu.sh beside the script or in tests/assign for -g" >&2
        exit 2
    }
    # Under rach2.i915fake QEMU's edu device, vendor 0x1234 as all of
    # QEMU's own, has the display class; in the guest it is no display.
    case $(cat "/sys/bus/pci/devices/$bdf/class" 2>/dev/null) in
        0x03*) [ "$(cat "/sys/bus/pci/devices/$bdf/vendor")" = 0x1234 ] || display=1 ;;
        '') echo "vtest-rach2: no function $bdf" >&2; exit 2 ;;
    esac
fi

# $qemu is QEMU's process, and with -g $gpu is gpu.sh's, which owns it.
work='' qemu='' relay='' gpu='' gpu_status=''
# shellcheck disable=SC2317 # the traps call it
cleanup()
{
    [ -z "$relay" ] || kill "$relay" 2>/dev/null
    # gpu.sh, sent SIGTERM, kills its QEMU and gives the function back.
    if [ -n "$gpu" ]; then kill "$gpu" 2>/dev/null; elif [ -n "$qemu" ]; then kill "$qemu" 2>/dev/null; fi
    [ -z "$relay" ] || wait "$relay" 2>/dev/null
    if [ -n "$gpu" ]; then wait "$gpu" 2>/dev/null; elif [ -n "$qemu" ]; then wait "$qemu" 2>/dev/null; fi
    [ -z "$work" ] || rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

# fail REASON: print the console's tail, QEMU's messages and the failure.
fail()
{
    if [ -n "$work" ] && [ -f "$console" ]; then
        tr -d '\r' < "$console" | tail -n 30 | sed 's/^/vtest-rach2: console: /'
        sed 's/^/vtest-rach2: qemu: /' "$work/qemu.err" 2>/dev/null
    fi
    [ -z "$gpu" ] || [ ! -f "$work/gpu.log" ] || sed 's/^/vtest-rach2: /' "$work/gpu.log"
    echo "vtest-rach2: result: fail: $*"
    exit 1
}

# qmp COMMAND...: send QMP's capabilities negotiation and each COMMAND, a JSON
# object, to QEMU's monitor over TCP with socat, and print what QEMU answers
# once it has answered each, once QEMU or socat ends, or once QMP has been
# silent for SECONDS. Until then a newline, which QMP's parser skips, goes
# each second, so that a socat that has ended ends the writer too.
qmp()
{
    : > "$work/qmp.out"
    answers=$(($# + 1))
    # shellcheck disable=SC2094 # the writer counts the answers socat has written
    {
        echo '{"execute":"qmp_capabilities"}'
        printf '%s\n' "$@"
        heard='' still=0
        while kill -0 "$qemu" 2>/dev/null && [ "$still" -lt "$silence" ] &&
            [ "$(tr -d '\r' < "$work/qmp.out" | grep -c '^{"\(return\|error\)"')" -lt "$answers" ]; do
            sleep 1
            echo || break
            size=$(wc -c < "$work/qmp.out")
            if [ "$size" != "$heard" ]; then
                heard=$size still=0
            else
                still=$((still + 1))
            fi
        done
    } | socat - "TCP:127.0.0.1:$((port + 1))" > "$work/qmp.out" 2>&1
    tr -d '\r' < "$work/qmp.out"
}

# hmp COMMAND: print the QMP command that runs the monitor's COMMAND.
hmp()
{
    printf '{"execute":"human-monitor-command","arguments":{"command-line":"%s"}}' "$1"
}

# dump [all]: print the KVM lines of /proc/stat, the interrupt counts of
# QEMU's VM in Rach2's KVM and QEMU's `info pic`, and with `all` its `info
# irq`, the vCPUs' registers and each local APIC too, over QMP, each line
# led by its source.
dump()
{
    [ -n "$qemu" ] || return 0
    all=$1
    grep -a '^kvm_' /proc/stat | sed 's/^/vtest-rach2: stat: /'
    for f in /proc/"$qemu"/fd/*; do
        [ "$(readlink "$f" 2>/dev/null)" = anon_inode:kvm-vm ] || continue
        grep -a '^kvm_' /proc/"$qemu"/fdinfo/"${f##*/}" | sed 's/^/vtest-rach2: fdinfo: /'
    done
    i=0
    set -- "$(hmp 'info pic')"
    [ -z "$all" ] || set -- "$@" "$(hmp 'info irq')" "$(hmp 'info registers -a')"
    while [ -n "$all" ] && [ "$i" -lt "$cpus" ]; do
        set -- "$@" "$(hmp "info lapic $i")"
        i=$((i + 1))
    done
    qmp "$@" | sed 's/\\r\\n/\n/g; s/\\n/\n/g' | sed 's/^/vtest-rach2: qmp: /'
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

echo "vtest-rach2: compile triangle.c with gcc"
gcc -O2 -o "$work/triangle" "$dir/triangle.c" -ldl 2>&1 | sed 's/^/vtest-rach2: gcc: /'
[ -x "$work/triangle" ] || fail "triangle.c did not compile"
echo "vtest-rach2: draw on llvmpipe"
draw llvmpipe llvmpipe || fail "the llvmpipe triangle failed"
want=$(checksum llvmpipe)

echo "vtest-rach2: boot the guest under $accel_option, $cpus CPUs, $mib MiB${bdf:+, with $bdf}"
qemu-img create -q -f qcow2 -F qcow2 -b "$dir/prepared.qcow2" "$work/run.qcow2" ||
    fail "the guest's overlay was not made"
# The cloud image boots only with a display adapter: without one, QEMU ends
# after GRUB's `Booting`. QEMU's standard VGA is the one vtest-guest.sh gives.
if [ -z "$bdf" ]; then
    console=$work/console.log
    : > "$console"
    qemu-system-x86_64 -nodefaults -no-reboot -display none -machine q35 \
        -accel "$accel_option" -cpu "$cpu" -smp "$cpus" -m "$mib" \
        -drive "file=$work/run.qcow2,if=virtio,format=qcow2" \
        -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$port-:5555" \
        -device virtio-net-pci,netdev=net0 -device virtio-rng-pci -device VGA \
        -serial "file:$console" -monitor none \
        -qmp "tcp:127.0.0.1:$((port + 1)),server,wait=off" 2> "$work/qemu.err" < /dev/null &
    qemu=$!
    alive=$qemu
else
    # gpu.sh boots a raw disk, which its QEMU opens as a snapshot.
    qemu-img convert -O raw "$work/run.qcow2" "$work/run.raw" ||
        fail "the guest's raw disk was not made"
    rm -f "$work/run.qcow2"
    console=$work/gpu/console
    set -- -pidfile "$work/qemu.pid" \
        -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$port-:5555" \
        -device virtio-net-pci,netdev=net0 -device virtio-rng-pci \
        -qmp "tcp:127.0.0.1:$((port + 1)),server,wait=off"
    [ "$display" = 1 ] || set -- "$@" -device VGA
    TMPDIR=$work bash "$gpush" -d "$bdf" -a "$accel_option" -m "$mib" -s "$cpus" -w 0 \
        -o "$work/gpu" -D "$work/run.raw" -- "$@" > "$work/gpu.log" 2>&1 < /dev/null &
    gpu=$!
    alive=$gpu
    : > "$work/qemu.err"
fi

# Read the console until the server is ready, the guest fails, QEMU ends or
# the console is silent too long.
quiet=0 waited=0 last=''
while :; do
    [ -n "$qemu" ] || [ ! -s "$work/qemu.pid" ] || qemu=$(cat "$work/qemu.pid")
    if grep -a -q 'vtest-guest: ready' "$console" 2>/dev/null; then
        break
    fi
    if grep -a -q 'vtest-guest: failed' "$console" 2>/dev/null; then
        fail "the guest reported $(grep -a 'vtest-guest: failed' "$console" | tr -d '\r' | tail -n 1)"
    fi
    if ! kill -0 "$alive" 2>/dev/null; then
        status=0
        wait "$alive" || status=$?
        qemu='' gpu=''
        fail "QEMU${bdf:+ under gpu.sh} exited with $status before the server was ready"
    fi
    size=$({ wc -c < "$console"; } 2>/dev/null || echo 0)
    if [ "$size" != "$last" ]; then
        last=$size quiet=0
    else
        quiet=$((quiet + 1))
    fi
    if [ "$quiet" -ge "$silence" ]; then
        dump all
        fail "the console was silent for $silence s"
    fi
    [ "$quiet" -eq 0 ] || [ $((quiet % 120)) -ne 0 ] || {
        echo "vtest-rach2: the console has been silent for $quiet s"
        dump all
    }
    sleep 1
    waited=$((waited + 1))
    if [ $((waited % 60)) -eq 0 ]; then
        echo "vtest-rach2: boot: $waited s, console $size bytes: $(tr -d '\r' < "$console" |
            grep -a . | tail -n 1 | cut -c1-120)"
    fi
done
echo "vtest-rach2: the guest's server is ready after $waited s"

# QEMU reads its monitor's socket with recvmsg; QMP must answer past its
# greeting.
answer=$(qmp '{"execute":"query-status"}')
printf '%s\n' "$answer" | sed 's/^/vtest-rach2: qmp: /'
printf '%s\n' "$answer" | grep -q '"status": *"running"' ||
    fail "QMP over TCP did not answer query-status with running"
dump
{
    grep -a 'vtest-guest: version: ' "$console" | tr -d '\r' |
        sed 's/^.*vtest-guest: version: /guest /'
    grep -a 'vtest-guest: renderer: ' "$console" | tr -d '\r' |
        sed 's/^.*vtest-guest: renderer: /guest_renderer /'
    [ -z "$bdf" ] || echo "function $bdf$( [ "$display" = 1 ] && echo ', a display function')"
    echo "accelerator $accel_option"
    echo "cpus $cpus"
    echo "memory_mib $mib"
    echo "qemu $(qemu-system-x86_64 --version | head -n 1)"
    echo "relay $(vtestrelay --version)"
} | sed 's/^/vtest-rach2: record: /'

sock=$work/vtest.sock
vtestrelay -1 -v -s "$sock" 127.0.0.1 "$port" 2> "$work/relay.out" &
relay=$!
until [ -S "$sock" ]; do
    kill -0 "$relay" 2>/dev/null || {
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
# A triangle that failed, or drew elsewhere, may never have connected, and
# the relay would wait in accept() for it.
if [ "$drew" -ne 0 ] || ! grep -q '^triangle: renderer: virgl' "$work/virpipe.out"; then
    kill "$relay" 2>/dev/null
fi
relayed=0
wait "$relay" || relayed=$?
relay=''
sed 's/^/vtest-rach2: /' "$work/relay.out"
[ "$drew" -eq 0 ] && [ "$relayed" -eq 0 ] ||
    fail "through the relay, the triangle exited $drew and the relay $relayed"
grep -q '^triangle: renderer: virgl' "$work/virpipe.out" ||
    fail "the virpipe triangle did not draw on virgl"
got=$(checksum virpipe)
if [ "$display" = 1 ]; then
    grep -a -q 'vtest-guest: renderer: gpu' "$console" ||
        fail "the guest's server does not render on $bdf: $(grep -a 'vtest-guest: renderer:' "$console" | tr -d '\r')"
    ! grep -i -q '^triangle: renderer: .*llvmpipe' "$work/virpipe.out" ||
        fail "the triangle through the relay drew on llvmpipe"
    [ -n "$got" ] || fail "the triangle through the relay gave no checksum"
    drew="the triangle through the relay checks on $(sed -n 's/^triangle: renderer: //p' "$work/virpipe.out"), $got, llvmpipe's $want"
else
    [ -n "$got" ] && [ "$got" = "$want" ] ||
        fail "the checksums differ: llvmpipe $want, through the relay $got"
    drew="the triangle through the relay equals llvmpipe's, $got"
fi

# The rung's measurements: a relay for any number of clients, each
# scene's client on virpipe through it, Weston on Pixman.
if [ -n "$measure" ]; then
    msock=$work/measure.sock
    vtestrelay -v -s "$msock" 127.0.0.1 "$port" 2> "$results/relay.log" &
    relay=$!
    until [ -S "$msock" ]; do
        kill -0 "$relay" 2>/dev/null || fail "the measurement's relay did not listen on $msock"
        sleep 1
    done
    measured=0
    for scene in $scenes; do
        echo "vtest-rach2: measure $scene through the relay"
        status=0
        GALLIUM_DRIVER=virpipe LIBGL_ALWAYS_SOFTWARE=1 VTEST_SOCKET_NAME=$msock \
            sh "$measure" -r vtest -p -v "$results/relay.log" ${seconds:+-t "$seconds"} \
            -o "$results/$scene" "$scene" > "$results/$scene.out" 2>&1 || status=$?
        tail -n 1 "$results/$scene.out" | sed 's/^/vtest-rach2: measure: /'
        [ "$status" = 0 ] || measured=$((measured + 1))
    done
    kill "$relay" 2>/dev/null
    wait "$relay" 2>/dev/null
    relay=''
    dump
fi

# gpu.sh kills QEMU on SIGUSR1 and gives the function back.
if [ -n "$gpu" ]; then
    kill -USR1 "$gpu" 2>/dev/null
    gpu_status=0
    wait "$gpu" || gpu_status=$?
    gpu='' qemu=''
    sed 's/^/vtest-rach2: /' "$work/gpu.log"
    if [ "$gpu_status" != 0 ] || ! grep -q '^gpu: result: pass' "$work/gpu.log"; then
        fail "handoff: gpu.sh exited $gpu_status: $(grep '^gpu: result' "$work/gpu.log")"
    fi
    echo "vtest-rach2: handoff: pass: $(sed -n 's/^gpu: result: pass: //p' "$work/gpu.log")"
fi
[ "${measured:-0}" = 0 ] || fail "$measured of the scenes $scenes did not measure"
echo "vtest-rach2: result: pass: $drew"
