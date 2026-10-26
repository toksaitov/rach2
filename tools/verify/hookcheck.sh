#!/bin/sh
#
# hookcheck.sh: the style checks of the git hooks, the fast part of
# `tests/run style` run on the files that a commit or a push to master changes.
#
# `commit`, which tools/git-hooks/pre-commit runs, formats each staged
# in-house C file and header, the set the style case `clang-format` reads,
# with the clang-format version tools/setup/judges records, writes the result
# over the file and stages it. A file whose staged text needs formatting and
# that also has unstaged changes is not rewritten, and the commit is refused.
# It then runs the text rules of the style case `text` on the staged files:
# textcheck.sh (non-ASCII bytes, a dash written as a spaced double hyphen,
# `-` bullets in Markdown and its other rules) and prosecheck's `columns` and
# `banned`. A fault refuses the commit.
#
# `push`, which tools/git-hooks/pre-push runs with the lines git gives that
# hook on its input, checks each update of master: the files that the pushed
# commits change, as the last of them has them, must be formatted and must
# pass the same text rules. It changes no file.
#
# `format FILE ...` formats the files, relative to the root of the tree, in
# place with the recorded version; a refused push names this command.
#
# The checks read a copy of the whole tree as the index or the pushed commit
# has it, in a private directory under TMPDIR, so that they judge what is
# committed and not the working files, and prosecheck walks the copy as it
# walks the tree in `tests/run style`. Only findings in the changed files are
# reported, so that a fault already on master refuses no unrelated commit.
# The checkers are those of the tree this script is in, which make builds
# when they are absent or older than their sources.
#
# The recorded clang-format is `clang-format` or `clang-format-N` on PATH, N
# its major version, whose first line of `--version` is the record's. When
# neither is, the files go to the testing host in one ssh call, as a tar file
# with the tree's .clang-format, and come back formatted. The host is
# `testing`, or the one RACH2_STYLE_HOST names; an empty name means none.
# With no recorded clang-format anywhere, or a checker that cannot be built,
# the script warns and goes on, unless RACH2_STYLE_GATE is `strict`, as in the
# worker loops' sessions, when it refuses.
#
# Usage: hookcheck.sh commit | push | format FILE ...
#
set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
MODE=${1-}
[ "$#" -gt 0 ] && shift
case $MODE in
    commit|push) [ "$#" -eq 0 ] ;;
    format) [ "$#" -gt 0 ] ;;
    *) false ;;
esac || { echo 'hookcheck: usage: hookcheck.sh commit | push | format FILE ...' >&2; exit 2; }
TOP=$(git rev-parse --show-toplevel) && cd "$TOP" || exit 2
JUDGED=$(sed -n 's/^clang-format: //p' "$REPO/tools/setup/judges")
PROSECHECK=$REPO/build/tools/verify/prosecheck
HOST=${RACH2_STYLE_HOST-testing}

WORK=
trap 'rm -rf ${WORK:+"$WORK"}' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
WORK=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/rach2-hookcheck.XXXXXX") || exit 2
FAULTS=$WORK/faults
: >"$FAULTS"

# fault TEXT: record a fault, which the report prints and which refuses.
fault() {
    printf 'style: %s\n' "$1" >>"$FAULTS"
}

# lacking WHAT: refuse, under RACH2_STYLE_GATE=strict, because WHAT, or
# else warn that the files went unchecked because of it.
lacking() {
    if [ "${RACH2_STYLE_GATE-}" = strict ]; then
        fault "$1, and RACH2_STYLE_GATE is strict"
    else
        echo "style: warning: $1; the files were not checked for it" >&2
    fi
}

# c_files TREE LIST: print the in-house C files and headers of TREE that the
# file LIST names, by the function inhouse_ch of the style cases' program,
# which lists them for the clang-format case.
c_files() {
    sed -n '/^inhouse_ch() {$/,/^}$/p' "$REPO/tests/cases/host/style.sh" >"$WORK/inhouse.sh"
    grep -q '^}$' "$WORK/inhouse.sh" || {
        fault "tests/cases/host/style.sh has no function inhouse_ch to read"
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

# check_format TREE LIST: write to $WORK/unformatted the in-house C files of
# the file LIST that the recorded clang-format changes in TREE, and their
# formatted text under $WORK/fmt.
check_format() {
    : >"$WORK/unformatted"
    c_files "$1" "$2" >"$WORK/cfiles" || return 0
    [ -s "$WORK/cfiles" ] || return 0
    rm -rf "$WORK/fmt"
    mkdir "$WORK/fmt"
    { echo .clang-format; cat "$WORK/cfiles"; } >"$WORK/fmtlist"
    (cd "$1" && COPYFILE_DISABLE=1 tar -c -f - -T "$WORK/fmtlist") | tar -x -f - -C "$WORK/fmt" ||
        { fault "cannot copy the C files and .clang-format to format them"; return 0; }
    format_dir "$WORK/fmt"
    case $? in
        0) ;;
        1) lacking "no $JUDGED here or on the host ${HOST:-(none)}"; return 0 ;;
        *) fault "clang-format failed on $(tr '\n' ' ' <"$WORK/cfiles")"; return 0 ;;
    esac
    while IFS= read -r _f; do
        cmp -s "$1/$_f" "$WORK/fmt/$_f" || echo "$_f"
    done <"$WORK/cfiles" >"$WORK/unformatted"
}

# check_text TREE LIST: record a fault for each finding of the text rules in
# the files of TREE that the file LIST names.
check_text() {
    _tree=$1
    _list=$2
    if ! make -s -C "$REPO" build/tools/verify/prosecheck build/tools/verify/asciiscan \
        >"$WORK/make.log" 2>&1; then
        cat "$WORK/make.log" >&2
        lacking "the text checkers could not be built"
        return 0
    fi
    _ifs=$IFS
    IFS='
'
    # shellcheck disable=SC2046 # one path a line
    set -- $(cat "$_list")
    IFS=$_ifs
    sh "$REPO/tools/verify/textcheck.sh" -C "$_tree" "$@" </dev/null >"$WORK/text.out" 2>&1
    _status=$?
    grep -E '^[^ :][^:]*:[0-9]+:' "$WORK/text.out" |
        sed -E 's/^([^:]+:[0-9]+): *\[([^]]*)\]$/\1: \2/; s/^([^:]+:[0-9]+):(.*[^ ]) +\[([^]]*)\]$/\1: \3: \2/' \
            >"$WORK/text.found"
    while IFS= read -r _l; do
        fault "$_l"
    done <"$WORK/text.found"
    if [ "$_status" -ne 0 ] && [ ! -s "$WORK/text.found" ]; then
        fault "textcheck.sh did not finish: $(head -n 1 "$WORK/text.out")"
    fi
    (cd "$_tree" && "$PROSECHECK" columns) >"$WORK/cols.out" 2>&1
    [ "$?" -ne 2 ] || fault "prosecheck columns did not run: $(head -n 1 "$WORK/cols.out")"
    awk -F: 'NR == FNR { want[$0] = 1; next } ($1 in want) && $2 ~ /^[0-9]+$/' \
        "$_list" "$WORK/cols.out" >"$WORK/cols.found"
    while IFS= read -r _l; do
        fault "$_l"
    done <"$WORK/cols.found"
    (cd "$_tree" && "$PROSECHECK" banned) >"$WORK/banned.out" 2>&1
    [ "$?" -ne 2 ] || fault "prosecheck banned did not run: $(head -n 1 "$WORK/banned.out")"
    awk -F': ' 'NR == FNR { want[$0] = 1; next } ($1 in want) && / use\(s\) of "/' \
        "$_list" "$WORK/banned.out" >"$WORK/banned.found"
    while IFS= read -r _l; do
        _f=${_l%%: *}
        _term=$(printf '%s\n' "$_l" | sed 's/.* use(s) of "\(.*\)"$/\1/')
        _lines=$(grep -n -i -F -- "$_term" "$_tree/$_f" | cut -d: -f1)
        if [ -z "$_lines" ]; then
            fault "$_l"
        fi
        for _n in $_lines; do
            fault "$_f:$_n: the banned term \"$_term\""
        done
    done <"$WORK/banned.found"
}

# report WHAT HOWTO: print the faults and HOWTO and exit 1, when there are
# faults; the refused WHAT is a commit or a push.
report() {
    [ -s "$FAULTS" ] || return 0
    {
        cat "$FAULTS"
        echo "style: the $1 is refused; $2"
    } >&2
    exit 1
}

case $MODE in
    commit)
        git diff --cached --name-only --diff-filter=d >"$WORK/files" || exit 2
        [ -s "$WORK/files" ] || exit 0
        git checkout-index -a --prefix="$WORK/tree/" || exit 2
        check_format "$WORK/tree" "$WORK/files"
        while IFS= read -r f; do
            if ! git diff --quiet -- "$f"; then
                fault "$f: not formatted, and has unstaged changes that formatting would overwrite"
                continue
            fi
            if cat "$WORK/fmt/$f" >"$f" && git add -- "$f"; then
                cp "$WORK/fmt/$f" "$WORK/tree/$f"
                echo "style: formatted $f" >&2
            else
                fault "$f: the formatted file could not be staged"
            fi
        done <"$WORK/unformatted"
        check_text "$WORK/tree" "$WORK/files"
        report commit "fix the lines above, add the files with git add, and commit again"
        ;;
    push)
        cat >"$WORK/updates"
        while read -r _lref lsha rref rsha <&3; do
            [ "$rref" = refs/heads/master ] || continue
            case $lsha in *[!0]*) ;; *) continue ;; esac
            case $rsha in
                *[!0]*)
                    git cat-file -e "$rsha^{commit}" 2>/dev/null || continue
                    base=$rsha ;;
                *) base=$(git hash-object -t tree /dev/null) ;;
            esac
            git diff --name-only --diff-filter=d "$base" "$lsha" >"$WORK/files" || exit 2
            [ -s "$WORK/files" ] || continue
            rm -rf "$WORK/tree" "$WORK/index"
            GIT_INDEX_FILE=$WORK/index git read-tree "$lsha" &&
                GIT_INDEX_FILE=$WORK/index git checkout-index -a --prefix="$WORK/tree/" || exit 2
            check_format "$WORK/tree" "$WORK/files"
            while IFS= read -r f; do
                n=$(cmp "$WORK/tree/$f" "$WORK/fmt/$f" 2>/dev/null | sed -n 's/.* line \([0-9]*\).*/\1/p')
                fault "$f:${n:-1}: not as $JUDGED formats it"
            done <"$WORK/unformatted"
            check_text "$WORK/tree" "$WORK/files"
            if [ -s "$WORK/unformatted" ]; then
                fix="run sh tools/verify/hookcheck.sh format $(tr '\n' ' ' <"$WORK/unformatted")and "
            fi
        done 3<"$WORK/updates"
        report push "${fix-}fix the lines above in a new commit, which the pre-commit hook checks, and push again"
        ;;
    format)
        printf '%s\n' "$@" >"$WORK/files"
        mkdir "$WORK/fmt"
        { echo .clang-format; cat "$WORK/files"; } >"$WORK/fmtlist"
        COPYFILE_DISABLE=1 tar -c -f - -T "$WORK/fmtlist" | tar -x -f - -C "$WORK/fmt" || exit 2
        format_dir "$WORK/fmt"
        case $? in
            0) ;;
            1) echo "hookcheck: no $JUDGED here or on the host ${HOST:-(none)}" >&2; exit 1 ;;
            *) echo "hookcheck: clang-format failed" >&2; exit 1 ;;
        esac
        for f; do
            cmp -s "$f" "$WORK/fmt/$f" && continue
            cat "$WORK/fmt/$f" >"$f" && echo "style: formatted $f"
        done
        ;;
esac
exit 0
