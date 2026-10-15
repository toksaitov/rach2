#!/bin/sh
#
# programs.sh: the programs an image stages from its `exec-dir` directories,
# kept as a file make can compare times with.
#
# `exec-dir` stages the regular files with an execute bit directly in one
# host directory, dotfiles left out. An image must be staged again when that
# set changes, and a directory's own time changes more often: whenever any
# entry is made or removed in it, and the directories an image stages
# programs from, build/tests among them, also have the work directories of
# proofs and suites. So mkimage.sh writes the set it staged to
# IMAGE.programs, one line a directory, the directory and then the names,
# and the image's dependency file lists that file in place of the
# directories. The root Makefile has this script check the file each time
# it reads the dependency files, and the file is written again, putting the
# image out of date, only when the set differs from the one recorded.
#
# Usage: programs.sh snapshot DIR...   print the set each DIR holds now
#        programs.sh check FILE...     write each FILE again whose
#                                      directories hold another set
set -u
# One order of names on every host, the one stageimage's strcmp gives.
LC_ALL=C
export LC_ALL

# snapshot DIR...: one line per directory, the directory and the sorted names
# of the regular files directly in it that the builder may execute, which
# are the built programs; a symbolic link is left out, as stageimage's lstat
# leaves it out.
snapshot() {
    for d in "$@"; do
        printf '%s' "$d"
        for f in "$d"/*; do
            [ -f "$f" ] && [ ! -L "$f" ] && [ -x "$f" ] && printf ' %s' "${f##*/}"
        done
        printf '\n'
    done
}

case "${1:-}" in
    snapshot)
        shift
        snapshot "$@"
        ;;
    check)
        shift
        for file in "$@"; do
            [ -f "$file" ] || continue
            # shellcheck disable=SC2046 # the directories, one word each
            now=$(snapshot $(cut -d' ' -f1 "$file"))
            if [ "$now" != "$(cat "$file")" ]; then
                printf '%s\n' "$now" > "$file.$$" && mv "$file.$$" "$file"
            fi
        done
        ;;
    *)
        echo "usage: programs.sh snapshot DIR... | check FILE..." >&2
        exit 2
        ;;
esac
