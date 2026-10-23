#!/bin/sh
#
# tooling_image_test.sh: vendor/gnu/tooling_image.sh leaves no stage and no
# lock when a signal stops its job, and removes those of a job killed
# outright on its next run.
#
# A private copy of the script runs in a scratch tree of its own, in a
# process group of its own, against a stand-in for `etude provision tooling`
# that makes a job directory in the cache as etude does and then waits. On a
# signal the stand-in takes FIX/delay seconds to end, one unless a row says
# otherwise, as etude does while it stops QEMU, notes in FIX/held whether the
# build disk's lock was still there by then, and removes its job directory;
# FIX/mode `bare` makes it a program with no handler, and `pass` a job that
# ends at once with its image. The rows: a job that passes and writes the
# image; SIGTERM, SIGINT and SIGHUP to the group, as a terminal or `etude
# admit` sends them, which end the script with 128 plus the signal's number;
# SIGTERM to the group with the stand-in that has no handler; SIGTERM to the
# script alone, as make passes one on, which must reach the job; SIGTERM to
# the group and again to the script while it waits for a job that takes two
# seconds to stop; and SIGTERM followed by SIGKILL two seconds later, as
# `etude admit` sends them, with a job that takes four seconds to stop, after
# which the next run must pass. After each, the tree must have no stage, no
# lock and no job directory, and the lock must have outlived the job. Every
# row runs under sh and, where the host has it, bash, which is the sh of
# macOS.
#
# Usage: sh tests/system/tooling_image_test.sh
#
set -u

[ "$#" -eq 0 ] || { echo 'tooling-image-test: usage: tooling_image_test.sh' >&2; exit 2; }
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=
pid=
trap '[ -z "$pid" ] || { kill -KILL "-$pid" 2> /dev/null; wait "$pid"; }
    [ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-tooling-image-test.XXXXXX") || exit 1
source=$repo/vendor/gnu/tooling_image.sh
tree=$work/tree
FIX=$work/fix
TEST_LOCK=$tree/build/vendor/gnu/tooling.disk.lock
export FIX TEST_LOCK
for t in git tar perl; do
    command -v "$t" > /dev/null 2>&1 || { echo "tooling-image-test: no $t on this host" >&2; exit 1; }
done

mkdir -p "$tree/vendor/gnu" "$tree/mk" "$tree/krffs" "$tree/src" "$tree/build/tools/etude" \
    "$tree/build/tools/verify" "$FIX" || exit 1
cp "$source" "$tree/vendor/gnu/tooling_image.sh" || exit 1
cp "$repo/vendor/gnu/arch.sh" "$tree/vendor/gnu/" && cp "$repo/mk/arch.sh" "$tree/mk/" || exit 1
: > "$tree/vendor/gnu/tooling_job.sh"
echo 'exit 0' > "$tree/krffs/manifest_deps.sh"
echo 'char crypt;' > "$tree/src/crypt.c"
cat > "$tree/build/tools/verify/sha256" << 'EOF'
#!/bin/sh
echo "feed  $1"
EOF
cat > "$tree/build/tools/etude/etude" << 'EOF'
#!/bin/sh
[ "${1:-} ${2:-}" = 'provision tooling' ] || exit 2
shift 2
cache=
while getopts r:C:p:a: o; do
    [ "$o" != C ] || cache=$OPTARG
done
shift $((OPTIND - 1))
[ "${1:-}" = run ] || exit 0
shift
out=
OPTIND=1
while getopts nc:m:s:t:i:d:o:O: o; do
    [ "$o" != O ] || out=$OPTARG
done
job=$cache/job-$$-0
mkdir -p "$job" && echo 'gnu-vm: make: the layer is built' > "$job/channel.log"
case $(cat "$FIX/mode") in
    pass)
        echo feed > "$out/digest"
        rm -rf "$job"
        exit 0 ;;
    bare)
        : > "$FIX/running"
        while :; do sleep 1; done ;;
esac
stopped()
{
    kill "$waiter" 2> /dev/null
    sleep "$(cat "$FIX/delay")"
    [ ! -L "$TEST_LOCK" ] || : > "$FIX/held"
    rm -rf "$job"
    exit "$1"
}
trap 'stopped 129' HUP
trap 'stopped 130' INT
trap 'stopped 143' TERM
while :; do sleep 1; done &
waiter=$!
: > "$FIX/running"
wait "$waiter"
EOF
chmod +x "$tree/build/tools/etude/etude" "$tree/build/tools/verify/sha256" || exit 1
(cd "$tree" && git init -q .) || exit 1

# run SHELL: start the copy under SHELL in a process group of its own whose
# leader's ID is $pid, with SIGINT as a terminal's process would have it.
run()
{
    rm -f "$FIX/running" "$FIX/held"
    # shellcheck disable=SC2086 # the shell's name and its options
    perl -e 'chdir shift or die; $SIG{INT} = "DEFAULT"; setpgrp(0, 0); exec @ARGV or die' \
        "$tree" env -u ETUDE_TOOLING_CACHE -u GNU_TOOLING_DRY_RUN RACH2_ARCH=i686 \
        RACH2_QEMU_ACCEL=tcg MUSL_CRYPT_SRC=src/crypt.c GNU_TOOLING_DISK_GIB=1 \
        $1 vendor/gnu/tooling_image.sh build/images/out.ext2 16 > "$work/out" 2>&1 &
    pid=$!
}

# running: wait until the stand-in's job runs, and fail when the copy ends
# first.
running()
{
    until [ -e "$FIX/running" ]; do
        case $(ps -o stat= -p "$pid" 2> /dev/null) in
            '' | Z*) return 1 ;;
        esac
        sleep 0.1
    done
}

# finish: wait for the copy, set $status to its exit status, and wait until
# no process of its group is left.
finish()
{
    status=0
    wait "$pid" 2> /dev/null || status=$?
    while kill -0 "-$pid" 2> /dev/null; do
        sleep 0.1
    done
    pid=
}

# clean: succeed when the tree has no stage, no lock and no job directory.
clean()
{
    set -- "$tree/build/vendor/gnu"/.tooling.* "$tree/build/tooling"/job-*
    for path in "$TEST_LOCK" "$@"; do
        if [ -e "$path" ] || [ -L "$path" ]; then
            echo "tooling-image-test: left: ${path#"$tree/"}"
            return 1
        fi
    done
}

# stopped SHELL SIGNAL STATUS [alone]: run the copy under SHELL with the
# waiting stand-in, send SIGNAL to its group, or to the script alone, and
# require STATUS, a clean tree, and the lock kept until the job ended.
stopped()
{
    run "$1"
    running || return 1
    if [ "${4:-}" = alone ]; then
        kill "-$2" "$pid"
    else
        kill "-$2" "-$pid"
    fi
    finish || return 1
    [ "$status" -eq "$3" ] && clean && [ -e "$FIX/held" ]
}

# row SHELL NAME: run row NAME under SHELL, and fail when it fails.
row()
{
    rm -rf "$tree/build/vendor" "$tree/build/tooling" "$tree/build/images" "$work/out"
    mkdir -p "$tree/build/tooling" || return 1
    echo wait > "$FIX/mode"
    echo 1 > "$FIX/delay"
    case $2 in
        pass)
            echo pass > "$FIX/mode"
            run "$1"
            finish || return 1
            [ "$status" -eq 0 ] && [ -f "$tree/build/images/out.ext2" ] && clean ;;
        term) stopped "$1" TERM 143 ;;
        int) stopped "$1" INT 130 ;;
        hup) stopped "$1" HUP 129 ;;
        bare)
            echo bare > "$FIX/mode"
            run "$1"
            running || return 1
            kill -TERM "-$pid"
            finish || return 1
            [ "$status" -eq 143 ] && clean ;;
        alone) stopped "$1" TERM 143 alone ;;
        twice)
            echo 2 > "$FIX/delay"
            run "$1"
            running || return 1
            kill -TERM "-$pid"
            sleep 0.5
            kill -TERM "$pid"
            finish || return 1
            [ "$status" -eq 143 ] && clean && [ -e "$FIX/held" ] ;;
        kill)
            echo 4 > "$FIX/delay"
            run "$1"
            running || return 1
            kill -TERM "-$pid"
            sleep 2
            kill -KILL "-$pid"
            finish || return 1
            echo pass > "$FIX/mode"
            run "$1"
            finish || return 1
            [ "$status" -eq 0 ] && [ -f "$tree/build/images/out.ext2" ] && clean ;;
    esac
}

shells="sh"
command -v bash > /dev/null 2>&1 && shells="$shells bash"
failed=0
count=0
for shell in $shells; do
    for name in pass term int hup bare alone twice kill; do
        status=
        count=$((count + 1))
        if ! row "$shell" "$name" >&2; then
            echo "tooling-image-test: failed: $shell $name: status ${status:-none}" >&2
            sed 's/^/  /' "$work/out" >&2
            failed=1
        fi
    done
done
[ "$failed" -eq 0 ] || exit 1
echo "tooling-image-test: result: pass: $count rows"
