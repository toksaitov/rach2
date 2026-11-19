#!/bin/sh
# netguest.sh: give Rach2's wired card to a gateway guest, reach the network
# through the guest over a tap link, and take the card back.
#
# Usage: netguest.sh -c BDF -i IMAGE [-n IFACE] [-a kvm|tcg] [-m MIB]
#                    [-s CPUS] [-r RULES] [-F 0|1] [-f PORT] [-V]
#                    [-p PEER] [-P PORT] [-N NETTPUT] [-o OUT] [-d SECONDS]
#                    [-k|-q] [-- COMMAND...]
#
# Run as root on Rach2's GNU root. BDF is the card, as 0000:01:00.0, which
# one of Rach2's drivers has, and IFACE its interface, the one whose device
# link in /sys/class/net is the card when -n does not name it. IMAGE is a
# gateway image that prepare.sh made, FreeBSD's or Debian's; the guest boots
# from a new overlay of it, which is thrown away.
#
# The script takes the interface's address off and sets it down, lets the
# card go from its driver and gives it to vfio-pci, and boots the guest in
# QEMU under ACCEL (kvm, Rach2's KVM, by default) with MIB of memory (512)
# and CPUS processors (1): the card through VFIO as the guest's outer
# interface, and a virtio network card on a tap interface of Rach2, tap0, as
# its inner one, with no mergeable receive buffers, since with them FreeBSD
# 15.1's vtnet gives every receive buffer a 4 KiB cluster and runs out of
# them. With -V the tap's queues go to /dev/vhost-net, the kernel's backend,
# and QEMU moves no frame itself. The guest's parameters, which its boot
# script reads from its second disk, are RULES filler rules (0), the filter
# on or, with -F 0, forwarding and address translation alone (1), and the
# port it forwards to Rach2's port 22 (22). Rach2 takes 10.77.0.2/24 on
# tap0, the guest has 10.77.0.1, and Rach2 routes PEER, the machine that
# runs `nettput -s` on PORT (5201), through the guest.
#
# Once the guest prints its ready line on its serial console and, with a
# PEER, NETTPUT (nettput beside this script, else on PATH) has had an answer
# from PEER through it, COMMAND runs with NETGUEST_QEMU_PID, NETGUEST_OUT,
# NETGUEST_PEER and NETGUEST_PORT set. Then the guest ends, in
# one of three ways: it powers itself off when QEMU's monitor presses its
# power button, so that its driver shuts the card down as it would on any
# machine; with -q QEMU quits at SIGTERM under a running guest; with -k QEMU
# is killed, the guest's death. Then the card goes back: vfio-pci lets it
# go, Rach2's driver takes it and the interface gets its addresses, the
# first still its primary, and its default routes, metrics and all, again,
# each step read back and tried three times before the run fails. The waits
# judge nothing by the clock: a guest that never readies, or never powers
# off, is ended by the caller's bound, and the card comes back on that way
# out too.
#
# The card goes back on every way out: at the end, after a failure, at a
# signal, and, with -d, once SECONDS have passed since the start, which is
# what brings back a machine whose only link is the card. Every process the
# script started is stopped first.
#
# The kernel's counts are printed at each step as `netguest: counts: STEP
# NAME=VALUE...`, with STEP one of start, given, ready, measured and back:
# of /proc/stat kvm_vms, kvm_vcpus, kvm_run_mappings, kvm_exits,
# kvm_irqfd_raises, kvm_ioeventfd_signals, vfio_groups, vfio_devices,
# vfio_dma_mappings, vfio_dma_pages, vfio_dma_io_pages, iommu_dma_faults,
# iommu_irq_faults, msi_vectors_free, vhost_net_detaches,
# vhost_net_detached_to_guest and vhost_net_detached_from_guest, and of
# /proc/net/dev tap_rx_bytes, tap_tx_bytes, tap_rx_packets and
# tap_tx_packets, tap0's. With -V the frames the backend moved, what the
# vhost_net_detached_ counts grew by from the start to the guest's end, are
# printed as `netguest: vhost: to_guest=N from_guest=M`; one attachment
# must have ended meanwhile, tap0's, so that the frames are its own.
#
# The run passes when the guest's ready line names the filter asked for,
# QEMU was still running when its end was asked and ended as asked, the card
# is back with its driver and its interface has its addresses and default
# routes again, PEER answers on it by a connection bound to that interface,
# the VFIO and KVM counts and the free interrupt vectors are what they were
# at the start, no IOMMU fault was counted, COMMAND exited 0, the power
# button's end came with the guest's own power-off on its console, and with
# -V QEMU's stderr has no line on vhost, which QEMU writes when it falls
# back to moving the frames itself. The last line is `netguest: result:
# pass` or `netguest: result: fail: REASON`. OUT, a new directory
# under TMPDIR unless -o names one, keeps the guest's console and QEMU's
# messages; a new one is removed after a pass and named after a failure, as
# `netguest: evidence: OUT`. Exit status 0, 1, or 2 for a usage error.

bdf='' image='' iface='' accel=kvm mib=512 cpus=1 rules=0 filter=1 fwd=22 vhost=0
peer='' port=5201 nettput='' out='' deadline=0 death=0 quit=0
usage()
{
    echo "usage: netguest.sh -c BDF -i IMAGE [-n IFACE] [-a kvm|tcg] [-m MIB] [-s CPUS]" >&2
    echo "                   [-r RULES] [-F 0|1] [-f PORT] [-V] [-p PEER] [-P PORT]" >&2
    echo "                   [-N NETTPUT] [-o OUT] [-d SECONDS] [-k|-q] [-- COMMAND...]" >&2
    exit 2
}
while getopts c:i:n:a:m:s:r:F:f:Vp:P:N:o:d:kq o; do
    case $o in
        c) bdf=$OPTARG ;;
        i) image=$OPTARG ;;
        n) iface=$OPTARG ;;
        a) accel=$OPTARG ;;
        m) mib=$OPTARG ;;
        s) cpus=$OPTARG ;;
        r) rules=$OPTARG ;;
        F) filter=$OPTARG ;;
        f) fwd=$OPTARG ;;
        V) vhost=1 ;;
        p) peer=$OPTARG ;;
        P) port=$OPTARG ;;
        N) nettput=$OPTARG ;;
        o) out=$OPTARG ;;
        d) deadline=$OPTARG ;;
        k) death=1 ;;
        q) quit=1 ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
[ -n "$bdf" ] && [ -n "$image" ] || usage
case $image in /*) ;; *) image=$PWD/$image ;; esac
for n in "$mib" "$cpus" "$rules" "$fwd" "$port" "$deadline"; do
    case $n in
        '' | *[!0-9]*) usage ;;
    esac
done
case $accel in
    kvm) cpu=host ;;
    tcg) cpu=max ;;
    *) usage ;;
esac
case $filter in 0 | 1) ;; *) usage ;; esac
netdev=,vnet_hdr=off,vhost=off
[ "$vhost" = 0 ] || netdev=,vnet_hdr=off,vhost=on

inner_mac=52:54:00:77:00:01
guest_ip=10.77.0.1
own_ip=10.77.0.2
tap=tap0
sys=/sys/bus/pci/devices/$bdf

# What must be undone: the card with vfio-pci, the interface without its
# address, and the processes.
given=0 stripped=0 qemu='' watchdog='' waiter='' made='' job='' new_out=0 passed=0
owner='' addrs='' routes='' start_counts='' launched=0
# The counts that must be as at the start once the guest is gone.
kept='kvm_vms kvm_vcpus kvm_run_mappings vfio_groups vfio_devices vfio_dma_mappings
vfio_dma_pages vfio_dma_io_pages iommu_dma_faults iommu_irq_faults msi_vectors_free'
# The counts that must be numbers at the start.
needed="$kept vhost_net_detaches vhost_net_detached_to_guest vhost_net_detached_from_guest"

# counts: print the counts the header comment names, as NAME=VALUE words.
counts()
{
    {
        cat /proc/stat
        awk -v t="$tap:" '$1 == t {
            print "tap_rx_bytes", $2; print "tap_rx_packets", $3
            print "tap_tx_bytes", $10; print "tap_tx_packets", $11 }' /proc/net/dev
    } | awk '$1 ~ /^(kvm_vms|kvm_vcpus|kvm_run_mappings|kvm_exits|kvm_irqfd_raises|kvm_ioeventfd_signals|vfio_groups|vfio_devices|vfio_dma_mappings|vfio_dma_pages|vfio_dma_io_pages|iommu_dma_faults|iommu_irq_faults|msi_vectors_free|vhost_net_detaches|vhost_net_detached_to_guest|vhost_net_detached_from_guest|tap_rx_bytes|tap_rx_packets|tap_tx_bytes|tap_tx_packets)$/ {
            printf "%s%s=%s", sep, $1, $2; sep = " " } END { print "" }'
}

# count NAME WORDS: print the value WORDS, a line of `counts`, gives NAME.
count()
{
    echo "$2" | tr ' ' '\n' | sed -n "s/^$1=//p"
}

# addrs_of IF: print IF's IPv4 addresses with their prefixes, the primary
# first, as words.
addrs_of()
{
    ip -4 -o addr show dev "$1" | awk '{ printf "%s%s", sep, $4; sep = " " } END { print "" }'
}

# routes_of IF: print IF's default routes as GATEWAY/METRIC words, GATEWAY
# - for a route by the interface alone.
routes_of()
{
    ip -4 route show default dev "$1" | awk '$1 == "default" { g = "-"; m = 0
        for (i = 1; i < NF; i++) { if ($i == "via") g = $(i + 1); if ($i == "metric") m = $(i + 1) }
        printf "%s%s/%s", sep, g, m; sep = " " } END { print "" }'
}

# route_words IF ROUTE: print the words of `ip route` that name the default
# route ROUTE, a GATEWAY/METRIC word, of IF.
route_words()
{
    if [ "${2%/*}" = - ]; then
        echo "default dev $1 metric ${2#*/}"
    else
        echo "default via ${2%/*} dev $1 metric ${2#*/}"
    fi
}

# put_addrs IF ADDRS: give IF the addresses ADDRS, the first its primary,
# taking off what it has first when another is its primary; each read back.
put_addrs()
{
    now=$(addrs_of "$1")
    if [ -n "$now" ] && [ "${now%% *}" != "${2%% *}" ]; then
        for x in $now; do
            ip addr del "$x" dev "$1"
        done
    fi
    for x in $2; do
        ip addr add "$x" dev "$1" 2> /dev/null
    done
    now=$(addrs_of "$1")
    [ "${now%% *}" = "${2%% *}" ] || return 1
    for x in $2; do
        case " $now " in *" $x "*) ;; *) return 1 ;; esac
    done
}

# put_routes IF ROUTES: give IF the default routes ROUTES, GATEWAY/METRIC
# words, each read back.
put_routes()
{
    for r in $2; do
        # shellcheck disable=SC2046 # the route's words
        ip route add $(route_words "$1" "$r") 2> /dev/null
    done
    now=$(routes_of "$1")
    for r in $2; do
        case " $now " in *" $r "*) ;; *) return 1 ;; esac
    done
}

# children_of PIDS: print the processes whose parent is one of PIDS.
children_of()
{
    cat /proc/[0-9]*/stat 2> /dev/null |
        awk -v p=" $1 " '{ s = $0; sub(/^.*\) /, "", s); split(s, f, " ")
            if (index(p, " " f[2] " ")) print $1 }'
}

# kill_tree PID: stop PID and every process under it, each before its
# children are looked for, so that none starts another or is orphaned
# meanwhile, then kill them all and reap PID.
kill_tree()
{
    all='' new=$1
    while [ -n "$new" ]; do
        for x in $new; do
            kill -STOP "$x" 2> /dev/null
        done
        all="$all $new"
        new=$(children_of "$new" | tr '\n' ' ')
    done
    for x in $all; do
        kill -KILL "$x" 2> /dev/null
        kill -CONT "$x" 2> /dev/null
    done
    wait "$1" 2> /dev/null
}

# alive PID: succeed while PID runs, a zombie counting as ended.
alive()
{
    s=$(sed -n 's/^.*) \([A-Z]\).*/\1/p' "/proc/$1/stat" 2> /dev/null)
    case $s in '' | Z | X) return 1 ;; esac
}

# driver_now: print the name of the driver that has the card, or nothing.
driver_now()
{
    d=$(readlink "$sys/driver" 2> /dev/null || :)
    echo "${d##*/}"
}

# take_back_once: the card from vfio-pci to Rach2's driver, and the
# interface its address and default route. A step counts as done only when
# what it should have changed reads back changed.
take_back_once()
{
    if [ "$given" = 1 ]; then
        if [ "$(driver_now)" = vfio-pci ]; then
            echo "$bdf" > /sys/bus/pci/drivers/vfio-pci/unbind 2> /dev/null ||
                echo "netguest: vfio-pci did not let $bdf go"
        fi
        echo > "$sys/driver_override" 2> /dev/null
        if [ -z "$(driver_now)" ]; then
            echo "$bdf" > "/sys/bus/pci/drivers/$owner/bind" 2> /dev/null ||
                echo "netguest: $owner did not take $bdf back"
        fi
        [ "$(driver_now)" = "$owner" ] || return 1
        echo "netguest: $bdf is back with $owner"
        given=0
    fi
    if [ "$stripped" = 1 ]; then
        ip link set "$iface" up || return 1
        [ -z "$addrs" ] || put_addrs "$iface" "$addrs" || return 1
        [ -z "$routes" ] || put_routes "$iface" "$routes" || return 1
        stripped=0
        echo "netguest: $iface is up${addrs:+ with $addrs}${routes:+, default routes $routes}"
    fi
    return 0
}

# take_back: take_back_once, three times at most, a second apart.
take_back()
{
    for try in 1 2 3; do
        take_back_once && return 0
        echo "netguest: the card or its interface is not back, attempt $try"
        sleep 1
    done
    return 1
}

# stop_sender: kill the send that traffic.sh's kill left running.
stop_sender()
{
    [ ! -s "$out/sender" ] || kill -KILL "$(cat "$out/sender")" 2> /dev/null
    rm -f "$out/sender"
}

# shellcheck disable=SC2317 # the traps call it
cleanup()
{
    [ -z "$watchdog" ] || kill_tree "$watchdog"
    [ -z "$waiter" ] || kill_tree "$waiter"
    [ -z "$job" ] || kill_tree "$job"
    [ -z "$out" ] || stop_sender
    if [ -n "$qemu" ]; then
        kill -KILL "$qemu" 2> /dev/null
        wait "$qemu" 2> /dev/null
        qemu=''
    fi
    if [ "$launched" = 1 ]; then
        # A child started just before a signal, whose process ID was not
        # kept, QEMU's among them, ends before the card is taken back, with
        # everything under it.
        for c in $(children_of $$); do
            kill_tree "$c"
        done
        wait 2> /dev/null
    fi
    take_back || echo "netguest: result: fail: $bdf or $iface is not back"
    exec 3>&- 4>&-
    [ -z "$made" ] || rm -f "$out/run.qcow2" "$out/param.img" "$out/mon.in" "$out/mon.out"
    [ "$new_out" = 0 ] || [ "$passed" = 0 ] || rm -rf "$out"
}
trap cleanup EXIT
trap 'fail "stopped by a signal or the deadline"' TERM
trap 'fail "stopped by SIGHUP"' HUP
trap 'fail "stopped by SIGINT"' INT

fail()
{
    if [ -f "$out/console" ]; then
        # What the guest said of its devices, then the console's end.
        tr -d '\r' < "$out/console" |
            grep -a -i -E '^(em|lem|igb|re|rl|vtnet|pci|pcib|virtio_pci)[0-9]|e1000|r8169|rtl81|vfio|interrupt storm' |
            head -n 60 | sed 's/^/netguest: console: device: /'
        tr -d '\r' < "$out/console" | tail -n 25 | sed 's/^/netguest: console: /'
        sed 's/^/netguest: qemu: /' "$out/qemu.err" 2> /dev/null
    fi
    [ "$new_out" = 0 ] || echo "netguest: evidence: $out"
    echo "netguest: result: fail: $*"
    exit 1
}

[ "$(id -u)" = 0 ] || fail "run as root"
[ -e "$sys" ] || fail "no function $bdf"
[ -f "$image" ] || fail "no image $image"
[ -e /dev/net/tun ] || fail "no /dev/net/tun: this kernel has no tap device"
[ -e /dev/vfio/vfio ] || fail "no /dev/vfio/vfio"
[ "$vhost" = 0 ] || [ -e /dev/vhost-net ] || fail "no /dev/vhost-net for -V"
if [ -z "$nettput" ]; then
    nettput=$(dirname "$0")/nettput
    [ -x "$nettput" ] || nettput=$(command -v nettput || :)
fi
[ -z "$peer" ] || [ -x "$nettput" ] || fail "no nettput for -p"
if [ -z "$iface" ]; then
    # The interface whose device link in /sys/class/net is the card.
    for n in /sys/class/net/*; do
        d=$(readlink "$n/device" 2> /dev/null)
        [ "${d##*/}" = "$bdf" ] || continue
        iface=${n##*/}
        break
    done
fi
[ -n "$iface" ] || fail "no interface of $bdf in /sys/class/net; name it with -n"
ip link show "$iface" > /dev/null 2>&1 || fail "no interface $iface"
owner=$(driver_now)
[ -n "$owner" ] && [ "$owner" != vfio-pci ] || fail "$bdf has no driver of Rach2's to come back to"
[ -e "/sys/bus/pci/drivers/$owner/unbind" ] || fail "the driver $owner lets no card go"
if [ -z "$out" ]; then
    out=$(mktemp -d "${TMPDIR:-/tmp}/rach2-netguest.XXXXXX") || fail "no scratch directory"
    new_out=1
else
    mkdir -p "$out" || fail "no directory $out"
fi
launched=1
if [ "$deadline" -gt 0 ]; then
    (
        sleep "$deadline"
        kill -TERM $$
    ) > /dev/null 2>&1 &
    watchdog=$!
fi

# in_background COMMAND...: run COMMAND as a child that the traps can stop,
# since a shell runs no trap until its foreground command ends; its status.
in_background()
{
    "$@" &
    waiter=$!
    wait "$waiter"
    rc=$?
    waiter=''
    return "$rc"
}

# reach DEV: ask PEER's nettput until it answers, each connection bound to
# DEV; prints nettput's result line.
reach()
{
    n=0
    while :; do
        n=$((n + 1))
        if "$nettput" -c "$peer" -I "$1" -p "$port" -n 1 wait > /dev/null 2>&1; then
            echo "nettput: result: wait attempts=$n"
            return 0
        fi
    done
}

echo "netguest: card $bdf, driver $owner, interface $iface, guest image ${image##*/}, vhost $vhost"
! ip link show "$tap" > /dev/null 2>&1 || fail "$tap is there before QEMU makes it"
start_counts=$(counts)
echo "netguest: counts: start $start_counts"
for name in $needed; do
    case $(count "$name" "$start_counts") in
        '' | *[!0-9]*) fail "the kernel gives no count $name" ;;
    esac
done

# The guest's disks and its monitor are made while the card is still here.
made=1
printf 'rules=%s opt=none state=1 fwd=%s filter=%s\n' "$rules" "$fwd" "$filter" |
    dd of="$out/param.img" bs=512 count=1 conv=sync 2> /dev/null || fail "no parameter disk"
dd if=/dev/zero of="$out/param.img" bs=512 seek=1 count=127 conv=notrunc 2> /dev/null ||
    fail "no parameter disk"
qemu-img create -q -f qcow2 -F qcow2 -b "$image" "$out/run.qcow2" || fail "no overlay of $image"
rm -f "$out/mon.in" "$out/mon.out"
# This script holds both pipes open, so that a write to the monitor never
# waits for a QEMU that has already exited.
if ! mkfifo "$out/mon.in" "$out/mon.out" || ! exec 3<> "$out/mon.in" 4<> "$out/mon.out"; then
    fail "no pipes for QEMU's monitor"
fi
monitor=pipe:$out/mon

# The interface's addresses and default routes, to put back.
addrs=$(addrs_of "$iface")
routes=$(routes_of "$iface")
echo "netguest: $iface has ${addrs:-no address}${routes:+, default routes $routes}"
stripped=1
for x in $addrs; do
    ip addr del "$x" dev "$iface" || fail "the address $x of $iface did not come off"
done
ip link set "$iface" down || fail "$iface did not go down"

given=1
echo "$bdf" > "/sys/bus/pci/drivers/$owner/unbind" || fail "$owner did not let $bdf go"
echo vfio-pci > "$sys/driver_override" || fail "no driver_override for $bdf"
echo "$bdf" > /sys/bus/pci/drivers/vfio-pci/bind || fail "vfio-pci did not take $bdf"
echo "netguest: counts: given $(counts)"

# shellcheck disable=SC3045 # dash and bash both set the locked memory limit
ulimit -l unlimited
: > "$out/console"
echo "netguest: boot the guest under $accel, $cpus CPUs, $mib MiB, $rules rules, filter $filter"
# The cloud images boot only with a display adapter, so QEMU's VGA is there.
qemu-system-x86_64 -name netguest,debug-threads=on -nodefaults -no-reboot -display none \
    -machine q35 -accel "$accel" -cpu "$cpu" -smp "$cpus" -m "$mib" \
    -drive "file=$out/run.qcow2,if=virtio,format=qcow2" \
    -drive "file=$out/param.img,if=virtio,format=raw,readonly=on" \
    -netdev "tap,id=inner,ifname=$tap,script=no,downscript=no$netdev" \
    -device "virtio-net-pci,netdev=inner,mac=$inner_mac,disable-legacy=on,mrg_rxbuf=off" \
    -device "vfio-pci,host=$bdf" -device VGA \
    -serial "file:$out/console" -monitor "$monitor" 2> "$out/qemu.err" < /dev/null &
qemu=$!

# QEMU makes the tap interface; Rach2's end of the link is configured here.
until ip link show "$tap" > /dev/null 2>&1; do
    kill -0 "$qemu" 2> /dev/null || fail "QEMU ended before it made $tap"
    sleep 0.2
done
ip addr add "$own_ip/24" dev "$tap" || fail "no address on $tap"
ip link set "$tap" up || fail "$tap did not come up"
if [ -n "$peer" ]; then
    ip route add "$peer/32" via "$guest_ip" dev "$tap" || fail "no route to $peer by the guest"
fi

while ! grep -a -q 'NETGUEST READY' "$out/console"; do
    if grep -a -q 'NETGUEST FAILED' "$out/console"; then
        fail "the guest reported $(grep -a 'NETGUEST FAILED' "$out/console" | tr -d '\r' | tail -n 1)"
    fi
    kill -0 "$qemu" 2> /dev/null || fail "QEMU ended before the guest was ready"
    sleep 0.5
done
ready=$(grep -a 'NETGUEST READY' "$out/console" | tr -d '\r' | tail -n 1 | sed 's/^.*NETGUEST //')
echo "netguest: guest: $ready"
case " $ready " in
    *" filter=$filter "*) ;;
    *) fail "the guest is ready with another filter than filter=$filter" ;;
esac
grep -a 'NETGUEST LOADED' "$out/console" | tr -d '\r' | tail -n 1 | sed 's/^.*NETGUEST /netguest: guest: /'
echo "netguest: counts: ready $(counts)"

# The guest's address translation may still be settling: the peer is asked
# until it answers through the guest, each attempt counted.
if [ -n "$peer" ]; then
    in_background reach "$tap" > "$out/first" 2>&1 ||
        fail "$peer did not answer through the guest: $(tail -n 1 "$out/first")"
    echo "netguest: first answer through the guest: $(sed -n 's/^nettput: result: //p' "$out/first")"
fi

status=0
if [ $# -gt 0 ]; then
    echo "netguest: run: $*"
    NETGUEST_QEMU_PID=$qemu NETGUEST_OUT=$out NETGUEST_PEER=$peer NETGUEST_PORT=$port "$@" &
    job=$!
    wait "$job" || status=$?
    job=''
    echo "netguest: the command exited $status"
fi
echo "netguest: guest: $(grep -a -c 'NETGUEST STATS' "$out/console") reports"
grep -a 'NETGUEST STATS' "$out/console" | tr -d '\r' | tail -n 1 | sed 's/^.*NETGUEST STATS/netguest: guest: stats/'
echo "netguest: counts: measured $(counts)"

# The guest ends, and the card comes back. QEMU must still be running, and
# end as it was asked: by itself, 0, or at SIGKILL, 137.
kill -0 "$qemu" 2> /dev/null || fail "QEMU ended before the guest was ended"
want=0
if [ "$death" = 1 ]; then
    if [ -f "$out/sender" ]; then
        alive "$(cat "$out/sender")" || fail "the send ended before QEMU was killed"
    fi
    echo "netguest: end: QEMU is killed under the running guest"
    kill -KILL "$qemu"
    want=137
elif [ "$quit" = 1 ]; then
    echo "netguest: end: QEMU quits under the running guest"
    kill -TERM "$qemu"
else
    echo "netguest: end: the guest's power button is pressed"
    seen=$(wc -c < "$out/console")
    echo system_powerdown >&3
fi
ended=0
wait "$qemu" 2> /dev/null || ended=$?
qemu=''
stop_sender
take_back || fail "$bdf or $iface did not come back"
[ "$ended" = "$want" ] || fail "QEMU exited $ended, not $want"
if [ "$death" = 0 ] && [ "$quit" = 0 ]; then
    # FreeBSD's and Linux's last words at a power-off by ACPI.
    tail -c +$((seen + 1)) "$out/console" | tr -d '\r' | grep -a -q -E 'Powering system off|reboot: Power down' ||
        fail "the guest's console shows no power-off after the power button"
fi
back=$(counts)
echo "netguest: counts: back $back"
if [ -n "$peer" ]; then
    in_background reach "$iface" > "$out/back" 2>&1 ||
        fail "$peer did not answer on $iface after the card came back: $(tail -n 1 "$out/back")"
    echo "netguest: first answer on $iface: $(sed -n 's/^nettput: result: //p' "$out/back")"
fi
[ "$(driver_now)" = "$owner" ] || fail "$bdf is not back with $owner"

for name in $kept; do
    was=$(count "$name" "$start_counts")
    now=$(count "$name" "$back")
    [ "$was" = "$now" ] || fail "$name is $now after the guest, $was before it"
done
if [ "$vhost" = 1 ]; then
    if grep -i vhost "$out/qemu.err" > /dev/null 2>&1; then
        fail "QEMU moved the frames itself: $(grep -i vhost "$out/qemu.err" | head -n 1)"
    fi
    ends=$(($(count vhost_net_detaches "$back") - $(count vhost_net_detaches "$start_counts")))
    [ "$ends" = 1 ] || fail "$ends vhost-net attachments ended during the boot, not tap0's alone"
    to=$(($(count vhost_net_detached_to_guest "$back") - $(count vhost_net_detached_to_guest "$start_counts")))
    from=$(($(count vhost_net_detached_from_guest "$back") - $(count vhost_net_detached_from_guest "$start_counts")))
    echo "netguest: vhost: to_guest=$to from_guest=$from"
fi
[ "$status" = 0 ] || fail "the command exited $status"
passed=1
echo "netguest: result: pass"
