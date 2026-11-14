#!/bin/sh
# deb.sh TREE ACCEL N: run loop.sh N times in the Debian reference guest of
# the tree TREE on the virgl profile, with Debian's piglit installed for the
# boot, outside the site's ledger, as rep.py's guest runs; the loop's output
# and the guest's record go to deb-ACCEL beside this file. DEBIAN_REF_CACHE
# names the main clone's prepared image when TREE is a copy of its own.
set -eu
tree=$1 accel=$2 n=$3
here=$(cd "$(dirname "$0")" && pwd)
mkdir -p "$here/deb-$accel"
DEBIAN_REF_CLAIM=unclaimed ACCEL=$accel \
    sh "$tree/tests/graphics/debian-ref/debian-ref.sh" run -p virgl -c 8 -m 8192 -g conformance -d \
    -r "$here/deb-$accel/record" -i "$here/loop.sh" -o "$here/deb-$accel/loop.out" \
    "cd /tmp && sudo DEBIAN_FRONTEND=noninteractive apt-get -q -y install piglit >> /tmp/apt.log 2>&1 && sh /tmp/loop.sh $n /tmp/loop.out"
