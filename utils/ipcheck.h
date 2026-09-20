/**
 * One judgement of whether an IPv4 configuration is one a machine can hold.
 *
 * Two programs decide this, over configurations that arrive two ways: `netup`
 * reads a static stanza out of `/etc/network/interfaces`, and `dhcpc` is handed
 * a lease by whatever answered on the wire. The rules are the kernel's own, and
 * they are here once, not in each caller, because a machine that
 * half-applies a configuration is left with neither the address it had nor the
 * one it was given, and two copies of a rule are two answers waiting to differ.
 */
#ifndef IPCHECK_H
#define IPCHECK_H

#include <stddef.h>

unsigned long ipcheck_mask(unsigned long address, unsigned long netmask);

int ipcheck_config(unsigned long address, unsigned long netmask, unsigned long gateway, char *err,
                   size_t errlen);

#endif /* IPCHECK_H */
