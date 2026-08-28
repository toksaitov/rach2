# qemu_devices.sh - shared QEMU audio and networking options.
# shellcheck shell=sh
#
# Sourced by the run/debug scripts. Set AUDIO_DEVICE (sb16, hda, or speaker;
# default speaker) before sourcing; this sets AUDIO_OPTS and NET_OPTS for the
# qemu-system-i386 command line.
#
# Networking (on QEMU's SLIRP user network) is enabled by default:
#   NET=off                     disable networking
#   NET_HOSTFWD="hostfwd=..."   override the host port forwards
#   NIC=rtl8139|e1000           choose the card (default rtl8139)

AUDIO_DEVICE="${AUDIO_DEVICE:-speaker}"

# Detect OS and set appropriate audio backend
case "$(uname -s)" in
    Darwin*)
        # macOS - use CoreAudio
        AUDIO_BACKEND="-audiodev coreaudio,id=audio0"
        ;;
    Linux*)
        # Linux - try PulseAudio first, fall back to SDL
        if command -v pulseaudio >/dev/null 2>&1 || command -v pipewire-pulse >/dev/null 2>&1; then
            AUDIO_BACKEND="-audiodev pa,id=audio0"
        else
            AUDIO_BACKEND="-audiodev sdl,id=audio0"
        fi
        ;;
    MINGW*|MSYS*|CYGWIN*)
        # Windows - use DirectSound
        AUDIO_BACKEND="-audiodev dsound,id=audio0"
        ;;
    *)
        # Unknown OS - try SDL as fallback
        AUDIO_BACKEND="-audiodev sdl,id=audio0"
        ;;
esac

# Configure audio device based on selection
case "$AUDIO_DEVICE" in
    sb16)
        # Sound Blaster 16 (ISA, IRQ 5, DMA 1/5)
        AUDIO_OPTS="$AUDIO_BACKEND -device sb16,audiodev=audio0"
        ;;
    hda)
        # Intel HD Audio (PCI)
        AUDIO_OPTS="$AUDIO_BACKEND -device intel-hda -device hda-duplex,audiodev=audio0"
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
# is the default because it is what every test boots against. The e1000 is the
# 82540EM, which is what QEMU attaches when nothing asks for a card, so it is
# what a plain qemu-system-i386 command line gets; a boot with NET=off, which
# passes no network options at all, therefore still finds a card this kernel
# can drive.
NET="${NET:-on}"
NIC="${NIC:-rtl8139}"
NET_HOSTFWD="${NET_HOSTFWD:-hostfwd=tcp::8080-:80}"
if [ "$NET" = "off" ]; then
    NET_OPTS=""
else
    NET_OPTS="-netdev user,id=net0,$NET_HOSTFWD -device $NIC,netdev=net0"
fi
