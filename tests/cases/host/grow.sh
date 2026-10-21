#!/bin/sh
#
# grow.sh: the program of the host cases of the grow suite,
# tests/cases/machine.grow.json: the parts of growing a root that are
# checked without booting Rach2.
#
# Each run is one case, named by its first argument:
#
#   measure  checkers/grow_measure.awk, which judges growroot's --measure
#            lines in the measured grow's console, on measures made up here.
#   check    check_plan and check_boot_copy of tools/dev/machine_grow.sh, on
#            real seed file systems and a whole boot partition.
#   boot     prepare_boot and keep_recovery of the same script, on two FAT
#            boot partitions, with bootctl, growboot and reflash built here.
#   space    the work directory, the log writes and the plan check of the
#            script, through its own functions.
#   memory   growroot built for the host with a requirement no machine
#            meets, which must refuse the grow by name.
#
# check, boot and space take as a second argument a copy of machine_grow.sh
# to test in place of the tree's, as the fault proofs of tests/proofs give
# one. Each copies the functions it tests out of the script and replaces only
# the guest's paths and the disk's reads and writes, so the decisions tested
# are the script's own.
#
# It writes the case's report records on its standard output, an assertion
# for each check (tools/etude/Readme.md, Report records), with the lines a
# person reads between them, and exits 0 when every assertion passed and 1
# when one failed. The manifest says what each case and each assertion
# checks.
#
# etude runs it in the case's workspace, with ETUDE_TREE naming the root of a
# built tree, to which it changes first, and TMPDIR a scratch directory of
# the case's own. Without TMPDIR its scratch goes under the tree's
# build/tests, since the seeds and disks take some hundreds of mebibytes.
#
# Usage, in the root of a built tree, or anywhere with ETUDE_TREE set to it:
#     sh tests/cases/host/grow.sh CASE [SCRIPT]
#
# The private directory, whose path the first line prints, is removed by a
# run that passes or is stopped by a signal, and kept by one that fails.
#

# shellcheck disable=SC2329 # the cases are called by name, at the end
set -u

CASE=${1-}
SCRIPT=${2-}
[ "$#" -ge 1 ] && [ "$#" -le 2 ] && [ -n "$CASE" ] || {
    echo "grow: usage: grow.sh CASE [SCRIPT]" >&2
    exit 2
}
case "$SCRIPT" in
    '' | /*) ;;
    *) SCRIPT=$(pwd)/$SCRIPT ;;
esac
if [ -n "${ETUDE_TREE-}" ]; then
    cd "$ETUDE_TREE" || exit 2
fi
[ -f tests/registry ] && [ -f tools/dev/machine_grow.sh ] || {
    echo "grow: run it in the root of the tree" >&2
    exit 2
}
TREE=$(pwd -P)
SCRIPT=${SCRIPT:-$TREE/tools/dev/machine_grow.sh}

unset W
status=0
trap 'status=$?; [ "$status" -ne 0 ] || rm -rf ${W:+"$W"}; exit "$status"' EXIT
trap 'rm -rf ${W:+"$W"}; exit 129' HUP
trap 'rm -rf ${W:+"$W"}; exit 130' INT
trap 'rm -rf ${W:+"$W"}; exit 143' TERM
SCRATCH=${TMPDIR:-$TREE/build/tests}
if ! mkdir -p "$SCRATCH" || ! W=$(umask 077; mktemp -d "$SCRATCH/grow.XXXXXX") ||
    ! W=$(cd "$W" && pwd -P); then
    echo "grow: cannot create a temporary directory" >&2
    exit 1
fi
printf 'grow: work: %s\n' "$W"
export W TREE
# shellcheck source=tools/setup/e2fsprogs_path.sh
. "$TREE/tools/setup/e2fsprogs_path.sh"

# ---- The report. ----

RECORDS=0
FAILED=""

# Write the report record WORDS, which fits in one write of 255 bytes.
record() {
    printf '@etude 1 %s\n' "$*"
}

# Make TEXT printable ASCII of at most 160 bytes, for a record's detail.
clip() {
    printf '%s' "$1" | tr -c ' -~' '?' | cut -c1-160
}

# pass ID: report that the assertion ID passed.
pass() {
    record "assert $1 pass"
    RECORDS=$((RECORDS + 1))
}

# flunk ID DETAIL: report that the assertion ID failed, with DETAIL. The
# functions of machine_grow.sh call a `fail` of their own, which the rows
# below define, so the report's is named otherwise.
flunk() {
    record "assert $1 fail $(clip "$2")"
    RECORDS=$((RECORDS + 1))
    FAILED="$FAILED $1"
}

# judge ID DETAIL COMMAND...: report the assertion ID passed when COMMAND
# succeeds, and failed with DETAIL otherwise.
judge() {
    _id=$1
    _detail=$2
    shift 2
    if "$@"; then
        pass "$_id"
    else
        flunk "$_id" "$_detail"
    fi
}

# broken DETAIL ID...: report each assertion ID failed with DETAIL, for a
# case whose setup failed before it could check them.
broken() {
    _detail=$1
    shift
    echo "grow: $_detail" >&2
    for _id in "$@"; do
        flunk "$_id" "$_detail"
    done
}

# ---- Rows run by machine_grow.sh's own functions. ----

# The rows of the check and boot cases depend on one another, as the
# script's steps do, so each set of rows runs in a new shell with errexit,
# from ROWS, and stops at the first that fails. `begin NAME` and `passed
# NAME` mark a row in the progress file, and `fail TEXT`, which the copied
# functions call too, prints TEXT and ends the shell.
cat > "$W/rowlib.sh" <<'EOF'
begin() { printf 'begin %s\n' "$1" >> "$progress"; }
passed() { printf 'pass %s\n' "$1" >> "$progress"; }
fail() { echo "grow: failed: $*" >&2; exit 1; }
EOF

# rows_run TAG FILE ARGS...: run the rows of FILE in a new shell, its output
# in TAG.log and its progress in TAG.progress.
rows_run() {
    _tag=$1
    shift
    : > "$W/$_tag.progress"
    progress="$W/$_tag.progress" sh -eu -c \
        '. "$W/rowlib.sh"; rows=$1; shift; . "$rows"' sh "$@" > "$W/$_tag.log" 2>&1 || :
}

# row_result TAG ROW: print nothing and succeed when ROW of TAG passed;
# otherwise print why it did not.
row_result() {
    if grep -qx "pass $2" "$W/$1.progress"; then
        return 0
    fi
    if grep -qx "begin $2" "$W/$1.progress"; then
        _why=$(sed -n 's/^grow: failed: //p' "$W/$1.log" | tail -1)
        printf '%s\n' "${_why:-the row ended without a result; see $W/$1.log}"
    else
        _last=$(sed -n 's/^begin //p' "$W/$1.progress" | tail -1)
        printf 'not reached: the row %s failed\n' "${_last:-before the first}"
    fi
    return 1
}

# extract NAMES OUT SED...: copy the functions NAMES, a pattern of names
# separated by |, whole out of the script under test into OUT, edited by the
# sed expressions SED.
extract() {
    _names=$1
    _out=$2
    shift 2
    awk -v names="^($_names)\\\\(\\\\)" '
        $0 ~ names {copy = 1}
        copy {print}
        copy && /^}/ {copy = 0}
    ' "$W/source.sh" > "$W/extracted.sh" && sed "$@" "$W/extracted.sh" > "$_out"
}

# ---- The cases. ----

# The checker of the measured grow's console accepts the measures of a grow
# whose phases each took less than a clock tick, and refuses measures whose
# counts are all zero, a phase's count that differs from its witness's, a
# missing witness line, a duplicated one, and phases out of order.
case_measure() {
    awk 'BEGIN {
        split("table check-readonly check-repair resize", phase, " ")
        split("100 105 105 112 112 123 123 140", c, " ")
        split("20 21 21 21 21 23 23 26", f, " ")
        print "grow-witness: whole before disk-commands 98 disk-flushes 20"
        for (i = 1; i <= 8; i++) {
            p = phase[int((i + 1) / 2)]
            e = i % 2 ? "before" : "after"
            printf "growroot: measure: %s %s monotonic-us 1000000 disk-commands %d disk-flushes %d\n",
                p, e, c[i], f[i]
            if (i >= 3)
                printf "grow-witness: %s %s disk-commands %d disk-flushes %d\n", p, e, c[i], f[i]
        }
        print "grow-witness: whole after disk-commands 140 disk-flushes 26"
    }' > "$W/valid-sub-tick"
    awk '{for (i = 1; i <= NF; i++) if ($i ~ /^[0-9]+$/) $i = 0; print}' \
        "$W/valid-sub-tick" > "$W/all-zero"
    sed 's/monotonic-us 1000000 disk-commands 112/monotonic-us 1000000 disk-commands 113/g' \
        "$W/valid-sub-tick" > "$W/wrong-phase"
    sed 1d "$W/valid-sub-tick" > "$W/missing-witness"
    { cat "$W/valid-sub-tick"; tail -1 "$W/valid-sub-tick"; } > "$W/duplicate-witness"
    sed 's/check-readonly before/check-repair before/g' "$W/valid-sub-tick" > "$W/wrong-order"
    for _name in valid-sub-tick all-zero wrong-phase missing-witness duplicate-witness wrong-order; do
        _want=1
        [ "$_name" != valid-sub-tick ] || _want=0
        _got=0
        awk -f tests/system/checkers/grow_measure.awk "$W/$_name" > "$W/$_name.log" 2>&1 || _got=$?
        judge "$_name" "the checker ended with status $_got, not $_want" [ "$_got" = "$_want" ]
    done
}

# check_plan must pass on the ranges machine_grow_plan.sh makes for an 8 MiB
# seed and a 1 GiB seed grown to the Samsung's size on an MBR, and the 1 GiB
# seed on a GUID table, and fail when the last range starts one length later;
# check_boot_copy must pass on an exact copy of the boot partition and fail
# on one whose last byte differs, with SHA256SUMS made again over it.
case_check() {
    _ids="plan-small ranges-small plan-samsung ranges-samsung plan-samsung-gpt ranges-samsung-gpt
          boot-copy boot-corrupt"
    # shellcheck disable=SC2086
    if ! cp "$SCRIPT" "$W/source.sh"; then
        broken "cannot read $SCRIPT" $_ids
        return
    fi
    # shellcheck disable=SC2086
    if ! cc -O2 -Wall -Wextra -o "$W/growset" tools/inspect/growset.c > "$W/cc.log" 2>&1 ||
        ! cc -O2 -Wall -Wextra -I utils -o "$W/reflash" utils/reflash.c utils/sectorcopy.c \
            utils/sha256.c >> "$W/cc.log" 2>&1; then
        broken "growset or reflash does not build on the host; see $W/cc.log" $_ids
        return
    fi
    for _profile in small samsung; do
        mkdir "$W/$_profile"
        if [ "$_profile" = small ]; then _size=8; _group=1024; else _size=1024; _group=32768; fi
        if ! dd if=/dev/zero of="$W/$_profile/seed.ext2" bs=1048576 count=0 seek="$_size" \
            2>"$W/$_profile/dd.log" ||
            ! E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -g "$_group" -I 128 \
                -O sparse_super,large_file,filetype,resize_inode,^dir_index,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize \
                "$W/$_profile/seed.ext2" > "$W/$_profile/mkfs.log" 2>&1; then
            # shellcheck disable=SC2086
            broken "the $_profile seed cannot be made; see $W/$_profile/mkfs.log" $_ids
            return
        fi
    done
    mkdir "$W/saved"
    # The last boot byte is nonzero, so a read of only the header cannot pass.
    dd if=/dev/zero of="$W/disk" bs=1048576 count=65 2>"$W/disk.log"
    printf x | dd of="$W/disk" bs=1 seek=68157439 conv=notrunc 2>>"$W/disk.log"
    dd if="$W/disk" of="$W/saved/boot.original" bs=1048576 skip=1 count=64 2>>"$W/disk.log"
    # hash_slice reads extents through the native reflash, and check_plan
    # runs the native growset in place of the guest's.
    extract 'hash_file|hash_slice|check_plan|check_boot_copy' "$W/checks.sh" \
        "s|/bin/growset|$W/growset|g"
    cat > "$W/rows.sh" <<'EOF'
reflash() { "$W/reflash" "$@"; }
. "$W/checks.sh"
work=$W
start=266240
for profile in small samsung samsung-gpt; do
    begin "plan-$profile"
    store="$W/${profile%-gpt}"
    case "$profile" in
        small) sectors=397312; table=mbr; table_sectors=8; disk_id=1acef00d ;;
        samsung) sectors=488397168; table=mbr; table_sectors=8; disk_id=1acef00d ;;
        samsung-gpt)
            sectors=488397168; table=gpt; table_sectors=40
            disk_id=1acef00d-0f1e-4d2c-8b3a-c0ffee000001
            ;;
    esac
    sh "$TREE/tools/dev/machine_grow_plan.sh" "$store/seed.ext2" "$sectors" GROWTEST \
        "$disk_id" "$W/$profile.plan" > "$W/$profile.plan.log" 2>&1 \
        || fail "machine_grow_plan.sh made no plan for $profile; see $W/$profile.plan.log"
    read -r _ _ _ seed count _ seed_hash < "$W/$profile.plan/grow.plan"
    cp "$W/$profile.plan/ranges.txt" "$W/ranges.txt"
    check_plan
    passed "plan-$profile"
    begin "ranges-$profile"
    cp "$W/ranges.txt" "$W/$profile.ranges"
    # This mutation keeps the byte total, seed coverage and bounds.
    awk 'NR == FNR {n++; next} FNR == n {$1 += $2} {print}' \
        "$W/$profile.ranges" "$W/$profile.ranges" > "$W/ranges.txt"
    if (check_plan) > "$W/$profile.shifted.log" 2>&1; then
        fail "check_plan accepted the $profile plan with its last range moved"
    fi
    grep -q 'ranges differ from the seed and target geometry' "$W/$profile.shifted.log" \
        || fail "check_plan refused the moved $profile range without saying the ranges differ"
    passed "ranges-$profile"
done
saved="$W/saved"
disk="$W/disk"
begin boot-copy
cp "$saved/boot.original" "$saved/boot.img"
check_boot_copy
passed boot-copy
begin boot-corrupt
printf y | dd of="$saved/boot.img" bs=1 seek=67108863 conv=notrunc 2>"$W/corrupt.log"
# The corrupted copy gets a matching SHA256SUMS, as `save` writes one.
(cd "$saved" && sha256sum boot.img > SHA256SUMS && sha256sum -c SHA256SUMS) > "$W/sums.log"
if (check_boot_copy) > "$W/boot.log" 2>&1; then
    fail 'check_boot_copy accepted a boot copy whose last byte differs'
fi
grep -q 'the boot backup differs from the disk' "$W/boot.log" \
    || fail 'check_boot_copy refused the changed copy without saying it differs'
passed boot-corrupt
EOF
    rows_run check "$W/rows.sh"
    for _id in $_ids; do
        _why=$(row_result check "$_id")
        judge "$_id" "$_why" [ -z "$_why" ]
    done
}

# Each row runs on two boot partitions: one at eight sectors to a cluster,
# as an MBR disk's is, and one at one sector to a cluster, as the EFI system
# partition of a GUID table is, whose environment is two clusters. A boot
# partition whose environment already starts recovery must not be written.
# One whose environment header is zeroed must be repaired with its 1024
# bytes alone, from the copy prepare_boot made; a repair torn after one
# sector must fail, and the retry must finish it. An environment that starts
# the candidate must be rewritten in place, and growboot must refuse to
# write its input.
case_boot() {
    _ids="unchanged unparseable torn repaired retry-unchanged candidate own-input"
    # shellcheck disable=SC2086
    if ! cp "$SCRIPT" "$W/source.sh"; then
        broken "cannot read $SCRIPT" $_ids
        return
    fi
    printf 'rach2.slot=recovery\n' > "$W/cmdline"
    sed "s|/proc/cmdline|$W/cmdline|g" utils/bootctl.c > "$W/bootctl.c"
    # shellcheck disable=SC2086
    if ! {
        cc -O2 -Wall -Wextra -I utils -o "$W/bootctl" "$W/bootctl.c" utils/bootslot.c \
            utils/bootverb.c utils/grubslot.c &&
            cc -O2 -Wall -Wextra -I utils -o "$W/growboot" utils/growboot.c utils/bootslot.c &&
            cc -O2 -Wall -Wextra -I utils -o "$W/reflash" utils/reflash.c utils/sectorcopy.c \
                utils/sha256.c &&
            cc -O2 -Wall -Wextra -o "$W/mkfatfixture" tests/host/mkfatfixture.c
    } > "$W/cc.log" 2>&1; then
        broken "the native programs do not build; see $W/cc.log" $_ids
        return
    fi
    "$W/mkfatfixture" "$W/boot.cluster8" --cluster8 > "$W/fat.log" 2>&1
    "$W/mkfatfixture" "$W/boot.cluster1" >> "$W/fat.log" 2>&1
    # Pad the independent fixtures to the machine helper's whole boot extent.
    for _fat in "$W/boot.cluster8" "$W/boot.cluster1"; do
        dd if=/dev/null of="$_fat" bs=1048576 seek=64 2>> "$W/fat.log"
    done
    # io.py stands for the disk: a read addressed in bytes, and a write that
    # it logs and, with GROW_BOOT_TEAR=1, cuts after its first sector.
    cat > "$W/io.py" <<'PY'
import os
import sys
mode, root, *args = sys.argv[1:]
if mode == 'dd':
    fields = dict(arg.split('=', 1) for arg in args)
    dst = open(fields['of'], 'wb') if 'of' in fields else sys.stdout.buffer
    with open(fields['if'], 'rb') as src:
        src.seek(int(fields['skip']))
        left = int(fields['count'])
        while left:
            data = src.read(min(left, 1048576))
            if not data:
                sys.exit(1)
            dst.write(data)
            left -= len(data)
else:
    path, at, length = args
    at, length = int(at), int(length)
    assert path == root + '/disk'
    assert at >= 1048576 and at + length <= 68157440
    data = sys.stdin.buffer.read(length + 1)
    assert len(data) == length
    tear = os.getenv('GROW_BOOT_TEAR') == '1'
    if tear:
        assert length == 1024
        data = data[:512]
    with open(root + '/writes', 'a') as log:
        log.write(f'{at} {len(data)}\n')
    with open(path, 'r+b') as dst:
        dst.seek(at)
        dst.write(data)
        dst.flush()
        os.fsync(dst.fileno())
    if tear:
        sys.exit(1)
PY
    extract 'hash_slice|prepare_boot|keep_recovery' "$W/functions.sh" \
        "s|/bin/growboot|$W/growboot|g"
    # The functions below replace only the disk's reads and writes and the
    # path of the guest's command line; the FAT parsing, the editing of the
    # environment and the decisions are the script's own.
    cat > "$W/rows.sh" <<'EOF'
# dd ARGS: io.py for a read addressed in bytes, the host's dd otherwise.
dd()
{
    case " $* " in
        *' iflag=skip_bytes,count_bytes '*) python3 "$W/io.py" dd "$W" "$@" ;;
        *) command dd "$@" ;;
    esac
}
# Copy the disk's 64 MiB boot partition to current-boot.img.
read_boot()
{
    command dd if="$disk" of="$W/current-boot.img" bs=1048576 skip=1 count=64 2>"$W/read.log"
    [ "$(wc -c < "$W/current-boot.img" | tr -d ' ')" = 67108864 ] || fail 'short boot read'
}
# reflash ARGS: the native reflash for --verify, and otherwise a write
# through io.py.
reflash()
{
    if [ "$1" = --verify ]; then
        "$W/reflash" "$@"
    else
        python3 "$W/io.py" write "$W" "$@"
    fi
}
# bootctl --target DEVICE ARGS: the native bootctl, on a copy of the boot
# partition read now when DEVICE is the disk's first partition.
bootctl()
{
    if [ "$2" = "${disk}p1" ]; then
        read_boot
        shift 2
        "$W/bootctl" --target "$W/current-boot.img" "$@"
    else
        "$W/bootctl" "$@"
    fi
}
. "$W/functions.sh"
geometry=$1
cp "$W/boot.$geometry" "$W/boot.original"
saved="$W/saved"
work=$W
disk="$W/disk"
rm -rf "$saved"
mkdir "$saved"
cp "$W/boot.original" "$saved/boot.img"
rm -f "$W/recovery.env"
begin unchanged
prepare_boot
command dd if=/dev/zero of="$disk" bs=1048576 count=1 2>"$W/create.log"
cat "$W/boot.original" >> "$disk"
command dd if="$W/recovery.env" of="$disk" bs=512 seek="$((2048 + env_sector))" \
    count=2 conv=notrunc 2>"$W/initial-env.log"
printf '# GRUB Environment Block\n# preserve this comment\nsaved_entry=recovery\n' > "$W/good-env"
n=$(wc -c < "$W/good-env" | tr -d ' ')
awk -v n="$((1024 - n))" 'BEGIN {for (i = 0; i < n; i++) printf "#"}' >> "$W/good-env"
command dd if="$W/good-env" of="$disk" bs=512 seek="$((2048 + env_sector))" \
    count=2 conv=notrunc 2>"$W/good-env.log"
: > "$W/writes"
keep_recovery
[ ! -s "$W/writes" ] || fail 'an environment that already starts recovery was rewritten'
passed unchanged
begin unparseable
# Damage the header, not just a value bootctl still accepts.
command dd if=/dev/zero of="$disk" bs=512 seek="$((2048 + env_sector))" \
    count=2 conv=notrunc 2>"$W/damage.log"
if bootctl --target "${disk}p1" env saved_entry recovery > "$W/unparseable.log" 2>&1; then
    fail 'bootctl parsed an environment whose header is zeroed'
fi
grep -q "the environment block header is not GRUB's" "$W/unparseable.log" \
    || fail 'bootctl refused the zeroed header without naming the header'
passed unparseable
begin torn
rm "$W/recovery.env"
prepare_boot
if (export GROW_BOOT_TEAR=1; keep_recovery) > "$W/torn.log" 2>&1; then
    fail 'a repair torn after one sector succeeded'
fi
grep -q 'repairing boot environment' "$W/torn.log" || fail 'the repair did not reach the torn write'
printf '%s 512\n' "$((1048576 + env_offset))" > "$W/expected-writes"
cmp "$W/expected-writes" "$W/writes" > /dev/null || fail 'the torn repair did not stop after one sector'
passed torn
begin repaired
rm "$W/recovery.env"
prepare_boot
keep_recovery
printf '%s 1024\n' "$((1048576 + env_offset))" >> "$W/expected-writes"
cmp "$W/expected-writes" "$W/writes" > /dev/null || fail 'the repair wrote outside the environment'
read_boot
cp "$W/boot.original" "$W/expected-boot.img"
command dd if="$W/recovery.env" of="$W/expected-boot.img" bs=512 seek="$env_sector" \
    count=2 conv=notrunc 2>"$W/expected-env.log"
cmp "$W/expected-boot.img" "$W/current-boot.img" > /dev/null \
    || fail 'the repaired boot partition differs from the prepared copy'
passed repaired
begin retry-unchanged
: > "$W/writes"
keep_recovery
[ ! -s "$W/writes" ] || fail 'the repaired environment was rewritten on the retry'
passed retry-unchanged
begin candidate
# A valid candidate default also changes through the prepared copy.
command dd if="$W/boot.original" of="$disk" bs=512 skip="$env_sector" \
    seek="$((2048 + env_sector))" count=2 conv=notrunc 2>"$W/candidate.log"
keep_recovery
printf '%s 1024\n' "$((1048576 + env_offset))" > "$W/candidate-writes"
cmp "$W/candidate-writes" "$W/writes" > /dev/null \
    || fail 'the candidate environment was not rewritten in place, 1024 bytes alone'
passed candidate
begin own-input
: > "$W/writes"
if "$W/growboot" "$saved/boot.img" "$saved/boot.img"; then
    fail 'growboot wrote over its input'
fi
cmp "$W/boot.original" "$saved/boot.img" > /dev/null || fail 'the boot backup was changed'
passed own-input
EOF
    for _geometry in cluster8 cluster1; do
        rows_run "$_geometry" "$W/rows.sh" "$_geometry"
    done
    for _id in $_ids; do
        _why=
        for _geometry in cluster8 cluster1; do
            if ! _text=$(row_result "$_geometry" "$_id"); then
                _why="$_geometry: $_text"
                break
            fi
        done
        judge "$_id" "$_why" [ -z "$_why" ]
    done
}

# The work directory must be on the store, and the store a mounted exFAT
# volume. A failed command, and a log write that fails under a file size
# limit, must be reported, the second on the console too. The plan of a
# 1 GiB seed for the Samsung's size must be accepted, a range list growset
# cannot write reported as a write and not as an unsupported geometry, and a
# target smaller than the seed refused. The measured grow must unmount the
# store and mount it again, report a failure, and refuse more than 64 KiB of
# output. alert, prepare_work, hash_file, check_plan, run_logged and
# run_measured are copied out of the script, with /proc/mounts, /dev/console
# and growset replaced by files and a native build.
case_space() {
    _ids="work absent producer log ranges output geometry measured measure-fail overflow"
    # shellcheck disable=SC2086
    if ! cp "$SCRIPT" "$W/source.sh"; then
        broken "cannot read $SCRIPT" $_ids
        return
    fi
    mkdir "$W/store"
    # shellcheck disable=SC2086
    if ! cc -O2 -Wall -Wextra -o "$W/growset" tools/inspect/growset.c > "$W/cc.log" 2>&1; then
        broken "growset does not build on the host; see $W/cc.log" $_ids
        return
    fi
    if ! dd if=/dev/zero of="$W/store/seed.ext2" bs=1048576 count=0 seek=1024 2>"$W/seed.log" ||
        ! E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -g 32768 -I 128 \
            -O sparse_super,large_file,filetype,resize_inode,^dir_index,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize \
            "$W/store/seed.ext2" > "$W/mkfs.log" 2>&1 ||
        ! "$W/growset" --save --ranges --lba 266240 "$W/store/seed.ext2" 488130928 > "$W/ranges.txt"; then
        # shellcheck disable=SC2086
        broken "the seed or its ranges cannot be made; see $W/mkfs.log" $_ids
        return
    fi
    _seed_line=$(sha256sum "$W/store/seed.ext2")
    proof=$W
    seed_hash=${_seed_line%% *}
    export proof seed_hash
    extract 'alert|prepare_work|hash_file|check_plan|run_logged|run_measured' "$W/functions.sh" \
        -e "s|/bin/growset|$W/growset|g" \
        -e "s|/proc/mounts|$W/mounts|g" \
        -e "s|\[ -c /dev/console \]|[ -f $W/console ]|g" \
        -e "s|> /dev/console|> $W/console|g"
    cat > "$W/case.sh" <<'CASE'
#!/bin/sh
set -eu
. "$proof/functions.sh"
fail() { alert "$*"; exit 1; }
store="$proof/store"
work="$proof/work"
plan="$proof/plan"
disk=/dev/disk0
source_device=/dev/disk1p1
start=266240
seed=2097152
count=488130928
disk_id=1acef00d
table=mbr
identity=MBRID=1acef00d
case "$1" in
    work)
        prepare_work
        case "$work" in
            "$store"/machine-grow.*) ;;
            *)
                # The work fault puts the directory in /tmp, outside the proof.
                case "$work" in /tmp/machine-grow.?*) rm -rf "$work" ;; esac
                fail 'work is outside the store'
                ;;
        esac
        ;;
    absent)
        prepare_work
        ;;
    producer)
        run_logged producer.log sh -c 'printf "producer output\n"; exit 23'
        ;;
    log)
        trap '' XFSZ
        ulimit -f 1
        run_logged output.log awk 'BEGIN { for (i=0;i<4096;i++) print "restore completion" }'
        ;;
    ranges)
        check_plan
        ;;
    output)
        trap '' XFSZ
        ulimit -f 1
        check_plan
        ;;
    geometry)
        count=8
        check_plan
        ;;
    measured|overflow|measure-fail)
        mode=$1
        umount() { printf 'unmounted\n' >> "$proof/transitions"; }
        mount() { printf 'mounted\n' >> "$proof/transitions"; }
        growroot() {
            case "$mode" in
                measured) printf 'measured output\n'; return 0 ;;
                measure-fail) printf 'measured failure\n'; return 23 ;;
                overflow) awk 'BEGIN {for(i=0;i<8192;i++) print "too much output"}' ;;
            esac
        }
        run_measured
        ;;
esac
CASE
    mkdir "$W/work" "$W/plan"
    printf '/dev/disk1p1 %s exfat rw 0 0\n' "$W/store" > "$W/mounts"
    printf 'plan\n' > "$W/plan/grow.plan"
    cp "$W/ranges.txt" "$W/plan/ranges.txt"
    cp "$W/ranges.txt" "$W/work/ranges.txt"

    space_case work 0 'logs:'
    judge work "$_why" [ -z "$_why" ]
    cp "$W/mounts" "$W/mounts.saved"
    : > "$W/mounts"
    space_case absent 1 'store must be a mounted exFAT volume'
    judge absent "$_why" [ -z "$_why" ]
    mv "$W/mounts.saved" "$W/mounts"
    space_case producer 1 'command failed; see .*producer.log'
    judge producer "$_why" [ -z "$_why" ]
    space_case log 1 'writing .*output.log'
    if [ -z "$_why" ] && ! grep -q 'writing .*output.log' "$W/console"; then
        _why='the failed log write did not reach the console'
    fi
    judge log "$_why" [ -z "$_why" ]
    _why=
    sh "$W/case.sh" ranges > "$W/ranges.log" 2>&1 || _why="check_plan refused the plan for the Samsung's size"
    judge ranges "$_why" [ -z "$_why" ]
    space_case output 1 'creating .*expected-ranges.txt'
    if [ -z "$_why" ] && ! grep -q 'writing the range list failed:' "$W/work/growset.log"; then
        _why='growset did not say that writing the range list failed'
    elif [ -z "$_why" ] && grep -q 'geometry is not supported' "$W/output.log"; then
        _why='a range list growset could not write was called an unsupported geometry'
    fi
    judge output "$_why" [ -z "$_why" ]
    space_case geometry 1 'smaller than the volume'
    judge geometry "$_why" [ -z "$_why" ]
    : > "$W/transitions"
    space_case measured 0 'measured output'
    printf 'unmounted\nmounted\n' > "$W/expected-transitions"
    if [ -z "$_why" ] && ! cmp "$W/expected-transitions" "$W/transitions" > /dev/null; then
        _why="the measured grow did not unmount the store and mount it again: $(tr '\n' ' ' < "$W/transitions")"
    fi
    judge measured "$_why" [ -z "$_why" ]
    space_case measure-fail 1 'measured grow failed'
    judge measure-fail "$_why" [ -z "$_why" ]
    space_case overflow 1 'grow output exceeds 64 KiB'
    judge overflow "$_why" [ -z "$_why" ]
}

# space_case NAME STATUS DIAGNOSTIC: run NAME of case.sh, which must exit
# with STATUS and print DIAGNOSTIC to its output or the console file; leave
# in _why what it did otherwise, or nothing.
space_case() {
    : > "$W/console"
    rm -f "$W/work/command.status" "$W/work/output.log"
    _got=0
    sh "$W/case.sh" "$1" > "$W/$1.log" 2>&1 || _got=$?
    _why=
    if [ "$_got" != "$2" ]; then
        _why="$1 ended with status $_got, not $2: $(tail -1 "$W/$1.log")"
    elif ! grep -q "$3" "$W/$1.log" "$W/console"; then
        _why="$1 did not say $3"
    fi
}

# A machine below the memory the grow needs is refused by name before
# anything is read. The refusal is proved on the host, where the memory can
# be dictated: the program is built with a requirement no machine meets, so
# what it reads from /proc/meminfo decides nothing. A host that has that file
# and one that does not both take this path.
case_memory() {
    if ! cc -DGROWROOT_MEMORY_MIB=1048576u -o "$W/growroot-host" utils/growroot.c \
        utils/gptdisk.c > "$W/growroot-host.log" 2>&1; then
        broken "growroot does not build on the host; see $W/growroot-host.log" status refused
        return
    fi
    _got=0
    "$W/growroot-host" MBRID=47470001,1 > "$W/memory.log" 2>&1 || _got=$?
    judge status "growroot ended with status $_got, not 1" [ "$_got" = 1 ]
    grep -q '^growroot: refused: [0-9]* MiB of memory; growing a root needs 1048576 MiB$' \
        "$W/memory.log"
    judge refused "growroot said: $(head -1 "$W/memory.log")" [ "$?" = 0 ]
}

# ---- The run. ----

ID=machine.grow.$CASE
if [ -n "${ETUDE_CASE:-}" ] && [ "$ETUDE_CASE" != "$ID" ]; then
    echo "grow: ETUDE_CASE is $ETUDE_CASE, and the argument names $ID" >&2
    exit 2
fi
case "$CASE" in
    measure | check | boot | space | memory) ;;
    *) echo "grow: no case $CASE" >&2; exit 2 ;;
esac
record "begin $ID"
"case_$CASE"
record "end $RECORDS"
if [ -z "$FAILED" ]; then
    echo "grow: result: passed: $ID"
    exit 0
fi
echo "grow: result: failed: $ID:$FAILED"
exit 1
