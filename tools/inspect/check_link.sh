#!/usr/bin/env sh

#
# check_link.sh: check that a program could link against what an image has.
#
# The guest compiles with tcc, and tcc links what the guest has: musl's
# `libc.a`, tcc's runtime `libtcc1.a`, and the startup objects. Nothing
# appends libgcc there as vendor/musl/muslgcc does on the host, so a symbol
# musl references and neither archive defines would stop a student's first
# link in the guest with an undefined symbol.
#
# The script checks the files that will be staged, on the host: the
# undefined symbols in them, less every symbol they define between them,
# must be none. A weak undefined symbol is not a failure, since a link
# resolves it to zero. A definition counts only when nm reports it with an
# upper-case type letter: a lower-case one is local to its own object and
# cannot resolve another object's reference, and the staged archives have
# such a local `__udivmoddi4` beside the global one.
#
# Three kinds of reference are excluded, and the awk program lists them.
# `main` is defined by the program being compiled. The linker's own symbols
# (`_GLOBAL_OFFSET_TABLE_` and the section markers) are places in the output
# image, which tcc defines as it lays the image out. And the references of
# an archive member no ordinary link reaches are not counted: `alloca86-bt.o`
# (`alloca86_64-bt.o` on x86-64) is the bounds-checking alloca and references
# the `bcheck.o` this build leaves out, and only a link with `-b` takes it.
#
# The symbols are read with CHECK_LINK_NM, the cross nm of the architecture
# the files are for, i686-elf-nm when it is not set.
#
# Usage: [CHECK_LINK_NM=x86_64-elf-nm] check_link.sh LIB_OR_OBJECT...
# Exits 1 when a symbol would be left undefined or nm fails on a file, and 2
# on a usage error or when nm is not found.
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
