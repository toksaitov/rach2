#!/bin/sh
# Build real targets with private link inputs and check each incremental link.
set -eu
REPO=$(cd "$(dirname "$0")/../.." && pwd)
cd "$REPO"
mkdir -p build
WORK=$(mktemp -d "$REPO/build/build-inputs.XXXXXX")
trap 'rm -rf "$WORK"' 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP
fail() { echo "build-inputs-test: $*" >&2; exit 1; }
cc=$(command -v i686-elf-gcc)
future=$(($(date +%Y) + 2))
mkdir -p "$WORK/musl/lib"
ln -s "$REPO/vendor/musl/install/include" "$WORK/musl/include"
for input in libc.a crt1.o crti.o crtn.o; do
    cp "vendor/musl/install/lib/$input" "$WORK/musl/lib/$input"
done
cp "$("$cc" -m32 -print-libgcc-file-name)" "$WORK/libgcc.a"
for input in libc.a crt1.o crti.o crtn.o libgcc.a; do
    case "$input" in
        libgcc.a) original="$WORK/libgcc.a" ;;
        *) original="$WORK/musl/lib/$input" ;;
    esac
    cp "$original" "$WORK/$input.original"
    chmod u+w "$original"
done
set_marker() {
    input=$1
    value=$2
    symbol=build_$(echo "$input" | tr . _)
    case "$input" in
        libgcc.a) changed="$WORK/libgcc.a" ;;
        *) changed="$WORK/musl/lib/$input" ;;
    esac
    printf '.globl %s\n.set %s, %s\n.section .note.GNU-stack,"",@progbits\n' "$symbol" "$symbol" "$value" > "$WORK/marker.s"
    "$cc" -m32 -c "$WORK/marker.s" -o "$WORK/$input.marker.o"
    case "$input" in
        *.a) i686-elf-ar r "$changed" "$WORK/$input.marker.o" ;;
        *) i686-elf-ld -m elf_i386 -r "$WORK/$input.original" "$WORK/$input.marker.o" -o "$changed" ;;
    esac
}
for input in libc.a crt1.o crti.o crtn.o libgcc.a; do
    set_marker "$input" 1
done
marker_flags=-Wl,-u,build_libc_a,-u,build_libgcc_a
link_programs() {
    make --no-print-directory -C utils "BUILD=$WORK/utils" \
        "PREFIX=$WORK/musl" "LIBGCC=$WORK/libgcc.a" "EXTRA=$marker_flags" \
        "$WORK/utils/echo" "$WORK/utils/sbin/init-login"
    make --no-print-directory -C tests "BUILD=$WORK/tests" \
        "MUSL_PREFIX=$WORK/musl" "MUSL_LIBGCC=$WORK/libgcc.a" \
        "MUSL_POST=$WORK/musl/lib/libc.a $WORK/libgcc.a $WORK/musl/lib/crtn.o -Wl,-z,noexecstack $marker_flags" \
        "$WORK/tests/dnstest"
}
case "${1:-all}" in
    all|libraries|kernel) ;;
    *) fail "expected all, libraries or kernel" ;;
esac
if [ "${1:-all}" != kernel ]; then
    link_programs
    for input in libc.a crt1.o crti.o crtn.o libgcc.a; do
        for lib in "$WORK"/musl/lib/* "$WORK/libgcc.a"; do
            touch -t 200001010000 "$lib"
        done
        for target in "$WORK/utils/echo" "$WORK/utils/sbin/init-login" "$WORK/tests/dnstest"; do
            touch -t "${future}01010000" "$target"
        done
        touch -t "${future}01010000" "$WORK/stamp"
        link_programs
        for target in "$WORK/utils/echo" "$WORK/utils/sbin/init-login" "$WORK/tests/dnstest"; do
            [ ! "$WORK/stamp" -nt "$target" ] || fail "idle build relinked $target"
        done
        set_marker "$input" 2
        touch -t "${future}01010001" "$changed"
        link_programs
        for target in "$WORK/utils/echo" "$WORK/utils/sbin/init-login" "$WORK/tests/dnstest"; do
            [ "$WORK/stamp" -nt "$target" ] || fail "$input did not relink $target"
            i686-elf-readelf -h "$target" | grep -q ELF32 || fail "$target is not elf32"
            i686-elf-nm "$target" | grep -q "00000002 A $symbol" || fail "$input marker did not reach $target"
        done
        echo "build-inputs-test: passed: $input relinks utilities and dnstest"
    done
fi
if [ "${1:-all}" != libraries ]; then
    cp rach2/sys/arch/i686/link.ld "$WORK/link.ld"
    make --no-print-directory -C rach2 "BUILD=$WORK/kernel" "LINKER_SCRIPT=$WORK/link"
    printf '\nreview_link_marker = 0x1234;\n' >> "$WORK/link.ld"
    touch -t "${future}01010000" "$WORK/kernel/kernel"
    touch -t "${future}01010001" "$WORK/link.ld"
    make --no-print-directory -C rach2 "BUILD=$WORK/kernel" "LINKER_SCRIPT=$WORK/link"
    i686-elf-nm "$WORK/kernel/kernel" | grep -q '00001234 A review_link_marker' ||
        fail "linker script edit did not reach the kernel"
    echo "build-inputs-test: passed: linker script edit reaches the validated kernel"
    # The loaders read a kernel without debug sections, which is what keeps it
    # inside a boot slot's 2 MiB; GDB reads kernel.debug, which has them.
    if i686-elf-readelf -S "$WORK/kernel/kernel" | grep -q '\.debug_'; then
        fail "the kernel a loader reads carries debug sections"
    fi
    i686-elf-readelf -S "$WORK/kernel/kernel.debug" | grep -q '\.debug_info' ||
        fail "kernel.debug lacks the debug sections"
    echo "build-inputs-test: passed: the kernel has no debug sections and kernel.debug keeps them"
fi
