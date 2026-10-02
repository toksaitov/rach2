# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# brotli: the libraries libbrotlicommon, libbrotlidec and libbrotlienc, which
# Chawan links to read compressed pages, and the brotli program, with the
# headers and pkg-config files. Upstream builds with CMake, which the build
# host need not have, so the recipe compiles the C sources itself into the
# libraries CMake makes, with their soname, .1, and version, 1.2.0.
VERSION=1.2.0
SOURCE_DIR=google-brotli-028fb5a

# brotli_lib NAME DIR LIB...: compile c/DIR into libNAME, linked with LIB.
brotli_lib()
{
    name=$1
    dir=$2
    shift 2
    upper=$(echo "$name" | tr '[:lower:]' '[:upper:]')
    for c in "$SRC/c/$dir"/*.c; do
        "$TARGET-gcc" -O2 -fPIC -I"$SRC/c/include" -DBROTLI_SHARED_COMPILATION \
            -D"${upper}_SHARED_COMPILATION" -c "$c" -o "$dir-$(basename "$c" .c).o"
    done
    "$TARGET-gcc" -shared -Wl,-soname,"lib$name.so.1" -o "lib$name.so.$VERSION" \
        "$dir"-*.o -L. "$@"
    ln -sf "lib$name.so.$VERSION" "lib$name.so.1"
    ln -sf "lib$name.so.1" "lib$name.so"
}

port_build()
{
    brotli_lib brotlicommon common -lm
    brotli_lib brotlidec dec -lbrotlicommon
    brotli_lib brotlienc enc -lbrotlicommon -lm
    "$TARGET-gcc" -O2 -I"$SRC/c/include" "$SRC/c/tools/brotli.c" -o brotli -L. \
        -lbrotlienc -lbrotlidec -lbrotlicommon -lm
}

port_install()
{
    mkdir -p "$DEST/usr/bin" "$DEST/usr/include" "$DEST/usr/lib/pkgconfig" \
        "$DEST/usr/share/man/man1"
    cp -P libbrotli*.so* "$DEST/usr/lib/"
    cp -R "$SRC/c/include/brotli" "$DEST/usr/include/"
    install -m 755 brotli "$DEST/usr/bin/brotli"
    install -m 644 "$SRC/docs/brotli.1" "$DEST/usr/share/man/man1/brotli.1"
    for pc in libbrotlicommon libbrotlidec libbrotlienc; do
        sed -e 's|@prefix@|/usr|; s|@exec_prefix@|/usr|; s|@libdir@|/usr/lib|' \
            -e "s|@includedir@|/usr/include|; s|@PACKAGE_VERSION@|$VERSION|; s|@libm@|-lm|" \
            "$SRC/scripts/$pc.pc.in" >"$DEST/usr/lib/pkgconfig/$pc.pc"
    done
}
