#!/usr/bin/env sh
#
# check_ldscript.sh: hold TinyCC's reader of a GNU ld script, the one
# rach2-linker-script.patch adds, to what GNU ld does with the same script.
#
# The script builds the x86-64 TinyCC for the host, from the pinned tarball
# with this directory's patches applied in build_tcc.sh's order, in a
# temporary directory it removes. It links ld-test/prog.s by ld-test/script.ld
# with TinyCC, with and without -g, and with GNU as and GNU ld, and requires
# of each executable:
#
#   * the first segment at BASE, file offset 0, with the headers, and .text,
#     aligned to 32 bytes, at file offset 0xc0, past the headers' 0xb0;
#   * the address the leaq takes PC-relative is data_base, ADDR(.data), and
#     not a PLT entry's;
#   * the second segment's memory reaching stack_top, the end of the page
#     .bss reserves after its contents by advancing `.`;
#   * image_end, assigned after SECTIONS, at stack_top, and HEADERS,
#     assigned before them, SIZEOF_HEADERS, 0xb0;
#   * with -g, image_marker in the symbol table, of the value 123.
#
# GNU ld's executable has a symbol table without -g, so the symbol rows read
# it; TinyCC writes one only with -g, so they read the -g link, and the
# layout rows the link without it, which is the one a kernel is.
#
# It needs a host C compiler and the x86_64-elf- binutils, and reads no
# build output.
#
# Usage: sh vendor/tcc/check_ldscript.sh
#
set -eu

SCRIPT_DIR=$(cd "$(dirname "$0")" && pwd)
TEST="$SCRIPT_DIR/ld-test"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/rach2-ldscript.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

echo "check-ldscript: building the host's x86-64 TinyCC"
tar xf "$SCRIPT_DIR/tcc-0.9.27.tar.bz2" -C "$WORK"
SRC="$WORK/tcc-0.9.27"
for p in rach2-output-policy.patch rach2-x86_64-static-link.patch \
         rach2-x86-asm.patch rach2-linker-script.patch \
         rach2-builtin-offsetof.patch rach2-va-list-trap.patch; do
    (cd "$SRC" && patch -p1 --batch --forward < "$SCRIPT_DIR/$p" >/dev/null)
done
(cd "$SRC" && ./configure >/dev/null && make x86_64-tcc >/dev/null 2>&1)
T64="$SRC/x86_64-tcc"
cd "$WORK"

FAILS=0
fail() {
    echo "check-ldscript: fail: $1" >&2
    FAILS=$((FAILS + 1))
}

"$T64" -nostdlib -static -Wl,-T,"$TEST/script.ld" -o tcc.elf "$TEST/prog.s"
"$T64" -nostdlib -static -g -Wl,-T,"$TEST/script.ld" -o tccg.elf "$TEST/prog.s"
x86_64-elf-as -o prog.o "$TEST/prog.s"
x86_64-elf-ld -T "$TEST/script.ld" -o gnu.elf prog.o

# phdr ELF N FIELD: field FIELD (2 offset, 3 address, 6 memory size) of the
# Nth PT_LOAD, in hexadecimal.
phdr() {
    x86_64-elf-readelf -lW "$1" | awk -v n="$2" -v f="$3" '$1 == "LOAD" { if (++k == n) print $f }'
}

# section ELF NAME FIELD: the address (4) or the file offset (5) of NAME.
section() {
    x86_64-elf-readelf -SW "$1" | sed 's/^ *\[ *[0-9]*\] *//' |
        awk -v s="$2" -v f="$3" '$1 == s { print "0x" $(f - 1) }'
}

# symbol ELF NAME: NAME's value, as 0x and its hexadecimal digits.
symbol() {
    x86_64-elf-nm "$1" | awk -v s="$2" '$3 == s { print "0x" $1 }'
}

# target ELF: the address the first leaq of .text takes, which objdump
# writes after a #, with 0x before it or without.
target() {
    x86_64-elf-objdump -d "$1" |
        awk '/lea / { for (i = 1; i <= NF; i++) if ($i == "#") { v = $(i + 1); sub(/^0x/, "", v); print "0x" v; exit } }'
}

hex() { printf '%d\n' "$1"; }

# layout NAME ELF: the layout rows of one executable.
layout() {
    if [ "$(hex "$(phdr "$2" 1 3)")" = "$(hex 0x400000)" ] &&
        [ "$(hex "$(phdr "$2" 1 2)")" = 0 ] &&
        [ "$(hex "$(section "$2" .text 5)")" = "$(hex 0xc0)" ]; then
        echo "check-ldscript: $1: the headers' segment at BASE, .text past them at 0xc0: OK"
    else
        fail "$1: the first segment or .text's place"
    fi
    if [ "$(hex "$(target "$2")")" = "$(hex "$(section "$2" .data 4)")" ]; then
        echo "check-ldscript: $1: the PC-relative reference reaches data_base, ADDR(.data): OK"
    else
        fail "$1: the PC-relative reference to data_base reaches $(target "$2")"
    fi
}

# symbols NAME ELF LAYOUT: the symbol rows of one executable's symbol table,
# the second segment of LAYOUT, the executable the table describes.
symbols() {
    top=$(symbol "$2" stack_top)
    vaddr=$(phdr "$3" 2 3)
    memsz=$(phdr "$3" 2 6)
    if [ -n "$top" ] && [ -n "$vaddr" ] && [ -n "$memsz" ] &&
        [ $(($(hex "$vaddr") + $(hex "$memsz"))) = "$(hex "$top")" ]; then
        echo "check-ldscript: $1: the data segment's memory reaches stack_top: OK"
    else
        fail "$1: the data segment's memory does not reach stack_top, ${top:-absent}"
    fi
    if [ -n "$top" ] && [ "$(symbol "$2" image_end)" = "$top" ] &&
        [ "$(hex "$(symbol "$2" HEADERS)")" = "$(hex 0xb0)" ]; then
        echo "check-ldscript: $1: image_end after SECTIONS is stack_top, HEADERS is 0xb0: OK"
    else
        fail "$1: image_end $(symbol "$2" image_end) and HEADERS $(symbol "$2" HEADERS)"
    fi
    if [ "$(hex "$(symbol "$2" image_marker)")" = 123 ]; then
        echo "check-ldscript: $1: image_marker is in the symbol table, 123: OK"
    else
        fail "$1: image_marker is not in the symbol table"
    fi
}

layout tinycc tcc.elf
layout gnu gnu.elf
symbols tinycc tccg.elf tcc.elf
symbols gnu gnu.elf gnu.elf

if [ "$FAILS" -eq 0 ]; then
    echo "check-ldscript: result: ok"
else
    echo "check-ldscript: result: $FAILS failure(s)"
    exit 1
fi
