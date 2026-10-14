# Doom for Rach2

`ports/doom/` is Doom for Rach2: the doomgeneric engine in `src/`, and an in-house platform layer that connects it to the kernel. `doomgeneric_rach2.c` supplies the functions doomgeneric asks of a platform, `i_pcsound.c` is the engine's sound and music module, `crt0.c` is the program's entry point, its nine system calls and most of the C library the engine uses, and the 20 headers in `include/` declare that library in place of a system's. `i_oplmusic.c`, adapted from Chocolate Doom's file of that name, plays the game's music on the Nuked OPL3 emulator. The build makes `/bin/doom`, one static program for i686 or x86-64 that links no C library. The port exists so that the system has a game, and so that one well-known program uses the kernel's framebuffer, keyboard, mouse, sound and clock interfaces together.

## Use

The game needs an IWAD, Doom's game data file, which the tree does not have. Put one at `ports/doom/doom1.wad`, where `ports/doom/.gitignore` keeps it out of Git, and check it:

```
sh ports/doom/fetch_wad.sh
```

The script downloads nothing. It reports whether the file is the shareware Doom 1.9 `doom1.wad`, by its SHA-256 digest; another IWAD, such as Freedoom's `freedoom1.wad` saved under that name, is accepted with its digest printed. With no file there, it says where one can be had and exits with status 1.

The manifests `os-real`, `grub-test`, `rach2-metal-disk` and `os-real-gnu` in `krffs/manifests/` stage the game as `/bin/doom` and the WAD as `/demo/doom1.wad`, each as an optional file, which the stager skips when it is absent ([krffs/Readme.md](../../krffs/Readme.md)). Put the WAD in place before `make` builds the images; without it the game exits with status 255 ([doom(1)](../../man/man1/doom.1)). To build the system and boot it in QEMU with a Sound Blaster 16:

```
make
cd rach2
./run.sh os-real sb16
```

`run.sh` takes `hda` or `speaker` in place of `sb16`, and after `make ARCH=x86_64`, `ARCH=x86_64 ./run.sh os-real sb16` boots the x86-64 system. Log in at the console and type `doom`. The game reads its keys and plays its sound through standard input, which must be the console: the console gives the process that owns `/fb` the raw scan codes, and the sound requests of [ioctl(2)](../../man/man2/ioctl.2) are the console's. Started from an SSH session, the game draws on the machine's screen but has no keys and no sound, and its standard input should be `/dev/null`, since a read from the session's terminal waits for input.

The controls come from the port's scan code table and the engine's default bindings:

* The arrow keys, or the keypad's 8, 4, 6 and 2, move and turn.
* Ctrl fires, Space opens doors and works switches, and either Shift runs.
* The digits select weapons, Tab shows the map, and Escape opens the menu.
* Moving the mouse turns and moves. Its left button fires, its right button makes the motion strafe, and its middle button moves forward.

[doom(1)](../../man/man1/doom.1) describes the command line, the default WAD, the size of the zone, how the game quits and what it cannot save.

## Sources and license

* `src/` is 177 of the 202 files of the `doomgeneric/` directory of [doomgeneric](https://github.com/ozkl/doomgeneric) at commit `fc601639494e`, each as it is there except the three that Local changes lists. It leaves out the build files, the other platforms' layers and sound modules, `gusconf.c`, `icon.c` and `mus2mid.c`. 170 of the files have the notice of the GNU General Public License, version 2 or later; `sha1.c`, from GnuPG, has version 3 or later; and `config.h`, `doom.h`, `doomgeneric.c`, `doomgeneric.h`, `dummy.c` and `mus2mid.h` have none.
* `midifile.c`, `midifile.h`, `mus2mid.c` and `mus2mid.h` are adapted from Chocolate Doom's files of those names, which are under the GPL, version 2 or later. The copies here have the copyright lines (Simon Howard's, and for `mus2mid` also id Software's and Ben Ryves's) but not the license notice.
* `opl3.c` and `opl3.h` are Nuked OPL3 1.8, copyright 2013-2020 Nuke.YKT, under the GNU Lesser General Public License, version 2.1 or later.
* `i_oplmusic.c` is adapted from Chocolate Doom's `i_oplmusic.c`, which is under the GPL, version 2 or later; it has no copyright line or license notice.
* The rest is in-house: `crt0.c`, `doomgeneric_rach2.c`, `i_pcsound.c`, `i_oplmusic.h`, the headers in `include/`, the `Makefile` and `fetch_wad.sh`. The repository declares no license for its own code ([Readme.md](../../Readme.md)).
* `ports/doom/` has no copy of these licenses' texts, and the tree has no game data.

## Local changes

Three files of `src/` differ from doomgeneric:

* `i_system.c`: `DEFAULT_RAM`, the zone's first request, is 16 MiB where doomgeneric has 6. Unless `-mb` is given, `I_ZoneBase` takes the platform's suggestion `DG_zone_mib` in place of it when the suggestion is at least `MIN_RAM`, 6 MiB, and below 16, and afterwards sets `DG_zone_mib` to the size it allocated. `I_Quit` calls `exit(0)` after the exit callbacks, where doomgeneric's returns.
* `d_main.c`: `demolumpname`, the name of the demo `-playdemo` plays, is static, since doomgeneric returns from `D_DoomMain` before the demo is played and the game keeps a pointer to the name until the demo ends.
* `p_maputl.c`: `InterceptsOverrun` converts a thing's pointer to `int` through `intptr_t`, which keeps its low 32 bits on x86-64.

Outside `src/`:

* `midifile.c` reads a MIDI file from memory (a `MEMFILE`) in place of a `FILE`, and refuses a channel event whose parameter byte has its top bit set, where Chocolate Doom's takes any byte.
* `opl3.c` includes `include/string.h` in place of the C library's headers, and defines `UINT64_C` and `uint_fast32_t` itself.
* `i_oplmusic.c` is Chocolate Doom's player simplified, as its header says, to the Nuked OPL3 emulator and timing by polling: the sound module asks it for samples, and it runs each of the song's events when the samples it has made reach the event's time.

## Design

### The runtime

`crt0.c` and the inline functions of `include/` are the program's C library. The entry point and the system calls are written once for each architecture, and the rest is the same C for both. `_start`, in assembly, reads `argc` and `argv` where the System V ABI puts them on the initial stack, aligns the stack to 16 bytes, calls `main` and passes its result to `_exit`. The nine system calls are `exit`, `read`, `write`, `open`, `close`, `lseek`, `brk`, `ioctl` and `gettimeofday`, made with `int $0x80` and the numbers of [`rach2/sys/arch/i686/sysnr.h`](../../rach2/sys/arch/i686/sysnr.h), or with `syscall` and the numbers of [`rach2/sys/arch/x86_64/sysnr.h`](../../rach2/sys/arch/x86_64/sysnr.h).

`malloc` hands out memory between the process's first break and its current one, in pieces aligned to 8 bytes. When a request does not fit, it moves the break by the shortfall rounded up to 64 KiB, and returns NULL if `brk` refuses. The kernel reserves the addresses at once and gives each page its frame at first touch, so the zone that the engine allocates at start costs memory only as the game fills it.

A `FILE` is a descriptor with no buffer, and `fopen` creates a file with mode 0666 less the umask. `printf` writes at most 1023 bytes a call, and `sscanf` reads only `%d`. `__divdi3` and its three kin do the 64-bit division that i686 code calls and no libgcc supplies. The headers define the rest inline, some as stubs that always fail.

### The platform layer

`main` in `doomgeneric_rach2.c` appends `-iwad /demo/doom1.wad` to the arguments; the engine takes the first `-iwad`, so one the caller gives is used. It calls `doomgeneric_Create`, which runs `D_DoomMain` to the start of the game loop, and then `doomgeneric_Tick`, one pass of the loop, forever. The engine calls back:

* `DG_Init` reads `MemFree` from `/proc/meminfo` and, when it has more than 8 whole MiB, leaves the whole MiB less 8 in `DG_zone_mib` for `I_ZoneBase`. It opens `/fb` for reading and writing, which puts the display in mode 13h, and `/mouse` for reading, and records the time. When `/fb` does not open it writes `Failed to open framebuffer` to standard error, and the game runs without drawing.
* `DG_DrawFrame` sends the palette when the engine has changed it, writes the frame, and reads the keyboard and the mouse.
* `DG_GetTicksMs` is the milliseconds since `DG_Init`, from `gettimeofday`, and `DG_SleepMs` calls `gettimeofday` until the time has passed.
* `DG_GetKey` gives the engine the next queued key transition, reading the keyboard first when the queue is empty.

The port defines `exit`, so every way out of the game runs the same code: `I_Quit`, after a confirmed Quit or the end of a demo played with `-playdemo`; `I_Error`; and an exit callback that calls `exit` itself, as `D_Endoom` does. `exit` calls `DG_Shutdown`, which closes the mouse and `/fb` and then prints the report, and `_exit` ends the process with the status given: 0 from `I_Quit`, 255 from the `exit(-1)` of `I_Error`. The last close of `/fb` by its owner puts the display back in text mode, so the report appears on the text screen.

The report is one line on standard output, printed every ten seconds while the game draws and once more by `DG_Shutdown`:

```
doom: N frames in M ms, the longest L ms; music emulator E ms in C calls for S samples after V events, mixer X ms; zone Z MiB
```

N is the frames drawn and M the time from the first to the last; L is the longest time between two frames; E is the time spent in the music player, which made C calls of the emulator for S stereo samples and ran V song events; X is the time spent mixing; and Z is the zone the engine allocated.

### Display and input

The engine draws each frame as 64,000 palette indices (`CMAP256`, 320 by 200), and `DG_DrawFrame` writes them to `/fb` as they are. A changed palette goes first, as 256 RGB triples, through `FB_SETPALETTE`. What the kernel does with them, in mode 13h on a VGA or scaled on the framebuffer of a machine booted by UEFI, is in [the display Readme](../../rach2/sys/display/Readme.md) and [ioctl(2)](../../man/man2/ioctl.2).

While the game owns `/fb`, a read of the console returns the set 1 scan codes in the kernel's raw scan code ring, and does not wait when the ring is empty ([the terminal Readme](../../rach2/sys/tty/Readme.md)). The game reads them a byte at a time after each frame. `scancode_to_doomkey` maps the low seven bits of a code to a Doom key, and the top bit tells a release from a press; a code with no entry, such as Alt or the 0xE0 prefix, is dropped. The transitions wait in a ring of 32 entries, so 31 can be unread at once, and one that finds the ring full is dropped.

`/mouse` gives three-byte packets, a byte of buttons and signed X and Y motion (`struct mouse_packet` in `rach2/sys/mouse.h`), which the game posts to the engine as mouse events after each frame. The open fails when the machine has no mouse, and the game goes on with the keyboard alone.

### Sound

`i_pcsound.c` defines `DG_sound_module` and `DG_music_module`, the modules `src/i_sound.c` compiles in under `FEATURE_SOUND`. The sound module lists the PC speaker and the Sound Blaster as its devices, so the engine's default device, the Sound Blaster, selects it on every machine. At start it asks the console for the audio driver (`AUDIO_GET_INFO` on descriptor 0). When the driver plays samples, as the SB16 and HDA drivers do, the module starts output of 11,025 Hz unsigned 8-bit mono, sets the volume to 80 and starts the music player. Otherwise it plays effects on the PC speaker, and the game has no music.

With sample output, an effect is a `DS` lump, unsigned 8-bit samples after an 8-byte header, and eight channels play at once at the engine's volumes; the stereo separation is ignored. Each time the engine updates its sound, the module works out from the clock how many output samples are due, at most 5040, and makes them in chunks of 315. For each chunk it takes 49716/11025 times as many stereo samples from the music player, averages them down to the output rate in 8 bits, adds the channels, and limits each sum to a byte. `AUDIO_WRITE` may take part of a chunk when the kernel's ring is full; the module writes the rest again until a call takes nothing, and drops what is left. What the kernel does with the stream is in [the audio Readme](../../rach2/sys/audio/Readme.md).

On the PC speaker an effect is a `DP` lump: a 4-byte header, then one tone value for each 7 ms. A value from 1 to 127 is a frequency, 175 Hz times 2 to the power (value - 1) / 24, which `KIOCSOUND` sounds; 0 and values above 127 are silence. The speaker sounds one effect at a time, and a new effect ends the one playing.

### Music

The player in `i_oplmusic.c` starts only when the WAD's `GENMIDI` lump is whole: the header `#OPL_II#` and 175 instrument records, 128 melodic and 47 percussion. A song lump in MIDI form is read as it is, and any other is converted from MUS by `mus2mid.c`; `midifile.c` reads the MIDI from memory. The player walks up to 32 tracks, each with its own wait in samples until its next event, so a MUS song's one track and a format 1 file's tempo and note tracks play alike. It runs the events that are due, at most 1024 on a track before a sample is made, and then has the emulator make samples up to the nearest next event. The samples per tick come from the tempo by a 64-bit product, and are at least one. A looping song starts again at its end; one that reaches its end twice without making a sample stops and gives silence. Nuked OPL3 makes stereo 16-bit samples at 49,716 Hz, and the player uses nine of its voices.

## Building

From the repository root:

```
make ports/doom
make ports/doom ARCH=x86_64
```

The root rule runs `make -C ports/doom` after the tool check, and `make` builds the port before any image that stages it. The output is `build/ports/doom/doom`, ELF32 i386, or `build/x86_64/ports/doom/doom`, ELF64 x86-64, with the objects beside it.

The `Makefile` compiles `crt0.c`, the six sources of `LOCAL_SRC` and the 80 of `DOOM_SRC`, which are all the `.c` files of `src/`, with the architecture's cross compiler ([`mk/toolchain.mk`](../../mk/toolchain.mk)), `-std=gnu99 -Os -ffreestanding`, and the headers of `include/` ahead of `src/` and the compiler's own. `CMAP256` makes the engine draw in palette indices, and `FEATURE_SOUND` compiles in the sound modules. It links with `-nostdlib -static` and the text at `ARCH_USER_TEXT` of [`mk/arch.mk`](../../mk/arch.mk): 0x08048000 on i686 and 0x400000 on x86-64, the address at which TinyCC links a program. A header edit rebuilds the objects that include it. `make -C ports/doom clean`, with `ARCH=x86_64` for that tree, removes the architecture's output directory.

The build does not check the program against the kernel's loader. `build/tools/inspect/elfcheck`, which `make` builds, does:

```
build/tools/inspect/elfcheck build/ports/doom/doom
build/tools/inspect/elfcheck --arch x86_64 build/x86_64/ports/doom/doom
```

## Updating

No script brings in a new doomgeneric, since `src/` is a copy and not a pinned tarball. To take a later commit, copy over each file of `src/` the file of that name in upstream's `doomgeneric/`, make the three changes of Local changes again, and add to `DOOM_SRC` any new engine source the link needs; leave out what upstream adds for other platforms. Then build both architectures and run the tests below. `midifile.c`, `mus2mid.c` and `i_oplmusic.c` are adapted rather than copied, so a change in Chocolate Doom must be merged into them by hand; a new release of Nuked OPL3 replaces `opl3.c` and `opl3.h` with the change to `opl3.c`'s includes made again.

## Limits

* Saved games do not reach their slots ([doom(1)](../../man/man1/doom.1)). `remove` and `rename` in `include/stdio.h` and `mkdir` in `include/sys/stat.h` always fail, and the engine ignores the failed rename of the temporary file it wrote.
* Settings are not saved. `src/m_config.c` writes `default.cfg` only under `ORIGCODE`, which this build does not define, so a change made in the menu lasts until the game ends.
* No key strafes: the scan code table maps neither Alt nor doomgeneric's strafe keys, so only the mouse's right button strafes.
* The game never sleeps: its loop runs without pause, and `DG_SleepMs` spins on the clock, so the game keeps one processor busy for as long as it runs.
* Sound is mono at 11,025 Hz and 8 bits, and the PC speaker plays effects only, one at a time.
* `free` never returns memory to the heap, and `realloc` always takes new memory.
* The port is built only by the cross compiler. It has no `guest.mk`, so Rach2 does not build it on itself, and `tests/run selfbuild` does not build it.

## Tests

* `tests/run doom`, in the `full` tier on both architectures, boots the os-real system from a disk and needs the WAD at `ports/doom/doom1.wad`. Without the WAD the game must exit with status 255. Started from `/root`, it must still be running after ten seconds with `/fb` open and a zone of at least 10 MiB. A demo played to its end must exit with status 0, and a demo recorded through the port's `fopen` must have mode 644.
* `tests/run uefi` boots by OVMF with the console on the framebuffer. Doom, started over SSH, must put a picture of more than 64 colors on the screen, and the console's colors must return when it is killed.
* `tests/run host` runs `tests/host/doom_platform_hosttest.c`, which builds `doomgeneric_rach2.c` and `src/i_system.c` with the device calls simulated and checks the arguments, the zone's size, and the order of the callbacks, the closes and the report at exit; and `tests/host/oplmusic_hosttest.c`, which plays songs built in memory through the music player and checks the instrument table, the MIDI reader's parameters, tempo, loops and a song of two tracks.
* `sh tests/host/doom_platform_mutate.sh` compiles four faults into a copy of the platform layer and `src/i_system.c`; six of the ten faults of `sh tests/host/userland_mutate.sh` are in the music player and the MIDI reader; and `sh tests/system/doom_mutate.sh` puts two into the tree for `tests/run doom`. Each fault must fail its test, which must pass before it and after the source is restored.
* No test checks the sound, or the picture beyond its colors.

[tests/system/Readme.md](../../tests/system/Readme.md) and [tests/host/Readme.md](../../tests/host/Readme.md) describe each test. Run a suite with `tests/run NAME`, with `ARCH=x86_64` for the x86-64 build, and a mutation script with `sh` from the repository root.
