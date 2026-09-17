#!/bin/sh
# diskrate.sh: the rate of the storage paths, each timed in the machine and
# held under a multiple of the raw device read the same boot measures: a
# file written and read back on exFAT and on FAT32, two hundred small
# files, sixteen mebibytes in 4 KiB calls, and a raw read through a pipe
# into a hash. The boot asks for the whole-disk nodes, which the ordinary
# kernel publishes only then and lets only the recovery build write, so
# the raw device is read and not written here. One boot of the os-real
# image with three IDE disks: a raw one, an exFAT volume and a FAT32 one.
#
# A row's time is judged against the raw read of that boot, not against
# seconds written for one machine, so the suite passes on any host whose
# emulator is slower or faster as a whole: the raw read is the machine's
# rate through the block layer, and a file system path costs a multiple of
# it that does not depend on the machine. The multiples have room of two
# and a half times over the worst of the two hosts measured on 2026-09-18,
# an Apple M-series Mac and a Ryzen under WSL, both emulating; the worst
# of the two was 0.74 for the pipe, 0.61 for the exFAT write, 1.03 and
# 1.04 for the two reads, 1.16 for the FAT32 write, 2.59 and 3.39 for the
# 4 KiB writes and 1.24 for the files. So a path that waits or spins fails
# by an order of magnitude while a slow host passes: a delay of 50 ms on
# each FAT32 write, 4096 of them, puts that row past twelve on both. A
# path that falls back to a page a request, a walk of the chain per block
# or a write through per cluster is caught by the command and cache-hit
# counts below, which do not depend on the machine at all. Times are read
# from /proc/uptime in hundredths of a second.
#
# A second boot, of the recovery kernel, which alone writes a whole-disk
# node, times a 4 MiB raw write and raw read of a USB stick in 32 KiB
# requests, the write as 128 dd calls of one piece each since the recovery
# image cannot hold 4 MiB, each piece carrying its index, and requires each
# direction to be exactly 128 commands on the stick.
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
DEFS='now() { read -r u _ < /proc/uptime; echo $(( ${u%.*} * 100 + 1${u#*.} - 100 )); }; t() { s=$(now); if eval "$2"; then echo T_$1 $(( $(now) - s )); else echo T_$1 FAIL; fi; }; moved() { n=$1; shift; "$@" 2>/tmp/dd.err && grep -q "^$n bytes" /tmp/dd.err; }; c() { echo C_$1 $(grep -E "^(disk-commands|cache-hits) " /proc/disk | head -2 | awk "{printf \"%s \", \$2}"); }; echo DEFS""DONE'
# shellcheck disable=SC2016
set -- \
    "expect=rach2 login:" "send=root" "expect=Password:" "send=$PASSWORD" "expect=# " \
    "send=$DEFS" "expect=DEFSDONE" \
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
# row NAME MULTIPLE TEXT: the row's time against MULTIPLE times the raw
# read of the same boot; a row whose command failed has no time and fails.
# The second read of a file is served from the cache, so a read row times a
# cold read and a warm one together. The pipe row runs first in the boot
# and three times: after other programs have run and exited, a program's
# buffer may land in a frame that held code, which the emulator keeps on
# its slow store path (entry 26 of milestone 50), and in the fresh boot the
# three runs are the kernel's rate; the least is taken.
rc=0
row() {
    t=$(least "$1")
    [ -n "$t" ] || fail "no time for $1 in $LOG"
    if awk -v t="$t" -v b="$BASE" -v m="$2" 'BEGIN {exit !(t <= m * b)}'; then
        echo "diskrate-test: $3: $(awk -v t="$t" -v b="$BASE" -v m="$2" 'BEGIN {printf "%.2f s, %.2f times the raw read, under %s", t / 100, t / b, m}'): OK"
    else
        echo "diskrate-test: FAIL: $3: $(awk -v t="$t" -v b="$BASE" -v m="$2" 'BEGIN {printf "%.2f s, %.2f times the raw read, over %s", t / 100, t / b, m}')" >&2
        rc=1
    fi
}
# floor NAME SECONDS TEXT: a device row held under an absolute time. The
# device boots have no raw read to measure against, and what these rows
# catch is a command that waits: the emulator answers a USB or AHCI command
# within milliseconds on every host measured, so a bound of a second a
# command is two orders of magnitude of room and not a machine's rate.
floor() {
    t=$(least "$1")
    [ -n "$t" ] || fail "no time for $1 in $LOG"
    if [ "$t" -le "$(( $2 * 100 ))" ]; then
        echo "diskrate-test: $3: $(awk -v t="$t" 'BEGIN {printf "%.2f", t / 100}') s, under $2 s: OK"
    else
        echo "diskrate-test: FAIL: $3: $(awk -v t="$t" 'BEGIN {printf "%.2f", t / 100}') s, over $2 s" >&2
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
    hits=$(( $(echo "$after" | awk '{print $3}') - $(echo "$before" | awk '{print $3}') ))
    # A counter that did not move is an instrument that stopped, not a fast row.
    if [ "$cmds" -gt 0 ] && [ "$hits" -ge 0 ] && [ "$cmds" -le "$2" ] && [ "$hits" -le "$3" ]; then
        echo "diskrate-test: $4: $cmds disk commands under $2, $hits cache hits under $3: OK"
    else
        echo "diskrate-test: FAIL: $4: $cmds disk commands against $2, $hits cache hits against $3" >&2
        rc=1
    fi
}
# The raw read is the baseline. It must have moved its bytes and taken
# time, since a zero would make every multiple pass; there is no rate it
# is held to, because it is the machine's.
BASE=$(least rawread)
[ -n "$BASE" ] || fail "no time for rawread in $LOG"
[ "$BASE" -gt 0 ] || fail "the raw read took no measurable time, so nothing can be judged against it"
echo "diskrate-test: 128 MiB read from the raw device in $(awk -v b="$BASE" 'BEGIN {printf "%.2f s, %.0f", b / 100, 131072 / b * 100}') KiB/s: the boot's baseline"
row pipehash 2 "64 MiB read raw through a pipe into sha256sum"
row exwrite 2 "64 MiB written into a file on exFAT and synced"
row exread 3 "the exFAT file read twice, cold and from the cache"
row fatwrite 4 "64 MiB written into a file on FAT32 and synced"
row fatread 3 "the FAT32 file read twice, cold and from the cache"
row exsmall 8 "16 MiB written on exFAT in 4 KiB calls"
row fatsmall 12 "16 MiB written on FAT32 in 4 KiB calls"
row exfiles 4 "two hundred 32 KiB files written on exFAT"
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
SDEFS='now() { read -r u _ < /proc/uptime; echo $(( ${u%.*} * 100 + 1${u#*.} - 100 )); }; t() { s=$(now); if eval "$2"; then echo T_$1 $(( $(now) - s )); else echo T_$1 FAIL; fi; }; moved() { n=$1; shift; "$@" 2>/tmp/dd.err && grep -q "^$n bytes" /tmp/dd.err; }; c() { echo C_$1 $(grep -E "^(disk-commands|cache-hits) " /proc/disk | head -2 | awk "{printf \"%s \", \$2}"); }; echo DEFS""DONE'
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
floor stickwrite 120 "4 MiB written raw to the stick in 32 KiB requests"
floor stickread 120 "4 MiB read raw from the stick in 32 KiB requests"
exact stickwrite stickwrite0 stickwrite1 128 "the stick write's data commands"
exact stickread stickwrite1 stickread1 128 "the stick read's data commands"
floor stickbig 120 "4 MiB read raw from the stick in 512 KiB requests"
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
floor ahciwrite 120 "32 MiB written to the AHCI disk in 1 MiB requests"
floor ahciread 120 "32 MiB read from the AHCI disk in 1 MiB requests"
floor ahcismall 120 "32 MiB read from the AHCI disk in 32 KiB requests"
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
# A far run reads half the pipe row's bytes through the same pipe and
# hashes nothing, so it is held under the pipe row's own time, the least
# of the three from the os-real boot on this machine.
LOG="$DIR/diskrate.log"
PIPE=$(least pipehash)
[ -n "$PIPE" ] || fail "no time for pipehash in $LOG"
n=0
for t in $(tr -d '\r' < "$PLOG" | grep -a "^PLACEPROBE FAR" | awk '{print $3}'); do
    n=$((n + 1))
    if [ "$t" -le "$(( PIPE * 10 ))" ]; then
        echo "diskrate-test: far run $n: $t ms, under the pipe row's $(( PIPE * 10 )) ms: OK"
    else
        echo "diskrate-test: FAIL: far run $n: $t ms, over the pipe row's $(( PIPE * 10 )) ms" >&2
        rc=1
    fi
done
[ "$n" -eq 3 ] || { echo "diskrate-test: FAIL: $n far runs, not 3" >&2; rc=1; }
echo "diskrate-test: the near runs, the emulator's cost of a buffer in a code page: $(tr -d '\r' < "$PLOG" | grep -a "^PLACEPROBE NEAR" | awk '{printf "%s ms ", $3}')"
[ "$rc" -eq 0 ] || fail "a row is over its bound"
echo "diskrate-test: PASS"
