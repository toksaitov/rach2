# Doom for Rach2

![Doom's E1M1 Running on Rach2](https://raw.githubusercontent.com/rachmiroff/images/refs/heads/main/rach2/doom.png)

A port of Doom to the Rach2 kernel using [doomgeneric](https://github.com/ozkl/doomgeneric) as the platform abstraction layer. The port runs as a freestanding ELF32 executable with VGA Mode 13h graphics, PS/2 keyboard and mouse input, real audio playback via SB16/HDA, and OPL3 FM music synthesis.

## Features

* VGA Mode 13h graphics (320x200, 256 colors)
* PS/2 keyboard input with full key mapping
* PS/2 mouse support for aiming and movement
* Real audio via Sound Blaster 16 or Intel HD Audio
* PC speaker fallback for sound effects
* OPL3 FM music via Nuked-OPL3 emulator
* MIDI/MUS music parsing from Chocolate Doom

## Source Structure

```
ports/doom/
├── Makefile              # Build configuration
├── crt0.c                # C runtime: _start, syscalls, 4MB heap
├── doomgeneric_rach2.c   # Platform layer for Rach2
├── i_pcsound.c           # PC speaker and audio driver interface
├── i_oplmusic.c/h        # OPL3 music driver (9-voice FM synthesis)
├── midifile.c/h          # MIDI file parser (from Chocolate Doom)
├── mus2mid.c/h           # MUS to MIDI converter (from Chocolate Doom)
├── opl3.c/h              # Nuked-OPL3 FM synthesizer emulator
├── include/              # Freestanding libc stubs
│   ├── stdio.h           # printf, FILE, fopen/fread/fclose
│   ├── stdlib.h          # malloc, free, atoi, rand
│   ├── string.h          # memcpy, memset, strlen, strcmp
│   └── ...               # Other standard headers
├── src/                  # doomgeneric engine (from ozkl/doomgeneric)
│   ├── d_main.c          # Doom main loop
│   ├── r_*.c             # Rendering engine
│   ├── p_*.c             # Game physics and logic
│   ├── s_sound.c         # Sound subsystem
│   └── ...               # 80 source files, 97 headers
└── doom1.wad             # Shareware Doom WAD file
```

## Platform Layer

The `doomgeneric_rach2.c` file implements the doomgeneric interface for Rach2:

| Function           | Implementation                                                   |
|--------------------|------------------------------------------------------------------|
| `DG_Init`          | Opens framebuffer device, switches to Mode 13h, sets VGA palette |
| `DG_DrawFrame`     | Writes 64000-byte framebuffer to VGA via write() syscall         |
| `DG_GetTicksMs`    | Uses gettimeofday() syscall, converts kernel time to milliseconds|
| `DG_SleepMs`       | Busy-waits using gettimeofday() for timing                       |
| `DG_GetKey`        | Reads PS/2 scancodes via read(), maps to Doom keycodes           |
| `DG_SetWindowTitle`| No-op (no window system)                                         |

Mouse input is polled from the `mouse` device file, posting `ev_mouse` events for turning and movement.

## Audio Architecture

### Sound Effects

The port uses a layered audio system with automatic hardware detection:

1. **Intel HD Audio** (PCI): MMIO registers, CORB/RIRB codec communication
2. **Sound Blaster 16** (ISA): 8-bit DMA, IRQ-driven double-buffering
3. **PC Speaker** (fallback): PIT channel 2 frequency control

Doom itself mixes sound effects and music into 11025 Hz, 8-bit mono chunks and submits them through `AUDIO_WRITE`.

On SB16 this format is consumed directly. On Intel HDA, the kernel may resample and upmix the stream to a codec-supported hardware format, typically 48 kHz, 16-bit stereo on real machines. DMX sound lumps from the WAD are parsed and mixed in software before being written to the selected audio device.

### Music

OPL3 FM synthesis provides the classic Doom music:

* **Nuked-OPL3**: Cycle-accurate OPL3 emulator generating audio samples
* **GENMIDI**: General MIDI instrument definitions from the WAD
* **MIDI/MUS**: Music format parsing from Chocolate Doom
* **9-voice polyphony**: Full OPL3 voice management with pitch bend and modulation

The OPL emulator runs at 49716 Hz and is downsampled into Doom's 11025 Hz mono mixer before the mixed stream is handed to the kernel audio layer. On HDA, the kernel may resample that stream again to match the hardware codec.

## Required Syscalls

| Syscall      | Number | Purpose                                                   |
|--------------|--------|-----------------------------------------------------------|
| read         | 3      | Keyboard scancodes, mouse packets, file I/O               |
| write        | 4      | Framebuffer output, audio output                          |
| open         | 5      | WAD file, framebuffer device, mouse device, audio device  |
| close        | 6      | Close file descriptors                                    |
| ioctl        | 54     | FB_SETPALETTE, KIOCSOUND, AUDIO_* commands                |
| gettimeofday | 78     | Timing for game loop and music                            |
| exit         | 1      | Clean shutdown                                            |

## Build

Requires a cross-compiler (`i686-elf-gcc` or `x86_64-elf-gcc`):

```sh
make        # Build doom executable
make clean  # Remove compiled files
```

This produces a ~390KB freestanding ELF32 executable.

## Running in Rach2

1. Build Doom:
   ```sh
   cd ports/doom
   make
   ```

2. Build the RAM disk with Doom included:
   ```sh
   cd krffs
   ./build_ramdisk.sh
   ```

3. Boot Rach2:
   ```sh
   cd rach2
   ./run_with_ram_disk.sh sb16   # Sound Blaster 16
   ./run_with_ram_disk.sh hda    # Intel HD Audio
   ```

4. Run from the shell:
   ```
   > doom
   ```

On supported real hardware, the same `doom` binary uses the kernel's HDA driver directly. Useful kernel-side audio debug commands during bring-up live under the `audio` command: `audio test scale`, `audio test write`, `audio test watch 440 2000 100`, `audio hda dump`, `audio hda codec`, `audio hda pins`, `audio hda stats`, and `audio hda out primary`.

## Controls

| Key          | Action                             |
|--------------|------------------------------------|
| Arrow keys   | Move forward/back, turn left/right |
| Ctrl         | Fire weapon                        |
| Space        | Use/open doors                     |
| Shift        | Run                                |
| Alt + arrows | Strafe                             |
| 1-7          | Select weapon                      |
| Tab          | Automap                            |
| Escape       | Menu                               |
| Mouse X      | Turn left/right                    |
| Mouse Y      | Move forward/back                  |
| Left click   | Fire                               |
| Right click  | Use                                |

## Memory Requirements

* **WAD file**: ~4MB (doom1.wad)
* **Heap**: 4MB bump allocator in crt0.c
* **Framebuffer**: 64KB at 0xA0000
* **Audio buffers**: Doom mixes in small 11025 Hz chunks; the kernel provides the larger DMA/ring buffers (for HDA this is roughly 32 KB of DMA plus an 8 KB write ring)

Total: Requires at least 10MB of available memory.

## Credits

* [doomgeneric](https://github.com/ozkl/doomgeneric) - Platform abstraction layer by Osman Celimli
* [Nuked-OPL3](https://github.com/nukeykt/Nuked-OPL3) - OPL3 emulator by Nuke.YKT
* [Chocolate Doom](https://github.com/chocolate-doom/chocolate-doom) - MIDI/MUS parsing code
* [id Software](https://github.com/id-Software/DOOM) - Original Doom source code
