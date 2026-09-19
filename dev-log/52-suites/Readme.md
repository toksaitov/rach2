# Milestone 52: the suites

Milestone 52 is accepted on 2026-09-19 at the commit that adds this record, from source `8c206859`. All 87 suites passed in each of six alternating sets on the testing host: three under KVM and three under the TCG emulator. The suites judge kernel counts, completion order and equality; waits end on the event they require. The runner distinguishes an idle process group from a silent computation, checks the declared ownership of image files, and runs the formerly separate suites in the pool. The acceptance includes two Codex review rounds and a rescue round. No Lenovo sitting was part of this milestone.

## What was fixed

The ATA driver transfers data through the PIIX bus master and sleeps on the channel's queue while a request is pending. The polled path pauses between status reads so that QEMU can complete the transfer. A caller without the channel waits for both BSY and DRQ to clear before issuing a command; DMA issue and data-port transfers run with interrupts disabled. These changes prevent a polled command from overlapping another caller's DMA. The driver also preserves a failed DMA result, selects the right drive for a flush, and avoids waiting for a channel owner when shutdown has stopped the scheduler. Disk wait ticks are counted in the request layer for every device, and the AHCI driver counts its polls.

Sleeping disk operations exposed faults above the driver. Root KRFFS data operations now take the mount lock, so one process cannot move a node while another writes it. Waiters acquire that lock in request order without a new scheduling point between `open` and descriptor registration. Page write-back preserves changes made while its disk request sleeps. The lag device claims its queue slot before starting the transfer and does not complete a slot whose transfer is unfinished. Ext2 frees blocks in a batch that writes the removed pointers and inode before the bitmaps and group descriptors; the truncate probe checks the command count.

The VGA text driver writes one cell per byte, scrolls by changing the CRTC start address, and updates the cursor once per write. The host test checks cell and port writes, including scrolling and graphics-mode behavior. The debug shell regains control when its init exits; that init ends and reaps the other user processes before returning, so no suspended child can retain a mount lock. An ordinary process exit with nothing runnable idles through the scheduler. `survivorprobe` checks the return to the shell, and its child runs until the kernel ends it. The timing record is judged by `pit_clock_check` on fixed samples, including the 8254 count and status conversion. Delay, wait and walk measurements remain diagnostic output.

The harness waits for tokens. `seen=` accepts a token anywhere since the last send, allowing a prompt and a USB device report to arrive in either order. `tcp=` waits for the server's first bytes. `probe=` ends when its token arrives, retries a command that lost its first bytes at boot, and ends on a specified failure text. `runutil until` waits for a record and `runutil gone` recognizes an exited child before its parent reaps it. Thirty suites replaced sleeps, `wait=` steps or duration judgments. Account and audio commands are sent at the prompts that read them; held-file and pending-read records follow the state they report.

The runner stops a suite only after no suite or console output and processor use below a tenth of a processor over the silent interval. It accounts for reaped children and changes in the process group. The registry declares each suite's output patterns under `build/images`; shared patterns and files without exactly one declared owner fail the run. The usb, diskrate, install and imagerace suites now run in the pool. `style` and `host` still precede the pool. Suite-specific images and fixtures remove shared writes, including the metal suite's temporary that restaged the common test image.

`tools/verify/suitecheck.sh` rejects numeric sleeps, `wait=` steps and probe duration bounds unless the interval has a recorded reason. The interval list has 22 entries and the checker has 27 self-test cases. It also rejects typed lines longer than 240 characters. Completion bounds remain scaled by `RACH2_TIMEOUT_FACTOR`; they do not establish a performance result. The cost review removed the second ext2 kernel build and its duplicate userland work, kept the installer suites as separate parts of one script, and replaced the account suite's paced input with prompt waits.

## What proves it

The closing sets each passed all 87 suites after a clean build of `8c206859`. The 75 native tests passed on both hosts under AddressSanitizer and UndefinedBehaviorSanitizer. They include the new `vga_text_hosttest` and `lag_hosttest`. The default suite has 73 entries: 57 root programs and 16 musl programs, including `survivorprobe`.

The mutation records in [notes.md](notes.md) identify the faults each check detects. Four VGA mutations fail its host test. The old lag wrapper fails three rows; removing its readiness guard fails the row that runs the timer during a transfer. Disabling the serial interrupt fails the console-load row. Restoring the scheduling call in init's exit prevents the shell's return. A clock mutation that counts the same period twice produces a level 3 timing record. The ext2 truncate mutation fails its command bound, and the restored implementation passes.

The first Codex review on 2026-09-18 covered `587c711f..8e422df4` in 13 units. The rescue round used `gpt-6-astra` at `xhigh`, read-only, for 7.7 minutes; it identified the lag-slot race, the orphan exit that halted with interrupts disabled, and the timing walk affected by host scheduling. The second review on 2026-09-19 covered `8e422df4..6e7210f8` in four units and returned 18 findings: 16 fixed and two rejected with reasons in the notes. The rejected proposals were a kernel hook to force the survivor probe's order and a change to the proc probe's idle-tick assertion after the leftover process had been removed. The closing sets include the fixes from both reviews and the rescue round.

## What was accepted

* Acceptance commit: the one that adds this record. The closing source is `8c206859`, which records the second review round; the code's last change is `ce1a133b`.
* Clean build: `build/` emptied except for its vendor tree, then `make -j32` on the testing host, from 07:03:33 to 07:03:38 host time on 2026-09-19.
* Kernel build: the repository defaults, `i686-elf-gcc` with `-ansi -Werror -Wextra -Wdeclaration-after-statement`, without libgcc. Native host tests use `-std=c99 -fsanitize=address,undefined -fno-sanitize-recover=all -g -O1`.
* Runner: default width, 64 workers on this host, with an unscaled idle limit of 300 s. The former list of suites run outside the pool is removed.
* Test commands: `RACH2_QEMU_ACCEL=kvm RACH2_TIMEOUT_FACTOR=4 tests/run all` and `RACH2_QEMU_ACCEL=tcg tests/run all`, alternating, three sets under each accelerator. Every set reported `run: result: all 87 suites passed`.
* Judges: `i686-elf-gcc` wraps `i686-linux-gnu-gcc` (Ubuntu 15.2.0-16ubuntu1) 15.2.0; native `cc` is gcc (Ubuntu 15.2.0-16ubuntu1) 15.2.0. The testing host uses cppcheck 2.19.0, Ubuntu clang-format 21.1.8 (6ubuntu1), ShellCheck 0.11.0, QEMU 10.2.1 (Debian 1:10.2.1+ds-1ubuntu3.2), rsync 3.4.1 and GNU Make 4.4.1, on Ubuntu 26.04.1 LTS with kernel 6.18.33.2-microsoft-standard-WSL2.
* Hardware: the testing host is a Windows machine with an AMD Ryzen 9 7950X3D, 32 threads and 200 GB of memory, running WSL2 Ubuntu with nested KVM. The Apple M4 Mac mini, macOS 27.0 and QEMU 11.1.1, was used for editing, host tests and single-probe boots. This milestone's machine proof is in QEMU; no Lenovo sitting was performed.

SHA-256 of the kernels and images used by the closing sets:

```
e551059c517ef3c5278048e3fe23a15f21165f1fbcb5a078bab9650bd9715152  build/rach2/kernel
8a0f5ea9b1842c6b9c88ae5f0ee4fe54d96a34009a200554f96a83df45660f63  build/rach2-recovery/kernel
1ae4f8b2c2184b0235949548bea4ee0e6d6e0ea9cc6c47a5ea8c995852adddd7  build/rach2-harness/kernel
15dcad306fb6791e8666740ad87bd0e74a7abd8a159fad34a4eba986216258bf  build/rach2-metal/kernel
ef55b47ea971307a4b2e9997a495a7daacd62e1a0f2729f41b30ed25a033aa05  build/images/test.krffs
d4e2a94c6efbcf0c5f60cdc0bf7b6b63075a079bffbbb34ab7da727f8d36b8f7  build/images/os-simple.krffs
f605e47251169ebb1d2ab575b89b3c173e85e89bf33d007462264d7bb28198e0  build/images/os-real.krffs
c435d9171ee5499e53b8d3bb1fc075f8a24c1ff917fbaf904b4e93a86704d44b  build/images/rach2-recovery-with-key.krffs
272e3fb0cd757b5ab43cdc0c08a4b3e9c76e181f00638c6070bf008eefe16e44  build/images/rach2-sealed.disk
```

The six sets ran on 2026-09-19. Start and end are host times; suite-seconds are the sum of suite durations, including work run in parallel.

| Set | Accelerator | Start    | End      | Suites passed | Suite-seconds |
| --- | ----------- | -------- | -------- | ------------- | ------------- |
| a1  | KVM         | 07:03:39 | 07:26:13 | 87            | 8160          |
| a1  | TCG         | 07:26:14 | 07:43:49 | 87            | 7844          |
| a2  | KVM         | 07:43:50 | 08:06:04 | 87            | 8000          |
| a2  | TCG         | 08:06:05 | 08:23:44 | 87            | 7855          |
| a3  | KVM         | 08:23:45 | 08:46:18 | 87            | 8139          |
| a3  | TCG         | 08:46:19 | 09:04:03 | 87            | 7755          |

The per-suite costs below are seconds from the third set under each accelerator. Phase 5 of [plan.md](plan.md#phase-5-the-cuts) requires a reason for the expensive suites; [notes.md](notes.md#phase-5-the-cuts) records the retained work and the cuts. The suites exceeding five minutes under either accelerator perform the following work:

* `sealed` operates the complete disk image over SSH through kernel trials, fallback, root repair, menu and boot-loader replacement, and root restoration. A separate control run checks that an undamaged image never enters fallback or recovery.
* `diskrate` exercises file and raw I/O, small files, small requests and buffer placement across the disk paths. Its command, cache-hit, poll and wait-tick counts distinguish repeated traversal, small transfers and driver polling; the printed rates are not pass criteria.
* `grow` resizes the root, checks every written sector against the computed grow set, checks the resulting file system, and repeats interrupted grows through repair and completion. These rows require recovery boots and file-system checks.
* `runner` tests the supervisor against its own fixture processes. Silent, active and signalled processes exercise idle limits, timeout scaling and cleanup; the test must let those processes progress or reach their bounds.
* `selfbuild` compiles the kernel twice in clean KRFFS workspaces, compares hashes, builds the userland, and boots the kernel it built through the probes. `selfbuild-ext2` keeps one clean kernel build and its boot; the second build, hash comparison and userland checks remain in the KRFFS run.

The three installer suites execute separate parts of one script. The account suite's former 400 s of paced input became prompt waits; its closing cost is 76 s under either accelerator. The stress suites end on the probe's token under a completion budget.

| Suite              | KVM seconds | TCG seconds |
| ------------------ | ----------- | ----------- |
| sealed             | 1004        | 706         |
| diskrate           | 366         | 392         |
| grow               | 321         | 345         |
| runner             | 305         | 305         |
| install            | 299         | 293         |
| selfbuild          | 295         | 577         |
| host               | 290         | 298         |
| installer-repair   | 263         | 261         |
| installer          | 258         | 259         |
| selfbuild-ext2     | 243         | 254         |
| dhcp               | 242         | 229         |
| mount              | 241         | 222         |
| installer-rehearse | 208         | 208         |
| large              | 200         | 178         |
| usb                | 170         | 131         |
| usbdisk            | 165         | 148         |
| krffsmount         | 162         | 158         |
| stress-ahci        | 144         | 135         |
| queue              | 139         | 122         |
| stress             | 135         | 143         |
| drill              | 134         | 137         |
| ext2               | 128         | 112         |
| loopdev            | 109         | 100         |
| parts              | 108         | 89          |
| bus                | 104         | 92          |
| memory             | 101         | 87          |
| default            | 100         | 102         |
| fatwrite           | 96          | 91          |
| netif              | 90          | 81          |
| exfat              | 86          | 81          |
| account            | 76          | 76          |
| menu               | 73          | 66          |
| subset             | 63          | 53          |
| load               | 63          | 60          |
| grub               | 58          | 54          |
| raw                | 50          | 43          |
| bootline           | 50          | 45          |
| bootpart           | 49          | 40          |
| disk-ahci          | 47          | 38          |
| style              | 46          | 46          |
| disk-q35           | 43          | 39          |
| metal              | 39          | 56          |
| conload            | 38          | 43          |
| disk               | 37          | 33          |
| authority          | 37          | 29          |
| ssh                | 35          | 25          |
| ssh-e1000          | 34          | 25          |
| persist-ahci       | 34          | 27          |
| netif-e1000        | 34          | 29          |
| diskdiag           | 33          | 25          |
| dhcp-e1000         | 33          | 31          |
| doom               | 32          | 31          |
| e1000              | 31          | 24          |
| cred-persist       | 31          | 27          |
| shutdown           | 28          | 15          |
| drive              | 28          | 29          |
| make               | 27          | 33          |
| noentropy          | 24          | 19          |
| socket             | 23          | 17          |
| ping               | 23          | 20          |
| addressed          | 23          | 20          |
| ping-e1000         | 22          | 20          |
| console            | 22          | 18          |
| procfiles          | 20          | 12          |
| persist            | 20          | 16          |
| fpu                | 20          | 17          |
| bootorder          | 20          | 16          |
| raw-e1000          | 19          | 16          |
| login              | 19          | 12          |
| diskpolicy         | 19          | 15          |
| tcc                | 17          | 21          |
| disks              | 17          | 13          |
| exitline           | 16          | 7           |
| packet-e1000       | 15          | 12          |
| packet             | 15          | 12          |
| dgram-e1000        | 15          | 11          |
| audio              | 14          | 11          |
| netconsole-e1000   | 13          | 9           |
| netconsole         | 13          | 9           |
| dgram              | 13          | 11          |
| reset              | 12          | 4           |
| fault              | 11          | 9           |
| crash              | 11          | 4           |
| pools              | 10          | 9           |
| diskfull           | 10          | 9           |
| imagerace          | 7           | 6           |
| krffscheck         | 1           | 2           |

The following counts were recorded in sets a1 and a3 under both accelerators on the testing host. The boot driver bounds are at most 100 status reads and two wait ticks per command.

| Set | Accelerator | Boot commands | Status reads/command | Wait ticks/command |
| --- | ----------- | ------------- | -------------------- | ------------------ |
| a1  | KVM         | 139102        | 9                    | 0                  |
| a1  | TCG         | 139096        | 11                   | 0                  |
| a3  | KVM         | 139096        | 9                    | 0                  |
| a3  | TCG         | 139096        | 11                   | 0                  |

The file-operation counts below were equal in all four sampled sets. Each row passed both bounds.

| Operation   | Disk commands | Command bound | Cache hits | Hit bound |
| ----------- | ------------- | ------------- | ---------- | --------- |
| exFAT write | 4621          | 9300          | 130910     | 400000    |
| exFAT reads | 4100          | 9300          | 65531      | 400000    |
| FAT32 reads | 4219          | 14800         | 130943     | 1300000   |

In each sample, the raw USB write transferred 4 MiB in 32 KiB requests with 128 commands, zero ticks in flight per command and no quarantine. The queue suite's fast-reader ordering row passed in each sample.

The load probe reported these counters. Each row processed six lines; the second stage uses the polled path.

| Set | Accelerator | Stage  | Timer | Disk   | Disk wait | Console | Lines |
| --- | ----------- | ------ | ----- | ------ | --------- | ------- | ----- |
| a1  | KVM         | first  | 26288 | 139908 | 0         | 0       | 6     |
| a1  | KVM         | polled | 24897 | 0      | 0         | 20      | 6     |
| a1  | TCG         | first  | 14782 | 169903 | 0         | 0       | 6     |
| a1  | TCG         | polled | 19825 | 0      | 0         | 31      | 6     |
| a3  | KVM         | first  | 24202 | 125715 | 0         | 57      | 6     |
| a3  | KVM         | polled | 19918 | 0      | 0         | 27      | 6     |
| a3  | TCG         | first  | 18100 | 120627 | 0         | 0       | 6     |
| a3  | TCG         | polled | 19890 | 0      | 0         | 28      | 6     |

The USB latency records below give entry, wake and read offsets in microseconds from the drain. Each record reports `msi 1`; the two rows for each set retain their log order.

| Set | Accelerator | Entry us | Wake us | Read us |
| --- | ----------- | -------- | ------- | ------- |
| a1  | KVM         | -112     | 661     | 758     |
| a1  | KVM         | -110     | 703     | 810     |
| a1  | TCG         | -38      | 921     | 1253    |
| a1  | TCG         | -28      | 861     | 1285    |
| a3  | KVM         | -106     | 637     | 723     |
| a3  | KVM         | -99      | 584     | 681     |
| a3  | TCG         | -40      | 917     | 1241    |
| a3  | TCG         | -32      | 925     | 1355    |

The console-load stage A times were printed with an `OK` result. The FAT write row counted commands for a 4 MiB write and its removal.

| Set | Accelerator | Console A ms | FAT write commands | Removal commands |
| --- | ----------- | ------------ | ------------------ | ---------------- |
| a1  | KVM         | 1330         | 1271               | 44               |
| a1  | TCG         | 2130         | 1271               | 44               |
| a3  | KVM         | 1550         | 1272               | 44               |
| a3  | TCG         | 2020         | 1273               | 44               |

The images check establishes that every file made has one declared owner; it cannot identify which suite wrote that file. A stopped hypervisor can stretch the timing record's printed delay, wait and walk, which are not judged. The metal suite and acceptance script retry a connection failure before SSH identification up to three times; a session that began is never repeated. The cause of the one reset seen under the loaded emulator was not established. A full listen backlog is now recorded. The survivor probe's child runs until the kernel ends it, so its order does not depend on processor speed.
