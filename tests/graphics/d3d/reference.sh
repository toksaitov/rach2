#!/bin/sh
# reference.sh: run Direct3D rows in the Debian reference guest with no
# reference, as the stored Linux references are taken.
#
#   reference.sh [-a ADAPTER] [-p PROFILE] [-c CPUS] [-m MIB] OUT FAMILY/ROW...
#
# Boots the Debian guest once with debian-ref.sh run in the display PROFILE
# (venus) with CPUS processors (8) and MIB of memory (16384), as a Done's
# conformance run, with this tree's bundle.tar, which it makes first, and the
# pinned GE-Proton, unpacked under /var/tmp. Each FAMILY/ROW, such as
# probe/proton/11, wine/wined3dvk/d3d9:visual or vkd3d/3, runs in turn as
# `run.py -a ADAPTER -R FAMILY ROW`, ADAPTER being venus unless -a names
# lavapipe, its output in OUT/N, N counting the rows from 1, with its exit
# status in OUT/N.status. The runner's lines come out as they are written.
# mkref.py makes the reference from OUT's directories. OUT gets the guest's
# record, and the host's half from common.sh host, as OUT/record.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../../.." && pwd)

die()
{
    echo "d3d-reference: $*" >&2
    exit 1
}

work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

adapter=venus profile=venus cpus=8 mib=16384
while getopts a:p:c:m: o; do
    case $o in
        a) adapter=$OPTARG ;;
        p) profile=$OPTARG ;;
        c) cpus=$OPTARG ;;
        m) mib=$OPTARG ;;
        *) exit 2 ;;
    esac
done
shift $((OPTIND - 1))
[ $# -ge 2 ] || die "usage: reference.sh [-a ADAPTER] [-p PROFILE] [-c CPUS] [-m MIB] OUT FAMILY/ROW..."
out=$1
shift
[ ! -e "$out" ] || die "$out exists"

make -s -C "$tree" ARCH=x86_64 build/tests/graphics/d3d/bundle.tar || die "the bundle could not be made"
proton=$(sh "$tree/vendor/gnu/fetch.sh" --path ge-proton) || die "GE-Proton could not be fetched"
mkdir -p "${TMPDIR:-$tree/build/tmp}"
work=$(mktemp -d "${TMPDIR:-$tree/build/tmp}/rach2-d3dref.XXXXXX")
rows="" n=0
for r in "$@"; do
    n=$((n + 1))
    family=${r%%/*} row=${r#*/}
    case $family in
        probe|wine|vkd3d) ;;
        *) die "$r: not probe/ROW, wine/ROW or vkd3d/ROW" ;;
    esac
    rows="$rows python3 /var/tmp/d3d/run.py -a $adapter -R $family '$row' /tmp/out/$n;
        echo \$? > /tmp/out/$n.status;"
done
sh "$tree/tests/graphics/debian-ref/debian-ref.sh" prepare -n
sh "$tree/tests/graphics/debian-ref/debian-ref.sh" run -p "$profile" -c "$cpus" -m "$mib" \
    -g conformance -d -r "$work/machine" \
    -i "$tree/build/tests/graphics/d3d/bundle.tar" -i "$proton" -o "$work/out.tar.gz" \
    "mkdir -p /var/tmp/d3d /var/tmp/proton /tmp/out &&
     tar -xf /tmp/bundle.tar -C /var/tmp/d3d && rm /tmp/bundle.tar &&
     tar -xzf /tmp/$(basename "$proton") -C /var/tmp/proton --strip-components=1 &&
     rm /tmp/$(basename "$proton") && export D3D_PROTON=/var/tmp/proton &&
     $rows tar -czf /tmp/out.tar.gz -C /tmp/out ." ||
    die "the guest's run failed"
mkdir -p "$out"
tar -xzf "$work/out.tar.gz" -C "$out"
{
    sed 's/^/guest /' "$work/machine"
    sh "$tree/tests/graphics/runners/common.sh" host
} > "$out/record"
echo "d3d-reference: result: $n rows in $out"
