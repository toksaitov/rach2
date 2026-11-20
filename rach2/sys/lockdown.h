/*
 * lockdown.h: Linux's kernel lockdown, a level that, once raised, refuses
 * the operations that could change the running kernel (integrity) or also
 * read its memory (confidentiality), for root as for any caller.
 */
#ifndef _LOCKDOWN_H_
#define _LOCKDOWN_H_

/* The levels, and the operations each guards, in Linux's order of `enum
 * lockdown_reason`: an operation is refused while the level is at or
 * above its number. */
#define LOCKDOWN_NONE                0
#define LOCKDOWN_DEV_MEM             1 /* /dev/mem, /dev/kmem and /dev/port */
#define LOCKDOWN_PCI_ACCESS          2 /* a PCI function's config and BARs */
#define LOCKDOWN_MSR                 3 /* a write of a model-specific register */
#define LOCKDOWN_ACPI_CALL           4 /* /proc/acpi/call, an unsigned module */
#define LOCKDOWN_DEBUGFS             5 /* debugfs beyond reading 0444 files */
#define LOCKDOWN_INTEGRITY_MAX       6
#define LOCKDOWN_CONFIDENTIALITY_MAX 7

/**
 * 0 when the level lets the operation `what`, a LOCKDOWN_* below
 * LOCKDOWN_CONFIDENTIALITY_MAX, go on, or -EPERM, with a line in the
 * kernel log naming it, as Linux's `security_locked_down`. The first call
 * takes the level from the boot's `lockdown=integrity` or
 * `lockdown=confidentiality`.
 */
int lockdown_refuses(int what);

/** Add /sys/kernel/security/lockdown, which shows the levels with the one in
 * force bracketed and takes a higher one from a writer its mode allows, as
 * Linux's securityfs file. */
void lockdown_sysfs_start(void);

#endif /* _LOCKDOWN_H_ */
