#ifndef _BOOTLOG_H_
#define _BOOTLOG_H_

#include "lib/kint.h"

/*
 * bootlog: what the machine printed, kept so it can be asked for afterwards.
 *
 * A machine with no serial port has three bad ways to be read and this is the
 * fourth. The screen scrolls the whole log past in under a second and has
 * about twenty-five lines of it, so reading it means filming the boot. The
 * netconsole broadcasts every line, but only to whoever was already capturing
 * when the power came on, so a question thought of afterwards is a reboot.
 * Holding the screen at the end helps only somebody standing in front of it.
 *
 * So the kernel keeps its own copy. Everything written to the serial port,
 * from the first line, lands in a fixed ring here, and `/dev/bootlog` hands it
 * out. `cat /dev/bootlog` over SSH answers "what did this machine say while it
 * was starting" at any point afterwards, with nothing arranged in advance.
 *
 * It is always capturing. There is no flag to remember to pass, because the
 * boot worth reading is usually the one nobody expected to have to read.
 */

/* Enough for a whole boot with room to spare: one is about 2.5 KB. When it
   fills, the OLDEST bytes go, since a machine that has been up for a while has
   a boot log worth less than what it is saying now. */
#define BOOTLOG_SIZE 16384

/** Take one character. Called from `serial_putc`, so from anywhere. */
void bootlog_putc(char c);

/**
 * Copy at most `cap` bytes starting at `offset` into `buf`.
 *
 * The log reads as a flat byte stream from the oldest character kept, so a
 * reader seeking around it sees one consistent order. Returns the number of
 * bytes copied, 0 at the end.
 */
uint32_t bootlog_read(char *buf, uint32_t cap, uint32_t offset);

/** How many bytes are held. */
uint32_t bootlog_len(void);

#endif /* _BOOTLOG_H_ */
