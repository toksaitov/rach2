#ifndef _NETCONSOLE_H_
#define _NETCONSOLE_H_

#include "../lib/kint.h"

/*
 * netconsole: the kernel log, repeated onto the wire as broadcast Ethernet
 * frames.
 *
 * A machine without a serial port has only its screen to show the log on,
 * and a screen cannot be scrolled back or compared with another boot's. This
 * copies the log's records into broadcast frames that any host on the
 * segment can capture, which makes the boot log a file on the development
 * machine. The records and nothing else: what the debug shell prints goes to
 * the screen and the port alone.
 *
 * It works below IP. There is no address to configure, no ARP to answer and
 * no peer to reach, so it works on the first boot on an unknown network, and
 * goes on working when the address configuration is what is wrong. The cost
 * is that the frames are broadcast and unacknowledged: it is a diagnostic,
 * and a frame lost on the wire is lost.
 *
 * Frame payload, after the 14-byte Ethernet header, all fields big-endian:
 *
 *     0   4  magic "RCH2"
 *     4   4  boot id, one random value per boot and never 0, so a capture
 *            that spans a reboot can be split by boot
 *     8   4  sequence number, from 0, so a gap in a capture is visible
 *    12   2  payload length in bytes, at most NETCONSOLE_PAYLOAD_MAX
 *    14   2  checksum over this header and the payload, with the field zero
 *    16   n  the log's characters, lines ending in a newline with no
 *            carriage return (the serial port adds those, this does not);
 *            a frame may end in the middle of a line and the next go on
 *
 * A receiver checks a frame by computing the same checksum over the whole
 * payload, the field included, and finding 0, as for the IP header.
 */

/* Local experimental EtherType 1 (IEEE Std 802), which is what this is. */
#define NETCONSOLE_ETHERTYPE 0x88b5

/* The header above, and the most log bytes one frame has. */
#define NETCONSOLE_HDR_LEN     16
#define NETCONSOLE_PAYLOAD_MAX 512

/**
 * Take one character of a log record, from `klog`'s netconsole destination,
 * which feeds this whether or not the machine has a serial port.
 *
 * The character joins a ring of 8191, or is counted and dropped when the
 * ring is full. Before `netconsole_enable` the characters only wait there.
 * After it, a newline starts `netconsole_flush`, and so do 512 characters
 * waiting unless the last drain was held for the link or a refused frame. A
 * character that comes while a frame is being sent only joins the ring, so a
 * driver that logs an error from inside a send does not send again.
 *
 * Any context, interrupt handlers and the panic path included: it allocates
 * nothing, waits for nothing, and takes `net_lock`.
 */
void netconsole_putc(char c);

/**
 * Choose the boot id, start sending, and send what the ring holds.
 *
 * Called once, at boot, after a card is up, and only when the boot line has
 * `rach2.netconsole`: broadcasting the kernel log is a decision for the
 * operator of the network, not a default. A second call does nothing.
 */
void netconsole_enable(void);

/**
 * Send the characters the ring holds when it is called, in frames of up to
 * NETCONSOLE_PAYLOAD_MAX through the first interface; a character leaves the
 * ring only once the card has taken its frame.
 *
 * Does nothing before `netconsole_enable` or while a frame is being sent.
 * While the first interface is not running (`netdev_running`), or after a
 * frame the card refuses, the characters stay in the ring and the drain is
 * held until the next newline. Takes `net_lock` for each frame.
 */
void netconsole_flush(void);

/** Whether the log is going out on the wire. */
int netconsole_is_enabled(void);

/** Log the EtherType, the boot id, the frames sent and the characters
 * dropped; nothing before `netconsole_enable`. */
void netconsole_print_info(void);

#endif /* _NETCONSOLE_H_ */
