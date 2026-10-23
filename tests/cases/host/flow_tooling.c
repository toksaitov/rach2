/* flow_tooling.c: prepared tooling product with native process and disk mocks. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include "sha256.h"
#include "volume.h"
#include <dirent.h>
#include <fnmatch.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
static const char *ev(const char *n)
{
    const char *v = getenv(n);
    return v ? v : "";
}
static const char *tp(const char *name)
{
    static char p[32][4096];
    static unsigned next;
    char *v = p[next++ % 32];
    snprintf(v, 4096, "%s/%s", ev("FIX"), name);
    return v;
}
static void channel(const char *file, const char *text)
{
    FILE *f = fopen(file, "ab");
    if (f) {
        fputs(text, f);
        fclose(f);
    }
}
static int contains(const char *file, const char *needle)
{
    FILE *f = fopen(file, "rb");
    size_t n = 0, len = strlen(needle);
    int c, held = 0;
    if (!f)
        return 0;
    while ((c = fgetc(f)) != EOF) {
        if (c == (unsigned char)needle[n])
            n++;
        else
            n = c == (unsigned char)needle[0] ? 1 : 0;
        if (n == len) {
            held = 1;
            break;
        }
    }
    fclose(f);
    return held;
}
struct drive {
    char id[80], file[4096];
};
struct qemu {
    struct drive drives[20];
    unsigned count;
    char console[4096], channel[4096], seed[4096], job[4096];
    unsigned serials;
};

static void option(const char *text, const char *key, char *out, size_t size)
{
    const char *at = strstr(text, key);
    size_t n;
    if (!at) {
        *out = 0;
        return;
    }
    at += strlen(key);
    n = strcspn(at, ",");
    if (n >= size)
        n = size - 1;
    memcpy(out, at, n);
    out[n] = 0;
}
static int qemu_devices(struct qemu *q, int argc, char **argv)
{
    int i;
    for (i = 1; i < argc; i++)
        if (!strcmp(argv[i], "-device") && i + 1 < argc &&
            !strncmp(argv[i + 1], "virtio-blk-pci,", 15)) {
            char id[80], serial[80];
            unsigned j;
            option(argv[++i], "drive=", id, sizeof(id));
            option(argv[i], "serial=", serial, sizeof(serial));
            for (j = 0; j < q->count && strcmp(q->drives[j].id, id); j++) {
            }
            if (j == q->count)
                return 0;
            if (!strcmp(serial, "etude-job"))
                snprintf(q->job, sizeof(q->job), "%s", q->drives[j].file);
            else if (!strncmp(serial, "etude-d", 7)) {
                char key[100];
                unsigned n = (unsigned)atoi(serial + 7);
                snprintf(key, sizeof(key), "ETUDE_DISK%u", n);
                fx_env(key, q->drives[j].file);
                if (n > q->serials)
                    q->serials = n;
            } else if (!*serial && strcmp(id, "root"))
                snprintf(q->seed, sizeof(q->seed), "%s", q->drives[j].file);
        }
    return 1;
}
static int qemu_arguments(struct qemu *q, int argc, char **argv)
{
    FILE *log = fopen(tp("qemu.args"), "wb");
    int i;
    memset(q, 0, sizeof(*q));
    if (!log)
        return 0;
    const char *name = strrchr(argv[0], '/');
    fprintf(log, "%s\n", name ? name + 1 : argv[0]);
    for (i = 1; i < argc; i++)
        fprintf(log, "%s\n", argv[i]);
    fclose(log);
    for (i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-drive") && i + 1 < argc) {
            if (q->count == 20)
                return 0;
            option(argv[++i], "id=", q->drives[q->count].id, sizeof(q->drives[0].id));
            option(argv[i], "file=", q->drives[q->count].file, sizeof(q->drives[0].file));
            q->count++;
        } else if (!strcmp(argv[i], "-serial") && i + 1 < argc) {
            const char *file = argv[++i];
            if (strncmp(file, "file:", 5))
                return 0;
            snprintf(*q->console ? q->channel : q->console, 4096, "%s", file + 5);
        }
    }
    if (!qemu_devices(q, argc, argv))
        return 0;
    return *q->console && *q->channel && fx_text(q->console, "") && fx_text(q->channel, "");
}
static int seed(struct qemu *q)
{
    if (!contains(q->seed, "provision.sh: make the tooling machine"))
        return 1;
    if (fx_has(tp("provision"), "fail"))
        channel(q->channel, "etude-tooling: provision: failed: apt-get install\n");
    else
        channel(q->channel,
                "etude-tooling: tool unshare /usr/bin/unshare\netude-tooling: provision: ok\n");
    return 0;
}
static int job_input(struct qemu *q)
{
    char why[1000];
    const char *map[] = {"in",      tp("vol/in"),      "job.sh", tp("vol/job.sh"),
                         "outputs", tp("vol/outputs"), NULL};
    fx_remove(tp("vol"));
    fx_remove(tp("st"));
    if (!fx_mkdir(tp("st/out")))
        return 0;
    if (etude_volume_read(q->job, map, why, sizeof(why))) {
        fprintf(stderr, "fixture: volume read: %s\n", why);
        return 0;
    }
    return fx_exists(tp("vol/job.sh")) && fx_exists(tp("vol/outputs")) && fx_exists(tp("vol/in"));
}
static int job_execute(struct qemu *q)
{
    struct fixture local;
    struct fx_child p;
    char number[20];
    int rc;
    const char *a[] = {"/bin/sh", tp("vol/job.sh"), NULL};
    memset(&local, 0, sizeof(local));
    snprintf(local.work, sizeof(local.work), "%s/mock-command.%ld", ev("FLOW_WORK"),
             (long)getpid());
    if (!fx_mkdir(local.work))
        return -1;
    snprintf(number, sizeof(number), "%u", q->serials);
    fx_env("ETUDE_DISKS", number);
    fx_env("ETUDE_WORK", tp("vol/in"));
    if (!fx_start(&local, &p, a, tp("vol/in"), NULL, NULL))
        return -1;
    while (!fx_poll(&p))
        fx_pause();
    rc = p.status;
    if (!fx_copy(p.out, tp("st/stdout")) || !fx_copy(p.err, tp("st/stderr")))
        return -1;
    fx_destroy(&local);
    return rc;
}
static int job_outputs(struct qemu *q, int rc)
{
    char *names = fx_read(tp("vol/outputs")), *name, *save = NULL, source[4096], target[4096],
         status[20], why[1000];
    struct stat st;
    struct etude_volume *v;
    for (name = strtok_r(names, "\n", &save); name; name = strtok_r(NULL, "\n", &save)) {
        if (!*name)
            continue;
        snprintf(source, sizeof(source), "%s/%s", tp("vol/in"), name);
        snprintf(target, sizeof(target), "%s/%s", tp("st/out"), name);
        if (!fx_tree(source, target)) {
            free(names);
            return 0;
        }
    }
    free(names);
    snprintf(status, sizeof(status), "%d\n", fx_has(tp("qemu"), "lie") ? rc + 1 : rc);
    if (!fx_text(tp("st/status"), status) || stat(q->job, &st))
        return 0;
    v = etude_volume_create(q->job, (uint64_t)st.st_size, why, sizeof(why));
    if (!v)
        return 0;
    {
        const char *entries[] = {"out", "stdout", "stderr", "status"};
        unsigned i;
        for (i = 0; i < 4; i++) {
            snprintf(source, sizeof(source), "st/%s", entries[i]);
            if (etude_volume_add(v, entries[i], tp(source)))
                return etude_volume_close(v), 0;
        }
    }
    return etude_volume_close(v) == 0;
}
static int qemu_mock(int argc, char **argv)
{
    struct qemu q;
    int rc;
    char line[100];
    if (!qemu_arguments(&q, argc, argv))
        return 1;
    if (*q.seed)
        return seed(&q);
    if (!*q.job || !job_input(&q))
        return 1;
    channel(q.channel, "etude-job: start\n");
    if (fx_has(tp("qemu"), "crash"))
        return 1;
    if (fx_has(tp("qemu"), "hang")) {
        struct timespec delay = {30, 0};
        nanosleep(&delay, NULL);
        fx_text(tp("woke"), "");
        return 0;
    }
    rc = job_execute(&q);
    if (rc < 0)
        return 1;
    snprintf(line, sizeof(line), "etude-job: ran %d\n", rc);
    channel(q.channel, line);
    if (!job_outputs(&q, rc))
        return 1;
    snprintf(line, sizeof(line), "etude-job: end %d\n", rc);
    channel(q.channel, line);
    return 0;
}
int flow_tooling_mock(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    name = name ? name + 1 : argv[0];
    if (!strcmp(name, "curl")) {
        if (argc < 6)
            return 2;
        return fx_copy(tp("image"), argv[5]) ? 0 : 1;
    }
    if (!strcmp(name, "qemu-img")) {
        char base[4096], out[5000], *slash;
        if (argc < 10)
            return 2;
        if (argv[6][0] == '/')
            snprintf(base, sizeof(base), "%s", argv[6]);
        else {
            snprintf(base, sizeof(base), "%s", argv[9]);
            slash = strrchr(base, '/');
            if (slash)
                *slash = 0;
            else
                strcpy(base, ".");
            strncat(base, "/", sizeof(base) - strlen(base) - 1);
            strncat(base, argv[6], sizeof(base) - strlen(base) - 1);
        }
        if (!fx_exists(base)) {
            fprintf(stderr, "qemu-img: %s is not found from overlay\n", argv[6]);
            return 1;
        }
        snprintf(out, sizeof(out), "overlay of %s\n", argv[6]);
        return fx_text(argv[9], out) ? 0 : 1;
    }
    if (!strncmp(name, "qemu-system-", 12))
        return qemu_mock(argc, argv);
    return 97;
}
struct tooling {
    struct fixture *f;
    char recipe[4096], cache[4096];
};

static int setup(struct tooling *t, struct fixture *f)
{
    char target[4096], image[1000], hex[65];
    struct sha256 hash;
    unsigned char bytes[32];
    const char *m[] = {"curl", "qemu-img", "qemu-system-x86_64", "qemu-system-aarch64"};
    unsigned i;
    t->f = f;
    snprintf(t->recipe, sizeof(t->recipe), "%s/tree/tools/etude/tooling", f->work);
    snprintf(t->cache, sizeof(t->cache), "%s/cache", f->work);
    snprintf(target, sizeof(target), "%s/tree/tools/etude", f->work);
    if (!fx_tree(flow_source(f, "tools/etude"), target))
        return 0;
    fx_env("FIX", flow_path(f, "fix"));
    fx_env("FLOW_TOOLING", "1");
    if (!fx_text(tp("image"), "the cloud image\n"))
        return 0;
    sha256_init(&hash);
    sha256_update(&hash, (const unsigned char *)"the cloud image\n", 16);
    sha256_final(&hash, bytes);
    sha256_hex(bytes, hex);
    snprintf(
        image, sizeof(image),
        "amd64 https://images.example/fixture.img %s\narm64 https://images.example/arm.img %s\n",
        hex, hex);
    snprintf(target, sizeof(target), "%s/image", t->recipe);
    if (!fx_text(target, image))
        return 0;
    for (i = 0; i < 4; i++)
        if (!fx_mock(f, m[i]))
            return 0;
    if (!flow_put(f, "job.sh",
                  "cat note.txt\ncat dir/inner.txt\necho \"disks $ETUDE_DISKS\" >&2\nprintf "
                  "'written in the machine\\n' > \"$ETUDE_DISK1\"\nprintf 'made\\n' > "
                  "result.txt\nmkdir tree && printf 'deep\\n' > tree/leaf.txt\n") ||
        !flow_put(f, "note.txt", "the note\n") || !flow_put(f, "dir/inner.txt", "the inner file\n"))
        return 0;
    return 1;
}
static void fresh(struct tooling *t)
{
    DIR *d = opendir(t->f->work);
    struct dirent *e;
    char path[4096];
    if (d) {
        while ((e = readdir(d)))
            if (fnmatch("*.jsonl", e->d_name, 0) == 0) {
                snprintf(path, sizeof(path), "%s/%s", t->f->work, e->d_name);
                fx_remove(path);
            }
        closedir(d);
    }
    fx_remove(t->cache);
    fx_remove(flow_path(t->f, "out"));
    fx_mkdir(flow_path(t->f, "out"));
    fx_remove(tp("qemu"));
    fx_remove(tp("provision"));
    fx_remove(tp("woke"));
}
static int run(struct tooling *t, const char *const extra[])
{
    const char *a[100] = {"/host-products/etude",
                          "provision",
                          "tooling",
                          "-r",
                          t->recipe,
                          "-C",
                          t->cache,
                          "-a",
                          "tcg"};
    unsigned i, n = 9;
    for (i = 0; extra[i] && n < 99; i++)
        a[n++] = extra[i];
    a[n] = NULL;
    return fx_run(t->f, a, t->f->work, NULL, NULL);
}
static int made(struct tooling *t)
{
    const char *a[] = {"prepare", NULL};
    return run(t, a) == 0;
}
static unsigned cache_files(struct tooling *t, const char *pattern, const char *text,
                            char out[4096])
{
    DIR *d = opendir(t->cache);
    struct dirent *e;
    unsigned n = 0;
    char path[4096];
    if (!d)
        return 0;
    while ((e = readdir(d)))
        if (!fnmatch(pattern, e->d_name, 0)) {
            snprintf(path, sizeof(path), "%s/%s", t->cache, e->d_name);
            if (!text || fx_has(path, text)) {
                n++;
                if (out)
                    snprintf(out, 4096, "%s", path);
            }
        }
    closedir(d);
    return n;
}
static int history(struct tooling *t, const char *name, const char *stage, const char *outcome)
{
    char file[4096], pattern[500];
    snprintf(file, sizeof(file), "%s/%s.jsonl", t->f->work, name);
    snprintf(pattern, sizeof(pattern), "\"stage\":\"%s\".*\"outcome\":\"%s\"", stage, outcome);
    char *s = fx_read(file);
    int ok = fx_regex(s, pattern);
    free(s);
    return ok;
}
#define HIST(t, name)                                                                    \
    "-H", flow_path((t)->f, name ".jsonl"), "-j", name, "-R", "run", "-P", "plan", "-M", \
        "machine", "-B", "boot", "-b", flow_path((t)->f, "store")
static int preparation_rows(struct tooling *t)
{
    struct fixture *f = t->f;
    char disk[4096];
    struct stat st;
    int ok = 1;
    fresh(t);
    fx_mkdir(flow_path(f, "store"));
    {
        const char *a[] = {"prepare", HIST(t, "made"), NULL};
        run(t, a);
    }
    ok = fx_check(f,
                  !f->status && strstr(f->out, "etude provision: result: made ") &&
                      cache_files(t, "tooling-*.qcow2", NULL, disk) == 1 && !stat(disk, &st) &&
                      !(st.st_mode & 0222) && fx_has(disk, "overlay of image-") &&
                      cache_files(t, "tooling-*.tools", "provision: ok", NULL) == 1 &&
                      !cache_files(t, "prepare-*", NULL, NULL) &&
                      history(t, "made", "one_time_setup", "pass"),
                  "prepared read-only tooling disk and history") &&
         ok;
    {
        const char *a[] = {"check", NULL};
        ok = fx_check(f, !run(t, a), "check finds prepared tooling") && ok;
    }
    made(t);
    ok = fx_check(f, !f->status && strstr(f->out, "etude provision: result: reused "),
                  "tooling reused") &&
         ok;
    fresh(t);
    fx_text(tp("image"), "another image\n");
    made(t);
    fx_text(tp("image"), "the cloud image\n");
    ok = fx_check(f,
                  f->status == 1 &&
                      (strstr(f->out, "has the digest") || strstr(f->err, "has the digest")) &&
                      !cache_files(t, "image-*", NULL, NULL) &&
                      !cache_files(t, "tooling-*", NULL, NULL),
                  "unchecked image not published") &&
         ok;
    fresh(t);
    fx_text(tp("provision"), "fail\n");
    made(t);
    ok = fx_check(f,
                  f->status == 1 && (strstr(f->out, "provision.sh: apt-get install") ||
                                     strstr(f->err, "provision.sh: apt-get install")),
                  "provision failure reported") &&
         ok;
    {
        const char *a[] = {"check", NULL};
        run(t, a);
        ok = fx_check(f, f->status && !cache_files(t, "tooling-*", NULL, NULL),
                      "failed provision publishes nothing") &&
             ok;
    }
    fresh(t);
    {
        const char *a[] = {"run", flow_path(f, "job.sh"), NULL};
        run(t, a);
    }
    return fx_check(f, f->status == 126 && strstr(f->err, "is not made"),
                    "unmade tooling refuses job") &&
           ok;
}
static int job_rows(struct tooling *t)
{
    struct fixture *f = t->f;
    int ok = 1;
    fresh(t);
    if (!made(t) || !flow_put(f, "disk.img", "the disk before\n"))
        return 0;
    {
        const char *a[] = {"run",
                           "-i",
                           flow_path(f, "note.txt"),
                           "-i",
                           flow_path(f, "dir"),
                           "-d",
                           flow_path(f, "disk.img"),
                           "-o",
                           "result.txt",
                           "-o",
                           "tree",
                           "-O",
                           flow_path(f, "out"),
                           HIST(t, "job"),
                           flow_path(f, "job.sh"),
                           NULL};
        run(t, a);
    }
    ok = fx_check(f,
                  !f->status && !strcmp(f->out, "the note\nthe inner file\n") &&
                      strstr(f->err, "disks 1\n") &&
                      strstr(f->err, "etude provision: cost: start ") &&
                      fx_equal(flow_path(f, "disk.img"), "written in the machine\n") &&
                      fx_equal(flow_path(f, "out/result.txt"), "made\n") &&
                      fx_equal(flow_path(f, "out/tree/leaf.txt"), "deep\n") &&
                      !cache_files(t, "job-*", NULL, NULL) &&
                      history(t, "job", "tooling_start", "pass") &&
                      history(t, "job", "execute", "pass") && history(t, "job", "cleanup", "pass"),
                  "job inputs, disks, outputs and history") &&
         ok;
    fresh(t);
    if (!made(t) || !flow_put(f, "three.sh", "echo failing >&2\nexit 3\n"))
        return 0;
    {
        const char *a[] = {"run", HIST(t, "status"), flow_path(f, "three.sh"), NULL};
        run(t, a);
    }
    return fx_check(f,
                    f->status == 3 && strstr(f->err, "failing\n") &&
                        history(t, "status", "execute", "failure") &&
                        history(t, "status", "cleanup", "pass"),
                    "failed job status and cleanup history") &&
           ok;
}
static int late_stop_rows(struct tooling *t)
{
    struct fixture *f = t->f;
    struct fx_child p;
    char channel_file[4096];
    int ok = 1;
    fresh(t);
    if (!made(t))
        return 0;
    fx_text(tp("qemu"), "hang\n");
    {
        const char *a[] = {"run", "-t", "2", "-k", HIST(t, "late"), flow_path(f, "job.sh"), NULL};
        run(t, a);
    }
    ok = fx_check(f,
                  f->status == 124 &&
                      strstr(f->err, "result: late: the deadline passed before the job ended") &&
                      !fx_exists(tp("woke")) && history(t, "late", "execute", "incomplete") &&
                      cache_files(t, "job-*", NULL, NULL) > 0,
                  "late job stops QEMU and keeps evidence") &&
         ok;
    fresh(t);
    if (!made(t))
        return 0;
    fx_text(tp("qemu"), "hang\n");
    {
        const char *a[] = {"/host-products/etude",
                           "provision",
                           "tooling",
                           "-r",
                           t->recipe,
                           "-C",
                           t->cache,
                           "-a",
                           "tcg",
                           "run",
                           HIST(t, "stop"),
                           flow_path(f, "job.sh"),
                           NULL};
        if (!fx_start(f, &p, a, f->work, NULL, NULL))
            return 0;
    }
    for (;;) {
        char job[4096];
        if (cache_files(t, "job-*", NULL, job)) {
            snprintf(channel_file, sizeof(channel_file), "%s/channel.log", job);
            if (fx_has(channel_file, "etude-job: start\n"))
                break;
        }
        if (fx_poll(&p))
            return 0;
        fx_pause();
    }
    kill(p.pid, SIGTERM);
    fx_wait(f, &p);
    return fx_check(f,
                    f->status == 143 &&
                        strstr(f->err, "result: stopped: a signal stopped the job") &&
                        !fx_exists(tp("woke")) && !cache_files(t, "job-*", NULL, NULL) &&
                        history(t, "stop", "execute", "cancelled"),
                    "cancelled job stops QEMU and removes scratch") &&
           ok;
}
static int crash_lie_rows(struct tooling *t)
{
    struct fixture *f = t->f;
    unsigned i;
    int ok = 1;
    for (i = 0; i < 2; i++) {
        fresh(t);
        if (!made(t))
            return 0;
        fx_text(tp("qemu"), i ? "lie\n" : "crash\n");
        const char *a[] = {"run", flow_path(f, "job.sh"), NULL};
        run(t, a);
        ok = fx_check(f,
                      f->status == 125 &&
                          strstr(f->err, i ? "status is not the one the runner reported"
                                           : "QEMU exited with status 1"),
                      i ? "runner status disagreement" : "QEMU crash") &&
             ok;
    }
    return ok;
}
static int argument(const char *file, const char *word)
{
    char *s = fx_read(file), *line, *save = NULL;
    int held = 0;
    for (line = strtok_r(s, "\n", &save); line; line = strtok_r(NULL, "\n", &save))
        if (!strcmp(line, word))
            held = 1;
    free(s);
    return held;
}
static int command_rows(struct tooling *t)
{
    struct fixture *f = t->f;
    const char *want[] = {"-nodefaults",
                          "-no-reboot",
                          "tcg",
                          "3",
                          "1024",
                          "user,model=virtio-net-pci",
                          "virtio-rng-pci",
                          "virtio-blk-pci,drive=root,bootindex=0",
                          "virtio-blk-pci,drive=job,serial=etude-job",
                          "virtio-blk-pci,drive=d1,serial=etude-d1",
                          "qemu-system-x86_64"};
    unsigned i;
    int ok = 1;
    fresh(t);
    if (!made(t) || !flow_put(f, "d.img", "x\n"))
        return 0;
    {
        const char *a[] = {"run",
                           "-n",
                           "-c",
                           "3",
                           "-m",
                           "1024",
                           "-d",
                           flow_path(f, "d.img"),
                           flow_path(f, "job.sh"),
                           NULL};
        run(t, a);
    }
    for (i = 0; i < 11; i++)
        ok = fx_check(f, argument(tp("qemu.args"), want[i]), want[i]) && ok;
    ok = fx_check(f, !argument(tp("qemu.args"), "virt"), "amd64 default PC") && ok;
    {
        const char *a[] = {"run", flow_path(f, "job.sh"), NULL};
        run(t, a);
    }
    return fx_check(f, argument(tp("qemu.args"), "none"), "default job has no network") && ok;
}
static int arm_rows(struct tooling *t)
{
    struct fixture *f = t->f;
    const char *want[] = {"qemu-system-aarch64",
                          "virt",
                          "edk2-aarch64-code.fd",
                          "hvf",
                          "max",
                          "virtio-blk-pci,drive=root,bootindex=0"};
    unsigned i;
    int ok = 1;
    fresh(t);
    if (!made(t))
        return 0;
    {
        const char *a[] = {"-p", "arm64", "-a", "hvf", "check", NULL};
        run(t, a);
    }
    ok = fx_check(f, f->status != 0, "arm64 cache differs from amd64") && ok;
    {
        const char *a[] = {"-p", "arm64", "-a", "hvf", "prepare", NULL};
        run(t, a);
    }
    ok = fx_check(f, !f->status && cache_files(t, "tooling-*.qcow2", NULL, NULL) == 2,
                  "two platform images published") &&
         ok;
    {
        const char *a[] = {"-p", "arm64", "-a", "hvf", "run", flow_path(f, "job.sh"), NULL};
        run(t, a);
    }
    ok = fx_check(f, !f->status, "arm64 job") && ok;
    for (i = 0; i < 6; i++)
        ok = fx_check(f, argument(tp("qemu.args"), want[i]), want[i]) && ok;
    {
        const char *a[] = {"-p", "sparc", "check", NULL};
        run(t, a);
    }
    return fx_check(f,
                    f->status && (strstr(f->out, "not amd64 or arm64") ||
                                  strstr(f->err, "not amd64 or arm64")),
                    "unknown architecture refused") &&
           ok;
}
int flow_tooling(struct fixture *f)
{
    struct tooling t;
    int ok = 1;
    if (!setup(&t, f))
        return 0;
    ok = preparation_rows(&t) && ok;
    ok = job_rows(&t) && ok;
    ok = late_stop_rows(&t) && ok;
    ok = crash_lie_rows(&t) && ok;
    ok = command_rows(&t) && ok;
    ok = arm_rows(&t) && ok;
    fx_env("FLOW_TOOLING", NULL);
    return ok;
}
