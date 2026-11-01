/**
 * The stable secret of IPv6's addresses, kept across boots, for init and
 * netup.
 */
#ifndef IP6SECRET_H
#define IP6SECRET_H

/* Where the secret is kept: an IPv6 address's text and a newline, mode
 * 0600, made at the first boot that finds none. */
#define IP6SECRET_KEPT "/var/lib/ipv6-stable-secret"

/**
 * Put the kept secret in force as `net.ipv6.conf.default.stable_secret`,
 * which every interface without a secret of its own makes its SLAAC
 * addresses with (RFC 7217), so that a machine keeps its addresses on a
 * network from one boot to the next, as Linux's do under systemd.
 *
 * A missing or unreadable file is replaced by a new secret of 16 random
 * bytes from getrandom(2), written under a temporary name, synced and
 * renamed into place; when that fails the new secret is used for this boot
 * alone. A secret already in force and the same is left as it is. Writing
 * a new one comes after the cards came up, so a card that already made an
 * address from a router's prefix, a global or site-local one in
 * /proc/net/if_inet6, has IPv6 turned off and on again through its
 * `disable_ipv6`, which makes its addresses again with the secret.
 *
 * Diagnostics go to stderr after `prog`, such as "netup".
 *
 * @return 0 when the secret is in force, -1 after a message when it is not.
 */
int ip6secret_keep(const char *prog);

#endif /* IP6SECRET_H */
