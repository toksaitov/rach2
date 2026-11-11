#!/bin/sh
#
# forward.sh: the forward direction on Rach2. Boot the Debian guest in QEMU
# on Rach2 with QEMU's own virtio-gpu on Rach2's render node, run a scene of
# scene.sh inside it and report what it drew.
#
# Usage: forward.sh [-a ACCEL] [-c CPUS] [-m MIB] [-p PORT] [-w SECONDS]
#                   [-o OUT] [-f FORMAT] [-r REF] [-M SCENE [-t SECONDS]]
#                   -d DISK -k KEY -g FILES virgl|venus
#
# DISK is the guest's disk, the Debian reference guest's image as a device or
# file in QEMU's FORMAT, raw by default, or qcow2 for the image file itself;
# KEY the private key of its user gfx; FILES a tar archive of
# what scene.sh needs beside it, unpacked in the guest's /tmp/forward. The
# guest has CPUS processors, 4 by default, and MIB of memory, 4096; QEMU
# forwards PORT of the loopback address, 2222 by default, to the guest's SSH
# server over its user network. OUT, the working directory by default, gets
# the guest's console as console.log, QEMU's own messages as qemu.log, the
# scene's output as scene.log, and for a scene that ran, the guest's /tmp/out
# as out.tar. REF is for venus: a frames file of vkcube.sh that the scene
# judges vkcube's frames by in place of the archive's vkcube.ref, copied into
# the guest, or `none`, which records the frames without judging them.
# SCENE, with SECONDS, has scene.sh run the test bed's measurement harness
# on the profile's device in place of its check (scene.sh -M).
#
# DISK is opened with QEMU's snapshot=on: the guest's writes go to a
# temporary overlay, a file without a name on OUT's file system, which is
# QEMU's TMPDIR, and DISK stays as it was. Rach2's ordinary kernel takes no
# write to a disk's node (disk(4)), the GNU root has no /var/tmp, where QEMU
# would put the overlay, and the next run finds the guest as this one did.
#
# The machine is q35 under ACCEL: `kvm`, the default, which is
# `-accel kvm,kernel-irqchip=split`, the interrupt controller Rach2's KVM
# has, with the host's processor, or `tcg`, QEMU's emulator, with its `max`,
# for a Rach2 that is itself a guest of KVM: its guest would be a guest of
# the third level, which is too slow to boot Debian (Readme.md).
# It has a virtio disk, network card and random number generator, and
# virtio-vga-gl, virtio-gpu with a VGA for the guest's GRUB, on egl-headless,
# which draws through Rach2's render node RENDER_NODE, /dev/dri/renderD128 by
# default:
#
#   virgl  the guest's GL through virglrenderer on Rach2's GL.
#   venus  the device with venus=on,blob=on and a host-visible window of
#          HOSTMEM, 4G by default, the guest's memory a shared memfd, which a
#          blob's mapping needs; virglrenderer's render server,
#          /usr/libexec/virgl_render_server, makes the guest's Vulkan calls
#          on Rach2's Vulkan driver, the one VK_DRIVER_FILES names, by
#          default Venus's.
#
# The runner waits for the guest's SSH server, copies FILES in, runs
# `scene.sh PROFILE` as gfx, copies its output out and powers the guest off.
# It waits as long as the guest's console or the scene's output grows; QEMU's
# end, or SECONDS without a byte from either, 600 by default, fails it. That
# bound is on silence, never on the run's length. It prints the scene's
# lines, each `bed-forward: NAME: VALUE`, and last the scene's result,
#
#   bed-forward: result: pass: PROFILE: DETAIL
#
# or `bed-forward: result: fail: REASON` of its own. It exits 0 on a pass, 1
# on a failure and 2 on a usage error.

accel=kvm cpus=4 mib=4096 port=2222 silence=600 out=. format=raw ref='' disk='' key='' files=''
measure='' seconds=''
usage()
{
    echo "usage: forward.sh [-a ACCEL] [-c CPUS] [-m MIB] [-p PORT] [-w SECONDS]" >&2
    echo "                  [-o OUT] [-f FORMAT] [-r REF] [-M SCENE [-t SECONDS]]" >&2
    echo "                  -d DISK -k KEY -g FILES virgl|venus" >&2
    exit 2
}
while getopts a:c:m:p:w:o:f:r:M:t:d:k:g: o; do
    case $o in
        M) measure=$OPTARG ;;
        t) seconds=$OPTARG ;;
        a) accel=$OPTARG ;;
        c) cpus=$OPTARG ;;
        m) mib=$OPTARG ;;
        p) port=$OPTARG ;;
        w) silence=$OPTARG ;;
        o) out=$OPTARG ;;
        f) format=$OPTARG ;;
        r) ref=$OPTARG ;;
        d) disk=$OPTARG ;;
        k) key=$OPTARG ;;
        g) files=$OPTARG ;;
        *) usage ;;
    esac
done
shift $((OPTIND - 1))
[ $# -eq 1 ] && [ -n "$disk" ] && [ -n "$key" ] && [ -n "$files" ] || usage
profile=$1
case $silence in
    '' | *[!0-9]*) usage ;;
esac
case $format in
    raw | qcow2) ;;
    *) usage ;;
esac
case $ref in
    '' | none) ;;
    *) [ -f "$ref" ] || { echo "forward.sh: no reference frames $ref" >&2; exit 2; } ;;
esac
case $accel in
    kvm) accel_option=kvm,kernel-irqchip=split cpu=host ;;
    tcg) accel_option=tcg cpu=max ;;
    *) usage ;;
esac
qemu=${QEMU:-qemu-system-x86_64}
node=${RENDER_NODE:-/dev/dri/renderD128}
gpu=virtio-vga-gl,edid=on,xres=1280,yres=800
case $profile in
    virgl)
        set --
        ;;
    venus)
        gpu=$gpu,venus=on,blob=on,hostmem=${HOSTMEM:-4G}
        set -- -object "memory-backend-memfd,id=ram,size=${mib}M,share=on" -machine memory-backend=ram
        VK_DRIVER_FILES=${VK_DRIVER_FILES:-/usr/share/vulkan/icd.d/venus_icd.x86_64.json}
        export VK_DRIVER_FILES
        ;;
    *) usage ;;
esac

pid=
fail()
{
    echo "bed-forward: result: fail: $*"
    exit 1
}
trap '[ -z "$pid" ] || kill "$pid" 2>/dev/null' EXIT
trap 'exit 129' HUP
trap 'exit 130' INT
trap 'exit 143' TERM
mkdir -p "$out" || fail "cannot make $out"
out=$(cd "$out" && pwd) || fail "cannot enter $out"
: > "$out/console.log"
: > "$out/scene.log"

# The guest's account, reached through the forwarded port with its key
# alone; the guest's host key is new with each image, so none is kept.
ssh_options="-i $key -o BatchMode=yes -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null"
ssh_options="$ssh_options -o LogLevel=ERROR -o ConnectTimeout=10"
guest()
{
    # shellcheck disable=SC2086 # the options are words
    ssh $ssh_options -p "$port" gfx@127.0.0.1 "$@" < /dev/null
}

echo "bed-forward: start: $qemu -accel $accel_option -cpu $cpu -smp $cpus -m $mib -device $gpu" \
    "-display egl-headless,rendernode=$node"
TMPDIR=$out "$qemu" -nodefaults -no-reboot -machine q35 -accel "$accel_option" -cpu "$cpu" \
    -smp "$cpus" -m "${mib}M" "$@" -drive "file=$disk,format=$format,if=virtio,snapshot=on" \
    -netdev "user,id=net0,hostfwd=tcp:127.0.0.1:$port-:22" -device virtio-net-pci,netdev=net0 \
    -device virtio-rng-pci -device "$gpu" -display "egl-headless,rendernode=$node" \
    -serial "file:$out/console.log" -monitor none > "$out/qemu.log" 2>&1 < /dev/null &
pid=$!

# quiet WHAT: fail when QEMU has ended, or when SECONDS have passed without a
# new byte on the console or in the scene's output; else wait a second.
size='' mark=$(date +%s)
quiet()
{
    kill -0 "$pid" 2>/dev/null ||
        fail "QEMU ended while $1: $(tail -n 2 "$out/qemu.log" | tr '\n' ' ')"
    now=$(cat "$out/console.log" "$out/scene.log" | wc -c)
    if [ "$now" != "$size" ]; then
        size=$now mark=$(date +%s)
    fi
    [ $(($(date +%s) - mark)) -lt "$silence" ] ||
        fail "nothing from the guest for $silence s while $1: $(tail -n 1 "$out/console.log")"
    sleep 1
}

began=$(date +%s)
until guest true 2> /dev/null; do
    quiet "its SSH server was awaited"
done
echo "bed-forward: guest: SSH answered after $(($(date +%s) - began)) s"
# shellcheck disable=SC2086 # the options are words
scp -q $ssh_options -P "$port" "$files" gfx@127.0.0.1:/tmp/forward.tar ||
    fail "the scenes could not be copied to the guest"
guest 'mkdir -p /tmp/forward && tar -xf /tmp/forward.tar -C /tmp/forward' ||
    fail "the scenes could not be unpacked in the guest"
scene_ref=
case $ref in
    '') ;;
    none) scene_ref=' -r none' ;;
    *)
        # shellcheck disable=SC2086 # the options are words
        scp -q $ssh_options -P "$port" "$ref" gfx@127.0.0.1:/tmp/forward/ref.frames ||
            fail "the reference $ref could not be copied to the guest"
        scene_ref=' -r /tmp/forward/ref.frames'
        ;;
esac

# The scene runs beside the watch on its output, so that a guest that stops
# is found by its silence.
(
    guest "sh /tmp/forward/scene.sh $profile -o /tmp/out$scene_ref${measure:+ -M $measure}${seconds:+ -t $seconds}" \
        > "$out/scene.log" 2>&1
    echo $? > "$out/scene.status"
) &
scene=$!
while [ ! -f "$out/scene.status" ]; do
    quiet "the scene ran"
done
wait "$scene"
grep '^bed-forward: ' "$out/scene.log" | grep -v '^bed-forward: result: '
guest 'tar -cf - -C /tmp out' > "$out/out.tar" 2> /dev/null || rm -f "$out/out.tar"
guest 'sudo systemctl poweroff' > /dev/null 2>&1
size='' mark=$(date +%s)
while kill -0 "$pid" 2>/dev/null; do
    now=$(wc -c < "$out/console.log")
    if [ "$now" != "$size" ]; then
        size=$now mark=$(date +%s)
    elif [ $(($(date +%s) - mark)) -ge 60 ]; then
        echo "bed-forward: guest: it did not power off; QEMU stopped"
        kill "$pid" 2>/dev/null
        break
    fi
    sleep 1
done
wait "$pid" 2>/dev/null
pid=

result=$(grep '^bed-forward: result: ' "$out/scene.log" | tail -n 1)
if [ "$(cat "$out/scene.status")" = 0 ] && [ "${result#bed-forward: result: pass: }" != "$result" ]; then
    echo "$result"
    exit 0
fi
[ -n "$result" ] || result="bed-forward: result: fail: the scene gave no result: $(tail -n 2 "$out/scene.log" | tr '\n' ' ')"
echo "$result"
exit 1
