/**
 * mapmusl: check file mappings, shared mappings, mremap and madvise.
 *
 * A private mapping of a file reads the file's bytes, including a second page
 * and zeros past the end; a store changes the mapping and not the file; the
 * mapping outlives the descriptor it was made from; a child after fork sees the
 * parent's bytes and its own stores stay its own. Through a shared anonymous
 * mapping and a shared mapping of a file in /dev/shm the parent sees a child's
 * store, a store through the mapping of the file reaches read(2), and after a
 * truncation and a growth a write to the file is seen through the mapping. On
 * ext2 a cache-warmed private page and a shared writable alias both raise
 * SIGBUS after truncation, and regrowth preserves the prefix and has a zero
 * tail; a private mapping of 4 MiB of an ext2 file without a hint reads
 * the file, and in a 64-bit program on x86-64 starts on a 2 MiB boundary,
 * as Linux's thp_get_unmapped_area places it. On x86-64, truncating a file
 * of 4 GiB and a page to 4 GiB cuts only its last page from the mapping. A
 * file or shared-memory mapping of 16 TiB is EOVERFLOW and one of a page
 * less is made; mprotect,
 * getrusage, a shrink and a move each pass terabytes without pages in the
 * run's time and reach the page beyond them. mprotect makes a private mapping
 * of a file opened read-only writable, and not a shared one.
 * Shared anonymous memory ignores a raw mmap2's page offset (mmap's, in bytes,
 * on x86-64), a file's offset whose last page would not fit in 32 bits is
 * EOVERFLOW, and a page of a file in memory past its end is never backed.
 * mremap without MREMAP_MAYMOVE does not grow a mapping into the one after it,
 * and with the flag moves the mapping with its bytes. A shrink in place keeps
 * the bytes of the page it keeps and frees the two pages it cuts:
 * MAP_FIXED_NOREPLACE maps them again, and the byte stored in them before
 * reads as zero. mremap fails with EINVAL for an old length that would wrap
 * past the end of memory, and leaves the mapping; a fixed mremap that shrinks
 * a mapping is held to RLIMIT_AS by what it leaves; madvise(MADV_DONTNEED)
 * and madvise(MADV_DONTNEED_LOCKED) give a private anonymous mapping zero
 * pages again, MADV_COLD keeps its bytes, an advice Linux does not define is
 * EINVAL, and madvise and msync fail with EINVAL for a length that would round
 * up to 0; an i386 program on the x86-64 kernel cannot give such a length,
 * and its 4 GiB less one is ENOMEM, as Linux's compat calls find no region
 * past the program's 4 GiB. mprotect over a hole fails with ENOMEM and
 * keeps the change of the page before it, accepts PROT_SEM and
 * PROT_GROWSDOWN over the stack, and refuses PROT_GROWSDOWN elsewhere and
 * PROT_GROWSUP with EINVAL. A write through
 * /proc/self/mem to a read-only page a fork shared leaves the page read-only.
 * On an ext2 root, through a shared mapping of a file the parent sees a
 * child's store, read(2) sees a store, the mapping sees a write(2) without
 * losing a store beside it, a readv(2) whose first buffer lies in the
 * mapping reads into its second buffer what the first stored there, as
 * Linux reads the vector from the shared page in order, and its stores
 * reach the file after munmap and
 * after a mapper's exit; on the KRFFS root, one that may be written is ENODEV,
 * and a read-only one through a descriptor open for writing may not be made
 * writable. A page of the program's own data segment that was written, and
 * one of its read-only data, read the program's file again after
 * madvise(MADV_DONTNEED), as on Linux, where exec maps each segment
 * privately from the file; so do they in a copy of the program run after
 * it renamed and unlinked its own file. Both run in a child the probe
 * execs, since the first program a boot loads has its segments copied.
 *
 * A NAME=VALUE argument, such as the `root=PATH` a native run appends
 * (tests/native/Readme.md), is not the child's mode.
 *
 * Prints `MAPMUSL OK` and exits 0 on success, and prints
 * `MAPMUSL FAIL (detail)` and exits 1 otherwise.
 *
 * The probe writes etude's report records (probe_report.h) for the case
 * ETUDE_CASE names, an assertion for each check, as `assertion_id` lists
 * them; a failed check fails its assertion and leaves those after it not
 * run. The branch of the group `arch` is `x86-64`, with the assertions from
 * `wide-file` to `huge-move-munmap`, or `i686`, with none of its own; that
 * of `root` is `ext2`, with those from `ext2-mmap` to `ext2-thp`, or
 * `krffs`, with `krffs-enodev` and `krffs-readonly`, by the type of /tmp.
 * The child that checks its segments writes none.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/utsname.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../probe_args.h"
#include "../probe_report.h"

extern char **environ;

#define FILE_PATH "/tmp/mapmusl"
#define SHM_PATH  "/dev/shm/mapmusl"
#define EXE_PATH  "/tmp/mapmusl.exe"
#define MOVED     "/tmp/mapmusl.moved"
#define PAGE      4096
#define HUGE_PAGE (2ul << 20)

/* Whether a mapping of a file that holds a 2 MiB page is aligned to one:
 * in a 64-bit program on x86-64, and in no 32-bit one, as on Linux. */
#if defined __x86_64__
#define THP_ALIGNS 1
#else
#define THP_ALIGNS 0
#endif

/*
 * A raw mapping at a file offset given in pages: i386's mmap2 takes the page
 * offset, and x86-64's mmap, which has no mmap2, the offset in bytes.
 */
#if defined SYS_mmap2
#define MMAP_PAGES(len, prot, flags, fd, pgoff) syscall(SYS_mmap2, 0, len, prot, flags, fd, pgoff)
#else
#define MMAP_PAGES(len, prot, flags, fd, pgoff) \
    syscall(SYS_mmap, 0, len, prot, flags, fd, (unsigned long)(pgoff) * PAGE)
#endif
#define EXT2_MAGIC 0xEF53

/* Linux's number, which musl's sys/mman.h does not define. */
#ifndef MADV_DONTNEED_LOCKED
#define MADV_DONTNEED_LOCKED 24
#endif

/* Two pages each of the program's initialized data and of its read-only
 * data, aligned so that nothing else shares their pages. */
static unsigned char seg_data[2 * PAGE] __attribute__((aligned(PAGE))) = {
    [0] = 'd', [PAGE - 1] = 'd', [PAGE] = 'e', [2 * PAGE - 1] = 'e'};
static const unsigned char seg_ro[2 * PAGE] __attribute__((aligned(PAGE))) = {
    [0] = 'r', [PAGE - 1] = 'r', [PAGE] = 'o', [2 * PAGE - 1] = 'o'};

/* The assertions, a check each, in the order the program makes them, with
 * the alternatives of `root` one after the other. */
static const char *const assertion_id[] = {"private-file",
                                           "private-mmap",
                                           "private-bytes",
                                           "private-store-stays",
                                           "private-fork",
                                           "private-munmap",
                                           "private-offset",
                                           "shared-anon-mmap",
                                           "shared-anon-child",
                                           "shm-file",
                                           "shm-mmap",
                                           "shm-child",
                                           "shm-read",
                                           "root-file",
                                           "ext2-mmap",
                                           "ext2-child",
                                           "ext2-read",
                                           "ext2-write",
                                           "ext2-readv",
                                           "ext2-msync",
                                           "ext2-reach-file",
                                           "ext2-cached-truncate",
                                           "ext2-thp",
                                           "krffs-enodev",
                                           "krffs-readonly",
                                           "trunc-file",
                                           "trunc-mmap",
                                           "trunc-freed",
#if defined __x86_64__
                                           "wide-file",
                                           "wide-mmap",
                                           "wide-first-page",
                                           "wide-last-page",
                                           "wide-eoverflow",
                                           "wide-less-one",
                                           "huge-mmap",
                                           "huge-last-page",
                                           "huge-mprotect-reach",
                                           "huge-mprotect",
                                           "huge-getrusage",
                                           "huge-shrink-child",
                                           "huge-shrink",
                                           "huge-shrink-cut",
                                           "huge-munmap",
                                           "huge-move-mmap",
                                           "huge-move",
                                           "huge-move-munmap",
#endif
                                           "ro-file",
                                           "ro-mmap",
                                           "ro-shared-eacces",
                                           "ro-private-mprotect",
                                           "ro-private-store",
                                           "offset-anon",
                                           "offset-file",
                                           "offset-eoverflow",
                                           "offset-mmap",
                                           "offset-sigbus",
                                           "remap-mmap",
                                           "remap-blocker",
                                           "remap-no-grow",
                                           "remap-move",
                                           "remap-shrink",
                                           "remap-shrink-kept",
                                           "remap-shrink-cut",
                                           "remap-wrap-einval",
                                           "advise-mmap",
                                           "dontneed",
                                           "cold",
                                           "dontneed-locked",
                                           "advice-einval",
                                           "madvise-zero-einval",
                                           "msync-zero-einval",
                                           "protect-gap-prefix",
                                           "protect-grows",
                                           "forced-mmap",
                                           "forced-fork-mprotect",
                                           "forced-proc-mem",
                                           "forced-read-only",
                                           "limit-mmap",
                                           "limit-size",
                                           "limit-set",
                                           "limit-restore",
                                           "limit-shrink",
                                           "exec-segments",
                                           "exec-copy",
                                           "exec-unlinked-segments"};

#define ASSERTIONS (sizeof(assertion_id) / sizeof(assertion_id[0]))

/* The index in `assertion_id` of the next assertion to report, the rows of
 * the alternative of `root` not taken, from `skip_from` to `skip_to`, and
 * whether this process writes the report, which the parent alone does. */
static unsigned int assertion_at, skip_from, skip_to;
static int reporting;

/** The index of the assertion `id` in `assertion_id`. */
static unsigned int assertion_index(const char *id)
{
    unsigned int i = 0;

    while (i < ASSERTIONS && strcmp(assertion_id[i], id) != 0) {
        i++;
    }
    return i;
}

/**
 * Begin the report, give the branches of `arch` and of `root`, the latter by
 * the type of the file system of /tmp, and set the rows of the alternative
 * of `root` not taken aside.
 */
static void report_start(void)
{
    struct statfs sfs;
    int ext2 = statfs("/tmp", &sfs) == 0 && sfs.f_type == EXT2_MAGIC;

    report_begin(report_case(environ, "guest.default.mapmusl"));
    reporting = 1;
#if defined __x86_64__
    report_branch("arch", "x86-64");
#else
    report_branch("arch", "i686");
#endif
    report_branch("root", ext2 ? "ext2" : "krffs");
    skip_from = assertion_index(ext2 ? "krffs-enodev" : "ext2-mmap");
    skip_to = assertion_index(ext2 ? "trunc-file" : "krffs-enodev");
}

/** The next assertion to report. */
static const char *row_next(void)
{
    if (assertion_at == skip_from) {
        assertion_at = skip_to;
    }
    return assertion_id[assertion_at++];
}

/** Report that the next assertion held. */
static void row_held(void)
{
    report_pass(row_next());
}

/** Report that the next assertion failed for `why`, that those after it
 * were not run, and the report's end. */
static void row_stop(const char *why)
{
    report_fail(row_next(), why);
    while (assertion_at < ASSERTIONS) {
        report_notrun(row_next(), "an earlier check failed");
    }
    report_end();
}

/** Report one failed check with errno, end the report at the assertion that
 * `what` fails when this process writes it, and return 1, the failure
 * status. */
static int fail(const char *what)
{
    printf("MAPMUSL FAIL (%s: errno %d)\n", what, errno);
    if (reporting) {
        row_stop(what);
    }
    return 1;
}

/** Wait for the child `pid`; whether it exited with status 0. */
static int wait_ok(pid_t pid)
{
    int status;

    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

/**
 * Check a private mapping of a file of two pages: its bytes and zeros past
 * the end, a store that stays out of the file, a fork's copy, and a mapping
 * from an offset. Returns 0, or 1 once the failure is reported.
 */
static int private_file(void)
{
    static char block[PAGE + 100];
    char check[4];
    char *m;
    pid_t pid;
    int fd;

    memset(block, 'a', PAGE);
    memset(block + PAGE, 'b', 100);
    fd = open(FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
    if (fd < 0 || write(fd, block, sizeof(block)) != (ssize_t)sizeof(block)) {
        return fail("the file");
    }
    row_held();
    m = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) {
        return fail("mmap MAP_PRIVATE of a file");
    }
    row_held();
    close(fd);
    if (m[0] != 'a' || m[PAGE - 1] != 'a' || m[PAGE] != 'b' || m[PAGE + 99] != 'b' ||
        m[PAGE + 100] != 0 || m[2 * PAGE - 1] != 0) {
        return fail("the mapping did not hold the file's bytes");
    }
    row_held();
    m[0] = 'X';
    fd = open(FILE_PATH, O_RDONLY);
    if (fd < 0 || read(fd, check, 1) != 1 || check[0] != 'a') {
        return fail("a store in a private mapping reached the file");
    }
    row_held();
    close(fd);
    pid = fork();
    if (pid == 0) {
        int ok = m[0] == 'X' && m[PAGE] == 'b';

        m[1] = 'Y';
        _exit(ok ? 0 : 1);
    }
    if (!wait_ok(pid) || m[1] != 'a') {
        return fail("fork and a private file mapping");
    }
    row_held();
    if (munmap(m, 2 * PAGE) != 0) {
        return fail("munmap");
    }
    row_held();
    m = mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, open(FILE_PATH, O_RDONLY), PAGE);
    if (m == MAP_FAILED || m[0] != 'b') {
        return fail("a mapping from an offset");
    }
    row_held();
    munmap(m, PAGE);
    unlink(FILE_PATH);
    return 0;
}

/**
 * Check that the parent sees a child's store through a shared anonymous
 * mapping and through a shared mapping of a file in /dev/shm, and that read
 * sees a store through the second. Returns 0, or 1 once the failure is
 * reported.
 */
static int shared(void)
{
    char *anon = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    char check[4];
    char *m;
    pid_t pid;
    int fd;

    if (anon == MAP_FAILED) {
        return fail("mmap MAP_SHARED|MAP_ANONYMOUS");
    }
    row_held();
    pid = fork();
    if (pid == 0) {
        anon[10] = 's';
        _exit(0);
    }
    if (!wait_ok(pid) || anon[10] != 's') {
        return fail("a child's store in a shared anonymous mapping");
    }
    row_held();
    munmap(anon, PAGE);

    fd = open(SHM_PATH, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || ftruncate(fd, PAGE) != 0) {
        return fail("a file in /dev/shm");
    }
    row_held();
    m = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        return fail("mmap MAP_SHARED of a file in /dev/shm");
    }
    row_held();
    pid = fork();
    if (pid == 0) {
        m[5] = 'c';
        _exit(0);
    }
    if (!wait_ok(pid) || m[5] != 'c') {
        return fail("a child's store in a shared file mapping");
    }
    row_held();
    m[6] = 'd';
    if (pread(fd, check, 2, 5) != 2 || check[0] != 'c' || check[1] != 'd') {
        return fail("a store in a shared mapping did not reach read");
    }
    row_held();
    munmap(m, PAGE);
    close(fd);
    unlink(SHM_PATH);
    return 0;
}

/**
 * Check that a truncation takes the pages past the new end out of every
 * mapping of a file in memory: after the file shrinks to nothing and grows
 * again, a write to it is seen through the mapping, which maps the file's
 * page and no longer the one the truncation freed. Returns 0, or 1 once the
 * failure is reported.
 */
static int truncated_shared(void)
{
    char *m;
    int fd = open(SHM_PATH, O_RDWR | O_CREAT | O_TRUNC, 0600);

    if (fd < 0 || ftruncate(fd, PAGE) != 0) {
        return fail("a file in /dev/shm to truncate");
    }
    row_held();
    m = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        return fail("mmap of a file in /dev/shm to truncate");
    }
    row_held();
    m[0] = 'o';
    if (ftruncate(fd, 0) != 0 || ftruncate(fd, PAGE) != 0 || pwrite(fd, "n", 1, 0) != 1 ||
        m[0] != 'n') {
        return fail("a shared mapping kept a page its file's truncation freed");
    }
    row_held();
    munmap(m, PAGE);
    close(fd);
    unlink(SHM_PATH);
    return 0;
}

#if defined __x86_64__
/**
 * Check a truncation's cut and the bound on a mapping's pages at x86-64's
 * widths. Returns 0, or 1 once the failure is reported.
 *
 * Of a file of 4 GiB and a page, truncated to 4 GiB, whose offset in bytes
 * passes 32 bits, only the last page is taken out, and the first stays in
 * memory, as mincore reports of each. The kernel numbers the pages of a file
 * or of shared memory in 32 bits, so a mapping of either is EOVERFLOW at 2^32
 * pages, 16 TiB, as Linux's sum of the offset and the pages is at 32 bits,
 * and is made at a page less.
 */
static int truncated_wide(void)
{
    const size_t four = (size_t)1 << 32, huge = (size_t)1 << 44;
    unsigned char in = 0;
    char *m;
    int fd = open(SHM_PATH, O_RDWR | O_CREAT | O_TRUNC, 0600);

    if (fd < 0 || ftruncate(fd, (off_t)(four + PAGE)) != 0) {
        return fail("a file in /dev/shm of 4 GiB and a page");
    }
    row_held();
    m = mmap(NULL, four + PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        return fail("mmap of a file in /dev/shm of 4 GiB and a page");
    }
    row_held();
    m[0] = 'a';
    m[four] = 'b';
    if (ftruncate(fd, (off_t)four) != 0 || mincore(m, PAGE, &in) != 0 || !(in & 1)) {
        return fail("a truncation to 4 GiB took the mapping's first page out");
    }
    row_held();
    if (mincore(m + four, PAGE, &in) != 0 || (in & 1)) {
        return fail("a truncation to 4 GiB left the page past it in the mapping");
    }
    row_held();
    munmap(m, four + PAGE);
    if (mmap(NULL, huge, PROT_READ, MAP_PRIVATE, fd, 0) != MAP_FAILED || errno != EOVERFLOW ||
        mmap(NULL, huge, PROT_READ, MAP_SHARED | MAP_ANONYMOUS, -1, 0) != MAP_FAILED ||
        errno != EOVERFLOW) {
        return fail("a mapping of a file or of shared memory of 2^32 pages is not EOVERFLOW");
    }
    row_held();
    m = mmap(NULL, huge - PAGE, PROT_READ, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED || munmap(m, huge - PAGE) != 0) {
        return fail("a mapping of shared memory of 2^32 pages less one");
    }
    row_held();
    close(fd);
    unlink(SHM_PATH);
    return 0;
}

/** Wait for the child `pid`; whether it died of SIGSEGV. */
static int died_of_segv(pid_t pid)
{
    int status;

    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFSIGNALED(status) &&
           WTERMSIG(status) == SIGSEGV;
}

/**
 * Check walks over terabytes with a page at the far end. Returns 0, or 1 once
 * the failure is reported.
 *
 * Each walk steps past the page tables a range lacks, so it finishes inside
 * the run's time limit, where a step for every page of 64 TiB would take
 * minutes, and it reaches the page beyond them: mprotect of 64 TiB makes it
 * read-only, getrusage finishes, a shrink to half takes it out, and a move of
 * 32 TiB takes along the page a read brought in, which mincore finds in
 * memory at its new place. The mprotect and the shrink each run in a child of
 * their own, which has the page in its tables before the call and touches it
 * after with no fork between: a fork would write-protect the page for
 * copying, or leave out one outside the regions, and hide a walk that missed
 * it. The mprotect's child writes the page first, so that it is its own and
 * writable.
 */
static int huge_walks(void)
{
    const size_t big = (size_t)1 << 46, half = big / 2;
    unsigned char in = 0;
    struct rusage ru;
    char *m, *far, *moved;
    pid_t pid;
    int status;

    m = mmap(NULL, big, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED) {
        return fail("mmap of 64 TiB");
    }
    row_held();
    far = m + big - PAGE;
    if (mprotect(far, PAGE, PROT_READ | PROT_WRITE) != 0) {
        return fail("mprotect of the last page of 64 TiB");
    }
    row_held();
    *far = 'z';
    pid = fork();
    if (pid == 0) {
        *far = 'y';
        if (mprotect(m, big, PROT_READ) != 0 || *far != 'y') {
            _exit(1);
        }
        *far = 'w';
        _exit(0);
    }
    if (!died_of_segv(pid)) {
        return fail("mprotect of 64 TiB did not reach its last page");
    }
    row_held();
    if (mprotect(m, big, PROT_READ) != 0) {
        return fail("mprotect of 64 TiB to one region");
    }
    row_held();
    if (getrusage(RUSAGE_SELF, &ru) != 0 || ru.ru_maxrss <= 0) {
        return fail("getrusage with 64 TiB mapped");
    }
    row_held();
    pid = fork();
    if (pid == 0) {
        (void)*(volatile char *)far;
        if (mremap(m, big, big / 2, 0) != m) {
            _exit(2);
        }
        (void)*(volatile char *)far;
        _exit(0);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid) {
        return fail("the child that shrinks 64 TiB");
    }
    row_held();
    if (WIFEXITED(status) && WEXITSTATUS(status) == 2) {
        return fail("a shrink of 64 TiB to half was refused");
    }
    row_held();
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV) {
        return fail("a shrink of 64 TiB to half kept its last page");
    }
    row_held();
    if (munmap(m, big) != 0) {
        return fail("munmap of 64 TiB");
    }
    row_held();
    m = mmap(NULL, 2 * half, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED || m[half - PAGE] != 0) {
        return fail("mmap of 64 TiB to move half of");
    }
    row_held();
    moved = mremap(m, half, half, MREMAP_MAYMOVE | MREMAP_FIXED, m + half);
    if (moved != m + half || mincore(moved + half - PAGE, PAGE, &in) != 0 || !(in & 1)) {
        return fail("a move of 32 TiB did not carry its last page");
    }
    row_held();
    if (munmap(m + half, half) != 0) {
        return fail("munmap of the 32 TiB moved");
    }
    row_held();
    return 0;
}
#else
/** Nothing to check on i686, where a mapping cannot reach 4 GiB. */
static int truncated_wide(void)
{
    return 0;
}

/** Nothing to check on i686, whose address space is 4 GiB. */
static int huge_walks(void)
{
    return 0;
}
#endif

/**
 * Check that a shared mapping of a file opened read-only cannot be made
 * writable: mprotect fails with EACCES, since stores through the mapping
 * would reach the file. A private mapping of the same descriptor may be
 * written, its stores its own. Returns 0, or 1 once the failure is reported.
 */
static int readonly_shared(void)
{
    char *ro, *priv;
    int fd = open(SHM_PATH, O_RDWR | O_CREAT | O_TRUNC, 0600);

    if (fd < 0 || write(fd, "file", 4) != 4) {
        return fail("a file in /dev/shm for a read-only mapping");
    }
    row_held();
    close(fd);
    fd = open(SHM_PATH, O_RDONLY);
    ro = mmap(NULL, PAGE, PROT_READ, MAP_SHARED, fd, 0);
    priv = mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, fd, 0);
    if (fd < 0 || ro == MAP_FAILED || priv == MAP_FAILED) {
        return fail("mmap of a file opened read-only");
    }
    row_held();
    if (mprotect(ro, PAGE, PROT_READ | PROT_WRITE) != -1 || errno != EACCES) {
        return fail("mprotect made a read-only file's shared mapping writable");
    }
    row_held();
    if (mprotect(priv, PAGE, PROT_READ | PROT_WRITE) != 0) {
        return fail("mprotect of a read-only file's private mapping");
    }
    row_held();
    priv[0] = 'P';
    if (ro[0] != 'f') {
        return fail("a store in a private mapping reached the shared one");
    }
    row_held();
    munmap(ro, PAGE);
    munmap(priv, PAGE);
    close(fd);
    unlink(SHM_PATH);
    return 0;
}

/**
 * Check the page offset of a raw mmap2. Returns 0, or 1 once the failure is
 * reported.
 *
 * Shared anonymous memory ignores it; an offset of 2^31 pages would double
 * the length of the memory's table of pages until the length wrapped to zero,
 * and the first touch would not end. A file's offset whose last page would
 * not fit in 32 bits is EOVERFLOW, and the page of a file in memory 2^31
 * pages in, past the end of the empty file, is never backed: a child that
 * touches it dies of SIGBUS, as in Linux.
 */
static int offsets(void)
{
    char *m = (char *)MMAP_PAGES(PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1,
                                 0x80000000u);
    int status;
    pid_t pid;
    int fd;

    if (m == MAP_FAILED) {
        return fail("mmap2 of shared anonymous memory at a page offset of 2^31");
    }
    row_held();
    m[0] = 'o';
    munmap(m, PAGE);
    fd = open(SHM_PATH, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return fail("a file in /dev/shm for the offsets");
    }
    row_held();
    if (MMAP_PAGES(PAGE, PROT_READ, MAP_SHARED, fd, 0xFFFFFFFFu) != -1 || errno != EOVERFLOW) {
        return fail("mmap2 of a file whose last page is past 32 bits");
    }
    row_held();
    m = (char *)MMAP_PAGES(PAGE, PROT_READ, MAP_SHARED, fd, 0x80000000u);
    if (m == MAP_FAILED) {
        return fail("mmap2 of a file at a page offset of 2^31");
    }
    row_held();
    pid = fork();
    if (pid == 0) {
        _exit(m[0] == 0 ? 0 : 1);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFSIGNALED(status) || WTERMSIG(status) != SIGBUS) {
        return fail("a touch of a file in memory past its end was not SIGBUS");
    }
    row_held();
    munmap(m, PAGE);
    close(fd);
    unlink(SHM_PATH);
    return 0;
}

/**
 * Check mremap's growth, move and shrink, and its EINVAL for a length that
 * wraps. Returns 0, or 1 once the failure is reported.
 */
static int remaps(void)
{
    char *m = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *blocker, *moved, *cut;

    if (m == MAP_FAILED) {
        return fail("mmap for mremap");
    }
    row_held();
    m[0] = 'r';
    blocker =
        mmap(m + PAGE, PAGE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (blocker == MAP_FAILED) {
        return fail("the mapping that blocks growth in place");
    }
    row_held();
    if (mremap(m, PAGE, 2 * PAGE, 0) != MAP_FAILED || errno != ENOMEM) {
        return fail("mremap without MREMAP_MAYMOVE grew into another mapping");
    }
    row_held();
    moved = mremap(m, PAGE, 3 * PAGE, MREMAP_MAYMOVE);
    if (moved == MAP_FAILED || moved == m || moved[0] != 'r') {
        return fail("mremap did not move the mapping with its bytes");
    }
    row_held();
    moved[3 * PAGE - 1] = 'e';
    if (mremap(moved, 3 * PAGE, PAGE, 0) != moved) {
        return fail("mremap did not shrink in place");
    }
    row_held();
    if (moved[0] != 'r') {
        return fail("the page a shrink kept lost its bytes");
    }
    row_held();
    cut = mmap(moved + PAGE, 2 * PAGE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
               -1, 0);
    if (cut != moved + PAGE || cut[2 * PAGE - 1] != 0) {
        return fail("the pages a shrink cut were still mapped");
    }
    row_held();
    munmap(cut, 2 * PAGE);
    if (mremap(moved, (size_t)-PAGE, PAGE, MREMAP_MAYMOVE | MREMAP_FIXED, moved) != MAP_FAILED ||
        errno != EINVAL || moved[0] != 'r') {
        return fail("mremap took an old length that wraps past the end of memory");
    }
    row_held();
    munmap(blocker, PAGE);
    munmap(moved, PAGE);
    return 0;
}

/**
 * The error of madvise and msync for a length of all ones: EINVAL where the
 * kernel's size_t is the program's, since the length rounds up to 0, and
 * ENOMEM for an i386 program on the x86-64 kernel, whose 64-bit size_t takes
 * it as 4 GiB less one, past every region the program can have.
 */
static int all_ones_error(void)
{
    struct utsname u;

    return sizeof(size_t) == 4 && uname(&u) == 0 && strcmp(u.machine, "x86_64") == 0 ? ENOMEM
                                                                                     : EINVAL;
}

/**
 * Check madvise's advice over a private anonymous mapping, and madvise's and
 * msync's error for a length of all ones (`all_ones_error`). Returns 0, or 1
 * once the failure is reported.
 */
static int advice(void)
{
    char *m = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    if (m == MAP_FAILED) {
        return fail("mmap for madvise");
    }
    row_held();
    m[0] = 'z';
    m[PAGE] = 'z';
    if (madvise(m, PAGE, MADV_DONTNEED) != 0 || m[0] != 0 || m[PAGE] != 'z') {
        return fail("MADV_DONTNEED");
    }
    row_held();
    m[0] = 'z';
    if (madvise(m, PAGE, MADV_COLD) != 0 || m[0] != 'z') {
        return fail("MADV_COLD");
    }
    row_held();
    if (madvise(m, PAGE, MADV_DONTNEED_LOCKED) != 0 || m[0] != 0 || m[PAGE] != 'z') {
        return fail("MADV_DONTNEED_LOCKED");
    }
    row_held();
    if (madvise(m, PAGE, 5) != -1 || errno != EINVAL || madvise(m, PAGE, 50) != -1 ||
        errno != EINVAL) {
        return fail("madvise took an advice Linux does not define");
    }
    row_held();
    if (madvise(m, (size_t)-1, MADV_DONTNEED) != -1 || errno != all_ones_error()) {
        return fail("madvise took a length that rounds up to 0 for an empty range");
    }
    row_held();
    if (msync(m, (size_t)-1, MS_SYNC) != -1 || errno != all_ones_error()) {
        return fail("msync took a length that rounds up to 0 for an empty range");
    }
    row_held();
    munmap(m, 2 * PAGE);
    return 0;
}

#ifndef PROT_SEM
#define PROT_SEM 0x8
#endif

/**
 * Check that mprotect changes the regions of its range one after another
 * and keeps the change of those before a page no region holds, failing
 * with ENOMEM, as Linux does, and that it accepts PROT_SEM, and
 * PROT_GROWSDOWN over the main stack alone, and refuses PROT_GROWSUP and
 * the two together with EINVAL. Returns 0, or 1 once the failure is
 * reported.
 */
static int protect_stops(void)
{
    char *m = mmap(NULL, 3 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *stack = (char *)((uintptr_t)&m & ~(uintptr_t)(PAGE - 1));
    int status;
    pid_t pid;

    if (m == MAP_FAILED || munmap(m + PAGE, PAGE) != 0) {
        return fail("mmap and munmap for mprotect");
    }
    m[0] = 'p';
    m[2 * PAGE] = 'p';
    if (mprotect(m, 3 * PAGE, PROT_READ) != -1 || errno != ENOMEM) {
        return fail("mprotect over a hole did not fail with ENOMEM");
    }
    pid = fork();
    if (pid == 0) {
        m[0] = 'q';
        _exit(0);
    }
    m[2 * PAGE] = 'q';
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFSIGNALED(status) ||
        WTERMSIG(status) != SIGSEGV || m[0] != 'p') {
        return fail("mprotect over a hole left the page before it writable");
    }
    row_held();
    if (mprotect(m + 2 * PAGE, PAGE, PROT_READ | PROT_WRITE | PROT_SEM) != 0 ||
        mprotect(m + 2 * PAGE, PAGE, PROT_READ | PROT_GROWSDOWN) != -1 || errno != EINVAL ||
        mprotect(m + 2 * PAGE, PAGE, PROT_READ | PROT_GROWSUP) != -1 || errno != EINVAL ||
        mprotect(m + 2 * PAGE, PAGE, PROT_READ | PROT_GROWSDOWN | PROT_GROWSUP) != -1 ||
        errno != EINVAL || mprotect(stack, PAGE, PROT_READ | PROT_WRITE | PROT_GROWSDOWN) != 0) {
        return fail("mprotect with PROT_SEM, PROT_GROWSDOWN or PROT_GROWSUP");
    }
    m[2 * PAGE] = 'r';
    row_held();
    munmap(m, 3 * PAGE);
    return 0;
}

/* Where `on_segv` returns to, set by `forced_write` around its store. */
static sigjmp_buf segv_jump;

/** Return from a SIGSEGV to `segv_jump`. */
static void on_segv(int sig)
{
    (void)sig;
    siglongjmp(segv_jump, 1);
}

/**
 * Check that a write through /proc/self/mem to a page a fork left
 * copy-on-write, in a region mprotect has made read-only since, goes into a
 * page of this process's own that stays read-only: a store from the program
 * after it faults, since the region allows none. Returns 0, or 1 once the
 * failure is reported.
 */
static int forced_write(void)
{
    char *m = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    struct sigaction sa, old;
    pid_t pid;
    int fd;

    if (m == MAP_FAILED) {
        return fail("mmap for a forced write");
    }
    row_held();
    m[0] = 'a';
    pid = fork();
    if (pid == 0) {
        _exit(0);
    }
    if (!wait_ok(pid) || mprotect(m, PAGE, PROT_READ) != 0) {
        return fail("fork and mprotect for a forced write");
    }
    row_held();
    fd = open("/proc/self/mem", O_RDWR);
    if (fd < 0 || pwrite(fd, "b", 1, (off_t)(unsigned long)m) != 1 || m[0] != 'b') {
        return fail("a write through /proc/self/mem");
    }
    row_held();
    close(fd);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_segv;
    sigaction(SIGSEGV, &sa, &old);
    if (sigsetjmp(segv_jump, 1) == 0) {
        *(volatile char *)m = 'c';
        sigaction(SIGSEGV, &old, NULL);
        return fail("a store reached a read-only page after a write through /proc/self/mem");
    }
    row_held();
    sigaction(SIGSEGV, &old, NULL);
    munmap(m, PAGE);
    return 0;
}

/**
 * The pages this process's regions cover, the first number of
 * /proc/self/statm, read with no allocation that would change it; 0 when it
 * cannot be read.
 */
static unsigned long mapped_pages(void)
{
    char text[64];
    ssize_t n;
    int fd = open("/proc/self/statm", O_RDONLY);

    if (fd < 0) {
        return 0;
    }
    n = read(fd, text, sizeof(text) - 1);
    close(fd);
    if (n <= 0) {
        return 0;
    }
    text[n] = '\0';
    return strtoul(text, NULL, 10);
}

/**
 * Check that a fixed mremap that shrinks a mapping of 64 pages to one is held
 * to RLIMIT_AS by what it leaves, 63 pages less than now: under a limit 32
 * pages below the present size it succeeds, with the pages between left for
 * the stack to grow into. Returns 0, or 1 once the failure is reported.
 */
static int remap_limit(void)
{
    struct rlimit old, lim;
    char *m = mmap(NULL, 64 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *dest = mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *moved;
    unsigned long pages;

    if (m == MAP_FAILED || dest == MAP_FAILED || munmap(dest, PAGE) != 0) {
        return fail("mmap for a fixed mremap that shrinks");
    }
    row_held();
    m[0] = 'k';
    pages = mapped_pages();
    if (pages < 64 || getrlimit(RLIMIT_AS, &old) != 0) {
        return fail("the process's size");
    }
    row_held();
    lim.rlim_cur = (pages - 32) * PAGE;
    lim.rlim_max = old.rlim_max;
    if (setrlimit(RLIMIT_AS, &lim) != 0) {
        return fail("setrlimit RLIMIT_AS");
    }
    row_held();
    moved = mremap(m, 64 * PAGE, PAGE, MREMAP_MAYMOVE | MREMAP_FIXED, dest);
    if (setrlimit(RLIMIT_AS, &old) != 0) {
        return fail("setrlimit RLIMIT_AS back");
    }
    row_held();
    if (moved != dest || moved[0] != 'k') {
        return fail("a fixed mremap that shrinks was held to its old size");
    }
    row_held();
    munmap(moved, PAGE);
    return 0;
}

/**
 * Check that `m`, the result of a writable shared mmap of `fd`, a file of the
 * KRFFS root, which has no write-back, failed with ENODEV, and that a
 * read-only shared mapping stays read-only. Returns 0, or 1 once the failure
 * is reported.
 */
static int krffs_shared(int fd, char *m)
{
    if (m != MAP_FAILED || errno != ENODEV) {
        return fail("a writable shared mapping of a KRFFS file was not ENODEV");
    }
    row_held();
    m = mmap(NULL, PAGE, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED || mprotect(m, PAGE, PROT_READ | PROT_WRITE) != -1 || errno != EACCES) {
        return fail("a read-only shared mapping of a KRFFS file was made writable");
    }
    row_held();
    munmap(m, PAGE);
    return 0;
}

/** A cached private page and its writable alias both disappear at truncate. */
static int ext2_cached_truncate(int fd)
{
    char buf[4], *ro = MAP_FAILED, *rw = MAP_FAILED;
    int status, which, rc = 0;
    pid_t pid;
    volatile char byte;

    if (pread(fd, buf, sizeof(buf), PAGE) != (ssize_t)sizeof(buf)) {
        return fail("warm the file cache before a read-around fault");
    }
    ro = mmap(NULL, 2 * PAGE, PROT_READ, MAP_PRIVATE, fd, 0);
    if (ro != MAP_FAILED) {
        byte = ((volatile char *)ro)[PAGE];
        rw = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    }
    if (rw == MAP_FAILED) {
        rc = fail("cached private and shared aliases before truncate");
        goto out;
    }
    byte = ((volatile char *)rw)[PAGE];
    (void)byte;
    rw[PAGE] = 'S';
    if (ftruncate(fd, PAGE) != 0) {
        rc = fail("truncate below the writable cached page");
        goto out;
    }
    for (which = 0; which < 2; which++) {
        pid = fork();
        if (pid == 0) {
            struct rlimit core = {0, 0};

            if (setrlimit(RLIMIT_CORE, &core) != 0) {
                _exit(2);
            }
            if (which == 0) {
                ((volatile char *)rw)[PAGE] = 'X';
            } else {
                byte = ((volatile char *)ro)[PAGE];
            }
            _exit(0);
        }
        if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFSIGNALED(status) ||
            WTERMSIG(status) != SIGBUS) {
            rc = fail("truncate left a cached alias accessible past EOF");
            goto out;
        }
    }
    if (ftruncate(fd, 2 * PAGE) != 0 || fsync(fd) != 0 || pread(fd, buf, 4, 0) != 4 ||
        memcmp(buf, "root", 4) != 0 || pread(fd, buf, 4, PAGE) != 4 ||
        memcmp(buf, "\0\0\0\0", 4) != 0) {
        rc = fail("cached truncate and regrowth changed the file's prefix or kept old bytes");
    }
out:
    if (ro != MAP_FAILED) {
        munmap(ro, 2 * PAGE);
    }
    if (rw != MAP_FAILED) {
        munmap(rw, 2 * PAGE);
    }
    return rc;
}

/**
 * Check that a private mapping of 4 MiB of `fd`, a file on ext2 that begins
 * with "root", made without a hint, reads the file, and in a 64-bit program
 * on x86-64 starts on a 2 MiB boundary, as Linux 6.12's ext2 and ext4 place
 * a mapping of a file that holds a 2 MiB page (thp_get_unmapped_area); a
 * 32-bit program's is not aligned, there or on Linux. The file is cut back
 * to its two pages, and the check's row is reported. Returns 0, or 1 once
 * the failure is reported.
 */
static int ext2_aligned(int fd)
{
    char *m;
    int rc = 0;

    if (ftruncate(fd, 2 * HUGE_PAGE) != 0) {
        return fail("grow a file on the root to 4 MiB");
    }
    m = mmap(NULL, 2 * HUGE_PAGE, PROT_READ, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED || memcmp(m, "root", 4) != 0) {
        rc = fail("a private mapping of 4 MiB of a file on the root");
    } else if (THP_ALIGNS && ((uintptr_t)m & (HUGE_PAGE - 1)) != 0) {
        errno = 0;
        rc = fail("a mapping of 4 MiB of a file on the root is not on a 2 MiB boundary");
    }
    if (m != MAP_FAILED) {
        munmap(m, 2 * HUGE_PAGE);
    }
    if (rc == 0 && ftruncate(fd, 2 * PAGE) != 0) {
        rc = fail("cut a file on the root back to two pages");
    }
    if (rc == 0) {
        row_held();
    }
    return rc;
}

/**
 * Check that `m`, a shared mapping of two pages of `fd`, a file on ext2, maps
 * the file's pages: a child's store is seen, read and write see the mapping's
 * stores and it sees theirs, a readv's later buffer sees what its first
 * stored into the mapping, and every store reaches the file. Unmaps `m`.
 * Returns 0, or 1 once the failure is reported.
 */
static int ext2_shared(int fd, char *m)
{
    struct iovec parts[2];
    char buf[8];
    pid_t pid;

    if (m == MAP_FAILED) {
        return fail("mmap MAP_SHARED of a file on the root");
    }
    row_held();
    pid = fork();
    if (pid == 0) {
        memcpy(m + PAGE, "kid", 3);
        _exit(0);
    }
    if (!wait_ok(pid) || memcmp(m + PAGE, "kid", 3) != 0) {
        return fail("a child's store in a shared mapping of a file on the root");
    }
    row_held();
    memcpy(m, "root", 4);
    if (pread(fd, buf, 4, 0) != 4 || memcmp(buf, "root", 4) != 0) {
        return fail("a store in a shared mapping of a file on the root did not reach read");
    }
    row_held();
    m[100] = 'S';
    if (pwrite(fd, "WR", 2, 8) != 2 || memcmp(m + 8, "WR", 2) != 0 || m[100] != 'S') {
        return fail("a write to a file on the root was not seen through its shared mapping, "
                    "or lost a store beside it");
    }
    row_held();
    parts[0].iov_base = m + 201;
    parts[0].iov_len = 1;
    parts[1].iov_base = buf;
    parts[1].iov_len = 1;
    buf[0] = 0;
    if (pwrite(fd, "AB", 2, 200) != 2 || lseek(fd, 200, SEEK_SET) != 200 ||
        readv(fd, parts, 2) != 2 || m[201] != 'A' || buf[0] != 'A') {
        return fail("a readv's second buffer did not see the byte its first, in a shared "
                    "mapping of a file on the root, stored there");
    }
    row_held();
    if (msync(m, 2 * PAGE, MS_SYNC) != 0) {
        return fail("msync of a shared mapping of a file on the root");
    }
    row_held();
    munmap(m, 2 * PAGE);
    pid = fork();
    if (pid == 0) {
        char *c = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);

        memcpy(c + 16, "gone", 4);
        _exit(c == MAP_FAILED);
    }
    if (!wait_ok(pid) || pread(fd, buf, 4, 16) != 4 || memcmp(buf, "gone", 4) != 0 ||
        pread(fd, buf, 3, PAGE) != 3 || memcmp(buf, "kid", 3) != 0 || pread(fd, buf, 1, 100) != 1 ||
        buf[0] != 'S') {
        return fail("the stores of a shared mapping of a file on the root did not reach it");
    }
    row_held();
    if (ext2_cached_truncate(fd) != 0) {
        return 1;
    }
    row_held();
    return 0;
}

/**
 * Check a shared mapping of a file of the root, by `ext2_shared` or
 * `krffs_shared` as the root's type is. Returns 0, or 1 once the failure is
 * reported.
 */
static int root_shared(void)
{
    struct statfs sfs;
    char *m;
    int rc;
    int fd = open(FILE_PATH, O_RDWR | O_CREAT | O_TRUNC, 0600);

    if (fd < 0 || ftruncate(fd, 2 * PAGE) != 0 || fstatfs(fd, &sfs) != 0) {
        return fail("a file on the root for a shared mapping");
    }
    row_held();
    m = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    rc = sfs.f_type == EXT2_MAGIC ? ext2_shared(fd, m) : krffs_shared(fd, m);
    if (rc == 0 && sfs.f_type == EXT2_MAGIC) {
        rc = ext2_aligned(fd);
    }
    close(fd);
    unlink(FILE_PATH);
    return rc;
}

/**
 * Check that the pages of `seg_data`, written, and of `seg_ro`, read, hold
 * the program file's bytes again after MADV_DONTNEED drops them. The
 * accesses are volatile, so the compiler reads the memory and not the
 * initializers. Returns 0, or 1 once the failure is reported.
 */
static int segments(void)
{
    volatile unsigned char *d = seg_data;
    const volatile unsigned char *r = seg_ro;

    if (d[0] != 'd' || d[PAGE] != 'e' || r[0] != 'r' || r[PAGE] != 'o') {
        return fail("the program's data did not hold its file's bytes");
    }
    d[0] = 'X';
    d[PAGE] = 'Y';
    d[PAGE + 1] = 'Z';
    if (madvise(seg_data, 2 * PAGE, MADV_DONTNEED) != 0 || d[0] != 'd' || d[PAGE - 1] != 'd' ||
        d[PAGE] != 'e' || d[PAGE + 1] != 0 || d[2 * PAGE - 1] != 'e') {
        return fail("a dropped page of the program's data segment did not read its file again");
    }
    if (madvise((void *)seg_ro, 2 * PAGE, MADV_DONTNEED) != 0 || r[0] != 'r' ||
        r[PAGE - 1] != 'r' || r[PAGE] != 'o' || r[2 * PAGE - 1] != 'o') {
        return fail("a dropped page of the program's read-only data did not read its file again");
    }
    return 0;
}

/**
 * Run `path`, the program's own file or a copy of it, with the argument
 * `mode`, "segments" or "unlinked", which `main` answers by checking its
 * segments (`segments`), after renaming and unlinking its own file for
 * "unlinked". The check runs in a program that execve loaded, since the
 * kernel copies the segments of the first program a boot loads, as which
 * this one may run. Returns whether the child exited with status 0.
 */
static int run_segments(const char *path, const char *mode)
{
    char *args[3];
    pid_t pid;

    args[0] = (char *)path;
    args[1] = (char *)mode;
    args[2] = NULL;
    pid = fork();
    if (pid == 0) {
        execv(path, args);
        _exit(1);
    }
    return pid > 0 && wait_ok(pid);
}

/**
 * Check the program's segments in a child that execs the program, and in
 * one that execs a copy of it at EXE_PATH and renames and unlinks the copy
 * first. Returns 0, or 1 once the failure is reported.
 */
static int exec_segments(void)
{
    static char buf[PAGE];
    ssize_t n = 0;
    int in, out;

    if (!run_segments("/proc/self/exe", "segments")) {
        return fail("the segments of the program run by execve");
    }
    row_held();
    in = open("/proc/self/exe", O_RDONLY);
    out = open(EXE_PATH, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    while (in >= 0 && out >= 0 && (n = read(in, buf, sizeof(buf))) > 0 &&
           write(out, buf, (size_t)n) == n) {
    }
    if (in < 0 || out < 0 || n != 0 || close(out) != 0) {
        return fail("a copy of the program");
    }
    row_held();
    close(in);
    if (!run_segments(EXE_PATH, "unlinked")) {
        unlink(EXE_PATH);
        return fail("the segments of a program whose file was renamed and unlinked");
    }
    row_held();
    return 0;
}

int main(int argc, char **argv)
{
    probe_args(argc, argv);
    if (probe_words() == 1 && strcmp(probe_word(1), "unlinked") == 0 &&
        (rename(EXE_PATH, MOVED) != 0 || unlink(MOVED) != 0)) {
        return fail("the rename and the unlink of the program's own file");
    }
    if (probe_words() == 1) {
        return segments();
    }
    report_start();
    if (private_file() || shared() || root_shared() || truncated_shared() || truncated_wide() ||
        huge_walks() || readonly_shared() || offsets() || remaps() || advice() || protect_stops() ||
        forced_write() || remap_limit() || exec_segments()) {
        return 1;
    }
    report_end();
    printf("MAPMUSL OK\n");
    return 0;
}
