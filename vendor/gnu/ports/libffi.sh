# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# libffi, which calls a C function whose arguments are known only when the
# program runs: Python's ctypes is built on it.
VERSION=3.8.0
