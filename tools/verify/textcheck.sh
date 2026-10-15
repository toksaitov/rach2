#!/usr/bin/env sh
#
# textcheck.sh: the text-style checks of the in-house files that a pattern
# can decide, from the Writing and Documentation sections of AGENTS.md. Run
# by tools/verify/check.sh; a finding fails `tests/run style`.
#
# The checks find: a non-ASCII byte (a Unicode dash, a smart quote, the
# ellipsis character) in the in-house C, headers, assembly, Markdown, shell,
# make, Python and configuration files; an em-dash written as a spaced "--"
# in a C, header, assembly, linker-script or awk comment, in an echo or
# printf message or a make $(error/warning/info) directive, in a Python
# docstring or message, on any line of a configuration file, or in Markdown
# prose; a Markdown bullet written as "-"; a "Phase N" or "Part N" diary tag
# in a code comment or a configuration file; and, in the component
# documents, a "##" heading out of the list and order of AGENTS.md, or a
# milestone cited by number. tools/verify/prosecheck.c checks the
# vocabulary, and a person checks the rest of the register rule in review.
# The script uses grep, find, awk and sed.
#
set -u

REPO="$(cd "$(dirname "$0")/../.." && pwd)"

# The byte scanner beside this script, built by make when missing or older
# than its source: the shell text tools disagree across platforms about NUL
# and high bytes, and C does not.
ASCIISCAN="$REPO/build/tools/verify/asciiscan"
make -s -C "$REPO" build/tools/verify/asciiscan || exit 1
cd "$REPO" || exit 1

findings=$(mktemp)
trap 'rm -f "$findings"' EXIT INT TERM

# Find in-house files under the source roots; the arguments are the name
# filter and -print. The extracted vendor trees and their built output
# (vendor/musl/install and the extracted musl, busybox, dropbear, tcc, pdpmake
# and e2fsprogs trees) are pruned, as are Doom's upstream sources:
# ports/doom/src and the Chocolate Doom and Nuked OPL3 files beside the
# platform layer. build/ and dev-log/ are not roots, so they are not searched.
prune_find() {
    find rach2 ish utils tests krffs tools mk vendor ports \
        -path 'ports/doom/src' -prune -o \
        -path 'ports/doom/midifile.[ch]' -prune -o \
        -path 'ports/doom/mus2mid.[ch]' -prune -o \
        -path 'ports/doom/opl3.[ch]' -prune -o \
        -path 'vendor/musl/install' -prune -o \
        -path 'vendor/musl/musl-*' -prune -o \
        -path 'vendor/busybox/busybox-*' -prune -o \
        -path 'vendor/tcc/tcc-*' -prune -o \
        -path 'vendor/pdpmake/pdpmake-*' -prune -o \
        -path 'vendor/dropbear/dropbear-*' -prune -o \
        -path 'vendor/e2fsprogs/e2fsprogs-*' -prune -o \
        "$@" 2>/dev/null
}

# The Markdown files: those at the top and those under the source roots.
md_files() {
    ls ./*.md 2>/dev/null
    prune_find -name '*.md' -print
}

# The component documents: every in-house Readme.md and NAME.Readme.md but the
# front page. The policy files, the skills, dev-log/, the generated documents
# and the lists keep their own form and are not among them.
doc_files() {
    prune_find '(' -name 'Readme.md' -o -name '*.Readme.md' ')' -print
    find man '(' -name 'Readme.md' -o -name '*.Readme.md' ')' -print 2>/dev/null
}

# Every in-house shell script, as tools/verify/scripts.sh lists them for the
# ShellCheck step, and the make files.
script_files() {
    sh tools/verify/scripts.sh
    ls ./Makefile 2>/dev/null
    prune_find '(' -name '*.mk' -o -name 'Makefile' -o -name 'Makefile.*' ')' -print
}

# The C files and headers.
ch_files() {
    prune_find '(' -name '*.c' -o -name '*.h' ')' -print
}

# The code that is neither C nor shell: the assembly, the linker script, the
# awk checkers and the C fragments a test includes whole. A string in any of
# them is double-quoted, so the C checks apply as they are.
other_code_files() {
    prune_find '(' -name '*.s' -o -name '*.ld' -o -name '*.awk' -o -name '*.inc' ')' -print
}

# The in-house Python files under the source roots.
py_files() {
    prune_find -name '*.py' -print
}

# The configuration the build and the checks read: the editor and formatter
# settings, the ignore lists, the image manifests, the checkers' lists and
# reasons, the GRUB configurations, the BusyBox configurations, the patch
# series, and the tree's other text files. The lists with no suffix are named
# one by one.
config_files() {
    ls ./.clang-format ./.clangd ./.gitignore 2>/dev/null
    ls tests/registry tests/probe-suites tests/probe-helpers tests/system/intervals \
        tools/setup/judges tools/verify/blame-ignore-revs man/aliases man/recovery 2>/dev/null
    prune_find '(' -name '.gitignore' -o -name '.gitattributes' -o -name '.gdbinit' \
        -o -name '*.manifest' -o -name '*.suppress' -o -name '*.reasons' \
        -o -name '*.allow' -o -name '*.cfg' -o -name '*.template' \
        -o -name '*.config' -o -name '*.conf' -o -name 'series' -o -name '*.txt' ')' \
        -print
}

{
    # Non-ASCII bytes in every in-house file: Markdown, shell and make, the
    # C, headers and other code, and the configuration files. The convention
    # step of tools/verify/check.sh checks the C files and headers too.
    { md_files; script_files; ch_files; other_code_files; py_files; config_files; } |
        sort -u | while IFS= read -r f; do
        [ -f "$f" ] || continue
        "$ASCIISCAN" "$f" || true
    done

    # A spaced "--" used as an em-dash in C, H and other code comments.
    # Double-quoted strings are removed first so a usage string is not
    # flagged; bare code never has a spaced "--", so what remains is in a
    # comment.
    { ch_files; other_code_files; } | sort -u | while IFS= read -r f; do
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

    # A spaced "--" on any line of a configuration file. None of these
    # formats writes an option or an operator that way, so the spaced form is
    # prose, in a comment or in text a file stages.
    config_files | sort -u | while IFS= read -r f; do
        [ -f "$f" ] || continue
        grep -n ' -- ' "$f" \
            | sed "s|^|$f:|; s|\$|  [prose em-dash]|"
    done

    # A "Phase N" or "Part N" development-diary tag in a code comment or a
    # configuration file. Strings are stripped from the code first, so a
    # capitalized "Phase" or "Part" followed by a number in what remains is a
    # comment marker. Phases are recorded in commit messages and dev-log/.
    { ch_files; other_code_files; } | sort -u | while IFS= read -r f; do
        [ -f "$f" ] || continue
        sed 's/"[^"]*"//g' "$f" | grep -nE '\b(Phase|Part) [0-9]' \
            | sed "s|^|$f:|; s|\$|  [Phase/Part diary tag]|"
    done
    config_files | sort -u | while IFS= read -r f; do
        [ -f "$f" ] || continue
        grep -nE '\b(Phase|Part) [0-9]' "$f" \
            | sed "s|^|$f:|; s|\$|  [Phase/Part diary tag]|"
    done

    # In Markdown, flag a spaced "--" and a "-" bullet, skipping fenced and
    # indented code so that a command example is not read as prose. A fence is
    # ``` or ~~~ after any leading blanks.
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

    # A component document's "##" headings come from the list in the
    # Documentation section of AGENTS.md, in its order, each at most once;
    # "Use" and "Running" share the first place. Fenced code is skipped.
    doc_files | sort -u | while IFS= read -r f; do
        [ -f "$f" ] || continue
        awk -v file="$f" '
            BEGIN {
                n = split("Use|Running|Results|Interfaces|Sources and license|" \
                    "Local changes|Design|Building|Updating|Adding tests|" \
                    "Limits|Tests", name, "|")
                for (i = 1; i <= n; i++)
                    rank[name[i]] = i
                rank["Running"] = rank["Use"]
                last = 0
            }
            /^[ \t]*```/ { fence = !fence; next }
            /^[ \t]*~~~/ { fence = !fence; next }
            fence         { next }
            /^## / {
                h = substr($0, 4)
                if (!(h in rank))
                    print file ":" NR ":  [heading not in the list]"
                else if (rank[h] <= last)
                    print file ":" NR ":  [heading out of order]"
                else
                    last = rank[h]
            }
        ' "$f"
    done

    # A component document cites no milestone by number: no "milestone N"
    # and no link into a milestone's directory under dev-log/.
    doc_files | sort -u | while IFS= read -r f; do
        [ -f "$f" ] || continue
        awk -v file="$f" '
            tolower($0) ~ /milestones? +[0-9]/ ||
            /dev-log\/[0-9]/ { print file ":" NR ":  [milestone cited]" }
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
