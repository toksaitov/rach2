#!/bin/sh
# run.sh: the netguest suite's cases, run as root on Rach2's GNU root: the
# wired card let go and taken back, and given to each gateway guest, with
# and without vhost-net, each case judged by what the kernel and the guest
# count.
#
# Usage: run.sh [-D] [-B] [-W] [-c BDF | -n IFACE] [-a ADDR/PREFIX]
#               [-d SECONDS] [-o OUT] [-A kvm|tcg] PEER PORT CASE...
#
# PEER is the dotted IPv4 address of the machine that runs `nettput -s` on
# PORT: the host of QEMU's user network under QEMU, the Mac on the Lenovo.
# The card is BDF, or the PCI function of IFACE, as /sys/class/net/IFACE/
# device gives it; ADDR/PREFIX is the address the card's interface takes
# when it has none. The directory of this script holds netguest.sh, nettput
# traffic.sh and the gateway images, freebsd.qcow2 and debian.qcow2; OUT, a new
# directory under TMPDIR unless -o names one, gets each case's directory. A
# new OUT is removed after a pass and named after a failure, as `netguest:
# evidence: OUT`. The guests run under kvm, or tcg with -A.
#
# A CASE is one of
#
#   cycle          the tap device alone, made, addressed and removed by
#                  iproute2; then three times the interface's address off,
#                  the driver letting the card go and taking it back, the
#                  address and default route back, and PEER answering by the
#                  card again; the free interrupt vectors as they were.
#   debian, freebsd
#                  a boot of the guest with its filter and QEMU moving the
#                  tap's frames: through the guest, nettput sends and
#                  receives 8388608 bytes and makes 1000 exchanges and 200
#                  connections, which tap0's byte counts must show each
#                  way; ten attempts at PEER's port 9, which the guest's
#                  deny rule must count.
#   debian-vhost, freebsd-vhost
#                  the same transfers through a boot with translation alone
#                  and the tap's queues on /dev/vhost-net: the backend must
#                  log at least 5541 frames each way, the fewest frames of at
#                  most 1514 bytes that carry 8388608 bytes, and the guest
#                  must count no denied packet.
#   kill           a boot of the Debian guest whose QEMU is killed while
#                  nettput sends through it, once tap0 has carried a
#                  mebibyte of it.
#
# Every boot also passes netguest.sh's own checks: the card back with its
# driver, PEER answering on it, the VFIO and KVM counts and the free
# interrupt vectors as at the start and no IOMMU fault (netguest.sh). No
# check reads the clock; a case that does not end is ended by its caller's
# bound, or, with -d, by netguest.sh's deadline of SECONDS a boot.
#
# On the Lenovo the card is the machine's only link, and three options are
# for that: -D starts the run again as a session of its own, with its
# output in OUT/log, its process ID in OUT/pid and its exit status in
# OUT/status at the end, and returns once that session has started; -W
# takes every IPv4 address and the default route off every other interface
# for the run, the wireless card, and stops its DHCP client, so that PEER
# answers by the card or not at all, and puts them back at the end, a
# failure to do either failing the run; -B reboots the machine when, the
# card back at the end, neither PEER nor the interface's gateway answers
# 2400 attempts, which on a trial boot returns the machine to the installed
# kernel. Every probe of PEER by the card first requires the kernel's
# route to PEER to be by the card's interface.
#
# Lines: netguest.sh's `netguest:` lines; `netguest: case: CASE: pass` or
# `netguest: case: CASE: fail: REASON`; and last, once everything is put
# back, `netguest: result: pass: N cases` or `netguest: result: fail: F of
# N cases failed`, with `, REASON` when putting back failed. Exit status 0
# when every case passed and everything was put back, 1 otherwise, 2 for a
# usage error.

here=$(cd "$(dirname "$0")" && pwd)
detach=0 reboot=0 others=0 bdf='' iface='' own='' deadline=0 out='' accel=kvm
usage()
{
    echo "usage: run.sh [-D] [-B] [-W] [-c BDF | -n IFACE] [-a ADDR/PREFIX] [-d SECONDS]" >&2
    echo "              [-o OUT] [-A kvm|tcg] PEER PORT CASE..." >&2
    exit 2
}
while getopts DBWc:n:a:d:o:A: o; do
    case $o in
        D) detach=1 ;;
        B) reboot=1 ;;
        W) others=1 ;;
        c) bdf=$OPTARG ;;
        n) iface=$OPTARG ;;
        a) own=$OPTARG ;;
        d) deadline=$OPTARG ;;
        o) out=$OPTARG ;;
        A) accel=$OPTARG ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
[ $# -ge 3 ] || usage
peer=$1 port=$2
shift 2
case $port$deadline in '' | *[!0-9]*) usage ;; esac
case $accel in kvm | tcg) ;; *) usage ;; esac
[ -n "$bdf" ] || [ -n "$iface" ] || usage
for c in "$@"; do
    case $c in
        cycle | debian | freebsd | debian-vhost | freebsd-vhost | kill) ;;
        *) usage ;;
    esac
done
# The bytes each way, and the fewest frames of at most 1514 bytes that
# carry them.
bytes=8388608 frames=5541
nettput=$here/nettput

new_out=0
if [ -z "$out" ]; then
    out=$(mktemp -d "${TMPDIR:-/tmp}/rach2-netguest.XXXXXX") || exit 1
    new_out=1
fi
mkdir -p "$out" || exit 1
if [ "$detach" = 1 ]; then
    command -v setsid > /dev/null || {
        echo "netguest: no setsid to detach with" >&2
        exit 1
    }
    rm -f "$out/status" "$out/pid"
    set -- -o "$out" -d "$deadline" -A "$accel" "$peer" "$port" "$@"
    [ -z "$own" ] || set -- -a "$own" "$@"
    [ -z "$iface" ] || set -- -n "$iface" "$@"
    [ -z "$bdf" ] || set -- -c "$bdf" "$@"
    [ "$others" = 0 ] || set -- -W "$@"
    [ "$reboot" = 0 ] || set -- -B "$@"
    setsid nohup sh "$0" "$@" > "$out/log" 2>&1 < /dev/null &
    launched=$!
    until [ -s "$out/pid" ]; do
        if ! kill -0 "$launched" 2> /dev/null && [ ! -s "$out/pid" ]; then
            echo "netguest: the detached run did not start: $(tail -n 1 "$out/log" 2> /dev/null)" >&2
            exit 1
        fi
        sleep 0.2
    done
    echo "netguest: started: process $(cat "$out/pid"), log $out/log, status in $out/status at the end"
    exit 0
fi
echo $$ > "$out/pid"

if [ -z "$bdf" ]; then
    bdf=$(readlink "/sys/class/net/$iface/device" 2> /dev/null)
    bdf=${bdf##*/}
fi
sys=/sys/bus/pci/devices/$bdf
if [ -z "$iface" ]; then
    # The interface whose device link in /sys/class/net is the card.
    for n in /sys/class/net/*; do
        d=$(readlink "$n/device" 2> /dev/null)
        [ "${d##*/}" = "$bdf" ] || continue
        iface=${n##*/}
        break
    done
fi
driver=$(readlink "$sys/driver" 2> /dev/null)
driver=${driver##*/}
failed=0 total=0 child='' tap9=0 link_saved=0 putback='' abort='the run was stopped' touched=0

note()
{
    echo "netguest: $*"
}

# up: the card's interface up, with ADDR/PREFIX when it has no address.
up()
{
    ip link set "$iface" up
    if [ -n "$own" ] && [ -z "$(ip -4 -o addr show dev "$iface")" ]; then
        ip addr add "$own" dev "$iface"
    fi
}

# route_of IF: print the gateway and the metric of IF's default route, as
# `GATEWAY METRIC`, or nothing.
route_of()
{
    ip -4 route show default dev "$1" | awk '{ g = ""; m = 0
        for (i = 1; i < NF; i++) { if ($i == "via") g = $(i + 1); if ($i == "metric") m = $(i + 1) }
        if (g != "") { print g, m; exit } }'
}

# others_off: take every IPv4 address and the default route off every
# other interface that has one, and stop its DHCP client. Each interface is
# recorded in OUT/others, as `IF GATEWAY METRIC PID ADDR...`, before
# anything of it changes; when the record cannot be written, nothing is
# changed and the status is 1.
others_off()
{
    : > "$out/others" || return 1
    for n in /sys/class/net/*; do
        o=${n##*/}
        case $o in "$iface" | lo | tap*) continue ;; esac
        a=$(ip -4 -o addr show dev "$o" | awk '{ printf "%s%s", sep, $4; sep = " " }')
        [ -n "$a" ] || continue
        r=$(route_of "$o")
        p=$(cat "/var/run/udhcpc.$o.pid" 2> /dev/null)
        echo "$o ${r:-- -} ${p:--} $a" >> "$out/others" && sync || return 1
        [ -z "$p" ] || kill -s STOP "$p" 2> /dev/null
        [ -z "$r" ] || ip route del default via "${r% *}" dev "$o" 2> /dev/null
        for x in $a; do
            ip addr del "$x" dev "$o"
        done
        note "others: $o: $a off, default route ${r:-none} off, DHCP client ${p:-none} stopped"
    done
}

# others_on: put back what others_off took, each address and route read
# back; the record is kept, and the status is 1, when anything is not back.
# shellcheck disable=SC2329 # finish calls it
others_on()
{
    [ -s "$out/others" ] || return 0
    back=0
    while read -r o g m p a; do
        for x in $a; do
            ip addr add "$x" dev "$o" 2> /dev/null
            ip -4 -o addr show dev "$o" | grep -q " $x " || back=1
        done
        if [ "$g" != - ]; then
            ip route add default via "$g" dev "$o" metric "$m" 2> /dev/null
            [ "$(route_of "$o")" = "$g $m" ] || back=1
        fi
        [ "$p" = - ] || kill -s CONT "$p" 2> /dev/null
        note "others: $o: $a back"
    done < "$out/others"
    [ "$back" = 0 ] || return 1
    rm -f "$out/others"
}

# link_save: keep the card's interface's address and default route, which
# link_restore puts back.
link_save()
{
    laddr=$(ip -4 -o addr show dev "$iface" | awk '{ print $4; exit }')
    lroute=$(route_of "$iface")
    link_saved=1
}

# link_restore: the card's interface up with the address and the default
# route link_save kept, each read back.
link_restore()
{
    ip link set "$iface" up || return 1
    if [ -n "$laddr" ]; then
        ip addr add "$laddr" dev "$iface" 2> /dev/null
        ip -4 -o addr show dev "$iface" | grep -q " $laddr " || return 1
    fi
    if [ -n "$lroute" ]; then
        ip route add default via "${lroute% *}" dev "$iface" metric "${lroute#* }" 2> /dev/null
        [ "$(route_of "$iface")" = "$lroute" ] || return 1
    fi
    return 0
}

# by_card: succeed when the kernel routes PEER by the card's interface.
by_card()
{
    ip route get "$peer" | grep -q " dev $iface "
}

# answers: succeed when PEER's nettput, or the interface's gateway, answers.
# shellcheck disable=SC2329 # finish calls it
answers()
{
    by_card && "$nettput" -c "$peer" -p "$port" -n 2400 wait > /dev/null 2>&1 && return 0
    gw=$(ip -4 route show default dev "$iface" |
        awk '{ for (i = 1; i < NF; i++) if ($i == "via") { print $(i + 1); exit } }')
    [ -n "$gw" ] && ping -c 3 "$gw" > /dev/null 2>&1
}

# shellcheck disable=SC2317,SC2329 # the traps call it
finish()
{
    status=$?
    trap - EXIT TERM HUP INT
    if [ -n "$child" ]; then
        kill -TERM "$child" 2> /dev/null
        wait "$child" 2> /dev/null
    fi
    # The card goes back to its driver whatever stopped the run, from
    # vfio-pci too.
    if [ -n "$driver" ] && [ "$driver" != vfio-pci ]; then
        now=$(readlink "$sys/driver" 2> /dev/null)
        now=${now##*/}
        if [ "$now" != "$driver" ]; then
            [ -z "$now" ] || echo "$bdf" > "/sys/bus/pci/drivers/$now/unbind" 2> /dev/null
            echo > "$sys/driver_override" 2> /dev/null
            echo "$bdf" > "/sys/bus/pci/drivers/$driver/bind" 2> /dev/null
            now=$(readlink "$sys/driver" 2> /dev/null)
            [ "${now##*/}" = "$driver" ] || putback="$putback, $bdf is not back with $driver"
        fi
    fi
    if [ "$tap9" = 1 ] && ip link show tap9 > /dev/null 2>&1; then
        ip tuntap del dev tap9 mode tap 2> /dev/null || putback="$putback, tap9 is still there"
    fi
    if [ "$link_saved" = 1 ]; then
        link_restore || putback="$putback, $iface is not back with its address and route"
    fi
    if [ "$reboot" = 1 ] && [ "$touched" = 1 ]; then
        up
        if ! answers; then
            note "neither $peer nor the gateway answers on $iface; rebooting"
            echo 1 > "$out/status"
            sync
            reboot
        fi
    fi
    others_on || putback="$putback, another interface is not back, as $out/others records"
    [ -z "$putback" ] || status=1
    if [ "$new_out" = 1 ] && [ "$detach" = 0 ] && [ "$status" != 0 ]; then
        note "evidence: $out"
    fi
    if [ "$failed" = 0 ] && [ "$status" = 0 ]; then
        note "result: pass: $total cases"
    elif [ "$failed" = 0 ]; then
        note "result: fail: 0 of $total cases failed${putback:-, $abort}"
    else
        note "result: fail: $failed of $total cases failed$putback"
    fi
    echo "$status" > "$out/status"
    sync
    [ "$new_out" = 0 ] || [ "$detach" = 1 ] || [ "$status" != 0 ] || rm -rf "$out"
    exit "$status"
}
trap finish EXIT
trap 'exit 143' TERM
trap 'exit 129' HUP
trap 'exit 130' INT

# run COMMAND...: run it as a child that the traps can stop; its status.
run()
{
    "$@" &
    child=$!
    wait "$child"
    rc=$?
    child=''
    return "$rc"
}

# judge CASE REASON: the case passes when REASON is empty.
judge()
{
    total=$((total + 1))
    if [ -z "$2" ]; then
        note "case: $1: pass"
    else
        note "case: $1: fail: $2"
        failed=$((failed + 1))
    fi
    sync
}

# delta NAME LOG FROM TO: what the count NAME grew by in LOG from the
# `netguest: counts: FROM` line to the TO line.
delta()
{
    awk -v n="$1" -v a="netguest: counts: $3 " -v b="netguest: counts: $4 " '
        function value(line,   i, f, k) {
            k = split(line, f, " ")
            for (i = 1; i <= k; i++) if (index(f[i], n "=") == 1) return substr(f[i], length(n) + 2)
            return ""
        }
        index($0, a) == 1 { x = value($0) }
        index($0, b) == 1 { y = value($0) }
        END { if (x == "" || y == "") print -1; else print y - x }' "$2"
}

# cycle: the tap device alone, then the card let go and taken back three
# times.
cycle()
{
    why=''
    tap9=1
    if ip tuntap add dev tap9 mode tap && ip link set tap9 up && ip addr add 10.99.0.1/24 dev tap9; then
        ip -o addr show dev tap9 | sed 's/^/netguest: tap: /'
    else
        why="the tap interface was not made"
    fi
    if ip link show tap9 > /dev/null 2>&1; then
        ip tuntap del dev tap9 mode tap || why="tap9 was not removed"
        ! ip link show tap9 > /dev/null 2>&1 || why="tap9 is still there"
    fi
    [ -n "$why" ] || tap9=0
    up
    was=$(awk '$1 == "msi_vectors_free" { print $2 }' /proc/stat)
    case $was in '' | *[!0-9]*) [ -n "$why" ] || why="the kernel gives no count msi_vectors_free" ;; esac
    link_save
    n=0
    while [ -z "$why" ] && [ "$n" -lt 3 ]; do
        n=$((n + 1))
        [ -z "$laddr" ] || ip addr del "$laddr" dev "$iface"
        ip link set "$iface" down
        echo "$bdf" > "/sys/bus/pci/drivers/$driver/unbind" || why="unbind $n"
        [ ! -e "$sys/driver" ] || why="the card still has a driver after unbind $n"
        echo "$bdf" > "/sys/bus/pci/drivers/$driver/bind" || why="bind $n"
        if ! link_restore; then
            why="cycle $n: $iface did not get its address and default route back"
        elif ! by_card; then
            why="cycle $n: $peer is not routed by $iface"
        elif run "$nettput" -c "$peer" -p "$port" -n 1000000 wait > "$out/cycle-$n" 2>&1; then
            note "cycle $n: $(sed -n 's/^nettput: result: //p' "$out/cycle-$n") after the card came back"
        else
            why="cycle $n: $peer does not answer after the card came back"
        fi
    done
    link_restore || [ -n "$why" ] || why="$iface did not get its address and default route back"
    link_saved=0
    now=$(awk '$1 == "msi_vectors_free" { print $2 }' /proc/stat)
    [ -n "$why" ] || [ "$was" = "$now" ] || why="msi_vectors_free is $now after the cycles, $was before"
    judge cycle "$why"
}

# guest CASE SYSTEM OPTIONS...: one boot of SYSTEM's guest through
# netguest.sh with OPTIONS, the transfers, and the case's judgment.
guest()
{
    name=$1 system=$2
    shift 2
    log=$out/$name.log
    if [ ! -f "$here/$system.qcow2" ]; then
        judge "$name" "no image $system.qcow2"
        return
    fi
    up
    case $name in
        kill) set -- "$@" -k -- sh "$here/traffic.sh" kill ;;
        debian | freebsd) set -- "$@" -- sh "$here/traffic.sh" deny ;;
        *) set -- "$@" -- sh "$here/traffic.sh" transfers ;;
    esac
    run sh "$here/netguest.sh" -c "$bdf" -n "$iface" -i "$here/$system.qcow2" -a "$accel" \
        -p "$peer" -P "$port" -N "$nettput" -o "$out/$name" -d "$deadline" "$@" > "$log" 2>&1
    cat "$log"
    why=''
    tail -n 1 "$log" | grep -q '^netguest: result: pass$' ||
        why=$(tail -n 1 "$log" | sed -n 's/^netguest: result: //p')
    [ -n "$why" ] || [ "$(grep -c '^netguest: result: pass$' "$log")" = 1 ] || why="netguest.sh did not pass"
    deny=$(grep -a 'netguest: guest: stats' "$log" | tail -n 1 | sed -n 's/.*deny9=\([0-9]*\).*/\1/p')
    case $name in
        kill) ;;
        *)
            [ -n "$why" ] || grep -a -q 'netguest: guest: stats' "$log" || why="the guest printed no report of its counts"
            [ -n "$why" ] || [ -n "$deny" ] || why="the guest could not read its deny count"
            tx=$(delta tap_tx_bytes "$log" ready measured)
            rx=$(delta tap_rx_bytes "$log" ready measured)
            [ -n "$why" ] || [ "$tx" -ge "$bytes" ] || why="tap0 sent $tx bytes, fewer than the $bytes sent"
            [ -n "$why" ] || [ "$rx" -ge "$bytes" ] || why="tap0 received $rx bytes, fewer than the $bytes received"
            ;;
    esac
    case $name in
        debian | freebsd)
            [ -n "$why" ] || [ "$deny" -gt 0 ] || why="the guest counted no denied packet"
            ;;
        *-vhost)
            [ -n "$why" ] || [ "$deny" = 0 ] || why="the guest denied $deny packets with no filter"
            line=$(grep '^netguest: vhost: ' "$log" | tail -n 1)
            to=$(echo "$line" | sed -n 's/.*to_guest=\([0-9]*\).*/\1/p')
            from=$(echo "$line" | sed -n 's/.*from_guest=\([0-9]*\).*/\1/p')
            [ -n "$why" ] || [ "${to:-0}" -ge "$frames" ] || why="vhost-net gave the guest ${to:-no} frames, fewer than $frames"
            [ -n "$why" ] || [ "${from:-0}" -ge "$frames" ] || why="vhost-net took ${from:-no} frames from the guest, fewer than $frames"
            ;;
    esac
    if [ -n "$why" ] && [ -f "$out/$name/console" ]; then
        tr -d '\r' < "$out/$name/console" | grep -a . | tail -n 30 | sed "s/^/netguest: $name: console: /"
    fi
    judge "$name" "$why"
}

note "card $bdf, driver ${driver:-none}, interface ${iface:-none}, peer $peer:$port, cases $*"
[ -e "$sys" ] && [ -n "$driver" ] && [ -n "$iface" ] || {
    abort="no card $bdf with a driver and an interface"
    exit 1
}
if [ "$others" = 1 ] && ! others_off; then
    abort="the record of the other interfaces could not be written"
    exit 1
fi
touched=1
for c in "$@"; do
    case $c in
        cycle) cycle ;;
        debian | freebsd) guest "$c" "$c" -F 1 ;;
        debian-vhost) guest "$c" debian -F 0 -V ;;
        freebsd-vhost) guest "$c" freebsd -F 0 -V ;;
        kill) guest "$c" debian -F 1 ;;
    esac
done
dmesg | grep -i -E 'tap0|vhost-net|vfio|iommu.*fault|panic|assert' | tail -n 40 | sed 's/^/netguest: dmesg: /'
[ "$failed" = 0 ] || exit 1
exit 0
