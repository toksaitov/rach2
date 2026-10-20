#!/bin/sh
#
# version.sh: build etude from this tree's source on a machine, over SSH,
# into that machine's cache of versions, unless the cache has it already.
# The machine needs a POSIX sh, make, a C compiler and a SHA-256 tool, and
# nothing of etude.
#
# Usage: sh tools/etude/version.sh [-r REV] [-c CC] [-C CACHE] [-w] HOST
#        sh tools/etude/version.sh [-r REV] [-c CC] [-C CACHE] -l
#        sh tools/etude/version.sh [-r REV] [-c CC] [-C CACHE] -s
#
# The source is that of the working tree, or with -r that of the commit REV.
# HOST is a destination of ssh(1), which runs `sh -s` there with the script
# on its standard input; with -w the machine is Windows, whose account's
# shell, cmd.exe or PowerShell, runs MSYS2's sh. -l runs the script here,
# and -s prints it and runs nothing. CC is the compiler's command line on the
# machine and CACHE the cache's directory, each by default the platform's
# (tools/etude/Readme.md, The version cache).
#
# On success it prints `etude-version: result: built KEY DIR` or
# `etude-version: result: reused KEY DIR`, DIR being the version's directory
# on the machine, and exits 0. Any other output of the machine's goes to
# stderr. Exit 1 when the version is neither built nor found, 2 on a usage
# error.
#
set -u

usage()
{
    echo 'usage: version.sh [-r REV] [-c CC] [-C CACHE] [-w] HOST | -l | -s' >&2
    exit 2
}

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd) || exit 2
tree=$(CDPATH='' cd -- "$here/../.." && pwd) || exit 2
rev=
cc=
cache=
windows=0
mode=remote
while getopts r:c:C:wls opt; do
    case $opt in
        r) rev=$OPTARG ;;
        c) cc=$OPTARG ;;
        C) cache=$OPTARG ;;
        w) windows=1 ;;
        l) mode=local ;;
        s) mode=script ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
if [ "$mode" = remote ]; then
    [ "$#" -eq 1 ] || usage
    host=$1
else
    [ "$#" -eq 0 ] && [ "$windows" = 0 ] || usage
fi

# shellcheck source=tools/etude/bootstrap.sh
. "$here/bootstrap.sh"

work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
work=$(mktemp -d "${TMPDIR:-/tmp}/etude-version.XXXXXX") || exit 1

fail()
{
    echo "etude-version: $*" >&2
    exit 1
}

# quote TEXT: print TEXT as one word that sh reads back as TEXT.
quote()
{
    printf "'%s'" "$(printf '%s' "$1" | sed "s/'/'\\\\''/g")"
}

src=$tree
if [ -n "$rev" ]; then
    src=$work/tree
    mkdir "$src" || exit 1
    git -C "$tree" archive --format=tar "$rev" -- tools/etude utils/sha256.c utils/sha256.h |
        tar -xf - -C "$src" || fail "cannot read the source of $rev"
fi
(cd "$src" && find tools/etude utils/sha256.c utils/sha256.h -type f) |
    etude_select | LC_ALL=C sort > "$work/files" || exit 1
[ -s "$work/files" ] || fail "no source in $src"
etude_list "$src" < "$work/files" > "$work/sources" || exit 1
source=$(etude_hash "$work/sources") || fail 'no SHA-256 tool'
nonce=$(od -An -N8 -tx1 /dev/urandom | tr -d ' \n')
[ "${#nonce}" -eq 16 ] || fail 'cannot read /dev/urandom'
end=@etude-$nonce

# Each file goes in a here-document, which carries text whose lines all end
# in a newline, and which the line $end does not end early.
while read -r f; do
    case $f in
        *[!A-Za-z0-9._/-]*) fail "$f: the name is not a plain path" ;;
    esac
    [ -z "$(tail -c 1 "$src/$f")" ] || fail "$f: the last line has no newline"
    [ "$(tr -d '\000' < "$src/$f" | wc -c)" -eq "$(wc -c < "$src/$f")" ] || fail "$f: the file has a NUL byte"
    ! grep -qx -- "$end" "$src/$f" || fail "$f: a line is $end"
done < "$work/files"

{
    echo "ETUDE_NONCE=$nonce"
    echo "ETUDE_SOURCE=$source"
    echo "ETUDE_CC=$(quote "$cc")"
    echo "ETUDE_CACHE=$(quote "$cache")"
    cat "$here/bootstrap.sh"
    echo
    echo 'etude_files()'
    echo '{'
    echo "    cat <<'$end'"
    cat "$work/files"
    echo "$end"
    echo '}'
    echo
    echo 'etude_unpack()'
    echo '{'
    sed 's#/[^/]*$##' "$work/files" | LC_ALL=C sort -u | while read -r d; do
        echo "    mkdir -p \"\$1/$d\" || return 1"
    done
    while read -r f; do
        echo "    cat > \"\$1/$f\" <<'$end' || return 1"
        cat "$src/$f"
        echo "$end"
    done < "$work/files"
    echo '}'
    echo
    echo 'etude_main < /dev/null'
} > "$work/script" || exit 1

case $mode in
    script) cat "$work/script"; exit ;;
    local) sh -s < "$work/script" > "$work/out" ;;
    remote)
        if [ "$windows" = 1 ]; then
            remote='C:\msys64\usr\bin\env.exe PATH=/usr/bin:/ucrt64/bin sh -s'
        else
            remote='sh -s'
        fi
        ssh -T -o BatchMode=yes -- "$host" "$remote" < "$work/script" > "$work/out" ;;
esac
status=$?
grep -v "^@etude-version $nonce " "$work/out" >&2
result=$(sed -n "s/^@etude-version $nonce //p" "$work/out")
[ "$status" -eq 0 ] || fail "the machine's sh exited $status"
case $result in
    built\ *|reused\ *) ;;
    '') fail 'the machine gave no result' ;;
    *) fail "the machine gave a result it should not: $result" ;;
esac
[ "$(printf '%s\n' "$result" | wc -l)" -eq 1 ] || fail 'the machine gave more than one result'
echo "etude-version: result: $result"
