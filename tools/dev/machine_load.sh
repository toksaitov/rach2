#!/bin/sh
#
# machine_load.sh KEY ADDRESS SESSIONS SECONDS [DIR]
#
# The load row of milestone 46, run against an installed machine over SSH:
# SESSIONS sessions, each copying the kernel tree and building it with the
# staged TinyCC and pdpmake, each for SECONDS seconds on the guest clock.
# Each session reports separately. DIR holds the logs; it defaults to the cwd.
#
# The sessions start a second apart. Dropbear allows five unauthenticated
# connections from one address at a time (MAX_UNAUTH_PER_IP in the port's
# default_options.h, which localoptions.h does not change), so twenty started
# at once get five and the rest are refused before they reach the key
# exchange. A second between them is more than the handshake needs and keeps
# the number of sessions the caller asked for.
#
# Each session's build output is kept in its own log beside the report, so a
# failure can be read afterwards; the copies are left in a private directory on
# the machine. The exit status is nonzero if any session failed to run, if
# fewer reported than were asked for, or if any build failed.
set -u
[ $# -ge 4 ] && [ $# -le 5 ] || {
    echo "usage: machine_load.sh KEY ADDRESS SESSIONS SECONDS [DIR]" >&2; exit 2; }
KEY=$1
ADDR=$2
SESSIONS=$3
SECONDS_EACH=$4
DIR=${5:-.}
LOAD_USER=${RACH2_LOAD_USER:-root}
case "$LOAD_USER" in
    ''|-*|*[!A-Za-z0-9_.-]*)
        echo "machine_load: invalid account name: $LOAD_USER" >&2; exit 2 ;;
esac

# Both numbers reach the machine inside a shell command, so each is required to
# be a bounded decimal before anything is sent: an argument that is not one
# would otherwise run there as root.
for v in "$SESSIONS" "$SECONDS_EACH"; do
    case "$v" in
        ''|*[!0-9]*) echo "machine_load: $v is not a whole number" >&2; exit 2 ;;
    esac
done
[ "$SESSIONS" -ge 1 ] && [ "$SESSIONS" -le 64 ] || {
    echo "machine_load: SESSIONS must be 1 to 64" >&2; exit 2; }
[ "$SECONDS_EACH" -ge 1 ] && [ "$SECONDS_EACH" -le 86400 ] || {
    echo "machine_load: SECONDS must be 1 to 86400" >&2; exit 2; }
[ -r "$KEY" ] || { echo "machine_load: no key at $KEY" >&2; exit 1; }
[ -d "$DIR" ] || { echo "machine_load: no directory at $DIR" >&2; exit 1; }

# One run's trees and logs are its own, so two runs cannot delete each other's
# work or read each other's results.
RUN=$$
SESSION_SCRIPT=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)/machine_load_session.sh
[ -r "$SESSION_SCRIPT" ] || exit 1
REMOTE="$(ssh -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
    -o LogLevel=ERROR -o ConnectTimeout=10 -o BatchMode=yes "$LOAD_USER@$ADDR" \
    'mktemp -d /tmp/rach2-load.XXXXXX')" || exit 1
case "$REMOTE" in
    /tmp/rach2-load.*) ;;
    *) echo "machine_load: the machine did not create a work directory" >&2; exit 1 ;;
esac
case "$REMOTE" in
    *[!A-Za-z0-9_./-]*) echo "machine_load: invalid work directory" >&2; exit 1 ;;
esac
echo 'machine_load: clock: guest delivered ticks; separate session windows; late successes counted separately'
echo "machine_load: working: $LOAD_USER@$ADDR $REMOTE, logs $DIR/load-$RUN-sessionN.log"
pids=""
n=1
while [ "$n" -le "$SESSIONS" ]; do
    ssh -i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null \
        -o LogLevel=ERROR -o ServerAliveInterval=30 -o ServerAliveCountMax=2 \
        -o ConnectTimeout=10 -o BatchMode=yes "$LOAD_USER@$ADDR" \
        "sh -s -- $n $SECONDS_EACH $REMOTE/b$n" \
        < "$SESSION_SCRIPT" \
        > "$DIR/load-$RUN-session$n.log" 2>&1 &
    pids="$pids $!"
    n=$((n + 1))
    sleep 1
done
rc=0
for pid in $pids; do
    wait "$pid" || rc=1
done
reports=$(grep -h "^SESSION" "$DIR"/load-"$RUN"-session*.log | sort -k2 -n)
echo "$reports"
count=$(echo "$reports" | grep -c "^SESSION")
[ "$count" -eq "$SESSIONS" ] || {
    echo "machine_load: $count sessions reported of $SESSIONS asked for" >&2; rc=1; }
echo "$reports" | grep -q "failed [1-9]" && {
    echo "machine_load: a session reported a failed build" >&2; rc=1; }
exit $rc
