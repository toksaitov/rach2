#!/bin/sh
#
# netguest.sh: the Debian gateway guest's boot script. setup.sh installs it
# as /root/netguest.sh, and the unit netguest.service runs it at each boot,
# once systemd-networkd has started; it stays, to print the counts.
#
# The guest stands between Rach2 and the network: it has the card Rach2 gave
# away through VFIO, the outer interface, and a virtio network card whose
# other end is Rach2's tap interface, the inner one. systemd-networkd gives
# the inner interface 10.77.0.1/24 and asks DHCP for the outer one's
# address, by the two files setup.sh wrote. The script waits for both,
# turns on forwarding, and loads nftables: NAT of 10.77.0.0/24 out of the
# outer interface, TCP port `fwd` of the outer interface forwarded to
# Rach2's port 22, and chains that drop what no rule accepts.
#
# Its parameters are one line of text in the first 512 bytes of the second
# virtio disk, /dev/vdb, padded with NULs:
#
#   rules=N opt=none|basic state=1|0 fwd=PORT filter=1|0
#
# and without the disk, or with an empty one, rules=0 opt=none state=1
# fwd=22 filter=1. `rules` is the number of filler rules, at most 131072;
# opt=basic loads the rules with `nft -o`, which folds rules that differ in
# one value into a set; `state=0` leaves the tracked connections' rule out
# of the forward chain, so that each forwarded packet walks the rules;
# `filter=0` leaves the filter out, a forward chain that accepts everything
# beside the translation, which is the guest's cost of forwarding alone.
#
# With the filter, TCP from the inner network to port 9 of any address is
# dropped by a rule with the counter deny9, which counts the packets Rach2
# sends at it on purpose.
#
# The rules mean what the FreeBSD guest's pf rules mean. A packet of a
# tracked connection is accepted first, as pf looks up its states first;
# then come the fillers, each with its own source address in 198.18.0.0/15,
# which nftables compares one after another, having no skip of pf's kind;
# then the rules that accept the inner network's packets, ICMP and the
# forwarded port. NAT keeps its connection tracking whatever `state` says.
#
# The last line the script writes to the serial port is one of
#
#   NETGUEST READY inner=IF outer=IF addr=A.B.C.D rules=N opt=O state=S filter=F
#   NETGUEST FAILED reason
#
# and before READY it writes `NETGUEST LOADED fillers=F total=T`, the
# filler rules and all the rules the forward chain holds once nft has
# loaded them. After READY, each 2 seconds, it writes
#
#   NETGUEST STATS outer_in=N outer_out=N deny9=N states=N
#
# the outer interface's packets in and out, the packets the deny rule
# dropped and the tracked connections, each 0 when it cannot be read but
# the deny rule's with the filter on, which is then `unknown`.

inner_mac=52:54:00:77:00:01
inner_addr=10.77.0.1/24
inner_net=10.77.0.0/24
host=10.77.0.2
param=/dev/vdb
conf=/run/netguest.nft

say()
{
    echo "NETGUEST $*" > /dev/ttyS0
}

fail()
{
    say "FAILED $*"
    exit 1
}

rules=0 opt=none state=1 fwd=22 filter=1
if [ -b "$param" ]; then
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
# the virtio card; the outer one is the first other Ethernet interface. A
# card's driver may load after this script starts, so both are waited for;
# a guest that never has them is ended by the caller's bound.
while :; do
    inner='' outer=''
    for d in /sys/class/net/*; do
        [ "$(cat "$d/type" 2> /dev/null)" = 1 ] || continue
        if [ "$(cat "$d/address")" = "$inner_mac" ]; then
            inner=${d##*/}
        elif [ -z "$outer" ]; then
            outer=${d##*/}
        fi
    done
    if [ -n "$inner" ] && [ -n "$outer" ]; then
        break
    fi
    sleep 1
done
[ -n "$inner" ] || fail "no interface has the address $inner_mac"
ip link set "$inner" up
ip addr replace "$inner_addr" dev "$inner" || fail "$inner did not take $inner_addr"
[ -n "$outer" ] || fail "no outer interface: only $inner is an Ethernet interface"
addr=''

# systemd-networkd asks DHCP for the outer interface's address.
ip link set "$outer" up
while [ -z "$addr" ]; do
    addr=$(ip -4 -o addr show dev "$outer" scope global | awk '{ sub("/.*", "", $4); print $4; exit }')
    [ -n "$addr" ] || sleep 1
done

# With state=0 only what comes back from the outer side is accepted by its
# tracked connection, as pf accepts it by the state of its translation;
# what the inner side sends walks the rules, packet by packet.
if [ "$state" = 1 ]; then
    tracked='ct state established,related accept'
    back=''
else
    tracked=''
    back="oifname \"$inner\" ip daddr $inner_net ct state established,related accept"
fi
{
    cat << EOF
flush ruleset
table ip netguest {
	counter deny9 {
		packets 0 bytes 0
	}
	chain prerouting {
		type nat hook prerouting priority dstnat; policy accept;
		iifname "$outer" ip daddr $addr tcp dport $fwd dnat to $host:22
	}
	chain postrouting {
		type nat hook postrouting priority srcnat; policy accept;
		oifname "$outer" ip saddr $inner_net masquerade
	}
EOF
    if [ "$filter" = 0 ]; then
        cat << EOF
	chain forward {
		type filter hook forward priority filter; policy accept;
	}
}
EOF
    else
        cat << EOF
	chain input {
		type filter hook input priority filter; policy drop;
		ct state established,related accept
		iifname "lo" accept
		iifname "$inner" ip saddr $inner_net tcp dport 9 counter name "deny9" drop
		iifname "$inner" ip saddr $inner_net accept
		ip protocol icmp accept
	}
	chain forward {
		type filter hook forward priority filter; policy drop;
		$tracked
EOF
        awk -v n="$rules" -v i="$inner" 'BEGIN {
            for (k = 0; k < n; k++)
                printf "\t\tiifname \"%s\" ip saddr 198.%d.%d.%d tcp dport 9 drop\n",
                    i, 18 + int(k / 65536), int(k / 256) % 256, k % 256
        }'
        cat << EOF
		iifname "$inner" ip saddr $inner_net tcp dport 9 counter name "deny9" drop
		iifname "$inner" ip saddr $inner_net accept
		$back
		iifname "$outer" oifname "$inner" ip daddr $host tcp dport 22 accept
		ip protocol icmp accept
	}
}
EOF
    fi
} > "$conf"

if [ "$opt" = basic ]; then
    nft -o -f "$conf" > "$conf.out" 2> "$conf.err" || fail "nft -o: $(head -n 1 "$conf.err")"
else
    nft -f "$conf" 2> "$conf.err" || fail "nft: $(head -n 1 "$conf.err")"
fi
# The table of tracked connections holds what pf's table of states is given
# in the FreeBSD guest, 200000, over as many buckets as pf's hash has.
sysctl -q -w net.netfilter.nf_conntrack_buckets=131072 2> /dev/null
sysctl -q -w net.netfilter.nf_conntrack_max=200000 2> /dev/null
# Forwarding starts only once the filter is loaded.
sysctl -q -w net.ipv4.ip_forward=1 || fail "forwarding was not turned on"

nft list chain ip netguest forward > "$conf.loaded" 2> /dev/null
say "LOADED fillers=$(grep -c 'ip saddr 198\.1[89]\.[0-9.]* tcp dport 9 drop' "$conf.loaded") total=$(grep -c -E '(accept|drop)$' "$conf.loaded") states_max=$(cat /proc/sys/net/netfilter/nf_conntrack_max 2> /dev/null) buckets=$(cat /proc/sys/net/netfilter/nf_conntrack_buckets 2> /dev/null)"
say "READY inner=$inner outer=$outer addr=$addr rules=$rules opt=$opt state=$state filter=$filter"

# The counts, each 2 seconds, for as long as the guest runs, read as the
# header comment says.
while :; do
    sleep 2
    rx=$(cat "/sys/class/net/$outer/statistics/rx_packets" 2> /dev/null)
    tx=$(cat "/sys/class/net/$outer/statistics/tx_packets" 2> /dev/null)
    deny=$(nft list counter ip netguest deny9 2> /dev/null |
        awk '{ for (f = 1; f < NF; f++) if ($f == "packets") { print $(f + 1); exit } }')
    [ -n "$deny" ] || [ "$filter" = 1 ] || deny=0
    states=$(cat /proc/sys/net/netfilter/nf_conntrack_count 2> /dev/null)
    say "STATS outer_in=${rx:-0} outer_out=${tx:-0} deny9=${deny:-unknown} states=${states:-0}"
done
