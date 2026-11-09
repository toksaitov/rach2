#!/bin/sh
#
# style.sh: the program of the style cases, tests/cases/host.style.json, the
# source checks of `tests/run style` over the in-house tree.
#
# The checks read the in-house code: the kernel, the KRFFS host tools, the
# utilities, the ish shell, the tests, the tools and the build scripts, and
# none of the vendored trees. Each run is one case, named by its argument:
# `assignments`, `cppcheck` and the others the manifest lists. It
# writes the case's report records on its standard output, an assertion for
# each check (tools/etude/Readme.md, Report records), with the lines a person
# reads between them, and exits 0 when every assertion passed and 1 when one
# failed. The manifest says what each case and each assertion checks.
#
# A check judged by a tool's version, that of cppcheck, clang-format or clang,
# reports the branch `recorded` under the version tools/setup/judges records
# and `other` under another, with no assertion of the findings, and prints
# what it found. The historical sparse case checks user pointers with Clang
# and a parsed cast audit on the preparation host. The sizes and manual cases
# use the pinned native pmccabe and mandoc tools. Missing tools fail evidence
# preparation. The other versioned cases retain their tooling-machine route.
#
# etude runs it in the case's workspace, with ETUDE_TREE naming the root of
# the tree it checks, ARCH naming the architecture of a case that has one, and
# TMPDIR the workspace, which etude keeps after a failure. The program changes
# to ETUDE_TREE and unsets it, so that a tree it checks in turn is not taken
# for this one. The checkers it runs are the artifacts the manifest's
# `prepare` lists, which etude builds first.
#
# Usage, in the root of the tree, or anywhere with ETUDE_TREE set to it:
#     sh tests/cases/host/style.sh CASE
#
# The logs go to a private directory under TMPDIR, whose path the first line
# prints; a run that passes or is stopped by a signal removes it, and one that
# fails keeps it.
#

# shellcheck disable=SC2329 # the cases are called by name, at the end
set -u

CASE=${1-}
[ "$#" -eq 1 ] && [ -n "$CASE" ] || { echo "style: usage: style.sh CASE" >&2; exit 2; }
case $(uname -s) in
    MINGW*|MSYS*|UCRT*|CLANG*)
        style_tree=${ETUDE_TREE:-$(pwd)}
        style_tree=$(cygpath -u "$style_tree") || exit 2
        case "$CASE" in
            sparse|sizes|manual) ETUDE_TREE=$style_tree ;;
            *) exec "$style_tree/build/tools/etude/etude" prepare tools style \
                "$(cygpath -m "$style_tree")" \
                "${ETUDE_COLLECTION_ARCH:-${ARCH:-i686}}" "$CASE" ;;
        esac ;;
esac
if [ -n "${ETUDE_TREE-}" ]; then
    cd "$ETUDE_TREE" || exit 2
fi
unset ETUDE_TREE
[ -f tests/registry ] && [ -f tools/verify/scripts.sh ] || {
    echo "style: run it in the root of the tree" >&2
    exit 2
}
REPO=$(pwd -P)

ASCIISCAN="$REPO/build/tools/verify/asciiscan"
GUESTCHECK="$REPO/build/tools/verify/guestcheck"
PROSECHECK="$REPO/build/tools/verify/prosecheck"
SIZECHECK="$REPO/build/tools/verify/sizecheck"
ETUDE="$REPO/build/tools/etude/etude"
ELFCHECK="$REPO/build/tools/inspect/elfcheck"
CFGCHECK="$REPO/build/tools/inspect/cfgcheck"
PECHECK="$REPO/build/tools/etude/win/pecheck"

# The exit trap keeps the logs only after a failure, for the paths it printed.
unset VDIR
status=0
trap 'status=$?; [ "$status" -ne 0 ] || rm -rf ${VDIR:+"$VDIR"}; exit "$status"' EXIT
trap 'rm -rf ${VDIR:+"$VDIR"}; exit 129' HUP
trap 'rm -rf ${VDIR:+"$VDIR"}; exit 130' INT
trap 'rm -rf ${VDIR:+"$VDIR"}; exit 143' TERM
VDIR=$(umask 077; mktemp -d "${TMPDIR:-/tmp}/rach2_verify.XXXXXX") || {
    echo "verify-style: cannot create a temporary directory" >&2
    exit 1
}
printf 'verify-style: logs: %s\n' "$VDIR"

# The ABI check links for the architecture the case is for, ARCH, i686 unless
# set, against that architecture's musl, as the build lays them out
# (mk/arch.mk): an ELF32 i386 program at 0x08048000 or an ELF64 x86-64 one at
# 0x400000. The kernel's Clang checks read the sources of that architecture
# as its compiler does (KCHECK_TARGET and KCHECK_WIDTH).
case "${ARCH:-i686}" in
    x86_64)
        KCHECK_TARGET=x86_64-elf
        KCHECK_WIDTH=-m64
        LIBC="$REPO/build/x86_64/vendor-work/musl/install"
        ABI_CROSS=x86_64-elf-
        ABI_CLASS=ELF64
        ABI_MACHINE='X86-64'
        ABI_TEXT=0x400000
        ABI_ENTRY='0x40[0-9a-f]*'
        ;;
    *)
        KCHECK_TARGET=i686-elf
        KCHECK_WIDTH=-m32
        LIBC="$REPO/vendor/musl/install"
        ABI_CROSS=i686-elf-
        ABI_CLASS=ELF32
        ABI_MACHINE='Intel 80386'
        ABI_TEXT=0x08048000
        ABI_ENTRY='0x804[0-9a-f]*'
        ;;
esac

# ---- The report. ----

RECORDS=0
FAILED=""

# Write the report record WORDS, which fits in one write of 255 bytes.
record() {
    printf '@etude 1 %s\n' "$*"
}

# Make TEXT printable ASCII of at most 160 bytes with no space at either
# end, for a record's detail, which a space there would keep from parsing.
clip() {
    _clip=$(printf '%s' "$1" | tr -c ' -~' '?' | sed 's/^ *//' | cut -c1-160 | sed 's/ *$//')
    printf '%s' "${_clip:-?}"
}

# pass ID: report that the assertion ID passed.
pass() {
    record "assert $1 pass"
    RECORDS=$((RECORDS + 1))
}

# fail ID DETAIL: report that the assertion ID failed, with DETAIL.
fail() {
    record "assert $1 fail $(clip "$2")"
    RECORDS=$((RECORDS + 1))
    FAILED="$FAILED $1"
}

# branch GROUP ALTERNATIVE: report the alternative of GROUP that ran.
branch() {
    record "branch $1 $2"
    RECORDS=$((RECORDS + 1))
}

# show LOG: print the first lines of LOG, indented, for the case's evidence.
show() {
    sed -n '1,20p' "$1" | sed 's/^/    /'
}

# check ID LOG COMMAND...: run COMMAND with its output in the log LOG under
# the logs' directory, and report the assertion ID by its status.
check() {
    _id=$1
    _log=$VDIR/$2
    shift 2
    if "$@" >"$_log" 2>&1; then
        pass "$_id"
    else
        fail "$_id" "$* failed; see $_log"
        show "$_log"
    fi
}

# built ID PATH: report the assertion ID failed and return 1 when the
# artifact PATH, which the case's preparation builds, is absent.
built() {
    [ -x "$2" ] && return 0
    fail "$1" "$2 is not built; etude's preparation builds it"
    return 1
}

# Print the in-house C files and headers: the kernel, any in-house file under
# vendor/musl, the KRFFS host tools, the utilities, the ish shell, the tests,
# the testbed's programs under bed/ and the tools. The vendored trees are left
# out (the musl source and install, termbox2 under krffs/vendor, and
# krffs/vs-dokany), and ports/doom is not read.
inhouse_ch() {
    find rach2 \( -name '*.c' -o -name '*.h' \) 2>/dev/null
    find vendor/musl -path '*/install' -prune -o -path '*/musl-*' -prune -o \
         \( -name '*.c' -o -name '*.h' \) -print 2>/dev/null
    find krffs utils ish term tests bed -path '*/vendor' -prune -o -path '*/vs-dokany' -prune \
         -o \( -name '*.c' -o -name '*.h' \) -print 2>/dev/null
    find tools \( -name '*.c' -o -name '*.h' \) 2>/dev/null
}

# ---- The cases. ----

# Every guest program the tests build is assigned once, and every native case
# names a program; each checker's test plants the faults it finds.
case_assignments() {
    check probecheck probecheck.log sh tools/verify/probecheck.sh
    check probecheck-test probecheck_test.log sh tests/system/probecheck_test.sh
    check nativecheck nativecheck.log sh tools/verify/nativecheck.sh
    check nativecheck-test nativecheck_test.log sh tests/system/nativecheck_test.sh
}

# cppcheck_status LOG STATUS FINDINGS: report the assertions RUNS and CLEAN
# of one part, kernel or host, by cppcheck's STATUS: --error-exitcode=2 makes
# a finding exit 2, and any other nonzero status means cppcheck failed to
# run, which fails RUNS under every version.
cppcheck_status() {
    _part=$1
    _log=$2
    _rc=$3
    if [ "$_rc" -eq 0 ] || [ "$_rc" -eq 2 ]; then
        pass "$_part-runs"
    else
        fail "$_part-runs" "cppcheck exited $_rc; see $_log"
    fi
    [ "$JUDGED" = recorded ] || return 0
    if [ "$_rc" -eq 0 ]; then
        pass "$_part-clean"
    else
        fail "$_part-clean" "$(grep -c 'error:\|warning:\|style:\|performance:\|portability:' "$_log") finding(s); see $_log"
        show "$_log"
    fi
}

# cppcheck over the kernel as i686's 32-bit C89 and as x86-64's 64-bit C89,
# and over the host programs (tools/, tests/system/, krffs/) as C99.
# cppcheck's findings change between majors in both directions: 2.19 reports
# a modulo of one in a mutation hook and an uninitialised word 2.21 does not,
# and 2.21 reports a shadowed argument and an unused cast 2.19 does not. So
# the findings are judged by the version tools/setup/judges records alone;
# another version reports what it found and fails nothing.
case_cppcheck() {
    _recorded=$(sed -n 's/^cppcheck: //p' "$REPO/tools/setup/judges")
    _found=$(cppcheck --version 2>/dev/null | head -1)
    JUDGED=other
    [ "$_found" != "$_recorded" ] || JUDGED=recorded
    branch version "$JUDGED"
    . "$REPO/tools/verify/srclists.sh"
    srclists "$REPO" "$VDIR" _
    _k32=0
    cppcheck --std=c89 --platform=unix32 -D__i386__ --enable=warning,style,performance,portability \
          --error-exitcode=2 --suppress=missingInclude --suppress=missingIncludeSystem \
          --suppressions-list="$REPO/tools/verify/cppcheck.suppress" \
          --file-list="$VDIR/_srcs.txt" >"$VDIR/verify_cppcheck.log" 2>&1 || _k32=$?
    # The x86-64 build's kernel sources, as the 64-bit code they are there:
    # its longs and pointers are 64 bits, which the 32-bit pass would judge
    # wrongly.
    _k64=0
    cppcheck --std=c89 --platform=unix64 -D__x86_64__ --enable=warning,style,performance,portability \
          --error-exitcode=2 --suppress=missingInclude --suppress=missingIncludeSystem \
          --suppressions-list="$REPO/tools/verify/cppcheck.suppress" \
          --file-list="$VDIR/_srcs64.txt" >>"$VDIR/verify_cppcheck.log" 2>&1 || _k64=$?
    # A pass that failed to run decides the part whatever the other found, so
    # a finding of one cannot hide the other's crash.
    if [ "$_k32" -eq 0 ] || { [ "$_k64" -ne 0 ] && [ "$_k64" -ne 2 ]; }; then
        _k32=$_k64
    fi
    _host=0
    cppcheck --std=c99 --platform=unix64 --enable=warning,style,performance,portability --error-exitcode=2 \
          --suppress=missingInclude --suppress=missingIncludeSystem \
          --suppressions-list="$REPO/tools/verify/cppcheck.suppress" \
          --file-list="$VDIR/_hostsrcs.txt" >"$VDIR/verify_cppcheck_host.log" 2>&1 || _host=$?
    cppcheck_status kernel "$VDIR/verify_cppcheck.log" "$_k32"
    cppcheck_status host "$VDIR/verify_cppcheck_host.log" "$_host"
    [ "$JUDGED" = recorded ] ||
        echo "style: cppcheck: findings unjudged under $_found; the record says $_recorded, and the testing host judges"
}

# ShellCheck over every in-house shell script, the list tools/verify/scripts.sh
# prints, so a script added anywhere in the tree is checked without being
# named here. SC1091 is off throughout, since a file sourced through a
# variable cannot be followed. The QEMU run scripts under rach2/ and the build
# scripts at krffs/*.sh also waive SC2086, since they split option strings
# into words on purpose, and SC2034, since their variables are read by the
# files that source them. An empty list fails, since it means the list was
# not made.
case_shellcheck() {
    if sh "$REPO/tools/verify/scripts.sh" >"$VDIR/_scripts.txt" && grep -q . "$VDIR/_scripts.txt"; then
        pass scripts-listed
    else
        fail scripts-listed "tools/verify/scripts.sh failed or listed nothing"
    fi
    _legacy=$(legacy_scripts)
    grep -vE "$_legacy" "$VDIR/_scripts.txt" >"$VDIR/_scripts_strict.txt"
    grep -E "$_legacy" "$VDIR/_scripts.txt" >"$VDIR/_scripts_legacy.txt"
    _sc=0
    xargs shellcheck -x -e SC1091 <"$VDIR/_scripts_strict.txt" \
        >"$VDIR/verify_shellcheck.log" 2>&1 || _sc=1
    xargs shellcheck -x -e SC1091 -e SC2034 -e SC2086 <"$VDIR/_scripts_legacy.txt" \
        >>"$VDIR/verify_shellcheck.log" 2>&1 || _sc=1
    if [ "$_sc" -eq 0 ]; then
        pass scripts-clean
    else
        fail scripts-clean "shellcheck found faults; see $VDIR/verify_shellcheck.log"
        show "$VDIR/verify_shellcheck.log"
    fi
}

# found ID WHAT TEXT: report the assertion ID failed with WHAT and print the
# first three lines of TEXT when TEXT is not empty, and passed otherwise.
found() {
    if [ -n "$3" ]; then
        fail "$1" "$2: $(echo "$3" | head -1)"
        echo "$3" | head -3 | sed 's/^/    /'
    else
        pass "$1"
    fi
}

# An idle wait, a halt with interrupts on that expects to resume, goes through
# the helpers in sys/proc/sched.c, which mark the processor idle, so that the
# timer tells an idle tick from a busy one, and let the big lock go for the
# halt. A process that waits in the kernel this way stays current, so nothing
# else says which tick was which. The halt itself is the architecture's
# `arch_idle_masked`, in sys/arch/*/cpu.c. A `sti; hlt` written anywhere
# else, or a call of it outside the helpers, would count its idle time as
# busy and hold the big lock through the halt, and no test
# would find it, so the check fails on both. A `cli; hlt` that stops the
# machine for good and a halt in early boot, before the scheduler accounts
# anything, are not idle waits and are not looked for. The check reads asm
# statements and the kernel's calls, not comments, and leaves out a host
# test's stub of it and the declaration in a header.
idle_waits() {
    inhouse_ch | grep -vE 'sys/arch/[^/]+/cpu\.c$' \
        | xargs grep -nE '__asm__[^;]*"[^"]*sti;[[:space:]]*hlt' 2>/dev/null
    inhouse_ch | grep -E '^rach2/' | grep -vE 'sys/proc/sched\.c$|sys/arch/[^/]+/cpu\.c$' \
        | xargs grep -nE '(^|[^_[:alnum:]])arch_idle_masked[[:space:]]*\(' 2>/dev/null \
        | grep -vE '^[^:]+\.h:'
}

# bash 3.2, the Mac's /bin/sh and /bin/bash, gives an EXIT trap a status of
# 0 when `set -u` stops a script that also has `set -e`, and a trap that ends
# in success lets the script exit 0; dash, bash 5 and BusyBox's ash report
# the failure. So a script in the scope below that sets both and traps EXIT
# keeps a `finished` flag, set where it succeeds, and its trap fails a run
# that never set it. The scope is the scripts under tools/ and krffs/, most of
# which the Mac runs, and the machine probes and the acceptance's su command.
# The flags are read from a `set` at the start of a line, after any other
# options: `-eu`, `-Ceu`, `-e -u`, the two on separate lines, or
# `-o errexit` and `-o nounset`. A flag on the `#!` line or in a second `set`
# after a `;` is not read. The trap is read from a `trap` at the start of a
# line, and may span lines, its first leaving its quote open and its last
# ending in the quote and its signals, EXIT or 0 among them.
unflagged() {
    _opts='^[[:space:]]*set([[:space:]]+([-+][A-Za-z]+|[-+]o[[:space:]]+[a-z]+))*[[:space:]]+'
    _sigs='([[:space:]]+[A-Z0-9]+)*[[:space:]]+(EXIT|0)([[:space:]]+[A-Z0-9]+)*[[:space:]]*$'
    # shellcheck disable=SC2016
    grep -E '^(tools|krffs)/|^tests/system/accept/su_command\.sh$' \
        | while IFS= read -r f; do
            if grep -qE "${_opts}(-[A-Za-z]*e[A-Za-z]*|-o[[:space:]]+errexit)([[:space:];]|\$)" "$f" &&
                grep -qE "${_opts}(-[A-Za-z]*u[A-Za-z]*|-o[[:space:]]+nounset)([[:space:];]|\$)" "$f" &&
                { grep -qE "^[[:space:]]*trap .*[[:space:]](EXIT|0)([[:space:];]|\$)" "$f" ||
                    { grep -qE "^[[:space:]]*trap[[:space:]]+(--[[:space:]]+)?('[^']*|\"[^\"]*)\$" "$f" &&
                        grep -qE "['\"]${_sigs}" "$f"; }; } &&
                ! grep -qF '[ -n "$finished" ]' "$f"; then
                echo "$f"
            fi
        done
}

# The conventions: no // comments, ASCII only, a guard in every header, an
# idle wait only through the scheduler's helpers, no end-of-options marker
# before a process group in kill, and a `finished` flag where bash 3.2 needs
# one.
# line_comments: print the `//` comments of the files named on standard input.
line_comments() {
    xargs grep -HnE '(^|[^:"])//' 2>/dev/null | grep -vE 'https?://|://'
}

# unguarded: print the headers named on standard input that have no guard.
unguarded() {
    grep '\.h$' | while read -r h; do
        grep -qE '^[[:space:]]*#[[:space:]]*ifndef[[:space:]]+[A-Za-z0-9_]+' "$h" ||
            grep -qE '^[[:space:]]*#[[:space:]]*pragma[[:space:]]+once' "$h" || echo "$h"
    done
}

# group_kills: print the lines of the scripts named on standard input that
# give `kill` an end-of-options marker before a process group. dash, the
# testing host's /bin/sh, fails on `--` in its kill builtin with "Illegal
# number: -", so `kill -TERM -- "-$child"` signals nothing, and a trap that
# stops a test's process group that way waits for the group to end by itself.
# After a signal option a negative pid is already a pid, so the form is
# `kill -TERM "-$child"`.
group_kills() {
    xargs grep -HnE 'kill[^|;&#]*[[:space:]]--[[:space:]]+"?-' 2>/dev/null \
        | grep -vE '^[^:]+:[0-9]+:[[:space:]]*#'
}

# legacy_scripts: print the pattern of the scripts ShellCheck judges with the
# two checks of unset and unquoted variables left off.
legacy_scripts() {
    echo '^(rach2|krffs)/[^/]*\.sh$'
}

case_conventions() {
    built ascii "$ASCIISCAN" || ASCIISCAN=false
    found no-line-comments "a // comment" "$(inhouse_ch | line_comments)"
    if [ "$ASCIISCAN" != false ]; then
        found ascii "non-ASCII bytes in" \
            "$(inhouse_ch | while read -r f; do "$ASCIISCAN" "$f" >/dev/null || echo "$f"; done | sort -u)"
    fi
    found header-guards "no header guard in" "$(inhouse_ch | unguarded)"
    found idle-wait "an idle wait outside idle_wait_irq_off/on" "$(idle_waits)"
    found group-kill "kill with an end-of-options marker before a process group, which dash refuses" \
        "$(sh "$REPO/tools/verify/scripts.sh" | group_kills)"
    found exit-flag "set -eu and an EXIT trap with no finished flag, which bash 3.2 lets exit 0" \
        "$(sh "$REPO/tools/verify/scripts.sh" | unflagged)"
}

# clang-format over every in-house C file and header, with --Werror so that a
# file it would change counts as a failure. The verdict depends on the tool's
# version: two majors format four files of this tree differently. So the
# check is judged by the version tools/setup/judges records, the testing
# host's; another version reports the count and fails nothing.
case_clang_format() {
    _recorded=$(sed -n 's/^clang-format: //p' "$REPO/tools/setup/judges")
    _found=$(clang-format --version </dev/null 2>/dev/null | head -1)
    _judged=other
    [ "$_found" != "$_recorded" ] || _judged=recorded
    branch version "$_judged"
    : >"$VDIR/verify_format.log"
    for f in $(inhouse_ch); do
        [ -f "$f" ] || continue
        clang-format --style=file --dry-run --Werror "$f" >/dev/null 2>&1 || echo "$f" >>"$VDIR/verify_format.log"
    done
    _drift=$(grep -c . "$VDIR/verify_format.log")
    if [ "$_judged" = other ]; then
        echo "style: clang-format: $_drift file(s) differ under $_found; the record says $_recorded, and the testing host judges"
    elif [ "$_drift" -eq 0 ]; then
        pass formatted
    else
        fail formatted "$_drift file(s) differ; see $VDIR/verify_format.log"
        show "$VDIR/verify_format.log"
    fi
}

# The link-dependency test: a changed input relinks what uses it.
case_build_inputs() {
    check link-inputs build-inputs.log sh tests/system/build_inputs_test.sh
}

# readelf ABI of a musl program linked here, so that no earlier build's
# artifact is judged.
case_abi() {
    if [ ! -f "$LIBC/lib/libc.a" ]; then
        fail musl-link "$LIBC/lib/libc.a is absent; make musl builds it for this ARCH"
        return
    fi
    printf 'int main(void){return 0;}\n' >"$VDIR/_abi.c"
    if ! "${ABI_CROSS}gcc" -static -nostdlib -nostartfiles -isystem "$LIBC/include" \
         -o "$VDIR/_abi" "$LIBC/lib/crt1.o" "$LIBC/lib/crti.o" "$VDIR/_abi.c" \
         "$LIBC/lib/libc.a" "$("${ABI_CROSS}gcc" -print-libgcc-file-name)" \
         "$LIBC/lib/crtn.o" -Wl,-z,noexecstack 2>"$VDIR/_abi.log"; then
        fail musl-link "the link failed; see $VDIR/_abi.log"
        show "$VDIR/_abi.log"
        return
    fi
    pass musl-link
    _h=$("${ABI_CROSS}readelf" -h "$VDIR/_abi" 2>/dev/null)
    _l=$("${ABI_CROSS}readelf" -lW "$VDIR/_abi" 2>/dev/null)
    _entry=$(echo "$_h" | sed -n 's/.*Entry point address: *//p')
    # shellcheck disable=SC2254 # the pattern is the architecture's
    if echo "$_h" | grep -q "$ABI_CLASS" &&
        echo "$_h" | grep -qE 'Type: *EXEC' &&
        echo "$_h" | grep -q "$ABI_MACHINE" &&
        ! echo "$_l" | grep -q 'INTERP' &&
        echo "$_l" | grep -qE "LOAD +0x[0-9a-f]+ +0x0*${ABI_TEXT#0x}" &&
        case "$_entry" in $ABI_ENTRY) true ;; *) false ;; esac; then
        echo "style: abi: $ABI_CLASS EXEC $ABI_MACHINE, static, LOAD@$ABI_TEXT, entry $_entry"
        pass elf-header
    else
        fail elf-header "off the ABI of $ABI_CLASS $ABI_MACHINE at $ABI_TEXT, entry $_entry"
    fi
}

# The kernel's source list against the disk, so that a new kernel source
# cannot be left out of the build unnoticed; the guest build lists; no product
# rule under tests/; every source in a compilation database; and the
# architecture layer.
case_sources() {
    check kernel-list verify_sources.log "$REPO/tools/verify/check_sources.sh"
    if built guest-lists "$GUESTCHECK"; then
        check guest-lists verify_guest.log "$GUESTCHECK" "$REPO"
    fi
    # No recipe under tests/ makes a product: the images, disks, ISOs and
    # sticks a run boots are the build's (the root Makefile and mk/), and a
    # suite names one only as a prerequisite. A product is a file under
    # build/images named rach2-*, or one of the image variables the pattern
    # lists. The dollar is grep's, a literal $(NAME) target.
    # shellcheck disable=SC2016
    found no-product-rule "a rule under tests/ makes a product" \
        "$(grep -n '^\(build/images/rach2-[a-z0-9-]*\.[a-z0-9]*\|\$(SEALED_DISK)\|\$(SEALED_EXT2)\|\$(RECOVERY_ISO)\|\$(RECOVERY_IMAGE)\|\$(METAL_IMAGE)\|\$(STICK_IMAGE)\|\$(INSTALL_STICK)\|\$(OS_REAL_IMAGE)\|\$(OS_SIMPLE_IMAGE)\|\$(TEST_IMAGE)\) *:' \
            "$REPO/tests/system/suites.mk" | sed 's/^/tests\/system\/suites.mk:/')"
    # Every in-house source is compiled by something: the generator writes
    # one compilation database per unit from the builds' dry runs, and
    # dbcheck.sh finds a source no unit, includer or test fixture script
    # names.
    if sh "$REPO/tools/dev/gen_compile_commands.sh" >"$VDIR/verify_dbcheck.log" 2>&1 &&
        sh "$REPO/tools/verify/dbcheck.sh" >>"$VDIR/verify_dbcheck.log" 2>&1; then
        echo "style: sources: $(tail -1 "$VDIR/verify_dbcheck.log")"
        pass compile-databases
    else
        fail compile-databases "see $VDIR/verify_dbcheck.log"
        show "$VDIR/verify_dbcheck.log"
    fi
    check architecture-layer verify_arch.log make -s -C "$REPO/rach2" check-arch
}

# Text style. tools/verify/textcheck.sh finds non-ASCII bytes, dashes written
# as a spaced double hyphen, and `-` list bullets in the in-house tree, and
# the headings and milestone citations of the component documents; prosecheck
# finds comment lines past 80 columns and, after its self-test, the terms with
# no valid use in this tree, in comments and in the strings a person reads.
# The rest of the register rule, and the repetition report, are for review;
# see AGENTS.md.
case_text() {
    check textcheck verify_text.log "$REPO/tools/verify/textcheck.sh"
    built comment-columns "$PROSECHECK" || return 0
    check comment-columns verify_cols.log "$PROSECHECK" columns
    check prosecheck-selftest verify_prose_selftest.log "$PROSECHECK" --selftest
    check banned-terms verify_prose.log "$PROSECHECK" banned
}

# Message formats. The kernel's formatter knows a fixed set of conversions,
# and writes one it does not know as its letter alone and takes no argument
# for it, so every later field of that message prints the argument meant for
# the field before it.
case_messages() {
    check kernel-formats verify_fmt.log "$REPO/tools/verify/fmtcheck.sh"
}

# Image reproducibility: one tree, one image. The check stages a scratch
# manifest twice with the KRFFS host tools, so they are built here first, as
# the kernel's case builds the kernel it checks; a binary left in build/ by
# an earlier build may come from other sources.
case_repro() {
    check krffs-tools verify_repro_build.log make -C "$REPO/krffs"
    check one-image verify_repro.log sh "$REPO/tools/verify/check_repro.sh"
}

# The checkers' own self-tests, and the tests of the build rules. Each checker
# runs on every build of what it checks, so here the checker itself is
# tested: elfcheck accepts a well-formed kernel and fails each malformed one a
# loader would load wrongly; cfgcheck accepts the configurations of GRUB's
# core and EFI images and fails every way of losing the recovery path or of
# describing two network setups; pecheck accepts PE images that import only
# Windows's own libraries; the compilation-database generator joins wrapped
# recipes and does not publish an invalid database; suitecheck.sh fails each
# form of a guessed duration and of a skip reported as a pass; etude check
# refuses each fault its fixtures plant; and the scripts check the recovery
# credentials, the vendor stamps' dry-run guard, the build rules in a private
# tree, the scope of the ShellCheck and text cases, this program's private
# log directory, and the git hooks, which must format a staged C file and
# refuse a commit or a push with a text fault in a scratch repository.
case_checkers() {
    built elfcheck "$ELFCHECK" && check elfcheck verify_elfcheck.log "$ELFCHECK" --selftest
    built cfgcheck "$CFGCHECK" && check cfgcheck verify_cfgcheck.log "$CFGCHECK" --selftest
    built pecheck "$PECHECK" && check pecheck verify_pecheck.log "$PECHECK" --selftest
    check compile-db-generator verify_compdb.log sh "$REPO/tools/dev/gen_compile_commands.sh" --selftest
    check suitecheck-selftest verify_suitecheck.log sh "$REPO/tools/verify/suitecheck.sh" --selftest
    built check-selftest "$ETUDE" &&
        check check-selftest verify_check.log "$ETUDE" check --selftest "$REPO/tools/etude/fixtures"
    check recovery-key verify_recovery_key.log sh "$REPO/tests/system/recovery_key_test.sh"
    check dryrun verify_dryrun.log sh "$REPO/tests/system/dryrun_test.sh"
    check build-rules verify_build_rules.log sh "$REPO/tests/system/build_rules_test.sh"
    check style-scope verify_style_scope.log sh "$REPO/tests/system/style_scope_test.sh"
    built style-cases "$ETUDE" && built style-cases "$ASCIISCAN" &&
        check style-cases verify_style_cases.log sh "$REPO/tests/system/style_cases_test.sh"
    check verify-temp verify_temp.log sh "$REPO/tests/system/verify_temp_test.sh"
    built style-hooks "$PROSECHECK" && built style-hooks "$ASCIISCAN" &&
        check style-hooks verify_style_hooks.log sh "$REPO/tests/system/style_hooks_test.sh"
}

# The dev-log index against the directories it lists. AGENTS.md sends a
# reader to dev-log/Readme.md for the milestones in order, so every finished
# milestone must have an entry, the entries must be in the directories'
# numeric order, and a directory with only a plan must have none.
# prosecheck's self-test, in the text case, tests the rules.
case_devlog() {
    built index "$PROSECHECK" && check index verify_devlog.log "$PROSECHECK" devlog
}

# The test registry against the suites it names. tests/run reads the
# registry and runs a suite by the family of cases whose identity ends in its
# name, so a row with no family is a suite that cannot run, and a family with
# no row is a suite nothing runs; a `run-` target in suites.mk is a suite
# that nothing runs either. No other check finds them. The generated lists
# follow.
case_registry() {
    awk -F'|' '!/^#/ && NF > 1 { print $1 }' "$REPO/tests/registry" | sort >"$VDIR/_reg.txt"
    found names-once "a suite is named twice in tests/registry" "$(uniq -d "$VDIR/_reg.txt")"
    if built families "$ETUDE"; then
        if ! "$ETUDE" check -s "$REPO/tools/etude/schemas" families "$REPO"/tests/cases/*.json \
            >"$VDIR/_families.txt" 2>"$VDIR/verify_families.log"; then
            fail families "etude check could not read the case manifests; see $VDIR/verify_families.log"
        else
            sed 's/ .*//; s/.*\.//' "$VDIR/_families.txt" | sort -u >"$VDIR/_cases.txt"
            found families "a suite with no family of cases, a family with no row, or a run- target" \
                "$(comm -23 "$VDIR/_reg.txt" "$VDIR/_cases.txt" | sed 's/^/no cases: /'
                   comm -13 "$VDIR/_reg.txt" "$VDIR/_cases.txt" | sed 's/^/no row: /'
                   grep -o '^run-[a-z0-9-]*' "$REPO/tests/system/suites.mk" | sed 's/^/a target: /')"
        fi
    fi
    # The generated inventory: tests/system/suites.md must be the output of
    # `tests/run inventory`, and suites.mk must have a sentence for every
    # suite.
    "$ETUDE" run -t "$REPO" inventory >"$VDIR/_inventory.md" 2>"$VDIR/verify_inventory.log"
    if cmp -s "$VDIR/_inventory.md" "$REPO/tests/system/suites.md"; then
        pass inventory
    else
        fail inventory "tests/system/suites.md is not tests/run inventory; regenerate it"
        diff "$REPO/tests/system/suites.md" "$VDIR/_inventory.md" | head -5 | sed 's/^/    /'
    fi
    found sentences "a suite has no \`tests/run NAME\` sentence in suites.mk" \
        "$(grep 'no sentence' "$VDIR/_inventory.md" | sed 's/:.*//')"
    # The generated list of programs: man/programs.md is `make programs-md`,
    # which builds BusyBox first for its applet table.
    if ! make -s -C "$REPO" programs-md PROGRAMS_MD="$VDIR/_programs.md" >"$VDIR/verify_programs.log" 2>&1; then
        fail programs-md "man/programs.sh failed; see $VDIR/verify_programs.log"
    elif ! cmp -s "$VDIR/_programs.md" "$REPO/man/programs.md"; then
        fail programs-md "man/programs.md is not man/programs.sh's output; regenerate it with make programs-md"
        diff "$REPO/man/programs.md" "$VDIR/_programs.md" | head -5 | sed 's/^/    /'
    else
        pass programs-md
    fi
    # The generated list of the system calls: rach2/syscalls.md is what
    # `make syscalls` writes from the two kernels' tables.
    if ! make -s -C "$REPO" syscalls SYSCALLS_MD="$VDIR/_syscalls.md" >"$VDIR/verify_syscalls.log" 2>&1; then
        fail syscalls-md "make syscalls failed; see $VDIR/verify_syscalls.log"
    elif ! cmp -s "$VDIR/_syscalls.md" "$REPO/rach2/syscalls.md"; then
        fail syscalls-md "rach2/syscalls.md is not what make syscalls writes; run it to regenerate the file"
        diff "$REPO/rach2/syscalls.md" "$VDIR/_syscalls.md" | head -5 | sed 's/^/    /'
    else
        pass syscalls-md
    fi
}

# The tests wait for events, assert on counts and report what they leave
# out. A sleep for a fixed time and an upper bound on a duration are guesses
# at a machine's speed, and a skip printed in a passing test reports the skip
# as a pass; each fails the check unless an interval with a reason that
# tests/system/intervals lists marks it, tests/system/unconverted counts it in
# an older test, or a case's manifest declares its timing. etude check checks
# the manifests.
case_waits() {
    check waits-and-criteria verify_suites.log sh "$REPO/tools/verify/suitecheck.sh"
}

# The manual covers the machine (tools/verify/mancheck.c): every program the
# manifests stage, every BusyBox applet and every registered system call has
# a page or an alias, every alias names a page, every page passes mandoc's
# lint, and the recovery image stages the pages man/recovery lists.
case_manual() {
    _man_goal=mancheck
    case $(uname -s) in
        MINGW*|MSYS*|UCRT*|CLANG*) _man_goal=mancheck-prepared ;;
    esac
    check mancheck verify_man.log make -s -C "$REPO" "$_man_goal"
}

# clang as a second front end over the kernel, with a short positive list of
# the -Weverything warnings judged useful for this tree. Judged by the clang
# version tools/setup/judges records; another version reports the count and
# fails nothing, as the format case does.
case_clang() {
    _recorded=$(sed -n 's/^clang: //p' "$REPO/tools/setup/judges")
    _found=$(clang --version </dev/null 2>/dev/null | head -1)
    _judged=other
    [ "$_found" != "$_recorded" ] || _judged=recorded
    branch version "$_judged"
    _warn="-Wcast-align -Wconditional-uninitialized -Wshadow-all -Wtypedef-redefinition -Wcomma"
    : >"$VDIR/verify_clang.log"
    _res=$(clang -print-resource-dir 2>/dev/null)
    _bad=0
    _files=$(make -s -C "$REPO/rach2" print-c-src)
    [ -n "${STYLE_IN_TOOLING:-}" ] ||
        make -s -C "$REPO/rach2" acpica >>"$VDIR/verify_clang.log" 2>&1 || _bad=1
    _acpi=$(make -s -C "$REPO/rach2" print-acpi-cflags)
    for f in $_files; do
        case $f in
            sys/acpi/*) _more=$_acpi ;;
            *) _more= ;;
        esac
        # shellcheck disable=SC2086
        (cd "$REPO/rach2" && clang --target="$KCHECK_TARGET" "$KCHECK_WIDTH" -ansi -ffreestanding -nostdinc \
            -isystem "$_res/include" $_more -fsyntax-only $_warn "$f") >>"$VDIR/verify_clang.log" 2>&1 ||
            _bad=$((_bad + 1))
    done
    _warnings=$(grep -c "warning:" "$VDIR/verify_clang.log")
    if [ "$_judged" = other ]; then
        echo "style: clang: $_warnings warning(s) under $_found; the record says $_recorded, and the testing host judges"
    elif [ "$_bad" -eq 0 ] && [ "$_warnings" -eq 0 ]; then
        echo "style: clang: 0 warnings over $(echo "$_files" | wc -w | tr -d ' ') files"
        pass no-warnings
    else
        fail no-warnings "$_warnings warning(s), $_bad file(s) failed; see $VDIR/verify_clang.log"
        show "$VDIR/verify_clang.log"
    fi
}

# Keep the historical case identity while checking the existing __user
# contract with Clang and a parsed audit of explicit __force casts. Both
# compiler fixtures and kernel inputs must pass on the preparation host.
case_sparse() {
    if ! command -v "${USERCHECK_CLANG:-clang}" >/dev/null 2>&1; then
        branch tool absent
        fail runs "clang is required for user-pointer checking"
        fail no-findings "the user-pointer contract was not checked"
        return
    fi
    branch tool present
    : >"$VDIR/verify_sparse.log"
    _bad=0
    python3 "$REPO/tests/system/usercheck_test.py" >>"$VDIR/verify_sparse.log" 2>&1 || _bad=1
    _files=$(make -s -C "$REPO/rach2" print-c-src)
    [ -n "${STYLE_IN_TOOLING:-}" ] ||
        make -s -C "$REPO/rach2" acpica >>"$VDIR/verify_sparse.log" 2>&1 || _bad=1
    _acpi=$(make -s -C "$REPO/rach2" print-acpi-cflags)
    # The ACPI files, sys/acpi/, take the build's flags for ACPICA's headers.
    # shellcheck disable=SC2086
    _plain=$(printf '%s\n' $_files | grep -v '^sys/acpi/')
    # shellcheck disable=SC2086
    _acpifiles=$(printf '%s\n' $_files | grep '^sys/acpi/')
    # shellcheck disable=SC2086
    (cd "$REPO/rach2" && python3 "$REPO/tools/verify/usercheck.py" --arch "${ARCH:-i686}" \
        $_plain) >>"$VDIR/verify_sparse.log" 2>&1 || _bad=1
    # shellcheck disable=SC2086
    (cd "$REPO/rach2" && python3 "$REPO/tools/verify/usercheck.py" --arch "${ARCH:-i686}" \
        --cflags="$_acpi" $_acpifiles) >>"$VDIR/verify_sparse.log" 2>&1 || _bad=1
    if [ "$_bad" -eq 0 ]; then
        pass runs
        pass no-findings
    else
        fail runs "user-pointer checks failed; see $VDIR/verify_sparse.log"
        fail no-findings "the compiler or cast audit reported a finding"
        show "$VDIR/verify_sparse.log"
    fi
}

# The size limits over every in-house C file. pmccabe's table gives the
# complexity; without pmccabe the three measures the checker takes itself are
# judged and the complexity waits for the testing host. The reasons file
# records every excess allowed and the value it may not grow past.
case_sizes() {
    built within-limits "$SIZECHECK" || return 0
    _files=$(inhouse_ch | grep '\.c$' | sort)
    _reasons="$REPO/tools/verify/sizes.reasons"
    _table="$VDIR/verify_pmccabe.txt"
    case $(uname -s) in
        MINGW*|MSYS*|UCRT*|CLANG*)
            _reasons=$(cygpath -m "$_reasons") || return 1
            _table=$(cygpath -m "$_table") || return 1 ;;
    esac
    set --
    if command -v pmccabe >/dev/null 2>&1; then
        branch complexity judged
        # shellcheck disable=SC2086
        pmccabe $_files >"$VDIR/verify_pmccabe.txt" 2>/dev/null
        set -- --pmccabe "$_table"
    else
        branch complexity unjudged
        echo "style: sizes: complexity not judged; pmccabe is absent here, and the testing host judges"
    fi
    # shellcheck disable=SC2086
    check within-limits verify_sizes.log "$SIZECHECK" "$@" "$_reasons" $_files
}

# The links between the documents: a link to a file that a commit moved
# leads the reader nowhere. The milestone checklists under dev-log/ are lists
# of links by their nature, so they are checked by name although the records
# around them are not.
case_links() {
    check documents verify_links.log sh "$REPO/tools/verify/linkcheck.sh"
    _checklists=$(find dev-log -name checklist.md | sort | tr '\n' ' ')
    # shellcheck disable=SC2086 # one path a word, listed above
    check checklists verify_checklists.log sh "$REPO/tools/verify/linkcheck.sh" $_checklists
}

# ---- The tooling machine. ----

# judged_here: succeed when this host judges the case itself: its tool is at
# the version tools/setup/judges records.
judged_here() {
    _found=$("$CASE" --version </dev/null 2>/dev/null | head -1)
    [ "$_found" = "$(sed -n "s/^$CASE: //p" "$REPO/tools/setup/judges")" ]
}

# in_tooling: run the case in etude's tooling machine, as the header says,
# and exit with the job's status, or return when no machine is made. The
# job's input is one tar file of the tree's tracked and unignored files under
# the directories the three cases read. tools/setup/elfwrap.sh makes the
# elf cross compilers there from Ubuntu's Linux ones, as on the testing host,
# for source checks that query their include directories. On a Mac with Apple
# silicon, unless ETUDE_TOOLING_ARCH says otherwise, the job runs in the arm64
# machine under Apple's hypervisor, as the GNU layer's build does, since the
# amd64 one runs under TCG there more than twenty times slower. A job that
# ends before its report does reports each assertion it did not report as not
# run.
in_tooling() {
    [ -z "${STYLE_IN_TOOLING:-}" ] || return 0
    # shellcheck source=tests/system/tooling.sh
    . "$REPO/tests/system/tooling.sh"
    if [ -z "${ETUDE_TOOLING_ARCH:-}" ] && [ "$(uname -s)/$(uname -m)" = Darwin/arm64 ] &&
        [ "$(sysctl -n kern.hv_support 2>/dev/null)" = 1 ]; then
        ETUDE_TOOLING_ARCH=arm64 RACH2_QEMU_ACCEL=hvf
        export ETUDE_TOOLING_ARCH RACH2_QEMU_ACCEL
    fi
    if ! tooling_made; then
        echo "style: $CASE: no tooling machine is made to judge it under the recorded version; etude provision tooling prepare makes one"
        return 0
    fi
    echo "style: $CASE: the case runs in etude's tooling machine"
    git -C "$REPO" ls-files -co --exclude-standard -- .clang-format mk rach2 krffs utils ish term \
        tests bed tools vendor/musl | while IFS= read -r f; do
        [ ! -f "$REPO/$f" ] || printf '%s\n' "$f"
    done >"$VDIR/files"
    # The machine's tree cannot unpack ACPICA, so its headers go with it.
    if [ "$CASE" = clang ] || [ "$CASE" = sparse ]; then
        make -s -C "$REPO/rach2" acpica >&2 || unrun "ACPICA could not be unpacked"
        _inc=$(make -s -C "$REPO/rach2" print-acpi-cflags | sed -n 's#.*-isystem \.\./\([^ ]*\).*#\1#p')
        (cd "$REPO" && find "$_inc" -type f) >>"$VDIR/files"
    fi
    if ! grep -q . "$VDIR/files" ||
        ! COPYFILE_DISABLE=1 tar -c --format ustar -f "$VDIR/tree.tar" -C "$REPO" -T "$VDIR/files"; then
        unrun "the job's files could not be packed"
    fi
    cat >"$VDIR/job.sh" <<EOF
set -u
cd "\$ETUDE_WORK" && mkdir scratch tree bin && tar -x -f tree.tar -C tree || exit 3
sh tree/tools/setup/elfwrap.sh i686 bin >&2 && sh tree/tools/setup/elfwrap.sh x86_64 bin >&2 ||
    exit 3
exec env PATH="\$ETUDE_WORK/bin:\$PATH" TMPDIR="\$ETUDE_WORK/scratch" \\
    ETUDE_TREE="\$ETUDE_WORK/tree" ETUDE_CASE=$ID ARCH=${ARCH:-i686} STYLE_IN_TOOLING=1 \\
    sh "\$ETUDE_WORK/tree/tests/cases/host/style.sh" $CASE
EOF
    tooling run -c 2 -m 2048 -s $(($(wc -c <"$VDIR/tree.tar") / 1048576 + 64)) \
        -i "$VDIR/tree.tar" -o scratch -O "$VDIR" "$VDIR/job.sh" >"$VDIR/job.out"
    _status=$?
    cat "$VDIR/job.out"
    [ "$_status" -eq 0 ] || [ ! -d "$VDIR/scratch" ] ||
        echo "style: $CASE: the logs of the job are under $VDIR/scratch"
    ! grep -q '^@etude 1 end ' "$VDIR/job.out" || exit "$_status"
    case $_status in
        125) unrun "the tooling machine failed; the lines of etude provision tooling say why" ;;
        126) unrun "the tooling machine is not made; etude provision tooling prepare makes it" ;;
        *) unrun "the tooling machine's job ended with status $_status before its report did" ;;
    esac
}

# unrun REASON: complete the report of a case whose job ended before its
# report did, which may have `begin` and some records already: give `begin`
# when it lacks it, each assertion it lacks as not run for REASON, and `end`.
unrun() {
    touch "$VDIR/job.out"
    grep -q '^@etude 1 begin ' "$VDIR/job.out" || record "begin $ID"
    RECORDS=$(grep -c -E '^@etude 1 (assert|notrun|branch) ' "$VDIR/job.out")
    _reported=" $(awk '$1 == "@etude" && $2 == 1 && ($3 == "assert" || $3 == "notrun") {
        printf "%s ", $4 }' "$VDIR/job.out")"
    case "$CASE" in
        cppcheck) _all="kernel-runs host-runs kernel-clean host-clean" ;;
        clang-format) _all=formatted ;;
        clang) _all=no-warnings ;;
    esac
    for _a in $_all; do
        case "$_reported" in
            *" $_a "*) ;;
            *) record "notrun $_a $(clip "$1")"; RECORDS=$((RECORDS + 1)) ;;
        esac
    done
    record "end $RECORDS"
    echo "style: result: incomplete: $ID: $1"
    exit 1
}

# ---- The run. ----

ID=host.style.$CASE
if [ -n "${ETUDE_CASE:-}" ] && [ "$ETUDE_CASE" != "$ID" ]; then
    echo "style: ETUDE_CASE is $ETUDE_CASE, and the argument names $ID" >&2
    exit 2
fi
case "$CASE" in
    assignments|cppcheck|shellcheck|conventions|clang-format|build-inputs) ;;
    abi|sources|text|messages|repro|checkers|devlog) ;;
    registry|waits|manual|clang|sparse|sizes|links) ;;
    *) echo "style: no case $CASE" >&2; exit 2 ;;
esac
case "$CASE" in
    cppcheck|clang-format|clang) judged_here || in_tooling ;;
esac
record "begin $ID"
"case_$(echo "$CASE" | tr - _)"
record "end $RECORDS"
if [ -z "$FAILED" ]; then
    echo "style: result: passed: $ID"
    exit 0
fi
echo "style: result: failed: $ID:$FAILED"
exit 1
