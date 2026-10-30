# Lenovo acceptance

The final sitting with the owner has not been held.

On 2026-10-30, task #1478 added the native case `lenovoacpi`. It checks ACPICA's error-free initialization, the EC's ports and enabled GPE, the three named input devices, the brightness events from `_Q11` and `_Q12`, and the installed acpid's changes to the requested and actual Intel backlight levels. It puts the original level back. The case runs first in the native collection, while the boot log is still present.

The probe compiled for i686 and x86-64 with `-Wall -Wextra -Werror`. No suite, host test or style check ran in this worker session. The probe has not run on the Lenovo, and this session changed no boot slot or disk there.

Acceptance still needs a trial boot with the milestone's kernel and user space, the native case, and the owner at the machine for each brightness and volume key, mute, poweroff, one-minute screen blanking and wake by a key, and brightness across a reboot. The previous device and backlight trials are recorded in issues #1458 and #1470; they do not supply the final sitting's results.
