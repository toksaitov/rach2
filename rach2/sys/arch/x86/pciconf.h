#ifndef _PCICONF_H_
#define _PCICONF_H_

#include "../../lib/kint.h"

/*
 * pciconf: the PC's configuration space beyond ../../pci.h. The platform
 * gives the ECAM window of segment 0 it mapped, and from then on every
 * configuration access to a bus inside the window goes through it.
 */

/** Use the ECAM window mapped at `va` for buses `first` to `last`, `va`
 * being bus `first`'s space. */
void pci_ecam_set(uintptr_t va, uint8_t first, uint8_t last);

/**
 * A dword of a function's extended configuration space, offsets 256 to
 * 4095, which only ECAM reaches; below 256 by whichever mechanism serves
 * the bus. All ones for an offset of 256 or more when no ECAM window covers
 * the bus.
 */
uint32_t pci_read_config32_ext(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset);

/** Write a dword of extended configuration space; nothing when no ECAM
 * window covers the bus and the offset is 256 or more. */
void pci_write_config32_ext(uint8_t bus, uint8_t device, uint8_t function, uint16_t offset,
                            uint32_t value);

#endif /* _PCICONF_H_ */
