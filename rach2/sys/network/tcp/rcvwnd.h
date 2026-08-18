#ifndef _RCVWND_H_
#define _RCVWND_H_

#include "../../lib/kint.h"

/**
 * Decide whether a reopened receive window is worth announcing.
 *
 * A receiver that drains its buffer has more room than it last told the peer
 * about, but saying so after every byte would spend a segment per byte and
 * invite the peer to answer with segments just as small. RFC 1122 4.2.3.3 sets
 * the rule used here: stay quiet until the window has grown by at least one
 * maximum segment size or half the buffer, whichever is smaller, then announce
 * the whole new window at once.
 *
 * A window wider than 65535 is reported as 65535, because an unscaled TCP
 * window is a 16-bit field and this stack negotiates no window scale.
 *
 * @param advertised the window last sent to the peer.
 * @param buffered   bytes the reader has not taken yet, `tail - head`.
 * @param ring       the receive buffer's size.
 * @param mss        the maximum segment size.
 * @return the window to advertise, or 0 to send nothing. Zero is never a
 *         window worth announcing, since it is never larger than what was
 *         already advertised, so it is free to mean "stay quiet".
 */
uint16_t rcvwnd_update(uint16_t advertised, uint32_t buffered, uint32_t ring, uint16_t mss);

#endif /* _RCVWND_H_ */
