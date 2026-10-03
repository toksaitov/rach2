# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# libuv, the event loop BIND is built on, built with CMake, since its
# release has no configure script, and without its tests.
VERSION=1.52.1
SOURCE_DIR=libuv-v1.52.1
DEPENDS="cmake"

port_build()
{
    configure_cmake -DLIBUV_BUILD_TESTS=OFF -DLIBUV_BUILD_BENCH=OFF
    make
}
