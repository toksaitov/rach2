#ifndef _RECOVERY_H_
#define _RECOVERY_H_

/*
 * recovery.h: whether this kernel was built to write its own disks, and the
 * string that says so in the built kernel.
 *
 * Writing a whole disk or a partition node at any offset is what reflashing a
 * broken root needs, and a mistake there destroys the machine. The boot
 * partition and the boot loader's gap are bounded windows that every build
 * may write, since installing a kernel or updating GRUB is ordinary work. The
 * choice is made when the kernel is built, not at boot, because a token on
 * the kernel command line can be edited at the GRUB prompt: the everyday
 * kernel compiles a write handler for the disk nodes that returns `-EPERM`
 * and has no other path, and only a kernel built with `-DRACH2_RECOVERY`
 * compiles one that writes (`dev_disk_write` in `fs/devfs.c`).
 *
 * This switch is independent of `RACH2_DISK_READONLY`, which is enforced in
 * the drivers by the command list of `diskcmd.c`, so a kernel built with it
 * cannot write the boot partition either. This one decides only whether
 * devfs has a writable disk node.
 */

/**
 * The policy string built into the kernel, "rach2: disk authority: recovery"
 * or "rach2: disk authority: ordinary", for `check_recovery.sh`.
 *
 * The string makes no decision. The write handler is chosen by its own
 * `#ifdef RACH2_RECOVERY` in `fs/devfs.c`, and this string is a second
 * conditional on the same macro, so the checker finds a kernel built without
 * the switch and offered as recovery, but not a tampered one. `tests/run
 * authority` boots both kernels and tests what they do.
 */
extern const char recovery_policy_banner[];

/**
 * Whether this build probes for a disk at boot and publishes its whole-disk
 * window without `rach2.diskdiag`, and puts the partition nodes under the
 * writer reservation: 1 in the recovery build, 0 otherwise.
 *
 * The everyday kernel probes only under `rach2.diskdiag`, and otherwise
 * publishes the disks its boot registered, whose writes its handler refuses.
 * The recovery kernel always probes, since writing the disk is what it is
 * built for and a boot entry should not need an extra token for it.
 */
int recovery_publishes_disk(void);

#endif /* _RECOVERY_H_ */
