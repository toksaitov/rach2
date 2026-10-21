#!/bin/sh
#
# authority.sh: the program of the host cases of
# tests/cases/guest.authority.json, which read a kernel's disk authority off
# the built artifact.
#
# The argument names the kernel and the authority it must have: `ordinary`,
# the everyday kernel, rach2/kernel, or `recovery`, the recovery kernel,
# rach2-recovery/kernel, each in the build tree of the architecture ARCH
# names, i686 unless set. tools/inspect/check_recovery.sh requires the
# kernel to have the banner of its authority and not the other's. The
# program writes the case's report records on its standard output
# (tools/etude/Readme.md, Report records), one assertion, `banner`, and exits
# 0 when it passed and 1 when it failed.
#
# etude runs it in the case's workspace with ETUDE_TREE naming the root of
# the tree, to which it changes first.
#
# Usage, in the root of the tree, or anywhere with ETUDE_TREE set to it:
#     sh tests/cases/host/authority.sh ordinary|recovery
#

set -u

AUTH=${1-}
case "$AUTH" in
    ordinary) COMPONENT=rach2 ;;
    recovery) COMPONENT=rach2-recovery ;;
    *) echo "authority: usage: authority.sh ordinary|recovery" >&2; exit 2 ;;
esac
if [ -n "${ETUDE_TREE-}" ]; then
    cd "$ETUDE_TREE" || exit 2
fi
[ -f tools/inspect/check_recovery.sh ] || {
    echo "authority: run it in the root of the tree" >&2
    exit 2
}
case "${ARCH:-i686}" in
    i686) KERNEL=build/$COMPONENT/kernel ;;
    x86_64) KERNEL=build/x86_64/$COMPONENT/kernel ;;
    *) echo "authority: no architecture $ARCH" >&2; exit 2 ;;
esac
ID=${ETUDE_CASE:-guest.authority.$AUTH-kernel}

printf '@etude 1 begin %s\n' "$ID"
if out=$(sh tools/inspect/check_recovery.sh "$KERNEL" "$AUTH" 2>&1); then
    printf '%s\n' "$out"
    printf '@etude 1 assert banner pass\n'
    status=0
else
    printf '%s\n' "$out"
    detail=$(printf '%s' "$out" | tail -n 1 | tr -c ' -~' '?' | sed 's/^ *//' | cut -c1-160 | sed 's/ *$//')
    [ -n "$detail" ] || detail="check_recovery.sh failed and printed nothing"
    printf '@etude 1 assert banner fail %s\n' "$detail"
    status=1
fi
printf '@etude 1 end 1\n'
exit "$status"
