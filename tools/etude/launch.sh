#!/bin/sh
# launch.sh: admit a build or an old runner under the installed site policy.
set -eu

# Refuse before execution when the installed authority cannot be used.
refuse()
{
    echo "etude-launch: result: refused: $*" >&2
    [ -z "$result" ] || printf 'refused\n' > "$result"
    exit 2
}

result=
if [ "${1:-}" = -r ]; then
    [ "$#" -ge 3 ] || exit 2
    result=$2
    shift 2
fi
[ "$#" -ge 2 ] || { echo 'usage: etude-launch [-r FILE] build|legacy COMMAND [ARGS...]' >&2; exit 2; }
kind=$1; shift
case $kind in build|legacy) ;; *) refuse "unknown launch kind $kind" ;; esac
site=${ETUDE_SITE:-/etc/rach2/etude-site}
if [ ! -e "$site" ] && [ ! -L "$site" ] && [ "${ETUDE_SITE+x}" != x ]; then
    exec "$@"
fi
[ -r "$site" ] || refuse "cannot read $site"
header="" endpoint="" launcher="" ledger="" locks="" bridge="" capacity="" wait="" seconds=""
seen=" "
while IFS=' ' read -r key value || [ -n "$key$value" ]; do
    case $seen in *" $key "*) refuse "duplicate field $key" ;; esac
    seen="$seen$key "
    [ -n "$value" ] || refuse "empty field $key"
    case $key in
        etude-site) [ -z "$header" ] || refuse 'duplicate header'; header=$value ;;
        endpoint) [ -z "$endpoint" ] || refuse 'duplicate endpoint'; endpoint=$value ;;
        launcher) [ -z "$launcher" ] || refuse 'duplicate launcher'; launcher=$value ;;
        ledger) [ -z "$ledger" ] || refuse 'duplicate ledger'; ledger=$value ;;
        locks) [ -z "$locks" ] || refuse 'duplicate locks'; locks=$value ;;
        bridge) [ -z "${bridge:-}" ] || refuse 'duplicate bridge'; bridge=$value ;;
        capacity) [ -z "$capacity" ] || refuse 'duplicate capacity'; capacity=$value ;;
        wait) [ -z "$wait" ] || refuse 'duplicate wait'; wait=$value ;;
        seconds) [ -z "$seconds" ] || refuse 'duplicate seconds'; seconds=$value ;;
        *) refuse "unknown field $key" ;;
    esac
done < "$site"
[ "$header" = 1 ] || refuse 'incompatible site format'
case $(uname -s)/$endpoint in Linux/linux|Darwin/macos) ;; *) refuse 'endpoint is disabled or incompatible' ;; esac
for path in "$site" "$launcher" "$ledger" "$locks" "${bridge:-}"; do
    case $path in /*) ;; *) refuse "not an absolute path: $path" ;; esac
done
[ -x "$bridge" ] && [ -x "$launcher" ] && [ -d "$locks" ] && [ -d "$ledger" ] ||
    refuse 'installed endpoint is unavailable'
[ "$(head -n 1 "$ledger/claims" 2>/dev/null)" = 'etude-ledger 1' ] ||
    refuse 'ledger is unavailable or incompatible'
printf '%s\n' "$capacity" | awk -F, '
    NF != 3 { exit 1 }
    { for (i = 1; i <= 3; i++) if ($i !~ /^[1-9][0-9]*$/ || length($i) > 15) exit 1 }
' || refuse 'capacity must give three positive amounts of at most 15 digits'
for bound in "$wait" "$seconds"; do
    case $bound in ''|*[!0-9]*|0*) refuse 'invalid time bound' ;; esac
    [ "${#bound}" -le 5 ] && [ "$bound" -le 86400 ] || refuse 'invalid time bound'
done
if [ "$kind" = legacy ]; then
    grep -q RACH2_PORT_OFFSET tests/system/suites.mk 2>/dev/null ||
        refuse 'the checkout does not support reserved port offsets'
fi
if [ -n "${ETUDE_ADMITTED:-}" ]; then
    [ "$ETUDE_ADMITTED" = "$site" ] || refuse 'nested launch changed the site'
    if [ -n "$result" ]; then
        status=0
        "$@" || status=$?
        if [ "$status" -ge 128 ]; then
            printf 'incomplete\n' > "$result"
        else
            printf 'exited %s\n' "$status" > "$result"
        fi
        exit "$status"
    fi
    exec "$@"
fi
[ -z "${RACH2_PORT_OFFSET+x}${SSH_TEST_PORT+x}" ] || refuse 'port overrides bypass admission'
cpu=${capacity%%,*}
memory=${capacity#*,}; memory=${memory%%,*}
RACH2_WORKERS=$((cpu / 1000 / 4))
[ "$RACH2_WORKERS" -gt 0 ] || RACH2_WORKERS=1
[ "$RACH2_WORKERS" -le 1024 ] || RACH2_WORKERS=1024
RACH2_MEMORY_MIB=$((memory * 3 / 4 / 1048576))
[ "$RACH2_MEMORY_MIB" -gt 0 ] || refuse 'capacity has no guest memory'
ETUDE_ADMITTED=$site
export ETUDE_ADMITTED RACH2_WORKERS RACH2_MEMORY_MIB
nonce=$(od -An -N16 -tx1 /dev/urandom | tr -d ' \n')
[ "${#nonce}" -eq 32 ] || refuse 'cannot make an attempt identity'
environment=$(command -v env) || refuse 'env is unavailable'
if [ -n "$result" ]; then
    exec "$bridge" -i -p -d "$locks" -w "$wait" -r "$result" "$ledger" \
        "launch:$nonce" "$(id -un)" alone "$capacity" "$capacity" "$seconds" "$environment" "$@"
fi
exec "$bridge" -i -p -d "$locks" -w "$wait" "$ledger" \
    "launch:$nonce" "$(id -un)" alone "$capacity" "$capacity" "$seconds" "$environment" "$@"
