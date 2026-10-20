/*
 * winstand.c: the stand-ins that the tooling fixtures of winfixture put on
 * etude-tooling's PATH for curl, qemu-img, QEMU, mformat, mmd and mcopy, as
 * tests/system/etude_tooling_test.sh does with shell scripts on POSIX
 * systems. One program plays each, by the name it is run under.
 *
 * The fixture's files are in FIX, the directory `fix` beside the directory
 * of the program. `curl` copies FIX/image. `qemu-img create` writes an
 * overlay that names its backing file, which must be found from the
 * overlay's directory. The mtools stand-ins keep a volume as a list of
 * entries in one file, not as a FAT file system. QEMU keeps its arguments in
 * FIX/qemu.args and plays the machine: with the seed volume it answers as
 * provision.sh does, and with the job volume it does what runner.sh does,
 * with a job of the lines below in place of a shell script. FIX/qemu says
 * how it misbehaves: `hang`, `crash`, `lie`, or `slow`, which holds memory
 * and uses processor time in the job; FIX/provision `fail` makes
 * provision.sh fail. A machine that hangs and is not stopped writes
 * FIX/woke when its sleep ends.
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
#include <sys/stat.h>

/* The most entries a volume has, and the longest name of one. */
#define ENTRIES  256
#define NAME_MAX 512

/* The first line of a volume's file. */
#define VOLUME "winstand volume\n"

/* One file or directory of a volume, by its path in the volume. */
struct entry {
    char name[NAME_MAX];
    int dir;
    char *data;
    size_t size;
};

struct volume {
    struct entry e[ENTRIES];
    size_t n;
};

static char fix[MAX_PATH * 2];

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

/** Say whether `path` is a directory. */
static int is_dir(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0 && (st.st_mode & S_IFDIR);
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
    char buf[MAX_PATH * 4];
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

/** Find entry `name` of `v`, or NULL. */
static struct entry *find(struct volume *v, const char *name)
{
    size_t i;

    for (i = 0; i < v->n; i++)
        if (strcmp(v->e[i].name, name) == 0)
            return &v->e[i];
    return NULL;
}

/** Give entry `name` of `v`, made when it is missing; exits when it is full. */
static struct entry *entry(struct volume *v, const char *name)
{
    struct entry *e = find(v, name);

    if (e != NULL)
        return e;
    if (v->n == ENTRIES || strlen(name) >= NAME_MAX)
        exit(3);
    e = &v->e[v->n++];
    memset(e, 0, sizeof *e);
    strcpy(e->name, name);
    return e;
}

/** Put a file of `n` bytes of `data`, which `v` takes, as `name`. */
static void put_file(struct volume *v, const char *name, char *data, size_t n)
{
    struct entry *e = entry(v, name);

    free(e->data);
    e->dir = 0;
    e->data = data;
    e->size = n;
}

/** Put a file of a copy of `n` bytes of `data` as `name`. */
static void put_text(struct volume *v, const char *name, const char *data, size_t n)
{
    char *d = malloc(n + 1);

    if (d == NULL)
        exit(3);
    memcpy(d, data, n);
    d[n] = '\0';
    put_file(v, name, d, n);
}

/** Read volume `path`; exits when it is not one. */
static void load(const char *path, struct volume *v)
{
    char *d, *p, *end, name[NAME_MAX];
    size_t size, n;
    char kind;

    v->n = 0;
    if ((d = slurp(path, &size)) == NULL || strncmp(d, VOLUME, strlen(VOLUME)) != 0)
        exit(4);
    end = d + size;
    for (p = d + strlen(VOLUME); p < end;) {
        unsigned long long len = 0;

        if (sscanf(p, "%c %511s %llu", &kind, name, &len) < 2 || (p = strchr(p, '\n')) == NULL)
            exit(4);
        p++;
        if (kind == 'd') {
            entry(v, name)->dir = 1;
            continue;
        }
        n = (size_t)len;
        if (kind != 'f' || (size_t)(end - p) < n + 1)
            exit(4);
        put_text(v, name, p, n);
        p += n + 1;
    }
    free(d);
}

/** Write volume `v` to `path`; exits when it cannot. */
static void save(const char *path, const struct volume *v)
{
    FILE *f = fopen(path, "wb");
    size_t i;

    if (f == NULL)
        exit(5);
    fputs(VOLUME, f);
    for (i = 0; i < v->n; i++) {
        if (v->e[i].dir) {
            fprintf(f, "d %s\n", v->e[i].name);
            continue;
        }
        fprintf(f, "f %s %llu\n", v->e[i].name, (unsigned long long)v->e[i].size);
        fwrite(v->e[i].data, 1, v->e[i].size, f);
        fputc('\n', f);
    }
    if (fclose(f) != 0)
        exit(5);
}

/** Copy file or directory `host` into `v` as `name`. */
static void add_host(struct volume *v, const char *host, const char *name)
{
    char pattern[MAX_PATH * 4], child[MAX_PATH * 4], inner[NAME_MAX];
    WIN32_FIND_DATAA fd;
    HANDLE h;
    size_t n;
    char *data;

    if (!is_dir(host)) {
        if ((data = slurp(host, &n)) == NULL)
            exit(6);
        put_file(v, name, data, n);
        return;
    }
    entry(v, name)->dir = 1;
    snprintf(pattern, sizeof pattern, "%s\\*", host);
    if ((h = FindFirstFileA(pattern, &fd)) == INVALID_HANDLE_VALUE)
        return;
    do {
        if (strcmp(fd.cFileName, ".") == 0 || strcmp(fd.cFileName, "..") == 0)
            continue;
        snprintf(child, sizeof child, "%s\\%s", host, fd.cFileName);
        snprintf(inner, sizeof inner, "%s/%s", name, fd.cFileName);
        add_host(v, child, inner);
    } while (FindNextFileA(h, &fd));
    FindClose(h);
}

/** Copy entry `name` of `v`, a file or a directory, out to `host`. */
static void extract(struct volume *v, const char *name, const char *host)
{
    char path[MAX_PATH * 4];
    size_t i, n = strlen(name);
    struct entry *e = find(v, name);
    int found = e != NULL;

    if (e != NULL && !e->dir) {
        make_parents(host);
        if (spill(host, "wb", e->data, e->size) != 0)
            exit(7);
        return;
    }
    _mkdir(host);
    for (i = 0; i < v->n; i++) {
        if (strncmp(v->e[i].name, name, n) != 0 || v->e[i].name[n] != '/')
            continue;
        found = 1;
        if (snprintf(path, sizeof path, "%s\\%s", host, v->e[i].name + n + 1) >= (int)sizeof path)
            exit(7);
        if (v->e[i].dir) {
            make_parents(path);
            _mkdir(path);
        } else {
            make_parents(path);
            if (spill(path, "wb", v->e[i].data, v->e[i].size) != 0)
                exit(7);
        }
    }
    if (!found)
        exit(7);
}

/** Copy every entry under `from` in `v` to the same place under `to`. */
static void copy_tree(struct volume *v, const char *from, const char *to)
{
    char name[NAME_MAX * 2];
    size_t i, n = strlen(from), count = v->n;

    for (i = 0; i < count; i++) {
        const char *rest = v->e[i].name + n;

        if (strncmp(v->e[i].name, from, n) != 0 || (*rest != '\0' && *rest != '/'))
            continue;
        snprintf(name, sizeof name, "%s%s", to, rest);
        if (v->e[i].dir) {
            entry(v, name)->dir = 1;
            continue;
        }
        put_text(v, name, v->e[i].data, v->e[i].size);
    }
}

/**
 * The mtools: `mformat -C -i IMG ... ::`, `mmd -i IMG ::DIR...` and
 * `mcopy [-s] [-n] -i IMG FROM TO`.
 */
static int mtools(const char *tool, int argc, char **argv)
{
    struct volume *v = calloc(1, sizeof *v);
    const char *img = NULL;
    char name[NAME_MAX], target[MAX_PATH * 4];
    int i;

    for (i = 1; i < argc && argv[i][0] == '-'; i++)
        if (strcmp(argv[i], "-i") == 0 && i + 1 < argc)
            img = argv[++i];
        else if (strcmp(argv[i], "-T") == 0 || strcmp(argv[i], "-v") == 0)
            i++;
    if (v == NULL || img == NULL)
        return 2;
    if (strcmp(tool, "mformat") == 0)
        return spill(img, "wb", VOLUME, strlen(VOLUME)) != 0;
    load(img, v);
    if (strcmp(tool, "mmd") == 0) {
        for (; i < argc; i++)
            entry(v, argv[i] + 2)->dir = 1;
    } else if (argc - i != 2) {
        return 2;
    } else if (strncmp(argv[i], "::", 2) == 0) {
        base_name(argv[i], name, sizeof name);
        if (is_dir(argv[i + 1]))
            snprintf(target, sizeof target, "%s\\%s", argv[i + 1], name);
        else
            snprintf(target, sizeof target, "%s", argv[i + 1]);
        extract(v, argv[i] + 2, target);
        return 0;
    } else {
        const char *to = argv[i + 1] + 2;
        size_t n = strlen(to);

        base_name(argv[i], name, sizeof name);
        if (n == 0 || to[n - 1] == '/')
            snprintf(target, sizeof target, "%s%s", to, name);
        else
            snprintf(target, sizeof target, "%s", to);
        add_host(v, argv[i], target);
    }
    save(img, v);
    return 0;
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
 * Run the job of volume `v` with its working files under `in/`, as runner.sh
 * runs job.sh in its working directory; give its status.
 */
static int run_job(struct volume *v, const char *disk, int ndisks, struct text *out,
                   struct text *err)
{
    struct entry *script = find(v, "job.sh");
    char line[1024], name[NAME_MAX], path[NAME_MAX + 8], *data;
    struct entry *e;
    const char *p;
    int status;

    if (script == NULL)
        return 127;
    for (p = script->data; *p != '\0';) {
        size_t n = strcspn(p, "\n");
        int at = 0;

        snprintf(line, sizeof line, "%.*s", (int)n, p);
        p += n + (p[n] == '\n');
        if (sscanf(line, "cat %511s", name) == 1) {
            snprintf(path, sizeof path, "in/%s", name);
            if ((e = find(v, path)) == NULL || e->dir)
                return 1;
            add_text(out, e->data, e->size);
        } else if (strncmp(line, "err ", 4) == 0) {
            add_text(err, line + 4, strlen(line + 4));
            add_text(err, "\n", 1);
        } else if (strcmp(line, "disks") == 0) {
            snprintf(name, sizeof name, "disks %d\n", ndisks);
            add_text(err, name, strlen(name));
        } else if (strncmp(line, "disk ", 5) == 0 && disk != NULL) {
            if (spill(disk, "wb", line + 5, strlen(line + 5)) != 0 || spill(disk, "ab", "\n", 1))
                return 1;
        } else if (sscanf(line, "write %511s %n", name, &at) == 1 && at > 0) {
            snprintf(path, sizeof path, "in/%s", name);
            if ((data = malloc(strlen(line + at) + 2)) == NULL)
                return 1;
            sprintf(data, "%s\n", line + at);
            put_file(v, path, data, strlen(data));
        } else if (sscanf(line, "exit %d", &status) == 1) {
            return status;
        } else if (line[0] != '\0') {
            return 127;
        }
    }
    return 0;
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
    static struct volume v;

    load(m->seed, &v);
    if (find(&v, "provision.sh") == NULL)
        return 1;
    if (strcmp(fix_word("provision"), "fail") == 0) {
        say(m->chan, "etude-tooling: provision: failed: apt-get install\n");
    } else {
        say(m->chan, "etude-tooling: tool unshare /usr/bin/unshare\n");
        say(m->chan, "etude-tooling: provision: ok\n");
    }
    return 0;
}

/** Copy each output that the volume's list names from `in/` to `out/`. */
static void copy_outputs(struct volume *v)
{
    char from[NAME_MAX + 8], to[NAME_MAX + 8];
    struct entry *e = find(v, "outputs");
    const char *list = e != NULL ? e->data : "";

    while (*list != '\0') {
        int k = (int)strcspn(list, "\n");

        if (k > 0 && k < NAME_MAX) {
            snprintf(from, sizeof from, "in/%.*s", k, list);
            snprintf(to, sizeof to, "out/%.*s", k, list);
            copy_tree(v, from, to);
        }
        list += k + (list[k] == '\n');
    }
}

/** Answer a boot with the job volume as runner.sh does, as FIX/qemu says. */
static int run(const struct machine *m)
{
    static struct volume v;
    static struct text out, err;
    const char *how = fix_word("qemu");
    char line[64], path[MAX_PATH * 2 + 16];
    int status;

    load(m->job, &v);
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
    status = run_job(&v, m->disk1, m->ndisks, &out, &err);
    snprintf(line, sizeof line, "etude-job: ran %d\n", status);
    say(m->chan, line);
    copy_outputs(&v);
    snprintf(line, sizeof line, "%d\n", strcmp(how, "lie") == 0 ? status + 1 : status);
    put_text(&v, "stdout", out.data, out.len);
    put_text(&v, "stderr", err.data, err.len);
    put_text(&v, "status", line, strlen(line));
    save(m->job, &v);
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
    if (strcmp(name, "mformat") == 0 || strcmp(name, "mmd") == 0 || strcmp(name, "mcopy") == 0)
        return mtools(name, argc, argv);
    fprintf(stderr, "winstand: no stand-in %s\n", name);
    return 2;
}
