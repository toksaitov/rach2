#!/bin/sh
# shellcheck disable=SC2086,SC2046 # no path or word list here has a space
# efault.sh [DISPLAY]: run2 step 10's "kvm run failed Bad address" again, with
# the refused page traced. step 10's guest (its qemu.sh, g, and in the guest
# gsess.sh and gproton.sh, all on p5 under /root/run2/step10 and
# /srv/run2/step10) runs WineD3D on GL through virgl, first with
# shmem_enabled at never (the control: the probe draws), then at always (the
# fault), each with efault.bt attached. DISPLAY is QEMU's: headless (the
# default, egl-headless on the render node, nothing on the panel) or sdl.
# shmem_enabled is put back by an exit trap. Output in $OUT/efault.
set -u
. /root/ref80/lib.sh
need bpftrace
O=$OUT/efault; mkdir -p $O
D=${1:-headless}
Q=/root/run2/step10/qemu.sh; G=/root/run2/step10/g; S10=/srv/run2/step10
SH=/sys/kernel/mm/transparent_hugepage/shmem_enabled
BT=
cleanup() {
	pkill -INT -x bpftrace && sleep 2
	echo never > $SH
	echo "efault: shmem_enabled put back: $(cat $SH)"
}
trap cleanup EXIT
trap 'exit 1' INT TERM HUP
say "efault: start, display $D"
modprobe kvm 2>/dev/null
echo "in kallsyms: hva_to_pfn_remapped $(grep -c -w hva_to_pfn_remapped /proc/kallsyms), hva_to_pfn $(grep -c -w hva_to_pfn /proc/kallsyms), follow_pfnmap_end $(grep -c -w follow_pfnmap_end /proc/kallsyms)" | tee $O/symbols.txt
bpftrace --version >> $O/symbols.txt
# st TAG: QEMU's run state, the huge shmem pages, QEMU's i915 GEM mappings.
st() {
	echo "qemu: $($Q qmp '{"execute": "query-status"}' 2>&1)"
	grep -E 'ShmemHugePages|AnonHugePages' /proc/meminfo | tr -s ' ' | tr '\n' ' '; echo
	p=$(cat $S10/qemu.pid 2>/dev/null)
	[ -n "$p" ] && echo "qemu maps of anon_inode:i915.gem: $(grep -c 'anon_inode:i915.gem' /proc/$p/maps)"
}
# one MODE: the probe once with shmem_enabled at MODE, traced.
one() {
	m=$1; echo $m > $SH
	echo "== $(date -u +%T) shmem_enabled $(cat $SH)"
	bpftrace $R80/efault.bt > $O/trace-$m.txt 2>&1 &
	BT=$!
	n=0; while ! grep -q Attaching $O/trace-$m.txt && [ $n -lt 60 ]; do sleep 1; n=$((n + 1)); done
	head -1 $O/trace-$m.txt
	if [ $REHEARSE = 1 ] || [ ! -x $Q ]; then
		echo "efault: rehearsal or no step 10 guest: the trace attached, no guest to run"
		sleep 2
	else
		$Q start ref80-$m $D || return 1
		$G sh gsess.sh start
		st
		timeout 400 $G sh gproton.sh ref80-$m d3d11-64-wined3d-gl:d3d11probe64.exe:PROTON_USE_WINED3D=1; echo "gproton exit $?"
		sleep 3
		st
		cp $S10/logs/qemu-ref80-$m.log $S10/logs/serial-ref80-$m.log $O/ 2>/dev/null
		p=$(cat $S10/qemu.pid 2>/dev/null)
		[ -n "$p" ] && grep -B1 -A22 'i915.gem' /proc/$p/smaps 2>/dev/null | grep -E 'i915.gem|VmFlags|KernelPageSize' | head -30 > $O/smaps-$m.txt
		if $Q qmp '{"execute": "query-status"}' | grep -q '"running": true'; then $Q stop; else $Q qmp '{"execute": "quit"}'; sleep 3; rm -f $S10/qemu.pid $S10/qmp.sock; fi
	fi
	kill -INT $BT; wait $BT 2>/dev/null; BT=
	echo "trace $m: $(grep -c " refused:" $O/trace-$m.txt) refusals"
	grep -A2 " refused:" $O/trace-$m.txt | head -12
}
{
	cat /sys/kernel/mm/transparent_hugepage/enabled $SH
	one never
	one always
	say "efault: done"
} 2>&1 | tee $O/run.txt
