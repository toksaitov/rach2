/**
 * The check of an IPv4 configuration that `netup` makes of a static stanza of
 * `/etc/network/interfaces` and `dhcpc` makes of a lease, before either one
 * changes the interface.
 *
 * The rules are the kernel's, checked here first because a configuration the
 * kernel fails part way leaves the machine with neither the address it had
 * nor the one it was given. Both programs use this one copy of the rules.
 */
#ifndef IPCHECK_H
#define IPCHECK_H

#include <stddef.h>

/**
 * The netmask that goes with `address`: `netmask`, or, when it is 0, the
 * classful mask of the address's first octet (/8 below 128, /16 below 192,
 * /24 above), which is the one the kernel takes for an address set with no
 * mask.
 */
unsigned long ipcheck_mask(unsigned long address, unsigned long netmask);

/**
 * Check `address`, `netmask` and `gateway`, in host byte order, as one
 * configuration.
 *
 * The address must be nonzero, must not have a first octet of 0, 127, or 224
 * and above, and must not be its subnet's network or broadcast address. The
 * netmask must be a run of ones from the top bit and shorter than 32 bits; a
 * zero netmask is checked as `ipcheck_mask` gives it, and the caller must
 * then apply that mask with the address. A nonzero gateway must pass the same
 * first-octet test and be a host of the address's subnet other than the
 * address; a zero gateway is not checked.
 *
 * Returns 0 when the configuration can be applied, or -1 with the reason in
 * `err`, cut to `errlen - 1` bytes and terminated, as a phrase the caller
 * prints after its own prefix. `err` may be NULL.
 */
int ipcheck_config(unsigned long address, unsigned long netmask, unsigned long gateway, char *err,
                   size_t errlen);

#endif /* IPCHECK_H */
