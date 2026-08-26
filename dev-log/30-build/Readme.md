# Milestone 30: the build

The build system and the tooling around it were rebuilt around a small public interface, one scripting language beside C, and a smaller required tool set. Nothing the operating system does changed: no kernel source, no utility, and no probe contract was touched except where a tool that reads an artifact moved.

## The interface

Six entries, and the root Readme documents them in one paragraph:

* `make` builds everything reusable: the kernel, the KRFFS host tools, the utilities, ISH, the guest probes and musl test programs, the five vendored projects, Doom, and the test, os-simple, and os-real RAM images.
* `make test` builds what the default suite needs and runs it.
* `make clean` removes in-house objects, generated support binaries, and images; `make distclean` also removes the vendor trees, the stamps, and the cached preflight. Credentials survive both.
* `tests/run NAME` runs one named suite. The runner owns the registry, asks Make for each suite's artifacts, checks the suite's runtime tools, and rejects an unknown name with the list of valid ones.
* `rach2/run.sh IMAGE` boots a built image.

`help`, `doctor`, `verify-style`, `verify-host`, `deps`, `full`, `compile-commands`, the per-image build targets, and the forty `*-test` targets left the advertised interface. Each survives this milestone as a one-line alias that prints a retirement notice; removing them is the follow-up work.

## The build

The default build is now correct rather than merely quick. Every published artifact with a semantic validator is built to a temporary file, validated, and renamed into place, and each validator is a prerequisite of a validation stamp that every consumer depends on, so editing a checker revalidates the artifact without recompiling it and a failed revalidation removes the artifact rather than leaving it. The three images became real file targets with a complete staging closure: the stager records the exact inputs it read, including traversed directories and absent optional files, so only an affected image restages. Vendor builds declare their outputs and their patch inputs, heal a deleted output at parse time, and join the caller's job budget instead of each choosing its own parallelism. Per-component fingerprints rebuild a tree when its flags change instead of refusing to build.

Each component's source lists live in one shared manifest read by both the host Makefile and the matching `guest.mk`, so the host and guest builds cannot drift apart.

## The languages

The support languages are POSIX sh and C. Python, Expect, Perl, `gtimeout`, `parted`, `grub-editenv`, host SHA-256 tools, and host `strings` are gone. Every binary format the build reads is now parsed by a small C program: ELF and multiboot (`tools/inspect/elfcheck.c`), MBR tables (`tools/image/mkmbr.c`), the GRUB core embedding (`tools/image/grubembed.c`), packet captures (`tests/system/checkers/pcapcheck.c`), KRFFS images and the set-id inventory (`tools/inspect/setidcheck.c`), FAT fixtures (`tests/host/mkfatfixture.c`), and the prose, guest-build, and configuration checks (`tools/verify/prosecheck.c`, `tools/verify/guestcheck.c`, `tools/inspect/cfgcheck.c`). The QEMU harness, the probe runner, and the pty driver are C as well.

Every replacement ran beside its predecessor before the predecessor was removed. The ELF checker was compared over 38 built binaries and both kernel profiles; the capture checker over 34 crafted captures and four live suites; the set-id guard over every manifest, seven images and a partition region, with two stager mutations caught; the fixture builders byte for byte over all twenty FAT variants; the prose checker over the whole repository and the pinned upstream corpus; and the runners on the matcher against Python's own regular expressions, on five live harness paths, on an identical 63-probe report, and on the pty driver's window size, prompt count, timeout token and refusal.

## Docker and GRUB

Docker left every build, image procedure, and test. Six GRUB boot artifacts are pinned under `vendor/grub/` with a provenance manifest, a digest verifier every consumer runs on the copy it is about to read, and a maintainer regeneration procedure pinned to one dated snapshot of the Ubuntu archive. Disks and ISOs are assembled natively with mtools, `xorriso`, and the project C tools; the disk a native build produces is byte-identical to the one the container produced except for two bytes of mformat's OEM version string.

## The dependencies

Build and run: GNU Make, a POSIX shell with the base utilities, a host C compiler, i686-elf GCC and binutils, tar, gzip, bzip2, `ssh-keygen`, and QEMU. Tests add the OpenSSH client, mtools, and a sanitizer-capable host compiler. Style checks add cppcheck, ShellCheck, and clang-format. `xorriso`, the GRUB host tools, Docker, Git and GDB are maintainer-only; FUSE is optional and needed only to host-mount an image.

## The measurements

The cold build (`make distclean && make`) takes 119 s serial and 35 s at `-j8`, against a 60 s phase 1 baseline that was serial in appearance only: each vendor wrapper chose its own parallelism then, and oversubscribed the machine when the caller asked for `-j`. The no-op build fell from 3 s to 1 s.

## Verification

`tests/run style` and `tests/run host` pass at every commit. `make distclean` followed by `make -j8` rebuilds the tree in 35 s, and the complete suite set (`tests/run all`, 43 suites) passes against what that cold build produced. `tests/run selfbuild` and `tests/run ssh` were run again on their own as direct evidence. Each converted tool was proved by a differential against the tool it replaced and, where the check is a rule rather than a translation, by a mutation that makes it fail.

The dependency list above was checked rather than asserted. A directory of stubs that report their own name and exit 127 was placed ahead of the path, shadowing `python`, `python2`, `python3`, `perl`, `expect`, `parted`, `grub-editenv`, `grub-mkimage`, `gtimeout`, `docker`, `strings`, `sha256sum`, `shasum` and `openssl`. A full build, `tests/run style`, `tests/run host`, `tests/run selfbuild` and `tests/run ssh` all pass with none of them reachable, and no stub was invoked.

Two faults in the conversions were found by running rather than by reading, and are recorded with the rest in `notes.md`: seven suite targets invoke a runner from their suite script rather than their recipe, so the prerequisite scan missed them and one suite failed on a binary that was never built; and a menu-truncation fixture padded from a literal length that was two bytes short, which ran past the slot and destroyed the GRUB environment block behind it.

Two independent reviews then read the new C. The first, over the six largest files, returned fifteen defects, four of which could pass a broken artifact or a failed test. Those four were memory faults in parsers reading untrusted bytes: 32-bit address arithmetic that wrapped, a payload length trusted without bounds, a section header read past its declared size, and a window length that underflowed. The rest were one fault repeated: a checker that could not read something reported the same result as a clean tree. The second review read the fixes, confirmed the four sound, and found seven more of the same kind in places the first had not reached, including one regression the fixes had introduced: requiring the probe's end marker to begin a line would have left a signal death after a partial line unbounded, reading as a hang. All twenty-two are fixed, each with a case that fails without its fix. `notes.md` records them, along with the three places where a fix goes beyond what the Python did.
