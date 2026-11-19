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
#                  iproute2, as tap9, which must not be there before; then
#                  three times the interface's addresses off, the driver
#                  letting the card go and taking it back, the addresses and
#                  default routes back, and PEER answering by the card
#                  again; the free interrupt vectors as they were.
#   debian, freebsd
#                  a boot of the guest with its filter and QEMU moving the
#                  tap's frames: through the guest, nettput sends and
#                  receives 8388608 bytes and makes 1000 exchanges and 200
#                  connections, which tap0's byte counts must show each
#                  way; ten attempts at PEER's port 9, each unanswered and
#                  unrefused until Rach2's TCP gives it up, which the
#                  guest's deny rule must count, at least one SYN each.
#   debian-vhost, freebsd-vhost
#                  the same transfers through a boot with translation alone
#                  and the tap's queues on /dev/vhost-net: the backend must
#                  count at least 5541 frames each way, the fewest frames of
#                  at most 1514 bytes that carry 8388608 bytes, and the
#                  guest's port-9 rule, which then drops nothing, must count
#                  no packet.
#   kill           a boot of the Debian guest whose QEMU is killed while
#                  nettput sends through it, once tap0 has carried a
#                  mebibyte of it.
#
# Every boot also passes netguest.sh's own checks: the card back with its
# driver, PEER answering on it, the VFIO and KVM counts and the free
# interrupt vectors as at the start and no IOMMU fault (netguest.sh). No
# check reads the clock; a case that does not end is ended by its caller's
# bound, or, with -d, by netguest.sh's deadline of SECONDS a boot and the
# same deadline on cycle's probes and the last probe of -B.
#
# On the Lenovo the card is the machine's only link, and three options are
# for that: -D starts the run again as a session of its own, with its
# output in OUT/log, its process ID in OUT/pid and its exit status in
# OUT/status at the end, and returns once that session has started; it
# keeps OUT, which its start line names, since OUT/status is the run's
# result. -W takes every IPv4 address and default route off every other
# interface for the run, the wireless card, and stops its DHCP client, so
# that PEER answers by the card or not at all, and puts them back at the
# end, the first address the primary again and the same DHCP client, by
# its start, running again, a failure to do either failing the run; a
# record OUT/others that an earlier run could not put back is put back
# first, and one of another format is refused. -B reboots the machine
# when, the card and the other interfaces back at the end, neither PEER
# answers 2400 attempts nor a gateway of the interface three pings by it,
# within -d's SECONDS when -d gives any, which on a trial boot returns the
# machine to the installed kernel. Every attempt at PEER by the card is a
# connection bound to the card's interface. The card's interface gets the
# addresses and default routes it had when the run began back at the end,
# whatever a case left, and every process the run started is killed.
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

# Until finish's traps, a signal removes only the scratch made here.
new_out=0 early=''
# A detached run started from here keeps OUT once it has written its process
# ID there, since it may hold its recovery record, and has it named.
trap 'if [ -z "$early" ]; then :; elif [ -s "$early/pid" ]; then
    echo "netguest: started: log $early/log" >&2; else rm -rf "$early"; fi' EXIT
trap 'exit 143' TERM
trap 'exit 129' HUP
trap 'exit 130' INT
if [ -z "$out" ]; then
    # Named before it is made, so that no signal comes between the directory
    # and the trap's knowing it.
    early=${TMPDIR:-/tmp}/rach2-netguest.$$
    mkdir "$early" || {
        early=''
        exit 1
    }
    new_out=1 out=$early
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
    early=''
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
laddrs='' lroutes=''

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

# state_of PID: print PID's state letter, as /proc/PID/stat gives it, or
# nothing.
state_of()
{
    sed -n 's/^.*) \([A-Z]\).*/\1/p' "/proc/$1/stat" 2> /dev/null
}

# started_at PID: print when PID started, in ticks since boot, as
# /proc/PID/stat gives it, or nothing.
started_at()
{
    awk '{ s = $0; sub(/^.*\) /, "", s); split(s, f, " "); print f[20] }' "/proc/$1/stat" 2> /dev/null
}

# children_of PIDS: print the processes whose parent is one of PIDS.
children_of()
{
    cat /proc/[0-9]*/stat 2> /dev/null |
        awk -v p=" $1 " '{ s = $0; sub(/^.*\) /, "", s); split(s, f, " ")
            if (index(p, " " f[2] " ")) print $1 }'
}

# stopped PIDS: wait until each of PIDS has stopped or ended, since SIGSTOP
# takes effect only when the process next runs; a process that sleeps
# uninterruptibly is waited for 100 times 0.1 s at most.
# shellcheck disable=SC2329 # kill_tree calls it
stopped()
{
    for x in $1; do
        t=0
        while [ "$t" -lt 100 ]; do
            case $(state_of "$x") in '' | T | t | Z | X) break ;; esac
            t=$((t + 1))
            sleep 0.1
        done
    done
}

# stop_tree PID: stop PID and every process under it, each stopped before
# its children are looked for, so that none starts another meanwhile, and
# print them, the deepest first.
# shellcheck disable=SC2329 # finish and kill_tree call it
stop_tree()
{
    all='' new=$1
    while [ -n "$new" ]; do
        for x in $new; do
            kill -STOP "$x" 2> /dev/null
        done
        stopped "$new"
        all="$new $all"
        new=$(children_of "$new" | tr '\n' ' ')
    done
    echo "$all"
}

# kill_tree PID: stop PID and every process under it (stop_tree), then
# kill them, the deepest first, so that none is orphaned while a process
# above it still lives, and reap PID.
# shellcheck disable=SC2329 # finish and in_time call it
kill_tree()
{
    for x in $(stop_tree "$1"); do
        kill -KILL "$x" 2> /dev/null
        kill -CONT "$x" 2> /dev/null
    done
    wait "$1" 2> /dev/null
}

# in_time COMMAND...: run COMMAND, with every process under it killed once
# -d's SECONDS have passed when -d gives any; its status, 137 when killed.
# shellcheck disable=SC2329 # cycle and finish call it
in_time()
{
    "$@" &
    job=$!
    dog=''
    if [ "$deadline" -gt 0 ]; then
        (
            sleep "$deadline"
            kill_tree "$job"
        ) > /dev/null 2>&1 &
        dog=$!
    fi
    child=$job
    wait "$job"
    rc=$?
    child=''
    [ -z "$dog" ] || kill_tree "$dog"
    return "$rc"
}

# The first line of OUT/others, which names its format.
others_format='netguest others 2'

# others_off: take every IPv4 address and default route off every other
# interface that has one, and stop its DHCP client. OUT/others records each
# interface, after its first line, as `IF ROUTES CLIENT ADDR...`, ROUTES the
# GATEWAY/METRIC words joined by commas or -, CLIENT the DHCP client's
# process ID and start, PID:START, or -, before anything of it changes. A
# record an earlier run left is put back first. The status is 1, with what
# was changed recorded, when anything fails.
others_off()
{
    if [ -s "$out/others" ]; then
        note "others: putting back what an earlier run recorded in $out/others"
        others_on || return 1
    fi
    echo "$others_format" > "$out/others" || return 1
    for n in /sys/class/net/*; do
        o=${n##*/}
        case $o in "$iface" | lo | tap*) continue ;; esac
        a=$(addrs_of "$o")
        r=$(routes_of "$o")
        [ -n "$a" ] || [ -n "$r" ] || continue
        p=$(cat "/var/run/udhcpc.$o.pid" 2> /dev/null)
        case $p in '' | 0 | *[!0-9]*) p='' ;; esac
        case $(state_of "${p:-0}") in '' | Z | X) p='' ;; esac
        s=''
        [ -z "$p" ] || s=$(started_at "$p")
        [ -n "$s" ] || p=''
        c=-
        [ -z "$p" ] || c=$p:$s
        rs=$(echo "$r" | tr ' ' ,)
        echo "$o ${rs:--} $c $a" >> "$out/others" && sync || return 1
        if [ -n "$p" ]; then
            kill -s STOP "$p" || return 1
            until [ "$(state_of "$p")" = T ]; do
                case $(state_of "$p") in '' | Z | X) return 1 ;; esac
                sleep 0.1
            done
        fi
        for x in $r; do
            # shellcheck disable=SC2046 # the route's words
            ip route del $(route_words "$o" "$x") 2> /dev/null
        done
        for x in $a; do
            ip addr del "$x" dev "$o" || return 1
        done
        [ -z "$(addrs_of "$o")" ] && [ -z "$(routes_of "$o")" ] || return 1
        note "others: $o: $a off, default routes ${r:-none} off, DHCP client ${p:-none} stopped"
    done
}

# others_on: put back what others_off took, each address, the first the
# primary, and route read back, and each DHCP client that still runs, the
# same process by its start, running again; the record is kept, and the
# status is 1, when anything is not back or the record is of another
# format.
# shellcheck disable=SC2329 # finish calls it
others_on()
{
    [ -s "$out/others" ] || return 0
    if [ "$(head -n 1 "$out/others")" != "$others_format" ]; then
        note "others: $out/others is not a record of this script's; put its interfaces back by hand"
        return 1
    fi
    back=0
    sed 1d "$out/others" > "$out/others.read" || return 1
    while read -r o rs c a; do
        [ -z "$a" ] || put_addrs "$o" "$a" || back=1
        [ "$rs" = - ] || put_routes "$o" "$(echo "$rs" | tr , ' ')" || back=1
        if [ "$c" != - ]; then
            p=${c%%:*} s=${c#*:}
            if [ -z "$s" ] || [ "$(started_at "$p")" != "$s" ]; then
                note "others: $o: DHCP client $p has ended; nothing to resume"
            else
                kill -s CONT "$p" 2> /dev/null || back=1
                while [ "$(state_of "$p")" = T ]; do
                    sleep 0.1
                done
                case $(state_of "$p") in '' | Z | X) back=1 ;; esac
            fi
        fi
        note "others: $o: $a back"
    done < "$out/others.read"
    rm -f "$out/others.read"
    [ "$back" = 0 ] || return 1
    rm -f "$out/others"
}

# link_save: keep the card's interface's addresses and default routes,
# which link_restore puts back.
link_save()
{
    laddrs=$(addrs_of "$iface")
    lroutes=$(routes_of "$iface")
    link_saved=1
}

# link_restore: the card's interface up with the addresses and the default
# routes link_save kept, each read back.
link_restore()
{
    ip link set "$iface" up || return 1
    [ -z "$laddrs" ] || put_addrs "$iface" "$laddrs" || return 1
    [ -z "$lroutes" ] || put_routes "$iface" "$lroutes"
}

# reach N: ask PEER's nettput until it answers, N attempts at most, each
# connection bound to the card's interface; prints nettput's result line.
# shellcheck disable=SC2329 # cycle and answers call it
reach()
{
    k=0
    while [ "$k" -lt "$1" ]; do
        k=$((k + 1))
        if "$nettput" -c "$peer" -I "$iface" -p "$port" -n 1 wait > /dev/null 2>&1; then
            echo "nettput: result: wait attempts=$k"
            return 0
        fi
    done
    echo "nettput: result: wait failed attempts=$k"
    return 1
}

# answers: succeed when PEER's nettput, or a gateway of the interface's
# default routes, answers by the interface; ping's own wait for replies
# decides only whether -B reboots.
# shellcheck disable=SC2329 # finish calls it
answers()
{
    reach 2400 > /dev/null && return 0
    for r in $(routes_of "$iface"); do
        [ "${r%/*}" != - ] || continue
        if ping -c 3 -I "$iface" "${r%/*}" > /dev/null 2>&1; then
            return 0
        fi
    done
    return 1
}

# shellcheck disable=SC2317,SC2329 # the traps call it
finish()
{
    status=$?
    trap - EXIT TERM HUP INT
    # A case's child, netguest.sh, puts the card back at SIGTERM; then every
    # process left under this one, a child started just before a signal
    # whose process ID was not kept among them, is killed.
    # The child's tree is read stopped, so that none of it starts another
    # meanwhile, then the child is told and the tree goes on; what outlives
    # the child is orphaned and killed.
    kept=''
    if [ -n "$child" ]; then
        kept=$(stop_tree "$child")
        kill -TERM "$child" 2> /dev/null
        for x in $kept; do
            kill -CONT "$x" 2> /dev/null
        done
        wait "$child" 2> /dev/null
    fi
    for x in $kept; do
        kill -KILL "$x" 2> /dev/null
    done
    for c in $(children_of $$); do
        kill_tree "$c"
    done
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
        link_restore || putback="$putback, $iface is not back with its addresses and routes"
    fi
    others_on || putback="$putback, another interface is not back, as $out/others records"
    if [ "$reboot" = 1 ] && [ "$touched" = 1 ]; then
        up
        if ! in_time answers; then
            note "neither $peer nor the gateway answers on $iface; rebooting"
            echo 1 > "$out/status"
            sync
            reboot
        fi
    fi
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
    if ip link show tap9 > /dev/null 2>&1; then
        why="tap9 is there before the case makes it"
    elif tap9=1 && ip tuntap add dev tap9 mode tap; then
        if ip link set tap9 up && ip addr add 10.99.0.1/24 dev tap9; then
            ip -o addr show dev tap9 | sed 's/^/netguest: tap: /'
        else
            why="the tap interface was not set up"
        fi
        ip tuntap del dev tap9 mode tap || why="tap9 was not removed"
        ! ip link show tap9 > /dev/null 2>&1 || why="tap9 is still there"
        [ -n "$why" ] || tap9=0
    else
        ! ip link show tap9 > /dev/null 2>&1 || ip tuntap del dev tap9 mode tap || why="tap9 was not removed"
        [ -n "$why" ] || why="the tap interface was not made"
        ! ip link show tap9 > /dev/null 2>&1 || why="$why; tap9 is still there"
        ip link show tap9 > /dev/null 2>&1 || tap9=0
    fi
    was=$(awk '$1 == "msi_vectors_free" { print $2 }' /proc/stat)
    case $was in '' | *[!0-9]*) [ -n "$why" ] || why="the kernel gives no count msi_vectors_free" ;; esac
    n=0
    while [ -z "$why" ] && [ "$n" -lt 3 ]; do
        n=$((n + 1))
        for x in $laddrs; do
            ip addr del "$x" dev "$iface"
        done
        ip link set "$iface" down
        echo "$bdf" > "/sys/bus/pci/drivers/$driver/unbind" || why="unbind $n"
        [ ! -e "$sys/driver" ] || why="the card still has a driver after unbind $n"
        echo "$bdf" > "/sys/bus/pci/drivers/$driver/bind" || why="bind $n"
        if ! link_restore; then
            why="cycle $n: $iface did not get its addresses and default routes back"
        elif in_time reach 1000000 > "$out/cycle-$n" 2>&1; then
            note "cycle $n: $(sed -n 's/^nettput: result: //p' "$out/cycle-$n") after the card came back"
        else
            why="cycle $n: $peer does not answer by $iface after the card came back: $(tail -n 1 "$out/cycle-$n")"
        fi
    done
    link_restore || [ -n "$why" ] || why="$iface did not get its addresses and default routes back"
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
    # The report traffic.sh judged by: the first after the transfers, or
    # after the attempts at port 9.
    deny=$(grep -a '^traffic: report: ' "$log" | tail -n 1 | sed -n 's/.*deny9=\([0-9][0-9]*\).*/\1/p')
    case $name in
        kill) ;;
        *)
            [ -n "$why" ] || grep -a -q '^traffic: report: ' "$log" || why="the guest printed no report of its counts"
            [ -n "$why" ] || [ -n "$deny" ] || why="the guest could not read its deny count"
            tx=$(delta tap_tx_bytes "$log" ready measured)
            rx=$(delta tap_rx_bytes "$log" ready measured)
            [ -n "$why" ] || [ "$tx" -ge "$bytes" ] || why="tap0 sent $tx bytes, fewer than the $bytes sent"
            [ -n "$why" ] || [ "$rx" -ge "$bytes" ] || why="tap0 received $rx bytes, fewer than the $bytes received"
            ;;
    esac
    case $name in
        debian | freebsd)
            [ -n "$why" ] || [ "$deny" -ge 10 ] || why="the guest counted $deny denied packets, fewer than the 10 attempts"
            ;;
        *-vhost)
            [ -n "$why" ] || [ "$deny" = 0 ] || why="the guest counted $deny packets to port 9 with no filter"
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
    abort="the other interfaces could not all be taken off, as $out/others records"
    exit 1
fi
touched=1
# The card's interface as the run found it, which finish puts back
# whatever a case left.
up
link_save
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
