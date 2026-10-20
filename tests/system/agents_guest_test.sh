#!/bin/sh
#
# agents_guest_test.sh: tests/system/accept/agents_guest.sh removes the
# scratch directories of its `secrets` and `work` commands on every exit path.
#
# A private copy of the script runs with a home and a TMPDIR of the test's
# own, in a process group of its own, as a terminal's interrupt or timeout(1)
# would end it. Rows check that `secrets` counts the strings of a log and
# `filtered` none, and `work` prints its check, each leaving TMPDIR empty;
# that a TERM sent to the group while `secrets` waits for its log, or while
# `work` runs its check, ends the command with status 143 and leaves TMPDIR
# empty; and that `secrets` whose reader has gone ends with status 141 and
# leaves TMPDIR empty. AGENTS_GUEST names the script to copy, by default the
# tree's; tests/proofs/agents-guest.json runs the rows against a copy without
# its traps.
#
# Usage: sh tests/system/agents_guest_test.sh
#
set -u

[ "$#" -eq 0 ] || { echo 'agents-guest-test: usage: agents_guest_test.sh' >&2; exit 2; }
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
work=
pid=
trap '[ -z "$pid" ] || { kill -KILL "-$pid" 2> /dev/null; wait "$pid"; }
    [ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-agents-guest-test.XXXXXX") || exit 1
source=${AGENTS_GUEST:-$repo/tests/system/accept/agents_guest.sh}
guest=$work/guest.sh
home=$work/home
scratch=$work/tmp

# guest IN OUT COMMAND...: run the copy with the test's home and TMPDIR, its
# standard input IN, its output OUT and its errors work/err, in a process
# group of its own whose leader's ID is $pid.
guest()
{
    in=$1 out=$2
    shift 2
    HOME=$home TMPDIR=$scratch perl -e '$SIG{PIPE} = "DEFAULT"; setpgrp(0, 0); exec @ARGV or die' \
        bash "$guest" "$@" < "$in" > "$out" 2> "$work/err" &
    pid=$!
}

# appear DIR PATTERN: wait until a path in DIR that PATTERN matches exists,
# for at most 30 seconds.
appear()
{
    tries=0
    while :; do
        for path in "$1"/$2; do
            [ ! -e "$path" ] || return 0
        done
        [ "$tries" -lt 300 ] || return 1
        sleep 0.1
        tries=$((tries + 1))
    done
}

# finish: wait for the guest, set $status to its exit status, and wait until
# no process of its group is left, for at most 30 seconds.
finish()
{
    status=0
    wait "$pid" 2> /dev/null || status=$?
    tries=0
    while kill -0 "-$pid" 2> /dev/null; do
        [ "$tries" -lt 300 ] || return 1
        sleep 0.1
        tries=$((tries + 1))
    done
    pid=
}

# clean: succeed when the guest's TMPDIR is empty.
clean()
{
    [ -z "$(ls -A "$scratch")" ]
}

# row NAME: run row NAME against the copy, and fail when it fails.
row()
{
    rm -rf "$scratch" "$work/in" "$work/out" "$work/mark" && mkdir "$scratch" || return 1
    case $1 in
        secrets)
            printf 'a log with rach2-offline-dummy-key in it\n' > "$work/in"
            guest "$work/in" "$work/out" secrets qemu
            finish || return 1
            [ "$status" -eq 0 ] && grep -qx '1 of [0-9]* strings' "$work/out" && clean ;;
        filtered)
            guest /dev/null "$work/out" filtered qemu
            finish || return 1
            [ "$status" -eq 0 ] && grep -qx '0 of [0-9]* strings' "$work/out" && clean ;;
        work)
            guest /dev/null "$work/out" work claude
            finish || return 1
            [ "$status" -eq 0 ] && grep -qx 'check checked' "$work/out" && clean ;;
        secrets-term)
            mkfifo "$work/in" || return 1
            guest "$work/in" "$work/out" secrets qemu
            exec 3> "$work/in"
            appear "$scratch" 'rach2-accept-secrets.*/log'
            kill -TERM "-$pid"
            finish || return 1
            exec 3>&-
            [ "$status" -eq 143 ] && clean ;;
        work-term)
            GUEST_TEST_MARK=$work/mark
            export GUEST_TEST_MARK
            guest /dev/null "$work/out" work claude
            unset GUEST_TEST_MARK
            appear "$work" mark
            kill -TERM "-$pid"
            finish || return 1
            [ "$status" -eq 143 ] && clean ;;
        secrets-pipe)
            mkfifo "$work/in" "$work/out" || return 1
            guest "$work/in" "$work/out" secrets qemu
            exec 3> "$work/in" 4< "$work/out"
            appear "$scratch" 'rach2-accept-secrets.*/log'
            exec 4<&-
            exec 3>&-
            finish || return 1
            [ "$status" -eq 141 ] && clean ;;
    esac
}

# rows: run every row against the copy, print `agents-guest-test: failed:
# NAME: status S` for each that fails, S being the guest's exit status, and
# fail when any did.
rows()
{
    failed=0
    for name in secrets filtered work secrets-term work-term secrets-pipe; do
        status=
        if ! row "$name"; then
            echo "agents-guest-test: failed: $name: status ${status:-none}"
            failed=1
        fi
    done
    return "$failed"
}

cp "$source" "$guest" || exit 1
mkdir -p "$home/rach2-accept/fixture" || exit 1
printf 'hello\n' > "$home/rach2-accept/fixture/greeting.txt"
cat > "$home/rach2-accept/fixture/check.sh" <<'EOF'
[ -z "${GUEST_TEST_MARK:-}" ] || { : > "$GUEST_TEST_MARK"; sleep 30; }
echo checked
EOF
mkdir "$scratch" || exit 1
for command in setup 'prepare claude'; do
    # shellcheck disable=SC2086 # the command and its argument
    if ! HOME=$home TMPDIR=$scratch bash "$guest" $command > /dev/null; then
        echo "agents-guest-test: $command failed" >&2
        exit 1
    fi
done
rows >&2 || exit 1
echo 'agents-guest-test: result: pass: 6 rows'
