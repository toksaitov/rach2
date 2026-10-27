/*
 * Check native execution on the desktop that runs the collection.
 * On Windows preparation substitutes winfixture and its declared helpers,
 * including these same complete-output assertions. On macOS the native
 * import must accept the symbolic link to the system's atos that the
 * sanitizer's preparation places, and refuse one to another program.
 *
 * host-alone: native platform fixture isolation
 * host-tools: git tar
 * host-deps: tests/host/compat/output_sinks.c tools/etude/capture.c
 * host-deps: tools/etude/posix/exec.c tools/etude/posix/keeper.c
 * host-deps: tools/etude/posix/process.c tools/etude/tooling_cache.c
 * host-deps: tests/host/compat/tooling_cache_fixture.c
 * host-deps: tests/host/compat/collection_tools_host.c
 * host-deps: tests/host/compat/command_host.c tools/etude/collection_tools.c
 * host-deps: tools/etude/json.c tools/etude/routine_proof.c
 * host-deps: tools/etude/digest.c utils/sha256.c
 * host-deps: tests/host/compat/retention_host.c
 * host-deps: tests/host/compat/routine_evidence_host.c
 * host-deps: tools/etude/routine_evidence.c tools/etude/routine_reader.c
 * host-deps: tools/etude/retention.c tools/etude/retention_io.c
 * host-deps: tools/etude/retention_proof.c tools/etude/retention_issues.c
 * host-deps: tools/etude/reference.c tools/etude/measurement_prepare.c
 * host-deps: tools/etude/preparation_record.c tools/etude/posix/store.c
 * host-deps: tools/etude/posix/authority_platform.c
 * host-deps: tools/etude/history_collect.c tools/etude/history_json.c
 * host-deps: tests/host/compat/routine_entry_host.c
 * host-deps: tools/etude/host_prepare_io.c
 * host-deps: tools/etude/host_prepare_build.c tools/etude/authority_parse.c
 * host-inputs: tools/etude/routine_inputs.c tools/etude/routine_sequence.c
 * host-inputs: tools/etude/routine_entry.c tools/etude/routine_owner_entry.c
 * host-inputs: tools/etude/routine_prepare.c tools/etude/routine_site.c
 * host-deps: tests/host/compat/routine_listener_host.c
 * host-deps: tools/etude/preparation_import_data.c
 * host-inputs: tools/ci/fixtures/routine_listener_test.c
 * host-inputs: tools/etude/routine_jobs.c
 * host-cflags: -I tools/etude -I tests/host/compat -I utils
 * host-cflags: -I tools/etude/include -Wall -Wextra -Werror
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "output_sinks.h"
#include "preparation_import.h"

int etude_test_tooling_cache(const char *tmp);
int etude_test_collection_tools(const char *self, const char *tmp);
int etude_test_collection_child(int argc, char **argv);
int etude_test_retention(const char *self);
int etude_test_retention_child(int argc, char **argv);
int etude_test_routine_evidence(void);
int etude_test_routine_entry(const char *self);
int etude_test_routine_listener(void);

#ifdef __APPLE__
#define SANITIZER "build/tests/host-suite/sanitizer"

static char atos_tree[4096];

static void atos_path(char *path, size_t size, const char *leaf)
{
    snprintf(path, size, "%s/%s%s%s", atos_tree, leaf[0] ? SANITIZER : "", leaf[0] ? "/" : "",
             leaf);
}

static void atos_cleanup(void)
{
    static const char *const leaves[] = {"atos", "atos.provenance.json", "tools.json"};
    static const char *const dirs[] = {SANITIZER, "build/tests/host-suite", "build/tests", "build",
                                       ""};
    char path[4096];

    if (!atos_tree[0])
        return;
    for (size_t i = 0; i < sizeof(leaves) / sizeof(leaves[0]); i++) {
        atos_path(path, sizeof(path), leaves[i]);
        unlink(path);
    }
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++) {
        snprintf(path, sizeof(path), "%s%s%s", atos_tree, dirs[i][0] ? "/" : "", dirs[i]);
        rmdir(path);
    }
}

static void atos_interrupted(int sig)
{
    atos_cleanup();
    signal(sig, SIG_DFL);
    raise(sig);
}

/* Import a sanitizer preparation whose atos is a link to `target`. */
static int atos_import(const char *target)
{
    char link[4096], proof[4096], manifest[4096], digest[65];
    struct json *record = hp_value(JSON_OBJECT), *inputs = hp_value(JSON_ARRAY),
                *roles = hp_value(JSON_OBJECT), *artifacts = NULL;
    const char *leaves[] = {"atos", "atos.provenance.json"};
    struct pi_context c;

    atos_path(link, sizeof(link), "atos");
    atos_path(proof, sizeof(proof), "atos.provenance.json");
    atos_path(manifest, sizeof(manifest), "tools.json");
    unlink(link);
    int bad = symlink(target, link) || hp_hash(link, digest);
    hp_member(record, "source", hp_string(target));
    hp_member(record, "sha256", hp_string(digest));
    bad = bad || hp_publish(proof, record);
    for (size_t i = 0; !bad && i < 2; i++) {
        char path[4096], hash[65];
        struct json *input = hp_value(JSON_OBJECT);
        atos_path(path, sizeof(path), leaves[i]);
        bad = hp_hash(path, hash);
        snprintf(path, sizeof(path), SANITIZER "/%s", leaves[i]);
        hp_member(input, "path", hp_string(path));
        hp_member(input, "sha256", hp_string(hash));
        hp_item(inputs, input);
    }
    json_free(record);
    record = hp_value(JSON_OBJECT);
    hp_member(record, "schema_version", hp_number(1));
    hp_member(record, "platform", hp_string("darwin"));
    hp_member(record, "inputs", inputs);
    bad = bad || hp_publish(manifest, record);
    hp_member(roles, SANITIZER "/tools.json", hp_string("native"));
    memset(&c, 0, sizeof(c));
    c.tree = atos_tree;
    if (!bad)
        artifacts = pi_native(&c, roles);
    bad = bad || !artifacts;
    json_free(artifacts);
    json_free(roles);
    json_free(record);
    return bad ? -1 : 0;
}

/* The native import accepts only a link to the system's own atos. */
static int sanitizer_import(const char *tmp)
{
    char path[4096], *atos = realpath("/usr/bin/atos", NULL), *other = realpath("/bin/ls", NULL);
    int bad = 0;

    snprintf(atos_tree, sizeof(atos_tree), "%s/etude-atos.XXXXXX", tmp);
    if (!atos || !other || !mkdtemp(atos_tree)) {
        atos_tree[0] = 0;
        fprintf(stderr, "etude-platform-test: the atos fixture could not be made\n");
        bad = 1;
    } else {
        atexit(atos_cleanup);
        signal(SIGHUP, atos_interrupted);
        signal(SIGINT, atos_interrupted);
        signal(SIGTERM, atos_interrupted);
        snprintf(path, sizeof(path), "%s/" SANITIZER, atos_tree);
        bad = hp_mkdir(path);
    }
    if (!bad && atos_import(atos)) {
        fprintf(stderr, "etude-platform-test: failed: the link to atos is refused\n");
        bad = 1;
    }
    if (!bad && !atos_import(other)) {
        fprintf(stderr, "etude-platform-test: failed: a link to another program is accepted\n");
        bad = 1;
    }
    free(atos);
    free(other);
    return bad;
}
#else
static int sanitizer_import(const char *tmp)
{
    (void)tmp;
    return 0;
}
#endif

static int platform_focused(int argc, char **argv, const char *tmp)
{
    if (argc == 2 && !strcmp(argv[1], "--routine-listener-only"))
        return etude_test_routine_listener();
    if (argc == 2 && !strcmp(argv[1], "--routine-entry-only"))
        return etude_test_routine_entry(argv[0]);
    if (argc == 2 && !strcmp(argv[1], "--evidence-only"))
        return etude_test_routine_evidence();
    if (argc == 2 && !strcmp(argv[1], "--retention-only"))
        return etude_test_retention(argv[0]);
    if (argc == 2 && !strcmp(argv[1], "--collection-only"))
        return etude_test_collection_tools(argv[0], tmp);
    if (argc == 2 && !strcmp(argv[1], "--sanitizer-import-only"))
        return sanitizer_import(tmp);
    return -1;
}

int main(int argc, char **argv)
{
    char path[4096];
    const char *tmp = getenv("TMPDIR");
    size_t i;

    int child = etude_test_collection_child(argc, argv);
    if (child >= 0)
        return child;
    child = etude_test_retention_child(argc, argv);
    if (child >= 0)
        return child;
    if (argc == 3 && strcmp(argv[1], "--big") == 0) {
        for (i = 0; i < 100000; i++)
            putchar((int)('a' + i % 23));
        fflush(stdout);
        fputs("big done\n", stderr);
        return 0;
    }
    if (tmp == NULL || tmp[0] == '\0')
        tmp = "/tmp";
    int focused = platform_focused(argc, argv, tmp);
    if (focused >= 0)
        return focused;
    if (etude_test_collection_tools(argv[0], tmp))
        return 1;
    if (etude_test_retention(argv[0]))
        return 1;
    if (etude_test_routine_listener())
        return 1;
    if (etude_test_routine_entry(argv[0]))
        return 1;
    if (etude_test_routine_evidence())
        return 1;
    if (etude_test_tooling_cache(tmp))
        return 1;
    if (sanitizer_import(tmp))
        return 1;
    if (argv[0][0] == '/')
        return etude_test_output_sinks(argv[0], "--big", tmp);
    if (getcwd(path, sizeof path) == NULL || strlen(path) + strlen(argv[0]) + 2 > sizeof path)
        return 1;
    strcat(path, "/");
    strcat(path, argv[0]);
    return etude_test_output_sinks(path, "--big", tmp);
}
