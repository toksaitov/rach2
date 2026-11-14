#!/bin/bash
# Exercise assignment on a private GNU root; the machine case owns this root.
# Usage: run.sh boot|nic-tcg|nic-kvm|dma|index|compat|kill|reassign|
#               gpu|gpu-uefi|gpu-grub|gpu-kill
set -eu
mode=${1:-}
work='' pid='' reader='' traffic='' keep=0
cleanup()
{
    [ -z "$pid" ] || { kill "$pid" 2>/dev/null || :; wait "$pid" 2>/dev/null || :; }
    [ -z "$reader" ] || { kill "$reader" 2>/dev/null || :; wait "$reader" 2>/dev/null || :; }
    [ -z "$traffic" ] || { kill "$traffic" 2>/dev/null || :; wait "$traffic" 2>/dev/null || :; }
    [ "$keep" = 1 ] || [ -z "$work" ] || rm -rf "$work"
}
trap cleanup EXIT
trap 'keep=1; exit 129' HUP
trap 'keep=1; exit 130' INT
trap 'keep=1; exit 143' TERM
fail()
{
    echo "assign: result: fail: $*" >&2
    echo "assign: evidence: $work" >&2
    keep=1
    exit 1
}
counts()
{
    awk '$1 ~ /^(vfio_(groups|devices|dma_mappings|dma_pages|dma_io_pages)|msi_vectors_free|kvm_(vms|vcpus|slots|tdp_pages|run_mappings))$/ { print }' /proc/stat
}
# Give the function to vfio-pci as a Linux user does: let its driver go, as
# virtio-pci has the NIC, then bind it by driver_override.
bind()
{
    echo vfio-pci > "/sys/bus/pci/devices/$1/driver_override"
    drv=$(readlink "/sys/bus/pci/devices/$1/driver" 2>/dev/null || :)
    if [ -n "$drv" ] && [ "${drv##*/}" != vfio-pci ]; then
        echo "$1" > "/sys/bus/pci/drivers/${drv##*/}/unbind"
    fi
    if [ "${drv##*/}" != vfio-pci ]; then
        echo "$1" > /sys/bus/pci/drivers/vfio-pci/bind
    fi
}
# The NIC is behind the recipe's one root port, so QEMU gives it FLR.
nic=0000:01:00.0
edu=0000:00:07.0
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-assign.XXXXXX")
[ -e /dev/vfio/vfio ] || fail 'VFIO is absent'
bind "$nic"
# In the gpu cases the fake Intel chip has EDU, and gpu.sh takes it from there.
case $mode in gpu*) ;; *) bind "$edu" ;; esac
before=$(counts)
[ "$(printf '%s\n' "$before" | wc -l)" = 11 ] || fail 'missing assignment counters'
echo "assign: before: $before"
awk '$1 == "iommu_dma_faults" || $1 == "iommu_irq_faults" { n++; if ($2 != 0) bad=1 }
     END { exit n != 2 || bad }' /proc/stat || fail 'IOMMU fault before injection'
ulimit -l unlimited

# Every iteration verifies disk data and an answered host-network packet.
traffic_loop()
{
    trap - EXIT
    trap 'exit 0' TERM
    while [ ! -e "$work/stop" ]; do
        dd if=/dev/urandom of="$work/data" bs=4096 count=16 2>/dev/null
        sha256sum "$work/data" > "$work/sum"
        sync
        sha256sum -c "$work/sum" >/dev/null || exit 1
        ping -c 1 10.0.2.2 >/dev/null 2>&1 || exit 1
        echo tick >> "$work/traffic"
    done
}
traffic_loop > "$work/traffic.out" 2>&1 &
traffic=$!
case $mode in
    boot)
        grep -q 'nvme' /proc/disk || fail 'NVMe disk absent'
        grep -q 'ahci' /proc/disk || fail 'AHCI disk absent'
        grep -q 'virtio' /proc/interrupts || fail 'virtio-gpu interrupt absent'
        grep -q 'nvme' /proc/interrupts || fail 'NVMe interrupt absent'
        dmesg | grep -q -E 'interrupt remapping: on, (xapic|x2apic) format, compatibility format refused' ||
            fail 'interrupt remapping is off'
        ;;
    dma|index|compat)
        # A unit whose status ignores GCMD.CFI delivers the forged message,
        # and no fault would ever be counted.
        if [ "$mode" = compat ] && dmesg | grep -q 'GSTS.CFIS does not follow GCMD.CFI'; then
            fail 'the outer QEMU delivers the compatibility format: it lacks qemu-vtd-cfi.patch'
        fi
        ./edu "$edu" "$mode" || fail "$mode injection failed"
        ;;
    nic-tcg|nic-kvm|kill|reassign)
        accel=tcg cpu=max
        [ "$mode" != nic-kvm ] || { accel=kvm,kernel-irqchip=split; cpu=host; }
        hold=
        case $mode in kill|reassign) hold=assign.hold ;; esac
        exits_before=$(awk '$1 == "kvm_exits" { print $2 }' /proc/stat)
        start=$(date +%s)
        mkfifo "$work/serial"
        tee "$work/console" < "$work/serial" &
        reader=$!
        qemu-system-x86_64 -nodefaults -no-reboot -display none -machine q35 \
            -accel "$accel" -cpu "$cpu" -m 128 -smp 1 \
            -kernel vmlinuz -initrd initrd -append "console=ttyS0 panic=-1 $hold" \
            -serial stdio -device "vfio-pci,host=$nic" \
            > "$work/serial" 2> "$work/qemu.err" < /dev/null &
        pid=$!
        while kill -0 "$pid" 2>/dev/null; do
            if [ -n "$hold" ] && grep -q 'ASSIGNGUEST TRAFFIC' "$work/console"; then
                awk '$1 == "vfio_dma_pages" && $2 > 0 { ok=1 } END { exit !ok }' /proc/stat || fail 'no live VFIO pins'
                awk '$1 == "vfio_dma_io_pages" && $2 > 0 { ok=1 } END { exit !ok }' /proc/stat || fail 'no live VFIO BAR mapping'
                echo "assign: live: $(counts)"
                resets_before=$(dmesg | grep -c 'pci: reset: 1:0.0' || :)
                kill -KILL "$pid"
                break
            fi
            sleep 1
        done
        rc=0
        wait "$pid" || rc=$?
        pid=
        wait "$reader" || fail 'serial reader failed'
        reader=
        cat "$work/qemu.err"
        if [ -n "$hold" ]; then
            resets_after=$(dmesg | grep -c 'pci: reset: 1:0.0' || :)
            [ "$resets_after" -gt "$resets_before" ] || fail 'no device reset after SIGKILL'
        fi
        [ "$rc" = 0 ] || { [ -n "$hold" ] && [ "$rc" = 137 ]; } || fail "QEMU exited $rc"
        grep -q 'ASSIGNGUEST OK' "$work/console" || fail 'guest did not ping gateway'
        exits_after=$(awk '$1 == "kvm_exits" { print $2 }' /proc/stat)
        echo "assign: exits: $((exits_after - exits_before)): $mode"
        echo "assign: elapsed: $(( $(date +%s) - start )) s: $mode"
        if [ "$mode" = reassign ]; then
            sh ../kvm/kvm-boot.sh -a tcg -k vmlinuz -i initrd -t 'ASSIGNGUEST OK' \
                -o "$work/second" -- -machine q35 -device "vfio-pci,host=$nic" || fail 'second guest failed'
        fi
        ;;
    gpu|gpu-uefi|gpu-grub|gpu-kill)
        # The Lenovo's script and command line, with EDU under the fake chip
        # of rach2.i915fake in the GPU's place; the guest finds EDU at 00:02.0
        # and reads its identification register through its BAR.
        set -- -d "$edu" -a tcg -m 256 -s 1 -o "$work/gpu" -t 'ASSIGNGUEST BAR0 0x010000ED'
        case $mode in
            gpu) set -- "$@" -k vmlinuz -i initrd -l assign.gpu ;;
            gpu-uefi) set -- "$@" -f uefi -k vmlinuz -i initrd -l assign.gpu ;;
            gpu-grub) set -- "$@" -f uefi -D grub.img ;;
            gpu-kill) set -- "$@" -k vmlinuz -i initrd -l 'assign.gpu assign.hold' -K 'ASSIGNGUEST HOLD' ;;
        esac
        start=$(date +%s)
        bash gpu.sh "$@" || fail "$mode: the function did not go to the guest and back"
        grep -q 'ASSIGNGUEST DEVICE 0x1234 0x11e8 ' "$work/gpu/console" || fail 'the guest did not find EDU at 00:02.0'
        [ "$mode" != gpu-grub ] || grep -q 'ASSIGNGUEST GRUB' "$work/gpu/console" || fail "the guest's GRUB did not run"
        echo "assign: elapsed: $(( $(date +%s) - start )) s: $mode"
        ;;
    *) fail 'unknown case' ;;
esac
while [ ! -s "$work/traffic" ]; do
    kill -0 "$traffic" 2>/dev/null || fail 'host traffic failed'
    sleep 1
done
kill -0 "$traffic" 2>/dev/null || fail 'host traffic failed'
touch "$work/stop"
wait "$traffic" || fail 'host traffic failed'
traffic=
after=$(counts)
echo "assign: after: $after"
awk -v mode="$mode" '
    $1 == "iommu_dma_faults" { n++; dma=$2; if ($2 != (mode == "dma")) bad=1 }
    $1 == "iommu_irq_faults" { n++; irq=$2; if ($2 != (mode == "index" || mode == "compat")) bad=1 }
    END { print "assign: faults: dma " dma ", irq " irq; exit n != 2 || bad }
' /proc/stat || fail 'unexpected IOMMU fault count'
[ "$before" = "$after" ] || fail 'assignment resources leaked'
echo "assign: traffic: $(wc -l < "$work/traffic") verified disk and network rounds"
echo "assign: result: pass: $mode"
