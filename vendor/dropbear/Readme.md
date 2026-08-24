# Dropbear 2022.83 integration

Rach2 uses a static Dropbear multi-call binary for the SSH server, client, and key generator. It is linked against the project [musl integration](../musl/Readme.md) and staged as `/bin/dropbearmulti`.

The tracked integration has six files: `.gitignore`, this Readme, `build_dropbear.sh`, `dropbear-2022.83.tar.bz2`, `localoptions.h`, and a Dropbear-specific `muslgcc` wrapper. The extracted source, staged binary, and stamp are ignored and reproducible. There are no project source patches.

## Build

From the repository root:

```
make dropbear
```

`make deps DEPS=full` and `make full` also build Dropbear through the same stamp. The stamp depends on the build script, archive, local options, wrapper, and musl stamp. `make check-dropbearmulti` requires the staged binary to be nonempty.

The build script:

1. Requires `dropbear-2022.83.tar.bz2` to have SHA-256 `bc5a121ffbc94b5171ad5ebe01be42746d50aa797c9549a4639894a16749443b`.
2. Replaces the extracted tree and copies `localoptions.h` into it.
3. Configures for `i686-linux-musl` through `muslgcc`, with `i686-elf-ar` and `i686-elf-ranlib`.
4. Uses bundled libtomcrypt and libtommath and disables zlib, syslog, hardening, utmp, utmpx, wtmp, wtmpx, lastlog, login functions, pututline, and pututxline.
5. Builds `dropbear`, `dbclient`, and `dropbearkey` into one `MULTI=1 STATIC=1` executable, strips it, and stages `build/vendor/dropbear/dropbearmulti`.

The current artifact is a stripped static ELF32 i386 `ET_EXEC`. `--disable-harden` prevents Dropbear's configure script from adding PIE flags, which the Rach2 ELF loader cannot support.

The wrapper has compile, partial-link, and final-link modes. Final links add musl's startup objects, `libc.a`, 32-bit libgcc, and `crtn.o` in the required order.

## Feature selection

`localoptions.h` explicitly enables Curve25519, ECDH, ChaCha20-Poly1305, AES-128 and AES-256 in CTR mode, Ed25519, server public-key authentication, and server password authentication. It disables the login MOTD. The header also defines `DROPBEAR_HMAC_SHA256`, which Dropbear 2022.83 does not read; HMAC-SHA256 remains enabled by the upstream `DROPBEAR_SHA2_256_HMAC` default. Other options absent from the local header retain their Dropbear defaults, including RSA and ECDSA. The configure options remove host facilities that Rach2 does not provide.

The multi-call dispatcher accepts the command as its first argument:

```
dropbearmulti dropbear [server options]
dropbearmulti dbclient [client options]
dropbearmulti dropbearkey [key options]
```

It also recognizes `ssh` as a client command. KRFFS has no symbolic links, so the images stage only `/bin/dropbearmulti` and use the argument form.

## Accounts and image staging

The os-real, GRUB-test, metal, metal-disk, and recovery profiles stage the multi-call binary at mode 0755. They also stage:

* root and `test` account records, with SHA-512 crypt hashes in `/etc/shadow` mode 0600;
* `/bin/sh` in `/etc/shells`;
* `/etc/dropbear` mode 0700 for generated host keys;
* root's authorized keys at `/root/.ssh/authorized_keys` mode 0600;
* the test account's authorized keys at `/home/test/.ssh/authorized_keys` mode 0600, owned by uid and gid 1000.

`krffs/gen_demo_key.sh` creates separate Ed25519 keypairs for root and the test account when their public files are absent. `krffs/gen_demo_password.sh` writes the two ignored plaintext credential files and the ignored `shadow` file. It uses the documented development passwords unless the caller supplies replacements, and gives each SHA-512 crypt hash a random salt.

## Starting the server

The os-real init starts the console login but does not start SSH. To run the RAM-root image with host port 2222 forwarded to guest port 22:

```
cd rach2
NET_HOSTFWD="hostfwd=tcp::2222-:22" ./run.sh os-real
```

Log in at the console as root with the password in `krffs/rach2_demo_password`, then start the server:

```
dropbearmulti dropbear -R -g -p 22
```

The server backgrounds after startup. From another terminal at the repository root:

```
ssh -i krffs/rach2_demo_ed25519 -p 2222 root@localhost
ssh -i krffs/rach2_test_ed25519 -p 2222 test@localhost
```

The test account may also authenticate with the password in `krffs/rach2_test_password`. `-g` disables password authentication for root while leaving root public-key authentication enabled. Blank passwords remain disabled because the command does not pass `-B`.

The metal and recovery images use `init-metal`. It runs this supervised command beside the console login:

```
dropbearmulti dropbear -F -R -g -p 22
```

`-F` keeps the server process in the foreground so init can identify its exit and restart it. Init waits five seconds before restarting an early failure.

## Entropy and host keys

`-R` generates missing host keys on the first connection. Dropbear tries `getrandom` and then `/dev/urandom`; both return `EIO` when the Rach2 CSPRNG lacks trusted hardware entropy. `rach2/run.sh` and `rach2/run_from_disk.sh` therefore use `-cpu max` by default, which exposes RDSEED or RDRAND. A CPU without either source cannot generate the delayed key.

Generated keys are written under `/etc/dropbear`. A RAM root loses them at shutdown. A writable disk root preserves them after the file-system data is flushed. Clients must expect a new host key for each new RAM-root boot.

Syslog is compiled out, so server diagnostics already go to stderr. The `-E` option is absent from this build and is rejected.

## Kernel boundary

The server depends on the ordinary socket stack, the trusted random interfaces, shadow password lookup and `crypt`, saved-user and saved-group identity changes, ownership and mode changes, `fsync`, and the KRFFS hard-link refusal used by Dropbear's host-key fallback. Interactive sessions also require pty allocation, controlling-terminal resolution through `/dev/tty`, termios, and window-size ioctls. The [network](../../rach2/sys/network/Readme.md), [random](../../rach2/sys/lib/krandom.Readme.md), [synthetic file-system](../../rach2/sys/fs/synthfs.Readme.md), and [descriptor I/O](../../rach2/sys/io/Readme.md) documents own those kernel contracts.

## Updating the pin

To update Dropbear:

1. Replace the archive with a release tarball from [the Dropbear site](https://matt.ucc.asn.au/dropbear/releases/).
2. Change `VERSION` and `TARBALL_SHA256` in `build_dropbear.sh`, and change `DROPBEAR_TARBALL` in the root Makefile.
3. Review `localoptions.h`, every configure option, the generated `config.h`, and the resolved defaults for changed or renamed features.
4. Rebuild and inspect the staged ELF before running the verification below.

The build script can apply `patches/*.patch` in filename order, but the root stamp rule does not depend on that directory. If a source patch is added, changed, or deleted, remove `vendor/dropbear/.stamp` before `make dropbear` until the stamp rule is extended to cover patches.

## Verification

```
make dropbear
make check-dropbearmulti
make full
make verify-style
make verify-host
make ssh-test
make metal-test
```

`make ssh-test` boots a writable os-real disk copy and starts the server from the console. It proves root key authentication, test password authentication, pty allocation, terminal size and termios, and the rejection of missing credentials, a wrong test password, and root password authentication under `-g`.

`make metal-test` supplies no console input. It proves automatic network configuration and server startup, root key authentication, pty allocation, a checked file transfer in both directions, and server restart under `init-metal`. The default `make test` image contains neither Dropbear nor BusyBox and does not exercise this integration.
