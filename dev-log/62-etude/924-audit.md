# Boot, installation, GNU and agent case audit

This is the case-by-case source audit for #924 against the converted C collection at 95459636c. All 26 requested families were read, including their original additions and the saved removal commits 19f56d630 through f4c853612. The original request counted 153 cases. The current collection has one additional full-size Lenovo installation rehearsal, for 154 cases. This change removes 14 machine cases and retains 140. The corresponding three GNU rows are also removed from the native collection.

The case lists below identify the retained product checks, not runtime passes. The converted C controllers, guest workloads and manifest assertions were read together. Compilation and shell/JSON syntax checks passed for the changed sources; the integrated preparation, style and affected-family runtime batch remains required on both architectures where applicable. No QEMU, hardware or suite was run for this audit.

The saved removals of `machine.fakeapi.curl` and `machine.agents.gh` are rejected. The former uniquely checks an untrusted CA with no request reaching the server, exact TLS/SSE/WebSocket client behavior, unexpected-request refusal and continued service afterwards. The latter uniquely checks the real Go client's untrusted-certificate and invalid-token failures. Native server self-tests and successful signed-in agent acceptance do not replace those client failures. Their original issues #481 and #480 explicitly require them.

The three screen rows and Muse's tmux row duplicated the same repository work that the retained headless rows and interactive acceptance judge. The screen scripts' notice, title and hello requests were setup protocol plumbing for those sessions, not separate product assertions. Their removal does not claim that the headless rows send those requests. The Antigravity settings-file assertion was a distinct behavior check and is transferred to interactive acceptance. Result mode fields, executable identity checks, server transcripts and process cleanup remain required by the native headless verifiers. Screen JSON files still referenced by rendering tools are retained.

Four retained GNU generators had weak exit-status checks: autoreconf, bison, flex and cmake could accept expected text from a failed generated program. They now require both the expected text and exit zero. GNU tar, OpenSSL and curl also acquire the exact checks needed to replace the three removed dependency boots.

The T11 and T31 entries of the earlier [task audit](audit.md) describe the collection before these deletions. This audit supersedes their references to `machine.etudeguest` and `machine.agents.procfd`. The channel tests and process-filesystem probes named below retain those contracts.

## Removed cases

* `machine.gnu.zlib`: Same library paths remain in `machine.gnu.openssl` (the OpenSSL compression round trip) and `machine.gnu.curl` (`curl -V` must name zlib/1.3.2).
* `machine.gnu.gzip`: The retained `machine.gnu.tar` now compresses and extracts the same 1,000-line input, compares its cksum and requires gzip 1.15. This executes the same gzip program through tar.
* `machine.gnu.m4`: The retained `machine.gnu.autoreconf` and `machine.gnu.bison` require m4 expansion to generate working source. A standalone macro greeting adds no different library or kernel path.
* `machine.bootline.nodisk-fsdisk`: `machine.bootline.cut` begins with the same nodisk fsdisk conflict and now requires its complete refusal text; its additional long suffix also checks truncation.
* `machine.bootline.rescue-fsdisk`: `machine.bootline.kept` supplies fsdisk rescue within the retained command-line limit and now rejects the complete refusal text as well as truncation.
* `machine.sealed.control`: Before its first mutation, `machine.sealed.fault` already checks candidate root, no armed trial, a DHCP lease and no fallback. It now also explicitly checks that recovery was not entered.
* `machine.drill.publish`: Recovery publication is checked by the mandatory recovery build; `machine.drill.repair` actually boots it, requires the authoritative disk device to be writable and repairs that disk. The deleted row only read publication diagnostics.
* `machine.installer.stick`: `machine.installer.stick-update` now compares the actual sealed image length with the manifest, checks the root length and requires the same disk1 MBR disk size and all three partition records after boot. Existing preparation verifies the manifest hashes.
* `machine.etudeguest.serial`: This checks the channel machinery. Native `etude_guest_hosttest.c` runs the actual helper over stdio and serial, including command outcomes, file transfer, hashes, cancellation and framing errors; `etude_serial_hosttest.c` checks serial framing. Retained machine cases use the same helper on Rach2.
* `machine.agents.agy-screen`: The retained `agy-print` requires the exact read, command, edit and commit exchange. `machine.agentsaccept.agy` checks first-run consent, trust and the interactive work, cancellation, resize, reconnect and resume. Its C controller now also requires the trusted directory exactly once in the settings file.
* `machine.agents.claude-screen`: The retained `claude-print` requires the exact read, command, edit and commit exchange. `machine.agentsaccept.claude` handles theme, key, security notes, trust and permission questions, then checks interactive work, cancellation, resize, reconnect and resume.
* `machine.agents.codex-screen`: The retained `codex-exec` requires the WebSocket exchange, its refusal and HTTPS fallback, edit and commit. `machine.agentsaccept.codex` checks trust and model questions, interactive work, cancellation, resize, reconnect and resume.
* `machine.agents.muse-tmux`: The retained `muse` requires the exact read, command, edit and commit exchange. `machine.agentsaccept.muse` checks the real interactive work and terminal lifecycle. The deleted row repeated the exchange in tmux without a different product check.
* `machine.agents.procfd`: This only required `test -d /proc/self/fd` to succeed. That exact directory-type check is transferred to the retained `guest.default.proclinkmusl` assertion `fd-directory`, using stat and S_ISDIR before descriptor-link operations.

## Every reviewed case

### account

The original addition is `d0bd4802d2`. The reviewed assertions are in `tests/cases/machine.account.json` and `account_scenario`.

* `machine.account.simple`: keep. Bare commands work after login; the created account has its home and shell; the unprivileged login reaches its home; a wrong password neither authenticates nor runs the command; the account authenticates with its own password; set-id passwd changes the caller password; the old password is refused without a shell; the new password is refused for another account; the new password reaches the correct home.
* `machine.account.real`: keep. Chpasswd reports the changed password; su changes to the requested uid; a wrong password cannot reach root; passwd refuses a wrong old password; passwd refuses another account record; set-id passwd changes the caller password; the changed password authenticates after a cold boot; httpd mints an MD5 crypt hash; anonymous HTTP is refused without serving the page; httpd accepts the hash it minted; mkpasswd matches the system crypt SHA512 vector; the regular uid is within 1000..60000; the system uid is within 100..999; the shadow record has a SHA512 hash; the account joined the shared group; the regular account is gone from every database and member list after reboot; the system account is gone from every database and member list after reboot; the shared group survives deleting its member.

### agents

The original addition is `1f6743a7c5`. The reviewed assertions are in `tests/cases/machine.agents.json` and `agents_scenario`. Issue #480 requires unsigned authentication and negative TLS/token behavior; those rows remain.

* `machine.agents.agy-print`: keep. The pinned Antigravity binary performs the exact Gemini API read, command, edit and commit sequence, with tool-result and executable-map checks.
* `machine.agents.agy-screen`: removed. The retained `agy-print` requires the exact read, command, edit and commit exchange. `machine.agentsaccept.agy` checks first-run consent, trust and the interactive work, cancellation, resize, reconnect and resume. Its C controller now also requires the trusted directory exactly once in the settings file.
* `machine.agents.agy`: keep. The unsigned first-run screen offers Google OAuth and preserves the packaged binary and license; no fake signed-in account replaces this state.
* `machine.agents.claude-print`: keep. The pinned Claude Code binary performs the exact Messages API read, command, edit and commit sequence, with tool-result and executable-map checks.
* `machine.agents.claude-screen`: removed. The retained `claude-print` requires the exact read, command, edit and commit exchange. `machine.agentsaccept.claude` handles theme, key, security notes, trust and permission questions, then checks interactive work, cancellation, resize, reconnect and resume.
* `machine.agents.claude`: keep. Unsigned authentication status and the first-run sign-in screen work, the packaged version is exact and managed updates stay disabled.
* `machine.agents.codex-exec`: keep. The packaged Codex binary reads and runs tools over WebSocket, falls back to HTTPS after refusal, edits and commits; exact results, executable maps and descendant cleanup are required.
* `machine.agents.codex-screen`: removed. The retained `codex-exec` requires the WebSocket exchange, its refusal and HTTPS fallback, edit and commit. `machine.agentsaccept.codex` checks trust and model questions, interactive work, cancellation, resize, reconnect and resume.
* `machine.agents.codex`: keep. The unsigned device-code screen starts the actual packaged process, with no alternate package under the fresh home and with daemon features disabled.
* `machine.agents.gh`: keep. The real Go client rejects an untrusted CA and an invalid token, accepts the trusted local service, reports unsigned authentication and preserves the packaged version, path and license.
* `machine.agents.muse-tmux`: removed. The retained `muse` requires the exact read, command, edit and commit exchange. `machine.agentsaccept.muse` checks the real interactive work and terminal lifecycle. The deleted row repeated the exchange in tmux without a different product check.
* `machine.agents.muse`: keep. The unsigned trust and settings path and the headless Responses API tool exchange remain, including exact file and committed-tree checks.
* `machine.agents.procfd`: removed. This only required `test -d /proc/self/fd` to succeed. That exact directory-type check is transferred to the retained `guest.default.proclinkmusl` assertion `fd-directory`, using stat and S_ISDIR before descriptor-link operations.
* `machine.agents.worker-session`: keep. The real worker loop completes two tasks against a fixture board and origin repository, with the expected result records and Git history.

### agentsaccept

The original addition is `982f9d14f2`. The reviewed assertions are in `tests/cases/machine.agentsaccept.json` and `agents_scenario`. Issue #500 requires full interactive work, terminal changes and persistent sign-in; each vendor remains a separate case.

* `machine.agentsaccept.agy`: keep. The actual agy interactive client handles startup dialogs, reads and edits the repository, runs its check and commits, cancels a long command, survives terminal resizing and a lost SSH client, resumes the conversation and retains sign-in across reboot. Its settings must record the trusted repository exactly once.
* `machine.agentsaccept.claude`: keep. The actual claude interactive client handles startup dialogs, reads and edits the repository, runs its check and commits, cancels a long command, survives terminal resizing and a lost SSH client, resumes the conversation and retains sign-in across reboot.
* `machine.agentsaccept.codex`: keep. The actual codex interactive client handles startup dialogs, reads and edits the repository, runs its check and commits, cancels a long command, survives terminal resizing and a lost SSH client, resumes the conversation and retains sign-in across reboot.
* `machine.agentsaccept.muse`: keep. The actual muse interactive client handles startup dialogs, reads and edits the repository, runs its check and commits, cancels a long command, survives terminal resizing and a lost SSH client, resumes the conversation and retains sign-in across reboot.

### bootline

The original addition is `f53735879c`. The reviewed assertions are in `tests/cases/machine.bootline.json` and `bootline_scenario`.

* `machine.bootline.nodisk-fsdisk`: removed. `machine.bootline.cut` begins with the same nodisk fsdisk conflict and now requires its complete refusal text; its additional long suffix also checks truncation.
* `machine.bootline.nodisk-root`: keep. Rach2.nodisk with rach2.root=disk is refused by name and the machine halts with no shell.
* `machine.bootline.blank`: keep. A disk root on a disk of 48 MiB of zeros mounts nothing and is refused, and the machine halts with no shell.
* `machine.bootline.init-loop`: keep. An ext2 root that mounts and whose /sbin/init is a symbolic link to itself is refused with ELOOP, and the machine halts with no shell.
* `machine.bootline.cut`: keep. A boot line of 668 bytes is cut before its last word, rach2.rescue, the cut is logged with its lengths, and the rach2.nodisk with rach2.fsdisk before it is refused.
* `machine.bootline.cpu-486`: keep. The i686 kernel on QEMU's 486, which has no time-stamp counter, refuses the processor by name before its own code runs, and the machine halts.
* `machine.bootline.cpu-pentium`: keep. The i686 kernel on QEMU's Pentium, which has no cmov, refuses the processor by name before its own code runs, and the machine halts.
* `machine.bootline.cpu-qemu32`: keep. The x86-64 kernel on QEMU's qemu32, which has no long mode, refuses the processor by name before its own code runs, and the machine halts.
* `machine.bootline.kept`: keep. A boot line of 248 bytes, which QEMU's loader gives the kernel after the kernel's path of some 150 bytes, is kept whole: its last word, rach2.rescue, exempts the rach2.nodisk with rach2.fsdisk before it, and the debug shell starts.
* `machine.bootline.rescue-fsdisk`: removed. `machine.bootline.kept` supplies fsdisk rescue within the retained command-line limit and now rejects the complete refusal text as well as truncation.
* `machine.bootline.rescue-init`: keep. Rach2.nodisk with rach2.root=disk and init=rescue reaches the debug shell it asks for.

### bootorder

The original addition is `3003bc9ec4`. The reviewed assertions are in `tests/cases/machine.bootorder.json` and `bootorder_scenario`.

* `machine.bootorder.behind`: keep. The GRUB disk on AHCI port 1 behind a blank disk on port 0, booted by SeaBIOS, boots to the login prompt with the blank disk numbered disk 0, itself disk 1, and its root found by the disk's MBR id on its third partition, /dev/disk1p3.

### bootpart

The original addition is `a7a3a7f1af`. The reviewed assertions are in `tests/cases/machine.bootpart.json` and `bootpart_scenario`.

* `machine.bootpart.absent`: keep. On a disk whose MBR has one KRFFS partition and no FAT-typed entry 1, the kernel publishes no /dev/bootpart: stat64 and open of it give ENOENT.
* `machine.bootpart.fixture`: keep. On a disk whose MBR has a FAT-typed entry 1 of 1024 sectors at LBA 102400, filled with a known pattern, /dev/bootpart is that partition: a 0600 character device by stat64 and fstat64, sector 5 has the pattern, SEEK_END is the partition's size, a read at or past the end gives 0, a write past it ENOSPC, an unaligned count or offset EINVAL, sector 7 written reads back, and an open by uid 1000 gives EACCES.
* `machine.bootpart.ramroot`: keep. A machine that mounts a RAM image and no disk file system still has /dev/bootpart and /dev/grubcore as 0600 devices; grubcore spans LBA 1 to 2047 and no more, reads at the end give 0, writes at and across the end ENOSPC, an unaligned offset or count EINVAL, a descriptor kept across a drop of privilege still seeks and reads but its write gives EPERM, and a new unprivileged open EACCES.
* `machine.bootpart.wide`: keep. On a sparse disk of 260 GiB over AHCI, /dev/disk0's 64-bit offset reaches every byte: a seek to the end gives the whole size, the sectors at 5 GiB and 258 GiB have their markers, SEEK_CUR crosses 4 GiB both ways, a seek before the start gives EINVAL, a duplicate shares the offset, and on i686 the 32-bit lseek past 4 GiB gives EOVERFLOW and still works below.

### cred-persist

The original addition is `90abeb0361`. The reviewed assertions are in `tests/cases/machine.cred-persist.json` and `credpersist_scenario`.

* `machine.cred-persist.reboots`: keep. Credential replacements preserve their contents, modes and owners across cold boots; a killed writer leaves a reported lock, and an explicit release remains absent after the next boot.

### drill

The original addition is `4f2809f036`. The reviewed assertions are in `tests/cases/machine.drill.json` and `drill_scenario`.

* `machine.drill.repair`: keep. One copy of the drill disk booted through SeaBIOS and GRUB again and again: the everyday kernel refuses to write its own disk and writes nothing; a destroyed root stops the boot without a login; the armed recovery entry comes up from partition 2, takes a lease and answers SSH with nothing typed at its console; the whole root arrives through one ssh pipe; recovery saved as the default comes back after a power cut; the machine boots from the repaired root; a recovery payload that will not load falls back to partition 1; and a destroyed partition 1 still reaches recovery through the core image.
* `machine.drill.publish`: removed. Recovery publication is checked by the mandatory recovery build; `machine.drill.repair` actually boots it, requires the authoritative disk device to be writable and repairs that disk. The deleted row only read publication diagnostics.
* `machine.drill.no-dhcp`: keep. The recovery kernel and image, booted directly as the core image boots them, on a card on a hub of QEMU's with nothing else on it, where no DHCP server answers, take the fixed address 192.168.0.105 and stop the DHCP client.

### etudeexec

The original addition is `245b949c91`. The reviewed assertions are in `tests/cases/machine.etudeexec.json` and `etudeexec_scenario`. Issue #601 requires the executor built by TinyCC and executed on Rach2, which desktop execution cannot replace.

* `machine.etudeexec.build`: keep. Os-real, booted with a second disk that is a tar archive of etude's source, the executor's test and the files they need, unpacks it into a tmpfs as root, and there etude's provision_version.sh builds a version by the TinyCC recipe and then reuses it, the version's record names the platform rach2 and the recipe's -D__rach2__, the built etude's check --selftest and check history pass, and the executor's test, compiled with the version's compiler and flags, passes with every row run, the subreaper's among them, while no etude program asks for a system call the kernel lacks.

### etudeguest

The original addition is `dbbdf80f97`. The reviewed assertions are in `tests/cases/machine.etudeguest.json` and `etudeguest_scenario`.

* `machine.etudeguest.serial`: removed. This checks the channel machinery. Native `etude_guest_hosttest.c` runs the actual helper over stdio and serial, including command outcomes, file transfer, hashes, cancellation and framing errors; `etude_serial_hosttest.c` checks serial framing. Retained machine cases use the same helper on Rach2.

### fakeapi

The original addition is `17f010aa2f`. The reviewed assertions are in `tests/cases/machine.fakeapi.json` and `agents_scenario`. Issue #481 requires the actual client and its negative certificate and request checks.

* `machine.fakeapi.curl`: keep. The actual curl and WebSocket clients check isolated routing, untrusted-certificate refusal before any service request, trusted TLS GET and JSON POST, exact multiline/chunked SSE, masked long WebSocket payloads, ping/pong, close 1000, refusal of an unexpected request and a subsequent successful request.

### gnu

The original addition is `6d86326b3e`. The reviewed assertions are in `tests/cases/machine.gnu.json` and `gnu_scenario`.

* `machine.gnu.logins`: keep. The GNU system boots on its ext2 root and init starts OpenSSH's sshd, which answers with nobody at the console: root logs in by key on a machine that made its host keys on this boot, the test account by key and by its password through /etc/shadow with one prompt, root's password is refused by the server, and scp puts a file on the machine that sftp fetches back byte for byte.
* `machine.gnu.names`: keep. `type -P` finds make, cc and grep in /usr/bin; /bin/bash and /bin/true are links into /usr/bin and /usr/sbin/ldconfig into /sbin; /bin/sh reports GNU bash and finds GNU grep; getent gives nobody 65534:65534 and nogroup 65534.
* `machine.gnu.glibc`: keep. Getconf names glibc 2.44 and the loader (`/lib/ld-linux.so.2` on i686, `/lib64/ld-linux-x86-64.so.2` on x86_64) prints 2.44; `ldconfig -p` reads the cache; en_US.UTF-8 has charmap UTF-8; getent services https is 443/tcp and protocols tcp is 6; Perl getservbyname ssh is 22; /etc/localtime links to Etc/UTC and /etc/timezone says Etc/UTC.
* `machine.gnu.zlib`: removed. Same library paths remain in `machine.gnu.openssl` (the OpenSSL compression round trip) and `machine.gnu.curl` (`curl -V` must name zlib/1.3.2).
* `machine.gnu.openssl`: keep. Version OpenSSL 4.0.2; SHA-256 of abc; /etc/ssl/cert.pem has 121 certificates; the first root of Debian's bundle verifies against the default store. Also retains the removed zlib compression round trip.
* `machine.gnu.openssh`: keep. `ssh -V` is OpenSSH_10.5p1; ssh-keygen makes an ed25519 key; ssh to the machine's own server runs a command, gives the PATH /usr/local/bin:/usr/bin:/bin:/usr/local/sbin:/usr/sbin:/sbin, and `ssh -tt who` lists root on a pts; scp writes a file and sftp fetches it back with the same text.
* `machine.gnu.ncurses`: keep. `tput -T vt100 cup 5 10` emits ESC [6;11H; infocmp names vt100|vt100-am.
* `machine.gnu.less`: keep. Less copies a file to a pipe unchanged; version less 710.
* `machine.gnu.vim`: keep. An ex-mode substitution writes the file; under strace, a backtracking search with a 100 ms limit finds line 1 and gives up on the line of sixty a's (results 1 and 0), and strace sees `timer_create(CLOCK_MONOTONIC` return 0.
* `machine.gnu.perl`: keep. A hash and numeric sort give 1,4,9,16,25; Digest::SHA of abc; an IO::Compress::Gzip round trip; four threads add 10 each to a shared counter under lock, giving 40.
* `machine.gnu.curl`: keep. Curl reads file:///etc/hostname as rach2; openssl makes a self-signed ed25519 certificate, and once `s_server -www` on 127.0.0.1:4433 says ACCEPT, curl with that certificate gets a page naming TLSv1.3. Also requires zlib/1.3.2 in the version output.
* `machine.gnu.git`: keep. Init, add and commit; the log shows the subject; a clone has the file; grep -P with a lookahead finds file:git.
* `machine.gnu.coreutils`: keep. Version 9.12; seq, sort -rn and head give 10; sha256sum of abc; `date -d @0` is 1970-01-01; Europe/Berlin at 0 is 01:00 CET and US/Eastern 19 EST; `who -b` equals the btime of /proc/stat formatted by date.
* `machine.gnu.bash`: keep. An associative array; lastpipe status 1; `printf %a 1` is 0x8p-3; `kill -l 40` is RTMIN+6; pwd as the test user in a directory it may not search; BASH_VERSINFO 5.3; a program run through /proc/self/fd/3; /proc/PID/exe through a relative link to an absolute link equals `readlink -f`; a 5000-byte here-document through a pipe gives 5001 bytes under `timeout 20`.
* `machine.gnu.grep`: keep. `grep -E -c` counts 2; `grep -oP` with a lookbehind gives 22; a match from a pipe into /dev/null exits 0; version 3.12.
* `machine.gnu.sed`: keep. An extended substitution gives he[ll]o; version 4.10.
* `machine.gnu.gawk`: keep. Strftime of 0 in UTC is 1970 and arithmetic gives 42; version GNU Awk 5.4.1.
* `machine.gnu.findutils`: keep. Find -name vt100 -printf; xargs -n 1 splits three words; version 4.11.0.
* `machine.gnu.diffutils`: keep. `diff -u` of two different files exits 1 with the +2 line; cmp of a file with itself exits 0; version 3.12.
* `machine.gnu.patch`: keep. Patch applies a unified difference to give one 2 three; version 2.8.
* `machine.gnu.tar`: keep. Tar -czf, -tzf lists dir/ and dir/file, -xzf extracts the file's text; version 1.35. The input is now 1,000 lines, its extracted cksum must agree and gzip must report 1.15.
* `machine.gnu.gzip`: removed. The retained `machine.gnu.tar` now compresses and extracts the same 1,000-line input, compares its cksum and requires gzip 1.15. This executes the same gzip program through tar.
* `machine.gnu.xz`: keep. A round trip keeps the cksum; version 5.8.4.
* `machine.gnu.file`: keep. File names /bin/bash by the architecture's ELF class, a text file as ASCII text and a gzip file; `file -z` sees ASCII text inside; `--mime-type` of bash is application/x-*.
* `machine.gnu.iproute2`: keep. Over netlink, `ip addr` shows 10.0.2.15/24, `ip route` the default via 10.0.2.2, `ip link` eth0, lo 127.0.0.1/8, `ip link show eth0` link/ether, `ip -s link` RX counters, `ip neigh` 10.0.2.2 lladdr, `ip route get` with src 10.0.2.15, `ip rule` 32766 main, `tc qdisc` noqueue on eth0, `ip netconf` forwarding and rp_filter off; a Perl listener that says it has bound port 5999 appears in /proc/net/tcp and in `ss -tln`; `ss -a` and `ss -s` print nothing on stderr and ss -s has TCP:; lspci reads 00:00.0 Class 0600 8086 from /sys.
* `machine.gnu.loopctl`: keep. `mkfs -t ext2` makes an ext2 volume in a file, which file names; loopctl attaches it by a relative name, lists it by its absolute path, and detaches it.
* `machine.gnu.losetup`: keep. `losetup -f --show` gives /dev/loopN and `-a` lists the image; mkfs.ext2 on the device; blockdev gives ro 0, readahead 256, block size 4096, sector 512; `losetup -d`; `mount -o loop` mounts it as ext2 and the unmount frees the device.
* `machine.gnu.e2fsprogs`: keep. Mke2fs; tune2fs -L and e2label read rowvol; resize2fs grows 8 MiB to 16 MiB and dumpe2fs -h gives 4096 blocks; `fsck -t ext2 -n` exits 0.
* `machine.gnu.pkgconf`: keep. `pkg-config --libs zlib` is -lz; version 2.5.1.
* `machine.gnu.procps`: keep. `ps -p 1` is init; `ps -u root` lists sshd; `pgrep -u root -x sshd` and `pgrep -c` find one; `uptime -p`, free and w print their fields; `vmstat 1 2` prints 4 lines; `kill -l 15` is TERM; sysctl kernel.hostname equals /proc; pmap prints a total.
* `machine.gnu.utillinux`: keep. Lsblk lists a disk; findmnt names / ext2 and `--verify` of /etc/fstab finds no errors; a lock whose holder says it holds it makes `flock -w 1` exit 1, and the freed lock is taken; prlimit NOFILE; ionice best-effort prio 5; a session as root on a terminal of the machine's own server, then last, the reboot record in utmp and `last -x reboot`; column -t; `script -e` exit 3; uuidgen form; dmesg; newgrp mode 4755 root; `unshare -r` as test gives uid 0; `unshare -pf --mount-proc` gives pid 1 and 3 processes; `unshare -u` changes only the inner hostname; `nsenter -t 1 -u` sees the outer one; lsns lists user.
* `machine.gnu.zstd`: keep. A round trip and a gzip-format round trip keep the cksum; `tar --zstd` creates and lists dir/ dir/in; version v1.5.7.
* `machine.gnu.rsync`: keep. `rsync -a` copies a tree that `diff -r` finds equal; `-ai --delete` itemizes exactly b and c; rsync over ssh to the machine's server copies the tree; version 3.5.1.
* `machine.gnu.nano`: keep. Text typed under script and ^X with -t saves the file; /etc/nanorc includes the syntax files; version 9.2.
* `machine.gnu.texinfo`: keep. Makeinfo builds an Info file that info reads; info reads the sed manual's Invoking node; version 7.3.
* `machine.gnu.strace`: keep. `strace -f` shows the write of 11 bytes and the child's execve of /usr/bin/true; `strace -c` prints a total.
* `machine.gnu.lsof`: keep. Once a Perl holder says it has a file open, `lsof -p` of it lists the file, `lsof FILE` names perl, and `lsof -i TCP:22 -s TCP:LISTEN` names sshd.
* `machine.gnu.tmux`: keep. A detached session's pane shows the command's output; a new window started with -c has its login shell in that directory, each pane read until it shows its text.
* `machine.gnu.cron`: keep. One crond runs; `crontab -` as test installs the line, which `crontab -l` returns, owned test:crontab in the spool; the job runs at the next minute of the guest's clock and writes cron row; /var/log/messages has no cron.err; `crontab -r`.
* `machine.gnu.zip`: keep. Zip -r; unzip -l and zipinfo -1 list z/d/a; unzip extracts a tree equal by diff -r; version Zip 3.0.
* `machine.gnu.htop`: keep. Version htop 3.5.3; `htop -n 1` under script draws a screen with Load average and exits 0.
* `machine.gnu.dosfstools`: keep. Mkfs.vfat on a 16 MiB file; `fsck.vfat -n` reports files; fatlabel renames to RENAMED.
* `machine.gnu.uname`: keep. Uname -s Linux, -m the booted architecture, -r 6.18.0-rach2, -v contains Rach2 1.0.0.
* `machine.gnu.ttygroup`: keep. A pty under script has group tty and mode 620; write and wall are tty 2755.
* `machine.gnu.disknode`: keep. /dev/disk0 is a block special file 400 root; a read of sector 0 gives 512 bytes; a write to it over the mounted root gives Device or resource busy.
* `machine.gnu.fallocate`: keep. Fallocate on ext2 gives 1 MiB size and at least 2048 blocks; a range over an existing file keeps its bytes; in /dev/shm a 1 MiB range and a punched 512 KiB hole leave size 1048576 and 1024 blocks.
* `machine.gnu.chattr`: keep. Chattr +i shows in lsattr and refuses an append, a removal and a rename to root; +a takes an append and refuses an overwrite, leaving data and more; -a allows the removal; an unprivileged owner's `chattr +i` gives Operation not permitted.
* `machine.gnu.filecache`: keep. A private read-only mapping of an ext2 file reads first; after an in-place write with the modification time put back, a new mapping reads other.
* `machine.gnu.shadow`: keep. Groupadd; useradd -m prints nothing and writes rowuser:x:1777:1777::/home/rowuser:/bin/bash; usermod sets the comment and adds the group test; chsh sets /bin/sh; `pwck -r -q`; userdel -r removes the account and the home; groupdel.
* `machine.gnu.chawan`: keep. `cha -v` names Chawan; `cha -d` puts a table's cells and a flex container's boxes each on one line and keeps static text without scripts; with scripting on, the script's text script 42 appears.
* `machine.gnu.python`: keep. Version 3.14; imports zlib, bz2, lzma, ssl, hashlib, ctypes, sqlite3, readline, curses, uuid, json, pyexpat and decimal; hashlib SHA-256; ctypes getpid equals os.getpid; sqlite3 sum 42; a Pool(2) map gives 6; ssl names OpenSSL 4.0.2; a venv without pip has its own prefix; a cached byte-code file has flags 3 (checked hash).
* `machine.gnu.gnupg`: keep. Version 2.5.24; sysconfdir /etc/gnupg; an ed25519 key is made; a clearsigned message verifies with Good signature; symmetric encryption and decryption with a passphrase return the text.
* `machine.gnu.m4`: removed. The retained `machine.gnu.autoreconf` and `machine.gnu.bison` require m4 expansion to generate working source. A standalone macro greeting adds no different library or kernel path.
* `machine.gnu.autoreconf`: keep. `autoreconf -i` over configure.ac and Makefile.am, configure, make, and the program prints hi from automake. The generated program must also exit zero.
* `machine.gnu.bison`: keep. Bison makes a parser of sums that cc compiles, and 1+2+3 gives 6. The generated program must also exit zero.
* `machine.gnu.flex`: keep. Flex makes a word counter that cc links with -lfl, and three words give 3. The generated program must also exit zero.
* `machine.gnu.jq`: keep. `.a | add` gives 6; `test("b+")` gives true.
* `machine.gnu.screen`: keep. `screen -dmS` starts a session, `-ls` lists it, `-X quit` ends it.
* `machine.gnu.sudo`: keep. `sudo -u test id -un` is test; sudo is 4755 root and /etc/sudoers 440 root.
* `machine.gnu.cmake`: keep. Cmake configures and builds a one-file project whose program prints hi from cmake. The generated program must also exit zero.
* `machine.gnu.dig`: keep. Once a Python responder on 127.0.0.1:5353 says it has bound its port, one question each from `dig +short` and from `host` gets 192.0.2.1.
* `machine.gnu.make`: keep. GNU make -j2 builds two objects by a pattern rule and links a program that prints 42 and exits 0; a second make says up to date.
* `machine.gnu.autotools`: keep. GNU sed's tarball, copied in by the suite, configures in a login shell with config.guess naming the build (i686-pc-linux-gnu or x86_64-pc-linux-gnu), builds with make -j2, and the sed made prints its version and substitutes heLLo.
* `machine.gnu.gcc`: keep. Gcc -O2 and cc build a C program that prints hello from gcc and exits 0; cc's version 16.2.0; g++ -O2 builds a C++ program that prints hello from g++ 6 and links libstdc++.so.6.
* `machine.gnu.gdb`: keep. Gdb stops at a breakpoint in f with x=20, steps to the return, prints y as 40, backtraces into main, and the program exits normally; a `catch syscall exit_group` catchpoint stops; `gdb --configuration` has --enable-tui.
* `machine.gnu.js`: keep. The SpiderMonkey shell sums 1000 to 499500 and reports inJit() and inIon() true after 5000 calls.
* `machine.gnu.dbus`: keep. The system bus answers GetConnectionUnixUser with the messagebus uid; /etc/machine-id is 32 hex digits; a login shell's session bus answers GetId.
* `machine.gnu.chromium`: keep. Ldd finds every library of chromium-headless-shell; run by test from a login shell with its sandbox, it dumps the DOM with script ran: 42 and logs no dbus/ complaint; a second start costs fewer than 200 disk commands (the file page cache).
* `machine.gnu.ntpd`: keep. One `ntpd -n` runs with the pool servers of /etc/ntp.conf; with the case's SNTP server, whose time starts at 2030-01-01, put in their place and ntpd killed, init starts one again, which says in /var/log/services that it set the time, and clockmusl finds the machine's clock agreeing with the server's bracketed reference.
* `machine.gnu.console`: keep. Root logs in at the serial console of the GNU system, which feeds the console's line discipline as the keyboard does, starts tmux there and runs a command in its pane, whose output reaches the console, and the machine still answers over SSH.

### grub

The original addition is `128a749db0`. The reviewed assertions are in `tests/cases/machine.grub.json` and `grub_scenario`.

* `machine.grub.replace`: keep. Bootctl grub writes a core image into the tail of the gap that is not in use and then selects it, so that the next boot runs the core it installed: the disk's unmarked core installs core B, the boot after it runs B and installs core A into the other tail, and the boot after that runs A; /dev/grubcore refuses a second opener while the first holds it and takes one once the holder has exited.

### install

The original addition is `4c2caae1ce`. The reviewed assertions are in `tests/cases/machine.install.json` and `install_scenario`.

* `machine.install.loop`: keep. The machine installs its own kernel, twice: on a GRUB disk booted with no -kernel, root edits a marker into the banner in /src/rach2, builds the kernel there, installs it as a one-boot trial, which a commit refuses, reboots into it, commits it and finds it again after a cold boot; the candidate corrupted at the extent bootctl names boots the fallback, which repairs it, and the repair is committed; then environment blocks the host writes select the fallback for a destroyed header and a complete old value, and the candidate for a complete new value.

### installer

The original addition is `561734f16f`. The reviewed assertions are in `tests/cases/machine.installer.json` and `installer_alone`, `installer_install`, `installer_mounted`, `installer_reflash`, `installer_spoiled`, `installer_stick`, `installer_stick_update`.

* `machine.installer.stick`: removed. `machine.installer.stick-update` now compares the actual sealed image length with the manifest, checks the root length and requires the same disk1 MBR disk size and all three partition records after boot. Existing preparation verifies the manifest hashes.
* `machine.installer.stick-update`: keep. The product's install stick has the root image and a manifest of the sealed disk on its volume, boots by USB and by IDE beside a copy of the sealed disk, saves the disk's boot and recovery partitions to its volume, updates the disk's root and arms the candidate, and writes the saved partitions back whole.
* `machine.installer.reflash`: keep. On the machine the product's stick boots, reflash writes 32 MiB of the sealed disk to a blank disk with its progress, reads them back to the digest sha256sum takes of the same bytes, and does the same for 2 MiB at a later offset without progress lines.
* `machine.installer.install`: keep. The installer on a machine with two patterned disks refuses wrong answers and writes nothing until told, installs onto the disk named each time, and the disks it writes boot with the accounts given, answer SSH with them, and boot their recovery systems with the key the install placed, or with SSH off when it placed none.
* `machine.installer.mounted`: keep. The installer does not offer a disk with something mounted from it, and says that no disk of the machine can take the image.
* `machine.installer.spoiled`: keep. A stick whose manifest declares another hash than its image's is refused by the installer, and the disk it was told to write is left as it was.
* `machine.installer.alone`: keep. On a machine with nothing but the stick, the installer says that the stick is the one it runs from and that no disk can take the image.

### installer-rehearse

The original addition is `8dcbdbbb0d`. The reviewed assertions are in `tests/cases/machine.installer-rehearse.json` and `installer_rehearse_sitting`.

* `machine.installer-rehearse.sitting`: keep. The Lenovo's install sitting rehearsed on a disk of 256 MiB: the recovery system saves the save set onto a save disk, the install from the test stick grows the root to the disk's end, the saved ranges are put back with reflash and read back, and neither the install nor the restore writes a sector outside the save set.
* `machine.installer-rehearse.lenovo`: keep. The Lenovo's install sitting rehearsed on a disk of the machine's 488397168 sectors with the product's stick: the recovery system saves the save set onto a save disk, the install from the stick grows the root to the disk's end, the saved ranges are put back with reflash and read back, and neither the install nor the restore writes a sector outside the save set. This added case is retained because its full-size disk changes address and write-range boundaries.

### installer-repair

The original addition is `19c8b09845`. The reviewed assertions are in `tests/cases/machine.installer-repair.json` and `installer_repair_convert`, `installer_repair_convert_stopped`, `installer_repair_cut`, `installer_repair_restore`, `installer_repair_resume`.

* `machine.installer-repair.cut`: keep. A write of the disk image that reflash -p was asked for ninety-six mebibytes of and given sixteen, stopped once it has printed its progress line for the sixteen, leaves exactly the image's first sixteen mebibytes over the disk that was there.
* `machine.installer-repair.restore`: keep. A disk with no boot code and no table is put back through rach2-recover's menu and boots with the image's accounts; changed and updated from the stick it keeps its password, keys and homes and names its archive's difference; with its root damaged it is updated from the archive on its recovery partition.
* `machine.installer-repair.convert`: keep. An MBR disk whose boot partition is FAT32 at eight sectors to a cluster is updated from the stick and converted: its boot partition made again at one sector to a cluster with its serial, GRUB's EFI image and the menu, its entry of type 0xef in the same place; a second update leaves the EFI image alone, and the disk boots by UEFI to a login on its root and by BIOS into its saved default.
* `machine.installer-repair.convert-stopped`: keep. The conversion of an MBR disk stopped after the boot partition's files were kept on the recovery partition and before its new file system had them is finished by the update run again, which makes the file system from the copy, converts the disk and removes the copy.
* `machine.installer-repair.resume`: keep. Updates stopped after the root's write are run again and give the machine back its accounts, homes, keys and their owners and modes: a stop planted on the disk with the archive in the old layout or in slot A, and stops made by copies of the installer that exit at a line of their own, before the archive is written and after the root is checked.

### login

The original addition is `533e7b181f`. The reviewed assertions are in `tests/cases/machine.login.json` and `login_scenario`.

* `machine.login.strict`: keep. Init reports the missing login at least twice; init explains rescue boot; the console never reaches uid zero.
* `machine.login.shell`: keep. The permissive init runs id as root.
* `machine.login.stopped`: keep. Init continues a session that stopped itself.
* `machine.login.rescue`: keep. The rescue boot reaches the kernel debug shell.

### make

The original addition is `887cb52ec2`. The reviewed assertions are in `tests/cases/machine.make.json` and `devtools_scenario`.

* `machine.make.rebuild`: keep. The installed system, as the test account at the console, builds a program of two sources and a shared header with make and tcc, finds it up to date, rebuilds exactly what a changed source or header needs, fails without the command line's compiler and names the makefile's, cleans with a file named clean present and builds this project's cat with make; after a reboot of the same disk the object's time is the one recorded, nothing rebuilds, and a source changed after the reboot rebuilds into the new program.

### man

The original addition is `c869ad0786`. The reviewed assertions are in `tests/cases/machine.man.json` and `man_scenario`.

* `machine.man.real`: keep. On os-real, logged in as root, man gives every staged page and every alias's page byte for byte through a pipe, shows rach2(7)'s first line and its last under the pager, and runs a pager that -P names with its options.
* `machine.man.recovery`: keep. On the recovery image, whose console is a shell without a login, man gives every page the image stages byte for byte through a pipe.

### menu

The original addition is `a945fe6c37`. The reviewed assertions are in `tests/cases/machine.menu.json` and `menu_scenario`.

* `machine.menu.staged`: keep. A menu staged through bootctl config adds its entries after the stub's three, a grubenv key round-trips through bootctl env and bootctl cancel removes it, the fallback number still selects the stub's own fallback entry once the candidate kernel is destroyed, a staged file torn from the host leaves the machine booting through the stub with GRUB's parse error, and the slot's extent does not move across the write.

### sealed

The original addition is `beca3a57f4`. The reviewed assertions are in `tests/cases/machine.sealed.json` and `sealed_scenario`.

* `machine.sealed.fault`: keep. With nothing typed at the console, the sealed disk comes up on its ext2 root and answers a key login; a kernel sent over SSH is installed, tried once and committed; a destroyed candidate is caught by the fallback and repaired from it; a missing, a faulty and a dirty root under an armed trial each land in recovery, which repairs the root; a staged menu is shown and a torn one leaves the machine booting; the boot loader replaced through /dev/grubcore boots with the old core zeroed; and a root destroyed with the machine stopped is rewritten from recovery through an ssh pipe and boots.
* `machine.sealed.control`: removed. Before its first mutation, `machine.sealed.fault` already checks candidate root, no armed trial, a DHCP lease and no fallback. It now also explicitly checks that recovery was not entered.

### selfbuild

The original addition is `c1c23f9c32`. The reviewed assertions are in `tests/cases/machine.selfbuild.json` and `selfbuild_scenario`.

* `machine.selfbuild.kernel`: keep. The machine builds its own kernel on a KRFFS root with the staged TinyCC and pdpmake twice, to the same bytes, the host takes the bytes it built off the disk and checks them, and GRUB starts them to a login prompt and to the debug shell, where every probe of the default suite passes.
* `machine.selfbuild.userland`: keep. The machine builds its own utilities and shell on a KRFFS root with the staged TinyCC and pdpmake, and the programs it built run: the file tools, the network tools, the refusals of the credential tools and the shell.
* `machine.selfbuild.inplace`: keep. Root builds the staged /src/ish where it is on a KRFFS root with the staged TinyCC and pdpmake, which needs the times the image gave the staged files.

### selfbuild-ext2

The original addition is `c1c23f9c32`. The reviewed assertions are in `tests/cases/machine.selfbuild-ext2.json` and `selfbuild_scenario`.

* `machine.selfbuild-ext2.kernel`: keep. The machine builds its own kernel on an ext2 root with the staged TinyCC and pdpmake, the host takes the bytes it built off the disk and checks them, and GRUB starts them to a login prompt and to the debug shell, where every probe of the default suite passes, and to the debug shell of an ext2 root, where the probes of an installed root pass.
* `machine.selfbuild-ext2.inplace`: keep. Root builds the staged /src/ish where it is on an ext2 root with the staged TinyCC and pdpmake, which needs the times the image gave the staged files.

### tcc

The original addition is `d25a97d8fc`. The reviewed assertions are in `tests/cases/machine.tcc.json` and `devtools_scenario`.

* `machine.tcc.compile`: keep. The installed system, as the test account at the console, compiles a C program with tcc and runs it; refuses -run, -shared, a link without -static and a raw-image link, each with its reason and with no output left; compiles an object; builds this project's cat, which agrees with BusyBox's over a 21 KB binary file and over two files; links ping from two sources; and the kernel's trace of missing system calls names none that tcc asked for.

### uefi

The original addition is `ff764d5f5e`. The reviewed assertions are in `tests/cases/machine.uefi.json` and `uefi_scenario`.

* `machine.uefi.installed`: keep. OVMF boots the installed GPT disk from its EFI system partition; the kernel finds its root by the disk's GUID, makes the framebuffer write-combining and shows the login prompt on the framebuffer in its own font; root logs in over SSH and bootctl names the candidate as booted; Doom draws its picture on the framebuffer, and the console has the screen back once the game is killed.
* `machine.uefi.recovery`: keep. The installed disk's recovery entry, armed on the host for one boot as grub-editenv arms it, boots by OVMF from partition 2 and answers SSH, and its boot consumed the trial.
* `machine.uefi.nomenu`: keep. With the menu deleted from the EFI system partition, the file GRUB's EFI image opens first, the image finds the recovery partition by its label and opens the menu there, and the recovery system comes up by OVMF and answers SSH.
* `machine.uefi.stick`: keep. OVMF boots the install stick's image, on a USB stick that boots first, from its EFI boot image, an MBR partition of type 0xEF, and the stick's recovery system answers SSH.
* `machine.uefi.cd`: keep. OVMF boots the recovery ISO, a CD, from its EFI El Torito entry, and the recovery system answers SSH.
* `machine.uefi.core`: keep. The installed GPT disk booted by SeaBIOS through its BIOS core: bootctl grub writes GRUB's GPT core into the BIOS boot partition's tail not in use and points the entry sector at it, leaving the table's sectors as they were; with the other tail zeroed on the host the disk boots by SeaBIOS through the new tail; a second install goes back to the first tail and is shown the same way; and the disk still boots by OVMF, whose path does not use the core.
* `machine.uefi.wide`: keep. The install stick, booted by OVMF beside a sparse 4 TiB NVMe disk, installs onto that disk and grows its root into the whole of it; OVMF then boots the NVMe disk alone, the first account logs in with the key the install took and finds the root on the NVMe disk larger than 2 TiB; and the kernel given on QEMU's command line with rach2.root=ext2 finds that root, whose entry runs past 2 TiB, by its scan of the table's first four entries.
* `machine.uefi.wide-bios`: keep. On i686, whose kernel does not reach the NVMe controller's BAR that OVMF places above 4 GiB, the same install by SeaBIOS: the stick installs onto a sparse 4 TiB NVMe disk and grows its root into the whole of it; SeaBIOS then boots the NVMe disk alone through its BIOS core, the first account finds the root on it larger than 2 TiB; and rach2.root=ext2 finds that root by the scan of the table's first four entries.
* `machine.uefi.damaged`: keep. A GPT disk whose primary header does not check, beside the install stick booted by SeaBIOS: the recovery system names the disk as read from its backup table, diskctl relocate writes the primary again, whose header then checks, and the disk boots by SeaBIOS with its kernel reading the primary table.

## Validation still required

The source changes have strict native Mac syntax checks for the changed C controllers and fake API assets, shell syntax checks for changed GNU and agent workloads, JSON parsing and a clean whitespace diff. These checks do not establish runtime equivalence. The coordinator will run preparation and the affected families together on the integrated candidate. The Antigravity acceptance run must reach the trust dialog and pass the transferred settings-file check. The retained bootline, sealed, installation-medium and GNU rows must exercise their strengthened assertions. Native inventories and immutable artifact records must be regenerated after the deleted cases change selection.
