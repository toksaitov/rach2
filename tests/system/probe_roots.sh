#!/bin/sh
# Run entry 51's probes with the same fixtures on either root type.
# Usage: probe_roots.sh KERNEL OUTPUT_PREFIX krffs|ext2 [PROBE...]
set -eu

repo=$(CDPATH='' cd -- "$(dirname -- "$0")/../.." && pwd)
cd "$repo"
kernel=$1
prefix=$2
root=$3
shift 3
[ "$#" -gt 0 ] || set -- shelltest limitprobe dirprobe unlinkdirprobe mvtest \
    vfsprobe dnstest devprobe orphanprobe modeprobe
mkdir -p "$(dirname "$prefix")"
child=

stop_child()
{
    if [ -n "$child" ]; then
        kill -TERM "-$child" 2>/dev/null || :
        wait "$child" 2>/dev/null || :
        while kill -0 "-$child" 2>/dev/null; do
            sleep 1
        done
        child=
    fi
}
trap stop_child EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

case "$root" in
    krffs)
        cp build/images/test.krffs "$prefix.krffs"
        for probe in "$@"; do
            build/krffs/readfile.krffs "$prefix.krffs" "/$probe" "$prefix.$probe.staged"
            cmp "build/tests/$probe" "$prefix.$probe.staged"
        done
        ;;
    ext2)
        # shellcheck source=tools/setup/e2fsprogs_path.sh
        . "$repo/tools/setup/e2fsprogs_path.sh"
        sh tools/image/mkext2.sh krffs/manifests/test.manifest "$prefix.vol" 16
        for probe in "$@"; do
            debugfs -R "dump /$probe $prefix.$probe.staged" "$prefix.vol" \
                >"$prefix.$probe.extract.log" 2>&1
            cmp "build/tests/$probe" "$prefix.$probe.staged"
        done
        dd if=/dev/zero of="$prefix.disk" bs=1048576 count=1 2>/dev/null
        cat "$prefix.vol" >>"$prefix.disk"
        build/tools/image/mkmbr --lba "$prefix.disk" 2048:32768:83
        ;;
    *) echo 'probe-roots: root must be krffs or ext2' >&2; exit 2 ;;
esac
cksum "$kernel" "$prefix".* >"$prefix.artifacts"

for probe in "$@"; do
    case "$root" in
        krffs) set -- --initrd "$prefix.krffs" --append rach2.rescue ;;
        ext2) set -- --drive "file=$prefix.disk,format=raw,if=ide,index=0,media=disk" \
            --append 'rach2.root=ext2 rach2.rescue' ;;
    esac
    build/tests/system/runutil group build/tests/system/harness \
        --kernel "$kernel" "$@" --qemu-arg=-cpu --qemu-arg=max \
        --net hubport,id=net0,hubid=0 --timeout 90 --steptmo 60 \
        --ok 'exited with status 0, returning to kernel shell' \
        --log "$prefix.$probe.serial.log" \
        "expect=Type 'help'" "send=exec /$probe" \
        'expect=returning to kernel shell' >"$prefix.$probe.log" 2>&1 &
    child=$!
    status=0
    wait "$child" || status=$?
    stop_child
    cat "$prefix.$probe.log" "$prefix.$probe.serial.log"
    [ "$status" -eq 0 ] || {
        echo "probe-roots: failed: $root $probe status $status" >&2; exit 1;
    }
    grep -Eq '(^|] )sys_exit: init process [0-9]+ exited with status 0, returning to kernel shell' \
        "$prefix.$probe.serial.log" || {
        echo "probe-roots: missing init exit: $root $probe" >&2; exit 1;
    }
    echo "probe-roots: result: $root $probe pass"
done
