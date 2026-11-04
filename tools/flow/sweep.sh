#!/bin/sh
#
# sweep.sh: remove what finished work left on the testing host, on Windows
# and on this computer, as the Resources section of AGENTS.md allows.
# release.sh and stop.sh run it, and the manager whenever space runs low.
#
# Usage: sweep.sh
#
# It runs `tidy` from lib.sh in this checkout, which removes its landed
# worktrees and branches that no process and no run evidence or loop state
# keeps; prunes, as `prune_logs` in lib.sh does, the session logs of each
# loop state directory under this checkout's build/flow, which a loop
# itself prunes only while it runs and holds no task; and runs `scratch.pl`
# with the stems of master's scratch names, here on this checkout's
# build/tmp and as root on the testing host on its /tmp and on the build/tmp
# of the owner's copy and of each account's clone, which removes the scratch
# of tests that were killed before their traps ran. It then runs
# `leftovers.pl`, through `leftovers` in lib.sh, on the host and here, which
# removes failed tests' directories, loose scratch in home directories and
# copies of the repository whose work master has; and runs `store_expire`,
# which removes the entries of the host's store of step outputs that no copy
# uses. It runs `tidy` in each worker's clone too, since a loop tidies only
# while it runs. `outputs` removes what a build or a run makes again and what
# nothing needs once its run has ended: in each tree on the testing host whose
# loop holds no session, the images, test output and scratch unchanged for a
# day, but `build/tests/failed`, and the run files of `build/runs` unchanged
# for two weeks; on Windows the test collections, tooling job directories,
# case images and scratch unchanged for a day; and here the entries of
# `build/tests` unchanged for two weeks. The caches, the build copies of the
# release and of Windows, and the tooling work disks stay. Each removal, and
# each step's total, is a line on stdout. Each step that reaches the host stops
# itself after ten minutes.

TESTING=testing

# stale is the shell function that `outputs` runs on each machine: stale DAYS
# PATH... removes each PATH in which nothing changed for DAYS days.
# shellcheck disable=SC2016 # the dollars are the function's
stale='
stale()
{
    days=$1
    shift
    for p in "$@"; do
        [ -e "$p" ] || continue
        case $p in */tests/failed) continue ;; esac
        [ -z "$(find "$p" -mtime -"$days" -print -quit 2> /dev/null)" ] || continue
        size=$(du -sk "$p" | cut -f 1)
        rm -rf "$p" && echo "sweep: removed $p: $size KiB"
    done
}
'

# outputs: remove what the header says from each machine.
outputs()
{
    {
        printf '%s\n' "$stale"
        for h in $HOMES; do
            b=$h/rach2/build
            printf '%s\n' "[ -s $b/flow/claude/session ] || stale 1 $b/images/* $b/x86_64/images/*" \
                "$b/i686/images/* $b/tests/* $b/x86_64/tests/* $b/i686/tests/* $b/tmp/*" |
                paste -s -d ' ' -
            echo "stale 14 $b/runs/*"
        done
    } | perl -e 'alarm shift; exec @ARGV' 600 ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" \
        'MSYS_NO_PATHCONV=1 wsl.exe -u root -- sh -s' 2>&1 | tr -d '\000\r' | sed "s/^/$TESTING: /"
    {
        printf '%s\n' "$stale"
        echo 'b=/d/rach2/build'
        # shellcheck disable=SC2016 # the dollars are the remote shell's
        echo 'stale 1 $b/tests/rach2-collection.* $b/tooling/job-* $b/images/cases/* $b/x86_64/images/cases/* $b/tmp/*'
    } | perl -e 'alarm shift; exec @ARGV' 600 ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" \
        'bash -s' 2>&1 | tr -d '\000\r' | sed 's/^/windows: /'
    eval "$stale"
    stale 14 "$repo"/build/tests/* 2>&1 | sed "s/^/$(hostname -s): /"
    git worktree prune
}

# clones: run `tidy` in each worker's clone, as its account.
clones()
{
    grep -v '^#' "$(dirname "$0")/workers" | while read -r name host _; do
        [ -n "$name" ] && [ "$host" != - ] || continue
        {
            cat "$(dirname "$0")/lib.sh"
            # shellcheck disable=SC2016 # the dollar is the remote shell's
            printf '%s\n' 'log() { echo "$*"; }' 'cd rach2 && git fetch -q origin && tidy'
        } | perl -e 'alarm shift; exec @ARGV' 600 ssh -o BatchMode=yes -o ConnectTimeout=10 "$host" \
            sh -s 2>&1 | sed "s/^/$name: /"
    done
}

main()
{
    repo=$(cd "$(dirname "$0")/../.." && pwd)
    cd "$repo" || exit 1
    git fetch -q origin || echo "sweep: the fetch failed; master's stems may be old"
    state=
    tidy
    for state in "$repo"/build/flow/*; do
        [ -f "$state/loop.log" ] && prune_logs 's-*'
    done
    state=
    clones
    stems=$(stems origin/master | tr '\n' ' ')
    # shellcheck disable=SC2086 # the stems are a word each
    perl "$(dirname "$0")/scratch.pl" -d "$repo/build/tmp" 86400 $stems 2>&1 |
        sed "s/^/$(hostname -s): /"
    dirs='-d /tmp'
    for h in $HOMES; do
        dirs="$dirs -d $h/rach2/build/tmp"
    done
    perl -e 'alarm shift; exec @ARGV' 600 \
        ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" \
        "MSYS_NO_PATHCONV=1 wsl.exe -u root -- perl - $dirs 86400 $stems" \
        < "$(dirname "$0")/scratch.pl" 2>&1 | sed "s/^/$TESTING: /"
    # shellcheck disable=SC2119 # leftovers takes -n, not the sweep's arguments
    leftovers 2>&1 | grep -v ': leftovers: keeps '
    outputs
    store_expire 2>&1
}

# shellcheck source=tools/flow/lib.sh
. "$(dirname "$0")/lib.sh"

# log TEXT: print TEXT, with the loop state directory it concerns, if any,
# first, since the sweep's removals are lines of its output and not of a
# loop's log.
log()
{
    echo "${state:+$state: }$*"
}

main "$@"
