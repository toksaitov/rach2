/* Check raw SSH input and return bytes through the existing case adapter. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include "../../../tests/cases/machine/netcase.h"
#define fix_dir    bulk_fix_dir
#define fix_path   bulk_fix_path
#define fix_passed bulk_fix_passed
#define fix_format bulk_fix_format
#include "fixdir_host.c"

static char *bulk_base, *bulk_disk;
static int bulk_failures, bulk_mode, bulk_running, bulk_ended, bulk_boots, bulk_input;
static char bulk_hash[65];
#define BULK_CHECK(test)                                                     \
    do {                                                                     \
        if (!(test)) {                                                       \
            fprintf(stderr, "netcase bulk: line %d: %s\n", __LINE__, #test); \
            bulk_failures++;                                                 \
        }                                                                    \
    } while (0)
static enum etude_status bulk_command(struct etude_case_machine *, const char *const[],
                                      const char *const[], const char *, const char *,
                                      struct etude_guest_result *);
static enum etude_status bulk_command_file(struct etude_case_machine *, const char *const[],
                                           const char *const[], const char *, const char *,
                                           struct etude_guest_result *);
static enum etude_status bulk_qmp(struct etude_case_machine *, const char *, const char *, char **);
static enum etude_status bulk_wait(struct etude_case_machine *, struct etude_termination *);
static enum etude_status bulk_bind(struct etude_case *, const char *, const char *, const char *);
static enum etude_status bulk_boot(struct etude_case *, const char *, const char *const[],
                                   struct etude_case_machine **, struct etude_termination *);
static const char *bulk_evidence(struct etude_case *, const char *);
#define etude_guest_command      bulk_command
#define etude_guest_command_file bulk_command_file
#define etude_qmp                bulk_qmp
#define etude_wait_end           bulk_wait
#define etude_bind_disk          bulk_bind
#define etude_boot               bulk_boot
#define etude_evidence           bulk_evidence
#include "../../../tests/cases/machine/netcase.c"
#undef etude_guest_command
#undef etude_guest_command_file
#undef etude_qmp
#undef etude_wait_end
#undef etude_bind_disk
#undef etude_boot
#undef etude_evidence

static char *bulk_path(const char *name)
{
    char path[4096];
    bulk_fix_format(path, sizeof path, "%s/%s", bulk_base, name);
    return strdup(path);
}
static const char *bulk_evidence(struct etude_case *c, const char *name)
{
    static char paths[8][4096];
    static unsigned next;
    char *path = paths[next++ % 8];
    (void)c;
    BULK_CHECK(snprintf(path, 4096, "%s/%s", bulk_base, name) < 4096);
    return path;
}
static void bulk_write(int changed)
{
    unsigned char block[4096];
    struct sha256 digest;
    unsigned char sum[32];
    for (size_t i = 0; i < sizeof block; i++)
        block[i] = (unsigned char)(i * 131 + 17);
    FILE *file = fopen(bulk_disk, "wb");
    BULK_CHECK(file != NULL);
    sha256_init(&digest);
    for (unsigned i = 0; file && i < 1024; i++) {
        size_t size = bulk_mode == 1 && i == 1023 ? sizeof block - 1 : sizeof block;
        if (changed && i == 1023)
            block[3] ^= 1;
        BULK_CHECK(fwrite(block, 1, size, file) == size);
        if (changed && i == 1023)
            block[3] ^= 1;
        sha256_update(&digest, block, sizeof block);
    }
    if (file)
        BULK_CHECK(!fclose(file));
    sha256_final(&digest, sum);
    sha256_hex(sum, bulk_hash);
}
static void bulk_result(struct etude_guest_result *result)
{
    memset(result, 0, sizeof *result);
    result->complete = 1;
    result->cleanup = ETUDE_CLEANUP_CONFIRMED;
    result->termination.end = ETUDE_END_EXITED;
    result->termination.value = 0;
}
static enum etude_status bulk_command(struct etude_case_machine *m, const char *const argv[],
                                      const char *const env[], const char *input,
                                      const char *output, struct etude_guest_result *result)
{
    (void)m;
    (void)env;
    BULK_CHECK(bulk_running && !input && !strcmp(argv[0], "/bin/sh") && !strcmp(argv[1], "-c") &&
               strstr(argv[2], "wc -c") && strstr(argv[2], "sync && sha256sum") &&
               strstr(argv[2], "of=/dev/disk1") && strstr(argv[2], "bs=65536") &&
               !strcmp(argv[5], "4194304"));
    bulk_write(bulk_mode == 2);
    const char *path = bulk_evidence(NULL, output);
    FILE *file = fopen(path, "wb");
    BULK_CHECK(file != NULL);
    if (file) {
        fprintf(file, "%s  %s\n", bulk_hash, argv[4]);
        BULK_CHECK(!fclose(file));
    }
    char error[256];
    snprintf(error, sizeof error, "%s.err", output);
    file = fopen(bulk_evidence(NULL, error), "wb");
    BULK_CHECK(file != NULL);
    if (file)
        BULK_CHECK(!fclose(file));
    bulk_result(result);
    return ETUDE_OK;
}
static enum etude_status bulk_command_file(struct etude_case_machine *m, const char *const argv[],
                                           const char *const env[], const char *input,
                                           const char *output, struct etude_guest_result *result)
{
    (void)m;
    (void)argv;
    (void)env;
    BULK_CHECK(bulk_running && !strcmp(input, "/dev/disk1"));
    bulk_input++;
    FILE *file = fopen(bulk_evidence(NULL, output), "wb");
    BULK_CHECK(file != NULL);
    if (file)
        BULK_CHECK(!fclose(file));
    char error[256];
    snprintf(error, sizeof error, "%s.err", output);
    file = fopen(bulk_evidence(NULL, error), "wb");
    BULK_CHECK(file != NULL);
    if (file)
        BULK_CHECK(!fclose(file));
    bulk_result(result);
    return ETUDE_OK;
}
static enum etude_status bulk_qmp(struct etude_case_machine *m, const char *command,
                                  const char *args, char **reply)
{
    (void)m;
    (void)args;
    BULK_CHECK(bulk_running && !strcmp(command, "quit"));
    *reply = strdup("{}");
    bulk_running = 0;
    return ETUDE_OK;
}
static enum etude_status bulk_wait(struct etude_case_machine *m, struct etude_termination *end)
{
    (void)m;
    (void)end;
    BULK_CHECK(!bulk_running);
    if (bulk_mode == 3)
        return ETUDE_LOST;
    bulk_ended = 1;
    return ETUDE_OK;
}
static enum etude_status bulk_bind(struct etude_case *c, const char *machine, const char *slot,
                                   const char *path)
{
    (void)c;
    BULK_CHECK(bulk_ended && !bulk_running && !strcmp(machine, "channel-client") &&
               !strcmp(slot, "channel-input") && !strcmp(path, bulk_disk));
    return ETUDE_OK;
}
static enum etude_status bulk_boot(struct etude_case *c, const char *name,
                                   const char *const disks[], struct etude_case_machine **machine,
                                   struct etude_termination *end)
{
    (void)c;
    (void)disks;
    (void)end;
    BULK_CHECK(bulk_ended && !bulk_running && !strcmp(name, "channel-client"));
    bulk_ended = 0;
    bulk_running = 1;
    bulk_boots++;
    *machine = (struct etude_case_machine *)1;
    return ETUDE_OK;
}
static void bulk_rows(void)
{
    char *args[] = {"ssh", "-T", "root@127.0.0.1", "cat /tmp/image", NULL};
    bulk_mode = 0;
    bulk_write(0);
    struct net_case u = {.client = (struct etude_case_machine *)1};
    bulk_running = 1;
    bulk_ended = 0;
    bulk_boots = 0;
    BULK_CHECK(net_ssh_attach(&u, bulk_disk) == ETUDE_OK && bulk_boots == 1);
    struct net_run r;
    BULK_CHECK(net_ssh_input(&u, args, bulk_disk, "raw-input", &r) == ETUDE_OK && bulk_input == 1);
    net_run_free(&r);
    for (int mode = 0; mode < 4; mode++) {
        memset(&u, 0, sizeof u);
        u.client = (struct etude_case_machine *)1;
        snprintf(u.client_input, sizeof u.client_input, "%s", bulk_disk);
        bulk_mode = 0;
        bulk_write(0);
        bulk_mode = mode;
        bulk_running = 1;
        bulk_ended = 0;
        bulk_boots = 0;
        char name[32];
        snprintf(name, sizeof name, "raw-return-%d", mode);
        enum etude_status status = net_ssh_download_disk(&u, args, name, &r);
        BULK_CHECK(status == (mode == 3 ? ETUDE_LOST : mode ? ETUDE_FAILED : ETUDE_OK));
        char *path = bulk_path(name);
        struct stat st;
        if (!mode) {
            BULK_CHECK(!stat(path, &st) && st.st_size == 4194304 && bulk_boots == 1 &&
                       bulk_running);
            FILE *file = fopen(path, "rb");
            struct sha256 digest;
            unsigned char buf[65536], sum[32];
            char hex[65];
            size_t n;
            sha256_init(&digest);
            BULK_CHECK(file != NULL);
            while (file && (n = fread(buf, 1, sizeof buf, file)))
                sha256_update(&digest, buf, n);
            if (file)
                BULK_CHECK(!fclose(file));
            sha256_final(&digest, sum);
            sha256_hex(sum, hex);
            BULK_CHECK(!strcmp(hex, bulk_hash));
        } else
            BULK_CHECK(stat(path, &st) && !bulk_boots);
        free(path);
        net_run_free(&r);
    }
}
int etude_test_netcase_bulk(void)
{
    bulk_base = strdup(bulk_fix_dir());
    bulk_disk = bulk_path("input.disk");
    bulk_rows();
    free(bulk_disk);
    free(bulk_base);
    if (!bulk_failures) {
        bulk_fix_passed();
        puts("netcase bulk: input, exact return and refusal checks passed");
    }
    return bulk_failures != 0;
}
