#ifndef _FRAG6_H_
#define _FRAG6_H_

#include "../../lib/kint.h"

/*
 * frag6: IPv6 fragments (RFC 8200, 4.5), and the calls the files of the
 * IPv6 layer make of each other to build, send and walk a datagram. No
 * other layer includes this header.
 *
 * A fragment received is kept in its datagram's reassembly queue, keyed by
 * its source, destination, identification and interface. The datagram is
 * given back to `ip6_walk` once every byte of it has come; it is given up
 * when two fragments overlap (RFC 5722), when its fragments disagree about
 * its end, past `FRAG6_PIECES` fragments, and `FRAG6_TIMEOUT_SECONDS` after
 * its first fragment came. A queue given up for an overlap stays, holding no
 * memory, until that time is out, so that the fragments still to come are
 * dropped too. At most `FRAG6_QUEUES` datagrams are reassembled at once,
 * in at most `FRAG6_MEM_MAX` bytes; a new one takes the place of the one
 * that would time out first.
 */

#define FRAG6_QUEUES          16
#define FRAG6_PIECES          64
#define FRAG6_MEM_MAX         (256u * 1024u)
/* RFC 8200, 4.5: the seconds a reassembly may take. */
#define FRAG6_TIMEOUT_SECONDS 60u

struct netdev;

/**
 * Process the datagram of `len` bytes at `dg`, received on `nd` (NULL for
 * the loopback backlog), from offset `off`, where a header of next-header
 * value `nh` starts; `nhoff` is the offset of the byte that names it, which
 * a parameter problem about `nh` points at. `reassembled` is nonzero for a
 * datagram `frag6_input` made, in which a fragment header is refused. The
 * caller has checked the fixed header and that the datagram is this
 * machine's. Called with interrupts off.
 */
void ip6_walk(struct netdev *nd, const uint8_t *dg, uint32_t len, uint32_t off, uint8_t nh,
              uint32_t nhoff, int reassembled);

/** Write the fixed header of a datagram of `plen` bytes of payload and
 * next header `nh` from `src` to `dst` with the hop limit `hlim` into the
 * first 40 bytes of `buf`, with traffic class 0 and flow label 0. */
void ip6_header(uint8_t *buf, const uint8_t *src, const uint8_t *dst, uint8_t nh, uint16_t plen,
                uint8_t hlim);

/** Set the traffic class of the fixed header at `buf` to `tclass`. */
void ip6_set_class(uint8_t *buf, uint8_t tclass);

/**
 * Hand the whole datagram of `len` bytes at `buf` to `out`, toward the next
 * hop `via`: onto the loopback backlog for `lo`, to the station address of a
 * group, or to a unicast hop by the neighbour cache (`nd6_output`). Returns
 * 0, or -EMSGSIZE or -ENOBUFS as `ip6_tx` says. Called with interrupts off.
 */
int ip6_emit(struct netdev *out, const uint8_t *via, const uint8_t *buf, uint32_t len);

/**
 * Take the fragment whose fragment header is at offset `off` of the
 * datagram of `len` bytes at `dg`, received on `nd`; `nhoff` is the offset
 * of the byte that names the fragment header.
 *
 * As RFC 8200, 4.5 says, a fragment other than the last whose length is not
 * a multiple of 8 is refused with a parameter problem pointing at the
 * payload length, and one that would make the datagram's payload longer
 * than 65535 bytes with one pointing at its fragment offset. A first
 * fragment that does not hold every header up to and including the upper
 * layer's is refused with a parameter problem of code 3 (RFC 7112). An
 * exact duplicate of a fragment held is dropped alone. Counts in
 * `net_mib6`. Called with interrupts off, from `ip6_walk`.
 */
void frag6_input(struct netdev *nd, const uint8_t *dg, uint32_t len, uint32_t off, uint32_t nhoff);

/**
 * Send the `plen` bytes at `payload`, of next header `nh`, from `src` to
 * `dst` with the hop limit `hlim` and traffic class `tclass`, in fragments
 * of at most `mtu` bytes by
 * `out` toward `via`. Every fragment but the last carries a multiple of 8
 * bytes of payload; all carry one identification, drawn at random, as RFC
 * 7739 advises. Returns 0, or the error of `ip6_emit` for the first
 * fragment that could not leave, after which no more are sent. Called with
 * interrupts off, from `ip6_tx`.
 */
int frag6_output(struct netdev *out, const uint8_t *via, const uint8_t *src, const uint8_t *dst,
                 uint8_t nh, const uint8_t *payload, uint16_t plen, uint8_t hlim, uint8_t tclass,
                 uint32_t mtu);

/**
 * Give up the reassemblies whose time is out, each counted as a timeout and
 * a failure. One whose first fragment came is answered with a time exceeded
 * of code 1, quoting that fragment, as RFC 8200, 4.5 says. Called once a
 * tick, with interrupts off, from `ip6_tick`.
 */
void frag6_tick(void);

#endif /* _FRAG6_H_ */
