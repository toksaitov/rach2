#!/bin/sh
#
# machine_wifi_soak.sh: soak an installed machine's Wi-Fi under disk load and
# count every drop by its reason, on the real radio and a real access point.
#
# Usage: machine_wifi_soak.sh SECONDS [ADDRESS]
#        machine_wifi_soak.sh run SECONDS
#
# The first form runs on the host: it sends this file to the machine at
# ADDRESS, 192.168.0.106 unless given, starts the second form there in a
# session of its own, follows its output over SSH, and keeps it with the
# run's logs in build/runs/wifi-soak-TIME. SSH uses the wired link, so a drop
# does not cut the session that watches it; a machine that hangs leaves here
# whatever it had printed. Each SSH command waits up to 10 minutes for a
# login and for an answer, since the load can hold them up that long.
# RACH2_MACHINE_MAC names the station address the
# machine must report, 28:d2:44:7e:e0:af by default, the Lenovo's cable.
#
# The second form is the soak. It joins RACH2_SOAK_NETWORK, TDA-NET unless
# given, a network wpa_supplicant already knows, and for SECONDS it runs:
#
#   * RACH2_SOAK_WRITERS writers, 6 unless given, each of which writes a
#     file of 6000 MiB of random bytes, flushes it, removes it and starts
#     again, and a `sync` loop. The files are under /tmp, on the root file
#     system, beside the service log wpa_supplicant writes. This load can
#     keep programs that need the disk, a new SSH login among them, waiting
#     for minutes, so the run stops it with what is already in memory: shell
#     builtins and files in /dev/shm.
#   * a ping of the gateway through wlan0, five times a second, whose
#     replies are stamped on arrival;
#   * a logger of every event wpa_supplicant reports, each stamped on arrival;
#   * a sampler of `iw dev wlan0 station dump` every 5 s, and of the kernel's
#     Wi-Fi lines every 30 s.
#
# The AP's own group rekey may be an hour or more apart, so the network's
# wpa_ptk_rekey is set to RACH2_SOAK_REKEY seconds, 120 unless given, in
# wpa_supplicant's memory through its control socket. init runs wpa_supplicant
# on the installed configuration and starts it again when it stops, so that
# in-memory copy is the run's own: nothing writes the file, and the end of the
# run sets the value back to 0 and enables every network again.
#
# The logs are kept in /dev/shm/wifi-soak, a tmpfs, so that no stamp waits for
# the loaded disk. All times are the seconds of /proc/uptime, the clock of
# dmesg. The run ends with one table:
#
#   * a renewal is pairwise when a `Key negotiation completed` line has no
#     association before it since the last such line, and group for `Group
#     rekeying completed`;
#   * a drop is a CTRL-EVENT-DISCONNECTED, counted by reason and by
#     locally_generated, and its rejoin time runs to the next
#     CTRL-EVENT-CONNECTED;
#   * tx retries and failures are summed over associations, since the
#     station's counters start again at each, from the samples, so a run of
#     drops can leave out up to 5 s of each;
#   * the longest ping gap is the longest time between two replies;
#   * the longest disk request is /proc/stat's latency_disk_us, the longest
#     since boot, so that it is the run's only when it grew during the run,
#     which the table says.
#
# Exit 0 when the run reached its end, 1 when a step failed, 2 on a usage
# error. Exit 0 says nothing of the link: the table does.
set -u

ctrl=/var/run/wpa_supplicant
iface=wlan0
dir=/dev/shm/wifi-soak
load=/tmp/wifi-soak

usage()
{
    echo 'usage: machine_wifi_soak.sh SECONDS [ADDRESS]' >&2
    exit 2
}

# Run this file on the machine and bring back what it wrote.
host()
{
    seconds=$1
    address=${2:-192.168.0.106}
    mac=${RACH2_MACHINE_MAC:-28:d2:44:7e:e0:af}
    self=$0
    root=$(cd "$(dirname "$0")/../.." && pwd)
    key=$root/krffs/rach2_demo_ed25519
    out=$root/build/runs/wifi-soak-$(date +%Y%m%d-%H%M%S)
    mkdir -p "$out" || exit 1
    chmod 600 "$key"
    set -- ssh -i "$key" -o BatchMode=yes -o ConnectTimeout=600 -o LogLevel=ERROR \
        -o ServerAliveInterval=15 -o ServerAliveCountMax=40 \
        -o UserKnownHostsFile="$out/known" -o StrictHostKeyChecking=accept-new \
        "root@$address"
    found=$("$@" cat /sys/class/net/eth0/address) || {
        echo "wifi-soak: failed: no answer from $address" >&2
        exit 1
    }
    [ "$found" = "$mac" ] || {
        echo "wifi-soak: failed: $address reports $found, not $mac" >&2
        exit 1
    }
    "$@" 'cat > /dev/shm/wifi-soak.sh' < "$self" || exit 1
    env=
    for v in NETWORK WRITERS REKEY; do
        eval "x=\${RACH2_SOAK_$v:-}"
        [ -z "$x" ] || env="$env RACH2_SOAK_$v='$x'"
    done
    "$@" "$env setsid sh /dev/shm/wifi-soak.sh run $seconds \
        > /dev/shm/wifi-soak.out 2>&1 < /dev/null & echo \$! > /dev/shm/wifi-soak.pid" ||
        exit 1
    "$@" 'read p < /dev/shm/wifi-soak.pid; tail -f --pid="$p" /dev/shm/wifi-soak.out' |
        tee "$out/out"
    "$@" 'cd /dev/shm/wifi-soak && tar cf - .' | tar xf - -C "$out" || {
        echo "wifi-soak: failed: the logs did not come back; they are on the machine" >&2
        exit 1
    }
    "$@" 'rm -rf /dev/shm/wifi-soak /dev/shm/wifi-soak.sh /dev/shm/wifi-soak.out /dev/shm/wifi-soak.pid'
    echo "wifi-soak: logs: $out"
    grep -q '^wifi-soak: end$' "$out/out"
}

cli()
{
    wpa_cli -p "$ctrl" -i "$iface" "$@"
}

# Stamp each line of standard input with the seconds since boot.
stamp()
{
    awk 'function up(l, a) {
             getline l < "/proc/uptime"; close("/proc/uptime")
             split(l, a, " "); return a[1]
         }
         { print up(), $0; fflush() }'
}

# wpa_supplicant's events: wpa_cli's interactive form, which prints each as
# `<LEVEL>TEXT` among the prompts of its line editor.
events()
{
    mkfifo "$dir/in" || return 1
    wpa_cli -p "$ctrl" -i "$iface" < "$dir/in" 2>&1 |
        awk '{ sub(/^.*\r/, "") }
            /^<[0-9]>/ { sub(/^<[0-9]>/, ""); print; fflush() }' |
        stamp | tee "$dir/events" &
    exec 3> "$dir/in"
}

# One number of /proc/stat, with builtins alone, which need no disk.
statnum()
{
    while read -r k v; do
        if [ "$k" = "$1" ]; then
            echo "$v"
            return
        fi
    done < /proc/stat
}

# The line of /proc/diskstats of the disk $rootdisk, with builtins alone.
diskline()
{
    while read -r l; do
        set -- $l
        if [ "$3" = "$rootdisk" ]; then
            echo "$l"
            return
        fi
    done < /proc/diskstats
}

# Wait up to 90 s for the network, with an address and a gateway.
joined()
{
    i=0
    while [ "$i" -lt 180 ]; do
        s=$(cli status | sed -n 's/^wpa_state=//p')
        a=$(cli status | sed -n 's/^ip_address=//p')
        g=${RACH2_SOAK_GATEWAY:-$(ip -4 route show default dev "$iface" | awk 'NR == 1 { print $3 }')}
        if [ "$s" = COMPLETED ] && [ -n "$a" ] && [ "$a" != 0.0.0.0 ] && [ -n "$g" ]; then
            gateway=$g
            return 0
        fi
        sleep 0.5
        i=$((i + 1))
    done
    return 1
}

# Write, flush and remove one file until the stop file appears. dd's process
# id goes to /dev/shm so that stopload can reach it.
writer()
{
    while [ ! -e "$dir/stop" ]; do
        dd if=/dev/urandom of="$load/w$1" bs=1M count=6000 conv=fsync 2> /dev/null &
        echo $! > "$dir/dd$1"
        wait $!
        rm -f "$load/w$1"
    done
}

syncer()
{
    while [ ! -e "$dir/stop" ]; do
        sync
    done
}

# Stop the writers with builtins alone; rm and sync end by themselves.
stopload()
{
    : > "$dir/stop"
    for f in "$dir"/dd*; do
        [ -e "$f" ] || continue
        read -r p < "$f" && kill "$p" 2> /dev/null
    done
}

sampler()
{
    while [ ! -e "$dir/stop" ]; do
        iw dev "$iface" station dump 2> /dev/null | awk '
            /tx packets:/ { t = $3 } /tx retries:/ { r = $3 } /tx failed:/ { f = $3 }
            /beacon loss:/ { b = $3 } /signal:/ { s = $2 }
            END { print "tx=" t + 0, "retries=" r + 0, "failed=" f + 0, "bl=" b + 0, "signal=" s }' |
            stamp >> "$dir/samples"
        sleep 5
    done
}

kernelwatch()
{
    while [ ! -e "$dir/stop" ]; do
        dmesg | grep -E 'wlan|ath9k|80211' >> "$dir/kernel.raw"
        sleep 30
    done
}

pinger()
{
    end=$(($(date +%s) + seconds))
    while :; do
        left=$((end - $(date +%s)))
        [ "$left" -gt 0 ] || break
        ping -I "$iface" -i 0.2 -w "$left" "$gateway" 2>&1
        sleep 1
    done | stamp > "$dir/ping"
}

# Print the table from the logs. T0 is the uptime at which the load began.
table()
{
    awk -v t0="$t0" '
        $1 < t0 { next }
        /Associated with|Trying to associate/ { assoc = 1 }
        /WPA: Key negotiation completed/ {
            if (assoc) joins++; else pairwise++
            assoc = 0
        }
        /WPA: Group rekeying completed/ { group++ }
        /CTRL-EVENT-DISCONNECTED/ {
            r = "?"; l = 0
            for (i = 2; i <= NF; i++) {
                if ($i ~ /^reason=/) { r = $i; sub(/^reason=/, "", r) }
                if ($i == "locally_generated=1") l = 1
            }
            k = "reason " r (l ? " locally_generated=1" : " from the AP")
            if (!(k in drops)) order[++n] = k
            drops[k]++; all++
            if (!down) { down = 1; at = $1 }
        }
        /CTRL-EVENT-CONNECTED/ && down {
            down = 0; d = $1 - at; sum += d; rejoins++
            if (d > worst) worst = d
            if (rejoins == 1 || d < best) best = d
        }
        END {
            printf "renewals pairwise        %d\n", pairwise
            printf "renewals group           %d\n", group
            printf "joins                    %d\n", joins
            printf "drops                    %d\n", all
            for (i = 1; i <= n; i++)
                printf "  %-22s %d\n", order[i], drops[order[i]]
            if (rejoins)
                printf "rejoin seconds           %d: least %.2f, mean %.2f, most %.2f\n",
                    rejoins, best, sum / rejoins, worst
            if (down)
                printf "still down at the end    since %s\n", at
        }' "$dir/events"
    # A counter falls when an association begins.
    awk '{
            for (i = 3; i <= NF; i++) {
                split($i, a, "=")
                if (a[1] == "signal") continue
                v = a[2] + 0
                if (v < last[a[1]]) total[a[1]] += last[a[1]]
                last[a[1]] = v
            }
        }
        END {
            t = total["tx"] + last["tx"]; r = total["retries"] + last["retries"]
            printf "tx packets               %d\n", t
            printf "tx retries               %d (%.1f%%)\n", r, t ? 100 * r / t : 0
            printf "tx failed                %d\n", total["failed"] + last["failed"]
            printf "missed beacons counted   %d\n", total["bl"] + last["bl"]
        }' "$dir/samples"
    awk '!seen[$0]++' "$dir/kernel.raw" > "$dir/kernel"
    printf 'beacon-loss lines        %d\n' "$(grep -c ': beacon loss: ' "$dir/kernel")"
    awk '/ packets transmitted, / { sent += $2; got += $5 }
        / icmp_seq=/ {
            if (prev && $1 - prev > gap) gap = $1 - prev
            prev = $1
        }
        END {
            printf "ping                     %d sent, %d answered, %.2f%% lost\n",
                sent, got, sent ? 100 * (sent - got) / sent : 0
            printf "longest ping gap         %.2f s\n", gap
        }' "$dir/ping"
    printf 'longest disk request     %s us, %s\n' "$disk1" "$([ "$disk1" -gt "$disk0" ] &&
        echo 'in this run' || echo "before this run; the run's own was no longer")"
    printf 'longest slot wait        %s us\n' "$(statnum latency_diskwait_us)"
    printf 'longest timer gap        %s us\n' "$(statnum latency_timer_us)"
    printf '%s\n%s\n' "$stats0" "$stats1" | awk '
        NR == 1 { w = $8; s = $10; m = $11 }
        NR == 2 {
            w = $8 - w; printf "disk writes              %d, %.1f GiB, mean %.2f ms\n",
                w, ($10 - s) / 2097152, w ? ($11 - m) / w : 0
        }'
}

# Stop the load and leave the network's settings as they were.
finish()
{
    trap - EXIT
    stopload
    trap '' TERM
    kill 0
    exec 3>&-
    wait
    rm -rf "$load" "$dir/in"
    [ -z "${id:-}" ] || {
        cli set_network "$id" wpa_ptk_rekey 0 > /dev/null
        cli enable_network all > /dev/null
    }
}

# Wait for the writers' files to go, so that the table's commands find the
# disk free.
wait_writers()
{
    while [ -n "$(ls "$load")" ]; do
        sleep 1
    done
}

soak()
{
    seconds=$1
    network=${RACH2_SOAK_NETWORK:-TDA-NET}
    writers=${RACH2_SOAK_WRITERS:-6}
    rekey=${RACH2_SOAK_REKEY:-120}
    id=
    trap finish EXIT
    trap 'exit 129' HUP
    trap 'exit 130' INT
    trap 'exit 143' TERM
    rm -rf "$dir" "$load"
    mkdir -p "$dir" "$load" || exit 1
    id=$(cli list_networks | awk -F '\t' -v n="$network" '$2 == n { print $1 }')
    [ -n "$id" ] || {
        echo "wifi-soak: failed: wpa_supplicant does not know $network" >&2
        exit 1
    }
    events || exit 1
    echo "wifi-soak: joining network $id with wpa_ptk_rekey $rekey"
    cli disconnect > /dev/null
    cli set_network "$id" wpa_ptk_rekey "$rekey" | grep -q OK || {
        echo 'wifi-soak: failed: wpa_supplicant refused wpa_ptk_rekey' >&2
        exit 1
    }
    cli select_network "$id" > /dev/null
    joined || {
        echo "wifi-soak: failed: no address on $network within 90 s" >&2
        exit 1
    }
    rootdisk=$(awk '$2 == "/" && $1 ~ /^\/dev\// {
        sub(/^\/dev\//, "", $1); sub(/p[0-9]+$/, "", $1); print $1 }' /proc/mounts)
    disk0=$(statnum latency_disk_us)
    stats0=$(diskline)
    set -- $(cat /proc/uptime)
    t0=$1
    echo "wifi-soak: load: $writers writers and sync, $seconds s, from uptime $t0 at $(date -u '+%H:%M:%S UTC'), ping $gateway"
    pinger &
    sampler &
    kernelwatch &
    i=0
    while [ "$i" -lt "$writers" ]; do
        writer "$i" &
        i=$((i + 1))
    done
    syncer &
    sleep "$seconds"
    stopload
    disk1=$(statnum latency_disk_us)
    stats1=$(diskline)
    echo "wifi-soak: load stopped at uptime $(cut -d ' ' -f 1 /proc/uptime)"
    wait_writers
    dmesg | grep -E 'wlan|ath9k|80211' >> "$dir/kernel.raw"
    echo "wifi-soak: table: $seconds s"
    table | tee "$dir/table"
    finish
    echo 'wifi-soak: end'
}

case "${1:-}" in
    run)
        [ "$#" -eq 2 ] || usage
        case "$2" in '' | *[!0-9]*) usage ;; esac
        soak "$2"
        ;;
    '' | *[!0-9]*) usage ;;
    *)
        [ "$#" -le 2 ] || usage
        host "$@"
        ;;
esac
