#!/usr/bin/env sh

#
# check_asm.sh: hold TinyCC's assemblers to GNU as on the code a kernel starts
# in, the code rach2-x86-asm.patch made them take.
#
# The script builds the i386 and the x86-64 TinyCC for the host, from the
# pinned tarball with this directory's patches applied in build_tcc.sh's
# order, in a temporary directory it removes. It then assembles the files
# under asm-test/ with each TinyCC and with GNU as and compares them:
#
#   insns.s       as 16-bit and as 32-bit code, with both TinyCCs, the
#                 instructions of the disassembly one for one with GNU as's,
#                 and the two TinyCCs' bytes equal;
#   sys64.s       as 64-bit code, the same against GNU as;
#   trampoline.s  16-bit, 32-bit and 64-bit code that takes its addresses as
#                 differences of labels, most of them later in the file:
#                 linked at one address, the TinyCC object and the GNU one
#                 must be equal byte for byte, linked by GNU ld or by TinyCC;
#                 its 16-bit and 32-bit half the same for i386;
#   pcrel.s       16-bit, 32-bit and 64-bit fields of a constant less a
#                 label, relocations against no symbol, the same against
#                 GNU's linked by GNU ld, by TinyCC from the object, and by
#                 TinyCC in the run that assembles them.
#
# The 32-bit pass leaves out four forms with a 16-bit register that the i386
# TinyCC has always encoded its own way (movw to a segment register with the
# operand-size prefix; sldt, smsw, str, and jmp and call through %ax without
# it), which code of the target's own size keeps as before.
#
# It needs a host C compiler and the binutils of both cross toolchains,
# i686-elf- and x86_64-elf-, and reads no build output.
#
# Usage: sh vendor/tcc/check_asm.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
TEST="$SCRIPT_DIR/asm-test"
WORK=$(mktemp -d "${TMPDIR:-/tmp}/check_asm.XXXXXX")
trap 'rm -rf "$WORK"' EXIT

echo "check-asm: building the host's i386 and x86-64 TinyCC"
tar xf "$SCRIPT_DIR/tcc-0.9.27.tar.bz2" -C "$WORK"
SRC="$WORK/tcc-0.9.27"
for p in rach2-output-policy.patch rach2-x86_64-static-link.patch \
         rach2-x86-asm.patch rach2-linker-script.patch \
         rach2-builtin-offsetof.patch rach2-va-list-trap.patch; do
    (cd "$SRC" && patch -p1 --batch --forward < "$SCRIPT_DIR/$p" >/dev/null)
done
(cd "$SRC" && ./configure >/dev/null && make i386-tcc x86_64-tcc >/dev/null 2>&1)
T32="$SRC/i386-tcc"
T64="$SRC/x86_64-tcc"
cd "$WORK"

FAILS=0
fail() {
    echo "check-asm: fail: $1" >&2
    FAILS=$((FAILS + 1))
}

# The instructions of an object's .text as objdump writes them, without
# their addresses and bytes.
text() {
    "$1" -d "$3" "$2" | awk -F'\t' 'NF >= 3 { print $3 }' | sed 's/ *$//'
}

# compare NAME SOURCE GNU-AS OBJDUMP MODE TCC: the disassembled text of the
# TinyCC object against that of the GNU one.
compare() {
    "$3" -o "$1.gnu.o" "$2"
    "$6" -c -o "$1.tcc.o" "$2"
    text "$4" "$1.gnu.o" "$5" > "$1.gnu.txt"
    text "$4" "$1.tcc.o" "$5" > "$1.tcc.txt"
    if cmp -s "$1.gnu.txt" "$1.tcc.txt"; then
        echo "check-asm: $1: $(wc -l < "$1.gnu.txt" | tr -d ' ') instructions as GNU as has them"
    else
        diff "$1.gnu.txt" "$1.tcc.txt" | sed 's/^/    /' >&2 || true
        fail "$1: TinyCC's instructions differ from GNU as's"
    fi
}

{ echo ".code16"; cat "$TEST/insns.s"; } > code16.s
{ echo ".code32"; sed -e '/^    movw %ax, %[des]s$/d' -e '/^    sldt %ax$/d' \
      -e '/^    smsw %ax$/d' -e '/^    str %ax$/d' -e '/^    jmp \*%ax$/d' \
      -e '/^    call \*%ax$/d' "$TEST/insns.s"; } > code32.s

compare code16-x86_64 code16.s x86_64-elf-as x86_64-elf-objdump -Mi8086 "$T64"
compare code16-i386 code16.s i686-elf-as i686-elf-objdump -Mi8086 "$T32"
compare code32-x86_64 code32.s x86_64-elf-as x86_64-elf-objdump -Mi386 "$T64"
compare code32-i386 code32.s i686-elf-as i686-elf-objdump -Mi386 "$T32"
compare sys64 "$TEST/sys64.s" x86_64-elf-as x86_64-elf-objdump -Mx86-64 "$T64"

for c in code16 code32; do
    x86_64-elf-objcopy -O binary -j .text "$c-x86_64.tcc.o" "$c-x86_64.bin"
    i686-elf-objcopy -O binary -j .text "$c-i386.tcc.o" "$c-i386.bin"
    if cmp -s "$c-x86_64.bin" "$c-i386.bin"; then
        echo "check-asm: $c: the x86-64 and the i386 TinyCC write the same bytes"
    else
        fail "$c: the x86-64 and the i386 TinyCC write different bytes"
    fi
done

# trampoline: linked at 1 MiB by GNU ld, and by TinyCC with an entry beside it.
printf '.globl _start\n_start:\n' > start.s
"$T64" -c -o tramp.tcc.o "$TEST/trampoline.s"
x86_64-elf-as -o tramp.gnu.o "$TEST/trampoline.s"
"$T64" -c -o start64.o start.s
x86_64-elf-ld -o tramp.gnu.elf -Ttext=0x100000 -e trampoline_start tramp.gnu.o
x86_64-elf-ld -o tramp.tcc.elf -Ttext=0x100000 -e trampoline_start tramp.tcc.o
"$T64" -nostdlib -static -Wl,-Ttext=0x100000 -o tramp.tcclink.elf tramp.tcc.o start64.o
x86_64-elf-objcopy -O binary -j .text tramp.gnu.elf tramp.gnu.bin
x86_64-elf-objcopy -O binary -j .text tramp.tcc.elf tramp.tcc.bin
x86_64-elf-objcopy -O binary -j .text tramp.tcclink.elf tramp.tcclink.bin
if cmp -s tramp.gnu.bin tramp.tcc.bin && cmp -s tramp.gnu.bin tramp.tcclink.bin; then
    echo "check-asm: trampoline: the same $(wc -c < tramp.gnu.bin | tr -d ' ') bytes as GNU as's, linked by GNU ld and by TinyCC"
else
    fail "trampoline: TinyCC's linked bytes differ from GNU's"
fi

# pcrel: fields of a constant less a label, relocations against no symbol,
# linked by GNU ld, by TinyCC from the object, and by TinyCC in the run that
# assembles them, where its linker adds to what the field holds.
"$T64" -c -o pcrel.tcc.o "$TEST/pcrel.s"
x86_64-elf-as -o pcrel.gnu.o "$TEST/pcrel.s"
x86_64-elf-ld -o pcrel.gnu.elf -Ttext=0x100000 -e pcrel_start pcrel.gnu.o
x86_64-elf-ld -o pcrel.tcc.elf -Ttext=0x100000 -e pcrel_start pcrel.tcc.o
"$T64" -nostdlib -static -Wl,-Ttext=0x100000 -o pcrel.tcclink.elf pcrel.tcc.o start64.o
"$T64" -nostdlib -static -Wl,-Ttext=0x100000 -o pcrel.tccrun.elf "$TEST/pcrel.s" start.s
for e in gnu tcc tcclink tccrun; do
    x86_64-elf-objcopy -O binary -j .text "pcrel.$e.elf" "pcrel.$e.bin"
done
if cmp -s pcrel.gnu.bin pcrel.tcc.bin && cmp -s pcrel.gnu.bin pcrel.tcclink.bin &&
    cmp -s pcrel.gnu.bin pcrel.tccrun.bin; then
    echo "check-asm: pcrel: the same $(wc -c < pcrel.gnu.bin | tr -d ' ') bytes as GNU as's, linked by GNU ld and by TinyCC"
else
    fail "pcrel: TinyCC's fields of a constant less a label differ from GNU's"
fi

# The 16-bit and 32-bit half for i386, which has no 64-bit field.
sed -e '/^\.code64/,/^\.balign/{/^\.balign/!d;}' -e 's/    ljmp [$]0x18.*/    hlt/' \
    -e '/\.quad trampoline_end - trampoline_start/d' "$TEST/trampoline.s" > tramp32.s
"$T32" -c -o tramp32.tcc.o tramp32.s
i686-elf-as -o tramp32.gnu.o tramp32.s
"$T32" -c -o start32.o start.s
i686-elf-ld -o tramp32.gnu.elf -Ttext=0x100000 -e trampoline_start tramp32.gnu.o
i686-elf-ld -o tramp32.tcc.elf -Ttext=0x100000 -e trampoline_start tramp32.tcc.o
"$T32" -nostdlib -static -Wl,-Ttext=0x100000 -o tramp32.tcclink.elf tramp32.tcc.o start32.o
i686-elf-objcopy -O binary -j .text tramp32.gnu.elf tramp32.gnu.bin
i686-elf-objcopy -O binary -j .text tramp32.tcc.elf tramp32.tcc.bin
i686-elf-objcopy -O binary -j .text tramp32.tcclink.elf tramp32.tcclink.bin
if cmp -s tramp32.gnu.bin tramp32.tcc.bin && cmp -s tramp32.gnu.bin tramp32.tcclink.bin; then
    echo "check-asm: trampoline, i386: the same $(wc -c < tramp32.gnu.bin | tr -d ' ') bytes as GNU as's, linked by GNU ld and by TinyCC"
else
    fail "trampoline, i386: TinyCC's linked bytes differ from GNU's"
fi

if [ "$FAILS" -ne 0 ]; then
    echo "check-asm: result: $FAILS failure(s)" >&2
    exit 1
fi
echo "check-asm: result: ok"
