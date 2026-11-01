# Doom for Rach2

`ports/doom/` is Doom for Rach2: the doomgeneric engine in `src/`, and an in-house platform layer that connects it to the kernel. `doomgeneric_rach2.c` supplies the functions doomgeneric asks of a platform, `i_pcsound.c` is the engine's sound and music module, `crt0.c` is the program's entry point, its nine system calls and most of the C library the engine uses, and the 21 headers in `include/` declare that library in place of a system's. `i_oplmusic.c`, adapted from Chocolate Doom's file of that name, plays the game's music on the Nuked OPL3 emulator. The build makes `/bin/doom`, one static program for i686 or x86-64 that links no C library. The port exists so that the system has a game, and so that one well-known program uses the kernel's framebuffer, keyboard, mouse, sound and clock interfaces together.

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

`run.sh` takes `hda` or `speaker` in place of `sb16`, and after `make ARCH=x86_64`, `ARCH=x86_64 ./run.sh os-real sb16` boots the x86-64 system. Log in at the console and type `doom`. The game plays its sound through `/dev/dsp`, or on the PC speaker through a `/dev/console` descriptor of its own where `/dev/dsp` does not open, so effects and music play on the machine whether it starts from the kernel console, `term`'s shell, an SSH session or with standard input from `/dev/null`. It reads its keys from `/dev/console` whenever standard input is a terminal and the game is in its foreground. Started from an SSH session in its foreground, the game draws on the machine's screen and reads the machine's keyboard; with standard input from `/dev/null` it has no keys. The screen must be free or owned by an ancestor, as when the game starts inside `term`.

The controls come from the port's scan code table and the engine's default bindings:

* The arrow keys, or the keypad's 8, 4, 6 and 2, move and turn.
* Ctrl fires, Space opens doors and works switches, and either Shift runs.
* The digits select weapons, Tab shows the map, and Escape opens the menu.
* Moving the mouse turns and moves. Its left button fires, its right button makes the motion strafe, and its middle button moves forward.

[doom(1)](../../man/man1/doom.1) describes the command line, the default WAD, the size of the zone, how the game quits and what it cannot save.

## Sources and license

* `src/` is 176 of the 202 files of the `doomgeneric/` directory of [doomgeneric](https://github.com/ozkl/doomgeneric) at commit `fc601639494e`, each as it is there except the three that Local changes lists. It leaves out the build files, the other platforms' layers and sound modules, `gusconf.c`, `icon.c`, `mus2mid.c` and `sha1.c`. 170 of the files have the notice of the GNU General Public License, version 2 or later, and `config.h`, `doom.h`, `doomgeneric.c`, `doomgeneric.h`, `dummy.c` and `mus2mid.h` have none.
* `midifile.c`, `midifile.h`, `mus2mid.c` and `mus2mid.h` are adapted from Chocolate Doom's files of those names, which are under the GPL, version 2 or later. The copies here have the copyright lines (Simon Howard's, and for `mus2mid` also id Software's and Ben Ryves's) but not the license notice.
* `opl3.c` and `opl3.h` are Nuked OPL3 1.8, copyright 2013-2020 Nuke.YKT, under the GNU Lesser General Public License, version 2.1 or later.
* `i_oplmusic.c` is adapted from Chocolate Doom's `i_oplmusic.c`, which is under the GPL, version 2 or later; it has no copyright line or license notice.
* `sha1_fips.c` is in-house, FIPS 180-4's SHA-1 behind doomgeneric's `src/sha1.h`. It takes the place of doomgeneric's `sha1.c`, which is GnuPG's and under the GPL, version 3 or later, a license that cannot be combined with Rach2's version 2 only.
* The rest is in-house: `crt0.c`, `doomgeneric_rach2.c`, `i_pcsound.c`, `i_oplmusic.h`, the headers in `include/`, the `Makefile` and `fetch_wad.sh`. Rach2's own code is under the GNU General Public License, version 2 only ([COPYING](../../COPYING)). The files under version 2 or later and the Nuked OPL3 files under the LGPL, version 2.1 or later, combine with it, so `/bin/doom` as a whole is under version 2 only. Each upstream file keeps its own notice.
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

A `FILE` is a descriptor with no buffer, and `fopen` creates a file with mode 0666 less the umask. `printf` writes at most 1023 bytes a call, and `sscanf` reads signed and unsigned decimal, octal and hexadecimal integers, with C whitespace matching. The integer limits and the `ssize_t` and `off_t` types follow the architecture; `ll` output conversions read 64 bits on both. `__divdi3` and its four kin do the 64-bit division that i686 code calls and no libgcc supplies. The headers define the rest inline, some as stubs that always fail.

### The platform layer

`main` in `doomgeneric_rach2.c` appends `-iwad /demo/doom1.wad` to the arguments; the engine takes the first `-iwad`, so one the caller gives is used. It calls `doomgeneric_Create`, which runs `D_DoomMain` to the start of the game loop, and then `doomgeneric_Tick`, one pass of the loop, forever. The engine calls back:

* `DG_Init` reads `MemFree` from `/proc/meminfo` and, when it has more than 8 whole MiB, leaves the whole MiB less 8 in `DG_zone_mib` for `I_ZoneBase`. It opens `/fb` for reading and writing, which puts the display in mode 13h, and `/mouse` for reading, and records the time. When `/fb` does not open it writes `Failed to open framebuffer` to standard error, and the game runs without drawing.
* `DG_DrawFrame` sends the palette when the engine has changed it, writes the frame, and reads the keyboard and the mouse.
* `DG_GetTicksMs` is the milliseconds since `DG_Init`, from `gettimeofday`, and `DG_SleepMs` calls `gettimeofday` until the time has passed.
* `DG_GetKey` gives the engine the next queued key transition, reading the keyboard first when the queue is empty.

The port defines `exit`, so every way out of the game runs the same code: `I_Quit`, after a confirmed Quit or the end of a demo played with `-playdemo`; `I_Error`; and an exit callback that calls `exit` itself, as `D_Endoom` does. `exit` calls `DG_Shutdown`, which closes the mouse and `/fb` and then prints the report, and `_exit` ends the process with the status given: 0 from `I_Quit`, 255 from the `exit(-1)` of `I_Error`. The last close of `/fb` by its owner puts the display back in text mode, so the report appears on the text screen.

The report is one line on standard output, printed after the first frame has been written to `/fb`, every ten seconds while the game draws, and once more by `DG_Shutdown`; the tests wait for the first as the sign that the game has drawn:

```
doom: N frames in M ms, the longest L ms; music emulator E ms in C calls for S samples after V events, mixer X ms; zone Z MiB; dsp queue Q samples, the most H, period P
```

N is the frames drawn and M the time from the first to the last; L is the longest time between two frames; E is the time spent in the music player, which made C calls of the emulator for S stereo samples and ran V song events; X is the time spent mixing; and Z is the zone the engine allocated. The last part is there once the sound module has written to `/dev/dsp`: Q is the samples queued there after its last update, `GETODELAY`'s reading with what the update wrote, H the most after any update, and P the samples of the device's period.

### Display and input

The engine draws each frame as 64,000 palette indices (`CMAP256`, 320 by 200), and `DG_DrawFrame` writes them to `/fb` as they are. A changed palette goes first, as 256 RGB triples, through `FB_SETPALETTE`. What the kernel does with them, in mode 13h on a VGA or scaled on the framebuffer of a machine booted by UEFI, is in [the display Readme](../../rach2/sys/display/Readme.md) and [ioctl(2)](../../man/man2/ioctl.2).

Once it owns `/fb`, the game takes the keyboard when its standard input is a terminal and it is in that terminal's foreground (`take_keyboard`): it opens `/dev/console` without waiting for reads, which is the keyboard's even when standard input is the pty of `term`, keeps its keyboard mode and terminal settings, sets raw termios, so that no scan code is edited, echoed, mapped or taken as a signal, and puts the keyboard in K_RAW, which the console allows the screen's owner ([console(4)](../../man/man4/console.4)). The console then gives the set 1 scan codes of every key ([the terminal Readme](../../rach2/sys/tty/Readme.md)), and the game reads them a byte at a time after each frame. `DG_Shutdown` puts the mode and the termios back; a game killed before it can is restored by the console, which gives the screen's last owner back the state from before it took the screen. Started in the background or with standard input from `/dev/null`, as the machine cases start it, the game draws without keys and leaves the console as it was. `scancode_to_doomkey` maps the low seven bits of a code to a Doom key, and the top bit tells a release from a press; a code with no entry, such as Alt or the 0xE0 prefix, is dropped. The transitions wait in a ring of 32 entries, so 31 can be unread at once, and one that finds the ring full is dropped.

`/mouse` gives three-byte packets, a byte of buttons and signed X and Y motion (`struct mouse_packet` in `rach2/sys/mouse.h`), which the game posts to the engine as mouse events after each frame. The open fails when the machine has no mouse, and the game goes on with the keyboard alone.

### Sound

`i_pcsound.c` defines `DG_sound_module` and `DG_music_module`, the modules `src/i_sound.c` compiles in under `FEATURE_SOUND`. The sound module lists the PC speaker and the Sound Blaster as its devices, so the engine's default device, the Sound Blaster, selects it on every machine. At start it opens `/dev/dsp` ([dsp(4)](../../man/man4/dsp.4)) for writing without waiting, with OSS's requests from musl's `<sys/soundcard.h>`, and sets 11,025 Hz unsigned 8-bit mono, which the SB16 plays as it is and HDA converts; it then starts the music player. When the device does not open, as with no sound card or under `rach2.noaudio`, or does not take that format exactly, the module opens `/dev/console` for reading and writing on a descriptor of its own, independently of standard input and the keyboard descriptor, plays effects on the PC speaker through it, and the game has no music. If neither opens, the game runs without sound. Shutdown closes the descriptor, and the close of `/dev/dsp` waits for nothing, since the descriptor does not wait. The module leaves the system volume as `/dev/mixer` ([mixer(4)](../../man/man4/mixer.4)) holds it; the game's own sound and music volumes scale its samples.

With sample output, an effect is a `DS` lump, unsigned 8-bit samples after an 8-byte header, and eight channels play at once at the engine's volumes; the stereo separation is ignored. Each time the engine updates its sound, the module reads `SNDCTL_DSP_GETODELAY`, the samples queued ahead of the next one written, in the device's buffer and in the driver's, and `SNDCTL_DSP_GETOSPACE`, which gives the buffer's part, and makes what brings the buffer up to its level: two periods, or a tic and a period where that is more, so that the buffer outlasts a game that updates once a tic. The kernel's part, the mix's and the driver's, which the program cannot shorten, is under five periods of the device's on HDA, whose period is a period of the mix, 1024 frames at 48000 Hz or 235 of the game's samples, and under three on the SB16, whose period is 512 frames at 44100 Hz or 128 samples. What is heard is then at most about 155 ms behind the game on HDA and 75 ms on the SB16. When the time since the last update is longer than what was queued then, the device has played silence for the rest, and each effect skips the part of that time since it started, so that none plays late and one started after the silence keeps its beginning; the music goes on from where it was. The first write starts the device with `SNDCTL_DSP_POST`, which would otherwise wait for its driver's buffer to fill. The module makes the samples in chunks of at most 315, one tic: for each chunk it takes 49716/11025 times as many stereo samples from the music player, averages them down to the output rate in 8 bits, adds the channels, and limits each sum to a byte. What a write does not take is dropped. What the kernel does with the stream is in [the audio Readme](../../rach2/sys/audio/Readme.md).

On the PC speaker an effect is a `DP` lump: a 4-byte header, then one tone value for each 7 ms. A value from 1 to 127 is a frequency, 175 Hz times 2 to the power (value - 1) / 24, which `KIOCSOUND` sounds as the PIT's divisor, 1193182 divided by the frequency; 0 and values above 127 are silence. The speaker sounds one effect at a time, and a new effect ends the one playing.

### Music

The player in `i_oplmusic.c` starts only when the WAD's `GENMIDI` lump is whole: the header `#OPL_II#` and 175 instrument records, 128 melodic and 47 percussion. A song lump in MIDI form is read as it is, and any other is converted from MUS by `mus2mid.c`; `midifile.c` reads the MIDI from memory. The player walks up to 32 tracks, each with its own wait in samples until its next event, so a MUS song's one track and a format 1 file's tempo and note tracks play alike. It runs the events that are due, at most 1024 on a track before a sample is made, and then has the emulator make samples up to the nearest next event. The samples per tick come from the tempo by a 64-bit product, and are at least one. A looping song starts again at its end; one that reaches its end twice without making a sample stops and gives silence. Nuked OPL3 makes stereo 16-bit samples at 49,716 Hz, and the player uses nine of its voices.

## Building

From the repository root:

```
make ports/doom
make ports/doom ARCH=x86_64
```

The root rule runs `make -C ports/doom` after the tool check, and `make` builds the port before any image that stages it. The output is `build/ports/doom/doom`, ELF32 i386, or `build/x86_64/ports/doom/doom`, ELF64 x86-64, with the objects beside it.

The `Makefile` compiles `crt0.c`, the seven sources of `LOCAL_SRC` and the 79 of `DOOM_SRC`, which are all the `.c` files of `src/`, with the architecture's cross compiler ([`mk/toolchain.mk`](../../mk/toolchain.mk)), `-std=gnu99 -Os -ffreestanding`, and the headers of `include/` ahead of `src/` and the compiler's own, and musl's installed headers (`ARCH_MUSL`) after them, with `-idirafter`, for `<sys/soundcard.h>` and the Linux headers it includes. `include/endian.h` stands in for musl's, which `<linux/soundcard.h>` includes and whose `uint32_t` is not i686-elf-gcc's. The program links no musl, so the build needs musl's install, which `make` makes first. `CMAP256` makes the engine draw in palette indices, and `FEATURE_SOUND` compiles in the sound modules. It links with `-nostdlib -static` and the text at `ARCH_USER_TEXT` of [`mk/arch.mk`](../../mk/arch.mk): 0x08048000 on i686 and 0x400000 on x86-64, the address at which TinyCC links a program. A header edit rebuilds the objects that include it. `make -C ports/doom clean`, with `ARCH=x86_64` for that tree, removes the architecture's output directory.

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

* `tests/run doom`, in the `full` tier on both architectures, runs the case of [`tests/cases/machine.doom.json`](../../tests/cases/machine.doom.json), which boots a disk root of its own, staged by `krffs/manifests/doom-test.manifest` with the WAD from `ports/doom/doom1.wad`; a tree without the WAD fails the case's first assertion. Without the WAD the game must exit with status 255 and the engine's error. Started from `/root`, it must report its first frame, and then still be running, with `/fb` open and a zone of at least 10 MiB. A demo played to its end must exit with status 0 after the game has reported more than one frame, so a game that stops drawing fails however fast the machine is, and a demo recorded through the port's `fopen` must have mode 644.
* `tests/run uefi` boots by OVMF with the console on the framebuffer, and its `installed` case fails its assertion `wad` without the WAD. Doom, started over SSH, must report its first frame and put a picture of more than 64 colors on the screen, and the console's colors must return once the killed game has gone.
* The `doom_libc` check of `tests/run host`, [`tests/cases/host/host_doom.c`](../../tests/cases/host/host_doom.c), runs the port's runtime linked with a freestanding fixture on a guest kernel of each ABI, i686 and x86-64. It checks integer limits and types, 64-bit output, integer scanning, empty sorts, small `getcwd` buffers and update streams.
* `tests/run host` runs `tests/host/doom_platform_hosttest.c`, which builds `doomgeneric_rach2.c` and `src/i_system.c` with the device calls simulated and checks the arguments, the zone's size, and the order of the callbacks, the closes and the report at exit; `tests/host/oplmusic_hosttest.c`, which plays songs built in memory through the music player and checks the instrument table, the MIDI reader's parameters, tempo, loops and a song of two tracks; and `tests/host/doom_sha1_hosttest.c`, which checks `sha1_fips.c` against FIPS 180's test vectors.
* `tests/run doom` has two more cases, `sound-hda` and `sound-sb16`, which boot the same disk with QEMU's Intel HDA or Sound Blaster 16 on its `none` audio backend and play demo1 to its end. The game must exit with status 0, its last report must have the queue of `/dev/dsp` with samples queued, and the most queued after an update must be at most a tic, 315 samples, and seven periods.
* No test judges what the sound sounds like, or the picture beyond its colors.

[tests/system/Readme.md](../../tests/system/Readme.md) and [tests/host/Readme.md](../../tests/host/Readme.md) describe each test. Run a suite with `tests/run NAME`, with `ARCH=x86_64` for the x86-64 build.
