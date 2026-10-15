#!/usr/bin/env sh

#
# build_tcc.sh: build the Tiny C Compiler for Rach2 from the pinned, vendored
# tarball, so the machine can compile a program on itself.
#
# The result is a static executable that RUNS on Rach2 and COMPILES for Rach2,
# for the architecture in RACH2_ARCH (mk/arch.sh): an i386 compiler on
# i686 Rach2, an x86-64 one on x86-64 Rach2, each the machine's native
# compiler. Both halves of that sentence have to be arranged, and the arranging
# is what most of this script is: tcc's configure decides what it is building
# for from the machine it is building on, and this machine is neither. An
# x86-64 build extracts into build/x86_64/vendor-work/tcc/ and stages under
# build/x86_64/vendor/tcc/.
#
# Three decisions, each read out of the tarball rather than guessed:
#
# A cross prefix, always. With an empty one, configure calls itself native and
# RUNS the conftest it just built (configure:309-316); our compiler emits
# Rach2 binaries, which the build host cannot run. So a toolchain directory is
# written here holding rach2-gcc (the musl wrapper), rach2-ar and rach2-strip,
# and configure is given --cross-prefix pointing at it. That also routes every
# tool through the wrapper, which is why no --ar is needed.
#
# A CPU, always. The cross prefix stops the conftest from running; it does not
# say what to build for. Without --cpu, configure takes the target from
# `uname -m` (configure:157), which on an arm64 Mac is arm64.
#
# No Mach-O. On a Mac, configure adds OSX to its configuration
# (configure:52-56), which makes the Makefile define TCC_TARGET_MACHO
# (Makefile:71) and rename the target to $(ARCH)-osx (Makefile:48). TARGETOS and
# CONFIG_OSX are overridden on the make command line, and the result is checked
# rather than hoped for.
#
# libtcc1.a, tcc's runtime, is normally built by running the tcc that was just
# built. lib/Makefile:23-27 offers <target>-libtcc1-usegcc=yes, which builds it
# with CC and AR instead; that is what is used, and nothing here ever executes
# the produced binary.
#
# The guest's own paths are baked in at configure time, because they are where
# the compiler will look when it runs on the machine, not where anything lives
# here.
#
# --config-musl is passed for a reason that is not the obvious one. It does set
# a dynamic loader path, which is nothing to us since we never emit a dynamic
# executable; what it also does is drop bcheck.o from the runtime
# (lib/Makefile:31-33), and bcheck.c is written against glibc's malloc hooks,
# which musl does not have. Without the flag the runtime does not build at all.
# The cost is that `tcc -b`, bounds checking, is not available on this machine.
#
# Usage: RACH2_ARCH=i686|x86_64 ./build_tcc.sh
#

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$SCRIPT_DIR/../.." && pwd)"
. "$REPO/mk/arch.sh"
cd "$SCRIPT_DIR"

VERSION="0.9.27"
TARBALL="tcc-${VERSION}.tar.bz2"
WORK="$ARCH_VENDOR/tcc"
SRC="$WORK/tcc-${VERSION}"
PREFIX="$MUSL_PREFIX"
WRAP="$SCRIPT_DIR/../musl/muslgcc"
# Out-of-tree staging dir, mirroring the source path vendor/tcc/ (mk/build.mk).
# It lives under build/ so a routine `make clean` never reaches it; only `make
# distclean` (which removes build/vendor) forces this rebuild.
STAGE="$ARCH_BUILD/vendor/tcc"
TOOLS="$STAGE/toolchain"

# tcc's name for the CPU, which is configure's --cpu, config.mak's ARCH and the
# prefix of lib/Makefile's runtime target; and the libgcc helpers the guest's
# libc references, which the runtime takes (see below).
case "$RACH2_ARCH" in
    i686)
        TCC_CPU=i386
        LIBGCC_HELPERS="_udivmoddi4.o _divmoddi4.o _mulxc3.o"
        ;;
    x86_64)
        TCC_CPU=x86_64
        LIBGCC_HELPERS="_mulsc3.o _muldc3.o _mulxc3.o"
        ;;
esac

# Pinned sha256 of the vendored tarball (published on savannah.gnu.org). The
# build refuses to proceed if the tarball does not match; the version is locked.
TARBALL_SHA256="de23af78fca90ce32dff2dd45b3432b2334740bb9bb7b05bf60fdbfc396ceb9c"

# Where the compiler will look when it runs on Rach2. These are guest paths.
GUEST_TCCDIR="/lib/tcc"
GUEST_INCLUDE="/lib/tcc/include:/include"
GUEST_LIBS="/lib"

sha256_of()
{
    make -s -C "$REPO" build/tools/verify/sha256 || return 1
    "$SCRIPT_DIR/../../build/tools/verify/sha256" "$1" | awk '{print $1}'
}

if [ ! -f "$PREFIX/lib/libc.a" ]; then
    echo "tcc: $PREFIX/lib/libc.a not found; run make musl in the top directory first" >&2
    exit 1
fi
if [ ! -f "$TARBALL" ]; then
    echo "tcc: $TARBALL missing (the vendored source)" >&2
    exit 1
fi

echo "tcc: verifying $TARBALL"
GOT="$(sha256_of "$TARBALL")"
if [ "$GOT" != "$TARBALL_SHA256" ]; then
    echo "tcc: sha256 mismatch for $TARBALL" >&2
    echo "  expected $TARBALL_SHA256" >&2
    echo "  got      $GOT" >&2
    exit 1
fi

echo "tcc: extracting into $WORK"
rm -rf "$SRC"
mkdir -p "$WORK"
tar xf "$TARBALL" -C "$WORK"

# The changes this project makes to somebody else's compiler: it refuses the
# output kinds this system cannot run; on x86-64 it links a static executable
# whose PLT and GOT TinyCC 0.9.27 leaves unfilled; and so that it builds the
# kernel on the machine, it assembles 16-bit and 32-bit code and the 64-bit
# system instructions, links by a GNU ld script, knows GCC's name for
# offsetof, and gives the x86-64 runtime's va_arg no call of abort().
# Patches applied here rather than an edited tree, so the vendored source stays
# as upstream shipped it and the diff a reader wants is a file in this
# repository. A patch that does not apply is a build that stops: silently
# shipping an unpatched compiler would ship one that accepts `-run` and dies
# strangely, or one whose x86-64 programs fault before main.
echo "tcc: applying the output policy, the x86-64 static link and the kernel's needs"
for p in rach2-output-policy.patch rach2-x86_64-static-link.patch \
         rach2-x86-asm.patch rach2-linker-script.patch \
         rach2-builtin-offsetof.patch rach2-va-list-trap.patch; do
    if ! (cd "$SRC" && patch -p1 --batch --forward < "$SCRIPT_DIR/$p" >/dev/null); then
        echo "tcc: $p did not apply to $SRC" >&2
        exit 1
    fi
done
if ! grep -q "sym->st_shndx != SHN_UNDEF))) {" "$SRC/tccelf.c" ||
   ! grep -q "so it is filled before" "$SRC/tccelf.c"; then
    echo "tcc: the x86-64 static link is not in the source it is about to build" >&2
    exit 1
fi
if ! grep -q "must be linked with -static" "$SRC/tcc.c" ||
   ! grep -q "must be an ELF" "$SRC/tcc.c"; then
    echo "tcc: the output policy is not in the source it is about to build" >&2
    exit 1
fi
if ! grep -q "s1->seg_size = 64;" "$SRC/tccasm.c" ||
   ! grep -q "^static void asm_modrm16" "$SRC/i386-asm.c" ||
   ! grep -q "R_X86_64_PC16" "$SRC/x86_64-link.c" ||
   ! grep -q "^static int ld_layout" "$SRC/tccelf.c" ||
   ! grep -q '"__builtin_offsetof(type, field)"' "$SRC/libtcc.c" ||
   ! grep -q "__builtin_trap();" "$SRC/lib/va_list.c"; then
    echo "tcc: the kernel's assembler, linker or runtime changes are not in the source it is about to build" >&2
    exit 1
fi

# The toolchain directory the cross prefix names. Three one-line scripts, so
# that ${prefix}gcc, ${prefix}ar and ${prefix}strip all resolve to the tools
# this project already uses.
echo "tcc: writing the cross toolchain to $TOOLS"
rm -rf "$TOOLS"
mkdir -p "$TOOLS"
cat > "$TOOLS/rach2-gcc" <<EOF
#!/bin/sh
exec "$WRAP" "\$@"
EOF
cat > "$TOOLS/rach2-ar" <<EOF
#!/bin/sh
exec ${CROSS}ar "\$@"
EOF
cat > "$TOOLS/rach2-strip" <<EOF
#!/bin/sh
exec ${CROSS}strip "\$@"
EOF
chmod 0755 "$TOOLS/rach2-gcc" "$TOOLS/rach2-ar" "$TOOLS/rach2-strip"

echo "tcc: configuring for $TCC_CPU, with the guest's own paths"
(
    cd "$SRC"
    ./configure \
        --cpu="$TCC_CPU" \
        --config-musl \
        --cross-prefix="$TOOLS/rach2-" \
        --tccdir="$GUEST_TCCDIR" \
        --sysincludepaths="$GUEST_INCLUDE" \
        --libpaths="$GUEST_LIBS" \
        --crtprefix="$GUEST_LIBS" \
        --extra-cflags="-O2 -Wno-error" >/dev/null
)

# On a Mac, configure has just written TARGETOS=Darwin and CONFIG_OSX=yes into
# config.mak, and the Makefile turns those into -DTCC_TARGET_MACHO and a target
# named i386-osx or x86_64-osx. Both are corrected in the file rather than only
# on the make command line, so that what config.mak says is what the build does,
# and so the libtcc1 target below keeps the name this script uses.
sed -e 's/^TARGETOS=.*/TARGETOS=Linux/' -e '/^CONFIG_OSX=/d' \
    "$SRC/config.mak" > "$SRC/config.mak.rach2"
mv "$SRC/config.mak.rach2" "$SRC/config.mak"

# What configure decided, asserted rather than assumed. ARCH is the target this
# build produces code for; the guest paths are the ones the compiler will look
# in when it runs on the machine; no CONFIG_OSX is what keeps the Makefile from
# defining TCC_TARGET_MACHO.
if ! grep -q "^ARCH=$TCC_CPU\$" "$SRC/config.mak"; then
    echo "tcc: configure did not select $TCC_CPU; config.mak says:" >&2
    grep '^ARCH=' "$SRC/config.mak" >&2 || true
    exit 1
fi
if grep -q '^CONFIG_OSX=' "$SRC/config.mak"; then
    echo "tcc: an OSX target survived; the build would emit Mach-O" >&2
    exit 1
fi
if ! grep -q 'CONFIG_TCCDIR "/lib/tcc"' "$SRC/config.h" ||
   ! grep -q 'CONFIG_TCC_SYSINCLUDEPATHS=.*/lib/tcc/include:/include' "$SRC/config.mak" ||
   ! grep -q 'CONFIG_TCC_CRTPREFIX=.*/lib' "$SRC/config.mak"; then
    echo "tcc: the guest's paths did not reach the build" >&2
    exit 1
fi

echo "tcc: building the compiler"
make -C "$SRC" \
    TARGETOS=Linux CONFIG_OSX= tcc >/dev/null

# The runtime, built with the cross gcc rather than by running the compiler that
# was just built. lib/Makefile honors this knob; the target name is the ARCH
# asserted above.
echo "tcc: building libtcc1.a with the cross gcc"
make -C "$SRC" TARGETOS=Linux CONFIG_OSX= "$TCC_CPU-libtcc1-usegcc=yes" libtcc1.a >/dev/null

# The runtime has to answer what the guest's libc ASKS FOR, and the guest's libc
# was compiled by gcc, which emits calls to helpers that live in libgcc. On
# i386, musl's vfprintf wants __udivmoddi4, __secs_to_tm wants __divmoddi4,
# cpowl wants __mulxc3. On x86-64 a 64-bit division is one instruction and
# needs no helper, and the complex powers want the multiplications of all
# three widths: cpowf __mulsc3, cpow __muldc3, cpowl __mulxc3. tcc's own
# runtime defines __udivmoddi4 only as a file-local symbol and the others not
# at all, because tcc's code generator never emits those calls; on the host
# the muslgcc wrapper appends libgcc and the question never comes up. In the
# guest there is no libgcc, so the objects are taken from the same compiler
# that built musl and added to the runtime archive.
#
# The list is written out rather than computed, and then CHECKED: if musl ever
# references another, the assertion below says so instead of a student meeting
# an undefined symbol at their first link.
echo "tcc: adding the libgcc helpers musl references to libtcc1.a"
LIBGCC="$("${CROSS}gcc" $MFLAG -print-libgcc-file-name)"
(
    cd "$SRC"
    rm -rf rach2-libgcc
    mkdir rach2-libgcc
    cd rach2-libgcc
    # shellcheck disable=SC2086 # the helpers are a list of words
    "${CROSS}ar" x "$LIBGCC" $LIBGCC_HELPERS
    # shellcheck disable=SC2086
    "${CROSS}ar" rcs ../libtcc1.a $LIBGCC_HELPERS
)

# On x86-64 the kernel links the runtime's va_list.o, whose __va_start and
# __va_arg every variadic function compiled by tcc calls. A kernel keeps out
# of the SSE registers, which hold the interrupted program's values, and out of
# the 128 bytes below the stack pointer, which an interrupt in the kernel
# overwrites; GCC at -O2 copies __va_start's struct through %xmm0 and may keep
# a leaf's locals below the stack pointer. So the object is built again without
# either, the same code for a program and a kernel.
if [ "$RACH2_ARCH" = x86_64 ]; then
    echo "tcc: building va_list.o for a kernel as well as a program"
    (
        cd "$SRC/lib"
        "$TOOLS/rach2-gcc" -O2 -mno-sse -mno-red-zone -c -o va_list.o va_list.c
        "${CROSS}ar" rcs ../libtcc1.a va_list.o
    )
    if "${CROSS}objdump" -d "$SRC/lib/va_list.o" | grep -q "xmm"; then
        echo "tcc: va_list.o uses an SSE register" >&2
        exit 1
    fi
fi

mkdir -p "$STAGE" "$STAGE/include"
echo "tcc: staging to $STAGE"
"${CROSS}strip" "$SRC/tcc" -o "$STAGE/tcc"
cp "$SRC/libtcc1.a" "$STAGE/libtcc1.a"
# tcc's own headers, which must be found BEFORE musl's: musl expects the
# compiler to provide stdarg.h and friends, and tcc with musl's copies ahead of
# its own is a known way to get a crash instead of an error.
for h in float.h stdarg.h stdbool.h stddef.h varargs.h; do
    cp "$SRC/include/$h" "$STAGE/include/$h"
done

# Nothing the guest's libc asks for may be left for the guest to discover. The
# question is asked of the artifacts that will actually be staged.
echo "tcc: checking the guest can link against what it will carry"
CHECK_LINK_NM="${CROSS}nm" sh "$SCRIPT_DIR/../../tools/inspect/check_link.sh" \
    "$PREFIX/lib/libc.a" "$STAGE/libtcc1.a" \
    "$PREFIX/lib/crt1.o" "$PREFIX/lib/crti.o" "$PREFIX/lib/crtn.o"

echo "tcc: built $VERSION ($(wc -c < "$STAGE/tcc") bytes, static, musl, $TCC_CPU, targeting $TCC_CPU)"
echo "tcc: rebuild the os-real image (make os-real-image) to install it"
