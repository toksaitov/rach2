#ifndef _NETCONSOLE_H_
#define _NETCONSOLE_H_

#include "../lib/kint.h"

/*
 * netconsole: the kernel log, repeated onto the wire, as UDP datagrams to the
 * targets of Linux's `netconsole=` and as broadcast Ethernet frames.
 *
 * A machine without a serial port has only its screen to show the log on,
 * and a screen cannot be scrolled back or compared with another boot's. This
 * copies the log's records onto the network, which makes the boot log a file
 * on the development machine. The records and nothing else: what the debug
 * shell prints goes to the screen and the port alone. Both forms are sent by
 * calling the card's transmit with interrupts off, which reclaims and waits
 * for its ring by polling, so they work from a panic as Linux's netpoll does.
 *
 * `netconsole=` gives one or more targets, separated by `;`, each in Linux's
 * form (Documentation/networking/netconsole.rst):
 *
 *     [+][r][src-port]@[src-ip]/[dev],[tgt-port]@<tgt-ip>/[tgt-macaddr]
 *
 * Each record goes to each target as one UDP datagram from src-ip:src-port
 * to tgt-ip:tgt-port, through the interface `dev` (a name, or a station
 * address with colons), in an Ethernet frame to tgt-macaddr, so that an
 * ordinary user's UDP socket on the target receives it: `nc -lu 6666`. The
 * defaults are Linux's: source port 6665, target port 6666, `eth0`, and the
 * broadcast station address, which needs no ARP. An address may be IPv4 or
 * IPv6, both of a target the same. A target without src-ip sends from the
 * interface's first address, or for IPv6 its first address whose link-local
 * scope matches tgt-ip's, taken at each datagram; while it has none the
 * records wait, where Linux refuses the target. `+` sends Linux's extended
 * form, `level,seq,usec,flags;text`, and `r` puts the release and a comma
 * before it.
 *
 * `rach2.netconsole` sends the same records below IP, in broadcast frames.
 * There is no address to configure, no ARP to answer and no peer to reach, so
 * it works on the first boot on an unknown network, and goes on working when
 * the address configuration is what is wrong. Only root can capture them,
 * since no socket but a packet socket receives an unknown EtherType. Both are
 * diagnostics: a frame or datagram lost on the wire is lost.
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

/* A target's ports when it names none, as Linux's `alloc_and_init` sets
 * them (drivers/net/netconsole.c), and the most log bytes one datagram
 * carries, Linux's MAX_PRINT_CHUNK. */
#define NETCONSOLE_LOCAL_PORT  6665
#define NETCONSOLE_REMOTE_PORT 6666
#define NETCONSOLE_CHUNK_MAX   1000

/**
 * Take one character of a log record, from `klog`'s netconsole destination,
 * which feeds this whether or not the machine has a serial port.
 *
 * The character joins a ring of 32767. Before `netconsole_enable` the
 * characters only wait there. After it, a newline starts `netconsole_flush`,
 * and so do 512 characters waiting unless the last drain was held for the
 * link or a refused frame. A character that comes while a frame is being
 * sent only joins the ring, so a driver that logs an error from inside a
 * send does not send again.
 *
 * The ring is shared by the frames and the targets, each of which keeps its
 * own place in it, and a character leaves it once all have sent it. A target
 * sends only whole lines, or NETCONSOLE_CHUNK_MAX characters of a longer one,
 * so a newline is what starts its drain.
 *
 * When the ring is full, each sender furthest behind that is held loses its
 * oldest record, so that one that cannot send costs the others nothing.
 * Otherwise, before any sender exists or while a drain runs, the character
 * is dropped with the rest of its record, so that the ring holds whole
 * records. A target is told of the records it lost with its next one: a
 * gap in the extended form's `seq`, or in the plain form Linux's line
 * `** N printk messages dropped **` in front of it in its datagram.
 *
 * Any context, interrupt handlers and the panic path included: it allocates
 * nothing, waits for nothing, and takes `net_lock`.
 */
void netconsole_putc(char c);

/**
 * Read `config`, the value of the boot line's `netconsole=`, take each target
 * it gives, start the frames too when `frames` is nonzero, as
 * `netconsole_enable` does, and begin each one's start, as `netconsole_flush`
 * describes it.
 *
 * Called once, at boot, after the cards are up, in place of
 * `netconsole_enable`, which would start the frames after the targets had
 * been sent the ring and it had let go of what they sent; a second call does
 * nothing. A target that does not parse, or whose interface does not exist,
 * is logged and left out, and the others are kept, as Linux does with
 * CONFIG_NETCONSOLE_DYNAMIC. Allocates the targets, so not from an
 * interrupt.
 *
 * @return the number of targets taken.
 */
int netconsole_setup(const char *config, int frames);

/**
 * Read `netpoll.carrier_timeout=` from the boot line, once, early in every
 * boot, as Linux's `parse_args` reads its parameters, logging each value
 * it refuses and each bare key as `netconsole: refused:` in the line's
 * order.
 */
void netconsole_read_params(void);

/**
 * Choose the boot id, and begin the frames' start, as `netconsole_flush`
 * describes it.
 *
 * Called once, at boot, after a card is up, and only when the boot line has
 * `rach2.netconsole` and no `netconsole=`, which starts the frames itself
 * (`netconsole_setup`): broadcasting the kernel log is a decision for the
 * operator of the network, not a default. A second call does nothing.
 */
void netconsole_enable(void);

/**
 * Send the characters the ring holds when it is called: in frames of up to
 * NETCONSOLE_PAYLOAD_MAX through the first interface after
 * `netconsole_enable`, and to each target as datagrams; a character leaves
 * the ring only once each card has taken what carries it.
 *
 * Each sender begins with a start, Linux's wait for carrier at
 * `netpoll_setup`: while its interface is not running (`netdev_running`), for
 * at most `netpoll.carrier_timeout=` seconds, 4 unless the boot line gives
 * another, and, when the start finds the link up with records held, at its
 * first check or later, for two seconds more, the settle, since a card can
 * lose what it sends the moment its link comes up. Every record written
 * before the settle ends waits with the held ones, so the records go out in
 * order. `netconsole_tick` ends the start and sends the held records; a panic
 * and `netconsole_shutdown` end it at once. A flush in which the settle ended
 * a start with records held logs `netconsole: settled:` once the drains it
 * made are done, which a suite waits for to know that they were sent. After
 * the start a sender sends whenever its link is up.
 *
 * Does nothing while a frame or datagram is being sent. While a sender is in
 * its start, while its link is down, while a target's source address is not
 * known, or after a frame the card refuses, the characters stay in the ring
 * for that sender, and after the start its drain is held until the next
 * newline. Takes `net_lock` for each frame and each datagram.
 */
void netconsole_flush(void);

/**
 * Drive the senders' starts from the boot processor's timer tick: every
 * 10 ms while one is in its start, try a drain, which sends the held records
 * once that start ends. Nothing after every start has ended. An interrupt
 * handler; the drain polls the card, as from any record's newline.
 */
void netconsole_tick(void);

/**
 * End every sender's start and send what the ring holds, as a panic does.
 * Called once, by `kernel_shutdown`, after its last record and before it
 * stops the cards, so that the goodbye and what waited for the settle go out.
 */
void netconsole_shutdown(void);

/**
 * Forget a drain that the panic stopped: in a fault inside a send, in a
 * handler that panicked between two of its datagrams, or on a processor the
 * panic stopped. Without this the panic's records would only join the ring.
 * The frame or datagram it was sending is sent again. Called by `k_panic`,
 * as it calls `klog_deliver_directly`.
 */
void netconsole_panic(void);

/** Whether the log is going out on the wire, in frames or to a target. */
int netconsole_is_enabled(void);

/** Log the EtherType, the boot id and the frames sent, each target and the
 * datagrams sent to it, and the characters dropped; nothing while neither
 * form is on. */
void netconsole_print_info(void);

#endif /* _NETCONSOLE_H_ */
