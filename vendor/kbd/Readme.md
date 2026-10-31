# kbd

kbd supplies `loadkeys`, `dumpkeys`, `setleds`, `kbdrate`, `showkey` and `kbd_mode`, linked statically against Rach2's musl. They use Linux's console keyboard interface. Init applies `/etc/vconsole.conf` with these tools before starting the first console session; [vconsole.conf(5)](../../man/man5/vconsole.conf.5) gives the settings.

## Use

The programs are installed in `/bin` on the images that stage Dropbear and on the GNU root, but for the recovery root, which carries only the three that init runs at boot, `loadkeys`, `setleds` and `kbdrate`. Keymaps are text files under `/usr/share/kbd/keymaps`; `vendor/keymaps/` supplies them. `loadkeys -u kg alt-shift` loads a layout followed by a switch overlay. `setleds` and `kbdrate` use descriptor 0: from a terminal emulator or SSH, redirect their input from `/dev/console`. `showkey` runs on the kernel console, where no other program is reading the keyboard.

The six manual pages under `man/` describe the tools, from the release's pages in `docs/man/man1/` and `docs/man/man8/`.

## Sources and license

`kbd-2.10.0.tar.xz` is the release from <https://www.kernel.org/pub/linux/utils/kbd/>. `build_kbd.sh` checks its SHA-256, `6e5ca4f8d76ee9e3a8db700b667f13e12aac9933828a64e1aaad93d26be9b479`, before extracting it. The programs and their pages are GPL-2.0-or-later; the tarball contains `COPYING` and the source notices. The kernel's keyboard implementation is Rach2's own code, reached through the Linux user interface. The musl patch supplies Linux 6.18.54's user headers with their syscall-note exception.

## Local changes

`patches/series` lists the patches, applied in order with `patch -p1 -F 0 --forward`. The build refuses a patch absent from the series or a series entry that cannot be applied.

`0001-preserve-raw-mode.patch` keeps `loadkeys -u` from changing K_RAW or K_MEDIUMRAW while it defines keys. Rach2 accepts Unicode entries in every keyboard mode, so a terminal emulator can keep reading keycodes throughout a load. In translated mode the temporary switch to K_UNICODE and its restoration remain. The same patch makes `kbd_mode` select K_OFF by its header definition instead of Linux's build version, since a Linux kernel version does not describe Rach2.

## Building

```
make kbd
make kbd ARCH=x86_64
```

The release carries `src/libkeymap/parser.c`, `parser.h`, `analyze.c` and `analyze.h`, generated from bison and flex sources. Neither generator is needed: the build sets `YACC=false` and `LEX=false`, so an unexpected regeneration fails. Configure is given the build machine's own triple, the target's Linux-musl triple, the musl compiler wrapper and the cross binutils.

The build disables shared libraries, NLS, vlock, tests, memory checks, XKB and data compression. It builds only the required libraries and the six programs. An empty pkg-config search path prevents host libraries from being selected; `ac_cv_lib_bz2_BZ2_bzDecompressInit=no` also suppresses kbd's fallback probe, which otherwise runs even with `--without-bzip2`. No compression library or runtime decompressor is required. The generated parser and scanner are taken from the tarball.

Each architecture extracts under its `build/vendor-work/kbd/`, stages the stripped tools under `build/vendor/kbd/`, and keeps a stamp under `build/vendor-work/kbd/`. x86-64 adds `x86_64/` after `build/`. The stamp names the script, tarball, patches, architecture settings, wrapper and musl stamp as inputs. A missing tool invalidates the stamp. Images depend on the staged files.

## Updating

Change the tarball, version and digest in `build_kbd.sh` and the tarball in the top Makefile. Rebase the patch with no fuzz, check that the generated lexer and parser still ship, and build both architectures. Update the manual pages from the release's pages and the program list through `man/programs.sh`.

## Tests

The worker builds the six tools for both architectures and boots QEMU to run `dumpkeys -h`. The keyboard's integration cases load layouts and check the ioctl interface. `tests/host/vconsole_hosttest.c` checks the settings reader, command order and recovery from each failed tool. Host tests run at the milestone's end.
