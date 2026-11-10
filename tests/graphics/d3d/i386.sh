#!/bin/sh
# i386.sh: give the Debian reference guest the i386 layer of Rach2's x86-64
# root, so that Proton's default mode runs an i386 program on its i386 Wine
# there as on Rach2. reference.sh runs it as root before the rows.
#
#   i386.sh I386-LIST VENUS-LIST
#
# I386-LIST is vendor/gnu/debian/i386.list, each package of which is
# installed for i386 from the guest's dated snapshot at the version it pins,
# but for vulkan-tools and mesa-utils-bin, the programs the root carries to
# show its i386 drivers, which Wine does not load. VENUS-LIST is
# vendor/gnu/debian/venus.list, whose i386 package gives Mesa 26.1.6's i386
# Venus driver, installed as setup.sh.in installs the amd64 one: under a path
# of its own, with an ICD, venus_icd.i686.json, that names it. Mesa 25.0.7's
# Venus ICD, which both architectures share, is diverted already. apt's
# output goes to /var/tmp/d3d-i386-apt.log, and to standard error only when
# apt fails: once, after an install that succeeded, apt repeated one warning
# past the 64 MiB of output the run may have.
set -eu

log=/var/tmp/d3d-i386-apt.log

die()
{
    echo "d3d-i386: $*" >&2
    exit 1
}

# aptq ARG...: run apt-get quietly into the log, showing its end on failure.
aptq()
{
    apt-get -q "$@" >> "$log" 2>&1 || {
        tail -n 40 "$log" >&2
        die "apt-get $1 failed; its output is in $log"
    }
}

work=
trap '[ -z "$work" ] || rm -rf "$work"' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

[ $# -eq 2 ] || die "usage: i386.sh I386-LIST VENUS-LIST"
list=$1 venus=$2
export DEBIAN_FRONTEND=noninteractive
: > "$log"
echo "d3d-i386: $(apt-get --version | head -n 1)"
dpkg --add-architecture i386
aptq update
packages=$(sed 's/#.*//' "$list" | awk 'NF && $1 !~ /^(vulkan-tools|mesa-utils-bin)(=|$)/ {
    n = index($1, "=")
    print n ? substr($1, 1, n - 1) ":i386" substr($1, n) : $1 ":i386"
}')
[ -n "$packages" ] || die "$list names no package"
# shellcheck disable=SC2086 # the list is words
aptq install -y $packages
echo "d3d-i386: apt: $(grep -c . "$log") lines, $(grep -c '^W: ' "$log") warnings"

# shellcheck disable=SC2046 # the line is NAME VERSION URL SHA-256
set -- $(awk '$1 == "mesa-vulkan-drivers-i386"' "$venus")
[ $# -eq 4 ] || die "$venus has no i386 line"
work=$(mktemp -d /var/tmp/d3d-i386.XXXXXX)
curl -sSfL --retry 3 -o "$work/venus.deb" "$3"
echo "$4  $work/venus.deb" | sha256sum -c --quiet - || die "the i386 Venus package's SHA-256 differs"
dpkg-deb -x "$work/venus.deb" "$work/data"
lib=/usr/lib/i386-linux-gnu/venus/libvulkan_virtio.so
icd=/usr/share/vulkan/icd.d/venus_icd.i686.json
install -D -m 755 "$work/data/usr/lib/i386-linux-gnu/libvulkan_virtio.so" "$lib"
sed "s|\"libvulkan_virtio.so\"|\"$lib\"|" "$work/data/usr/share/vulkan/icd.d/virtio_icd.json" > "$icd"
grep -q "\"$lib\"" "$icd" || die "$icd does not name $lib"
aptq clean -y
echo "d3d-i386: result: $(echo "$packages" | wc -l) i386 packages and Venus $2 installed"
