/* Compiled programs used by the legacy set-driver regression. */
#ifdef _WIN32
#include "win/files.h"
#include "win/winfmt.h"
#endif
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "desktop_host.h"
#include "command_host.h"
#include "legacyset_fixture.h"
#include "tree_host.h"
#include "etude/artifact.h"
#include "json.h"

#define ROOM 32768

/** Create PATH and its parent directories. */
static int parents(const char *path)
{
    char copy[ROOM];
    char *at;
    if (snprintf(copy, sizeof(copy), "%s", path) >= (int)sizeof(copy))
        return -1;
    at = copy + 1;
    if (copy[0] == '/' && copy[1] == '/') {
        at = strchr(copy + 2, '/');
        at = at ? strchr(at + 1, '/') : NULL;
        if (!at)
            return -1;
        at++;
    }
    for (; *at; at++) {
        if (*at != '/' || at[-1] == ':')
            continue;
        *at = 0;
        if (mkdir(copy, 0755) != 0 && errno != EEXIST)
            return -1;
        *at = '/';
    }
    return 0;
}

int legacy_fixture_copy(const char *from, const char *to)
{
    unsigned char data[65536];
    size_t n;
    FILE *input, *output;
    int bad;
    if (parents(to) != 0 || (input = fopen(from, "rb")) == NULL)
        return -1;
    output = fopen(to, "wb");
    if (output == NULL) {
        fclose(input);
        return -1;
    }
    while ((n = fread(data, 1, sizeof(data), input)) != 0)
        if (fwrite(data, 1, n, output) != n)
            break;
    bad = ferror(input) || ferror(output);
    bad |= fclose(input) != 0;
    bad |= fclose(output) != 0;
    return bad ? -1 : chmod(to, 0755);
}

/** Write one fixture file, appending when requested. */
static int text_file(const char *path, const char *text, int append)
{
    FILE *file;
    int bad;
    if (parents(path) != 0 || (file = fopen(path, append ? "ab" : "wb")) == NULL)
        return -1;
    bad = fputs(text, file) < 0;
    bad |= fclose(file) != 0;
    return bad ? -1 : 0;
}

/** The initial files replace the fixture Makefile's recipes. */
int legacy_fixture_stage(const char *self, const char *tree, const char *guest_build,
                         const char *arch)
{
    static const char *const prefixes[] = {"build", "build/x86_64"};
    static const char *const links[][2] = {{"kernel", "rach2/kernel"},
                                           {"test.krffs", "images/test.krffs"},
                                           {"etude-guest", "tools/etude/guest/etude-guest"},
                                           {"rngprobe", "tests/rngprobe"}};
    char path[ROOM], source[ROOM];
    const char *build = !strcmp(arch, "x86_64") ? prefixes[1] : prefixes[0];
    size_t i;
    int bad = 0;
    for (i = 0; i < 2; i++) {
        snprintf(path, sizeof(path), "%s/%s/rach2/kernel", tree, prefixes[i]);
        bad |= text_file(path, "kernel\n", 0) != 0;
        snprintf(path, sizeof(path), "%s/%s/images/disk.img", tree, prefixes[i]);
        bad |= text_file(path, "disk\n", 0) != 0;
        snprintf(path, sizeof(path), "%s/%s/cases/probe", tree, prefixes[i]);
        bad |= legacy_fixture_copy(self, path) != 0;
        snprintf(path, sizeof(path), "%s/%s/cases/scenario", tree, prefixes[i]);
        bad |= legacy_fixture_copy(self, path) != 0;
    }
    snprintf(path, sizeof(path), "%s/build/tests/unused", tree);
    bad |= parents(path) != 0;
    snprintf(path, sizeof(path), "%s/cases/scenario.source", tree);
    bad |= legacy_fixture_copy(self, path) != 0;
    if (guest_build == NULL)
        return bad ? -1 : 0;
    for (i = 0; i < sizeof(links) / sizeof(links[0]); i++) {
        snprintf(source, sizeof(source), "%s/%s", guest_build, links[i][1]);
        snprintf(path, sizeof(path), "%s/%s/guest/%s", tree, build, links[i][0]);
        bad |= legacy_fixture_copy(source, path) != 0;
    }
    snprintf(source, sizeof(source), "%s/rach2/kernel", guest_build);
    snprintf(path, sizeof(path), "%s/%s/rach2-fixture/kernel", tree, build);
    bad |= legacy_fixture_copy(source, path) != 0;
    bad |= text_file(path, "fixture\n", 1) != 0;
    return bad ? -1 : 0;
}

/** Print the memory claims that the workload can actually see. */
static void claims(void)
{
    const char *ledger = getenv("FIXTURE_LEDGER");
    char path[ROOM], line[4096];
    FILE *file;
    if (ledger == NULL)
        return;
    snprintf(path, sizeof(path), "%s/claims", ledger);
    if ((file = fopen(path, "rb")) == NULL)
        return;
    while (fgets(line, sizeof(line), file))
        if (!strncmp(line, "resource memory_bytes ", 22))
            printf("ledger: %s", line);
    fclose(file);
}

static const char *value(const char *name)
{
    const char *v = getenv(name);
    return v != NULL ? v : "unset";
}

/** Print the exact argument and environment records used by the assertions. */
static void probe_head(int argc, char **argv)
{
    char cwd[ROOM], path[ROOM];
    int i;
    printf("program: %s", argv[0]);
    for (i = 1; i < argc; i++)
        printf(" %s", argv[i]);
    printf("\ncwd: %s\ntree: %s\n", realpath(".", cwd) ? cwd : "", value("ETUDE_TREE"));
    printf("case: %s arch: %s seed: %s inherited: %s\n", value("ETUDE_CASE"), value("ARCH"),
           value("ETUDE_SEED"), value("FIXTURE_INHERITED"));
    printf("tmpdir: %s\n", value("TMPDIR"));
    snprintf(path, sizeof(path), "%s/made", value("TMPDIR"));
    text_file(path, "made\n", 0);
    printf("@etude 1 begin %s\n@etude 1 progress ticks 1\n", value("ETUDE_CASE"));
}

/** Meet a concurrently running case using two files of this fixture. */
static int meet(int argc, char **argv)
{
    char path[ROOM], text[256];
    if (argc < 4)
        return 121;
    snprintf(path, sizeof(path), "%s/meet/%s.here", value("ETUDE_TREE"), argv[2]);
    if (text_file(path, "here\n", 0) != 0)
        return 121;
    snprintf(path, sizeof(path), "%s/meet/%s.here", value("ETUDE_TREE"), argv[3]);
    while (access(path, F_OK) != 0)
        host_sleep_ms(100); /* interval: recheck the other case's start witness */
    snprintf(path, sizeof(path), "%s/meet/log", value("ETUDE_TREE"));
    snprintf(text, sizeof(text), "%s met\n", argv[2]);
    return text_file(path, text, 1) == 0 ? 0 : 121;
}

static int mark(int argc, char **argv)
{
    char path[ROOM], text[256];
    if (argc < 4)
        return 121;
    snprintf(path, sizeof(path), "%s/marks", value("ETUDE_TREE"));
    snprintf(text, sizeof(text), "start %s\n", argv[2]);
    if (text_file(path, text, 1) != 0)
        return 121;
    host_sleep_ms((unsigned long)(strtod(argv[3], NULL) * 1000)); /* injected_delay */
    snprintf(text, sizeof(text), "end %s\n", argv[2]);
    return text_file(path, text, 1) == 0 ? 0 : 121;
}

static int probe_role(int argc, char **argv)
{
    const char *mode = argc > 1 ? argv[1] : "pass";
    char path[ROOM];
    int i, last;
    setvbuf(stdout, NULL, _IONBF, 0);
    probe_head(argc, argv);
    if (!strcmp(mode, "long") || !strcmp(mode, "hold")) {
        puts(!strcmp(mode, "hold") ? "holding" : "long running");
        if (!strcmp(mode, "hold")) {
            snprintf(path, sizeof(path), "%s/started", value("ETUDE_TREE"));
            if (text_file(path, "started\n", 0) != 0)
                return 121;
        }
        host_wait_forever();
    }
    if (!strcmp(mode, "meet") && meet(argc, argv) != 0)
        return 121;
    if (!strcmp(mode, "mark") && mark(argc, argv) != 0)
        return 121;
    if (!strcmp(mode, "tick") || !strcmp(mode, "slow")) {
        last = !strcmp(mode, "tick") ? 16 : 40;
        for (i = 2; i <= last; i++) {
            host_sleep_ms(300); /* injected_delay: progress versus gap and case bounds */
            printf("@etude 1 progress ticks %d\n", i);
        }
    }
    if (!strcmp(mode, "plain"))
        puts("plain ran");
    if (!strcmp(mode, "claim"))
        claims();
    puts(!strcmp(mode, "fail") ? "@etude 1 assert ran fail the count was 2"
                               : "@etude 1 assert ran pass");
    if (strcmp(mode, "cut"))
        puts("@etude 1 end 1");
    return 0;
}

static int scenario_role(int argc, char **argv)
{
    struct json *invocation = NULL;
    char text[131072], error[256], path[ROOM];
    const char *scratch, *id = value("ETUDE_CASE");
    FILE *file;
    size_t n;
    int rc = 0;
    if (argc != 2 || (file = fopen(argv[1], "rb")) == NULL)
        return 121;
    n = fread(text, 1, sizeof(text), file);
    fclose(file);
    if (json_parse(text, n, 1, &invocation, error, sizeof(error)) != 0)
        return 121;
    scratch = json_text(invocation, "scratch");
    if (scratch == NULL) {
        json_free(invocation);
        return 121;
    }
    printf("scenario: %s %s\n@etude 1 begin %s\n", argv[0], argv[1], id);
    snprintf(path, sizeof(path), "%s/evidence/vm.1.console", scratch);
    rc |= text_file(path, "Rach2 kernel booting\n", 0) != 0;
    if (strstr(id, ".panic"))
        rc |= text_file(path, "<2>[    0.500000] panic: page fault\n", 1) != 0;
    if (strstr(id, ".allowed"))
        rc |= text_file(path, "<2>[    1.250000] init: exited with status 1\n", 1) != 0;
    if (strstr(id, ".noconsole")) {
        snprintf(path, sizeof(path), "%s/evidence", scratch);
        rc |= host_remove_tree(path) != 0;
    }
    if (strstr(id, ".accounting"))
        claims();
    json_free(invocation);
    if (rc)
        return 121;
    if (strstr(id, ".lost")) {
        puts("@etude 1 progress stage 1");
        return 1;
    }
    puts("@etude 1 assert booted pass\n@etude 1 progress stage 1\n@etude 1 end 1");
    return 0;
}

/** Hash the real staged file into the preparation record. */
static int record_file(FILE *record, const char *tree, const char *goal)
{
    struct stat st;
    char path[ROOM], cache[ROOM], hex[65];
    long long mtime;
    snprintf(path, sizeof(path), "%s/%s", tree, goal);
    snprintf(cache, sizeof(cache), "%s/build/tests/digests", tree);
    if (etude_digest_file(cache, path, &st, hex) != 0)
        return -1;
#ifdef __APPLE__
    mtime = (long long)st.st_mtimespec.tv_sec * 1000000000 + st.st_mtimespec.tv_nsec;
#else
    mtime = (long long)st.st_mtim.tv_sec * 1000000000 + st.st_mtim.tv_nsec;
#endif
    return fprintf(record, "file %s %llu %llu %lld %s\n", hex, (unsigned long long)st.st_size,
                   (unsigned long long)st.st_ino, mtime, goal) < 0
               ? -1
               : 0;
}

/** A goal already listed by an earlier option is recorded only once. */
static int previous_goal(char **argv, int before, const char *goal)
{
    int i;
    for (i = 2; i + 1 < before; i += 2)
        if (!strcmp(argv[i], "-b") && !strcmp(argv[i + 1], goal))
            return 1;
    return 0;
}

/** A preparation fixture with the same artifact identities and goals. */
static int prepare_role(int argc, char **argv)
{
    const char *tree = NULL, *arches = NULL, *record = NULL, *kernel = NULL;
    char defaults[2][256] = {{0}};
    FILE *file;
    int i, bad = 0;
    if (getenv("FIXTURE_BREAK")) {
        puts("broken components\netude prepare: result: failed: broken components");
        return 1;
    }
    for (i = 2; i + 1 < argc; i += 2) {
        if (!strcmp(argv[i], "-t"))
            tree = argv[i + 1];
        if (!strcmp(argv[i], "-a"))
            arches = argv[i + 1];
        if (!strcmp(argv[i], "-o"))
            record = argv[i + 1];
        if (!strcmp(argv[i], "-n"))
            kernel = argv[i + 1];
    }
    if (!tree || !arches || !record || (file = fopen(record, "wb")) == NULL)
        return 121;
    fprintf(file, "etude-prepare 1\ntree %s\n", tree);
    for (i = 0; i < 2; i++) {
        const char *arch = i ? "x86_64" : "i686";
        if (!strstr(arches, arch))
            continue;
        snprintf(defaults[i], sizeof(defaults[i]), "%s/rach2%s%s/kernel",
                 i ? "build/x86_64" : "build", kernel ? "-" : "", kernel ? kernel : "");
        fprintf(file, "arch %s\nkernel %s\n", arch, defaults[i]);
        bad |= record_file(file, tree, defaults[i]) != 0;
    }
    for (i = 2; i + 1 < argc; i += 2)
        if (!strcmp(argv[i], "-b") && strcmp(argv[i + 1], defaults[0]) &&
            strcmp(argv[i + 1], defaults[1]) && !previous_goal(argv, i, argv[i + 1]))
            bad |= record_file(file, tree, argv[i + 1]) != 0;
    bad |= fclose(file) != 0;
    puts(bad ? "etude prepare: result: failed: fixture artifact missing"
             : "etude prepare: result: prepared: fixture artifacts");
    return bad ? 1 : 0;
}

/** Inherit the role's standard streams and its existing process ownership. */
static int forward(char **argv)
{
#ifdef _WIN32
    char *line = NULL;
    wchar_t *program = etude_win_wide(argv[0]), *command = NULL;
    STARTUPINFOW startup;
    PROCESS_INFORMATION process;
    DWORD code = 121;
    int i, started = 0;
    memset(&startup, 0, sizeof(startup));
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    HANDLE handles[3] = {GetStdHandle(STD_INPUT_HANDLE), GetStdHandle(STD_OUTPUT_HANDLE),
                         GetStdHandle(STD_ERROR_HANDLE)};
    HANDLE copies[3] = {NULL, NULL, NULL};
    for (i = 0; i < 3; i++)
        if (!DuplicateHandle(GetCurrentProcess(), handles[i], GetCurrentProcess(), &copies[i], 0,
                             TRUE, DUPLICATE_SAME_ACCESS))
            break;
    startup.hStdInput = copies[0];
    startup.hStdOutput = copies[1];
    startup.hStdError = copies[2];
    if (i == 3 && program && etude_win_cmdline(argv[0], (const char *const *)argv + 1, &line) == 0)
        command = etude_win_wide(line);
    if (command)
        started =
            CreateProcessW(program, command, NULL, NULL, TRUE, 0, NULL, NULL, &startup, &process);
    for (i = 0; i < 3; i++)
        if (copies[i])
            CloseHandle(copies[i]);
    free(line);
    free(command);
    free(program);
    if (started) {
        if (WaitForSingleObject(process.hProcess, INFINITE) == WAIT_OBJECT_0)
            GetExitCodeProcess(process.hProcess, &code);
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    return (int)code;
#else
    execv(argv[0], argv);
    return 121;
#endif
}

/** The real QEMU runs directly after its exact argument record is appended. */
static int qemu_role(int argc, char **argv)
{
    const char *qemu = getenv("LEGACY_REAL_QEMU"), *evidence = getenv("LEGACY_GUEST_EVIDENCE");
    char path[ROOM];
    FILE *file;
    int i;
    if (!qemu || !evidence)
        return 121;
    snprintf(path, sizeof(path), "%s/qemu.argv", evidence);
    if ((file = fopen(path, "ab")) == NULL)
        return 121;
    for (i = 1; i < argc; i++)
        fprintf(file, "%s%s", i > 1 ? " " : "", argv[i]);
    fputc('\n', file);
    if (fclose(file) != 0)
        return 121;
    argv[0] = (char *)qemu;
    return forward(argv);
}

int legacy_fixture_role(int argc, char **argv)
{
    const char *name = strrchr(argv[0], '/');
    const char *back = strrchr(argv[0], '\\');
    name = back && (!name || back > name) ? back + 1 : name ? name + 1 : argv[0];
    if (!strcmp(name, "probe"))
        return probe_role(argc, argv);
    if (!strcmp(name, "scenario"))
        return scenario_role(argc, argv);
    if (!strncmp(name, "qemu-system-", 12)) {
        if (strstr(argv[0], "/qemu/")) {
            puts("FIXTURE-BOOT-OK");
            host_sleep_ms(200); /* injected_delay: stand-in's workload */
            return 0;
        }
        return qemu_role(argc, argv);
    }
    if (argc > 1 && !strcmp(argv[1], "prepare"))
        return prepare_role(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "check")) {
        argv[0] = (char *)getenv("LEGACY_ETUDE");
        return argv[0] ? forward(argv) : 121;
    }
    return -1;
}
