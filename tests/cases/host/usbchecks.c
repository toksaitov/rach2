/* Native checks of USB record judgments and the extracted VFS unmount code. */
#include <stdio.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include "etude/exec.h"
#include "../../system/checkers/records.h"
#include "../../host/compat/sanitizer_env_host.h"
#include "sanitizer_tool.h"
#ifdef _WIN32
#include "win/files.h"
#endif

static char *run(const char *path, const char *mode)
{
    struct etude_exec *ex = etude_exec_new();
    struct etude_exec_spec spec;
    struct etude_proc *p;
    const struct etude_exec_result *r;
    char *argv[] = {(char *)path, (char *)mode, NULL};
    char *out = NULL;
    memset(&spec, 0, sizeof(spec));
    spec.attempt_id = mode ? mode : "test";
    spec.argv = argv;
#ifndef _WIN32
    char symbolizer[4096], resolved[4096];
    const char *tree = getenv("ETUDE_TREE");
    if (snprintf(symbolizer, sizeof(symbolizer), "%s/build/tests/host-suite/sanitizer/atos",
                 tree ? tree : ".") >= (int)sizeof(symbolizer))
        goto failed;
#ifdef __APPLE__
    if (host_sanitizer_source(symbolizer, resolved, sizeof(resolved)))
        goto failed;
#else
    resolved[0] = 0;
#endif
    spec.envp = host_sanitizer_environment(resolved);
    if (spec.envp == NULL)
        goto failed;
#else
    spec.envp = environ;
#endif
    spec.output_max = 1u << 20;
    spec.grace_us = 5000000;
    if (!ex || etude_exec_start(ex, &spec, &p) != 0)
        goto failed;
    while (etude_exec_wait(ex, -1) != p)
        ;
    r = etude_proc_result(p);
#ifdef __APPLE__
    if (host_sanitizer_source(symbolizer, resolved, sizeof(resolved))) {
        fprintf(stderr, "usbchecks: native sanitizer tool changed\n");
        etude_proc_free(p);
        goto failed;
    }
#endif
    if (r->end == ETUDE_END_EXITED && r->value == 0 && r->cleanup == ETUDE_CLEANUP_CONFIRMED &&
        r->out.total == r->out.kept) {
        out = malloc(r->out.kept + 1);
        if (out) {
            memcpy(out, r->out.data, r->out.kept);
            out[r->out.kept] = '\0';
        }
    }
    if (!out && r->err.kept)
        fwrite(r->err.data, 1, r->err.kept, stderr);
    etude_proc_free(p);
    etude_exec_free(ex);
    return out;
failed:
    fprintf(stderr, "usbchecks: prepared child: %s\n", strerror(errno));
    if (ex)
        etude_exec_free(ex);
    return NULL;
}

static int combined(const char *a, const char *b, int want)
{
    size_t n = strlen(a) + strlen(b) + 1;
    char *s = malloc(n);
    int ok;
    if (!s)
        return 0;
    snprintf(s, n, "%s%s", a, b);
    ok = check_usb_records(s, 0, 2) == want;
    free(s);
    return ok;
}

static int changed(const char *text, const char *from, const char *to)
{
    const char *at = strstr(text, from);
    char *s;
    size_t before, n;
    int ok;
    if (!at)
        return 0;
    before = (size_t)(at - text);
    n = strlen(text) - strlen(from) + strlen(to) + 1;
    s = malloc(n);
    if (!s)
        return 0;
    memcpy(s, text, before);
    strcpy(s + before, to);
    strcpy(s + before + strlen(to), at + strlen(from));
    ok = !check_usb_records(s, 0, 1);
    free(s);
    return ok;
}

static int diagnostics(const char *record)
{
    static const char text[] = "usb: latency: waiting for a read\n";
    size_t n = strlen(record) + 2 * strlen(text) + 1;
    char *s = malloc(n);
    int ok;
    if (!s)
        return 0;
    snprintf(s, n, "%s%s%s", text, record, text);
    ok = check_usb_records(s, 0, 1) && !check_usb_records(text, 0, 1);
    free(s);
    return ok;
}

static int latency(const char *program)
{
    static const char *const modes[] = {"spin", "timer", "random", "pump", "work", "undispatched"};
    static const int wants[] = {1, 0, 1, 0, 0, 0};
    char *out[6] = {0}, *test = run(program, NULL);
    int ok = test != NULL, i;
    free(test);
    for (i = 0; i < 6; i++) {
        out[i] = run(program, modes[i]);
        if (!out[i] || check_usb_records(out[i], i == 2 || i == 4, 1) != wants[i])
            ok = 0;
    }
    if (out[0] && out[1] && out[3]) {
        ok &= check_usb_records(out[1], 1, 1);
        ok &= combined(out[1], out[0], 1);
        ok &= combined(out[1], out[1], 0);
        ok &= combined(out[0], out[3], 0);
        ok &= !check_usb_records(out[0], 0, 2);
        ok &= changed(out[0], " tracked 1,", " tracked x,");
        ok &= changed(out[0], " dispatch 1,", " dispatch 1, dispatch 1,");
        ok &= changed(out[0], " pumps 0,", "");
        ok &= !check_usb_records("", 0, 1);
        ok &= diagnostics(out[0]);
    } else {
        ok = 0;
    }
    for (i = 0; i < 6; i++)
        free(out[i]);
    return ok;
}

int main(int argc, char **argv)
{
    const char *tree = getenv("ETUDE_TREE"), *id, *assertion, *binary;
    char path[4096];
    int ok;
    if (argc != 2)
        return 2;
    if (!strcmp(argv[1], "latency-checker")) {
        id = "machine.usb.latency-checker";
        assertion = "checker";
        binary = "tests/usbstamp_hosttest";
    } else if (!strcmp(argv[1], "vfs-unmount")) {
        id = "machine.usbdisk.vfs-unmount";
        assertion = "unmount-cases";
        binary = "vfs_unmount";
    } else {
        return 2;
    }
    if (getenv("ETUDE_CASE") && strcmp(getenv("ETUDE_CASE"), id))
        return 2;
    if (!tree)
        tree = ".";
    if (snprintf(path, sizeof(path), "%s/build/tests/host-suite/%s", tree, binary) >=
        (int)sizeof(path))
        return 2;
    printf("@etude 1 begin %s\n", id);
    if (!strcmp(argv[1], "latency-checker")) {
        ok = latency(path);
    } else {
        char *text = run(path, NULL);
        ok = text != NULL;
        if (text)
            fputs(text, stdout);
        free(text);
    }
    printf("@etude 1 assert %s %s\n@etude 1 end 1\n", assertion,
           ok ? "pass" : "fail the prepared check or its record judgment failed");
    return !ok;
}
