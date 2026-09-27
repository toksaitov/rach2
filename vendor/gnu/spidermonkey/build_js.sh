#!/usr/bin/env sh

#
# build_js.sh: the SpiderMonkey shell of Firefox 140.16.0esr, for Rach2.
#
# Usage: build_js.sh [STEP...]
#
# Builds js, the shell of SpiderMonkey, Firefox's JavaScript engine, from
# Firefox's source for i686-linux-gnu with its JIT, and installs it under the
# prefix /usr into build/vendor/gnu/js, the staging root. The build follows
# Mozilla's instructions for SpiderMonkey: mach with a mozconfig for the js
# project, cross-compiled with --target, here by the compilers `make
# gnu-toolchain` built, so that the shell links against their glibc. The
# steps are:
#
#   rust      install Rust 1.86.0, the release Firefox 140 is built with, for
#             this host and for i686, from the pinned components into
#             build/vendor/gnu/rust
#   cbindgen  build cbindgen 0.28.0, the release Firefox 140's automation
#             uses, from its crate; configure requires it, though the shell
#             does not use it
#   source    unpack Firefox's source and apply python-3.14.patch, which lets
#             mach run with Python 3.14
#   build     configure and build the shell with mach, and install it
#   check     run a script with the shell through the sysroot's loader, and
#             print what the shell reports about its JIT
#
# With no STEP all five run from nothing, and naming steps runs only those
# over what an earlier run left. Each step writes its output to a log in
# build/vendor/gnu/log. cargo fetches cbindgen's dependencies from crates.io
# and checks each against the digest in the crate's Cargo.lock; Firefox's
# own crates are in its source. mach and cargo run GNU_JOBS jobs, or one for
# every processor when GNU_JOBS is not set, under make as well, since mach
# starts its own make.
#

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
GNU="$(cd "$DIR/.." && pwd)"
REPO="$(cd "$GNU/../.." && pwd)"
OUT="$REPO/build/vendor/gnu"
SRC="$OUT/src/js"
WORK="$OUT/work"
LOG="$OUT/log"
CROSS="$OUT/cross"
SYSROOT="$OUT/sysroot"
RUST="$OUT/rust"
ROOT="$OUT/js"
TARGET=i686-linux-gnu
PATH="$RUST/bin:$CROSS/bin:$PATH"
CARGO_HOME="$RUST/cargo-home"
CARGO_BUILD_JOBS="${GNU_JOBS:-$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)}"
export PATH CARGO_HOME CARGO_BUILD_JOBS
JOBS="-j$CARGO_BUILD_JOBS"

if [ ! -x "$CROSS/bin/$TARGET-gcc" ]; then
    echo "gnu: js: no cross compiler; run make gnu-toolchain first" >&2
    exit 1
fi
for tool in cc c++ make m4 patch python3 xz; do
    if ! command -v "$tool" >/dev/null 2>&1; then
        echo "gnu: js: $tool not found; the GNU layer builds on a Linux host with the tools vendor/gnu/Readme.md lists" >&2
        exit 1
    fi
done

# extract NAME DEST: unpack a checked tarball afresh into an empty DEST and
# print the tree it made there.
extract()
{
    tarball="$("$GNU/fetch.sh" --path "$1")"
    rm -rf "$2"
    mkdir -p "$2"
    tar -xf "$tarball" -C "$2"
    set -- "$2"/*
    echo "$1"
}

step_rust()
{
    rm -rf "$RUST"
    for c in rustc cargo rust-std rust-std-i686; do
        t="$(extract "$c" "$WORK/js-rust")"
        "$t/install.sh" --prefix="$RUST" --disable-ldconfig
    done
    rm -rf "$WORK/js-rust"
    rustc --version
    cargo --version
}

step_cbindgen()
{
    t="$(extract cbindgen "$WORK/js-cbindgen")"
    cargo build --release --locked --manifest-path "$t/Cargo.toml" \
        --target-dir "$WORK/js-cbindgen/target"
    cp "$WORK/js-cbindgen/target/release/cbindgen" "$RUST/bin/cbindgen"
    cbindgen --version
}

step_source()
{
    tree="$(extract firefox "$SRC")"
    patch -d "$tree" -p1 -F 0 --forward < "$DIR/python-3.14.patch"
}

# The mozconfig is the one Mozilla's instructions give for an optimized
# shell, with the target and the compilers given, and with
# --disable-bootstrap, which keeps mach from downloading toolchains of its
# own. SSE2 arithmetic is what Mozilla's automation builds 32-bit x86 with,
# so that the interpreter and the JIT round alike. SpiderMonkey alone takes
# zlib from the system by default, and the sysroot has none, so the copy in
# Firefox's tree is built in. configure requires llvm-objdump, which only the
# browser's packaging runs, so LLVM_OBJDUMP is the cross toolchain's objdump.
step_build()
{
    set -- "$SRC"/*
    tree="$1"
    obj="$WORK/js-obj"
    rm -rf "$obj" "$WORK/js-state"
    mkdir -p "$obj" "$WORK/js-state"
    cat > "$WORK/js-mozconfig" <<EOF
ac_add_options --enable-project=js
ac_add_options --target=$TARGET
ac_add_options --prefix=/usr
ac_add_options --enable-optimize
ac_add_options --disable-debug
ac_add_options --disable-tests
ac_add_options --disable-jemalloc
ac_add_options --without-system-zlib
ac_add_options --disable-bootstrap
mk_add_options MOZ_OBJDIR=$obj
EOF
    cd "$tree"
    env MOZCONFIG="$WORK/js-mozconfig" MOZBUILD_STATE_PATH="$WORK/js-state" \
        MACH_BUILD_PYTHON_NATIVE_PACKAGE_SOURCE=none \
        CC="$TARGET-gcc" CXX="$TARGET-g++" HOST_CC=cc HOST_CXX=c++ \
        AR="$TARGET-ar" NM="$TARGET-nm" RANLIB="$TARGET-ranlib" \
        STRIP="$TARGET-strip" LLVM_OBJDUMP="$CROSS/bin/$TARGET-objdump" \
        CFLAGS="-msse -msse2 -mfpmath=sse" CXXFLAGS="-msse -msse2 -mfpmath=sse" \
        RUSTC="$RUST/bin/rustc" CARGO="$RUST/bin/cargo" \
        CBINDGEN="$RUST/bin/cbindgen" \
        python3 ./mach --no-interactive build "$JOBS"
    rm -rf "$ROOT"
    mkdir -p "$ROOT/usr/bin"
    cp -L "$obj/dist/bin/js" "$ROOT/usr/bin/js"
    "$TARGET-strip" "$ROOT/usr/bin/js"
}

# The script calls a function in a loop until SpiderMonkey has compiled it,
# then asks the shell whether the function runs in JIT code and in Ion, the
# optimizing compiler. Ion compiles on the calling thread, so the compilation
# is done by the time the loop is. The library path gives the loader the
# sysroot's glibc and the cross toolchain's C++ runtime.
step_check()
{
    cat > "$WORK/js-check.js" <<'EOF'
function sum(n) {
    let s = 0;
    for (let i = 0; i < n; i++)
        s += i;
    return [s, inJit(), inIon()];
}
let r;
for (let k = 0; k < 5000; k++)
    r = sum(1000);
let o = getJitCompilerOptions();
print("sum(1000) = " + r[0]);
print("baseline.enable = " + o["baseline.enable"] + ", ion.enable = " + o["ion.enable"]);
print("inJit() = " + r[1] + ", inIon() = " + r[2]);
EOF
    "$SYSROOT/lib/ld-linux.so.2" \
        --library-path "$SYSROOT/lib:$SYSROOT/usr/lib:$CROSS/$TARGET/lib" \
        "$ROOT/usr/bin/js" --ion-offthread-compile=off "$WORK/js-check.js" > "$WORK/js-check.out"
    cat "$WORK/js-check.out"
    grep -q 'inJit() = true, inIon() = true' "$WORK/js-check.out"
}

if [ $# -eq 0 ]; then
    rm -rf "$SRC" "$WORK"/js-* "$ROOT" "$RUST"
    set -- rust cbindgen source build check
fi
mkdir -p "$LOG"
for step in "$@"; do
    case "$step" in
        rust|cbindgen|source|build|check) ;;
        *) echo "gnu: js: no step named $step" >&2; exit 2 ;;
    esac
    echo "gnu: js: $step"
    log="$LOG/js-$step.log"
    t0=$(date +%s)
    set +e
    ( set -e; "step_$step" ) > "$log" 2>&1
    status=$?
    set -e
    if [ $status -ne 0 ]; then
        tail -n 40 "$log" >&2
        echo "gnu: js: $step: failed; the log is $log" >&2
        exit 1
    fi
    if [ "$step" = check ]; then
        cat "$log"
    fi
    echo "gnu: js: $step: done in $(($(date +%s) - t0)) s"
done
