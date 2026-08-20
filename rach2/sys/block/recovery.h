#ifndef _RECOVERY_H_
#define _RECOVERY_H_

/*
 * recovery: the string that says whether this kernel may write its own disk.
 *
 * Milestone 25 splits disk authority in two. Level one is the boot partition
 * and the boot loader's gap, bounded windows every image may write, because
 * installing a kernel or updating GRUB is ordinary work. Level two is the whole
 * device at any offset, which is what reflashing a broken root needs and what
 * no everyday boot has any business doing: a mistake there destroys the machine
 * rather than one file.
 *
 * The two are told apart at build time, not at boot. A token on the kernel
 * command line would be editable at the GRUB prompt by anyone standing at the
 * machine, which is the same objection this project already recorded when it
 * made the read-only disk policy a compile-time switch. So the everyday kernel
 * compiles a `/dev/disk0` whose write returns `-EPERM` with no way to ask
 * again, and only a kernel built with `-DRACH2_RECOVERY` compiles one that can
 * write at all.
 *
 * This is NOT `RACH2_DISK_READONLY`, and cannot be. That switch is enforced
 * below devfs, in the drivers' command allowlist, so a kernel carrying it
 * cannot write the boot partition either, and both the everyday and the
 * recovery kernel need to. The two switches are independent: this one decides
 * only whether devfs exposes a writable whole-disk node.
 *
 * The banner below is how a built kernel says which of the two it is, the same
 * way the disk policy's does, and `tools/check_recovery.py` reads it in both
 * directions: a recovery artifact must carry the recovery banner, and an
 * everyday one must not. That catches a kernel built without the switch and
 * offered as recovery, which is the mistake worth catching early; it is not
 * proof against a tampered artifact, since the banner and the handler are
 * separate conditionals on the same macro. `make authority-test` boots both
 * kernels and is the proof of what they actually do.
 */

/**
 * The policy string built into the artifact, for `tools/check_recovery.py`.
 *
 * This file holds no decision of its own. Whether the whole-disk write exists
 * is settled by `#ifdef RACH2_RECOVERY` where that handler is written, in
 * `sys/fs/devfs.c`, because the choice is which function body to compile and
 * not a value to test at run time. What lives here is the string that lets the
 * choice be read back off a built kernel, and it is a second conditional on the
 * same macro rather than the same one, which is why the checker catches a
 * mis-built artifact and not a tampered one.
 */
extern const char recovery_policy_banner[];

/**
 * Whether this build publishes the whole-disk window without being asked for it.
 *
 * The everyday kernel publishes `/dev/disk0` only under `rach2.diskdiag`, so no
 * image grows a raw whole-disk node by accident. The recovery kernel publishes
 * it always: rewriting the disk is the only reason that build exists, and the
 * moment it is needed is the moment nothing else can repair the machine, which
 * is no time to discover that a menu entry was missing a token.
 */
int recovery_publishes_disk(void);

#endif /* _RECOVERY_H_ */
