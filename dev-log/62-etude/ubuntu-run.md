# The whole collection on Ubuntu

This is the record of T90 of [the plan](plan.md), #679: the whole collection run on the testing PC's Ubuntu under TCG, on 2026-10-22 and 23. KVM does not work in that Ubuntu (#878), and the owner dropped the run under KVM.

The collection ran first on `2470763f819d480ad6339bd6861619fc10354317`, the commit the plan first fixed for every environment. i686 passed whole; x86_64 failed two cases, one of them again when run alone. It then ran on the fixed candidate `8d54b55ea10d92e2e84693c7d374c669249d4a37`, the first commit to pass the whole collection on both architectures (#888), which is the Lenovo's candidate. i686 passed whole. x86_64 failed only the two `codexapi` cases, because the host's Codex had been upgraded past the version they pin (#928), and both passed on the same build once the pinned version was back. Times are UTC.

## Machine and build

| Item | Value |
| --- | --- |
| Machine | The testing PC, `SFW-DPC`: AMD Ryzen 9 7950X3D, 16 cores and 32 threads, 125.8 GiB of memory |
| System | Ubuntu 26.04.1 LTS in WSL2, Linux 6.18.54.1-microsoft-standard-WSL2 |
| QEMU | 10.2.1 (Debian 1:10.2.1+ds-1ubuntu3.2), `qemu-system-i386` and `qemu-system-x86_64` |
| Compiler | GCC 15.2.0 (Ubuntu 15.2.0-16ubuntu1), for the host and, through `/usr/local/bin/i686-elf-gcc` and `x86_64-elf-gcc`, for the kernel |
| Admission | The installed launcher of [the Ubuntu enrollment](ubuntu.md), shared with the other accounts and ci |
| Checkout | The account's own clone, held at the candidate with no change for the whole of the runs |
| Width | `RACH2_WORKERS=16`, `RACH2_MEMORY_MIB=32768`, 2 processors to each machine |
| Free disk before the runs on 2470763f8 | 155 GiB of 1007 GiB |

For 2470763f8 the build tree was warm: it held the build of an earlier commit of the same day, which each architecture's first run brought up to the candidate in its preparation. The i686 GNU root was missing and was built first, alone, as the one-time setup that the budget leaves out: `make -j12 build/images/gnu-test.disk` took 2595 s. The x86_64 GNU root was there, and its three images took 62 s.

For 8d54b55ea the same clone held the build of 09cd650ee, the previous candidate. The i686 run's preparation brought it up to the candidate in 188 s. The x86_64 run's preparation took 3259 s, most of it rebuilding the x86_64 GNU root, whose inputs had changed; the budget counted all of it as the run's preparation, not as one-time setup. Between the two days the host's unattended upgrades changed some packages, among them rust-coreutils, and ccache was installed; QEMU and GCC stayed the versions above.

## Runs

| Run | Commit | Architecture, accelerator | Start | Seconds | Preparation s | Processor s | Mean width | Items | Result |
| --- | --- | --- | --- | ---: | ---: | ---: | ---: | --- | --- |
| `tests.i686.20261022T094830Z.1056105` | 2470763f8 | i686, KVM | 10-22 09:48:36 | 818 | 272 | 1455 | 1.48 | 584: 62 passed, 245 failed, 277 incomplete | no machine could use KVM |
| `tests.i686.20261022T100344Z.2790428` | 2470763f8 | i686, TCG | 10-22 10:03:52 | 2695 | 2 | 13606 | 5.22 | 584 passed | pass |
| `tests.x86_64.20261022T104935Z.3497832` | 2470763f8 | x86_64, TCG | 10-22 10:49:42 | 3024 | 316 | 13089 | 5.42 | 609: 607 passed, 2 failed | failure |
| `tests.x86_64.20261022T114128Z.3436439` | 2470763f8 | x86_64, TCG | 10-22 11:41:29 | 1220 | 2 | 152 | 1.07 | 3: 2 passed, 1 failed | failure |
| `tests.i686.20261022T213635Z.2719254` | 8d54b55ea | i686, TCG | 10-22 21:36:52 | 2991 | 188 | 14631 | 5.00 | 585 passed | pass |
| `tests.x86_64.20261022T223205Z.2295779` | 8d54b55ea | x86_64, TCG | 10-22 22:32:17 | 5508 | 3259 | 37043 | 2.80 | 610: 608 passed, 2 failed | failure |
| `tests.x86_64.20261023T045218Z.108211` | 8d54b55ea | x86_64, TCG | 10-23 04:52:18 | 27 | 21 | 143 | 0.27 | 2 passed | pass |

The full runs are each one `tests/run all`. The fourth run is `tests/run agents sealed` with `RACH2_AGENTS_ROWS=muse-tmux`, which runs the two cases that failed again, with the other case of `sealed`; the last is `tests/run codexapi`, on the build of the full x86_64 run before it. Seconds are the run record's interval, from the first grant of capacity to the end of cleanup. time(1) around the whole command measured 19 s more for each full run on 2470763f8, the launcher's admission and the runner's own start; on 8d54b55ea it measured 29 s more for i686 and 281 s more for x86_64, whose runner started 263 s after the command, in the launcher's admission. The mean width is the run's useful concurrency: the number of cases running, integrated over the interval and divided by it; each full run reached the 16 it was given at some time. The full x86_64 run on 8d54b55ea spent 59% of its interval in preparation, with no case running; over the rest its mean width was 6.85. Processor seconds are those of the cases' process trees, QEMU included, and of the preparation.

Every attempt of every run confirmed its cleanup, and no run left a claim in the machine's ledger. No case was retried or cancelled.

On 2470763f8 the i686 run under TCG reported 7630 assertions, all passed. The x86_64 run reported 7937, of which 7930 passed. `machine.agents.muse-tmux` failed its `application` assertion and did not run the two after it; `machine.sealed.fault` passed every assertion of its scenario. Each of the two then exited 1, failed `etude.termination`, and left a report with no end record, which counts as missing. The first run's assertions are those of 584 cases that never booted: 1486 passed, 592 failed, 1148 not run and 4512 missing.

On 8d54b55ea each architecture has one case more than on 2470763f8, `machine.ext2.busy`. The i686 run reported 7639 assertions, all passed. The full x86_64 run reported 7946, of which 7934 passed: each `codexapi` case failed its `version` assertion and `etude.termination`, and the two left their 8 other assertions not run. The run of `codexapi` alone reported the same 18 assertions of the two cases, all passed. So every one of the 7946 assertions of x86_64 passed on 8d54b55ea, 7928 of them in the full run and 18 in the run after it.

## The time of a run

On 2470763f8 the collection took 5719 s on this machine under TCG: 2695 s for i686 and 3024 s for x86_64, run one after the other, the second with its two failures. Two parts of each run decide most of it:

* The `style` and `host` suites run first with nothing beside them. Machines started 578 s into the i686 run and 864 s into the x86_64 run, whose preparation took 316 s of that.
* One case then decides when the run ends. `machine.gnu.autotools`, which configures and compiles a program inside the GNU system, ran 1949 s on i686 and ended the run; on x86_64 it ran 1503 s, and `machine.sealed.fault`, which waited for its machine to end until its bound expired, ran 1723 s and ended the run ten minutes after it.

On 8d54b55ea the collection took 8499 s: 2991 s for i686 and 5508 s for x86_64. Without the 3259 s of the x86_64 run's preparation, which rebuilt the GNU root, it took 5240 s, against 5719 s on 2470763f8, whose i686 GNU root was built before its run. The same two parts decide it:

* Machines started 752 s into the i686 run, 188 s of it preparation, and 3803 s into the x86_64 run, 544 s after its preparation ended.
* `machine.gnu.autotools` ended both runs: it ran 2041 s on i686 and 1623 s on x86_64. With #885 fixed, `machine.sealed.fault` ran 597 s on x86_64.

The run under KVM gives no time of the collection, since none of its machines ran.

## Failures

On 2470763f8:

* KVM: QEMU cannot open `/dev/kvm` in this WSL, where Linux sees no SVM in the processor, although `.wslconfig` asks for nested virtualization. Every machine of the first run ended at its start with `accel_failed`, and etude did not fall back to another accelerator, as the design requires. #878 has the evidence and what would fix it on the Windows side.
* `machine.sealed.fault` on x86_64, in both runs: an SSH command that should end the machine, `sync; poweroff -f` in the full run and `sync; reboot -f` in the rerun, left it running and idle until the case's bound expired. #885.
* The scenario of that case exited 1 in the full run without saying why, since its failure came after its last assertion and `netcase.c` reports a failure only through the next one. #886.
* `machine.agents.muse-tmux` on x86_64, in the full run only: the scripted session finished, but Muse showed its exit dialog and did not end after the row's two Ctrl-C presses. The row waits for any change of the screen between them, which depends on the machine's speed. It passed alone in the rerun. #887.

On 8d54b55ea:

* `host.codexapi.exchange` and `host.codexapi.refusal` on x86_64, in the full run: the host's Codex said `codex-cli 0.163.0`, and the cases pin 0.162.1, so each failed its `version` assertion and exited 1. The host's Codex had been upgraded the evening before; once it was back at 0.162.1, both cases passed alone on the same build. #928.

## Cost by family

The cost of a family is the sum of its cases' elapsed seconds, and its processor seconds, in a full run under TCG. A dash marks a family that does not run on that architecture. The first table is of the runs on 2470763f8.

| Family | i686 cases | i686 s | i686 processor s | x86_64 cases | x86_64 s | x86_64 processor s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `machine.gnu` | 70 | 5488 | 7345 | 70 | 2856 | 3448 |
| `machine.sealed` | 2 | 550 | 432 | 2 (1 failed) | 1735 | 412 |
| `machine.installer` | 7 | 831 | 455 | 7 | 992 | 421 |
| `guest.default` | 121 | 896 | 449 | 120 | 916 | 443 |
| `machine.agentsaccept` | - | - | - | 4 | 1717 | 1625 |
| `machine.agents` | - | - | - | 14 (1 failed) | 1475 | 1345 |
| `machine.selfbuild` | 3 | 740 | 686 | 3 | 697 | 641 |
| `machine.installer-repair` | 5 | 686 | 366 | 5 | 649 | 351 |
| `machine.grow` | 9 | 453 | 302 | 9 | 571 | 271 |
| `machine.diskrate` | 2 | 351 | 122 | 2 | 496 | 99 |
| `machine.uefi` | 8 | 370 | 356 | 8 | 379 | 366 |
| `machine.install` | 1 | 310 | 170 | 1 | 298 | 168 |
| `host.host` | 6 | 261 | 692 | 6 | 283 | 779 |
| `host.style` | 25 | 297 | 304 | 25 | 225 | 228 |
| `machine.man` | 2 | 264 | 261 | 2 | 157 | 158 |
| `guest.stress` | 1 | 121 | 30 | 1 | 207 | 37 |
| `machine.doom` | 1 | 156 | 154 | 1 | 156 | 157 |
| `machine.installer-rehearse` | 1 | 163 | 61 | 1 | 147 | 67 |
| `guest.stress-ahci` | 1 | 108 | 110 | 1 | 199 | 204 |
| `machine.selfbuild-ext2` | 2 | 145 | 83 | 2 | 147 | 88 |
| `machine.etudeexec` | 1 | 155 | 150 | 1 | 91 | 91 |
| `machine.mount` | 15 | 117 | 22 | 15 | 118 | 29 |
| `machine.drill` | 3 | 104 | 68 | 3 | 118 | 66 |
| `machine.metal` | 8 | 128 | 113 | 8 | 83 | 76 |
| `machine.dhcp` | 10 | 76 | 20 | 10 | 80 | 25 |
| `machine.bus` | 4 | 58 | 36 | 4 | 88 | 54 |
| `machine.large` | 3 | 60 | 54 | 3 | 75 | 56 |
| `machine.ext2` | 20 | 63 | 35 | 20 | 65 | 40 |
| `machine.smp` | 7 | 58 | 64 | 7 | 65 | 87 |
| `machine.menu` | 1 | 61 | 62 | 1 | 62 | 62 |
| `guest.queue` | 3 | 51 | 41 | 3 | 62 | 60 |
| `machine.parts` | 16 | 43 | 42 | 16 | 49 | 48 |
| `guest.noentropy` | 4 | 40 | 17 | 4 | 44 | 20 |
| `guest.authority` | 4 | 45 | 17 | 4 | 35 | 16 |
| `machine.usb` | 31 | 38 | 35 | 31 | 37 | 34 |
| `machine.usbdisk` | 15 | 39 | 31 | 15 | 37 | 33 |
| `machine.fatwrite` | 2 | 25 | 8 | 2 | 47 | 11 |
| `machine.bigmem` | - | - | - | 3 | 69 | 70 |
| `machine.netif` | 5 | 33 | 4 | 5 | 34 | 5 |
| `guest.disk-ahci` | 1 | 27 | 26 | 1 | 40 | 40 |
| `machine.krffsmount` | 9 | 24 | 11 | 9 | 42 | 12 |
| `guest.disk-q35` | 1 | 27 | 26 | 1 | 39 | 39 |
| `machine.loopdev` | 6 | 26 | 8 | 6 | 40 | 13 |
| `machine.grub` | 1 | 30 | 21 | 1 | 35 | 19 |
| `guest.disk` | 1 | 24 | 6 | 1 | 35 | 5 |
| `host.drive` | 1 | 28 | 2 | 1 | 28 | 1 |
| `machine.ssh` | 1 | 34 | 6 | 1 | 21 | 6 |
| `machine.ssh-e1000` | 1 | 33 | 6 | 1 | 19 | 6 |
| `machine.exfat` | 3 | 21 | 7 | 3 | 32 | 10 |
| `machine.fakeapi` | - | - | - | 1 | 50 | 53 |
| `host.runner` | 4 | 25 | 14 | 4 | 24 | 13 |
| `guest.subset` | 7 | 24 | 14 | 7 | 23 | 16 |
| `machine.account` | 2 | 24 | 12 | 2 | 23 | 10 |
| `machine.memory` | 13 | 15 | 15 | 13 | 26 | 26 |
| `host.imagerace` | 5 | 26 | 15 | 5 | 13 | 11 |
| `guest.fpu` | 7 | 22 | 17 | 5 | 15 | 11 |
| `guest.ping` | 1 | 21 | 11 | 1 | 16 | 7 |
| `guest.ping-e1000` | 1 | 20 | 10 | 1 | 14 | 5 |
| `machine.addressed` | 1 | 18 | 1 | 1 | 14 | 1 |
| `machine.clock` | - | - | - | 1 | 31 | 22 |
| `machine.make` | 1 | 14 | 14 | 1 | 16 | 15 |
| `machine.persist-ahci` | 1 | 15 | 13 | 1 | 12 | 12 |
| `guest.fdlock` | 3 | 11 | 7 | 3 | 14 | 11 |
| `machine.procfiles` | 4 | 12 | 8 | 4 | 13 | 10 |
| `machine.login` | 4 | 13 | 5 | 4 | 10 | 3 |
| `machine.shutdown` | 8 | 8 | 8 | 8 | 13 | 13 |
| `guest.fault` | 3 | 11 | 7 | 3 | 9 | 5 |
| `machine.diskdiag` | 1 | 10 | 10 | 1 | 9 | 8 |
| `machine.bootorder` | 1 | 9 | 9 | 1 | 9 | 9 |
| `machine.bootpart` | 4 | 8 | 7 | 4 | 10 | 9 |
| `guest.textbusy` | - | - | - | 2 | 17 | 7 |
| `machine.netif-e1000` | 2 | 7 | 1 | 2 | 7 | 2 |
| `machine.audio` | 2 | 7 | 2 | 2 | 7 | 2 |
| `machine.dhcp-e1000` | 1 | 7 | 3 | 1 | 7 | 3 |
| `guest.pools` | 2 | 7 | 5 | 2 | 6 | 4 |
| `host.museapi` | - | - | - | 2 | 11 | 3 |
| `machine.tcc` | 1 | 5 | 4 | 1 | 7 | 5 |
| `machine.disks` | 1 | 6 | 5 | 1 | 5 | 5 |
| `machine.bootline` | 10 | 6 | 5 | 9 | 4 | 4 |
| `machine.servicelog` | 1 | 6 | 5 | 1 | 4 | 4 |
| `machine.conload` | 1 | 6 | 6 | 1 | 3 | 3 |
| `host.codexapi` | - | - | - | 2 | 9 | 2 |
| `machine.exitline` | 4 | 5 | 4 | 4 | 3 | 3 |
| `machine.raw` | 4 | 4 | 3 | 4 | 4 | 3 |
| `machine.etudeguest` | 1 | 5 | 4 | 1 | 3 | 2 |
| `guest.packet-e1000` | 1 | 4 | 2 | 1 | 4 | 3 |
| `guest.packet` | 1 | 4 | 2 | 1 | 3 | 3 |
| `guest.diskfull` | 1 | 3 | 2 | 1 | 3 | 2 |
| `machine.reset` | 4 | 4 | 3 | 4 | 2 | 2 |
| `machine.crash` | 4 | 3 | 3 | 4 | 2 | 2 |
| `host.krffscheck` | 8 | 4 | 2 | 8 | 2 | 1 |
| `machine.cred-persist` | 1 | 2 | 2 | 1 | 3 | 3 |
| `machine.load` | 2 | 2 | 2 | 2 | 3 | 2 |
| `machine.e1000` | 3 | 3 | 2 | 3 | 2 | 2 |
| `machine.diskpolicy` | 2 | 3 | 3 | 2 | 2 | 2 |
| `machine.raw-e1000` | 1 | 2 | 1 | 1 | 2 | 1 |
| `machine.netconsole-e1000` | 2 | 2 | 2 | 2 | 1 | 1 |
| `machine.console` | 2 | 2 | 2 | 2 | 1 | 1 |
| `machine.socket` | 2 | 2 | 2 | 2 | 1 | 1 |
| `machine.persist` | 1 | 2 | 2 | 1 | 1 | 1 |
| `machine.netconsole` | 2 | 2 | 2 | 2 | 1 | 1 |
| `machine.dgram-e1000` | 1 | 2 | 1 | 1 | 1 | 1 |
| `machine.dgram` | 1 | 1 | 1 | 1 | 1 | 1 |
| All families | 584 | 14084 | 13600 | 609 | 16389 | 12690 |

The second table is of the full runs on 8d54b55ea. Its `host.codexapi` row is that of the full x86_64 run; the run of the two cases alone took 7 s and 2 processor s.

| Family | i686 cases | i686 s | i686 processor s | x86_64 cases | x86_64 s | x86_64 processor s |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| `machine.gnu` | 70 | 6267 | 7986 | 70 | 3193 | 3678 |
| `guest.default` | 121 | 1064 | 512 | 120 | 1035 | 506 |
| `machine.agentsaccept` | - | - | - | 4 | 1696 | 1556 |
| `machine.selfbuild` | 3 | 753 | 695 | 3 | 737 | 671 |
| `machine.agents` | - | - | - | 14 | 1443 | 1343 |
| `machine.installer-repair` | 5 | 659 | 387 | 5 | 661 | 351 |
| `machine.sealed` | 2 | 579 | 415 | 2 | 609 | 366 |
| `machine.diskrate` | 2 | 521 | 111 | 2 | 509 | 97 |
| `machine.uefi` | 8 | 410 | 395 | 8 | 429 | 353 |
| `machine.installer` | 7 | 433 | 442 | 7 | 384 | 376 |
| `machine.grow` | 9 | 370 | 303 | 9 | 432 | 245 |
| `machine.install` | 1 | 368 | 175 | 1 | 398 | 165 |
| `host.host` | 6 | 279 | 762 | 6 | 259 | 705 |
| `guest.stress` | 1 | 245 | 34 | 1 | 250 | 36 |
| `host.style` | 25 | 222 | 230 | 25 | 259 | 267 |
| `guest.stress-ahci` | 1 | 163 | 166 | 1 | 249 | 248 |
| `machine.man` | 2 | 224 | 224 | 2 | 168 | 167 |
| `machine.doom` | 1 | 156 | 155 | 1 | 157 | 156 |
| `machine.selfbuild-ext2` | 2 | 143 | 89 | 2 | 142 | 85 |
| `machine.etudeexec` | 1 | 137 | 136 | 1 | 105 | 103 |
| `machine.bus` | 4 | 134 | 76 | 4 | 100 | 58 |
| `machine.mount` | 15 | 116 | 21 | 15 | 117 | 22 |
| `machine.large` | 3 | 60 | 51 | 3 | 162 | 54 |
| `machine.drill` | 3 | 102 | 71 | 3 | 106 | 68 |
| `machine.usb` | 31 | 39 | 36 | 31 | 166 | 36 |
| `machine.metal` | 8 | 102 | 98 | 8 | 90 | 81 |
| `guest.queue` | 3 | 85 | 82 | 3 | 70 | 64 |
| `machine.dhcp` | 10 | 76 | 20 | 10 | 76 | 20 |
| `machine.ext2` | 21 | 66 | 36 | 21 | 64 | 29 |
| `machine.menu` | 1 | 64 | 64 | 1 | 62 | 61 |
| `machine.installer-rehearse` | 1 | 57 | 51 | 1 | 62 | 53 |
| `guest.disk-ahci` | 1 | 65 | 62 | 1 | 52 | 49 |
| `guest.disk-q35` | 1 | 64 | 62 | 1 | 51 | 49 |
| `machine.krffsmount` | 9 | 28 | 12 | 9 | 79 | 12 |
| `machine.smp` | 7 | 56 | 68 | 7 | 50 | 65 |
| `guest.disk` | 1 | 58 | 5 | 1 | 48 | 5 |
| `machine.parts` | 16 | 46 | 43 | 16 | 49 | 44 |
| `guest.noentropy` | 4 | 49 | 19 | 4 | 44 | 14 |
| `machine.grub` | 1 | 47 | 20 | 1 | 44 | 19 |
| `machine.loopdev` | 6 | 41 | 10 | 6 | 47 | 9 |
| `machine.fatwrite` | 2 | 40 | 8 | 2 | 44 | 7 |
| `machine.exfat` | 3 | 33 | 7 | 3 | 37 | 6 |
| `machine.usbdisk` | 15 | 34 | 31 | 15 | 33 | 28 |
| `machine.netif` | 5 | 33 | 4 | 5 | 33 | 4 |
| `host.drive` | 1 | 28 | 1 | 1 | 28 | 1 |
| `guest.subset` | 7 | 25 | 16 | 7 | 26 | 17 |
| `guest.authority` | 4 | 23 | 17 | 4 | 28 | 15 |
| `machine.account` | 2 | 24 | 12 | 2 | 24 | 11 |
| `host.runner` | 4 | 23 | 13 | 4 | 23 | 12 |
| `guest.fpu` | 7 | 27 | 19 | 5 | 15 | 10 |
| `machine.ssh` | 1 | 19 | 5 | 1 | 20 | 4 |
| `machine.ssh-e1000` | 1 | 20 | 5 | 1 | 19 | 4 |
| `machine.fakeapi` | - | - | - | 1 | 38 | 41 |
| `machine.memory` | 13 | 18 | 17 | 13 | 17 | 15 |
| `host.imagerace` | 5 | 18 | 14 | 5 | 15 | 12 |
| `machine.raw` | 4 | 5 | 4 | 4 | 28 | 3 |
| `machine.clock` | - | - | - | 1 | 31 | 22 |
| `machine.procfiles` | 4 | 16 | 10 | 4 | 15 | 9 |
| `guest.ping-e1000` | 1 | 15 | 5 | 1 | 15 | 7 |
| `machine.bigmem` | - | - | - | 3 | 27 | 26 |
| `guest.ping` | 1 | 13 | 4 | 1 | 13 | 4 |
| `machine.make` | 1 | 15 | 14 | 1 | 10 | 10 |
| `machine.persist-ahci` | 1 | 12 | 12 | 1 | 11 | 11 |
| `guest.fdlock` | 3 | 12 | 8 | 3 | 11 | 8 |
| `machine.addressed` | 1 | 10 | 1 | 1 | 11 | 1 |
| `guest.fault` | 3 | 11 | 7 | 3 | 9 | 6 |
| `machine.login` | 4 | 10 | 3 | 4 | 10 | 2 |
| `host.museapi` | - | - | - | 2 | 18 | 2 |
| `machine.diskdiag` | 1 | 9 | 9 | 1 | 9 | 8 |
| `machine.bootorder` | 1 | 9 | 9 | 1 | 9 | 9 |
| `machine.shutdown` | 8 | 9 | 8 | 8 | 8 | 8 |
| `guest.textbusy` | - | - | - | 2 | 17 | 8 |
| `machine.bootpart` | 4 | 8 | 8 | 4 | 8 | 8 |
| `machine.load` | 2 | 7 | 4 | 2 | 7 | 4 |
| `machine.audio` | 2 | 7 | 2 | 2 | 6 | 1 |
| `machine.dhcp-e1000` | 1 | 7 | 3 | 1 | 7 | 3 |
| `machine.netif-e1000` | 2 | 7 | 1 | 2 | 7 | 1 |
| `guest.pools` | 2 | 7 | 5 | 2 | 6 | 4 |
| `guest.packet` | 1 | 8 | 7 | 1 | 3 | 2 |
| `machine.tcc` | 1 | 5 | 5 | 1 | 5 | 3 |
| `machine.disks` | 1 | 5 | 5 | 1 | 5 | 5 |
| `machine.servicelog` | 1 | 5 | 5 | 1 | 5 | 5 |
| `machine.bootline` | 10 | 5 | 4 | 9 | 5 | 4 |
| `machine.raw-e1000` | 1 | 2 | 1 | 1 | 7 | 1 |
| `machine.conload` | 1 | 4 | 4 | 1 | 4 | 4 |
| `guest.packet-e1000` | 1 | 4 | 3 | 1 | 3 | 2 |
| `host.krffscheck` | 8 | 4 | 1 | 8 | 3 | 1 |
| `machine.exitline` | 4 | 4 | 3 | 4 | 3 | 2 |
| `guest.diskfull` | 1 | 4 | 3 | 1 | 3 | 2 |
| `machine.etudeguest` | 1 | 3 | 3 | 1 | 3 | 2 |
| `machine.diskpolicy` | 2 | 3 | 2 | 2 | 3 | 2 |
| `machine.reset` | 4 | 3 | 2 | 4 | 3 | 2 |
| `machine.crash` | 4 | 2 | 2 | 4 | 2 | 2 |
| `machine.cred-persist` | 1 | 2 | 2 | 1 | 2 | 2 |
| `machine.e1000` | 3 | 2 | 2 | 3 | 2 | 2 |
| `machine.console` | 2 | 2 | 1 | 2 | 2 | 1 |
| `machine.persist` | 1 | 2 | 1 | 1 | 1 | 1 |
| `machine.dgram-e1000` | 1 | 2 | 1 | 1 | 1 | 1 |
| `machine.socket` | 2 | 1 | 1 | 2 | 1 | 1 |
| `machine.netconsole-e1000` | 2 | 1 | 1 | 2 | 1 | 1 |
| `machine.netconsole` | 2 | 1 | 1 | 2 | 1 | 1 |
| `host.codexapi` | - | - | - | 2 (2 failed) | 2 | 1 |
| `machine.dgram` | 1 | 1 | 1 | 1 | 1 | 1 |
| All families | 585 | 14973 | 14469 | 610 | 15420 | 12686 |

## Load from outside

The other accounts' tests and ci ran on the machine throughout, as the shared admission allows. A sampler read the machine each minute during the two full runs under TCG on 2470763f8:

| Measure | i686 run, median and peak | x86_64 run, median and peak |
| --- | --- | --- |
| Load average over one minute | 14.3, 39.6 | 14.1, 27.8 |
| Processor pressure, `some` over 60 s, % | 0.7, 20.3 | 0.6, 3.3 |
| Memory pressure, `some` over 60 s, % | 0.2, 2.1 | 0.5, 2.8 |
| Disk pressure, `some` over 60 s, % | 0.9, 10.6 | 1.1, 12.2 |
| QEMU processes on the machine, every account's | 3, 22 | 7, 17 |
| Other accounts' claims in the ledger | 4, 6 | 3, 5 |

Available memory never fell below 109 GiB. The run records hold no measure of this load, since the set driver does not observe other work and does not measure caches; these samples are the only record of it.

No sampler ran during the runs on 8d54b55ea, so their outside load is not recorded. The other accounts' tests and ci ran beside them, as before.

## Records

Each run's history, the store of its blobs and each case's output, as `RACH2_RESULTS` keeps them, are on the testing host with the run's output and time(1)'s report: the runs on 2470763f8 in `~toksaitov-d/etude-runs/679/`, with the load samples, and those on 8d54b55ea in `~toksaitov-d/etude-runs/888/`, in `i686-tcg-8d54b55ea/`, `x86_64-tcg-8d54b55ea/` and `x86_64-tcg-8d54b55ea-codexapi/`. The workspaces of the failed cases are under `~toksaitov-d/rach2/build/tests/failed/`, named by run and case.

## Use in the comparison

8d54b55ea is the one commit on which both architectures passed on this machine: one run of each, the x86_64 one made of the full run and the run of its two `codexapi` cases after it. The site comparison of [the measurements](measurements.md) asks for five complete passing runs of each configuration before it calls one machine faster than another.
