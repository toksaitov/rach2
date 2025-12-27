#!/bin/sh

# Start the kernel in QEMU with RAM disk
# -serial stdio redirects COM1 output to terminal for debugging
# -initrd loads a RAM disk as a multiboot module

# Detect OS and set appropriate audio backend for PC speaker
case "$(uname -s)" in
    Darwin*)
        # macOS - use CoreAudio
        AUDIO_OPTS="-audiodev coreaudio,id=audio0 -machine pcspk-audiodev=audio0"
        ;;
    Linux*)
        # Linux - try PulseAudio first, fall back to SDL
        if command -v pulseaudio >/dev/null 2>&1 || command -v pipewire-pulse >/dev/null 2>&1; then
            AUDIO_OPTS="-audiodev pa,id=audio0 -machine pcspk-audiodev=audio0"
        else
            AUDIO_OPTS="-audiodev sdl,id=audio0 -machine pcspk-audiodev=audio0"
        fi
        ;;
    MINGW*|MSYS*|CYGWIN*)
        # Windows - use DirectSound
        AUDIO_OPTS="-audiodev dsound,id=audio0 -machine pcspk-audiodev=audio0"
        ;;
    *)
        # Unknown OS - try SDL as fallback
        AUDIO_OPTS="-audiodev sdl,id=audio0 -machine pcspk-audiodev=audio0"
        ;;
esac

qemu-system-i386 -kernel kernel -initrd ../krffs/file_system.krffs -serial stdio $AUDIO_OPTS
