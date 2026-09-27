/**
 * File mappings, shared mappings, mremap and madvise.
 *
 * A private mapping of a file reads the file's bytes, including a second
 * page and zeros past the end; a store changes the mapping and not the file;
 * the mapping outlives the descriptor it was made from; a child after fork
 * sees the parent's bytes and its own stores stay its own. A shared
 * anonymous mapping and a shared mapping of a file in /dev/shm carry a
 * child's store to the parent, and a store through the mapping of the file
 * reaches read(2). mremap grows a mapping in place or moves it with its
 * bytes, and shrinks one, and refuses an old length that would wrap past
 * the end of memory, leaving the mapping; madvise(MADV_DONTNEED) gives a
 * private anonymous mapping zero pages again.
 *
 * Prints "MAPMUSL OK" on success and "MAPMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
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
    munmap(m, 2 * PAGE);
    return 0;
}

int main(void)
{
    if (private_file() || shared() || remaps()) {
        return 1;
    }
    printf("MAPMUSL OK\n");
    return 0;
}
