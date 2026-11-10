#!/bin/sh
# List each package of a Debian layer's closure with its Multi-Arch field,
# from the indices fetch_debs.sh resolved it against. Usage: multiarch.sh
# LAYER, after fetch_debs.sh LAYER.
#
# The list, work/debian/LAYER/multiarch.txt of the GNU build tree, has a line
# NAME FIELD for each package of closure.txt, FIELD being `none` where the
# package has none. install_root.sh takes only the `same` packages of the
# x86-64 root's i386 layer, as dpkg installs only those of a second
# architecture beside the first's.

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
GNU="$(cd "$DIR/.." && pwd)"
# shellcheck disable=SC2034 # arch.sh reads it
REPO="$(cd "$GNU/../.." && pwd)"
. "$GNU/arch.sh"
LAYER=$1
case $LAYER in
    *[!a-z0-9_-]*|'') echo "gnu: debian: invalid layer: $LAYER" >&2; exit 2 ;;
esac
WORK="$GNU_OUT/work/debian/$LAYER"
for f in "$WORK/closure.txt" "$WORK/trixie.Packages"; do
    [ -f "$f" ] || { echo "gnu: debian: no $f; run fetch_debs.sh $LAYER" >&2; exit 1; }
done

# A package of one name and version has one Multi-Arch field in every suite.
awk '/^Package: / { pkg = $2 }
    /^Version: / { ver = $2 }
    /^Multi-Arch: / { ma = $2 }
    /^$/ { if (pkg != "") print pkg, ver, ma == "" ? "none" : ma
           pkg = ver = ma = "" }' "$WORK"/*.Packages |
    awk 'NR == FNR { ma[$1 " " $2] = $3; next }
        !(($1 " " $2) in ma) {
            print "gnu: debian: " $1 " " $2 ": not in the indices" > "/dev/stderr"
            exit 1
        }
        { print $1, ma[$1 " " $2] }' - "$WORK/closure.txt" > "$WORK/multiarch.txt"
echo "gnu: debian: result: $(grep -c ' same$' "$WORK/multiarch.txt") of" \
    "$(wc -l < "$WORK/closure.txt") packages are Multi-Arch: same"
