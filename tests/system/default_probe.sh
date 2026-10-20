#!/bin/sh
# default_probe.sh: build and run probes of the default suite for a mutation
# proof whose faults are in the kernel, as a catalogue of tests/proofs gives
# it in its build and its test.
#
# `--build DIR` builds the kernel and the test image of the architecture
# that ARCH names, i686 by default, and copies the kernel to DIR/kernel, the
# artifact the proof compares. With PROBE arguments it boots the test image
# with those probes alone, as `make run-default` does with DEFAULT_PROBES,
# prints the runner's output and then the boot's console, where a probe's
# FAIL lines are, and prints `default-probe: fail: PROBE did not pass` for
# each probe the runner did not pass. It exits with the runner's status.
#
# Usage: sh tests/system/default_probe.sh --build DIR
#        sh tests/system/default_probe.sh PROBE...
set -eu
repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
RACH2_ARCH=${RACH2_ARCH:-${ARCH:-i686}}
REPO=$repo
. "$repo/mk/arch.sh"
tree=build$ARCH_SUFFIX
case ${1:-} in
    --build)
        [ "$#" -eq 2 ] || { echo 'usage: default_probe.sh --build DIR' >&2; exit 2; }
        make -s rach2 test-image "ARCH=$RACH2_ARCH" </dev/null
        cp "$tree/rach2/kernel" "$2/kernel"
        exit 0
        ;;
    '' | -*)
        echo 'usage: default_probe.sh --build DIR | PROBE...' >&2
        exit 2
        ;;
esac
log=$tree/images/default-test.log
out=
trap '[ -z "$out" ] || rm -f "$out"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
out=$(mktemp "${TMPDIR:-/tmp}/rach2-default-probe.XXXXXX")
rm -f "$log"
status=0
make -s run-default "ARCH=$RACH2_ARCH" "DEFAULT_PROBES=$*" >"$out" 2>&1 </dev/null || status=$?
cat "$out"
[ ! -f "$log" ] || cat "$log"
for probe in "$@"; do
    grep -Eq "^  $probe +.* PASS\$" "$out" ||
        echo "default-probe: fail: $probe did not pass"
done
exit "$status"
