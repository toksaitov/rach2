#!/bin/sh
#
# hookcheck.sh: the checks of the git hook pre-commit, and a command that
# formats files by hand.
#
# `commit`, which tools/git-hooks/pre-commit runs, formats each staged
# in-house C file and header, the set the style case `clang-format` reads,
# with the clang-format version tools/setup/judges records, writes the result
# over the file and stages it. A file whose staged text needs formatting and
# that also has unstaged changes is left as it is, with a warning, since
# formatting it would overwrite those changes. When the formatting cannot be
# set up, as on a full disk, the commit goes on unformatted with a warning.
#
# It then judges the staged C files, as they will be committed, by the size
# limits of tools/verify/sizecheck.c, against the staged
# tools/verify/sizes.reasons, and refuses the commit, with status 1, when a
# function is past a limit or a reasons entry of those files is stale. The
# checker is compiled from this tree's source with `cc` on each commit. The
# complexity is judged when a pmccabe is on PATH or under
# build/host-tools/native, where `make host-pmccabe` builds it, and is left
# to `tests/run style` otherwise, with a warning that says so. Without `cc`
# the sizes are not judged, with a warning.
#
# Every commit, whatever it stages, is then judged by the `columns` and
# `banned` checks of tools/verify/prosecheck.c, compiled the same way. They
# read the whole staged tree, as the style cases do, so that vendored and
# skipped files count as they do there, and a finding in a staged file
# refuses the commit. A finding in a file the commit does not touch is left
# for its author. tools/verify/quickcheck.sh then runs the rest of the style
# checks that take seconds, and a rough reading of the added lines. Every
# check runs, so that one refusal lists all there is to fix.
#
# `push`, which tools/git-hooks/pre-push runs, judges what a push gives a
# milestone branch in the same way, since a rebase runs no commit hook: the
# files that differ are refused if unformatted, and quickcheck.sh also links
# the host tests and runs cppcheck and Clang's warnings. A push to any other
# branch passes, so that unfinished work can always be saved. `range BASE TIP`
# judges the same by hand, as before a release's preparation.
#
# `format FILE ...` formats the files, relative to the root of the tree, in
# place with the recorded version.
#
# The checks read the staged files, with the tree's .clang-format, from a
# private directory under TMPDIR, or the tree's build/tmp when TMPDIR is
# unset, so that they judge what is committed and not the working files.
#
# The recorded clang-format is `clang-format` or `clang-format-N` on PATH, N
# its major version, whose first line of `--version` is the record's. When
# neither is, the files go to the testing host in one ssh call, as a tar file
# with the tree's .clang-format, and come back formatted. The host is
# `testing`, or the one RACH2_STYLE_HOST names; an empty name means none.
# With no recorded clang-format anywhere, the script warns and goes on.
#
# Usage: hookcheck.sh commit | push | range BASE TIP | format FILE ...
#
set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
MODE=${1-}
[ "$#" -gt 0 ] && shift
# stage_formatted: write each file of $WORK/cfiles that $WORK/fmt holds
# formatted over the working file, stage it, and copy it into $WORK/tree, so
# that the tree there is what the commit will hold.
stage_formatted() {
    while IFS= read -r f; do
        cmp -s "$WORK/tree/$f" "$WORK/fmt/$f" && continue
        if ! git diff --quiet -- "$f"; then
            echo "hookcheck: warning: $f is not formatted and has unstaged changes; it was committed as it is" >&2
            continue
        fi
        if cat "$WORK/fmt/$f" >"$f" && git add -- "$f"; then
            cat "$WORK/fmt/$f" >"$WORK/tree/$f"
            echo "style: formatted $f" >&2
        else
            echo "hookcheck: warning: the formatted $f could not be staged" >&2
        fi
    done <"$WORK/cfiles"
}

# local_pmccabe: print the command of the recorded pmccabe, on PATH or under
# the build/host-tools/native of this tree or of the clone it is a worktree
# of, or else of any pmccabe on PATH, which counts the same for this rough use.
local_pmccabe() {
    _main=$(cd "$(git rev-parse --git-common-dir)/.." && pwd)
    for _c in pmccabe "$TOP/build/host-tools/native/bin/pmccabe" \
        "$_main/build/host-tools/native/bin/pmccabe"; do
        command -v "$_c" >/dev/null 2>&1 || continue
        if [ "$("$_c" -V </dev/null 2>&1 | head -n 1)" = "$PMCCABE_JUDGED" ]; then
            echo "$_c"
            return 0
        fi
    done
    command -v pmccabe
}

# sizes: judge the staged C files in $WORK/tree by sizecheck, against the
# entries of the staged reasons file for those files alone, since an entry
# of a file not read would be called stale. Sets REFUSED on a finding.
sizes() {
    grep '\.c$' "$WORK/cfiles" >"$WORK/sizefiles" || return 0
    if ! command -v cc >/dev/null 2>&1; then
        echo 'hookcheck: warning: no cc here to build tools/verify/sizecheck.c; the sizes were not judged' >&2
        return 0
    fi
    if ! cc -o "$WORK/sizecheck" "$REPO/tools/verify/sizecheck.c" >"$WORK/cc.err" 2>&1; then
        cat "$WORK/cc.err" >&2
        echo 'hookcheck: warning: tools/verify/sizecheck.c did not compile; the sizes were not judged' >&2
        return 0
    fi
    _reasons=$WORK/tree/tools/verify/sizes.reasons
    [ -f "$_reasons" ] || _reasons=$REPO/tools/verify/sizes.reasons
    awk -F: 'NR == FNR { want[$0] = 1; next } $1 in want' "$WORK/sizefiles" "$_reasons" \
        >"$WORK/reasons"
    set --
    if _pm=$(local_pmccabe); then
        # shellcheck disable=SC2046 # one path a line, none with a space
        (cd "$WORK/tree" && "$_pm" $(cat "$WORK/sizefiles")) >"$WORK/pmccabe.txt" 2>/dev/null
        set -- --pmccabe "$WORK/pmccabe.txt"
    else
        echo "hookcheck: warning: complexity not judged; pmccabe ${PMCCABE_JUDGED##* } is not on PATH or under build/host-tools/native; run \`make host-pmccabe\` to build it there" >&2
    fi
    # shellcheck disable=SC2046 # one path a line, none with a space
    (cd "$WORK/tree" && "$WORK/sizecheck" "$@" "$WORK/reasons" $(cat "$WORK/sizefiles")) >"$WORK/sizes.out" 2>&1
    case $? in
        0) return 0 ;;
        1)
            grep -v ' finding(s)$' "$WORK/sizes.out" >&2
            echo 'hookcheck: refused: the staged C files break the size limits of tools/verify/sizecheck.c; split the function, or correct its entry in tools/verify/sizes.reasons' >&2
            REFUSED=1
            ;;
        *)
            cat "$WORK/sizes.out" >&2
            echo 'hookcheck: warning: sizecheck could not read the staged files; the sizes were not judged' >&2
            ;;
    esac
}

# prose: judge $WORK/tree by prosecheck's `columns` and `banned`, and keep the
# findings of the staged files, each of which begins with its path and a
# colon. Sets REFUSED on one.
prose() {
    if ! command -v cc >/dev/null 2>&1; then
        echo 'hookcheck: warning: no cc here to build tools/verify/prosecheck.c; the prose was not judged' >&2
        return 0
    fi
    if ! cc -o "$WORK/prosecheck" "$REPO/tools/verify/prosecheck.c" >"$WORK/cc.err" 2>&1; then
        cat "$WORK/cc.err" >&2
        echo 'hookcheck: warning: tools/verify/prosecheck.c did not compile; the prose was not judged' >&2
        return 0
    fi
    (cd "$WORK/tree" && "$WORK/prosecheck" columns; "$WORK/prosecheck" banned) >"$WORK/prose.out" 2>&1
    awk -F: 'NR == FNR { want[$0] = 1; next } $1 in want' "$WORK/files" "$WORK/prose.out" >"$WORK/prose.bad"
    [ -s "$WORK/prose.bad" ] || return 0
    sort -u "$WORK/prose.bad" >&2
    echo 'hookcheck: refused: the staged files break the comment width or the terms of tools/verify/prosecheck.c' >&2
    REFUSED=1
}

# quick: run tools/verify/quickcheck.sh, with $1 its option if any, over the
# change that $WORK/files and $WORK/diff name. Sets REFUSED on a finding.
quick() {
    TMPDIR=$WORK sh "$REPO/tools/verify/quickcheck.sh" ${1:+"$1"} "$WORK/tree" "$WORK/files" \
        "$WORK/cfiles" "$WORK/diff"
    case $? in
        0) ;;
        1)
            echo 'hookcheck: refused: the change breaks the style checks listed above' >&2
            REFUSED=1
            ;;
        *) echo 'hookcheck: warning: tools/verify/quickcheck.sh could not run' >&2 ;;
    esac
}

case $MODE in
    commit | push) [ "$#" -eq 0 ] ;;
    range) [ "$#" -eq 2 ] ;;
    format) [ "$#" -gt 0 ] ;;
    *) false ;;
esac || { echo 'hookcheck: usage: hookcheck.sh commit | push | range BASE TIP | format FILE ...' >&2; exit 2; }
REFUSED=0

# unready TEXT: say that the formatting cannot be set up, for TEXT; a commit
# goes on unformatted, a push goes on, and `format` fails.
unready() {
    echo "hookcheck: warning: $1; the files were not formatted" >&2
    [ "$MODE" = format ] && exit 2
    exit 0
}

TOP=$(git rev-parse --show-toplevel) || unready 'no work tree'
cd "$TOP" || unready "cannot enter $TOP"
JUDGED=$(sed -n 's/^clang-format: //p' "$REPO/tools/setup/judges")
PMCCABE_JUDGED=$(sed -n 's/^pmccabe: //p' "$REPO/tools/setup/judges")
HOST=${RACH2_STYLE_HOST-testing}

WORK=
trap 'rm -rf ${WORK:+"$WORK"}' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
SCRATCH=${TMPDIR:-$TOP/build/tmp}
WORK=$(mkdir -p "$SCRATCH" && umask 077 && mktemp -d "$SCRATCH/rach2-hookcheck.XXXXXX") ||
    unready "no scratch directory in $SCRATCH"

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
d=$(mktemp -d /tmp/rach2-hookcheck.XXXXXX) || exit 3
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

# list_c: list the in-house C files of $WORK/files in $WORK/cfiles.
list_c() {
    : >"$WORK/cfiles"
    grep -q '\.[ch]$' "$WORK/files" || return 0
    c_files "$WORK/tree" "$WORK/files" >"$WORK/cfiles"
}

# unformatted: refuse the files of $WORK/cfiles that $WORK/fmt holds formatted
# otherwise, in a change already committed.
unformatted() {
    while IFS= read -r f; do
        cmp -s "$WORK/tree/$f" "$WORK/fmt/$f" && continue
        echo "$f: not formatted; run sh tools/verify/hookcheck.sh format $f" >&2
        REFUSED=1
    done <"$WORK/cfiles"
}

# judge BASE TIP: judge what TIP changes from BASE, in a copy of TIP.
judge() {
    _index=$WORK/index
    rm -rf "$WORK/tree" "$_index"
    if ! GIT_INDEX_FILE=$_index git read-tree "$2" ||
        ! GIT_INDEX_FILE=$_index git checkout-index -a --prefix="$WORK/tree/"; then
        unready "$2 cannot be copied"
    fi
    git diff --name-only --diff-filter=d "$1" "$2" >"$WORK/files" ||
        unready 'the changed files cannot be listed'
    [ -s "$WORK/files" ] || return 0
    git diff -U0 -M --no-color --no-ext-diff --no-prefix "$1" "$2" >"$WORK/diff"
    list_c
    if [ -s "$WORK/cfiles" ]; then
        if ! copy_out "$WORK/tree" "$WORK/cfiles"; then
            echo 'hookcheck: warning: cannot copy the C files; their format was not judged' >&2
        elif formatted; then
            unformatted
        fi
        sizes
    fi
    prose
    quick -p
}

# base_of REMOTE TIP: print where the milestone branch stands, or where TIP
# left master when the branch is new or its head is not here.
base_of() {
    case $1 in
        *[!0]*)
            if git cat-file -e "$1^{commit}" 2>/dev/null; then
                echo "$1"
                return 0
            fi
            ;;
    esac
    git merge-base origin/master "$2"
}

case $MODE in
    commit)
        git diff --cached --name-only --diff-filter=d >"$WORK/files" ||
            unready 'the staged files cannot be listed'
        [ -s "$WORK/files" ] || exit 0
        git checkout-index -a --prefix="$WORK/tree/" || unready 'the index cannot be copied'
        list_c
        if [ -s "$WORK/cfiles" ]; then
            if ! copy_out "$WORK/tree" "$WORK/cfiles"; then
                echo 'hookcheck: warning: cannot copy the staged C files; they were not formatted' >&2
            elif formatted; then
                stage_formatted
            fi
            sizes
        fi
        prose
        git diff --cached -U0 -M --no-color --no-ext-diff --no-prefix >"$WORK/diff"
        quick
        ;;
    push)
        while read -r _ _tip _ref _remote; do
            case $_ref in
                refs/heads/milestone/*) ;;
                *) continue ;;
            esac
            case $_tip in
                *[!0]*) ;;
                *) continue ;;
            esac
            _base=$(base_of "$_remote" "$_tip") || unready "no base for $_ref"
            judge "$_base" "$_tip"
        done
        ;;
    range)
        judge "$1" "$2"
        ;;
    format)
        printf '%s\n' "$@" >"$WORK/files"
        copy_out . "$WORK/files" || unready 'the files cannot be copied'
        formatted || exit 1
        for f; do
            cmp -s "$f" "$WORK/fmt/$f" && continue
            cat "$WORK/fmt/$f" >"$f" && echo "style: formatted $f"
        done
        ;;
esac
exit "$REFUSED"
