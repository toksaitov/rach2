#!/bin/sh
# shellcheck disable=SC2086,SC2046 # no path or word list here has a space
# selftests.sh [ROUNDS]: the KVM selftests of Linux 6.12.115, the running
# kernel's upstream release, built static as 81-6b builds them, and their
# timing cases run ROUNDS times (3 by default) with their default sizes.
# Without /dev/kvm (the rehearsal) each case reports its own skip. Output in
# $OUT/selftests.
set -eu
. /root/ref80/lib.sh
need make gcc rsync
O=$OUT/selftests; L=$S80/src/linux; T=$L/tools/testing/selftests/kvm; mkdir -p $O
N=${1:-3}
# The cases that print times: page faults served from user space, dirty
# logging, memory slots, page tables, access tracking, interprocessor
# interrupts.
CASES="demand_paging_test dirty_log_perf_test memslot_perf_test
memslot_modification_stress_test kvm_page_table_test access_tracking_perf_test
x86_64/xapic_ipi_test"
if [ ! -x $T/demand_paging_test ]; then
	say "selftests: headers"
	make -C $L headers > $O/headers.log 2>&1
	say "selftests: build"
	make -C $T -j"$(nproc)" EXTRA_CFLAGS=-static > $O/build.log 2>&1
fi
for c in $CASES; do
	echo "$c: $(file -b $T/$c | cut -d, -f1-2 | tr -d '\n') $(sha256sum $T/$c | cut -c1-16)"
done > $O/binaries.txt
i=1
while [ $i -le "$N" ]; do
	for c in $CASES; do
		n=$(echo $c | tr / -)
		say "selftests: round $i: $c"
		rc=0; ( cd $T && timeout 600 ./$c ) > $O/$n.$i.txt 2>&1 || rc=$?
		echo "exit $rc" >> $O/$n.$i.txt
	done
	i=$((i + 1))
done
say "selftests: exits"
grep -H '^exit' $O/*.txt
