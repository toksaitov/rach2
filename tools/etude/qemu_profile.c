/*
 * qemu_profile.c: QEMU's command line for a case's QEMU profile, and the
 * reading of QEMU's report that it could not start an accelerator. Both are
 * the same on every host.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "etude/processor.h"
#include "etude/qemu.h"

static const char *const accel_names[ETUDE_ACCEL_COUNT] = {"tcg", "kvm", "hvf", "whpx"};

/* The network cards a profile may name, each on a user network of its own. */
static const char *const nic_models[] = {"rtl8139", "e1000", NULL};

/* The places of the IDE controllers of QEMU's PC, two on each channel. */
#define IDE_PLACES 4

const char *etude_accel_name(enum etude_accel accel)
{
    return (unsigned)accel < ETUDE_ACCEL_COUNT ? accel_names[accel] : NULL;
}

int etude_accel_parse(const char *name, enum etude_accel *accel)
{
    int i;

    for (i = 0; i < ETUDE_ACCEL_COUNT; i++) {
        if (strcmp(name, accel_names[i]) == 0) {
            *accel = (enum etude_accel)i;
            return 0;
        }
    }
    return -1;
}

/* The command line being made: its arguments and the bytes that keep them. */
struct line {
    char **argv;
    size_t n, max;
    char *buf;
    size_t used, size;
    int error;
};

/* How emit writes a string: with its commas doubled, as QEMU's option syntax
 * needs inside a value, and with each single quote written as '\'', as a
 * word that a shell's single quotes enclose needs. */
#define QUOTE_COMMAS 1
#define QUOTE_SHELL  2

/** Write byte `c` at the end of the line's bytes, and keep room for a NUL. */
static void put(struct line *l, char c)
{
    if (l->used + 1 >= l->size) {
        l->error = E2BIG;
        return;
    }
    l->buf[l->used++] = c;
}

/**
 * Write `format` and a NUL at the end of the line's bytes, each `%s`
 * replaced by the next string of `ap`, quoted as the flags `quote` say. A
 * line without room for all of it has the error E2BIG.
 */
static void emit(struct line *l, int quote, const char *format, va_list ap)
{
    const char *f, *s;

    for (f = format; *f != '\0' && !l->error; f++) {
        if (f[0] != '%' || f[1] != 's') {
            put(l, *f);
            continue;
        }
        for (s = va_arg(ap, const char *); *s != '\0'; s++) {
            if (*s == ',' && (quote & QUOTE_COMMAS))
                put(l, ',');
            if (*s == '\'' && (quote & QUOTE_SHELL)) {
                put(l, '\'');
                put(l, '\\');
                put(l, '\'');
            }
            put(l, *s);
        }
        f++;
    }
    if (l->error)
        return;
    if (l->used + 1 >= l->size) {
        l->error = E2BIG;
        return;
    }
    l->buf[l->used++] = '\0';
}

/** Add an argument made from `format`, as emit writes it. */
static void add(struct line *l, int quote, const char *format, ...)
{
    char *start = l->buf + l->used;
    va_list ap;

    if (l->error)
        return;
    if (l->n + 2 > l->max) {
        l->error = E2BIG;
        return;
    }
    va_start(ap, format);
    emit(l, quote, format, ap);
    va_end(ap);
    if (l->error)
        return;
    l->argv[l->n++] = start;
    l->argv[l->n] = NULL;
}

/** Append `format` to the last argument, as emit writes it. */
static void append(struct line *l, int quote, const char *format, ...)
{
    va_list ap;

    if (l->error || l->n == 0)
        return;
    l->used--;
    va_start(ap, format);
    emit(l, quote, format, ap);
    va_end(ap);
}

/** Say whether `s` is a nonempty word of printable ASCII without a space. */
static int word(const char *s)
{
    if (s == NULL || *s == '\0')
        return 0;
    for (; *s != '\0'; s++)
        if (*s <= ' ' || *s > '~')
            return 0;
    return 1;
}

/** Count the strings of a list ended by NULL; a NULL list has none. */
static size_t count(const char *const *list)
{
    size_t n = 0;

    while (list != NULL && list[n] != NULL)
        n++;
    return n;
}

/** Count the disks of a list ended by a NULL path; a NULL list has none. */
static size_t disk_count(const struct etude_qemu_disk *disks)
{
    size_t n = 0;

    while (disks != NULL && disks[n].path != NULL)
        n++;
    return n;
}

/** Say whether `s` is a serial number that QEMU's options take as it is. */
static int serial_word(const char *s)
{
    return word(s) && strlen(s) <= ETUDE_QEMU_SERIAL_MAX && strchr(s, ',') == NULL;
}

int etude_qemu_disks_check(const struct etude_qemu_disk *disks)
{
    const struct etude_qemu_disk *d;
    size_t ide = 0;

    if (disk_count(disks) > ETUDE_QEMU_DISKS)
        return EINVAL;
    for (d = disks; d != NULL && d->path != NULL; d++) {
        if (*d->path == '\0')
            return EINVAL;
        if (d->bus == ETUDE_BUS_IDE) {
            if (++ide > IDE_PLACES || d->serial != NULL || (d->cdrom && d->discard))
                return EINVAL;
        } else if (d->bus != ETUDE_BUS_NVME || d->cdrom || !serial_word(d->serial)) {
            return EINVAL;
        }
    }
    return 0;
}

/** Say whether a device model has a user network. */
static int nic_model(const char *model)
{
    size_t i;

    for (i = 0; nic_models[i] != NULL; i++)
        if (strcmp(model, nic_models[i]) == 0)
            return 1;
    return 0;
}

/**
 * Read four decimal IPv4 octets and leave `end` at the next byte.
 * Return 0, or EINVAL for malformed octets.
 */
static int ipv4(const char *s, uint32_t *value, const char **end)
{
    unsigned i, octet, n;

    *value = 0;
    for (i = 0; i < 4; i++) {
        const char *start = s;

        for (octet = n = 0; *s >= '0' && *s <= '9'; s++, n++) {
            octet = octet * 10 + (unsigned)(*s - '0');
            if (n == 3 || octet > 255)
                return EINVAL;
        }
        if (n == 0 || (n > 1 && *start == '0') || (i < 3 && *s++ != '.'))
            return EINVAL;
        *value = (*value << 8) | octet;
    }
    *end = s;
    return 0;
}

/**
 * Read network `s`, four decimal IPv4 octets, a slash and a decimal prefix of
 * 0 to 32, into `net` and `mask`.
 * Return 0, or EINVAL for a malformed network or one with host bits set.
 */
static int subnet(const char *s, uint32_t *net, uint32_t *mask)
{
    const char *end, *digits;
    unsigned bits = 0;

    if (ipv4(s, net, &end) != 0 || *end++ != '/')
        return EINVAL;
    digits = end;
    while (*end >= '0' && *end <= '9') {
        bits = bits * 10 + (unsigned)(*end++ - '0');
        if (bits > 32)
            return EINVAL;
    }
    if (end == digits || *end != '\0' || (end - digits > 1 && *digits == '0'))
        return EINVAL;
    *mask = bits == 0 ? 0 : UINT32_MAX << (32 - bits);
    return (*net & *mask) == *net ? 0 : EINVAL;
}

/**
 * Check the IPv4 network and the addresses supplied for one card.
 * Return 0, or EINVAL for a malformed network or an address outside it.
 */
static int network_check(const struct etude_qemu_device *d)
{
    const char *end, *addresses[] = {d->host, d->dhcpstart, d->dns};
    uint32_t net = UINT32_C(0x0a000200), mask = UINT32_C(0xffffff00), address;
    size_t i;

    if (d->net != NULL && subnet(d->net, &net, &mask) != 0)
        return EINVAL;
    for (i = 0; i < sizeof addresses / sizeof addresses[0]; i++) {
        if (addresses[i] == NULL)
            continue;
        if (ipv4(addresses[i], &address, &end) != 0 || *end != '\0' || (address & mask) != net ||
            address == net || address == (net | ~mask))
            return EINVAL;
    }
    return 0;
}

/**
 * Read six hexadecimal octets of a nonzero unicast station address.
 * Return 0 with the address in `value`, or EINVAL for an invalid address.
 */
static int station(const char *s, uint64_t *value)
{
    unsigned i, octet, digit, j;

    *value = 0;
    for (i = 0; i < 6; i++) {
        for (octet = j = 0; j < 2; j++) {
            if (*s >= '0' && *s <= '9')
                digit = (unsigned)(*s - '0');
            else if (*s >= 'a' && *s <= 'f')
                digit = (unsigned)(*s - 'a') + 10;
            else if (*s >= 'A' && *s <= 'F')
                digit = (unsigned)(*s - 'A') + 10;
            else
                return EINVAL;
            s++;
            octet = octet * 16 + digit;
        }
        if ((i == 0 && (octet & 1)) || (i < 5 && *s++ != ':'))
            return EINVAL;
        *value = (*value << 8) | octet;
    }
    return *s == '\0' && *value != 0 ? 0 : EINVAL;
}

/** Say whether device `d` gives an option that only a network card can have. */
static int card_options(const struct etude_qemu_device *d)
{
    return d->net != NULL || d->host != NULL || d->dhcpstart != NULL || d->dns != NULL ||
           d->mac != NULL || d->forward_count != 0 || d->restricted || d->guest_forward_count != 0;
}

int etude_qemu_devices_check(const struct etude_qemu_device *devices)
{
    unsigned char default_used[256] = {0};
    const uint64_t default_prefix = UINT64_C(0x525400123400);
    size_t i, j;
    unsigned last;
    uint64_t mac, other;
    const struct etude_qemu_device *d;

    for (i = 0; devices != NULL && devices[i].model != NULL; i++) {
        d = &devices[i];
        if (!word(d->model))
            return EINVAL;
        if (!nic_model(d->model)) {
            if (card_options(d))
                return EINVAL;
            continue;
        }
        if (network_check(d) != 0)
            return EINVAL;
        if (d->mac == NULL) {
            for (last = 0x56; last < 0xff && default_used[last]; last++)
                ;
            if (last == 0xff)
                return EINVAL;
            default_used[last] = 1;
            continue;
        }
        if (station(d->mac, &mac) != 0)
            return EINVAL;
        if ((mac & ~UINT64_C(0xff)) == default_prefix) {
            if (default_used[mac & 0xff])
                return EINVAL;
            default_used[mac & 0xff] = 1;
            continue;
        }
        for (j = 0; j < i; j++) {
            if (nic_model(devices[j].model) && devices[j].mac != NULL) {
                if (station(devices[j].mac, &other) != 0 || mac == other)
                    return EINVAL;
            }
        }
    }
    return 0;
}

/**
 * Give address `s`, one that network_check has taken, or `fallback` when the
 * card does not give it.
 */
static uint32_t address_or(const char *s, uint32_t fallback)
{
    const char *end;
    uint32_t address;

    return s != NULL && ipv4(s, &address, &end) == 0 ? address : fallback;
}

int etude_qemu_guest_address(const struct etude_qemu_device *d, const char *address)
{
    uint32_t net = UINT32_C(0x0a000200), mask = UINT32_C(0xffffff00), a, dhcp;
    const char *end;

    if (address == NULL || ipv4(address, &a, &end) != 0 || *end != '\0' ||
        (d->net != NULL && subnet(d->net, &net, &mask) != 0))
        return 0;
    dhcp = address_or(d->dhcpstart, net | (UINT32_C(0x020f) & ~mask));
    return network_check(d) == 0 && (a & mask) == net && a != net && a != (net | ~mask) &&
           a != address_or(d->host, net | (UINT32_C(0x0202) & ~mask)) &&
           a != address_or(d->dns, net | (UINT32_C(0x0203) & ~mask)) && (a < dhcp || a - dhcp > 15);
}

/** Count the devices of a list ended by a NULL model; NULL has none. */
static size_t device_count(const struct etude_qemu_device *devices)
{
    size_t n = 0;

    while (devices != NULL && devices[n].model != NULL)
        n++;
    return n;
}

/**
 * Give local port `i` of profile `p`, or 0 when the profile does not hold it:
 * there is no such port, or it is not one of 1024 to 65535.
 */
static unsigned held_port(const struct etude_qemu_profile *p, unsigned i)
{
    if (p->ports == NULL || i >= p->port_count || p->ports[i] < 1024 || p->ports[i] > 65535)
        return 0;
    return p->ports[i];
}

/**
 * Say whether a forward of profile `p` that comes before forward `j` of
 * device `i` forwards the local port of `f` by the protocol of `f`.
 */
static int forwarded_before(const struct etude_qemu_profile *p, size_t i, size_t j,
                            const struct etude_qemu_forward *f)
{
    const struct etude_qemu_forward *g;
    size_t k, n;

    for (k = 0; k <= i; k++)
        for (n = 0; n < (k < i ? p->devices[k].forward_count : j); n++) {
            g = &p->devices[k].forwards[n];
            if (!g->udp == !f->udp && held_port(p, g->port) == held_port(p, f->port))
                return 1;
        }
    return 0;
}

/**
 * Say whether a TCP forward of profile `p` forwards local port `port` to the
 * guest, so that QEMU listens there.
 */
static int forwarded_tcp(const struct etude_qemu_profile *p, unsigned port)
{
    const struct etude_qemu_forward *f;
    size_t i, j;

    for (i = 0; i < device_count(p->devices); i++)
        for (j = 0; j < p->devices[i].forward_count; j++) {
            f = &p->devices[i].forwards[j];
            if (!f->udp && held_port(p, f->port) == port)
                return 1;
        }
    return 0;
}

/**
 * Say whether a guest forward of card `d` that comes before guest forward `j`
 * has the address and the guest port of `g`.
 */
static int guest_forwarded_before(const struct etude_qemu_device *d, size_t j,
                                  const struct etude_qemu_guest_forward *g)
{
    size_t k;

    for (k = 0; k < j; k++)
        if (strcmp(d->guest_forwards[k].address, g->address) == 0 &&
            d->guest_forwards[k].guest_port == g->guest_port)
            return 1;
    return 0;
}

/**
 * Say whether `relay` is a program, a nonempty word, with its arguments, each
 * of printable ASCII.
 */
static int relay_words(const char *const *relay)
{
    const char *s;
    size_t i;

    if (relay == NULL || relay[0] == NULL || relay[0][0] == '\0')
        return 0;
    for (i = 0; relay[i] != NULL; i++)
        for (s = relay[i]; *s != '\0'; s++)
            if (*s < ' ' || *s > '~')
                return 0;
    return 1;
}

/**
 * Check the guest forwards of card `d` of profile `p`: each from an address
 * of the card's network that QEMU can forward, to a local port the profile
 * holds and that no TCP forward makes QEMU listen on, through a relay QEMU
 * can run, and none from an address and a port that another has.
 */
static int guest_forward_check(const struct etude_qemu_profile *p,
                               const struct etude_qemu_device *d)
{
    const struct etude_qemu_guest_forward *g;
    size_t j;

    if (d->guest_forward_count > 0 && d->guest_forwards == NULL)
        return EINVAL;
    for (j = 0; j < d->guest_forward_count; j++) {
        g = &d->guest_forwards[j];
        if (!etude_qemu_guest_address(d, g->address) || g->guest_port < 1 ||
            g->guest_port > 65535 || held_port(p, g->port) == 0 ||
            forwarded_tcp(p, held_port(p, g->port)) || !relay_words(g->relay) ||
            guest_forwarded_before(d, j, g))
            return EINVAL;
    }
    return 0;
}

/**
 * Check the forwards of the cards of profile `p`: each from a local port the
 * profile holds to a port of the guest, no local port forwarded twice by one
 * protocol, which QEMU could not listen on twice, and guest forwards that
 * guest_forward_check takes.
 */
static int forward_check(const struct etude_qemu_profile *p)
{
    const struct etude_qemu_device *d;
    const struct etude_qemu_forward *f;
    size_t i, j;

    for (i = 0; i < device_count(p->devices); i++) {
        d = &p->devices[i];
        if (d->forward_count > 0 && d->forwards == NULL)
            return EINVAL;
        for (j = 0; j < d->forward_count; j++) {
            f = &d->forwards[j];
            if (held_port(p, f->port) == 0 || f->guest_port < 1 || f->guest_port > 65535 ||
                forwarded_before(p, i, j, f))
                return EINVAL;
        }
        if (guest_forward_check(p, d) != 0)
            return EINVAL;
    }
    return 0;
}

/** Add a card's network options to `buf`; validated values fit in 256 bytes. */
static void network_options(const struct etude_qemu_device *d, char *buf, size_t size)
{
    snprintf(buf, size, "%s%s%s%s%s%s%s%s", d->net ? ",net=" : "", d->net ? d->net : "",
             d->host ? ",host=" : "", d->host ? d->host : "", d->dhcpstart ? ",dhcpstart=" : "",
             d->dhcpstart ? d->dhcpstart : "", d->dns ? ",dns=" : "", d->dns ? d->dns : "");
}

/**
 * Append guest forward `g` of profile `p` to a `-netdev`: QEMU runs the
 * relay's words, each in a shell's single quotes, and the local port.
 */
static void add_guest_forward(struct line *l, const struct etude_qemu_profile *p,
                              const struct etude_qemu_guest_forward *g)
{
    char guest[16], port[16];
    size_t i;

    snprintf(guest, sizeof(guest), "%u", g->guest_port);
    snprintf(port, sizeof(port), "%u", held_port(p, g->port));
    append(l, 0, ",guestfwd=tcp:%s:%s-cmd:", g->address, guest);
    for (i = 0; g->relay[i] != NULL; i++)
        append(l, QUOTE_COMMAS | QUOTE_SHELL, "'%s' ", g->relay[i]);
    append(l, 0, "%s", port);
}

/**
 * Add the `-netdev` of card `d`, whose id is `id`: a user network with the
 * card's options, its restriction, its forwards of the local ports of profile
 * `p` and its guest forwards.
 */
static void add_network(struct line *l, const struct etude_qemu_profile *p,
                        const struct etude_qemu_device *d, const char *id)
{
    char options[256], port[16], guest[16];
    const struct etude_qemu_forward *f;
    size_t i;

    network_options(d, options, sizeof(options));
    add(l, 0, "-netdev");
    add(l, 0, "user,id=%s%s", id, options);
    if (d->restricted)
        append(l, 0, ",restrict=on");
    for (i = 0; i < d->forward_count; i++) {
        f = &d->forwards[i];
        snprintf(port, sizeof(port), "%u", held_port(p, f->port));
        snprintf(guest, sizeof(guest), "%u", f->guest_port);
        append(l, 0, ",hostfwd=%s:127.0.0.1:%s-:%s", f->udp ? "udp" : "tcp", port, guest);
    }
    for (i = 0; i < d->guest_forward_count; i++)
        add_guest_forward(l, p, &d->guest_forwards[i]);
}

/**
 * Attach device `d` of profile `p`; `*nics` counts the cards already on user
 * networks.
 */
static void add_device(struct line *l, const struct etude_qemu_profile *p,
                       const struct etude_qemu_device *d, unsigned *nics)
{
    char id[16];

    if (nic_model(d->model)) {
        snprintf(id, sizeof(id), "net%u", (*nics)++);
        add_network(l, p, d, id);
        add(l, 0, "-device");
        add(l, 0, "%s,netdev=%s%s%s", d->model, id, d->mac ? ",mac=" : "", d->mac ? d->mac : "");
        return;
    }
    if (strcmp(d->model, "vga") == 0) {
        add(l, 0, "-vga");
        add(l, 0, "std");
        return;
    }
    add(l, 0, "-device");
    add(l, 0, "%s", d->model);
}

/** Add the selected audio card and its WAV backend at the launcher's path. */
static void add_audio(struct line *l, const char *card, const char *path)
{
    if (card != NULL) {
        add(l, 0, "-audiodev");
        add(l, 1, "wav,id=etude-audio,path=%s", path);
        if (strcmp(card, "hda") == 0) {
            add(l, 0, "-device");
            add(l, 0, "intel-hda,id=etude-hda");
            add(l, 0, "-device");
            add(l, 0, "hda-duplex,bus=etude-hda.0,audiodev=etude-audio");
        } else {
            add(l, 0, "-device");
            add(l, 0, "sb16,audiodev=etude-audio");
        }
    }
}

/**
 * Add the arguments that attach disk `d`, the `n`th of the profile counted
 * from 0; `*ide` counts the places of the IDE controllers taken before it.
 */
static void add_disk(struct line *l, const struct etude_qemu_disk *d, size_t n, unsigned *ide)
{
    const char *discard = d->discard ? ",discard=unmap,detect-zeroes=unmap" : "";
    char format[128], id[32];

    if (d->bus == ETUDE_BUS_NVME) {
        snprintf(id, sizeof(id), "etude-disk%u", (unsigned)n);
        snprintf(format, sizeof(format), "file=%%s,format=raw,if=none,id=%s%s", id, discard);
        add(l, 0, "-drive");
        add(l, 1, format, d->path);
        add(l, 0, "-device");
        add(l, 0, "nvme,drive=%s,serial=%s", id, d->serial);
        return;
    }
    snprintf(format, sizeof(format), "file=%%s,format=raw,if=ide,index=%u,media=%s%s", (*ide)++,
             d->cdrom ? "cdrom,readonly=on" : "disk", discard);
    add(l, 0, "-drive");
    add(l, 1, format, d->path);
}

/** Add one argument of the strings of `list` separated by spaces. */
static void add_joined(struct line *l, const char *const *list)
{
    char *start = l->buf + l->used;
    size_t i, len;

    if (l->error)
        return;
    if (l->n + 2 > l->max) {
        l->error = E2BIG;
        return;
    }
    for (i = 0; list[i] != NULL; i++) {
        len = strlen(list[i]);
        if (l->used + len + 2 > l->size) {
            l->error = E2BIG;
            return;
        }
        if (i > 0)
            l->buf[l->used++] = ' ';
        memcpy(l->buf + l->used, list[i], len);
        l->used += len;
    }
    l->buf[l->used++] = '\0';
    l->argv[l->n++] = start;
    l->argv[l->n] = NULL;
}

int etude_qemu_memory(const struct etude_qemu_profile *p, struct etude_qemu_memory *out)
{
    struct etude_qemu_memory m = {p->memory_bytes, p->memory_bytes, ETUDE_QEMU_OVERHEAD, 0};

    if (p->memory_bytes == 0 || p->memory_bytes % (1u << 20) != 0)
        return EINVAL;
    if (p->memory_profile != NULL && strcmp(p->memory_profile, "reserved") != 0) {
        if (strcmp(p->memory_profile, "bigmem-boundary-v1") != 0 || p->architecture == NULL ||
            strcmp(p->architecture, "x86_64") != 0 || p->cpu == NULL ||
            strcmp(p->cpu, "max-nopdpe1gb") != 0 || p->processors != 1 ||
            p->memory_bytes != ETUDE_BIGMEM_CONFIGURED || p->firmware != ETUDE_FIRMWARE_DIRECT ||
            p->initrd == NULL || p->audio != NULL || disk_count(p->disks) != 0 ||
            device_count(p->devices) != 0 || count(p->boot_options) != 1 ||
            strcmp(p->boot_options[0], "rach2.rescue") != 0)
            return EINVAL;
        m.resident_bytes = ETUDE_BIGMEM_RESIDENT;
        m.overhead_bytes = ETUDE_BIGMEM_OVERHEAD;
    }
    if (m.resident_bytes > UINT64_MAX - m.overhead_bytes)
        return EOVERFLOW;
    *out = m;
    return 0;
}

/** Check what QEMU's command line cannot express or would ignore. */
static int profile_check(const struct etude_qemu_profile *p)
{
    size_t i;
    struct etude_qemu_memory memory;

    if (etude_qemu_memory(p, &memory) != 0)
        return EINVAL;
    if (p->architecture == NULL ||
        (strcmp(p->architecture, "i686") != 0 && strcmp(p->architecture, "x86_64") != 0))
        return EINVAL;
    if (etude_processor_find(p->cpu) == NULL || p->processors < 1 || p->processors > 64)
        return EINVAL;
    if (p->memory_bytes == 0 || p->memory_bytes % (1u << 20) != 0)
        return EINVAL;
    if (etude_qemu_disks_check(p->disks) != 0)
        return EINVAL;
    if (etude_qemu_devices_check(p->devices) != 0)
        return EINVAL;
    for (i = 0; i < count(p->boot_options); i++)
        if (!word(p->boot_options[i]))
            return EINVAL;
    if (forward_check(p) != 0)
        return EINVAL;
    if (p->firmware == ETUDE_FIRMWARE_DIRECT)
        return p->kernel != NULL && *p->kernel != '\0' ? 0 : EINVAL;
    if (p->firmware != ETUDE_FIRMWARE_SEABIOS && p->firmware != ETUDE_FIRMWARE_OVMF)
        return EINVAL;
    return p->initrd == NULL && count(p->boot_options) == 0 && disk_count(p->disks) > 0 ? 0
                                                                                        : EINVAL;
}

/** Check the selected audio card and its launcher-made WAV path. */
static int audio_check(const char *card, const char *path)
{
    if (card == NULL)
        return 0;
    if (strcmp(card, "hda") != 0 && strcmp(card, "sb16") != 0)
        return EINVAL;
    return path != NULL && *path != '\0' ? 0 : EINVAL;
}

int etude_qemu_argv(const struct etude_qemu_profile *p, const struct etude_qemu_site *s,
                    const char *console, const char *qmp, const char *audio,
                    const struct etude_qemu_line *out)
{
    int x86_64 = p->architecture != NULL && strcmp(p->architecture, "x86_64") == 0;
    const char *program = x86_64 ? s->qemu_x86_64 : s->qemu_i686;
    const char *ovmf = x86_64 ? s->ovmf_x86_64 : s->ovmf_i686;
    struct line l = {out->argv, 0, out->max, out->buf, 0, out->size, 0};
    char number[32], cpu[256];
    unsigned nics = 0, ide = 0;
    size_t i;
    int e;

    if ((e = profile_check(p)) != 0)
        return e;
    if ((e = audio_check(p->audio, audio)) != 0)
        return e;
    if (p->memory_profile != NULL && strcmp(p->memory_profile, "bigmem-boundary-v1") == 0 &&
        !s->boundary_lazy)
        return ENOTSUP;
    if (etude_accel_name(s->accel) == NULL || program == NULL)
        return EINVAL;
    if (p->firmware == ETUDE_FIRMWARE_OVMF && ovmf == NULL)
        return ENOENT;
    if (out->max < 1 || out->size < 1 ||
        etude_processor_cpu(etude_processor_find(p->cpu), cpu, sizeof cpu) != 0)
        return E2BIG;
    out->argv[0] = NULL;
    add(&l, 0, "%s", program);
    add(&l, 0, "-accel");
    add(&l, 0, "%s", etude_accel_name(s->accel));
    add(&l, 0, "-nodefaults");
    add(&l, 0, "-display");
    add(&l, 0, "none");
    add(&l, 0, "-no-reboot");
    add(&l, 0, "-S");
    add(&l, 0, "-cpu");
    add(&l, 0, "%s", cpu);
    snprintf(number, sizeof(number), "%u", p->processors);
    add(&l, 0, "-smp");
    add(&l, 0, "%s", number);
    snprintf(number, sizeof(number), "%lluB", (unsigned long long)p->memory_bytes);
    add(&l, 0, "-m");
    add(&l, 0, "%s", number);
    if (p->memory_profile != NULL && strcmp(p->memory_profile, "bigmem-boundary-v1") == 0) {
        add(&l, 0, "-object");
#if defined(__APPLE__)
        add(&l, 0, "memory-backend-ram,id=etude-ram,size=%s,prealloc=off,merge=off", number);
#else
        add(&l, 0, "memory-backend-ram,id=etude-ram,size=%s,prealloc=off,merge=off,dump=off",
            number);
#endif
        add(&l, 0, "-machine");
        add(&l, 0, "memory-backend=etude-ram");
        add(&l, 0, "-overcommit");
        add(&l, 0, "mem-lock=off");
    }
    if (p->firmware == ETUDE_FIRMWARE_DIRECT) {
        add(&l, 0, "-kernel");
        add(&l, 0, "%s", p->kernel);
        if (p->initrd != NULL) {
            add(&l, 0, "-initrd");
            add(&l, 0, "%s", p->initrd);
        }
    } else if (p->firmware == ETUDE_FIRMWARE_OVMF) {
        add(&l, 0, "-drive");
        add(&l, 1, "if=pflash,format=raw,readonly=on,file=%s", ovmf);
    }
    for (i = 0; i < disk_count(p->disks); i++)
        add_disk(&l, &p->disks[i], i, &ide);
    add_audio(&l, p->audio, audio);
    for (i = 0; i < device_count(p->devices); i++)
        add_device(&l, p, &p->devices[i], &nics);
    add(&l, 0, "-chardev");
    add(&l, 1, "socket,id=etude-console,path=%s", console);
    add(&l, 0, "-serial");
    add(&l, 0, "chardev:etude-console");
    add(&l, 0, "-chardev");
    add(&l, 1, "socket,id=etude-qmp,path=%s", qmp);
    add(&l, 0, "-mon");
    add(&l, 0, "chardev=etude-qmp,mode=control");
    if (count(p->boot_options) > 0) {
        add(&l, 0, "-append");
        add_joined(&l, p->boot_options);
    }
    return l.error;
}

int etude_qemu_accel_refused(const char *err, enum etude_accel accel)
{
    char needle[32];
    const char *line, *at, *end;

    if (err == NULL || etude_accel_name(accel) == NULL)
        return 0;
    snprintf(needle, sizeof(needle), ": -accel %s: ", etude_accel_name(accel));
    for (line = err; *line != '\0'; line = *end == '\0' ? end : end + 1) {
        end = strchr(line, '\n');
        if (end == NULL)
            end = line + strlen(line);
        at = strstr(line, needle);
        if (at != NULL && at < end && at > line)
            return 1;
    }
    return 0;
}
