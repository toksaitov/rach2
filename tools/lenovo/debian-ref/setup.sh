#!/bin/sh
# shellcheck disable=SC2086,SC2046 # no path or word list here has a space
# setup.sh: the packages and pinned sources the 80-2 session adds to the
# Lenovo's Debian, with Mesa, the kernel and the browsers held at the versions
# the earlier references used. Run as root; output in $OUT/setup.
set -eu
. /root/ref80/lib.sh
O=$OUT/setup; mkdir -p $O $S80/src
# What the earlier steps installed and the references were taken on; held so
# that no dependency of the new packages moves them.
HOLD="libgl1-mesa-dri libegl-mesa0 libgbm1 libglx-mesa0 mesa-vulkan-drivers
mesa-va-drivers mesa-libgallium libdrm2 libdrm-intel1 linux-image-6.12.115+deb13.1-amd64
chromium firefox-esr qemu-system-x86 libvirglrenderer1 weston cage xwayland"
# The earlier steps' packages, already on the Lenovo; the rehearsal guest
# needs them too.
BASE="build-essential python3 python3-websocket curl ffmpeg mesa-utils
libgl1-mesa-dri mesa-vulkan-drivers weston cage seatd xwayland firefox-esr
qemu-system-x86 intel-gpu-tools pciutils strace"
# New in this session: bpftrace for the KVM trace, rsync for the selftests'
# header step, the two native games, grim for cage's screen, ALSA's tools
# for the silent card.
NEW="bpftrace rsync supertuxkart 0ad grim alsa-utils libnuma-dev"
say "setup: start"
# The packages are fetched into memory, not onto p5's 32 GiB root, which
# holds the game, step 10's guest and step 9's builds.
A=/dev/shm/apt-ref80; mkdir -p $A/partial
AO="-o Dir::Cache::Archives=$A"
dpkg-query -W -f '${Package} ${Version}\n' > $O/packages-before.txt
apt-get update > $O/apt-update.log 2>&1
[ $REHEARSE = 1 ] && DEBIAN_FRONTEND=noninteractive apt-get install -y $AO $BASE > $O/apt-base.log 2>&1
held=""
for p in $HOLD; do dpkg-query -W "$p" > /dev/null 2>&1 && held="$held $p"; done
# shellcheck disable=SC2086
apt-mark hold $held > $O/hold.log
# shellcheck disable=SC2086
DEBIAN_FRONTEND=noninteractive apt-get install -y $AO --no-install-recommends $NEW > $O/apt-new.log 2>&1
# shellcheck disable=SC2086
apt-mark unhold $held >> $O/hold.log
apt-get clean; rm -rf $A
dpkg-query -W -f '${Package} ${Version}\n' > $O/packages-after.txt
diff $O/packages-before.txt $O/packages-after.txt > $O/packages-diff.txt || true
# shellcheck disable=SC2086
dpkg-query -W -f '${Package} ${Version}\n' $NEW $held mesa-utils intel-gpu-tools 2> /dev/null > $O/versions.txt || true
cd $C80
sha256sum -c $R80/SHA256SUMS > $O/sources.txt
rm -rf $S80/src/kvm-unit-tests $S80/src/linux
tar xzf kvm-unit-tests-5a221342.tar.gz -C $S80/src
mv $S80/src/kvm-unit-tests-5a221342a025948fa7d84f0c404748f6ba57f3ad $S80/src/kvm-unit-tests
# The selftests need the tools tree, the UAPI headers and what `make headers`
# reads; the drivers, file systems and documentation stay packed.
tar xJf linux-6.12.115.tar.xz -C $S80/src --exclude='linux-6.12.115/drivers' \
	--exclude='linux-6.12.115/Documentation' --exclude='linux-6.12.115/sound' \
	--exclude='linux-6.12.115/fs' --exclude='linux-6.12.115/net'
mv $S80/src/linux-6.12.115 $S80/src/linux
du -sh $S80/src/* > $O/src-sizes.txt
df -h / > $O/df.txt
say "setup: done"
cat $O/packages-diff.txt | grep -c '^>' | sed 's/^/setup: packages added or changed: /'
cat $O/df.txt
