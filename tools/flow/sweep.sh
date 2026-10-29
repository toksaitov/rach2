#!/bin/sh
#
# sweep.sh: remove what finished work left on the testing host and on this
# computer, as the Resources section of AGENTS.md allows. The manager runs
# it at the end of each milestone and whenever space runs low.
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
# uses. Each removal, and each step's total, is a line on stdout. Each step
# that reaches the host stops itself after ten minutes.

TESTING=testing

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
