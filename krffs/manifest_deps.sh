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
# A directory is printed as the files under it. A `tree` line stages a source
# directory, `utils`, `rach2` or `ish`, and the Makefile has a phony target of
# each of those names: make holds a phony target to be out of date every time,
# so an image whose manifest staged a source tree was rebuilt on every
# invocation, and two suites of a parallel run rebuilt it at once. The files
# are also the right answer for `exec-dir`, whose directory keeps its time
# when a program in it is relinked.
set -u
[ -n "${1:-}" ] || { echo "usage: manifest_deps.sh MANIFEST" >&2; exit 2; }
awk '$1 == "file" || $1 == "exec-dir" || $1 == "tree" { print $4 }
     $1 == "optional-file" { print "?" $4 }' "$1" | while IFS= read -r p; do
    case "$p" in
        '?'*) p=${p#?}; [ -e "$p" ] || continue ;;
    esac
    if [ -d "$p" ]; then
        find "$p" -type f
    else
        echo "$p"
    fi
done
