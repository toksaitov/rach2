#!/bin/sh
# default_probe.sh: build and run probes of the default suite for a mutation
# proof whose faults are in the kernel, as a catalogue of tests/proofs gives
# it in its build and its test.
#
# `--build DIR` builds the kernel and the test image of the architecture
# that ARCH names, i686 by default, and copies the kernel to DIR/kernel, the
# artifact the proof compares. With PROBE arguments it boots the test image
# with those probes alone, in one machine under the runner,
# tests/system/runner/runtests.c, prints the runner's output and then the
# boot's console, where a probe's FAIL lines are, and prints
# `default-probe: fail: PROBE did not pass` for each probe the runner did not
# pass. It exits with the runner's status. The runner's --timeout is one
# deadline for the whole run and --steptmo the window of one probe;
# musl/pwauthmusl waits out the credential lock's ten-second retry delay
# twice, so a probe may take tens of seconds to pass.
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
        make -s rach2 test-image build/tests/system/runtests "ARCH=$RACH2_ARCH" </dev/null
        cp "$tree/rach2/kernel" "$2/kernel"
        exit 0
        ;;
    '' | -*)
        echo 'usage: default_probe.sh --build DIR | PROBE...' >&2
        exit 2
        ;;
esac
log=$tree/images/default-test.log
probes=$*
out=
trap '[ -z "$out" ] || rm -f "$out"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
out=$(mktemp "${TMPDIR:-/tmp}/rach2-default-probe.XXXXXX")
rm -f "$log"
status=0
set --
for probe in $probes; do
    set -- "$@" --probe "$probe"
done
RACH2_ARCH=$RACH2_ARCH RACH2_QEMU=$ARCH_QEMU build/tests/system/runtests \
    --device rtl8139,netdev=net0 --kernel "$tree/rach2/kernel" --initrd "$tree/images/test.krffs" \
    --timeout 300 --steptmo 60 --log "$log" "$@" >"$out" 2>&1 </dev/null || status=$?
cat "$out"
[ ! -f "$log" ] || cat "$log"
for probe in $probes; do
    grep -Eq "^  $probe +.* PASS\$" "$out" ||
        echo "default-probe: fail: $probe did not pass"
done
exit "$status"
