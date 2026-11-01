/**
 * Reading and judging `/etc/network/interfaces`. See netcfg.h.
 */

#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <stdlib.h>
#include <arpa/inet.h>

#include "ipcheck.h"
#include "netcfg.h"

/** Copy the reason `what` into `err`, cut to `errlen - 1` bytes and
 * terminated; nothing when `err` is NULL or `errlen` is 0. */
static void say(char *err, size_t errlen, const char *what)
{
    if (err && errlen > 0) {
        strncpy(err, what, errlen - 1);
        err[errlen - 1] = '\0';
    }
}

/** The dotted quad `s` in host byte order, or 0 when `s` is not one; 0.0.0.0
 * also gives 0, which is no usable address. */
static unsigned long parse_ip(const char *s)
{
    unsigned long v = 0;
    int i;

    for (i = 0; i < 4; i++) {
        unsigned long part = 0;
        int digits = 0;

        while (*s >= '0' && *s <= '9') {
            part = part * 10 + (unsigned long)(*s - '0');
            if (part > 255) {
                return 0;
            }
            s++;
            digits++;
        }
        if (digits == 0) {
            return 0;
        }
        v = (v << 8) | part;
        if (i < 3) {
            if (*s != '.') {
                return 0;
            }
            s++;
        }
    }
    return *s == '\0' ? v : 0;
}

/** The first word of `line`, terminated in place, and in `*rest` where the
 *  rest of the line begins. */
static char *word(char *line, char **rest)
{
    char *p = line;
    char *start;

    while (*p == ' ' || *p == '\t') {
        p++;
    }
    start = p;
    while (*p && *p != ' ' && *p != '\t' && *p != '\n') {
        p++;
    }
    if (*p) {
        *p++ = '\0';
    }
    *rest = p;
    return start;
}

/**
 * A name longer than the field would be stored cut and compared whole, so its
 * stanza would never be found; cutting both sides instead would make two long
 * names that begin alike the same interface. No interface can have such a
 * name, since the field is the kernel's, so a name that does not fit is a
 * mistake in the file.
 */
int netcfg_name_ok(const char *name)
{
    size_t n = strlen(name);

    return n > 0 && n < NETCFG_NAME_MAX;
}

void netcfg_set_name(char *dst, const char *src)
{
    size_t n = strnlen(src, NETCFG_NAME_MAX - 1);

    memset(dst, 0, NETCFG_NAME_MAX);
    memcpy(dst, src, n);
}

/**
 * Check a completed pass and seek back for the second pass. Returns 0, or -1
 * after reporting a read or seek failure and closing `f`.
 */
static int finish_pass(FILE *f, const char *path, int pass)
{
    const char *operation;

    if (ferror(f)) {
        operation = "read";
    } else if (pass == 0 && fseek(f, 0, SEEK_SET) != 0) {
        operation = "seek";
    } else {
        return 0;
    }
    fprintf(stderr, "netup: cannot %s %s: %s\n", operation, path, strerror(errno));
    fclose(f);
    return -1;
}

/**
 * Take the line `key value` of the inet6 stanza of `s`: an address, as
 * `ADDR` or `ADDR/LEN`, a prefix's length as `netmask`, or a gateway.
 * Other keys are passed over. A value that cannot be used sets `bad6` and
 * is reported.
 */
static void read6(const char *path, struct netcfg_iface *s, const char *key, char *value)
{
    char *slash = strchr(value, '/');
    char *end;
    long len = -1;

    if (strcmp(key, "address") != 0 && strcmp(key, "netmask") != 0 && strcmp(key, "gateway") != 0) {
        return;
    }
    if (strcmp(key, "netmask") == 0 || (strcmp(key, "address") == 0 && slash)) {
        const char *digits = slash && strcmp(key, "address") == 0 ? slash + 1 : value;

        len = strtol(digits, &end, 10);
        if (*digits < '0' || *digits > '9' || *end != '\0' || len > 128) {
            fprintf(stderr, "netup: %s: %s: '%s' is not a prefix's length\n", path, s->name,
                    digits);
            s->bad6 = 1;
            return;
        }
        s->prefix6 = (int)len;
        if (strcmp(key, "netmask") == 0) {
            return;
        }
        *slash = '\0';
    }
    if (inet_pton(AF_INET6, value, strcmp(key, "address") == 0 ? s->address6 : s->gateway6) != 1) {
        fprintf(stderr, "netup: %s: %s: '%s' is not an IPv6 address\n", path, s->name, value);
        s->bad6 = 1;
    }
}

int netcfg_read(const char *path, struct netcfg_iface *want, int count, int max, int add_auto)
{
    FILE *f = fopen(path, "r");
    char line[NETCFG_LINE_MAX];
    char keep[NETCFG_LINE_MAX];
    struct netcfg_iface *current = NULL;
    int inet6 = 0;
    int pass;

    if (!f) {
        fprintf(stderr, "netup: cannot read %s: %s\n", path, strerror(errno));
        return -1;
    }

    for (pass = 0; pass < 2; pass++) {
        while (fgets(line, sizeof(line), f)) {
            char *rest;
            char *key;
            size_t n = strlen(line);

            if (n == sizeof(line) - 1 && line[n - 1] != '\n') {
                fprintf(stderr, "netup: %s: a line longer than %d characters\n", path,
                        (int)sizeof(line) - 1);
                fclose(f);
                return -1;
            }
            memcpy(keep, line, n + 1);
            key = word(keep, &rest);
            if (key[0] == '#' || key[0] == '\0') {
                continue;
            }

            if (pass == 0) {
                if (strcmp(key, "auto") == 0 || strcmp(key, "allow-hotplug") == 0) {
                    char *name;

                    while (*(name = word(rest, &rest)) != '\0') {
                        int i;
                        int seen = 0;

                        for (i = 0; i < count; i++) {
                            if (strcmp(want[i].name, name) == 0) {
                                seen = 1;
                            }
                        }
                        if (!netcfg_name_ok(name)) {
                            fprintf(stderr,
                                    "netup: %s: '%s' is not a name an "
                                    "interface can have\n",
                                    path, name);
                            fclose(f);
                            return -1;
                        }
                        if (seen || !add_auto) {
                            continue;
                        }
                        if (count == max) {
                            fprintf(stderr,
                                    "netup: %s: more interfaces than this can "
                                    "bring up\n",
                                    path);
                            fclose(f);
                            return -1;
                        }
                        memset(&want[count], 0, sizeof(want[count]));
                        netcfg_set_name(want[count].name, name);
                        count++;
                    }
                }
                continue;
            }

            if (strcmp(key, "auto") == 0 || strcmp(key, "allow-hotplug") == 0) {
                continue;
            }
            if (strcmp(key, "iface") == 0) {
                char *name = word(rest, &rest);
                char *family = word(rest, &rest);
                char *method = word(rest, &rest);
                int i;

                current = NULL;
                inet6 = strcmp(family, "inet6") == 0;
                if (strcmp(family, "inet") != 0 && !inet6) {
                    continue;
                }
                if (!netcfg_name_ok(name)) {
                    fprintf(stderr,
                            "netup: %s: '%s' is not a name an interface can "
                            "have\n",
                            path, name);
                    fclose(f);
                    return -1;
                }
                for (i = 0; i < count; i++) {
                    if (strcmp(want[i].name, name) == 0) {
                        current = &want[i];
                    }
                }
                if (!current) {
                    continue;
                }
                if (inet6) {
                    current->prefix6 = -1;
                    if (strcmp(method, "auto") == 0) {
                        current->method6 = NETCFG6_AUTO;
                    } else if (strcmp(method, "static") == 0) {
                        current->method6 = NETCFG6_STATIC;
                    } else if (strcmp(method, "manual") == 0 || strcmp(method, "loopback") == 0) {
                        current->method6 = NETCFG6_MANUAL;
                    } else {
                        fprintf(stderr,
                                "netup: %s: %s: inet6 method '%s' is not one this "
                                "understands\n",
                                path, name, method);
                        current->method6 = NETCFG6_MANUAL;
                        current->bad6 = 1;
                    }
                    continue;
                }
                current->found = 1;
                if (strcmp(method, "dhcp") == 0) {
                    current->dhcp = 1;
                } else if (strcmp(method, "static") == 0) {
                    current->dhcp = 0;
                } else {
                    fprintf(stderr,
                            "netup: %s: %s: method '%s' is not one this "
                            "understands\n",
                            path, name, method);
                    current->bad = 1;
                }
                continue;
            }
            if (!current) {
                continue;
            }
            if (inet6) {
                read6(path, current, key, word(rest, &rest));
                continue;
            }
            if (strcmp(key, "address") == 0 || strcmp(key, "netmask") == 0 ||
                strcmp(key, "gateway") == 0) {
                char *value = word(rest, &rest);
                unsigned long ip = parse_ip(value);

                if (ip == 0) {
                    fprintf(stderr, "netup: %s: %s: '%s' is not an address\n", path, current->name,
                            value);
                    current->bad = 1;
                    continue;
                }
                if (strcmp(key, "address") == 0) {
                    current->address = ip;
                } else if (strcmp(key, "netmask") == 0) {
                    current->netmask = ip;
                } else {
                    current->gateway = ip;
                }
            }
        }
        if (finish_pass(f, path, pass) != 0) {
            return -1;
        }
    }
    fclose(f);
    return count;
}

/**
 * Changing the address ends every connection this machine has, so a stanza
 * that would fail at its last line must fail before its first, or the way in
 * is cut off for nothing. What the file must give is decided here; whether
 * the addresses can be applied is decided by `ipcheck_config`, which the DHCP
 * client applies to its leases too.
 *
 * A stanza must give a netmask, as netup(8) says, and none is taken from the
 * address's class.
 */
int netcfg_check_static(const struct netcfg_iface *s, char *err, size_t errlen)
{
    if (s->address == 0) {
        say(err, errlen, "a static stanza with no address");
        return -1;
    }
    if (s->netmask == 0) {
        say(err, errlen, "a static stanza with no netmask");
        return -1;
    }
    return ipcheck_config(s->address, s->netmask, s->gateway, err, errlen);
}

/** Whether the 16 bytes at `a` are all `v` but the last, which is `last`. */
static int all_but_last(const unsigned char *a, unsigned char v, unsigned char last)
{
    int i;

    for (i = 0; i < 15 && a[i] == v; i++) {
    }
    return i == 15 && a[15] == last;
}

/** Whether `a` is ::, ::1, a group, ff00::/8, or an IPv4-mapped address,
 * ::ffff:0:0/96: no address an interface's own or a gateway may be. */
static int not_unicast(const unsigned char *a)
{
    static const unsigned char mapped[12] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0xff, 0xff};

    return all_but_last(a, 0, 0) || all_but_last(a, 0, 1) || a[0] == 0xff ||
           memcmp(a, mapped, sizeof(mapped)) == 0;
}

int netcfg_check_static6(const struct netcfg_iface *s, char *err, size_t errlen)
{
    if (all_but_last(s->address6, 0, 0)) {
        say(err, errlen, "a static inet6 stanza with no address");
        return -1;
    }
    if (s->prefix6 < 0) {
        say(err, errlen, "a static inet6 stanza with no prefix length");
        return -1;
    }
    if (not_unicast(s->address6)) {
        say(err, errlen, "an inet6 address no interface can hold");
        return -1;
    }
    if (!all_but_last(s->gateway6, 0, 0) && not_unicast(s->gateway6)) {
        say(err, errlen, "an inet6 gateway that is no router's address");
        return -1;
    }
    return 0;
}
