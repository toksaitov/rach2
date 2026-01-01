#!/bin/sh

# Start the kernel in QEMU with RAM disk
# -serial stdio redirects COM1 output to terminal for debugging
# -initrd loads a RAM disk as a multiboot module
#
# Usage: ./run_with_ram_disk.sh [audio_device]
#   audio_device: sb16, hda, or speaker (default: speaker)
#
# Examples:
#   ./run_with_ram_disk.sh      # PC speaker only (default)
#   ./run_with_ram_disk.sh sb16 # Sound Blaster 16
#   ./run_with_ram_disk.sh hda  # Intel HD Audio

AUDIO_DEVICE="${1:-speaker}"

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

qemu-system-i386 -kernel kernel -initrd ../krffs/file_system.krffs -serial stdio $AUDIO_OPTS
