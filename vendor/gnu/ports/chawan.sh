# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# Chawan, cha, a web browser for the terminal with an HTML and CSS engine of
# its own, with flow, table and flex layout, and JavaScript in QuickJS, which
# its configuration turns on; HTTP and HTTPS through OpenSSL and brotli, SFTP
# through libssh2, Gopher and Gemini; and images as sixels or by the kitty
# protocol in a terminal that shows them. Each page loads in a process of its
# own under a seccomp filter. Chawan is written in Nim, so the recipe builds
# Nim 2.2.12 for the build host from its release, which has the compiler as
# C, and that Nim compiles Chawan to C for the target, which the cross
# compiler builds; chac, which compiles the startup script to QuickJS byte
# code, is built for the build host and runs there. Chawan takes a terminal
# it does not know for an XTerm, so the recipe adds the Rach2 console,
# TERM=rach2, to its table of terminals.
VERSION=0.5~dev1
SOURCE_DIR=chawan-0.5~dev1+git20260916+ds
DEPENDS="openssl brotli libssh2"

# chawan_make ARG...: run Chawan's Makefile with the host Nim and the target.
chawan_make()
{
    cross="$TARGET-gcc"
    FLAGS="--cpu:i386 --os:linux --gcc.exe:$cross --gcc.linkerexe:$cross"
    FLAGS="$FLAGS --i386.linux.gcc.exe:$cross --i386.linux.gcc.linkerexe:$cross"
    export FLAGS
    make -C "$SRC" NIM="$WORK/nim-2.2.12/bin/nim" CC="$cross" PKG_CONFIG="$TARGET-pkg-config" \
        PREFIX=/usr "$@"
}

# chawan_rach2: describe the Rach2 console in Chawan's table of terminals: it
# answers the device attributes and the cursor position, has the ANSI colors,
# and needs the right edge's column kept free, since a character there wraps
# at once and scrolls at the bottom right. It has no scroll region, alternate
# screen, line-drawing set, mouse, bracketed paste or title, which Chawan
# would use on an XTerm.
chawan_rach2()
{
    f="$SRC/src/local/term.nim"
    sed -i -e 's/^    ttPutty = "putty"$/&\n    ttRach2 = "rach2"/' \
        -e 's/^  ttPutty: XtermCompatible + TrueColorFlag,$/&\n  ttRach2: {tfMargin} + AnsiColorFlag,/' "$f"
    grep -q '^    ttRach2 = "rach2"$' "$f" && grep -q '^  ttRach2: {tfMargin} + AnsiColorFlag,$' "$f"
}

port_build()
{
    chawan_rach2 || return 1
    tar -xf "$("$DIR/fetch.sh" --path nim)" -C "$WORK"
    (
        cd "$WORK/nim-2.2.12" || exit 1
        CC=cc sh build.sh
    )
    chawan_make
}

port_install()
{
    chawan_make DESTDIR="$DEST" install
}
