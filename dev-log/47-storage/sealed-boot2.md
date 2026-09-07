# The sealed suite's second boot: what is known

## The symptom

`tests/run sealed` fails with `sealed-test: FAIL: the machine came back but never answered SSH (boot 2)`.
Every other suite passes: metal, drill, drill-ext2, large, grow, parts, the host tier, style.

The machine's own console (`build/images/rach2-sealed.disk.log.fault`, which the first
QEMU keeps writing across restarts) shows, on the second boot:

    rach2 login: init: network: ifup -a did not configure the interface (status -1)

`status -1` is from an instrumented `utils/init.c`: it means `WIFEXITED(status)` was
false, so BusyBox `ifup` did not exit but was killed by a signal. On the first boot of
the same machine `udhcpc` runs, gets a lease, and SSH answers. On the second boot
`udhcpc` prints nothing at all, so `ifup` dies before it starts the client.

No kernel warning of any kind appears on that boot: no page fault, no out-of-memory,
no device error. The driver comes up normally (`rtl8139: link 100 Mb/s`), and the root
mounts (`root: MBRID=1acef00d,4: /dev/disk0p4, ext2`).

`utils/init.c` prints the signal, and the run of 19:10 gave it: **signal 9**. On this
kernel that is not a killer at work. `exec_fail_destroyed()` in `rach2/sys/syscall.c`
(line 5765) sets `exit_code = SIGKILL & 0x7F` when an `execve` fails after
`free_user_pages` has destroyed the caller's image, so the parent sees a death by
signal nine. So `ifup` did not run: the exec of its program failed part way through
loading, and BusyBox's image was already gone.

Boot 1 runs the KRFFS root and boot 2 the ext2 root, so this is an exec reading its
program from ext2 and failing. Other execs on that same boot succeed (dropbear
starts), so it is not every load. `exec_fail_destroyed` now takes a step number and
logs `exec: pid N died: the image was destroyed and step S failed`; step 1 is
`elf_load_image`, steps 2 and 3 are the stack. That kernel is built but the run that
would print it has not been made yet.

## What has been ruled out, each by direct test

* Not a stale interface-state file. `/var/run/ifstate` is unlinked by init before
  `ifup` runs, and both roots carry that init.
* Not the ext2 root. Booting that root directly and running `ifup -a` by hand gets a
  lease and exits zero.
* Not the restart itself. A restart driven at the console, and one sent over SSH,
  both come back with the network up: `scratchpad/boot2.sh` does this in 40 seconds.
* Not the kernel install or the trial. `boot2.sh install` copies the marked kernel,
  runs `bootctl kernel`, attempts a commit, reads the status, restarts, and boot 2
  comes up on the candidate ext2 root with SSH answering in 1 to 2 seconds.
* Not the one-shot GRUB entry. Arming `next_entry=fallback` by hand, and with the
  suite's own arming code, both make the machine boot the KRFFS fallback root.
* Not the boot-partition checks. Those were separate failures, since fixed.

So the difference between the harness that works and the suite that fails is still
unidentified. The suite does more before its restart than `boot2.sh` does.

## Where to look

* `tests/system/suites/sealed.sh`, from `phase "first cold boot"` at line 310 through
  `reboot_machine 2` at line 377. Everything the suite sends to the machine in that
  window is a candidate. `boot2.sh` replicates only part of it.
* `utils/init.c`, `configure_network()` at line 343: forks `ifup -a`, waits, reports.
* The console log is `build/images/rach2-sealed.disk.log.fault`; the suite's own
  output goes to the runner. A rerun overwrites the log, so read it first.

## The fast reproduction

`/private/tmp/claude-501/-Users-toksaitov-Desktop-rach2/efb96526-e443-41d8-b412-58860e87b0c4/scratchpad/boot2.sh`
takes a command to run on boot 1 and an optional `arm-after`, boots the sealed disk,
waits for SSH, restarts the machine, waits again, and prints the second boot's network
lines. About 40 seconds a run against the suite's 12 minutes. Extending it with the
rest of the suite's boot-1 commands, one at a time, should find which one leads to the
dead network.

## The state of the milestone

Everything else in milestone 47 is done and committed through `7e136b1`: the five
suites that failed the full set, all nineteen Codex review findings, and the disk-path
speed-up. What remains for the close is this failure, then the milestone Readme with
its hashes and the index entry. The handoff section in `dev-log/47-storage/plan.md`
carries the rest.

## Diagnosis and fix, 2026-09-07

`elf_load_image()` in `rach2/sys/proc/elf.c` kept `phdrs[ELF_MAX_PHNUM]` static. The argument workspace in `sys_execve()` was already per process, but the loader's program headers were still shared. Init starts the console login and update process before it forks `ifup`; their execs can overlap. An ext2 cache miss follows `exec_vfs_read` -> `vfs_read` -> `ext2_read` -> `read_data` -> `read_bytes` -> `bcache_get` -> `block_request` -> `req_run_one`, which sleeps in `wq_wait_event_nointr`. A second exec overwrites the suspended loader's headers. `in_syscall` prevents timer preemption, but does not prevent this explicit wait.

The serial variant of the two-boot harness reproduced step 1 failures in login and Dropbear. Instrumentation compared the program headers across each segment read and reported:

```text
elf: headers changed: pid 2 segment 0 off 4096 got 4096 wanted 4096
elf: header: before vaddr 8048000 offset 0 size 155ec; after vaddr 8048000 offset 0 size dbf4c
exec: read: pid 2 off 901120 wanted 531 got 0
elf: reject: pid 2 line 472
exec: load failed: pid 2 name bin/login
exec: pid 2 died: the image was destroyed and step 1 failed
elf: headers changed: pid 18 segment 0 off 49152 got 4096 wanted 4096
elf: header: before vaddr 8048000 offset 0 size 4f9d0; after vaddr 8048000 offset 0 size 155ec
elf: reject: pid 18 line 380
exec: load failed: pid 18 name bin/dropbearmulti
exec: pid 18 died: the image was destroyed and step 1 failed
```

The line numbers above belong to the instrumented source. Line 472 rejects a segment read shorter than requested; line 380 rejects a segment that precedes the previous segment's page-aligned end. The first read that observed changed headers completed all 4096 bytes. The later zero-byte read was ext2's correct EOF answer to an offset taken from another executable's headers. `i686-elf-readelf -l` confirms the sizes above in the staged login, BusyBox and Dropbear binaries; offset 901120 (`0xdc000`) and length 531 (`0x213`) describe BusyBox's second load segment, which login was incorrectly trying to read from its own file. This is a shared-loader-state defect exposed by sleeping disk reads, not evidence of corrupt disk data or an incorrect ext2 EOF return.

The fix makes the program-header array automatic: 32 entries of 32 bytes, private to each call on its process's 16 KiB kernel stack. The temporary comparison and rejection instrumentation was removed; the existing step-numbered `exec_fail_destroyed()` diagnostic remains.

The host ELF regression completes a second load inside the first load's segment-read callback, then checks both images' segment addresses, program-header address and data. It passes with private headers, fails with exit status 5 when the array is restored to static, and passes after restoration and recompilation, under AddressSanitizer and UndefinedBehaviorSanitizer.

The original SSH harness could not start in the Codex sandbox: QEMU could not establish its forwarding rule, and a localhost bind returned `Operation not permitted`. All reproduction files were kept under `build/boot2-debug/`. `serial-boot2.py` uses a private copy of the sealed disk, stages the built kernel on its FAT partition while QEMU is stopped, and performs `bootctl status`, the mount and command-line reads, the kernel install, the refused early commit, status and `sync; reboot -f` through the serial login. It preserves AHCI, user networking, DHCP and the GRUB trial boot. In these runs both boots mounted the ext2 root, correcting the earlier assumption that boot 1 necessarily used KRFFS.

Two runs of the same serial variant with the fixed kernel reached login, acquired a DHCP lease and started Dropbear without a step-numbered exec failure. The complete style checker passed using a copy of `tools/verify/check.sh` whose repository and scratch paths point inside this repository. The full host suite was not run because several of its fixtures use fixed paths under `/tmp`; the changed ELF host test was compiled and run separately with the suite's sanitizer flags. The full SSH-driven `tests/run sealed` remains unverified in this sandbox; a serial boot does not establish that the full suite passes. The before-and-after console logs and the mutation output are under `build/boot2-debug/`.
