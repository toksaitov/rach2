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
set -u
[ -n "${1:-}" ] || { echo "usage: manifest_deps.sh MANIFEST" >&2; exit 2; }
awk '$1 == "file" || $1 == "exec-dir" || $1 == "tree" { print $4 }
     $1 == "optional-file" { print "?" $4 }' "$1" | while IFS= read -r p; do
    case "$p" in
        '?'*) [ -e "${p#?}" ] && echo "${p#?}" ;;
        *) echo "$p" ;;
    esac
done
