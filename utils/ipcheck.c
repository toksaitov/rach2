/**
 * Judging an IPv4 configuration. See ipcheck.h.
 */

#include <string.h>

#include "ipcheck.h"

/** Copy the reason `what` into `err`, cut to `errlen - 1` bytes and
 * terminated; nothing when `err` is NULL or `errlen` is 0. */
static void say(char *err, size_t errlen, const char *what)
{
    if (err && errlen > 0) {
        strncpy(err, what, errlen - 1);
        err[errlen - 1] = '\0';
    }
}

/**
 * Whether `ip` is an address no machine can have: a first octet of 0 is "this
 * network", 127 is the loopback, and 224 and above are multicast and
 * reserved. The kernel's test is the same (`netconf_address_is_impossible`),
 * and `SIOCSIFADDR` fails with EINVAL for such an address.
 */
static int impossible(unsigned long ip)
{
    unsigned long first = (ip >> 24) & 0xff;

    return first == 0 || first == 127 || first >= 224;
}

unsigned long ipcheck_mask(unsigned long ip, unsigned long netmask)
{
    unsigned long first = (ip >> 24) & 0xff;

    if (netmask != 0) {
        return netmask;
    }
    if (first < 128) {
        return 0xFF000000UL;
    }
    if (first < 192) {
        return 0xFFFF0000UL;
    }
    return 0xFFFFFF00UL;
}

/**
 * The host part of a mask that is a run of ones from the top bit is a run of
 * ones from bit 0, which plus one is a power of two. A /32 mask passes that
 * test and leaves no host address, so it is tested apart.
 */
int ipcheck_config(unsigned long address, unsigned long netmask, unsigned long gateway, char *err,
                   size_t errlen)
{
    unsigned long mask = ipcheck_mask(address, netmask);
    unsigned long inverted;

    if (address == 0) {
        say(err, errlen, "there is no address");
        return -1;
    }
    if (impossible(address)) {
        say(err, errlen, "the address is not one a machine can hold");
        return -1;
    }
    inverted = ~mask & 0xFFFFFFFFUL;
    if ((inverted & (inverted + 1)) != 0 || mask == 0xFFFFFFFFUL) {
        say(err, errlen, "the netmask is not a run of ones");
        return -1;
    }
    if (address == (address & mask) || address == (address | inverted)) {
        say(err, errlen,
            "the address is this subnet's network or broadcast address, "
            "not a host");
        return -1;
    }
    if (gateway != 0) {
        if (impossible(gateway)) {
            say(err, errlen, "the gateway is not an address a machine can hold");
            return -1;
        }
        if ((gateway & mask) != (address & mask) || gateway == address ||
            gateway == (gateway & mask) || gateway == (gateway | inverted)) {
            say(err, errlen, "the gateway is not a host on this subnet");
            return -1;
        }
    }
    return 0;
}
