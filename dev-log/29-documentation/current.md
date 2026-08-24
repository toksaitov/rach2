# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* File ready to commit: `ports/doom/Readme.md`.
* First unchecked file after this commit boundary: `dev-log/01-foundation/01.md`. Do not start it before the Doom commit.
* The 182-line original, 118-line rewrite, and complete product diff have been read whole.
* The only product document in this commit will be `ports/doom/Readme.md`.
* Intended commit: the Doom port Readme with its worklist, notes, and checkpoint updates.

## Material claims

* Source ownership, tracked inputs, upstream-derived code, build rules, artifact, ELF contract, and image staging.
* Graphics, keyboard, mouse, timing, files, audio, OPL music, controls, syscalls, memory, and kernel dependencies.
* IWAD acquisition and digest, run procedure, working directory, hardware modes, and exact verification.

## Narrow evidence map

* Build and staging: `ports/doom/Makefile`, tracked paths, artifact, `.gitignore`, `fetch_wad.sh`, root targets, and manifests.
* Platform contract: `doomgeneric_rach2.c`, `crt0.c`, `i_pcsound.c`, `i_oplmusic.c`, local headers, and imported source boundaries.
* Kernel and verification: framebuffer, input, audio ioctls and implementations, run scripts, the absence of a runtime target, and the ELF checker.

## Open checks

* None. The file is ready to commit.

## Checks run for this file

* The port tracks its Makefile, integration Readme, local runtime and platform sources, 21 stub headers, audio and music sources, and the imported Doom engine tree. WAD files are ignored.
* The component Makefile uses `i686-elf-gcc`, GNU99, freestanding static i386 flags, a fixed text address, automatic dependencies, 80 imported engine sources, six local sources, and `crt0.c`.
* The artifact is `build/ports/doom/doom`, currently a 393,652-byte static ELF32 i386 executable. The root `ports/doom` target depends on the kernel and invokes the component Makefile.
* os-real, GRUB-test, and metal-disk manifests stage the executable as optional `/bin/doom` and the operator-supplied WAD as optional `/doom1.wad`.
* `fetch_wad.sh` does not download data. It requires `doom1.wad`, verifies the shareware v1.9 digest when matched, and accepts another IWAD with a warning because the engine recognizes more names internally.
* The platform opens relative `fb` and optional `mouse`, writes indexed 320 by 200 frames and 768-byte palettes, reads set 1 scan codes from fd 0, and uses `gettimeofday` for ticks and busy waits.
* The local runtime wraps eight syscalls and supplies a fixed 8 MiB non-reclaiming heap. The old syscall table omitted `lseek`; the rewrite names the complete wrapper set without duplicating syscall numbers.
* PCM mode requests 11,025 Hz unsigned 8-bit mono, mixes eight effect channels with 49,716 Hz OPL3 output, and writes through console audio ioctls. SB16 consumes the format; HDA converts it. PC speaker fallback uses `DP` lumps and has no OPL music.
* The fixed argument vector is `doom -iwad doom1.wad`, so the staged WAD resolves only when the program starts from `/`. QEMU accepts `sb16`, `hda`, and `speaker` audio selections.
* The rewrite is 118 lines and 6,064 bytes. It removes the stale source inventory, incomplete syscall table, unsupported total-memory figure, UI table, and false silent-skip claim.
* Every named repository path resolves. The banned-term, watched-phrase, direct ASCII, and `git diff --check` checks pass.
* The WAD helper reports the expected absent operator file and its two supported acquisition choices; no WAD was added.
* `make ports/doom` reports the build current. The direct ELF checker accepts the 393,652-byte static ELF32 i386 artifact.
* `make full` passes and reports the optional WAD omission while rebuilding os-real. `make verify-style` passes all 13 sections. `make verify-host` passes all 36 host tests.

## Exact next action

Stage the four scoped files, run `make verify-style`, inspect the staged diff and identity, and commit.
