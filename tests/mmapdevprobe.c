/**
 * mmapdevprobe: mappings of device memory through /dev/mem, the first user
 * of a file's mmap operation, as Linux gives them to root.
 *
 * The probe maps the local APIC's registers shared and reads its version, a
 * page of the first megabyte and compares it with pread's bytes, and reads
 * the mapping's line in /proc/self/maps. It checks the refusals: a private
 * mapping (EINVAL), a frame of RAM (EPERM), a writable shared mapping
 * through a descriptor open for reading alone (EACCES), and mprotect's
 * write on a read-only one of those (EACCES). A fork's child reads the same
 * version through the parent's mapping; after MADV_DONTFORK a child that
 * touches the address dies of SIGSEGV, since it has no mapping there.
 * MADV_DOFORK and MADV_DONTNEED are EINVAL over device memory, mremap's
 * growth is EFAULT, and a move with MREMAP_FIXED carries the registers to
 * the new address. Last, munmap.
 *
 * Prints "MMAPDEVPROBE OK" and exits 0 when every check passes, and
 * otherwise "MMAPDEVPROBE FAIL: what" and exits 1 at the first failure.
 * etude's report (probe_report.h), for the case ETUDE_CASE names, has an
 * assertion for each check in order: `open`, `apic`, `maps`, `low`,
 * `refused`, `nowrite`, `fork`, `dontfork`, `advice`, `mremap` and
 * `munmap`. Freestanding; direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_report.h"

#define PAGE 4096L

#define O_RDONLY 0
#define O_RDWR   2

#define PROT_READ   0x1
#define PROT_WRITE  0x2
#define MAP_SHARED  0x01
#define MAP_PRIVATE 0x02
#define MAP_ANON    0x20

#define MREMAP_MAYMOVE 1
#define MREMAP_FIXED   2

#define MADV_DONTNEED 4
#define MADV_DONTFORK 10
#define MADV_DOFORK   11

#define EPERM  1
#define EFAULT 14
#define EACCES 13
#define EINVAL 22

#define SIGSEGV 11

#define WIFSIGNALED(s) (((s) & 0x7F) != 0 && ((s) & 0x7F) != 0x7F)
#define WTERMSIG(s)    ((s) & 0x7F)
#define WEXITSTATUS(s) (((s) >> 8) & 0xFF)

#if defined __x86_64__
#define SYS_MADVISE 28
#else
#define SYS_MADVISE 219
#endif

/* The local APIC's registers, where QEMU and the PC put them, and the
 * offset of its version register. */
#define APIC_BASE    0xFEE00000UL
#define APIC_VERSION 0x30
/* A page of the BIOS area in the first megabyte. */
#define LOW_PAGE     0xF0000L
/* A frame of RAM, at 32 MiB. */
#define RAM_PAGE     0x2000000L

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT, (long)status);
    __builtin_unreachable();
}

/** Say `what` failed, fail the current assertion with it, and exit 1. */
static void fail(const char *what)
{
    probe_line("MMAPDEVPROBE FAIL: ", what, 0);
    report_stop(what);
    sys_exit(1);
}

/** A mapping of `len` bytes of `fd` at the byte offset `off`, shared or
 * not by `flags`: its address, or a negated errno. */
static long map_mem(int fd, long len, int prot, int flags, unsigned long off)
{
    return probe_mmap(0, len, prot, flags, fd, (long)off);
}

/** The version byte of the local APIC whose registers are mapped at `at`. */
static unsigned apic_version(long at)
{
    return ((volatile unsigned *)at)[APIC_VERSION / 4] & 0xFFu;
}

/** Write `v` as lowercase hex of at least eight digits into `out`, with a
 * terminating NUL, as /proc/PID/maps gives an address. */
static void hex8(char *out, unsigned long v)
{
    char d[20];
    int n = 0, i;

    do {
        d[n++] = "0123456789abcdef"[v & 0xF];
        v >>= 4;
    } while (v != 0 || n < 8);
    for (i = 0; i < n; i++) {
        out[i] = d[n - 1 - i];
    }
    out[n] = '\0';
}

/** Whether `s` holds `t` at or after its start. */
static int contains(const char *s, const char *t)
{
    const char *a, *b;

    for (; *s; s++) {
        for (a = s, b = t; *a && *b && *a == *b; a++, b++) {
        }
        if (!*b) {
            return 1;
        }
    }
    return 0;
}

/** Whether `s` begins with `t`. */
static int begins(const char *s, const char *t)
{
    while (*t && *s == *t) {
        s++;
        t++;
    }
    return *t == '\0';
}

/**
 * Whether /proc/self/maps has a line for the mapping at `at` that reads
 * `rw-s fee00000` and names /dev/mem.
 */
static int maps_names(long at)
{
    static char buf[16384];
    char want[24], *line, *end;
    long n, got = 0;
    int fd = (int)syscall3(SYS_OPEN, (long)"/proc/self/maps", O_RDONLY, 0);

    if (fd < 0) {
        return 0;
    }
    while ((n = syscall3(SYS_READ, fd, (long)(buf + got), (long)sizeof(buf) - 1 - got)) > 0) {
        got += n;
    }
    syscall1(SYS_CLOSE, fd);
    buf[got] = '\0';
    hex8(want, (unsigned long)at);
    for (line = buf; *line; line = end + 1) {
        for (end = line; *end && *end != '\n'; end++) {
        }
        if (!*end) {
            break;
        }
        *end = '\0';
        if (begins(line, want) && contains(line, " rw-s fee00000 ") && contains(line, "/dev/mem")) {
            return 1;
        }
    }
    return 0;
}

/* The version the parent read, which a child compares with its own read. */
static unsigned parent_version;

/**
 * Fork a child that reads the APIC's version at `at` and exits 0 when it is
 * the parent's; the wait status, or -1 when the fork or the wait fails.
 */
static int child_reads(long at)
{
    int status = 0;
    long pid = syscall0(SYS_FORK);

    if (pid == 0) {
        sys_exit(apic_version(at) == parent_version ? 0 : 1);
    }
    if (pid < 0 || syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        return -1;
    }
    return status;
}

/** Open /dev/mem, map the APIC and the first megabyte, and check the
 * refusals: `open`, `apic`, `maps`, `low`, `refused` and `nowrite`. The
 * descriptor, open for reading and writing; the APIC's mapping in `*apic`. */
static int map_rows(long *apic)
{
    static char low[64];
    int fd = (int)syscall3(SYS_OPEN, (long)"/dev/mem", O_RDWR, 0);
    int ro;
    long at, r;
    int i;

    if (fd < 0) {
        fail("open of /dev/mem for reading and writing");
    }
    report_held();
    at = map_mem(fd, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, APIC_BASE);
    if (sys_failed(at) || apic_version(at) == 0) {
        fail("the local APIC's version through a shared mapping");
    }
    parent_version = apic_version(at);
    *apic = at;
    report_held();
    if (!maps_names(at)) {
        fail("/proc/self/maps has no rw-s line at fee00000 naming /dev/mem");
    }
    report_held();
    r = map_mem(fd, PAGE, PROT_READ, MAP_SHARED, LOW_PAGE);
    if (sys_failed(r) || probe_pread(fd, low, sizeof(low), LOW_PAGE) != (long)sizeof(low)) {
        fail("a mapping and a pread of the first megabyte");
    }
    for (i = 0; i < (int)sizeof(low); i++) {
        if (((volatile char *)r)[i] != low[i]) {
            fail("the first megabyte's mapping differs from its pread");
        }
    }
    syscall2(SYS_MUNMAP, r, PAGE);
    report_held();
    if (map_mem(fd, PAGE, PROT_READ, MAP_PRIVATE, APIC_BASE) != -EINVAL ||
        map_mem(fd, PAGE, PROT_READ, MAP_SHARED, RAM_PAGE) != -EPERM) {
        fail("a private mapping is not EINVAL, or a frame of RAM not EPERM");
    }
    ro = (int)syscall3(SYS_OPEN, (long)"/dev/mem", O_RDONLY, 0);
    if (ro < 0 || map_mem(ro, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, APIC_BASE) != -EACCES) {
        fail("a writable shared mapping through a read-only descriptor is not EACCES");
    }
    report_held();
    r = map_mem(ro, PAGE, PROT_READ, MAP_SHARED, APIC_BASE);
    if (sys_failed(r) || apic_version(r) != parent_version ||
        syscall3(SYS_MPROTECT, r, PAGE, PROT_READ | PROT_WRITE) != -EACCES) {
        fail("mprotect's write on a mapping through a read-only descriptor is not EACCES");
    }
    syscall2(SYS_MUNMAP, r, PAGE);
    syscall1(SYS_CLOSE, ro);
    report_held();
    return fd;
}

/** The fork, advice, mremap and munmap rows over the APIC's mapping `at`:
 * `fork`, `dontfork`, `advice`, `mremap` and `munmap`. */
static void life_rows(long at)
{
    int status;
    long to, r;

    status = child_reads(at);
    if (status < 0 || WIFSIGNALED(status) || WEXITSTATUS(status) != 0) {
        fail("a fork's child does not read the parent's APIC version");
    }
    report_held();
    if (syscall3(SYS_MADVISE, at, PAGE, MADV_DONTFORK) != 0) {
        fail("MADV_DONTFORK over the mapping");
    }
    status = child_reads(at);
    if (status < 0 || !WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV) {
        fail("a child touching a MADV_DONTFORK mapping did not die of SIGSEGV");
    }
    report_held();
    if (syscall3(SYS_MADVISE, at, PAGE, MADV_DOFORK) != -EINVAL ||
        syscall3(SYS_MADVISE, at, PAGE, MADV_DONTNEED) != -EINVAL) {
        fail("MADV_DOFORK or MADV_DONTNEED over device memory is not EINVAL");
    }
    report_held();
    if (syscall5(SYS_MREMAP, at, PAGE, 2 * PAGE, MREMAP_MAYMOVE, 0) != -EFAULT) {
        fail("mremap's growth of device memory is not EFAULT");
    }
    to = probe_mmap(0, 4 * PAGE, PROT_READ, MAP_PRIVATE | MAP_ANON, -1, 0);
    if (sys_failed(to)) {
        fail("an anonymous mapping to move onto");
    }
    to += PAGE;
    r = syscall5(SYS_MREMAP, at, PAGE, PAGE, MREMAP_MAYMOVE | MREMAP_FIXED, to);
    if (r != to || apic_version(to) != parent_version) {
        fail("mremap's move does not carry the APIC's registers to the new address");
    }
    report_held();
    if (syscall2(SYS_MUNMAP, to, PAGE) != 0) {
        fail("munmap of the moved mapping");
    }
    report_held();
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Run the rows in order; `_start` calls it with the initial stack. */
void probe_main(unsigned long *sp)
{
    static const char *const ids[] = {"open", "apic",     "maps",   "low",    "refused", "nowrite",
                                      "fork", "dontfork", "advice", "mremap", "munmap"};
    long apic = 0;
    unsigned i;
    int fd;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.mmapdevprobe"));
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        report_plan(ids[i]);
    }
    fd = map_rows(&apic);
    life_rows(apic);
    syscall1(SYS_CLOSE, fd);
    report_end();
    probe_line("MMAPDEVPROBE OK", 0, 0);
    sys_exit(0);
}
