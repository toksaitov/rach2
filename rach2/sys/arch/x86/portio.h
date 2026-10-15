#ifndef _PORTIO_H_
#define _PORTIO_H_

#include "../../lib/kint.h"

/*
 * portio: the x86's I/O port instructions, which the PC's own devices (the
 * 8259, the PIT, the 8042, the serial port, ATA) are reached through. Outside
 * arch/ only the drivers of PC devices include this header: ATA, the Sound
 * Blaster 16 and its DMA controller, and VGA's text and graphics modes. A PCI
 * driver reaches its I/O space through `pci_io_read8` and the rest (pci.h).
 */

/** Write byte `val` to I/O port `port`. */
void outb(uint16_t port, uint8_t val);

/** Write word `val` to I/O port `port`. */
void outw(uint16_t port, uint16_t val);

/** Write double-word `val` to I/O port `port`. */
void outd(uint16_t port, uint32_t val);

/** Read a byte from I/O port `port`. */
uint8_t inb(uint16_t port);

/** Read a word from I/O port `port`. */
uint16_t inw(uint16_t port);

/** Read a double-word from I/O port `port`. */
uint32_t ind(uint16_t port);

/** Read `count` 16-bit words from I/O port `port` into `buffer`. */
void insw(uint16_t port, void *buffer, uint32_t count);

/** Write `count` 16-bit words from `buffer` to I/O port `port`. */
void outsw(uint16_t port, const void *buffer, uint32_t count);

/** Aliases under the conventional outl/inl names. */
#define outl outd
#define inl  ind

#endif /* _PORTIO_H_ */
