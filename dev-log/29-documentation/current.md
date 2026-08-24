# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* File ready to commit: `vendor/dropbear/Readme.md`.
* First unchecked file after this commit boundary: `vendor/tcc/Readme.md`. Do not start it before the Dropbear commit.
* The 117-line original, 124-line rewrite, and complete product diff have been read whole.
* The only product document in this commit will be `vendor/dropbear/Readme.md`.
* Intended commit: the Dropbear integration Readme with its worklist, notes, and checkpoint updates.

## Material claims

* Release pin, digest, tracked inputs, extracted-tree lifecycle, build options, wrapper, stamp, output, and dispatch syntax.
* Enabled authentication, key, cipher, MAC, client/server, logging, and record features from the resolved options.
* Image staging, manual os-real startup, supervised metal startup, entropy requirements, host-key persistence, and login provisioning.
* Kernel interfaces required for sockets, entropy, files, identities, ptys, controlling terminals, and window size.
* Exact automated coverage, update procedure, and current limits.

## Narrow evidence map

* Build: `vendor/dropbear/build_dropbear.sh`, `localoptions.h`, `muslgcc`, archive, extracted options, root variables and stamp rules, staged binary.
* Images and accounts: os-real and metal manifests, credential generators, `utils/init.c`, and exact start commands.
* Runtime and tests: `tools/ssh_test.sh`, `tools/metal_test.sh`, pty and authentication probes, and only the kernel functions needed to decide documented interfaces.

## Open checks

* Audit the pin, build, resolved feature set, wrapper, stamp, and dispatch in bounded batches.
* Audit os-real and metal staging, account files and modes, startup commands, entropy and persistence claims.
* Audit SSH and metal harness assertions and the stated kernel boundary.
* Remove the development narrative and rewrite the current integration contract.

## Checks run for this file

* Six files are tracked: `.gitignore`, this Readme, the build script, the 2022.83 archive, `localoptions.h`, and the Dropbear-specific `muslgcc` wrapper. There is no tracked patch directory or patch.
* The direct archive SHA-256 is `bc5a121ffbc94b5171ad5ebe01be42746d50aa797c9549a4639894a16749443b`, matching the build script.
* The script requires installed musl, replaces the extracted tree, has an optional filename-ordered patch loop, copies `localoptions.h`, derives the host build triple, and configures for `i686-linux-musl` through the wrapper.
* Configure disables zlib, syslog, hardening, utmp, utmpx, wtmp, wtmpx, lastlog, login functions, pututline, and pututxline, and selects bundled libtomcrypt and libtommath. The generated `config.h` confirms those disabled facilities.
* The build requests `dropbear`, `dbclient`, and `dropbearkey` in one `MULTI=1 STATIC=1` binary, strips it, and stages `build/vendor/dropbear/dropbearmulti`.
* The stamp depends on the script, `localoptions.h`, wrapper, exact archive, and musl stamp. It does not depend on a patch directory or wildcard even though the script has a patch loop. Adding or changing a patch can therefore leave the stamp current; record this non-document defect and require manual stamp removal if the Readme retains patch instructions.
* The current artifact is a 329,480-byte stripped static ELF32 i386 `ET_EXEC` with no interpreter. The link uses a writable, executable GNU stack.
* The wrapper is separate from BusyBox's. Its functional modes and link order match, but its comments and partial-link description are component-specific.
* `localoptions.h` explicitly selects Curve25519, ECDH, ChaCha20-Poly1305, AES-CTR, Ed25519, server public-key and password authentication, and no MOTD. Its `DROPBEAR_HMAC_SHA256` name is unused in 2022.83; HMAC-SHA256 remains enabled by the upstream `DROPBEAR_SHA2_256_HMAC` default. Unmentioned features retain Dropbear defaults.
* os-real, GRUB-test, metal RAM, metal-disk, and recovery manifests stage the multi-call binary at `/bin/dropbearmulti` mode 0755. They stage `/etc/dropbear` mode 0700, the two accounts and SHA-512 shadow hashes, `/bin/sh` in `/etc/shells`, and root and test authorized-key files with the documented ownership and restrictive modes.
* `gen_demo_key.sh` creates separate Ed25519 pairs for root and test when their public files are absent and can compose machine-local public additions. `gen_demo_password.sh` uses fixed development defaults unless overridden, stores the plaintexts in ignored files, and writes random-salt SHA-512 crypt entries to ignored `shadow`.
* The ordinary os-real init starts only the console login. A person starts `dropbearmulti dropbear -R -g -p 22`; it backgrounds by default. The metal and recovery images use `init-metal`, which runs `dropbearmulti dropbear -F -R -g -p 22`, supervises it, and waits five seconds before restarting an early failure.
* `-R` writes generated host keys under `/etc/dropbear`. Keys last only for a RAM-root boot and persist on a writable disk root. `-g` disables root password authentication but permits root public-key authentication. `-F` is necessary only for init supervision. With syslog compiled out, stderr is already used and `-E` is an invalid option.
* `rach2/run.sh` boots with `CPU=max` by default. Dropbear first tries `getrandom`, then `/dev/urandom`; both Rach2 paths refuse untrusted entropy, so delayed host-key generation fails when the CPU supplies neither RDSEED nor RDRAND.
* `make ssh-test` boots a writable os-real disk copy, logs in at the console, starts Dropbear, proves root key login and test password login over ptys, checks the requested 40x100 window and termios, and rejects missing credentials, a wrong test password, and root's correct password under `-g`.
* `make metal-test` types nothing at the console. It checks command-line network configuration, root key login, a pty, a 1,000-line file in both directions by checksum, and server restart under init with a different pid. It uses `StrictHostKeyChecking=no` with a per-run known-hosts file; the original claim that the test does not disable host-key checking is false.
* The runtime source uses `getrandom` and `/dev/urandom`, `fsync`, hard-link creation with an `EPERM` or `EACCES` fallback, effective and saved identity changes, ownership and mode changes, `/dev/tty`, ptys, and window-size ioctls. The corresponding kernel syscalls and device paths are registered; the SSH harness exercises the authentication and pty boundary end to end.
* The rewrite is 124 lines and 7,721 bytes. It replaces the development narrative with the current pin, build, feature boundary, multicall syntax, account staging, manual and supervised startup, entropy and host-key lifetime, kernel requirements, update procedure, and exact tests.
* All five relative links and every named repository path resolve. The Dropbear release URL returns HTTP 200. Every named Make target exists. The banned-term, watched-phrase, direct ASCII, and `git diff --check` checks pass.
* `make dropbear` reports the build current. `make check-dropbearmulti` passes.
* `make ssh-test` passes root key and test password login, pty and 40x100 terminal checks, and all three refusal cases.
* `make metal-test` passes unattended startup, pty, bidirectional checksum, different-pid restart, boot-log transfer, and the acceptance script.
* `make full` passes. `make verify-style` passes. `make verify-host` passes all 36 host tests.

## Exact next action

Stage the four scoped files, inspect the staged diff and identity, and commit.
