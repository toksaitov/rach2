# shellcheck shell=sh disable=SC2154 # install_root.sh sets the variables
#
# install_kvm.sh: install the KVM layer (Readme.md) over the x86-64 GNU root.
#
# install_root.sh sources this once the desktop closure and the games layer
# are in TREE, so that it uses that script's TREE, OUT, WORK, install_debian,
# install_part and needs, and a failure here stops the root; on i686 it does
# nothing. It installs the QEMU closure that `make ARCH=x86_64 gnu-kvm`
# staged in OUT/debian/kvm, less the packages SKIP and the desktop closure
# already account for, refusing a file the root already has, and then the
# tests of OUT/kvm under /opt. Since nothing resolved the closure against the
# root, it then refuses the root when a library that a program, library or
# module of the closure needs is in none of the root's library directories:
# QEMU's modules, which QEMU opens itself, among them.
#

if [ "$RACH2_ARCH" = x86_64 ]; then
    install_debian kvm

    # Each ELF file the layer installed; a link is left to its target. A
    # library may name glibc's loader, which only /lib64 has.
    while read -r path; do
        f="$TREE/${path#./}"
        if [ -L "$f" ] || [ ! -f "$f" ] ||
            [ "$(od -An -tx1 -N4 "$f" | tr -d ' \n')" != 7f454c46 ]; then
            continue
        fi
        needs "the KVM layer's /${path#./}" "$f" lib usr/lib "usr/lib/$GNU_MULTIARCH" lib64
    done < "$WORK/new"
    if [ ! -x "$TREE/usr/bin/qemu-system-x86_64" ] ||
        [ ! -f "$TREE/usr/lib/$GNU_MULTIARCH/qemu/hw-display-virtio-gpu-gl.so" ]; then
        echo "gnu: install: refused: the KVM layer has no qemu-system-x86_64 or no GL module" >&2
        exit 1
    fi

    install_part kvm
fi
