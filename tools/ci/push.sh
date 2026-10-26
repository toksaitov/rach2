#!/bin/sh
# Check the explicit affected suites on the selected local execution site.
set -eu
[ "$#" -ge 2 ] || {
    echo 'usage: tools/ci/push.sh NEW_OUT SUITE|ARCH:SUITE...' >&2
    exit 2
}
out=$1
shift
ETUDE_PURPOSE=push
export ETUDE_PURPOSE
program=${ETUDE:-}
if [ -z "$program" ]; then
    site=${ETUDE_SITE:-}
    if [ -z "$site" ]; then
        case $(uname -s) in
            MINGW*|MSYS*|UCRT*|CLANG*|CYGWIN*) site=C:/ProgramData/rach2/etude-site ;;
            *) site=/etc/rach2/etude-site ;;
        esac
    fi
    program=$(sed -n 's/^etude //p' "$site") || exit 2
fi
case $program in /*|[A-Za-z]:/*) ;; *) echo 'ci: installed Etude is absent' >&2; exit 2 ;; esac
exec "$program" run routine push "$out" "$@"
