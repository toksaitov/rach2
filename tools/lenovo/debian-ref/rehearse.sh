#!/bin/sh
# shellcheck disable=SC2086,SC2046 # no path or word list here has a space
# rehearse.sh up|down: the 80-2 session's rehearsal guest on the Mac. A sparse
# disk the size of the Lenovo's with its partition table, p1 as mac.sh arm
# leaves it (run2's image with GRUB's linux and ext2 modules, boot.cfg staged
# as bootctl config stages it and next_entry=recovery as bootctl trial recovery
# sets it), and Debian's root as run2's setup wrote it to p5. OVMF boots it
# under TCG through the same GRUB and menu, so the boot through next_entry
# with intel_iommu=off is what the Lenovo will run. SSH on 127.0.0.1:2280.
set -eu
D=$(cd "$(dirname "$0")" && pwd)
T=$(cd "$D/../../.." && pwd)
M=$(cd "$(git -C "$T" rev-parse --git-common-dir)/.." && pwd)
C=$M/build/cache/80-2; H=$C/rehearse; RUN2=$M/build/cache/run2/lenovo-setup
K2=$M/build/flow/coordinator/graphics-research/run2
KEY=$T/krffs/rach2_demo_ed25519
SECTORS=488397168	# the Lenovo's Samsung SSD 870 EVO, /proc/disk
P5=421287936		# p5's first sector
QD=/opt/homebrew/share/qemu
case ${1:-} in
up)
	mkdir -p $H
	if [ ! -s $H/gpt-head.bin ]; then
		ssh -i $KEY -o BatchMode=yes -o UserKnownHostsFile=$K2/lenovo-known root@192.168.0.106 \
			'dd if=/dev/disk0 bs=512 count=34 2>/dev/null' > $H/gpt-head.bin
		ssh -i $KEY -o BatchMode=yes -o UserKnownHostsFile=$K2/lenovo-known root@192.168.0.106 \
			"dd if=/dev/disk0 bs=512 skip=$((SECTORS - 33)) count=33 2>/dev/null" > $H/gpt-tail.bin
	fi
	[ "$(wc -c < $H/gpt-head.bin)" -eq 17408 ] && [ "$(wc -c < $H/gpt-tail.bin)" -eq 16896 ] ||
		{ echo "rehearse: the partition table copies are short" >&2; exit 1; }
	sh $D/mac.sh grubmods
	cp $RUN2/p1-run2.img $H/p1.img
	mcopy -o -i $H/p1.img $C/grub/linux.mod $C/grub/ext2.mod $C/grub/fshelp.mod ::/boot/grub/x86_64-efi/
	python3 -I - $H/p1.img $D/boot.cfg <<'PY'
import sys
img, menu = sys.argv[1], open(sys.argv[2], "rb").read()
MENU, ENV = 10259 * 512, 10275 * 512
assert len(menu) <= 8192
f = open(img, "r+b")
f.seek(MENU); f.write(menu + b"#" * (8192 - len(menu)))
f.seek(ENV); env = f.read(1024)
new = env.replace(b"next_entry=\n", b"next_entry=recovery\n", 1)
assert new != env and new.endswith(b"#" * 16)
f.seek(ENV); f.write(new[:1024])
PY
	rm -f $H/disk.raw
	mkfile -n $((SECTORS * 512)) $H/disk.raw
	dd if=$H/gpt-head.bin of=$H/disk.raw conv=notrunc 2> /dev/null
	dd if=$H/gpt-tail.bin of=$H/disk.raw bs=512 seek=$((SECTORS - 33)) conv=notrunc 2> /dev/null
	dd if=$H/p1.img of=$H/disk.raw bs=1m oseek=1 conv=notrunc 2> /dev/null
	dd if=$RUN2/root-run2.ext4 of=$H/disk.raw bs=512 seek=$P5 conv=notrunc 2> /dev/null
	cp $QD/edk2-i386-vars.fd $H/vars.fd
	rm -f $H/known $H/qemu.pid
	qemu-system-x86_64 -machine q35 -accel tcg,thread=multi -cpu max -smp 4 -m 6144 \
		-drive if=pflash,format=raw,readonly=on,file=$QD/edk2-x86_64-code.fd \
		-drive if=pflash,format=raw,file=$H/vars.fd \
		-drive file=$H/disk.raw,format=raw,if=none,id=d0,cache=unsafe -device ahci,id=ahci \
		-device ide-hd,drive=d0,bus=ahci.0 \
		-netdev user,id=n0,hostfwd=tcp:127.0.0.1:2280-:22 -device e1000e,netdev=n0 \
		-serial file:$H/serial.log -display none -monitor none -no-reboot \
		-pidfile $H/qemu.pid -daemonize
	echo "rehearse: qemu pid $(cat $H/qemu.pid)"
	t0=$(date +%s)
	while [ $(($(date +%s) - t0)) -lt 600 ]; do
		ssh -i $KEY -o BatchMode=yes -o ConnectTimeout=5 -o LogLevel=ERROR -o UserKnownHostsFile=$H/known \
			-o StrictHostKeyChecking=accept-new -p 2280 root@127.0.0.1 true 2> /dev/null &&
			{ echo "rehearse: ssh after $(($(date +%s) - t0)) s"; exit 0; }
		sleep 5
	done
	echo "rehearse: no ssh after 600 s" >&2; tail -20 $H/serial.log >&2; exit 1
	;;
down)
	[ -s $H/qemu.pid ] && kill "$(cat $H/qemu.pid)" 2> /dev/null || true
	sleep 2; rm -f $H/disk.raw $H/qemu.pid $H/vars.fd $H/p1.img
	echo "rehearse: down"
	;;
*) echo "usage: rehearse.sh up|down" >&2; exit 1 ;;
esac
