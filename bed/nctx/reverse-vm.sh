#!/bin/sh
# reverse-vm.sh: build and serve native context in the driver's Debian VM.
# prepare runs once in build.sh's overlay; serve runs only on a boot with
# nctx.reverse=native or nctx.reverse=fallback on the kernel command line.
set -eu
top=/opt/rach2-nctx

die()
{
    echo "nctx-vm: $*" >&2
    exit 1
}

prepare()
{
    src=$(cd "$1" && pwd)
    DEBIAN_FRONTEND=noninteractive apt-get -q -y install \
        "linux-headers-$(uname -r)" build-essential pkg-config libdrm-dev libvulkan-dev \
        libepoxy-dev libgbm-dev libx11-dev
    make -C "$src/bed/vm/module"
    PKG_CONFIG_PATH=$top/virglrenderer/lib/pkgconfig \
        make -C "$src/bed/vm/backend" LDFLAGS="-Wl,-rpath,$top/virglrenderer/lib"
    install -m 644 "$src/bed/vm/module/vgpushm.ko" "$top/vgpushm.ko"
    install -m 755 "$src/bed/vm/backend/vgpu-backend" "$top/vgpu-backend"
    install -m 755 "$0" "$top/reverse-vm"
    install -m 755 "$(dirname "$0")/relocations.sh" "$top/relocations"
    systemctl mask serial-getty@ttyS0.service
    mkdir -p /etc/systemd/system.conf.d
    cat > /etc/systemd/system.conf.d/nctx.conf <<'CONF'
[Manager]
DefaultTimeoutStartSec=infinity
DefaultDeviceTimeoutSec=infinity
CONF
    cat > /etc/systemd/system/nctx-reverse.service <<'UNIT'
[Unit]
Description=Serve the reversed native-context transport
ConditionKernelCommandLine=nctx.reverse
After=systemd-udev-settle.service
Wants=systemd-udev-settle.service
[Service]
Type=simple
ExecStart=/opt/rach2-nctx/reverse-vm serve
StandardOutput=tty
StandardError=tty
TTYPath=/dev/ttyS0
[Install]
WantedBy=multi-user.target
UNIT
    systemctl enable nctx-reverse.service
    echo "nctx-vm: result: built for $(uname -r)"
}

serve()
{
    mode=
    read -r cmdline < /proc/cmdline
    for arg in $cmdline; do
        case $arg in nctx.reverse=*) mode=${arg#*=} ;; esac
    done
    insmod "$top/vgpushm.ko"
    export LD_LIBRARY_PATH=$top/virglrenderer/lib
    case $mode in
        native)
            node=
            for dev in /sys/class/drm/renderD*; do
                [ -e "$dev/device/driver" ] || continue
                case $(readlink -f "$dev/device/driver") in
                    */i915) node=/dev/dri/${dev##*/}; break ;;
                esac
            done
            [ -n "$node" ] || die "no i915 render node for native context"
            echo "nctx-vm: native context on $node"
            exec "$top/vgpu-backend" -v -x -N -g "$node"
            ;;
        fallback)
            echo "nctx-vm: native context disabled; virgl on llvmpipe"
            export LIBGL_ALWAYS_SOFTWARE=1
            exec "$top/vgpu-backend" -v -x
            ;;
        *) die "no nctx.reverse=native or nctx.reverse=fallback" ;;
    esac
}

case ${1:-} in
    prepare) prepare "$2" ;;
    serve) serve ;;
    *) die "usage: reverse-vm.sh prepare SOURCE | serve" ;;
esac
