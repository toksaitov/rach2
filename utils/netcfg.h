/**
 * The reading of `/etc/network/interfaces` and the check of a static stanza,
 * for `netup`, with no socket and no ioctl.
 *
 * It is apart from netup.c, which configures the interface through a socket
 * and needs root, so that `tests/host/netcfg_hosttest.c` can drive it over
 * files written for the purpose, the malformed ones among them. A machine is
 * configured from what this returns, and a file read wrongly is a machine
 * that comes up wrong or not at all.
 *
 * The diagnostics begin `netup:` because that is the program a person ran.
 */
#ifndef NETCFG_H
#define NETCFG_H

#include <stddef.h>

/* An interface name with its terminator, which is what Linux's IFNAMSIZ is.
 * Written out so this file needs no system header and compiles anywhere. */
#define NETCFG_NAME_MAX 16

/* The size of the line buffer, the terminator included. A line that does not
 * fit with its newline fails the read; it is not split. */
#define NETCFG_LINE_MAX 256

/** One interface, as the file describes it. */
struct netcfg_iface {
    char name[NETCFG_NAME_MAX];
    int dhcp; /* the method is dhcp, not static */
    int bad;  /* a line of this stanza could not be used; do not configure it */
    /* The stanza's addresses in host byte order, 0 for one it does not give. */
    unsigned long address;
    unsigned long netmask;
    unsigned long gateway;
    int found; /* the file has an inet stanza for it */
};

/** Whether `name` fits an interface name field: 1 for a name of 1 to
 * `NETCFG_NAME_MAX - 1` bytes, 0 otherwise. */
int netcfg_name_ok(const char *name);

/** Copy the interface name `src` into `dst`, a field of `NETCFG_NAME_MAX`
 * bytes, as an `ifreq` has: cut to fit, terminated and zero-filled. */
void netcfg_set_name(char *dst, const char *src);

/**
 * Read the interfaces file `path` into `want`, which has room for `max`
 * entries.
 *
 * The first `count` entries, 0 <= count <= max, name the interfaces asked
 * for; each must be zero but for a name `netcfg_name_ok` accepts. With
 * `add_auto` set, each interface an `auto` or `allow-hotplug` line names
 * that is not among them is added. An entry with an `inet` stanza gets
 * `found`, its method and the addresses the stanza gives; a method other
 * than `dhcp` or `static`, or an address that is not a dotted quad, sets
 * `bad` and is reported. Stanzas of other families are skipped. The caller
 * checks `found` and `bad` before it configures an interface.
 *
 * The file is read twice through one stream, the names first and the
 * stanzas second, so a stanza may come before or after the line that names
 * its interface.
 *
 * Returns the number of entries, or -1 after a message when the file cannot
 * be opened, read or rewound, a line does not fit in `NETCFG_LINE_MAX`, an
 * `auto`, `allow-hotplug` or `inet` line names an interface
 * `netcfg_name_ok` does not accept, or more than `max` interfaces are named.
 */
int netcfg_read(const char *path, struct netcfg_iface *want, int count, int max, int add_auto);

/**
 * Check the static stanza `s` whole, before any of it is applied: it must
 * give an address and a netmask, and the three addresses must pass
 * `ipcheck_config`.
 *
 * Returns 0 when the stanza can be applied, or -1 with the reason in `err`,
 * as `ipcheck_config` writes one, for the caller to print after its own
 * prefix and a test to tell one reason from another.
 */
int netcfg_check_static(const struct netcfg_iface *s, char *err, size_t errlen);

#endif /* NETCFG_H */
