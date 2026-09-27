/**
 * File mappings, shared mappings, mremap and madvise.
 *
 * A private mapping of a file reads the file's bytes, including a second
 * page and zeros past the end; a store changes the mapping and not the file;
 * the mapping outlives the descriptor it was made from; a child after fork
 * sees the parent's bytes and its own stores stay its own. A shared
 * anonymous mapping and a shared mapping of a file in /dev/shm carry a
 * child's store to the parent, and a store through the mapping of the file
 * reaches read(2), and after a truncation and a growth a write to the file
 * is seen through the mapping. mprotect will not make a shared mapping of a
 * file opened read-only writable, and will a private one. Shared anonymous
 * memory ignores a raw mmap2's page offset, a file's offset whose last page
 * would not fit in 32 bits is EOVERFLOW, and a page of a file in memory
 * past 1 GiB is never backed. mremap grows a mapping in place or moves it
 * with its bytes, and shrinks one, and refuses an old length that would
 * wrap past the end of memory, leaving the mapping; a fixed mremap that
 * shrinks a mapping is held to RLIMIT_AS by what it leaves;
 * madvise(MADV_DONTNEED) gives a private anonymous mapping zero pages
 * again, and madvise and msync refuse a length that would round up to 0. A
 * write through /proc/self/mem to a read-only page a fork shared leaves the
 * page read-only.
 *
 * Prints "MAPMUSL OK" on success and "MAPMUSL FAIL (detail)" otherwise.
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
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#define FILE_PATH "/tmp/mapmusl"
#define SHM_PATH  "/dev/shm/mapmusl"
#define PAGE      4096

static int fail(const char *what)
{
    printf("MAPMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

static int wait_ok(pid_t pid)
{
    int status;

    return waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

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

/* A truncation takes the pages past the new end out of every mapping of a
   file in memory: after the file shrinks to nothing and grows again, a
   write to it is seen through the mapping, which maps the file's page and
   no longer the one the truncation freed. */
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

/* A shared mapping of a file opened read-only may never be written:
   mprotect refuses write with EACCES, since stores through the mapping
   would reach the file. A private mapping of the same descriptor may be
   written, its stores its own. */
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

/* The page offset of a raw mmap2. Shared anonymous memory ignores it; an
   offset of 2^31 pages would double the length of the memory's table of
   pages until the length wrapped to zero, and the first touch would not end.
   A file's offset whose last page would not fit in 32 bits is EOVERFLOW,
   and the page of a file in memory 2^31 pages in, past the 1 GiB such a
   file holds, is never backed: a child that touches it dies of SIGSEGV. */
static int offsets(void)
{
    char *m = (char *)syscall(SYS_mmap2, 0, PAGE, PROT_READ | PROT_WRITE,
                              MAP_SHARED | MAP_ANONYMOUS, -1, 0x80000000u);
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
    if (syscall(SYS_mmap2, 0, PAGE, PROT_READ, MAP_SHARED, fd, 0xFFFFFFFFu) != -1 ||
        errno != EOVERFLOW) {
        return fail("mmap2 of a file whose last page is past 32 bits");
    }
    m = (char *)syscall(SYS_mmap2, 0, PAGE, PROT_READ, MAP_SHARED, fd, 0x80000000u);
    if (m == MAP_FAILED) {
        return fail("mmap2 of a file at a page offset of 2^31");
    }
    pid = fork();
    if (pid == 0) {
        _exit(m[0] == 0 ? 0 : 1);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFSIGNALED(status) || WTERMSIG(status) != SIGSEGV) {
        return fail("a touch of a file in memory past 1 GiB was not SIGSEGV");
    }
    munmap(m, PAGE);
    close(fd);
    unlink(SHM_PATH);
    return 0;
}

static int remaps(void)
{
    char *m = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    char *blocker, *moved;

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
    if (mremap(moved, 0xFFFFF000u, PAGE, MREMAP_MAYMOVE | MREMAP_FIXED, moved) != MAP_FAILED ||
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
    if (madvise(m, 0xFFFFFFFFu, MADV_DONTNEED) != -1 || errno != EINVAL) {
        return fail("madvise took a length that rounds up to 0 for an empty range");
    }
    if (msync(m, 0xFFFFFFFFu, MS_SYNC) != -1 || errno != EINVAL) {
        return fail("msync took a length that rounds up to 0 for an empty range");
    }
    munmap(m, 2 * PAGE);
    return 0;
}

static sigjmp_buf segv_jump;

static void on_segv(int sig)
{
    (void)sig;
    siglongjmp(segv_jump, 1);
}

/* A write through /proc/self/mem to a page a fork left copy-on-write, in a
   region mprotect has made read-only since, goes into a page of this
   process's own that stays read-only: a store from the program after it
   faults, since the region allows none. */
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

/* The pages this process's regions cover, the first number of
   /proc/self/statm, read with no allocation that would change it. */
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

/* A fixed mremap that shrinks a mapping of 64 pages to one is held to
   RLIMIT_AS by what it leaves, 63 pages less than now: under a limit 32
   pages below the present size it succeeds, with the pages between left
   for the stack to grow into. */
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

int main(void)
{
    if (private_file() || shared() || truncated_shared() || readonly_shared() || offsets() ||
        remaps() || forced_write() || remap_limit()) {
        return 1;
    }
    printf("MAPMUSL OK\n");
    return 0;
}
