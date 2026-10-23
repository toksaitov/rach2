# shellcheck shell=sh
# hold.sh: the hold of this tree for a script that changes its sources in
# place or builds in it, through `etude lock tree`.
#
# No suite may build or boot the tree while such a script runs. A script
# sources this file and calls
#
#   etude_hold "$repo" NAME "$repo/PATH/OF/THE/SCRIPT" "$@"
#
# before it touches the tree. Unless the tree is already held for it, this
# compiles etude with its `lock` verb alone into a private directory under
# TMPDIR, so that nothing is written in the tree before the hold, and runs the
# script again under `etude lock tree`. The helper takes the tree's lock, runs
# the script with RACH2_TREE_HELD naming the helper, and removes the lock when
# the script has ended; the script, and any tests/run or script it starts, finds
# the tree held by an ancestor and goes on without taking it again.

# etude_held REPO: whether the run lock of the tree at REPO is held for an
# ancestor of this shell: RACH2_TREE_HELD names a process, the lock's owner
# record names the same one, and it is this shell, its parent, or that
# parent's, up to the first process. Windows also accepts membership in the
# live holder's command job, across an ended MSYS intermediate process.
etude_held()
{
    case $(uname -s) in
        MINGW*|MSYS*|UCRT*|CLANG*)
            [ -n "${RACH2_HOLD_HELPER:-}" ] && [ -x "$RACH2_HOLD_HELPER" ] || return 1
            MSYS2_ARG_CONV_EXCL='*' "$RACH2_HOLD_HELPER" lock tree -q "$(cygpath -m "$1")"
            return $? ;;
    esac
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

# etude_lock_build REPO OUT CC FLAG... HOLD: compile into OUT etude with
# its `lock` verb alone, from the tree at REPO, by the compiler CC with the
# FLAGs, HOLD being the system half of the tree's lock under tools/etude.
etude_lock_build()
{
    etude_lock_repo=$1/tools/etude
    etude_lock_out=$2
    shift 2
    etude_lock_cc=$1
    shift
    etude_lock_flags=
    while [ "$#" -gt 1 ]; do
        etude_lock_flags="$etude_lock_flags $1"
        shift
    done
    # shellcheck disable=SC2086 # one flag a word
    "$etude_lock_cc" -std=c99 -O2 $etude_lock_flags -DETUDE_LOCK_ONLY -I"$etude_lock_repo/include" \
        -I"$etude_lock_repo" -o "$etude_lock_out" "$etude_lock_repo/etude.c" \
        "$etude_lock_repo/lock_tree.c" "$etude_lock_repo/$1" "$etude_lock_repo/lock_ports.c" \
        "$etude_lock_repo/portlock.c" "$etude_lock_repo/win/winfmt.c"
}

# etude_hold REPO NAME SCRIPT ARGS...: return when the tree at REPO is held
# for this script; otherwise run `sh SCRIPT ARGS` under `etude lock tree`, which
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
    case $(uname -s) in
        MINGW*|MSYS*|UCRT*|CLANG*)
            trap '[ -z "$etude_hold_dir" ] || rm -rf "$etude_hold_dir"' 0
            etude_hold_source=$(cygpath -m "$etude_hold_repo") || exit 1
            etude_hold_output=$(cygpath -m "$etude_hold_dir") || exit 1
            if ! etude_lock_build "$etude_hold_source" "$etude_hold_output/etude.exe" \
                "${ETUDE_WIN_CC:-/ucrt64/bin/gcc}" -static -D_WIN32_WINNT=0x0A00 win/hold.c; then
                echo "$etude_hold_name: etude did not compile" >&2
                exit 1
            fi
            RACH2_HOLD_HELPER=$(cygpath -m "$etude_hold_dir/etude.exe") || exit 1
            export RACH2_HOLD_HELPER
            if etude_held "$etude_hold_repo"; then
                rm -rf "$etude_hold_dir"
                unset RACH2_HOLD_HELPER
                trap - 0 HUP INT TERM
                return 0
            fi
            etude_hold_arg_conv=${MSYS2_ARG_CONV_EXCL-}
            MSYS2_ARG_CONV_EXCL='*' "$RACH2_HOLD_HELPER" lock tree "$(cygpath -m "$etude_hold_repo")" "$etude_hold_name" \
                "$(cygpath -m "$(command -v env)")" "MSYS2_ARG_CONV_EXCL=$etude_hold_arg_conv" \
                "$(cygpath -m "$(command -v sh)")" "$@"
            exit $? ;;
    esac
    if ! etude_lock_build "$etude_hold_repo" "$etude_hold_dir/etude" cc -D_DEFAULT_SOURCE posix/hold.c; then
        echo "$etude_hold_name: etude did not compile" >&2
        rm -rf "$etude_hold_dir"
        exit 1
    fi
    exec "$etude_hold_dir/etude" lock tree -s "$etude_hold_dir" "$etude_hold_repo" "$etude_hold_name" \
        sh "$@"
}
