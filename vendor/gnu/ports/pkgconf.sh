# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# pkgconf, which answers as pkg-config, as Debian's does. It serves the
# build too: the build host has no pkg-config, and a copy built for it is
# the $TARGET-pkg-config that configure looks for first on build_port.sh's
# path, reading only the root's files through the PKG_CONFIG_LIBDIR and
# PKG_CONFIG_SYSROOT_DIR that build_port.sh sets. Every port but the runtime
# is built after this one.
VERSION=2.5.1
CONFIGURE_ARGS="--with-system-libdir=/lib:/usr/lib --with-system-includedir=/usr/include"

port_build()
{
    mkdir host
    (
        cd host || exit
        CC=cc "$SRC/configure" --prefix="$OUT/host" --disable-shared
        make
        make install
    )
    ln -sf "$OUT/host/bin/pkgconf" "$WRAP/$TARGET-pkg-config"
    configure_autoconf
    make
}

port_install()
{
    make install DESTDIR="$DEST"
    ln -s pkgconf "$DEST/usr/bin/pkg-config"
}
