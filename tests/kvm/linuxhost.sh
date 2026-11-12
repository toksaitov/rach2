#!/bin/sh
#
# linuxhost.sh: run a corpus of the KVM suite under Linux's KVM in place of
# Rach2's, on the Linux host that guests/mklinuxhost.sh makes, booted by the
# testing PC's QEMU under KVM with the machine of the suite's nested cases,
# so that Linux's results sit beside Rach2's.
#
# Usage: linuxhost.sh [-d DATA] [-o LOG] HOSTDIR KIND LIST
#
# HOSTDIR has mklinuxhost.sh's vmlinuz and initrd; KIND and LIST are
# corpus.sh's, LIST a list of the data disk such as unit-subset.list. The
# machine is that of machine.kvm.json's `-nested` cases: the host-svm
# processor profile (tools/etude/processor.c), two processors, 6 GiB, and
# on an ich9-ahci the x86-64 GNU root's disk, build/x86_64/images/
# gnu-test.disk, and the suite's data disk, build/tests/cases/kvm/kvm.ext2,
# both under QEMU's snapshot, so that neither is written; -d names another
# data disk in place of kvm.ext2, with LIST among its files. The host passes
# `linuxhost.corpus=KIND:LIST` to linux-host-init, which runs corpus.sh in
# the GNU root and powers off. The console goes to LOG,
# build/tmp/linuxhost.log by default; the last line printed is corpus.sh's
# result line, and the exit status 0 when it is `pass`, 1 otherwise and 2 on
# a usage error. QEMU is the one on the path, `qemu-system-x86_64`.

repo=$(cd "$(dirname "$0")/../.." && pwd) || exit 2
log=$repo/build/tmp/linuxhost.log
data=$repo/build/tests/cases/kvm/kvm.ext2
usage()
{
    echo "usage: linuxhost.sh [-d DATA] [-o LOG] HOSTDIR KIND LIST" >&2
    exit 2
}
while getopts d:o: opt; do
    case $opt in
        d) data=$OPTARG ;;
        o) log=$OPTARG ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 3 ] && [ -f "$1/vmlinuz" ] && [ -f "$1/initrd" ] || usage
host=$1 kind=$2 list=$3
disk=$repo/build/x86_64/images/gnu-test.disk
for f in "$disk" "$data"; do
    [ -f "$f" ] || { echo "linuxhost: no $f" >&2; exit 1; }
done

# QEMU's snapshot writes its overlays under TMPDIR.
TMPDIR=$repo/build/tmp
export TMPDIR
mkdir -p "$TMPDIR" || exit 1
qemu=
trap '[ -z "$qemu" ] || { kill "$qemu" 2> /dev/null; wait "$qemu"; }' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

qemu-system-x86_64 -accel kvm -nodefaults -display none -no-reboot \
    -cpu host,svm=on,npt=on,nrip-save=on,tsc-scale=on,vmcb-clean=on,flushbyasid=on,pause-filter=on,decodeassists=off,v-vmsave-vmload=off,vgif=off,enforce=on \
    -smp 2 -m 6G \
    -kernel "$host/vmlinuz" -initrd "$host/initrd" \
    -append "console=ttyS0 linuxhost.corpus=$kind:$list" \
    -device ich9-ahci,id=ahci \
    -drive "if=none,id=gnu,format=raw,snapshot=on,file=$disk" \
    -device ide-hd,drive=gnu,bus=ahci.0 \
    -drive "if=none,id=kvm,format=raw,snapshot=on,file=$data" \
    -device ide-hd,drive=kvm,bus=ahci.1 \
    -serial "file:$log" &
qemu=$!
wait "$qemu"
qemu=
result=$(grep -a '^kvm-corpus: result: ' "$log" | tail -n 1)
echo "${result:-linuxhost: result: fail: no result line in $log}"
case $result in
    'kvm-corpus: result: pass'*) exit 0 ;;
esac
exit 1
