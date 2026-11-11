/* The one EFI runtime service the kernel calls: ResetSystem, for reboot=e. */
#ifndef _EFIRT_H_
#define _EFIRT_H_

#include "../../boot.h"

/**
 * Find ResetSystem through the system table the loader passed and make
 * what a call needs, once, at boot, after the heap: on x86-64 a PML4 whose
 * lower half maps each of the EFI map's runtime ranges at its physical
 * address, since the loader never called SetVirtualAddressMap. Leaves the
 * service uncallable, and says why in `efirt_absent`, when the machine did
 * not boot by UEFI, the firmware's word size is not the kernel's, a table
 * is not what it should be, ResetSystem lies outside every runtime range or
 * the tables could not be had.
 */
void efirt_init(const struct boot_info *boot);

/** NULL when `efirt_reset` will call ResetSystem; otherwise why it will
 * not, as a log's words. */
const char *efirt_absent(void);

/**
 * Call ResetSystem with EfiResetWarm when `warm` is nonzero and
 * EfiResetCold otherwise, a status of EFI_SUCCESS and no data, as Linux's
 * `efi_reboot` does (drivers/firmware/efi/reboot.c:13-47). Interrupts must be
 * off and the other processors stopped. Returns only when the service does,
 * or cannot be called, so that the caller goes on to its next method.
 */
void efirt_reset(int warm);

#endif /* _EFIRT_H_ */
