#!/bin/sh
#
# gpt_write_refusal.sh: the recovery shell's `disk write` refuses a GPT disk.
#
# `disk write` in the recovery kernel's debug shell proves the whole-disk
# write path by writing the boot disk's last sector and reading it back. Past
# every MBR partition that sector is scratch space, but on a GUID-partitioned
# disk it is the backup header, and a disk whose backup header is gone opens
# only in a repair tool (entry 124 of the shortcomings list). The command
# reads LBA 1 first and refuses a disk that has `EFI PART` there, and where
# LBA 1 has none it reads the last sector and refuses a disk that has it
# there (entry 172).
#
# The GPT disks are made as the parts suite's two GPT disks are: 32 MiB,
# seed 1, six labelled entries. Each is checked to end in its backup header,
# booted as the recovery kernel's only disk and sent the command, and the
# row requires the refusal on the console, no round-trip, and every byte of
# the disk as it was. The disks run in this order, and the row stops at its
# first failure:
#
#   g  a GPT disk with no protective MBR, its sector 0 zeroed;
#   h  the same with its primary header zeroed as well;
#   b  a GPT disk with both headers whole;
#   c  one whose primary header's checksum is broken, so a reader takes its
#      table from the backup;
#   d  one whose primary header's sector is zeroed, so LBA 1 says nothing
#      and the table is in the backup alone;
#   e  an MBR disk whose partition reaches the last sector, as a root grown
#      into the whole disk does, which must be refused and left as it was
#      (entry 195);
#   f  an MBR disk whose partition ends before the last sector, which must
#      take the round-trip and change in that sector alone.
#
# A protective MBR's entry covers the whole disk, so on b, c and d the MBR
# check refuses too; on g and h the GPT checks alone keep the disk. They go
# first so that a GPT check taken out stops the row on a disk it wrote,
# which is where the mutation test judges it (entry 206).
#
# Usage: gpt_write_refusal.sh RECOVERY_KERNEL IMAGE HARNESS MKGPT OUT
#
# OUT is a path prefix: OUT.b.disk to OUT.h.disk are the disks, and OUT.b.log
# to OUT.h.log the consoles, which are kept.
# `tests/run parts` runs this on the recovery kernel, and gpt_write_mutate.sh
# on kernels built from a private copy.
#
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3; MKGPT=$4; OUT=$5
fail() { echo "gpt-write: FAIL: $1" >&2; exit 1; }

# `gpt_disk DISK [--damage-primary]`: the parts suite's GPT disk.
gpt_disk() {
    disk=$1
    shift
    rm -f "$disk"
    dd if=/dev/zero of="$disk" bs=1048576 count=32 2>/dev/null || fail "the GPT disk $disk"
    "$MKGPT" --seed 1 "$@" "$disk" 64:2048:fat=ONE 4096:4096:data=TWO 8192:8192:fat=DATA3 \
        16384:32768:data=ROOT 49152:1024:data 50176:1024:data=SIX > /dev/null \
        || fail "mkgpt $disk"
}

# `mbr_disk DISK COUNT`: a 32 MiB disk whose one primary partition, type
# 0x83, starts at sector 2048 and is COUNT sectors long, given as the four
# octal escapes of its little-endian length.
mbr_disk() {
    disk=$1
    rm -f "$disk"
    dd if=/dev/zero of="$disk" bs=1048576 count=32 2>/dev/null || fail "the MBR disk $disk"
    # shellcheck disable=SC2059 # the length's escapes are part of the format
    printf "\\000\\000\\000\\000\\203\\000\\000\\000\\000\\010\\000\\000$2" |
        dd of="$disk" bs=1 seek=446 conv=notrunc 2>/dev/null || fail "the table of $disk"
    printf '\125\252' | dd of="$disk" bs=1 seek=510 conv=notrunc 2>/dev/null ||
        fail "the signature of $disk"
}

# `refused NAME WHAT EXPECT [gpt]`: the disk OUT.NAME.disk booted and sent
# `disk write`, which must answer EXPECT; with `gpt`, the disk is first
# checked to end in its backup header.
refused() {
    disk="$OUT.$1.disk"
    saved="$OUT.$1.saved"
    log="$OUT.$1.log"
    sectors=$(($(wc -c < "$disk") / 512))
    if [ "${4:-}" = gpt ]; then
        last=$(dd if="$disk" bs=512 skip=$((sectors - 1)) count=1 2>/dev/null | head -c 8)
        [ "$last" = "EFI PART" ] || fail "$2: the disk's last sector is not its backup GPT header"
    fi
    cp "$disk" "$saved" || fail "$2: the copy of the disk"
    rm -f "$log"
    if ! "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --qemu-arg=-cpu --qemu-arg=max \
        --drive "file=$disk,format=raw,if=ide,index=0,media=disk" \
        --append "rach2.rescue" --timeout 180 --steptmo 30 --log "$log" \
        --ok "not a scratch sector" --never "last-sector round-trip" \
        "boot=Type 'help'" "send=disk write" "expect=$3" \
        > "$OUT.$1.harness.log" 2>&1; then
        sed -n 's/^HARNESS: /gpt-write: /p' "$OUT.$1.harness.log" >&2
        tail -20 "$log" >&2
        fail "$2: the recovery shell's disk write was not refused"
    fi
    cmp "$saved" "$disk" > /dev/null || fail "$2: the refused disk write changed the disk"
    rm -f "$saved"
    echo "gpt-write: $2: the disk write is refused and every byte is as it was: OK"
}

# `written NAME WHAT`: the disk OUT.NAME.disk, whose last sector no partition
# holds, takes the round-trip, and only that sector changes, to the pattern
# the command writes.
written() {
    disk="$OUT.$1.disk"
    saved="$OUT.$1.saved"
    log="$OUT.$1.log"
    sectors=$(($(wc -c < "$disk") / 512))
    cp "$disk" "$saved" || fail "$2: the copy of the disk"
    rm -f "$log"
    if ! "$HARNESS" --kernel "$KERNEL" --initrd "$IMAGE" --qemu-arg=-cpu --qemu-arg=max \
        --drive "file=$disk,format=raw,if=ide,index=0,media=disk" \
        --append "rach2.rescue" --timeout 180 --steptmo 30 --log "$log" \
        --ok "last-sector round-trip OK" --never "not a scratch sector" \
        "boot=Type 'help'" "send=disk write" "expect=disk: last-sector round-trip OK" \
        > "$OUT.$1.harness.log" 2>&1; then
        sed -n 's/^HARNESS: /gpt-write: /p' "$OUT.$1.harness.log" >&2
        tail -20 "$log" >&2
        fail "$2: the recovery shell's disk write did not take the round-trip"
    fi
    cmp -n $(((sectors - 1) * 512)) "$saved" "$disk" > /dev/null ||
        fail "$2: the round-trip changed a sector before the last"
    python3 -c 'import sys; d = open(sys.argv[1], "rb").read()[-512:]; sys.exit(d != bytes((i ^ 0x5A) & 0xFF for i in range(512)))' "$disk" ||
        fail "$2: the last sector does not hold the round-trip's pattern"
    rm -f "$saved"
    echo "gpt-write: $2: the disk write takes the round-trip and changes the last sector alone: OK"
}

gpt_disk "$OUT.g.disk"
dd if=/dev/zero of="$OUT.g.disk" bs=512 count=1 conv=notrunc 2>/dev/null ||
    fail "the protective MBR of disk g"
refused g "a GPT disk with no protective MBR" "disk: GPT disk; the last sector is its backup header" gpt
gpt_disk "$OUT.h.disk"
dd if=/dev/zero of="$OUT.h.disk" bs=512 count=2 conv=notrunc 2>/dev/null ||
    fail "the protective MBR and the primary header of disk h"
refused h "a GPT disk with no protective MBR whose primary header is gone" \
    "disk: GPT disk; the last sector is its backup header" gpt
gpt_disk "$OUT.b.disk"
refused b "a GPT disk" "disk: GPT disk; the last sector is its backup header" gpt
gpt_disk "$OUT.c.disk" --damage-primary
refused c "a GPT disk read from its backup" "disk: GPT disk; the last sector is its backup header" gpt
gpt_disk "$OUT.d.disk"
dd if=/dev/zero of="$OUT.d.disk" bs=512 seek=1 count=1 conv=notrunc 2>/dev/null ||
    fail "the primary header of disk d"
refused d "a GPT disk whose primary header is gone" "disk: GPT disk; the last sector is its backup header" gpt
mbr_disk "$OUT.e.disk" '\000\370\000\000'
refused e "an MBR disk whose partition reaches the last sector" \
    "disk: the last sector belongs to partition 1, not a scratch sector"
mbr_disk "$OUT.f.disk" '\000\360\000\000'
written f "an MBR disk whose partition ends before the last sector"
rm -f "$OUT.b.disk" "$OUT.c.disk" "$OUT.d.disk" "$OUT.e.disk" "$OUT.f.disk" "$OUT.g.disk" "$OUT.h.disk"
