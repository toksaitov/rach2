#!/bin/sh
# bed-shm.sh: Rach2's virtio-gpu over the region it shares with the test
# bed's VM, run under Rach2's KVM (tests/cases/machine.bed-shm.json).
#
#   sh bed-shm.sh -d DISK [-p PROBE] [-c CRC] [-o DIR] [-m MIB] [-n CPUS]
#
# ivshmemd attaches a region of 32 MiB to /dev/vgpu-shm, and two more
# attaches are refused; QEMU boots DISK, the VM that `bed/vm/image/vm.sh
# rach2` makes, under -accel kvm with the region on an ivshmem-doorbell of
# two vectors; once virtio-gpu has bound over the region, PROBE draws the
# display frame with virgl in the VM and reads it back, and its CRC must be
# CRC, llvmpipe's. The doorbells must have carried the round trips with no
# exit to QEMU: every doorbell the VM rang reached Rach2 through a KVM
# ioeventfd and every one Rach2 rang through an irqfd. Then QEMU is stopped
# with fenced submissions pending and killed: ivshmemd must detach with the
# VM's death and every waiter must see an error. Each check prints one line,
# `bed-shm: CHECK: pass` or `fail` with the reason, and the last line is
# `bed-shm: result: pass` or `fail`. The console of the VM, ivshmemd's
# output and the counters are kept in DIR.
set -u
disk='' probe=./vgshmprobe crc=6e8e217c out=/tmp/bed-shm mib=2048 cpus=2
while getopts d:p:c:o:m:n: o; do
    case $o in
        d) disk=$OPTARG ;;
        p) probe=$OPTARG ;;
        c) crc=$OPTARG ;;
        o) out=$OPTARG ;;
        m) mib=$OPTARG ;;
        n) cpus=$OPTARG ;;
        *) exit 2 ;;
    esac
done
[ -n "$disk" ] || { echo "usage: bed-shm.sh -d DISK [-p PROBE] [-c CRC] [-o DIR]" >&2; exit 2; }
daemon='' qemu='' waiter='' bad=0
trap 'kill -9 $qemu $waiter 2>/dev/null; kill $daemon 2>/dev/null; wait 2>/dev/null' EXIT
rm -rf "$out"
mkdir -p "$out"

pass() { echo "bed-shm: $1: pass${2:+: $2}"; }
fail() { echo "bed-shm: $1: fail: $2"; bad=$((bad + 1)); }

# stat NAME: a count of /proc/stat.
stat() { awk -v n="$1" '$1 == n { print $2 }' /proc/stat; }

# info FIELD: a field of ivshmemd -i's report, as `name value`.
info() { ivshmemd -i | tr ',' '\n' | sed -n "s/^.* $1 \([0-9/]*\).*$/\1/p" | head -n 1; }

# busy PID: the processor time of process PID, in ticks, which grows while
# a guest of it runs and stays while the guest waits halted.
busy() { awk '{ print $14 + $15 }' "/proc/$1/stat" 2>/dev/null || echo 0; }

# until_line FILE PATTERN SILENCE [PID]: wait until FILE has a line
# matching PATTERN, for as long as process PID runs and, within each SILENCE
# seconds, FILE grows or PID uses processor time.
until_line()
{
    last=-1 still=0
    until grep -q "$2" "$1" 2>/dev/null; do
        [ -z "${4:-}" ] || kill -0 "$4" 2>/dev/null || return 1
        sleep 1
        size=$({ wc -c < "$1"; } 2>/dev/null || echo 0):$([ -z "${4:-}" ] || busy "$4")
        if [ "$size" = "$last" ]; then
            still=$((still + 1))
            [ "$still" -lt "$3" ] || return 1
        else
            last=$size still=0
        fi
    done
}

# The attach, once, and two refused.
ivshmemd -s 32M "$out/ivs.sock" > "$out/ivshmemd.out" 2>&1 &
daemon=$!
until_line "$out/ivshmemd.out" 'listening on' 30 || true
refused=0
for i in 1 2; do
    if ivshmemd -s 32M "$out/other$i.sock" > "$out/refused$i.out" 2>&1; then
        :
    elif grep -q 'attach: refused.*busy' "$out/refused$i.out"; then
        refused=$((refused + 1))
    fi
done
if grep -q 'attached: 33554432 bytes' "$out/ivshmemd.out" && [ "$refused" -eq 2 ]; then
    pass attach "attached once, refused twice"
else
    fail attach "$(cat "$out/ivshmemd.out" "$out/refused1.out" | head -n 3 | tr '\n' ' ')"
fi

# The VM, under Rach2's KVM, with the region on its doorbell device.
TMPDIR=$out qemu-system-x86_64 -accel kvm -machine q35,kernel-irqchip=split -cpu host -smp "$cpus" \
    -m "${mib}M" -nodefaults -no-reboot -display none -monitor none \
    -drive file="$disk",if=virtio,format=raw,snapshot=on \
    -chardev socket,path="$out/ivs.sock",id=ivs -device ivshmem-doorbell,chardev=ivs,vectors=2 \
    -serial file:"$out/console.log" > "$out/qemu.out" 2>&1 &
qemu=$!
if until_line "$out/ivshmemd.out" 'QEMU connected' 60 "$qemu" &&
    until_line "$out/console.log" 'bed-vm: ready' 600 "$qemu"; then
    tries=0
    until [ "$(info bound)" = 1 ] || [ "$tries" -ge 120 ] || ! kill -0 "$qemu" 2>/dev/null; do
        tries=$((tries + 1))
        sleep 1
    done
fi
ivshmemd -i > "$out/info-bound.txt" 2>&1
if [ "$(info bound)" != 1 ]; then
    fail draw "virtio-gpu did not bind over the region: $(tail -n 2 "$out/console.log" "$out/qemu.out" 2>&1 | tr '\n' ' ')"
    fail doorbell "no device"
else
    io0=$(stat kvm_ioeventfd_signals) irq0=$(stat kvm_irqfd_raises)
    woken0=$(info woken | cut -d/ -f1) rung0=$(info rung | cut -d/ -f1)
    "$probe" draw "$crc" > "$out/draw.out" 2>&1
    io1=$(stat kvm_ioeventfd_signals) irq1=$(stat kvm_irqfd_raises)
    woken1=$(info woken | cut -d/ -f1) rung1=$(info rung | cut -d/ -f1)
    polls=$(info polls)
    ivshmemd -i > "$out/info-drawn.txt" 2>&1
    if grep -q 'VGSHMPROBE OK' "$out/draw.out"; then
        pass draw "$(sed -n 's/^VGSHMPROBE DRAW: //p' "$out/draw.out")"
    else
        fail draw "$(grep VGSHMPROBE "$out/draw.out" | tr '\n' ' ')"
    fi
    woken=$((woken1 - woken0)) rung=$((rung1 - rung0))
    io=$((io1 - io0)) irq=$((irq1 - irq0))
    echo "bed-shm: counts: rung $rung, woken $woken, polls $polls, ioeventfd $io, irqfd $irq" \
        > "$out/counts.txt"
    if [ "$(info doorbells)" = 1 ] && [ "$woken" -gt 0 ] && [ "$io" -ge "$woken" ] &&
        [ "$irq" -ge "$rung" ] && [ "$rung" -gt 0 ]; then
        pass doorbell "rung $rung, woken $woken, ioeventfd $io, irqfd $irq"
    else
        fail doorbell "$(cat "$out/counts.txt"), doorbells $(info doorbells)"
    fi
fi

# The VM's death with fences pending.
kill -STOP "$qemu" 2>/dev/null
"$probe" pending 4 > "$out/pending.out" 2>&1 &
waiter=$!
until_line "$out/pending.out" 'VGSHMPROBE PENDING\|VGSHMPROBE FAIL' 30 || true
kill -9 "$qemu" 2>/dev/null
wait "$qemu" 2>/dev/null
qemu=''
wait "$waiter" 2>/dev/null
waiter=''
wait "$daemon" 2>/dev/null
daemon=''
if grep -q 'VGSHMPROBE OK' "$out/pending.out" &&
    grep -q 'result: detached: QEMU closed' "$out/ivshmemd.out"; then
    pass death "$(sed -n 's/^VGSHMPROBE FENCE: //p' "$out/pending.out" | head -n 1)"
else
    fail death "$(grep 'VGSHMPROBE\|result' "$out/pending.out" "$out/ivshmemd.out" | tr '\n' ' ')"
fi

if [ "$bad" -eq 0 ]; then
    echo "bed-shm: result: pass"
else
    echo "bed-shm: result: fail: $bad check(s)"
    exit 1
fi
