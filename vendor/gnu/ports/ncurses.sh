# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# ncurses with wide characters, its default, its shared and static libraries,
# and the terminfo database. tic compiles the database while ncurses
# installs, and a cross build cannot run the target's tic, so a tic for the
# build host is built first from the same sources and given to the install
# as TIC_PATH. The C++ binding is left out, and the programs are installed
# without the build host's strip. The libraries have only their wide names,
# libncursesw and the rest, so a link with -lncurses, -lcurses, -lform,
# -lpanel or -lmenu, which configure scripts and makefiles still ask for,
# finds a linker script of that name that reads the wide library, as Linux
# From Scratch installs them.
VERSION=6.6
CONFIGURE_ARGS="--with-shared --without-debug --without-ada --without-cxx-binding --disable-stripping --with-manpage-format=normal --enable-pc-files --with-pkg-config-libdir=/usr/lib/pkgconfig"

port_build()
{
    mkdir host
    (
        cd host || exit
        CC=cc "$SRC/configure" --without-shared --without-debug --without-ada \
            --without-cxx-binding
        make -C include
        make -C progs tic
    )
    configure_autoconf
    make
}

port_install()
{
    make install DESTDIR="$DEST" TIC_PATH="$WORK/host/progs/tic"
    for lib in ncurses form panel menu; do
        echo "INPUT(-l${lib}w)" >"$DEST/usr/lib/lib$lib.so"
    done
    echo "INPUT(-lncursesw)" >"$DEST/usr/lib/libcurses.so"
}
