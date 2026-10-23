#!/usr/bin/env sh

#
# check_recovery.sh: read a kernel's disk authority off the built artifact.
#
# rach2/sys/block/recovery.c compiles one of two strings into the kernel:
# `rach2: disk authority: recovery` when it is built with -DRACH2_RECOVERY,
# whose disk nodes can be written, and `rach2: disk authority: ordinary`
# otherwise. This script searches the binary for them with binfind and
# requires the expected string present and the other absent, so a kernel
# built into the wrong tree fails before anything stages it.
#
# The string and the write handler are separate conditionals, so the check
# finds a build mistake and says nothing of a kernel edited afterwards.
# The rach2-recovery target runs it on both kernels, and `tests/run authority`
# boots them and requires a write by the ordinary one to fail. The authority
# itself is described in rach2/sys/block/Readme.md.
#
# Usage: check_recovery.sh <kernel> recovery|ordinary
#

set -eu
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
CHECK="$REPO/build/tools/inspect/check_recovery"
[ -x "$CHECK" ] || make -s -C "$REPO" build/tools/inspect/check_recovery
exec "$CHECK" "$@"
