# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# lsof, which lists the files processes have open from /proc. Upstream signs
# nothing, so the source is Debian's: its tarball has the files of the
# release's tag, without the dialects of other systems and without the
# generated autotools files, so lsof is built with its own Configure script,
# in a copy of the source. Configure asks the C compiler which C library it
# has and reads the kernel's version from the headers; for a cross build the
# compiler, the archiver and the headers are named, and the build host, the
# user and the date are left out of the version lsof -v prints, as Debian
# leaves them out. The manual page includes the file `version` with roff's
# .so, which the page is installed with in its place, as soelim would.
VERSION=4.99.4

port_build()
{
    cp -a "$SRC/." .
    LSOF_CC="$TARGET-gcc" LINUX_CONF_CC=cc LINUX_CLIB=-DGLIBCV=244 \
        LSOF_AR="$TARGET-ar cr" LSOF_RANLIB="$TARGET-ranlib" \
        LSOF_INCLUDE="$ROOT/usr/include" LINUX_INCL="$ROOT/usr/include" \
        LSOF_CCDATE=none LSOF_HOST=none LSOF_LOGNAME=none LSOF_SYSINFO=none LSOF_USER=none \
        ./Configure -n linux
    make
}

port_install()
{
    mkdir -p "$DEST/usr/bin" "$DEST/usr/share/man/man8"
    cp lsof "$DEST/usr/bin/lsof"
    sed -e '/^\.so \.\/version$/r version' -e '/^\.so \.\/version$/d' Lsof.8 \
        >"$DEST/usr/share/man/man8/lsof.8"
}
