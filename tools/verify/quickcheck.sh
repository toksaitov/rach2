#!/bin/sh
#
# quickcheck.sh: the checks of `tests/run style` that fit in a commit, over
# the files a change touches, and a rough reading of the lines it adds.
#
# tools/verify/hookcheck.sh runs it on what a commit stages and on what a
# push gives a milestone branch. It runs every check and prints every
# finding, `path:line: what` wherever the check knows the line, so that one
# list fixes one change. Exit 0 when there is no finding, 1 when there is
# one, 2 when it cannot run. A tool that is missing is left out with a
# warning: the check stays in `tests/run style`.
#
# Over the files the change touches, a line that begins `<<<<<<< ` or
# `>>>>>>> `, or is `=======` alone, is refused as a merge's conflict marker,
# and the checks are those of the style cases `conventions`, `shellcheck` and
# `text`, kernel-format's `fmtcheck.sh`, and `make -C rach2 check-arch`
# narrowed to the shared kernel files. A whole check of the tree runs when
# its inputs changed: `check_sources.sh`, `probecheck.sh` and
# `nativecheck.sh`, guestcheck, `linkcheck.sh`, mancheck and
# rach2/syscalls.md, the dev-log index, and with a built etude the suite
# inventory and `suitecheck.sh`.
#
# The rough reading judges only the lines the change adds, since older code
# does not all follow these rules: a C function whose block is missing, a new
# source file with no opening comment, a word the register rule forbids, a
# "not X, it's Y" reversal, and a message outside `component: detail`. The
# patterns were tried on the lines of the last 700 commits, and the kinds
# that fired on correct text were left out; a reviewer judges the rest of the
# Writing and Comments sections of AGENTS.md.
#
# With -p, for a push, it also runs cppcheck and the kernel's Clang warnings
# when the versions tools/setup/judges records are here, and links the host
# tests and fuzz harnesses that compile a changed kernel source, since a new
# call in that source needs a stub there. Only an undefined symbol counts:
# the compile is rougher than the suite's.
#
# TREE is a whole copy of the tree as the change leaves it, FILES its changed
# paths, CFILES those of them that are in-house C, one a line, and DIFF the
# output of `git diff -U0 --no-prefix` for the change. TMPDIR is absolute.
#
# Usage: quickcheck.sh [-p] TREE FILES CFILES DIFF
#        quickcheck.sh -H SOURCE, which -p runs for each host test
#
set -u

REPO=$(cd "$(dirname "$0")/../.." && pwd)
PUSH=0
if [ "${1-}" = -p ]; then
    PUSH=1
    shift
fi
HOST=
if [ "${1-}" = -H ]; then
    HOST=${2-}
    shift 2
    set -- '' '' '' ''
fi
[ "$#" -eq 4 ] || { echo 'quickcheck: usage: quickcheck.sh [-p] TREE FILES CFILES DIFF' >&2; exit 2; }
TREE=$1
FILES=$2
CFILES=$3
DIFF=$4
JOBS=${RACH2_QUICK_JOBS:-8}
# A hook's git settings name the commit's repository; the tools run here, make
# among them, read others.
unset GIT_DIR GIT_INDEX_FILE GIT_WORK_TREE

WORK=
trap 'rm -rf ${WORK:+"$WORK"}' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
WORK=$(umask 077 && mktemp -d "${TMPDIR:-/tmp}/rach2-quickcheck.XXXXXX") || exit 2
OUT=$WORK/findings
: >"$OUT"

# The rules of the conventions case have one home, the style cases' program.
for f in line_comments unguarded idle_waits group_kills legacy_scripts unflagged; do
    sed -n "/^$f() {\$/,/^}\$/p" "$REPO/tests/cases/host/style.sh"
done >"$WORK/conventions.sh"
# shellcheck source=/dev/null
. "$WORK/conventions.sh"

# inhouse_ch: print the files idle_waits reads, here the changed ones.
# shellcheck disable=SC2329 # idle_waits calls it
inhouse_ch() { cat "$CFILES"; }

# warn TEXT: say what was left out.
warn() { echo "quickcheck: warning: $1" >&2; }

# found WHAT: add each line of standard input to the findings, with WHAT
# after it. WHAT holds no `|`, `&` or backslash.
found() { sed "s|\$|: $1|" >>"$OUT"; }

# run COMMAND...: add what COMMAND prints to the findings when it fails.
run() { "$@" >"$WORK/run.out" 2>&1 || cat "$WORK/run.out" >>"$OUT"; }

# touched PATTERN: say whether a changed path matches the extended PATTERN.
touched() { grep -Eq "$1" "$FILES"; }

# in_tree NAME: say whether the copy has tools/verify/NAME. The hook's own
# test works in a small tree without it.
in_tree() { [ -f "$TREE/tools/verify/$1" ]; }

# conventions: the C half of the style case `conventions`.
conventions() {
    [ -s "$CFILES" ] || return 0
    cd "$TREE" || return 0
    line_comments <"$CFILES" | cut -d: -f1,2 | found 'a // comment'
    unguarded <"$CFILES" | found 'no header guard'
    idle_waits | cut -d: -f1,2 | found 'an idle wait outside idle_wait_irq_off or idle_wait_irq_on'
    cd - >/dev/null || exit 2
}

# scripts: the changed scripts, by the style cases' list, through the shell
# half of `conventions` and through ShellCheck.
scripts() {
    in_tree scripts.sh || return 0
    (cd "$TREE" && sh tools/verify/scripts.sh 2>/dev/null) | grep -Fx -f "$FILES" >"$WORK/scripts"
    [ -s "$WORK/scripts" ] || return 0
    (cd "$TREE" && group_kills <"$WORK/scripts") | cut -d: -f1,2 |
        found 'kill with an end-of-options marker before a process group, which dash refuses'
    (cd "$TREE" && unflagged <"$WORK/scripts") |
        found 'set -eu and an EXIT trap with no finished flag, which bash 3.2 lets exit 0'
    if ! command -v shellcheck >/dev/null 2>&1; then
        warn 'no shellcheck here; the scripts were not judged by it'
        return 0
    fi
    _legacy=$(legacy_scripts)
    grep -vE "$_legacy" "$WORK/scripts" >"$WORK/strict"
    grep -E "$_legacy" "$WORK/scripts" >"$WORK/legacy"
    cd "$TREE" || return 0
    [ ! -s "$WORK/strict" ] || xargs shellcheck -f gcc -x -e SC1091 <"$WORK/strict" >>"$OUT" 2>&1
    [ ! -s "$WORK/legacy" ] ||
        xargs shellcheck -f gcc -x -e SC1091 -e SC2034 -e SC2086 <"$WORK/legacy" >>"$OUT" 2>&1
    cd - >/dev/null || exit 2
}

# text: textcheck.sh and, for the kernel, fmtcheck.sh, over the changed files.
text() {
    xargs sh "$REPO/tools/verify/textcheck.sh" -C "$TREE" <"$FILES" >"$WORK/text.out" 2>&1 ||
        cat "$WORK/text.out" >>"$OUT"
    grep -E '^rach2/.*\.[ch]$' "$FILES" >"$WORK/kfiles" || return 0
    in_tree fmtcheck.sh || return 0
    xargs sh "$TREE/tools/verify/fmtcheck.sh" <"$WORK/kfiles" >"$WORK/fmt.out" 2>&1 ||
        grep -v ' file(s) with a conversion' "$WORK/fmt.out" >>"$OUT"
}

# unquote PATH: print PATH as git names it with core.quotePath's quoting
# undone, and a newline after it: a path git writes in double quotes, as it
# does one with a backslash, a quote or a control or non-ASCII byte, has its
# C escapes read, as git's quote.c writes them.
unquote() {
    case $1 in
        \"*\") printf '%s\n' "$1" | LC_ALL=C awk '{
            s = substr($0, 2, length($0) - 2); out = ""
            for (i = 1; i <= length(s); i++) {
                c = substr(s, i, 1)
                if (c != "\\") { out = out c; continue }
                c = substr(s, ++i, 1)
                if (c ~ /[0-7]/) {
                    v = 0
                    for (j = 0; j < 3; j++) v = v * 8 + index("01234567", substr(s, i + j, 1)) - 1
                    out = out sprintf("%c", v); i += 2
                } else if (c == "n") out = out "\n"
                else if (c == "t") out = out "\t"
                else if (c == "a") out = out sprintf("%c", 7)
                else if (c == "b") out = out sprintf("%c", 8)
                else if (c == "f") out = out sprintf("%c", 12)
                else if (c == "r") out = out sprintf("%c", 13)
                else if (c == "v") out = out sprintf("%c", 11)
                else out = out c
            }
            print out
        }' ;;
        *) printf '%s\n' "$1" ;;
    esac
}

# markers: find the conflict markers a merge leaves in the changed files. A
# finding names the path as git quotes it when the path has a newline, so
# that each finding stays one line.
markers() {
    nl='
'
    while IFS= read -r q; do
        f=$(unquote "$q"; echo x)
        f=${f%x}
        f=${f%"$nl"}
        [ -f "$TREE/$f" ] || continue
        case $f in *"$nl"*) name=$q ;; *) name=$f ;; esac
        grep -nIE '^(<<<<<<< |=======$|>>>>>>> )' "$TREE/$f" | cut -d: -f1 |
            while IFS= read -r n; do printf '%s:%s\n' "$name" "$n"; done
    done <"$FILES" | found 'a conflict marker'
}

# arch_layer: `make -C rach2 check-arch` over the changed shared kernel files.
# A changed shared header is read by that check whichever file it is given.
arch_layer() {
    [ -f "$TREE/rach2/sources.mk" ] || return 0
    touched '^rach2/.*\.[ch]$' || return 0
    if ! command -v clang >/dev/null 2>&1; then
        warn 'no clang here; check-arch was left out'
        return 0
    fi
    sed -n 's|^rach2/\(.*\.c\)$|\1|p' "$FILES" >"$WORK/kc"
    _shared=$(tr -s ' \\\t' '\n' <"$TREE/rach2/sources.mk" | grep -Fx -f "$WORK/kc" | tr '\n' ' ')
    if [ -z "$_shared" ]; then
        grep -E '^rach2/sys/.*\.h$' "$FILES" | grep -qv '^rach2/sys/arch/' || return 0
        _shared=kernel.c
    fi
    run make -s -C "$TREE/rach2" check-arch C_SRC_SHARED="$_shared"
}

# mancheck_arch ARCH: mancheck over ARCH's tables, with no applets, since
# that table comes from BusyBox's build; it says so in one line, left out.
mancheck_arch() {
    (cd "$TREE" && "$WORK/mancheck" man "rach2/sys/arch/$1/sysnr.h" "$2" "$WORK/applets.h" \
        utils/programs.mk krffs/manifests/rach2-recovery.manifest \
        krffs/manifests/os-simple.manifest krffs/manifests/os-real.manifest \
        krffs/manifests/rach2-metal-disk.manifest krffs/manifests/rach2-term.manifest) 2>&1 |
        grep -vE 'no applet name was read|names without a page or a fault' >>"$OUT"
}

# manual: the pages of the programs and system calls, and rach2/syscalls.md.
manual() {
    touched '^man/|/systab\.c$|/sysnr\.h$|/syscall_net\.c$|^utils/programs\.mk$|^krffs/manifests/|^rach2/syscalls\.md$' ||
        return 0
    in_tree mancheck.c || return 0
    if ! command -v mandoc >/dev/null 2>&1; then
        warn 'no mandoc here; the manual pages were not judged'
        return 0
    fi
    cc -o "$WORK/mancheck" "$TREE/tools/verify/mancheck.c" 2>"$WORK/cc.err" || {
        warn 'tools/verify/mancheck.c did not compile'
        return 0
    }
    : >"$WORK/applets.h"
    mancheck_arch i686 rach2/sys/arch/i686/systab.c:rach2/sys/syscall_net.c
    mancheck_arch x86_64 rach2/sys/arch/x86_64/systab.c
    (cd "$TREE" && "$WORK/mancheck" --calls man rach2/sys/arch/i686/sysnr.h \
        rach2/sys/arch/i686/systab.c rach2/sys/arch/x86_64/sysnr.h \
        rach2/sys/arch/x86_64/systab.c) >"$WORK/syscalls.md" 2>/dev/null
    cmp -s "$WORK/syscalls.md" "$TREE/rach2/syscalls.md" ||
        echo 'rach2/syscalls.md: not what make syscalls writes; run it' >>"$OUT"
}

# registry: the suites' list, and with an etude newer than its source, of this
# tree or of the clone it is a worktree of, their inventory and their waits.
registry() {
    touched '^tests/' || return 0
    [ -f "$TREE/tests/registry" ] || return 0
    awk -F'|' '!/^#/ && NF > 1 { print $1 }' "$TREE/tests/registry" | sort | uniq -d |
        sed 's|^|tests/registry: |' | found 'a suite is named twice'
    _main=$(cd "$REPO" && cd "$(git rev-parse --git-common-dir)/.." && pwd)
    for _etude in "$REPO/build/tools/etude/etude" "$_main/build/tools/etude/etude" ''; do
        [ -x "$_etude" ] || continue
        [ -n "$(find "$REPO/tools/etude" -name '*.[ch]' -newer "$_etude" | head -n 1)" ] || break
    done
    if [ -z "$_etude" ]; then
        warn 'no etude built from this source; the suites inventory and their waits were not judged'
        return 0
    fi
    ETUDE=$_etude SUITECHECK_ROOT=$TREE run sh "$REPO/tools/verify/suitecheck.sh"
    "$_etude" run -t "$TREE" inventory >"$WORK/inventory.md" 2>/dev/null
    cmp -s "$WORK/inventory.md" "$TREE/tests/system/suites.md" ||
        echo 'tests/system/suites.md: not the output of tests/run inventory; regenerate it' >>"$OUT"
}

# whole: the checks of the whole tree whose inputs changed.
whole() {
    if touched '^rach2/.*(\.c|sources\.mk)$' && in_tree check_sources.sh; then
        run sh "$TREE/tools/verify/check_sources.sh"
    fi
    if touched '^tests/|^krffs/manifests/' && in_tree probecheck.sh; then
        PROBECHECK_ROOT=$TREE run sh "$REPO/tools/verify/probecheck.sh"
        NATIVECHECK_ROOT=$TREE run sh "$REPO/tools/verify/nativecheck.sh"
    fi
    if touched '(sources|programs|guest)\.mk$' && in_tree guestcheck.c &&
        cc -o "$WORK/guestcheck" "$TREE/tools/verify/guestcheck.c" 2>"$WORK/cc.err"; then
        run "$WORK/guestcheck" "$TREE"
    fi
    if touched '\.md$' && in_tree linkcheck.sh; then
        run sh "$TREE/tools/verify/linkcheck.sh"
    fi
    if touched '^dev-log/|^tools/verify/prosecheck\.c$' && in_tree prosecheck.c &&
        cc -o "$WORK/prosecheck" "$TREE/tools/verify/prosecheck.c" 2>"$WORK/cc.err"; then
        (cd "$TREE" && run "$WORK/prosecheck" devlog)
        run "$WORK/prosecheck" --selftest
    fi
}

# added: list the added lines of DIFF in $WORK/added as PATH, a tab, the line
# number, a tab and the text, and the files DIFF creates in $WORK/new. A hunk
# counts its lines, since an added line may itself start with `++`.
added() {
    awk -v new="$WORK/new" '
        left > 0 && /^\+/ { printf "%s\t%d\t%s\n", path, line++, substr($0, 2); left--; next }
        /^--- / { fresh = ($2 == "/dev/null"); next }
        /^\+\+\+ / {
            path = substr($0, 5)
            if (fresh) print path >new
            next
        }
        /^@@ / {
            n = split($3, a, ",")
            line = substr(a[1], 2) + 0
            left = (n > 1) ? a[2] + 0 : 1
        }
    ' "$DIFF" >"$WORK/added"
    [ -f "$WORK/new" ] || : >"$WORK/new"
}

# wording: judge the added lines by the register and message rules. The files
# that name the forbidden words, and the dev log's own quotations, are left
# out; so are the host tests' reports and the vendored trees.
wording() {
    grep -vE '^(tools/verify/(prose|quickcheck)|tests/system/style_hooks_test|AGENTS\.md|CLAUDE\.md|dev-log/|vendor/|ports/doom/src/)' \
        "$WORK/added" | awk -F'\t' '
        {
            text = $0
            sub(/^[^\t]*\t[^\t]*\t/, "", text)
            low = tolower(text)
            where = $1 ":" $2 ": "
            if (low ~ /(^|[^a-z])(honest|honestly|fiction|fictional)([^a-z]|$)/)
                print where "a word the register rule forbids: honest or fiction"
            if (low ~ /(^|[^a-z])(not|[a-z]+n.t) [^.,;:()]*, *(it.s|it is|that.s|that is)( |$)/)
                print where "a \"not X, it is Y\" reversal, which the register rule forbids"
            msg = ""
            if ($1 ~ /\.sh$/ && text ~ />&2/ && match(text, /(echo|printf) +["\047]/))
                msg = substr(text, RSTART + RLENGTH)
            else if ($1 ~ /\.c$/ && $1 !~ /^tests\/host\// && match(text, /fprintf\(stderr, "/))
                msg = substr(text, RSTART + RLENGTH)
            if (msg != "" && msg !~ /^[ %\\]/ &&
                msg !~ /^[a-z0-9_.+\/${}-]+( [a-z0-9_-]+)?: /)
                print where "a message outside `component: detail`, in lowercase"
            if ($1 ~ /^rach2\// && text ~ /klog(_v)?\(KLOG_[A-Z]+, "[^"]*", "[A-Z][a-z]/)
                print where "a kernel message that starts with a capital"
        }' >>"$OUT"
}

# opening: judge each new C file and script for its opening comment.
opening() {
    grep -E '\.([ch]|sh)$' "$WORK/new" | grep -v '^vendor/' | while IFS= read -r f; do
        [ -f "$TREE/$f" ] || continue
        case $f in
            *.sh) sed -n '/^#!/d; /^#/q; /./{s/.*/x/;p;q;}' "$TREE/$f" | grep -q x ;;
            *) grep -vE '^$|^#(ifndef|define) ' "$TREE/$f" | sed -n 1p | grep -qv '^/\*' ;;
        esac && echo "$f:1: a source file begins with a comment on what it is for"
    done >>"$OUT"
}

# blocks: find each C function the change adds without a comment before it.
# A definition is a `{` in the first column under a signature that starts in
# the first column. An exported function may have its block on the declaration
# in a header, so a function a header declares passes, and so does main.
blocks() {
    grep '\.c$' "$CFILES" | while IFS= read -r f; do
        [ -f "$TREE/$f" ] || continue
        awk -F'\t' -v want="$f" '$1 == want { print $2 }' "$WORK/added" >"$WORK/lines"
        [ -s "$WORK/lines" ] || continue
        awk -v file="$f" '
            FNR == NR { fresh[$0] = 1; next }
            /^\{/ {
                if (start && sig !~ /^(typedef|struct|union|enum|extern "C")/ && sig !~ /=[ \t]*$/ &&
                    fresh[start] && above !~ /\*\/[ \t]*$/) {
                    name = sig
                    sub(/\(.*/, "", name)
                    sub(/.*[ *]/, "", name)
                    printf "%s %s %d %s\n", file, name, start, (sig ~ /^static/) ? "static" : "open"
                }
                start = 0
                next
            }
            /^[A-Za-z_]/ {
                if ($0 ~ /[;:]$/ || $0 !~ /\(/) { start = 0 } else { start = FNR; sig = $0; above = prev }
            }
            /^$/ || /^[#}]/ { start = 0 }
            { prev = $0 }
        ' "$WORK/lines" "$TREE/$f"
    done | while read -r f name line scope; do
        [ "$name" != main ] || continue
        if [ "$scope" = open ] && (cd "$TREE" &&
            find rach2 tests utils krffs tools term ish -name '*.h' \
                -exec grep -lE "(^|[ *])$name\(" {} + 2>/dev/null | grep -q .); then
            continue
        fi
        echo "$f:$line: a function with no /** block before it"
    done >>"$OUT"
}

# host_compile SOURCE: compile and link one host test or fuzz harness in the
# current directory, roughly as tools/etude/host_prepare.c does, and print its
# undefined symbols. The flags leave the sanitizers off; -O1 stays, since it
# drops the dead calls the test has no stub for.
host_compile() {
    _deps=$(sed -nE 's/^.*(host|fuzz)-deps:[[:space:]]*//p' "$1" | sed 's/\*.*//' | tr '\n' ' ')
    _more=$(sed -nE 's/^.*(host|fuzz)-cflags:[[:space:]]*//p' "$1" | sed 's/\*.*//' | tr '\n' ' ')
    # shellcheck disable=SC2086 # the lists are one word an argument
    cc -std=c99 -D_DEFAULT_SOURCE -D_FORTIFY_SOURCE=0 -DRACH2_HOSTTEST -DFIXTURE_NO_MAIN -DFUZZ_REPLAY \
        -O1 -Wno-empty-body -ffreestanding -w -iquote rach2/sys -iquote rach2/sys/arch/x86 \
        -iquote rach2/sys/arch/i686 -iquote rach2/sys/lib -iquote rach2/sys/proc -I rach2/sys \
        -I rach2/sys/lib -I rach2/sys/proc -I utils -I krffs -I tests/host/compat \
        -I tools/etude/include $_more -o "$WORK/host.$$" "$1" $_deps 2>&1 |
        sed -n "s|.*undefined reference to [\`']\([^']*\)'.*|$1: no stub for \1|p; s|^ *\"_\([A-Za-z0-9_]*\)\", referenced from:.*|$1: no stub for \1|p" |
        grep -v ' no stub for main$' | sort -u
    rm -f "$WORK/host.$$"
}

# host_links: link what compiles a changed kernel source. Each source is found
# by its name in an include or a host-deps line.
host_links() {
    touched '^rach2/.*\.c$' || return 0
    [ -d "$TREE/tests/host" ] || return 0
    sed -n 's|^rach2/.*/\([^/]*\.c\)$|\1|p' "$FILES" | sort -u >"$WORK/names"
    (cd "$TREE" && while IFS= read -r n; do
        grep -lE "[\"/ ]$n([\" ]|\$)" tests/host/*_hosttest.c tests/fuzz/*_fuzz.c 2>/dev/null
    done <"$WORK/names") | sort -u >"$WORK/hosts"
    [ -s "$WORK/hosts" ] || return 0
    (cd "$TREE" && xargs -P "$JOBS" -I{} sh "$REPO/tools/verify/quickcheck.sh" -H {} <"$WORK/hosts") >>"$OUT"
}

# cppcheck_list STD PLATFORM DEFINE LIST: cppcheck over the changed files of
# the source list LIST, as the case `cppcheck` reads them, in the current
# directory.
cppcheck_list() {
    sed 's|^\./||' "$WORK/$4" | grep -Fx -f "$FILES" >"$WORK/cpp.list" || return 0
    # shellcheck disable=SC2086 # no define is no word
    cppcheck --std="$1" --platform="$2" $3 -q --enable=warning,style,performance,portability \
        --suppress=missingInclude --suppress=missingIncludeSystem \
        --suppressions-list=tools/verify/cppcheck.suppress \
        --template='{file}:{line}: {message} [{id}]' --file-list="$WORK/cpp.list" >>"$OUT" 2>&1
}

# cppcheck_files: the case `cppcheck`, on the changed files, under its
# recorded version alone, since another finds other things.
cppcheck_files() {
    [ -s "$CFILES" ] || return 0
    _want=$(sed -n 's/^cppcheck: //p' "$REPO/tools/setup/judges")
    [ "$(cppcheck --version 2>/dev/null)" = "$_want" ] || {
        warn "cppcheck is not $_want here; it was left out"
        return 0
    }
    # shellcheck source=tools/verify/srclists.sh
    . "$REPO/tools/verify/srclists.sh"
    cd "$TREE" || return 0
    srclists . "$WORK" _
    cppcheck_list c89 unix32 -D__i386__ _srcs.txt
    cppcheck_list c89 unix64 -D__x86_64__ _srcs64.txt
    cppcheck_list c99 unix64 '' _hostsrcs.txt
    cd - >/dev/null || exit 2
}

# clang_warnings: the case `clang` on the changed kernel files, under its
# recorded version alone, on i686, as the case's first run reads them.
clang_warnings() {
    _want=$(sed -n 's/^clang: //p' "$REPO/tools/setup/judges")
    grep '^rach2/.*\.c$' "$FILES" | grep -v '^rach2/sys/acpi/' >"$WORK/kc2" || return 0
    [ "$(clang --version 2>/dev/null | head -n 1)" = "$_want" ] || {
        warn "clang is not $_want here; its warnings were left out"
        return 0
    }
    _res=$(clang -print-resource-dir)
    sed 's|^rach2/||' "$WORK/kc2" | (cd "$TREE/rach2" && xargs -P "$JOBS" -n 1 clang --target=i686-elf -m32 \
        -ansi -ffreestanding -nostdinc -isystem "$_res/include" -fsyntax-only -Wcast-align \
        -Wconditional-uninitialized -Wshadow-all -Wtypedef-redefinition -Wcomma) 2>&1 |
        grep 'warning:' >>"$OUT"
}

if [ -n "$HOST" ]; then
    host_compile "$HOST"
    exit 0
fi

conventions
scripts
text
markers
arch_layer
manual
registry
whole
added
wording
opening
blocks
if [ "$PUSH" = 1 ]; then
    host_links
    cppcheck_files
    clang_warnings
fi

[ -s "$OUT" ] || exit 0
sort -u "$OUT" >&2
exit 1
