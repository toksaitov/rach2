# libnl

libnl is the netlink library of the musl userland's Wi-Fi tools: wpa_supplicant, hostapd ([vendor/hostap](../hostap/Readme.md)) and iw ([vendor/iw](../iw/Readme.md)) link its `libnl-3` and `libnl-genl-3` statically to speak nl80211, the kernel's generic-netlink family of wireless devices. No image stages the libraries themselves.

## Sources and license

`libnl-3.12.0.tar.gz` is the release from <https://github.com/thom311/libnl/releases/tag/libnl3_12_0>. [`build_libnl.sh`](build_libnl.sh) checks its SHA-256, `fc51ca7196f1a3f5fdf6ffd3864b50f4f9c02333be28be4eeca057e103c0dd18`, the digest the release publishes beside it, before extracting it.

libnl is under the GNU Lesser General Public License version 2.1. The programs that link it are static, so their corresponding sources are the pinned tarballs and build scripts of this directory and of the programs'; every image that stages one of them stages `COPYING` as `/etc/libnl.COPYING`.

`linux-uapi/` has nine of Linux's user-interface headers, as `make headers_install` of Linux 7.0 writes them (Ubuntu's `linux-libc-dev` 7.0.0): `asm/byteorder.h`, `asm/swab.h` and `asm/errno.h` of x86, which serve i686 and x86-64 alike, `asm-generic/errno.h` and `errno-base.h`, and `linux/filter.h`, `bpf_common.h`, `errqueue.h` and `time_types.h`. They are under the GPL version 2 with the Linux syscall note ([LICENSES/exceptions/Linux-syscall-note](../../LICENSES/exceptions/Linux-syscall-note)), which leaves a program that includes them free of the GPL.

## Local changes

There are no source patches. libnl's own build compiles against the copies of Linux's headers in its `include/linux-private/`, and the ports compile against them too, with `linux-uapi/` for the few more that libnl and hostap include. musl installs none of these, and the build host's must not be used: a Linux host's cross compiler has them and would hide a missing one, and the bare `i686-elf` compiler of the Mac has none. Both sets are staged with libnl's headers.

configure insists on a pkg-config of at least 0.9.0, only to look for the `check` library of the unit tests. The script writes a stand-in that answers that question and finds no package, so the tests are not built and the build host needs no pkg-config. configure runs without the command-line programs, threads or debugging messages.

`libnl-route-3` is not built. No program configured here links it: hostap links it only for MACsec and dynamic VLANs, which its configurations leave out. Its filter-expression parsers are made at build time with bison and flex, which the tarball does not ship generated, and the Mac's bison is too old for them.

## Building

```
make libnl
make libnl ARCH=x86_64
```

The builds of hostap and iw make it first. Each architecture extracts under `build/vendor-work/libnl/` or `build/x86_64/vendor-work/libnl/` and stages `include/` (libnl's `netlink/`, `linux-private/` and `linux-uapi/`), `lib/libnl-3.a`, `lib/libnl-genl-3.a` and `COPYING` under `build/vendor/libnl/` or `build/x86_64/vendor/libnl/`. A program compiles with `-I` that `include/` and links `-lnl-genl-3 -lnl-3`. The stamp's inputs are the script, the tarball, the headers of `linux-uapi/`, the musl wrapper, the architecture table and musl's stamp.

## Updating

Put the new release here, change `VERSION` and `TARBALL_SHA256` in the script and `LIBNL_TARBALL` in the root Makefile, and build hostap and iw for both architectures. A port that includes a header neither libnl's copies nor `linux-uapi/` has fails on the Mac and not on Linux; to find one, build with `MUSLGCC_DEFINES="-nostdinc -idirafter $(i686-elf-gcc -print-file-name=include)"`, which leaves out the host's headers as the Mac's compiler does, and add the header from Linux's installed headers to `linux-uapi/` and to `LIBNL_UAPI` in the root Makefile.

## Tests

The programs exercise the library: [vendor/hostap](../hostap/Readme.md#tests) and [vendor/iw](../iw/Readme.md#tests) say how.
