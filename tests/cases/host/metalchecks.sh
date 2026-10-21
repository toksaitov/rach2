#!/bin/sh
#
# metalchecks.sh: the program of the host cases of the metal family,
# tests/cases/machine.metal.json, which boot nothing.
#
# `transport` runs tests/system/accept/transport_test.sh, the whole of
# metal_accept.sh against a stand-in for ssh in each of its cases of a fault.
# `password` runs tests/system/accept/password_test.sh, which requires
# su_command.sh to answer the password prompt without the password reaching
# any transcript. Each reports one assertion (tools/etude/Readme.md, Report
# records) and exits 0 when it passed and 1 when it failed.
#
# etude runs it in the case's workspace, with ETUDE_TREE naming the root of
# the tree, to which it changes first.
#
# Usage, in the root of the tree, or anywhere with ETUDE_TREE set to it:
#     sh tests/cases/host/metalchecks.sh transport|password
#

set -u

CASE=${1-}
[ "$#" -eq 1 ] || { echo "metalchecks: usage: metalchecks.sh CASE" >&2; exit 2; }
case "$CASE" in
    transport) ID=machine.metal.transport ASSERTION=transport
        SCRIPT=tests/system/accept/transport_test.sh ;;
    password) ID=machine.metal.password ASSERTION=password
        SCRIPT=tests/system/accept/password_test.sh ;;
    *) echo "metalchecks: no case $CASE" >&2; exit 2 ;;
esac
if [ -n "${ETUDE_CASE:-}" ] && [ "$ETUDE_CASE" != "$ID" ]; then
    echo "metalchecks: ETUDE_CASE is $ETUDE_CASE, and the argument names $ID" >&2
    exit 2
fi
cd "${ETUDE_TREE:-.}" || exit 2
[ -f "$SCRIPT" ] || { echo "metalchecks: run it in the root of the tree, or set ETUDE_TREE" >&2; exit 2; }

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
