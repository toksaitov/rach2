# iw

iw is Linux's configuration program for wireless devices: it lists the radios and their interfaces, scans, shows the link and the stations, and sets the regulatory domain, all through nl80211. Rach2 builds it unmodified and statically against the ported [musl](../musl/Readme.md) and [libnl](../libnl/Readme.md). `os-real`, the metal disk and the recovery system stage it as `/bin/iw`; [iw(8)](../../man/man8/iw.8) describes it.

## Use

```
iw dev
iw phy
iw dev wlan0 scan
iw dev wlan0 link
```

## Sources and license

`iw-6.17.tar.xz` is the release from <https://www.kernel.org/pub/software/network/iw/>. Its signature, made with Johannes Berg's key `A5E03CB13B3733C48DDA1478D74AA23BCB0F6800` over the uncompressed tar, was verified with gpg; [`build_iw.sh`](build_iw.sh) checks the tarball's SHA-256, `7d182e498289ab39b257da6780d562e415377107f50358ee5b55b8cfe40b1e33`, before extracting it. iw is under the ISC license that `COPYING` gives; every image that stages it stages that file as `/etc/iw.COPYING`.

## Local changes

There are no source patches. iw's Makefile asks pkg-config for libnl unless `NO_PKG_CONFIG` is set; the script sets it and gives libnl's flags itself. It gives `CFLAGS` in the environment, since the Makefile appends its warnings and `EXTRA_CFLAGS` to it and a `CFLAGS` on make's command line would replace them, and it defines `__linux__` for the reason [hostap's Readme](../hostap/Readme.md#local-changes) gives.

The Makefile makes `nl80211-commands.inc`, the table of nl80211's command names, from `nl80211.h` with GNU sed's `\L`, which the Mac's sed lacks. The script makes the same file first with awk, and make finds it newer than `nl80211.h` and keeps it.

## Building

```
make iw
make iw ARCH=x86_64
```

The build makes libnl first. Each architecture extracts under `build/vendor-work/iw/` or `build/x86_64/vendor-work/iw/` and stages the stripped `iw`, 0.3 MB, and `COPYING` under `build/vendor/iw/` or `build/x86_64/vendor/iw/`. The stamp's inputs are the script, the tarball, the musl wrapper, the architecture table and the stamps of musl and libnl.

## Updating

Put the new release here after checking its signature, change `VERSION` and `TARBALL_SHA256` in the script and `IW_TARBALL` in the root Makefile, compare the Makefile's rule for `nl80211-commands.inc` with the script's awk, and build both architectures.

## Limits

iw needs generic netlink with nl80211 from the kernel ([dev-log/69-wifi](../../dev-log/69-wifi/plan.md)); until that is merged, `iw --version` works and every other command reports that nl80211 is not found.

## Tests

The port is checked by booting `os-real` in QEMU on each architecture and running `iw --version`. The Wi-Fi suite reads `iw dev`, `iw phy`, a scan and the link.
