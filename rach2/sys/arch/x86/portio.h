#ifndef _PORTIO_H_
#define _PORTIO_H_

#include "../../lib/kint.h"

/*
 * The x86's I/O ports: the instructions that read and write them, which the
 * PC's own devices (the 8259, the PIT, the 8042, the serial port, ATA) and
 * the older PCI devices are reached through. No shared code uses them.
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
