#!/bin/sh
# guest.sh: the native-context stack's steps inside the Debian 13 builder
# guest, which build.sh copies in with the tarballs and runs as `gfx`.
#
#   guest.sh mesa TARBALL OUT        build Mesa into /opt/rach2-nctx/mesa
#   guest.sh virgl TARBALL OUT       build virglrenderer into .../virglrenderer
#   guest.sh image MESA VIRGL        install the backports kernel, QEMU and
#                                    both builds: the VM image's variant
#   guest.sh check                   print what the variant holds
#
# Every step reads its versions from ./pins and the snapshot's date from
# ./snapshot, beside this script. A build writes OUT, a tar of its prefix,
# and OUT.log, the meson summary and the apt downloads.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
top=/opt/rach2-nctx
export DEBIAN_FRONTEND=noninteractive

die()
{
    echo "nctx: $*" >&2
    exit 1
}

pin()
{
    awk -v n="$1" '$1 == n { print $2 }' "$here/pins"
}

# backports: add trixie-backports, binaries and sources, from the snapshot of
# the guest's own date. apt keeps its default priority of 100 for it, so
# only what a step names by version comes from there.
backports()
{
    [ ! -f /etc/apt/sources.list.d/backports.sources ] || return 0
    sudo tee /etc/apt/sources.list.d/backports.sources > /dev/null <<SOURCES
Types: deb deb-src
URIs: http://snapshot.debian.org/archive/debian/$(cat "$here/snapshot")
Suites: trixie-backports
Components: main
Signed-By: /usr/share/keyrings/debian-archive-keyring.gpg
SOURCES
    sudo apt-get -q update > /dev/null
}

# fetched LOG: append apt's download total from its output LOG to the step's
# record.
fetched()
{
    grep -E '^(Need to get|Fetched) ' "$1" | sed 's/^/apt: /' >> "$log" || true
}

# meson_build NAME TARBALL OUT OPTIONS...: unpack TARBALL, configure it into
# $top/NAME with OPTIONS, build, install under a staging root and tar the
# prefix to OUT.
meson_build()
{
    name=$1 tarball=$2 out=$3
    shift 3
    work=$HOME/nctx-$name
    rm -rf "$work"
    mkdir -p "$work/src" "$work/root"
    tar -xf "$tarball" -C "$work/src" --strip-components=1
    meson setup "$work/b" "$work/src" --prefix="$top/$name" --libdir=lib \
        --buildtype=release -Db_ndebug=true "$@" > "$work/setup.log" 2>&1 ||
        { tail -n 40 "$work/setup.log" >&2; die "$name: meson setup failed"; }
    grep -E 'intel-virtio|HAVE_INTEL_VIRTIO|drm-renderers|i915|Gallium drivers|Vulkan drivers|Platforms|LLVM|venus|drm' \
        "$work/setup.log" | sed 's/^/meson: /' >> "$log" || true
    ninja -C "$work/b" > "$work/build.log" 2>&1 ||
        { tail -n 40 "$work/build.log" >&2; die "$name: build failed"; }
    DESTDIR=$work/root ninja -C "$work/b" install > "$work/install.log" 2>&1 ||
        { tail -n 40 "$work/install.log" >&2; die "$name: install failed"; }
    tar -C "$work/root" -cJf "$out" ."$top/$name"
    echo "size: $(wc -c < "$out") bytes, $(du -sk "$work/root$top/$name" | cut -f1) KiB installed" >> "$log"
    rm -rf "$work"
}

mesa()
{
    log=$2.log
    : > "$log"
    backports
    # Its build dependencies come from trixie-backports where it has them,
    # as for Debian's own build of this version (directx-headers-dev).
    sudo apt-get -q -y -t trixie-backports build-dep "$(pin mesa_source)" \
        > "$HOME/apt-mesa.log" 2>&1 ||
        { tail -n 30 "$HOME/apt-mesa.log" >&2; die "mesa: build-dep failed"; }
    fetched "$HOME/apt-mesa.log"
    dpkg-query -W -f '${Package} ${Version}\n' | grep '~bpo' | sed 's/^/backports: /' >> "$log" || true
    # Crocus for Haswell, with Intel's virtio path; virgl, which the loader
    # takes on a virtio-gpu node that offers no native-context capset; and
    # llvmpipe for software. GL, GLES 2, EGL and GBM through glvnd as
    # Debian's Mesa; no Vulkan, since hasvk has no native context.
    meson_build mesa "$1" "$2" \
        -Dintel-virtio-experimental=true \
        -Dgallium-drivers=crocus,virgl,llvmpipe -Dvulkan-drivers= \
        -Dplatforms=x11,wayland -Dglvnd=enabled -Dgbm=enabled -Degl=enabled \
        -Dglx=dri -Dgles1=disabled -Dgles2=enabled -Dllvm=enabled \
        -Dshared-llvm=enabled -Dgallium-va=disabled -Dvideo-codecs= \
        -Dgallium-rusticl=false -Dbuild-tests=false -Dtools= -Dvalgrind=disabled
}

virgl()
{
    log=$2.log
    : > "$log"
    backports
    sudo apt-get -q -y build-dep "$(pin virgl_source)" > "$HOME/apt-virgl.log" 2>&1 ||
        { tail -n 30 "$HOME/apt-virgl.log" >&2; die "virgl: build-dep failed"; }
    sudo apt-get -q -y install meson ninja-build libvulkan-dev libdrm-dev \
        >> "$HOME/apt-virgl.log" 2>&1 || die "virgl: install failed"
    fetched "$HOME/apt-virgl.log"
    # i915's native context beside virgl and Venus, which QEMU's capsets
    # offer as before; Venus's render server runs each context in a process.
    meson_build virglrenderer "$1" "$2" \
        -Dvenus=true -Ddrm-renderers=i915-experimental -Dvideo=false \
        -Dtests=false
}

image()
{
    log=$HOME/image.log
    : > "$log"
    backports
    q=$(pin qemu)
    sudo apt-get -q -y install "$(pin kernel)" "qemu-system-x86=$q" \
        "qemu-system-common=$q" "qemu-system-data=$q" "qemu-system-gui=$q" \
        "qemu-system-modules-opengl=$q" "qemu-utils=$q" "qemu-block-extra=$q" \
        > "$HOME/apt-image.log" 2>&1 ||
        { tail -n 30 "$HOME/apt-image.log" >&2; die "image: install failed"; }
    fetched "$HOME/apt-image.log"
    sudo tar -xJf "$1" -C /
    sudo tar -xJf "$2" -C /
    sudo install -m 755 "$here/qemu-nctx" "$here/nctx-env" "$top/"
    cat "$log"
}

check()
{
    echo "kernel: $(uname -r)"
    echo "qemu: $(qemu-system-x86_64 --version | head -n 1)"
    echo "qemu: native context: $(qemu-system-x86_64 -device virtio-gpu-gl-pci,help 2>&1 | grep -o 'drm_native_context=[^ ]*' || echo absent)"
    lib=$(LD_LIBRARY_PATH=$top/virglrenderer/lib ldd \
        /usr/lib/x86_64-linux-gnu/qemu/hw-display-virtio-gpu-gl.so | awk '/virglrenderer/ { print $3 }')
    echo "qemu: virglrenderer: $(readlink -f "$lib")"
    echo "virglrenderer: i915 renderer: $(grep -c -a 'pci_revision_id:' "$(readlink -f "$lib")" > /dev/null && echo present || echo absent)"
    g=$(ls "$top"/mesa/lib/libgallium-*.so)
    echo "mesa: $(basename "$g")"
    echo "mesa: crocus: $(grep -c -a 'crocus' "$g" > /dev/null && echo present || echo absent)"
    echo "mesa: crocus's virtio path: $(grep -c -a INTEL_VIRTIO_FORCE_VTEST "$g" > /dev/null && echo present || echo absent)"
}

case ${1:-} in
    mesa) mesa "$2" "$3" ;;
    virgl) virgl "$2" "$3" ;;
    image) image "$2" "$3" ;;
    check) check ;;
    *) die "usage: guest.sh mesa|virgl TARBALL OUT | image MESA VIRGL | check" ;;
esac
