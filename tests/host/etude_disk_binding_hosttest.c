/*
 * Check private disk binding without starting QEMU: regular private files
 * may replace a declared empty slot; artifacts, hard links, outside files
 * and undeclared slots may not.
 *
 * host-deps: tools/etude/serial.c tools/etude/processor.c
 * host-deps: tools/etude/posix/qemu.c tools/etude/qemu_profile.c
 * host-deps: tools/etude/posix/exec.c tools/etude/posix/keeper.c
 * host-deps: tools/etude/posix/process.c tools/etude/posix/store.c
 * host-deps: tools/etude/capture.c tools/etude/history_json.c
 * host-deps: tools/etude/guest/wire.c utils/sha256.c
 * host-deps: tools/etude/volume/volume.c
 * host-cflags: -I tools/etude/include -I tools/etude -I tools/etude/posix
 * host-cflags: -I utils -Wall -Wextra -Werror
 */
#include "../../tools/etude/posix/scenario.c"

static char fixture[4096], scratch[4096], disk_file[4096], immutable[4096], alias[4096];
static char invocation[4096];
static int failures;

static void remove_fixture(void)
{
    if (alias[0])
        unlink(alias);
    if (disk_file[0])
        unlink(disk_file);
    if (immutable[0])
        unlink(immutable);
    if (invocation[0])
        unlink(invocation);
    if (scratch[0])
        rmdir(scratch);
    if (fixture[0])
        rmdir(fixture);
}

static void check_binding(int held, const char *what)
{
    if (!held) {
        fprintf(stderr, "etude_disk_binding: %s\n", what);
        failures++;
    }
}

static void path_of(char *out, const char *base, const char *name)
{
    if (snprintf(out, 4096, "%s/%s", base, name) >= 4096)
        exit(2);
}

static void write_fixture(const char *path)
{
    FILE *f = fopen(path, "wb");
    if (f == NULL || fputs("private image", f) < 0 || fclose(f) != 0)
        exit(2);
}

int etude_scenario(struct etude_case *c)
{
    (void)c;
    return 1;
}

static void invocation_architectures(void)
{
    const char *manifest, *store;
    struct etude_case c;
    struct eh_json *declared, *helper_profile;
    FILE *f;

    path_of(invocation, fixture, "invocation.json");
    f = fopen(invocation, "wb");
    if (f == NULL ||
        fputs("{\"schema_version\":1,\"run_id\":\"run\",\"attempt_id\":\"attempt\","
              "\"case_id\":\"host.fixture\",\"architecture\":\"none\",\"scratch\":\"scratch\","
              "\"manifest\":\"manifest\",\"store\":\"store\",\"grace_us\":1,\"cleanup_us\":1,"
              "\"console_max_bytes\":1,\"output_max_bytes\":1,\"resend_us\":1}",
              f) < 0 ||
        fclose(f) != 0)
        exit(2);
    memset(&c, 0, sizeof(c));
    check_binding(load_invocation(&c, invocation, &manifest, &store) == 0 &&
                      !strcmp(etude_case_architecture(&c), "none"),
                  "architecture-none invocation refused or changed");
    declared = eh_json_parse("{\"architectures\":\"none\"}");
    c.kase = declared;
    check_binding(case_architecture(&c), "architecture-none case refused");
    c.arch = "i686";
    check_binding(!case_architecture(&c), "architecture-none case accepted i686");
    eh_json_free(declared);
    declared = eh_json_parse("{\"architectures\":[\"i686\",\"x86_64\"]}");
    c.kase = declared;
    check_binding(case_architecture(&c), "i686 declaration refused");
    c.arch = "x86_64";
    check_binding(case_architecture(&c), "x86_64 declaration refused");
    c.arch = "none";
    check_binding(!case_architecture(&c), "architecture-specific case accepted none");
    struct machine_decl helper;
    memset(&helper, 0, sizeof(helper));
    helper.helper = 1;
    helper_profile = eh_json_parse("{\"architecture\":\"x86_64\"}");
    helper.qemu = helper_profile;
    check_binding(!strcmp(machine_architecture(&c, &helper), "x86_64"),
                  "neutral case lost its helper architecture");
    eh_json_free(helper_profile);
    eh_json_free(declared);
    c.kase = NULL;
    case_free(&c);
}

/** Preserve the source basename used by mcopy's directory destination. */
static void image_upload_arguments(void)
{
    static const char *const paths[] = {"/host/Busy Box", "Busy Box", "D:/host/Busy Box",
#ifdef _WIN32
                                          "D:\\host\\Busy Box",
#endif
                                          NULL};
    struct image_invocation v;
    const char *upload[] = {"mcopy", "-i", "disk@@1048576", NULL, "::/", NULL};
    char oversized[4096];
    const char *invalid[] = {"", "/", ".", "/host/..", oversized, NULL};

    for (size_t i = 0; paths[i] != NULL; i++) {
        upload[3] = paths[i];
        memset(&v, 0, sizeof v);
        check_binding(image_copy_arguments(upload, 5, 3, &v) == ETUDE_OK &&
                          v.input == paths[i] && v.destination == NULL && v.args[5] &&
                          !strcmp(v.args[5], "/dev/shm/image-input/Busy Box") &&
                          !strcmp(upload[2], "disk@@1048576") && !strcmp(upload[4], "::/"),
                      "mcopy upload changed its basename or another operand");
    }
    memset(oversized, 'a', sizeof(oversized) - 1);
    oversized[sizeof(oversized) - 1] = '\0';
    for (size_t i = 0; invalid[i] != NULL; i++) {
        upload[3] = invalid[i];
        memset(&v, 0, sizeof v);
        check_binding(image_copy_arguments(upload, 5, 3, &v) == ETUDE_REFUSED,
                      "mcopy accepted an empty, dot or oversized staged basename");
    }
}

/** Keep large image operands in the permanent tmpfs, not the boot module. */
static void image_work_paths(void)
{
    struct image_invocation v;
    struct machine_decl decl;
    struct etude_case_machine machine;
    char startup[128];
    const char *dump[] = {"debugfs", "-R", "dump /large /host/result", "disk", NULL};
    const char *write[] = {"debugfs", "-w", "-R", "write /host/input /large", "disk", NULL};
    const char *copy[] = {"mcopy", "-n", "-i", "disk", "::/large", "/host/result", NULL};

    memset(&v, 0, sizeof v);
    check_binding(image_request(dump, 2, 1, &v) == ETUDE_OK && v.destination &&
                      !strcmp(v.destination, "/host/result") && v.args[2] &&
                      !strcmp(v.args[2], "dump \"/large\" \"/dev/shm/image-file\""),
                  "debugfs dump must use tmpfs and retain the host destination");
    memset(&v, 0, sizeof v);
    check_binding(image_request(write, 3, 1, &v) == ETUDE_OK && v.input &&
                      !strcmp(v.input, "/host/input") && v.args[3] &&
                      !strcmp(v.args[3], "write \"/dev/shm/image-file\" \"/large\""),
                  "debugfs write must use tmpfs and retain the host input");
    memset(&v, 0, sizeof v);
    check_binding(image_copy_arguments(copy, 6, 3, &v) == ETUDE_OK && v.destination &&
                      !strcmp(v.destination, "/host/result") && v.args[7] &&
                      !strcmp(v.args[7], "/dev/shm/image-file"),
                  "mcopy output must use the same tmpfs endpoint");
    memset(&decl, 0, sizeof decl);
    memset(&machine, 0, sizeof machine);
    machine.decl = &decl;
    decl.id = "image-helper";
    decl.helper = 1;
    check_binding(guest_image_state(&machine) &&
                      !etude_serial_guest_line(startup, sizeof(startup), 0,
                                               guest_image_state(&machine), 2000000) &&
                      !strcmp(startup, "/etude-guest -s /dev/shm/etude-guest -t / -r 2000\n"),
                  "image-helper responder objects must use tmpfs");
    decl.id = "products";
    check_binding(!guest_image_state(&machine) &&
                      !etude_serial_guest_line(startup, sizeof(startup), 0,
                                               guest_image_state(&machine), 2000000) &&
                      !strcmp(startup, "/etude-guest -t / -r 2000\n"),
                  "other helpers must keep their existing responder state");
    decl.id = "image-helper";
    decl.helper = 0;
    check_binding(!guest_image_state(&machine) &&
                      !etude_serial_guest_line(startup, sizeof(startup), 0,
                                               guest_image_state(&machine), 2000000) &&
                      !strcmp(startup, "/etude-guest -t / -r 2000\n"),
                  "a non-helper must keep its existing responder state");
}

int main(void)
{
    struct etude_case c;
    struct machine_decl *d;
    struct eh_json *profile;
    const char *tmp = getenv("TMPDIR");
    char request[] = "dump \"/file with spaces\" \"/tmp/out file\"", *words[4];

    atexit(remove_fixture);
#ifdef _WIN32
    if (tmp == NULL)
        tmp = getenv("TEMP");
#endif
    if (tmp == NULL)
        tmp = "/tmp";
    path_of(fixture, tmp, "rach2-etude-bind.XXXXXX");
    if (mkdtemp(fixture) == NULL)
        return 2;
    invocation_architectures();
    image_work_paths();
    image_upload_arguments();
    path_of(scratch, fixture, "attempt");
    path_of(disk_file, scratch, "disk");
    path_of(alias, scratch, "alias");
    path_of(immutable, fixture, "artifact");
    if (mkdir(scratch, 0700) != 0)
        return 2;
    write_fixture(disk_file);
    write_fixture(immutable);
    memset(&c, 0, sizeof(c));
    c.scratch = scratch;
    c.decls = calloc(1, sizeof(*c.decls));
    if (c.decls == NULL)
        return 2;
    c.decl_count = 1;
    d = c.decls;
    d->id = "helper";
    profile = eh_json_parse("{\"disks\":[{\"name\":\"slot\",\"size_bytes\":512},"
                            "{\"name\":\"artifact\"}]}");
    if (profile == NULL)
        return 2;
    d->qemu = profile;
    check_binding(etude_bind_disk(&c, "helper", "slot", disk_file) == ETUDE_OK,
                  "private regular file refused");
    check_binding(bound_disk(&c, d, "slot") != NULL, "accepted binding was not retained");
    check_binding(etude_bind_disk(&c, "helper", "missing", disk_file) == ETUDE_REFUSED,
                  "undeclared disk accepted");
    check_binding(etude_bind_disk(&c, "helper", "artifact", disk_file) == ETUDE_REFUSED,
                  "artifact slot accepted");
    check_binding(etude_bind_disk(&c, "helper", "slot", immutable) == ETUDE_REFUSED,
                  "file outside the attempt accepted");
    check_binding(etude_bind_disk(&c, "helper", "slot", scratch) == ETUDE_REFUSED,
                  "directory accepted as a disk");
#ifdef _WIN32
    {
        wchar_t *to = etude_win_long_path(alias), *from = etude_win_long_path(immutable);
        if (to == NULL || from == NULL || !CreateHardLinkW(to, from, NULL))
            return 2;
        free(to);
        free(from);
    }
#else
    if (link(immutable, alias) != 0)
        return 2;
#endif
    check_binding(etude_bind_disk(&c, "helper", "slot", alias) == ETUDE_REFUSED,
                  "hard link to an artifact accepted");
    check_binding(image_words(request, words, 4) == 3 &&
                      strcmp(words[1], "/file with spaces") == 0 &&
                      strcmp(words[2], "/tmp/out file") == 0,
                  "quoted debugfs pathnames changed");
    eh_json_free(profile);
    case_free(&c);
    if (!failures)
        puts("etude_disk_binding_hosttest: pass");
    return failures != 0;
}
