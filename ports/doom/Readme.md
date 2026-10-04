# Doom for Rach2

This port runs Doom as a freestanding static program, ELF32 i386 or ELF64 x86-64. It uses the doomgeneric engine with a Rach2 platform layer for VGA graphics, keyboard and mouse input, elapsed time, PCM sound, PC speaker sound, and synthesized music.

The engine sources under `src/` come from doomgeneric and its Doom ancestry. `midifile.c` and `mus2mid.c` are adapted from Chocolate Doom. `opl3.c` and `opl3.h` contain Nuked OPL3. The files above `src/`, the stub headers under `include/`, and the component Makefile form the Rach2 integration.

## Build

From the repository root, run:

```
make ports/doom
make ports/doom ARCH=x86_64
```

The component Makefile uses the architecture's compiler (`mk/arch.mk`), `i686-elf-gcc -m32` or `x86_64-elf-gcc -m64`, with GNU99, freestanding static flags, local replacement headers, and a fixed text address: `0x08048000` on i686 and `0x400000` on x86-64, where TinyCC places a program's. Header dependencies rebuild the affected objects. The output is `build/ports/doom/doom`, or `build/x86_64/ports/doom/doom`.

The build contains `crt0.c`, six local platform and audio sources, and the 80 engine sources named by `DOOM_SRC`. `crt0.c` supplies `_start`, nine Linux system call wrappers, written with `int $0x80` and the i386 numbers or with `syscall` and the x86-64 numbers, the file interface used by the engine, a small libc subset, and a bump heap over the process's `brk` range, grown 64 KiB at a time. `free` does not reclaim memory.

The engine's zone stores game data. Its initial request is 16 MiB. The platform reads `MemFree` from `/proc/meminfo`, converts it to whole MiB and subtracts 8 MiB. The engine uses that suggestion only when it is at least 6 MiB and less than 16 MiB. A smaller suggestion leaves the request at 16 MiB: 13 MiB free suggests 5 MiB but requests 16 MiB. This is not a lower-bound clamp. If allocation fails, the engine retries in 1 MiB steps down to 6 MiB. The runtime reserves addresses through `brk`; physical pages are allocated on first access, so a successful allocation does not ensure enough physical memory for the zone. `-mb N` requests exactly N MiB without those retries.

Run `make -C ports/doom clean` only when the component output must be removed. Ordinary rebuilds do not need it.

## Platform interface

`doomgeneric_rach2.c` supplies the doomgeneric callbacks:

* `DG_Init` opens `/fb` and the optional `/mouse` device, clears local state, and records the start time.
* `DG_DrawFrame` sends a changed 256-entry RGB palette through `FB_SETPALETTE`, writes the 320 by 200 indexed frame, and polls both input devices.
* `DG_GetTicksMs` and `DG_SleepMs` use `gettimeofday`; sleep is a busy wait.
* `DG_GetKey` returns queued key transitions read from standard input.
* `DG_SetWindowTitle` does nothing because Rach2 has no window system.
* `DG_Shutdown` closes the mouse and framebuffer descriptors and prints the final report after text mode returns. The port's `exit` calls it, including when an engine callback exits directly.

The local runtime wraps `exit`, `read`, `write`, `open`, `close`, `lseek`, `ioctl`, and `gettimeofday`. File access uses those wrappers. The port does not link musl. Its `mkdir`, `remove`, and `rename` stubs always fail. Normal save slots do not work: the engine ignores the failed rename of the temporary save and can report success without updating the slot.

### Graphics and input

The framebuffer path is `/fb`, independent of the working directory. Opening it selects VGA mode 13h. Each frame is 64,000 palette indices. Palette updates pass 768 RGB bytes to the kernel. On a machine booted by UEFI, which has no VGA, the kernel draws each frame on the framebuffer at the largest whole scale that fits, three on a 1366x768 screen, in the middle (`fbcon_gfx_frame` in `rach2/sys/display/fbcon.c`).

Keyboard input arrives as PS/2 set 1 scan codes on standard input. The platform layer maps letters, digits, arrows, modifiers, function keys, Enter, Escape, Tab, and the usual Doom action keys. Its queue can contain at most 31 unread transitions.

Mouse input comes from `/mouse` when that device opens successfully. Each three-byte packet supplies button bits and signed X and Y movement. Doom continues with keyboard input if the mouse is unavailable.

## Sound and music

The sound module issues `AUDIO_GET_INFO` on the console descriptor. If SB16 or HDA PCM output is available, it starts 11,025 Hz unsigned 8-bit mono playback, sets volume to 80, and mixes eight sound-effect channels with synthesized music. `AUDIO_WRITE` can accept a short chunk when the kernel ring is full; the port retries the unwritten portion until the call stops making progress, then drops the remainder.

SB16 consumes the requested format directly. HDA converts it to the codec's selected sample rate, sample width, and channel count. The kernel audio subsystem selects the hardware and converts formats.

Music uses the WAD's `GENMIDI` instruments and the local MUS and MIDI readers. Music initialization fails if the instrument lump is missing, shorter than its header and 175 instrument records, or has an invalid header. The MIDI reader rejects channel parameters above 127. The OPL player iterates up to 32 tracks, each with its own wait in samples, and generates samples up to the nearest event on any track. A converted MUS lump has one track; a format 1 file can put tempo on track 0 and notes on later tracks. Tempo calculations use 64-bit products and allow at least one sample per tick.

A nonempty song can loop at a buffer boundary or several times within one buffer. A song that reaches its end twice without generating a sample stops and returns silence. `tests/host/oplmusic_hosttest.c` checks instrument and MIDI validation, two-track playback, tempo arithmetic, buffer-boundary loops and empty songs. The Nuked OPL3 emulator generates stereo 16-bit samples at 49,716 Hz. The mixer reduces those samples to its 11,025 Hz mono stream before sending the combined bytes to the kernel.

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

The os-real, GRUB-test, and metal-disk manifests declare both `/bin/doom` and `/demo/doom1.wad` as optional files. Image construction reports and skips an absent optional file. The normal image targets build the executable first, but a fresh clone still lacks the WAD.

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

After login, start Doom from any directory:

```
doom
```

The port prints one line every ten seconds and once more on normal quit, after the text screen returns: `doom: N frames in M ms, the longest L ms; music emulator E ms in C calls for S samples after V events, mixer X ms; zone Z MiB`. The zone field reports the allocated size. Confirming Quit in the menu runs the engine's exit callbacks, restores text mode, prints the report and exits with status zero.

The port forwards command-line arguments to the engine and appends `-iwad /demo/doom1.wad` as the default. An explicit `-iwad PATH` takes precedence; a relative PATH is resolved from the current directory. For example, `doom -mb 10` requests a 10 MiB zone with the staged WAD.

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
build/tools/inspect/elfcheck build/ports/doom/doom
```

The root `make` rebuilds the port and the images that carry it. `tests/run doom` first starts Doom with the WAD moved away and requires the engine's `IWAD file '/demo/doom1.wad' not found!` and exit status 255, then launches Doom from `/root` on a disk root, checks that it remains running after ten seconds, checks framebuffer initialization and a zone of at least 10 MiB, and records disk command counts. It requires the separately supplied WAD. These checks do not establish visual or audible quality; check those by booting os-real and playing the game.

`tests/host/doom_platform_hosttest.c` checks argument forwarding, zone selection, normal quit and direct exit from a callback with simulated device calls. `sh tests/host/doom_platform_mutate.sh` compiles and runs it alone under ASan and UBSan, requires each compiled fault to fail, then restores, touches and rebuilds the source and requires a pass. It does not boot the kernel or render a frame.
