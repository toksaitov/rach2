#!/bin/sh
# diskrate.sh: the rate of the storage paths, each timed in the machine and
# held under a multiple of a baseline the same boot measures: a file
# written and read back on exFAT and on FAT32, two hundred small files,
# sixteen mebibytes in 4 KiB calls, and a raw read through a pipe into a
# hash. The boot asks for the whole-disk nodes, which the ordinary kernel
# publishes only then and lets only the recovery build write, so the raw
# device is read and not written here. One boot of the os-real image with
# three IDE disks: a raw one, an exFAT volume and a FAT32 one.
#
# No row is judged by its time. The times are printed, with the rates,
# for the record of the machine; what a slow row would have meant is
# counted by the kernel and judged as a count, the same on every machine.
# A path that falls back to a page a request, a walk of the chain per
# block or a write through per cluster shows in the disk commands and the
# cache hits each row costs, which /proc/disk counts and the rows below
# hold under bounds set from the counts measured. A driver that spins on
# the drive or waits out a deadline shows in the status reads it makes and
# the ticks its requests spend in flight per command, which /proc/disk
# counts as disk-polls and disk-wait-ticks: this driver reads the status a
# few dozen times before it sleeps and is woken by the interrupt, so the
# boot's polls per command stay under a hundred and its ticks in flight
# per command under two, where a polled driver reads thousands and a
# waited-out deadline spends three thousand ticks in flight and is
# quarantined, which the device boots below hold at none. Multiples of a
# measured baseline were tried on 2026-09-18 and dropped the same day:
# the 4 KiB write rows cost four
# times the exFAT write in the emulator and twenty-one under KVM, since
# they are command counts times a per-command latency the machine sets,
# and no multiple fits both. Times are read from /proc/uptime in
# hundredths of a second.
#
# A second boot, of the recovery kernel, which alone writes a whole-disk
# node, times a 4 MiB raw write and raw read of a USB stick in 32 KiB
# requests, the write as 128 dd calls of one piece each since the recovery
# image cannot hold 4 MiB, each piece carrying its index, and requires each
# direction to be exactly 128 commands on the stick, none quarantined and
# none hundreds of ticks in flight; the AHCI boot's rows are held the same
# way. The times are printed for the record.
#
# A third boot, of the test image on the ordinary kernel, runs
# `placeprobe`, entry 26's placement experiment: 32 MiB read through a pipe
# into a buffer whose first page holds executed code, and into one that
# holds none, three runs each; the far runs are held to the pipe row's
# bound and every time is printed beside the pipe row's three.
#
# Usage: diskrate.sh KERNEL OSREAL HARNESS DIR PASSWORD MKEXFATFIXTURE MKMBR \
#        RKERNEL RIMAGE TESTIMAGE
set -u
KERNEL=$1; OSREAL=$2; HARNESS=$3; DIR=$4; PASSWORD=$5; MKEXFAT=$6; MKMBR=$7
RKERNEL=$8; RIMAGE=$9; TESTIMAGE=${10}

fail() { echo "diskrate-test: FAIL: $1" >&2; exit 1; }

mkdir -p "$DIR"
RAW="$DIR/raw.img"; EX="$DIR/exfat.img"; FAT="$DIR/fat.img"; LOG="$DIR/diskrate.log"
dd if=/dev/zero of="$RAW" bs=1048576 seek=255 count=1 2>/dev/null || fail "the raw disk"
"$MKEXFAT" --make "$DIR/exfat.vol" --sectors 522240 --offset 2048 >/dev/null || fail "the exFAT volume"
{ dd if=/dev/zero bs=1048576 count=1 2>/dev/null; cat "$DIR/exfat.vol"; } > "$EX" || fail "the exFAT disk"
rm -f "$DIR/exfat.vol"
"$MKMBR" --lba "$EX" 2048:522240:07 >/dev/null || fail "the exFAT disk's table"
dd if=/dev/zero of="$FAT" bs=1048576 seek=255 count=1 2>/dev/null || fail "the FAT32 disk"
"$MKMBR" --lba "$FAT" 2048:520192:0c >/dev/null || fail "the FAT32 disk's table"
mformat -i "$FAT@@1048576" -F -T 520192 -v RATEFAT :: || fail "mformat"

# The rows are typed as short lines, since the console takes a line of
# bounded length: two functions are defined on the machine first. `t NAME
# COMMAND` runs COMMAND through `eval` and prints `T_NAME SECONDS`, the
# machine's clock read before and after, or `T_NAME FAIL` when it failed;
# `moved BYTES COMMAND...` runs a dd that must report BYTES copied.
# shellcheck disable=SC2016
DEFS='now() { read -r u _ < /proc/uptime; echo $(( ${u%.*} * 100 + 1${u#*.} - 100 )); }; t() { s=$(now); if eval "$2"; then echo T_$1 $(( $(now) - s )); else echo T_$1 FAIL; fi; }; moved() { n=$1; shift; "$@" 2>/tmp/dd.err && grep -q "^$n bytes" /tmp/dd.err; }; c() { echo C_$1 $(awk "/^disk-commands /{c=\$2} /^disk-polls /{p=\$2} /^disk-wait-ticks /{w=\$2} /^cache-hits /{h=\$2} /^disk-quarantines /{q=\$2} END{print c, p, w, h, q}" /proc/disk); }; echo DEFS""DONE'
# shellcheck disable=SC2016
set -- \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" "expect=# " \
    "send=$DEFS" "expect=DEFSDONE" \
    "send=c boot0" "expect=C_boot0" \
    "send=t pipehash 'dd if=/dev/disk0 bs=1M count=64 2>/tmp/dd.err | sha256sum >/dev/null && grep -q \"^67108864 bytes\" /tmp/dd.err'" "expect=T_pipehash" \
    "send=t pipehash 'dd if=/dev/disk0 bs=1M count=64 2>/tmp/dd.err | sha256sum >/dev/null && grep -q \"^67108864 bytes\" /tmp/dd.err'" "expect=T_pipehash" \
    "send=t pipehash 'dd if=/dev/disk0 bs=1M count=64 2>/tmp/dd.err | sha256sum >/dev/null && grep -q \"^67108864 bytes\" /tmp/dd.err'" "expect=T_pipehash" \
    "send=t rawread 'moved 134217728 dd if=/dev/disk0 of=/dev/null bs=1M count=128'" "expect=T_rawread" \
    "send=mkdir -p /e /f && mount -t exfat /dev/disk1p1 /e && mount -t vfat /dev/disk2p1 /f && echo MOUNT\"\"ED" "expect=MOUNTED" \
    "send=c exwrite0" "expect=C_exwrite0" \
    "send=t exwrite 'moved 67108864 dd if=/dev/zero of=/e/big bs=1M count=64 && sync'" "expect=T_exwrite" \
    "send=c exwrite1" "expect=C_exwrite1" \
    "send=t exread 'moved 67108864 dd if=/e/big of=/dev/null bs=1M && moved 67108864 dd if=/e/big of=/dev/null bs=1M'" "expect=T_exread" \
    "send=c fatwrite0" "expect=C_fatwrite0" \
    "send=t fatwrite 'moved 67108864 dd if=/dev/zero of=/f/big bs=1M count=64 && sync'" "expect=T_fatwrite" \
    "send=c fatwrite1" "expect=C_fatwrite1" \
    "send=t fatread 'moved 67108864 dd if=/f/big of=/dev/null bs=1M && moved 67108864 dd if=/f/big of=/dev/null bs=1M'" "expect=T_fatread" \
    "send=c exsmall0" "expect=C_exsmall0" \
    "send=t exsmall 'moved 16777216 dd if=/dev/zero of=/e/small bs=4k count=4096 && sync'" "expect=T_exsmall" \
    "send=c exsmall1" "expect=C_exsmall1" \
    "send=c fatsmall0" "expect=C_fatsmall0" \
    "send=t fatsmall 'moved 16777216 dd if=/dev/zero of=/f/small bs=4k count=4096 && sync'" "expect=T_fatsmall" \
    "send=c fatsmall1" "expect=C_fatsmall1" \
    "send=c exfiles0" "expect=C_exfiles0" \
    "send=t exfiles 'i=0; n=0; while [ \$i -lt 200 ]; do dd if=/dev/disk0 of=/e/small-\$i bs=4k skip=\$i count=8 2>/dev/null && n=\$((n+1)); i=\$((i+1)); done; sync; [ \$n = 200 ] && [ \$(stat -c %s /e/small-199) = 32768 ]'" "expect=T_exfiles" \
    "send=c exfiles1" "expect=C_exfiles1" \
    "send=umount /e && umount /f && echo ALL\"\"DONE" "expect=ALLDONE"
"$HARNESS" --kernel "$KERNEL" --initrd "$OSREAL" \
    --qemu-arg=-cpu --qemu-arg=max --qemu-arg=-m --qemu-arg=512 --append "rach2.diskdiag" \
    --drive "file=$RAW,format=raw,if=ide,index=0,media=disk" \
    --drive "file=$EX,format=raw,if=ide,index=1,media=disk" \
    --drive "file=$FAT,format=raw,if=ide,index=2,media=disk" \
    --timeout 900 --steptmo 300 --ok "ALLDONE" --log "$LOG" "$@" > /dev/null 2>&1 \
    || fail "the boot did not reach its end; see $LOG"

# least NAME: the row's time in hundredths, the least of its runs where a
# row runs more than once; empty when the row's command failed.
least() {
    tr -d '\r' < "$LOG" | grep -a "^T_$1 [0-9]" | awk '{print $2}' | sort -n | head -1
}
# row NAME KIB TEXT: the row's time and rate, printed for the record; a
# row whose command failed has no time and fails, since its bytes were not
# moved. The second read of a file is served from the cache, so a read row
# times a cold read and a warm one together. The pipe row runs first in
# the boot and three times: after other programs have run and exited, a
# program's buffer may land in a frame that held code, which the emulator
# keeps on its slow store path (entry 26 of milestone 50), and in the fresh
# boot the three runs are the kernel's rate; the least is taken.
rc=0
row() {
    t=$(least "$1")
    [ -n "$t" ] || fail "no time for $1 in $LOG"
    echo "diskrate-test: $3: $(awk -v t="$t" -v k="$2" 'BEGIN {if (t > 0) printf "%.2f s, %.0f KiB/s", t / 100, k / t * 100; else printf "%.2f s", t / 100}')"
}
# sleeps FROM TO TEXT: between two counter reads, the status reads the
# driver made per command and the ticks its callers slept per command,
# each under its bound; a driver that spins or waits out a deadline is over
# by orders of magnitude.
sleeps() {
    before=$(tr -d '\r' < "$LOG" | grep -a "^C_$1 " | tail -1)
    after=$(tr -d '\r' < "$LOG" | grep -a "^C_$2 " | tail -1)
    [ -n "$before" ] && [ -n "$after" ] || fail "no counters for $1 or $2 in $LOG"
    cmds=$(( $(echo "$after" | awk '{print $2}') - $(echo "$before" | awk '{print $2}') ))
    polls=$(( $(echo "$after" | awk '{print $3}') - $(echo "$before" | awk '{print $3}') ))
    waits=$(( $(echo "$after" | awk '{print $4}') - $(echo "$before" | awk '{print $4}') ))
    [ "$cmds" -gt 0 ] || fail "no disk command between $1 and $2"
    if [ "$polls" -le "$(( cmds * 100 ))" ] && [ "$waits" -le "$(( cmds * 2 ))" ]; then
        echo "diskrate-test: $3: $cmds commands, $(( polls / cmds )) status reads and $(( waits / cmds )) wait ticks a command, under 100 and 2: OK"
    else
        echo "diskrate-test: FAIL: $3: $cmds commands, $(( polls / cmds )) status reads and $(( waits / cmds )) wait ticks a command, against 100 and 2" >&2
        rc=1
    fi
}
# waits FROM TO TEXT: between two counter reads on a device boot, the
# ticks the row's requests spent in flight per command and the requests a
# deadline quarantined. What these rows catch is a command that waits: a
# request that waits out a deadline spends three thousand ticks in flight
# by itself and is quarantined, where a USB or AHCI command the emulator
# answers spends none to a hundred, so the bound is the same on every
# machine and no machine's rate.
waits() {
    before=$(tr -d '\r' < "$LOG" | grep -a "^C_$1 " | tail -1)
    after=$(tr -d '\r' < "$LOG" | grep -a "^C_$2 " | tail -1)
    [ -n "$before" ] && [ -n "$after" ] || fail "no counters for $1 or $2 in $LOG"
    cmds=$(( $(echo "$after" | awk '{print $2}') - $(echo "$before" | awk '{print $2}') ))
    ticks=$(( $(echo "$after" | awk '{print $4}') - $(echo "$before" | awk '{print $4}') ))
    held=$(( $(echo "$after" | awk '{print $6}') - $(echo "$before" | awk '{print $6}') ))
    [ "$cmds" -gt 0 ] || fail "no disk command between $1 and $2"
    if [ "$ticks" -le "$(( cmds * 500 ))" ] && [ "$held" -eq 0 ]; then
        echo "diskrate-test: $3: $cmds commands, $(( ticks / cmds )) ticks in flight a command, none quarantined: OK"
    else
        echo "diskrate-test: FAIL: $3: $cmds commands, $(( ticks / cmds )) ticks in flight a command, $held quarantined" >&2
        rc=1
    fi
}
# counts NAME CMDS HITS TEXT: the disk commands and the cache hits the row
# took, from the counters read before and after it, each under its bound.
# The bounds are from the counts measured, with room above them, and a
# path that walks a chain from its start for every call, or writes each
# cluster's entry through, is over them by far.
counts() {
    before=$(tr -d '\r' < "$LOG" | grep -a "^C_$1""0 " | tail -1)
    after=$(tr -d '\r' < "$LOG" | grep -a "^C_$1""1 " | tail -1)
    [ -n "$before" ] && [ -n "$after" ] || fail "no counters for $1 in $LOG"
    cmds=$(( $(echo "$after" | awk '{print $2}') - $(echo "$before" | awk '{print $2}') ))
    hits=$(( $(echo "$after" | awk '{print $5}') - $(echo "$before" | awk '{print $5}') ))
    # A counter that did not move is an instrument that stopped, not a fast row.
    if [ "$cmds" -gt 0 ] && [ "$hits" -ge 0 ] && [ "$cmds" -le "$2" ] && [ "$hits" -le "$3" ]; then
        echo "diskrate-test: $4: $cmds disk commands under $2, $hits cache hits under $3: OK"
    else
        echo "diskrate-test: FAIL: $4: $cmds disk commands against $2, $hits cache hits against $3" >&2
        rc=1
    fi
}
row rawread 131072 "128 MiB read from the raw device"
row pipehash 65536 "64 MiB read raw through a pipe into sha256sum"
row exwrite 65536 "64 MiB written into a file on exFAT and synced"
row exread 131072 "the exFAT file read twice, cold and from the cache"
row fatwrite 65536 "64 MiB written into a file on FAT32 and synced"
row fatread 131072 "the FAT32 file read twice, cold and from the cache"
row exsmall 16384 "16 MiB written on exFAT in 4 KiB calls"
row fatsmall 16384 "16 MiB written on FAT32 in 4 KiB calls"
row exfiles 6400 "two hundred 32 KiB files written on exFAT"
sleeps boot0 exfiles1 "the boot's driver waits"
# The hits' bounds are three times the counts measured on 2026-09-10 (exFAT
# write 131 thousand, FAT32 write 427 thousand, exFAT small 37 thousand,
# FAT32 small 115 thousand, exFAT files 21 thousand) and far under the
# counts with a chain hint dropped (2.7 million, 10 million, 8.4 million,
# 17 million and 356 thousand), where the driver walks a chain from its
# start for every call. The commands' bounds are twice the counts measured
# (4618, 7388, 40970, 49764 and 17727) and under the counts with exFAT's
# run entries written through one at a time.
counts exwrite 9300 400000 "the exFAT write's counts"
counts fatwrite 14800 1300000 "the FAT32 write's counts"
counts exsmall 82000 120000 "the exFAT small writes' counts"
counts fatsmall 99600 350000 "the FAT32 small writes' counts"
counts exfiles 35500 65000 "the exFAT files' counts"

# ---- the stick: 4 MiB written raw and read raw in 32 KiB requests, from
#      the recovery kernel, each timed and each exactly 128 commands ----
STICK="$DIR/stick.img"; SLOG="$DIR/stick.log"
dd if=/dev/zero of="$STICK" bs=1048576 count=16 2>/dev/null || fail "the stick image"
# shellcheck disable=SC2016
SDEFS='now() { read -r u _ < /proc/uptime; echo $(( ${u%.*} * 100 + 1${u#*.} - 100 )); }; t() { s=$(now); if eval "$2"; then echo T_$1 $(( $(now) - s )); else echo T_$1 FAIL; fi; }; moved() { n=$1; shift; "$@" 2>/tmp/dd.err && grep -q "^$n bytes" /tmp/dd.err; }; c() { echo C_$1 $(awk "/^disk-commands /{c=\$2} /^disk-polls /{p=\$2} /^disk-wait-ticks /{w=\$2} /^cache-hits /{h=\$2} /^disk-quarantines /{q=\$2} END{print c, p, w, h, q}" /proc/disk); }; echo DEFS""DONE'
"$HARNESS" --kernel "$RKERNEL" --initrd "$RIMAGE" \
    --qemu-arg=-cpu --qemu-arg=max --qemu-arg=-m --qemu-arg=512 \
    --qemu-arg=-device --qemu-arg=qemu-xhci,id=xhci \
    --drive "if=none,id=stick,format=raw,file=$STICK" \
    --qemu-arg=-device --qemu-arg=usb-storage,bus=xhci.0,drive=stick,id=st0 \
    --timeout 600 --steptmo 300 --ok "STICKDONE" --log "$SLOG" \
    "expect=recovery image" "expect=~ #" \
    "send=$SDEFS" "expect=DEFSDONE" \
    "send=yes RATE-STICK-PATTERN-LINE | head -c 32768 > /tmp/base; p() { printf PIECE-%03d- \$1; dd if=/tmp/base bs=32758 count=1 2>/dev/null; }; echo PAT\"\"DONE" "expect=PATDONE" \
    "send=c stickwrite0" "expect=C_stickwrite0" \
    "send=t stickwrite 'i=0; while [ \$i -lt 128 ]; do p \$i > /tmp/blk; moved 32768 dd if=/tmp/blk of=/dev/disk0 bs=32k seek=\$((32 + i)) count=1 || break; i=\$((i + 1)); done; sync; [ \$i = 128 ]'" "expect=T_stickwrite" \
    "send=c stickwrite1" "expect=C_stickwrite1" \
    "send=t stickread 'dd if=/dev/disk0 bs=32k skip=32 count=128 2>/tmp/dd.err | cksum > /tmp/sum && grep -q \"^4194304 bytes\" /tmp/dd.err'" "expect=T_stickread" \
    "send=c stickread1" "expect=C_stickread1" \
    "send=t stickbig 'dd if=/dev/disk0 bs=512k skip=2 count=8 2>/tmp/dd.err | cksum > /tmp/sumbig && grep -q \"^4194304 bytes\" /tmp/dd.err'" "expect=T_stickbig" \
    "send=c stickbig1" "expect=C_stickbig1" \
    "send=i=0; while [ \$i -lt 128 ]; do p \$i; i=\$((i + 1)); done | cksum > /tmp/want; cmp /tmp/sum /tmp/want && echo SAME; cmp /tmp/sumbig /tmp/want && echo SAME\"\"BIG; echo STICK\"\"DONE" "expect=STICKDONE" > /dev/null 2>&1 \
    || fail "the stick boot did not reach its end; see $SLOG"
tr -d '\r' < "$SLOG" | grep -aqx "SAME" || { echo "diskrate-test: FAIL: the stick did not read back what was written" >&2; rc=1; }
# exact NAME FROM TO N TEXT: the commands between two counter reads, exactly N.
exact() {
    before=$(tr -d '\r' < "$SLOG" | grep -a "^C_$2 " | tail -1 | awk '{print $2}')
    after=$(tr -d '\r' < "$SLOG" | grep -a "^C_$3 " | tail -1 | awk '{print $2}')
    [ -n "$before" ] && [ -n "$after" ] || fail "no counters for $1 in $SLOG"
    cmds=$(( after - before ))
    if [ "$cmds" -eq "$4" ]; then
        echo "diskrate-test: $5: $cmds commands, exactly $4: OK"
    else
        echo "diskrate-test: FAIL: $5: $cmds commands, not $4" >&2
        rc=1
    fi
}
LOG="$SLOG"
waits stickwrite0 stickwrite1 "4 MiB written raw to the stick in 32 KiB requests"
waits stickwrite1 stickread1 "4 MiB read raw from the stick in 32 KiB requests"
exact stickwrite stickwrite0 stickwrite1 128 "the stick write's data commands"
exact stickread stickwrite1 stickread1 128 "the stick read's data commands"
waits stickread1 stickbig1 "4 MiB read raw from the stick in 512 KiB requests"
exact stickbig stickread1 stickbig1 8 "the stick read's data commands at 512 KiB a request"
tr -d '\r' < "$SLOG" | grep -aqx "SAMEBIG" || { echo "diskrate-test: FAIL: the 512 KiB reads did not read what the 32 KiB reads read" >&2; rc=1; }
for r in stickwrite stickread; do
    echo "diskrate-test: $r: $(tr -d '\r' < "$SLOG" | grep -a "^T_$r [0-9]" | tail -1 | awk '{printf "%.2f s, %d KB/s", $2 / 100, ($2 > 0 ? 409600 / $2 : 409600)}')"
done

# ---- the AHCI disk, whose requests are the block layer's whole limit: 32
#      MiB written in 1 MiB calls and read back in 1 MiB and in 32 KiB
#      calls, the commands counted per call size and the bytes the same ----
AHCI="$DIR/ahci.img"; ALOG="$DIR/ahci.log"
dd if=/dev/zero of="$AHCI" bs=1048576 count=64 2>/dev/null || fail "the AHCI image"
"$HARNESS" --kernel "$RKERNEL" --initrd "$RIMAGE" \
    --qemu-arg=-cpu --qemu-arg=max --qemu-arg=-m --qemu-arg=512 --append "rach2.disk=ahci" \
    --drive "file=$AHCI,format=raw,if=none,id=drv0" \
    --qemu-arg=-device --qemu-arg=ich9-ahci,id=ahci0 \
    --qemu-arg=-device --qemu-arg=ide-hd,drive=drv0,bus=ahci0.0 \
    --timeout 900 --steptmo 300 --ok "AHCIDONE" --never "PATFAIL" --log "$ALOG" \
    "expect=recovery image" "expect=~ #" \
    "send=$SDEFS" "expect=DEFSDONE" \
    "send=yes RATE-AHCI-PATTERN-LINE | head -c 32768 > /tmp/b32; i=0; while [ \$i -lt 32 ]; do dd if=/tmp/b32 of=/dev/disk0 bs=32k seek=\$((128 + i)) count=1 2>/dev/null || break; i=\$((i + 1)); done; sync; if [ \$i = 32 ]; then echo PAT\"\"DONE; else echo PAT\"\"FAIL; fi" "expect=PATDONE" \
    "send=c ahciwrite0" "expect=C_ahciwrite0" \
    "send=t ahciwrite 'i=0; while [ \$i -lt 32 ]; do moved 1048576 dd if=/dev/disk0 of=/dev/disk0 bs=1M skip=4 seek=\$((8 + i)) count=1 || break; i=\$((i + 1)); done; sync; [ \$i = 32 ]'" "expect=T_ahciwrite" \
    "send=c ahciwrite1" "expect=C_ahciwrite1" \
    "send=t ahciread 'dd if=/dev/disk0 bs=1M skip=8 count=32 2>/tmp/dd.err | cksum > /tmp/suma && grep -q \"^33554432 bytes\" /tmp/dd.err'" "expect=T_ahciread" \
    "send=c ahciread1" "expect=C_ahciread1" \
    "send=t ahcismall 'dd if=/dev/disk0 bs=32k skip=256 count=1024 2>/tmp/dd.err | cksum > /tmp/sumb && grep -q \"^33554432 bytes\" /tmp/dd.err'" "expect=T_ahcismall" \
    "send=c ahcismall1" "expect=C_ahcismall1" \
    "send=i=0; while [ \$i -lt 1024 ]; do cat /tmp/b32; i=\$((i + 1)); done | cksum > /tmp/wanta; cmp /tmp/suma /tmp/wanta && cmp /tmp/sumb /tmp/wanta && echo SAME\"\"AHCI; echo AHCI\"\"DONE" "expect=AHCIDONE" > /dev/null 2>&1 \
    || fail "the AHCI boot did not reach its end; see $ALOG"
LOG="$ALOG"
waits ahciwrite0 ahciwrite1 "32 MiB written to the AHCI disk in 1 MiB requests"
waits ahciwrite1 ahciread1 "32 MiB read from the AHCI disk in 1 MiB requests"
waits ahciread1 ahcismall1 "32 MiB read from the AHCI disk in 32 KiB requests"
for r in ahciwrite ahciread ahcismall; do
    echo "diskrate-test: $r: $(tr -d '\r' < "$ALOG" | grep -a "^T_$r [0-9]" | tail -1 | awk '{printf "%.2f s", $2 / 100}')"
done
SLOG="$ALOG"
exact ahciwrite ahciwrite0 ahciwrite1 96 "the AHCI copy's commands, a read, a write and a flush per mebibyte"
exact ahciread ahciwrite1 ahciread1 32 "the AHCI read's commands at 1 MiB a request"
exact ahcismall ahciread1 ahcismall1 1024 "the AHCI read's commands at 32 KiB a request"
tr -d '\r' < "$ALOG" | grep -aqx "SAMEAHCI" || { echo "diskrate-test: FAIL: the AHCI disk did not read back what was written" >&2; rc=1; }

# ---- the placement experiment: the pipe read into a page that holds code
#      and into one that does not, each three times ----
PLOG="$DIR/place.log"
"$HARNESS" --kernel "$KERNEL" --initrd "$TESTIMAGE" \
    --qemu-arg=-cpu --qemu-arg=max \
    --timeout 900 --steptmo 600 --ok "PLACEPROBE OK" --never "PLACEPROBE FAIL" --log "$PLOG" \
    "expect=Type 'help'" "send=exec /placeprobe" "expect=PLACEPROBE OK" > /dev/null 2>&1 \
    || fail "the placement boot did not reach its end; see $PLOG"
echo "diskrate-test: the pipe row's three times: $(tr -d '\r' < "$DIR/diskrate.log" | grep -a "^T_pipehash [0-9]" | awk '{printf "%.2f s ", $2 / 100}')"
# Which processes were runnable twenty seconds into the first pipe run: a
# runnable process beside the pipe's two costs every hand-off a quantum
# (entry 26), so the record says who it was.
echo "diskrate-test: $(tr -d '\r' < "$PLOG" | grep -a "^PLACEPROBE code")"
# The far runs are printed beside the near ones: the experiment's record,
# not a judgement, since what it measures is the emulator's store path.
n=0
for t in $(tr -d '\r' < "$PLOG" | grep -a "^PLACEPROBE FAR" | awk '{print $3}'); do
    n=$((n + 1))
    echo "diskrate-test: far run $n: $t ms"
done
[ "$n" -eq 3 ] || { echo "diskrate-test: FAIL: $n far runs, not 3" >&2; rc=1; }
echo "diskrate-test: the near runs, the emulator's cost of a buffer in a code page: $(tr -d '\r' < "$PLOG" | grep -a "^PLACEPROBE NEAR" | awk '{printf "%s ms ", $3}')"
[ "$rc" -eq 0 ] || fail "a row is over its bound"
echo "diskrate-test: PASS"
