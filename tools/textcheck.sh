#!/usr/bin/env sh
#
# textcheck.sh: mechanical text-style checks for the in-house files (see
# CLAUDE.md for the rules). It is called by tools/check.sh as a hard gate; a
# finding fails `make verify-style`.
#
# It checks only objective things that cannot misfire: non-ASCII bytes (which
# kills Unicode dashes, smart quotes, and the ellipsis character) across the
# in-house C, headers, Markdown, shell, make, and Python; the em-dash written as
# a spaced "--" in a C or H comment, in an echo or printf message or a make
# $(error/warning/info) directive, in a Python docstring or message, or in
# Markdown prose; Markdown bullets that use "-"; and a leftover "Phase N" or
# "Part N" diary tag in a C or H comment. Vocabulary is checked by
# tools/prose_check.py, not here, and the half of the register rule that needs
# a judgement is checked by a person in review. It uses grep, find, awk, sed,
# perl.
#
set -u

REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO" || exit 1

findings=$(mktemp)
trap 'rm -f "$findings"' EXIT INT TERM

# Find in-house files under the source roots. The vendored extracted trees and
# their built output (vendor/musl/install and the
# vendor/*/{musl,busybox,dropbear}-* trees) are pruned; the out-of-tree build/
# at the repo root is not a search root, so it is never descended into.
# ports/doom and the dev-log records are not roots, so they are skipped. The
# caller appends the name filter and -print.
prune_find() {
    find rach2 ish utils tests krffs tools mk vendor \
        -path 'vendor/musl/install' -prune -o \
        -path 'vendor/musl/musl-*' -prune -o \
        -path 'vendor/busybox/busybox-*' -prune -o \
        -path 'vendor/tcc/tcc-*' -prune -o \
        -path 'vendor/pdpmake/pdpmake-*' -prune -o \
        -path 'vendor/dropbear/dropbear-*' -prune -o \
        "$@" 2>/dev/null
}

md_files() {
    ls ./*.md 2>/dev/null
    prune_find -name '*.md' -print
}

# Shell, make, the two extensionless muslgcc wrappers, and the shell scripts
# this project ships inside its images, which end in .script rather than .sh.
script_files() {
    ls ./Makefile vendor/busybox/muslgcc vendor/dropbear/muslgcc 2>/dev/null
    prune_find '(' -name '*.sh' -o -name '*.mk' -o -name '*.script' \
                   -o -name 'Makefile' ')' -print
}

ch_files() {
    prune_find '(' -name '*.c' -o -name '*.h' ')' -print
}

# The Rach2-authored Python tools under tools/.
py_files() {
    prune_find -name '*.py' -print
}

{
    # Non-ASCII bytes in every in-house file: Markdown, shell and make, and the
    # C and headers (the convention check in tools/check.sh also covers the C/H,
    # so this is a second line of defense for a stray Unicode dash or smart
    # quote).
    { md_files; script_files; ch_files; py_files; } | sort -u | while IFS= read -r f; do
        [ -f "$f" ] || continue
        perl -ne 'print "$ARGV:$.: non-ASCII byte\n" if /[^\x09\x0a\x0d\x20-\x7e]/' "$f"
    done

    # A spaced "--" used as an em-dash in C and H comments. Double-quoted
    # strings are removed first so a usage string is not flagged; bare code
    # never has a spaced "--", so what remains is in a comment.
    ch_files | sort -u | while IFS= read -r f; do
        [ -f "$f" ] || continue
        sed 's/"[^"]*"//g' "$f" | grep -n ' -- ' | sed "s|^|$f:|; s|\$|  [prose em-dash]|"
    done

    # A spaced "--" used as an em-dash inside an echo or printf message, or
    # inside a make $(error/warning/info ...) directive, in shell and make
    # files. The echo and printf check is anchored to a command position, so the
    # end-of-options "set -- ..." and a "--flag" in bare code are not flagged;
    # the $(...) check matches the prose the directive prints, where a spaced
    # "--" is likewise an em-dash, not a flag.
    script_files | sort -u | while IFS= read -r f; do
        [ -f "$f" ] || continue
        grep -nE '^[[:space:]]*@?(echo|printf).*"[^"]* -- ' "$f" \
            | sed "s|^|$f:|; s|\$|  [prose em-dash in a message]|"
        grep -nE '\$\((error|warning|info)\b[^)]* -- ' "$f" \
            | sed "s|^|$f:|; s|\$|  [prose em-dash in a make directive]|"
    done

    # A spaced "--" used as an em-dash in the Python tools (a docstring, a
    # comment, or a stderr message). An argparse "--flag" has no surrounding
    # spaces, so it is not matched; only the spaced prose form is.
    py_files | sort -u | while IFS= read -r f; do
        [ -f "$f" ] || continue
        grep -nE ' -- ' "$f" \
            | sed "s|^|$f:|; s|\$|  [prose em-dash]|"
    done

    # A "Phase N" or "Part N" development-diary tag in a C or H comment. Strings
    # are stripped first, so a capitalized "Phase" or "Part" followed by a
    # number in what remains is a comment marker (the legitimate place to record
    # phases is the commit message and the dev-log, not the source).
    ch_files | sort -u | while IFS= read -r f; do
        [ -f "$f" ] || continue
        sed 's/"[^"]*"//g' "$f" | grep -nE '\b(Phase|Part) [0-9]' \
            | sed "s|^|$f:|; s|\$|  [Phase/Part diary tag]|"
    done

    # In Markdown, flag a spaced "--" and a "-" bullet, skipping fenced and
    # indented code so a command example is never mistaken for prose. Fences are
    # ``` or ~~~ with up to a few leading spaces.
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
