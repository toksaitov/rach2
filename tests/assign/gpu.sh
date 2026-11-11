#!/bin/bash
#
# gpu.sh: give a function that Rach2's Intel graphics driver has to a guest
# of QEMU through vfio-pci, and take it back when QEMU ends.
#
# Usage: gpu.sh [-d BDF] [-f seabios|uefi] [-a kvm|tcg] [-c CPU] [-m MIB]
#               [-s CPUS] [-t TOKEN] [-K TOKEN] [-w SECONDS] [-o DIR] [-v]
#               (-k KERNEL [-i INITRD] [-l ARGS] | -D DISK)
#               [-- QEMU-ARGUMENT...]
#
# BDF is the function, 0000:00:02.0 by default, the Lenovo's GPU. The script
# runs as root on Rach2. It writes the function's name to
# /sys/bus/pci/drivers/i915/unbind, which turns the display off and resets
# the GPU; gives the function to vfio-pci through driver_override and
# vfio-pci's bind; and starts QEMU as the run on Linux did
# (dev-log/76-graphics/research/run2/step11.md):
#
#   qemu-system-x86_64 -machine q35,kernel-irqchip=split -nodefaults -vga none
#       -device vfio-pci,host=BDF,bus=pcie.0,addr=02.0,x-igd-opregion=on
#
# with x-igd-opregion=on only for an Intel display function, which alone
# has an OpRegion, by the ids /sys shows once the driver has let it go;
# while the driver has it, /sys shows the ids the driver reports, which for
# its fake chip in QEMU are a display's. The guest has no other display, so
# its console is the serial port, kept in DIR/console; with -v, a function
# that is not a display has QEMU's standard VGA beside it, for a guest whose
# boot needs a display adapter.
#
# The firmware is SeaBIOS, the default, or UEFI, QEMU's OVMF from OVMF_CODE
# and OVMF_VARS or Debian's paths. The guest is KERNEL with INITRD, which
# QEMU loads itself and boots with `console=ttyS0 panic=-1` and ARGS, or the
# raw DISK with a boot loader of its own, on the machine's AHCI controller,
# which QEMU's snapshot keeps unchanged. ACCEL is `kvm`, the default, with
# CPU `host`, or `tcg` with CPU `max`; MIB is the guest's memory, 2048 by
# default, all of which VFIO pins, and CPUS its processors, 2 by default.
#
# The script reads the console once a second. TOKEN, when given, must appear
# there before QEMU ends. With -K the script kills QEMU with SIGKILL when
# its TOKEN appears, the death under load, and SIGUSR1 to the script does
# the same at once, for a caller that judges the moment by what it sees on
# the host; otherwise it waits for the guest to power off. SECONDS without a
# byte on the console, 600 by default, stops QEMU and fails the run; 0 waits
# for ever, for a guest whose console is quiet while its desktop runs.
#
# When QEMU has ended, however it ended, and on every failure after the
# unbind, the script takes the function back from vfio-pci, restores its
# driver_override and writes its name to /sys/bus/pci/drivers/i915/bind,
# which resets the function and binds the driver again. It then requires the
# driver on the function with a DRM card, its log of the release and the bind,
# no IOMMU fault counted since the start, and the counts of VFIO's groups,
# devices, mappings and pins, of KVM's machines and of the free interrupt
# vectors as they were at the start.
#
# It prints `gpu: ACTION` lines, the console, and `gpu: result: pass: ...`
# or `gpu: result: fail: REASON`, and exits 0 on a pass, 1 on a failure and
# 2 on a usage error. DIR, by default a new directory under TMPDIR that a
# pass removes, keeps the console and QEMU's messages.

set -eu
bdf=0000:00:02.0 firmware=seabios accel=kvm cpu='' mib=2048 cpus=2
token='' kill_token='' silence=600 out='' kernel='' initrd='' args='' disk='' vga=0
usage()
{
    echo "usage: gpu.sh [-d BDF] [-f seabios|uefi] [-a kvm|tcg] [-c CPU] [-m MIB] [-s CPUS]" >&2
    echo "              [-t TOKEN] [-K TOKEN] [-w SECONDS] [-o DIR] [-v]" >&2
    echo "              (-k KERNEL [-i INITRD] [-l ARGS] | -D DISK) [-- ARG...]" >&2
    exit 2
}
while getopts d:f:a:c:m:s:t:K:w:o:k:i:l:D:v o; do
    case $o in
        d) bdf=$OPTARG ;;
        f) firmware=$OPTARG ;;
        a) accel=$OPTARG ;;
        c) cpu=$OPTARG ;;
        m) mib=$OPTARG ;;
        s) cpus=$OPTARG ;;
        t) token=$OPTARG ;;
        K) kill_token=$OPTARG ;;
        w) silence=$OPTARG ;;
        o) out=$OPTARG ;;
        k) kernel=$OPTARG ;;
        i) initrd=$OPTARG ;;
        l) args=$OPTARG ;;
        D) disk=$OPTARG ;;
        v) vga=1 ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
if [ -n "$kernel" ] && [ -n "$disk" ] || [ -z "$kernel$disk" ]; then
    usage
fi
case $silence in '' | *[!0-9]*) usage ;; esac
case $firmware in seabios | uefi) ;; *) usage ;; esac
case $accel in
    kvm) cpu=${cpu:-host} ;;
    tcg) cpu=${cpu:-max} ;;
    *) usage ;;
esac
case $bdf in
    [0-9a-f][0-9a-f][0-9a-f][0-9a-f]:[0-9a-f][0-9a-f]:[0-9a-f][0-9a-f].[0-7]) ;;
    *) usage ;;
esac

dev=/sys/bus/pci/devices/$bdf
i915=/sys/bus/pci/drivers/i915
vfio=/sys/bus/pci/drivers/vfio-pci
pid='' work='' taken=0 override='' failed=0

# The name of the function's driver, or nothing.
driver()
{
    local link
    link=$(readlink "$dev/driver" 2>/dev/null) || :
    echo "${link##*/}"
}
# The function's DRM card, by the name of its directory, or nothing.
card()
{
    local c
    for c in "$dev"/drm/card[0-9]*; do
        [ -d "$c" ] && echo "${c##*/}" && return
    done
    :
}
counts()
{
    awk '$1 ~ /^(vfio_(groups|devices|dma_mappings|dma_pages|dma_io_pages)|msi_vectors_free|kvm_(vms|vcpus|slots|tdp_pages|run_mappings))$/ { print }' /proc/stat
}
faults()
{
    awk '$1 == "iommu_dma_faults" || $1 == "iommu_irq_faults" { print }' /proc/stat
}
# The driver's newest handoff line for the function, with its time stamp.
newest()
{
    dmesg | grep "i915: handoff: $bdf: " | tail -n 1
}
# Whether the newest handoff line is ACTION and differs from MARK, the newest
# before it; a count of the lines misses a new one once the kernel's log ring
# has dropped an old one.
logged_since()
{
    local l
    l=$(newest)
    [ "$l" != "$2" ] || return 1
    case $l in *"i915: handoff: $bdf: $1"*) return 0 ;; esac
    return 1
}
# Take the function from vfio-pci, restore its driver_override and bind the
# Intel driver again; each step only where it is still to do, so that it
# runs after a failure at any point. Returns 1 when the driver is not back.
give_back()
{
    if [ "$(driver)" = vfio-pci ]; then
        echo "gpu: unbind: $bdf from vfio-pci"
        if ! echo "$bdf" > "$vfio/unbind"; then
            echo "gpu: unbind: vfio-pci kept $bdf" >&2
        fi
    fi
    if [ -e "$dev/driver_override" ]; then
        echo "$override" > "$dev/driver_override" || :
    fi
    if [ -z "$(driver)" ]; then
        echo "gpu: bind: $bdf to i915"
        if ! echo "$bdf" > "$i915/bind"; then
            echo "gpu: bind: i915 did not take $bdf" >&2
        fi
    fi
    [ "$(driver)" = i915 ]
}
cleanup()
{
    if [ -n "$pid" ]; then
        kill -KILL "$pid" 2>/dev/null || :
        wait "$pid" 2>/dev/null || :
    fi
    if [ "$taken" = 1 ]; then
        if ! give_back; then
            echo "gpu: bind: $bdf is without its driver" >&2
        fi
    fi
    [ -z "$work" ] || [ "$failed" = 1 ] || rm -rf "$work"
}
trap cleanup EXIT
trap 'failed=1; exit 129' HUP
trap 'failed=1; exit 130' INT
trap 'failed=1; exit 143' TERM
stop=0
trap 'stop=1' USR1
fail()
{
    echo "gpu: result: fail: $*" >&2
    echo "gpu: evidence: $out" >&2
    failed=1
    exit 1
}

[ -d "$dev" ] || fail "no function $bdf"
[ -e /dev/vfio/vfio ] || fail 'VFIO is absent'
[ -e "$dev/iommu_group" ] || fail "$bdf is in no IOMMU group"
[ "$(driver)" = i915 ] || fail "the Intel driver does not have $bdf"
[ -n "$(card)" ] || fail "$bdf has no DRM card"
if [ -z "$out" ]; then
    work=$(mktemp -d "${TMPDIR:-/tmp}/rach2-gpu.XXXXXX")
    out=$work
fi
mkdir -p "$out"
: > "$out/console"

# The guest's firmware and what it boots.
if [ "$firmware" = uefi ]; then
    code=${OVMF_CODE:-} vars=${OVMF_VARS:-}
    for f in /usr/share/OVMF/OVMF_CODE_4M.fd /usr/share/OVMF/OVMF_CODE.fd; do
        [ -n "$code" ] || [ ! -f "$f" ] || code=$f
    done
    for f in /usr/share/OVMF/OVMF_VARS_4M.fd /usr/share/OVMF/OVMF_VARS.fd; do
        [ -n "$vars" ] || [ ! -f "$f" ] || vars=$f
    done
    [ -f "$code" ] && [ -f "$vars" ] || fail 'no OVMF firmware; set OVMF_CODE and OVMF_VARS'
    cp "$vars" "$out/vars.fd"
    set -- -drive "if=pflash,format=raw,unit=0,readonly=on,file=$code" \
        -drive "if=pflash,format=raw,unit=1,file=$out/vars.fd" "$@"
fi
if [ -n "$kernel" ]; then
    set -- -kernel "$kernel" ${initrd:+-initrd "$initrd"} \
        -append "console=ttyS0 panic=-1${args:+ $args}" "$@"
else
    set -- -drive "file=$disk,format=raw,if=none,id=guest,snapshot=on" \
        -device ide-hd,drive=guest,bus=ide.0 "$@"
fi

before=$(counts)
faults_before=$(faults)
mark=$(newest)
override=$(cat "$dev/driver_override" 2>/dev/null || :)
[ "$override" != '(null)' ] || override=
card_before=$(card)
echo "gpu: before: $bdf on $card_before; $(echo "$before" | tr '\n' ' ')"
ulimit -l unlimited

echo "gpu: unbind: $bdf from i915"
taken=1
echo "$bdf" > "$i915/unbind" || fail "the Intel driver did not release $bdf"
[ -z "$(driver)" ] || fail "$bdf still has a driver after the unbind"
logged_since released "$mark" || fail 'the driver did not log the release'
mark=$(newest)
echo "gpu: bind: $bdf to vfio-pci"
echo vfio-pci > "$dev/driver_override"
echo "$bdf" > "$vfio/bind" || fail "vfio-pci did not take $bdf"
[ "$(driver)" = vfio-pci ] || fail "$bdf is not vfio-pci's after the bind"
# The function's own ids, which /sys shows once no driver answers for it.
opregion='' display=0
echo "gpu: function: $bdf, vendor $(cat "$dev/vendor"), class $(cat "$dev/class")"
case $(cat "$dev/class") in 0x03*) display=1 ;; esac
[ "$display" = 0 ] || [ "$(cat "$dev/vendor")" != 0x8086 ] || opregion=,x-igd-opregion=on
set -- -device "vfio-pci,host=$bdf,bus=pcie.0,addr=02.0$opregion" "$@"
[ "$vga" = 0 ] || [ "$display" = 1 ] || set -- "$@" -device VGA

qemu=${QEMU:-qemu-system-x86_64}
echo "gpu: start: $qemu -machine q35,kernel-irqchip=split -accel $accel -cpu $cpu -m $mib" \
    "-smp $cpus -nodefaults -vga none $*"
"$qemu" -machine q35,kernel-irqchip=split -accel "$accel" -cpu "$cpu" -m "$mib" -smp "$cpus" \
    -nodefaults -vga none -display none -no-reboot -serial "file:$out/console" "$@" \
    2> "$out/qemu.err" < /dev/null &
pid=$!

# Read the console until QEMU's end, the token or SIGUSR1 that kills it, or
# the bound on its silence.
seen=0 killed=0 quiet=0 waited=0 last='' reason=''
while kill -0 "$pid" 2>/dev/null; do
    if [ "$stop" = 1 ] ||
        { [ -n "$kill_token" ] && grep -q -F -e "$kill_token" "$out/console"; }; then
        if [ "$stop" = 1 ]; then
            echo "gpu: kill: QEMU on SIGUSR1; $(counts | tr '\n' ' ')"
        else
            echo "gpu: kill: QEMU on $kill_token; $(counts | tr '\n' ' ')"
        fi
        kill -KILL "$pid" 2>/dev/null || :
        killed=1
        break
    fi
    size=$(wc -c < "$out/console")
    if [ "$size" != "$last" ]; then
        last=$size quiet=0
    else
        quiet=$((quiet + 1))
    fi
    if [ "$silence" != 0 ] && [ "$quiet" -ge "$silence" ]; then
        reason="the console was silent for $silence s"
        kill -KILL "$pid" 2>/dev/null || :
        break
    fi
    sleep 1
    waited=$((waited + 1))
done
status=0
wait "$pid" || status=$?
pid=
cat "$out/console"
cat "$out/qemu.err" >&2
[ -z "$token" ] || ! grep -q -F -e "$token" "$out/console" || seen=1

# The function goes back whatever the guest did; the verdict comes after.
back=1
give_back || back=0
taken=0
[ -z "$reason" ] || fail "$reason"
if [ "$killed" = 1 ]; then
    [ "$status" = 137 ] || fail "QEMU exited with $status after SIGKILL"
else
    [ "$status" = 0 ] || fail "QEMU exited with $status: $(tail -n 1 "$out/qemu.err")"
fi
[ -z "$token" ] || [ "$seen" = 1 ] || fail "the guest did not print $token"
[ "$back" = 1 ] || fail "the Intel driver did not take $bdf back"
logged_since bound "$mark" || fail 'the driver did not log the bind'
card_after=$(card)
[ -n "$card_after" ] || fail "$bdf has no DRM card after the bind"
[ -e "/dev/dri/$card_after" ] || fail "/dev/dri/$card_after is absent"
dmesg | grep "i915: handoff: $bdf: " | tail -n 2
after=$(counts)
echo "gpu: after: $bdf on $card_after; $(echo "$after" | tr '\n' ' ')"
[ "$(faults)" = "$faults_before" ] || fail "IOMMU faults counted: $(faults | tr '\n' ' ')"
[ "$before" = "$after" ] || fail 'assignment resources leaked'
how=''
[ "$killed" = 0 ] || how=', QEMU killed'
echo "gpu: result: pass: $bdf to the guest and back to i915 after $waited s$how"
