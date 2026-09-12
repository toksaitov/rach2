#!/bin/sh
#
# machine_load.sh KEY ADDRESS SESSIONS SECONDS [DIR]
#
# The load row of milestone 46, run against an installed machine over SSH:
# SESSIONS sessions, each copying the kernel tree and building it with the
# staged TinyCC and pdpmake over and over for SECONDS, with a report a
# session. DIR is where the logs go, the current directory by default.
#
# The sessions start a second apart. Dropbear allows five unauthenticated
# connections from one address at a time (MAX_UNAUTH_PER_IP in the port's
# default_options.h, which localoptions.h does not change), so twenty started
# at once get five and the rest are refused before they reach the key
# exchange. A second between them is more than the handshake needs and keeps
# the number of sessions the caller asked for.
#
# It reads nothing from the machine but the build's own output, and leaves
# each session's copy under /tmp/load on it.
set -u
KEY=$1
ADDR=$2
SESSIONS=$3
SECONDS_EACH=$4
DIR=${5:-.}

[ -r "$KEY" ] || { echo "machine_load: no key at $KEY" >&2; exit 1; }
n=1
while [ "$n" -le "$SESSIONS" ]; do
    ssh -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR -o ServerAliveInterval=30 "root@$ADDR" \
        "mkdir -p /tmp/load; rm -rf /tmp/load/b$n; cp -r /src/rach2 /tmp/load/b$n; \
         cd /tmp/load/b$n; built=0; failed=0; \
         end=\$((\$(cut -d. -f1 /proc/uptime) + $SECONDS_EACH)); \
         while [ \$(cut -d. -f1 /proc/uptime) -lt \$end ]; do \
             make -f guest.mk clean > /dev/null 2>&1; \
             if make -f guest.mk > /dev/null 2>&1; then built=\$((built+1)); \
             else failed=\$((failed+1)); fi; \
         done; echo \"SESSION $n builds \$built failed \$failed\"" \
        > "$DIR/session$n.log" 2>&1 &
    n=$((n + 1))
    sleep 1
done
wait
grep -h "^SESSION" "$DIR"/session*.log | sort -k2 -n
