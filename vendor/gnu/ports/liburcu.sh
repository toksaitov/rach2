# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# Userspace RCU, the library of read-copy-update that BIND's tables use.
VERSION=0.15.7
SOURCE_DIR=userspace-rcu-0.15.7
CONFIGURE_ARGS="--disable-static"
