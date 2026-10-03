# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# CMake, the build system generator, with ccmake on the root's ncurses and
# the libraries its release carries, curl among them over the root's
# OpenSSL. CMake is built with CMake: the recipe bootstraps one for the build
# host into build/vendor/gnu/host, which the ports built with CMake also run,
# and that one builds the root's for the target.
VERSION=4.3.4
DEPENDS="openssl ncurses"

port_build()
{
    mkdir -p host
    (
        cd host || exit 1
        "$SRC/bootstrap" --prefix="$OUT/host" --parallel="$(getconf _NPROCESSORS_ONLN)" \
            -- -DCMAKE_USE_OPENSSL=OFF -DBUILD_TESTING=OFF >/dev/null
        make >/dev/null
        make install >/dev/null
    )
    configure_cmake -DBUILD_TESTING=OFF -DCMAKE_USE_OPENSSL=ON -DBUILD_CursesDialog=ON \
        -DCMAKE_DOC_DIR=share/doc/cmake -DCMAKE_MAN_DIR=share/man
    make
}
