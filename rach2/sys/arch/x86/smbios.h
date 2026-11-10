#ifndef _SMBIOS_H_
#define _SMBIOS_H_

#include "../../lib/kint.h"

/*
 * smbios: what the firmware says about itself.
 *
 * The firmware publishes a table, found by the entry point UEFI's
 * configuration table names or by an anchor this scans the read-only region
 * below a megabyte for. Three strings are read out of it. The vendor of the
 * BIOS decides whether a reset command meant for one firmware's
 * system-management handler is sent at all: a machine whose firmware is not
 * the one that command belongs to must not be sent it, since the byte
 * reaches a handler that was written for something else. The system's maker
 * and product name choose a driver's quirks for a machine, as Linux's DMI
 * tables choose them.
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
 * Whether the system's manufacturer holds `maker` and its product name holds
 * `name`, as the System Information structure gives them and as Linux's
 * `DMI_MATCH` of DMI_SYS_VENDOR and DMI_PRODUCT_NAME matches them: each a
 * substring, case-sensitively. A table that names neither matches nothing.
 * A driver's quirk for a machine is chosen by this, as Linux's DMI table
 * chooses it.
 */
int smbios_system_is(const char *maker, const char *name);

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
