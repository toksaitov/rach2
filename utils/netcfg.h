/**
 * Reading and judging `/etc/network/interfaces`, with no system call in it.
 *
 * `netup` is two things: the reading of a file, which needs nothing, and the
 * configuring of an interface, which needs a socket and root. This is the
 * first half. It is separate because a machine is configured from what this
 * returns, and a file it read wrongly is a machine that comes up wrong or not
 * at all; `tests/host/netcfg_hosttest.c` drives it over files written for the
 * purpose, including the malformed ones nobody would keep in `/etc`.
 *
 * The diagnostics say `netup:` because that is the program a reader ran.
 */
#ifndef NETCFG_H
#define NETCFG_H

#include <stddef.h>

/* An interface name with its terminator, which is what Linux's IFNAMSIZ is.
 * Written out so this file needs no system header and compiles anywhere. */
#define NETCFG_NAME_MAX 16

/* The longest line the file may hold. Longer is refused, not split. */
#define NETCFG_LINE_MAX 256

/** One interface, as the file describes it. */
struct netcfg_iface {
    char name[NETCFG_NAME_MAX];
    int dhcp;  /* the method is dhcp, not static */
    int bad;   /* a line in this stanza was refused; do not configure it */
    unsigned long address;
    unsigned long netmask;
    unsigned long gateway;
    int found; /* the file has a stanza for it at all */
};

int netcfg_name_ok(const char *name);
void netcfg_set_name(char *dst, const char *src);
int netcfg_read(const char *path, struct netcfg_iface *want, int count, int max,
                int add_auto);
int netcfg_check_static(const struct netcfg_iface *s, char *err, size_t errlen);

#endif /* NETCFG_H */
