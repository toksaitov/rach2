#!/bin/sh
#
# sweep.sh: remove what finished work left on the testing host and on this
# computer, as the Resources section of AGENTS.md allows. The manager runs
# it at the end of each milestone and whenever space runs low.
#
# Usage: sweep.sh
#
# It runs `scratch.pl` as root on the testing host
# with the stems of master's scratch names, which removes from its /tmp the
# scratch of tests that were killed before their traps ran; runs
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
    perl -e 'alarm shift; exec @ARGV' 600 \
        ssh -o BatchMode=yes -o ConnectTimeout=10 "$TESTING" \
        "MSYS_NO_PATHCONV=1 wsl.exe -u root -- perl - /tmp 86400 $(stems origin/master | tr '\n' ' ')" \
        < "$(dirname "$0")/scratch.pl" 2>&1 | sed "s/^/$TESTING: /"
    leftovers 2>&1 | grep -v ': leftovers: keeps '
    store_expire 2>&1
}

. "$(dirname "$0")/lib.sh"
main "$@"
