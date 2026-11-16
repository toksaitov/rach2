#!/bin/sh
# shellcheck disable=SC2086,SC2046 # no path or word list here has a space
# mac.sh CMD: the Mac's side of the 80-2 session on the Lenovo's Debian, and of
# its rehearsal in a Debian 13 guest under QEMU.
#   lock | unlock     take or give back the Lenovo (build/flow/lenovo)
#   arm               from Rach2: save p1, copy GRUB's linux and ext2
#                     modules into its current FAT, stage boot.cfg, which
#                     boots Debian's kernel and initrd from p5, with
#                     bootctl config, arm one boot with bootctl trial
#                     recovery, and reboot into Debian
#   wait-debian | wait-rach2   wait up to 6 min for the system to answer SSH
#   push              the scripts and the games layer's replays to
#                     /root/ref80, the pinned sources to /srv/ref80/cache
#   run STEP ARGS     sh /root/ref80/STEP.sh ARGS on the target
#   sh CMD            any command on the target
#   fetch             the target's /srv/ref80/out into build/runs/80-2/
#   leave             leave.sh --now on Debian: end state, rtcwake
#   restore           from Rach2: put p1 back as arm found it and check it
#   rehearse-up | rehearse-down   the rehearsal guest
# REHEARSE=1 points push, run, sh and fetch at the rehearsal guest.
set -eu
D=$(cd "$(dirname "$0")" && pwd)
T=$(cd "$D/../../.." && pwd)
M=$(cd "$(git -C "$T" rev-parse --git-common-dir)/.." && pwd)
KEY=$T/krffs/rach2_demo_ed25519
K2=$M/build/flow/coordinator/graphics-research/run2
C=$M/build/cache/80-2
OUTDIR=$M/build/runs/80-2
LOCK=$M/build/flow/lenovo
HOST=192.168.0.106
SSHO="-i $KEY -o BatchMode=yes -o ConnectTimeout=10 -o LogLevel=ERROR -o StrictHostKeyChecking=accept-new -o ServerAliveInterval=15"
rssh() { /usr/bin/ssh $SSHO -o UserKnownHostsFile=$K2/lenovo-known root@$HOST "$@"; }
if [ "${REHEARSE:-0}" = 1 ]; then
	OUTDIR=$M/build/runs/80-2-rehearsal
	tssh() { /usr/bin/ssh $SSHO -o UserKnownHostsFile=$C/rehearse/known -p 2280 root@127.0.0.1 "$@"; }
else
	tssh() { /usr/bin/ssh $SSHO -o UserKnownHostsFile=$K2/lenovo-known-debian root@$HOST "$@"; }
fi
say() { echo "$(date +%T) mac: $*"; }
# waitfor FN: until FN true answers, at most 360 s.
waitfor() {
	t0=$(date +%s)
	while [ $(($(date +%s) - t0)) -lt 360 ]; do
		$1 true 2> /dev/null && { say "answered $(($(date +%s) - t0)) s after the wait began"; return 0; }
		sleep 5
	done
	say "no answer in 360 s"; return 1
}
# p1write IMG: write the runs where IMG differs from p1 as read now, through
# /dev/bootpart on the running Rach2, and print p1's hash afterwards.
p1write() {
	rssh 'dd if=/dev/bootpart bs=1048576 2>/dev/null' > $C/p1-now.img
	python3 -I $D/p1runs.py "$1" $C/p1-now.img > $C/p1-write-runs.txt
	say "p1: $(wc -l < $C/p1-write-runs.txt | tr -d ' ') runs differ from $(basename "$1")"
	/usr/bin/scp -q $SSHO -o UserKnownHostsFile=$K2/lenovo-known "$1" root@$HOST:/dev/shm/p1-want.img
	want=$(shasum -a 256 "$1" | cut -d' ' -f1)
	{ echo 'set -e'; echo "[ \"\$(sha256sum /dev/shm/p1-want.img | cut -d' ' -f1)\" = $want ]"
	  while read -r s n; do echo "dd if=/dev/shm/p1-want.img of=/dev/bootpart bs=1M iflag=skip_bytes,count_bytes oflag=seek_bytes skip=$((s * 512)) seek=$((s * 512)) count=$((n * 512)) status=none"; done < $C/p1-write-runs.txt
	  echo 'sync; rm -f /dev/shm/p1-want.img; sha256sum /dev/bootpart'; } | rssh 'sh -s'
	rm -f $C/p1-now.img
}
# grubmods: GRUB's linux, ext2 and fshelp modules in $C/grub, from the
# grub-efi-amd64-bin package that vendor/grub/manifest.txt pins, whose build
# made the boot partition's BOOTX64.EFI.
grubmods() {
	[ -s $C/grub/ext2.mod ] && [ -s $C/grub/fshelp.mod ] && [ -s $C/grub/linux.mod ] && return 0
	snap=$(awk '$1 == "snapshot" { print $2 }' $T/vendor/grub/manifest.txt)
	set -- $(awk '$1 == "package" && $2 == "grub-efi-amd64-bin" { print $3, $5 }' $T/vendor/grub/manifest.txt)
	g=$(mktemp -d "$C/grub-deb.XXXXXX")
	curl -sSfL -o $g/bin.deb "$snap/pool/main/g/grub2-unsigned/grub-efi-amd64-bin_${1}_amd64.deb"
	[ "$(shasum -a 256 $g/bin.deb | cut -d' ' -f1)" = "$2" ] || { echo "mac: grub-efi-amd64-bin $1: wrong SHA-256" >&2; rm -rf $g; exit 1; }
	(cd $g && ar x bin.deb data.tar.xz && tar xf data.tar.xz ./usr/lib/grub/x86_64-efi/linux.mod \
	    ./usr/lib/grub/x86_64-efi/ext2.mod ./usr/lib/grub/x86_64-efi/fshelp.mod)
	mkdir -p $C/grub
	cp $g/usr/lib/grub/x86_64-efi/linux.mod $g/usr/lib/grub/x86_64-efi/ext2.mod $g/usr/lib/grub/x86_64-efi/fshelp.mod $C/grub/
	rm -rf $g
}
case ${1:-} in
grubmods) grubmods ;;
lock)
	mkdir "$LOCK" || { echo "mac: the Lenovo is taken: $(cat "$LOCK/issue")" >&2; exit 1; }
	echo 2560 > "$LOCK/issue"; say "lock taken for 2560" ;;
unlock) rm -rf "$LOCK"; say "lock given back" ;;
arm)
	rssh 'grep -q rach2.slot=candidate /proc/cmdline' || { echo "mac: the Lenovo is not running Rach2's candidate" >&2; exit 1; }
	rssh 'cat /etc/rach2/release; bootctl status' | tee $C/rach2-before.txt
	rssh 'dd if=/dev/bootpart bs=1048576 2>/dev/null' > $C/p1-before.img
	shasum -a 256 $C/p1-before.img | tee $C/p1-before.sha256
	# Allocate in the current FAT: old sector runs can name a new file's
	# clusters, and old FAT sectors can discard its allocation chains.
	# Debian's kernel and initrd stay on p5, since with 16 MiB kernel slots
	# p1 has 31 MB free and they take 49.
	grubmods
	cp $C/p1-before.img $C/p1-debian.img
	mmd -i $C/p1-debian.img ::/boot/grub/x86_64-efi 2>/dev/null ||
	    mdir -i $C/p1-debian.img ::/boot/grub/x86_64-efi > /dev/null
	mcopy -o -i $C/p1-debian.img $C/grub/linux.mod $C/grub/ext2.mod $C/grub/fshelp.mod ::/boot/grub/x86_64-efi/
	p1write $C/p1-debian.img
	/usr/bin/scp -q $SSHO -o UserKnownHostsFile=$K2/lenovo-known $D/boot.cfg root@$HOST:/dev/shm/boot.cfg
	rssh 'bootctl config /dev/shm/boot.cfg && bootctl trial recovery && bootctl status && rm -f /dev/shm/boot.cfg' | tee $C/arm.txt
	say "rebooting Rach2 into Debian"
	rssh 'nohup sh -c "sleep 2; reboot" > /dev/null 2>&1 &'
	;;
wait-debian) waitfor tssh ;;
wait-rach2) waitfor rssh ;;
push)
	tssh 'mkdir -p /root/ref80 /srv/ref80/cache'
	tar --no-xattrs --no-mac-metadata -cf - -C $D --exclude mac.sh --exclude rehearse.sh . | tssh 'tar xf - -C /root/ref80'
	# The games' fixed replays are the games layer's, which Rach2 plays too.
	tar --no-xattrs --no-mac-metadata -cf - -C $T/vendor/gnu/games replays | tssh 'tar xf - -C /root/ref80'
	[ "${REHEARSE:-0}" = 1 ] && tssh 'touch /root/ref80/rehearse'
	cut -d' ' -f3 $D/SHA256SUMS | while read -r f; do
		tssh "test -s /srv/ref80/cache/$f" < /dev/null 2> /dev/null || cat $C/$f | tssh "cat > /srv/ref80/cache/$f"
	done
	tssh 'cd /srv/ref80/cache && sha256sum -c /root/ref80/SHA256SUMS'
	;;
run) shift; s=$1; shift; tssh "sh /root/ref80/$s.sh $*" ;;
sh) shift; tssh "$@" ;;
fetch)
	mkdir -p $OUTDIR
	tssh 'tar cf - -C /srv/ref80 out' | tar xf - -C $OUTDIR
	tssh 'tar cf - -C /root/ref80 replays' 2> /dev/null | tar xf - -C $OUTDIR 2> /dev/null || true
	say "fetched into $OUTDIR: $(du -sh $OUTDIR | cut -f1)"
	;;
leave) tssh 'sh /root/ref80/leave.sh --now' ;;
restore)
	rssh 'grep -q rach2.slot=candidate /proc/cmdline' || { echo "mac: the Lenovo is not running Rach2's candidate" >&2; exit 1; }
	p1write $C/p1-before.img
	echo "mac: want $(cut -d' ' -f1 $C/p1-before.sha256)"
	rssh 'bootctl status'
	;;
rehearse-up) sh $D/rehearse.sh up ;;
rehearse-down) sh $D/rehearse.sh down ;;
*) sed -n '2,22p' "$0" >&2; exit 1 ;;
esac
