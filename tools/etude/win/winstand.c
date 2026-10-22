/*
 * winstand.c: the stand-ins that the tooling fixtures of winfixture put on
 * etude-tooling's PATH for curl, qemu-img and QEMU, as
 * tests/system/etude_tooling_test.sh does with shell scripts on POSIX
 * systems. One program plays each, by the name it is run under.
 *
 * The fixture's files are in FIX, the directory `fix` beside the directory
 * of the program. `curl` copies FIX/image. `qemu-img create` writes an
 * overlay that names its backing file, which must be found from the
 * overlay's directory. QEMU keeps its arguments in FIX/qemu.args and plays
 * the machine: with the seed volume, which must have the bytes of the
 * recipe's provision.sh, it answers as provision.sh does, and with the job
 * volume it does what runner.sh does, reading and writing the volume's
 * archive with etude-volume's code, volume/volume.c, and with a job of the
 * lines below in place of a shell script. FIX/qemu says how it misbehaves:
 * `hang`, `crash`, `lie`, or `slow`, which holds memory and uses processor
 * time in the job; FIX/provision `fail` makes provision.sh fail. A machine
 * that hangs and is not stopped writes FIX/woke when its sleep ends.
 *
 * A job's lines: `cat NAME` writes the working file NAME to the standard
 * output, `err TEXT` writes TEXT to the standard error, `disks` writes
 * `disks N` there, `disk TEXT` writes TEXT to the first disk, `write NAME
 * TEXT` makes the working file NAME, and `exit N` ends the job with N.
 */
#include <windows.h>
#include <direct.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "volume.h"

/* The longest name of a working file. */
#define NAME_MAX 512

static char fix[MAX_PATH * 2];

/* Why the job volume could not be read or written. */
static char why[512];

/** Give the file's contents with a NUL after them, or NULL. */
static char *slurp(const char *path, size_t *size)
{
    FILE *f = fopen(path, "rb");
    char *d;
    long n;

    if (f == NULL || fseek(f, 0, SEEK_END) != 0 || (n = ftell(f)) < 0 ||
        fseek(f, 0, SEEK_SET) != 0 || (d = malloc((size_t)n + 1)) == NULL) {
        if (f != NULL)
            fclose(f);
        return NULL;
    }
    if (fread(d, 1, (size_t)n, f) != (size_t)n) {
        fclose(f);
        free(d);
        return NULL;
    }
    fclose(f);
    d[n] = '\0';
    if (size != NULL)
        *size = (size_t)n;
    return d;
}

/** Write `n` bytes of `data` to `path`, in mode `mode`; 0 on success. */
static int spill(const char *path, const char *mode, const char *data, size_t n)
{
    FILE *f = fopen(path, mode);

    if (f == NULL)
        return -1;
    if (fwrite(data, 1, n, f) != n) {
        fclose(f);
        return -1;
    }
    return fclose(f) != 0 ? -1 : 0;
}

/** Give FIX/NAME's first line, or "" when it has none. */
static const char *fix_word(const char *name)
{
    static char word[64];
    char path[MAX_PATH * 2 + 64];
    char *d;

    snprintf(path, sizeof path, "%s\\%s", fix, name);
    word[0] = '\0';
    if ((d = slurp(path, NULL)) != NULL) {
        sscanf(d, "%63s", word);
        free(d);
    }
    return word;
}

/** Give the last name of `path`, which may end in separators, in `out`. */
static void base_name(const char *path, char *out, size_t size)
{
    size_t end = strlen(path), start;

    while (end > 0 && (path[end - 1] == '/' || path[end - 1] == '\\'))
        end--;
    start = end;
    while (start > 0 && path[start - 1] != '/' && path[start - 1] != '\\')
        start--;
    snprintf(out, size, "%.*s", (int)(end - start), path + start);
}

/** Make the directories above file `path` that are missing. */
static void make_parents(const char *path)
{
    char buf[MAX_PATH * 6];
    char *p;

    snprintf(buf, sizeof buf, "%s", path);
    for (p = buf + 1; *p; p++) {
        if (*p == '/' || *p == '\\') {
            char c = *p;

            *p = '\0';
            _mkdir(buf);
            *p = c;
        }
    }
}

/** Append a line to the channel `path`. */
static void say(const char *path, const char *line)
{
    spill(path, "ab", line, strlen(line));
}

/* A job's standard output or error. */
struct text {
    char data[65536];
    size_t len;
};

/** Append `n` bytes of `data` to `t`. */
static void add_text(struct text *t, const char *data, size_t n)
{
    if (n > sizeof t->data - t->len)
        exit(8);
    memcpy(t->data + t->len, data, n);
    t->len += n;
}

/** Hold 64 MiB and use half a second of processor time, for the mode `slow`. */
static void slow(void)
{
    size_t size = (size_t)64 << 20, i;
    char *p = VirtualAlloc(NULL, size, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    FILETIME t[4];
    ULONGLONG used = 0;

    if (p == NULL)
        exit(9);
    for (i = 0; i < size; i += 4096)
        p[i] = 1;
    while (used < 5000000u) {
        GetProcessTimes(GetCurrentProcess(), &t[0], &t[1], &t[2], &t[3]);
        used = ((ULONGLONG)t[2].dwHighDateTime << 32 | t[2].dwLowDateTime) +
               ((ULONGLONG)t[3].dwHighDateTime << 32 | t[3].dwLowDateTime);
    }
}

/**
 * Run the job whose files the volume gave in `vol`, with its working files
 * under `in`, as runner.sh runs job.sh in its working directory; give its
 * status.
 */
static int run_job(const char *vol, const char *disk, int ndisks, struct text *out,
                   struct text *err)
{
    char line[1024], name[NAME_MAX], path[MAX_PATH * 6], *script, *data;
    const char *p;
    size_t size;
    int status = 0, at;

    snprintf(path, sizeof path, "%s\\job.sh", vol);
    if ((script = slurp(path, NULL)) == NULL)
        return 127;
    for (p = script; *p != '\0';) {
        size_t n = strcspn(p, "\n");

        at = 0;
        snprintf(line, sizeof line, "%.*s", (int)n, p);
        p += n + (p[n] == '\n');
        if (sscanf(line, "cat %511s", name) == 1) {
            snprintf(path, sizeof path, "%s\\in\\%s", vol, name);
            if ((data = slurp(path, &size)) == NULL) {
                status = 1;
                break;
            }
            add_text(out, data, size);
            free(data);
        } else if (strncmp(line, "err ", 4) == 0) {
            add_text(err, line + 4, strlen(line + 4));
            add_text(err, "\n", 1);
        } else if (strcmp(line, "disks") == 0) {
            snprintf(name, sizeof name, "disks %d\n", ndisks);
            add_text(err, name, strlen(name));
        } else if (strncmp(line, "disk ", 5) == 0 && disk != NULL) {
            if (spill(disk, "wb", line + 5, strlen(line + 5)) != 0 || spill(disk, "ab", "\n", 1)) {
                status = 1;
                break;
            }
        } else if (sscanf(line, "write %511s %n", name, &at) == 1 && at > 0) {
            snprintf(path, sizeof path, "%s\\in\\%s", vol, name);
            make_parents(path);
            if (spill(path, "wb", line + at, strlen(line + at)) != 0 ||
                spill(path, "ab", "\n", 1) != 0) {
                status = 1;
                break;
            }
        } else if (sscanf(line, "exit %d", &status) == 1) {
            break;
        } else if (line[0] != '\0') {
            status = 127;
            break;
        }
    }
    free(script);
    return status;
}

/* A boot of the machine, as QEMU's arguments give it. */
struct machine {
    char ids[16][32], files[16][MAX_PATH * 2];
    size_t n; /* drives */
    const char *seed, *job, *console, *chan, *disk1;
    int ndisks;
};

/** Keep QEMU's arguments in FIX/qemu.args, one to a line. */
static int keep_args(int argc, char **argv)
{
    char path[MAX_PATH * 2 + 32];
    FILE *f;
    int i;

    snprintf(path, sizeof path, "%s\\qemu.args", fix);
    if ((f = fopen(path, "wb")) == NULL)
        return -1;
    for (i = 1; i < argc; i++)
        fprintf(f, "%s\n", argv[i]);
    return fclose(f) != 0 ? -1 : 0;
}

/** Take the argument `a` of `-device virtio-blk-pci`; -1 for no such drive. */
static int device(struct machine *m, const char *a)
{
    const char *id = strstr(a, "drive="), *s = strstr(a, "serial=");
    size_t k, len;

    if (id == NULL)
        return -1;
    id += 6;
    len = strcspn(id, ",");
    for (k = 0; k < m->n; k++)
        if (strncmp(m->ids[k], id, len) == 0 && m->ids[k][len] == '\0')
            break;
    if (k == m->n)
        return -1;
    if (s != NULL && strncmp(s + 7, "etude-job", 9) == 0) {
        m->job = m->files[k];
    } else if (s != NULL && strncmp(s + 7, "etude-d", 7) == 0) {
        m->ndisks++;
        if (m->disk1 == NULL)
            m->disk1 = m->files[k];
    } else if (s == NULL && strcmp(m->ids[k], "root") != 0) {
        m->seed = m->files[k];
    }
    return 0;
}

/**
 * Read QEMU's arguments as tooling.c makes them: each `-drive` with its id
 * and file, each `-device virtio-blk-pci` with its drive and serial, and two
 * `-serial file:`, the console and the channel.
 */
static int parse(int argc, char **argv, struct machine *m)
{
    const char *id, *f;
    int i;

    for (i = 1; i + 1 < argc; i++) {
        const char *a = argv[i + 1];

        if (strcmp(argv[i], "-drive") == 0 && m->n < 16 && (id = strstr(a, "id=")) != NULL &&
            (f = strstr(a, "file=")) != NULL) {
            snprintf(m->ids[m->n], sizeof m->ids[0], "%.*s", (int)strcspn(id + 3, ","), id + 3);
            snprintf(m->files[m->n], sizeof m->files[0], "%.*s", (int)strcspn(f + 5, ","), f + 5);
            m->n++;
        } else if (strcmp(argv[i], "-device") == 0 && strncmp(a, "virtio-blk-pci,", 15) == 0 &&
                   device(m, a) != 0) {
            return -1;
        } else if (strcmp(argv[i], "-serial") == 0 && strncmp(a, "file:", 5) == 0) {
            *(m->console == NULL ? &m->console : &m->chan) = a + 5;
        }
    }
    return m->console != NULL && m->chan != NULL ? 0 : -1;
}

/** Answer a boot with the seed volume as provision.sh does. */
static int provision(const struct machine *m)
{
    size_t size, i;
    char *seed = slurp(m->seed, &size);
    int found = 0;

    for (i = 0; seed != NULL && i + 12 <= size && !found; i++)
        found = memcmp(seed + i, "provision.sh", 12) == 0;
    free(seed);
    if (!found)
        return 1;
    if (strcmp(fix_word("provision"), "fail") == 0) {
        say(m->chan, "etude-tooling: provision: failed: apt-get install\n");
    } else {
        say(m->chan, "etude-tooling: tool unshare /usr/bin/unshare\n");
        say(m->chan, "etude-tooling: provision: ok\n");
    }
    return 0;
}

/**
 * Write the job's results over volume `img` as runner.sh does: out/ with each
 * output that the volume's list in `vol` names, then stdout, stderr and the
 * status `line`. A failure leaves its reason in `why`.
 */
static int results(const char *img, const char *vol, const struct text *out, const struct text *err,
                   const char *line)
{
    char path[MAX_PATH * 6], from[MAX_PATH * 6], name[NAME_MAX + 8], *list, *l;
    struct etude_volume *v;
    long long size;
    FILE *f;

    snprintf(why, sizeof why, "it cannot be read");
    if ((f = fopen(img, "rb")) == NULL || _fseeki64(f, 0, SEEK_END) != 0 ||
        (size = _ftelli64(f)) <= 0) {
        if (f != NULL)
            fclose(f);
        return -1;
    }
    fclose(f);
    snprintf(path, sizeof path, "%s\\outputs", vol);
    if ((list = slurp(path, NULL)) == NULL ||
        (v = etude_volume_create(img, (uint64_t)size, why, sizeof why)) == NULL) {
        free(list);
        return -1;
    }
    etude_volume_dir(v, "out");
    for (l = list; *l != '\0';) {
        int k = (int)strcspn(l, "\n");

        if (k > 0 && k < NAME_MAX) {
            snprintf(from, sizeof from, "%s\\in\\%.*s", vol, k, l);
            snprintf(name, sizeof name, "out/%.*s", k, l);
            etude_volume_add(v, name, from);
        }
        l += k + (l[k] == '\n');
    }
    free(list);
    etude_volume_text(v, "stdout", out->data, out->len);
    etude_volume_text(v, "stderr", err->data, err->len);
    etude_volume_text(v, "status", line, strlen(line));
    return etude_volume_close(v);
}

/** Answer a boot with the job volume as runner.sh does, as FIX/qemu says. */
static int run(const struct machine *m)
{
    static struct text out, err;
    const char *how = fix_word("qemu");
    char line[64], path[MAX_PATH * 2 + 16], vol[MAX_PATH * 2 + 32];
    char script[sizeof vol + 8], outputs[sizeof vol + 8], in[sizeof vol + 8];
    const char *map[] = {"job.sh", script, "outputs", outputs, "in", in, NULL};
    int status;

    snprintf(vol, sizeof vol, "%s\\vol%lu", fix, (unsigned long)GetCurrentProcessId());
    snprintf(script, sizeof script, "%s\\job.sh", vol);
    snprintf(outputs, sizeof outputs, "%s\\outputs", vol);
    snprintf(in, sizeof in, "%s\\in", vol);
    if (_mkdir(vol) != 0 || etude_volume_read(m->job, map, why, sizeof why) != 0) {
        fprintf(stderr, "winstand: the job volume %s: %s\n", m->job, why);
        return 1;
    }
    say(m->chan, "etude-job: start\n");
    if (strcmp(how, "crash") == 0)
        return 1;
    if (strcmp(how, "hang") == 0) {
        Sleep(30000);
        snprintf(path, sizeof path, "%s\\woke", fix);
        spill(path, "wb", "", 0);
        return 0;
    }
    if (strcmp(how, "slow") == 0)
        slow();
    status = run_job(vol, m->disk1, m->ndisks, &out, &err);
    snprintf(line, sizeof line, "etude-job: ran %d\n", status);
    say(m->chan, line);
    snprintf(line, sizeof line, "%d\n", strcmp(how, "lie") == 0 ? status + 1 : status);
    if (results(m->job, vol, &out, &err, line) != 0) {
        fprintf(stderr, "winstand: the job volume %s: %s\n", m->job, why);
        return 1;
    }
    snprintf(line, sizeof line, "etude-job: end %d\n", status);
    say(m->chan, line);
    return 0;
}

/** The machine: a provisioning boot or a job's, by its disks. */
static int qemu(int argc, char **argv)
{
    static struct machine m;

    if (keep_args(argc, argv) != 0 || parse(argc, argv, &m) != 0 ||
        spill(m.console, "wb", "", 0) != 0 || spill(m.chan, "wb", "", 0) != 0)
        return 1;
    if (m.seed != NULL)
        return provision(&m);
    return m.job != NULL ? run(&m) : 1;
}

/** curl -fsSL --retry 3 -o OUT URL: copy FIX/image to OUT. */
static int curl(int argc, char **argv)
{
    char path[MAX_PATH * 2 + 16], *d;
    size_t n;
    int rc;

    snprintf(path, sizeof path, "%s\\image", fix);
    if (argc != 7 || strcmp(argv[4], "-o") != 0 || (d = slurp(path, &n)) == NULL)
        return 2;
    rc = spill(argv[5], "wb", d, n) != 0;
    free(d);
    return rc;
}

/** qemu-img create -q -f qcow2 -b BASE -F qcow2 OUT [SIZE]: an overlay. */
static int qemu_img(int argc, char **argv)
{
    char dir[MAX_PATH * 4], base[MAX_PATH * 6], text[MAX_PATH * 4];
    size_t n;

    if (argc < 10 || strcmp(argv[1], "create") != 0 || strcmp(argv[5], "-b") != 0)
        return 2;
    snprintf(dir, sizeof dir, "%s", argv[9]);
    n = strlen(dir);
    while (n > 0 && dir[n - 1] != '/' && dir[n - 1] != '\\')
        n--;
    dir[n] = '\0';
    if (argv[6][0] == '/' || argv[6][0] == '\\' || argv[6][1] == ':')
        snprintf(base, sizeof base, "%s", argv[6]);
    else
        snprintf(base, sizeof base, "%s%s", dir, argv[6]);
    if (GetFileAttributesA(base) == INVALID_FILE_ATTRIBUTES) {
        fprintf(stderr, "qemu-img: %s is not found from %s\n", argv[6], dir);
        return 1;
    }
    snprintf(text, sizeof text, "overlay of %s\n", argv[6]);
    return spill(argv[9], "wb", text, strlen(text)) != 0;
}

int main(int argc, char **argv)
{
    char self[MAX_PATH * 2], name[MAX_PATH], *slash, *dot;
    DWORD n;

    n = GetModuleFileNameA(NULL, self, sizeof self);
    if (n == 0 || n >= sizeof self)
        return 2;
    base_name(self, name, sizeof name);
    if ((dot = strrchr(name, '.')) != NULL)
        *dot = '\0';
    if ((slash = strrchr(self, '\\')) == NULL)
        return 2;
    *slash = '\0';
    if ((slash = strrchr(self, '\\')) == NULL)
        return 2;
    *slash = '\0';
    if (snprintf(fix, sizeof fix, "%s\\fix", self) >= (int)sizeof fix)
        return 2;
    if (strcmp(name, "curl") == 0)
        return curl(argc, argv);
    if (strcmp(name, "qemu-img") == 0)
        return qemu_img(argc, argv);
    if (strcmp(name, "qemu-system-x86_64") == 0)
        return qemu(argc, argv);
    fprintf(stderr, "winstand: no stand-in %s\n", name);
    return 2;
}
