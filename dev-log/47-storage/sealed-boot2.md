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
