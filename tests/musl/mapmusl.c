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
 * x86-64 a truncation to 4 GiB of a file of 4 GiB and a page takes the last
 * page alone out of a mapping of it, and a mapping of a file or of shared
 * memory of 16 TiB is EOVERFLOW and one of a page less is made; mprotect,
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
 * gives a private anonymous mapping zero pages again, and madvise and msync
 * fail with EINVAL for a length that would round up to 0. A write through
 * /proc/self/mem to a read-only page a fork shared leaves the page read-only.
 * On an ext2 root, through a shared mapping of a file the parent sees a
 * child's store, read(2) sees a store, the mapping sees a write(2) without
 * losing a store beside it, and its stores reach the file after munmap and
 * after a mapper's exit; on the KRFFS root, one that may be written is ENODEV,
 * and a read-only one through a descriptor open for writing may not be made
 * writable.
 *
 * Prints `MAPMUSL OK` and exits 0 on success, and prints
 * `MAPMUSL FAIL (detail)` and exits 1 otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <setjmp.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define FILE_PATH "/tmp/mapmusl"
#define SHM_PATH  "/dev/shm/mapmusl"
#define PAGE      4096

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

/** Report one failed check with errno and return 1, the failure status. */
static int fail(const char *what)
{
    printf("MAPMUSL FAIL (%s: errno %d)\n", what, errno);
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
    m = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (m == MAP_FAILED) {
        return fail("mmap MAP_PRIVATE of a file");
    }
    close(fd);
    if (m[0] != 'a' || m[PAGE - 1] != 'a' || m[PAGE] != 'b' || m[PAGE + 99] != 'b' ||
        m[PAGE + 100] != 0 || m[2 * PAGE - 1] != 0) {
        return fail("the mapping did not hold the file's bytes");
    }
    m[0] = 'X';
    fd = open(FILE_PATH, O_RDONLY);
    if (fd < 0 || read(fd, check, 1) != 1 || check[0] != 'a') {
        return fail("a store in a private mapping reached the file");
    }
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
    if (munmap(m, 2 * PAGE) != 0) {
        return fail("munmap");
    }
    m = mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, open(FILE_PATH, O_RDONLY), PAGE);
    if (m == MAP_FAILED || m[0] != 'b') {
        return fail("a mapping from an offset");
    }
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
    pid = fork();
    if (pid == 0) {
        anon[10] = 's';
        _exit(0);
    }
    if (!wait_ok(pid) || anon[10] != 's') {
        return fail("a child's store in a shared anonymous mapping");
    }
    munmap(anon, PAGE);

    fd = open(SHM_PATH, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || ftruncate(fd, PAGE) != 0) {
        return fail("a file in /dev/shm");
    }
    m = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        return fail("mmap MAP_SHARED of a file in /dev/shm");
    }
    pid = fork();
    if (pid == 0) {
        m[5] = 'c';
        _exit(0);
    }
    if (!wait_ok(pid) || m[5] != 'c') {
        return fail("a child's store in a shared file mapping");
    }
    m[6] = 'd';
    if (pread(fd, check, 2, 5) != 2 || check[0] != 'c' || check[1] != 'd') {
        return fail("a store in a shared mapping did not reach read");
    }
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
    m = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        return fail("mmap of a file in /dev/shm to truncate");
    }
    m[0] = 'o';
    if (ftruncate(fd, 0) != 0 || ftruncate(fd, PAGE) != 0 || pwrite(fd, "n", 1, 0) != 1 ||
        m[0] != 'n') {
        return fail("a shared mapping kept a page its file's truncation freed");
    }
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
    m = mmap(NULL, four + PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        return fail("mmap of a file in /dev/shm of 4 GiB and a page");
    }
    m[0] = 'a';
    m[four] = 'b';
    if (ftruncate(fd, (off_t)four) != 0 || mincore(m, PAGE, &in) != 0 || !(in & 1)) {
        return fail("a truncation to 4 GiB took the mapping's first page out");
    }
    if (mincore(m + four, PAGE, &in) != 0 || (in & 1)) {
        return fail("a truncation to 4 GiB left the page past it in the mapping");
    }
    munmap(m, four + PAGE);
    if (mmap(NULL, huge, PROT_READ, MAP_PRIVATE, fd, 0) != MAP_FAILED || errno != EOVERFLOW ||
        mmap(NULL, huge, PROT_READ, MAP_SHARED | MAP_ANONYMOUS, -1, 0) != MAP_FAILED ||
        errno != EOVERFLOW) {
        return fail("a mapping of a file or of shared memory of 2^32 pages is not EOVERFLOW");
    }
    m = mmap(NULL, huge - PAGE, PROT_READ, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED || munmap(m, huge - PAGE) != 0) {
        return fail("a mapping of shared memory of 2^32 pages less one");
    }
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
    far = m + big - PAGE;
    if (mprotect(far, PAGE, PROT_READ | PROT_WRITE) != 0) {
        return fail("mprotect of the last page of 64 TiB");
    }
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
    if (mprotect(m, big, PROT_READ) != 0) {
        return fail("mprotect of 64 TiB to one region");
    }
    if (getrusage(RUSAGE_SELF, &ru) != 0 || ru.ru_maxrss <= 0) {
        return fail("getrusage with 64 TiB mapped");
    }
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
    if (WIFEXITED(status) && WEXITSTATUS(status) == 2) {
        return fail("a shrink of 64 TiB to half was refused");
    }
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV) {
        return fail("a shrink of 64 TiB to half kept its last page");
    }
    if (munmap(m, big) != 0) {
        return fail("munmap of 64 TiB");
    }
    m = mmap(NULL, 2 * half, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (m == MAP_FAILED || m[half - PAGE] != 0) {
        return fail("mmap of 64 TiB to move half of");
    }
    moved = mremap(m, half, half, MREMAP_MAYMOVE | MREMAP_FIXED, m + half);
    if (moved != m + half || mincore(moved + half - PAGE, PAGE, &in) != 0 || !(in & 1)) {
        return fail("a move of 32 TiB did not carry its last page");
    }
    if (munmap(m + half, half) != 0) {
        return fail("munmap of the 32 TiB moved");
    }
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
    close(fd);
    fd = open(SHM_PATH, O_RDONLY);
    ro = mmap(NULL, PAGE, PROT_READ, MAP_SHARED, fd, 0);
    priv = mmap(NULL, PAGE, PROT_READ, MAP_PRIVATE, fd, 0);
    if (fd < 0 || ro == MAP_FAILED || priv == MAP_FAILED) {
        return fail("mmap of a file opened read-only");
    }
    if (mprotect(ro, PAGE, PROT_READ | PROT_WRITE) != -1 || errno != EACCES) {
        return fail("mprotect made a read-only file's shared mapping writable");
    }
    if (mprotect(priv, PAGE, PROT_READ | PROT_WRITE) != 0) {
        return fail("mprotect of a read-only file's private mapping");
    }
    priv[0] = 'P';
    if (ro[0] != 'f') {
        return fail("a store in a private mapping reached the shared one");
    }
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
 * touches it dies of SIGSEGV.
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
    m[0] = 'o';
    munmap(m, PAGE);
    fd = open(SHM_PATH, O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0) {
        return fail("a file in /dev/shm for the offsets");
    }
    if (MMAP_PAGES(PAGE, PROT_READ, MAP_SHARED, fd, 0xFFFFFFFFu) != -1 || errno != EOVERFLOW) {
        return fail("mmap2 of a file whose last page is past 32 bits");
    }
    m = (char *)MMAP_PAGES(PAGE, PROT_READ, MAP_SHARED, fd, 0x80000000u);
    if (m == MAP_FAILED) {
        return fail("mmap2 of a file at a page offset of 2^31");
    }
    pid = fork();
    if (pid == 0) {
        _exit(m[0] == 0 ? 0 : 1);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV) {
        return fail("a touch of a file in memory past its end was not SIGSEGV");
    }
    munmap(m, PAGE);
    close(fd);
    unlink(SHM_PATH);
    return 0;
}

/**
 * Check mremap's growth, move and shrink, its EINVAL for a length that wraps,
 * and madvise and msync. Returns 0, or 1 once the failure is reported.
 */
static int remaps(void)
{
    char *m = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *blocker, *moved, *cut;

    if (m == MAP_FAILED) {
        return fail("mmap for mremap");
    }
    m[0] = 'r';
    blocker =
        mmap(m + PAGE, PAGE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (blocker == MAP_FAILED) {
        return fail("the mapping that blocks growth in place");
    }
    if (mremap(m, PAGE, 2 * PAGE, 0) != MAP_FAILED || errno != ENOMEM) {
        return fail("mremap without MREMAP_MAYMOVE grew into another mapping");
    }
    moved = mremap(m, PAGE, 3 * PAGE, MREMAP_MAYMOVE);
    if (moved == MAP_FAILED || moved == m || moved[0] != 'r') {
        return fail("mremap did not move the mapping with its bytes");
    }
    moved[3 * PAGE - 1] = 'e';
    if (mremap(moved, 3 * PAGE, PAGE, 0) != moved) {
        return fail("mremap did not shrink in place");
    }
    if (moved[0] != 'r') {
        return fail("the page a shrink kept lost its bytes");
    }
    cut = mmap(moved + PAGE, 2 * PAGE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE,
               -1, 0);
    if (cut != moved + PAGE || cut[2 * PAGE - 1] != 0) {
        return fail("the pages a shrink cut were still mapped");
    }
    munmap(cut, 2 * PAGE);
    if (mremap(moved, (size_t)-PAGE, PAGE, MREMAP_MAYMOVE | MREMAP_FIXED, moved) != MAP_FAILED ||
        errno != EINVAL || moved[0] != 'r') {
        return fail("mremap took an old length that wraps past the end of memory");
    }
    munmap(blocker, PAGE);
    munmap(moved, PAGE);

    m = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (m == MAP_FAILED) {
        return fail("mmap for madvise");
    }
    m[0] = 'z';
    m[PAGE] = 'z';
    if (madvise(m, PAGE, MADV_DONTNEED) != 0 || m[0] != 0 || m[PAGE] != 'z') {
        return fail("MADV_DONTNEED");
    }
    if (madvise(m, (size_t)-1, MADV_DONTNEED) != -1 || errno != EINVAL) {
        return fail("madvise took a length that rounds up to 0 for an empty range");
    }
    if (msync(m, (size_t)-1, MS_SYNC) != -1 || errno != EINVAL) {
        return fail("msync took a length that rounds up to 0 for an empty range");
    }
    munmap(m, 2 * PAGE);
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
    m[0] = 'a';
    pid = fork();
    if (pid == 0) {
        _exit(0);
    }
    if (!wait_ok(pid) || mprotect(m, PAGE, PROT_READ) != 0) {
        return fail("fork and mprotect for a forced write");
    }
    fd = open("/proc/self/mem", O_RDWR);
    if (fd < 0 || pwrite(fd, "b", 1, (off_t)(unsigned long)m) != 1 || m[0] != 'b') {
        return fail("a write through /proc/self/mem");
    }
    close(fd);
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_segv;
    sigaction(SIGSEGV, &sa, &old);
    if (sigsetjmp(segv_jump, 1) == 0) {
        *(volatile char *)m = 'c';
        sigaction(SIGSEGV, &old, NULL);
        return fail("a store reached a read-only page after a write through /proc/self/mem");
    }
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
    m[0] = 'k';
    pages = mapped_pages();
    if (pages < 64 || getrlimit(RLIMIT_AS, &old) != 0) {
        return fail("the process's size");
    }
    lim.rlim_cur = (pages - 32) * PAGE;
    lim.rlim_max = old.rlim_max;
    if (setrlimit(RLIMIT_AS, &lim) != 0) {
        return fail("setrlimit RLIMIT_AS");
    }
    moved = mremap(m, 64 * PAGE, PAGE, MREMAP_MAYMOVE | MREMAP_FIXED, dest);
    if (setrlimit(RLIMIT_AS, &old) != 0) {
        return fail("setrlimit RLIMIT_AS back");
    }
    if (moved != dest || moved[0] != 'k') {
        return fail("a fixed mremap that shrinks was held to its old size");
    }
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
    m = mmap(NULL, PAGE, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED || mprotect(m, PAGE, PROT_READ | PROT_WRITE) != -1 || errno != EACCES) {
        return fail("a read-only shared mapping of a KRFFS file was made writable");
    }
    munmap(m, PAGE);
    return 0;
}

/**
 * Check that `m`, a shared mapping of two pages of `fd`, a file on ext2, maps
 * the file's pages: a child's store is seen, read and write see the mapping's
 * stores and it sees theirs, and every store reaches the file. Unmaps `m`.
 * Returns 0, or 1 once the failure is reported.
 */
static int ext2_shared(int fd, char *m)
{
    char buf[8];
    pid_t pid;

    if (m == MAP_FAILED) {
        return fail("mmap MAP_SHARED of a file on the root");
    }
    pid = fork();
    if (pid == 0) {
        memcpy(m + PAGE, "kid", 3);
        _exit(0);
    }
    if (!wait_ok(pid) || memcmp(m + PAGE, "kid", 3) != 0) {
        return fail("a child's store in a shared mapping of a file on the root");
    }
    memcpy(m, "root", 4);
    if (pread(fd, buf, 4, 0) != 4 || memcmp(buf, "root", 4) != 0) {
        return fail("a store in a shared mapping of a file on the root did not reach read");
    }
    m[100] = 'S';
    if (pwrite(fd, "WR", 2, 8) != 2 || memcmp(m + 8, "WR", 2) != 0 || m[100] != 'S') {
        return fail("a write to a file on the root was not seen through its shared mapping, "
                    "or lost a store beside it");
    }
    if (msync(m, 2 * PAGE, MS_SYNC) != 0) {
        return fail("msync of a shared mapping of a file on the root");
    }
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
    m = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    rc = sfs.f_type == EXT2_MAGIC ? ext2_shared(fd, m) : krffs_shared(fd, m);
    close(fd);
    unlink(FILE_PATH);
    return rc;
}

int main(void)
{
    if (private_file() || shared() || root_shared() || truncated_shared() || truncated_wide() ||
        huge_walks() || readonly_shared() || offsets() || remaps() || forced_write() ||
        remap_limit()) {
        return 1;
    }
    printf("MAPMUSL OK\n");
    return 0;
}
