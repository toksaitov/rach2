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
# their fourth field, which is read through RACH2_PATH_MAP the way the stager
# reads it (stageimage.krffs.c, host_path): an x86-64 build maps build/... to
# build/x86_64/..., and the path printed is the one the stager will read. An
# optional file that is absent is left out, since a missing prerequisite
# would stop make where the manifest allows absence. A path under `build/`
# that is not there yet is left out too: it is a component's product, which
# the image rule's order-only prerequisites build first, and on a built tree
# it exists and is printed, so a rebuilt program still restages its image. A
# source path is printed whether or not it exists, since its absence is a
# fault the stager reports.
#
# A directory is printed as the files the stager would take from it, and by the
# same rule the stager uses: `exec-dir` takes the regular executable files of
# that one directory, `tree` takes every regular file under it, and neither
# takes a name that begins with a dot. The directory
# itself is not printed. It would keep its time when a program in it is
# relinked, and the source trees a `tree` line stages, `utils`, `rach2` and
# `ish`, are also the names of phony targets, which make holds to be out of
# date every time: the image was then rebuilt on every invocation, and two
# suites of a parallel run rebuilt it at once.
set -u
[ -n "${1:-}" ] || { echo "usage: manifest_deps.sh MANIFEST" >&2; exit 2; }

# map_path PATH: set `mapped` to the path the stager reads for PATH, by the
# first FROM=TO pair of RACH2_PATH_MAP whose FROM begins it. It runs in the
# loop's own shell, with no command substitution, since make reads every
# manifest this way each time it starts.
map_path() {
    mapped=$1
    for pair in ${RACH2_PATH_MAP:-}; do
        from=${pair%%=*}
        case "$1" in
            "$from"*) mapped=${pair#*=}${1#"$from"}; return ;;
        esac
    done
}

awk '$1 == "file" || $1 == "optional-file" || $1 == "exec-dir" || $1 == "tree" {
         print $1 " " $4
     }' "$1" | while read -r kind p; do
    map_path "$p"
    p=$mapped
    case "$kind" in
        file)
            case "$p" in
                build/*) [ -e "$p" ] && echo "$p" ;;
                *) echo "$p" ;;
            esac
            ;;
        optional-file)
            [ -e "$p" ] && echo "$p"
            ;;
        exec-dir)
            [ -d "$p" ] &&
                find "$p" -maxdepth 1 -type f -not -name '.*' \
                    \( -perm -u+x -o -perm -g+x -o -perm -o+x \)
            ;;
        tree)
            if [ -d "$p" ]; then
                find "$p" -name '.*' -prune -o -type f -print
            elif [ -e "$p" ] || [ "${p#build/}" = "$p" ]; then
                echo "$p"
            fi
            ;;
    esac
done
