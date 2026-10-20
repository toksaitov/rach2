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

/* The most disks the IDE controllers of QEMU's PC take. */
#define IDE_DISKS 4

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

/**
 * Add an argument made from `format`, in which each `%s` is replaced by the
 * next string, with its commas doubled when `quote` is set, as QEMU's option
 * syntax needs inside a value.
 */
static void add(struct line *l, int quote, const char *format, ...)
{
    char *start = l->buf + l->used;
    const char *f, *s;
    va_list ap;

    if (l->error)
        return;
    if (l->n + 2 > l->max) {
        l->error = E2BIG;
        return;
    }
    va_start(ap, format);
    for (f = format; *f != '\0'; f++) {
        if (f[0] == '%' && f[1] == 's') {
            for (s = va_arg(ap, const char *); *s != '\0'; s++) {
                if (l->used + 2 >= l->size)
                    break;
                if (*s == ',' && quote)
                    l->buf[l->used++] = ',';
                l->buf[l->used++] = *s;
            }
            f++;
        } else if (l->used + 1 < l->size) {
            l->buf[l->used++] = *f;
        }
    }
    va_end(ap);
    if (l->used + 1 >= l->size) {
        l->error = E2BIG;
        return;
    }
    l->buf[l->used++] = '\0';
    l->argv[l->n++] = start;
    l->argv[l->n] = NULL;
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

/**
 * Add the arguments that attach device `name`; `*nics` counts the network
 * cards before it, each of which has a user network of its own.
 */
static void add_device(struct line *l, const char *name, unsigned *nics)
{
    char id[16];
    int i;

    for (i = 0; nic_models[i] != NULL; i++) {
        if (strcmp(name, nic_models[i]) == 0) {
            snprintf(id, sizeof(id), "net%u", (*nics)++);
            add(l, 0, "-netdev");
            add(l, 0, "user,id=%s", id);
            add(l, 0, "-device");
            add(l, 0, "%s,netdev=%s", name, id);
            return;
        }
    }
    if (strcmp(name, "vga") == 0) {
        add(l, 0, "-vga");
        add(l, 0, "std");
        return;
    }
    add(l, 0, "-device");
    add(l, 0, "%s", name);
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
            p->initrd == NULL || p->audio != NULL || count(p->disks) != 0 ||
            count(p->devices) != 0 || count(p->boot_options) != 1 ||
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
    if (count(p->disks) > IDE_DISKS)
        return EINVAL;
    for (i = 0; i < count(p->devices); i++)
        if (!word(p->devices[i]))
            return EINVAL;
    for (i = 0; i < count(p->boot_options); i++)
        if (!word(p->boot_options[i]))
            return EINVAL;
    if (p->firmware == ETUDE_FIRMWARE_DIRECT)
        return p->kernel != NULL && *p->kernel != '\0' ? 0 : EINVAL;
    if (p->firmware != ETUDE_FIRMWARE_SEABIOS && p->firmware != ETUDE_FIRMWARE_OVMF)
        return EINVAL;
    return p->initrd == NULL && count(p->boot_options) == 0 && count(p->disks) > 0 ? 0 : EINVAL;
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
    unsigned nics = 0;
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
        add(&l, 0, "-bios");
        add(&l, 0, "%s", ovmf);
    }
    for (i = 0; i < count(p->disks); i++) {
        snprintf(number, sizeof(number), "%u", (unsigned)i);
        add(&l, 0, "-drive");
        add(&l, 1, "file=%s,format=raw,if=ide,index=%s,media=disk", p->disks[i], number);
    }
    add_audio(&l, p->audio, audio);
    for (i = 0; i < count(p->devices); i++)
        add_device(&l, p->devices[i], &nics);
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
