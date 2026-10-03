#!/usr/bin/env sh
#
# check_sources.sh: kernel source-list guard (run via `make check-sources`, and
# run by the style check, tools/verify/check.sh).
#
# Every C source on disk in the kernel tree (rach2/kernel.c + rach2/sys/**/*.c)
# must appear in the explicit C_SRC list in rach2/sources.mk of some
# architecture, and every C_SRC entry must exist on disk. C_SRC is the shared
# list and the architecture's own, sys/arch/ARCH/sources.mk, so the list
# checked is the union over every architecture that has a processor's
# directory with a source list: an i686 file is in i686's alone, an x86-64
# one in x86-64's alone, and the PC's in both. A new kernel .c that is not added to C_SRC would be
# silently left out of the build (and out of the ABI/boot tests), so this fails
# loudly instead. Doom and the vendored musl/busybox/dropbear trees keep their
# own source lists and are intentionally NOT gated here.
#
# Exits non-zero (and prints the offending files) if the list and the disk
# disagree.
#
set -u

REPO="$(cd "$(dirname "$0")/../.." && pwd)"
K="$REPO/rach2"

# The architectures: each directory under sys/arch with a cpu.h, the
# processor's, and a sources.mk. sys/arch/x86 has the PC's list, which the
# two x86 architectures include, and no cpu.h; arm and arm64 have no list.
arches=$(cd "$K/sys/arch" && for d in */; do
    d=${d%/}
    [ -f "$d/cpu.h" ] && [ -f "$d/sources.mk" ] && echo "$d"
done)
if [ -z "$arches" ]; then
    echo "check-sources: no architecture with a source list under rach2/sys/arch" >&2
    exit 2
fi

# The explicit lists the builds compile, expanded by Make (one source per
# line). Printing a list compiles nothing, so CROSS_CC names a command that
# exists, and a machine without one architecture's cross compiler still
# checks its list.
listed=$(for a in $arches; do
    make --no-print-directory -C "$K" ARCH="$a" CROSS_CC=true print-c-src 2>/dev/null
done | tr ' ' '\n' | sed '/^[[:space:]]*$/d' | sort -u)

# Every C source actually on disk in the kernel tree.
ondisk=$(cd "$K" && find kernel.c sys -name '*.c' 2>/dev/null | sed 's#^\./##' | sort -u)

# Fail closed if mktemp cannot give us scratch files: without these the
# comparison below would find nothing and the check would falsely print OK.
lf=$(mktemp) || { echo "check-sources: mktemp failed" >&2; exit 2; }
df=$(mktemp) || { echo "check-sources: mktemp failed" >&2; rm -f "$lf"; exit 2; }
trap 'rm -f "$lf" "$df"' EXIT INT TERM
printf '%s\n' "$listed" >"$lf"
printf '%s\n' "$ondisk" >"$df"

rc=0

unlisted=$(comm -13 "$lf" "$df")
if [ -n "$unlisted" ]; then
    echo "check-sources: kernel C source(s) on disk but MISSING from C_SRC in rach2/sources.mk:" >&2
    echo "$unlisted" | sed 's/^/  /' >&2
    echo "  -> add them to C_SRC (the build silently skips an unlisted source)." >&2
    rc=1
fi

missing=$(comm -23 "$lf" "$df")
if [ -n "$missing" ]; then
    echo "check-sources: C_SRC lists kernel source(s) that do NOT exist on disk:" >&2
    echo "$missing" | sed 's/^/  /' >&2
    echo "  -> remove the stale entry from C_SRC (or restore the file)." >&2
    rc=1
fi

if [ "$rc" -eq 0 ]; then
    echo "check-sources: OK ($(grep -c . "$lf") kernel sources listed and present, for" $arches")"
fi
exit "$rc"
