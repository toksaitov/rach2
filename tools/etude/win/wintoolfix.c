/*
 * wintoolfix.c: the tooling fixtures of winfixture. Each runs etude-tooling,
 * beside winfixture.exe, through etude/exec.h, with copies of winstand.exe
 * in place of curl, qemu-img and QEMU, and judges a row of
 * tests/system/etude_tooling_test.sh by the command's status and output,
 * the cache, the stand-ins' files and the history it wrote. One more row
 * requires a job's steps to have QEMU's processor time and working set,
 * sampled through its job.
 */
#include <direct.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "etude/exec.h"
#include "etude/tooling.h"
#include "sha256.h"
#include "winexec.h"
#include "winfixture.h"

/* The end of the arguments of tooling(), a null pointer of their type. */
#define END ((char *)NULL)

/* The longest path a fixture makes. */
#define PATH 2048

/* The programs winstand.exe stands in for. */
static const char *const stand_ins[] = {"curl", "qemu-img", "qemu-system-x86_64"};

/* The fixture's directories under scratch\tooling, and its files. */
static char base[PATH], bin[PATH], fix[PATH], cache[PATH], recipe[PATH], outdir[PATH], store[PATH],
    schema[PATH], tool[PATH];

/* What one run of etude-tooling gave. */
struct ran {
    int status; /* its exit status, or -1 when it did not exit */
    char *out;
    char *err;
};

/** Give `dir\name` in a buffer of the caller's, or "" when it is too long. */
static char *join(char *buf, const char *dir, const char *name)
{
    int n = snprintf(buf, PATH, "%s\\%s", dir, name);

    if (n < 0 || n >= PATH)
        buf[0] = '\0';
    return buf;
}

/** Remove `path`, a file or a tree, read-only files among them. */
static void remove_tree(const char *path)
{
    char pattern[PATH], child[PATH];
    WIN32_FIND_DATAA fd;
    DWORD attrs = GetFileAttributesA(path);
    HANDLE h;

    if (attrs == INVALID_FILE_ATTRIBUTES)
        return;
    SetFileAttributesA(path, FILE_ATTRIBUTE_NORMAL);
    if (!(attrs & FILE_ATTRIBUTE_DIRECTORY)) {
        DeleteFileA(path);
        return;
    }
    h = FindFirstFileA(join(pattern, path, "*"), &fd);
    if (h != INVALID_HANDLE_VALUE) {
        do {
            if (strcmp(fd.cFileName, ".") != 0 && strcmp(fd.cFileName, "..") != 0)
                remove_tree(join(child, path, fd.cFileName));
        } while (FindNextFileA(h, &fd));
        FindClose(h);
    }
    RemoveDirectoryA(path);
}

/** Write `text` to `path`; 0 on success. */
static int put(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    size_t n = strlen(text);

    if (f == NULL)
        return -1;
    if (fwrite(text, 1, n, f) != n) {
        fclose(f);
        return -1;
    }
    return fclose(f) != 0 ? -1 : 0;
}

/** Give the contents of `path` with a NUL after them, or NULL. */
static char *get(const char *path)
{
    FILE *f = fopen(path, "rb");
    char *d = NULL;
    long n;

    if (f != NULL && fseek(f, 0, SEEK_END) == 0 && (n = ftell(f)) >= 0 &&
        fseek(f, 0, SEEK_SET) == 0 && (d = malloc((size_t)n + 1)) != NULL) {
        if (fread(d, 1, (size_t)n, f) == (size_t)n) {
            d[n] = '\0';
        } else {
            free(d);
            d = NULL;
        }
    }
    if (f != NULL)
        fclose(f);
    return d;
}

/** Say whether the file `path` has exactly `text`. */
static int has(const char *path, const char *text)
{
    char *d = get(path);
    int same = d != NULL && strcmp(d, text) == 0;

    free(d);
    return same;
}

/** Say whether the file `path` contains `text`. */
static int contains(const char *path, const char *text)
{
    char *d = get(path);
    int in = d != NULL && strstr(d, text) != NULL;

    free(d);
    return in;
}

/** Find a file of the cache that `glob` matches; its path, or NULL. */
static const char *cached(const char *glob)
{
    static char path[PATH];
    char pattern[PATH];
    WIN32_FIND_DATAA fd;
    HANDLE h;

    h = FindFirstFileA(join(pattern, cache, glob), &fd);
    if (h == INVALID_HANDLE_VALUE)
        return NULL;
    FindClose(h);
    return join(path, cache, fd.cFileName);
}

/**
 * Give this process's environment in UTF-8, with `bin` first on its path,
 * named `Path`, as Windows spells it, and without the entries of the current
 * directories of drives, whose names begin with `=`.
 */
static char **environment(void)
{
    wchar_t *block = GetEnvironmentStringsW(), *w;
    char **env, *u, *e;
    size_t n = 0, i = 0;

    if (block == NULL)
        return NULL;
    for (w = block; *w != L'\0'; w += wcslen(w) + 1)
        n++;
    env = calloc(n + 2, sizeof *env);
    for (w = block; env != NULL && *w != L'\0'; w += wcslen(w) + 1) {
        if (*w == L'=' || (u = etude_win_utf8(w)) == NULL)
            continue;
        if (_strnicmp(u, "PATH=", 5) == 0 && (e = malloc(strlen(u) + strlen(bin) + 2)) != NULL) {
            sprintf(e, "Path=%s;%s", bin, u + 5);
            free(u);
            u = e;
        }
        env[i++] = u;
    }
    FreeEnvironmentStringsW(block);
    return env;
}

/** Free an environment of environment(). */
static void free_environment(char **env)
{
    size_t i;

    for (i = 0; env != NULL && env[i] != NULL; i++)
        free(env[i]);
    free(env);
}

/** Give the path of history stream `stream` in a buffer of the caller's. */
static char *history(const char *stream, char *path)
{
    char file[64];

    snprintf(file, sizeof file, "%s.jsonl", stream);
    return join(path, base, file);
}

/**
 * Run etude-tooling with the fixture's recipe and cache, the command
 * `command` and the arguments after it, which end in NULL, and wait up to
 * BOUND for its end. A `stream` other than NULL gives the command the
 * history options of that stream, in base\STREAM.jsonl.
 *
 * @return 0, or 1 when it did not start or end.
 */
static int tooling(struct ran *r, const char *stream, const char *command, ...)
{
    char *argv[64] = {tool, "-r", recipe, "-C", cache, "-a", "tcg", (char *)command};
    char path[PATH];
    const struct etude_exec_result *res;
    struct etude_exec_spec spec;
    struct etude_proc *p = NULL, *done = NULL;
    struct etude_exec *ex;
    ULONGLONG t0 = GetTickCount64();
    char **env = environment();
    size_t n = 8;
    va_list ap;

    if (stream != NULL) {
        char *h[] = {"-H", history(stream, path),
                     "-j", (char *)stream,
                     "-R", "run",
                     "-P", "plan",
                     "-M", "machine",
                     "-B", "boot",
                     "-b", store,
                     "-S", schema};

        memcpy(argv + n, h, sizeof h);
        n += sizeof h / sizeof h[0];
    }
    va_start(ap, command);
    while (n + 1 < sizeof argv / sizeof argv[0] && (argv[n] = va_arg(ap, char *)) != NULL)
        n++;
    va_end(ap);
    argv[n] = NULL;
    r->status = -1;
    r->out = r->err = NULL;
    memset(&spec, 0, sizeof spec);
    spec.attempt_id = "tooling";
    spec.argv = argv;
    spec.envp = env;
    spec.output_max = 1 << 20;
    if (env == NULL || (ex = etude_exec_new()) == NULL) {
        free_environment(env);
        return failed("no executor");
    }
    if (etude_exec_start(ex, &spec, &p) != 0) {
        etude_exec_free(ex);
        free_environment(env);
        return failed("etude-tooling does not start");
    }
    while (done != p && GetTickCount64() - t0 < BOUND)
        done = etude_exec_wait(ex, 1000);
    res = etude_proc_result(p);
    if (done == p && res->end == ETUDE_END_EXITED)
        r->status = res->value;
    r->out = _strdup(res->out.data != NULL ? res->out.data : "");
    r->err = _strdup(res->err.data != NULL ? res->err.data : "");
    etude_exec_free(ex);
    free_environment(env);
    if (done != p)
        return failed("etude-tooling did not end");
    return r->out == NULL || r->err == NULL ? failed("out of memory") : 0;
}

/** Free what a run gave. */
static void ran_free(struct ran *r)
{
    free(r->out);
    free(r->err);
}

/** Say whether history `path` has a step of stage `stage` with `outcome`. */
static int step(const char *path, const char *stage, const char *outcome)
{
    char *d = get(path), *line, *end;
    char want_stage[64], want_outcome[64];
    int found = 0;

    snprintf(want_stage, sizeof want_stage, "\"stage\":\"%s\"", stage);
    snprintf(want_outcome, sizeof want_outcome, "\"outcome\":\"%s\"", outcome);
    for (line = d; line != NULL && *line != '\0' && !found; line = end) {
        end = line + strcspn(line, "\n");
        if (*end != '\0')
            *end++ = '\0';
        found = strstr(line, want_stage) != NULL && strstr(line, want_outcome) != NULL;
    }
    free(d);
    return found;
}

/** Make the fixture's files afresh: the recipe, the image and the stand-ins. */
static int fresh(void)
{
    static const char *const files[] = ETUDE_TOOLING_RECIPE;
    char path[PATH], self[PATH], line[256], hex[65];
    unsigned char digest[32];
    struct sha256 s;
    size_t i;

    join(base, scratch, "tooling");
    remove_tree(base);
    join(bin, base, "bin");
    join(fix, base, "fix");
    join(recipe, base, "recipe");
    join(outdir, base, "out");
    join(store, base, "store");
    snprintf(cache, sizeof cache, "%s\\cache\\deep", base);
    snprintf(schema, sizeof schema, "%s\\..\\schemas\\history.schema.json", scratch);
    join(tool, scratch, "etude-tooling.exe");
    if (_mkdir(base) != 0 || _mkdir(bin) != 0 || _mkdir(fix) != 0 || _mkdir(recipe) != 0 ||
        _mkdir(outdir) != 0 || _mkdir(store) != 0)
        return failed("%s cannot be made", base);
    for (i = 1; files[i] != NULL; i++)
        if (put(join(path, recipe, files[i]), files[i]) != 0)
            return failed("%s cannot be written", path);
    if (put(join(path, fix, "image"), "the cloud image\n") != 0)
        return failed("the image cannot be written");
    sha256_init(&s);
    sha256_update(&s, (const unsigned char *)"the cloud image\n", 16);
    sha256_final(&s, digest);
    sha256_hex(digest, hex);
    snprintf(line, sizeof line, "amd64 https://images.example/fixture.img %s\n", hex);
    if (put(join(path, recipe, "image"), line) != 0)
        return failed("the recipe's image cannot be written");
    join(self, scratch, "winstand.exe");
    for (i = 0; i < sizeof stand_ins / sizeof stand_ins[0]; i++) {
        snprintf(line, sizeof line, "%s.exe", stand_ins[i]);
        if (!CopyFileA(self, join(path, bin, line), FALSE))
            return failed("%s cannot be copied to %s", self, path);
    }
    return 0;
}

/** Make the machine; 0 when it was made. */
static int made(void)
{
    struct ran r;
    int rc;

    if (tooling(&r, NULL, "prepare", END) != 0)
        return 1;
    rc = r.status != 0 ? failed("the machine was not made: %s", r.out) : 0;
    ran_free(&r);
    return rc;
}

/** Write the fixture's job `text` as base\NAME, and give its path. */
static char *job(const char *name, const char *text, char *path)
{
    join(path, base, name);
    return put(path, text) == 0 ? path : NULL;
}

/** A machine made, recorded as one step, found by check and then reused. */
static int row_made(void)
{
    char path[PATH];
    struct ran r;
    const char *disk;
    int rc = 0;

    if (tooling(&r, "made", "prepare", END) != 0)
        return 1;
    disk = cached("tooling-*.qcow2");
    if (r.status != 0 || strstr(r.out, "etude-tooling: result: made ") == NULL)
        rc = failed("prepare gave %d: %s%s", r.status, r.out, r.err);
    else if (disk == NULL || !(GetFileAttributesA(disk) & FILE_ATTRIBUTE_READONLY))
        rc = failed("no read-only disk was published");
    else if (!contains(disk, "overlay of image-"))
        rc = failed("the disk does not name its image from the cache");
    else if (cached("tooling-*.tools") == NULL ||
             !contains(cached("tooling-*.tools"), "provision: ok"))
        rc = failed("no record of the tools");
    else if (cached("prepare-*") != NULL)
        rc = failed("the scratch was left");
    else if (!step(history("made", path), "one_time_setup", "pass"))
        rc = failed("no passing one_time_setup step");
    ran_free(&r);
    if (rc != 0 || tooling(&r, NULL, "check", END) != 0)
        return rc != 0 ? rc : 1;
    if (r.status != 0)
        rc = failed("check does not find it: %s", r.out);
    ran_free(&r);
    if (rc != 0 || tooling(&r, NULL, "prepare", END) != 0)
        return rc != 0 ? rc : 1;
    if (strstr(r.out, "etude-tooling: result: reused ") == NULL)
        rc = failed("not reused: %s", r.out);
    ran_free(&r);
    return rc;
}

/** An image of another digest is refused, and nothing is kept. */
static int row_digest(void)
{
    char path[PATH];
    struct ran r;
    int rc = 0;

    if (put(join(path, fix, "image"), "another image\n") != 0)
        return failed("the image cannot be written");
    if (tooling(&r, NULL, "prepare", END) != 0)
        return 1;
    if (r.status != 1 || strstr(r.out, "has the digest") == NULL)
        rc = failed("prepare gave %d: %s%s", r.status, r.out, r.err);
    else if (cached("image-*") != NULL || cached("tooling-*") != NULL)
        rc = failed("an unchecked image or a disk was kept");
    ran_free(&r);
    return rc;
}

/** A provision that fails publishes nothing. */
static int row_provision(void)
{
    char path[PATH];
    struct ran r;
    int rc = 0;

    if (put(join(path, fix, "provision"), "fail\n") != 0)
        return failed("the mode cannot be written");
    if (tooling(&r, NULL, "prepare", END) != 0)
        return 1;
    if (r.status != 1 || strstr(r.out, "provision.sh: apt-get install") == NULL)
        rc = failed("prepare gave %d: %s%s", r.status, r.out, r.err);
    else if (cached("tooling-*") != NULL)
        rc = failed("a part or a disk was left");
    ran_free(&r);
    if (rc != 0 || tooling(&r, NULL, "check", END) != 0)
        return rc != 0 ? rc : 1;
    if (r.status != 1)
        rc = failed("a failed machine was published");
    ran_free(&r);
    return rc;
}

/** A job with its inputs, a disk and its outputs, recorded as three steps. */
static int row_job(void)
{
    static const char *const stages[] = {"tooling_start", "execute", "cleanup"};
    char hist[PATH], script[PATH], note[PATH], dir[PATH], inner[PATH], disk[PATH], p[PATH];
    struct ran r;
    size_t i;
    int rc = 0;

    if (made() != 0)
        return 1;
    if (job("job.txt",
            "cat note.txt\ncat dir/inner.txt\ndisks\ndisk written in the machine\n"
            "write result.txt made\nwrite tree/leaf.txt deep\n",
            script) == NULL ||
        put(join(note, base, "note.txt"), "the note\n") != 0 || _mkdir(join(dir, base, "dir")) ||
        put(join(inner, dir, "inner.txt"), "the inner file\n") != 0 ||
        put(join(disk, base, "disk.img"), "the disk before\n") != 0)
        return failed("the job's files cannot be written");
    if (tooling(&r, "job", "run", "-i", note, "-i", dir, "-d", disk, "-o", "result.txt", "-o",
                "tree", "-O", outdir, script, END) != 0)
        return 1;
    history("job", hist);
    if (r.status != 0)
        rc = failed("status %d: %s", r.status, r.err);
    else if (strcmp(r.out, "the note\nthe inner file\n") != 0)
        rc = failed("the output is %s", r.out);
    else if (strstr(r.err, "disks 1\n") == NULL ||
             strstr(r.err, "etude-tooling: cost: start ") == NULL)
        rc = failed("the error is %s", r.err);
    else if (!has(disk, "written in the machine\n"))
        rc = failed("the disk was not written");
    else if (!has(join(p, outdir, "result.txt"), "made\n") ||
             !has(join(p, outdir, "tree\\leaf.txt"), "deep\n"))
        rc = failed("the outputs were not copied back");
    else if (cached("job-*") != NULL)
        rc = failed("the job's directory was left");
    for (i = 0; rc == 0 && i < 3; i++)
        if (!step(hist, stages[i], "pass"))
            rc = failed("no passing %s step", stages[i]);
    ran_free(&r);
    return rc;
}

/** A job that exits 3 fails its execute step and passes its cleanup. */
static int row_status(void)
{
    char hist[PATH], script[PATH];
    struct ran r;
    int rc = 0;

    if (made() != 0)
        return 1;
    if (job("three.txt", "err failing\nexit 3\n", script) == NULL)
        return failed("the job cannot be written");
    if (tooling(&r, "status", "run", script, END) != 0)
        return 1;
    history("status", hist);
    if (r.status != 3 || strstr(r.err, "failing\n") == NULL)
        rc = failed("status %d: %s", r.status, r.err);
    else if (!step(hist, "execute", "failure") || !step(hist, "cleanup", "pass"))
        rc = failed("the steps of a job that failed");
    ran_free(&r);
    return rc;
}

/** A run before the machine is made. */
static int row_unmade(void)
{
    char script[PATH];
    struct ran r;
    int rc = 0;

    if (job("job.txt", "exit 0\n", script) == NULL)
        return failed("the job cannot be written");
    if (tooling(&r, NULL, "run", script, END) != 0)
        return 1;
    if (r.status != 126 || strstr(r.err, "is not made") == NULL)
        rc = failed("status %d: %s", r.status, r.err);
    ran_free(&r);
    return rc;
}

/** A machine that hangs past the deadline is stopped, and its files kept. */
static int row_late(void)
{
    char hist[PATH], script[PATH], path[PATH];
    struct ran r;
    int rc = 0;

    if (made() != 0)
        return 1;
    if (job("job.txt", "exit 0\n", script) == NULL || put(join(path, fix, "qemu"), "hang\n") != 0)
        return failed("the job cannot be written");
    if (tooling(&r, "late", "run", "-t", "2", "-k", script, END) != 0)
        return 1;
    history("late", hist);
    if (r.status != 124 ||
        strstr(r.err, "result: late: the deadline passed before the job ended") == NULL)
        rc = failed("status %d: %s", r.status, r.err);
    else if (GetFileAttributesA(join(path, fix, "woke")) != INVALID_FILE_ATTRIBUTES)
        rc = failed("QEMU was not stopped at the deadline");
    else if (!step(hist, "execute", "incomplete"))
        rc = failed("the late job is not incomplete");
    else if (cached("job-*") == NULL)
        rc = failed("the evidence was not kept");
    ran_free(&r);
    return rc;
}

/** A machine that crashes, or whose runner's status the volume contradicts. */
static int row_fails(const char *how, const char *why)
{
    char script[PATH], path[PATH];
    struct ran r;
    int rc = 0;

    if (made() != 0)
        return 1;
    if (job("job.txt", "exit 0\n", script) == NULL || put(join(path, fix, "qemu"), how) != 0)
        return failed("the job cannot be written");
    if (tooling(&r, NULL, "run", script, END) != 0)
        return 1;
    if (r.status != 125 || strstr(r.err, why) == NULL)
        rc = failed("status %d: %s", r.status, r.err);
    ran_free(&r);
    return rc;
}

/** Say whether the file `path` has the line `line`. */
static int has_line(const char *path, const char *line)
{
    char *d = get(path), *p;
    size_t n = strlen(line);
    int found = 0;

    for (p = d; p != NULL && !found && (p = strstr(p, line)) != NULL; p++)
        found = (p == d || p[-1] == '\n') && (p[n] == '\n' || p[n] == '\0');
    free(d);
    return found;
}

/** QEMU's command line: its shape, its network and its disks, root first. */
static int row_line(void)
{
    static const char *const want[] = {
        "-nodefaults",
        "-no-reboot",
        "tcg",
        "3",
        "1024",
        "user,model=virtio-net-pci",
        "virtio-rng-pci",
        "virtio-blk-pci,drive=root,bootindex=0",
        "virtio-blk-pci,drive=job,serial=etude-job",
        "virtio-blk-pci,drive=d1,serial=etude-d1",
    };
    char script[PATH], disk[PATH], args[PATH];
    struct ran r;
    size_t i;
    int rc = 0;

    if (made() != 0)
        return 1;
    if (job("job.txt", "exit 0\n", script) == NULL || put(join(disk, base, "d.img"), "x\n") != 0)
        return failed("the job cannot be written");
    join(args, fix, "qemu.args");
    if (tooling(&r, NULL, "run", "-n", "-c", "3", "-m", "1024", "-d", disk, script, END) != 0)
        return 1;
    ran_free(&r);
    for (i = 0; rc == 0 && i < sizeof want / sizeof want[0]; i++)
        if (!has_line(args, want[i]))
            rc = failed("no argument %s", want[i]);
    if (rc != 0 || tooling(&r, NULL, "run", script, END) != 0)
        return rc != 0 ? rc : 1;
    ran_free(&r);
    if (!has_line(args, "none"))
        rc = failed("a job without -n has a network");
    return rc;
}

/**
 * Find the value of measurement `name` in the step `step_id` of history
 * `path` that has `method`; -1 when there is none.
 */
static long long measured(const char *path, const char *step_id, const char *name,
                          const char *method)
{
    char *d = get(path), *line, *end, *m;
    char want_step[64], want_name[64], want_method[96];
    long long value = -1;

    snprintf(want_step, sizeof want_step, "\"step_id\":\"%s\"", step_id);
    snprintf(want_name, sizeof want_name, "\"%s\":{\"value\":", name);
    snprintf(want_method, sizeof want_method, ",\"method\":\"%s\"", method);
    for (line = d; line != NULL && *line != '\0' && value < 0; line = end) {
        end = line + strcspn(line, "\n");
        if (*end != '\0')
            *end++ = '\0';
        if (strstr(line, want_step) == NULL || (m = strstr(line, want_name)) == NULL)
            continue;
        m += strlen(want_name);
        if (strncmp(m + strspn(m, "0123456789"), want_method, strlen(want_method)) == 0)
            value = strtoll(m, NULL, 10);
    }
    free(d);
    return value;
}

/** A slow job's step has QEMU's processor time and working set from its job. */
static int row_sample(void)
{
    char hist[PATH], script[PATH], path[PATH];
    long long cpu, memory;
    struct ran r;
    int rc = 0;

    if (made() != 0)
        return 1;
    if (job("job.txt", "exit 0\n", script) == NULL || put(join(path, fix, "qemu"), "slow\n") != 0)
        return failed("the job cannot be written");
    if (tooling(&r, "sample", "run", script, END) != 0)
        return 1;
    history("sample", hist);
    cpu = measured(hist, "tooling.job", "tree_cpu_us", "tooling-qemu-job-cpu-win32-v1");
    memory =
        measured(hist, "tooling.job", "combined_peak_bytes", "tooling-qemu-working-set-win32-v1");
    if (r.status != 0)
        rc = failed("status %d: %s", r.status, r.err);
    else if (cpu < 250000 || memory < 64ll << 20)
        rc = failed("the job's step has %lld us and %lld bytes sampled", cpu, memory);
    ran_free(&r);
    return rc;
}

/** Run `row` with the fixture's files made afresh, and remove them. */
static int fixture(int (*row)(void))
{
    int rc = fresh() != 0 ? 1 : row();

    remove_tree(base);
    return rc;
}

/** The crash row. */
static int row_crash(void)
{
    return row_fails("crash\n", "QEMU exited with status 1");
}

/** The lie row. */
static int row_lie(void)
{
    return row_fails("lie\n", "status is not the one the runner reported");
}

int fx_tooling_made(void)
{
    return fixture(row_made);
}

int fx_tooling_digest(void)
{
    return fixture(row_digest);
}

int fx_tooling_provision(void)
{
    return fixture(row_provision);
}

int fx_tooling_job(void)
{
    return fixture(row_job);
}

int fx_tooling_status(void)
{
    return fixture(row_status);
}

int fx_tooling_unmade(void)
{
    return fixture(row_unmade);
}

int fx_tooling_late(void)
{
    return fixture(row_late);
}

int fx_tooling_crash(void)
{
    return fixture(row_crash);
}

int fx_tooling_lie(void)
{
    return fixture(row_lie);
}

int fx_tooling_line(void)
{
    return fixture(row_line);
}

int fx_tooling_sample(void)
{
    return fixture(row_sample);
}
