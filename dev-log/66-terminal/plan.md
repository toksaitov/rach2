# The agents' terminal

Coding agents such as Claude Code, Codex, Gemini CLI and agy start on Rach2 but cannot be used at its console: Enter does not select in their menus, many other keys send nothing or the wrong bytes, their glyphs print as `?`, and a wide character shifts the rest of its row. The keyboard knows only the US layout, and a USB keyboard does not repeat. Doom does not start on the installed Lenovo. This milestone gives Rach2 a keyboard with Linux's model, interfaces and tools and the layouts of the CIS countries, fixes the kernel console, boots BIOS machines on a framebuffer as UEFI ones are, and adds `term`, a terminal that runs in user space on the whole framebuffer, built on libtsm, where the agents draw in color as they do on a desktop terminal. The kernel console keeps `TERM=rach2`, which is true of it, so the agents draw without color there; color comes with `term`. The terminal research is `build/flow/coordinator/reports/Terminal emulator libraries for agents.md`; the keyboard research is `build/flow/coordinator/research_notes/Keyboard support/linux_keyboard.md`; Codex's review of both is `build/flow/coordinator/codex/keyboard-terminal-answer.md`.

## The owner's decisions

* The engine is libtsm 4.8.0 (github.com/kmscon/libtsm, MIT), the research's recommendation; the owner's words on 31 October: "I like it, use that one". libghostty-vt is looked at again when libghostty 1.0 is tagged. Codex, asked on 1 November whether the choice was good, kept it.
* `term` is the console's default: init starts it at boot on the console in place of the kernel console's login, and the kernel console comes back whenever `term` exits or fails. The owner's answer on 31 October: "Yes". The images people boot, the Lenovo's among them, carry the boot option that starts it; test images do not, so the machine cases keep their login on COM1 (task 28).
* The screen follows one rule on both architectures and both firmwares, agreed with the owner on 1 November: a framebuffer when the loader gives one, and the VGA text mode when it does not.

  | Firmware, mode | 32-bit kernel | 64-bit kernel | Screen | Terminal | Doom |
  |---|---|---|---|---|---|
  | BIOS, default | yes | yes | framebuffer from the video BIOS (VBE) | term | its 320x200 picture scaled onto the framebuffer |
  | BIOS, text (a menu entry, or no usable graphics mode) | yes | yes | VGA text, 80x25 | kernel console | the VGA's mode 13h |
  | UEFI | yes | yes | the firmware's framebuffer | term | scaled onto the framebuffer |

* The keyboard is professional and follows Linux, so that Linux's programs and tools port unchanged: layouts with a switch between two, configurable repeat, Caps Lock and Num Lock with their LEDs, dead keys where a layout needs them. The owner's words on 1 November: "similar to Linux (maybe even with the same interface for easier app porting work)", and "Do not overcomplicate".
* The keyboard's tools are kbd's `loadkeys`, `dumpkeys`, `setleds`, `kbdrate` and `showkey`, in place of BusyBox's `loadkmap`; the layouts are kbd's text keymaps generated from XKB's data (xkeyboard-config, MIT) by console-setup's `ckbcomp` in WSL on the build host; the set is the CIS countries' layouts, Kyrgyz first, with the common Western ones; the switch between two layouts and the Caps Lock tweaks are small maps loaded on top of a layout. The owner approved all four on 1 November ("Yes").

## Measured

* The Lenovo's framebuffer, from its kernel log on 31 October: RGB, 1366x768, 32 bits a pixel, pitch 5504, mapped write-combining; the console draws Unifont at 2x, 85x24 cells. A whole frame is 4,227,072 bytes.
* `/demo` on the Lenovo's installed root holds only `hello.txt`. `doom` there, run by ssh on 31 October, prints "IWAD file '/demo/doom1.wad' not found!" and exits 255, as `doom_scenario.c:135` expects. With the WAD copied to `/tmp` and `-iwad /tmp/doom1.wad`, it gets through every init step to "ST_Init: Init status bar." and runs until killed at 15 s.
* The keyboard today, checked against the code on 1 November:
  * One US layout in two fixed tables (`kbd.c:44`, `:77`); F1 to F12, Caps Lock, Num Lock and the keypad's digits map to 0 (`kbd.c:54-66`); no lock state and no LED (`input.c:185-215`, `evdev.c:445`).
  * Enter and keypad Enter send LF (`kbd.c:48`, `:81`; `input.c:225-230`); Shift+Tab sends HT; the arrows and editing keys send fixed sequences, so modifiers on them are lost (`input.c:66-99`); Ctrl with a non-letter sends the plain character (`input.c:134-140`); Backspace sends DEL.
  * One `shift_held`, one `ctrl_held` and one `alt_held` for both sides, so releasing one Shift clears Shift while the other is held (`input.c:186-207`).
  * A USB keyboard does not repeat: an unchanged report makes no event (`usb/hid.c:187`); a PS/2 keyboard repeats by its own typematic. USB Print Screen and Pause map to 0 (`usb/hid.c:60`).
  * The console ignores DECCKM and DECKPAM: private CSI sequences are swallowed and `ESC =` and `ESC >` have no handler (`kio.c:654`, `:727`).
  * `console_ioctl` answers TIOCLINUX, KIOCSOUND and the audio requests and none of Linux's KD* keyboard requests (`syscall.c:2085-2100`). BusyBox's `loadkmap`, `dumpkmap`, `kbd_mode`, `showkey` and `setkeycodes` are off (`vendor/busybox/rach2.config:373`).
  * Canonical erase removes one byte (`n_tty.c:142`), so it breaks a Cyrillic letter.
  * The owner of `/fb` reads set-1 scancodes from a 64-slot ring that drops codes when full (`kbd.c:8-24`, `syscall_fs.c:4520-4524`), but poll on the console descriptor watches the line discipline (`io/poll.c:87-93`), and the same keys still reach the discipline as type-ahead (`tty/Readme.md:99`).
  * `/fb` refuses a second process with EBUSY (`syscall_fs.c:2384`), so Doom started inside `term` cannot open it.
* libtsm 4.8.0, read from its tarball (SHA-256 `50811e9e94fc798eb5dcbaff20f81067c942ec47f329f91bbe4caeb35a61ff05`):
  * The Kitty push `CSI > 1 u`, pop `CSI < u`, set `CSI = 1 u` and query `CSI ? u` are ignored: the CSI collector drops `<` and `=` and `do_csi` has no case for `u` (tsm-vte.c:1019-1056, 2179). DECRQM is a FIXME (tsm-vte.c:2038-2044). The OSC buffer is 128 bytes (tsm-vte.c:118).
  * `tsm_screen_write` drops a character of width 0 and clips one of width 2 that starts in the last column (tsm-screen.c:1107-1125); `tsm_screen_draw2` draws an empty cell for a symbol of several code points (tsm-render.c), so `term` draws through `tsm_screen_draw`.
  * Its key encoder sends Shift+F1 as F11 and swaps the application keypad's divide and multiply against xterm (tsm-vte.c); it takes an `ascii` keysym beside the real one for Ctrl combinations (tsm-vte.c:2845-2880).
  * The LGPL hash table is `src/shared/shl-htable.c` and its header `shl-htable.h`, which has inline code; only `tsm-unicode.c` includes it (tsm-unicode.c:64). The library's C files are 8,194 lines.
* Linux's keyboard, from the keyboard research: up to 256 keymap tables of `NR_KEYS` entries, one per combination of the modifiers Shift, AltGr, Control, Alt, ShiftL, ShiftR, CtrlL and CtrlR; a layout switch is a lock on one of them (`AltGr_Lock` in kbd's Russian maps). The kernel's default accent table of 68 entries serves the dead keys of German, French, Spanish, Portuguese and US International. Linux's repeat defaults are 250 ms and 33 ms. On Linux, Caps Lock does not shift Cyrillic letters in a Unicode keymap. `keyboard.c` and `defkeymap.map` are GPL-2.0; Rach2's own code declares no license (Readme.md:63), so the translator is written to Linux's interface, not copied.
* kbd 2.10.0 is the current release (kernel.org, 25 May 2026), GPL-2.0-or-later. `loadkeys` uses KDGKBTYPE, KDGKBMODE, KDGETMODE, KDSKBENT, KDSKBMODE, KDSKBSENT and KDSKBDIACRUC (else KDSKBDIACR); `-b` and `-m` make no request. Its `getfd` finds the console through `/dev/tty` and then `/dev/console`; Rach2 has `/dev/console` and `/dev/tty` and no `/dev/tty0` (devfs.c:1588-1589).
* ckbcomp puts XKB's second group on ShiftL and its third and fourth on ShiftR, maps `ISO_Next_Group` to `ShiftL_Lock`, fills every cell a group leaves empty from the first group, so Ctrl+C sends ETX on a Cyrillic layer, and writes 128 tables, 64 KiB in the kernel. kbd's data has Kyrgyz, Kazakh (8-bit), Tajik, Tatar, Bashkir, Belarusian, Ukrainian, Georgian and Romanian maps and none for Uzbek, Azerbaijani, Armenian or Turkmen; XKB has all of them.
* The kernel console's font has the Kazakh and Kyrgyz letters, Ukrainian and Belarusian ones (vga_font.c:12-25), and not the Uzbek, Tajik, Tatar and Bashkir extras or the Armenian and Georgian scripts, which it draws as `?`. Unifont, which `term` draws with, has all of them.
* The loops' times, from GitHub's records of milestones 64 and 65, from a task's first take to its close: tasks of 3 to 7 subtasks of code took 10 to 60 minutes (#1438 21, #1445 55, #1452 41, #1460 25, #1466 31, #1472 23; #1529 21, #1537 11, #1555 21); fix tasks of 5 to 9 subtasks 5 to 15; the two tasks that rebuilt the GNU root about 100 (#1543, #1549); the release of milestone 65 about 12 hours, nearly all machine runs. A task's time followed its builds and boots, not its count of subtasks.
* Rach2's tty has what a terminal needs: raw mode and OPOST (n_tty.c:105-118), signal characters (n_tty.c:248), a pty whose master's TIOCSWINSZ sends SIGWINCH to the foreground group (pty.c:441-447). VTIME is ignored (tty/Readme.md:95).
* Python 3 for esctest2: 3.15.2 on the Mac, 3.14.4 in WSL on the testing host.
* GRUB's BIOS images carry no video module (`vendor/grub/manifest.txt:52-54`), and the menu sets `gfxpayload=text` on BIOS (`tools/image/mkdisk_grub.sh:277-285`). The console already draws on RGB framebuffers of 8 to 32 bits a pixel and on 8-bit indexed ones (`fbcon.c:208-212`).
* Not yet measured: whether TinyCC builds and links libtsm for i686 and x86_64; whether kbd's tarball builds against Rach2's musl without flex and bison; whether the research assistants' accounts on the testing host can run Docker; the time of a whole-screen write and of a scroll on the Lenovo; the 32-bit kernel on the Lenovo under UEFI. Each is the first subtask of the task that needs it.

## The split

* Nine loops take tasks: five on the testing host in the research assistants' accounts, and on the Mac the owner's Codex loop and the manager's three. An `owner-only` task goes to the last four; the board cannot tell which account has Docker, so the two tasks that need it are `owner-only`, and the Mac runs them (`/usr/local/bin/docker`).
* A task has 4 to 7 subtasks, owns its files, and merges once. The measured times put such a task at 10 to 60 minutes of one loop, with its builds and boots taking most of it. Work is split where its files part, since the milestone's time follows the longest chain of waits, not the number of tasks.
* A task is done when its code is written and compiled and its tests are written, listed and compiled. As AGENTS.md says, host tests and suites run at the end of the milestone, not in its tasks; a worker builds and boots the system only to see what reading the code cannot settle. So a provider merges without waiting for its consumers, and two tasks join the pieces: task 25 the keyboard, task 28 the terminal.
* Task 1 lands the kernel's interfaces and `/fb`'s header for programs first, with typed stubs and today's input path untouched, so that eight tasks build against them instead of against each other's code. Task 2 lands libtsm, with compiling stubs of the calls task 21 adds, and `term`'s skeleton, on which six tasks wait.
* Two tasks may edit one file at once only on the boundary the task names: different functions, or different sections of a document. Lists take lines from any task: `rach2/sources.mk`, the manifests and the images' prerequisites (`Makefile:401`, `mk/gnu-image.mk:31`), the Makefile's stamps and cases, `tests/cases/host.host.json`, which lists every host test (tests/host/Readme.md:25), and the LICENSES table of `man/programs.sh`. The second task to merge resolves its rebase. libtsm's patches are no such list: a patch made on the pristine tarball fails at fuzz 0 when another patch changed its context, so the patches to `tsm-vte.c` come one task after another, and each is made on the series before it.
* esctest2 is a suite: task 24 brings it in with its runner, and the end run runs it with the host tests, so its failures join the one batch of fixes and no task edits `term` or the patches beside the tasks that own them. `term/Readme.md` is read whole in that batch too, after every task has written its section.
* The long parts start first: kbd's configure against musl (task 4), the layouts' generation (task 5) and GRUB's regeneration (task 6) in Docker, and libtsm (task 2). No task rebuilds the GNU root or runs an agent in QEMU: workers build musl images, recorded agent output (task 12) stands in for the agents, and only task 29 builds a GNU image, from the retained build as the release does. Each new machine case boots once and checks all it can, since every case runs again on four machines in every release.
* The longest chains are four tasks deep before the Lenovo: 2, 18, 22, 28 and 1, 14, 22, 28 and 1, 15, 25, 28.

| Task | Waits on | Accounts | Its long part |
|---|---|---|---|
| 1. The kernel's interfaces | | any | |
| 2. libtsm and term's skeleton | | any | |
| 3. Doom's WAD in the Lenovo's build | | `owner-only` | |
| 4. kbd's tools and the settings at boot | | any | kbd's configure against musl |
| 5. The layouts, made from XKB | | `owner-only` | generating and checking each layout |
| 6. A framebuffer under BIOS | | `owner-only` | GRUB's regeneration |
| 7. Unifont | | any | |
| 8. Timed reads | | any | |
| 9. The key table | | any | xterm's bytes for every key |
| 10. The keyboard's devices, state and repeat | 1 | any | |
| 11. The keyboard's LEDs | 1 | any | |
| 12. The console's glyphs, widths, modes and erase | 1 | any | recording the agents' output |
| 13. /fb at the native mode | 1 | any | |
| 14. The keymap translator | 1 | any | |
| 15. Linux's keyboard requests | 1 | any | |
| 16. term: the pty, the screen and the loop | 1, 2 | any | |
| 17. term: drawing | 1, 2, 7 | any | |
| 18. libtsm: the keyboard protocols | 2 | any | the Kitty encoder |
| 19. libtsm: wide and combining characters | 2 | any | |
| 20. libtsm built on Rach2 | 2 | any | the TinyCC builds in QEMU |
| 21. libtsm: probes, modes and strings | 18 | any | |
| 22. term: keys, scrollback and selection | 4, 9, 14, 16, 18 | any | |
| 23. /fb passed down and back | 13, 15 | any | |
| 24. esctest2 and the Unicode tests | 12, 16, 18, 19, 21 | any | |
| 25. The keyboard joined and tested | 4, 5, 8, 9, 10, 11, 12, 14, 15 | any | the PS/2 and USB cases |
| 26. The SeaBIOS cases | 6 | any | four boots |
| 27. term built on Rach2 | 13, 16, 17, 19, 20, 21, 22, 25 | any | the TinyCC builds in QEMU |
| 28. term as the console | 4, 6, 16, 17, 21, 22, 23, 25 | any | the UEFI and BIOS cases |
| 29. The Lenovo | all | `owner-only` | the trial boots and the owner's checks |

The board's order is the table's. Tasks 1 to 9 are Ready at once, one for each loop.

## Tasks

### 1. The kernel's interfaces

One short branch lands the declarations later tasks build against, with typed stubs, and leaves today's input path as it is.

* Subtasks:
  * `rach2/sys/kd.h` and `kd.c`: `kd_key_event(device, keycode, value)`, the hook the keyboard's stream will call, empty until task 25 joins it; `kd_ioctl()`, ENOTTY for now, called from `console_ioctl` (syscall.c:2085-2100); `kd_state_save()` and `kd_state_restore()`, declared, which keep the keyboard's mode, locks, LEDs and repeat and the console's termios for the screen's owners; their lines in `rach2/sources.mk`;
  * `rach2/sys/keymap.h`: the translator's interface, with no kernel calls, so that the kernel, `term` and a host test compile it: its state; the tables, 256 of 256 entries in Linux's encoding (`K(t,v)`, `U ^ 0xF000`); the function strings; the accent table; `keymap_key()`, which takes a keycode and a press, repeat or release and gives text, a function string, a special key with its modifiers, or a lock change; `keymap_encode()`, which gives xterm's bytes for a result under cursor and keypad modes passed in; `keymap.c` with a typed definition of each, returning nothing;
  * `input.h`: `input_set_repeat(delay_ms, period_ms)`, `input_get_repeat()`, `input_cancel_repeat()` and `input_set_leds(mask)`, empty in `input.c`; the hook through which the PS/2 keyboard's interrupt hands a command's reply (ACK, RESEND and the like) to the LED code before the byte reaches the key stream, today dropped in `input_set1_event` (input.c:266); `kio.h`: `kio_key_modes()`, which returns 0 in `kio.c`;
  * `rach2/sys/display/fb.h`: the native mode's ioctl numbers; the structure that reports width, height, pitch, bits a pixel, the kind (RGB or indexed) and each channel's offset and width; the event that tells an owner it lost or regained the screen; the same numbers and structures in a header for programs, staged with the musl headers, which Doom's port and `term` include;
  * each interface's contract in its header's comments.
* Files: `rach2/sys/kd.c`, `kd.h`, `keymap.c`, `keymap.h`, their lines in `rach2/sources.mk`, the dispatch line in `rach2/sys/syscall.c`, the stubs in `input.c`, `input.h`, `kio.c`, `kio.h`, `rach2/sys/display/fb.h`, the header for programs and its staging.
* Starts at once, first on the board.
* Done: the tree builds for both architectures, nothing behaves differently, and every header states its contract.

### 2. libtsm and term's skeleton

libtsm 4.8.0 comes in as a pinned tarball and builds, with no LGPL file, for both architectures and the host, and `term` exists as an empty program.

* Subtasks:
  * `vendor/libtsm/` with the tarball, its digest, `build_libtsm.sh`, a patch series applied with `patch -F 0 --forward` as `vendor/pdpmake` does, and a Readme;
  * a small hash table of Rach2's own in place of both `shl-htable.c` and `shl-htable.h`;
  * a static `libtsm.a` and its headers for each architecture, built with the cross compiler behind a stamp, and for the host;
  * a first patch declaring the calls task 21 adds (mode 2026's hold, focus events, the clipboard's callback), with stubs that compile, so that `term` builds against them before task 21 fills them in;
  * `term/term.h` with the program's parts and the calls between them (pty, screen, draw, font, keys, select, host), each in a section of its own; files `term/main.c`, `pty.c`, `screen.c`, `draw.c`, `font.c`, `keys.c`, `select.c` and `host.c` that compile; `mk/term.mk`, which the Makefile includes, and `term/sources.mk`; `term -V` prints libtsm's version; the first lines of `term/Readme.md`, with a section named for each later task;
  * libtsm and its wcwidth in the LICENSES table of `man/programs.sh`.
* Files: `vendor/libtsm/`, the Makefile's libtsm stamp and its include of `mk/term.mk`, `mk/term.mk`, `term/`, `man/programs.sh`.
* Starts at once, second on the board.
* Done: `term -V` builds for i686, x86_64 and the host, and no LGPL file is compiled.

### 3. Doom's WAD in the Lenovo's build

The release builds the Lenovo's root with the WAD.

* The cause found: the release builds the Lenovo's root in a copy synced from a fresh worktree (release.sh, its build step), which has no `ports/doom/doom1.wad`, since Git does not track it, and the manifests stage it as `optional-file` (os-real-gnu.manifest:230). The full run's workflow copies `/etc/rach2/doom1.wad` into its copy (`.github/workflows/full.yml`); the Lenovo's build does not.
* Subtasks: in the release's remote build command, copy `/etc/rach2/doom1.wad` into the copy's `ports/doom/` and check it with `ports/doom/fetch_wad.sh` before `make machine`, and stop without it; after the build, check that `os-real-gnu.ext2` holds `/demo/doom1.wad` with the vendored e2fsprogs' `debugfs`, and stop without it; `tools/Readme.md`.
* Files: `tools/flow/release.sh`, `tools/Readme.md`.
* A change to the process: it goes to master by fast-forward once the copy step works on the testing host. The task builds no GNU root; this milestone's release is the first to run the check. `owner-only`, since the release runs from the owner's account.
* Done: release.sh stops without the WAD, and copies and checks it otherwise; task 29 plays the game.

### 4. kbd's tools and the settings at boot

kbd's tools run on Rach2, and init applies the keyboard's settings before the console's login.

* Subtasks:
  * first, whether kbd 2.10.0's tarball carries its generated lexer and parser, so that neither flex nor bison is needed, recorded here; then `vendor/kbd/` with the tarball (kernel.org), its digest and `build_kbd.sh`, configured as `build_dropbear.sh` is (build and host triples, the compiler wrapper, the cross binutils; build_dropbear.sh:108-117), with static libraries, NLS, vlock, kbd's tests and XKB off, and no host compression library found; `loadkeys`, `dumpkeys`, `setleds`, `kbdrate`, `showkey` and `kbd_mode` for both architectures behind a stamp; the data directory `/usr/share/kbd`, so the maps are found in `/usr/share/kbd/keymaps`;
  * `loadkeys -u` switches a console that is not in K_UNICODE to it and back while it loads; under `term` that breaks `term`'s input for the load. Since task 15 takes Unicode entries in every mode, a patch in `vendor/kbd/patches/` keeps `loadkeys` from switching a console in K_RAW or K_MEDIUMRAW, made after reading `loadkeys.c` and named in the Readme;
  * `linux/kd.h` and `linux/keyboard.h`, with Linux's numbers and structures, in the musl header patch;
  * the six programs in the manifests that stage `dropbearmulti` and in the images' prerequisites; their pages under `man/`, from kbd's own; kbd in the LICENSES table;
  * `/etc/vconsole.conf`: systemd's `KEYMAP=` and `KEYMAP_TOGGLE=`, and Rach2's own `KEYMAP_OPTIONS=` (such as `caps-ctrl`), `REPEAT_DELAY=`, `REPEAT_RATE=` and `NUMLOCK=`, which the page names as Rach2's; init reads the assignments without a shell and, before the console's login, runs `loadkeys -u` with the maps, then `kbdrate` and `setleds`, each with its standard input on `/dev/console`, since `setleds` and `kbdrate` use descriptor 0; after any failure it loads `us` again, since a failed load leaves some entries changed; a host test of the reading; `man/man5/vconsole.conf.5` and its paragraph in the init section of `utils/Readme.md`.
* Files: `vendor/kbd/`, the Makefile's kbd stamp, the musl header patch, the manifests' lines, the pages, `man/programs.sh`, the reading of `/etc/vconsole.conf` in `utils/init.c`, its paragraph in `utils/Readme.md`, `man/man5/vconsole.conf.5`.
* Starts at once.
* Done: the six programs build for both architectures and are staged, `dumpkeys -h` runs on Rach2 in QEMU, and the host test is written. Task 25 loads a layout with them.

### 5. The layouts, made from XKB

The CIS countries' layouts and the common Western ones, as kbd's text keymaps, generated from XKB's data on the build host.

* Subtasks:
  * `vendor/keymaps/regen_keymaps.sh`, run as `vendor/grub/regen_grub.sh` is, in the pinned Ubuntu container at a dated snapshot: console-setup's `ckbcomp` on xkeyboard-config's data writes each layout paired with US as its first layout; the script then puts Caps Lock back as `Caps_Lock` where ckbcomp put `CtrlL_Lock`, since task 14's rule shifts Cyrillic and the LED follows Caps Lock, and replaces ckbcomp's `strings as usual`, Linux's console strings, with an include of xterm's function strings that every map shares; kbd's `loadkeys -m`, which writes the tables, the strings and the accents, checks each map; a manifest of the inputs' versions and digests and of each output's digest; the maps committed and changed only through the script;
  * the layouts: Kyrgyz (`kg`), Kazakh (`kz`), Russian (`ru`), Ukrainian (`ua`), Belarusian (`by`), Uzbek (`uz` and `uz(latin)`), Tajik (`tj`), Tatar (`ru(tt)`), Bashkir (`ru(bak)`), Azerbaijani (`az`), Turkmen (`tm`), Romanian of Moldova (`md`), Armenian (`am`), Georgian (`ge`); US, British (`gb`), German, French, US International, Dvorak and Colemak;
  * maps loaded on top of a layout: the switch to the second layout on `ShiftL_Lock`, the lock ckbcomp gives it, by Alt+Shift (`alt-shift`, the default), Ctrl+Shift, Caps Lock (with Shift+Caps Lock as Caps Lock), Right Alt, and Super+Space; Caps Lock as Ctrl (`caps-ctrl`) and as Escape (`caps-escape`); each checked from both layouts and with its two keys in either order, and changing no other table;
  * the maps staged under `/usr/share/kbd/keymaps` by the manifests that stage kbd, and the images' prerequisites;
  * `vendor/keymaps/Readme.md`: the list, the switch keys, which letters the kernel console's font lacks, and the sources: xkeyboard-config's MIT notices for the data, ckbcomp as a GPL-2.0-or-later tool run on the build host, and where ckbcomp's own compose and remapping tables come from.
* Files: `vendor/keymaps/`, the manifests' lines, its line in `man/programs.sh`.
* Starts at once. `owner-only`, for Docker on the Mac.
* Done: every map and add-on passes `loadkeys -m` in the container, and every layout's letters match its XKB symbols file.

### 6. A framebuffer under BIOS

GRUB gives a BIOS machine a framebuffer, as UEFI does, and the text mode stays as the fallback.

* Subtasks:
  * GRUB's BIOS images rebuilt by `vendor/grub/regen_grub.sh` with `vbe`, `video`, `video_fb` and `all_video`, and with `multiboot2` where an image lacks it, and reviewed as the images always are;
  * the MBR disk boots by Multiboot 1 (`mkdisk_grub.sh:403-407`), whose header asks for no video (`arch/i686/loader.s:15-19`, `arch/x86_64/loader.s:30-35`), so GRUB gives it text whatever the menu says: either both headers ask for a linear framebuffer with no preferred size, or the MBR disk boots by Multiboot 2, whichever boots both architectures;
  * the menu (`tools/image/mkdisk_grub.sh`) on BIOS sets a graphics mode for the kernel explicitly, not `keep` from a text terminal: `gfxpayload` set to a mode list that ends in `auto`, or `gfxmode=auto` with `terminal_output gfxterm` and `gfxpayload=keep`, whichever gives the panel's own mode under SeaBIOS, with text as the fallback; and it gains an entry that starts the same kernel in text mode;
  * the kernel's boot log names the mode it got;
  * `vendor/grub/Readme.md`, `tools/image/Readme.md`, and the framebuffer paragraph of `rach2/sys/display/Readme.md`.
* Files: `vendor/grub/` (through its script), both `loader.s`, `tools/image/mkdisk_grub.sh`, `tools/image/Readme.md`, that paragraph.
* Starts at once. `owner-only`, for Docker on the Mac.
* Done: each architecture boots under SeaBIOS on a framebuffer by default, by its boot log, and in text from its menu entry.

### 7. Unifont

GNU Unifont 18.0.01 comes in for `term`, converted at build time.

* Subtasks: `vendor/unifont/` with `unifont_all-18.0.01.hex.gz`, the file `tools/font/mkfont.sh` verifies, and its digest; a converter run at build time that writes a packed font of every glyph at 8 and 16 pixels wide, behind a stamp, from `mk/unifont.mk`; the packed format in a header that `term/font.c` reads; Unifont in the LICENSES table; `vendor/unifont/Readme.md`; a host test that reads a Latin, a Cyrillic, an Armenian, a CJK and a braille glyph.
* Files: `vendor/unifont/`, the converter, `mk/unifont.mk` and its include in the Makefile, the header, `man/programs.sh`, the host test.
* Starts at once.
* Done: the packed font builds, and the host test is written.

### 8. Timed reads

The line discipline honours VMIN and VTIME, which kbd's `showkey` sets (VMIN 18, VTIME 1) and Rach2 ignores (n_tty.c:307, tty/Readme.md:95).

* Subtasks: the four cases of VMIN and VTIME in a noncanonical read, by POSIX, on the console and on a pty: `tty_read` (tty.c:187) owns the blocking wait and arms and clears its deadline as the wait queue's contract says (wait_queue.h:69), and the readiness it shares with poll (`n_tty_readable`, n_tty.c:307) splits so poll is unchanged; a host test of each case by ticks; the Limits of `rach2/sys/tty/Readme.md` and `man/man4/console.4` corrected.
* Files: `tty_read` in `rach2/sys/tty/tty.c`, the readiness helpers in `n_tty.c`, their header contracts, its host test, those lines.
* Starts at once.
* Done: the code and the host test are written; `showkey` returns after a key in task 25's case.

### 9. The key table

One table of every key with xterm's bytes, which both the console and `term` must match.

* Subtasks: the table's form and its reader; the letters, digits and punctuation with every combination of Shift, Ctrl and Alt, both sides; the function keys, the cursor keys and the editing keys with their modifiers; the keypad with Num Lock on and off and in application mode; the line discipline's part, raw and cooked Enter and ISIG on and off; the `rach2` terminfo entry (`vendor/gnu/rach2.ti`) lists every key the console sends, checked with `tic` alone.
* xterm's version and resources are fixed in the table's header, and the bytes come from xterm's `ctlseqs` and its `input.c`.
* Files: the table and its reader under `tests/host/`, `vendor/gnu/rach2.ti`.
* Starts at once.
* Done: the table covers every key of a 105-key keyboard; task 25 runs it through the translator and task 22 through `term`.

### 10. The keyboard's devices, state and repeat

One engine in the kernel takes every keyboard's presses and releases as Linux keycodes and keeps their state.

* Subtasks:
  * PS/2 and USB events become one stream of (device, Linux keycode, press, repeat or release) into `kd_key_event()`; the keyboard's interrupt (`kbd_irq`, ps2kbd.c:53) hands a command's reply to task 1's hook and every other byte to the stream; the set-1 decoding keeps its prefixes per device; Rach2's set-1 to keycode table (`input_set1_event`, input.c:253-276) is the one map; USB gains Print Screen and Pause; the raw set-1 bytes are kept as a view, for K_RAW; today's translation (`input_key`, input.c:169-238) stays fed with set-1 bytes from the PS/2 interrupt and from `hid.c` (hid.c:160-171) as it is until task 25 replaces it;
  * held keys per device and per side, from which Shift, Ctrl, Alt, AltGr and Super are derived, so a release of one side leaves the other held;
  * repeat in software for every keyboard, in one place: 250 ms, then every 33 ms, by deadlines that accumulate so the 10 ms tick (apictimer.c:37) does not stretch 33 ms to 40; a PS/2 make code for a key already down is dropped, so typematic needs no programming; no repeat of modifiers and locks; repeat stops at release, at disconnect and on `input_cancel_repeat()`; `input_set_repeat()` and `input_get_repeat()`;
  * evdev produces EV_REP events and advertises EV_REP and EV_LED (evdev.c:421-512, its capability bits and events; its requests are task 15's);
  * the kernel's debug shell ends a line on CR as well as LF at the keyboard (`shell_input_char`, kernel.c:518-530), as it does on serial;
  * host tests of the state across both sides and two keyboards, of repeat by ticks and counts, and of a full queue.
* Files: `rach2/sys/input.c`, `input.h`, `rach2/sys/kbd.c`, `kbd.h`, the decoding in `rach2/sys/arch/x86/ps2kbd.c`, the reports in `rach2/sys/usb/hid.c`, `hid.h`, the capability bits and events of `rach2/sys/evdev.c`, the debug shell's input in `rach2/kernel.c`, their host tests.
* Waits on task 1.
* Done: on QEMU's PS/2 and USB keyboards a held key repeats at the set rate and both Shift keys work alone and together, and the host tests are written.

### 11. The keyboard's LEDs

The LEDs follow the locks on PS/2 and USB keyboards.

* Subtasks: the 8042's write helpers, now static in `ps2mouse.c` (`ps2_wait_input`, ps2mouse.c:74), shared with the keyboard; PS/2 command 0xED and its byte, acknowledged through task 1's reply hook, from a deferred context and spaced 50 ms apart; USB SET_REPORT Output (bRequest 0x09, wValue 0x0200, one byte) through a control transfer with an OUT data stage, which `usb/core.c` lacks (`control_out`, core.c:428, sends no data); `input_set_leds()` sending to every keyboard, and again after one is connected; host tests of the requests.
* Files: `rach2/sys/arch/x86/ps2mouse.c` and a shared 8042 file, the LED command in `ps2kbd.c`, `rach2/sys/usb/core.c`, the output report in `rach2/sys/usb/hid.c`, the LED part of `input.c`, their host tests.
* Waits on task 1.
* Done: in QEMU the LED requests reach both keyboards, by QEMU's monitor, and the host tests are written.

### 12. The console's glyphs, widths, modes and erase

The kernel console stands in for the glyphs the agents draw, gives a wide character two cells, draws a combining mark in no cell, keeps the cursor and keypad modes, and erases a whole UTF-8 character.

* Today a character with no stand-in is drawn as `?` (`CHAR_NON_PRINTABLE`, kio.h:40), every code point takes one cell (`vt_utf8`, kio.c:625-647), the stand-in table's ranges are 16 bits wide, and canonical erase removes one byte (n_tty.c:142).
* Subtasks:
  * Claude Code's output recorded in WSL with util-linux's `script`: its start screen, a menu, a reply with emoji and a spinner, under `TERM=rach2` and `TERM=xterm-256color`, kept under `tests/fixtures/agents/` for this task and task 24; Codex is signed in only in the owner's accounts, so task 29 looks at it and at Gemini CLI;
  * stand-ins in `vt_near_table` (kio.c:94-108), each the nearest glyph the font has: U+23FA, U+25CF and U+25CB as a bullet; the spinners U+2722 to U+273D as `*`; braille U+2800 to U+28FF as `.`; U+276F and U+203A as `>`; U+23BF as a box corner; the blocks U+2580 to U+259F as the nearest block; check marks and crosses as `v` and `x`; arrows as the font's arrows or ASCII; and any other the recordings show;
  * a width table, `rach2/sys/wcwidth.c`, ported in C89 from libtsm 4.8.0's `external/wcwidth/wcwidth.c` (MIT) with its notice, so that the console and `term` agree; a character of width 2 takes two cells, its glyph or `?` and a blank, and wraps first when it would start in the last column; a character of width 0 takes no cell;
  * DECCKM (`CSI ? 1 h/l`) and DECKPAM and DECKPNM (`ESC =`, `ESC >`) kept, reset by `ESC c`, and given by `kio_key_modes()`;
  * `IUTF8` in the line discipline: canonical erase removes a whole UTF-8 character and rubs out its width;
  * host tests of the stand-ins, the widths of the agents' glyphs, CJK and emoji, the modes and the erase, and `fbcon_hosttest.c`, which includes `kio.c` (fbcon_hosttest.c:56), built with the width table; the console's section of `rach2/sys/display/Readme.md` and the fallback and modes in `man/man4/console.4` (console.4:116).
* Files: `rach2/sys/kio.c`, `kio.h`, `rach2/sys/wcwidth.c` and its line in `rach2/sources.mk`, the erase in `rach2/sys/tty/n_tty.c`, `tests/fixtures/agents/`, the host tests, those sections.
* Waits on task 1.
* Done: the `TERM=rach2` recordings played with `cat` on the console in QEMU draw no `?` in the agents' own interface and no row shifted, and the host tests are written.

### 13. /fb at the native mode

A program can take the framebuffer at its native mode and draw on it with `pwrite`.

* Today `/fb` has only mode 13h, 320x200 at one byte a pixel, which `fbcon_gfx_frame` scales onto the screen; it has `write` of 64000 bytes and three ioctls, and no mmap or pwrite (`display/Readme.md`, syscall_fs.c:765-780, syscall.c:2169-2198; `ksys_pwrite64` refuses a descriptor without `ops`, syscall_fs.c:9197-9199). `write` checks the graphics mode, not the writer (display/Readme.md:113). `open_device_name` ignores every flag but O_DIRECTORY, so `/fb` loses O_CLOEXEC (syscall_fs.c:2460-2466).
* Subtasks:
  * the ioctl that reports the framebuffer in task 1's structure, refused with ENODEV on a text console, and the ioctl that enters the native mode, for RGB framebuffers of 15, 16, 24 and 32 bits, ENODEV on an indexed one; after it the console draws nothing until the owner gives it back, closes `/fb` or exits (`exit_release_session`, syscall_proc.c:90-92, releases it and the console redraws);
  * `pwrite`, and `lseek` with `write`, of pixel rows at byte offsets, through the descriptor's operations; O_CLOEXEC kept;
  * `write`, `pwrite` and the ioctls refused with EBUSY to any process but the owner, so that a child that inherits the descriptor cannot draw;
  * host tests of the ioctls' arguments, the offsets' bounds and the owner's check across fork;
  * `rach2/sys/display/Readme.md`, a `/fb` subsection of `man/man4/console.4`, moved out of Blanking (console.4:144-150), the framebuffer requests of `man/man2/ioctl.2` (ioctl.2:46), `man/man2/open.2` (397, 461).
* Files: `rach2/sys/display/vga_gfx.c`, `rach2/sys/display/fbcon.c`, `rach2/sys/display/fb.h`, the `/fb` parts of `rach2/sys/syscall_fs.c` and `rach2/sys/syscall.c`, their host tests, those sections.
* Waits on task 1.
* Done: a program in QEMU fills the screen at its native depth with `pwrite`, and the console redraws when it exits; the host tests are written.

### 14. The keymap translator

One C89 file of pure logic turns keys into text and sequences with Linux's keymaps, for the kernel console and for `term`.

* Subtasks:
  * `rach2/sys/keymap.c`, written for Rach2 to Linux's interface (`linux/keyboard.h`, `linux/kd.h`, `keymaps(5)`), not copied from `keyboard.c` or `defkeymap.map`: the shift state, the locks (KT_LOCK) and sticky locks (KT_SLOCK), the table index with the ShiftL and ShiftR weights the layouts use, the pending accent, the Compose key, and the results of `keymap.h`;
  * the default tables: US, from today's tables in Linux's encoding, with Enter as CR and function strings with xterm's sequences; the default accent table, written from X11's compose data (MIT) with its notice; Caps Lock shifts a Unicode letter whose Shift entry is its other case, so Cyrillic capitals work, which Linux's console does not do;
  * `keymap_encode()`: xterm's bytes for every key under the modes passed in, with `CSI 1;m` for modifiers on the cursor, editing and function keys, Shift+Tab as `CSI Z`, Alt as an ESC prefix, Ctrl with `@ [ \ ] ^ _` and Space as their control codes;
  * a host test with maps loaded from text in the test: German and French dead keys, compose cancelled, Russian Ctrl+C on a two-layout map switched by `ShiftL_Lock`, Dvorak and Colemak letters, Caps Lock on Cyrillic, sticky locks;
  * the translator in the kernel's build and the host test's, both word sizes.
* Files: `rach2/sys/keymap.c`, `keymap.h`, the host test.
* Waits on task 1.
* Done: the code and the host test are written and compile on both word sizes.

### 15. Linux's keyboard requests

The console answers Linux's keyboard requests, with Linux's numbers, structures, errors and permissions.

* Subtasks:
  * `kd.c`: KDGKBTYPE (KB_101), KDGKBMODE and KDSKBMODE with K_UNICODE as the default and K_XLATE, K_RAW, K_MEDIUMRAW and K_OFF, KDGKBENT and KDSKBENT (with K_HOLE and K_NOSUCHMAP, which frees a table), KDGKBSENT and KDSKBSENT with their bounds, KDGKBDIACR, KDSKBDIACR, KDGKBDIACRUC and KDSKBDIACRUC, KDKBDREP, KDGETLED and KDSETLED, KDGKBLED and KDSKBLED, KDGKBMETA and KDSKBMETA, KDGETMODE; the tables' storage, allocated on demand;
  * two departures from Linux, both documented: KDGKBENT returns Unicode entries in every mode but K_XLATE, and KDSKBENT takes them in every mode, so `loadkeys` works under `term`;
  * permissions as Linux's: the mode and LED requests to the console's controlling session or the screen's owner, or root; the tables, strings and accents to root; every request refused on a pty;
  * in K_RAW and K_MEDIUMRAW the kernel's translator and lock keys are off and the key bytes, the raw set-1 view in K_RAW, go through the line discipline, as on Linux, so poll and nonblocking reads work and the console's shortcuts and type-ahead stop; in those modes and K_OFF, COM1's bytes do not enter the console's input; `kd_state_save()` and `kd_state_restore()` keep the mode, the locks, the LEDs and the repeat times;
  * EVIOCGREP and EVIOCSREP in evdev's requests, through `input_set_repeat()`; EVIOCSREP writes, so the gate that admits only reading requests (evdev.c:513) changes;
  * host tests of each request's arguments, errors and permissions, of the modes and of the state's save and restore; `rach2/sys/tty/Readme.md` ("Keystrokes"), the keyboard section of `man/man4/console.4`, the terminal requests of `man/man2/ioctl.2` (ioctl.2:262).
* Files: `rach2/sys/kd.c`, `kd.h`, the requests in `rach2/sys/evdev.c`, the console's input routing, `tty_input_char` in `rach2/sys/tty/tty.c` (tty.h:133) and `console_pump`, and nothing of `n_tty.c` or `tty_read`, which are tasks 8's and 12's, their host tests, those sections.
* Waits on task 1.
* Done: the code and the host tests are written; task 25 joins the requests to the translator.

### 16. term: the pty, the screen and the loop

`term` runs a login or a shell on a pty and keeps libtsm's screen.

* Subtasks:
  * the loop in `term/main.c`, which owns every dispatch: poll on the master, the console and `/fb`; the drawing and the keys are called through `term.h`; input served between bursts of output, so typing stays prompt; quit when the child exits, giving the screen and the keyboard back to the console;
  * the pty in `term/pty.c`: open `/dev/ptmx`, fork, and in the child `setsid`, open the slave, `TIOCSCTTY`, set the size with TIOCSWINSZ on the master, which sends SIGWINCH itself (pty.c:441-447), and exec with `TERM=xterm-256color` and `COLORTERM=truecolor`; open the master and `/fb` with `O_CLOEXEC`; read the master only after the child has opened its slave (pty.c:205-215); write to it without blocking, with a queue; `term -l` runs `/bin/login`;
  * every call into libtsm in `term/screen.c`: the screen and the parser, the replies written to the pty, the bell, the clipboard and mode 2026's hold passed to the parts that use them;
  * `term -H` in `term/host.c`: a child on a host pty with no screen, which dumps the screen's cells on request, for tests;
  * `/etc/term.conf` read at start; a host test of a child's round trip through `term -H`;
  * its section of `term/Readme.md`.
* Files: `term/main.c`, `pty.c`, `screen.c`, `host.c`, their sections of `term/term.h` and `term/Readme.md`, the host test.
* Waits on tasks 1 and 2.
* Done: the code and the host test are written, and `term -H` runs a shell on the host.

### 17. term: drawing

`term` draws libtsm's cells on the whole framebuffer, redrawing only what changed.

* Subtasks:
  * the font in `term/font.c`, from task 7's packed Unifont, at two sizes, 1x and 2x (170x48 and 85x24 cells on the Lenovo), chosen by a key and kept in `/etc/term.conf`, with a resize that tells libtsm and the pty;
  * a back buffer in RAM, cached glyphs, and rows or spans of damage written with `pwrite` through task 13's native mode, in each RGB format it reports;
  * libtsm's cells through `tsm_screen_draw`, so combining marks show, with 24-bit color; bold, underline, dim and inverse drawn apart, italic slanted; wide characters across two cells;
  * the cursor's shapes and visibility; a visible bell; the redraw held while mode 2026 is set and forced after its bound;
  * a host test that draws into a framebuffer in memory and checks it; its section of `term/Readme.md`.
* Files: `term/draw.c`, `font.c`, their sections of `term/term.h` and `term/Readme.md`, the host test.
* Waits on tasks 1, 2 and 7.
* Done: the code and the host test are written. Task 28 sees it on the screen.

### 18. libtsm: the keyboard protocols

The first of the patches to `tsm-vte.c`.

* Subtasks: the CSI collector takes `<` and `=` and `do_csi` dispatches `u` (tsm-vte.c:1019-1056, 2179); the Kitty keyboard protocol, flags 1 (disambiguate), 2 (repeat and release) and 4 (alternate keys), with its query, push, pop and set and a stack for each screen; its key encoder, ported from foot (MIT) or Ghostty (MIT) under their notices; plain `CSI s` and `CSI u` (SCOSC, SCORC) kept apart from Kitty's private forms; the legacy encoder fixed to xterm's: Shift+F1 to F12 with `;m`, the application keypad's divide and multiply; host tests, the protocol's own examples among them.
* Files: its patches under `vendor/libtsm/patches/`, their lines in the series and in `vendor/libtsm/Readme.md`, its host tests.
* Waits on task 2. Each libtsm patch is written to be offered to kmscon/libtsm.
* Done: the series applies to the pinned tarball, the library builds as in task 2, and the host tests are written.

### 19. libtsm: wide and combining characters

Patches to `tsm-screen.c` alone, which no other task patches.

* Subtasks: a character of width 2 that starts in the last column wraps first (tsm-screen.c:1107-1125); a combining mark joins the cell before it through `tsm_symbol_append` instead of being dropped; host tests of both, with the agents' wide glyphs and decomposed accents.
* Files: its patches, their lines in the series and the Readme, its host tests.
* Waits on task 2.
* Done: as task 18's.

### 20. libtsm built on Rach2

* Subtasks: first, whether TinyCC compiles and links libtsm's C files for i686 and x86_64 against Rach2's musl, with `__SIZEOF_POINTER__` defined, which TinyCC does not predefine, recorded here; `term/guest.mk`, which builds libtsm and `term` on Rach2 with TinyCC and pdpmake against the staged sources; the sources staged as the other guest builds are; a build and a run of `term -V` on Rach2 in QEMU for each architecture.
* Files: `term/guest.mk`, the manifests' lines for the staged sources, its section of `term/Readme.md`.
* Waits on task 2.
* Done: `term -V` built on Rach2 runs on both architectures.

### 21. libtsm: probes, modes and strings

The second of the patches to `tsm-vte.c`, made on task 18's.

* Subtasks: the calls task 2 declared, filled in; DA1, DSR and CPR, OSC 10 and 11; XTVERSION to both `CSI > q` and `CSI > 0 q`; DECRQM for every mode libtsm knows and those of tasks 18 and 21, with mode 2027 answered as not recognized; replies in the order the probes came, when DA1 ends a group; synchronized output, mode 2026, with a hold the renderer reads and an end the renderer forces after a bounded time; focus events, mode 1004, with a call that sends `CSI I` and `CSI O`; an OSC buffer large enough for OSC 52 (tsm-vte.c:118), with a callback that keeps the clipboard; unknown and oversized OSC, DCS, APC, PM and SOS strings, split across reads, consumed without effect; host tests of each.
* Files: its patches, their lines in the series and the Readme, its host tests.
* Waits on task 18.
* Done: as task 18's.

### 22. term: keys, scrollback and selection

`term` reads keys through the console's keymap and gives the agents xterm's and Kitty's encodings.

* Subtasks:
  * `term/keys.c`: K_MEDIUMRAW on the console and task 14's translator, with each key's entry read by KDGKBENT as it is pressed, and the function strings and accents by KDGKBSENT and KDGKBDIACRUC when a map may have changed, so a map loaded while `term` runs takes effect; the set of keys down, since K_MEDIUMRAW gives a repeat as another press, cleared when the screen comes back; the lock state read with KDGKBLED and written back with KDSKBLED, so `setleds` and `term` agree; libtsm's keysym, the `ascii` from the base table for Ctrl, and for the Kitty protocol the unshifted, shifted and base keys, the base from the tables with the layout lock cleared; press, repeat and release;
  * scrollback: Shift+PageUp and Shift+PageDown through a history of a set length, kept apart from the alternate screen;
  * selection from the keyboard in `term/select.c`: a mode with keys to move, mark, copy and leave; paste into the pty, bracketed when mode 2004 is set; one clipboard for the selection and OSC 52, emptied at logout;
  * key tests through `term -H` and its pty: task 9's table, and the Kitty cases, Shift+Enter, Russian Ctrl+C with flags 1 and 4, and release events;
  * its section of `term/Readme.md`, which also says that `showkey` is run on the kernel console, since under `term` the two would read the same keys, and that `setleds` and `kbdrate` act on the console given as their standard input.
* Files: `term/keys.c`, `select.c`, their sections of `term/term.h` and `term/Readme.md`, the key tests.
* Waits on tasks 4, 9, 14, 16 and 18.
* Done: the code and the key tests are written. Task 28 sees it at the console.

### 23. /fb passed down and back

The owner of the screen can give it to a program it starts and take it back, and the console always comes back whole.

* Subtasks:
  * a descendant of the owner may open `/fb`, which suspends the owner, tells it so with task 1's event on its `/fb` descriptor, and gives the screen and the keyboard back to it, with the same event, when the descendant closes `/fb` or exits; owners stack, so Doom runs inside `term`; an owner that died while suspended is skipped;
  * `kd_state_save()` and `kd_state_restore()` written: the keyboard's mode, locks, LEDs and repeat and the console's termios, saved when the screen passes to a descendant and restored at each return, so Doom killed inside `term` leaves `term` its own settings;
  * when the last owner gives the screen up or is killed: the state from before the first owner took the screen restored and its input queue flushed (syscall_proc.c:430 releases the screen today and nothing else), and the console redrawn;
  * host tests of the stack across fork and exec, a dead owner in the stack, and the console's recovery after a SIGKILL;
  * the `/fb` subsections of `rach2/sys/display/Readme.md`, `man/man4/console.4` and `man/man2/ioctl.2`, and the type-ahead limit of `rach2/sys/tty/Readme.md` (line 99).
* Files: the ownership parts of `rach2/sys/syscall_fs.c` and `rach2/sys/display/fbcon.c`, `rach2/sys/syscall_proc.c` (the release on exit), their host tests, those sections.
* Waits on tasks 13 and 15.
* Done: the code and the host tests are written, and in QEMU a program that owns the screen starts another that takes it and gives it back.

### 24. esctest2 and the Unicode tests

* Subtasks: esctest2 from a pinned tarball under `vendor/esctest2/` (GPL-2, used only as a test and staged on no image), run under `term -H` on the host by esctest2's own driver from a `tests/registry` entry, with no shell or Python script of Rach2's; the form of the list of known failures, each with its reason, which may only shrink and on which no case of tasks 18, 19 and 21 may stand; Unicode tests from task 12's `TERM=xterm-256color` recordings: decomposed accents, Cyrillic, CJK, variation selectors, emoji with modifiers, flags and ZWJ sequences; its section of `term/Readme.md`.
* Files: `vendor/esctest2/`, the runner, the Unicode tests, its section of `term/Readme.md`.
* Waits on tasks 12, 16, 18, 19 and 21. The end run runs it; its failures go into the batch of fixes, and the list is written then, with what does not yet draw right recorded in `term/Readme.md`.
* Done: the runner and the tests are written and compile.

### 25. The keyboard joined and tested

The console types through the new keymaps, Doom reads its keys through the new modes, and machine cases press real keys.

* Subtasks:
  * `kd_key_event()` through task 14's translator into the console's tty, with the modes from `kio_key_modes()`; today's translation in `input.c` and `kbd.c` removed;
  * the owner's scancode read (syscall_fs.c:4515-4525) removed; Doom's port opens `/dev/console`, since inside `term` its descriptor 0 is a pty, sets K_RAW, raw termios and nonblocking input, includes task 1's header, and restores all of it on exit (doomgeneric_rach2.c:40-48, 187); it takes the keyboard only when its standard input is a terminal and it is in the foreground, so the Doom case, which starts it in the background with input from `/dev/null` and types its commands on COM1 (doom_scenario.c:31, 151), still reaches its shell;
  * the host test that runs task 9's table through `keymap_key()` and `keymap_encode()`;
  * the USB case, which presses Enter with QEMU's `sendkey ret` (usb_scenario.c:356-397), checks CR;
  * one machine case for each keyboard, PS/2 and USB, that runs `loadkeys -u kg alt-shift` and presses keys with `sendkey`: Latin and Cyrillic letters, the switch both ways, Caps Lock on Cyrillic, Russian Ctrl+C, a held key's repeats counted by the kernel, the LEDs read by KDGETLED and seen by QEMU, `showkey` and `kbdrate`;
  * `tests/keylatprobe.c` for the new path; the keyboard's paragraphs of `rach2/sys/tty/Readme.md` checked against the code.
* Files: `rach2/sys/kd.c` (its event path), the old translation in `input.c` and `kbd.c`, the scancode read in `rach2/sys/syscall_fs.c`, `ports/doom/doomgeneric_rach2.c`, `tests/cases/machine/usb_scenario.c`, the new cases, their `tests/cases/machine.NAME.json` files and lines in the Makefile's list, `tests/keylatprobe.c`.
* Waits on tasks 4, 5, 8, 9, 10, 11, 12, 14 and 15.
* Done: both keyboard cases and the USB case pass in QEMU, and Doom plays at the console with its keys.

### 26. The SeaBIOS cases

* Subtasks: one machine case for each architecture under SeaBIOS that boots on a framebuffer and then from the text entry, reads the console off a screen dump, and shows Doom's picture scaled on the framebuffer and in mode 13h in text, with the WAD from `/etc/rach2/doom1.wad` as the full run copies it, checked by `ports/doom/fetch_wad.sh`, since a clone has no WAD and the manifests stage it as optional (os-real.manifest:219); their `tests/cases/machine.NAME.json` files and lines in the Makefile's list.
* Files: the new cases, their files and lines.
* Waits on task 6.
* Done: both cases pass in QEMU.

### 27. term built on Rach2

* Subtasks: the finished `term`, with the whole patch series, the packed font and every part, built on Rach2 with `term/guest.mk` for each architecture; what TinyCC refuses fixed in `term/` or recorded; the guest build's section of `term/Readme.md`.
* Files: `term/guest.mk`, fixes in `term/`, that section.
* Waits on tasks 16, 17, 19, 20, 21, 22 and 25, since `term` draws only through task 13's native mode and reads keys only once task 25 has removed the scancode read that the screen's owner gets today.
* Done: `term` built on Rach2 runs a shell there, on both architectures.

### 28. term as the console

init starts `term` at boot on the images people boot, the serial login stays where the machine cases need it, and the kernel console comes back when `term` fails.

* Every machine case logs in on COM1, which today shares the console terminal with the keyboard and the screen (tty/Readme.md:3, :43); all 71 of `tests/cases/machine.*.json` say `"console": "serial"`. With `term` on a framebuffer, the login would leave COM1.
* Subtasks:
  * `rach2.console=term`: a boot option with which init starts `term` as the console session. The images a person boots carry it in their GRUB menu through `RACH2_DISK_APPEND` (mkdisk_grub.sh:84, 423), as the sealed disk carries its tokens (Makefile:1484): the Lenovo's install stick and installed disk that `make machine` builds, and the installed disk's menu that the installer writes from a stick that has it. Test images carry no option, so every existing machine case keeps its login on COM1 and no case, runner or schema changes: the runner refuses boot options for SeaBIOS and OVMF machines anyway (qemu_profile.c:1020-1026), whose command line comes from the disk's menu. With the option and K_MEDIUMRAW, COM1's bytes stay out of the console's input (task 15), so `term` and a serial login never share it;
  * with the option: init asks `/fb` for the framebuffer and starts `/bin/term -l` as the console session of `init-login` and `init-metal` when it has one, and `/bin/login` on the kernel console when it gets ENODEV, so a text boot pays no failed start; after a clean exit, such as a logout, init starts `term` again; after a failure (an exec error, a status other than 0, a signal, or an exit within two seconds of its start), `/bin/login` on the kernel console; neither path starts a shell without a login (init.c:58-65); without the option, today's login; `init-shell` and `init-recovery` do not change;
  * `term` in the manifests of os-real, the GNU root and the install stick's root and in the images' prerequisites, and a check that the GNU root, which installs ncurses's whole database (vendor/gnu/ports/ncurses.sh), has `xterm-256color`;
  * one machine case under UEFI that boots a disk built with the option, keeping `"console": "serial"` as the capture of COM1 (qemu_profile.c:1245-1246), sees in a screen dump a drawn screen that is not the kernel console's, judged by its colors as `screen_scenario.c` judges, logs in with QMP's `send-key`, has a child send DA1, XTVERSION, DECRQM 2026 and `CSI ? u` and write the replies, with its own CR and LF since the console is in `term`'s raw termios, to `/dev/console`, which COM1 shows, starts Doom inside `term` and sees the screen come back, kills `term` with SIGKILL and sees the kernel console's login; one under BIOS on a framebuffer with the probes;
  * every machine case that logs in at the console still passes, since its images carry no option;
  * `man/man1/term.1`, `man/man5/term.conf.5`, the init section of `utils/Readme.md`, `man/man8/init.8`, the boot option's page, and `man/programs.md` as `man/programs.sh` writes it.
* Files: the console session and the option in `utils/init.c`, the option in `make machine`'s disks and the installer's menu, the image the new cases boot, the manifests' lines, the new cases, their files and lines, those documents.
* Waits on tasks 4, 6, 16, 17, 21, 22, 23 and 25.
* Done: the two cases pass in QEMU, and so does every case that logs in at the console.

### 29. The Lenovo

The milestone's work on the machine it is for, before the end run.

* Subtasks:
  * the milestone's branch built for the Lenovo as the release builds it, from the testing host's retained build, so that only changed packages build, with the WAD; a trial boot of the 64-bit build and then of the 32-bit one under UEFI, as the `machine` skill says;
  * the owner's checks at the Lenovo's keyboard and screen, first, while `term` holds the console: Claude Code, Codex and Gemini CLI run in `term` and draw in color, a list of a dozen lines: a Claude Code menu selects with Enter, Shift+Enter, Kyrgyz and Russian with Alt+Shift, Caps Lock on Cyrillic, the LEDs, repeat, scrollback, copy and paste, Doom from `term`'s shell, and the kernel console's login after `term` is killed;
  * then over ssh, with `term` killed and the screen free, since a program started over ssh descends from the ssh service and not from `term`: `doom` runs, by the frames the kernel counts, and a probe times a whole-screen write, a scroll and a spinner's update, recorded here;
  * what fails, fixed on the milestone branch and tried again; the results recorded here.
* Files: this plan's record; fixes in the files of the task they belong to.
* Waits on every other task. `owner-only`. The Mac's tmux sessions reach the Lenovo only once macOS's Local Network permission allows them, which the owner grants.
* Done: every line passes on the 64-bit build, the 32-bit kernel boots and runs `term` under UEFI, and the times are recorded. The release then installs the 64-bit build, and its acceptance runs `doom` again.

## Set aside

* libxkbcommon and the XKB data in the image: C11 and meson, or gnu11 with flex and bison, for what the console's keymaps already express; maps generated on the build host cover the layouts in scope.
* BusyBox's `loadkmap` and `dumpkmap` and binary maps: kbd's tools and text maps replace them, by the owner's decision of 1 November. Their file carries no function strings or accents, and kbd's maps are what Linux's documents and users know.
* KDGETKEYCODE and KDSETKEYCODE, and `setkeycodes` and `getkeycodes`: Rach2 keeps its own set-1 table. Per-console keyboard state, console switching, SAK and hardware typematic programming.
* COM1 as a terminal of its own with its own login, which Fable proposed: the boot option of task 28 keeps the serial login on every test image with a few lines in init and the images' menus, where a separate terminal would change the console's design and every case that reads keys through COM1.
* The Uzbek, Tajik, Tatar and Bashkir extras and the Armenian and Georgian scripts in the kernel console's font: `term` draws them from Unifont, and the kernel console is the fallback.
* modifyOtherKeys: every agent tries the Kitty protocol first. Retested through tmux, which Codex uses with it.
* OSC 8 links opened, desktop notifications, mouse modes beyond libtsm's, full text shaping, color emoji and graphics protocols: their sequences are consumed, and the limits are recorded.
* An `xterm-256color` terminfo entry in the musl root: no program there reads terminfo, and the GNU root has the whole database.
* mmap of the framebuffer, unless task 29's times show writes too slow.
* 32-bit UEFI firmware, found on a few Atom tablets and netbooks of 2013 to 2015: it needs another GRUB image and no machine here has it.

## The reviews

### The plan's first draft

Codex and a Fable subagent reviewed the first draft on 31 October, each on its own. Every point below was checked against the code.

* Both: `/dev/input/event*` is mode 0600, so a user's `term` cannot read evdev. Changed: `term` reads keys from the console, now in K_MEDIUMRAW (task 22).
* Both: the owner's scancode reads do not keep keys from the console's discipline. Changed: the keyboard modes of task 15 stop the console's processing, and task 25 removes the scancode read.
* Both: a descriptor inherited by a child can draw. Changed: task 13 refuses every process but the owner.
* Fable: pwrite needs the descriptor's operations, which `/fb` lacks. Changed: named in task 13.
* Codex: libtsm drops combining marks and clips a wide character in the last column. Changed: task 19 fixes both.
* Codex: `shl-htable.h` is LGPL too, with inline code. Changed: task 2 replaces both files.
* Codex: the guest build was missing. Changed: `term/` is a component of its own, built on Rach2 by tasks 20 and 27.
* Codex: the include of `mk/term.mk` made a cycle, and subtasks of an unmerged task do not reach the branch. Changed: task 2 is the library and the empty component, merged before the libtsm and `term` tasks.
* Both: tasks shared Readmes, `console.4` and the Makefile. Changed: shared documents are edited by named section, lists take lines from any task, and a function two tasks need has one writer (The split).
* Fable: `man/programs.md` is written by `man/programs.sh`. Changed: tasks edit the script's table; task 28 regenerates the file.
* Codex: the pty's master reads EOF before a slave opens, and its writes can block. Changed: task 16 gives the order of startup and writes without blocking.
* Both: the PS/2 host test checked only the scancode. Changed: tasks 9, 14 and 25 test through the keymap and the line discipline.
* Codex: a trial boot does not show that the installed root changed. Changed: task 3 checks the image the release builds, and the release's acceptance confirms the installed one.
* Codex: "all but the listed failures" set no floor. Changed: the list may only shrink, and the libtsm tasks' cases must pass.
* Codex: the introduction promised color at the kernel console. Changed: the console keeps `TERM=rach2`.
* Fable: the musl root's terminfo entry has no reader. Set aside.
* Fable: VTIME is ignored. Set aside then; task 8 adds it now, since kbd's `showkey` needs it.
* Fable: `term` must not send SIGWINCH itself. Changed: task 16.

### The keyboard and the terminal

On 1 November the owner asked for a professional keyboard with Linux's interfaces and for a second opinion on the terminal. A research agent read Linux's keyboard code, kbd, BusyBox and four framebuffer terminals; Codex reviewed the plan, the terminal choice and a keyboard design against the code. Each point was checked against the code.

Followed:

* Codex: keep libtsm; libghostty-vt needs Zig and is unproven on i686, libvterm needs the same work, st drops combining marks, foot brings too much, and a new core is more code to keep.
* Codex: scrollback, keyboard selection and paste; `tsm_screen_draw` for combining marks; two font sizes and distinct attributes, cursor shapes and a bell; the probe contract and a bounded mode 2026; Kitty flag 2, which Codex CLI asks for; the legacy key fixes; Unicode limits tested on real agent output and recorded. Tasks 17, 18, 19, 21, 22 and 24.
* Codex: the redraw measured on scrolling, spinners and typing during output, with a back buffer and damage; the poll gap of the scancode ring; Doom inside `term`. Tasks 15, 17, 23 and 29.
* Codex: three more faults, the shared modifier flags, USB Print Screen and Pause, and erase of one byte; one keyboard engine with repeat in one place and accumulated deadlines; the keyboard split into devices and keymaps; a settings file read before login; one key table against xterm. Tasks 4, 9, 10, 12, 14 and 15.
* Codex: kbd's `loadkeys`, `dumpkeys` and `kbdrate` with text maps, and the function-string and accent requests they need. The owner chose them on 1 November over BusyBox's `loadkmap`. Tasks 4, 5 and 15.
* Research: Linux's model of 256 tables and its value encoding; one translator compiled into the kernel and `term`; LEDs by PS/2 0xED and the USB output report; the default accent table for dead keys; Caps Lock on Cyrillic letters, which Linux lacks; ckbcomp's layout on ShiftL, which keeps Ctrl+C on a Cyrillic layer.

Changed from one review by the other:

* Codex proposed a versioned record of translated keys from the kernel to `term`. The research's way is taken instead: `term` sets K_MEDIUMRAW and runs the same translator with each entry read by KDGKBENT, so `term` uses Linux's interfaces only, its input passes through the console's line discipline and polls as any tty does, and a map loaded while it runs takes effect at once.

Changed in that revision, after the owner's choice of kbd:

* The translator is written to Linux's interface instead of ported from `keyboard.c`, and the default accent table from X11's compose data instead of Linux's: both of Linux's files are GPL-2.0, and Rach2's own code declares no license.
* The layouts come from XKB through ckbcomp, which covers Uzbek, Azerbaijani, Armenian and Turkmen, which kbd's data lacks, and gives every layout one form.

### The split

On 1 November the owner asked for the split that finishes fastest, judged on the loops' measured times. Codex and a Fable subagent reviewed the first split, of 25 tasks, each on its own. Every point below was checked against the code.

Both, followed:

* Task 1 promised to move today's translation behind a hook that takes Linux keycodes, but that translation is indexed by set-1 scancode inside `input_key` (input.c:169-238). Changed: task 1 is headers and typed stubs, today's path stays fed by task 10, and task 25 replaces it.
* Task 2 built `term` files that task 1 created, a wait the table did not show. Changed: task 2 owns `term`'s skeleton.
* The three libtsm tasks patched `tsm-vte.c` at once, and a patch made on the pristine file fails at fuzz 0 when another changed its context. Changed as Fable proposed: task 18 first, task 21 on its patches, and the screen's fixes, in `tsm-screen.c`, a task of their own beside them; Codex's alternative, three tasks with named functions, still needs each patch remade on the others.
* The key table waited on nothing it uses. Changed: task 9 starts at once.
* The board cannot offer a task only to an account with Docker. Changed: tasks 5 and 6 are `owner-only`, for the Mac's Docker.
* The serial login: Fable found that `term` at boot takes the login off COM1, where every case logs in; Codex found the cases' helpers wait for text prompts (netcase.c:251). Changed: the boot option `rach2.console=term` in task 28, carried only by the images people boot.
* The read path that hands the screen's owner raw scancodes (syscall_fs.c:4520) had no owner. Changed: task 25 removes it with Doom's move to K_RAW.
* `term` needs the function strings, the accents and the lock state as well as the entries. Changed: task 22.
* `vconsole.conf`'s quoted list of maps is not systemd's format. Changed: `KEYMAP=` and `KEYMAP_TOGGLE=`, with Rach2's own settings named as such (task 4).
* Task 19 built an empty `term`. Changed: task 20 checks the compiler early, and task 27 builds the finished program.

Fable, followed:

* The MBR disk boots by Multiboot 1, whose header asks for no video, so GRUB gives it text (loader.s:15-19). Changed: task 6 owns both headers or the disk's loader.
* The LEDs need the 8042 helper static in `ps2mouse.c` and `control_out` static in `usb/core.c`, which no keyboard file has. Changed: task 11, beside task 10.
* `kbd_mode` is the way back from a console a killed program left in K_RAW. Changed: staged by task 4.
* Every host test is listed in `tests/cases/host.host.json`; init should ask `/fb` before starting `term`, so text boots pay no failed start; programs need a header of `/fb`'s numbers. Changed: The split, tasks 28 and 13.

Codex, followed:

* The tasks' Done clauses ran host tests, which AGENTS.md leaves to the end of the milestone. Changed: a task is done with its tests written and compiled, and tasks 25 and 28 join the pieces.
* Agents in QEMU need a GNU image, which a worker's clone does not have. Changed: task 12 records the agents' output, which tasks 12 and 24 play, and only task 29 runs the agents, from the retained build.
* `showkey` sets VMIN 18 and VTIME 1, which Rach2 ignores. Changed: task 8.
* `loadkeys -u` switches a raw console to K_UNICODE for the load, and Linux refuses Unicode entries outside K_UNICODE. Changed: task 15 takes them in every mode, and task 4 patches `loadkeys` to leave a raw console's mode alone.
* ckbcomp puts `CtrlL_Lock` on Caps Lock and writes Linux's console strings; `loadkeys -b` drops strings and accents. Changed: task 5 normalizes both and checks with `loadkeys -m`.
* `setleds` and `kbdrate` act on descriptor 0. Changed: init gives them `/dev/console` (task 4), and `term`'s Readme says so (task 22).
* Permissions: Linux lets the console's owner set modes and LEDs and keeps tables for root. Changed: task 15.
* Restoring the keyboard mode alone leaves the console without echo after a killed owner. Changed: task 23 restores termios, the queue, the locks and repeat, and skips dead owners.
* `/fb` loses O_CLOEXEC (syscall_fs.c:2460-2466); channel offsets alone do not describe a format; GRUB's `keep` from a text terminal keeps text. Changed: tasks 13 and 6.
* Doom reads descriptor 0, a pty inside `term`. Changed: task 25 opens `/dev/console`.

Set aside or decided otherwise:

* Fable proposed COM1 as a terminal of its own. Set aside for a boot option, as the section above says.

The second round, in which each reviewer argued against the revision:

* Both: the runner cannot pass a boot option to the SeaBIOS and OVMF machines, whose command line comes from the disk's menu (qemu_profile.c:1020-1026). Codex proposed putting `rach2.console=serial` into the test disks' menus; Fable proposed inverting it, so that only the images people boot carry an option. Fable's is taken: it changes three images instead of every test disk, and no case.
* Codex: the PS/2 LED command's reply must be taken before the key stream drops it, and `control_out` sends no data. Changed: task 1's reply hook, task 11's OUT data stage.
* Codex: the Doom case starts Doom in the background and types on COM1, which K_RAW would block. Changed: Doom takes the keyboard only in the foreground with a terminal on its input (task 25).
* Codex: `tty_read` owns the blocking wait, not `n_tty.c`. Changed: task 8 owns it; Fable named task 15's boundary, `tty_input_char`.
* Codex: esctest2's fixes would edit files other tasks own. Changed: task 24 brings in the suite, and the end run runs it with the host tests.
* Codex: `showkey` and `term` would read the same keys, and a program started over ssh is no descendant of `term`. Changed: `showkey` runs on the kernel console (task 22), and task 29's ssh checks run with `term` killed.
* Both: missing waits (16 and 17 on 1, 24 on 12, 27 on 13 and 25); `term` needs task 21's calls before task 21 lands. Changed: the table, and task 2's stubs of those calls.
* Fable: termios restored only at the last owner leaves `term` in Doom's raw settings after Doom is killed. Changed: task 23 restores at each return.
* Fable: Codex is signed in only in the owner's accounts. Changed: task 12 records Claude Code; task 29 looks at Codex and Gemini CLI.
* Codex proposed keeping the LEDs inside the devices' task, since their events share one order; the LEDs are an output path through files the devices' task does not touch, so they run beside it.
* Codex proposed that the translator wait on the key table; with tests run at the end, the translator merges without it, and the table's test joins them.

### The end run

The branch, rebased onto master at 4ba5ba202 as 4e21d9440, built on both architectures after `make clean` in the owner's copy on the testing host. `tests/run -a both style host` was refused at preparation, since style failed there, so the host tests did not run. Codex reviewed 4ba5ba202..4e21d9440. Every finding below was checked against the code, and they are fixed in three batches, each with its own files.

Batch 1, term (#1776):

* high, Codex: an oversized `wait` command overwrites the heap. `term/host.c:469` copies the input line, which grows to 131072 bytes, into `wait_text`, which holds 65536, with no length check.
* medium, Codex: term reads the descendant's keys after it loses the screen. In `term/main.c:356`, when both are ready, `read_fb_event` handles `FB_EVENT_LOST` and then `term_keys_read` still runs.
* medium, Codex: Kitty key events omit the lock modifiers. `term/keys.c:467` leaves out Caps Lock, and Num Lock everywhere but the keypad.
* medium, Codex: function keys bound by a loaded map skip Kitty event reporting. `term/keys.c:509` sends the map's string, such as Shift+F1's `ESC[1;2P`, literally and records no key event.
* style: `clang-format` changes `term/unifont.c` and `term/unifont.h`. Comment lines over 80 columns are in `tests/host/term_draw_hosttest.c`, `term_hosttest.c`, `term_keys_hosttest.c`, `term_unicode_hosttest.c`, `libtsm_probes_hosttest.c`, `libtsm_width_hosttest.c`, `tests/cases/host/esctest2.c` and `vendor/libtsm/build_libtsm.sh`, and shellcheck flags `vendor/libtsm/build_libtsm.sh` lines 136 and 163.

Batch 2, the kernel's keyboard and display (#1777):

* medium, Codex: releasing a modifier on one keyboard clears it for another that still holds it, since `rach2/sys/kd.c:716` discards the device and the translator keeps one bit for each keycode.
* medium, Codex: switching to raw mode and back leaves a modifier stuck. `kd_set_mode` (`rach2/sys/kd.c:153`) neither clears nor rebuilds the held state, so a release in `K_RAW` never reaches `keymap_key`.
* medium, Codex: the shipped layouts' Alt bindings, such as Alt+F1 `Console_1`, Alt+Left `Decr_Console` and Alt+Up `KeyboardSignal` (`vendor/keymaps/xkb/common.inc:62`), emit nothing, since the translator handles none of those actions. The key table's replay covers only the built-in map.
* medium, Codex: retaking `/fb` keeps an offset that is invalid in the new mode. `rach2/sys/display/fbdev.c:427` calls `fb_take` without rewinding, so a write after a seek past 64000 bytes in native mode, a release and a retake in mode 13h fails with `EFBIG`, although the header promises that a mode change resets the offset.
* low, Codex: `rach2/sys/display/Readme.md:123` still says that an owner's child cannot open `/fb`.
* style: cppcheck finds `navigation[v]` and `parameter[v]` past their ends at `rach2/sys/keymap.c:1716` under the test `v >= 15`, and three unused members of `kernel_termios` in `tests/keylatprobe.c:61-64`. `rach2/sys/wcwidth.c` uses "full stop" and an em-dash written as `--` (line 11). Comment lines over 80 columns are in `rach2/include/rach2/fb.h`, `rach2/sys/display/fb.h`, `fbdev.c`, `keymap.c`, `syscall_fs.c`, `rach2/sys/tty/n_tty.h`, `utils/vconsole.c`, `tests/host/kd_hosttest.c`, `keytable.c`, `keytable_hosttest.c`, `tty_hosttest.c`, `vconsole_hosttest.c` and `ps2mouse_hosttest.c`. Shellcheck flags `vendor/keymaps/gen_keymaps.sh`, `regen_keymaps.sh` and `vendor/kbd/build_kbd.sh`.

Batch 3, the build, the images and the tests (#1778):

* medium, style: the build-rules test's clean tree has no `term/`, so `mk/term.mk:12` cannot include `term/sources.mk`, and eight of its rows fail, `clean-without-cross-compiler` first; the compilation database (`dbcheck`) fails with it.
* medium, Codex: `vendor/gnu/host_checks_files.sh:18` copies no `tests/fixtures/agents`, the recording that `term_unicode_hosttest` declares as a host input, so the GNU host checks' preparation fails.
* medium, Codex: `tests/cases/host/esctest2.c:389` returns 1 on Windows, and the suite has no platform limit, so the release's Windows run fails it.
* medium, Codex: os-real's images carry no `term`. Task 28 put `term` in os-real's manifest, and `krffs/manifests/os-real.manifest:100` stages its sources but not `/bin/term` or its font.
* style: `tests/cases/machine.installer.json:53`, `stick-update`, has a `memory_bytes` that does not reserve QEMU's memory and overhead. Suitecheck finds fixed sleeps that no case declares in `tests/cases/host/build_ports.c:157`, `build_together.c:285`, `flow_status.c:109`, `flow_tooling.c:240`, `host_products_drive.c:140,291,296` and `host_suite_probe.c:137`, and sleeps without a reason in `tests/musl/clockmusl.c:122` and `tests/lagprobe.c:369`. Shellcheck flags `tools/flow/release.sh:1327` (SC2015). Comment lines over 80 columns are in `tools/flow/release.sh:153`, `tools/verify/guestcheck.c:6`, `tests/cases/machine/seabios_scenario.c:9-12`, `usb_scenario.c:341` and `tests/host/etude_artifact_hosttest.c:937`. `vendor/unifont/Readme.md:13` has a heading out of order.
* low, Codex: `man/programs.md:22` lists `dumpkeys`, `kbd_mode` and `showkey` for recovery, whose manifest leaves them out.

Batch 4, the host tests (#1779), found by `tests/run -a both host` on c1d6caeda, which stopped at preparation:

* `tests/host/fixtures/krffs_open.c:255,262` declares `open_framebuffer` and `open_mouse` with no arguments, and `syscall_open.inc:57` now passes `flags`.
* `hub_hosttest` links `rach2/sys/usb/core.c`, which now calls `hid_led_sent` and `hid_led_report`, and the test defines neither. The batch builds every host test with `make -k` to find any more.

The second run, `tests/run -a both style host` on b7afbdd6e after the four batches, was again refused at preparation by style, so the host tests have still not run. Its findings:

Batch 5, function sizes (#1780). `sizecheck` finds, over its limits and not in its reasons file:

* `rach2/sys/keymap.c`: `keymap_key` (171 lines, complexity 91) and `keymap_encode` (complexity 30); `rach2/sys/syscall_fs.c`: `sys__llseek` (121 lines, past the 118 allowed); `rach2/sys/usb/core.c`: `control_write` (7 parameters).
* `term/draw.c`: `draw_cell` (8 parameters) and `draw_cb` (10, complexity 28); `term/host.c`: `command` (26) and `term_host_run` (32); `term/keys.c`: `xterm_key` (30) and `make_key` (7 parameters, complexity 26); `term/main.c`: `loop` (31); `term/screen.c`: `snapshot_cell` (10 parameters).
* `tests/cases/host/esctest2.c`: `results` (30); `tests/cases/machine/installer_stick_update.c`: `boot_update` (110 lines); `seabios_scenario.c`: `play` and `termconsole_scenario.c`: `screen` (7 parameters each); `tests/host/fbdev_hosttest.c`: `requests` (27).

Batch 6, the other checks (#1781):

* registry: `tests/system/suites.md` is not what `tests/run inventory` prints, and the `seabios` suite has no `tests/run NAME` sentence in `suites.mk`.
* conventions: `tests/cases/machine/installer_stick_update.c:269` has `//` inside a sed string, which the line-comment rule takes for a comment.
* waits: `tests/lagprobe.c:369` and `tests/musl/clockmusl.c:122` declare a timing assumption at a line with no sleep and no bound, after batch 3 gave them reasons.
* text: textcheck reads the extracted libtsm tree, `vendor/libtsm/libtsm-4.8.0/`, which `.gitignore` names and which the first run did not report.
* text: `tools/flow/board.sh:105` was 101 columns; fixed on master in 03c21c78d.
