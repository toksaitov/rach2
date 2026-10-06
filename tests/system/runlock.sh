# runlock.sh: the tree lock for a script that changes sources in place.
#
# A fault script puts a fault into this tree's sources and builds and boots
# it, so no suite may build or boot the tree, and no other proof may save a
# source, while it runs. tests/run keeps two runs apart with a POSIX record
# lock on build/.run.guard and a directory, build/.run.lock, whose owner
# file names the holder. A script sources this file and calls
#
#   runlock_hold "$repo" NAME "$repo/PATH/OF/THE/SCRIPT" "$@"
#
# before it touches the tree. Unless the tree is already held for it, this
# runs the script again under runlock_exec.sh, which takes both locks, runs
# the script as its child and removes the directory when the child ends.
# The child, and any tests/run or script it starts, finds the tree held by
# an ancestor through RACH2_TREE_HELD and goes on without taking it again.

# runlock_held REPO: whether the run lock of the tree at REPO is held by an
# ancestor of this shell: RACH2_TREE_HELD names a process, the lock's owner
# file names the same one, and it is this shell's parent, or that parent's,
# up to the first process.
runlock_held()
{
    runlock_owner=${RACH2_TREE_HELD:-}
    [ -n "$runlock_owner" ] || return 1
    [ "$(sed -n 's/^pid //p' "$1/build/.run.lock/owner" 2>/dev/null)" = "$runlock_owner" ] ||
        return 1
    runlock_pid=$$
    while [ -n "$runlock_pid" ] && [ "$runlock_pid" -gt 1 ]; do
        [ "$runlock_pid" = "$runlock_owner" ] && return 0
        runlock_pid=$(ps -o ppid= -p "$runlock_pid" 2>/dev/null | tr -d ' ')
    done
    return 1
}

# runlock_hold REPO NAME SCRIPT ARGS...: return when the tree at REPO is
# held for this script; otherwise run SCRIPT with ARGS again under
# runlock_exec.sh, which does not return. NAME prefixes the wrapper's
# messages. SCRIPT is an absolute path, since the caller has changed
# directory to the top of the tree.
runlock_hold()
{
    runlock_repo=$1
    runlock_name=$2
    shift 2
    runlock_held "$runlock_repo" && return 0
    exec sh "$runlock_repo/tests/system/runlock_exec.sh" "$runlock_name" "$@"
}
