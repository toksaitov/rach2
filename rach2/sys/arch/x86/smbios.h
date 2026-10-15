#ifndef _SMBIOS_H_
#define _SMBIOS_H_

#include "../../lib/kint.h"

/*
 * smbios: what the firmware says about itself.
 *
 * The firmware publishes a table, found by the entry point UEFI's
 * configuration table names or by an anchor this scans the read-only region
 * below a megabyte for. Only one thing is read out of it: the
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

/** Record the UEFI system table the loader passed, at physical `systab` or
 * 0 for none, and whether its pointers are 64 bits wide (`wide`); its
 * configuration table names the entry point. Drops the cached vendor.
 * Called from `platform_firmware_init`. */
void smbios_init(uint64_t systab, int wide);

/**
 * Whether the BIOS vendor's name begins with `prefix`, case-sensitively; an
 * empty `prefix` matches nothing.
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
 * The structures are what an anchor yields, so this is where the reader of
 * the structures is put under test; the search for the anchor is put under
 * test through `smbios_set_memory`. NULL restores the search, and the cached
 * answer is dropped either way, so a test may present one table after
 * another.
 */
void smbios_set_table(const void *at, unsigned int len);

/** For a host test: read physical memory through `read`, which gives the
 * address of `len` bytes at physical `at` or NULL, in place of the kernel's
 * mappings; NULL restores them. The cached answer is dropped. */
void smbios_set_memory(const uint8_t *(*read)(uint64_t at, uint32_t len));

#endif /* _SMBIOS_H_ */
