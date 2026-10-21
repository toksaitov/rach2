/*
 * legacy_guest.c: the attempts of the set driver's guest cases. Each boots
 * the case's QEMU profile from the run's artifact store under the
 * supervisor, starts etude-guest at the debug shell as libetude-case's
 * helper machines do, and runs the case's invocation through the serial
 * controller, its program staged in the image or sent from the store.
 */
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "etude/qemu.h"
#include "../json.h"
#include "legacy_guest.h"
#include "posix.h"
#include "../serial.h"
#include "sha256.h"

/* The debug shell's greeting, and the command that starts etude-guest
 * there with the image's root as the directory of staged files. */
#define SHELL_READY "Type 'help' for available commands."
#define GUEST_EXEC  "exec /etude-guest --stage / --resend-ms "

/* The most devices, kernel options and strings of the devices' properties
 * (each name, each value, and the end of each device's list) of a profile
 * the driver takes. */
#define DEVICES_MAX    32
#define OPTIONS_MAX    32
#define PROPERTIES_MAX 64

/* The room for a path, a line of the record and a reason. */
#define PATH_LEN 4096
#define LINE_LEN 8192
#define WHY_LEN  400

/* Where an attempt is. */
enum phase {
    BOOTING,  /* the machine boots to the debug shell */
    OPENING,  /* etude-guest's session opens */
    PUTTING,  /* the program goes to etude-guest's store */
    INVOKING, /* and then the invocation */
    RUNNING,  /* the program runs */
    ENDED     /* the run was cleaned or failed; the machine is stopped */
};

struct legacy_guest {
    struct legacy_guest_spec spec;
    const struct legacy_case *k;
    struct etude_supervised *a;
    struct etude_machine *m;
    struct etude_serial *serial;
    enum phase phase;
    size_t fed;    /* the console bytes given to the controller */
    int closed;    /* the controller was told the console closed */
    int stopping;  /* the machine was asked to stop */
    int cancelled; /* a stop of the run cancelled the attempt */
    int cleaned;   /* etude-guest reported the run cleaned */
    uint64_t cleaned_us, cancel_until;
    char *bytes; /* the program's bytes while a put of them goes on */
    char *inv;   /* the invocation */
    size_t ninv;
    char inv_sha[65], token[ETUDE_FRAME_TOKEN_LEN + 1];
    char prog_sha[65];
    uint64_t prog_size;
    char prog_path[PATH_LEN]; /* the program's object in the store */
    char console[PATH_LEN];
    char why[WHY_LEN];
    FILE *out, *err;
    struct etude_qemu_profile profile;
    struct etude_qemu_disk disks[ETUDE_QEMU_DISKS + 1];
    char disk_paths[ETUDE_QEMU_DISKS][PATH_LEN];
    struct etude_qemu_device devices[DEVICES_MAX + 1];
    const char *properties[PROPERTIES_MAX];
    const char *options[OPTIONS_MAX + 1];
    char kernel[PATH_LEN], initrd[PATH_LEN];
    char qemu_i686[PATH_LEN], qemu_x86_64[PATH_LEN];
    struct etude_qemu_site site;
    struct etude_machine_spec mspec;
    struct etude_ask ask;
};

/** Write why the attempt failed into `out`, `size` bytes; -1. */
static int failed(char *out, size_t size, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(out, size, fmt, ap);
    va_end(ap);
    return -1;
}

/* ---- The artifacts. ---- */

/** Give the goal of the artifact `name` of case `k`, or NULL. */
static const char *goal_of(const struct legacy_case *k, const char *name)
{
    size_t i;

    for (i = 0; name != NULL && i < k->ngoals; i++)
        if (strcmp(k->names[i], name) == 0)
            return k->goals[i];
    return NULL;
}

/**
 * Find the digest of `goal` in the preparation's record `record`, from its
 * line `file DIGEST SIZE INODE MTIME PATH`; 0, or -1 when it has none.
 */
static int recorded(const char *record, const char *goal, char hex[65])
{
    char line[LINE_LEN];
    unsigned long long size, inode;
    long long mtime;
    FILE *f = fopen(record, "r");
    int at, found = -1;

    if (f == NULL)
        return -1;
    while (found != 0 && fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\n")] = '\0';
        at = -1;
        if (sscanf(line, "file %64s %llu %llu %lld %n", hex, &size, &inode, &mtime, &at) == 4 &&
            at > 0 && strcmp(line + at, goal) == 0)
            found = 0;
    }
    fclose(f);
    return found;
}

/**
 * Put the artifact `name` of the attempt's case in the store, with the
 * digest the record gives it, and give its digest, its object's path and its
 * size; 0, or -1 with why in `g->why`.
 */
static int stock(struct legacy_guest *g, const char *name, char hex[65], char *path, uint64_t *size)
{
    const char *goal = goal_of(g->k, name);
    char src[PATH_LEN], got[65];

    if (goal == NULL)
        return failed(g->why, sizeof(g->why), "the artifact %s is not the case's", name);
    if (recorded(g->spec.record, goal, hex) != 0)
        return failed(g->why, sizeof(g->why), "the artifact %s, %s, is not in the record %s", name,
                      goal, g->spec.record);
    snprintf(src, sizeof(src), "%s/%s", g->spec.tree, goal);
    if (etude_store_put(g->spec.store, src, hex, got, size) != 0)
        return failed(g->why, sizeof(g->why), "the artifact %s cannot be stored: %s", goal,
                      errno == EBADMSG ? "it changed after the preparation" : strerror(errno));
    if (etude_store_path(g->spec.store, hex, path, PATH_LEN) != 0)
        return failed(g->why, sizeof(g->why), "the artifact %s has no path in the store", goal);
    return 0;
}

/**
 * Make disk `i` of the profile from `d`, a name or an object of the
 * profile's `disks`: an empty sparse disk of its size, a CD whose medium is
 * the artifact's object, or a private copy of the artifact; each made under
 * the workspace's `disks`. 0, or -1 with why in `g->why`.
 */
static int make_disk(struct legacy_guest *g, size_t i, const struct json *d)
{
    const char *name = d->type == JSON_STRING ? d->string : json_text(d, "name");
    const char *bus = d->type == JSON_OBJECT ? json_text(d, "bus") : NULL;
    const char *media = d->type == JSON_OBJECT ? json_text(d, "media") : NULL;
    const struct json *size = d->type == JSON_OBJECT ? json_member(d, "size_bytes") : NULL;
    const struct json *discard = d->type == JSON_OBJECT ? json_member(d, "discard") : NULL;
    const struct json *boot = d->type == JSON_OBJECT ? json_member(d, "boot") : NULL;
    struct etude_qemu_disk *q = &g->disks[i];
    char hex[65], object[PATH_LEN];
    uint64_t n;
    int fd;

    q->bus = bus == NULL                  ? ETUDE_BUS_IDE
             : strcmp(bus, "nvme") == 0   ? ETUDE_BUS_NVME
             : strcmp(bus, "ahci") == 0   ? ETUDE_BUS_AHCI
             : strcmp(bus, "device") == 0 ? ETUDE_BUS_DEVICE
                                          : ETUDE_BUS_IDE;
    q->id = d->type == JSON_OBJECT ? json_text(d, "id") : NULL;
    q->cdrom = media != NULL && strcmp(media, "cdrom") == 0;
    q->serial = d->type == JSON_OBJECT ? json_text(d, "serial") : NULL;
    q->discard = discard != NULL && discard->integer;
    q->boot = boot != NULL && boot->integer;
    q->path = g->disk_paths[i];
    snprintf(g->disk_paths[i], PATH_LEN, "%s/disks/%s", g->spec.work, name);
    if (size != NULL) {
        fd = open(g->disk_paths[i], O_WRONLY | O_CREAT | O_EXCL, 0600);
        if (fd < 0 || ftruncate(fd, (off_t)size->integer) != 0) {
            if (fd >= 0)
                close(fd);
            return failed(g->why, sizeof(g->why), "the empty disk %s cannot be made: %s", name,
                          strerror(errno));
        }
        close(fd);
        return 0;
    }
    if (stock(g, name, hex, object, &n) != 0)
        return -1;
    if (q->cdrom) {
        snprintf(g->disk_paths[i], PATH_LEN, "%s", object);
        return 0;
    }
    if (etude_store_copy(g->spec.store, hex, g->disk_paths[i]) != 0)
        return failed(g->why, sizeof(g->why), "the disk %s cannot be copied: %s", name,
                      strerror(errno));
    return 0;
}

/** Read the devices of the profile `q`, with their buses, ports, drives and
 * properties, into the attempt's list, ended by a NULL model; 0, or -1 with
 * why in `g->why`. */
static int make_devices(struct legacy_guest *g, const struct json *q)
{
    const struct json *list = json_member(q, "devices"), *d, *restrict_, *hub, *props;
    struct etude_qemu_device *v;
    size_t i, j, k = 0;

    for (i = 0; list != NULL && i < list->count; i++) {
        if (i == DEVICES_MAX)
            return failed(g->why, sizeof(g->why), "the profile has more than %d devices",
                          DEVICES_MAX);
        d = list->item[i];
        v = &g->devices[i];
        v->model = d->type == JSON_STRING ? d->string : json_text(d, "model");
        if (d->type != JSON_OBJECT)
            continue;
        v->id = json_text(d, "id");
        v->bus = json_text(d, "bus");
        v->port = json_text(d, "port");
        v->drive = json_text(d, "drive");
        props = json_member(d, "properties");
        if (props != NULL && props->count > 0) {
            if (k + 2 * props->count + 1 > PROPERTIES_MAX)
                return failed(g->why, sizeof(g->why),
                              "the profile's devices have more than %d strings of properties",
                              PROPERTIES_MAX);
            v->properties = g->properties + k;
            for (j = 0; j < props->count; j++) {
                g->properties[k++] = props->item[j]->name;
                g->properties[k++] = props->item[j]->string;
            }
            g->properties[k++] = NULL;
        }
        v->net = json_text(d, "net");
        v->host = json_text(d, "host");
        v->dhcpstart = json_text(d, "dhcpstart");
        v->dns = json_text(d, "dns");
        v->mac = json_text(d, "mac");
        restrict_ = json_member(d, "restrict");
        v->restricted = restrict_ != NULL && restrict_->integer;
        hub = json_member(d, "hub");
        v->hub = hub != NULL && hub->integer;
    }
    return 0;
}

/** Give the integer member `name` of `q`, or 0 when it has none. */
static long long integer_of(const struct json *q, const char *name)
{
    const struct json *v = json_member(q, name);

    return v != NULL && v->type == JSON_INTEGER ? v->integer : 0;
}

/**
 * Make the machine's profile from the case's: its kernel and RAM image as
 * the store's objects, its disks, its devices and its kernel options; 0, or
 * -1 with why in `g->why`.
 */
static int make_profile(struct legacy_guest *g)
{
    const struct json *q = g->k->qemu, *disks = json_member(q, "disks");
    const struct json *opts = json_member(q, "boot_options"), *i8042 = json_member(q, "i8042");
    const char *firmware = json_text(q, "firmware"), *initrd = json_text(q, "initrd");
    struct etude_qemu_profile *p = &g->profile;
    char hex[65], dir[PATH_LEN];
    uint64_t n;
    size_t i;

    snprintf(dir, sizeof(dir), "%s/disks", g->spec.work);
    if (disks != NULL && disks->count > 0 && mkdir(dir, 0700) != 0)
        return failed(g->why, sizeof(g->why), "%s cannot be made: %s", dir, strerror(errno));
    if (disks != NULL && disks->count > ETUDE_QEMU_DISKS)
        return failed(g->why, sizeof(g->why), "the profile has more than %d disks",
                      ETUDE_QEMU_DISKS);
    for (i = 0; disks != NULL && i < disks->count; i++)
        if (make_disk(g, i, disks->item[i]) != 0)
            return -1;
    if (opts != NULL && opts->count > OPTIONS_MAX)
        return failed(g->why, sizeof(g->why), "the profile has more than %d kernel options",
                      OPTIONS_MAX);
    for (i = 0; opts != NULL && i < opts->count; i++)
        g->options[i] = opts->item[i]->string;
    if (make_devices(g, q) != 0 || stock(g, json_text(q, "kernel"), hex, g->kernel, &n) != 0 ||
        (initrd != NULL && stock(g, initrd, hex, g->initrd, &n) != 0))
        return -1;
    p->architecture = g->spec.arch;
    p->kernel = g->kernel;
    p->initrd = initrd != NULL ? g->initrd : NULL;
    p->disks = g->disks;
    p->cpu = json_text(q, "cpu");
    p->processors = (unsigned)integer_of(q, "processors");
    p->memory_bytes = (uint64_t)integer_of(q, "memory_bytes");
    p->memory_profile = json_text(q, "memory_profile");
    p->audio = json_text(q, "audio");
    p->firmware = firmware != NULL && strcmp(firmware, "seabios") == 0 ? ETUDE_FIRMWARE_SEABIOS
                                                                       : ETUDE_FIRMWARE_DIRECT;
    p->devices = g->devices;
    p->boot_options = g->options;
    p->no_i8042 = i8042 != NULL && i8042->type == JSON_BOOLEAN && !i8042->integer;
    p->q35 = json_text(q, "machine") != NULL && strcmp(json_text(q, "machine"), "q35") == 0;
    return 0;
}

/**
 * Give the path of `name` on PATH in `out`, or NULL when it is not there.
 * The directory RACH2_QEMU_SHIMS names, where tests/run puts its QEMU
 * wrappers, each of which adds an `-accel` of its own, is passed by, so that
 * QEMU is given the one accelerator the launcher asks for.
 */
static const char *on_path(const char *name, char *out, size_t size)
{
    const char *path = getenv("PATH"), *shims = getenv("RACH2_QEMU_SHIMS"), *end;
    size_t len;

    for (; path != NULL && *path != '\0'; path = *end == ':' ? end + 1 : end) {
        end = strchr(path, ':');
        if (end == NULL)
            end = path + strlen(path);
        len = (size_t)(end - path);
        if (len == 0 || (shims != NULL && len == strlen(shims) && strncmp(path, shims, len) == 0))
            continue;
        snprintf(out, size, "%.*s/%s", (int)len, path, name);
        if (access(out, X_OK) == 0)
            return out;
    }
    return NULL;
}

/** Fill the site of the attempt's machine: the accelerator and QEMU's
 * program for its architecture; 0, or -1 with why in `g->why`. */
static int make_site(struct legacy_guest *g)
{
    int x86_64 = strcmp(g->spec.arch, "x86_64") == 0;
    const char *program = x86_64 ? "qemu-system-x86_64" : "qemu-system-i386";

    if (etude_accel_parse(g->spec.accel, &g->site.accel) != 0)
        return failed(g->why, sizeof(g->why), "the accelerator %s is not QEMU's", g->spec.accel);
    if (on_path(program, x86_64 ? g->qemu_x86_64 : g->qemu_i686, PATH_LEN) == NULL)
        return failed(g->why, sizeof(g->why), "%s is not on PATH", program);
    g->site.qemu_i686 = g->qemu_i686;
    g->site.qemu_x86_64 = g->qemu_x86_64;
    return 0;
}

/* ---- The invocation. ---- */

/** Append `s` to `b` as a JSON string. */
static void put_string(struct jbuf *b, const char *s)
{
    char esc[8];

    jbuf_puts(b, "\"");
    for (; *s != '\0'; s++) {
        if (*s == '"' || *s == '\\') {
            esc[0] = '\\';
            esc[1] = *s;
            jbuf_put(b, esc, 2);
        } else if ((unsigned char)*s < 0x20) {
            snprintf(esc, sizeof(esc), "\\u%04x", (unsigned)(unsigned char)*s);
            jbuf_puts(b, esc);
        } else {
            jbuf_put(b, s, 1);
        }
    }
    jbuf_puts(b, "\"");
}

/** Append the member `name` with the string `value` to `b`, after a comma
 * unless `first`. */
static void put_member(struct jbuf *b, const char *name, const char *value, int first)
{
    if (!first)
        jbuf_puts(b, ",");
    put_string(b, name);
    jbuf_puts(b, ":");
    put_string(b, value);
}

/** Give the SHA-256 of `n` bytes at `p` in hex. */
static void digest_of(const void *p, size_t n, char hex[65])
{
    struct sha256 h;
    unsigned char d[32];

    sha256_init(&h);
    sha256_update(&h, (const unsigned char *)p, n);
    sha256_final(&h, d);
    sha256_hex(d, hex);
}

/**
 * Write the attempt's invocation (schemas/invocation.schema.json): the
 * case's program and arguments in the workspace, the case's identity, seed
 * and architecture in its environment, its scratch as TMPDIR, and its bound
 * as the deadline; and its digest and the attempt's token, the first digits
 * of the digest of the attempt's identity.
 */
static void make_invocation(struct legacy_guest *g)
{
    const struct legacy_case *k = g->k;
    struct jbuf b = {NULL, 0, 0};
    char num[64], hex[65];
    size_t i;

    digest_of(g->spec.attempt_id, strlen(g->spec.attempt_id), hex);
    snprintf(g->token, sizeof(g->token), "%.*s", ETUDE_FRAME_TOKEN_LEN, hex);
    jbuf_puts(&b, "{\"schema_version\":1,");
    put_member(&b, "run_id", g->spec.run_id, 1);
    put_member(&b, "attempt_id", g->spec.attempt_id, 0);
    put_member(&b, "attempt_token", g->token, 0);
    put_member(&b, "case_id", k->case_id, 0);
    put_member(&b, "program", k->names[k->program], 0);
    jbuf_puts(&b, ",\"arguments\":[");
    for (i = 0; i < k->nargs; i++) {
        if (i > 0)
            jbuf_puts(&b, ",");
        put_string(&b, k->args[i]);
    }
    jbuf_puts(&b, "],\"environment\":{");
    put_member(&b, "PATH", "/bin:/usr/bin:/sbin:/usr/sbin", 1);
    put_member(&b, "HOME", "${WORKSPACE}", 0);
    put_member(&b, "ETUDE_CASE", k->case_id, 0);
    if (k->seeded) {
        snprintf(num, sizeof(num), "%lld", k->seed);
        put_member(&b, "ETUDE_SEED", num, 0);
    }
    if (strcmp(k->arch, "none") != 0)
        put_member(&b, "ARCH", k->arch, 0);
    if (k->scratch)
        put_member(&b, "TMPDIR", "${WORKSPACE}", 0);
    jbuf_puts(&b, "},\"stdin\":null,\"role\":\"ordinary\",\"files\":[{");
    put_member(&b, "name", k->names[k->program], 1);
    put_member(&b, "sha256", g->prog_sha, 0);
    snprintf(num, sizeof(num), ",\"size_bytes\":%llu,", (unsigned long long)g->prog_size);
    jbuf_puts(&b, num);
    put_member(&b, "mode", "0755", 1);
    jbuf_puts(&b, "}],\"fixtures\":[{\"fixture\":\"workspace\",\"given\":[]}]");
    snprintf(num, sizeof(num), ",\"deadline_us\":%llu", (unsigned long long)k->case_us);
    jbuf_puts(&b, num);
    snprintf(num, sizeof(num), ",\"grace_us\":%llu", (unsigned long long)g->spec.grace_us);
    jbuf_puts(&b, num);
    snprintf(num, sizeof(num), ",\"output_max_bytes\":%lu}\n",
             (unsigned long)LEGACY_GUEST_OUTPUT_MAX);
    jbuf_puts(&b, num);
    g->inv = b.data;
    g->ninv = b.len;
    digest_of(g->inv, g->ninv, g->inv_sha);
}

/* ---- The session. ---- */

/** Write a frame of the controller's to the machine's console. */
static int serial_write(void *ctx, const char *buf, size_t n)
{
    struct legacy_guest *g = ctx;

    return etude_machine_type(g->m, buf, n);
}

/** Take `n` bytes of the program's descriptor `fd` into its file. */
static void serial_output(void *ctx, int fd, const char *buf, size_t n)
{
    struct legacy_guest *g = ctx;
    FILE *f = fd == 1 ? g->out : g->err;

    if (f != NULL && fwrite(buf, 1, n, f) == n)
        fflush(f);
}

/** Stop the machine, once. */
static void stop_machine(struct legacy_guest *g)
{
    if (g->stopping)
        return;
    g->stopping = 1;
    etude_machine_cancel(g->m);
}

/** End the attempt for what `fmt` says, unless it has a reason, and stop
 * the machine. */
static void lose(struct legacy_guest *g, const char *fmt, ...)
{
    va_list ap;

    if (g->why[0] == '\0' && !g->cancelled) {
        va_start(ap, fmt);
        vsnprintf(g->why, sizeof(g->why), fmt, ap);
        va_end(ap);
    }
    g->phase = ENDED;
    stop_machine(g);
}

/** Ask the case behind attempt `id` to stop, for its watchdog; 0 once
 * etude-guest was asked, or an error number when no run goes on, after
 * which the supervisor stops the machine. */
static int ask(void *ctx, const char *id)
{
    struct legacy_guest *g = ctx;

    (void)id;
    if (g->phase != RUNNING || etude_serial_status(g->serial) != ETUDE_SERIAL_BUSY)
        return ECANCELED;
    return etude_serial_cancel(g->serial, posix_now_us()) == 0 ? 0 : errno;
}

/** Search `n` bytes at `hay` for `text`; the offset past it, or -1. */
static long find_text(const char *hay, size_t n, const char *text)
{
    size_t len = strlen(text), i;

    for (i = 0; len > 0 && i + len <= n; i++)
        if (hay[i] == text[0] && memcmp(hay + i, text, len) == 0)
            return (long)(i + len);
    return -1;
}

/** Start etude-guest at the debug shell, whose greeting ends at `at`, and
 * begin to open its session. */
static void start_session(struct legacy_guest *g, long at, uint64_t now)
{
    struct etude_serial_conf sc;
    char command[96];

    snprintf(command, sizeof(command), GUEST_EXEC "%llu\n",
             (unsigned long long)((LEGACY_GUEST_RESEND_US + 999) / 1000));
    if (etude_machine_type(g->m, command, strlen(command)) != 0) {
        lose(g, "the debug shell's console closed");
        return;
    }
    memset(&sc, 0, sizeof(sc));
    sc.run_id = g->spec.attempt_id;
    sc.resend_us = LEGACY_GUEST_RESEND_US;
    sc.output_max = LEGACY_GUEST_OUTPUT_MAX;
    sc.write = serial_write;
    sc.output = serial_output;
    sc.ctx = g;
    g->fed = (size_t)at;
    if ((g->serial = etude_serial_new(&sc)) == NULL || etude_serial_open(g->serial, now) != 0) {
        lose(g, "etude-guest's session could not be opened: %s", strerror(errno));
        return;
    }
    g->phase = OPENING;
}

/** Give the console bytes not yet read to the controller, and tell it when
 * the console closed or passed its bound. */
static void feed_session(struct legacy_guest *g, uint64_t now)
{
    const struct etude_machine_result *r = etude_machine_result(g->m);

    if (g->serial == NULL || g->closed)
        return;
    if (g->fed < r->console.kept) {
        etude_serial_feed(g->serial, r->console.data + g->fed, r->console.kept - g->fed, now);
        g->fed = r->console.kept;
    }
    if (r->console.closed || r->state == ETUDE_MACHINE_ENDED || r->console.total > g->fed) {
        etude_serial_closed(g->serial);
        g->closed = 1;
        return;
    }
    etude_serial_tick(g->serial, now);
}

/** Read the program's object into `g->bytes` for a put of its bytes; 0, or
 * -1. */
static int read_program(struct legacy_guest *g)
{
    FILE *f = fopen(g->prog_path, "rb");
    size_t got = 0;

    if (f != NULL && (g->bytes = malloc((size_t)g->prog_size + 1)) != NULL)
        got = fread(g->bytes, 1, (size_t)g->prog_size, f);
    if (f != NULL)
        fclose(f);
    return g->bytes != NULL && got == (size_t)g->prog_size ? 0 : -1;
}

/** Say what etude-guest's last operation came to, for a reason. */
static const char *session_why(const struct legacy_guest *g)
{
    const char *why = etude_serial_why(g->serial);

    return etude_serial_status(g->serial) == ETUDE_SERIAL_LOST ? "the console was lost"
           : why[0] != '\0'                                    ? why
                                                               : "it did not complete";
}

/** Move the session on from the operation that ended. */
static void step(struct legacy_guest *g, uint64_t now)
{
    enum etude_serial_status st;
    const struct etude_serial_result *r;
    const char *name = g->k->names[g->k->program];

    if (g->serial == NULL || g->phase == BOOTING || g->phase == ENDED ||
        (st = etude_serial_status(g->serial)) == ETUDE_SERIAL_BUSY)
        return;
    switch (g->phase) {
    case OPENING:
        if (st != ETUDE_SERIAL_DONE) {
            lose(g, "etude-guest's session did not open: %s", session_why(g));
            return;
        }
        etude_supervised_stage(g->a, ETUDE_STAGE_TRANSFER);
        g->phase = PUTTING;
        if (etude_serial_put(g->serial, name, g->prog_size, g->prog_sha, "0755", NULL, now) != 0)
            lose(g, "the program could not be put: %s", strerror(errno));
        return;
    case PUTTING:
        if (st == ETUDE_SERIAL_REFUSED && g->bytes == NULL) {
            if (read_program(g) != 0)
                lose(g, "the program's object %s cannot be read", g->prog_path);
            else if (etude_serial_put(g->serial, name, g->prog_size, g->prog_sha, "0755", g->bytes,
                                      now) != 0)
                lose(g, "the program could not be put: %s", strerror(errno));
            return;
        }
        if (st != ETUDE_SERIAL_DONE) {
            lose(g, "etude-guest did not store the program: %s", session_why(g));
            return;
        }
        free(g->bytes);
        g->bytes = NULL;
        g->phase = INVOKING;
        if (etude_serial_put(g->serial, "invocation.json", g->ninv, g->inv_sha, "0644", g->inv,
                             now) != 0)
            lose(g, "the invocation could not be put: %s", strerror(errno));
        return;
    case INVOKING:
        if (st != ETUDE_SERIAL_DONE) {
            lose(g, "etude-guest did not store the invocation: %s", session_why(g));
            return;
        }
        etude_supervised_stage(g->a, ETUDE_STAGE_EXECUTE);
        if (etude_serial_run(g->serial, g->token, g->inv_sha, now) != 0) {
            lose(g, "the invocation could not be run: %s", strerror(errno));
            return;
        }
        g->phase = RUNNING;
        return;
    case RUNNING:
        r = etude_serial_result(g->serial);
        if (st == ETUDE_SERIAL_DONE && r->complete) {
            g->cleaned = 1;
            g->cleaned_us = now;
            etude_supervised_end(g->a, r->end, r->value, r->cleanup);
            g->phase = ENDED;
            stop_machine(g);
            return;
        }
        lose(g, "etude-guest did not report the run whole: %s", session_why(g));
        return;
    default:
        return;
    }
}

void legacy_guest_serve(struct legacy_guest *g)
{
    const struct etude_machine_result *r = etude_machine_result(g->m);
    uint64_t now = posix_now_us();
    long at;

    if (g->phase == BOOTING) {
        at = find_text(r->console.data, r->console.kept, SHELL_READY);
        if (at >= 0)
            start_session(g, at, now);
        else if (r->state == ETUDE_MACHINE_ENDED || r->console.closed ||
                 r->console.total > r->console.kept)
            lose(g, "the machine ended before its debug shell greeted");
    }
    feed_session(g, now);
    step(g, now);
    if (g->cancelled && !g->stopping && now >= g->cancel_until)
        stop_machine(g);
}

void legacy_guest_cancel(struct legacy_guest *g)
{
    uint64_t now = posix_now_us();

    if (g->cancelled)
        return;
    g->cancelled = 1;
    g->cancel_until = now + g->spec.grace_us;
    if (g->phase != RUNNING || etude_serial_status(g->serial) != ETUDE_SERIAL_BUSY ||
        etude_serial_cancel(g->serial, now) != 0)
        stop_machine(g);
}

const char *legacy_guest_end(const struct legacy_guest *g, const struct etude_supervised_result *r,
                             int *value)
{
    const struct etude_serial_result *s = g->serial != NULL ? etude_serial_result(g->serial) : NULL;

    *value = -1;
    if (g->cleaned && (r->expiry == ETUDE_EXPIRY_NONE || g->cleaned_us <= r->expired_us) &&
        (s->end == ETUDE_END_EXITED || s->end == ETUDE_END_SIGNALED)) {
        *value = s->value;
        return s->end == ETUDE_END_EXITED ? "exited" : "signaled";
    }
    if (r->expiry != ETUDE_EXPIRY_NONE || g->cancelled ||
        (g->cleaned && s->end == ETUDE_END_STOPPED))
        return "stopped";
    return "lost";
}

const char *legacy_guest_why(const struct legacy_guest *g)
{
    return g->why;
}

const char *legacy_guest_console(const struct legacy_guest *g)
{
    return g->console;
}

int legacy_guest_start(struct etude_supervisor *s, const struct legacy_guest_spec *spec,
                       struct legacy_guest **out, struct etude_supervised **a, char *why,
                       size_t size)
{
    struct legacy_guest *g = calloc(1, sizeof(*g));
    *out = NULL;
    if (g == NULL)
        return failed(why, size, "memory ran out");
    g->spec = *spec;
    g->k = spec->k;
    snprintf(g->console, sizeof(g->console), "%s/console", spec->work);
    if (make_site(g) != 0 || make_profile(g) != 0 ||
        stock(g, g->k->names[g->k->program], g->prog_sha, g->prog_path, &g->prog_size) != 0)
        goto fail;
    if ((g->out = fopen(spec->out, "w")) == NULL || (g->err = fopen(spec->err, "w")) == NULL) {
        failed(g->why, sizeof(g->why), "the program's output cannot be written: %s",
               strerror(errno));
        goto fail;
    }
    make_invocation(g);
    g->ask.fn = ask;
    g->ask.ctx = g;
    g->mspec.attempt_id = spec->attempt_id;
    g->mspec.profile = &g->profile;
    g->mspec.site = &g->site;
    g->mspec.envp = spec->envp;
    g->mspec.console_path = g->console;
    g->mspec.console_max = LEGACY_GUEST_CONSOLE_MAX;
    g->mspec.grace_us = spec->grace_us;
    g->mspec.memory_reserved_bytes = spec->memory_bytes;
    g->mspec.reg = spec->reg;
    g->mspec.reg_ctx = spec->reg_ctx;
    if (etude_supervise_machine(s, &g->mspec, spec->wd, 0, &g->ask, a) != 0) {
        failed(g->why, sizeof(g->why), "QEMU could not start: %s", strerror(errno));
        goto fail;
    }
    g->a = *a;
    g->m = etude_supervised_machine(*a);
    *out = g;
    return 0;
fail:
    snprintf(why, size, "%s", g->why);
    legacy_guest_free(g);
    return -1;
}

void legacy_guest_free(struct legacy_guest *g)
{
    if (g == NULL)
        return;
    if (g->out != NULL)
        fclose(g->out);
    if (g->err != NULL)
        fclose(g->err);
    if (g->serial != NULL)
        etude_serial_free(g->serial);
    free(g->bytes);
    free(g->inv);
    free(g);
}
