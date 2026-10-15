# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# The runtime of the GNU system, which every other port depends on. It is
# installed from the toolchain rather than built: the whole sysroot that
# build_toolchain.sh made, which is glibc with its loader, LOADER, at
# /lib/ld-linux.so.2 on i686 and /lib64/ld-linux-x86-64.so.2 on x86-64, and
# its shared libraries in /lib, their headers and static libraries in /usr,
# and libxcrypt's libcrypt.so.1 in /usr/lib, so that the root is also the
# sysroot the ports compile against. To it are added GCC's shared libraries,
# libgcc_s.so.1 in /lib and libstdc++.so.6 in /usr/lib, and a locale archive
# with C.UTF-8 and en_US.UTF-8. The archive is written by the target's own
# localedef, run here through the staged loader, since its format is the one
# the glibc that reads it expects.
VERSION=2.44
SOURCE=

port_build()
{
    :
}

port_install()
{
    cp -a "$SYSROOT/." "$DEST/"
    cp -L "$CROSS/$TARGET/lib/libgcc_s.so.1" "$DEST/lib/"
    cp -L "$CROSS/$TARGET/lib/libstdc++.so.6" "$DEST/usr/lib/"
    mkdir -p "$DEST/usr/lib/locale"
    for locale in C.UTF-8 en_US.UTF-8; do
        I18NPATH="$DEST/usr/share/i18n" "$DEST$LOADER" \
            --library-path "$DEST/lib:$DEST/usr/lib" "$DEST/usr/bin/localedef" \
            --prefix="$DEST" -i "${locale%.UTF-8}" -f UTF-8 "$locale"
    done
}
