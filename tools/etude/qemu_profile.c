/*
 * qemu_profile.c: QEMU's command line for a case's QEMU profile, and the
 * reading of QEMU's report that it could not start an accelerator. Both are
 * the same on every host.
 */
#include <ctype.h>
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#ifdef _WIN32
#include <direct.h>
#endif

#include "etude/processor.h"
#include "etude/qemu.h"

static const char *const accel_names[ETUDE_ACCEL_COUNT] = {"tcg", "kvm", "hvf", "whpx"};

/* HMP reads QEMU's halt state, so WHPX must deliver halt exits to QEMU. */
static const char *accel_option(enum etude_accel accel)
{
    return accel == ETUDE_ACCEL_WHPX ? "whpx,kernel-irqchip=off" : etude_accel_name(accel);
}

/**
 * The processors QEMU gives profile `p` under `accel`: the profile's, but one
 * under TCG for a profile with AMD's virtualization. On a thread for each
 * processor, TCG lets an x87 status load on any processor rewrite processor
 * 0's hflags2 by a plain read and write, which can undo the nested-paging
 * flag of processor 0's VMRUN; on one thread for all of them it delivers
 * interrupts into a nested guest that has them masked
 * (dev-log/upstream.md). One processor has neither.
 */
static unsigned machine_processors(const struct etude_qemu_profile *p, enum etude_accel accel)
{
    const struct etude_processor *cpu = etude_processor_find(p->cpu);

    if (accel == ETUDE_ACCEL_TCG && cpu != NULL &&
        etude_processor_feature(cpu, "svm") == ETUDE_FEATURE_PRESENT)
        return 1;
    return p->processors;
}

/* The network cards a profile may name, each on a user network of its own. */
static const char *const nic_models[] = {"rtl8139", "e1000", "virtio-net-pci", NULL};

/* The places of the IDE controllers of QEMU's PC, two on each channel, and
 * the ports of an ich9-ahci. */
#define IDE_PLACES 4
#define AHCI_PORTS 6

/* The longest identity of a device or disk. */
#define ID_MAX 32

/* The members of a device that the launcher sets, which a property cannot. */
static const char *const set_members[] = {"id", "bus", "port", "drive", "netdev", "mac", NULL};

/* The environment that puts QEMU's GL on the testing PC's GPU, not llvmpipe. */
static const char *const virgl_env[] = {
    "GALLIUM_DRIVER=d3d12",
    "MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA",
    NULL,
};

/* The environment of Venus over the host's lavapipe, with QEMU's GL on
 * llvmpipe, since Venus's frames are black through d3d12. llvmpipe's
 * compiled shaders in virglrenderer's render server, which inherits it, look
 * libm's functions up in the process's global scope, where libm is only if
 * preloaded; without it they call address 0. */
static const char *const venus_env[] = {
    "GALLIUM_DRIVER=llvmpipe",
    "VK_DRIVER_FILES=/usr/share/vulkan/icd.d/lvp_icd.json",
    "LD_PRELOAD=/usr/lib/x86_64-linux-gnu/libm.so.6",
    NULL,
};

/* Where Mesa keeps the shaders that QEMU's GL and Venus's render server
 * compile under a GL display, below the tree that ETUDE_TREE names, so that
 * they stay between runs and out of the home directory, where Mesa would put
 * them. */
#define MESA_HOST_CACHE "/build/cache/mesa-host"

/* A display profile: the device it attaches and, for one whose frames the
 * host's GL draws through QEMU's egl-headless display, QEMU's environment. */
struct display {
    const char *name;
    const char *device;     /* QEMU's model, or NULL for no display device */
    const char *properties; /* the device's own, after the window's */
    const char *const *env; /* NULL, or the GL display's environment */
    int shared;             /* the guest's RAM is a shared memfd, for blobs */
};

static const struct display displays[] = {
    {"none", NULL, "", NULL, 0},
    {"std-vga", "VGA", "", NULL, 0},
    {"virtio-vga", "virtio-vga", "", NULL, 0},
    {"virtio-gpu", "virtio-gpu-pci", "", NULL, 0},
    {"virgl", "virtio-vga-gl", "", virgl_env, 0},
    {"venus", "virtio-vga-gl", ",venus=on,blob=on,hostmem=4G", venus_env, 1},
    {NULL, NULL, NULL, NULL, 0},
};

/* The properties of every display device: an EDID whose preferred mode is
 * the window's fixed size, 1280x800. */
#define DISPLAY_WINDOW "edid=on,xres=1280,yres=800"

/* The host's render node, which egl-headless opens; on the testing PC it is
 * WSL's vgem, and Mesa's d3d12 driver draws on the GPU behind /dev/dxg. */
#define DISPLAY_RENDER_NODE "/dev/dri/renderD128"

/* The device models that a display profile replaces, so that a profile with
 * one has no other display, and the USB tablet, since its pointer is the PS/2
 * mouse. */
static const char *const display_models[] = {
    "vga",
    "VGA",
    "virtio-vga",
    "virtio-vga-gl",
    "virtio-gpu",
    "virtio-gpu-pci",
    "virtio-gpu-gl",
    "virtio-gpu-gl-pci",
    "bochs-display",
    "cirrus-vga",
    "qxl",
    "qxl-vga",
    "vmware-svga",
    "ramfb",
    "secondary-vga",
    "usb-tablet",
    NULL,
};

/** Find the display profile called `name`, or NULL. */
static const struct display *display_find(const char *name)
{
    const struct display *d;

    for (d = displays; name != NULL && d->name != NULL; d++)
        if (strcmp(d->name, name) == 0)
            return d;
    return NULL;
}

/** Give the environment of profile `p`'s GL display, or NULL for none. */
static const char *const *display_env(const struct etude_qemu_profile *p)
{
    const struct display *d = display_find(p->display);

    return d != NULL ? d->env : NULL;
}

int etude_qemu_display_device(const char *model)
{
    size_t i;

    for (i = 0; model != NULL && display_models[i] != NULL; i++)
        if (strcmp(display_models[i], model) == 0)
            return 1;
    return 0;
}

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

/** Say whether `s` is a word without a `,` or `=`, as a value of QEMU's. */
static int plain(const char *s)
{
    return word(s) && strpbrk(s, ",=") == NULL;
}

/**
 * Say whether `s` is an identity QEMU takes: a letter, then letters, digits,
 * `-` and `_`, at most ID_MAX in all. A `.` is left out, since it ends the
 * identity in a bus's name.
 */
static int qemu_id(const char *s)
{
    size_t n;

    if (s == NULL || !((*s >= 'A' && *s <= 'Z') || (*s >= 'a' && *s <= 'z')))
        return 0;
    n = strspn(s, "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_");
    return n <= ID_MAX && s[n] == '\0';
}

/** Say whether `s` is a USB port: numbers from 1, separated by `.`. */
static int usb_port(const char *s)
{
    if (s == NULL)
        return 0;
    for (;;) {
        if (*s < '1' || *s > '9')
            return 0;
        s += strspn(s, "0123456789");
        if (*s == '\0')
            return 1;
        if (*s++ != '.')
            return 0;
    }
}

/** Say whether `name` is in `list`, ended by NULL. */
static int listed(const char *const *list, const char *name)
{
    for (; *list != NULL; list++)
        if (strcmp(*list, name) == 0)
            return 1;
    return 0;
}

/** Say whether `s` is a nonempty text of printable ASCII, spaces allowed, of
 * at most `max` bytes. */
static int text(const char *s, size_t max)
{
    size_t n;

    for (n = 0; s[n] != '\0'; n++)
        if (n == max || s[n] < ' ' || s[n] > '~')
            return 0;
    return n > 0;
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

/** Say whether a disk of `disks` boots first; QEMU boots `-kernel` first. */
static int boot_disk(const struct etude_qemu_disk *disks)
{
    size_t i;

    for (i = 0; i < disk_count(disks); i++)
        if (disks[i].boot)
            return 1;
    return 0;
}

/** Say whether `s` is a serial number that QEMU's options take as it is. */
static int serial_word(const char *s)
{
    return word(s) && strlen(s) <= ETUDE_QEMU_SERIAL_MAX && strchr(s, ',') == NULL;
}

/**
 * Say whether disk `d` of `disks`, on ETUDE_BUS_DEVICE, is one the launcher
 * attaches: an identity of QEMU's that no earlier disk has, on a disk that is
 * no CD, has no serial and does not boot first, since its device orders it.
 */
static int device_disk_ok(const struct etude_qemu_disk *disks, const struct etude_qemu_disk *d)
{
    const struct etude_qemu_disk *e;

    if (!qemu_id(d->id) || d->cdrom || d->serial != NULL || d->boot)
        return 0;
    for (e = disks; e != d; e++)
        if (e->id != NULL && strcmp(e->id, d->id) == 0)
            return 0;
    return 1;
}

/**
 * Check disk `d`, on IDE, AHCI, NVMe or virtio, against its controller,
 * `*ide` and `*ahci` counting the disks on IDE and on AHCI before it: a place
 * free; a CD on IDE alone, of a raw medium, without a serial or `discard`;
 * and a serial on IDE, NVMe or virtio alone, which NVMe needs.
 *
 * @return 0, or EINVAL.
 */
static int bus_disk_check(const struct etude_qemu_disk *d, size_t *ide, size_t *ahci)
{
    if (d->bus == ETUDE_BUS_IDE) {
        if (++*ide > IDE_PLACES || (d->cdrom && (d->serial != NULL || d->qcow2)) ||
            (d->cdrom && d->discard))
            return EINVAL;
    } else if (d->bus == ETUDE_BUS_AHCI) {
        if (++*ahci > AHCI_PORTS || d->serial != NULL || d->cdrom)
            return EINVAL;
    } else if (d->bus == ETUDE_BUS_VIRTIO) {
        if (d->cdrom)
            return EINVAL;
    } else if (d->bus != ETUDE_BUS_NVME || d->cdrom || d->serial == NULL) {
        return EINVAL;
    }
    return 0;
}

int etude_qemu_disks_check(const struct etude_qemu_disk *disks)
{
    const struct etude_qemu_disk *d;
    size_t ide = 0, ahci = 0, boot = 0;

    if (disk_count(disks) > ETUDE_QEMU_DISKS)
        return EINVAL;
    for (d = disks; d != NULL && d->path != NULL; d++) {
        if (*d->path == '\0' || (d->id != NULL) != (d->bus == ETUDE_BUS_DEVICE))
            return EINVAL;
        if (d->boot && ++boot > 1)
            return EINVAL;
        if (d->bus == ETUDE_BUS_DEVICE ? !device_disk_ok(disks, d)
                                       : bus_disk_check(d, &ide, &ahci) != 0)
            return EINVAL;
        if (d->serial != NULL && !serial_word(d->serial))
            return EINVAL;
    }
    return 0;
}

int etude_qemu_nic(const char *model)
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
    return d->hub || d->tap_queues != 0 || d->net != NULL || d->host != NULL ||
           d->dhcpstart != NULL || d->dns != NULL || d->mac != NULL || d->forward_count != 0 ||
           d->restricted || d->guest_forward_count != 0;
}

/** Say whether card `d` gives an option that only a user network takes. */
static int user_options(const struct etude_qemu_device *d)
{
    return d->net != NULL || d->host != NULL || d->dhcpstart != NULL || d->dns != NULL ||
           d->forward_count != 0 || d->guest_forward_count != 0 || d->restricted;
}

/**
 * Check the backend of card `i` of `devices`: a card on a hub or a tap has
 * no option of a user network, a tap is on no hub and has at most
 * ETUDE_TAP_QUEUES_MAX queues, and no card before it has a tap. 0 or EINVAL.
 */
static int backend_check(const struct etude_qemu_device *devices, size_t i)
{
    const struct etude_qemu_device *d = &devices[i];
    size_t j;

    if ((d->hub || d->tap_queues != 0) && user_options(d))
        return EINVAL;
    if (d->tap_queues == 0)
        return 0;
    if (d->hub || d->tap_queues > ETUDE_TAP_QUEUES_MAX)
        return EINVAL;
    for (j = 0; j < i; j++)
        if (devices[j].tap_queues != 0)
            return EINVAL;
    return 0;
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
        if (!etude_qemu_nic(d->model)) {
            if (card_options(d))
                return EINVAL;
            continue;
        }
        if (backend_check(devices, i) != 0 || network_check(d) != 0)
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
            if (etude_qemu_nic(devices[j].model) && devices[j].mac != NULL) {
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

    if (d->hub || address == NULL || ipv4(address, &a, &end) != 0 || *end != '\0' ||
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

/** Say whether device `d` has no member but its model. */
static int bare(const struct etude_qemu_device *d)
{
    return d->id == NULL && d->bus == NULL && d->port == NULL && d->drive == NULL &&
           count(d->properties) == 0;
}

/** Say whether a device of `p` before device `at` has the identity of the
 * `len` bytes of `id`. */
static int earlier(const struct etude_qemu_device *p, size_t at, const char *id, size_t len)
{
    size_t i;

    for (i = 0; i < at; i++)
        if (p[i].id != NULL && strlen(p[i].id) == len && strncmp(p[i].id, id, len) == 0)
            return 1;
    return 0;
}

/** Say whether `bus` is a numbered bus or the bus of an earlier root port. */
static int bus_ok(const struct etude_qemu_device *p, size_t at, const char *bus)
{
    const char *dot = strrchr(bus, '.');
    size_t i;

    for (i = 0; i < at; i++)
        if (strcmp(p[i].model, "pcie-root-port") == 0 && p[i].id != NULL &&
            strcmp(p[i].id, bus) == 0)
            return 1;

    return dot != NULL && dot[1] != '\0' && dot[1 + strspn(dot + 1, "0123456789")] == '\0' &&
           earlier(p, at, bus, (size_t)(dot - bus));
}

/** Say whether `drive` is a disk of `p` on ETUDE_BUS_DEVICE that no device
 * before device `at` attaches. */
static int drive_ok(const struct etude_qemu_profile *p, size_t at, const char *drive)
{
    size_t i;

    for (i = 0; i < at; i++)
        if (p->devices[i].drive != NULL && strcmp(p->devices[i].drive, drive) == 0)
            return 0;
    for (i = 0; i < disk_count(p->disks); i++)
        if (p->disks[i].id != NULL && strcmp(p->disks[i].id, drive) == 0)
            return 1;
    return 0;
}

/** Say whether the properties of `d` are pairs of names and values that are
 * neither the members the launcher sets nor need QEMU's quoting. */
static int properties_ok(const struct etude_qemu_device *d)
{
    size_t n = count(d->properties), i;

    if (n % 2 != 0)
        return 0;
    for (i = 0; i < n; i += 2)
        if (!plain(d->properties[i]) || !plain(d->properties[i + 1]) ||
            listed(set_members, d->properties[i]))
            return 0;
    return 1;
}

/**
 * Check the devices of `p` beyond etude_qemu_devices_check: a model QEMU
 * takes as it is, `vga` alone, identities given once, a bus that an earlier
 * device provides, a USB port, a disk on a device that one device alone
 * attaches, each such disk attached, and properties as properties_ok says.
 */
static int attached_check(const struct etude_qemu_profile *p)
{
    const struct etude_qemu_device *d = p->devices;
    size_t i, j;

    for (i = 0; i < device_count(d); i++) {
        if (!plain(d[i].model) || (strcmp(d[i].model, "vga") == 0 && !bare(&d[i])))
            return EINVAL;
        if (d[i].id != NULL && (!qemu_id(d[i].id) || earlier(d, i, d[i].id, strlen(d[i].id))))
            return EINVAL;
        if ((d[i].bus != NULL && !bus_ok(d, i, d[i].bus)) ||
            (d[i].port != NULL && !usb_port(d[i].port)) ||
            (d[i].drive != NULL && !drive_ok(p, i, d[i].drive)) || !properties_ok(&d[i]))
            return EINVAL;
    }
    for (i = 0; i < disk_count(p->disks); i++) {
        if (p->disks[i].id == NULL)
            continue;
        for (j = 0; j < device_count(d); j++)
            if (d[j].drive != NULL && strcmp(d[j].drive, p->disks[i].id) == 0)
                break;
        if (j == device_count(d))
            return EINVAL;
    }
    return 0;
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
 * Attach device `d` of profile `p`; `*nics` counts the cards before it. A
 * card's network is its user network, or with `hub` a port of hub `*nics`,
 * which no other card shares, and it is recorded when `p` has a capture of it.
 */
static void add_device(struct line *l, const struct etude_qemu_profile *p,
                       const struct etude_qemu_device *d, unsigned *nics)
{
    const char *const members[] = {"id", d->id, "bus", d->bus, "port", d->port, "drive", d->drive};
    const struct etude_qemu_capture *c;
    char id[16], hub[16];
    size_t i;

    if (strcmp(d->model, "vga") == 0) {
        add(l, 0, "-vga");
        add(l, 0, "std");
        return;
    }
    if (etude_qemu_nic(d->model)) {
        snprintf(id, sizeof(id), "net%u", *nics);
        if (d->hub) {
            snprintf(hub, sizeof(hub), "%u", *nics);
            add(l, 0, "-netdev");
            add(l, 0, "hubport,id=%s,hubid=%s", id, hub);
        } else if (d->tap_queues != 0) {
            snprintf(hub, sizeof(hub), "%u", d->tap_queues);
            add(l, 0, "-netdev");
            add(l, 0,
                "tap,id=%s,ifname=" ETUDE_TAP_NAME ",queues=%s,script=no,downscript=no,vhost=off",
                id, hub);
        } else {
            add_network(l, p, d, id);
        }
        add(l, 0, "-device");
        add(l, 0, "%s,netdev=%s", d->model, id);
        if (d->mac != NULL)
            append(l, 0, ",mac=%s", d->mac);
    } else {
        add(l, 0, "-device");
        add(l, 0, "%s", d->model);
    }
    for (i = 0; i < sizeof(members) / sizeof(members[0]); i += 2)
        if (members[i + 1] != NULL)
            append(l, 0, ",%s=%s", members[i], members[i + 1]);
    for (i = 0; i < count(d->properties); i += 2)
        append(l, 0, ",%s=%s", d->properties[i], d->properties[i + 1]);
    if (etude_qemu_nic(d->model)) {
        for (c = p->captures; c != NULL && c->path != NULL; c++) {
            if (c->card == *nics) {
                add(l, 0, "-object");
                add(l, 1, "filter-dump,id=%s-dump,netdev=%s,file=%s", id, id, c->path);
            }
        }
        (*nics)++;
    }
}

/** Give the profile's audio backend, "wav" when it names none. */
static const char *audio_backend(const struct etude_qemu_profile *p)
{
    return p->audio_backend != NULL ? p->audio_backend : "wav";
}

/**
 * Add the selected audio card on the profile's backend: `wav` records the
 * output to the launcher's file at `path`, `none` discards it, and `sdl`
 * opens SDL's input at the format of the profile's raw input file, so that
 * QEMU passes the file's samples to the card unconverted. HDA is the
 * controller and a codec on its bus: under `wav`, which has no input, the
 * output-only codec, since QEMU on Windows ends when a codec's ADC finds no
 * input; under the others the duplex codec, with an ADC and an input pin.
 */
static void add_audio(struct line *l, const struct etude_qemu_profile *p, const char *path)
{
    const char *backend = audio_backend(p);

    if (p->audio == NULL)
        return;
    add(l, 0, "-audiodev");
    if (strcmp(backend, "wav") == 0)
        add(l, QUOTE_COMMAS, "wav,id=etude-audio,path=%s", path);
    else if (strcmp(backend, "sdl") == 0)
        add(l, 0, "sdl,id=etude-audio,in.frequency=48000,in.channels=2,in.format=s16");
    else
        add(l, 0, "none,id=etude-audio");
    if (strcmp(p->audio, "hda") == 0) {
        add(l, 0, "-device");
        add(l, 0, "intel-hda,id=etude-hda");
        add(l, 0, "-device");
        add(l, 0, "%s,bus=etude-hda.0,audiodev=etude-audio",
            strcmp(backend, "wav") == 0 ? "hda-output" : "hda-duplex");
    } else {
        add(l, 0, "-device");
        add(l, 0, "sb16,audiodev=etude-audio");
    }
}

/**
 * Add the arguments that attach disk `d`, the `n`th of the profile counted
 * from 0; `*ide` counts the places of the IDE controllers taken before it,
 * and `*ahci` the ports of the ich9-ahci. A disk on IDE that boots first or
 * has a serial is a device of its own, since only a device takes a boot index
 * or a serial. On the q35 machine, `q35` true, the places on IDE are the ports
 * of its own AHCI, the buses `ide.0` to `ide.5` of one unit each, where the
 * PC's two IDE channels have two units each.
 */
static void add_disk(struct line *l, const struct etude_qemu_disk *d, size_t n, int q35,
                     unsigned *ide, unsigned *ahci)
{
    const char *discard = d->discard ? ",discard=unmap,detect-zeroes=unmap" : "";
    const char *flush = d->unsafe ? ",cache=unsafe" : "";
    const char *type = d->qcow2 ? "qcow2" : "raw";
    const char *boot = d->boot ? ",bootindex=0" : "";
    char format[192], id[32], device[128];

    if (d->bus == ETUDE_BUS_DEVICE) {
        snprintf(format, sizeof(format), "file=%%s,format=%s,if=none,id=%s%s%s", type, d->id,
                 discard, flush);
        add(l, 0, "-drive");
        add(l, 1, format, d->path);
        return;
    }
    if (d->bus == ETUDE_BUS_IDE && !d->boot && d->serial == NULL) {
        snprintf(format, sizeof(format), "file=%%s,format=%s,if=ide,index=%u,media=%s%s%s", type,
                 (*ide)++, d->cdrom ? "cdrom,readonly=on" : "disk", discard, flush);
        add(l, 0, "-drive");
        add(l, 1, format, d->path);
        return;
    }
    snprintf(id, sizeof(id), "etude-disk%u", (unsigned)n);
    snprintf(format, sizeof(format), "file=%%s,format=%s,if=none,id=%s%s%s%s", type, id,
             d->cdrom ? ",media=cdrom,readonly=on" : "", discard, flush);
    add(l, 0, "-drive");
    add(l, 1, format, d->path);
    add(l, 0, "-device");
    if (d->bus == ETUDE_BUS_NVME) {
        add(l, 0, "nvme,drive=%s,serial=%s%s", id, d->serial, boot);
        return;
    }
    if (d->bus == ETUDE_BUS_VIRTIO) {
        add(l, 0, "virtio-blk-pci,drive=%s%s%s%s", id, d->serial != NULL ? ",serial=" : "",
            d->serial != NULL ? d->serial : "", boot);
        return;
    }
    if (d->bus == ETUDE_BUS_AHCI) {
        snprintf(device, sizeof(device), "ide-hd,drive=%s,bus=etude-ahci.%u", id, (*ahci)++);
    } else {
        snprintf(device, sizeof(device), "%s,drive=%s,bus=ide.%u,unit=%u%s%s",
                 d->cdrom ? "ide-cd" : "ide-hd", id, q35 ? *ide : *ide / 2, q35 ? 0u : *ide % 2,
                 d->serial != NULL ? ",serial=" : "", d->serial != NULL ? d->serial : "");
        (*ide)++;
    }
    add(l, 0, "%s%s", device, boot);
}

/** Add the ich9-ahci of the disks of profile `p`, when one is on AHCI. */
static void add_ahci(struct line *l, const struct etude_qemu_profile *p)
{
    size_t i;

    for (i = 0; i < disk_count(p->disks); i++) {
        if (p->disks[i].bus == ETUDE_BUS_AHCI) {
            add(l, 0, "-device");
            add(l, 0, "ich9-ahci,id=etude-ahci");
            return;
        }
    }
}

/** Add one argument of the strings of `list`, which may be NULL, and then
 * of `last`, when it is not NULL, separated by spaces. */
static void add_joined(struct line *l, const char *const *list, const char *last)
{
    char *start = l->buf + l->used;
    size_t i, n = count(list), len;

    if (l->error)
        return;
    if (l->n + 2 > l->max) {
        l->error = E2BIG;
        return;
    }
    for (i = 0; i < n + (last != NULL); i++) {
        const char *piece = i < n ? list[i] : last;

        len = strlen(piece);
        if (l->used + len + 2 > l->size) {
            l->error = E2BIG;
            return;
        }
        if (i > 0)
            l->buf[l->used++] = ' ';
        memcpy(l->buf + l->used, piece, len);
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
            p->initrd == NULL || p->audio != NULL || p->display != NULL ||
            disk_count(p->disks) != 0 || device_count(p->devices) != 0 || p->no_i8042 || p->q35 ||
            count(p->boot_options) != 1 || strcmp(p->boot_options[0], "rach2.rescue") != 0)
            return EINVAL;
        m.resident_bytes = ETUDE_BIGMEM_RESIDENT;
        m.overhead_bytes = ETUDE_BIGMEM_OVERHEAD;
    }
    if (m.resident_bytes > UINT64_MAX - m.overhead_bytes)
        return EOVERFLOW;
    *out = m;
    return 0;
}

/** Check that each capture of `p` has a file and is of a card of its own. */
static int captures_check(const struct etude_qemu_profile *p)
{
    const struct etude_qemu_capture *c, *d;
    size_t i;
    unsigned nics = 0;

    for (i = 0; i < device_count(p->devices); i++)
        nics += (unsigned)etude_qemu_nic(p->devices[i].model);
    for (c = p->captures; c != NULL && c->path != NULL; c++) {
        if (*c->path == '\0' || c->card >= nics)
            return EINVAL;
        for (d = p->captures; d != c; d++)
            if (d->card == c->card)
                return EINVAL;
    }
    return 0;
}

/**
 * Check the firmware of profile `p` against what it boots: the direct
 * firmware's kernel, with no disk that boots first, or a disk that SeaBIOS or
 * OVMF boots, with no RAM image or boot options.
 *
 * @return 0, or EINVAL.
 */
static int firmware_check(const struct etude_qemu_profile *p)
{
    if (p->firmware == ETUDE_FIRMWARE_DIRECT)
        return p->kernel != NULL && *p->kernel != '\0' && !boot_disk(p->disks) ? 0 : EINVAL;
    if (p->firmware != ETUDE_FIRMWARE_SEABIOS && p->firmware != ETUDE_FIRMWARE_OVMF)
        return EINVAL;
    return p->initrd == NULL && count(p->boot_options) == 0 && disk_count(p->disks) > 0 ? 0
                                                                                        : EINVAL;
}

/**
 * Check the display of profile `p`: a known profile, and with one no other
 * display device, no USB tablet and an 8042 for the PS/2 mouse.
 *
 * @return 0, or EINVAL.
 */
static int display_check(const struct etude_qemu_profile *p)
{
    size_t i;

    if (p->display == NULL)
        return 0;
    if (display_find(p->display) == NULL || p->no_i8042)
        return EINVAL;
    for (i = 0; i < device_count(p->devices); i++)
        if (etude_qemu_display_device(p->devices[i].model))
            return EINVAL;
    return 0;
}

/** The boot option the launcher adds for a virtio console: port 1, where
 * QEMU puts the first `virtserialport`, port 0 being a `virtconsole`'s. */
#define CONSOLE_OPTION "console=vport0p1"

/** The PCI slot of the virtio console, above those QEMU gives the devices
 * of a profile in turn and below the q35 machine's own at 0x1f. */
#define CONSOLE_SLOT "0x1d"

/** Say whether one of `options` names a console, as `console=ttyS0`. */
static int names_console(const char *const *options)
{
    size_t i;

    for (i = 0; i < count(options); i++)
        if (strncmp(options[i], "console=", 8) == 0)
            return 1;
    return 0;
}

enum etude_console etude_qemu_console_default(const char *kernel, enum etude_firmware firmware,
                                              const char *const *boot_options)
{
    return firmware == ETUDE_FIRMWARE_DIRECT && kernel != NULL && strcmp(kernel, "kernel") == 0 &&
                   !names_console(boot_options)
               ? ETUDE_CONSOLE_VIRTIO
               : ETUDE_CONSOLE_UART;
}

/**
 * Check the console of profile `p`: the UART, or a virtio console on a
 * machine whose kernel the launcher gives the boot line, and whose line
 * names no other console.
 *
 * @return 0, or EINVAL.
 */
static int console_check(const struct etude_qemu_profile *p)
{
    if (p->console == ETUDE_CONSOLE_UART)
        return 0;
    return p->console == ETUDE_CONSOLE_VIRTIO && p->firmware == ETUDE_FIRMWARE_DIRECT &&
                   !names_console(p->boot_options)
               ? 0
               : EINVAL;
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
    if (etude_qemu_devices_check(p->devices) != 0 || attached_check(p) != 0)
        return EINVAL;
    for (i = 0; i < count(p->boot_options); i++)
        if (!word(p->boot_options[i]))
            return EINVAL;
    if (forward_check(p) != 0 || captures_check(p) != 0)
        return EINVAL;
    if (p->smbios_vendor != NULL && !text(p->smbios_vendor, ETUDE_SMBIOS_VENDOR_MAX))
        return EINVAL;
    if (display_check(p) != 0 || console_check(p) != 0)
        return EINVAL;
    return firmware_check(p);
}

/**
 * Check the profile's audio: a known card, a known backend, the launcher's
 * WAV path `path` under `wav`, and an input file under `sdl` and no other.
 * A profile without a card names no backend and no input.
 */
static int audio_check(const struct etude_qemu_profile *p, const char *path)
{
    const char *backend = audio_backend(p);
    int sdl = strcmp(backend, "sdl") == 0;

    if (p->audio == NULL)
        return p->audio_backend == NULL && p->audio_input == NULL ? 0 : EINVAL;
    if (strcmp(p->audio, "hda") != 0 && strcmp(p->audio, "sb16") != 0)
        return EINVAL;
    if (!sdl && strcmp(backend, "wav") != 0 && strcmp(backend, "none") != 0)
        return EINVAL;
    if (sdl != (p->audio_input != NULL) || (sdl && *p->audio_input == '\0'))
        return EINVAL;
    if (strcmp(backend, "wav") == 0 && (path == NULL || *path == '\0'))
        return EINVAL;
    return 0;
}

/** Return whether the profile needs x86-64 QEMU and firmware. */
static int machine_x86_64(const struct etude_qemu_profile *p)
{
    return p->firmware == ETUDE_FIRMWARE_OVMF ||
           (p->architecture != NULL && strcmp(p->architecture, "x86_64") == 0);
}

/** Say whether profile `p` has the memory profile of the boundary machine. */
static int boundary(const struct etude_qemu_profile *p)
{
    return p->memory_profile != NULL && strcmp(p->memory_profile, "bigmem-boundary-v1") == 0;
}

/**
 * Add the lazily allocated RAM of the boundary machine, of `size`, QEMU's
 * size with its unit.
 */
static void add_boundary_memory(struct line *l, const char *size)
{
    add(l, 0, "-object");
#if defined(__APPLE__) || defined(_WIN32)
    add(l, 0, "memory-backend-ram,id=etude-ram,size=%s,prealloc=off,merge=off", size);
#else
    add(l, 0, "memory-backend-ram,id=etude-ram,size=%s,prealloc=off,merge=off,dump=off", size);
#endif
    add(l, 0, "-machine");
    add(l, 0, "memory-backend=etude-ram");
    add(l, 0, "-overcommit");
    add(l, 0, "mem-lock=off");
}

/**
 * Back the guest's RAM, of `size`, QEMU's size with its unit, with a shared
 * memfd, which Venus's blobs map into the host's render server.
 */
static void add_shared_memory(struct line *l, const char *size)
{
    add(l, 0, "-object");
    add(l, 0, "memory-backend-memfd,id=etude-ram,size=%s,share=on", size);
    add(l, 0, "-machine");
    add(l, 0, "memory-backend=etude-ram");
}

/**
 * Give the path of an ACPI table as -acpitable's file= takes it, or NULL for
 * one it cannot take. QEMU splits that value at each colon, so a Windows path
 * goes without its drive letter, which must name the drive of the working
 * directory that QEMU inherits.
 */
static const char *acpi_table_path(const char *path)
{
#ifdef _WIN32
    char cwd[4096];

    if (isalpha((unsigned char)path[0]) && path[1] == ':' && (path[2] == '/' || path[2] == '\\')) {
        if (_getcwd(cwd, sizeof(cwd)) == NULL || cwd[1] != ':' ||
            toupper((unsigned char)cwd[0]) != toupper((unsigned char)path[0]))
            return NULL;
        path += 2;
    }
#endif
    return strchr(path, ':') == NULL ? path : NULL;
}

/**
 * Give a display profile under KVM the host's processor: `cpu`, the processor
 * profile's model and features, becomes QEMU's `host` model with the same
 * features and an invariant time-stamp counter, which Rach2's vDSO clock
 * reads. QEMU's named models ask for features that nested KVM may lack, and
 * a graphics run is measured against a Linux guest on the same processor.
 *
 * @return 0, or E2BIG when `size` bytes do not hold the result.
 */
static int display_cpu(const struct etude_qemu_profile *p, const struct etude_qemu_site *s,
                       char *cpu, size_t size)
{
    const char *features = strchr(cpu, ',');
    char host[256];
    int n;

    if (p->display == NULL || s->accel != ETUDE_ACCEL_KVM)
        return 0;
    n = snprintf(host, sizeof(host), "host%s,invtsc=on", features != NULL ? features : "");
    if (n < 0 || (size_t)n >= sizeof(host) || (size_t)n >= size)
        return E2BIG;
    memcpy(cpu, host, (size_t)n + 1);
    return 0;
}

/**
 * Check profile `p` against site `s`, the audio file `audio` and the size of
 * `out`, and write the -cpu value into `cpu`, of `size` bytes.
 *
 * @return 0, or the error etude_qemu_argv returns.
 */
static int argv_check(const struct etude_qemu_profile *p, const struct etude_qemu_site *s,
                      const char *audio, const struct etude_qemu_line *out, char *cpu, size_t size)
{
    int x86_64 = machine_x86_64(p);
    int e;

    if ((e = profile_check(p)) != 0)
        return e;
    if ((e = audio_check(p, audio)) != 0)
        return e;
    if (boundary(p) && !s->boundary_lazy)
        return ENOTSUP;
    if (etude_accel_name(s->accel) == NULL || (x86_64 ? s->qemu_x86_64 : s->qemu_i686) == NULL)
        return EINVAL;
    if (p->firmware == ETUDE_FIRMWARE_OVMF && (x86_64 ? s->ovmf_x86_64 : s->ovmf_i686) == NULL)
        return ENOENT;
#ifdef _WIN32
    if (display_env(p) != NULL)
        return ENOTSUP;
#endif
    if (out->max < 1 || out->size < 1 ||
        etude_processor_cpu(etude_processor_find(p->cpu), cpu, size) != 0)
        return E2BIG;
    return display_cpu(p, s, cpu, size);
}

/**
 * Attach profile `p`'s display device, at the window's fixed size.
 */
static void add_display(struct line *l, const struct etude_qemu_profile *p)
{
    const struct display *d = display_find(p->display);

    if (d == NULL || d->device == NULL)
        return;
    add(l, 0, "-device");
    add(l, 0, "%s,%s%s", d->device, DISPLAY_WINDOW, d->properties);
}

/**
 * Add profile `p`'s machine options: the q35 chipset, no i8042, the SMBIOS
 * vendor and an ACPI table.
 *
 * @return 0, or EINVAL for an ACPI table that -acpitable cannot take.
 */
static int add_machine(struct line *l, const struct etude_qemu_profile *p, enum etude_accel accel)
{
    const char *table;
    size_t i;

    if (accel == ETUDE_ACCEL_KVM) {
        for (i = 0; p->devices != NULL && p->devices[i].model != NULL; i++) {
            if (strcmp(p->devices[i].model, "intel-iommu") == 0) {
                /* QEMU remaps interrupts only with a userspace I/O APIC. */
                add(l, 0, "-machine");
                add(l, 0, "kernel-irqchip=split");
                break;
            }
        }
    }
    if (p->q35) {
        add(l, 0, "-machine");
        add(l, 0, "q35");
    }
    if (p->no_i8042) {
        add(l, 0, "-machine");
        add(l, 0, "i8042=off");
    }
    if (p->smbios_vendor != NULL) {
        add(l, 0, "-smbios");
        add(l, 1, "type=0,vendor=%s", p->smbios_vendor);
    }
    if (p->acpi_table == NULL)
        return 0;
    if ((table = acpi_table_path(p->acpi_table)) == NULL)
        return EINVAL;
    add(l, 0, "-acpitable");
    add(l, QUOTE_COMMAS, "file=%s", table);
    return 0;
}

/** Add what profile `p`'s firmware boots: the kernel and RAM image, or OVMF. */
static void add_firmware(struct line *l, const struct etude_qemu_profile *p, const char *ovmf)
{
    if (p->firmware == ETUDE_FIRMWARE_DIRECT) {
        add(l, 0, "-kernel");
        add(l, 0, "%s", p->kernel);
        if (p->initrd != NULL) {
            add(l, 0, "-initrd");
            add(l, 0, "%s", p->initrd);
        }
    } else if (p->firmware == ETUDE_FIRMWARE_OVMF) {
        add(l, 0, "-drive");
        add(l, 1, "if=pflash,format=raw,readonly=on,file=%s", ovmf);
    }
}

/**
 * Start QEMU in a user and network namespace of its own when a card of `p`
 * has a tap, as unshare(1) makes them: a shell there makes the tap, with
 * the queues the card asks for, gives its end ETUDE_TAP_HOST, brings it up,
 * and becomes QEMU, whose words follow. The namespace's root is the user's
 * own identity, so no privilege is needed, and the namespace ends with
 * QEMU. Returns 0, or ENOTSUP on a host other than Linux.
 */
static int add_tap_namespace(struct line *l, const struct etude_qemu_profile *p)
{
    size_t i;

    for (i = 0; i < device_count(p->devices) && p->devices[i].tap_queues == 0; i++)
        ;
    if (i == device_count(p->devices))
        return 0;
#ifdef __linux__
    add(l, 0, "%s", ETUDE_TAP_UNSHARE);
    add(l, 0, "--user");
    add(l, 0, "--map-root-user");
    add(l, 0, "--net");
    add(l, 0, "%s", ETUDE_TAP_SHELL);
    add(l, 0, "-c");
    add(l, 0,
        ETUDE_TAP_IP " tuntap add dev " ETUDE_TAP_NAME " mode tap multi_queue && " ETUDE_TAP_IP
                     " addr add " ETUDE_TAP_HOST " dev " ETUDE_TAP_NAME " && " ETUDE_TAP_IP
                     " link set " ETUDE_TAP_NAME " up && exec \"$@\"");
    add(l, 0, "etude-tap");
    return 0;
#else
    return ENOTSUP;
#endif
}

int etude_qemu_argv(const struct etude_qemu_profile *p, const struct etude_qemu_site *s,
                    const char *console, const char *qmp, const char *audio,
                    const struct etude_qemu_line *out)
{
    int x86_64 = machine_x86_64(p);
    const char *program = x86_64 ? s->qemu_x86_64 : s->qemu_i686;
    const char *ovmf = x86_64 ? s->ovmf_x86_64 : s->ovmf_i686;
    struct line l = {out->argv, 0, out->max, out->buf, 0, out->size, 0};
    char number[32], cpu[256];
    unsigned nics = 0, ide = 0, ahci = 0;
    size_t i;
    int e;

    if ((e = argv_check(p, s, audio, out, cpu, sizeof cpu)) != 0)
        return e;
    out->argv[0] = NULL;
    if ((e = add_tap_namespace(&l, p)) != 0)
        return e;
    add(&l, 0, "%s", program);
    add(&l, 0, "-accel");
    add(&l, 0, "%s", accel_option(s->accel));
    add(&l, 0, "-nodefaults");
    add(&l, 0, "-display");
    if (display_env(p) != NULL)
        add(&l, 0, "egl-headless,rendernode=%s", DISPLAY_RENDER_NODE);
    else
        add(&l, 0, "none");
    add(&l, 0, "-no-reboot");
    add(&l, 0, "-S");
    add(&l, 0, "-cpu");
    add(&l, 0, "%s", cpu);
    snprintf(number, sizeof(number), "%u", machine_processors(p, s->accel));
    add(&l, 0, "-smp");
    add(&l, 0, "%s", number);
    snprintf(number, sizeof(number), "%lluB", (unsigned long long)p->memory_bytes);
    add(&l, 0, "-m");
    add(&l, 0, "%s", number);
    if (boundary(p))
        add_boundary_memory(&l, number);
    else if (p->display != NULL && display_find(p->display)->shared)
        add_shared_memory(&l, number);
    if ((e = add_machine(&l, p, s->accel)) != 0)
        return e;
    add_display(&l, p);
    add_firmware(&l, p, ovmf);
    add_ahci(&l, p);
    for (i = 0; i < disk_count(p->disks); i++)
        add_disk(&l, &p->disks[i], i, p->q35, &ide, &ahci);
    add_audio(&l, p, audio);
    for (i = 0; i < device_count(p->devices); i++)
        add_device(&l, p, &p->devices[i], &nics);
    add(&l, 0, "-chardev");
#ifdef _WIN32
    add(&l, 1, "socket,id=etude-console,host=127.0.0.1,port=%s", console);
#else
    add(&l, 1, "socket,id=etude-console,path=%s", console);
#endif
    if (p->console == ETUDE_CONSOLE_VIRTIO) {
        /* After the profile's devices, at a slot of its own, so that each
         * of them has the slot it has beside a UART. */
        add(&l, 0, "-device");
        add(&l, 0, "virtio-serial-pci,addr=" CONSOLE_SLOT);
        add(&l, 0, "-device");
        add(&l, 0, "virtserialport,chardev=etude-console");
    } else {
        add(&l, 0, "-serial");
        add(&l, 0, "chardev:etude-console");
    }
    add(&l, 0, "-chardev");
#ifdef _WIN32
    add(&l, 1, "socket,id=etude-qmp,host=127.0.0.1,port=%s", qmp);
#else
    add(&l, 1, "socket,id=etude-qmp,path=%s", qmp);
#endif
    add(&l, 0, "-mon");
    add(&l, 0, "chardev=etude-qmp,mode=control");
#ifdef _WIN32
    /* A Windows process that exits resets its sockets; see posix/qemu.c. */
    add(&l, 0, "-no-shutdown");
#endif
    if (count(p->boot_options) > 0 || p->console == ETUDE_CONSOLE_VIRTIO) {
        add(&l, 0, "-append");
        add_joined(&l, p->boot_options, p->console == ETUDE_CONSOLE_VIRTIO ? CONSOLE_OPTION : NULL);
    }
    return l.error;
}

/** Say whether `entry` of an environment sets one of SDL's audio variables. */
static int sdl_audio_variable(const char *entry)
{
    return strncmp(entry, "SDL_AUDIO", 9) == 0 || strncmp(entry, "SDL_DISKAUDIO", 13) == 0;
}

/** Say whether `entry` of an environment sets a variable that `env` sets. */
static int env_variable(const char *const *env, const char *entry)
{
    size_t i, n;

    for (i = 0; env != NULL && env[i] != NULL; i++) {
        n = (size_t)(strchr(env[i], '=') - env[i]) + 1;
        if (strncmp(entry, env[i], n) == 0)
            return 1;
    }
    return 0;
}

/** The value that `envp` gives variable `name`, or NULL. */
static const char *env_value(char *const *envp, const char *name)
{
    size_t i, n = strlen(name);

    for (i = 0; envp != NULL && envp[i] != NULL; i++)
        if (strncmp(envp[i], name, n) == 0 && envp[i][n] == '=')
            return envp[i] + n + 1;
    return NULL;
}

int etude_qemu_env(const struct etude_qemu_profile *p, char *const *envp, const char *input,
                   const struct etude_qemu_line *out)
{
#ifdef _WIN32
    static const char null_device[] = "NUL";
#else
    static const char null_device[] = "/dev/null";
#endif
    struct line l = {out->argv, 0, out->max, out->buf, 0, out->size, 0};
    size_t i;
    const char *const *gl;
    const char *tree;
    int e;

    if ((e = audio_check(p, "wav")) != 0)
        return e;
    if (out->max == 0)
        return E2BIG;
    out->argv[0] = NULL;
    gl = display_env(p);
    for (i = 0; envp != NULL && envp[i] != NULL; i++)
        if (!sdl_audio_variable(envp[i]) && !env_variable(gl, envp[i]))
            add(&l, 0, "%s", envp[i]);
    for (i = 0; gl != NULL && gl[i] != NULL; i++)
        add(&l, 0, "%s", gl[i]);
    tree = env_value(envp, "ETUDE_TREE");
    if (gl != NULL && tree != NULL && *tree != '\0' &&
        env_value(envp, "MESA_SHADER_CACHE_DIR") == NULL)
        add(&l, 0, "MESA_SHADER_CACHE_DIR=%s" MESA_HOST_CACHE, tree);
    if (p->audio != NULL && strcmp(audio_backend(p), "sdl") == 0) {
        if (input == NULL || *input == '\0')
            return EINVAL;
        add(&l, 0, "SDL_AUDIODRIVER=disk");
        add(&l, 0, "SDL_DISKAUDIOFILEIN=%s", input);
        add(&l, 0, "SDL_DISKAUDIOFILE=%s", null_device);
    }
    return l.error;
}

int etude_qemu_accel_refused(const char *err, enum etude_accel accel)
{
    char needle[32], configured[64];
    const char *line, *at, *end;

    if (err == NULL || etude_accel_name(accel) == NULL)
        return 0;
    snprintf(needle, sizeof(needle), ": -accel %s: ", etude_accel_name(accel));
    snprintf(configured, sizeof(configured), ": -accel %s: ", accel_option(accel));
    for (line = err; *line != '\0'; line = *end == '\0' ? end : end + 1) {
        end = strchr(line, '\n');
        if (end == NULL)
            end = line + strlen(line);
        at = strstr(line, needle);
        if (at == NULL)
            at = strstr(line, configured);
        if (at != NULL && at < end && at > line)
            return 1;
    }
    return 0;
}
