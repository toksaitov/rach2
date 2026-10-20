# The native inventory

This is the native inventory of milestone 62, task T04 of [the plan](plan.md): for every guest program and every suite of [the inventory of assertions](inventory.md), whether it can run natively on the Lenovo, what it needs there, and, when it cannot, the specific reason it needs a machine of QEMU instead. T44 adapts the programs this file marks native, and T93 runs them. The units and their names are those of the inventory; the reading was made at commit `0637897c`.

## The parts

* [The guest programs](native-guest.md): the 103 probes of `tests/Makefile`, the 82 programs of `tests/musl/`, and the eleven checks of `tests/system/runner/runtests.c`.
* The suites and the rows, below: the 101 suites of `tests/registry` as scenarios, and the 88 rows that `gnu`, `agents` and two other scripts run.

The scripts, checkers and fixtures of [the suites' part](inventory-suites.md#the-scripts) and the whole of [the host part](inventory-host.md) run on the controller's host, not on Rach2, so they have no native plan. Two of them already run against the Lenovo: `tests/system/accept/metal_accept.sh`, the release's acceptance, and `tests/system/manual_probes.sh`, which a person runs; `tests/system/accept/agents_accept.sh` runs against it by hand once the owner has signed in to the agents.

## The native machine

A native run is on the Lenovo G50-70, with the installed system as a release leaves it:

* The x86-64 kernel, booted by UEFI from its committed slot with the machine's own boot line. A native case gets no boot of its own, no command-line option and no fresh kernel: the kernel has run since the last boot, and stays up after the case.
* An Intel Core i3-4030U with four logical processors, and 16 GiB of memory.
* One disk, a Samsung SSD 870 EVO of 488397168 sectors on AHCI port 0, with the boot, recovery and root partitions of the accepted system. The root is `os-real-gnu.ext2`, grown to 232.8 GiB and mounted at `/` as ext2. A native run writes the disk only through files on that root; it opens no `/dev/diskN` node for writing.
* A Realtek RTL8168GU on the owner's LAN, one xHCI controller (8086:9c31) with an EHCI beside it, the HDA controller with its codec 0x14f1510f, and a UEFI framebuffer. The machine has no serial port and no `isa-debug-exit` port.
* The services that `init-metal` starts (`utils/init.c`): the console login, `/bin/update`, OpenSSH's `sshd` on port 22, the system log daemon, BusyBox's `ntpd` while `/etc/ntp.conf` names a server, D-Bus's system bus and `crond`. They run beside every case and move the counters the whole machine shares: free memory, the disk's command counts, the network's, the console's and the scheduler's. `ntpd` may step the real-time clock while a case runs.
* The accounts of the GNU root's `/etc/passwd`: `root` 0, `test` 1000, `sshd` 74, `messagebus` 81 and `nobody` 65534, and their groups, with `tty` 5, `sudo` 27 and `crontab` 63. The owner may be logged in as `test`. `/tmp` is a directory of mode 1777 on the root, not a tmpfs.
* Four loop devices, `/dev/loop0` to `/dev/loop3`, which `loopctl` attaches to files ([loopctl(8)](../../man/man8/loopctl.8)); mounts of vfat, exfat, ext2, krffs and tmpfs ([mount(8)](../../man/man8/mount.8)); and the namespaces of [`rach2/sys/proc/ns.Readme.md`](../../rach2/sys/proc/ns.Readme.md), of which a network namespace other than the first has no interface, not even loopback, and at most 32 mount namespaces exist at once.

The plan names the native transport: the controller logs in as root over SSH without a terminal and runs `etude-guest --stdio`, which starts each case as its own child. The probes and musl programs are static executables, so they need no library of the root's; they are copied from the build into the case's workspace.

## How a native case runs

Every native case has this start, which T11 and T44 give `etude-guest`; an entry names only what a case needs beyond it:

* The case runs as root, as an ordinary process: a child of `etude-guest`, in a session and process group of its own, not the kernel's init; an orphan it leaves goes to `init-metal`.
* Descriptor 0 is `/dev/null` or a pipe of the input the case declares, and descriptors 1 and 2 are pipes that `etude-guest` reads; no other descriptor is open. No descriptor is a terminal, and the case has no controlling terminal.
* The working directory is the case's workspace (below), the umask is 022, the signal mask is empty and every disposition is the default. The resource limits are those of `/etc/rach2/limits`, which `init-metal` sets and every session inherits: 256 processes soft and 512 hard, 1024 descriptors, and no limit on the address space. The process limit binds a case only after it drops to another id, since the kernel does not count root's processes (`fork_nproc`, `rach2/sys/syscall_proc.c:2085`). The environment has `PATH`, and `HOME` set to the workspace, so that nothing a case or a program it runs writes under `$HOME` reaches root's home.
* The case's executables come from the candidate's build and are copied into its workspace. What it runs from the root, such as BusyBox, the GNU programs and the manual pages, is the installed root's, which is the build of the last release unless the candidate's root is installed too; a case that compares a file of the root with the candidate's build needs that.
* The result is the case's exit status and its output. Under QEMU the test image runs each probe as the kernel's transient init at the debug shell, and `runtests.c` reads the kernel's records around it; natively nothing is read from the kernel's log unless the case declares `log`.

## The fixtures

A fixture is what `etude-guest` prepares for a case before it starts and removes after it ends, on every path. A fixture the preparation can make on the host, such as an image of a file system, is an artifact of T19, copied to the machine and checked by its hash.

* `workspace`: a directory of the case's own, made by `etude-guest` under its scratch directory on the root, mode 0755 and owned by root, and the case's working directory. A case that drops to an id from `ids` makes the directories that id writes. It is removed after a pass and kept after a failure, with its path in the record. Every native case has one; its file system is the root's, ext2.
* `stage NAMES`: the companion programs and files the case runs or reads, copied into the workspace under the names and modes the entry gives. A set-id file is owned by root; the root's ext2 is mounted without `nosuid`.
* `volume TYPE`: a file system of its own, one of `krffs`, `ext2`, `vfat` or `exfat`, from an image the preparation made, copied into the workspace, attached to a loop device the case reserves, and mounted on a directory of the workspace without `nosuid`, which the kernel sets only when it is asked; unmounted and detached at the end. The machine has four loop devices, so at most four cases with a volume run at once, and a case that tests loop devices itself counts against the same four.
* `tmpfs SIZE`: a tmpfs of the given size mounted on a directory of the workspace.
* `mountns`: the case runs in a mount namespace of its own, so that its mounts and those of its fixtures are hidden from the rest of the machine and go when the namespace ends.
* `root TYPE`: a private root, for a case that uses fixed absolute paths such as `/d` or `/vp`, writes `/etc`, or reads the type of `/`. The preparation makes a tree with the case's executables, `/etc` files and empty `/proc`, `/dev` and `/tmp`; it goes on a `volume TYPE`, or on the workspace when TYPE is `ext2-dir`, and the case enters it by `chroot` inside a `mountns`, with `/proc`, `/sys` and `/dev` mounted there, and `/dev/shm` and `/dev/mqueue` where the case needs them. TYPE is the file system the case expects of `/`, as the test image's KRFFS root and the ext2 roots of `tests/system/probe_roots.sh` give it now. `/proc/mounts` lists every mount by its path from the machine's root, not from the caller's (`procfs_gen_mounts`, `rach2/sys/fs/procfs.c:2707`), so inside the chroot the line for `/` is the machine's ext2 root; a case that reads the type of `/` from `/proc/mounts` takes it from `statfs` of `/` or as an argument first, a change of T44.
* `ids`: user and group ids that no account of the machine has and no process of the machine runs as, given to the case as arguments. A case that fixes 1000 or 1001 in its source needs them as arguments first, since `test` is 1000 on the machine.
* `ipcns`, `utsns`: an IPC or a UTS namespace of the case's own, for a case that makes System V objects or message queues by fixed keys or names, or sets the host name.
* `pty`: a pseudo-terminal pair that `etude-guest` opens, the case's descriptors 0 to 2 on its slave side and the slave its controlling terminal in a session of its own, the master side kept by `etude-guest`, which writes the input the case declares and reads what the case writes.
* `ports`: local TCP and UDP ports on 127.0.0.1 that `etude-guest` reserves and gives the case as arguments, in place of fixed ports; port 22 and the ports of the machine's services are never given.
* `peer`: a network peer on the controller's side of the LAN, or a second process of `etude-guest` on 127.0.0.1, which plays the part the suite's host script plays under QEMU; its address and port are arguments.
* `log`: the kernel's records written while the case ran, read by `etude-guest` with `syslog(2)` action 3 before and after it, since action 2 moves the place the system log daemon reads from. Other activity on the machine writes records among them, so a check may require a record and may require the absence of a panic, an oops or a record that names the case's own processes, and may not require the absence of anything else.
* `alone`: no other native case runs on the machine at the same time. The machine's services still run, so `alone` does not make a count of the whole machine exact; it removes only the cases' own interference.
* `console`: the case uses the machine's console and framebuffer, with `alone`, and puts them back as it found them. A native run can put nothing on the console's input: no key, no mouse report, no typed line.

## The entries

Each guest program is one line:

    * `UNIT` (suites): Role: ROLE. Privileges: PRIVILEGES. Paths: PATHS. Devices: DEVICES. Global: GLOBAL. Children: CHILDREN. Recovery: RECOVERY. Native: PLAN.

* Role is `ordinary` when any process can be the case, the transient init role of the test image adding nothing it uses; `init` when it needs to be the kernel's designated init (the orphans reparented to it, the kernel's record of its exit, the panic when it dies); and `boot` when it needs a boot of its own: a kernel command line, a fresh kernel's state, a reset or a power-off.
* Privileges are what it needs and what it changes: `root`, `any`, or root with the ids it drops to and where.
* Paths are the absolute paths it makes, needs or needs absent; `relative` when it uses only its working directory, `/proc/self` and the synthetic devices.
* Devices are the devices it opens beyond `/dev/null`, `/dev/zero`, `/dev/full`, `/dev/random`, `/dev/urandom` and `/dev/tty` of its own terminal, and what it assumes of their hardware.
* Global is the state outside its own processes and files that it reads or changes: a count or a table of the whole kernel, a mount, the host name, the clock, the network's configuration, the console, a loop device; `none` when it has none.
* Children are the processes it starts, and whether it reaps every one on every path, a failed check's included.
* Recovery is what a failure or a kill leaves behind and what removes it.
* A suite is one line with its plan alone, `* \`NAME\` (TASK): Native: PLAN.`, and a row the same without the task, since their programs' entries give the rest.
* The plan is one of these forms:
  * `native: FIXTURES.` The case runs unchanged with the fixtures named.
  * `native after T44: CHANGES; FIXTURES.` T44 makes the named changes first, such as a path taken as an argument or a check separated, and the assertions stay the same.
  * `split in T44: PART native, FIXTURES; PART machine, REASON.` Some modes, rows or checks run natively; the rest stay in QEMU for the reason given.
  * `machine: REASON.` It stays a machine scenario in QEMU.
  * `host: WHAT.` A suite that boots no Rach2 and runs on the controller's host; it has no part on the Lenovo.
  * `removed in TNN.` The inventory removes the unit in that task, so it gets no native plan.

A reason begins with its class, then says which assertion needs it:

* `QEMU`: the assertion is about QEMU or needs it: its monitor, a device removed or added, an injected NMI, its exit, its audio capture, a throttled or failing virtual disk, a processor model chosen by `-cpu`, or a time limit QEMU's own pause makes.
* `card`: it tests the driver of a device the Lenovo does not have, such as the e1000, the IDE controller, NVMe, the SB16 or a USB device model of QEMU's.
* `init`: it needs the designated init's contract, which a child of `etude-guest` cannot have.
* `boot`: it needs a kernel command line, a fresh kernel's state, a reset or a power-off.
* `exhaustion`: it exhausts a resource of the whole machine on purpose, such as its memory or its processes, which on the Lenovo would starve the services and the other cases.
* `count`: it judges a count, a table or the clock of the whole machine, such as free memory, the heap, `/proc/disk`, the scheduler's or the console's counts, the sockets of `/proc/net` or the terminals of `/dev/pts`, which the Lenovo's services, its logins and `ntpd` move while the case runs.
* `feature`: it needs a processor feature, or its absence, that the Lenovo's processor does not give.
* `layout`: it needs disks, partitions or controllers in an arrangement of its own, which the Lenovo's one disk cannot give without a raw write.
* `disk`: it writes a disk node, or would write one if the refusal it checks failed; a native run writes the Lenovo's disk only through files of its root.
* `network`: it takes down or reconfigures an interface, or needs a network of its own (a DHCP server, a second card, a capture of the wire, QEMU's user-network addresses), which the Lenovo's one interface cannot give while it carries the native transport on the owner's LAN.
* `image`: it tests a system image other than the Lenovo's installed root: os-simple, os-real with Dropbear, the metal image, the recovery system, the installer's stick or the test image's debug shell.
* `system`: it changes the installed system's own configuration, accounts or services (a file of `/etc`, an account's keys or crontab, a running daemon, the clock), which a native run leaves as it found them.
* `firmware`: it needs a memory map or a firmware arrangement of its own.
* `input`: it needs keys, mouse reports or typed lines on the console, which a native run cannot make.
* `architecture`: it runs on i686 alone, and the Lenovo runs the x86-64 kernel.

## The counts

| Part | Section | Units | Native | Native after T44 | Split | Machine | Host | Removed |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| Guest | The probes | 103 | 29 | 27 | 29 | 17 | 0 | 1 |
| Guest | The musl programs | 82 | 38 | 27 | 14 | 3 | 0 | 0 |
| Guest | The default suite's own checks | 11 | 7 | 1 | 0 | 1 | 0 | 2 |
| Suites | The suites | 101 | 2 | 6 | 25 | 60 | 6 | 2 |
| Suites | The rows | 88 | 56 | 7 | 7 | 13 | 0 | 5 |
| All | | 385 | 132 | 68 | 75 | 94 | 6 | 10 |

Every unit has a plan; none is left open. Of the 196 guest units, 172 run natively in whole or in part, and every machine plan names its reason.

## The suites

A suite is a scenario: its boots, its disks and devices, and the steps its host script takes. Its programs have entries of their own in [the guest programs](native-guest.md); a suite's entry gives the native plan of the scenario's own steps and says where its programs' plans are. The suites are in the order of [the suites' part](inventory-suites.md#the-suites), with the task that converts each.

* `gnu` (T65): Native: split in T44: the rows native, as the rows below say; the boot of a fresh copy of the root, its first host keys, the console's login prompt and the logins by key and by password to accounts the suite made machine, boot and system, since on the Lenovo they would need a boot, new host keys and keys or passwords given to its accounts.
* `agents` (T66): Native: split in T44: the rows' checks of the pinned versions, paths and licences native, as the rows below say; the sessions against the fake API machine, QEMU, since the fake server is reached through the user network's forward of 10.0.2.100:443 and names the suite writes into the machine's `/etc/hosts`.
* `agentsaccept` (T66): Native: machine: QEMU: the rehearsal plays the fake API through the user network's forward; the same script's `lenovo` mode, against the real services after the owner has signed in, is the Lenovo's counterpart and is run by hand.
* `install` (T79): Native: machine: boot: it builds and installs a kernel, boots it as a trial, commits it and corrupts the candidate slot through `/dev/bootpart`, which on the Lenovo is the accepted system's boot.
* `selfbuild` (T82): Native: split in T44: the builds of the kernel, `utils` and `ish` by the staged TinyCC and pdpmake from clean workspaces, their equal hashes and the smoke checks native in a workspace, with the sources staged from the candidate; the KRFFS disk, the host's check of its free bytes and the probe suite on the rebuilt kernel machine, boot.
* `installer-repair` (T80): Native: machine: layout: it writes target disks beside a stick, kills a `reflash` that runs, and puts a destroyed disk back.
* `installer` (T80): Native: machine: layout: the stick boots by the firmware with no kernel given and writes the image to target disks.
* `menu` (T79): Native: machine: boot: four boots of one GRUB disk, with a candidate kernel corrupted and a staged menu truncated from the host.
* `uefi` (T81): Native: machine: firmware: each part boots under OVMF or SeaBIOS from a disk arrangement of its own, NVMe among them.
* `installer-rehearse` (T80): Native: machine: layout: it rehearses the sitting on a disk and two sticks of its own.
* `selfbuild-ext2` (T82): Native: split in T44: as `selfbuild`, the build native; the ext2 root booted with `rach2.root=ext2`, `debugfs` on the disk image and the probes on the rebuilt kernel machine, boot.
* `sealed` (T79): Native: machine: boot: trial kernels sent over SSH are installed, committed and destroyed from the host; its loop-device step is the subject of `loopdev`, which has a native part.
* `fakeapi` (T66): Native: machine: QEMU: the restricted user network, on which the test account finds no route to an outside address, and the server's name the suite writes into `/etc/hosts`.
* `dhcp-e1000` (T85): Native: machine: card: the e1000.
* `dhcp` (T85): Native: machine: network: leases from the suite's DHCP server, `ifup` of an unconfigured interface and a capture of the broadcasts; on the Lenovo the interface carries the transport.
* `netif` (T85): Native: machine: network: `netifprobe` takes the interface down, and the stack half needs two cards.
* `netif-e1000` (T85): Native: machine: card: the e1000.
* `mount` (T77): Native: split in T44: the rows whose programs [the guest part](native-guest.md) marks native run there on a `volume`; the rows under `rach2.blocklag`, `rach2.fsdisk` or a boot fixture machine, boot.
* `fdlock` (T70): Native: split in T44: `fdstale` as its entry says; the host part host; `bootprobe shared` under `rach2.blocklag` machine, boot.
* `grow` (T80): Native: machine: layout: `growroot` and the grow plans act on whole disks.
* `drill` (T79): Native: machine: boot: the recovery drill destroys the superblock and boots the recovery entry.
* `account` (T70): Native: split in T44: `adduser`, `su` and `passwd` through its set-id bit native in a `root ext2` with BusyBox staged and `ids`; the boots of os-simple and of os-real on a disk machine, image.
* `large` (T76): Native: native after T44: the three volumes as sparse `volume ext2`, `volume exfat` and `volume vfat` of 6 GiB, the remount in place of the suite's, and the checks after it run by `e2fsck -fn` on the machine and by the host's checkers on the image fetched back; `bigfileprobe` as its entry says.
* `usb` (T73): Native: machine: card: QEMU's USB keyboards, mice, hubs and their hot-plugging through the monitor, and `rach2.hubslow` boots.
* `raw-e1000` (T84): Native: machine: card: the e1000.
* `raw` (T84): Native: split in T44: `rawprobe` as its entry says; the capture of its frames, the first hop 10.0.2.2 and the static `rach2.ip` machine, network.
* `krffsmount` (T77): Native: split in T44: the read to the last byte, the write and `fsck.krffs --strict` of the image fetched back native on a `volume krffs`; the kill at `KRFFSPROBE WRITING` and the readback after a reboot machine, QEMU and boot.
* `diskrate` (T74): Native: machine: card: three IDE disks and the ATA driver's bus-master DMA and polls.
* `usbdisk` (T73): Native: machine: QEMU: sticks pulled and plugged back through the monitor.
* `stress-ahci` (T75): Native: split in T44: `stressprobe` as its entry says, on a `volume krffs`, which reaches the Lenovo's AHCI disk through the root's file system; the root through `rach2.fsdisk` on a 48 MB partition of QEMU's AHCI disk machine, boot.
* `default` (T68): Native: split in T44: the programs and the runner's checks as their entries in [the guest part](native-guest.md) say; the boot of the test image and the debug shell that runs each program as a transient init machine, image.
* `stress` (T75): Native: split in T44: `stressprobe` as for `stress-ahci`; the IDE disk machine, card.
* `procfiles` (T70): Native: split in T44: half 2, the commands of an os-real login (`ps`, `df`, `free`, `uptime`, `ifconfig` and the rest) exiting 0 without "cannot read", native, with the interface checks read from the machine's own interface; half 1, `procprobe` on boots with a RAM module, `rach2.fswindow` and `rach2.fsdisk`, machine, boot.
* `bigmem` (T69): Native: machine: firmware: guests of 8 and 72 GiB and `-cpu max,pdpe1gb=off`.
* `memory` (T69): Native: machine: firmware: guests of 8 MiB to 4096 MiB, each a memory map of its own, and a recovery boot refused for its size.
* `parts` (T78): Native: machine: layout: four AHCI disks of crafted tables, two of them written.
* `bootpart` (T78): Native: machine: layout: boots with no boot partition, the fixture, a RAM root and a disk of 260 GB.
* `loopdev` (T77): Native: split in T44: attach, mount, read, write, unmount, detach and reattach of vfat, ext2 and KRFFS images, the refusals, the attach record (by `log`) and the image on an ext2 volume native in a `mountns`, reserving the loop devices it uses; the lag row under `rach2.blocklag` machine, boot; the USB stick pulled under a loop device machine, QEMU.
* `tcc` (T82): Native: native after T44: the compiles and runs in a workspace as an id from `ids` in place of the test account, and the system-call trace's "not implemented" records read by `log`.
* `make` (T82): Native: native after T44: the builds in a workspace as an id from `ids` in place of the test account.
* `smp` (T69): Native: split in T44: `smpmusl` on the Lenovo's four processors and the installed system's `nproc` of 4 and parallel `ish` build native; the boots on 1 processor, with `maxcpus=2`, the preemption on 2 and the NMI machine, boot and QEMU.
* `clock` (T69): Native: machine: QEMU: the guest's clocks are compared across 30 stops of the QEMU process by SIGSTOP.
* `host` (T23): Native: host: the host tests, self-tests, proofs and fuzz replays.
* `noentropy` (T69): Native: machine: feature: the four boots need a processor without RDRAND, which `-cpu qemu64` gives and the Lenovo's processor cannot be made into.
* `shutdown` (T83): Native: machine: boot: `poweroff` and `halt` end the machine.
* `fatwrite` (T76): Native: split in T44: the write, the host's walk, `mtools` and `fsck.fat -n` of the image fetched back native on a `volume vfat`; the readback after a reboot, the kill at `FATWRITE WRITING` and the repair mount machine, boot and QEMU.
* `ssh` (T86): Native: machine: image: it tests os-real's Dropbear and its console login, which the Lenovo's GNU root replaces with OpenSSH; the release's `metal_accept.sh` checks the Lenovo's own server.
* `ssh-e1000` (T86): Native: machine: card: the e1000.
* `exfat` (T76): Native: split in T44: as `fatwrite`, on a `volume exfat`.
* `ext2` (T76): Native: split in T44: the probes of `probe_roots.sh` and `musl/mapmusl` on a `root ext2`, the fixture's link rows on a `volume ext2`, and the cost rows counted by the loop device's own write and sync counts in `/proc/loop` native; the rows on a root booted with `rach2.root=ext2` machine, boot.
* `cred-persist` (T70): Native: machine: boot: `creddisk` checks the credentials written on one boot and read back on the next.
* `packet-e1000` (T84): Native: machine: card: the e1000.
* `addressed` (T85): Native: machine: network: an address given by `rach2.ip` and the capture of its broadcasts.
* `ping` (T84): Native: split in T44: `pingtest` as its entry says, with the gateway and an address nobody answers given by a `peer`.
* `packet` (T84): Native: split in T44: `packetprobe` as its entry says.
* `ping-e1000` (T84): Native: machine: card: the e1000.
* `load` (T75): Native: machine: boot: the writers' lag of `rach2.blocklag` and the polled row's `rach2.atapio`.
* `authority` (T70): Native: machine: boot: the everyday and the recovery kernel each booted on the wide fixture.
* `queue` (T75): Native: machine: layout: two disposable AHCI disks and a private ext2 volume; boot: the root-fault row needs a KRFFS root through its page-cache window and a raw second partition. T37 checks deadlines and independent completion in controlled host models, without disks or QEMU throttles.
* `persist-ahci` (T75): Native: machine: boot: two boots of one disk.
* `subset` (T77): Native: machine: boot: each boot has a `rach2.noX` option.
* `bus` (T78): Native: machine: card: four IDE drives and a 130 GiB disk.
* `exitline` (T71): Native: machine: image: the debug shell's exit lines and the console's output of a logged-in program.
* `login` (T70): Native: machine: init: `init-shell`, `init-login` and `rach2.rescue` as the kernel's init, and a stopped console session continued by init.
* `man` (T71): Native: native after T44: every page through `man` on the installed root compared with the build's rendering, when the candidate's root is installed, and the pager's rows on a `pty`; the recovery image's pages machine, image.
* `metal` (T86): Native: machine: image: the metal image in QEMU with sticks, a hub and a keyboard; the release's `metal_accept.sh` is its counterpart on the Lenovo.
* `bootline` (T71): Native: machine: boot: boot lines refused, a blank root disk and processors below the kernel's needs.
* `servicelog` (T71): Native: machine: boot: an ext2 root with `init-shell` filled by orphans, a poweroff and a second boot.
* `persist` (T75): Native: machine: card: two boots of one disk on IDE.
* `style` (T64): Native: host: the style checks.
* `dgram-e1000` (T84): Native: machine: card: the e1000.
* `dgram` (T84): Native: split in T44: `dgramprobe` as its entry says, the datagram from outside sent by a `peer`; the capture of its broadcast machine, network.
* `diskdiag` (T74): Native: machine: image: the read-only metal kernel, whose sectors are hashed against the host's image.
* `e1000` (T86): Native: machine: card: an rtl8139, an e1000 and an e1000e.
* `disk-ahci` (T74): Native: machine: boot: `blockprobe` tests the page cache of a root mounted by `rach2.fsdisk`, as its entry says, here on the AHCI disk.
* `grub` (T78): Native: machine: disk: `bootctl grub` writes GRUB's core image, which on the Lenovo is the accepted system's.
* `disk-q35` (T74): Native: machine: card: the q35 machine's controller.
* `doom` (T72): Native: native after T44: the game on the Lenovo's framebuffer with the WAD staged, by `console`: the missing WAD's exit 255, the geometry, the zone and the disk commands it reports, demo1 played to exit 0, and the `-record` file of mode 644, with T35's readiness in place of the survival of a `sleep 10`.
* `disk` (T74): Native: machine: card: IDE.
* `diskpolicy` (T74): Native: machine: image: the read-write and the read-only kernels, the second leaving its image byte for byte the same.
* `console` (T71): Native: machine: input: what `stdintest` and `ttyprobe` read is typed at the console.
* `fpu` (T69): Native: split in T44: `fpuprobe` and `xsaveprobe` on the Lenovo's processor, the branch it takes declared (T41); the boots with `rach2.noxsave` and `rach2.nofxsr` and on `-cpu qemu64` machine, boot and feature.
* `socket` (T84): Native: native after T44: `udptest` and `tcptest` as their entries say, with the host's echo server as a `peer`.
* `bootorder` (T78): Native: machine: layout: a GRUB disk behind a blank one on the second AHCI port.
* `reset` (T83): Native: machine: boot: each row resets the machine.
* `netconsole-e1000` (T86): Native: machine: card: the e1000.
* `disks` (T78): Native: machine: layout: two AHCI disks of different sizes, each partition written.
* `audio` (T72): Native: split in T44: `audio test scale` on the Lenovo's HDA with `intrprobe`'s count of the controller's own interrupts native, by `alone`; the capture of the notes by QEMU's wav file machine, QEMU; the SB16 machine, card.
* `netconsole` (T86): Native: machine: boot: `rach2.netconsole` and the capture of its frames.
* `crash` (T83): Native: machine: QEMU: QEMU is killed after the mount report, and the volume is checked from the host.
* `pools` (T68): Native: split in T44: `poolprobe` as its entry says.
* `runner` (T89): Native: removed in T89.
* `diskfull` (T75): Native: native: `diskfull` in a `root krffs` on a small volume, as its entry says.
* `fault` (T75): Native: native: `vmfault`, `vmro` and `winprobe` killed by SIGSEGV, then `hello`, as their entries say.
* `imagerace` (T87): Native: host: the image builders' concurrency on the host.
* `krffscheck` (T77): Native: host: `fsck.krffs` on images made on the host.
* `codexapi` (T67): Native: host: Codex against the fake API in a network namespace of the host.
* `museapi` (T67): Native: host: Muse against the fake API in a network namespace of the host.
* `conload` (T71): Native: machine: input: the console's receive path is fed by the serial port, which the Lenovo lacks.
* `drive` (T89): Native: removed in T89.
* `textbusy` (T70): Native: split in T44: `textbusymusl` and `bindholdmusl` as their entries say; the 128 MiB guest with `rach2.rescue` machine, boot.

## The rows

The GNU rows run on the GNU system's ext2 root, which is the Lenovo's own root, as root under bash, one row a session (`tests/system/gnu_rows.sh`). Every GNU row's native plan includes one change of T44: `WORK` and `HOME`, which the script fixes at `/tmp/gnu/ROW` and `/root` (`gnu_rows.sh:26`, `gnu_rows.sh:30`), become directories of the workspace, and the architecture argument is `x86_64`. A row marked `native` needs that change and nothing else. A daemon a row starts, such as tmux's server, screen's session, a session bus, `openssl s_server` or a Python responder, belongs to the case's processes, which `etude-guest` confirms have ended. A row compares the root's programs with versions the GNU build pins, so it needs the candidate's root installed when the candidate changes a pin.

The agents rows run as the `test` account in `/tmp/agents` (`tests/system/agents_rows.sh`), on a root with the agents layer, which the release's `make machine GNU_AGENTS=all` gives the Lenovo. On the Lenovo the `test` account's home may hold the owner's own sign-ins to the agents, so a native agents row runs as an id from `ids` with `HOME` in the workspace, and never as `test`.

* `gnu_rows.sh row_names`: Native: native.
* `gnu_rows.sh row_glibc`: Native: native.
* `gnu_rows.sh row_zlib`: Native: native.
* `gnu_rows.sh row_openssl`: Native: native.
* `gnu_rows.sh row_openssh`: Native: split in T44: the version and the key `ssh-keygen` makes in the workspace native; the login to the machine's own server, `ssh -tt who`, `scp` and `sftp` machine, system, since the row adds its key to `/root/.ssh/authorized_keys` (`gnu_rows.sh:139`).
* `gnu_rows.sh row_ncurses`: Native: native.
* `gnu_rows.sh row_less`: Native: native.
* `gnu_rows.sh row_vim`: Native: native.
* `gnu_rows.sh row_perl`: Native: native.
* `gnu_rows.sh row_curl`: Native: native after T44: port 4433 from `ports`; `/etc/hostname` read as the machine's own name.
* `gnu_rows.sh row_git`: Native: native.
* `gnu_rows.sh row_coreutils`: Native: native.
* `gnu_rows.sh row_bash`: Native: native; the directory the `test` account may not search is in the workspace, and the account is used, not changed.
* `gnu_rows.sh row_grep`: Native: native.
* `gnu_rows.sh row_sed`: Native: native.
* `gnu_rows.sh row_gawk`: Native: native.
* `gnu_rows.sh row_findutils`: Native: native.
* `gnu_rows.sh row_diffutils`: Native: native.
* `gnu_rows.sh row_patch`: Native: native.
* `gnu_rows.sh row_tar`: Native: native.
* `gnu_rows.sh row_gzip`: Native: native.
* `gnu_rows.sh row_xz`: Native: native.
* `gnu_rows.sh row_file`: Native: native.
* `gnu_rows.sh row_iproute2`: Native: native after T44: the address, the route, the gateway and its neighbour entry read from the machine's configuration in place of SLIRP's 10.0.2.15 and 10.0.2.2, the listener's port 5999 from `ports`, and the PCI identity of 00:00.0 read from the machine.
* `gnu_rows.sh row_loopctl`: Native: native, with one loop device reserved.
* `gnu_rows.sh row_losetup`: Native: native, with one loop device reserved; the mount is in a `mountns`.
* `gnu_rows.sh row_e2fsprogs`: Native: native.
* `gnu_rows.sh row_pkgconf`: Native: native.
* `gnu_rows.sh row_procps`: Native: native after T44: `pgrep -c` counts the listening `sshd` alone, since the native transport's own session adds `sshd` processes.
* `gnu_rows.sh row_utillinux`: Native: native.
* `gnu_rows.sh row_zstd`: Native: native.
* `gnu_rows.sh row_rsync`: Native: split in T44: the local copies and `--delete` native; the copy over SSH to the machine's server machine, system, since the row adds its key to `/root/.ssh/authorized_keys` (`gnu_rows.sh:540`).
* `gnu_rows.sh row_nano`: Native: native.
* `gnu_rows.sh row_texinfo`: Native: native.
* `gnu_rows.sh row_strace`: Native: native.
* `gnu_rows.sh row_lsof`: Native: native.
* `gnu_rows.sh row_tmux`: Native: native.
* `gnu_rows.sh row_cron`: Native: machine: system: it installs a crontab for the `test` account in the machine's spool and waits for the machine's `crond` to run it (`gnu_rows.sh:613`).
* `gnu_rows.sh row_zip`: Native: native.
* `gnu_rows.sh row_htop`: Native: native.
* `gnu_rows.sh row_dosfstools`: Native: native.
* `gnu_rows.sh row_uname`: Native: native.
* `gnu_rows.sh row_ttygroup`: Native: native.
* `gnu_rows.sh row_disknode`: Native: split in T44: the node's type and mode and the read of sector 0 native; the write refused over the mounted root machine, disk, since a fault in that refusal would write the Lenovo's sector 0.
* `gnu_rows.sh row_fallocate`: Native: native.
* `gnu_rows.sh row_chattr`: Native: native after T44: `/tmp/rowattr` (`gnu_rows.sh:838`) in the workspace.
* `gnu_rows.sh row_filecache`: Native: native.
* `gnu_rows.sh row_shadow`: Native: machine: system: `useradd`, `usermod`, `chsh`, `userdel` and `groupdel` change the machine's `/etc/passwd`, `/etc/group` and `/etc/shadow`.
* `gnu_rows.sh row_chawan`: Native: native.
* `gnu_rows.sh row_python`: Native: native.
* `gnu_rows.sh row_gnupg`: Native: native.
* `gnu_rows.sh row_m4`: Native: native.
* `gnu_rows.sh row_autoreconf`: Native: native.
* `gnu_rows.sh row_bison`: Native: native.
* `gnu_rows.sh row_flex`: Native: native.
* `gnu_rows.sh row_jq`: Native: native.
* `gnu_rows.sh row_screen`: Native: native.
* `gnu_rows.sh row_sudo`: Native: native.
* `gnu_rows.sh row_cmake`: Native: native.
* `gnu_rows.sh row_dig`: Native: native after T44: the responder's port 5353 from `ports`.
* `gnu_rows.sh row_make`: Native: native.
* `gnu_rows.sh row_autotools`: Native: native, with GNU sed's tarball by `stage`.
* `gnu_rows.sh row_gcc`: Native: native.
* `gnu_rows.sh row_gdb`: Native: native.
* `gnu_rows.sh row_js`: Native: native.
* `gnu_rows.sh row_dbus`: Native: native; the login shell's session bus is started under the workspace's `HOME`, and its daemon is one of the case's processes.
* `gnu_rows.sh row_chromium`: Native: split in T44: `ldd` and the DOM dump of a run with its sandbox native, run as `test` with `HOME` in a workspace directory that account owns, so that its own home is not written; the second start's count of fewer than 200 disk commands machine, since `/proc/disk` counts the whole machine's commands and the Lenovo's services move it.
* `gnu_rows.sh row_ntpd`: Native: machine: system: it writes `/etc/ntp.conf`, kills the machine's `ntpd` and sets the clock to 2030.
* `tests/system/agents/agy.sh`: Native: split in T44: the version, the path, the terms, the login shell's variable and `--help` native; the offline screen machine, QEMU, since the Lenovo is on line and the row needs the restricted user network.
* `tests/system/agents/agy-print.sh`: Native: machine: QEMU: the session plays against the fake Gemini API, reached through the user network's forward and the names the suite writes into `/etc/hosts`.
* `tests/system/agents/agy-screen.sh`: Native: machine: QEMU: as `agy-print.sh`.
* `tests/system/agents/board_test.sh`: Native: removed in T31.
* `tests/system/agents/claude.sh`: Native: native after T44: as an id from `ids` with `HOME` in the workspace, so that `claude auth status` reads an empty configuration.
* `tests/system/agents/claude-print.sh`: Native: machine: QEMU: as `agy-print.sh`, against the fake Anthropic API.
* `tests/system/agents/claude-screen.sh`: Native: machine: QEMU: as `claude-print.sh`.
* `tests/system/agents/codex.sh`: Native: native after T44: as an id from `ids` with `HOME` in the workspace, so that `~/.codex` is absent.
* `tests/system/agents/codex-exec.sh`: Native: machine: QEMU: as `agy-print.sh`, against the fake OpenAI API.
* `tests/system/agents/codex-screen.sh`: Native: machine: QEMU: as `codex-exec.sh`.
* `tests/system/agents/gh.sh`: Native: split in T44: the version, the path, the licence, `gh auth status` and the TLS checks against `openssl s_server` on a port from `ports` native; the invalid token sent to the fake `api.github.com` machine, QEMU, since that name reaches the fake server only through `/etc/hosts` and the forward.
* `tests/system/agents/limited_test.sh`: Native: removed in T31.
* `tests/system/agents/muse.sh`: Native: split in T44: the version, the path, the terms, `--help` and the first screen with its trust record native, as an id from `ids` with `HOME` in the workspace; `muse exec` playing `muse.json` machine, QEMU, as `agy-print.sh`.
* `tests/system/agents/muse-tmux.sh`: Native: machine: QEMU: as `agy-print.sh`.
* `tests/system/agents/release_resume_test.sh`: Native: removed in T31.
* `tests/system/agents/tidy_test.sh`: Native: removed in T31.
* `tests/system/agents/worker-session.sh`: Native: machine: QEMU: the Claude Code sessions play against the fake Anthropic API, as `claude-print.sh`.
* `tests/system/agents/worker_park_test.sh`: Native: removed in T31.
* `tests/system/stackoverrun_row.sh`: Native: machine: boot: each boot has `rach2.stackoverrun=WHERE`.
* `tests/system/gpt_write_refusal.sh`: Native: machine: disk: the recovery shell's `disk write` is offered seven disks, and a fault in its refusal would write them.
