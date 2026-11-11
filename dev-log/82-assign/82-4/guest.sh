#!/bin/sh
# guest.sh NAME MODE [OPTION...]: on Rach2, as root, give the Lenovo's GPU
# to step 11's guest in g82 mode MODE (probe, run or hold) through gpu.sh,
# which sits beside this script, and take it back. OPTIONs go to gpu.sh
# before the guest's own: -t TOKEN, -K TOKEN, -f uefi.
#
# The guest's files are in G82_DIR, /data/t2666/g82 by default, as prep.sh
# put them there; QEMU's command line is 82-4.md's. Everything the run
# writes is in G82_OUT/NAME, /data/t2666/out/NAME by default: gpu.sh's lines
# in run.log, the guest's console, QEMU's messages, SeaBIOS's log, and the
# exit status in status. QEMU's snapshot of the disk goes under TMPDIR,
# /data/t2666/tmp by default. It prints run.log at the end and exits with
# gpu.sh's status.
set -u
here=$(cd "$(dirname "$0")" && pwd)
[ $# -ge 2 ] || { echo "usage: guest.sh NAME probe|run|hold [OPTION...]" >&2; exit 2; }
name=$1 mode=$2
shift 2
g=${G82_DIR:-/data/t2666/g82}
out=${G82_OUT:-/data/t2666/out}/$name
export TMPDIR=${TMPDIR:-/data/t2666/tmp}
rm -rf "$out"
mkdir -p "$out" "$TMPDIR"
bash "$here/gpu.sh" -s 4 -m 3072 -o "$out" "$@" \
    -k "$g/vmlinuz" -i "$g/initrd.img" \
    -l "root=/dev/vda1 ro console=ttyS0,115200n8 modprobe.blacklist=snd_hda_intel,snd_hda_codec_hdmi,ath9k g82=$mode" \
    -- -drive "file=$g/guest.qcow2,if=virtio,format=qcow2,snapshot=on" \
    -drive "file=$g/seed.img,if=virtio,format=raw,readonly=on" \
    -netdev user,id=n0 -device virtio-net-pci,netdev=n0 \
    -device virtio-rng-pci -device virtio-keyboard-pci -device virtio-tablet-pci \
    -chardev "file,id=seabios,path=$out/seabios.log" -device isa-debugcon,iobase=0x402,chardev=seabios \
    > "$out/run.log" 2>&1 < /dev/null
st=$?
echo $st > "$out/status"
cat "$out/run.log"
exit $st
