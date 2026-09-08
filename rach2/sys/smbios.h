#ifndef _SMBIOS_H_
#define _SMBIOS_H_

/*
 * smbios: what the firmware says about itself.
 *
 * The firmware publishes a table in the read-only region below a megabyte,
 * found by an anchor this scans for. Only one thing is read out of it: the
 * vendor of the BIOS, which is what decides whether a reset command meant for
 * one firmware's system-management handler is sent at all. A machine whose
 * firmware is not the one that command belongs to must not be sent it: the
 * byte reaches a handler that was written for something else.
 *
 * Nothing here writes, and nothing here is required. A machine with no table,
 * or a table this cannot read, reports an empty vendor and the caller falls
 * back to what it would do on an unknown machine.
 */

/**
 * The BIOS vendor string, or "" when the table is absent or unreadable.
 *
 * Read once and kept: the table is in memory the firmware leaves alone, but
 * scanning for it on every call would be a scan of 64 KiB for an answer that
 * cannot change.
 */
const char *smbios_bios_vendor(void);

/**
 * Whether the BIOS vendor's name begins with `prefix`, case-sensitively.
 *
 * The vendor strings are the firmware's own text, and a machine's is matched
 * by its beginning because the rest carries a version and a year that differ
 * between machines of the same firmware.
 */
int smbios_vendor_is(const char *prefix);

/**
 * Read the structures at `at`, `len` bytes of them, rather than scanning for
 * an anchor; for a host test.
 *
 * The structures are what an anchor yields, so this is where the reader is
 * put under test. The anchor's own walk is not: its address field is thirty-
 * two bits, and a test on a sixty-four bit host cannot put a table where such
 * a field can point at it. NULL restores the scan, and the cached answer is
 * dropped either way, so a test may present one table after another.
 */
void smbios_set_table(const void *at, unsigned int len);

#endif /* _SMBIOS_H_ */
