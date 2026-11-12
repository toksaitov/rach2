#!/bin/sh
# reverse.sh: run the matched Mesa on Rach2 through the backend VM.
# Usage: reverse.sh fallback|native ASSETS OUTPUT
# ASSETS holds build.sh reverse's products and the reference VM's key.
# Native mode takes the Lenovo's GPU through tests/assign/gpu.sh. It needs
# that task's trial go and the Lenovo notice before it is run.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
tree=$(cd "$here/../.." && pwd)
mode=${1:-} assets=${2:-} out=${3:-}
daemon='' vm='' profile=0 made=0

die()
{
    echo "nctx-reverse: result: fail: $*" >&2
    exit 1
}

guest()
{
    ssh -n -i "$assets/id_ed25519" -o BatchMode=yes -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5 \
        -p "$port" gfx@127.0.0.1 "$@"
}

# Stop the guest first; gpu.sh then returns its GPU to Rach2's driver.
cleanup()
{
    if [ -n "$vm" ]; then
        # A guest that takes SSH but no longer runs commands would hold the
        # request open, so it gets 20 seconds before the VM is stopped.
        guest 'sudo poweroff' > /dev/null 2>&1 &
        stop=$!
        n=0
        while kill -0 "$stop" 2>/dev/null && [ "$n" -lt 20 ]; do
            sleep 1
            n=$((n + 1))
        done
        kill -TERM "$stop" 2>/dev/null || true
        wait "$stop" 2>/dev/null || true
        kill -TERM "$vm" 2>/dev/null || true
        wait "$vm" 2>/dev/null || true
    fi
    if [ -n "$daemon" ]; then
        kill -TERM "$daemon" 2>/dev/null || true
        wait "$daemon" 2>/dev/null || true
    fi
    if [ "$made" = 1 ]; then
        rm -f "$out/ivs.sock"
        rm -rf "$out/tmp"
    fi
}
trap cleanup EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM

case $mode in fallback|native) ;; *) die "usage: reverse.sh fallback|native ASSETS OUTPUT" ;; esac
[ -d "$assets" ] && [ -n "$out" ] || die "name the assets and a new output directory"
assets=$(cd "$assets" && pwd)
PATH=$assets:$PATH
export PATH
command -v glmark2-drm > /dev/null || die "the GNU root has no glmark2-drm"
mkdir "$out" || die "the output directory must be new"
made=1
out=$(cd "$out" && pwd)
mkdir "$out/tmp"
TMPDIR=$out/tmp
export TMPDIR
echo "nctx-reverse: evidence: $out"
(cd "$assets" && sha256sum -c reverse.sha256) || die "the assets differ from their digests"
[ -e /dev/vgpu-shm ] || die "the kernel has no shared-memory GPU transport"
port=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
ivshmemd -s 64M "$out/ivs.sock" > "$out/ivshmemd.log" 2>&1 &
daemon=$!
tries=0
until [ -S "$out/ivs.sock" ]; do
    kill -0 "$daemon" 2>/dev/null || die "ivshmemd ended"
    tries=$((tries + 1))
    [ "$tries" -lt 30 ] || die "ivshmemd made no socket"
    sleep 1
done

set -- -drive "file=$assets/reverse.qcow2,if=none,id=vm,format=qcow2,snapshot=on" \
    -device virtio-blk-pci,drive=vm,ioeventfd=off \
    -chardev "socket,path=$out/ivs.sock,id=ivs" -device ivshmem-doorbell,chardev=ivs,vectors=2 \
    -netdev "user,id=net,hostfwd=tcp:127.0.0.1:$port-:22" \
    -device virtio-net-pci,netdev=net,romfile= -device virtio-rng-pci
args="root=/dev/vda1 rw nctx.reverse=$mode"
if [ "$mode" = native ]; then
    bash "$tree/tests/assign/gpu.sh" -m 4096 -s 2 -o "$out/gpu" \
        -k "$assets/reverse-vmlinuz" -i "$assets/reverse-initrd.img" -l "$args" -- "$@" \
        > "$out/gpu.log" 2>&1 &
    vm=$!
    console=$out/gpu/console
else
    qemu-system-x86_64 -accel tcg -cpu max -machine q35 \
        -smp 2 -m 4096 -nodefaults -display none -no-reboot -monitor none \
        -kernel "$assets/reverse-vmlinuz" -initrd "$assets/reverse-initrd.img" \
        -append "console=ttyS0 panic=-1 $args" -serial "file:$out/console" "$@" \
        > "$out/qemu.log" 2>&1 &
    vm=$!
    console=$out/console
fi

# Report boot progress, and stop on an ended process or a silent console.
last=0 quiet=0 tries=0
until guest true 2>/dev/null; do
    kill -0 "$vm" 2>/dev/null || die "the VM ended before SSH answered"
    size=$({ wc -c < "$console"; } 2>/dev/null || echo 0)
    if [ "$size" = "$last" ]; then quiet=$((quiet + 1)); else quiet=0; last=$size; fi
    [ "$quiet" -lt 120 ] || die "the VM's console stopped before SSH answered"
    tries=$((tries + 1))
    if [ $((tries % 10)) -eq 0 ]; then
        echo "nctx-reverse: boot: $tries tries, $size console bytes"
    fi
    sleep 1
done
guest 'uname -a; sudo systemctl status nctx-reverse --no-pager' > "$out/backend.txt" ||
    die "the backend service failed"
tries=0
until ivshmemd -i | grep -q 'bound 1,'; do
    tries=$((tries + 1))
    [ "$tries" -lt 60 ] || die "virtio-gpu did not bind over the region"
    sleep 1
done
node='' card=''
for dev in /sys/class/drm/renderD*; do
    case $(readlink -f "$dev/device") in
        */platform/virtio_gpu) node=/dev/dri/${dev##*/} ;;
    esac
done
for dev in /sys/class/drm/card[0-9]*; do
    case $(readlink -f "$dev/device") in
        */platform/virtio_gpu) card=/dev/dri/${dev##*/} ;;
    esac
done
[ -n "$node" ] && [ -n "$card" ] || die "the transport has no DRM nodes"
"$assets/nctxcheck" capset "$node" > "$out/capset.txt"
cat "$out/capset.txt"
if [ "$mode" = native ]; then want=present; else want=absent; fi
grep -q "DRM native context $want" "$out/capset.txt" || die "unexpected capset mask"

tar -xJf "$assets/mesa-nctx.tar.xz" -C /
unset LIBGL_ALWAYS_SOFTWARE GALLIUM_DRIVER MESA_LOADER_DRIVER_OVERRIDE
# shellcheck source=nctx-env
. "$here/nctx-env"
export NCTX_RENDER_NODE="$node"
"$assets/nctxcheck" info > "$out/info.txt"
cat "$out/info.txt"
if [ "$mode" = native ]; then want='Mesa Intel'; else want=virgl; fi
grep -q "gl renderer: $want" "$out/info.txt" || die "Mesa selected another renderer"
"$assets/nctxcheck" triangle > "$out/triangle.txt"
cat "$out/triangle.txt"
"$assets/nctxcheck" fence > "$out/fence.txt"
cat "$out/fence.txt"
if [ "$mode" = native ]; then
    guest 'sudo /opt/rach2-nctx/relocations start' > "$out/profile-start.txt"
    profile=1
fi
if ! glmark2-drm --off-screen --winsys-options "drm-device=$card" -s 1280x800 \
    -b build:use-vbo=true --validate > "$out/validate.txt" 2>&1; then
    cat "$out/validate.txt"
    die "glmark2 validation failed"
fi
cat "$out/validate.txt"
grep -q 'Validation: Success' "$out/validate.txt" || die "glmark2 did not validate its pixels"
if ! glmark2-drm --off-screen --winsys-options "drm-device=$card" -s 1280x800 \
    -b build:use-vbo=true > "$out/glmark2.txt" 2>&1; then
    cat "$out/glmark2.txt"
    die "glmark2 scene failed"
fi
cat "$out/glmark2.txt"
grep -q "GL_RENDERER: *$want" "$out/glmark2.txt" || die "glmark2 selected another renderer"
if [ "$profile" = 1 ]; then
    guest 'sudo /opt/rach2-nctx/relocations stop' > "$out/relocations.txt"
    cat "$out/relocations.txt"
    profile=0
fi
ivshmemd -i > "$out/counts.txt"
guest 'sudo poweroff'
wait "$vm" || die "the VM or the GPU handoff failed"
vm=
wait "$daemon" || die "ivshmemd failed to detach"
daemon=
cat "$out/counts.txt"
echo "nctx-reverse: result: pass: $mode"
