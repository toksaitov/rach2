---
name: machine
description: Write the development flash drive or a disk of the Lenovo, or update the installed Lenovo. Use before any write to a physical disk and before a hardware sitting.
---

# Machine

The Hardware section of `AGENTS.md` says what may be written and when the owner is needed. This skill has the checks, the Lenovo's procedures and the sittings.

## Targets

* This work is the owner's: only the owner's sessions on the Mac mini, the manager and its worker sessions among them, write a disk or work on the Lenovo. `tools/flow/release.sh`, which the manager starts, installs on the Lenovo each commit that passed on every machine.
* The owner has authorized writes to two targets: the development flash drive on the Mac mini, and any disk of the Lenovo. Both hold nothing but this work, and both are a sandbox: write them as the work needs, without asking and without naming them to the owner first, and take no backup, restore or hash for their safety. Report afterwards what was written and where. Any other target, such as a second flash drive, another machine or hardware of another kind, needs the owner's authorization.
* The Mac mini's internal SSD, an APPLE SSD AP0512Z with serial `0ba02f1140bcba20`, is never written.
* A check of the target runs before every write, in place of asking. A target on the Mac must be external, removable and on USB, and must report the flash drive's serial and size: a SanDisk 3.2Gen1 of 61,524,148,224 bytes with USB serial `00014101091624031318`. Its device node changes between connections, so the serial and the size identify it. A target that is internal, or that reports the SSD's serial, is refused and the write does not run.
* The system itself needs no host device path: the kernel probes its own disk controller and chooses its root from its boot options, as `rach2/sys/block/Readme.md` describes.

## Reaching the Lenovo

* The Lenovo is on the Mac's network, not the testing host's, so sittings and stick writes run from the Mac. The Mac reaches it over Wi-Fi: `en1` is the Mac's Wi-Fi port, on 5 GHz, and `en0` has no link. The Lenovo's 192.168.0.106 is wired only on the Lenovo's side, and the testing host's 192.168.0.1 is a separate network.
* The committed development key reaches every machine built from this tree, as `tools/Readme.md` says under Working an installed machine from the host. When Linux runs on the Lenovo and IPv4 cannot reach it, it answers SSH on its link-local address, `fe80::2ad2:44ff:fe7e:e0af%en1`.
* A program built on the Mac and started from a Claude Code session there is charged to Claude Code by macOS's Local Network privacy, and its UDP or TCP to the Lenovo fails with EHOSTUNREACH until Claude Code has that permission (System Settings, Privacy & Security, Local Network). Apple's own `ssh` and `nc` are exempt, so they still work from the same shell. The release's native datagram cases need the permission.
* Its disk stays in its bay, since removing and refitting it is an hour of physical work. A machine left unbootable is reinstalled from the flash drive, which stays plugged into it.
* The machine's own offsets are in `dev-log/43-machine/hardware.md`. Take a path from there or from `tools/Readme.md`; do not derive one.

## Taking the Lenovo

* One task works the Lenovo at a time, since several milestones may be open at once. Before its first boot or write, a task takes the machine with `mkdir "$(git rev-parse --git-common-dir)/../build/flow/lenovo"` and writes its issue number to `issue` in that directory; when the mkdir fails, another task holds it, named in that file, and this one waits for it with the tool's own notice, never taking it. The task removes the directory once the release is back in both slots.

## Trial boots

* Over SSH, a worker tries its own kernel whenever QEMU cannot show what it needs: `bootctl kernel FILE` puts it in the candidate slot and arms one boot of it, and the boot after that, or a power cycle after a hang, returns to the fallback slot. Since the candidate slot held the installed release, check first that the fallback holds the same kernel, by the SHA-256 of each slot's bytes against the release record; if it does not, run `bootctl fallback` while the release is the booted, committed candidate. Afterwards put the release back with `bootctl kernel`, a boot, `bootctl commit` and `bootctl fallback`, and check both slots again.
* A trial boot of code that has never run on the Lenovo waits for a review that it is ready, since a hang costs the machine until the owner is home. The worker writes on the issue why each new path cannot stop the machine: every register sequence checked against the reference driver line by line, every wait bounded by a timeout, interrupts masked until their handler is ready, and the host test against the chip model running the very path the trial will run, start to finish. The manager reads that and the diff, and says go on the issue before the boot; a trial with any doubt left runs the new path one step at a time, each step logged over SSH before the next.
* A silent trial runs whenever the work is ready, at any hour; none is held for a time of day. If one leaves the machine off or hung, record it on the issue and go on with other work: the machine waits until the owner is home, the hang is pushed to the owner's phone while the owner is awake, and the owner is never asked to come to the machine for it.
* Linux may be booted on the Lenovo for discovery, once, through GRUB's `next_entry`. It leaves by `rtcwake -m off -s 120` rather than a warm reboot, which once left the firmware unable to boot, and what it added to the boot partition is deleted afterwards.

## Sound and screen

* The owner works beside the Lenovo, so a step there is silent and does not take the screen unless it must. Run `mixer vol 1` after every boot, since HDA starts at 100, and again once a program that sets the volume, such as Doom, has started. Measure sound by the kernel's counters and `/proc/audio` at that level, not by ear.
* A step that plays sound aloud or shows something on the screen, Doom, `audiotest` or a tone among them, is announced first: comment "Lenovo notice:" and what will play on the task's issue, which the manager passes to the owner, and wait ten minutes before running it.

## Sittings

* A sitting, which needs someone at the machine, comes last in a task, once every part is written and the machine is built, installed and through its automatic steps. The owner is asked for it only with the time it will be ready, and names the time.
* During a sitting the owner gets one step at a time: the step, its command and what to report, and then nothing more until the owner says it is done and what happened. Results are told apart by who produced them and when. A helper stages its scripts ahead but starts nothing audible, on screen or waiting on the owner until the owner has that step. Everything else, background fixes and merges among them, waits until the sitting ends.

## Updating the installed system

* `make machine`, with `ARCH=x86_64` for the Lenovo and on the testing host and with `GNU_AGENTS=all` as the release gives it, builds everything the machine is given, recovery included; a part built by hand can be older than the rest.
* The installed Lenovo is updated over SSH: a kernel through `bootctl` on the running system; a root through the recovery system with `rach2-install --console term --update`, which keeps the machine's accounts; a partition the installer does not know about through `reflash`. The recovery partition's own key, `boot/recovery_authorized_keys`, holds the development key, so the disk's recovery system answers SSH; its recovery kernel and image are the files `boot/recovery` and `boot/recovery.krffs`, which a release replaces in place, keeping the key and the accounts' archive. `tools/flow/release.sh` runs these steps in order and says why that order; read it before an update by hand.
* A root update rewrites `/opt`, so afterwards `sh build/cache/crysis/push-lenovo.sh` puts the Crysis copy back for the graphics work.
* The manual has the commands: `rach2-install(8)` for an install, `recovery(7)` for the recovery system and the three update paths, `bootctl(8)` and `reflash(8)` for the two writes, and `disk(7)` for the device nodes they write through, each limited to one region of the disk.
