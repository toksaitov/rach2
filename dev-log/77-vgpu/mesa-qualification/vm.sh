#!/bin/sh
#
# vm.sh: the Debian 13 guest of task 77-7a in WSL on the testing PC, under
# KVM with Venus over the host's lavapipe and QEMU's GL on llvmpipe (vgem).
#
# Usage:
#   vm.sh seed               make the SSH key and the NoCloud seed
#   vm.sh disk NAME BACKING  make NAME.qcow2, an overlay of BACKING (40 GiB)
#   vm.sh start NAME         boot NAME.qcow2; returns once SSH answers
#   vm.sh ssh CMD...         run CMD in the guest
#   vm.sh put SRC... DIR     copy files into the guest's DIR
#   vm.sh get SRC DIR        copy a guest path into DIR
#   vm.sh stop               power the guest off and wait for QEMU to exit
#
# Images live in C (build/cache/77-7a), runs in O (build/tmp/77-7a). QEMU's
# stderr, with virglrenderer's, goes to O/qemu-NAME.log; the guest's console
# to O/console-NAME.log.
#
set -eu
C=${C:-/home/toksaitov-m/rach2/build/cache/77-7a}
O=${O:-/home/toksaitov-m/rach2/build/tmp/77-7a}
PORT=${PORT:-22599}
SMP=${SMP:-16}
MEM=${MEM:-16G}
KEY=$O/key
SSHOPT="-i $KEY -p $PORT -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR -o ConnectTimeout=5"

seed() {
    [ -f "$KEY" ] || ssh-keygen -q -t ed25519 -N '' -C 77-7a -f "$KEY"
    d=$O/seed
    rm -rf "$d"
    mkdir -p "$d"
    printf 'instance-id: rach2-77-7a\nlocal-hostname: gfx\n' >"$d/meta-data"
    : >"$d/network-config"
    cat >"$d/user-data" <<EOF
#cloud-config
users:
  - name: gfx
    groups: [sudo, video, render, input]
    shell: /bin/bash
    sudo: ALL=(ALL) NOPASSWD:ALL
    ssh_authorized_keys: [$(cat "$KEY.pub")]
growpart: {mode: auto, devices: ['/']}
package_update: false
EOF
    xorriso -as mkisofs -quiet -V cidata -J -r -o "$C/seed.iso" "$d"
}

disk() {
    qemu-img create -q -f qcow2 -F qcow2 -b "$2" "$C/$1.qcow2" 40G
}

start() {
    n=$1
    [ ! -f "$O/qemu.pid" ] || ! kill -0 "$(cat "$O/qemu.pid")" 2>/dev/null || {
        echo "vm: a guest is running" >&2
        exit 1
    }
    mkdir -p "$O/hostcache"
    rm -rf "$O/hostcache/"*
    VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json \
        MESA_SHADER_CACHE_DIR=$O/hostcache VIRGL_LOG_LEVEL=info \
        setsid -f qemu-system-x86_64 -name "77-7a-$n" \
        -accel kvm -cpu host -smp "$SMP" -m "$MEM" \
        -object memory-backend-memfd,id=mem,size=$MEM,share=on \
        -machine q35,memory-backend=mem \
        -vga none -device virtio-vga-gl,venus=on,blob=on,hostmem=4G \
        -display egl-headless,rendernode=/dev/dri/renderD128 \
        -drive file="$C/$n.qcow2",if=virtio,format=qcow2,cache=unsafe \
        -drive file="$C/seed.iso",if=virtio,format=raw,readonly=on \
        -netdev user,id=n0,hostfwd=tcp:127.0.0.1:$PORT-:22 \
        -device virtio-net-pci,netdev=n0 \
        -serial file:"$O/console-$n.log" -monitor none \
        -qmp unix:"$O/qmp.sock",server=on,wait=off \
        -pidfile "$O/qemu.pid" </dev/null >"$O/qemu-$n.log" 2>&1
    i=0
    while ! ssh $SSHOPT gfx@127.0.0.1 true 2>/dev/null; do
        i=$((i + 1))
        [ $i -lt 120 ] || {
            echo "vm: no ssh after 240 s" >&2
            exit 1
        }
        sleep 2
    done
    echo "vm: $n up after about $((i * 2)) s"
}

stop() {
    [ -f "$O/qemu.pid" ] || return 0
    p=$(cat "$O/qemu.pid")
    ssh $SSHOPT gfx@127.0.0.1 'sudo systemctl poweroff' 2>/dev/null || true
    i=0
    while kill -0 "$p" 2>/dev/null; do
        i=$((i + 1))
        [ $i -lt 60 ] || {
            echo "vm: QEMU still up after 120 s; quitting it" >&2
            timeout 5 python3 -I -c 'import socket,sys
s = socket.socket(socket.AF_UNIX); s.connect(sys.argv[1]); s.recv(4096)
s.sendall(b"{\"execute\":\"qmp_capabilities\"}{\"execute\":\"quit\"}\n")
s.recv(4096)' "$O/qmp.sock" || kill "$p"
            sleep 2
            break
        }
        sleep 2
    done
    rm -f "$O/qemu.pid"
    echo "vm: stopped"
}

cmd=${1-}
[ $# -gt 0 ] && shift
case $cmd in
seed) seed ;;
disk) disk "$@" ;;
start) start "$@" ;;
stop) stop ;;
ssh) exec ssh $SSHOPT gfx@127.0.0.1 "$@" ;;
put)
    eval "dst=\${$#}"
    n=$(($# - 1))
    set -- $(printf '%s\n' "$@" | head -n $n)
    scp -q -r ${SSHOPT%%-p *} -P "$PORT" -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR "$@" "gfx@127.0.0.1:$dst"
    ;;
get)
    scp -q -r -i "$KEY" -P "$PORT" -o StrictHostKeyChecking=no \
        -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR "gfx@127.0.0.1:$1" "$2"
    ;;
*)
    sed -n '3,15p' "$0" >&2
    exit 2
    ;;
esac
