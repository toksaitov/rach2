#!/bin/sh
#
# usbchecks.sh: the program of the host cases of the USB families,
# tests/cases/machine.usb.json and tests/cases/machine.usbdisk.json, which
# boot nothing.
#
# `latency-checker` runs tests/system/usb_latency_test.sh, which requires
# checkers/usblatency.awk, the judge of the keystroke cases' records, to
# accept and refuse the records of a host test built with the kernel's
# recorder. `vfs-unmount` runs tests/system/vfs_unmount_test.sh, the unmount
# functions of rach2/sys/fs/vfs.c that a pulled stick's mounts go through,
# compiled with counting substitutes under the sanitizers. Each reports one
# assertion (tools/etude/Readme.md, Report records) and exits 0 when it
# passed and 1 when it failed.
#
# etude runs it in the case's workspace, with ETUDE_TREE naming the root of
# the tree, to which it changes first.
#
# Usage, in the root of the tree, or anywhere with ETUDE_TREE set to it:
#     sh tests/cases/host/usbchecks.sh latency-checker|vfs-unmount
#

set -u

CASE=${1-}
[ "$#" -eq 1 ] || { echo "usbchecks: usage: usbchecks.sh CASE" >&2; exit 2; }
case "$CASE" in
    latency-checker) ID=machine.usb.latency-checker ASSERTION=checker
        SCRIPT=tests/system/usb_latency_test.sh ;;
    vfs-unmount) ID=machine.usbdisk.vfs-unmount ASSERTION=unmount-cases
        SCRIPT=tests/system/vfs_unmount_test.sh ;;
    *) echo "usbchecks: no case $CASE" >&2; exit 2 ;;
esac
if [ -n "${ETUDE_CASE:-}" ] && [ "$ETUDE_CASE" != "$ID" ]; then
    echo "usbchecks: ETUDE_CASE is $ETUDE_CASE, and the argument names $ID" >&2
    exit 2
fi
cd "${ETUDE_TREE:-.}" || exit 2
[ -f "$SCRIPT" ] || { echo "usbchecks: run it in the root of the tree, or set ETUDE_TREE" >&2; exit 2; }

printf '@etude 1 begin %s\n' "$ID"
status=0
sh "$SCRIPT" || status=$?
if [ "$status" -eq 0 ]; then
    printf '@etude 1 assert %s pass\n' "$ASSERTION"
else
    printf '@etude 1 assert %s fail %s exited with status %s\n' "$ASSERTION" "$SCRIPT" "$status"
fi
printf '@etude 1 end 1\n'
[ "$status" -eq 0 ] || exit 1
