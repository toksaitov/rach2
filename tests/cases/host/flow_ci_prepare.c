/* Public preparation helpers keep caches and transfer verifiable evidence. */
#define _POSIX_C_SOURCE 200809L
#include "flow_check.h"
#include "sha256.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int file_hash(const char *path, char hex[65], size_t *size)
{
    unsigned char bytes[8192], digest[32];
    struct sha256 hash;
    FILE *f = fopen(path, "rb");
    size_t n;
    if (!f)
        return 0;
    *size = 0;
    sha256_init(&hash);
    while ((n = fread(bytes, 1, sizeof bytes, f)) != 0) {
        *size += n;
        sha256_update(&hash, bytes, n);
    }
    int ok = !ferror(f);
    fclose(f);
    sha256_final(&hash, digest);
    sha256_hex(digest, hex);
    return ok;
}

int flow_ci_reader_record(struct fixture *f, const char *program)
{
    char hex[65], text[32768];
    size_t size;
    if (!file_hash(program, hex, &size))
        return 0;
    const char *base = strrchr(program, '/');
    if (!base)
        return 0;
    int n = snprintf(text, sizeof text, "etude-prepare 1\ntree %.*s\nfile %s %lu 0 0 %s\n",
                     (int)(base - program), program, hex, (unsigned long)size, base + 1);
    if (n < 0 || (size_t)n >= sizeof text || !flow_put(f, "reader.record", text))
        return 0;
    fx_env("ETUDE_REPORT_READER_RECORD", flow_path(f, "reader.record"));
    return 1;
}

/* Keep the prepared helper distinct from the subject reader stand-in. */
static int native_helper(struct fixture *f, char program[4096])
{
    const char *root = getenv("HOST_PRODUCTS");
    if (!root || !*root)
        root = "/host-products";
    int n = snprintf(program, 4096, "%s/native-helper-etude", f->work);
    if (n < 0 || n >= 4096)
        return 0;
    if (fx_exists(program))
        return 1;
    char source[4096];
    n = snprintf(source, sizeof source, "%s/etude", root);
    return n >= 0 && n < (int)sizeof source && fx_copy(source, program);
}

static int clean_prepare(struct fixture *f, const char *program)
{
    const char makefile[] = "ARCH ?= i686\n"
                            "B = build\n"
                            "ifeq ($(ARCH),x86_64)\nB = build/x86_64\nendif\n"
                            ".PHONY: components ubsan-kernel\ncomponents: ; @true\n"
                            "ubsan-kernel:\n\t@mkdir -p $(B)/rach2-ubsan\n"
                            "\t@printf 'new kernel\\n' > $(B)/rach2-ubsan/kernel\n"
                            "build/clean.done:\n\t@printf 'new output\\n' > $@\n";
    if (!flow_put(f, "tree/tests/Makefile", "PROGS = probe\n") ||
        !flow_put(f, "tree/Makefile", makefile))
        return 0;
    char receipt[8192];
    int n = snprintf(receipt, sizeof receipt, "ETUDE_PREPARATION_RECEIPT=%s",
                     flow_path(f, "tree/build/tests/host-suite/current/receipt"));
    if (n < 0 || n >= (int)sizeof receipt)
        return 0;
    const char *environment[] = {
        "PATH=/usr/bin:/bin",
        "ETUDE_PREPARATION_CLEAN=1",
        "ETUDE_PREPARATION_RUN_ID=fixture-clean",
        receipt,
        "ETUDE_PREPARATION_COMMIT=1111111111111111111111111111111111111111",
        "ETUDE_PREPARATION_GRANT",
        "ETUDE_PREPARATION_IMPORT",
        "RACH2_TREE_HELD",
        NULL};
    const char *argv[] = {program,
                          "lock",
                          "tree",
                          flow_path(f, "tree"),
                          "fixture-clean",
                          program,
                          "prepare",
                          "-t",
                          flow_path(f, "tree"),
                          "-a",
                          "i686,x86_64",
                          "-n",
                          "ubsan",
                          "-o",
                          flow_path(f, "tree/build/tests/host-suite/current/record"),
                          "-b",
                          "build/clean.done",
                          NULL};
    fx_run(f, argv, NULL, environment, NULL);
    return f->status == 0 && fx_has(flow_path(f, "tree/build/tests/host-suite/current/receipt"),
                                    "\"inhouse\":\"clean\"");
}

static int clean_row(struct fixture *f)
{
    const char *files[] = {"build/rach2/kernel",
                           "build/x86_64/rach2-o2/kernel",
                           "build/tools/check",
                           "build/tests/host-suite/old",
                           "build/vendor/gnu/keep",
                           "build/x86_64/vendor-work/keep",
                           "build/tests/failed/keep",
                           "build/host-tools/native/keep",
                           "build/tests/host-suite/current/grant"};
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        char name[256];
        snprintf(name, sizeof name, "tree/%s", files[i]);
        if (!flow_put(f, name, "bytes"))
            return 0;
    }
    char program[4096];
    int ok = native_helper(f, program) && clean_prepare(f, program);
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        char name[256];
        snprintf(name, sizeof name, "tree/%s", files[i]);
        ok = ok && fx_exists(flow_path(f, name)) == (i >= 4);
    }
    return fx_check(f, ok, "release clean keeps vendor caches, evidence and current grant");
}

static int transfer_row(struct fixture *f)
{
    if (!flow_put(f, "transfer/history/run.jsonl", "complete history\n") ||
        !flow_put(f, "transfer/store/blob", "assertion evidence\n") ||
        !flow_put(f, "transfer/context/context.json", "bound context\n") ||
        !flow_put(f, "transfer/objects/large-image", "artifact object\n"))
        return 0;
    char program[4096];
    if (!native_helper(f, program))
        return 0;
    const char *stage[] = {
        program, "prepare", "evidence", "stage", flow_path(f, "transfer"), flow_path(f, "shared"),
        NULL};
    fx_run(f, stage, NULL, NULL, NULL);
    int ok = f->status == 0 && !fx_exists(flow_path(f, "shared/objects/large-image"));
    const char *restore[] = {
        program, "prepare", "evidence", "restore", flow_path(f, "shared"), flow_path(f, "restored"),
        NULL};
    fx_run(f, restore, NULL, NULL, NULL);
    ok = ok && f->status == 0 &&
         fx_equal(flow_path(f, "restored/history/run.jsonl"), "complete history\n") &&
         fx_equal(flow_path(f, "restored/store/blob"), "assertion evidence\n");
    ok = fx_check(f, ok, "shared evidence survives transfer without executable objects");
    if (!flow_put(f, "shared/store/blob", "corrupt\n"))
        return 0;
    const char *bad[] = {program,
                         "prepare",
                         "evidence",
                         "restore",
                         flow_path(f, "shared"),
                         flow_path(f, "corrupt-restored"),
                         NULL};
    fx_run(f, bad, NULL, NULL, NULL);
    return fx_check(f, f->status != 0 && !fx_exists(flow_path(f, "corrupt-restored")),
                    "corrupt shared evidence is refused before publication") &&
           ok;
}

static int reader_row(struct fixture *f)
{
    if (!flow_ci_reader_record(f, f->self))
        return 0;
    char program[4096];
    if (!native_helper(f, program))
        return 0;
    const char *argv[] = {program, "prepare", "reader", f->self, flow_path(f, "reader.record"),
                          NULL};
    fx_run(f, argv, NULL, NULL, NULL);
    int ok = fx_check(f, f->status == 0, "native local reader matches installed receipt");
    if (!flow_put(f, "reader.record", "etude-prepare 1\ntree /\n"))
        return 0;
    fx_run(f, argv, NULL, NULL, NULL);
    return fx_check(f, f->status != 0, "unrecorded native reader refused before execution") && ok;
}

int flow_ci_preparation(struct fixture *f)
{
    int ok = clean_row(f);
    ok = transfer_row(f) && ok;
    return reader_row(f) && ok;
}

int flow_ci_retention_setup(struct fixture *f)
{
    char program[4096];
    if (!native_helper(f, program))
        return 0;
    char hex[65], text[32768];
    size_t bytes;
    if (!file_hash(flow_path(f, "native.site"), hex, &bytes))
        return 0;
    int n = snprintf(text, sizeof text,
                     "{\"format\":\"etude-retention-policy\",\"schema_version\":1,"
                     "\"root\":\"%s\",\"site_sha256\":\"%s\",\"max_bytes\":1000000000,"
                     "\"max_age_days\":14,\"reserve_bytes\":10000,"
                     "\"issue_snapshot_max_age_seconds\":3600,\"baselines\":[],"
                     "\"anchors\":[],\"open_issues\":null}\n",
                     flow_path(f, "retained-evidence"), hex);
    if (n < 0 || (size_t)n >= sizeof text || !flow_put(f, "retention.policy", text))
        return 0;
    fx_env("ETUDE_RETENTION", flow_path(f, "retention.policy"));
    const char *argv[] = {program,
                          "prepare",
                          "retention",
                          "--policy",
                          flow_path(f, "retention.policy"),
                          "--site",
                          flow_path(f, "native.site"),
                          "init",
                          NULL};
    fx_run(f, argv, NULL, NULL, NULL);
    return f->status == 0;
}

int flow_ci_retained_out(struct fixture *f, char *out, size_t size)
{
    char program[4096];
    if (!native_helper(f, program))
        return 0;
    const char *argv[] = {program,
                          "prepare",
                          "retention",
                          "--policy",
                          flow_path(f, "retention.policy"),
                          "--site",
                          flow_path(f, "native.site"),
                          "allocate",
                          NULL};
    fx_run(f, argv, NULL, NULL, NULL);
    if (f->status != 0 || !f->out)
        return 0;
    size_t n = strcspn(f->out, "\r\n");
    if (n == 0 || n >= size)
        return 0;
    memcpy(out, f->out, n);
    out[n] = 0;
    return 1;
}
