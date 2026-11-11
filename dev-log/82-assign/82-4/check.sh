#!/bin/sh
# check.sh TAG: on Rach2, as root, the state of the Lenovo's GPU between the
# runs of 82-4b's step: its driver and nodes, the panel's pipe CRC at its
# own mode by crcmodes (78-10d's tool, beside this script), Crocus named by
# eglinfo, two of piglit's readbacks on the GPU's render node, the backlight
# and the kernel's counts of IOMMU faults, VFIO, KVM and free vectors. The
# desktop's Weston is ended first, since crcmodes needs the DRM master and
# the release would unplug the device under it; rach2-desktop starts the
# session again only on SIGHUP.
#
# Lines begin "check: TAG:". The last is "check: TAG: result: pass" when the
# driver is i915, the CRC is stable, eglinfo names crocus and both readbacks
# pass, or "check: TAG: result: fail: WHAT". It exits 0 or 1.
here=$(cd "$(dirname "$0")" && pwd)
tag=${1:?usage: check.sh TAG}
d=/sys/bus/pci/devices/0000:00:02.0
bin=/usr/lib/x86_64-linux-gnu/piglit/bin
bad=

say() { echo "check: $tag: $*"; }

drv=$(basename "$(readlink $d/driver 2> /dev/null)" 2> /dev/null)
card=$(ls $d/drm 2> /dev/null | grep -m 1 '^card')
render=$(ls $d/drm 2> /dev/null | grep -m 1 '^renderD')
if pkill -x weston; then
    n=0
    while pgrep -x weston > /dev/null && [ $n -lt 10 ]; do sleep 1; n=$((n + 1)); done
    say "desktop: weston ended"
fi
say "driver ${drv:-none}, ${card:-no card}, ${render:-no render node}"
[ "$drv" = i915 ] && [ -n "$card" ] && [ -n "$render" ] || { say 'result: fail: no i915 card'; exit 1; }
for c in /sys/class/drm/$card-*; do say "connector: ${c##*/} $(cat $c/status)"; done
b=/sys/class/backlight/intel_backlight
[ -d $b ] && say "backlight: $(cat $b/brightness) of $(cat $b/max_brightness), actual $(cat $b/actual_brightness)"

crc=$("$here/crcmodes" -D /dev/dri/$card -d /sys/kernel/debug/dri/${card#card} 1366x768:aspect 2>&1)
echo "$crc" | sed "s/^/check: $tag: /"
echo "$crc" | grep -q '^crcmodes: mode 1366x768 aspect crc ' || bad="$bad crc"

egl=$(EGL_LOG_LEVEL=debug eglinfo -B -p surfaceless 2>&1)
echo "$egl" | grep -E 'using driver|pci id|renderer' | sed "s/^/check: $tag: eglinfo: /"
echo "$egl" | grep -q 'driver crocus$' || bad="$bad eglinfo"

for t in gl-1.0-readpixsanity fbo-readpixels; do
    r=$(PIGLIT_PLATFORM=gbm WAFFLE_GBM_DEVICE=/dev/dri/$render "$bin/$t" -auto -fbo 2>&1)
    st=$?
    say "piglit: $t: exit $st: $(echo "$r" | grep -m 1 'PIGLIT:')"
    echo "$r" | grep -q '"result": "pass"' && [ $st = 0 ] || bad="$bad $t"
done

grep -E '^(iommu_dma_faults|iommu_irq_faults|vfio_|msi_vectors_free|kvm_)' /proc/stat |
    tr '\n' ' ' | sed "s/^/check: $tag: counts: /"
echo
if [ -n "$bad" ]; then
    say "result: fail:$bad"
    exit 1
fi
say 'result: pass'
