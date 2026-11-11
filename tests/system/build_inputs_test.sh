#!/bin/sh
# build_inputs_test.sh [all|libraries|kernel]: check that the build relinks a
# program when one of its link inputs changes, and only then.
#
# Private copies of musl's libc.a and start files and of libgcc.a each get a
# marker symbol. Two utilities and dnstest are linked against them; an idle
# build must relink nothing, and a changed input must relink all three with its
# new marker. The kernel is linked with a private linker script, whose edit must
# reach the kernel, and the kernel a loader reads must have no debug sections
# and no symbols but _start and _end while kernel.debug keeps them. The
# fixture is for the architecture ARCH names, i686 when it is unset, which
# tests/run gives each case: its compiler, linker script and musl
# (mk/arch.sh). Each make below is given that architecture, and no flags of a
# make above.
set -eu
RACH2_ARCH=${ARCH:-i686}
unset ARCH MAKEFLAGS MFLAGS
REPO=$(cd "$(dirname "$0")/../.." && pwd)
cd "$REPO"
. "$REPO/mk/arch.sh"
case "$RACH2_ARCH" in
    i686) class=ELF32 emulation=elf_i386 ;;
    *) class=ELF64 emulation=elf_x86_64 ;;
esac
mkdir -p build
WORK=
trap '[ -z "$WORK" ] || rm -rf "$WORK"' 0
trap 'exit 130' INT
trap 'exit 143' TERM HUP
WORK=$(mktemp -d "$REPO/build/build-inputs.XXXXXX")
# Report $* and exit 1.
fail() { echo "build-inputs-test: $*" >&2; exit 1; }
cc=$(command -v "${CROSS}gcc")
future=$(($(date +%Y) + 2))
mkdir -p "$WORK/musl/lib"
ln -s "$MUSL_PREFIX/include" "$WORK/musl/include"
for input in libc.a crt1.o crti.o crtn.o; do
    cp "$MUSL_PREFIX/lib/$input" "$WORK/musl/lib/$input"
done
cp "$("$cc" "$MFLAG" -print-libgcc-file-name)" "$WORK/libgcc.a"
for input in libc.a crt1.o crti.o crtn.o libgcc.a; do
    case "$input" in
        libgcc.a) original="$WORK/libgcc.a" ;;
        *) original="$WORK/musl/lib/$input" ;;
    esac
    cp "$original" "$WORK/$input.original"
    chmod u+w "$original"
done
# set_marker INPUT VALUE: give the private INPUT the marker symbol
# build_INPUT with VALUE; its path is left in `changed`.
set_marker() {
    input=$1
    value=$2
    symbol=build_$(echo "$input" | tr . _)
    case "$input" in
        libgcc.a) changed="$WORK/libgcc.a" ;;
        *) changed="$WORK/musl/lib/$input" ;;
    esac
    printf '.globl %s\n.set %s, %s\n.section .note.GNU-stack,"",@progbits\n' "$symbol" "$symbol" "$value" > "$WORK/marker.s"
    "$cc" "$MFLAG" -c "$WORK/marker.s" -o "$WORK/$input.marker.o"
    case "$input" in
        *.a) "${CROSS}ar" r "$changed" "$WORK/$input.marker.o" ;;
        *) "${CROSS}ld" -m "$emulation" -r "$WORK/$input.original" "$WORK/$input.marker.o" -o "$changed" ;;
    esac
}
for input in libc.a crt1.o crti.o crtn.o libgcc.a; do
    set_marker "$input" 1
done
marker_flags=-Wl,-u,build_libc_a,-u,build_libgcc_a
# Make the two utilities and dnstest against the private inputs.
link_programs() {
    make --no-print-directory -C utils -f "$REPO/utils/Makefile" "ARCH=$RACH2_ARCH" "BUILD=$WORK/utils" \
        "PREFIX=$WORK/musl" "LIBGCC=$WORK/libgcc.a" "EXTRA=$marker_flags" \
        "$WORK/utils/echo" "$WORK/utils/sbin/init-login"
    make --no-print-directory -C tests -f "$REPO/tests/Makefile" "ARCH=$RACH2_ARCH" "BUILD=$WORK/tests" \
        "MUSL_PREFIX=$WORK/musl" "MUSL_LIBGCC=$WORK/libgcc.a" \
        "MUSL_POST=$WORK/musl/lib/libc.a $WORK/libgcc.a $WORK/musl/lib/crtn.o -Wl,-z,noexecstack $ARCH_LDFLAGS $marker_flags" \
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
            "${CROSS}readelf" -h "$target" | grep -q "$class" || fail "$target is not $class"
            "${CROSS}nm" "$target" | grep -Eq "^0+2 A $symbol\$" || fail "$input marker did not reach $target"
        done
        echo "build-inputs-test: passed: $input relinks utilities and dnstest"
    done
fi
if [ "${1:-all}" != libraries ]; then
    cp "rach2/sys/arch/$RACH2_ARCH/link.ld" "$WORK/link.ld"
    make --no-print-directory -C rach2 -f "$REPO/rach2/Makefile" "ARCH=$RACH2_ARCH" "BUILD=$WORK/kernel" \
        "LINKER_SCRIPT=$WORK/link"
    printf '\nreview_link_marker = 0x1234;\n' >> "$WORK/link.ld"
    touch -t "${future}01010000" "$WORK/kernel/kernel"
    touch -t "${future}01010001" "$WORK/link.ld"
    make --no-print-directory -C rach2 -f "$REPO/rach2/Makefile" "ARCH=$RACH2_ARCH" "BUILD=$WORK/kernel" \
        "LINKER_SCRIPT=$WORK/link"
    "${CROSS}nm" "$WORK/kernel/kernel.debug" | grep -Eq '^0+1234 A review_link_marker$' ||
        fail "linker script edit did not reach the kernel"
    echo "build-inputs-test: passed: linker script edit reaches the validated kernel"
    # The loaders read a kernel without debug sections and with no symbols but
    # _start and _end, which elfcheck compares with the entry and the image
    # end, and is about half the linked file's size. GDB and nm read
    # kernel.debug, which has them all. nm prints a local symbol's type in
    # lowercase.
    if "${CROSS}readelf" -S "$WORK/kernel/kernel" | grep -q '\.debug_'; then
        fail "the kernel a loader reads carries debug sections"
    fi
    kept=$("${CROSS}nm" "$WORK/kernel/kernel" | awk '{ print $NF }' | sort | tr '\n' ' ')
    [ "$kept" = "_end _start " ] ||
        fail "the kernel a loader reads keeps the symbols $kept, not _end and _start alone"
    "${CROSS}readelf" -S "$WORK/kernel/kernel.debug" | grep -q '\.debug_info' ||
        fail "kernel.debug lacks the debug sections"
    "${CROSS}nm" "$WORK/kernel/kernel.debug" | grep -q ' [a-z] ' ||
        fail "kernel.debug lacks the local symbols"
    echo "build-inputs-test: passed: the kernel has no debug sections and no symbols but _start and _end, and kernel.debug keeps them"
fi
