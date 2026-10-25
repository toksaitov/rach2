/* wincollectionfix.c: native artifact and QEMU checks for the collection. */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "files.h"
#include "winfixture.h"
#include "etude/artifact.h"
#include "etude/ledger.h"
#include "etude/qemu.h"

/** Form a bounded path; report failure before a fixture uses it. */
static int path_of(char *out, size_t size, const char *format, ...)
{
    va_list args;
    int n;
    va_start(args, format);
    n = vsnprintf(out, size, format, args);
    va_end(args);
    if (n < 0 || (size_t)n >= size) {
        errno = ENAMETOOLONG;
        return -1;
    }
    return 0;
}

/** Remove this fixture's private tree, without following reparse points. */
static void discard(const char *path)
{
    struct stat st;
    DIR *d;
    struct dirent *e;
    char child[4096];
    if (lstat(path, &st) != 0)
        return;
    if (!S_ISDIR(st.st_mode)) {
        unlink(path);
        return;
    }
    d = opendir(path);
    if (d == NULL)
        return;
    while ((e = readdir(d)) != NULL) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, ".."))
            continue;
        if (path_of(child, sizeof child, "%s/%s", path, e->d_name) == 0)
            discard(child);
    }
    closedir(d);
    rmdir(path);
}

/** Check binary publication, read-only objects and 64-bit sparse offsets. */
int fx_collection_files(void)
{
    char work[4096], path[4096], copy[4096], store[4096], object[4096], hex[65], second[65];
    struct etude_store *s = NULL;
    struct stat st;
    uint64_t size;
    int fd = -1, rc = 1;
    const char *why = "the private directory could not be made";
    path_of(work, sizeof work, "%s/rach2-files.XXXXXX", scratch);
    if (mkdtemp(work) == NULL)
        goto done;
    path_of(path, sizeof path, "%s/bytes-\xc3\xa9", work);
    path_of(copy, sizeof copy, "%s/copy", work);
    path_of(store, sizeof store, "%s/store", work);
    why = "binary bytes or their digest did not survive publication";
    fd = open(path, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd < 0 || write(fd, "\x00\x0a\x0d\x1a\xff", 5) != 5 || fsync(fd) != 0)
        goto done;
    close(fd);
    fd = -1;
    s = etude_store_open(store);
    if (s == NULL || etude_store_put(s, path, NULL, hex, &size) != 0 || size != 5 ||
        etude_store_copy(s, hex, copy) != 0 || etude_digest_file(NULL, copy, &st, second) != 0 ||
        strcmp(hex, second))
        goto done;
    path_of(object, sizeof object, "%s/objects/%s", store, hex);
    why = "the published object is writable after its partial link was removed";
    if (stat(object, &st) != 0 || (st.st_mode & 0222))
        goto done;
    why = "a repeated publication or an incorrect digest was accepted incorrectly";
    if (etude_store_put(s, path, hex, second, &size) != 0 || strcmp(hex, second))
        goto done;
    memset(second, '0', 64);
    second[64] = '\0';
    if (etude_store_put(s, path, second, hex, &size) == 0 || errno != EBADMSG)
        goto done;
    why = "a file offset or size was truncated at 4 GiB";
    fd = open(copy, O_RDWR | O_TRUNC);
    if (fd < 0 || lseek(fd, INT64_C(5) << 30, SEEK_SET) != (INT64_C(5) << 30) ||
        write(fd, "x", 1) != 1 || fstat(fd, &st) != 0 || st.st_size != (INT64_C(5) << 30) + 1)
        goto done;
    rc = 0;
done:
    if (fd >= 0)
        close(fd);
    if (s != NULL)
        etude_store_close(s);
    discard(work);
    return rc ? failed("%s: %s", why, strerror(errno)) : 0;
}

struct registration {
    struct etude_ledger *ledger;
    int refused;
    struct etude_pid process;
};

/** Register the held process, or refuse it before its first instruction. */
static int register_qemu(void *ctx, const char *id, const struct etude_pid *keeper,
                         const struct etude_pid *process)
{
    struct registration *r = ctx;
    r->process = *process;
    return r->refused ? EPERM : etude_ledger_register(r->ledger, id, keeper, process);
}

/** Check that missing and refused processes leave nothing running. */
static int unrun(struct etude_qemu *q, struct etude_machine_spec *spec,
                 struct etude_qemu_site *site, struct registration *reg, const char *program)
{
    struct etude_machine *m = NULL;
    struct etude_pid state;
    site->qemu_x86_64 = "C:/etude-no-such-program.exe";
    if (etude_qemu_start(q, spec, &m) == 0 || m != NULL)
        return -1;
    site->qemu_x86_64 = program;
    reg->refused = 1;
    if (etude_qemu_start(q, spec, &m) == 0 || m != NULL ||
        etude_process_state(reg->process.pid, &state) != 0)
        return -1;
    reg->refused = 0;
    return 0;
}

/** Wait for the selected accelerator to start the machine. */
static int running(struct etude_qemu *q, struct etude_machine *m,
                   const struct etude_qemu_site *site)
{
    const struct etude_machine_result *r;
    uint64_t until = etude_win_mono_us() + (uint64_t)BOUND * 1000u;
    while ((r = etude_machine_result(m))->state == ETUDE_MACHINE_STARTING &&
           etude_win_mono_us() < until)
        etude_qemu_wait(q, 10);
    if (r->state == ETUDE_MACHINE_RUNNING && r->accel != NULL &&
        strcmp(r->accel, etude_accel_name(site->accel)) == 0)
        return 0;
    if (r->qemu->err.data != NULL)
        fprintf(stderr, "%s", r->qemu->err.data);
    return -1;
}

/** Suspend, resume and cancel the machine, then release its confirmed claim. */
static int finish(struct etude_qemu *q, struct etude_machine *m, struct etude_ledger *ledger)
{
    const struct etude_machine_result *r;
    uint64_t until = etude_win_mono_us() + (uint64_t)BOUND * 1000u;
    if (etude_machine_suspend(m) != 0 ||
        etude_machine_result(m)->qemu->suspension != ETUDE_SUSPEND_STOPPED ||
        etude_machine_resume(m) != 0)
        return -1;
    etude_machine_cancel(m);
    while (etude_machine_result(m)->state != ETUDE_MACHINE_ENDED && etude_win_mono_us() < until)
        etude_qemu_wait(q, 10);
    r = etude_machine_result(m);
    if (r->state != ETUDE_MACHINE_ENDED || r->qemu->cleanup != ETUDE_CLEANUP_CONFIRMED ||
        r->qemu->cpu_us.quality != ETUDE_QUALITY_EXACT)
        return -1;
    return etude_ledger_finish(ledger, "windows-qemu", r->qemu->cleanup);
}

/** Refuse an unavailable accelerator without starting another. */
static int refused_accel(struct etude_qemu *q, struct etude_machine_spec *spec,
                         struct etude_qemu_site *site, struct etude_ledger *ledger,
                         const struct etude_ledger_res *capacity)
{
    struct etude_machine *m = NULL;
    const struct etude_machine_result *r;
    uint64_t until = etude_win_mono_us() + (uint64_t)BOUND * 1000u;
    site->accel = ETUDE_ACCEL_KVM;
    spec->attempt_id = "windows-refused-accel";
    if (etude_ledger_reserve(ledger, spec->attempt_id, capacity, 1, capacity, 1) != 0 ||
        etude_qemu_start(q, spec, &m) != 0)
        return -1;
    while (etude_machine_result(m)->state != ETUDE_MACHINE_ENDED && etude_win_mono_us() < until)
        etude_qemu_wait(q, 10);
    r = etude_machine_result(m);
    if (r->state != ETUDE_MACHINE_ENDED || r->end != ETUDE_END_ACCEL_FAILED || r->running_us != 0 ||
        r->qemu->cleanup != ETUDE_CLEANUP_CONFIRMED)
        return -1;
    return etude_ledger_finish(ledger, spec->attempt_id, r->qemu->cleanup);
}

/** Make the real QEMU disk exceed the legacy Windows path limit. */
static int long_directory(char *path, size_t size, const char *root)
{
    char next[4096];

    if (path_of(path, size, "%s", root) != 0)
        return -1;
    while (strlen(path) < MAX_PATH) {
        if (path_of(next, sizeof next, "%s/long-file-operands", path) != 0 ||
            mkdir(next, 0700) != 0 || path_of(path, size, "%s", next) != 0)
            return -1;
    }
    return 0;
}

/** Check registration, accelerator selection, suspension and cleanup. */
int fx_collection_qemu(void)
{
    char work[4096], diskdir[4096], disk[4096], ledger[4096], program[4096];
    struct etude_qemu *q = NULL;
    struct etude_machine *m = NULL;
    struct etude_qemu_profile p;
    struct etude_qemu_site site;
    struct etude_machine_spec spec;
    struct etude_qemu_disk disks[2];
    struct registration reg;
    struct etude_ledger_res capacity = {"memory_bytes", UINT64_C(256) << 20};
    const char *given = getenv("ETUDE_WIN_QEMU"), *accel = getenv("ETUDE_WIN_ACCEL");
    const char *why = "QEMU is not on PATH; set ETUDE_WIN_QEMU";
    int fd = -1, rc = 1;
    memset(&reg, 0, sizeof reg);
    path_of(work, sizeof work, "%s/rach2-qemu-check.XXXXXX", scratch);
    if (mkdtemp(work) == NULL)
        goto done;
    if (given == NULL) {
        if (etude_win_program("qemu-system-x86_64", program, sizeof program) != 0)
            goto done;
        given = program;
    }
    why = "the long private disk directory could not be made";
    if (long_directory(diskdir, sizeof diskdir, work) != 0)
        goto done;
    path_of(disk, sizeof disk, "%s/empty.raw", diskdir);
    path_of(ledger, sizeof ledger, "%s/ledger", work);
    fd = open(disk, O_RDWR | O_CREAT | O_EXCL, 0600);
    why = "the private disk or ledger could not be made";
    if (fd < 0 || ftruncate(fd, 1 << 20) != 0)
        goto done;
    close(fd);
    fd = -1;
    reg.ledger = etude_ledger_open(ledger);
    q = etude_qemu_new();
    if (reg.ledger == NULL || q == NULL)
        goto done;
    memset(&p, 0, sizeof p);
    memset(&site, 0, sizeof site);
    memset(&spec, 0, sizeof spec);
    memset(disks, 0, sizeof disks);
    disks[0].path = disk;
    disks[0].bus = ETUDE_BUS_IDE;
    p.architecture = "x86_64";
    p.cpu = "max";
    p.processors = 1;
    p.memory_bytes = UINT64_C(128) << 20;
    p.firmware = ETUDE_FIRMWARE_SEABIOS;
    p.disks = disks;
    if (etude_accel_parse(accel != NULL ? accel : "whpx", &site.accel) != 0)
        goto done;
    site.qemu_i686 = site.qemu_x86_64 = given;
    spec.attempt_id = "windows-qemu";
    spec.profile = &p;
    spec.site = &site;
    spec.console_max = 65536;
    spec.grace_us = UINT64_C(1000000);
    spec.reg = register_qemu;
    spec.reg_ctx = &reg;
    why = "a missing executable or a refused registration left a process";
    if (unrun(q, &spec, &site, &reg, given) != 0)
        goto done;
    why = "the machine failed to start under its explicitly requested accelerator";
    if (etude_ledger_reserve(reg.ledger, spec.attempt_id, &capacity, 1, &capacity, 1) != 0 ||
        etude_qemu_start(q, &spec, &m) != 0)
        goto done;
    if (running(q, m, &site) != 0)
        goto done;
    why = "the machine's suspension, accounting or cleanup failed";
    if (finish(q, m, reg.ledger) != 0)
        goto done;
    why = "an unavailable accelerator fell back or left its claim";
    if (refused_accel(q, &spec, &site, reg.ledger, &capacity) != 0)
        goto done;
    rc = 0;
done:
    if (fd >= 0)
        close(fd);
    if (q != NULL)
        etude_qemu_free(q);
    if (reg.ledger != NULL)
        etude_ledger_close(reg.ledger);
    discard(work);
    return rc ? failed("%s: %s", why, strerror(errno)) : 0;
}
