# Milestone 31: setup

Milestone 30 rebuilt the build system and left two things behind: forty-seven retirement aliases, and a machine-setup story that worked on macOS and trailed off on Linux. This closes both, and the stale claims found while closing them.

## The aliases

The forty-seven aliases are gone. `make full`, `make deps`, `make doctor`, `make help`, `make verify-style`, `make verify-host`, `make compile-commands`, and the forty `*-test` targets each printed a notice naming its replacement; they now fail with make's own message. The interface is what the root Readme documents and nothing else: `make`, `make test`, `make clean`, `make distclean`, `tests/run NAME`, and `rach2/run.sh IMAGE`.

Every document, comment, and manifest that named a retired target now names the current command. That is 23 files, including the kernel and utility sources whose comments cite the suite that exercises them.

## Machine setup

`tools/setup/bootstrap_mac.sh` and `mac-formulae.txt` are deleted. A script that installed dependencies on one platform and printed a note on the other made Linux the second-class path, and a manifest read by one script was a second list to keep in step with `doctor.sh`.

In their place `tools/Readme.md` gives the commands for the latest macOS and for Debian and Ubuntu, including the cross-toolchain build the Debian archive has no package for. That section states which tools are needed to build, which are needed to pass `tests/run style`, and which are optional, and it explains why the toolchain must carry `all-target-libgcc`: the kernel is linked `-nostdlib` and uses none, but the ported musl, BusyBox, and Dropbear link it through the `muslgcc` wrappers, so a compiler-only toolchain builds the kernel and then fails in the userland.

## What the checks claimed and what was true

Five declarations did not match the tree, each found by reading the code rather than by a failing run.

* `tests/registry` still required `perl` for the style suite and `expect` for four others. Neither is invoked anywhere; both left with the Python conversion in milestone 30. Because `tests/run` refuses a suite whose declared tool is absent, a machine that followed the documented dependency list would have had five suites refuse to start.
* The same registry declared no `mformat` for the seven suites that build GRUB disks, which do need mtools.
* `doctor.sh` called `ssh-keygen` optional. The default build runs `krffs/gen_demo_key.sh` to provision the demo login keys, so it is required. It is now probed by generating a throwaway key, because `ssh-keygen` has no `--version` and the previous probe would have reported a working tool as missing.
* `doctor.sh` checked neither mtools nor `xorriso`. The first is required, the second optional for `make isos`.
* `mk/toolchain.mk` offered `x86_64-elf-gcc` as a fallback. It cannot work: `vendor/musl/build_libc.sh` configures with `CROSS_COMPILE=i686-elf-` and both `muslgcc` wrappers call `i686-elf-gcc` by name, and `preflight.sh` requires the `i686-elf` binutils regardless. The fallback is removed and the fixed prefix is stated where a reader meets it.

Docker also left the build in milestone 30, but six comments in the Makefile and five elsewhere still called it required. `tests/system/suites/selfbuild.sh` contradicted itself within twenty-five lines, saying the GRUB half needs Docker and then that there is nothing to skip when Docker is absent. Docker is still genuinely required by `vendor/grub/regen_grub.sh`, the maintainer regeneration procedure, and `doctor.sh` reports it there and only there.

## Comment width

`tools/verify/prosecheck.c` classified a file as hash-commented by its extension, so `.mk` files were checked and files named `Makefile` were not. Ninety-eight comment lines across seven makefiles were over the eighty columns CLAUDE.md requires, unseen because the checker never looked. The classifier now names `Makefile`, the root Makefile joins the walked roots, and the ninety-eight lines are wrapped. The reflow was checked by comparing the set of comment words before and after in every file: no word was lost.

The classifier also still listed `.py`, which named nothing after milestone 30.

## Verification

`tests/run style` and `tests/run host` pass. A full build from the reflowed makefiles produces the same tree. `sh tools/setup/doctor.sh` reports every tool in its correct tier and exits zero.
