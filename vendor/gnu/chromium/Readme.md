# Debian's chromium-headless-shell

This directory gives the [GNU layer](../Readme.md) a web browser: Debian's `chromium-headless-shell` for i386 or amd64, as `ARCH` selects, with D-Bus's system bus, which the browser asks for, and the packages they depend on. Nothing is compiled. `make gnu-chromium` runs `fetch_chromium.sh`, which unpacks the packages into `chromium/` of the layer's build tree; `install_root.sh` copies the files the root needs from there into the root image.

## Sources and license

The packages come from Debian 13 (trixie) and its security suite, which has the newer Chromium, as snapshot.debian.org has them at one fixed time. [`fetch_chromium.sh`](fetch_chromium.sh) pins that time (`SNAPSHOT`), the SHA-256 of each suite's `InRelease` (`SUITES`), and the primary fingerprint of each archive key (`KEYS`); the keys themselves are pinned in [`../sources-extra.txt`](../sources-extra.txt). The indices and packages are kept in `../downloads/debian`, which Git ignores, and checked again every time a step uses them.

Chromium is under the BSD 3-clause license, with its third-party code under their own licenses. Each package installs its terms as `/usr/share/doc/PACKAGE/copyright`.

## Building

`fetch_chromium.sh` has five steps, and runs all of them from nothing when none is named. Each writes a log, `log/chromium-STEP.log`.

1. `index` builds a keyring from the pinned keys after checking each fingerprint, fetches each suite's `InRelease`, checks its SHA-256 and verifies its signature with `gpgv`, then fetches the `main` `Packages.xz` index for the architecture and checks it against the digest the signed text lists.
2. `resolve` follows `Depends` and `Pre-Depends` from `chromium-headless-shell`, `dbus-daemon` and `dbus-system-bus-common` through both indices; the security suite's entry for a package replaces the stable suite's. A dependency resolves to its first alternative that is a package, or else to the first package that provides one. A dependency on `adduser` is taken as met, since the image already has the bus's user. Versions are not compared: one snapshot of a suite and its security updates is consistent. The closure is written to `work/chromium/closure.txt`.
3. `fetch` downloads each package and checks it against the SHA-256 its index lists.
4. `unpack` checks each package again and unpacks its data into the root, where `/bin`, `/lib` and `/sbin`, and on amd64 `/lib64`, are links into `/usr`, as trixie's merged `/usr` has them. Each package's file list is kept in `work/chromium/files/`, which `install_root.sh` reads.
5. `check` runs the sysroot's loader with `--list` on the browser and on each shared object in `usr/lib/chromium`, with the sysroot's `/lib` first on the library path, then the root's library directories and the RUNPATH directories taken inside the root. It fails when the browser is missing or not an ELF executable, or when a library is not found or resolves outside the root and the sysroot. It reports the newest glibc symbol version the packages other than `libc6` need, beside the sysroot's glibc version. Last, it runs the browser once with `--no-sandbox --headless --dump-dom` on a local page, in a copy of the root under `unshare` and `chroot`, and reports the result without failing on it.

The browser is `usr/lib/chromium/chromium-headless-shell`. The host tools are those the [GNU layer](../Readme.md) lists.

## Updating

Choose a new snapshot time, record each suite's `InRelease` digest after `gpgv` verifies it, and set `SNAPSHOT` and `SUITES` in the script. A new Debian release also needs its archive keys added to `sources-extra.txt` and `KEYS`. Then compare the closure with the `SKIP` list of [`../install_root.sh`](../install_root.sh), which names the packages whose libraries the root builds itself.

## Tests

The `check` step runs with every build. On Rach2, the `chromium` row of `tests/run gnu` resolves the browser's libraries through the loader's cache and has the test account run it with its sandbox on a local page; [`tests/system/Readme.md`](../../../tests/system/Readme.md) describes it.
