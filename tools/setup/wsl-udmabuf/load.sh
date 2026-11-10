#!/bin/sh
#
# load.sh: load the udmabuf module that build.sh made into the running WSL
# kernel, after checking that it was built for this kernel. Runs as root,
# after each start of WSL, before a run that needs Venus:
#
#   ssh testing "MSYS_NO_PATHCONV=1 wsl.exe -u root -- sh \
#       /home/testing/rach2/tools/setup/wsl-udmabuf/load.sh [MODULE]"
#
# MODULE is build/cache/wsl-udmabuf/$(uname -r)/udmabuf.ko of this tree by
# default. The module is loaded with insmod, neither installed under
# /lib/modules nor set to load at boot, so a restart of WSL leaves the kernel
# without it. systemd's stock rule makes /dev/udmabuf root:kvm, mode 0660, so
# an account that runs QEMU must be in the group kvm. Loading it taints the
# kernel, and an oops in it panics the whole WSL machine for every account,
# since the kernel has panic_on_oops and panic=-1. The module's size_limit_mb,
# the largest buffer it makes, is set to 16384, a guest's whole memory, in
# place of its 64: lavapipe exports each allocation that Venus maps through
# udmabuf, so with 64 a Venus allocation over 64 MiB fails on the host and
# its context dies. The parameter is raised on a module already loaded too.
# Exits 0 when the module is loaded, already or now, with that limit.
set -u

PATH=$PATH:/usr/sbin:/sbin
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)
module=${1:-$tree/build/cache/wsl-udmabuf/$(uname -r)/udmabuf.ko}

die()
{
    echo "wsl-udmabuf: $*" >&2
    exit 1
}

limit=16384
param=/sys/module/udmabuf/parameters/size_limit_mb

[ "$(id -u)" -eq 0 ] || die "run as root"
echo "wsl-udmabuf: $(date -Is) $(uname -r)"
if grep -q '^udmabuf ' /proc/modules; then
    if [ "$(cat "$param")" -lt "$limit" ]; then
        echo "$limit" > "$param" || die "size_limit_mb could not be set"
    fi
    echo "wsl-udmabuf: result: already loaded: $(grep '^udmabuf ' /proc/modules), size_limit_mb $(cat "$param")"
    exit 0
fi
[ -f "$module" ] || die "no $module; build it with build.sh"
echo "wsl-udmabuf: $module sha256 $(sha256sum "$module" | cut -c1-64)"
vermagic=$(modinfo -F vermagic "$module")
case $vermagic in
    "$(uname -r) "*) echo "wsl-udmabuf: vermagic matches: $vermagic" ;;
    *) die "vermagic $vermagic is not $(uname -r)'s; not loading" ;;
esac
sh "$here/crc.sh" "$module" || exit 1
echo "wsl-udmabuf: tainted before: $(cat /proc/sys/kernel/tainted)"
lines=$(dmesg | wc -l)
insmod "$module" size_limit_mb="$limit" || die "insmod failed"
sleep 1
dmesg | tail -n +"$((lines + 1))"
echo "wsl-udmabuf: tainted after: $(cat /proc/sys/kernel/tainted)"
grep '^udmabuf ' /proc/modules
ls -l /dev/udmabuf
echo "wsl-udmabuf: kvm: $(getent group kvm | cut -d: -f4)"
echo "wsl-udmabuf: size_limit_mb: $(cat "$param")"
echo "wsl-udmabuf: result: loaded"
