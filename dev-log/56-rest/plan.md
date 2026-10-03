# Milestone 56: the rest of milestone 55's open items

## The goal

Every item that `dev-log/55-open/notes.md` leaves open is fixed, and so are the three faults of the console that Chawan showed on 2026-10-03, so that the Lenovo runs a system with nothing recorded against it but the framebuffer. The owner chose on 2026-10-03 to take every item but the terminal browsers drawn by Chromium or Firefox, which wait for a framebuffer.

## The items

From `dev-log/55-open/notes.md`:

* `/dev/mqueue` is missing: the POSIX queues work through their calls, and no file system lists them.
* Each file operation on ext2 waits for its own disk flush.
* The browser's program is read from the disk again at each start, since the block cache stops at half the kernel heap.
* A pseudo-terminal belongs to its user's group, not to a group `tty`, so there is no `write`.
* The kernel's name is Rach2, which `config.guess`, CMake and shell scripts do not know.
* A tmpfs takes `size=` and does nothing with it, a tmpfs file stops at 1 GiB, and `fallocate` is EOPNOTSUPP on every type.
* ext2 has no file attributes: `lsattr` lists none and `chattr +i` or `+a` fails.
* The boot disk's whole-disk node is published only with `rach2.diskdiag` or in recovery, and a bind of a directory on KRFFS is refused.
* `ping` prints no `ttl=`.
* The GNU root's image differs between builds in the date of a manual page dated `$Mdocdate$`.
* sudo, CMake, Autoconf, Automake, m4, flex, Bison, jq, screen, and `dig` and `host` are not ported.

From the use of Chawan on the console on 2026-10-03:

* The console's font is code page 437, which has no Cyrillic letters, and a page in Russian or Kyrgyz is all question marks.
* The console's keyboard sends nothing for Alt, so a program's Meta keys need Esc first.
* The console has no UTF-8 locale, though its screen now draws UTF-8.

## Decisions

* The kernel's name: `uname -s` reports `Linux`, the owner's decision of 2026-10-03, after the answer that `config.guess`, CMake and shell scripts match the name exactly. Rach2 is named in the release, the Linux version whose interface the kernel follows, `6.18.0` from the toolchain's headers, with `-rach2` after it, and in the version, `#1 Rach2 1.0.0`, as WSL put Microsoft in its release.
* The console's font keeps code page 437's single and double box drawing, shades and blocks in their places, 0xB0 to 0xDF, where the adapter widens the line glyphs to nine columns, and puts in the rest of the upper half the Cyrillic letters of Russian, Kyrgyz, Kazakh, Ukrainian and Belarusian and a few signs, from GNU Unifont, under its OFL-1.1, loaded into the adapter's character memory at boot and checked by reading it back. The box glyphs that join single and double lines give their places to letters. A Latin letter with an accent is drawn as its letter without it. A machine whose adapter keeps its font keeps code page 437.
* ext2 writes back as Linux's does on a mount without `sync` or `dirsync`: a dirty block waits in the cache and is written by the periodic sync of `update` every thirty seconds, and at once by `sync`, `fsync`, `fdatasync`, `syncfs`, an unmount and the shutdown; a full cache writes before it evicts. A mount with either keeps the ordered write-through.
* File pages are cached in free physical memory beyond the kernel heap, and given back when memory runs short.
* A pseudo-terminal is `root:tty`, group 5, mode 0620, as Linux's devpts gives it with `gid=5,mode=620`, and `write` and `wall` are set-group-ID `tty`.
* The ports follow `vendor/gnu`'s rules, as milestone 55's did: a pinned tarball checked by SHA-256, verified against the upstream's signature or Debian's signed source index, a recipe, the manual pages and a row of the gnu suite. `dig` and `host` come from BIND 9.
* Codex reviews each batch from 2026-10-04, when the owner's quota allows it.

## How the work runs

* In batches, as the Batches section of `AGENTS.md` describes, each checked before the next starts.
* The console's answers to queries, its UTF-8 and Chawan's terminal entry, made on 2026-10-03 after milestone 55's close, are the milestone's first batch.

## Phases

### Phase 1: the console

The screen's answers to the device attributes and the cursor position, its UTF-8, and Chawan's `rach2` terminal; the font with Cyrillic; Alt as Esc; `LANG=C.UTF-8` on the console.

### Phase 2: the kernel's small items

`uname`; the group `tty`, `write` and `wall`; `ping`'s `ttl=`; the mqueue file system at `/dev/mqueue`; the boot disk's whole-disk node and a bind on KRFFS; a fixed date for the manual pages dated `$Mdocdate$`.

### Phase 3: the file systems

tmpfs's `size=` and files past 1 GiB; `fallocate` on ext2 and tmpfs; ext2's file attributes; ext2's write-back; the page cache in free memory.

### Phase 4: the ports

m4, Autoconf, Automake, Bison, flex, jq, screen, sudo, CMake, and BIND 9's `dig` and `host` with what they build on.

### Phase 5: the close

The build from clean and `tests/run all`; the root and the kernel installed on the Lenovo, each item tried there as a user; the record.
