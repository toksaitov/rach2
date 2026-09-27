# Debian's chromium-headless-shell

This directory takes Debian's `chromium-headless-shell` for i386, with the closure of the packages it depends on, and unpacks them into a root of their own, `build/vendor/gnu/chromium`. It is the browser that milestone 54 (`dev-log/54-gnu/plan.md`) has Rach2 start; nothing here is built. `make gnu-chromium` runs `fetch_chromium.sh`, as `mk/gnu-extra.mk` says. It runs on a Linux host only and is not part of `make`.

## The suite

Debian 13 (trixie) and sid both build Chromium for i386. The script takes trixie, the stable suite, with its security suite, trixie-security, which has the Chromium of sid, 154.0.8037.57, rebuilt for trixie as 154.0.8037.57-1~deb13u1; trixie itself has 150. trixie's packages are built against its glibc 2.41, and sid's Chromium needs 2.43. Both suites are read as snapshot.debian.org has them at the time `20260927T082144Z`, so that a later run fetches the same packages.

## The steps

`fetch_chromium.sh` has five steps, `index`, `resolve`, `fetch`, `unpack` and `check`, and runs all five from nothing when none is named. Each step writes its output to a log in `build/vendor/gnu/log`.

1. `index` makes a keyring of the Debian archive keys pinned in `../sources-extra.txt`, after checking that each key's primary fingerprint is the one the script has: the archive keys of Debian 12 and 13, the stable release key of 13, and the security archive keys of 12 and 13. Their fingerprints are the ones ftp-master.debian.org publishes, and Ubuntu's `debian-archive-keyring` package has the same ones. The step fetches each suite's `InRelease`, requires the SHA-256 the script has for it, and verifies its signatures with `gpgv`: trixie's by the archive keys of 12 and 13 and the release key of 13, trixie-security's by both security keys. It then fetches the i386 `Packages.xz` index of `main` and checks it against the SHA-256 that the verified text lists.
2. `resolve` follows `Depends` and `Pre-Depends` from `chromium-headless-shell` through both indices, the security suite's entry for a package replacing the stable suite's. A dependency is its first alternative that is a package, or else the first one a package provides. Versions are not compared: within one snapshot of a suite and its security updates, the archive keeps them satisfiable. The closure is 125 packages of 171 MB. Ten come from trixie-security: `chromium-headless-shell`, `chromium-common`, `libblkid1`, `libexpat1`, `libfreetype6`, `libmount1`, `libnss3`, `libpng16-16t64`, `libuuid1` and `libxml2`. `chromium-common` depends on `x11-utils` and `xdg-utils`, which bring in the X libraries and Mesa.
3. `fetch` downloads each package from the snapshot and checks it against the SHA-256 its index lists.
4. `unpack` checks each package against the SHA-256 its index lists again and unpacks its data into the root, whose `/bin`, `/lib` and `/sbin` are links into `/usr`, as trixie's merged `/usr` expects. The root is 558 MB, and it has Debian's own glibc, 2.41, from `libc6`.

The indices and the packages are kept in `../downloads/debian`, which Git ignores, and checked again every time a step uses them.

## The check

The browser is `usr/lib/chromium/chromium-headless-shell`. The check lists, with the sysroot's loader and its `--list`, the libraries that the browser and the shared objects beside it need. The sysroot's `/lib` is first on the library path, so that glibc's libraries come from the sysroot as they will on Rach2, and then come the root's library directories and the directories their RUNPATH entries name, taken inside the root, such as libpulse's `/usr/lib/i386-linux-gnu/pulseaudio`. The check fails when the browser is missing or is not an executable ELF file, when a shared object beside it is not an ELF file, and when a library is not found or resolves outside the root and the sysroot. The browser needs 40 libraries directly and 70 in all besides the loader, and every one is found: glibc's in the sysroot, the rest in the root.

It reports next the newest glibc symbol version that the files of the root need, apart from `libc6`'s own: `GLIBC_2.39`, which `chromium-headless-shell` and `chromium-common` need. The sysroot's glibc is 2.44, which defines every version the root needs; a package that needed a version newer than 2.44 would need a newer glibc.

Last, it runs the browser once, with `--no-sandbox --headless --dump-dom` on a local page whose script writes into the document. The browser finds its data files and starts its helper processes through `/proc/self/exe`, which is a link to the loader when the loader is started by hand; run that way on the testing host, the browser looked for `icudtl.dat` beside Debian's loader and stopped with `SIGTRAP`. So the run is in a copy of the root, under `unshare(1)` with new user, mount and process namespaces and the host's `/dev`, `/proc` and `/sys` mounted in the copy, where `chroot(1)` starts the browser and the kernel loads the root's own `/lib/ld-linux.so.2` for it. On the testing host the browser exits with status 0 and prints the document with the text the page's script wrote, `script ran: 42`. The run reports what happened and does not decide the step.

## Times

On the testing host, a Ryzen 9 7950X3D under WSL2, the first run fetched the 125 packages from snapshot.debian.org in 99 s. With the packages kept in `../downloads/debian`, `make gnu-chromium` took 10 s: unpacking 2 s, the check 6 s, and the other steps a second each or less.

## Host

GnuPG (`gpg` and `gpgv`), xz, GNU tar, `ar` from binutils, curl or wget, and the cross toolchain's sysroot and `readelf` for the check; `unshare(1)` and `chroot(1)` for its run.
