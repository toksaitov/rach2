#!/bin/sh
# Kill QEMU during a guest boot and count its KVM resources back.
# Usage: leak.sh -k KERNEL -i INITRD [-- QEMU-ARGUMENT...]

kernel='' initrd='' work='' pid='' keep=0
trap '[ -z "$pid" ] || { kill "$pid" 2>/dev/null; wait "$pid" 2>/dev/null; }; [ "$keep" = 1 ] || [ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
while getopts k:i: option; do
    case $option in
        k) kernel=$OPTARG ;;
        i) initrd=$OPTARG ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
if [ -z "$kernel" ] || [ -z "$initrd" ]; then
    echo 'usage: leak.sh -k KERNEL -i INITRD [-- QEMU-ARGUMENT...]' >&2
    exit 2
fi

# The seven KVM counters include references held by run and guest-page maps.
counts()
{
    awk '$1 ~ /^kvm_(vms|vcpus|slots|tdp_pages|run_mappings|gpa_maps|gpa_map_pages)$/ {
        print $1, $2
    }' /proc/stat
}
fail()
{
    echo "kvm-leak: result: fail: $1" >&2
    echo "kvm-leak: evidence: $work" >&2
    keep=1
    exit 1
}

work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-kvm-leak.XXXXXX") || exit 1
before=$(counts) || fail 'cannot read initial counts'
[ "$(printf '%s\n' "$before" | wc -l)" = 7 ] || fail 'missing KVM counters'
qemu=${QEMU:-qemu-system-x86_64}
"$qemu" -nodefaults -no-reboot -display none -accel kvm,kernel-irqchip=split \
    -cpu host -m 256 -smp 1 -kernel "$kernel" -initrd "$initrd" \
    -append 'console=ttyS0 panic=-1' -serial "file:$work/console" "$@" \
    > "$work/stdout" 2> "$work/stderr" < /dev/null &
pid=$!
while ! grep -q 'Linux version' "$work/console" 2>/dev/null; do
    if ! kill -0 "$pid" 2>/dev/null; then
        wait "$pid"
        pid=
        fail 'QEMU ended before the guest started'
    fi
    sleep 1
done
echo 'kvm-leak: guest: running'
counts > "$work/live"
awk '$1 == "kvm_vms" || $1 == "kvm_vcpus" || $1 == "kvm_slots" ||
     $1 == "kvm_tdp_pages" || $1 == "kvm_run_mappings" {
         if ($2 == 0) exit 1; n++
     } END { if (n != 5) exit 1 }' "$work/live" || fail 'guest has no live KVM resources'
awk '$1 == "VmPin:" { found=1; if ($2 != 0) exit 1 }
     END { if (!found) exit 1 }' "/proc/$pid/status" || fail 'QEMU pinned a page'
old_pid=$pid
kill -KILL "$pid" || fail 'cannot kill QEMU'
wait "$pid"
status=$?
pid=
[ "$status" = 137 ] || fail "QEMU ended with $status, not SIGKILL"
[ ! -d "/proc/$old_pid" ] || fail 'QEMU still owns its address space'
after=$(counts) || fail 'cannot read final counts'
[ "$before" = "$after" ] || fail 'KVM resources did not return to their initial counts'
echo 'kvm-leak: result: pass'
