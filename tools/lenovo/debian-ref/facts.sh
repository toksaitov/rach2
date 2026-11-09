#!/bin/sh
# shellcheck disable=SC2086,SC2046 # no path or word list here has a space
# facts.sh NAME: the boot's configuration as each 80-2 number needs it beside
# it: command line, IOMMU state, i915's memory, huge pages, Mesa and the
# kernel's versions, what runs, the disk. Output in $OUT/facts/NAME.txt.
. /root/ref80/lib.sh
N=${1:-boot}; O=$OUT/facts; mkdir -p $O
{
	say "facts: $N"
	echo "== uname"; uname -a
	echo "== cmdline"; cat /proc/cmdline
	echo "== uptime"; uptime
	echo "== iommu groups: $(find /sys/kernel/iommu_groups -mindepth 1 -maxdepth 1 2>/dev/null | wc -l)"
	echo "== kernel log: DMAR, IOMMU, i915"
	dmesg 2>/dev/null | grep -E 'DMAR: IOMMU|iommu: Default|VT-d active|Transparent Hugepages|stolen memory|Initialized i915|Found HASWELL' | head -20
	echo "== i915 capabilities and regions"
	grep -E '^iommu' /sys/kernel/debug/dri/0/i915_capabilities 2>/dev/null
	head -20 /sys/kernel/debug/dri/0/i915_gem_objects 2>/dev/null
	echo "== huge pages"
	cat /sys/kernel/mm/transparent_hugepage/enabled /sys/kernel/mm/transparent_hugepage/shmem_enabled
	grep -E 'ShmemHugePages|AnonHugePages|MemTotal|MemAvailable' /proc/meminfo
	echo "== cpu"; grep -m1 'model name' /proc/cpuinfo; nproc
	echo "== governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo -)"
	echo "== gpu clocks: min $(cat $GPU/gt_min_freq_mhz 2>/dev/null) max $(cat $GPU/gt_max_freq_mhz 2>/dev/null) act $(cat $GPU/gt_act_freq_mhz 2>/dev/null)"
	echo "== versions"
	dpkg-query -W -f '${Package} ${Version}\n' linux-image-6.12.115+deb13.1-amd64 libgl1-mesa-dri mesa-vulkan-drivers libdrm2 qemu-system-x86 cage weston xwayland firefox-esr chromium supertuxkart 0ad bpftrace intel-gpu-tools 2>/dev/null
	echo "== modules"
	lsmod | awk 'NR > 1 && /^(kvm|snd|vgem|uinput|ath9k|vfio|i915)/ {print $1}' | tr '\n' ' '; echo
	echo "== processes, kernel threads left out"
	ps --ppid 2 -p 2 -N -o user,pid,etime,comm
	echo "== disk"; df -h / /srv 2>/dev/null
	echo "== backlight $(cat /sys/class/backlight/intel_backlight/brightness 2>/dev/null)"
	echo "== grubenv"
	dd if=/dev/disk/by-partuuid/43424140-4544-4746-4849-4a4b4c4d4e4f bs=512 skip=10275 count=2 status=none 2>/dev/null | tr -d '#' | grep -a '='
} > $O/$N.txt 2>&1
cat $O/$N.txt
