#!/bin/sh
#
# netguest.sh: the FreeBSD gateway guest's boot script. setup.sh installs it
# as /root/netguest.sh, and /etc/rc.local runs it near the end of each boot.
#
# The guest stands between Rach2 and the network: it has the card Rach2 gave
# away through VFIO, the outer interface, and a virtio network card whose
# other end is Rach2's tap interface, the inner one. The script gives the
# inner interface 10.77.0.1/24, asks DHCP for the outer one's address,
# turns on forwarding, and loads pf: NAT of 10.77.0.0/24 out of the outer
# interface, TCP port `fwd` of the outer interface forwarded to Rach2's port
# 22, and a filter that blocks what no rule passes.
#
# Its parameters are one line of text in the first 512 bytes of the second
# virtio disk, /dev/vtbd1, padded with NULs:
#
#   rules=N opt=none|basic state=1|0 fwd=PORT filter=1|0
#
# and without the disk, or with an empty one, rules=0 opt=none state=1
# fwd=22 filter=1. `rules` is the number of filler rules, at most 131072;
# `opt` is pf's `set ruleset-optimization`; `state=0` passes the inner
# interface's packets with `no state`, so that each of them walks the
# rules; `filter=0` leaves the filter out, `pass all` after the
# translation, which is the guest's cost of forwarding alone.
#
# With the filter, TCP from the inner network to port 9 of any address is
# blocked by a rule with the label deny9, which counts the packets Rach2
# sends at it on purpose.
#
# pf looks a packet up in its states first, and only a packet of no state
# walks the rules. It evaluates every rule, the last match deciding, but
# skips a run of rules that share a field the packet failed in the first of
# them. So each filler names its own source address, in 198.18.0.0/15,
# which the benchmarking range never uses: a TCP packet that arrives on the
# inner interface is compared with each of them, and a packet of another
# protocol or direction skips them all at once. With opt=basic pfctl folds
# the fillers into one rule over a table, which is what the option is for.
#
# NAT and the forward keep their states whatever `state` says: in pf a
# translation is a state, so the outer interface is never stateless.
# NAT takes its source ports from 1024 to 65535 and not from pf's 50001 to
# 65535, whose 15535 ports a few seconds of new connections to one address
# would use up while their closed states still hold them.
#
# The last line the script prints on the console, which is the serial port,
# is one of
#
#   NETGUEST READY inner=IF outer=IF addr=A.B.C.D rules=N opt=O state=S filter=F
#   NETGUEST FAILED reason
#
# and before READY it prints `NETGUEST LOADED fillers=F total=T`, the
# filler rules and all the rules pf holds once pfctl has loaded them. After
# READY, each 2 seconds, a process left behind prints
#
#   NETGUEST STATS outer_in=N outer_out=N deny9=N states=N
#
# the outer interface's packets in and out, the packets the deny rule
# blocked and pf's states, each 0 when it cannot be read but the deny
# rule's with the filter on, which is then `unknown`.

inner_mac=52:54:00:77:00:01
inner_addr=10.77.0.1/24
inner_net=10.77.0.0/24
host=10.77.0.2
param=/dev/vtbd1
conf=/var/run/netguest.pf.conf

say()
{
    echo "NETGUEST $*" > /dev/console
}

fail()
{
    say "FAILED $*"
    exit 1
}

rules=0 opt=none state=1 fwd=22 filter=1
if [ -c "$param" ]; then
    for word in $(dd if="$param" bs=512 count=1 2> /dev/null | tr '\n' ' ' | tr -cd 'a-z0-9= '); do
        case $word in
            rules=*) rules=${word#rules=} ;;
            opt=*) opt=${word#opt=} ;;
            state=*) state=${word#state=} ;;
            fwd=*) fwd=${word#fwd=} ;;
            filter=*) filter=${word#filter=} ;;
        esac
    done
fi
case $rules in
    '' | *[!0-9]*) fail "rules=$rules is no number" ;;
esac
[ "$rules" -le 131072 ] || fail "rules=$rules is more than 131072"
case $opt in
    none | basic) ;;
    *) fail "opt=$opt is neither none nor basic" ;;
esac
case $state in
    0 | 1) ;;
    *) fail "state=$state is neither 0 nor 1" ;;
esac
case $fwd in
    '' | *[!0-9]*) fail "fwd=$fwd is no port" ;;
esac
case $filter in
    0 | 1) ;;
    *) fail "filter=$filter is neither 0 nor 1" ;;
esac

# The inner interface is the one with the address QEMU was told to give
# the virtio card; the outer one is the first other Ethernet interface.
inner='' outer=''
for i in $(ifconfig -l ether); do
    mac=$(ifconfig "$i" | awk '$1 == "ether" { print $2; exit }')
    if [ "$mac" = "$inner_mac" ]; then
        inner=$i
    elif [ -z "$outer" ]; then
        outer=$i
    fi
done
[ -n "$inner" ] || fail "no interface has the address $inner_mac"
ifconfig "$inner" inet "$inner_addr" up || fail "$inner did not take $inner_addr"
[ -n "$outer" ] || fail "no outer interface: only $inner is an Ethernet interface"
addr=''

# dhclient returns once it has a lease, or gives up on a link that is down
# or a network that does not answer; it is asked again until there is a
# lease, and a guest that never gets one is ended by the caller's bound.
ifconfig "$outer" up
while [ -z "$addr" ]; do
    pid=/var/run/dhclient/dhclient.$outer.pid
    [ ! -f "$pid" ] || kill "$(cat "$pid")" 2> /dev/null
    dhclient "$outer" > /dev/null 2>&1
    addr=$(ifconfig "$outer" inet | awk '$1 == "inet" { print $2; exit }')
    [ -n "$addr" ] || sleep 2
done

# The rules of the inner interface keep state or not, as `state` says; the
# rest always do.
if [ "$state" = 1 ]; then
    how='' icmp='' back=''
else
    how=' flags any no state' icmp=' no state'
    back="pass out on \$int_if inet from any to $inner_net$how"
fi
{
    cat << EOF
ext_if = "$outer"
int_if = "$inner"
set ruleset-optimization $opt
set state-policy if-bound
set limit states 200000
set skip on lo0
nat on \$ext_if inet from $inner_net to any -> (\$ext_if) port 1024:65535
rdr on \$ext_if inet proto tcp from any to (\$ext_if) port $fwd -> $host port 22
EOF
    if [ "$filter" = 0 ]; then
        echo "pass all$how"
    else
        echo 'block all'
        awk -v n="$rules" -v i="$inner" 'BEGIN {
            for (k = 0; k < n; k++)
                printf "block in on %s inet proto tcp from 198.%d.%d.%d to any port 9\n",
                    i, 18 + int(k / 65536), int(k / 256) % 256, k % 256
        }'
        cat << EOF
block in quick on \$int_if inet proto tcp from $inner_net to any port 9 label "deny9"
pass in on \$int_if inet from $inner_net to any$how
$back
pass out on \$ext_if inet all
pass in on \$ext_if inet proto tcp from any to $host port 22
pass out on \$int_if inet proto tcp from any to $host port 22$how
pass inet proto icmp all$icmp
EOF
    fi
} > "$conf"

kldload -n pf 2> /dev/null
kldstat -q -m pf || fail "pf did not load"
pfctl -f "$conf" 2> "$conf.err" || fail "pfctl: $(head -n 1 "$conf.err")"
pfctl -e > /dev/null 2>&1
pfctl -s info 2> /dev/null | grep -q 'Status: Enabled' || fail "pf is not enabled"
# Forwarding starts only once the filter is loaded.
sysctl net.inet.ip.forwarding=1 > /dev/null || fail "forwarding was not turned on"

pfctl -s rules > "$conf.loaded" 2> /dev/null
say "LOADED fillers=$(grep -c ' from 198\.1[89]\.' "$conf.loaded") total=$(grep -c . "$conf.loaded") tables=$(grep -c '<__automatic' "$conf.loaded") states_max=$(pfctl -s memory 2> /dev/null | awk '$1 == "states" { print $4; exit }') buckets=$(sysctl -n net.pf.states_hashsize 2> /dev/null)"
say "READY inner=$inner outer=$outer addr=$addr rules=$rules opt=$opt state=$state filter=$filter"

# The counts, each 2 seconds, from a process that outlives this script,
# read as the header comment says.
stats()
{
    while :; do
        sleep 2
        # shellcheck disable=SC2046 # two words, or none
        set -- $(netstat -n -I "$outer" 2> /dev/null | awk '$3 ~ /^<Link/ { print $5, $8; exit }')
        deny=$(pfctl -s labels 2> /dev/null | awk '$1 == "deny9" { print $3; exit }')
        [ -n "$deny" ] || [ "$filter" = 1 ] || deny=0
        states=$(pfctl -s info 2> /dev/null | awk '/current entries/ { print $3; exit }')
        say "STATS outer_in=${1:-0} outer_out=${2:-0} deny9=${deny:-unknown} states=${states:-0}"
    done
}
# rc.local's jobs are in rc's process group, which gets SIGHUP when rc ends
# and the console's session with it; the reports must outlive that.
(
    trap '' HUP
    stats
) > /dev/null 2>&1 &
