# shellcheck shell=sh
# hold.sh: the hold of this tree for a script that changes its sources in
# place or builds in it, through etude-hold.
#
# A fault script puts a fault into the tree's sources and builds and boots
# it, so no suite may build or boot the tree, and no other proof may save a
# source, while it runs. A script sources this file and calls
#
#   etude_hold "$repo" NAME "$repo/PATH/OF/THE/SCRIPT" "$@"
#
# before it touches the tree. Unless the tree is already held for it, this
# compiles etude-hold into a private directory under TMPDIR, so that nothing
# is written in the tree before the hold, and runs the script again under
# it. The helper takes the tree's lock, runs the script with RACH2_TREE_HELD
# naming the helper, and removes the lock when the script has ended; the
# script, and any tests/run or script it starts, finds the tree held by an
# ancestor and goes on without taking it again.

# etude_held REPO: whether the run lock of the tree at REPO is held for an
# ancestor of this shell: RACH2_TREE_HELD names a process, the lock's owner
# record names the same one, and it is this shell, its parent, or that
# parent's, up to the first process.
etude_held()
{
    etude_held_owner=${RACH2_TREE_HELD:-}
    [ -n "$etude_held_owner" ] || return 1
    [ "$(sed -n 's/^pid //p' "$1/build/.run.lock/owner" 2>/dev/null)" = "$etude_held_owner" ] ||
        return 1
    etude_held_pid=$$
    while [ -n "$etude_held_pid" ] && [ "$etude_held_pid" -gt 1 ]; do
        [ "$etude_held_pid" = "$etude_held_owner" ] && return 0
        etude_held_pid=$(ps -o ppid= -p "$etude_held_pid" 2>/dev/null | tr -d ' ')
    done
    return 1
}

# etude_hold REPO NAME SCRIPT ARGS...: return when the tree at REPO is held
# for this script; otherwise run `sh SCRIPT ARGS` under etude-hold, which
# does not return. NAME prefixes the helper's messages. SCRIPT is an
# absolute path, since the caller may have changed directory. The helper
# removes its private directory as it starts.
etude_hold()
{
    etude_hold_repo=$1
    etude_hold_name=$2
    shift 2
    etude_held "$etude_hold_repo" && return 0
    etude_hold_dir=
    trap 'rm -rf ${etude_hold_dir:+"$etude_hold_dir"}; exit 129' HUP
    trap 'rm -rf ${etude_hold_dir:+"$etude_hold_dir"}; exit 130' INT
    trap 'rm -rf ${etude_hold_dir:+"$etude_hold_dir"}; exit 143' TERM
    etude_hold_dir=$(mktemp -d "${TMPDIR:-/tmp}/rach2-hold.XXXXXX") || exit 1
    if ! cc -std=c99 -O2 -D_DEFAULT_SOURCE -o "$etude_hold_dir/etude-hold" \
        "$etude_hold_repo/tools/etude/etude_hold.c" "$etude_hold_repo/tools/etude/posix/hold.c"; then
        echo "$etude_hold_name: etude-hold did not compile" >&2
        rm -rf "$etude_hold_dir"
        exit 1
    fi
    exec "$etude_hold_dir/etude-hold" -s "$etude_hold_dir" "$etude_hold_repo" "$etude_hold_name" \
        sh "$@"
}
