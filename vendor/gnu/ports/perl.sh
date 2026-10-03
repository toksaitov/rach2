# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# Perl. Its Configure is its own: it compiles test programs and runs them to
# learn what the system has, and the build then runs the miniperl and perl it
# has linked. The build host runs i386 and x86-64 programs, so Perl is
# configured as a native build whose programs run on the host. While Perl
# builds, gcc on the path is a wrapper around the cross compiler that links
# each program with the root's loader as its interpreter, and LD_LIBRARY_PATH
# points that loader at the root's libraries rather than the host's. Configure
# records the compiler as gcc, its name on the target. It also asks the
# compiler and the build host where headers and libraries are; config.sh gets
# the root's directories in place of its answers before the build, and the
# root's path is removed from the installed configuration. perl is then linked
# again without the wrapper's interpreter, so that it asks for the loader at
# its own path, LOADER, as the other programs do. It has interpreter threads,
# as Debian's perl has, so that `use threads` works.
VERSION=5.44.0

# perl_env COMMAND...: run a step of Perl's build with the wrapper and the
# root's libraries.
perl_env()
{
    PATH="$WORK/bin:$PATH" LD_LIBRARY_PATH="$ROOT/lib:$ROOT/usr/lib" "$@"
}

port_build()
{
    mkdir -p "$WORK/bin"
    cat >"$WORK/bin/gcc" <<EOF
#!/bin/sh
for a in "\$@"; do
    case "\$a" in
        -c | -E | -S | -shared)
            exec "$CROSS/bin/$TARGET-gcc" --sysroot="$ROOT" "\$@"
            ;;
    esac
done
if [ -n "\$PERL_FINAL" ]; then
    exec "$CROSS/bin/$TARGET-gcc" --sysroot="$ROOT" "\$@"
fi
exec "$CROSS/bin/$TARGET-gcc" --sysroot="$ROOT" "\$@" \\
    -Wl,--dynamic-linker="$ROOT$LOADER"
EOF
    chmod +x "$WORK/bin/gcc"
    cd "$SRC" || exit
    perl_env sh Configure -des -Dcc=gcc -Dprefix=/usr -Darchname="${TARGET%%-*}-linux" -Dusethreads \
        -Dlibpth="$ROOT/usr/lib $ROOT/lib" -Dplibpth="$ROOT/usr/lib $ROOT/lib" \
        -Dusrinc="$ROOT/usr/include" -Dlocincpth=' ' -Dloclibpth=' ' \
        -Dman1dir=/usr/share/man/man1 -Dman3dir=/usr/share/man/man3 \
        -Dpager='/usr/bin/less -R' -Dmyhostname=localhost \
        -Dcf_email=root@localhost -Dperladmin=root@localhost
    sed -e "s|^libpth=.*|libpth='$ROOT/usr/lib $ROOT/lib'|" \
        -e "s|^libspath=.*|libspath='$ROOT/usr/lib $ROOT/lib'|" \
        -e "s|^incpth=.*|incpth='$ROOT/usr/include'|" config.sh >config.sh.new
    mv config.sh.new config.sh
    perl_env sh Configure -S
    perl_env make
}

port_install()
{
    cd "$SRC" || exit
    perl_env make install DESTDIR="$DEST"
    grep -rlI "$ROOT" "$DEST/usr/lib/perl5" | while IFS= read -r f; do
        sed -i "s|$ROOT||g" "$f"
    done
    rm -f perl
    PERL_FINAL=1 perl_env make perl
    for f in perl "perl$VERSION"; do
        cp perl "$DEST/usr/bin/$f"
    done
}
