/**
 * vdsomusl: check the vDSO, the library the kernel maps into each image so
 * that a program reads the clock without a system call, and its data page.
 *
 * Given `clock=tsc`, the machine must keep its clock from the TSC, as an
 * x86-64 kernel does under `rach2.tscreliable` on QEMU's emulator; given
 * `clock=syscall`, from its other reference, as under `rach2.notsc` and on
 * every i686, where the vDSO's functions make the system call themselves.
 *
 * On x86-64, in order:
 *
 * `auxv`: AT_SYSINFO_EHDR gives the address of an x86-64 ELF header of a
 * shared object. `maps`: /proc/self/maps has `[vvar]`, one page readable
 * alone, just below it, and `[vdso]`, one page readable and executable, at
 * it. `symbols`: the image's hash table and version tables give
 * `__vdso_clock_gettime`, `__vdso_gettimeofday`, `__vdso_time`,
 * `__vdso_getcpu` and `__vdso_clock_getres`, each of version LINUX_2.6, as
 * musl and glibc look them up.
 *
 * `source`: /proc/stat's `vdso_tsc` is 1 under `clock=tsc` and 0 under
 * `clock=syscall`.
 *
 * `agree`: each of the clocks the vDSO answers, real time, the monotonic,
 * raw and boot clocks and the two coarse ones, read through it lies between
 * two readings of the same clock by the system call, and so do
 * gettimeofday's and time's answers. `resolution`: its clock_getres gives
 * what the system call gives for clocks 0 to 7, and both of its calls give
 * -EINVAL for clock 12.
 *
 * `getcpu`: pinned to each processor in turn, the vDSO's getcpu and
 * sched_getcpu give that processor, on node 0.
 *
 * `ordered`: a million readings of the monotonic clock through the vDSO on
 * one processor, and two thousand taken on each processor in turn, are each
 * no earlier than the one before.
 *
 * On i686, `absent`: there is no AT_SYSINFO_EHDR, and maps names neither
 * region; then `source`.
 *
 * On both, `calls`: 100,000 calls of clock_gettime, gettimeofday and time
 * through musl raise /proc/stat's `clock_calls`, the kernel's count of the
 * clock's system calls, by fewer than 1,000 under `clock=tsc`, the other
 * processes' share, and by at least 100,000 under `clock=syscall`.
 *
 * It prints "VDSOMUSL OK" and exits 0 on success, or prints "VDSOMUSL FAIL
 * (detail)" and exits 1 at the first failure. etude's report
 * (probe_report.h), for the case ETUDE_CASE names, or guest.vdso.tsc or
 * guest.vdso.syscall as `clock=` says, has the assertions above in their
 * order, with the branch of the group `arch`, `x86-64` or `i686`.
 */

#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/auxv.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "../probe_args.h"
#include "../probe_report.h"

/* The calls the `calls` row makes, and the most the other processes may add
 * meanwhile where the vDSO answers them. */
#define CALLS  100000L
#define OTHERS 1000L

/* The readings of the `ordered` row on one processor, and on each in turn. */
#define READINGS 1000000L
#define TURNS    2000L

/* The highest clock the kernel defines, and one it does not. */
#define CLOCK_LAST 7
#define CLOCK_NONE 12

/* Whether the clock is to run from the TSC, as `clock=tsc` says. */
static int want_tsc;

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("VDSOMUSL FAIL (%s)\n", what);
    report_stop(what);
    return 1;
}

/**
 * Read the number that follows the word `key` at the start of a line of
 * /proc/stat into `*value`: 0, or -1 when the file or the line is missing.
 */
static int stat_field(const char *key, unsigned long *value)
{
    char line[256];
    size_t n = strlen(key);
    FILE *f = fopen("/proc/stat", "r");
    int found = -1;

    if (f == NULL) {
        return -1;
    }
    while (found != 0 && fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, key, n) == 0 && line[n] == ' ') {
            *value = strtoul(line + n + 1, NULL, 10);
            found = 0;
        }
    }
    fclose(f);
    return found;
}

/**
 * Find the line of /proc/self/maps whose name is `name`: 0 with its start,
 * end and permissions in `*lo`, `*hi` and `perm`, of five bytes, or -1 when
 * there is none.
 */
static int maps_region(const char *name, unsigned long *lo, unsigned long *hi, char *perm)
{
    char line[512];
    FILE *f = fopen("/proc/self/maps", "r");
    int found = -1;

    if (f == NULL) {
        return -1;
    }
    while (found != 0 && fgets(line, sizeof(line), f) != NULL) {
        char *nl = strchr(line, '\n');
        size_t len;
        size_t n = strlen(name);

        if (nl != NULL) {
            *nl = '\0';
        }
        len = strlen(line);
        if (len > n && strcmp(line + len - n, name) == 0 && line[len - n - 1] == ' ' &&
            sscanf(line, "%lx-%lx %4s", lo, hi, perm) == 3) {
            found = 0;
        }
    }
    fclose(f);
    return found;
}

/** Make the calling thread run on processor `cpu` alone: 0, or -1. */
static int pin(int cpu)
{
    cpu_set_t set;

    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    return sched_setaffinity(0, sizeof(set), &set);
}

/** The processors online, at least 1. */
static int processors(void)
{
    long n = sysconf(_SC_NPROCESSORS_ONLN);

    return n < 1 ? 1 : (int)n;
}

/**
 * Make 100,000 calls of the clock through musl, a quarter each of
 * clock_gettime of real time and of the monotonic clock, gettimeofday and
 * time, and check the kernel's count of the clock's system calls over them.
 */
static int calls(void)
{
    unsigned long before;
    unsigned long after;
    unsigned long delta;
    struct timespec ts;
    struct timeval tv;
    long i;

    if (stat_field("clock_calls", &before) != 0) {
        return fail("/proc/stat has no clock_calls line");
    }
    for (i = 0; i < CALLS / 4; i++) {
        clock_gettime(CLOCK_REALTIME, &ts);
        clock_gettime(CLOCK_MONOTONIC, &ts);
        gettimeofday(&tv, NULL);
        (void)time(NULL);
    }
    if (stat_field("clock_calls", &after) != 0) {
        return fail("/proc/stat has no clock_calls line");
    }
    delta = (unsigned long)(uint32_t)(after - before);
    printf("vdsomusl: calls: %ld through musl, %lu system calls\n", CALLS, delta);
    if (want_tsc && delta >= (unsigned long)OTHERS) {
        return fail("the clock calls made system calls where the vDSO reads the TSC");
    }
    if (!want_tsc && delta < (unsigned long)CALLS) {
        return fail("fewer system calls of the clock than calls where the vDSO has no TSC");
    }
    report_held();
    return 0;
}

/** Check that /proc/stat's `vdso_tsc` is what `clock=` says. */
static int source(void)
{
    unsigned long tsc;

    if (stat_field("vdso_tsc", &tsc) != 0) {
        return fail("/proc/stat has no vdso_tsc line");
    }
    if (tsc != (unsigned long)want_tsc) {
        return fail(want_tsc ? "the clock does not run from the TSC"
                             : "the clock runs from the TSC under the system call's case");
    }
    report_held();
    return 0;
}

#if defined __x86_64__

typedef long (*gettime_fn)(long clk, struct timespec *ts);
typedef long (*gettimeofday_fn)(struct timeval *tv, void *tz);
typedef long (*time_fn)(long *t);
typedef long (*getcpu_fn)(unsigned *cpu, unsigned *node, void *cache);

/* The vDSO's functions, as `symbols` finds them. */
static gettime_fn vdso_clock_gettime;
static gettimeofday_fn vdso_gettimeofday;
static time_fn vdso_time;
static getcpu_fn vdso_getcpu;
static gettime_fn vdso_clock_getres;

/* The vDSO's tables, as `symbols` finds them through its dynamic section. */
static const unsigned char *image;
static const Elf64_Word *hashtab;
static const Elf64_Sym *symtab;
static const char *strtab;
static const Elf64_Half *versym;
static const Elf64_Verdef *verdef;

/** Check that AT_SYSINFO_EHDR gives an x86-64 shared object's ELF header. */
static int auxv(void)
{
    const Elf64_Ehdr *eh;

    image = (const unsigned char *)getauxval(AT_SYSINFO_EHDR);
    if (image == NULL) {
        return fail("no AT_SYSINFO_EHDR in the auxiliary vector");
    }
    eh = (const Elf64_Ehdr *)image;
    if (memcmp(eh->e_ident, ELFMAG, SELFMAG) != 0 || eh->e_ident[EI_CLASS] != ELFCLASS64 ||
        eh->e_type != ET_DYN || eh->e_machine != EM_X86_64) {
        return fail("AT_SYSINFO_EHDR is not an x86-64 shared object's ELF header");
    }
    report_held();
    return 0;
}

/** Check maps' `[vvar]` and `[vdso]` against AT_SYSINFO_EHDR. */
static int maps(void)
{
    unsigned long lo;
    unsigned long hi;
    unsigned long at = (unsigned long)image;
    char perm[5];

    if (maps_region("[vdso]", &lo, &hi, perm) != 0 || lo != at || hi != at + 4096 ||
        strcmp(perm, "r-xp") != 0) {
        return fail("maps has no [vdso] of one r-xp page at AT_SYSINFO_EHDR");
    }
    if (maps_region("[vvar]", &lo, &hi, perm) != 0 || lo != at - 4096 || hi != at ||
        strcmp(perm, "r--p") != 0) {
        return fail("maps has no [vvar] of one r--p page below the vDSO");
    }
    report_held();
    return 0;
}

/** The SysV ELF hash of `name`, which DT_HASH's buckets are chosen by. */
static unsigned long elf_hash(const char *name)
{
    unsigned long h = 0;
    unsigned long g;

    while (*name != '\0') {
        h = (h << 4) + (unsigned char)*name++;
        g = h & 0xf0000000ul;
        if (g != 0) {
            h ^= g >> 24;
        }
        h &= ~g;
    }
    return h;
}

/** Whether version index `index` of the image's definitions is named
 * `version`, as musl's checkver asks. */
static int version_is(Elf64_Half index, const char *version)
{
    const Elf64_Verdef *d = verdef;

    index &= 0x7fff;
    for (;;) {
        if (!(d->vd_flags & VER_FLG_BASE) && (d->vd_ndx & 0x7fff) == index) {
            const Elf64_Verdaux *a = (const Elf64_Verdaux *)((const char *)d + d->vd_aux);

            return strcmp(strtab + a->vda_name, version) == 0;
        }
        if (d->vd_next == 0) {
            return 0;
        }
        d = (const Elf64_Verdef *)((const char *)d + d->vd_next);
    }
}

/** The address of the image's defined function `name` of version
 * LINUX_2.6, found through its hash table, or NULL. */
static void *lookup(const char *name)
{
    Elf64_Word nbucket = hashtab[0];
    const Elf64_Word *bucket = hashtab + 2;
    const Elf64_Word *chain = bucket + nbucket;
    Elf64_Word i;

    for (i = bucket[elf_hash(name) % nbucket]; i != STN_UNDEF; i = chain[i]) {
        const Elf64_Sym *s = symtab + i;

        if (ELF64_ST_TYPE(s->st_info) == STT_FUNC && s->st_shndx != SHN_UNDEF &&
            strcmp(strtab + s->st_name, name) == 0 && version_is(versym[i], "LINUX_2.6")) {
            return (void *)(image + s->st_value);
        }
    }
    return NULL;
}

/** Find the dynamic section's tables and the five functions through them. */
static int symbols(void)
{
    const Elf64_Ehdr *eh = (const Elf64_Ehdr *)image;
    const Elf64_Phdr *ph = (const Elf64_Phdr *)(image + eh->e_phoff);
    const Elf64_Dyn *dyn = NULL;
    int i;

    for (i = 0; i < eh->e_phnum; i++) {
        if (ph[i].p_type == PT_DYNAMIC) {
            dyn = (const Elf64_Dyn *)(image + ph[i].p_offset);
        }
    }
    for (; dyn != NULL && dyn->d_tag != DT_NULL; dyn++) {
        const unsigned char *p = image + dyn->d_un.d_ptr;

        switch (dyn->d_tag) {
        case DT_HASH:
            hashtab = (const Elf64_Word *)p;
            break;
        case DT_SYMTAB:
            symtab = (const Elf64_Sym *)p;
            break;
        case DT_STRTAB:
            strtab = (const char *)p;
            break;
        case DT_VERSYM:
            versym = (const Elf64_Half *)p;
            break;
        case DT_VERDEF:
            verdef = (const Elf64_Verdef *)p;
            break;
        default:
            break;
        }
    }
    if (hashtab == NULL || symtab == NULL || strtab == NULL || versym == NULL || verdef == NULL) {
        return fail("the vDSO lacks a hash, symbol, string or version table");
    }
    vdso_clock_gettime = (gettime_fn)lookup("__vdso_clock_gettime");
    vdso_gettimeofday = (gettimeofday_fn)lookup("__vdso_gettimeofday");
    vdso_time = (time_fn)lookup("__vdso_time");
    vdso_getcpu = (getcpu_fn)lookup("__vdso_getcpu");
    vdso_clock_getres = (gettime_fn)lookup("__vdso_clock_getres");
    if (vdso_clock_gettime == NULL || vdso_gettimeofday == NULL || vdso_time == NULL ||
        vdso_getcpu == NULL || vdso_clock_getres == NULL) {
        return fail("a __vdso_ function of version LINUX_2.6 is missing");
    }
    report_held();
    return 0;
}

/** A timespec in nanoseconds. */
static long long ns(const struct timespec *ts)
{
    return (long long)ts->tv_sec * 1000000000LL + ts->tv_nsec;
}

/** Clock `clk` by the system call, in nanoseconds, or -1. */
static long long sys_ns(long clk)
{
    struct timespec ts;

    return syscall(SYS_clock_gettime, clk, &ts) == 0 ? ns(&ts) : -1;
}

/** Check the vDSO's readings of each clock and of gettimeofday and time
 * against the system call's. */
static int agree(void)
{
    static const long clocks[] = {CLOCK_REALTIME,         CLOCK_MONOTONIC,
                                  CLOCK_MONOTONIC_RAW,    CLOCK_REALTIME_COARSE,
                                  CLOCK_MONOTONIC_COARSE, CLOCK_BOOTTIME};
    struct timespec ts;
    struct timeval tv;
    long long before;
    long long after;
    long long x;
    long t;
    unsigned i;

    for (i = 0; i < sizeof(clocks) / sizeof(clocks[0]); i++) {
        before = sys_ns(clocks[i]);
        if (vdso_clock_gettime(clocks[i], &ts) != 0) {
            return fail("the vDSO's clock_gettime failed");
        }
        after = sys_ns(clocks[i]);
        x = ns(&ts);
        if (before < 0 || x < before || x > after || ts.tv_nsec < 0 || ts.tv_nsec >= 1000000000L) {
            printf("vdsomusl: clock %ld: system call %lld, vDSO %lld, system call %lld\n",
                   clocks[i], before, x, after);
            return fail("a clock read through the vDSO is not between two system calls'");
        }
    }
    before = sys_ns(CLOCK_REALTIME) / 1000;
    if (vdso_gettimeofday(&tv, NULL) != 0) {
        return fail("the vDSO's gettimeofday failed");
    }
    after = sys_ns(CLOCK_REALTIME) / 1000;
    x = (long long)tv.tv_sec * 1000000LL + tv.tv_usec;
    if (x < before || x > after || tv.tv_usec < 0 || tv.tv_usec >= 1000000L) {
        return fail("the vDSO's gettimeofday is not between two system calls' real time");
    }
    before = sys_ns(CLOCK_REALTIME) / 1000000000LL;
    t = vdso_time(NULL);
    after = sys_ns(CLOCK_REALTIME) / 1000000000LL;
    if (t < before || t > after) {
        return fail("the vDSO's time is not between two system calls' real time");
    }
    report_held();
    return 0;
}

/** Check the vDSO's clock_getres against the system call's, and clock 12. */
static int resolution(void)
{
    struct timespec v;
    struct timespec s;
    long clk;

    for (clk = 0; clk <= CLOCK_LAST; clk++) {
        if (vdso_clock_getres(clk, &v) != 0 || syscall(SYS_clock_getres, clk, &s) != 0 ||
            v.tv_sec != s.tv_sec || v.tv_nsec != s.tv_nsec) {
            printf("vdsomusl: clock %ld: vDSO %ld ns, system call %ld ns\n", clk, (long)v.tv_nsec,
                   (long)s.tv_nsec);
            return fail("the vDSO's clock_getres differs from the system call's");
        }
    }
    if (vdso_clock_getres(CLOCK_NONE, &v) != -EINVAL ||
        vdso_clock_gettime(CLOCK_NONE, &v) != -EINVAL) {
        return fail("clock 12 through the vDSO is not -EINVAL");
    }
    report_held();
    return 0;
}

/** Check the vDSO's getcpu and sched_getcpu on each processor in turn. */
static int getcpu_row(void)
{
    int n = processors();
    int i;

    for (i = 0; i < n; i++) {
        unsigned cpu = 99;
        unsigned node = 99;

        if (pin(i) != 0) {
            return fail("sched_setaffinity refused a processor that is online");
        }
        if (vdso_getcpu(&cpu, &node, NULL) != 0 || cpu != (unsigned)i || node != 0 ||
            sched_getcpu() != i) {
            printf("vdsomusl: pinned to %d: getcpu %u node %u, sched_getcpu %d\n", i, cpu, node,
                   sched_getcpu());
            return fail("getcpu does not give the processor the thread is pinned to");
        }
    }
    report_held();
    return 0;
}

/** Check that the vDSO's monotonic readings never go back, on one
 * processor and across all of them. */
static int ordered(void)
{
    struct timespec ts;
    long long last;
    long long x;
    int n = processors();
    long i;

    pin(0);
    vdso_clock_gettime(CLOCK_MONOTONIC, &ts);
    last = ns(&ts);
    for (i = 0; i < READINGS; i++) {
        vdso_clock_gettime(CLOCK_MONOTONIC, &ts);
        x = ns(&ts);
        if (x < last) {
            printf("vdsomusl: reading %ld: %lld after %lld\n", i, x, last);
            return fail("a monotonic reading through the vDSO went back");
        }
        last = x;
    }
    for (i = 0; i < TURNS * n; i++) {
        if (pin((int)(i % n)) != 0) {
            return fail("sched_setaffinity refused a processor that is online");
        }
        vdso_clock_gettime(CLOCK_MONOTONIC, &ts);
        x = ns(&ts);
        if (x < last) {
            printf("vdsomusl: on cpu %ld: %lld after %lld\n", i % n, x, last);
            return fail("a monotonic reading through the vDSO went back across processors");
        }
        last = x;
    }
    report_held();
    return 0;
}

/** The rows of x86-64's vDSO, up to `source`. */
static int rows(void)
{
    if (auxv() != 0 || maps() != 0 || symbols() != 0 || source() != 0 || agree() != 0 ||
        resolution() != 0 || getcpu_row() != 0 || ordered() != 0) {
        return 1;
    }
    return 0;
}

#else

/** Check that the i686 image has no vDSO, then `source`. */
static int rows(void)
{
    unsigned long lo;
    unsigned long hi;
    char perm[5];

    if (getauxval(AT_SYSINFO_EHDR) != 0 || maps_region("[vdso]", &lo, &hi, perm) == 0 ||
        maps_region("[vvar]", &lo, &hi, perm) == 0) {
        return fail("an i686 image has a vDSO");
    }
    report_held();
    return source();
}

#endif

/** Begin the report, with the branch of the architecture, and plan its
 * assertions in the order they are checked. */
static void plan(void)
{
#if defined __x86_64__
    static const char *const ids[] = {"auxv",       "maps",   "symbols", "source", "agree",
                                      "resolution", "getcpu", "ordered", "calls"};
#else
    static const char *const ids[] = {"absent", "source", "calls"};
#endif
    unsigned i;

    report_begin(report_case(environ, want_tsc ? "guest.vdso.tsc" : "guest.vdso.syscall"));
#if defined __x86_64__
    report_branch("arch", "x86-64");
#else
    report_branch("arch", "i686");
#endif
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        report_plan(ids[i]);
    }
}

int main(int argc, char **argv)
{
    const char *mode;

    probe_args(argc, argv);
    mode = probe_arg("clock", "syscall");
    want_tsc = strcmp(mode, "tsc") == 0;
    plan();
    if (!want_tsc && strcmp(mode, "syscall") != 0) {
        return fail("clock= is neither tsc nor syscall");
    }
    if (rows() != 0 || calls() != 0) {
        return 1;
    }
    report_end();
    printf("VDSOMUSL OK\n");
    return 0;
}
