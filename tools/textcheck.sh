#!/usr/bin/env sh
#
# textcheck.sh: mechanical text-style checks for the in-house files (see
# text-standard-plan.md, and CLAUDE.md for the rules). It is called by tools/check.sh,
# advisory at first and a hard gate from a later phase.
#
# It checks only objective things that cannot misfire: non-ASCII bytes (which kills
# Unicode dashes, smart quotes, and the ellipsis character), the em-dash written as a
# spaced "--" in prose, and Markdown bullets that use "-". Voice and vocabulary are
# judged in review, not here. It uses grep, find, awk, sed, and perl; rg is gone.
#
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

findings=$(mktemp)
trap 'rm -f "$findings"' EXIT INT TERM

# Find in-house files under the source roots. The vendored extracted trees and their
# built output (libc/install, libc/musl-*, busybox/busybox-*, dropbear/dropbear-*) and
# the out-of-tree .build/ are pruned. ports/doom and the dev-log records are not roots,
# so they are skipped. The caller appends the name filter and -print.
prune_find() {
    find rach2 ish utils tests krffs tools mk libc busybox dropbear \
        -path '*/.build' -prune -o \
        -path 'libc/install' -prune -o \
        -path 'libc/musl-*' -prune -o \
        -path 'busybox/busybox-*' -prune -o \
        -path 'dropbear/dropbear-*' -prune -o \
        "$@" 2>/dev/null
}

md_files() {
    ls ./*.md 2>/dev/null
    prune_find -name '*.md' -print
}

# Shell, make, and the two extensionless muslgcc wrappers.
script_files() {
    ls ./Makefile busybox/muslgcc dropbear/muslgcc 2>/dev/null
    prune_find '(' -name '*.sh' -o -name '*.mk' -o -name 'Makefile' ')' -print
}

ch_files() {
    prune_find '(' -name '*.c' -o -name '*.h' ')' -print
}

{
    # Non-ASCII bytes in Markdown, shell, and make files. The kernel C and H are
    # already checked by tools/check.sh.
    { md_files; script_files; } | sort -u | while IFS= read -r f; do
        [ -f "$f" ] || continue
        perl -ne 'print "$ARGV:$.: non-ASCII byte\n" if /[^\x09\x0a\x0d\x20-\x7e]/' "$f"
    done

    # A spaced "--" used as an em-dash in C and H comments. Double-quoted strings are
    # removed first so a usage string is not flagged; bare code never holds a spaced
    # "--", so what remains is in a comment.
    ch_files | sort -u | while IFS= read -r f; do
        [ -f "$f" ] || continue
        sed 's/"[^"]*"//g' "$f" | grep -n ' -- ' | sed "s|^|$f:|; s|\$|  [prose em-dash]|"
    done

    # In Markdown, flag a spaced "--" and a "-" bullet, skipping fenced and indented
    # code so a command example is never mistaken for prose. Fences are ``` or ~~~ with
    # up to a few leading spaces.
    md_files | sort -u | while IFS= read -r f; do
        [ -f "$f" ] || continue
        awk -v file="$f" '
            /^[ \t]*```/ { fence = !fence; next }
            /^[ \t]*~~~/ { fence = !fence; next }
            fence        { next }
            /^    /      { next }
            index($0, " -- ") { print file ":" NR ":  [prose em-dash]" }
            /^[ \t]*- /  { print file ":" NR ":  [use * for the bullet]" }
        ' "$f"
    done
} >>"$findings"

n=$(wc -l <"$findings" | tr -d ' ')
if [ "$n" -eq 0 ]; then
    echo "textcheck: clean"
    exit 0
fi
echo "textcheck: $n finding(s)" >&2
sort "$findings" >&2
exit 1
