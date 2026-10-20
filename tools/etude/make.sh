#!/bin/sh
# make.sh: keep top-level builds inside the admitted job count.
set -eu
# A scrubbed child environment uses one job within its enclosing run.
if [ "${ETUDE_BUILD_JOBS+x}" != x ]; then
    ETUDE_BUILD_JOBS=1
    if [ -z "${ETUDE_MAKE_REAL:-}" ]; then
        etude_self=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
        etude_search=${PATH:-/usr/bin:/bin}:
        while [ -n "$etude_search" ]; do
            etude_dir=${etude_search%%:*}
            etude_search=${etude_search#*:}
            [ -n "$etude_dir" ] || etude_dir=.
            [ -f "$etude_dir/make" ] && [ -x "$etude_dir/make" ] || continue
            etude_dir=$(CDPATH='' cd -- "$etude_dir" 2>/dev/null && pwd) || continue
            [ "$etude_dir" != "$etude_self" ] || continue
            ETUDE_MAKE_REAL=$etude_dir/make
            export ETUDE_MAKE_REAL
            break
        done
    fi
fi
case ${ETUDE_BUILD_JOBS:-} in
    ''|*[!0-9]*|0*) echo 'etude-make: invalid job count' >&2; exit 2 ;;
esac
[ "${#ETUDE_BUILD_JOBS}" -le 4 ] && [ "$ETUDE_BUILD_JOBS" -le 1024 ] || exit 2
case ${ETUDE_MAKE_REAL:-} in
    /*) ;;
    *) echo 'etude-make: make is not an absolute path' >&2; exit 2 ;;
esac
[ "$ETUDE_MAKE_REAL" != "$0" ] || exit 2
# Recursive makes inherit their parent's jobserver.
case ${MAKELEVEL:-0} in
    0)
        requested=$( { printf '%s\n' "${MAKEFLAGS:-}"; printf '%s\n' "$@"; } | awk -v limit="$ETUDE_BUILD_JOBS" '
            BEGIN { jobs = 1 }
            function take(arg) {
                if (pending) {
                    pending = 0
                    if (arg ~ /^[1-9][0-9]*$/) { jobs = arg; return }
                }
                if (arg == "-j" || arg == "--jobs") { jobs = limit; pending = 1 }
                else if (arg ~ /^-j[0-9]+$/) jobs = substr(arg, 3)
                else if (arg ~ /^--jobs=/) jobs = substr(arg, 8)
            }
            NR == 1 {
                n = split($0, flags, /[[:space:]]+/)
                for (i = 1; i <= n && flags[i] != "--"; i++) take(flags[i])
                pending = 0; next
            }
            { take($0) }
            END { print (jobs + 0 >= 1 && jobs + 0 < limit + 0 ? jobs : limit) }
        ')
        set -- "$@" "-j$requested"
        ;;
esac
exec "$ETUDE_MAKE_REAL" "$@"
