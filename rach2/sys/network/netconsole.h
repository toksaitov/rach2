#ifndef _NETCONSOLE_H_
#define _NETCONSOLE_H_

#include "../lib/kint.h"

/*
 * netconsole: the kernel's log, repeated onto the wire.
 *
 * The machine this kernel is being carried to has no serial port, so on the
 * first boots there is nothing to read but the screen, and a screen cannot be
 * scrolled back or diffed. This copies the log's records into broadcast
 * Ethernet frames that any host on the segment can capture, which turns the
 * boot log into a file on the development machine. The records and nothing
 * else: what the debug shell prints goes to the screen and the port alone and
 * is never broadcast.
 *
 * It sits below IP on purpose. There is no address to configure, no ARP to
 * answer and no peer to be reachable, so it works on the first boot on an
 * unknown network, and it keeps working when the address configuration is the
 * thing that is wrong. The cost is that the frames are broadcast and
 * unacknowledged: this is a diagnostic, not a log transport.
 *
 * Frame payload, after the 14-byte Ethernet header, all fields big-endian:
 *
 *     0   4  magic "RCH2"
 *     4   4  boot id, one random value per boot, so a capture that spans a
 *            reboot can be split by boot
 *     8   4  sequence number, from 0, so a gap in a capture is visible
 *    12   2  payload length in bytes
 *    14   2  checksum over this header and the payload, with the field zero
 *    16   n  the record text, which ends in one newline and carries no
 *            carriage return; the serial port adds those, this does not
 *
 * A receiver verifies by summing the whole payload including the checksum
 * field and checking for zero, the same convention as the IP header.
 */

/* Local experimental EtherType 1 (IEEE Std 802), which is what this is. */
#define NETCONSOLE_ETHERTYPE 0x88b5

/* The header above, and the most log bytes one frame has. */
#define NETCONSOLE_HDR_LEN     16
#define NETCONSOLE_PAYLOAD_MAX 512

/**
 * Take one character of a log record.
 *
 * Called from the log's fourth destination, which consults nothing serial, so
 * a machine with no COM1 still feeds this. Until `netconsole_enable`,
 * characters only accumulate in a bounded buffer; after it, a completed line
 * is sent. This never transmits while a transmit is already in progress, so a
 * driver that reports an error from inside a flush cannot recurse.
 */
void netconsole_putc(char c);

/**
 * Start transmitting, and send whatever was buffered before now.
 *
 * Called once, after a network device is up, and only when the boot command
 * line asked for it: broadcasting the kernel log is a decision for the
 * operator of the network, not a default.
 */
void netconsole_enable(void);

/** Send whatever is buffered now, in as many frames as it takes. */
void netconsole_flush(void);

/** Whether the log is going out on the wire. */
int netconsole_is_enabled(void);

/** Record the ethertype, the boot id, and the frame and drop counts. */
void netconsole_print_info(void);

#endif /* _NETCONSOLE_H_ */
