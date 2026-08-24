# Milestone 29 current-file checkpoint

This file is a bounded resume map for the one documentation file in progress. It is not evidence. Reopen the named repository location before using a checked fact in an edit. Replace obsolete state instead of appending a diary.

## Position

* File ready to commit: `utils/Readme.md`.
* First unchecked file after this commit boundary: `vendor/musl/Readme.md`. Do not start it before the utilities commit.
* The original document, 140-line rewrite, and complete product change have been read whole.
* The only product document in this commit will be `utils/Readme.md`.
* Intended commit: the utilities Readme with its worklist, notes, and checkpoint updates.

## Material claims

* The complete utility inventory, companion libraries, build maps, output tiers, host subset, guest build, and image staging.
* The command-line and behavioral contract of the basic file, process, and network programs.
* The DHCP and static-network configuration formats, validation, ownership files, lifetime, and cleanup behavior.
* The boot-slot, GRUB-slot, reflash, shutdown, init, login, account, password, su, and sudo contracts.
* The credential transaction, password prompt, publication, crash residue, and recovery rules.
* The exact default, host, guest, specialized, and manual verification coverage.

## Narrow evidence map

* Inventory and builds: tracked files under `utils/`, `utils/Makefile`, `utils/Makefile.linux`, `utils/guest.mk`, and the relevant shared and root Makefile rules.
* Basic programs: each named entry point, its option parser, and focused freestanding or musl probes.
* Network programs: `ping.c`, `nc.c`, `httpd.c`, `dhcpc.c`, `dhcpmsg.[ch]`, `netup.c`, `netcfg.[ch]`, `ipcheck.[ch]`, `resolv.c`, and their host and guest tests.
* Boot and recovery: `bootctl.c`, `bootslot.[ch]`, `bootverb.[ch]`, `grubslot.[ch]`, `reflash.c`, `sectorcopy.[ch]`, raw-device contracts, and specialized harnesses.
* Accounts and init: `halt.c`, `init.c`, `login.c`, `su.c`, `passwd.c`, `adduser.c`, `sudo.c`, `pwauth.[ch]`, manifests, and exact probes.
* Staging and verification: relevant manifests, `tools/check_guest.py`, `tools/check_setid.py`, `tools/selfbuild_test.sh`, `tools/runtests.py`, host tests, and named root target recipes.

## Open checks

* Stage the utilities Readme and three milestone control files, inspect the staged diff and identity, and commit.

## Checks run for this file

* The component has 48 tracked files: three build files, one Readme, 29 program entry points, and 15 companion headers or sources.
* The main build produces 19 ordinary programs under `build/utils/` and ten selected or privileged artifacts under `build/utils/sbin/`. It prunes stale files at both levels before an incremental build.
* The main build uses `i686-elf-gcc` when available, otherwise `CROSS_CC`, and links each static ELF32 program with the ported musl startup objects, libc, libgcc, and `crtn`. The root target orders `utils` after the libc stamp.
* `Makefile.linux` uses the host compiler and builds only 12 portable file utilities under `build/utils/linux/`.
* `guest.mk` builds all 29 programs locally through `cc -static`. Its companion-source and `-D` mappings match the host Makefile's intended map and are checked by `tools/check_guest.py`.
* Companion maps: `ping` and `nc` use `resolv`; `dhcpc` uses `dhcpmsg` and `ipcheck`; `netup` uses `netcfg` and `ipcheck`; `reflash` uses `sectorcopy`; `bootctl` uses `bootslot`, `bootverb`, and `grubslot`; `login`, `su`, `passwd`, and both `adduser` builds use `pwauth`.
* The 12 portable tools were read whole. They are deliberately small: no option parsing except `rm -r`; `mv` is one `rename`; `cp` copies one regular byte stream; `ls [DIR]` hides dot entries and prints only a dirent type plus fixed `rw-`; `touch` and `mkdir` accept multiple paths.
* `cat` accepts multiple files or stdin but suppresses open and read errors and returns 0. `cp` reports open and write failures but does not distinguish a read error from EOF. Record these as implemented limitations if the final Readme summarizes error behavior.
* Recursive `rm` uses a 256-byte path buffer, skips a child whose constructed path does not fit, and then reports failure when the nonempty parent cannot be removed.
* `ping [host]` resolves an optional host, defaults to `10.0.2.2`, sends four requests through `SC_PING`, reports each non-busy failure as a timeout, and succeeds if at least one reply arrives. `EBUSY` stops it immediately.
* `nc <host> <port>` is a blocking TCP client. It relays one input chunk, reads one reply chunk, and repeats until EOF or error. Its decimal parser accepts trailing non-digits and values above 65535 through the later `unsigned short` conversion.
* `httpd [port]` binds all addresses on port 80 by default and serves one connection at a time. It reads at most 2048 request bytes, always returns one generated HTTP/1.0 200 page, HTML-escapes reflected fields, and retains six recent connection records.
* `dhcpc` defaults to `eth0`. Its options are `-i`, `-b`, `-k`, and a lease cap `-l` from 10 through 604800 seconds. The stop form rejects `-b` but accepts and ignores `-l`; this is recorded as a source defect.
* A client claims `/var/run/dhcpc.INTERFACE.pid` with `O_EXCL`. The file records its pid and configured address. A new client or `-k` removes a stale recorded address only when the interface still has that address, then removes the claim. Empty or unreadable claims are not taken over.
* The foreground client tries four discovers with waits of 2, 4, 8, and 16 seconds. `-b` backgrounds after that first obtain round whether it configured an address or will retry. The parent publishes the child's pid before exiting; the child creates a session and reconnects descriptors 0 through 2 to `/dev/console`.
* A usable acknowledgement is applied as address, optional netmask, replacement default route, and an atomic rename of `/etc/resolv.conf.new`. Failure removes the address and asks again. The claim records the address before application so a later client can clean up after an abrupt death.
* The client renews by connected UDP at T1, rebinds by packet-socket broadcast at T2, and removes the address at expiry or NAK. It honors valid server T1/T2 values, otherwise uses one half and seven eighths. Leases below ten seconds are refused and longer leases are capped at one week or the lower `-l` value.
* `dhcpmsg` builds complete IPv4, UDP, and BOOTP requests, validates received IPv4 and UDP framing and checksums, bounds every option, keeps at most three DNS servers, and rejects unsafe domain bytes. `dhcpmsg_hosttest` covers framing, parsing, hostile options, IPv4 configuration, and lease arithmetic.
* `netup [-a] [INTERFACE ...]` brings up at most eight distinct interfaces. With no arguments or `-a`, it includes names from both `auto` and `allow-hotplug`; explicit names do not require either directive. It ignores non-IPv4 and unrequested stanzas and refuses unsupported methods, bad addresses, overlong lines, and interface names longer than 15 bytes.
* A DHCP stanza forks `/bin/dhcpc -i NAME -b` and waits for its parent status. A static stanza requires an address and netmask, permits no gateway, rejects a live or incomplete DHCP claim, validates the whole tuple, then applies address, mask, and route. It restores the old address and mask after a later failure but cannot restore a lost default route.
* `netcfg_hosttest` covers two-pass ordering, `auto` and `allow-hotplug`, explicit selection, ignored stanzas, malformed files and names, capacity, and every static IPv4 refusal.
* The resolver tries a strict dotted quad, `/etc/hosts` then `/hosts` only when the first path is absent, and finally one DNS A query. It uses the first valid nameserver from `/etc/resolv.conf` or `/resolv.conf`, defaulting to `10.0.2.3`, and retries for about six seconds.
* `dnstest` exercises numeric and hosts parsing, crafted DNS queries and replies, the staged hosts file, two live names, and NXDOMAIN. The test image stages the fallback files at `/hosts` and `/resolv.conf`; shipped images stage the `/etc` forms.
* `bootctl` accepts exactly `status`, `kernel FILE`, `config FILE`, `grub FILE`, `commit`, `trial ENTRY`, `cancel`, and `env KEY [VALUE]`. The parser rejects surplus operands and old option-like spellings. Its host parser test omits `grub`; this is recorded as a test defect.
* Boot-partition commands require readable `/proc/cmdline` and `/dev/bootpart`. A kernel install validates the candidate and fallback, saves fallback, writes and verifies the fixed candidate slot, then arms one trial. Commit requires both a candidate boot token and GRUB's consumed-trial marker. The active ordinary or fixed-address entry pair determines the ids written.
* `config` overwrites and verifies the fixed menu slot with `#` padding. `env` permits only `saved_entry` and `next_entry` and validates their entry ids. `trial` permits seven stub entries; `cancel` removes `next_entry` instead of writing the empty consumed marker.
* `grub` validates a 2-through-1024-sector core image, writes and verifies the inactive 1023-sector gap tail, then switches LBA 1. It never writes sector 0. The final LBA 1 write remains vulnerable to a torn write, and a partial final image sector cannot be detected from GRUB's sector count.
* `reflash DEVICE OFFSET BYTES` opens the device for writing, measures it with `SEEK_END`, requires nonzero 512-byte-aligned bounds, and copies stdin in 64-sector chunks. It retries interrupted I/O, accepts short device writes, and fails unless stdin ends exactly after the declared count.
* Host tests cover FAT32 validation, kernel validation, slot and environment transitions, menu replacement, fault injection, fixed-address ids, GRUB layout and interruption order, and exact stream bounds and failures. `bootpart-test`, `grub-test`, `menu-test`, `install-test`, `authority-test`, `drill-test`, and `sealed-test` exercise the associated device and guest workflows.
* `halt [-p|-r]` calls `reboot` directly, powers off by default, and always returns failure if the call returns. It is an ordinary `build/utils` program swept into os-simple; BusyBox supplies the corresponding os-real applets.
* One `init.c` builds `init-shell`, `init-login`, and `init-metal`. All clear one level of `/var/run`, reap orphans, continue stopped children, respawn the console session, and map `SIGUSR1`, `SIGUSR2`, and `SIGTERM` to halt, poweroff, and restart. The strict builds contain no shell fallback; `init-metal` also configures userland networking when `rach2.ip=none` and supervises foreground Dropbear.
* `login` resets the terminal, allows three failed nonblank attempts with a two-second delay, authenticates without distinguishing missing users, drops groups then gid and uid, falls back to `/` when the home cannot be entered, prints the motd after the drop, and execs a login shell with five constructed environment variables.
* `su [-] [-l|--login] [USER] [-c COMMAND]` defaults to root. Non-root callers prove the target password; root does not. It closes inherited descriptors, clears supplementary groups, drops gid then uid, constructs the same five-variable environment, and enters the target home only in login mode.
* The reference `sudo COMMAND [ARG ...]` is staged only in the test image at mode 04755. It authorizes the caller's real numeric uid from `/etc/sudoers`, closes descriptors 3 through 63, assumes real, effective, and saved root ids, and execs the explicit path with only `PATH` and `HOME`.
* `passwd [--] [USER]` lets non-root callers change only their own account and requires their old password; root is exempt. It collects and hashes the new password before locking, then re-reads and checks the caller, unique records, old hash, record shape, and newline before publishing one shadow replacement.
* `adduser [-u UID] [-s SHELL] [--] NAME` is root-only and not set-id. It chooses the first uid and gid from 1000 through 60000 that is free in both databases unless `-u` is given, then publishes locked shadow, group, a 0700 owned home, and passwd in that order. It releases the transaction before execing `passwd`.
* `pwauth` limits passwords to 100 bytes and names to 63. Readers take no lock. Writers create `/etc/.pwlock` with `O_EXCL`, retry ten times, never remove an existing lock, and tell root to remove a stale one. `cred_rewrite` uses `<path>.tmp.<pid>`, preserves owner and mode, flushes before and after rename, and distinguishes unchanged, published, and published-but-unflushed outcomes.
* The default suite includes `suidprobe`, `sudoprobe`, `musl/suidmusl`, `musl/pwauthmusl`, `musl/passwdmusl`, `musl/addusermusl`, `musl/loginmusl`, and `rebootprobe`. Host tests cover password comparison, salts, prompt and signal handling, transaction ownership, replacement, fork and exit behavior. Specialized coverage includes `account-test`, `shutdown-test`, `login-test`, `metal-test`, `cred-persist-test`, and the sealed SSH flows.
* The rewrite removes the external image and development-history narrative. It now covers the exact program inventory, build maps, implemented limitations, network, boot, account and credential contracts, staging boundaries, and verification ownership.
* Every relative link resolves. Every named root target exists. The banned-term, direct ASCII, watched-phrase, repository text, and `git diff --check` checks pass.
* `make verify-style` passes. `make verify-host` passes all 36 host tests at the end of the file-system tools and userland group.

## Exact next action

Stage the four scoped paths, inspect the staged diff and identity, and commit.
