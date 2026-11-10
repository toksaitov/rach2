#!/bin/sh
#
# kvm-boot.sh: boot a guest in QEMU on Rach2 to the token it prints on its
# serial console, and fail it when QEMU ends without the token or the
# console stays silent too long.
#
# Usage: kvm-boot.sh [-a ACCEL] [-I IRQCHIP] [-c CPU] [-m MIB] [-s CPUS]
#                   [-t TOKEN] [-w SECONDS] [-o LOG]
#                   (-k KERNEL [-i INITRD] [-l ARGS] | -d DISK)
#                   [-- QEMU-ARGUMENT...]
#
# ACCEL is `kvm`, the default, which runs the guest under Rach2's KVM with
# the in-kernel interrupt controller, or `tcg`, QEMU's emulator. IRQCHIP is
# `on` by default; `split` keeps the PIC, IOAPIC and PIT in QEMU, and `off`
# keeps the local APIC there too. IRQCHIP applies only under KVM. CPU is
# QEMU's processor model with its features, `host` under KVM
# and `max` under TCG by default; MIB the guest's memory, 256 by default;
# CPUS its processors, 1 by default. The guest is KERNEL with INITRD, booted
# with `console=ttyS0 panic=-1` and ARGS on its command line, or the raw
# DISK, booted by SeaBIOS, which QEMU's snapshot keeps unchanged. TOKEN is
# the text that ends the boot, `KVMGUEST OK` by default, which the Debian
# guest of guests/ prints. The guest's console goes to LOG, by default
# kvm-boot.log in the working directory, and QEMU's own messages to LOG.err;
# QEMU-ARGUMENTs after `--` go to QEMU as they are. QEMU is the program QEMU
# names, qemu-system-x86_64 by default.
#
# The runner reads the console once a second. The token passes the boot,
# after which the guest has as long again to power off before QEMU is
# stopped; QEMU's end without the token, or SECONDS without a byte on the
# console, 600 by default, fails it. That bound is on silence, not on the
# boot's length: a guest that writes goes on as long as it writes. It prints
# the console, then `kvm-boot: result: pass: TOKEN` or
# `kvm-boot: result: fail: REASON`, and exits 0 on a pass, 1 on a failure and
# 2 on a usage error.

accel=kvm irqchip=on cpu='' mib=256 cpus=1 token='KVMGUEST OK' silence=600 log=kvm-boot.log
kernel='' initrd='' args='' disk=''
usage()
{
    echo "usage: kvm-boot.sh [-a ACCEL] [-I IRQCHIP] [-c CPU] [-m MIB] [-s CPUS] [-t TOKEN] [-w SECONDS]" >&2
    echo "                   [-o LOG] (-k KERNEL [-i INITRD] [-l ARGS] | -d DISK) [-- ARG...]" >&2
    exit 2
}
while getopts a:I:c:m:s:t:w:o:k:i:l:d: o; do
    case $o in
        a) accel=$OPTARG ;;
        I) irqchip=$OPTARG ;;
        c) cpu=$OPTARG ;;
        m) mib=$OPTARG ;;
        s) cpus=$OPTARG ;;
        t) token=$OPTARG ;;
        w) silence=$OPTARG ;;
        o) log=$OPTARG ;;
        k) kernel=$OPTARG ;;
        i) initrd=$OPTARG ;;
        l) args=$OPTARG ;;
        d) disk=$OPTARG ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
if [ -n "$kernel" ] && [ -n "$disk" ] || [ -z "$kernel$disk" ]; then
    usage
fi
case $silence in
    '' | *[!0-9]*) usage ;;
esac
case $irqchip in
    on | split | off) ;;
    *) usage ;;
esac
case $accel in
    kvm)
        accel_option=kvm,kernel-irqchip=$irqchip
        cpu=${cpu:-host}
        ;;
    tcg)
        accel_option=tcg
        cpu=${cpu:-max}
        ;;
    *) usage ;;
esac
qemu=${QEMU:-qemu-system-x86_64}

if [ -n "$kernel" ]; then
    set -- -kernel "$kernel" ${initrd:+-initrd "$initrd"} \
        -append "console=ttyS0 panic=-1${args:+ $args}" "$@"
else
    set -- -drive "file=$disk,format=raw,if=ide,snapshot=on" "$@"
fi

pid=
trap '[ -z "$pid" ] || kill "$pid" 2>/dev/null' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
: > "$log"
echo "kvm-boot: start: $qemu -accel $accel_option -cpu $cpu -m $mib -smp $cpus" \
    "${kernel:-$disk}"
"$qemu" -nodefaults -no-reboot -display none -accel "$accel_option" -cpu "$cpu" \
    -m "$mib" -smp "$cpus" -serial "file:$log" "$@" 2> "$log.err" < /dev/null &
pid=$!

# Read the console until the token, QEMU's end or the silence bound.
seen=0 quiet=0 waited=0 status='' reason=''
while :; do
    if [ "$seen" = 0 ] && grep -q -F -e "$token" "$log"; then
        seen=1 quiet=0
    fi
    if ! kill -0 "$pid" 2>/dev/null; then
        wait "$pid"
        status=$?
        pid=
        grep -q -F -e "$token" "$log" && seen=1
        break
    fi
    size=$(wc -c < "$log")
    if [ "$size" != "${last:-}" ]; then
        last=$size quiet=0
    else
        quiet=$((quiet + 1))
    fi
    if [ "$quiet" -ge "$silence" ]; then
        if [ "$seen" = 1 ]; then
            reason="the guest did not power off; QEMU stopped"
        else
            reason="the console was silent for $silence s"
        fi
        kill "$pid" 2>/dev/null
        wait "$pid" 2>/dev/null
        pid=
        break
    fi
    sleep 1
    waited=$((waited + 1))
done

cat "$log"
if [ "$seen" = 1 ]; then
    [ -z "$reason" ] || echo "kvm-boot: $reason"
    echo "kvm-boot: result: pass: $token after $waited s"
    exit 0
fi
[ -n "$reason" ] || reason="QEMU exited with $status before the token: $(tail -n 1 "$log.err")"
echo "kvm-boot: result: fail: $reason"
exit 1
