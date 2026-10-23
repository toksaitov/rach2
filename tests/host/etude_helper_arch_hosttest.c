/*
 * Check helper architecture selection without starting QEMU. An explicit
 * helper architecture selects its launch profile and memory accounting,
 * while an ordinary machine cannot override the case architecture.
 *
 * host-deps: tools/etude/serial.c tools/etude/processor.c
 * host-deps: tools/etude/posix/qemu.c tools/etude/qemu_profile.c
 * host-deps: tools/etude/posix/exec.c tools/etude/posix/keeper.c
 * host-deps: tools/etude/posix/process.c tools/etude/posix/store.c
 * host-deps: tools/etude/capture.c tools/etude/history_json.c
 * host-deps: tools/etude/guest/wire.c tools/etude/volume/volume.c utils/sha256.c
 * host-cflags: -I tools/etude/include -I tools/etude -I tools/etude/posix
 * host-cflags: -I utils -Wall -Wextra -Werror
 */
#include "../../tools/etude/posix/scenario.c"

static int failures;

static void check_arch(int held, const char *what)
{
    if (!held) {
        fprintf(stderr, "etude_helper_arch: %s\n", what);
        failures++;
    }
}

/** Exercise the invocation loader as well as the profile's architecture. */
static void declaration(const char *helper, const char *arch, int allowed, const char *wanted)
{
    struct etude_case c;
    char manifest[1024];
    int loaded;
    memset(&c, 0, sizeof(c));
    c.arch = "i686";
    snprintf(manifest, sizeof(manifest),
             "{\"steps\":[],\"machines\":[{\"machine_id\":\"products\",\"helper\":%s,"
             "\"console\":\"serial\",\"qmp\":true,\"qemu\":{%s}}]}",
             helper, arch);
    c.kase = c.inv = eh_json_parse(manifest);
    if (c.kase == NULL)
        exit(2);
    loaded = load_steps(&c);
    check_arch((loaded == 0) == allowed, "declaration acceptance differs");
    if (loaded == 0)
        check_arch(strcmp(machine_architecture(&c, c.decls), wanted) == 0,
                   "the effective machine architecture differs");
    free(c.steps);
    free(c.decls);
    eh_json_free((struct eh_json *)c.kase);
}

/** A fixed x86-64 helper's boundary profile uses its own memory rules even
 * when the case is i686; falling back to the case would reserve all 72 GiB. */
static void memory(void)
{
    struct etude_case c;
    struct machine_decl d;
    struct eh_json *qemu;
    memset(&c, 0, sizeof(c));
    memset(&d, 0, sizeof(d));
    c.arch = "i686";
    qemu = eh_json_parse("{\"architecture\":\"x86_64\",\"initrd\":\"initrd\","
                         "\"cpu\":\"max-nopdpe1gb\",\"processors\":1,"
                         "\"memory_bytes\":77309411328,\"memory_profile\":\"bigmem-boundary-v1\","
                         "\"firmware\":\"direct\",\"disks\":[],\"devices\":[],"
                         "\"boot_options\":[\"rach2.rescue\"]}");
    if (qemu == NULL)
        exit(2);
    d.qemu = qemu;
    d.helper = 1;
    check_arch(decl_need(&c, &d) == ETUDE_BIGMEM_RESIDENT + ETUDE_BIGMEM_OVERHEAD,
               "memory accounting ignored the helper architecture");
    d.helper = 0;
    check_arch(strcmp(machine_architecture(&c, &d), "i686") == 0,
               "an ordinary machine changed architecture");
    eh_json_free(qemu);
}

int etude_scenario(struct etude_case *c)
{
    (void)c;
    return 1;
}

int main(void)
{
    declaration("true", "\"architecture\":\"x86_64\"", 1, "x86_64");
    declaration("true", "\"architecture\":\"i686\"", 1, "i686");
    declaration("true", "", 1, "i686");
    declaration("false", "", 1, "i686");
    declaration("false", "\"architecture\":\"x86_64\"", 0, NULL);
    declaration("false", "\"architecture\":\"i686\"", 0, NULL);
    declaration("true", "\"architecture\":\"arm64\"", 0, NULL);
    declaration("true", "\"architecture\":17", 0, NULL);
    memory();
    return failures != 0;
}
