#!/bin/sh
# shellcheck disable=SC2086,SC2046 # no path or word list here has a space
# leave.sh [--now]: the end of the 80-2 session on Debian. Records the end
# state (nothing of the session running, shmem_enabled at never, no sound
# module, p1's grubenv with next_entry empty), unloads snd-dummy, unmounts the
# game's overlay, and with --now powers off by `rtcwake -m off -s 120`, the
# way Linux leaves the Lenovo (a warm reboot once left its firmware unable to
# boot); 120 s later GRUB boots Rach2's saved candidate. Output in
# $OUT/leave.txt.
. /root/ref80/lib.sh
{
	say "leave: end state"
	pkill -u gfx -x cage; pkill -9 -u gfx -f 'Crysis.exe|wineserver|firefox|chromium|supertuxkart|pyrogenesis'
	pkill -INT -x bpftrace
	[ -s /srv/run2/step10/qemu.pid ] && /root/run2/step10/qemu.sh stop
	sleep 2
	quiet_check
	echo never > /sys/kernel/mm/transparent_hugepage/shmem_enabled
	echo "shmem_enabled: $(cat /sys/kernel/mm/transparent_hugepage/shmem_enabled)"
	modprobe -r snd-dummy 2>/dev/null
	echo "sound modules after: $(lsmod | awk '/^snd/ {print $1}' | tr '\n' ' ')"
	mountpoint -q /srv/run2/step9/crysis && umount /srv/run2/step9/crysis
	echo "overlay mounted: $(mount | grep -c /srv/run2/step9/crysis)"
	rm -rf $S80/prof /run/user/1000/* 2>/dev/null
	df -h /
	du -sh $S80 $S80/* 2>/dev/null
	echo "grubenv: $(dd if=/dev/disk/by-partuuid/43424140-4544-4746-4849-4a4b4c4d4e4f bs=512 skip=10275 count=2 status=none | tr -d '#' | grep -a = | tr '\n' ' ')"
} 2>&1 | tee $OUT/leave.txt
if [ "${1:-}" = --now ]; then
	say "leave: rtcwake -m off -s 120 in 2 s" | tee -a $OUT/leave.txt
	sync
	nohup sh -c 'sleep 2; rtcwake -m off -s 120' > /dev/null 2>&1 &
fi
