#!/usr/bin/env sh
#
# manifest_deps.sh: the host paths a manifest stages, one per line, for the
# image rules' prerequisites. A rebuilt program the manifest names then
# rebuilds the image that carries it; before this the image rules took the
# components as order-only prerequisites and a stale image could boot.
#
# Usage: manifest_deps.sh MANIFEST
#
# `file`, `optional-file`, `exec-dir` and `tree` lines name a host path in
# their fourth field; an optional file that is absent is left out, since a
# missing prerequisite would stop make where the manifest allows absence.
#
# A directory is printed as the files the stager would take from it, and by the
# same rule the stager uses: `exec-dir` takes the regular executable files of
# that one directory, `tree` takes every regular file under it. The directory
# itself is not printed. It would keep its time when a program in it is
# relinked, and the source trees a `tree` line stages, `utils`, `rach2` and
# `ish`, are also the names of phony targets, which make holds to be out of
# date every time: the image was then rebuilt on every invocation, and two
# suites of a parallel run rebuilt it at once.
set -u
[ -n "${1:-}" ] || { echo "usage: manifest_deps.sh MANIFEST" >&2; exit 2; }
awk '$1 == "file" || $1 == "optional-file" || $1 == "exec-dir" || $1 == "tree" {
         print $1 " " $4
     }' "$1" | while read -r kind p; do
    case "$kind" in
        file)
            echo "$p"
            ;;
        optional-file)
            [ -e "$p" ] && echo "$p"
            ;;
        exec-dir)
            [ -d "$p" ] &&
                find "$p" -maxdepth 1 -type f \
                    \( -perm -u+x -o -perm -g+x -o -perm -o+x \)
            ;;
        tree)
            if [ -d "$p" ]; then
                find "$p" -type f
            else
                echo "$p"
            fi
            ;;
    esac
done
