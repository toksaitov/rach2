#!/bin/sh
# lenovo.sh: the netguest suite's native form, run from the Mac against the
# Rach2 that runs on the Lenovo: stage the suite on the machine, run its
# cases there with the machine's own wired card, and fetch the result.
#
# Usage: lenovo.sh [-s STAGE] [-o OUT] [-P PORT] [-d SECONDS] [-C CASES]
#                  stage|run|fetch|clean
#
#   stage  copy to REMOTE (/data/netguest) on the machine run.sh,
#          netguest.sh and traffic.sh of this directory and nettput and the
#          gateway images, freebsd.qcow2 and debian.qcow2, of STAGE
#          (build/tests/cases/netguest of this tree), each file checked by
#          SHA-256 and skipped when it is there already.
#   run    start the Mac's nettput peer on PORT (5201), require the machine
#          to reach it, start run.sh detached on the machine with CASES
#          (`cycle debian debian-vhost freebsd freebsd-vhost kill`), wait
#          for its status, fetch its directory into OUT, and judge it by the
#          status, which must be 0, and the log's result line, which must
#          be a pass.
#   fetch  read only: fetch the run's directory into OUT again.
#   clean  remove REMOTE from the machine, unless it holds REMOTE/run/others,
#          the record of the other interfaces that a run could not put
#          back, which the next run puts back first.
#
# The machine's address and its wired interface come from the machines
# file, tools/flow/machines: the address of `lenovo network` and the
# interface of `lenovo interface`, the card being the PCI function behind
# that interface. RACH2_MACHINE_ADDRESS stands in for the address, and when
# RACH2_MACHINE_MAC is set the interface must report that station address
# before anything runs; this suite writes no address, station address or
# serial of the machine. PEER is the Mac's address on the
# interface that routes to the machine. The machine is reached as root with
# the development key, krffs/rach2_demo_ed25519, and must run a kernel with
# /dev/net/tun, /dev/vhost-net and VFIO, on the GNU root with its QEMU.
#
# The card is the machine's only link, so the run does not depend on the
# session that starts it: run.sh runs as a session of its own (-D) and keeps
# its output in REMOTE/run/log and its exit status in REMOTE/run/status; it
# takes the wireless card's address off for the run (-W), so that the Mac
# answers by the wired card or not at all, and gives each guest's boot
# SECONDS (1800) before netguest.sh takes the card back (-d); and it
# reboots the machine when the network does not answer once the card is
# back (-B), which returns a trial boot to the installed kernel. While a
# guest has the card the machine does not answer, and this script asks for
# the status again every 15 seconds; that wait judges nothing, but a run
# that the machine, when it answers, no longer has, and that wrote no
# status, fails.
#
# macOS charges a program started from a Claude Code session to that
# session's Local Network permission, and the Mac's nettput is such a
# program: start this script from a shell that has the permission, such as
# the tmux server the release uses (`tmux -L rach2-lan`). `run` proves the
# path before it starts anything on the machine.
#
# The run is silent and leaves the screen alone. The machine skill says how
# the machine is taken and booted; this script boots nothing. OUT is a new
# build/runs/netguest-lenovo-TIME unless -o names one. Lines are `lenovo:
# ...`; the last is `lenovo: result: COMMAND: pass` or `lenovo: result:
# COMMAND: fail: REASON`. Exit 0, 1, or 2 for a usage error.
set -u

here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../.." && pwd)
key=$tree/krffs/rach2_demo_ed25519
remote=/data/netguest
stage=$tree/build/tests/cases/netguest
out='' port=5201 deadline=1800
cases='cycle debian debian-vhost freebsd freebsd-vhost kill'

usage()
{
    echo "usage: lenovo.sh [-s STAGE] [-o OUT] [-P PORT] [-d SECONDS] [-C CASES]" >&2
    echo "                 stage|run|fetch|clean" >&2
    exit 2
}
while getopts s:o:P:d:C: o; do
    case $o in
        s) stage=$OPTARG ;;
        o) out=$OPTARG ;;
        P) port=$OPTARG ;;
        d) deadline=$OPTARG ;;
        C) cases=$OPTARG ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 1 ] || usage
command=$1
case $command in stage | run | fetch | clean) ;; *) usage ;; esac
case $port$deadline in '' | *[!0-9]*) usage ;; esac

peerpid=''
# shellcheck disable=SC2317,SC2329 # the traps call it
cleanup()
{
    [ -z "$peerpid" ] || kill "$peerpid" 2> /dev/null
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

pass()
{
    echo "lenovo: result: $command: pass"
    exit 0
}

fail()
{
    echo "lenovo: result: $command: fail: $*"
    exit 1
}

# setting NAME: the value the machines file gives the Lenovo's NAME.
setting()
{
    awk -v n="$1" '$1 == "lenovo" && $2 == n { print $3; exit }' "$tree/tools/flow/machines"
}

address=${RACH2_MACHINE_ADDRESS:-$(setting network)}
address=${address%%/*}
iface=$(setting interface)
[ -n "$address" ] && [ -n "$iface" ] || fail "the machines file gives no lenovo network or interface"
chmod 600 "$key" 2> /dev/null

# on COMMAND: run COMMAND on the machine as root.
on()
{
    ssh -i "$key" -o BatchMode=yes -o ConnectTimeout=10 -o ServerAliveInterval=15 \
        -o ServerAliveCountMax=4 -o LogLevel=ERROR -o UserKnownHostsFile=/dev/null \
        -o StrictHostKeyChecking=no "root@$address" "$@"
}

# identify: require the machine to answer, to have the interface, and, with
# RACH2_MACHINE_MAC, the station address.
identify()
{
    station=$(on "cat /sys/class/net/$iface/address") || fail "the machine at $address does not answer"
    if [ -n "${RACH2_MACHINE_MAC:-}" ] && [ "$station" != "$RACH2_MACHINE_MAC" ]; then
        fail "$iface of the machine at $address has another station address than RACH2_MACHINE_MAC"
    fi
    echo "lenovo: machine $address, interface $iface, card $(on "basename \$(readlink /sys/class/net/$iface/device)")"
}

# fetch_run: copy REMOTE/run into OUT/run.
fetch_run()
{
    [ -n "$out" ] || out=$tree/build/runs/netguest-lenovo-$(date +%Y%m%dT%H%M%S)
    mkdir -p "$out" || fail "no directory $out"
    rm -rf "$out/run"
    on "tar -cf - -C $remote run" | tar -xf - -C "$out" || fail "the run's directory was not fetched"
    echo "lenovo: the run's directory is in $out/run"
}

case $command in
    stage)
        identify
        on "mkdir -p $remote" || fail "no $remote on the machine"
        for f in "$here/run.sh" "$here/netguest.sh" "$here/traffic.sh" "$stage/nettput" \
            "$stage/freebsd.qcow2" "$stage/debian.qcow2"; do
            [ -f "$f" ] || fail "no $f"
            name=${f##*/}
            sum=$(shasum -a 256 "$f" | cut -d ' ' -f 1)
            [ -n "$sum" ] || fail "no SHA-256 of $f"
            if [ "$(on "sha256sum $remote/$name 2> /dev/null" | cut -d ' ' -f 1)" = "$sum" ]; then
                echo "lenovo: $name is there"
                continue
            fi
            on "cat > $remote/$name.part" < "$f" || fail "$name was not copied"
            [ "$(on "sha256sum $remote/$name.part" | cut -d ' ' -f 1)" = "$sum" ] ||
                fail "$name arrived with another SHA-256"
            on "mv $remote/$name.part $remote/$name && chmod 755 $remote/$name" || fail "$name was not kept"
            echo "lenovo: $name copied, SHA-256 $sum"
        done
        pass
        ;;
    run)
        identify
        peer=$(route -n get "$address" 2> /dev/null | awk '$1 == "interface:" { print $2 }')
        peer=$(ipconfig getifaddr "${peer:-en0}")
        [ -n "$peer" ] || fail "the Mac has no address on the way to $address"
        nettput=$tree/build/tests/cases/netguest/nettput-host
        [ -x "$nettput" ] || make -C "$tree" -s build/tests/cases/netguest/nettput-host ||
            fail "the Mac's nettput did not build"
        [ -n "$out" ] || out=$tree/build/runs/netguest-lenovo-$(date +%Y%m%dT%H%M%S)
        mkdir -p "$out" || fail "no directory $out"
        "$nettput" -s -p "$port" > "$out/peer.out" 2> "$out/peer.err" &
        peerpid=$!
        until grep -q 'nettput: serving on' "$out/peer.out" 2> /dev/null; do
            kill -0 "$peerpid" 2> /dev/null || fail "the Mac's nettput did not start: $(cat "$out/peer.err")"
            sleep 0.2
        done
        # The card is still the machine's own here, and -d bounds the probe.
        on "timeout -s KILL $deadline $remote/nettput -c $peer -p $port -n 200 wait" ||
            fail "the machine does not reach the Mac's nettput at $peer:$port; see the header on Local Network"
        echo "lenovo: peer $peer:$port answers the machine; starting the cases: $cases"
        # shellcheck disable=SC2086 # the cases are words
        on "sh $remote/run.sh -D -B -W -n $iface -d $deadline -o $remote/run $peer $port $cases" ||
            fail "run.sh did not start"
        until [ -n "$(on "cat $remote/run/status 2> /dev/null" 2> /dev/null)" ]; do
            # 1 from kill is a run that has ended; 255 is a machine that
            # does not answer, while a guest has the card.
            on "kill -0 \$(cat $remote/run/pid) 2> /dev/null" 2> /dev/null
            if [ $? = 1 ] && [ -z "$(on "cat $remote/run/status 2> /dev/null")" ]; then
                fetch_run
                fail "the run on the machine ended without a status"
            fi
            sleep 15
        done
        fetch_run
        cat "$out/run/log"
        result=$(grep '^netguest: result: ' "$out/run/log" | tail -n 1)
        status=$(cat "$out/run/status")
        case $status:$result in
            "0:netguest: result: pass:"*) pass ;;
            *) fail "status $status: ${result:-no result line}" ;;
        esac
        ;;
    fetch)
        identify
        fetch_run
        pass
        ;;
    clean)
        identify
        # One command on the machine, so that the record is looked for and
        # the directory removed together.
        on "test ! -e $remote/run/others || exit 3; rm -rf $remote"
        case $? in
            0) ;;
            3) fail "$remote/run/others records interfaces a run did not put back; run again to put them back" ;;
            *) fail "$remote was not removed" ;;
        esac
        pass
        ;;
esac
