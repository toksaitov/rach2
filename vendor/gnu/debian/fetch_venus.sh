#!/bin/sh
# Stage Mesa 26.1.6's Venus driver, alone, from the package venus.list pins,
# under a library path and ICD file of its own. Usage: fetch_venus.sh [STAGE].
#
# The stage, STAGE of the GNU build tree, venus/ by default, holds:
#
#   usr/lib/MULTIARCH/venus/libvulkan_virtio.so   the driver
#   usr/share/vulkan/icd.d/venus_icd.ARCH.json    its ICD, by absolute path
#   usr/share/doc/mesa-venus/copyright            the package's notice
#
# where MULTIARCH is x86_64-linux-gnu or i386-linux-gnu and ARCH is x86_64 or
# i686. Debian's own ICD names the library bare, and Mesa 25.0.7's library of
# the same name is in the loader's search path, so the ICD here names its
# driver's path; install_root.sh removes 25.0.7's Venus ICD and library.
#
# The driver is marked DF_1_NODELETE (nodelete.py), so that the Vulkan
# loader's dlclose leaves it mapped: it registers a thread-specific data
# destructor that it never deletes, which a thread would otherwise call at
# exit after the driver was unloaded and loaded again at another address.

set -e

DIR="$(cd "$(dirname "$0")" && pwd)"
GNU="$(cd "$DIR/.." && pwd)"
# shellcheck disable=SC2034 # arch.sh and fetch_asset.sh read it
REPO="$(cd "$GNU/../.." && pwd)"
. "$GNU/arch.sh"
. "$DIR/fetch_asset.sh"
OUT="$GNU_OUT"
case ${1:-venus} in
    *[!a-z0-9_-]*) echo "gnu: venus: invalid stage: $1" >&2; exit 2 ;;
esac
STAGE="$OUT/${1:-venus}"
WORK="$OUT/work/${1:-venus}"
LIB="usr/lib/$GNU_MULTIARCH/venus/libvulkan_virtio.so"
ICD="usr/share/vulkan/icd.d/venus_icd.$RACH2_ARCH.json"

for tool in ar xz tar python3; do
    command -v "$tool" >/dev/null 2>&1 || {
        echo "gnu: venus: $tool not found" >&2
        exit 1
    }
done

deb=$(asset "mesa-vulkan-drivers-$GNU_DEB_ARCH" "$DIR/venus.list")
rm -rf "$STAGE" "$WORK"
mkdir -p "$WORK/data"
trap 'rm -rf "$WORK"' EXIT
member=$(ar t "$deb" | grep '^data\.tar\.xz$') || {
    echo "gnu: venus: ${deb##*/} has no data.tar.xz" >&2
    exit 1
}
ar p "$deb" "$member" | xz -dc | tar -xf - -C "$WORK/data" \
    "./usr/lib/$GNU_MULTIARCH/libvulkan_virtio.so" \
    ./usr/share/vulkan/icd.d/virtio_icd.json \
    ./usr/share/doc/mesa-vulkan-drivers/copyright

# Debian's ICD is four fields, of which the library's path is the one
# changed; any other form is refused, so that a new pin is read again.
icd="$WORK/data/usr/share/vulkan/icd.d/virtio_icd.json"
if ! grep -q '^        "library_path": "libvulkan_virtio.so"$' "$icd" ||
    [ "$(grep -c '"' "$icd")" -ne 4 ]; then
    echo "gnu: venus: refused: virtio_icd.json is not the ICD this script rewrites:" >&2
    cat "$icd" >&2
    exit 1
fi

mkdir -p "$STAGE/${LIB%/*}" "$STAGE/${ICD%/*}" "$STAGE/usr/share/doc/mesa-venus"
cp "$WORK/data/usr/lib/$GNU_MULTIARCH/libvulkan_virtio.so" "$STAGE/$LIB"
python3 -I "$DIR/nodelete.py" "$STAGE/$LIB" >/dev/null
sed "s|\"libvulkan_virtio.so\"|\"/$LIB\"|" "$icd" > "$STAGE/$ICD"
cp "$WORK/data/usr/share/doc/mesa-vulkan-drivers/copyright" \
    "$STAGE/usr/share/doc/mesa-venus/copyright"
chmod 0755 "$STAGE/$LIB"
chmod 0644 "$STAGE/$ICD" "$STAGE/usr/share/doc/mesa-venus/copyright"
echo "gnu: venus: result: $(awk -v n="mesa-vulkan-drivers-$GNU_DEB_ARCH" \
    '$1 == n { print $2 }' "$DIR/venus.list") staged as /$LIB and /$ICD"
