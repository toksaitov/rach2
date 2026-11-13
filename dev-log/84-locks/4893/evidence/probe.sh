#!/bin/busybox sh
# Exercise PCI message delivery while changing processor affinity.
BB=/bin/busybox
$BB mkdir -p /proc /sys /dev
export PATH=/sbin:/usr/sbin:/bin:/usr/bin
$BB mount -t proc proc /proc 2>/dev/null
$BB mount -t sysfs sysfs /sys 2>/dev/null
$BB mount -t devtmpfs devtmpfs /dev 2>/dev/null
if [ ! -e /proc/disk ]; then
    for mod in ahci sd_mod nvme xhci-pci usb-storage; do
        /sbin/modprobe "$mod" 2>&1
    done
    $BB sleep 1
fi
printf 'AFFINITY BOOT\n'
$BB uname -a
$BB cat /proc/interrupts
for path in /proc/irq/*/smp_affinity; do
    [ -f "$path" ] || continue
    printf '%s ' "$path"
    $BB cat "$path"
    [ -f "${path%/*}/effective_affinity" ] && $BB cat "${path%/*}/effective_affinity"
done
printf 'AFFINITY MOVE\n'
for path in /proc/irq/*/smp_affinity; do
    [ -f "$path" ] || continue
    echo 8 > "$path" 2>/dev/null || continue
    printf '%s ' "$path"
    $BB cat "${path%/*}/effective_affinity"
done
for dev in /dev/disk0 /dev/disk1 /dev/disk2 /dev/sda /dev/sdb /dev/nvme0n1; do
    [ -b "$dev" ] || continue
    $BB dd if="$dev" of=/dev/null bs=4096 count=128 2>&1
    printf 'AFFINITY READ %s %s\n' "$dev" "$?"
done
$BB cat /proc/interrupts
if [ -e /dev/dsp ]; then
    printf 'AFFINITY AUDIO\n'
    $BB dd if=/dev/zero of=/dev/dsp bs=4096 count=8 2>&1
    printf 'AFFINITY AUDIO STATUS %s\n' "$?"
fi
printf 'AFFINITY RACE\n'
(
    for round in 1 2 3 4 5 6 7 8; do
        for dev in /dev/disk0 /dev/disk1 /dev/disk2 /dev/sda /dev/sdb /dev/nvme0n1; do
            [ -b "$dev" ] || continue
            $BB dd if="$dev" of=/dev/null bs=4096 count=128 2>/dev/null || exit 1
        done
    done
) &
reader=$!
for round in 1 2 3 4 5 6 7 8; do
    for mask in 1 2 4 8; do
        for path in /proc/irq/*/smp_affinity; do
            [ -f "$path" ] || continue
            echo "$mask" > "$path" 2>/dev/null
        done
    done
done
wait "$reader"
printf 'AFFINITY RACE STATUS %s\n' "$?"
$BB cat /proc/interrupts
printf 'AFFINITY RETURN\n'
for path in /proc/irq/*/smp_affinity; do
    [ -f "$path" ] || continue
    echo 1 > "$path" 2>/dev/null || continue
    printf '%s ' "$path"
    $BB cat "${path%/*}/effective_affinity"
done
for dev in /dev/disk0 /dev/disk1 /dev/disk2 /dev/sda /dev/sdb /dev/nvme0n1; do
    [ -b "$dev" ] || continue
    $BB dd if="$dev" of=/dev/null bs=4096 count=128 2>&1
done
$BB cat /proc/interrupts
printf 'AFFINITY DONE\n'
$BB sync
$BB poweroff -f
