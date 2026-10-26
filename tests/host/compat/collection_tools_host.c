/* Exercise native staging through the existing tooling job boundary. */
#define _POSIX_C_SOURCE 200809L
#if defined(__APPLE__) && !defined(_DARWIN_C_SOURCE)
#define _DARWIN_C_SOURCE
#endif
#include <ctype.h>
#include <dirent.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "command_host.h"
#include "../../../tools/etude/include/etude/tooling.h"
extern char **environ;
#include "../../../utils/sha256.h"

int etude_collection_native_goal(const char *goal);
int etude_collection_tools_main(int argc, char **argv);
static char collection_root[4096], collection_self[4096];
static int collection_failures, collection_style;
/* The processors a preparation row expects, those its provisioning got, and
 * those of the last job. */
static unsigned collection_cpus, collection_provisioned, collection_job_cpus;

#define COLLECTION_CHECK(test)                                                                     \
    do {                                                                                           \
        if (!(test)) {                                                                             \
            fprintf(stderr, "collection-tools: failed at %s:%d: %s\n", __FILE__, __LINE__, #test); \
            collection_failures++;                                                                 \
        }                                                                                          \
    } while (0)

static char *collection_path(const char *name)
{
    static char paths[16][4096];
    char *path = paths[(unsigned)collection_style++ % 16];
    if (snprintf(path, 4096, "%s/%s", collection_root, name) >= 4096)
        exit(2);
    return path;
}

static void collection_put(const char *path, const char *text)
{
    FILE *file = fopen(path, "wb");
    if (!file || fputs(text, file) < 0 || fclose(file))
        exit(2);
}

static int collection_bytes(const char *path, const char *expected)
{
    FILE *file = fopen(path, "rb");
    char bytes[512];
    if (!file)
        return 0;
    size_t n = fread(bytes, 1, sizeof bytes, file);
    int rc = !ferror(file) && n == strlen(expected) && !memcmp(bytes, expected, n);
    fclose(file);
    return rc;
}

static int collection_delete(const char *path)
{
    struct stat st;
    if (lstat(path, &st))
        return errno == ENOENT ? 0 : -1;
    if (!S_ISDIR(st.st_mode))
        return unlink(path);
    DIR *dir = opendir(path);
    struct dirent *entry;
    int rc = !dir;
    while (dir && (entry = readdir(dir))) {
        if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, ".."))
            continue;
        char child[4096];
        if (snprintf(child, sizeof child, "%s/%s", path, entry->d_name) >= (int)sizeof child ||
            collection_delete(child))
            rc = 1;
    }
    if (dir)
        closedir(dir);
    return rc ? -1 : rmdir(path);
}

static int collection_copy_child(int argc, char **argv, const char *row)
{
    if (!strcmp(row, "copy-in")) {
        const char *base = argc == 5 ? strrchr(argv[3], '/') : NULL;
        return base && !strcmp(base + 1, "Busy ' Box") && !strcmp(argv[4], "::/") &&
                       collection_bytes(argv[3], "input bytes")
                   ? 0
                   : 92;
    }
    if (!strcmp(row, "partial")) {
        if (argc != 5)
            return 93;
        collection_put(argv[4], "partial");
        return 7;
    }
    if (!strcmp(row, "missing"))
        return 1;
    return 97;
}

static int collection_debug_child(int argc, char **argv, const char *disk)
{
    if (argc != 4 || strcmp(argv[1], "-R") || strcmp(argv[3], disk))
        return 96;
    /* The request has one independently quoted local destination. */
    const char *start = strstr(argv[2], "'file0/"), *end = strrchr(argv[2], '\'');
    if (!start || !end || end <= start)
        return 96;
    char path[4096];
    size_t n = (size_t)(end - start - 1);
    if (n >= sizeof path)
        return 96;
    memcpy(path, start + 1, n);
    path[n] = 0;
    collection_put(path, "dump bytes");
    return 0;
}

static int collection_disk_child(int argc, char **argv, const char *row, const char *disk)
{
    if (!strcmp(row, "offset")) {
        char expected[4096];
        snprintf(expected, sizeof expected, "%s@@1048576", disk);
        return argc == 4 && !strcmp(argv[1], "-i") && !strcmp(argv[2], expected) &&
                       !strcmp(argv[3], "::/a file")
                   ? 0
                   : 91;
    }
    if (!strcmp(row, "blocks"))
        return argc == 4 && !strcmp(argv[1], "-F") && !strcmp(argv[2], disk) &&
                       !strcmp(argv[3], "16384")
                   ? 0
                   : 94;
    if (!strcmp(row, "populate")) {
        char file[4096];
        if (argc != 6 || snprintf(file, sizeof file, "%s/file", argv[3]) >= (int)sizeof file)
            return 95;
        return !strcmp(argv[2], "-d") && collection_bytes(file, "seed") && !strcmp(argv[4], disk) &&
                       !strcmp(argv[5], "16384")
                   ? 0
                   : 95;
    }
    return 97;
}

/* The same fixture executable provides tool operands judged independently. */
int etude_test_collection_child(int argc, char **argv)
{
    const char *tool = strrchr(argv[0], '/');
    tool = tool ? tool + 1 : argv[0];
    if (!strcmp(tool, "qemu-img")) {
        if (argc != 10 || strcmp(argv[1], "convert"))
            return 98;
        collection_put(argv[9], "private workspace");
        return 0;
    }
    if (strcmp(tool, "mtype") && strcmp(tool, "mcopy") && strcmp(tool, "mke2fs") &&
        strcmp(tool, "debugfs"))
        return -1;
    const char *row = getenv("COLLECTION_TEST_ROW"), *disk = getenv("ETUDE_DISK1");
    if (!row || !disk)
        return 90;
    if (!strcmp(tool, "mcopy"))
        return collection_copy_child(argc, argv, row);
    if (!strcmp(row, "debugfs"))
        return collection_debug_child(argc, argv, disk);
    return collection_disk_child(argc, argv, row, disk);
}

/* Read one word of a translated command: single quotes, a backslash outside
 * them, and the disk variables of the tooling job inside double quotes. */
static int collection_word(const char **cursor, char *word, size_t size)
{
    const char *p = *cursor;
    size_t n = 0;
    int quote = 0;
    while (*p && (quote || *p != ' ')) {
        const char *piece = p;
        size_t length = 1;
        if (*p == quote || (!quote && (*p == '\'' || *p == '"'))) {
            quote = *p == quote ? 0 : *p;
            p++;
            continue;
        }
        if (quote != '\'' && *p == '$') {
            char name[32];
            if (quote != '"' || strncmp(p, "$ETUDE_DISK", 11))
                return -1;
            size_t digits = strspn(p + 11, "0123456789");
            if (!digits || digits > 2)
                return -1;
            snprintf(name, sizeof name, "%.*s", (int)(10 + digits), p + 1);
            if (!(piece = getenv(name)))
                return -1;
            length = strlen(piece);
            p += 11 + digits;
        } else if (!quote && *p == '\\' && p[1]) {
            piece = ++p;
            p++;
        } else if (!quote && !isalnum((unsigned char)*p) && !strchr("@:/._-+=", *p))
            return -1;
        else
            p++;
        if (n + length >= size)
            return -1;
        memcpy(word + n, piece, length);
        n += length;
    }
    if (quote)
        return -1;
    word[n] = 0;
    *cursor = *p ? p + 1 : p;
    return 0;
}

/**
 * Run the one tool command of a translated request's job script.
 *
 * The script is `set +e`, the command, and an exit with its status. The
 * command's first word names a stand-in under the fixture's `bin`, which
 * runs by absolute path with the remaining words as its arguments, in the
 * job's directory. Any other script is refused with status 2.
 */
static int collection_script(const char *run, const char *path)
{
    static char words[16][8192];
    static const char head[] = "set +e\n", tail[] = "\nstatus=$?\nexit \"$status\"\n";
    char text[16384], program[4096], *argv[17];
    FILE *file = fopen(path, "rb");
    size_t n = file ? fread(text, 1, sizeof text - 1, file) : 0, argc = 0;
    text[n] = 0;
    if (file)
        fclose(file);
    char *end = strncmp(text, head, sizeof head - 1) ? NULL : strchr(text + sizeof head - 1, '\n');
    if (!end || strcmp(end, tail))
        return 2;
    *end = 0;
    for (const char *cursor = text + sizeof head - 1; *cursor; argc++)
        if (argc == 16 || collection_word(&cursor, words[argc], sizeof words[argc]))
            return 2;
    if (!argc || !*words[0] || strchr(words[0], '/') ||
        snprintf(program, sizeof program, "%s/bin/%s", collection_root, words[0]) >=
            (int)sizeof program)
        return 2;
    argv[0] = program;
    for (size_t i = 1; i < argc; i++)
        argv[i] = words[i];
    argv[argc] = NULL;
    struct etude_exec *executor = etude_exec_new();
    struct etude_proc *process = NULL;
    struct etude_exec_spec spec = {.attempt_id = "collection-fixture-job",
                                   .argv = argv,
                                   .envp = environ,
                                   .cwd = run,
                                   .output_max = 65536,
                                   .grace_us = 1000000};
    int rc = 2;
    if (executor && !etude_exec_start(executor, &spec, &process)) {
        while (etude_exec_wait(executor, -1) != process) {
        }
        const struct etude_exec_result *result = etude_proc_result(process);
        if (result->err.kept)
            fwrite(result->err.data, 1, result->err.kept, stderr);
        if (result->end == ETUDE_END_EXITED && result->cleanup == ETUDE_CLEANUP_CONFIRMED)
            rc = result->value;
    }
    if (executor)
        etude_exec_free(executor);
    return rc;
}

static int collection_source_archive(const struct etude_tooling_job *job)
{
    const char *archive = NULL;
    int bundle = 0;
    for (size_t i = 0; job->inputs && job->inputs[i]; i++) {
        const char *base = strrchr(job->inputs[i], '/');
        if (base && !strcmp(base, "/source.tar"))
            archive = job->inputs[i];
        if (base && !strcmp(base, "/source.bundle"))
            bundle++;
    }
    char names[4096];
    const char *list[] = {host_tool_path("tar"), "-tf", archive, NULL};
    if (!archive || bundle != 1 || host_command(list, NULL, NULL, names, sizeof names) ||
        !strstr(names, "one.c\n") || !strstr(names, "ports/doom/doom1.wad\n") ||
        !strstr(names, "krffs/recovery_authorized_keys\n"))
        return 1;
    return 0;
}

static int collection_prepared_job(const struct etude_tooling_job *job)
{
    FILE *file = fopen(job->script, "rb");
    char script[16384];
    size_t n = file ? fread(script, 1, sizeof script - 1, file) : 0;
    script[n] = 0;
    if (file)
        fclose(file);
    char make[64];
    snprintf(make, sizeof make, "make -j%u ARCH='i686' 'components'", collection_cpus);
    if (!job->network || job->cpus != collection_cpus ||
        job->memory_mib != (collection_cpus > 4 ? collection_cpus * 1024 : 4096) ||
        collection_provisioned != collection_cpus || job->volume_mib < 512 || !job->disks ||
        !job->disks[0] || !job->disks[1] || job->disks[2] || !strstr(script, "rsync -rc") ||
        !strstr(script, "git --literal-pathspecs add -f") || !strstr(script, make) ||
        strstr(script, "make -B") || !strstr(script, "tar --sparse -cf"))
        return 1;
    if (collection_source_archive(job))
        return 1;
    char seed[4096];
    snprintf(seed, sizeof seed, "%s/seed", collection_root);
    const char *make[] = {
        host_tool_path("tar"), "-cf", job->disks[1], "-C", seed, "build/images/one.img",
        "build/rach2/kernel",  NULL};
    return host_command(make, NULL, NULL, NULL, 0);
}

/* Replace only the existing tooling transport, not staging or translation.
 * Only preparation may ask for a network; any other job ends with status 3. */
int etude_tooling_run(const struct etude_tooling_site *site, const struct etude_tooling_job *job,
                      struct etude_tooling_result *result)
{
    char run[4096];
    (void)site;
    memset(result, 0, sizeof(*result));
    result->outcome = ETUDE_TOOLING_ENDED;
    result->cost.cleanup = ETUDE_CLEANUP_CONFIRMED;
    snprintf(run, sizeof run, "%s/run.XXXXXX", collection_root);
    if (!mkdtemp(run))
        return -1;
    int rc = 0;
    for (size_t i = 0; job->disks && job->disks[i]; i++) {
        char name[32];
        snprintf(name, sizeof name, "ETUDE_DISK%zu", i + 1);
        if (setenv(name, job->disks[i], 1))
            rc = 2;
    }
    for (size_t i = 0; job->inputs && job->inputs[i]; i++) {
        const char *source = job->inputs[i], *base = strrchr(source, '/');
        char target[4096];
        if (!base ||
            snprintf(target, sizeof target, "%s/%s", run, base + 1) >= (int)sizeof target ||
            symlink(source, target))
            rc = 2;
    }
    collection_job_cpus = job->cpus;
    if (getenv("COLLECTION_TEST_ROW") && !strcmp(getenv("COLLECTION_TEST_ROW"), "prepare"))
        rc = collection_prepared_job(job);
    else if (job->network)
        rc = 3;
    else if (getenv("COLLECTION_TEST_ROW") && !strcmp(getenv("COLLECTION_TEST_ROW"), "style")) {
        FILE *file = fopen(job->script, "rb");
        char text[8192];
        size_t n = file ? fread(text, 1, sizeof text - 1, file) : 0;
        text[n] = 0;
        if (file)
            fclose(file);
        rc = !strstr(text, "sh tests/cases/host/style.sh 'manual'\n") ||
             !strstr(text, "make -j1 ARCH='i686' 'build/tests/style/manual.json'\n") ||
             !strstr(text, "trap 'cd /; umount /mnt/collection' EXIT");
    } else if (!rc) {
        setenv("ETUDE_WORK", run, 1);
        rc = collection_script(run, job->script);
    }
    for (size_t i = 0; job->outputs && job->outputs[i]; i++) {
        char source[4096], target[4096], resolved[4096];
        snprintf(source, sizeof source, "%s/%s", run, job->outputs[i]);
        snprintf(target, sizeof target, "%s/%s", job->outdir, job->outputs[i]);
        if (realpath(source, resolved) && rename(resolved, target))
            rc = 2;
    }
    if (collection_delete(run))
        rc = 2;
    result->status = rc;
    return 0;
}

int etude_tooling_prepare(const struct etude_tooling_site *site, unsigned cpus, uint64_t deadline,
                          struct etude_tooling_result *result)
{
    (void)site;
    (void)deadline;
    collection_provisioned = cpus;
    memset(result, 0, sizeof(*result));
    result->outcome = ETUDE_TOOLING_ENDED;
    result->cost.cleanup = ETUDE_CLEANUP_CONFIRMED;
    return 0;
}

void etude_tooling_result_free(struct etude_tooling_result *result)
{
    free(result->out);
    free(result->err);
}

static void collection_command(const char *row, const char *tool, const char *disk,
                               const char *first, const char *second, int expected)
{
    setenv("COLLECTION_TEST_ROW", row, 1);
    char *args[] = {"tool",       collection_root, (char *)tool,   "-i",
                    (char *)disk, (char *)first,   (char *)second, NULL};
    int count = second ? 7 : 6;
    COLLECTION_CHECK(etude_collection_tools_main(count, args) == expected);
    COLLECTION_CHECK(collection_job_cpus == 1);
}

static void collection_goals(void)
{
    const char *native[] = {"build/tests/cases/machine/disks_scenario",
                            "build/tests/cases/gnu/sntpserver",
                            "build/tests/style/sparse.json",
                            "build/tests/style/sizes.json",
                            "build/tests/style/manual.json",
                            "build/x86_64/tests/style/manual.json",
                            "build/tools/inspect/elfcheck",
                            "build/tests/host-suite/sanitizer/tools.json",
                            "build/tests/host-suite/scripts/flow",
                            "build/tests/host-suite/tests/usbstamp_hosttest",
                            NULL};
    const char *guest[] = {"build/tests/cases/machine/disks/one.disk",
                           "build/x86_64/images/test.krffs",
                           "build/tests/cases/gnu/rows.sh",
                           "build/tools/etude/guest/etude-guest",
                           "build/tests/style/abi.json",
                           "build/tests/host/tooling_machine",
                           "build/tests/host-suite/sanitizer/atos",
                           NULL};
    for (size_t i = 0; native[i]; i++)
        COLLECTION_CHECK(etude_collection_native_goal(native[i]));
    for (size_t i = 0; guest[i]; i++)
        COLLECTION_CHECK(!etude_collection_native_goal(guest[i]));
}

static void collection_rows(const char *disk)
{
    char offset[4096];
    snprintf(offset, sizeof offset, "%s@@1048576", disk);
    collection_command("offset", "mtype", offset, "::/a file", NULL, 0);
    char source[4096];
    snprintf(source, sizeof source, "%s/Busy ' Box", collection_root);
    collection_put(source, "input bytes");
    collection_command("copy-in", "mcopy", disk, source, "::/", 0);
    char dest[4096];
    snprintf(dest, sizeof dest, "%s/returned bytes", collection_root);
    collection_command("partial", "mcopy", disk, "::/file", dest, 7);
    COLLECTION_CHECK(collection_bytes(dest, "partial"));
    snprintf(dest, sizeof dest, "%s/absent", collection_root);
    collection_command("missing", "mcopy", disk, "::/file", dest, 1);
    COLLECTION_CHECK(access(dest, F_OK) != 0 && errno == ENOENT);
    setenv("COLLECTION_TEST_ROW", "blocks", 1);
    char *blocks[] = {"tool", collection_root, "mke2fs", "-F", (char *)disk, "16384"};
    COLLECTION_CHECK(!etude_collection_tools_main(6, blocks));
    char directory[4096], file[4096];
    snprintf(directory, sizeof directory, "%s/ext2 files", collection_root);
    COLLECTION_CHECK(!mkdir(directory, 0700));
    snprintf(file, sizeof file, "%s/ext2 files/file", collection_root);
    collection_put(file, "seed");
    setenv("COLLECTION_TEST_ROW", "populate", 1);
    char *populate[] = {"tool", collection_root, "mke2fs",     "-F",
                        "-d",   directory,       (char *)disk, "16384"};
    COLLECTION_CHECK(!etude_collection_tools_main(8, populate));
    snprintf(dest, sizeof dest, "%s/dump bytes", collection_root);
    char request[8192];
    snprintf(request, sizeof request, "dump /guest '%s'", dest);
    setenv("COLLECTION_TEST_ROW", "debugfs", 1);
    char *dump[] = {"tool", collection_root, "debugfs", "-R", request, (char *)disk};
    COLLECTION_CHECK(!etude_collection_tools_main(6, dump));
    COLLECTION_CHECK(collection_bytes(dest, "dump bytes"));
}

static void collection_style_row(void)
{
    char cache[4096], tests[4096], cases[4096], manifest[4096], disk[4096];
    snprintf(cache, sizeof cache, "%s/cache", collection_root);
    snprintf(tests, sizeof tests, "%s/tests", collection_root);
    snprintf(cases, sizeof cases, "%s/tests/cases", collection_root);
    snprintf(manifest, sizeof manifest, "%s/tests/cases/host.style.json", collection_root);
    COLLECTION_CHECK(!mkdir(cache, 0700) && !mkdir(tests, 0700) && !mkdir(cases, 0700));
    collection_put(manifest,
                   "{\"cases\":[{\"case_id\":\"host.style.manual\",\"prepare\":["
                   "{\"node\":\"tests/style/manual.json\",\"per_architecture\":false}]}]}");
    struct sha256 hash;
    unsigned char raw[32];
    char hex[65];
    sha256_init(&hash);
    sha256_update(&hash, (const unsigned char *)collection_root, strlen(collection_root));
    sha256_final(&hash, raw);
    sha256_hex(raw, hex);
    snprintf(disk, sizeof disk, "%s/cache/collection-%.16s-i686.img", collection_root, hex);
    collection_put(disk, "retained workspace");
    setenv("ETUDE_TOOLING_CACHE", cache, 1);
    setenv("COLLECTION_TEST_ROW", "style", 1);
    char *args[] = {"style", collection_root, "i686", "manual"};
    COLLECTION_CHECK(!etude_collection_tools_main(4, args));
    COLLECTION_CHECK(collection_job_cpus == 1);
    COLLECTION_CHECK(collection_bytes(disk, "retained workspace"));
    unsetenv("ETUDE_TOOLING_CACHE");
}

static int collection_git(const char *const extra[])
{
    const char *argv[32] = {host_tool_path("git"), "-C", collection_root};
    size_t n = 3;
    for (size_t i = 0; extra[i] && n + 1 < 32; i++)
        argv[n++] = extra[i];
    argv[n] = NULL;
    return host_command(argv, NULL, NULL, NULL, 0);
}

static void collection_prepare_row(void)
{
    const char *directories[] = {"ports",      "ports/doom",        "krffs",           "seed",
                                 "seed/build", "seed/build/images", "seed/build/rach2"};
    for (size_t i = 0; i < sizeof directories / sizeof(*directories); i++)
        COLLECTION_CHECK(!mkdir(collection_path(directories[i]), 0700));
    collection_put(collection_path("one.c"), "int one;\n");
    collection_put(collection_path("ports/doom/doom1.wad"), "fixture WAD");
    collection_put(collection_path("krffs/recovery_authorized_keys"), "fixture key");
    collection_put(collection_path("seed/build/images/one.img"), "image bytes");
    collection_put(collection_path("seed/build/rach2/kernel"), "kernel bytes");
    const char *init[] = {"init", "-q", NULL};
    const char *add[] = {"add", "one.c", NULL};
    const char *commit[] = {"-c",      "user.name=fixture",
                            "-c",      "user.email=fixture@example.invalid",
                            "-c",      "commit.gpgsign=false",
                            "commit",  "-qm",
                            "fixture", NULL};
    COLLECTION_CHECK(!collection_git(init));
    COLLECTION_CHECK(!collection_git(add));
    COLLECTION_CHECK(!collection_git(commit));
    setenv("COLLECTION_TEST_ROW", "prepare", 1);
    char *jobs = getenv("ETUDE_BUILD_JOBS"), *cpus = getenv("RACH2_CPUS");
    jobs = jobs ? strdup(jobs) : NULL;
    cpus = cpus ? strdup(cpus) : NULL;
    static const char *const counts[][2] = {{NULL, "3"}, {"6", "3"}};
    for (size_t i = 0; i < sizeof counts / sizeof *counts; i++) {
        if (counts[i][0])
            setenv("ETUDE_BUILD_JOBS", counts[i][0], 1);
        else
            unsetenv("ETUDE_BUILD_JOBS");
        setenv("RACH2_CPUS", counts[i][1], 1);
        collection_cpus = (unsigned)atoi(counts[i][0] ? counts[i][0] : counts[i][1]);
        collection_provisioned = 0;
        char *args[] = {"prepare", collection_root, "i686", "", "build/images/one.img"};
        COLLECTION_CHECK(!etude_collection_tools_main(5, args));
        COLLECTION_CHECK(collection_bytes(collection_path("build/images/one.img"), "image bytes"));
        COLLECTION_CHECK(collection_bytes(collection_path("build/rach2/kernel"), "kernel bytes"));
    }
    if (jobs)
        setenv("ETUDE_BUILD_JOBS", jobs, 1);
    else
        unsetenv("ETUDE_BUILD_JOBS");
    if (cpus)
        setenv("RACH2_CPUS", cpus, 1);
    else
        unsetenv("RACH2_CPUS");
    free(jobs);
    free(cpus);
}

static void collection_cleanup(void)
{
    if (*collection_root)
        collection_delete(collection_root);
}

int etude_test_collection_tools(const char *self, const char *tmp)
{
    snprintf(collection_root, sizeof collection_root, "%s/rach2-collection-test.XXXXXX", tmp);
    if (!mkdtemp(collection_root) || !realpath(self, collection_self))
        return 1;
    if (atexit(collection_cleanup))
        return 1;
    char *canonical = realpath(collection_root, NULL);
    if (!canonical || strlen(canonical) >= sizeof collection_root)
        return 1;
    strcpy(collection_root, canonical);
    free(canonical);
    char *bin = strdup(collection_path("bin"));
    COLLECTION_CHECK(bin && !mkdir(bin, 0700));
    const char *tools[] = {"mtype", "mcopy", "mke2fs", "debugfs", "qemu-img"};
    for (size_t i = 0; i < 5; i++) {
        char target[4096];
        snprintf(target, sizeof target, "%s/bin/%s", collection_root, tools[i]);
        COLLECTION_CHECK(!symlink(collection_self, target));
    }
    const char *support[] = {"git", "tar"};
    for (size_t i = 0; i < 2; i++) {
        char target[4096];
        snprintf(target, sizeof target, "%s/bin/%s", collection_root, support[i]);
        COLLECTION_CHECK(!symlink(host_tool_path(support[i]), target));
    }
    const char *old = getenv("PATH");
    char *saved = old ? strdup(old) : NULL;
    char path[16384];
    snprintf(path, sizeof path, "%s:%s", bin, old ? old : "/bin:/usr/bin");
    setenv("PATH", path, 1);
    setenv("RACH2_QEMU_ACCEL", "whpx", 1);
    char *disk = strdup(collection_path("a disk.img"));
    collection_put(disk, "disk bytes");
    collection_goals();
    collection_rows(disk);
    collection_style_row();
    collection_prepare_row();
    if (saved)
        setenv("PATH", saved, 1);
    else
        unsetenv("PATH");
    unsetenv("COLLECTION_TEST_ROW");
    unsetenv("ETUDE_WORK");
    unsetenv("ETUDE_DISK1");
    COLLECTION_CHECK(!collection_delete(collection_root));
    free(saved);
    free(disk);
    free(bin);
    if (!collection_failures)
        puts("collection-tools: native transfers passed");
    return collection_failures != 0;
}
