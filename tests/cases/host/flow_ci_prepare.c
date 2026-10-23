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
    int n = snprintf(text, sizeof text,
                     "etude-prepare 1\ntree %.*s\nfile %s %lu 0 0 %s\n",
                     (int)(base - program), program, hex, (unsigned long)size, base + 1);
    if (n < 0 || (size_t)n >= sizeof text || !flow_put(f, "reader.record", text))
        return 0;
    fx_env("ETUDE_REPORT_READER_RECORD", flow_path(f, "reader.record"));
    return 1;
}

static int clean_row(struct fixture *f)
{
    const char *files[] = {"build/rach2/kernel", "build/x86_64/rach2-o2/kernel",
                            "build/tools/check", "build/tests/host-suite/old",
                            "build/vendor/gnu/keep", "build/x86_64/vendor-work/keep",
                            "build/tests/failed/keep", "build/host-tools/native/keep",
                            "build/tests/host-suite/current/grant"};
    for (size_t i = 0; i < sizeof files / sizeof *files; i++) {
        char name[256];
        snprintf(name, sizeof name, "tree/%s", files[i]);
        if (!flow_put(f, name, "bytes"))
            return 0;
    }
    if (!flow_put(f, "tree/tests/Makefile", "PROGS = probe\n") ||
        !flow_put(f, "tree/Makefile", "# fixture\n"))
        return 0;
    const char *argv[] = {"python3", flow_source(f, "tools/ci/clean_inhouse.py"),
                           flow_path(f, "tree"), "both",
                           flow_path(f, "tree/build/tests/host-suite/current"), NULL};
    fx_run(f, argv, NULL, NULL, NULL);
    int ok = f->status == 0;
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
    const char *script = flow_source(f, "tools/ci/native_evidence.py");
    const char *stage[] = {"python3", script, "stage", flow_path(f, "transfer"),
                            flow_path(f, "shared"), NULL};
    fx_run(f, stage, NULL, NULL, NULL);
    int ok = f->status == 0 && !fx_exists(flow_path(f, "shared/objects/large-image"));
    const char *restore[] = {"python3", script, "restore", flow_path(f, "shared"),
                              flow_path(f, "restored"), NULL};
    fx_run(f, restore, NULL, NULL, NULL);
    ok = ok && f->status == 0 &&
         fx_equal(flow_path(f, "restored/history/run.jsonl"), "complete history\n") &&
         fx_equal(flow_path(f, "restored/store/blob"), "assertion evidence\n");
    ok = fx_check(f, ok, "shared evidence survives transfer without executable objects");
    if (!flow_put(f, "shared/store/blob", "corrupt\n"))
        return 0;
    const char *bad[] = {"python3", script, "restore", flow_path(f, "shared"),
                          flow_path(f, "corrupt-restored"), NULL};
    fx_run(f, bad, NULL, NULL, NULL);
    return fx_check(f, f->status != 0 && !fx_exists(flow_path(f, "corrupt-restored")),
                    "corrupt shared evidence is refused before publication") && ok;
}

static int reader_row(struct fixture *f)
{
    if (!flow_ci_reader_record(f, f->self))
        return 0;
    const char *script = flow_source(f, "tools/ci/native_reader.py");
    const char *argv[] = {"python3", script, f->self, flow_path(f, "reader.record"), NULL};
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
