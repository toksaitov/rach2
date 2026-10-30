# Lenovo acceptance

The final sitting with the owner has not been held.

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

Acceptance still needs the owner at the machine for each brightness and volume key, mute, poweroff, one-minute screen blanking and wake by a key, and brightness across a reboot. The previous device and backlight trials are recorded in issues #1458 and #1470; they do not supply the sitting's results.
