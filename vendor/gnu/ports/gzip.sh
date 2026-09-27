# shellcheck shell=sh disable=SC2034 # build_port.sh reads the variables
# GNU gzip, with gunzip, zcat and its other scripts. The scripts run gzip,
# grep and the rest by their bare names, so configure is told to start them
# with /bin/bash: under /bin/sh, BusyBox's shell, a bare name finds BusyBox's
# applet before it searches the path.
VERSION=1.15
CACHE="CONFIG_SHELL=/bin/bash"
