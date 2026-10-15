# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# BIND 9's tools for asking a name server: dig, host, nslookup, delv and
# nsupdate, with the libraries they share, on libuv, Userspace RCU, OpenSSL
# and libcap; without DNS over HTTPS, the statistics channel's libraries,
# LMDB and GSSAPI. The server and its tools are built and left out of the
# root, which runs no name server. A library of the build links against
# another of its own, which the linker finds through -rpath-link. BIND would
# give OpenSSL its own allocator, and OpenSSL 4 runs its cleanup again from
# its library's destructor, after BIND has freed the allocator's pool, which
# aborts every program at its exit before it flushes its output; so the
# recipe builds a copy of the tree with that call taken out, and OpenSSL
# keeps its own allocator.
VERSION=9.20.4
SOURCE_DIR=bind-9.20.4
DEPENDS="libuv liburcu openssl libcap"
CONFIGURE_ARGS="--disable-doh --without-libxml2 --without-json-c --without-jemalloc \
--without-lmdb --without-gssapi --without-libidn2 --disable-static \
--without-readline"

port_build()
{
    l="$WORK/lib"
    cp -a "$SRC/." .
    sed -i 's/(void)CRYPTO_set_mem_functions(/0 \&\& CRYPTO_set_mem_functions(/' lib/isc/tls.c
    grep -q "0 && CRYPTO_set_mem_functions(" lib/isc/tls.c
    (
        SRC=.
        configure_autoconf "LDFLAGS=-Wl,-rpath-link=$l/isc/.libs:$l/dns/.libs:$l/ns/.libs:$l/isccc/.libs:$l/isccfg/.libs"
    )
    make
}

port_install()
{
    make install DESTDIR="$DEST"
    rm -rf "${DEST:?}/usr/sbin" "${DEST:?}/etc" "${DEST:?}/var" "${DEST:?}/run"
    for page in "$DEST"/usr/share/man/man[158]/*; do
        case ${page##*/} in
            dig.1 | host.1 | nslookup.1 | delv.1 | nsupdate.1) ;;
            *) rm -f "$page" ;;
        esac
    done
    for tool in "$DEST"/usr/bin/*; do
        case ${tool##*/} in
            dig | host | nslookup | delv | nsupdate) ;;
            *) rm -f "$tool" ;;
        esac
    done
}
