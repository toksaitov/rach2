# Milestone 50: the machine and the shortcomings

## The goal

Put the system of milestones 45 through 49 on the Lenovo, in one sitting of the owner's, and fix every previously known problem, every problem recorded during those milestones, and everything the machine shows. Milestones 45 through 49 prove their work in QEMU and do not touch the machine, so this milestone is the first run on the Lenovo of the new kernel, the disk registry, the AHCI interrupt path, the per-controller USB stack, the installer and the stick. The list of shortcomings is written as the work goes: an entry the day the problem is found, and nothing is deferred past this milestone. The manuals and the audit follow it, so that what they describe is the finished system and not one with known defects; an entry made during those two reopens the technical work before the document that describes it is accepted. The milestone is complete when the Lenovo runs the installed system with every device it had before, the twenty-session load has run on it for an hour, every entry is marked fixed with its proof, and the close below has run.

## What the tree settles before this plan

* The stick behind the SuperSpeed hub stopped answering and its removal panicked the kernel once; the record leaves the reset-and-removal sequence with stale completions open (`dev-log/43-machine/notes.md:291`). A healthy stick pulled is not the same test.
* Milestone 44 needed four full runs to get one clean, and records the failures as pooled execution (`dev-log/44-krffsmount/notes.md:412`). A successful rerun does not explain the earlier failure. Diagnose the kernel, harness and scheduling before closing the entry.
* The Lenovo's disk is 488397168 sectors of 512 bytes on SATA, internal, and does not leave its bay (`dev-log/43-machine/hardware.md:43`). It has the sealed layout: a 64 MiB boot partition, the 500 MiB KRFFS fallback root, the 64 MiB recovery partition, and a fourth entry of 2 GiB with the 1 GiB ext2 root in it; the rest is unused. It has one xHCI controller and an EHCI the kernel names (`hardware.md:41`).
* Milestone 43's recovery stick, with its older kernel and its own host key, boots the machine today (`hardware.md:121`). That recovery repairs every ext2 partition it finds during its own boot (`utils/init.c:200`, the call at `:243`), has `reflash` and no `rach2-recover`, and is not changed by this milestone.
* The Lenovo's USB sockets are the right black, the left black and the left blue, with the hub on the blue one (`hardware.md:131`).
* The metal acceptance writes a file to the first USB disk it finds with a FAT or exFAT volume (`tests/system/accept/metal_accept.sh:500`, the write at `:527`). It would write to a recovery stick's image half.
* The hardware rule in `CLAUDE.md`: a write that cannot be undone by rebuilding needs the owner's approval at the time, with the device shown; before a write to the Lenovo's disk that is not a kernel through `bootctl`, a stick with recovery is booted and shown to work, every sector about to be overwritten is saved on the host and on that stick, and recovery is the saved boot default until the write is verified.
* Milestone 49's installer prints the device, asks for its name typed back, and prepares the image with recovery as its saved default. Its save set is the image's extent and the block ranges the grow writes into the groups it adds, computed by milestone 47's tool from the new image's superblock, the root's offset on the target and the target's size. Its phase 5 rehearses this milestone's sitting in QEMU on a disk of the Lenovo's size and layout, with the restore made through `rach2-recover` and through the older recovery's `reflash`.
* Milestone 24's boot switches skip individual device probes, milestone 47 keeps a boot option for polled AHCI, milestone 46's kernel prints its memory map at boot, and netconsole sends the kernel log as frames the host captures before SSH is up.

## Entries

An entry has: a date; the milestone that found it; its severity, with a loss of data or a panic first; how to reproduce it; the contract it breaks, with file and line; what it depends on; the test that will show it fixed; and, once done, the commit and the proof. The two above are the first entries, numbered 1 and 2, and the list is worked in order of severity and dependency, not of the code it touches. What the machine shows in the sitting is entered the same way and worked in phase 4.

An entry is fixed when its test passes in QEMU and its review has run. A fix made after the sitting reaches the Lenovo in milestone 52's sitting, which is that milestone's deployment, and the entry says so; a kernel fix may go on earlier through `bootctl` over SSH when the machine is on and the owner approves that write, and this is never required. The documents may be written once every entry is fixed. A fix without which the machine does not run at all calls the owner again with the same preparation as the sitting, and the report says that it did.

1. The stopped stick: reproduce the never-answering reset and the removal with completions still pending, in QEMU with a stick that stops answering, and fix what the kernel does with the stale completions.
2. The pooled runs: find why a suite that passes alone fails in the pool, which is milestone 52's scheduling work brought forward as far as the cause requires.
3. The acceptance's stick: the metal acceptance takes the stick it writes by identity from its arguments and refuses a stick that contains a Rach2 recovery ISO; proved in QEMU with three sticks plugged, two of them recovery sticks, and by the mutation that drops the refusal.

## Decisions

### The code is reviewed before the machine is written

The installer, the kernel and the recovery image this milestone's fixes changed are what the sitting writes with, and `CLAUDE.md` puts a destructive write after the review of the code that performs it. So the review of phases 1 and 2 runs in full before the sitting: build from clean, the full set, the Codex review, every finding fixed, the full set again. The fixes made after the sitting are reviewed once, as one batch, at the close.

### The owner is called once, with everything ready

Before the owner is called, the following are done and recorded in `notes.md`:

* The reviewed kernel and images built, their hashes recorded, and the stick image booted in QEMU.
* The save-set tool run against the Lenovo's recorded geometry: its ranges, their total bytes, and the new stick's free space recorded; milestone 49's rehearsal run again against that manifest.
* The restore commands written out twice, for `rach2-recover` on the new stick and for `reflash` on milestone 43's stick.
* The third stick's layout decided, one exFAT volume labelled `RACH2TEST` over the whole stick, written in the sitting's first step; the pull test's read command, a `dd` of the whole stick to nowhere over SSH; its readiness signal, the first 64 MiB reported by the host; and its deadline, the removal reported by the kernel within one minute of the pull.
* Every answer the installer asks for prepared: the account name and the key in `notes.md`, the passwords in the credential file on the host that the acceptance reads.
* The sitting's commands in order, with what each shows, what the owner is asked, the observable condition for each physical action, and which boot switch answers which symptom.

Nothing in the sitting is discovered on the day. A step that turns out to need something not prepared stops the sitting with the disk untouched or restored, and the owner is called again only after the gap is closed.

### The install replaces the grow

Milestone 47 proved `growroot` in QEMU. The Lenovo does not get its root grown in place; the installer writes the sealed image and grows the ext2 root into the free space of the disk, as milestone 49 decided. The saved set is what the installer writes: the image's extent and the growth ranges from the tool. The rest of the disk is not written and is not saved.

### A known-working recovery path stays outside the change

Milestone 43's stick is kept as it is. If the new stick fails to boot, or fails after the disk has been written, the older stick boots and its `reflash` puts the saved set back. Its boot-time repair may write into a damaged root before the restore runs; the restore rewrites every sector of the set, and the sectors outside the set were unused before the install, so the repair changes nothing the restore does not overwrite. Milestone 49's rehearsal proves this path with a recovery ISO built at the commit milestone 43 installed. The new stick is a second stick, and the acceptance and the pull test use a third stick that carries nothing; milestone 43's stick is not written at all, and the new stick's boot image is not written after its preparation; the save goes to the new stick's data volume.

### The pull test is the removal half of entry 1

There is no way to make a physical stick stop answering on demand. On the Lenovo the third stick is read over SSH from the host, and while the read runs the owner pulls it when the host says so; the kernel must report the removal, keep running, and take the stick back when it is plugged in again. The never-answering half is proved in QEMU by entry 1.

### What the owner does

At the desk, at the start: approves the write of the new stick and the formatting of the third, naming each as the host reports it, and plugs both into the notebook. At the notebook, at the start: powers it on, plugs in the keyboard, mouse, hub, card and the third stick as milestone 43 had them, and chooses the new stick as the boot device when asked. At the desk: approves each later write by naming the device the installer or the host reports, including the change of the saved boot default. At the notebook, once: types the target disk's name into the installer and the prepared answers. At the notebook, once more: pulls the third stick when the host says so and plugs it back. At the notebook, last: moves the keyboard and then the mouse through each of the three sockets and the hub, typing a line and moving the pointer at each, with the host reading what arrived. The owner's part ends before the load runs.

## Phases

### Phase 1: the fixes

The entries recorded so far, in order of severity, each with the smallest suite that covers it and a mutation for any new check.

### Phase 2: the review before the machine

Once and in this order: build from clean; the full set; everything that comes up fixed; the whole milestone's diff so far to Codex in units of six files, most serious first; every finding fixed; the full set again. Then the preparation the decision above lists, ending with the rehearsal in QEMU and the sitting's commands in `notes.md`.

### Phase 3: the sitting

One sitting, in this order, each step with a durable state before the next, each timed as it runs, the boot state recorded after each transition:

1. The new stick written on the host from the proved image and the third stick formatted, the owner naming each, the new stick hashed back and both identities recorded.
2. Power on, the devices and the sticks plugged in, the netconsole capture started on the host, the machine on the network.
3. The new stick booted on the Lenovo, its recovery menu shown to work, `rach2-recover` listing the disks with the Lenovo's disk shown as the rule describes, and the memory map from the netconsole capture saved for milestone 46's fixture.
4. The save: the owner names the disk; every sector of the save set copied to the host and to the new stick; the stick's copy hashed back against the host's. A failed hash ends the sitting here, with the disk untouched.
5. Recovery made the saved boot default, with the owner's approval, and the change read back.
6. The install: `rach2-install` on the Lenovo's disk, the owner naming it and giving the prepared answers; the readback against the manifest hash; the trial boot of the candidate with recovery still the saved default. An install that fails is restored from the new stick with the written commands, and the sitting ends.
7. From the running candidate: the root's identity, its writable mount, its grown capacity and the installed credentials checked; the acceptance with those credentials and the third stick; only then `bootctl commit`.
8. The devices: the metal acceptance on the installed system; the reboot from SSH; the pull test on the third stick; the keyboard and the mouse at each socket and through the hub, the host reading each line typed and each pointer movement from the console over SSH.

The owner's part ends here. Then, unattended: twenty sessions running milestone 46's load row for an hour; the numbers into `hardware.md` and the Readme's Limits section; the sitting's times, including the stick's sustained read during the save and the install against milestone 49's ten-minute budget. What the machine shows becomes entries. The notebook may be turned off after the load; nothing later in this milestone needs it.

### Phase 4: the machine's entries

The entries phase 3 added, worked as phase 1 worked its own.

### Phase 5: the close

Once and in this order: build from clean; the full set; the diff since phase 2 to Codex in units of six files, most serious first; every finding fixed; the full set again. Record the results, the sitting's log with its times and boot states, and the numbers in `notes.md` and the milestone's Readme. Add the index entry.

## Risks

* Notify the owner when a severe problem is added. One entry that loses data matters more than twenty that do not.
* The sitting is the first run of five milestones' kernel changes on real hardware, and the new recovery image shares them. A new stick that does not boot ends the sitting at step 3 with the disk untouched. A boot that stops before SSH has the netconsole capture and the screen; the boot switches skip individual probes, and polled AHCI is a boot option. A damaged install is restored from the new stick, or from milestone 43's stick if the new one no longer boots, with the written commands.
* The save writes the image's extent and the growth ranges to the stick, the path that once failed. The hash back is the check.
* An interruption during the image copy leaves the disk's own loader and recovery partition half written; the stick is the boot device then, and its recovery is what boots. Milestone 49's rows prove both boundaries.
* Two sittings, this one and milestone 52's, are the plan. A hardware failure that needs the owner's hands calls a third, prepared the same way, and is reported as such.
