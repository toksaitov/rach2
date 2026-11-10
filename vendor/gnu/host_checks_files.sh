#!/bin/sh
# vendor/gnu/host_checks_files.sh: the files of the tree that the GNU host
# checks compile, read and run, which are both the inputs of the
# gnu-host-checks step's key and the files its tree carries.
#
#   host_checks_files.sh         print them, one to a line
#   host_checks_files.sh TREE    copy them into TREE, with their modes and times
#
# They are the files under PATHS that git tracks or, where the tree is not a
# git checkout, as in the tooling machine, that build/gnu-source-files lists,
# less compressed files and disk images. A check that comes to compile or read
# a file elsewhere fails in the step until its path is added here; a file of a
# directory below is in the key whether or not a check uses it.
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
# etude_legacy lists tests/cases at run time, so the directory goes whole.
PATHS='AGENTS.md CLAUDE.md Readme.md mk/host-check.mk mk/host-fixtures.mk
    tests/host tests/fuzz tests/native tests/cases tests/system/checkers
    tests/system/fakeapi tests/fixtures/agents tests/fixtures/lenovo-gpu/i915_opregion.bin
    tests/fixtures/lenovo-gpu/i915_vbt.bin tests/musl tests/clockref.h tests/registry
    tools/etude tools/inspect tools/verify tools/image tools/ci tools/font
    tools/flow/test_status.py tools/setup/e2fsprogs_path.sh tools/host_files.h
    rach2/sys rach2/kernel.h rach2/vendor rach2/include krffs utils ish term ports/doom
    bed/vm/backend bed/shm
    vendor/gnu/fstools/positional_io.c vendor/gnu/ports/wpa_supplicant.sh'

# list: print the files, or nothing and fail when there is no list to read.
list()
{
    if git rev-parse --git-dir > /dev/null 2>&1; then
        # shellcheck disable=SC2086 # PATHS is a list of words
        names=$(git -c core.quotepath=off ls-files -- $PATHS) || return 1
    elif [ -f build/gnu-source-files ]; then
        names=$(tr '\0' '\n' < build/gnu-source-files | awk -v paths="$PATHS" '
            BEGIN { n = split(paths, p) }
            { for (i = 1; i <= n; i++) if ($0 == p[i] || index($0, p[i] "/") == 1) { print; next } }')
    else
        return 1
    fi
    printf '%s\n' "$names" | grep -v -E '\.(xz|gz|bz2|zst|wad|img)$' | while IFS= read -r f; do
        if [ -f "$f" ]; then printf '%s\n' "$f"; fi
    done
}

if [ $# -eq 0 ]; then
    list
    exit
fi
[ $# -eq 1 ] || {
    echo 'usage: host_checks_files.sh [TREE]' >&2
    exit 2
}
files=$(list) || {
    echo 'host checks: no git checkout and no build/gnu-source-files to list the files from' >&2
    exit 1
}
mkdir -p "$1"
printf '%s\n' "$files" | tar -c -f - -T - | tar -x -p -f - -C "$1"
