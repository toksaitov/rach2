#ifndef _PCICONF_H_
#define _PCICONF_H_

#include "../../lib/kint.h"
#include "../../pci.h"

/*
 * pciconf: what the PC's platform alone sets up of configuration space. The
 * platform gives the ECAM window of segment 0 it mapped, and from then on
 * every configuration access to a bus inside the window, those of
 * ../../pci.h, goes through it.
 */

/** Use the ECAM window mapped at `va` for buses `first` to `last`, `va`
 * being bus `first`'s space. */
void pci_ecam_set(uintptr_t va, uint8_t first, uint8_t last);

#endif /* _PCICONF_H_ */
