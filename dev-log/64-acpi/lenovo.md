# Lenovo acceptance

The sitting with the owner was held on 2026-10-30 in two parts. The first found three faults and the second passed their fixes; brightness across a reboot is checked in the release.

On 2026-10-30, task #1478 added the native case `lenovoacpi`. It checks ACPICA's error-free initialization, the EC's ports and enabled GPE, the three named input devices, the brightness events from `_Q11` and `_Q12`, and the installed acpid's changes to the requested and actual Intel backlight levels. It puts the original level back. The case runs first in the native collection, while the boot log is still present.

## The native case on a trial boot

On 2026-10-30 the x86-64 kernel of milestone/64-acpi at 9a2be2906 was put in the Lenovo's candidate slot with `bootctl kernel` and booted once with `bootctl trial candidate`. The installed root is the last release's and has no acpid, so the milestone's BusyBox, `acpi.map`, `acpid.conf` and the actions under `/etc/acpi` were copied to `/dev/shm` and acpid was started from there with `-c`, `-a` and `-M`; the release installs the milestone's root before its native run and needs none of this.

The first look at the boot log found a bug in the case: it took the first ACPICA record that begins with a number, which on the Lenovo is `5 ACPI AML tables successfully acquired and loaded`, as the count of objects, and would have failed. 8dad2fa08 makes it look past such records for `N objects, E errors`.

With that fix, `native-deploy run ... -- lenovoacpi` passed all nine of its assertions, `pass lenovoacpi: status 0, end 0 value 0, complete 1, cleanup confirmed, removed 1`:

* `acpi: acpica: 2872 objects, 0 errors`, and no ACPICA error record.
* `acpi: ec: \_SB.PCI0.LPCB.EC0 at ports 0x62 and 0x66, gpe 0x7` and `acpi: ec: gpe 0x7 enabled, status 0x00`.
* The devices "AT Translated Set 2 keyboard", "Power Button" and "Video Bus".
* `_Q11` gave KEY_BRIGHTNESSDOWN and acpid's `brightness/down` moved the level from 468 to 375; `_Q12` gave KEY_BRIGHTNESSUP and `brightness/up` moved it back to 468, the actual level following each time.
* The level was put back to 937.

The Lenovo was then rebooted to its installed kernel in the fallback slot; nothing was committed, and the files under `/dev/shm` were removed. The prepared payload and the run's results stay under `/var/native` as the deployment leaves them.

## The sitting

Both parts ran on trial boots of the x86-64 kernel of milestone/64-acpi, with the milestone's acpid, actions and mixer started from `/dev/shm` as above, the console's blank set to one minute with `ESC [ 9 ; 1 ]`, and the backlight, the panel registers and each new kernel record logged once a second over SSH. The release, 66b7ce0c, stayed in the fallback slot.

The first part, on the kernel at 0790450bc, passed the brightness keys, the volume keys and mute, and blank and wake by Shift or any key; one mute press was lost. It found three faults, each made a task:

* #1526: brightness-up on a blanked console left the panel dark in two tries of three, though the kernel recorded the unblank and the PWM registers read on, and the EC then gave no queries for the brightness keys. Running `\_SB.PCI0.LPCB.EC0._Q12` through `/proc/acpi/call` brought both back. The kernel blanked by a duty cycle of 0 under the firmware's own backlight path; c179580d1 blanks by the panel sequencer's backlight enable, PP_CONTROL bit 2, as i915's `intel_pps_backlight_power` does, keeping the duty cycle the firmware sees.
* #1525: the power button restarted the machine instead of switching it off, after two different shutdown messages. 82311e3a0 powers off through ACPICA, which runs `_PTS(5)`, disables every GPE and clears every status before it writes SLP_EN.
* #1527: ACPICA's table list and setup records were 43 of the boot's 187 records on the console. fd0051c0b logs them at debug, so the console shows `acpi: acpica: 2872 objects, 0 errors` and errors alone.

The second part, on the kernel at c179580d1 (544b5399), passed all three fixes. Of 16 blanks, each was ended by the owner's key: five by brightness-up, five by brightness-down and six by Shift. The panel came back every time at the level the keys had set, and the brightness keys answered after each. One short press of the power button showed the power-off records and switched the Lenovo off, and it stayed off; the SSH log ends at the press, since the network goes first. The boot screen showed one ACPICA line.

Both parts also showed xHCI commands that got no answer at 4 to 6 s after some restarts, under the release as well, with the hubs on ports 2 and 10 refused and the keyboard and mouse behind them lost until they were plugged in again. That was #1528: two slow devices that had been on the EHCI side until the port switch did not take their address within the kernel's 1 s. f89b48105 gives each port reset 50 ms of recovery, Address Device Linux's 5 s and four tries, each after another port reset. Eight trial boots in a row, each after a `reboot` from the trial kernel, logged no such record and found every device by 4 s. A restart after the power button was not tried with the fix.

The fallback slot had held an unreleased build since 2026-10-29, which GRUB booted twice with saved_entry=fallback during the first part. b33b9f7eb, on master, makes the release copy each committed release into the fallback slot with `bootctl fallback`, and the machine skill checks both slots before a trial.

After each part the release was put back in both slots, checked by each slot's SHA-256, and the backlight written to 30%.
