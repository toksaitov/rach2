#!/bin/sh
#
# hookcheck.sh: the formatting of the git hook pre-commit, and a command that
# formats files by hand.
#
# `commit`, which tools/git-hooks/pre-commit runs, formats each staged
# in-house C file and header, the set the style case `clang-format` reads,
# with the clang-format version tools/setup/judges records, writes the result
# over the file and stages it. A file whose staged text needs formatting and
# that also has unstaged changes is left as it is, with a warning, since
# formatting it would overwrite those changes. The hook refuses nothing: the
# rest of `tests/run style` runs at the end of a milestone.
#
# `format FILE ...` formats the files, relative to the root of the tree, in
# place with the recorded version.
#
# The formatting reads the staged files, with the tree's .clang-format, from
# a private directory under TMPDIR, so that it judges what is committed and
# not the working files.
#
# The recorded clang-format is `clang-format` or `clang-format-N` on PATH, N
# its major version, whose first line of `--version` is the record's. When
# neither is, the files go to the testing host in one ssh call, as a tar file
# with the tree's .clang-format, and come back formatted. The host is
# `testing`, or the one RACH2_STYLE_HOST names; an empty name means none.
# With no recorded clang-format anywhere, the script warns and goes on.
#
# Usage: hookcheck.sh commit | format FILE ...
#
set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
MODE=${1-}
[ "$#" -gt 0 ] && shift
case $MODE in
    commit) [ "$#" -eq 0 ] ;;
    format) [ "$#" -gt 0 ] ;;
    *) false ;;
esac || { echo 'hookcheck: usage: hookcheck.sh commit | format FILE ...' >&2; exit 2; }
TOP=$(git rev-parse --show-toplevel) && cd "$TOP" || exit 2
JUDGED=$(sed -n 's/^clang-format: //p' "$REPO/tools/setup/judges")
HOST=${RACH2_STYLE_HOST-testing}

WORK=
trap 'rm -rf ${WORK:+"$WORK"}' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
WORK=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/rach2-hookcheck.XXXXXX") || exit 2

# c_files TREE LIST: print the in-house C files and headers of TREE that the
# file LIST names, by the function inhouse_ch of the style cases' program,
# which lists them for the clang-format case.
c_files() {
    sed -n '/^inhouse_ch() {$/,/^}$/p' "$REPO/tests/cases/host/style.sh" >"$WORK/inhouse.sh"
    grep -q '^}$' "$WORK/inhouse.sh" || {
        echo "hookcheck: tests/cases/host/style.sh has no function inhouse_ch to read" >&2
        return 1
    }
    # shellcheck source=/dev/null
    . "$WORK/inhouse.sh"
    (cd "$1" && inhouse_ch) | grep -Fx -f "$2"
    return 0
}

# local_format: print the command of the recorded clang-format on PATH.
local_format() {
    _major=$(printf '%s\n' "$JUDGED" | sed -n 's/.*version \([0-9][0-9]*\)\..*/\1/p')
    for _c in clang-format ${_major:+"clang-format-$_major"}; do
        command -v "$_c" >/dev/null 2>&1 || continue
        if [ "$("$_c" --version </dev/null 2>/dev/null | head -n 1)" = "$JUDGED" ]; then
            echo "$_c"
            return 0
        fi
    done
    return 1
}

# remote_format DIR: format DIR's C files on the host, as format_dir says.
# The script that ssh sends carries DIR as base64 in a here-document and
# prints the formatted tree between two marker lines. The host's sshd starts
# a Windows shell, so the command enters WSL as tools/dev/testing.sh does.
remote_format() {
    (cd "$1" && COPYFILE_DISABLE=1 tar -c --format ustar -f - .) >"$WORK/send.tar" || return 2
    {
        printf "want='%s'\n" "$JUDGED"
        cat <<'EOF'
set -u
major=$(printf '%s\n' "$want" | sed -n 's/.*version \([0-9][0-9]*\)\..*/\1/p')
fmt=
for c in clang-format "clang-format-$major"; do
    command -v "$c" >/dev/null 2>&1 || continue
    [ "$("$c" --version </dev/null 2>/dev/null | head -n 1)" = "$want" ] && fmt=$c && break
done
[ -n "$fmt" ] || { echo "hookcheck: no $want on $(hostname)" >&2; exit 1; }
d=$(mktemp -d) || exit 3
trap 'rm -rf "$d"' EXIT
base64 -d <<'HOOKCHECK-TAR-END' | tar -x -f - -C "$d" || exit 3
EOF
        base64 <"$WORK/send.tar" | fold -w 76
        cat <<'EOF'
HOOKCHECK-TAR-END
cd "$d" && find . \( -name '*.c' -o -name '*.h' \) -exec "$fmt" -i --style=file {} + || exit 3
echo hookcheck-begin
tar -c -f - . | base64
echo hookcheck-end
EOF
    } >"$WORK/remote.sh"
    ssh -o BatchMode=yes -o ConnectTimeout=10 "$HOST" 'MSYS_NO_PATHCONV=1 wsl.exe -- sh -s' \
        <"$WORK/remote.sh" >"$WORK/back.txt" 2>"$WORK/ssh.err"
    [ "$?" -ne 255 ] || return 1
    if ! tr -d '\r' <"$WORK/back.txt" | grep -qx hookcheck-end; then
        cat "$WORK/ssh.err" >&2
        grep -q '^hookcheck: no ' "$WORK/ssh.err" && return 1
        return 2
    fi
    tr -d '\r' <"$WORK/back.txt" | sed -n '/^hookcheck-begin$/,/^hookcheck-end$/p' |
        sed '1d;$d' | base64 -d | tar -x -f - -C "$1" || return 2
}

# format_dir DIR: format in place, with the recorded clang-format, every C
# file and header under DIR, which has the tree's .clang-format at its root.
# Returns 1 when no recorded clang-format was found, and 2 when one failed.
format_dir() {
    if _fmt=$(local_format); then
        (cd "$1" && find . \( -name '*.c' -o -name '*.h' \) -exec "$_fmt" -i --style=file {} +) ||
            return 2
        return 0
    fi
    [ -n "$HOST" ] || return 1
    remote_format "$1"
}

# copy_out FROM LIST: copy .clang-format and the files that LIST names from
# the directory FROM into $WORK/fmt, where format_dir formats them.
copy_out() {
    rm -rf "$WORK/fmt"
    mkdir "$WORK/fmt"
    { echo .clang-format; cat "$2"; } >"$WORK/fmtlist"
    (cd "$1" && COPYFILE_DISABLE=1 tar -c -f - -T "$WORK/fmtlist") | tar -x -f - -C "$WORK/fmt"
}

# formatted: run format_dir on $WORK/fmt and say why it did not format.
formatted() {
    format_dir "$WORK/fmt"
    case $? in
        0) return 0 ;;
        1) echo "hookcheck: warning: no $JUDGED here or on the host ${HOST:-(none)}; the files were not formatted" >&2 ;;
        *) echo "hookcheck: warning: clang-format failed; the files were not formatted" >&2 ;;
    esac
    return 1
}

case $MODE in
    commit)
        git diff --cached --name-only --diff-filter=d >"$WORK/files" || exit 2
        [ -s "$WORK/files" ] || exit 0
        git checkout-index -a --prefix="$WORK/tree/" || exit 2
        c_files "$WORK/tree" "$WORK/files" >"$WORK/cfiles" || exit 0
        [ -s "$WORK/cfiles" ] || exit 0
        copy_out "$WORK/tree" "$WORK/cfiles" || { echo 'hookcheck: warning: cannot copy the staged C files; they were not formatted' >&2; exit 0; }
        formatted || exit 0
        while IFS= read -r f; do
            cmp -s "$WORK/tree/$f" "$WORK/fmt/$f" && continue
            if ! git diff --quiet -- "$f"; then
                echo "hookcheck: warning: $f is not formatted and has unstaged changes; it was committed as it is" >&2
                continue
            fi
            if cat "$WORK/fmt/$f" >"$f" && git add -- "$f"; then
                echo "style: formatted $f" >&2
            else
                echo "hookcheck: warning: the formatted $f could not be staged" >&2
            fi
        done <"$WORK/cfiles"
        ;;
    format)
        printf '%s\n' "$@" >"$WORK/files"
        copy_out . "$WORK/files" || exit 2
        formatted || exit 1
        for f; do
            cmp -s "$f" "$WORK/fmt/$f" && continue
            cat "$WORK/fmt/$f" >"$f" && echo "style: formatted $f"
        done
        ;;
esac
exit 0
