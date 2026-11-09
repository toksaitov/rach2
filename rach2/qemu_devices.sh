# qemu_devices.sh: the QEMU options the run and debug scripts share.
# shellcheck shell=sh
#
# Sourced by the run/debug scripts and tests/graphics/debian-ref. Set
# AUDIO_DEVICE (sb16, hda, speaker or none; default speaker) before sourcing;
# this sets AUDIO_OPTS, NET_OPTS, DISPLAY_OPTS and ACCEL_OPTS for the QEMU
# command line, and QEMU, BUILD_DIR and MAKE_ARCH, the words a make of that
# build begins with, for the architecture.
#
# ARCH picks the architecture as mk/arch.mk does: i686, the default, boots
# qemu-system-i386 on what build/ holds, and x86_64 boots qemu-system-x86_64
# on what build/x86_64/ holds.
#
# Networking (on QEMU's SLIRP user network) is enabled by default:
#   NET=off                     attach no network card (-nic none)
#   NET_HOSTFWD="hostfwd=..."   override the host port forwards
#   NIC=rtl8139|e1000           choose the card (default rtl8139)
#
# QEMU opens a window, which on Linux and the other Unix systems needs an X or
# Wayland display. With neither DISPLAY nor WAYLAND_DISPLAY set, as in an ssh
# session, the machine runs with no window and its console is the serial port
# alone. DISPLAY_MODE is passed to -display and replaces that choice:
#   DISPLAY_MODE=none           no window on any host
#   DISPLAY_MODE=gtk|sdl|cocoa  the QEMU display of that name
#
# DISPLAY_PROFILE attaches the display device of etude's display profile of
# that name (tools/etude/Readme.md, The QEMU launcher) in place of QEMU's
# default VGA, with an EDID whose preferred mode is the fixed window of
# 1280x800: std-vga, virtio-vga, virtio-gpu, virgl or none. virgl draws with
# the host's GL on its render node through QEMU's egl-headless display, and
# exports the environment that puts that GL on the testing PC's GPU. The
# pointer is the PS/2 mouse of the PC's 8042; no USB tablet is attached.
#
# ACCEL_OPTS is the accelerator of a graphics run, which the graphics scripts
# use: KVM with the host's processor and its invariant time-stamp counter,
# which Rach2's vDSO clock reads, where /dev/kvm opens for reading and
# writing, and TCG with CPU (max) elsewhere. ACCEL=tcg or ACCEL=kvm chooses.
# The run scripts keep QEMU's default, TCG.

AUDIO_DEVICE="${AUDIO_DEVICE:-speaker}"

case "${ARCH:-i686}" in
    i686)
        QEMU=qemu-system-i386
        BUILD_DIR="$(cd "$(dirname "$0")" && pwd)/../build"
        MAKE_ARCH=
        ;;
    x86_64)
        QEMU=qemu-system-x86_64
        BUILD_DIR="$(cd "$(dirname "$0")" && pwd)/../build/x86_64"
        MAKE_ARCH="ARCH=x86_64 "
        ;;
    *)
        echo "run: ARCH is i686 or x86_64, not $ARCH" >&2
        exit 2
        ;;
esac

# The host's audio backend.
case "$(uname -s)" in
    Darwin*)
        # macOS: CoreAudio.
        AUDIO_BACKEND="-audiodev coreaudio,id=audio0"
        ;;
    Linux*)
        # Linux: PulseAudio when it or PipeWire's server for it is installed,
        # SDL otherwise.
        if command -v pulseaudio >/dev/null 2>&1 || command -v pipewire-pulse >/dev/null 2>&1; then
            AUDIO_BACKEND="-audiodev pa,id=audio0"
        else
            AUDIO_BACKEND="-audiodev sdl,id=audio0"
        fi
        ;;
    MINGW*|MSYS*|CYGWIN*)
        # Windows: DirectSound.
        AUDIO_BACKEND="-audiodev dsound,id=audio0"
        ;;
    *)
        # Any other system: SDL.
        AUDIO_BACKEND="-audiodev sdl,id=audio0"
        ;;
esac

case "$(uname -s)" in
    Darwin*|MINGW*|MSYS*|CYGWIN*)
        ;;
    *)
        if [ -z "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]; then
            DISPLAY_MODE="${DISPLAY_MODE:-none}"
        fi
        ;;
esac
if [ -n "${DISPLAY_MODE:-}" ]; then
    DISPLAY_OPTS="-display $DISPLAY_MODE"
else
    DISPLAY_OPTS=""
fi

DISPLAY_WINDOW="edid=on,xres=1280,yres=800"
case "${DISPLAY_PROFILE:-}" in
    "")
        ;;
    none)
        DISPLAY_OPTS="-vga none $DISPLAY_OPTS"
        ;;
    std-vga)
        DISPLAY_OPTS="-vga none -device VGA,$DISPLAY_WINDOW $DISPLAY_OPTS"
        ;;
    virtio-vga)
        DISPLAY_OPTS="-vga none -device virtio-vga,$DISPLAY_WINDOW $DISPLAY_OPTS"
        ;;
    virtio-gpu)
        DISPLAY_OPTS="-vga none -device virtio-gpu-pci,$DISPLAY_WINDOW $DISPLAY_OPTS"
        ;;
    virgl)
        DISPLAY_OPTS="-vga none -device virtio-vga-gl,$DISPLAY_WINDOW"
        DISPLAY_OPTS="$DISPLAY_OPTS -display egl-headless,rendernode=/dev/dri/renderD128"
        GALLIUM_DRIVER=d3d12
        MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA
        export GALLIUM_DRIVER MESA_D3D12_DEFAULT_ADAPTER_NAME
        ;;
    *)
        echo "run: DISPLAY_PROFILE is std-vga, virtio-vga, virtio-gpu, virgl or none, not $DISPLAY_PROFILE" >&2
        exit 2
        ;;
esac

if [ -z "${ACCEL:-}" ]; then
    if [ -r /dev/kvm ] && [ -w /dev/kvm ]; then
        ACCEL=kvm
    else
        ACCEL=tcg
    fi
fi
case "$ACCEL" in
    kvm)
        ACCEL_OPTS="-accel kvm -cpu host,invtsc=on"
        ;;
    *)
        ACCEL_OPTS="-accel $ACCEL -cpu ${CPU:-max}"
        ;;
esac

# The audio device AUDIO_DEVICE chose.
case "$AUDIO_DEVICE" in
    sb16)
        # Sound Blaster 16 (ISA, IRQ 5, DMA 1/5)
        AUDIO_OPTS="$AUDIO_BACKEND -device sb16,audiodev=audio0"
        ;;
    hda)
        # Intel HD Audio (PCI)
        AUDIO_OPTS="$AUDIO_BACKEND -device intel-hda -device hda-duplex,audiodev=audio0"
        ;;
    none)
        AUDIO_OPTS=""
        ;;
    speaker|*)
        # PC Speaker only (default)
        AUDIO_OPTS="$AUDIO_BACKEND -machine pcspk-audiodev=audio0"
        ;;
esac

# Networking on the SLIRP user network, enabled by default. Guest IP
# 10.0.2.15; the host is reachable from the guest at 10.0.2.2. Host port 8080
# is forwarded to the guest's httpd on port 80.
#
# The kernel drives two of QEMU's cards, so the card is a choice. The RTL8139
# is the default because it is the card the test harness gives a machine. The
# e1000 is the 82540EM, which is what QEMU attaches when nothing asks for a
# card, so it is what a plain qemu-system-i386 command line gets. NET=off
# passes -nic none, which turns that default card off, so the machine has no
# network card at all.
NET="${NET:-on}"
NIC="${NIC:-rtl8139}"
NET_HOSTFWD="${NET_HOSTFWD:-hostfwd=tcp::8080-:80}"
if [ "$NET" = "off" ]; then
    NET_OPTS="-nic none"
else
    NET_OPTS="-netdev user,id=net0,$NET_HOSTFWD -device $NIC,netdev=net0"
fi
