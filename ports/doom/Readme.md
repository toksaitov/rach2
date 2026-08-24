# Doom for Rach2

This port runs Doom as a freestanding static ELF32 i386 program. It uses the doomgeneric engine with a Rach2 platform layer for VGA graphics, keyboard and mouse input, elapsed time, PCM sound, PC speaker sound, and synthesized music.

The engine sources under `src/` come from doomgeneric and its Doom ancestry. `midifile.c` and `mus2mid.c` are adapted from Chocolate Doom. `opl3.c` and `opl3.h` contain Nuked OPL3. The files above `src/`, the stub headers under `include/`, and the component Makefile form the Rach2 integration.

## Build

From the repository root, run:

```
make ports/doom
```

The component Makefile uses `i686-elf-gcc` with GNU99, freestanding static i386 flags, local replacement headers, and a fixed text address of `0x08048000`. Header dependencies rebuild the affected objects. The output is `build/ports/doom/doom`.

The build contains `crt0.c`, six local platform and audio sources, and the 80 engine sources named by `DOOM_SRC`. `crt0.c` supplies `_start`, eight Linux i386-compatible syscall wrappers, the file interface used by the engine, a small libc subset, and a fixed 8 MiB bump heap. `free` does not reclaim memory.

Run `make -C ports/doom clean` only when the component output must be removed. Ordinary rebuilds do not need it.

## Platform interface

`doomgeneric_rach2.c` supplies the doomgeneric callbacks:

* `DG_Init` opens `fb` and the optional `mouse` device relative to the current directory, clears local state, and records the start time.
* `DG_DrawFrame` sends a changed 256-entry RGB palette through `FB_SETPALETTE`, writes the 320 by 200 indexed frame, and polls both input devices.
* `DG_GetTicksMs` and `DG_SleepMs` use `gettimeofday`; sleep is a busy wait.
* `DG_GetKey` returns queued key transitions read from standard input.
* `DG_SetWindowTitle` does nothing because Rach2 has no window system.
* `DG_Shutdown` closes the framebuffer and mouse descriptors.

The local runtime wraps `exit`, `read`, `write`, `open`, `close`, `lseek`, `ioctl`, and `gettimeofday`. File access uses those wrappers. The port does not link musl.

### Graphics and input

The framebuffer path is `/fb` when Doom starts from `/`. Opening it selects VGA mode 13h. Each frame is 64,000 palette indices. Palette updates pass 768 RGB bytes to the kernel.

Keyboard input arrives as PS/2 set 1 scan codes on standard input. The platform layer maps letters, digits, arrows, modifiers, function keys, Enter, Escape, Tab, and the usual Doom action keys. Its queue can contain at most 31 unread transitions.

Mouse input comes from `/mouse` when that device opens successfully. Each three-byte packet supplies button bits and signed X and Y movement. Doom continues with keyboard input if the mouse is unavailable.

## Sound and music

The sound module asks the kernel for audio information through an ioctl on the console descriptor. If SB16 or HDA PCM output is available, it starts 11,025 Hz unsigned 8-bit mono playback, sets volume to 80, and mixes eight sound-effect channels with synthesized music. `AUDIO_WRITE` can accept a short chunk when the kernel ring is full; the port retries the unwritten portion until the call stops making progress, then drops the remainder.

SB16 consumes the requested format directly. HDA converts it to the codec's selected sample rate, sample width, and channel count. The kernel audio subsystem selects the hardware and converts formats.

Music uses the WAD's `GENMIDI` instruments and the local MUS and MIDI readers. The Nuked OPL3 emulator generates stereo 16-bit samples at 49,716 Hz. The mixer reduces those samples to its 11,025 Hz mono stream before sending the combined bytes to the kernel.

If no PCM driver is available, sound effects use the WAD's `DP` lumps and `KIOCSOUND` to drive the PC speaker. This fallback does not play synthesized music.

## Game data

The program requires an IWAD. WAD files are operator-supplied, ignored by Git, and never built. Put the selected file at:

```
ports/doom/doom1.wad
```

`ports/doom/fetch_wad.sh` checks that path. It verifies the id Software Doom shareware v1.9 file when its SHA-256 digest is:

```
1d7d43be501e67d927e415e0b8f3e29c3bf33075e859721816f652a526cac771
```

Another Doom IWAD, including Freedoom Phase 1, may use the same staged filename. The helper reports its digest but cannot verify its identity.

The os-real, GRUB-test, and metal-disk manifests declare both `/bin/doom` and `/doom1.wad` as optional files. Image construction reports and skips an absent optional file. The normal image targets build the executable first, but a fresh clone still lacks the WAD.

## Run

Build the image after placing the WAD:

```
./ports/doom/fetch_wad.sh
make os-real-image
```

Boot with one of the QEMU audio devices:

```
cd rach2
./run.sh os-real sb16
./run.sh os-real hda
./run.sh os-real speaker
```

After login, start Doom from the root directory:

```
cd /
doom
```

The port discards its incoming argument vector and always passes `-iwad doom1.wad` to the engine. That name is relative, while the manifest stages `/doom1.wad`. Starting in another directory fails to find the WAD and prints `IWAD file 'doom1.wad' not found!`.

The same binary can use the kernel HDA driver on supported hardware. Hardware boot and disk-image procedures are described in the root and kernel Readmes.

## Default controls

* Arrow keys move and turn.
* Left Ctrl fires.
* Space uses doors and switches.
* Shift runs.
* Number keys select weapons.
* Tab opens the automap.
* Escape opens the menu.
* Mouse movement turns and moves; the button bits use Doom's configured mouse bindings.

## Verification

`make ports/doom` proves the freestanding build and dependency graph. Check the output against the kernel loader with:

```
python3 tools/check_elf.py build/ports/doom/doom
```

`make full` rebuilds the port and the images that carry it. The repository has no automated test that launches Doom, validates an IWAD, or judges graphics and audio output. Test those paths by booting os-real and running the program as shown above.
