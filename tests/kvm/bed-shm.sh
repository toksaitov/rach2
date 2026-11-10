#!/bin/sh
# bed-shm.sh: Rach2's virtio-gpu over the region it shares with the test
# bed's VM, run under Rach2's KVM (tests/cases/machine.bed-shm.json).
#
#   sh bed-shm.sh -d DISK [-p PROBE] [-c CRC] [-k NOCOPY] [-o DIR] [-m MIB]
#       [-n CPUS]
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
# VM's death, every fence must fail and a wait after the death must too.
#
# With -k the draw, the doorbells and the death with fences pending give
# way to the buffers mapped from the VM's own pages: NOCOPY, nocopy.c, is
# built with the GNU root's GCC and run on Venus. Its rounds must keep both
# buffers coherent in fence order, the VM's backend must have answered
# their page lists, and their maps must be gone once the program has ended
# (`pages`, `coherent`, `release`); the probe's refusals must be refused
# (`refuse`); the timings of 83-5's comparison are kept in DIR/measure.txt
# (`measure`); and with a mapping held and touched the VM is killed: the
# touch after must raise SIGBUS, and every map and the frames it held must
# be gone (`revoke`, `leak`).
#
# Each check prints one line, `bed-shm: CHECK: pass` or `fail` with the
# reason, and the last line is `bed-shm: result: pass` or `fail`, after the
# VM's last 40 lines of console, `bed-shm: vm: LINE`, on a failure. The
# console of the VM, ivshmemd's output and the counters are kept in DIR.
set -u
disk='' probe=./vgshmprobe crc=6e8e217c out=/tmp/bed-shm mib=2048 cpus=2 nocopy=''
icd=/usr/share/vulkan/icd.d/venus_icd.x86_64.json
while getopts d:p:c:k:o:m:n: o; do
    case $o in
        d) disk=$OPTARG ;;
        p) probe=$OPTARG ;;
        c) crc=$OPTARG ;;
        k) nocopy=$OPTARG ;;
        o) out=$OPTARG ;;
        m) mib=$OPTARG ;;
        n) cpus=$OPTARG ;;
        *) exit 2 ;;
    esac
done
[ -n "$disk" ] || { echo "usage: bed-shm.sh -d DISK [-p PROBE] [-c CRC] [-o DIR]" >&2; exit 2; }
daemon='' qemu='' waiter='' holder='' bad=0
trap 'kill -9 $qemu $waiter $holder 2>/dev/null; kill $daemon 2>/dev/null; wait 2>/dev/null' EXIT
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

# quiet FILE SILENCE [PID]: one second of a wait, which `last=-1 still=0`
# starts; it fails once process PID has ended, or once for SILENCE seconds
# FILE has not grown and PID has used no processor time.
quiet()
{
    [ -z "${3:-}" ] || kill -0 "$3" 2>/dev/null || return 1
    sleep 1
    size=$({ wc -c < "$1"; } 2>/dev/null || echo 0):$([ -z "${3:-}" ] || busy "$3")
    if [ "$size" = "$last" ]; then
        still=$((still + 1))
        [ "$still" -lt "$2" ] || return 1
    else
        last=$size still=0
    fi
}

# until_line FILE PATTERN SILENCE [PID]: wait until FILE has a line
# matching PATTERN, while quiet FILE SILENCE PID holds.
until_line()
{
    last=-1 still=0
    until grep -q "$2" "$1" 2>/dev/null; do
        quiet "$1" "$3" "${4:-}" || return 1
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

# The VM, under Rach2's KVM, with the region on its doorbell device. Its
# disk takes no ioeventfd, and QEMU raises a disk's interrupts without an
# irqfd, so that both counts are the doorbells'.
TMPDIR=$out qemu-system-x86_64 -accel kvm -machine q35,kernel-irqchip=split -cpu host -smp "$cpus" \
    -m "${mib}M" -nodefaults -no-reboot -display none -monitor none \
    -drive file="$disk",if=none,id=vm,format=raw,snapshot=on \
    -device virtio-blk-pci,drive=vm,ioeventfd=off \
    -chardev socket,path="$out/ivs.sock",id=ivs -device ivshmem-doorbell,chardev=ivs,vectors=2 \
    -serial file:"$out/console.log" > "$out/qemu.out" 2>&1 &
qemu=$!
# The VM's backend binds virtio-gpu once its service starts, before the
# VM's boot has ended.
if until_line "$out/ivshmemd.out" 'QEMU connected' 60 "$qemu"; then
    last=-1 still=0
    until [ "$(info bound)" = 1 ]; do
        quiet "$out/console.log" 600 "$qemu" || break
    done
fi
ivshmemd -i > "$out/info-bound.txt" 2>&1

# nc ARGS: nocopy on Venus.
nc() { VK_DRIVER_FILES=$icd "$out/nocopy" "$@"; }

# nocopy_rows: the buffers mapped from the VM's pages, then the VM killed
# with one mapping live.
nocopy_rows()
{
    if [ "$(info bound)" != 1 ]; then
        for c in build pages coherent release refuse measure revoke leak; do
            fail "$c" "virtio-gpu did not bind over the region: $(tail -n 2 "$out/console.log" "$out/qemu.out" 2>&1 | tr '\n' ' ')"
        done
        return
    fi
    if gcc -std=c99 -O2 -o "$out/nocopy" "$nocopy" -ldl > "$out/build.out" 2>&1; then
        pass build "nocopy built"
    else
        fail build "$(head -n 3 "$out/build.out" | tr '\n' ' ')"
    fi
    maps0=$(stat kvm_gpa_maps) pages0=$(stat kvm_gpa_map_pages)
    nc rounds 16 1048576 > "$out/rounds.out" 2>&1
    ivshmemd -i > "$out/info-rounds.txt" 2>&1
    pins=$(grep -c 'backend: pin: resource' "$out/console.log")
    if grep -q '^NOCOPY MAPPED' "$out/rounds.out" && [ "$pins" -gt 0 ]; then
        pass pages "$pins blobs pinned, $(info maps) maps, $(sed -n 's/^NOCOPY MAPPED: //p' "$out/rounds.out")"
    else
        fail pages "pins $pins: $(grep 'NOCOPY' "$out/rounds.out" | tr '\n' ' ')"
    fi
    if grep -q '^NOCOPY OK' "$out/rounds.out"; then
        pass coherent "$(sed -n 's/^NOCOPY ROUNDS: //p' "$out/rounds.out")"
    else
        fail coherent "$(grep 'NOCOPY' "$out/rounds.out" | tr '\n' ' ')"
    fi
    if [ "$(stat kvm_gpa_maps)" = "$maps0" ] && [ "$(stat kvm_gpa_map_pages)" = "$pages0" ] &&
        [ "$(info map_live)" = 0 ]; then
        pass release "kvm_gpa_maps $maps0, kvm_gpa_map_pages $pages0, map_live 0"
    else
        fail release "kvm_gpa_maps $maps0 to $(stat kvm_gpa_maps), kvm_gpa_map_pages $pages0 to $(stat kvm_gpa_map_pages), map_live $(info map_live)"
    fi
    "$probe" refuse > "$out/refuse.out" 2>&1
    if grep -q 'VGSHMPROBE OK' "$out/refuse.out" &&
        grep -q 'backend: map: resource .*: refused: not exported' "$out/console.log"; then
        pass refuse "$(sed -n 's/^VGSHMPROBE REFUSE: //p' "$out/refuse.out" | tr '\n' ';')"
    else
        fail refuse "$(grep 'VGSHMPROBE' "$out/refuse.out" | tr '\n' ' ')"
    fi
    { qemu-system-x86_64 --version | head -n 1; uname -sr; } > "$out/measure.txt" 2>&1
    ok=1
    for b in 4096 65536 1048576 16777216 67108864; do
        nc serial 8 "$b" >> "$out/measure.txt" 2>&1 || ok=0
    done
    nc held 32 65536 >> "$out/measure.txt" 2>&1 || ok=0
    nc bandwidth 67108864 >> "$out/measure.txt" 2>&1 || ok=0
    for b in 4096 1048576 16777216; do
        "$probe" measure "$b" >> "$out/measure.txt" 2>&1 || ok=0
    done
    if [ "$ok" = 1 ]; then
        pass measure "$(grep -c '^NOCOPY [A-Z]*:\|^VGSHMPROBE MEASURE' "$out/measure.txt") figures"
    else
        fail measure "$(grep 'FAIL' "$out/measure.txt" | head -n 2 | tr '\n' ' ')"
    fi

    # The VM's death with a mapping live and touched.
    nc hold 1048576 > "$out/hold.out" 2>&1 &
    holder=$!
    until_line "$out/hold.out" 'NOCOPY HELD\|NOCOPY FAIL' 120 "$holder" || true
    ivshmemd -i > "$out/info-held.txt" 2>&1
    held=$(stat kvm_gpa_map_pages)
    kill -9 "$qemu" 2>/dev/null
    wait "$qemu" 2>/dev/null
    qemu=''
    until_line "$out/hold.out" 'NOCOPY SIGBUS\|NOCOPY FAIL' 60 "$holder" || true
    kill -9 "$holder" 2>/dev/null
    wait "$holder" 2>/dev/null
    holder=''
    wait "$daemon" 2>/dev/null
    daemon=''
    if grep -q '^NOCOPY SIGBUS' "$out/hold.out" && grep -q '^NOCOPY HELD' "$out/hold.out"; then
        pass revoke "$(sed -n 's/^NOCOPY SIGBUS: //p' "$out/hold.out")"
    else
        fail revoke "$(grep 'NOCOPY' "$out/hold.out" | tr '\n' ' ')"
    fi
    if [ "$(stat kvm_gpa_maps)" = "$maps0" ] && [ "$(stat kvm_gpa_map_pages)" = "$pages0" ] &&
        [ "$held" -gt "$pages0" ]; then
        pass leak "kvm_gpa_map_pages $pages0, $held while held, $(stat kvm_gpa_map_pages) after"
    else
        fail leak "kvm_gpa_maps $maps0 to $(stat kvm_gpa_maps), kvm_gpa_map_pages $pages0, $held while held, $(stat kvm_gpa_map_pages) after"
    fi
}

# virgl_rows: the frame drawn, the doorbells, and the VM's death with
# fences pending.
virgl_rows()
{
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
}

if [ -n "$nocopy" ]; then
    nocopy_rows
else
    virgl_rows
fi

if [ "$bad" -eq 0 ]; then
    echo "bed-shm: result: pass"
else
    # The VM's console is lost with the machine; its last lines go with
    # the result.
    tail -n 40 "$out/console.log" 2>/dev/null | tr -d '\r' | sed 's/^/bed-shm: vm: /'
    echo "bed-shm: result: fail: $bad check(s)"
    exit 1
fi
