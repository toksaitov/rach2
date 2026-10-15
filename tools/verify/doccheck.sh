#!/usr/bin/env sh

#
# doccheck.sh: the check of a change to documents and comments alone, which
# AGENTS.md gives such a change in place of the build and the suites.
#
# The change is every tracked file that differs from BASE, origin/master
# unless given, whether committed or not; a file git does not track is not
# pushed, and is not part of it. It is a document change when each of its
# files is a Markdown file; a manual page under man/; a C file or header
# whose tokens, once its comments are removed, are those of BASE; a shell
# script (`*.sh`, `tests/run` or a program under `krffs/` named `*.script`)
# whose interpreter line and commands are those of BASE; or a makefile
# or manifest whose changed lines are all comments or blank lines that its
# reader skips. The script names any other file and exits 2, and the change
# is then checked as code.
#
# The tokens are those of `-fpreprocessed -E`, which removes the comments and
# expands nothing, with the white space between tokens made one line break
# and the inside of every string and character literal kept as it is. The
# lines a backslash continues are joined first, and the tokens of each
# preprocessing directive are set between two empty lines, since a line
# break ends a directive: a line joined to a directive or split from one
# changes the list. A C file the preprocessor fails on, at BASE or in the
# work tree, is not a document change. The commands of a shell script are
# its text as bash prints it back from a function, which drops the comments
# and keeps here-documents and quoted text whole. In a makefile, a comment or
# blank line is skipped only outside a define and when neither it nor the
# line before ends in a backslash. A deleted file and a new one are compared
# with an empty file. A file whose mode or type changes, such as a script
# that loses its execute bit, is not a document change, nor is a new file
# that is not a plain one of mode 644.
#
# The checks are the steps of `tests/run style` that read documents and
# comments: textcheck.sh, prosecheck's comment width, terms and dev-log
# index, linkcheck.sh, mancheck, the three generated lists (suites.md,
# programs.md and syscalls.md), and clang-format over the changed C files
# under the version tools/setup/judges records. They take seconds in a tree
# that has been built. mancheck and programs.md read BusyBox's applet table,
# so a tree without BusyBox's build is refused rather than made to build it:
# on the testing host every clone and copy has one, and the owner's computer
# reaches its copy there with `tools/dev/testing.sh run`. Make takes the
# build that is there as current (`-o`), so a comment changed in one of its
# prerequisites, such as mk/arch.sh, does not rebuild musl and BusyBox.
#
# Usage: doccheck.sh [BASE]
# Exit: 0 when every check passes; 1 when one fails; 2 when the change is not
# a document change, or a tool is missing.
#

set -u

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$REPO" || exit 2
BASE=${1:-origin/master}
TOKCC=${TOKCC:-i686-elf-gcc}
unset ARCH

git rev-parse -q --verify "$BASE^{commit}" >/dev/null || {
    echo "doccheck: no commit $BASE" >&2; exit 2
}
command -v "$TOKCC" >/dev/null 2>&1 || {
    echo "doccheck: $TOKCC not found; TOKCC names the compiler that removes comments" >&2; exit 2
}
command -v bash >/dev/null 2>&1 || {
    echo "doccheck: bash not found; it reads the shell scripts" >&2; exit 2
}

work=$(mktemp -d "${TMPDIR:-/tmp}/doccheck.XXXXXX") || exit 2
trap 'rm -rf "$work"' 0

# The tokens of the C text on stdin, one a line, literals kept whole and
# each directive set between empty lines; the status is 1, with the
# compiler's errors in $work/pperr, when the text does not preprocess.
tokens() {
    "$TOKCC" -fpreprocessed -dD -E -P -x c - > "$work/pp" 2> "$work/pperr" ||
        return 1
    awk '
    {
        line = $0
        while (line ~ /\\$/ && (getline more) > 0)
            line = substr(line, 1, length(line) - 1) more
        directive = line ~ /^[ \t]*#/
        if (directive) print ""
        out = ""; q = ""; sep = 0
        for (i = 1; i <= length(line); i++) {
            c = substr(line, i, 1)
            if (q != "") {
                out = out c
                if (c == "\\") { i++; out = out substr(line, i, 1) }
                else if (c == q) q = ""
            } else if (c == " " || c == "\t") {
                sep = 1
            } else {
                if (sep && out != "") out = out "\n"
                sep = 0
                if (c == "\"" || c == "\047") q = c
                out = out c
            }
        }
        if (out != "") print out
        if (directive) print ""
    }' "$work/pp"
}

# Report that FILE, at the place WHERE, does not preprocess.
unparsed() {
    echo "doccheck: $1: the preprocessor fails on it $2" >&2
    sed "s|^<stdin>:|$1:|; s/^/    /" "$work/pperr" | head -5 >&2
}

# The interpreter line of the shell script FILE, then its commands as bash
# prints them back from a function. The script must pass `bash -n` before it
# becomes the body of that function, so that the eval defines it and runs
# nothing. Fails when bash cannot parse the script.
commands() {
    sed -n '1{/^#!/p;}' "$1"
    BASH_ENV='' bash -c 'bash -n "$1" 2>/dev/null || exit 1
eval "doccheck_script() {
:
$(cat "$1")
}" 2>/dev/null && declare -f doccheck_script' sh "$1"
}

# The numbers of the lines of the makefile on stdin that make skips: blank
# and comment lines outside a define, where neither the line nor the one
# before it ends in a backslash. A comment ending in one goes on to the next
# line, and a recipe line ending in one goes on to a comment after it.
skipped() {
    awk '
    {
        bs = match($0, /\\+$/) ? RLENGTH % 2 : 0
        if (!cont && !def && !bs && $0 ~ /^[ \t]*(#.*)?$/) print NR
        if (!cont && $0 ~ /^[ \t]*((override|export)[ \t]+)*define([ \t]|$)/) def++
        else if (!cont && def && $0 ~ /^[ \t]*endef([ \t]|$)/) def--
        cont = bs
    }'
}

# Whether every line the diff of FILE removes is numbered in $work/before and
# every line it adds in $work/after.
only_skipped() {
    git diff -U0 "$BASE" -- "$1" | awk -v before="$work/before" -v after="$work/after" '
    BEGIN {
        while ((getline n < before) > 0) old[n] = 1
        while ((getline n < after) > 0) new[n] = 1
    }
    /^@@ / {
        split(substr($2, 2), a, ","); split(substr($3, 2), b, ",")
        for (i = a[1]; i < a[1] + ((2 in a) ? a[2] : 1); i++) if (!(i in old)) bad = 1
        for (i = b[1]; i < b[1] + ((2 in b) ? b[2] : 1); i++) if (!(i in new)) bad = 1
    }
    END { exit bad }'
}

# The text of FILE at BASE, or nothing when BASE has no such file.
at_base() {
    git cat-file -e "$BASE:$1" 2>/dev/null && git show "$BASE:$1"
}

git diff --raw --no-renames "$BASE" -- > "$work/files" || exit 2
if [ ! -s "$work/files" ]; then
    echo "doccheck: no file differs from $BASE"; exit 0
fi

code=0
cfiles=""
while read -r omode nmode _ _ status f; do
    omode=${omode#:}
    if [ "$status" = A ] && [ "$nmode" != 100644 ]; then
        echo "doccheck: $f: a new file of mode $nmode, not a plain file" >&2
        code=1
        continue
    elif [ "$status" != A ] && [ "$status" != D ] && [ "$omode" != "$nmode" ]; then
        echo "doccheck: $f: its mode changes from $omode to $nmode" >&2
        code=1
        continue
    fi
    case "$f" in
    *.md|man/man[1-9]/*)
        ;;
    *.c|*.h)
        : > "$work/after"
        if ! at_base "$f" | tokens > "$work/before"; then
            unparsed "$f" "at $BASE"
            code=1
        elif [ -f "$f" ] && ! tokens < "$f" > "$work/after"; then
            unparsed "$f" "in the work tree"
            code=1
        elif ! cmp -s "$work/before" "$work/after"; then
            echo "doccheck: $f: its code differs from $BASE, not only its comments" >&2
            code=1
        fi
        [ -f "$f" ] && cfiles="$cfiles $f"
        ;;
    *.sh|tests/run|krffs/*.script)
        at_base "$f" > "$work/old"
        if [ -f "$f" ]; then cat "$f"; fi > "$work/now"
        if [ "$status" = A ]; then
            echo "doccheck: $f: a new file" >&2
            code=1
        elif ! commands "$work/old" > "$work/before" ||
            ! commands "$work/now" > "$work/after"; then
            echo "doccheck: $f: bash cannot parse it, here or at $BASE" >&2
            code=1
        elif ! cmp -s "$work/before" "$work/after"; then
            echo "doccheck: $f: its commands differ from $BASE, not only its comments" >&2
            code=1
        fi
        ;;
    */Makefile|Makefile|*.mk)
        if [ "$status" = A ]; then
            echo "doccheck: $f: a new file" >&2
            code=1
        else
            at_base "$f" | skipped > "$work/before"
            if [ -f "$f" ]; then skipped < "$f"; fi > "$work/after"
            if ! only_skipped "$f"; then
                echo "doccheck: $f: a changed line is one make reads" >&2
                code=1
            fi
        fi
        ;;
    *.manifest)
        if [ "$status" = A ]; then
            echo "doccheck: $f: a new file" >&2
            code=1
        elif git diff -U0 "$BASE" -- "$f" | grep -E '^[-+]' | grep -v -E '^(\+\+\+|---) ' |
            cut -c2- | grep -v -E '^[[:space:]]*(#.*)?$' | grep -q .; then
            echo "doccheck: $f: a changed line is not a comment" >&2
            code=1
        fi
        ;;
    *)
        echo "doccheck: $f: not a document, a manual page or a commented source" >&2
        code=1
        ;;
    esac
done < "$work/files"
if [ "$code" -ne 0 ]; then
    echo "doccheck: result: not a document change; check it as code, as AGENTS.md says" >&2
    exit 2
fi

stamp=vendor/busybox/.stamp
if [ ! -f "$stamp" ]; then
    echo "doccheck: this tree has no BusyBox build, whose applets the manual's check reads; run doccheck on the testing host" >&2
    exit 2
fi
make -s build/tools/verify/prosecheck >/dev/null || exit 2
fail=0
run() {
    what=$1; shift
    if "$@" > "$work/log" 2>&1; then
        echo "doccheck: pass: $what"
    else
        echo "doccheck: fail: $what" >&2
        sed 's/^/    /' "$work/log" | head -20 >&2
        fail=1
    fi
}
same() {
    cmp -s "$1" "$2" || { diff "$2" "$1" | head -10; return 1; }
}

run "text style" sh tools/verify/textcheck.sh
run "comment width" build/tools/verify/prosecheck columns
run "terms" build/tools/verify/prosecheck banned
run "dev-log index" build/tools/verify/prosecheck devlog
run "links" sh tools/verify/linkcheck.sh
# shellcheck disable=SC2046 # one path a word
run "dev-log checklists' links" sh tools/verify/linkcheck.sh $(find dev-log -name checklist.md | sort)
run "manual" make -s -o "$stamp" mancheck
sh tests/run inventory > "$work/suites.md" 2>/dev/null
run "tests/system/suites.md is tests/run inventory" same "$work/suites.md" tests/system/suites.md
make -s -o "$stamp" programs-md PROGRAMS_MD="$work/programs.md" >/dev/null 2>&1
run "man/programs.md is make programs-md" same "$work/programs.md" man/programs.md
make -s syscalls SYSCALLS_MD="$work/syscalls.md" >/dev/null 2>&1
run "rach2/syscalls.md is make syscalls" same "$work/syscalls.md" rach2/syscalls.md

recorded=$(sed -n 's/^clang-format: //p' tools/setup/judges)
found=$(clang-format --version </dev/null 2>/dev/null | head -1)
if [ -z "$cfiles" ]; then
    :
elif [ "$found" = "$recorded" ]; then
    # shellcheck disable=SC2086 # one path a word
    run "clang-format" clang-format --style=file --dry-run --Werror $cfiles
else
    echo "doccheck: skip: clang-format is $found here, and the testing host's $recorded judges"
fi

if [ "$fail" -ne 0 ]; then
    echo "doccheck: result: fail" >&2
    exit 1
fi
echo "doccheck: result: pass: $(wc -l < "$work/files" | tr -d ' ') file(s), documents and comments only"
