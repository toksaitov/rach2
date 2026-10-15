#!/bin/sh
# parts.sh: partition tables and roots by identity, on four AHCI disks.
#
# Disk A has an MBR with a KRFFS partition, a FAT32 one, an extended one
# holding three logical partitions and a fourth primary; disk B a GPT with
# six labelled partitions, one of them FAT32 and one an ext2 root; disk C
# the same GPT with its primary header damaged; disk D nothing. One boot
# runs partsprobe over them all, and a second runs it on disk E alone, a
# sparse 5 TiB GPT whose one partition starts past sector 2^32, since the
# AHCI driver publishes four disks. Then the kernel is asked for its root by
# identity: a GPT partition's GUID, the same with the disks swapped so the
# root's number changes, the same with the clone attached so the identity
# is ambiguous, an MBR id and entry for a KRFFS root, a label that two disks
# share, the node form, and an identity no disk has.
#
# Usage: parts.sh KERNEL IMAGE HARNESS MKMBR MKGPT EXT2IMAGE DIR
set -u
KERNEL=$1; IMAGE=$2; HARNESS=$3; MKMBR=$4; MKGPT=$5; EXT2=$6; DIR=$7
REPO=$(cd "$(dirname "$0")/../../.." && pwd)
fail() { echo "parts-test: FAIL: $1" >&2; exit 1; }
mkdir -p "$DIR"
A="$DIR/a.disk"; B="$DIR/b.disk"; C="$DIR/c.disk"; D="$DIR/d.disk"; E="$DIR/e.disk"

# Disk A: mkdisk lays the KRFFS partition at LBA 2048 for 48 MiB; the table
# is then rewritten around it with the id, the FAT32 primary, the extended
# partition and its logical ones, and the fourth primary.
sh "$REPO/krffs/mkdisk.sh" "$REPO/krffs/manifests/disk-test.manifest" "$A" 64 48 >/dev/null 2>&1 \
    || fail "mkdisk"
"$MKMBR" --lba --id 0a0a0a0a "$A" 2048:98304:7f 100352:8192:0c 108544:14336:05 122880:4096:83 \
    108545:4096:83:logical 112642:4096:0c:logical 116739:4096:83:logical >/dev/null || fail "mkmbr"
mformat -i "$A"@@$((100352 * 512)) -F -T 8192 -v APRIM :: || fail "mformat A p2"
mformat -i "$A"@@$((112642 * 512)) -F -T 4096 -v ALOG :: || fail "mformat A p6"

# `gpt_disk DISK [--damage-primary]`: the GPT disk with seed 1, its table's
# report in DISK.gpt.
gpt_disk() {
    disk=$1
    shift
    rm -f "$disk"
    dd if=/dev/zero of="$disk" bs=1048576 count=32 2>/dev/null || fail "the GPT disk $disk"
    "$MKGPT" --seed 1 "$@" "$disk" 64:2048:fat=ONE 4096:4096:data=TWO 8192:8192:fat=DATA3 \
        16384:32768:data=ROOT 49152:1024:data 50176:1024:data=SIX > "$disk.gpt" \
        || fail "mkgpt $disk"
}
# Disk B: the GPT, with the FAT32 volume in entry 3 and the ext2 root in 4.
gpt_disk "$B"
mformat -i "$B"@@$((8192 * 512)) -F -T 8192 -v DATA3 :: || fail "mformat B p3"
dd if="$EXT2" of="$B" bs=512 seek=16384 conv=notrunc 2>/dev/null || fail "the ext2 root into B"
G4=$(sed -n 's/^mkgpt: entry 4 guid //p' "$B.gpt")
[ -n "$G4" ] || fail "entry 4's GUID"
# Disk C: the clone with the damaged primary.
gpt_disk "$C" --damage-primary
# Disk D: nothing.
rm -f "$D"
dd if=/dev/zero of="$D" bs=1048576 count=8 2>/dev/null || fail "the blank disk"
# Disk E: 5 TiB and sparse, so that its size in sectors and in KiB and its
# partition's first sector are each past 32 bits.
rm -f "$E"
dd if=/dev/zero of="$E" bs=1048576 count=1 seek=$((5 * 1024 * 1024 - 1)) 2>/dev/null \
    || fail "the 5 TiB disk"
"$MKGPT" --seed 5 "$E" 4294969344:2048:data=FAR > /dev/null || fail "mkgpt $E"

# `run PORTS ARGS...` boots with the disks named in PORTS on ports 0 upward.
run() {
    ports=$1; shift
    set -- "$@" --qemu-arg=-device --qemu-arg=ich9-ahci,id=ahci0
    n=0
    for disk in $ports; do
        case $disk in
            A) file=$A ;;
            B) file=$B ;;
            C) file=$C ;;
            E) file=$E ;;
            *) file=$D ;;
        esac
        set -- "$@" --drive "file=$file,format=raw,if=none,id=drv$n" \
            --qemu-arg=-device --qemu-arg=ide-hd,drive=drv$n,bus=ahci0.$n
        n=$((n + 1))
    done
    "$HARNESS" --kernel "$KERNEL" --qemu-arg=-cpu --qemu-arg=max "$@"
}

run "A B C D" --initrd "$IMAGE" --append "rach2.disk=ahci rach2.diskdiag rach2.rescue" \
    --timeout 120 --steptmo 60 --ok "PARTSPROBE OK" --never "PARTSPROBE FAIL" --log "$DIR/probe.log" \
    "expect=Type 'help'" "send=exec /partsprobe" "expect=PARTSPROBE" > /dev/null 2>&1 \
    || fail "the partsprobe boot"
echo "parts-test: four tables read, the nodes as numbered, two partitions written, the table read again: OK"
mtype -i "$A"@@$((112642 * 512)) ::/logical.txt 2>/dev/null | grep -q "logical six says hello" \
    || fail "the file written through the logical partition"
mtype -i "$B"@@$((8192 * 512)) ::/gpt.txt 2>/dev/null | grep -q "gpt three says hello" \
    || fail "the file written through the GPT partition"
echo "parts-test: both files are where the nodes said: OK"
run "E" --initrd "$IMAGE" --append "rach2.disk=ahci rach2.diskdiag rach2.rescue" \
    --timeout 120 --steptmo 60 --ok "PARTSPROBE OK" --never "PARTSPROBE FAIL" --log "$DIR/large.log" \
    "expect=Type 'help'" "send=exec /partsprobe large" "expect=PARTSPROBE" > /dev/null 2>&1 \
    || fail "the 5 TiB disk's boot"
echo "parts-test: the 5 TiB disk's numbers past 32 bits in /proc/disk, /sys/block and /proc/partitions: OK"

# root_boot WHAT PORTS SPEC WANT NEVER: boot with the disks PORTS and the root
# named SPEC to the shell, and require the record WANT and, when NEVER is not
# empty, the absence of NEVER; WHAT labels the log and the result.
root_boot() {
    what=$1; ports=$2; spec=$3; want=$4; never=$5
    if [ -n "$never" ]; then
        run "$ports" --append "rach2.disk=ahci rach2.root=$spec rach2.rescue" \
            --timeout 90 --steptmo 60 --ok "$want" --never "$never" --log "$DIR/$what.log" \
            "expect=Type 'help'" > /dev/null 2>&1 || fail "$what"
    else
        run "$ports" --append "rach2.disk=ahci rach2.root=$spec rach2.rescue" \
            --timeout 90 --steptmo 60 --ok "$want" --log "$DIR/$what.log" \
            "expect=Type 'help'" > /dev/null 2>&1 || fail "$what"
    fi
    echo "parts-test: $what: OK"
}
root_boot "the root by GPT GUID" "A B D" "PARTUUID=$G4" "ext2: root: /dev/disk1p4" ""
root_boot "the same root with the disks swapped" "B A D" "PARTUUID=$G4" "ext2: root: /dev/disk0p4" ""
root_boot "the same identity on two disks refused" "A B C" "PARTUUID=$G4" \
    "root: PARTUUID=$G4: ambiguous: /dev/disk1p4 and /dev/disk2p4" "ext2: root:"
root_boot "a KRFFS root by MBR id and entry" "D A" "MBRID=0a0a0a0a,1" \
    "root: MBRID=0a0a0a0a,1: /dev/disk1p1, KRFFS" ""
root_boot "a label two disks share refused" "B C" "LABEL=DATA3" \
    "root: LABEL=DATA3: ambiguous: /dev/disk0p3 and /dev/disk1p3" "root: /dev"
root_boot "the node form" "A B" "/dev/disk1p4" "ext2: root: /dev/disk1p4" ""
root_boot "an identity no disk has" "A B" "MBRID=deadbeef,1" \
    "root: MBRID=deadbeef,1: not found on any of 2 disks" "root: /dev"
