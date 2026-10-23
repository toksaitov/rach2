/* Native routine entry keeps one collection and validates both projections. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *env(const char *key)
{
    const char *value = getenv(key);
    return value ? value : "";
}

static const char *path(const char *name)
{
    static char paths[16][8192];
    static unsigned next;
    char *out = paths[next++ % 16];
    int n = snprintf(out, 8192, "%s/%s", env("CI_NATIVE_OUT"), name);
    return n >= 0 && n < 8192 ? out : "";
}

static int argument(int argc, char **argv, const char *wanted)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], wanted))
            return 1;
    return 0;
}

static int counted_vectors(int argc, char **argv)
{
    int position = 10;
    for (unsigned phase = 0; phase < 2; phase++) {
        if (position + 2 > argc)
            return 0;
        const char *program = argv[position++];
        char *end;
        const char *text = argv[position++];
        unsigned long count = strtoul(text, &end, 10);
        if (*text < '1' || *text > '9' || *end || count < 2 ||
            count > (unsigned long)(argc - position))
            return 0;
        if (strcmp(program, argv[position]) || strcmp(argv[position + 1], phase ? "run" : "admit"))
            return 0;
        position += (int)count;
    }
    return position == argc;
}

static int sequence(int argc, char **argv)
{
    if (!counted_vectors(argc, argv) || !argument(argc, argv, "admit") ||
        !argument(argc, argv, "run") || !argument(argc, argv, "-I") || !argument(argc, argv, "-g"))
        return 2;
    int push = !strcmp(env("CI_NATIVE_MODE"), "push");
    if (push && (!argument(argc, argv, "i686:host") || !argument(argc, argv, "x86_64:default") ||
                 strcmp(env("RACH2_CASES"), "host.fixture.host,machine.fixture.default")))
        return 2;
    if (fx_exists(path("sequence.calls")) || !fx_text(path("sequence.calls"), "one sequence\n") ||
        !fx_text(path("selection.list"),
                 "style none host.fixture.style\nhost none host.fixture.host\n"
                 "default i686 machine.fixture.default\ndefault x86_64 machine.fixture.default\n"))
        return 2;
    if (!strcmp(env("CI_NATIVE_MODE"), "cancel"))
        return 130;
    if (strcmp(env("CI_NATIVE_MODE"), "missing-history")) {
        if (!fx_mkdir(path("store")) || !fx_text(path("history/run.jsonl"), "fixture\n"))
            return 2;
    }
    return !strcmp(env("CI_NATIVE_MODE"), "pass") || push ? 0 : 1;
}

static int verdict(int argc, char **argv)
{
    if (!argument(argc, argv, "--project-architecture") ||
        !argument(argc, argv, "--require-budget") || !argument(argc, argv, "-b") ||
        !argument(argc, argv, "-c"))
        return 2;
    if (!strcmp(env("CI_NATIVE_MODE"), "push") && !argument(argc, argv, "--selection"))
        return 2;
    int bad = !strcmp(env("CI_NATIVE_MODE"), "budget") ||
              !strcmp(env("CI_NATIVE_MODE"), "cleanup") || !fx_exists(path("history/run.jsonl"));
    if (bad) {
        puts("etude check: result: failed: runner");
        return 1;
    }
    if (!strcmp(env("CI_NATIVE_MODE"), "foreign") && argument(argc, argv, "x86_64")) {
        puts("etude check: result: failed: default");
        return 1;
    }
    puts("etude check: result: passed");
    return 0;
}

int flow_ci_native_mock(int argc, char **argv)
{
    if (argc == 3 && !strcmp(argv[1], "observe") && !strcmp(argv[2], "parent")) {
        puts("1 1");
        return 0;
    }
    if (argc > 2 && !strcmp(argv[1], "observe") && !strcmp(argv[2], "sequence"))
        return sequence(argc, argv);
    if (argc > 2 && !strcmp(argv[1], "check") && !strcmp(argv[2], "verdict"))
        return verdict(argc, argv);
    if (argc > 1 && !strcmp(argv[1], "run") && argument(argc, argv, "inventory")) {
        puts("style|full|\nhost|full|\ndefault|core|\nshutdown|core|\nfpu|core|x86_64\nbigmem|full|"
             "x86_64");
        return 0;
    }
    return 2;
}

static int setup(struct fixture *f)
{
    const char *files[] = {"run.sh",
                           "native_entry.sh",
                           "native_sequence.sh",
                           "native_prepare.sh",
                           "native_site.sh",
                           "push.sh",
                           "site.sh",
                           "site"};
    for (size_t i = 0; i < sizeof(files) / sizeof(*files); i++) {
        char from[160], to[200];
        snprintf(from, sizeof(from), "tools/ci/%s", files[i]);
        snprintf(to, sizeof(to), "tree/tools/ci/%s", files[i]);
        if (!flow_copy(f, from, to))
            return 0;
    }
    if (!fx_mock(f, "native-etude") || !fx_mock(f, "uname") ||
        !flow_put(f, "native-ledger/claims", "etude-ledger 1\n") ||
        !fx_mkdir(flow_path(f, "native-locks")))
        return 0;
    char site[32768];
    int n = snprintf(site, sizeof(site),
                     "etude-site 2\nendpoint linux\nlauncher %s\nledger %s\nlocks %s\n"
                     "etude %s\ncapacity 24000,103079215104,171798691840\nwait 600\nseconds 600\n",
                     f->self, flow_path(f, "native-ledger"), flow_path(f, "native-locks"),
                     flow_path(f, "bin/native-etude"));
    if (n < 0 || (size_t)n >= sizeof(site) || !flow_put(f, "native.site", site))
        return 0;
    fx_env("ETUDE_SITE", flow_path(f, "native.site"));
    fx_env("ETUDE", flow_path(f, "bin/native-etude"));
    fx_env("RACH2_QEMU_ACCEL", "tcg");
    fx_env("ARCH", "i686 x86_64");
    fx_env("SUITES", "");
    return 1;
}

static int row(struct fixture *f, const char *mode, unsigned number)
{
    char out[8192], name[120], file[16384];
    snprintf(out, sizeof(out), "%s/native.%u", f->work, number);
    fx_env("CI_NATIVE_OUT", out);
    fx_env("CI_NATIVE_MODE", mode);
    const char *argv[] = {"/bin/sh", "tools/ci/run.sh", "core", out, NULL};
    fx_run(f, argv, flow_path(f, "tree"), NULL, NULL);
    int ok = f->status == (!strcmp(mode, "pass") ? 0 : !strcmp(mode, "cancel") ? 125 : 1);
    for (unsigned i = 0; i < 2; i++) {
        const char *arch = i ? "x86_64" : "i686";
        snprintf(file, sizeof(file), "%s/core-%s/done", out, arch);
        ok = ok && fx_exists(file) == !!strcmp(mode, "cancel");
        if (!strcmp(mode, "cancel"))
            continue;
        snprintf(file, sizeof(file), "%s/core-%s/failed", out, arch);
        const char *failed = !strcmp(mode, "pass")      ? ""
                             : !strcmp(mode, "foreign") ? (i ? "default\n" : "")
                                                        : "runner\n";
        ok = ok && fx_equal(file, failed);
    }
    snprintf(file, sizeof(file), "%s/sequence.calls", out);
    ok = ok && fx_equal(file, "one sequence\n");
    snprintf(name, sizeof(name), "native shared collection: %s", mode);
    return fx_check(f, ok, name);
}

static int push_row(struct fixture *f)
{
    const char *out = flow_path(f, "native.push");
    fx_env("CI_NATIVE_OUT", out);
    fx_env("CI_NATIVE_MODE", "push");
    fx_env("RACH2_CASES", "host.fixture.host,machine.fixture.default");
    const char *argv[] = {"/bin/sh", "tools/ci/push.sh", out, "i686:host", "x86_64:default", NULL};
    fx_run(f, argv, flow_path(f, "tree"), NULL, NULL);
    fx_env("RACH2_CASES", NULL);
    int ok = f->status == 0 && fx_equal(path("sequence.calls"), "one sequence\n") &&
             fx_exists(path("push-i686/done")) && fx_exists(path("push-x86_64/done"));
    return fx_check(f, ok, "push keeps explicit suites and case filter through native sequence");
}

int flow_ci_native(struct fixture *f)
{
    const char *modes[] = {"pass", "foreign", "budget", "cleanup", "missing-history", "cancel"};
    if (!setup(f))
        return 0;
    int ok = 1;
    for (unsigned i = 0; i < sizeof(modes) / sizeof(*modes); i++)
        ok = row(f, modes[i], i) && ok;
    ok = push_row(f) && ok;
    fx_env("ETUDE", "");
    fx_env("ETUDE_SITE", "");
    fx_env("CI_NATIVE_OUT", "");
    fx_env("CI_NATIVE_MODE", "");
    return ok;
}
