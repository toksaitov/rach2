---
name: machine
description: Write the development flash drive or a disk of the Lenovo, or update the installed Lenovo. Use before any write to a physical disk and before a hardware sitting.
---

# Machine

## Targets

* The owner has authorized writes to two targets: the development flash drive on the Mac mini, and any disk of the Lenovo. Both have nothing on them but this work, and both are a sandbox: write them as the work needs, without asking and without naming them to the owner first, and take no backup, restore or hash for their safety. Report afterwards what was written and where. Any other target, such as a second flash drive, another machine or hardware of another kind, needs the owner's authorization.
* The Mac mini's internal SSD, an APPLE SSD AP0512Z with serial `0ba02f1140bcba20`, is never written.
* A check of the target runs before every write, in place of asking. A target on the Mac must be external, removable and on USB, and must report the flash drive's serial and size: a SanDisk 3.2Gen1 of 61,524,148,224 bytes with USB serial `00014101091624031318`. Its device node changes between connections, so the serial and the size identify it. A target that is internal, or that reports the SSD's serial, is refused and the write does not run.
* The system itself needs no host device path: the kernel probes its own disk controller and chooses its root from its boot options, as `rach2/sys/block/Readme.md` describes.

## The Lenovo

* The Lenovo is on the Mac's network, not the testing host's, so hardware sittings (sessions with the owner at the machine) and stick writes run from the Mac.
* Its disk stays in its bay, since removing and refitting it is an hour of physical work. A machine left unbootable is reinstalled from the flash drive, which stays plugged into it.
* `make machine`, with `ARCH=x86_64` for the Lenovo and on the testing host, builds everything the machine is given, recovery included, after the style check; a part built by hand can be older than the rest.
* The installed Lenovo is updated over SSH: a kernel through `bootctl` on the running system; a root through the recovery system with `rach2-install --update`, which keeps the machine's accounts; a partition the installer does not know about through `reflash`.
* The manual has the commands: `rach2-install(8)` for an install, `recovery(7)` for the recovery system and the three update paths, `bootctl(8)` and `reflash(8)` for the two writes, and `disk(7)` for the device nodes they write through, each limited to one region of the disk.
* The machine's own offsets are in `dev-log/43-machine/hardware.md`, and the credential check before a root write is in `tools/Readme.md`, under Working an installed machine from the host. Take a path from these; do not derive one.
