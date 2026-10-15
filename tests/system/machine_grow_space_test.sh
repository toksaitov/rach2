#!/bin/sh
# machine_grow_space_test.sh [--mutate | --texts]: check the work directory,
# the log writes and the plan check of tools/dev/machine_grow.sh on the
# host, through the script's own functions.
#
# alert, prepare_work, hash_file, check_plan, run_logged and run_measured are
# copied out of the script, with /proc/mounts, /dev/console and growset
# replaced by files and a native build. The work directory must be on the
# store, and the store a mounted exFAT volume. A failed command, and a log
# write that fails under a file size limit, must be reported, the second on
# the console too. The plan of a 1 GiB seed for the Samsung's size must be
# accepted, a range list growset cannot write reported as a write and not as
# an unsupported geometry, and a target smaller than the seed refused. The
# measured grow must unmount the store and mount it again, report a failure,
# and refuse more than 64 KiB of output. --mutate puts the work directory in
# /tmp, ignores a failed log write, and calls a failed growset an unsupported
# geometry, and requires each to fail its row; --texts checks that each
# fault changes the script.
# shellcheck disable=SC2034,SC2016,SC2154
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
if [ "${1:-}" != --texts ]; then
    . "$repo/tests/system/runlock.sh"
    runlock_hold "$repo" machine-grow-space-test "$repo/tests/system/machine_grow_space_test.sh" "$@"
fi
proof=$(mktemp -d "${TMPDIR:-/tmp}/rach2-grow-space.XXXXXX")
proof=$(CDPATH='' cd -- "$proof" && pwd -P)
echo "machine-grow-space-test: logs: $proof"
# Report $* and exit 1.
fail() { echo "machine-grow-space-test: failed: $*" >&2; exit 1; }
[ "$#" = 0 ] || [ "$*" = --mutate ] || [ "$*" = --texts ] || fail 'usage: machine_grow_space_test.sh [--mutate | --texts]'
cp "$repo/tools/dev/machine_grow.sh" "$proof/original.sh"
cp "$proof/original.sh" "$proof/source.sh"

# mutate_source MUTATION: $proof/source.sh is $proof/original.sh with
# MUTATION in it, and `diagnostic` the words its failing row prints.
mutate_source()
{
    case "$1" in
        work)
            sed 's|mktemp -d "$store/machine-grow.XXXXXX"|mktemp -d /tmp/machine-grow.XXXXXX|' "$proof/original.sh" > "$proof/source.sh"
            diagnostic='work returned 1, expected 0'
            ;;
        log)
            sed 's/fail "writing $work\/$log_name"/: "writing $work\/$log_name"/' "$proof/original.sh" > "$proof/source.sh"
            diagnostic='log returned 0, expected 1'
            ;;
        diagnostic)
            sed 's/fail "creating $work\/expected-ranges.txt; growset failed (see $work\/growset.log)"/fail "the seed geometry is not supported"/' "$proof/original.sh" > "$proof/source.sh"
            diagnostic='output diagnostic'
            ;;
    esac
}

if [ "$*" = --texts ]; then
    for mutation in work log diagnostic; do
        mutate_source "$mutation"
        if cmp -s "$proof/original.sh" "$proof/source.sh"; then
            echo "machine-grow-space-test: the $mutation fault is not in tools/dev/machine_grow.sh" >&2
            exit 1
        fi
    done
    trap - EXIT
    rm -rf "$proof"
    echo 'machine-grow-space-test: texts: every fault is once in its source'
    exit 0
fi
trap 'cp "$proof/original.sh" "$proof/source.sh"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
cc -O2 -Wall -Wextra -o "$proof/growset" "$repo/tools/inspect/growset.c"
# shellcheck source=tools/setup/e2fsprogs_path.sh
. "$repo/tools/setup/e2fsprogs_path.sh"
mkdir "$proof/store"
dd if=/dev/zero of="$proof/store/seed.ext2" bs=1048576 count=0 seek=1024 2>"$proof/seed.log"
E2FSPROGS_FAKE_TIME=1 mke2fs -q -F -t ext2 -b 4096 -g 32768 -I 128 \
    -O sparse_super,large_file,filetype,resize_inode,^dir_index,^ext_attr,^has_journal,^extent,^64bit,^flex_bg,^metadata_csum,^huge_file,^dir_nlink,^extra_isize \
    "$proof/store/seed.ext2" >"$proof/mkfs.log" 2>&1
"$proof/growset" --save --ranges --lba 266240 "$proof/store/seed.ext2" 488130928 > "$proof/ranges.txt"
seed_line=$(sha256sum "$proof/store/seed.ext2")
seed_hash=${seed_line%% *}
export proof seed_hash

# Copy the six functions whole from source.sh into functions.sh, with their
# host paths replaced.
extract()
{
    awk '
        /^(alert|prepare_work|hash_file|check_plan|run_logged|run_measured)\(\)/ {copy = 1}
        copy {print}
        copy && /^}/ {copy = 0}
    ' "$proof/source.sh" > "$proof/extracted.sh"
    sed -e "s|/bin/growset|$proof/growset|g" \
        -e "s|/proc/mounts|$proof/mounts|g" \
        -e "s|\[ -c /dev/console \]|[ -f $proof/console ]|g" \
        -e "s|> /dev/console|> $proof/console|g" \
        "$proof/extracted.sh" > "$proof/functions.sh"
}
cat > "$proof/case.sh" <<'CASE'
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
case "$1" in
    work)
        prepare_work
        case "$work" in "$store"/machine-grow.*) ;; *) fail 'work is outside the store' ;; esac
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
mkdir "$proof/work" "$proof/plan"
printf '/dev/disk1p1 %s exfat rw 0 0\n' "$proof/store" > "$proof/mounts"
printf 'plan\n' > "$proof/plan/grow.plan"
cp "$proof/ranges.txt" "$proof/plan/ranges.txt"
cp "$proof/ranges.txt" "$proof/work/ranges.txt"

# run_case CASE STATUS DIAGNOSTIC: run CASE of case.sh, which must exit with
# STATUS and print DIAGNOSTIC to its output or the console file.
run_case()
{
    case_name=$1
    expected=$2
    diagnostic=$3
    : > "$proof/console"
    rm -f "$proof/work/command.status" "$proof/work/output.log"
    case_result=0
    sh "$proof/case.sh" "$case_name" >"$proof/$phase-$case_name.log" 2>&1 || case_result=$?
    [ "$case_result" = "$expected" ] || fail "$case_name returned $case_result, expected $expected"
    grep -q "$diagnostic" "$proof/$phase-$case_name.log" "$proof/console" || fail "$case_name diagnostic"
}
# rows PHASE: run every case on the functions of source.sh.
rows()
{
    phase=$1
    extract
    run_case work 0 'logs:'
    cp "$proof/mounts" "$proof/mounts.saved"
    : > "$proof/mounts"
    run_case absent 1 'store must be a mounted exFAT volume'
    mv "$proof/mounts.saved" "$proof/mounts"
    run_case producer 1 'command failed; see .*producer.log'
    run_case log 1 'writing .*output.log'
    grep -q 'writing .*output.log' "$proof/console" || fail 'log failure was absent from the independent console'
    sh "$proof/case.sh" ranges || fail 'supported geometry'
    run_case output 1 'creating .*expected-ranges.txt'
    grep -q 'writing the range list failed:' "$proof/work/growset.log" || fail 'calculator write diagnostic'
    if grep -q 'geometry is not supported' "$proof/$phase-output.log"; then fail 'output failure called geometry'; fi
    run_case geometry 1 'smaller than the volume'
    : > "$proof/transitions"
    run_case measured 0 'measured output'
    printf 'unmounted\nmounted\n' > "$proof/expected-transitions"
    cmp "$proof/expected-transitions" "$proof/transitions" || fail 'measurement mount lifecycle'
    run_case measure-fail 1 'measured grow failed'
    run_case overflow 1 'grow output exceeds 64 KiB'
    echo "machine-grow-space-test: $phase: 10 cases passed"
}
# Run rows in a fresh shell so a conditional caller cannot disable errexit.
export repo
sed -n '/^run_case()/,/^# Run rows/p' "$0" | sed '$d' > "$proof/rows.sh"
sed -n '/^extract()/,/^cat > /p' "$0" | sed '$d' > "$proof/extract.sh"
# run_rows PHASE: run rows in a new shell, with its functions read from this
# file.
run_rows()
{
    sh -eu -c 'fail() { echo "machine-grow-space-test: failed: $*" >&2; exit 1; }; . "$proof/extract.sh"; . "$proof/rows.sh"; rows "$1"' sh "$1"
}
run_rows fixed
if [ "$#" = 1 ]; then
    for mutation in work log diagnostic; do
        mutate_source "$mutation"
        if run_rows "$mutation" >"$proof/$mutation.log" 2>&1; then fail "$mutation mutation passed"; fi
        grep -q "$diagnostic" "$proof/$mutation.log" || { cat "$proof/$mutation.log" >&2; fail "$mutation wrong failure"; }
        echo "machine-grow-space-test: mutation: $mutation detected"
        cp "$proof/original.sh" "$proof/source.sh"
        run_rows "$mutation-restored"
    done
fi
