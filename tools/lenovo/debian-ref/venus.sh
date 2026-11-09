#!/bin/sh
# shellcheck disable=SC2086,SC2046 # no path or word list here has a space
# venus.sh [CASE ARGS...]: the WebGPU-on-Venus trace. run2 step 10's guest
# under KVM with QEMU's display headless (egl-headless on the render node,
# nothing on the panel), risk 15's settings (intel_iommu=off on the host, as
# this boot has it, and shmem_enabled at never), virglrenderer's log at info,
# then gwebgpu.sh in the guest, every switch set or the one CASE given.
# Output in $OUT/venus.
set -u
. /root/ref80/lib.sh
O=$OUT/venus; mkdir -p $O
Q=/root/run2/step10/qemu.sh; G=/root/run2/step10/g; S10=/srv/run2/step10
if [ $REHEARSE = 1 ] || [ ! -x $Q ]; then
	echo "venus: no KVM or no step 10 guest here (rehearsal); gwebgpu.sh checked with sh -n" | tee $O/skip.txt
	sh -n $R80/gwebgpu.sh && echo "venus: gwebgpu.sh parses"
	exit 0
fi
SH=/sys/kernel/mm/transparent_hugepage/shmem_enabled
say "venus: start; iommu groups $(find /sys/kernel/iommu_groups -mindepth 1 -maxdepth 1 | wc -l), shmem_enabled $(cat $SH)" | tee $O/run.txt
grep -q '\[never\]' $SH || { echo "venus: shmem_enabled is not never" | tee -a $O/run.txt; exit 1; }
$Q start ref80-venus headless VIRGL_LOG_LEVEL=info >> $O/run.txt 2>&1 || exit 1
$G 'cat > webgpu80.sh' < $R80/gwebgpu.sh
$G sh gsess.sh start >> $O/run.txt 2>&1
$G 'grep -m3 -i -E "virgl|venus|renderer" out/gweston.log' >> $O/run.txt 2>&1
$G 'VN_DEBUG=init vulkaninfo --summary' > $O/vulkaninfo.txt 2>&1
timeout 1800 $G sh webgpu80.sh "$@" 2>&1 | tee -a $O/run.txt
$G 'tar cf - -C out ref80-webgpu' | tar xf - -C $O
$G sh gsess.sh stop > /dev/null 2>&1
cp $S10/logs/qemu-ref80-venus.log $O/ 2>/dev/null
echo "qemu: $($Q qmp '{"execute": "query-status"}')" >> $O/run.txt
$Q stop >> $O/run.txt 2>&1
say "venus: done" | tee -a $O/run.txt
