#!/bin/sh
# Build the private input-seat module against the reference guest's Weston.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
out=$1
work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$(dirname "$out")" "${TMPDIR:-$here/../../../build/tmp}"
work=$(mktemp -d "${TMPDIR:-$here/../../../build/tmp}/browser-seat.XXXXXX")
sh "$here/../debian-ref/debian-ref.sh" prepare -n
sh "$here/../debian-ref/debian-ref.sh" run -c 2 -m 2048 -l -i "$here/seat.c" -o "$work/seat.so" \
    'sudo DEBIAN_FRONTEND=noninteractive apt-get -q -y install libweston-14-dev pkg-config > /tmp/seat-build.log 2>&1 ||
     { cat /tmp/seat-build.log; exit 1; };
     cc -Wall -Wextra -Werror -fPIC -shared $(pkg-config --cflags libweston-14) /tmp/seat.c -o /tmp/seat.so'
mv "$work/seat.so" "$out"
