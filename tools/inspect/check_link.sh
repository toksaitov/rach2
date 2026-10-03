#!/usr/bin/env sh

#
# check_link.sh: ask whether a program could link against what an image has.
#
# The guest compiles with tcc, and tcc links what the guest has: musl's
# `libc.a`, tcc's runtime `libtcc1.a`, and the startup objects. Nothing
# appends libgcc there the way the host's wrapper does, so a symbol musl
# references and neither archive defines is not a build-time question at
# all: it is a student's first link failing with an undefined symbol, which
# is a bad way to learn about libgcc.
#
# So the question is asked here, on the host, of the very files that will
# be staged: every undefined symbol in the libraries, minus everything they
# define between them, must be empty. A weak undefined is not a failure,
# since a link resolves those to zero, and neither is a symbol the C
# library itself defines elsewhere in the same archive. A definition counts
# only when nm reports it with an upper-case type letter: a lower-case one
# is local to its own object and cannot resolve another object's reference,
# and this archive holds exactly that case, a local `__udivmoddi4` beside
# the global one.
#
# Three exclusions, each stated rather than quietly dropped. `main` is
# provided by the program being compiled. The linker's own symbols
# (`_GLOBAL_OFFSET_TABLE_` and the section markers) name places in the
# output image, and tcc defines them as it lays the image out. And an
# archive member no ordinary link reaches may reference what it likes:
# `alloca86-bt.o` is the bounds-checking alloca and wants the `bcheck.o`
# this build leaves out, and a link without `-b` never asks for it.
#
# The symbols are read with CHECK_LINK_NM, the cross nm of the architecture
# the files are for, i686-elf-nm when it is not set.
#
# Usage: [CHECK_LINK_NM=x86_64-elf-nm] check_link.sh LIB_OR_OBJECT...
# Exits non-zero if anything would be left undefined.
#

set -eu

NM="${CHECK_LINK_NM:-i686-elf-nm}"

if [ $# -eq 0 ]; then
    echo "usage: check_link.sh LIB_OR_OBJECT..." >&2
    exit 2
fi
command -v "$NM" >/dev/null 2>&1 || {
    echo "check-link: cannot run $NM: not found" >&2
    exit 2
}

TMPD="$(mktemp -d)"
# bash 3.2, the Mac's sh, gives an EXIT trap a status of 0 when `set -u`
# stops a script that also has `set -e`, so a run that reached no success
# exit fails here.
finished=
status=0
trap 'status=$?; rm -rf "$TMPD"; [ -n "$finished" ] || [ "$status" -ne 0 ] || status=1; exit "$status"' EXIT
trap 'rm -rf "$TMPD"' INT TERM

for path in "$@"; do
    if ! "$NM" "$path" > "$TMPD/nm.out" 2> "$TMPD/nm.err"; then
        echo "check-link: $NM failed on $path: $(cat "$TMPD/nm.err")" >&2
        exit 1
    fi
    case "$path" in
        *.a)
            grep -q '[^[:space:]]' "$TMPD/nm.out" || {
                echo "check-link: $NM reported no symbols in the archive $path, so nothing in it was actually read" >&2
                exit 1
            }
            ;;
    esac
    awk -v def="$TMPD/defined" -v undef="$TMPD/undefined" '
    BEGIN {
        unreached["alloca86-bt.o"] = 1
        unreached["alloca86_64-bt.o"] = 1
        excused["main"] = 1
        excused["_GLOBAL_OFFSET_TABLE_"] = 1
        excused["_etext"] = 1; excused["_edata"] = 1; excused["_end"] = 1
        excused["__bss_start"] = 1; excused["__ehdr_start"] = 1
        excused["__init_array_start"] = 1; excused["__init_array_end"] = 1
        excused["__fini_array_start"] = 1; excused["__fini_array_end"] = 1
        excused["__preinit_array_start"] = 1
        excused["__preinit_array_end"] = 1
        member = ""
    }
    # An archive member header ("name.o:"); an address never starts one.
    /:$/ && $0 !~ /^[0-9-]/ { member = substr($0, 1, length($0) - 1); next }
    NF == 2 && $1 == "U" {
        if (!(member in unreached) && !($2 in excused))
            print $2 >> undef
        next
    }
    NF == 2 && ($1 == "w" || $1 == "v") { next }
    NF == 3 && $2 != "U" && $2 != "u" && $2 != "N" && $2 ~ /^[A-Z]$/ {
        print $3 >> def
    }
    ' "$TMPD/nm.out"
done

: >> "$TMPD/defined"
: >> "$TMPD/undefined"
sort -u "$TMPD/defined" > "$TMPD/defined.s"
sort -u "$TMPD/undefined" > "$TMPD/undefined.s"
comm -23 "$TMPD/undefined.s" "$TMPD/defined.s" > "$TMPD/missing"

if [ -s "$TMPD/missing" ]; then
    echo "check-link: FAIL" >&2
    while IFS= read -r name; do
        echo "  - $name is referenced and nothing here defines it; a link in the guest would stop on it" >&2
    done < "$TMPD/missing"
    echo "  the guest links libc.a, libtcc1.a and the crt objects and nothing else: no libgcc is appended there the way the host's wrapper appends it" >&2
    exit 1
fi

echo "check-link: OK ($(wc -l < "$TMPD/defined.s" | tr -d ' ') symbols defined, nothing left undefined)"
finished=1
