#!/bin/sh
#
# stage_logins.sh: put a directory of logins into root's home in an ext2 root
# image, for `make machine LOGINS=DIR` (mk/gnu-image.mk).
#
# Usage: stage_logins.sh IMAGE DIR
#        stage_logins.sh --digest [DIR]
#
# DIR is laid out from the machine's root, as tools/dev/machine_logins.sh
# pull writes it, and its tree root/ is what is staged: each directory and
# file below DIR/root goes to the same place below /root in IMAGE, owned by
# root, with its mode in DIR less the group's and others' write bits, as
# mkext2.sh gives what its overlay adds. A file IMAGE has at that place is
# replaced. Every file is then read back by SHA-256 and IMAGE must check
# clean with e2fsck; a failure leaves IMAGE part written, and the rule that
# runs this removes it. DIR's other trees, such as home/, are not staged,
# since an install makes its account's home anew and an update keeps the
# machine's.
#
# --digest prints one line that changes when what DIR/root would stage
# changes, its names, modes and contents, and `none` when DIR is empty or
# not given; the rule keeps it in a stamp, so that an image staged with
# other logins, or none, is made again. It names no file and gives no
# digest of one.
#
# Exit 0 when the logins are staged, 1 when a step fails, 2 on a usage
# error.
set -u

ME=stage-logins
fail()
{
    echo "$ME: failed: $*" >&2
    exit 1
}

# digest [FILE]: print the SHA-256 of FILE, or of standard input.
digest()
{
    if command -v sha256sum > /dev/null; then
        sha256sum ${1:+"$1"}
    else
        shasum -a 256 ${1:+"$1"}
    fi | cut -d' ' -f1
}

# perm PATH: print PATH's permission bits in octal.
perm()
{
    stat -c %a "$1" 2> /dev/null || stat -f %Lp "$1"
}

# tree DIR: check DIR/root and print what is below it, a node to a line, as
# `d MODE PATH` or `f MODE PATH`, PATH relative to DIR/root and MODE the
# staged mode.
tree()
{
    [ -d "$1/root" ] || fail "$1 has no directory root"
    odd=$(cd "$1/root" && find . ! -type f ! -type d | head -n 1)
    [ -z "$odd" ] || fail "$1/root has ${odd#./}, which is neither a file nor a directory"
    ! (cd "$1/root" && find . | LC_ALL=C grep -q '[[:cntrl:]"\\]') ||
        fail "$1/root has a name with a quote, a backslash or a control character"
    (cd "$1/root" && find . -mindepth 1 | LC_ALL=C sort) | while IFS= read -r p; do
        p=${p#./}
        mode=$(perm "$1/root/$p") || exit 1
        if [ -d "$1/root/$p" ]; then t=d; else t=f; fi
        printf '%s %03o %s\n' "$t" $((0$mode & 0755)) "$p"
    done
}

# home PATH: print PATH with a leading ~/ made $HOME's, since make passes
# LOGINS=~/... on as it was typed.
home()
{
    # shellcheck disable=SC2088
    case $1 in "~/"*) printf '%s\n' "$HOME/${1#??}" ;; *) printf '%s\n' "$1" ;; esac
}

if [ "${1:-}" = --digest ]; then
    [ "$#" -le 2 ] || { echo "usage: stage_logins.sh --digest [DIR]" >&2; exit 2; }
    if [ -z "${2:-}" ]; then
        echo none
        exit 0
    fi
    dir=$(home "$2")
    list=$(tree "$dir") || exit 1
    printf '%s\n' "$list" | while read -r t mode p; do
        [ -n "$p" ] || continue
        printf '%s %s %s %s\n' "$t" "$mode" "$p" "$([ "$t" = d ] || digest "$dir/root/$p")"
    done | digest
    exit 0
fi

[ "$#" -eq 2 ] || { echo "usage: stage_logins.sh IMAGE DIR" >&2; exit 2; }
image=$1
dir=$(home "$2")
[ -f "$image" ] || fail "$image is not a file"
REPO=$(cd "$(dirname "$0")/../.." && pwd)
. "$REPO/tools/setup/e2fsprogs_path.sh"
for t in debugfs e2fsck; do
    command -v "$t" > /dev/null || fail "$t is missing; install e2fsprogs"
done

work=
trap 'rm -rf ${work:+"$work"}' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$REPO/build/tmp" || exit 1
work=$(mktemp -d "$REPO/build/tmp/stage-logins.XXXXXX") || exit 1
tree "$dir" > "$work/tree" || exit 1
[ -s "$work/tree" ] || fail "$dir/root is empty"

# debugfs goes on past a command that fails, so `mkdir` on a directory the
# image has and `rm` of a file it lacks cost only a line of its log; what was
# meant to be there is checked below.
while read -r t mode p; do
    if [ "$t" = d ]; then
        echo "mkdir \"/root/$p\""
        echo "set_inode_field \"/root/$p\" mode 040$mode"
    else
        echo "rm \"/root/$p\""
        echo "write \"$dir/root/$p\" \"/root/$p\""
        echo "set_inode_field \"/root/$p\" mode 0100$mode"
    fi
    echo "set_inode_field \"/root/$p\" uid 0"
    echo "set_inode_field \"/root/$p\" gid 0"
done < "$work/tree" > "$work/script"
debugfs -w -f "$work/script" "$image" > "$work/debugfs.log" 2>&1 ||
    fail "debugfs could not open $image"

n=0
while read -r t mode p; do
    debugfs -R "stat \"/root/$p\"" "$image" > "$work/stat" 2> /dev/null
    want=directory
    [ "$t" = d ] || want=regular
    grep -q "Type: $want .*Mode: *0*$mode " "$work/stat" ||
        fail "/root/$p is not a $want node of mode $mode in $image"
    grep -q 'User: *0 *Group: *0 ' "$work/stat" || fail "/root/$p is not root's in $image"
    [ "$t" = d ] && continue
    got=$(debugfs -R "cat \"/root/$p\"" "$image" 2> /dev/null | digest)
    [ "$got" = "$(digest "$dir/root/$p")" ] || fail "/root/$p in $image differs from $dir/root/$p"
    n=$((n + 1))
done < "$work/tree"
e2fsck -fn "$image" > "$work/e2fsck.log" 2>&1 ||
    fail "$image does not check clean: $(tail -n 3 "$work/e2fsck.log" | tr '\n' ' ')"
echo "$ME: result: $n files of $dir/root in /root of $image"
