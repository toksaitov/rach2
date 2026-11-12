#!/bin/sh
#
# linux-corpus.sh: run a list of Linux's KVM selftests or kvm-unit-tests
# under Linux's KVM in the outer QEMU that the KVM suite's nested cases boot
# Rach2 in, so that its results stand beside Rach2's at the same depth and
# on the same processor.
#
# Usage: linux-corpus.sh [-a ACCEL] [-k KIND] LIST
#
# KIND is corpus.sh's, `selftests` by default or `unit`, and LIST a list of
# this directory in that corpus's form, such as selftests-gap.list or
# unit-subset.list, which the KVM suite's data disk, kvm.ext2, carries. The
# host is mklinuxhost.sh's Debian kernel, made into build/kvm/linuxhost when
# it is absent there; it boots with the GNU root's disk,
# build/x86_64/images/gnu-test.disk, and kvm.ext2, each behind a qcow2 overlay
# of its own, under ACCEL, `kvm` by default, on the processor of the
# `host-svm` profile with two processors and 6 GiB, as the KVM suite's nested
# cases give Rach2, and with `linuxhost=KIND:LIST` on its command line runs
# `corpus.sh KIND LIST` in the root and powers off (guests/linux-host-init).
# Each line of the console is written to standard output after the seconds
# since the epoch at which it came, for the record; corpus.sh gives each
# test's seconds itself. It ends when QEMU does, and the overlays go on every
# exit.

usage()
{
    echo "usage: linux-corpus.sh [-a ACCEL] [-k KIND] LIST" >&2
    exit 2
}

DIR="$(cd "$(dirname "$0")" && pwd)"
REPO="$(cd "$DIR/../.." && pwd)"
accel=kvm
kind=selftests
while getopts a:k: opt; do
    case $opt in
        a) accel=$OPTARG ;;
        k) kind=$OPTARG ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 1 ] || usage
case $kind in
    unit | selftests) ;;
    *) usage ;;
esac
list=$1
[ -f "$DIR/$list" ] || {
    echo "linux-corpus: no list $list in $DIR" >&2
    exit 2
}
host=$REPO/build/kvm/linuxhost
disk=$REPO/build/x86_64/images/gnu-test.disk
data=$REPO/build/tests/cases/kvm/kvm.ext2
for f in "$disk" "$data"; do
    [ -f "$f" ] || {
        echo "linux-corpus: no $f: make ARCH=x86_64 $f" >&2
        exit 1
    }
done
if [ ! -f "$host/vmlinuz" ] || [ ! -f "$host/initrd" ] ||
    [ "$host/initrd" -ot "$DIR/guests/linux-host-init" ]; then
    MKCPIO=$REPO/build/tools/wifi/mkcpio sh "$DIR/guests/mklinuxhost.sh" "$host" >&2 || exit 1
fi

work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$REPO/build/tmp"
work=$(mktemp -d "$REPO/build/tmp/rach2-linuxcorpus.XXXXXX") || exit 1
qemu-img create -q -f qcow2 -F raw -b "$disk" "$work/gnu.qcow2" || exit 1
qemu-img create -q -f qcow2 -F raw -b "$data" "$work/kvm.qcow2" || exit 1

# host-svm: the SVM features the testing PC's KVM under WSL gives its guest
# and none of those it lacks (tools/etude/processor.c), with the invariant
# TSC that rach2.tscreliable stands for on Rach2's side.
cpu=host,svm=on,npt=on,nrip-save=on,tsc-scale=on,vmcb-clean=on,flushbyasid=on
cpu=$cpu,pause-filter=on,decodeassists=off,v-vmsave-vmload=off,vgif=off,invtsc=on
[ "$accel" = kvm ] || cpu=max,svm=on,npt=on
qemu-system-x86_64 -accel "$accel" -machine q35 -cpu "$cpu" -smp 2 -m 6144 \
    -nodefaults -no-reboot -display none -monitor none -serial stdio \
    -kernel "$host/vmlinuz" -initrd "$host/initrd" \
    -append "console=ttyS0 linuxhost=$kind:$list" \
    -drive "if=none,id=gnu,file=$work/gnu.qcow2,format=qcow2" -device ide-hd,drive=gnu,bus=ide.0 \
    -drive "if=none,id=kvm,file=$work/kvm.qcow2,format=qcow2" -device ide-hd,drive=kvm,bus=ide.1 \
    < /dev/null | tr -d '\r' | while IFS= read -r line; do
    printf '%s %s\n' "$(date +%s)" "$line"
done
