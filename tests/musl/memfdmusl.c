/**
 * memfdmusl: check memfd_create and the seals of a memfd.
 *
 * A memfd made with MFD_CLOEXEC and MFD_ALLOW_SEALING is close-on-exec, open
 * for reading and writing, a regular file of mode 0777 whose size fstat
 * reports, and its /proc/self/fd link reads "/memfd:NAME (deleted)". Its
 * seals start empty. Under F_SEAL_SHRINK a smaller ftruncate fails with EPERM
 * and a larger one succeeds. Under F_SEAL_GROW a write past the end and a
 * larger ftruncate fail and an overwrite succeeds, and a writev whose two
 * elements together pass the end fails whole, with nothing written. Under
 * F_SEAL_FUTURE_WRITE a write fails; after F_SEAL_SEAL a further seal fails;
 * F_GET_SEALS reports each one.
 *
 * A memfd made without MFD_ALLOW_SEALING starts with F_SEAL_SEAL, and
 * F_ADD_SEALS on it fails with EPERM. One made with MFD_NOEXEC_SEAL has mode
 * 0666 and F_SEAL_EXEC, and `fchmod` to 0755 fails with EPERM. The seal
 * commands fail with EINVAL on a KRFFS file. On a tmpfs file with a name
 * F_ADD_SEALS fails with EPERM, since the file is sealed against sealing, but
 * with EINVAL for a bit that is no seal, which is checked first; through a
 * descriptor not open for writing it fails with EPERM for that bit too, since
 * the access is checked before both. A name of 250 bytes, MFD_HUGETLB and an
 * unknown flag fail with EINVAL.
 *
 * Prints `MEMFDMUSL OK` and exits 0 on success, and prints
 * `MEMFDMUSL FAIL (detail)` and exits 1 otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/uio.h>
#include <unistd.h>

/* The flag and the seal of Linux 6.3, which this musl does not name. */
#define MFD_NOEXEC_SEAL 0x0008u
#define F_SEAL_EXEC     0x0020

/* The bit after F_SEAL_EXEC, the last seal. */
#define NO_SEAL 0x0040

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("MEMFDMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

/** Whether the call's -1 came with `err`. */
static int refused(long rc, int err)
{
    return rc == -1 && errno == err;
}

/** Whether the file open on `fd` is `size` bytes long. */
static int size_is(int fd, off_t size)
{
    struct stat st;

    return fstat(fd, &st) == 0 && st.st_size == size;
}

/**
 * Check a sealable memfd: its descriptor, its file and its /proc/self/fd
 * link, then each seal in turn. Returns 0, or 1 after the failure is printed.
 */
static int sealing(void)
{
    struct iovec grow[2] = {{(void *)"y", 1}, {(void *)"z", 1}};
    struct iovec fit[2] = {{(void *)"M", 1}, {(void *)"e", 1}};
    char link[64], path[32], buf[8];
    struct stat st;
    int fd = memfd_create("memfdmusl", MFD_CLOEXEC | MFD_ALLOW_SEALING);
    ssize_t n;

    if (fd < 0 || fcntl(fd, F_GETFD) != FD_CLOEXEC || (fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDWR) {
        return fail("the descriptor");
    }
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) || (st.st_mode & 07777) != 0777 ||
        st.st_size != 0 || write(fd, "memory", 6) != 6 || !size_is(fd, 6)) {
        return fail("the file");
    }
    snprintf(path, sizeof(path), "/proc/self/fd/%d", fd);
    n = readlink(path, link, sizeof(link) - 1);
    if (n < 0 || (link[n] = '\0', strcmp(link, "/memfd:memfdmusl (deleted)") != 0)) {
        return fail("the /proc/self/fd name");
    }
    if (fcntl(fd, F_GET_SEALS) != 0 || fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK) != 0 ||
        !refused(ftruncate(fd, 3), EPERM) || ftruncate(fd, 10) != 0 || !size_is(fd, 10)) {
        return fail("F_SEAL_SHRINK");
    }
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_GROW) != 0 || !refused(pwrite(fd, "x", 1, 10), EPERM) ||
        !refused(ftruncate(fd, 11), EPERM) || pwrite(fd, "M", 1, 0) != 1 || !size_is(fd, 10)) {
        return fail("F_SEAL_GROW");
    }
    if (lseek(fd, 9, SEEK_SET) != 9 || !refused(writev(fd, grow, 2), EPERM) ||
        pread(fd, buf, 1, 9) != 1 || buf[0] != '\0' || !size_is(fd, 10) ||
        lseek(fd, 0, SEEK_SET) != 0 || writev(fd, fit, 2) != 2) {
        return fail("a writev under F_SEAL_GROW");
    }
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE) != 0 ||
        !refused(pwrite(fd, "m", 1, 0), EPERM) || pread(fd, buf, 6, 0) != 6 ||
        memcmp(buf, "Memory", 6) != 0) {
        return fail("F_SEAL_FUTURE_WRITE");
    }
    if (fcntl(fd, F_ADD_SEALS, F_SEAL_SEAL) != 0 ||
        !refused(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE), EPERM) ||
        fcntl(fd, F_GET_SEALS) !=
            (F_SEAL_SHRINK | F_SEAL_GROW | F_SEAL_FUTURE_WRITE | F_SEAL_SEAL)) {
        return fail("F_SEAL_SEAL");
    }
    close(fd);
    return 0;
}

/**
 * Check a memfd without MFD_ALLOW_SEALING, one with MFD_NOEXEC_SEAL, the seal
 * commands on a KRFFS file and on a named tmpfs file, and the names and flags
 * memfd_create rejects. Returns 0, or 1 after the failure is printed.
 */
static int others(void)
{
    char longname[251];
    struct stat st;
    int fd, ro;

    fd = memfd_create("plain", 0);
    if (fd < 0 || fcntl(fd, F_GETFD) != 0 || fcntl(fd, F_GET_SEALS) != F_SEAL_SEAL ||
        !refused(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE), EPERM)) {
        return fail("a memfd without MFD_ALLOW_SEALING");
    }
    close(fd);
    fd = memfd_create("noexec", MFD_NOEXEC_SEAL);
    if (fd < 0 || fstat(fd, &st) != 0 || (st.st_mode & 07777) != 0666 ||
        fcntl(fd, F_GET_SEALS) != F_SEAL_EXEC || !refused(fchmod(fd, 0755), EPERM)) {
        return fail("MFD_NOEXEC_SEAL");
    }
    close(fd);
    fd = open("/tmp/memfdmusl", O_RDWR | O_CREAT | O_TRUNC, 0600);
    if (fd < 0 || !refused(fcntl(fd, F_GET_SEALS), EINVAL) ||
        !refused(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE), EINVAL)) {
        return fail("the seal commands on a KRFFS file");
    }
    close(fd);
    unlink("/tmp/memfdmusl");
    fd = open("/dev/shm/memfdmusl", O_RDWR | O_CREAT | O_TRUNC, 0600);
    ro = open("/dev/shm/memfdmusl", O_RDONLY);
    if (fd < 0 || ro < 0 || fcntl(fd, F_GET_SEALS) != F_SEAL_SEAL ||
        !refused(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE), EPERM) ||
        !refused(fcntl(fd, F_ADD_SEALS, NO_SEAL), EINVAL) ||
        !refused(fcntl(ro, F_ADD_SEALS, F_SEAL_WRITE), EPERM) ||
        !refused(fcntl(ro, F_ADD_SEALS, NO_SEAL), EPERM)) {
        return fail("the seal commands on a named tmpfs file");
    }
    close(fd);
    close(ro);
    unlink("/dev/shm/memfdmusl");
    memset(longname, 'n', sizeof(longname) - 1);
    longname[sizeof(longname) - 1] = '\0';
    if (!refused(memfd_create(longname, 0), EINVAL) ||
        !refused(memfd_create("huge", MFD_HUGETLB), EINVAL) ||
        !refused(memfd_create("odd", 0x100), EINVAL)) {
        return fail("the refused names and flags");
    }
    return 0;
}

int main(void)
{
    if (sealing() || others()) {
        return 1;
    }
    printf("MEMFDMUSL OK\n");
    return 0;
}
