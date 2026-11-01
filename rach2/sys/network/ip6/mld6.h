#ifndef _MLD6_H_
#define _MLD6_H_

#include "../../lib/kint.h"

/*
 * mld6: multicast listener discovery, the host's part of MLDv2 (RFC 3810)
 * with MLDv1 (RFC 2710) beside it for a link whose querier speaks only
 * that.
 *
 * A card reports every group it is a member of, whoever joined it, but
 * ff02::1 and the groups of scope 0 and 1, which are never reported. The
 * tick compares each card's groups with those it reported: a group joined
 * is reported twice, the second time within a second, as a change to
 * exclude nothing, and a group left the same way as a change to include
 * nothing. A general query is answered with one report of every group
 * after a random part of its maximum response delay, and a query for one
 * group with that group's. While an MLDv1 querier was heard in the last
 * 260 seconds, the reports are MLDv1's and a group left sends one done.
 *
 * Reports leave from the card's link-local address, or from :: before it
 * has one, with a hop limit of 1 and a router alert.
 */

struct netdev;

/**
 * Take the MLD message of `len` bytes at `msg`, whose type is 130, 131,
 * 132 or 143 and whose checksum `icmp6_rx` has checked, from `src` to
 * `dst` with the hop limit `hlim`, received on `nd`. A query from a source
 * that is not link-local or with a hop limit other than 1 is refused and
 * counted as an ICMPv6 input error. Called with interrupts off.
 */
void mld6_rx(struct netdev *nd, const uint8_t *src, const uint8_t *dst, uint8_t hlim,
             const uint8_t *msg, uint16_t len);

/** Send the reports and answers that are due. Called from `nd6_tick`. */
void mld6_tick(void);

#endif /* _MLD6_H_ */
