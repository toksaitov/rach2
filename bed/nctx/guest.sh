#!/bin/sh
# guest.sh: the native-context stack's steps inside a Debian 13 guest, which
# build.sh copies in with the inputs and runs as `gfx` in ~/nctx.
#
#   guest.sh mesa TARBALL OUT        build Mesa into /opt/rach2-nctx/mesa
#   guest.sh virgl TARBALL OUT       build virglrenderer into .../virglrenderer
#   guest.sh qemu VIRGL OUT          build QEMU into .../qemu against VIRGL,
#                                    the virglrenderer build's tar
#   guest.sh image MESA VIRGL QEMU   install the backports kernel and QEMU
#                                    packages and the three builds: the VM
#                                    image's variant
#   guest.sh check                   print what the variant holds
#   guest.sh capsets [NODE]          the capsets a guest of QEMU reads
#
# Every step reads its versions from ./pins and the snapshot's date from
# ./snapshot, beside this script. A build writes OUT, a tar of its prefix,
# and OUT.log: the options its configuration chose, the packages taken from
# trixie-backports and the size of apt's downloads.
set -eu

here=$(cd "$(dirname "$0")" && pwd)
top=/opt/rach2-nctx

die()
{
    echo "nctx: $*" >&2
    exit 1
}

pin()
{
    awk -v n="$1" '$1 == n { print $2 }' "$here/pins"
}

# apt_get NAME ARGS...: run apt-get ARGS as root with no questions, its
# output in ~/apt-NAME.log, and record its download total in the step's log.
apt_get()
{
    a=$HOME/apt-$1.log
    shift
    # shellcheck disable=SC2024 # the log is the user's, not root's
    sudo DEBIAN_FRONTEND=noninteractive apt-get -q -y "$@" > "$a" 2>&1 ||
        { tail -n 30 "$a" >&2; die "apt-get $*: failed"; }
    grep -E '^(Need to get|Fetched) ' "$a" | sed 's/^/apt: /' >> "$log" || true
}

# from_backports: record the installed packages that came from backports.
from_backports()
{
    dpkg-query -W -f '${Package} ${Version}\n' | grep '~bpo' |
        sed 's/^/backports: /' >> "$log" || true
}

# backports: add trixie-backports, binaries and sources, from the snapshot of
# the guest's own date. apt keeps its default priority of 100 for it, so
# only what a step names comes from there.
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

# install_tar NAME OUT: tar the staging root's $top/NAME, tar it to OUT and
# record its size.
install_tar()
{
    tar -C "$work/root" -cJf "$2" ."$top/$1"
    echo "size: $(wc -c < "$2") bytes, $(du -sk "$work/root$top/$1" | cut -f1) KiB installed" >> "$log"
}

# run_logged NAME CMD...: run CMD with its output in $work/NAME.log, showing
# the log's end if it fails.
run_logged()
{
    l=$work/$1.log
    shift
    "$@" > "$l" 2>&1 || { tail -n 40 "$l" >&2; die "$*: failed"; }
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
    run_logged setup meson setup "$work/b" "$work/src" --prefix="$top/$name" \
        --libdir=lib --buildtype=release -Db_ndebug=true "$@"
    grep -E 'intel-virtio|drm-renderers|drm-i915|venus|Platforms|LLVM' \
        "$work/setup.log" | sed 's/^ */meson: /' >> "$log" || true
    run_logged build ninja -C "$work/b"
    run_logged install env DESTDIR="$work/root" ninja -C "$work/b" install
    install_tar "$name" "$out"
    rm -rf "$work"
}

mesa()
{
    log=$2.log
    : > "$log"
    backports
    # Its build dependencies come from trixie-backports where it has them,
    # as for Debian's own build of this version (directx-headers-dev).
    apt_get mesa -t trixie-backports build-dep "$(pin mesa_source)"
    from_backports
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
    apt_get virgl build-dep "$(pin virgl_source)"
    apt_get virgl2 install meson ninja-build libvulkan-dev libdrm-dev
    # i915's native context beside virgl and Venus, which QEMU's capsets
    # offer as before; Venus's render server runs each context in a process.
    meson_build virglrenderer "$1" "$2" \
        -Dvenus=true -Ddrm-renderers=i915-experimental -Dvideo=false \
        -Dtests=false
}

qemu()
{
    log=$2.log
    : > "$log"
    backports
    apt_get qemu -t trixie-backports build-dep "qemu=$(pin qemu)"
    from_backports
    sudo tar -xJf "$1" -C /
    work=$HOME/nctx-qemu
    rm -rf "$work"
    mkdir -p "$work/root"
    (cd "$work" && apt-get -q source "qemu=$(pin qemu)") > "$HOME/apt-qemu-source.log" 2>&1 ||
        { tail -n 30 "$HOME/apt-qemu-source.log" >&2; die "qemu: apt-get source failed"; }
    grep -E '^(Need to get|Fetched) ' "$HOME/apt-qemu-source.log" | sed 's/^/apt: source: /' >> "$log" || true
    src=$(find "$work" -mindepth 1 -maxdepth 1 -type d -name 'qemu-*')
    # Debian's source with its patches, as backports builds it, for x86-64
    # alone and with only what the check needs, so that it links nothing a
    # plain Debian 13 host lacks: KVM, the GL devices through epoxy and GBM,
    # virglrenderer 1.3.0 found by its run path, user networking and pixman.
    # The firmware is Debian's, from the packages the variant installs.
    mkdir -p "$work/b"
    (cd "$work/b" && export PKG_CONFIG_PATH="$top/virglrenderer/lib/pkgconfig" &&
        run_logged configure "$src/configure" --prefix="$top/qemu" \
        --target-list=x86_64-softmmu --without-default-features \
        --enable-kvm --enable-tcg --enable-opengl --enable-virglrenderer \
        --enable-slirp --enable-pixman --disable-tools --disable-docs \
        --disable-install-blobs \
        --firmwarepath=/usr/share/qemu:/usr/share/seabios:/usr/lib/ipxe/qemu \
        --extra-ldflags="-Wl,-rpath,$top/virglrenderer/lib")
    grep -E 'virglrenderer|OpenGL|GBM|slirp|KVM' "$work/configure.log" |
        sed 's/^ */configure: /' >> "$log" || true
    run_logged build make -C "$work/b" -j "$(nproc)"
    run_logged install make -C "$work/b" install DESTDIR="$work/root"
    install_tar qemu "$2"
    rm -rf "$work"
}

image()
{
    log=$HOME/image.log
    : > "$log"
    backports
    q=$(pin qemu)
    # The kernel needs backports' linux-base and QEMU its SeaBIOS; every
    # other package stays the reference guest's. Debian's QEMU stays beside
    # the bed's build, for its firmware and as the comparison.
    apt_get image install "$(pin kernel)" "qemu-system-x86=$q" \
        linux-base/trixie-backports seabios/trixie-backports \
        "qemu-system-common=$q" "qemu-system-data=$q" "qemu-system-gui=$q" \
        "qemu-system-modules-opengl=$q" "qemu-utils=$q" "qemu-block-extra=$q"
    from_backports
    for t in "$@"; do
        sudo tar -xJf "$t" -C /
    done
    sudo install -m 755 "$here/qemu-nctx" "$top/"
    sudo install -m 644 "$here/nctx-env" "$top/"
    cat "$log"
}

# present FILE STRING: "present" when FILE holds STRING, else "absent".
present()
{
    if grep -q -a -F "$2" "$1"; then echo present; else echo absent; fi
}

check()
{
    echo "kernel: $(uname -r)"
    echo "debian qemu: $(qemu-system-x86_64 --version | head -n 1)"
    echo "qemu: $("$top/qemu-nctx" --version | head -n 1)"
    echo "qemu: native context: $("$top/qemu-nctx" -device virtio-gpu-gl-pci,help 2>&1 | grep -o 'drm_native_context=[^ ]*' || echo absent)"
    lib=$(ldd "$top/qemu/bin/qemu-system-x86_64" | awk '/virglrenderer/ { print $3 }')
    echo "qemu: virglrenderer: $(readlink -f "$lib")"
    echo "virglrenderer: i915 renderer: $(present "$lib" 'pci_revision_id:')"
    g=$(ls "$top"/mesa/lib/libgallium-*.so)
    echo "mesa: $(basename "$g")"
    echo "mesa: crocus: $(present "$g" crocus)"
    echo "mesa: crocus's virtio path: $(present "$g" INTEL_VIRTIO_FORCE_VTEST)"
}

# capsets [NODE]: boot a small guest of the running kernel under QEMU, by
# default qemu-nctx, with drm_native_context=on and its GL drawn on render
# NODE (vgem's, loaded here, by default), and print the capsets its
# virtio-gpu driver reads, with what QEMU and virglrenderer report. The guest
# is the kernel, the virtio-gpu module and its dependencies, and BusyBox, in
# an initramfs. QEMU may be a command line, such as Debian's QEMU with
# LD_LIBRARY_PATH set by env.
capsets()
{
    k=$(uname -r)
    w=$HOME/capsets
    rm -rf "$w"
    mkdir -p "$w/root/bin" "$w/root/m" "$w/root/proc" "$w/root/sys"
    log=/dev/null
    [ -x /bin/busybox ] || apt_get busybox install busybox-static
    cp /bin/busybox "$w/root/bin/"
    # The module and what it needs, deepest first, decompressed for insmod.
    need()
    {
        for d in $(/sbin/modinfo -k "$k" -F depends "$1" | tr ',' ' '); do
            need "$d"
        done
        grep -qx "$1" "$w/order" 2> /dev/null || echo "$1" >> "$w/order"
    }
    need virtio-gpu
    {
        echo '#!/bin/busybox sh'
        echo '/bin/busybox --install -s /bin'
        echo 'mount -t proc proc /proc; mount -t sysfs sys /sys'
        echo 'mount -t debugfs debugfs /sys/kernel/debug'
        while read -r m; do
            f=$(/sbin/modinfo -k "$k" -F filename "$m")
            case $f in
                *.xz) xz -dc "$f" > "$w/root/m/$m.ko" ;;
                *.ko) cp "$f" "$w/root/m/$m.ko" ;;
                *) continue ;;
            esac
            echo "insmod /m/$m.ko"
        done < "$w/order"
        echo 'sleep 3'
        echo 'dmesg | grep -i -E "virtio.gpu|\[drm\]|cap set" | sed "s/^/guest: /"'
        echo 'grep -E "cap sets|context init" /sys/kernel/debug/dri/0/virtio-gpu-features | sed "s/^/guest: /"'
        echo 'poweroff -f'
    } > "$w/root/init"
    chmod 755 "$w/root/init"
    (cd "$w/root" && find . | cpio -o -H newc 2> /dev/null | gzip > "$w/initrd")
    sudo cp "/boot/vmlinuz-$k" "$w/vmlinuz"
    sudo chmod a+r "$w/vmlinuz"
    node=${1:-}
    if [ -z "$node" ]; then
        sudo modprobe vgem
        node=$(for n in /dev/dri/renderD*; do
            case $(readlink -f "/sys/class/drm/$(basename "$n")") in
                */vgem/*) echo "$n" ;;
                *) ;;
            esac
        done | head -n 1)
    fi
    [ -n "$node" ] || die "capsets: no render node"
    echo "host: qemu ${QEMU:-$top/qemu-nctx}, render node $node"
    # Native context needs blob resources and host memory, and blobs a
    # shared memory backend.
    # shellcheck disable=SC2086 # QEMU may be a command line
    timeout 300 ${QEMU:-$top/qemu-nctx} -accel tcg -m 512 \
        -object memory-backend-memfd,id=mem,size=512M,share=on \
        -machine q35,memory-backend=mem -nodefaults \
        -no-reboot -kernel "$w/vmlinuz" -initrd "$w/initrd" \
        -append 'console=ttyS0 loglevel=4 panic=-1' -serial stdio \
        -display "egl-headless,rendernode=$node" \
        -device virtio-gpu-gl-pci,blob=true,hostmem=256M,drm_native_context=on 2>&1 |
        tr -d '\r' || true
    rm -rf "$w"
}

case ${1:-} in
    mesa) mesa "$2" "$3" ;;
    virgl) virgl "$2" "$3" ;;
    qemu) qemu "$2" "$3" ;;
    image) shift; image "$@" ;;
    check) check ;;
    capsets) capsets "${2:-}" ;;
    *) die "usage: guest.sh mesa|virgl TARBALL OUT | qemu VIRGL OUT | image MESA VIRGL QEMU | check | capsets [NODE]" ;;
esac
