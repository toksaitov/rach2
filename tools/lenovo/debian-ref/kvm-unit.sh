#!/bin/sh
# shellcheck disable=SC2086,SC2046 # no path or word list here has a space
# kvm-unit.sh [ROUNDS]: kvm-unit-tests at the pinned commit 5a221342, built
# for x86-64, and its vmexit group run ROUNDS times (3 by default) under
# Debian's QEMU with KVM: the cost of each kind of exit, in guest TSC cycles.
# The rehearsal runs the same group under TCG. Output in $OUT/kvm-unit.
set -eu
. /root/ref80/lib.sh
need qemu-system-x86_64 make gcc
O=$OUT/kvm-unit; K=$S80/src/kvm-unit-tests; mkdir -p $O
N=${1:-3}
ACCEL=kvm; [ $REHEARSE = 1 ] && ACCEL=tcg
cd $K
say "kvm-unit: build"
[ -f x86/vmexit.flat ] || { ./configure > $O/configure.log 2>&1 && make -j"$(nproc)" > $O/build.log 2>&1; }
ls -l x86/vmexit.flat > $O/build-result.txt
qemu-system-x86_64 --version > $O/qemu-version.txt
i=1
while [ $i -le "$N" ]; do
	say "kvm-unit: vmexit round $i, accel $ACCEL"
	rm -rf logs
	ACCEL=$ACCEL QEMU=/usr/bin/qemu-system-x86_64 ./run_tests.sh -g vmexit -v > $O/round$i.txt 2>&1 || true
	mkdir -p $O/round$i-logs; cp logs/*.log $O/round$i-logs/ 2>/dev/null || true
	i=$((i + 1))
done
say "kvm-unit: summary"
grep -h -E '^(PASS|FAIL|SKIP)' $O/round*.txt | sort | uniq -c
