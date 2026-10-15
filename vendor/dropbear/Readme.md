# Dropbear

Dropbear is the SSH server, client and key generator of the os-real, metal and recovery systems. Rach2 builds it as one static multi-call program, `dropbearmulti`, linked against the ported [musl](../musl/Readme.md), because it is small, has no dependency beyond its bundled cryptography libraries, and builds with its own autoconf script. [dropbear(8)](../../man/man8/dropbear.8) is its manual page on this system: the commands, the options and how the images start the server.

## Use

The os-real RAM image stages the program but its init starts no server. To reach it from the host, boot it with a port forwarded:

```
cd rach2
NET_HOSTFWD="hostfwd=tcp::2222-:22" ./run.sh os-real
```

log in at the console as root, and start the server:

```
dropbearmulti dropbear -R -g -p 22
```

`-R` makes the missing host keys at the first connection, and `-g` refuses root's password while root's key is accepted. From the repository root on the host:

```
ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost
ssh -i krffs/rach2_test_ed25519 -p 2222 test@localhost
```

The [krffs Readme](../../krffs/Readme.md) describes the development keys and passwords. On a RAM root the host keys are lost at shutdown, so each boot presents a new one. The metal and recovery systems start the server from their init, as dropbear(8) and [sshd.conf(5)](../../man/man5/sshd.conf.5) say.

## Sources and license

The source is the release tarball from [the Dropbear site](https://matt.ucc.asn.au/dropbear/releases/) in this directory. [`build_dropbear.sh`](build_dropbear.sh) has its version in `VERSION` and its SHA-256 in `TARBALL_SHA256`, and refuses a tarball whose checksum differs; the root `Makefile` gives the same file as `DROPBEAR_TARBALL`. Dropbear is under an MIT-style license, with the licenses of its bundled parts beside it in its `LICENSE` file.

## Local changes

The source is not patched: [`patches/series`](patches/series) lists no patch. The changes are the build options.

[`localoptions.h`](localoptions.h), copied into the tree before the build, turns on Curve25519, ECDH, ChaCha20-Poly1305, AES-128 and AES-256 in CTR mode, Ed25519, and the server's public-key and password authentication, and turns off the message of the day. Its `DROPBEAR_HMAC_SHA256` is a name Dropbear does not read; HMAC-SHA256 is on through the upstream default `DROPBEAR_SHA2_256_HMAC`. Every option the header does not name keeps its upstream default, RSA and ECDSA among them.

The configure options use the bundled libtomcrypt and libtommath and turn off zlib, syslog, hardening, utmp, utmpx, wtmp, wtmpx, lastlog, the login functions, `pututline` and `pututxline`. `--disable-harden` keeps configure from adding position-independent flags, since the Rach2 ELF loader does not relocate a PIE. With syslog compiled out the server writes its diagnostics to standard error, and `-E` is not an option of this build.

The program is built with the server, `dbclient` and `dropbearkey`. It dispatches on its first argument, and also takes `ssh` there for the client; KRFFS has no symbolic links, so the images stage only `/bin/dropbearmulti` and use the argument form.

## Building

`make dropbear` builds for the architecture `ARCH` selects, i686 by default; `make dropbear ARCH=x86_64` builds for x86-64. The root `make` builds it too, through the same stamp. The script takes the architecture from `RACH2_ARCH` through `mk/arch.sh`, which gives the cross prefix, `i686-elf-` or `x86_64-elf-`, the configure host `i686-linux-musl` or `x86_64-linux-musl`, and the directories. An i686 build extracts into this directory and stages under `build/vendor/dropbear/`; an x86-64 build extracts under `build/x86_64/vendor-work/dropbear/` and stages under `build/x86_64/vendor/dropbear/`. Each has its own stamp.

`build_dropbear.sh`:

1. Checks the tarball's SHA-256, deletes the extracted tree and extracts it again.
2. Applies the patches `patches/series` lists, in order, with `patch -p1 -F 0`. It fails on a named patch that is absent, one that does not apply, and a `patches/*.patch` the series does not list.
3. Copies in `localoptions.h`.
4. Configures with `CC` set to the `muslgcc` wrapper and the cross `ar` and `ranlib`, with the options above and `--build` from Dropbear's own `config.guess`.
5. Builds `dropbearmulti` with `MULTI=1 STATIC=1`, strips it and stages it.

The result is a stripped static `ET_EXEC`, ELF32 i386 or ELF64 x86-64. The stamp depends on the script, the tarball, `localoptions.h`, `muslgcc`, `mk/arch.sh`, `patches/series` and every patch, and the musl stamp. `make check-dropbearmulti` requires the staged program to be nonempty. The [manifests](../../krffs/manifests/) stage it at mode 0755 as `/bin/dropbearmulti`.

## Updating

1. Put the new release tarball in this directory and delete the old one.
2. Change `VERSION` and `TARBALL_SHA256` in `build_dropbear.sh`, and `DROPBEAR_TARBALL` in the root `Makefile`.
3. Read the new `default_options.h` and `sysoptions.h` against `localoptions.h`, and the configure options against the new `configure`, for renamed or changed options.
4. A source patch goes under `patches/` with its name in `patches/series`.
5. Rebuild, and run the checks under Tests.

## Tests

`make check-dropbearmulti` runs with every image build that stages the program. The suites, described in [suites.md](../../tests/system/suites.md):

* `tests/run ssh` boots os-real from a disk, starts the server from the console as Use does, and logs in as root by key and as the test account by password, over a pty; it requires the refusal of a missing key, a wrong password and root's password under `-g`.
* `tests/run metal` boots the metal image with nothing typed at the console, logs in by key to the server its init started, and requires init to start the server again after it is killed.
* `tests/run noentropy` runs `dropbearmulti dropbearkey` on a CPU with neither RDSEED nor RDRAND, and requires it to stop with its error that it could not read the random device and to write no key. `rach2/run.sh` passes `-cpu max` for this reason, so that the kernel's random source has a trusted seed.
* `tests/run sealed`, `drill`, `installer` and `uefi` reach their machines over SSH.
