/* Check KRFFS boot-root holders opened through a bind mount.
 *
 * Twenty rows, each a kind of holder whose file loses its name by unlink or
 * by a rename over it, through the original directory and then through its
 * bind. Each row requires the free blocks of the file system to come back
 * exactly, so etude's report (probe_report.h), an assertion for each row for
 * the case ETUDE_CASE names, is written only once the rows have ended: a
 * record written while they run would grow etude-guest's copy of the output
 * on the same file system. The first failed check ends the program, failing
 * its row with the check and errno. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../probe_report.h"

/* The rows in the order they run, each its assertion's identity: the kind
 * of holder, how the name goes, and the mount it goes through. */
static const char *const rows[20] = {
    "descriptor-unlink",
    "descriptor-rename",
    "mapping-unlink",
    "mapping-rename",
    "flight-unlink",
    "flight-rename",
    "image-unlink",
    "image-rename",
    "forked-image-unlink",
    "forked-image-rename",
    "descriptor-unlink-bind",
    "descriptor-rename-bind",
    "mapping-unlink-bind",
    "mapping-rename-bind",
    "flight-unlink-bind",
    "flight-rename-bind",
    "image-unlink-bind",
    "image-rename-bind",
    "forked-image-unlink-bind",
    "forked-image-rename-bind",
};
static int rows_done;

static char work[256], original[320], alias[320];
static char program[384], replacement[384], churn[384], bound[384], bound_replacement[384];
static pid_t child;
static int mounted, created, remove_through_bind;
static void *mapped;
static size_t mapped_len;

/** Reap the image and remove the bind and every fixture on every exit. */
static void cleanup(void)
{
    int fd;

    if (mapped && mapped != MAP_FAILED) {
        munmap(mapped, mapped_len);
        mapped = NULL;
    }
    if (child > 0) {
        kill(child, SIGKILL);
        waitpid(child, NULL, 0);
        child = 0;
    }
    for (fd = 3; fd < 1024; fd++) {
        close(fd);
    }
    if (mounted) {
        umount(alias);
        mounted = 0;
    }
    if (created) {
        unlink(program);
        unlink(replacement);
        unlink(churn);
        rmdir(alias);
        rmdir(original);
        rmdir(work);
    }
}

/** End through the cleanup installed before creating scratch. */
static void interrupted(int sig)
{
    exit(128 + sig);
}

/** Write the report: the rows that ended held, and then, when `why` is not
 * null, the next row failed with `why` and the rest not run. */
static void report_rows(const char *why)
{
    int i;

    report_begin(report_case(environ, "guest.textbusy.bindhold"));
    for (i = 0; i < 20; i++) {
        report_plan(rows[i]);
    }
    for (i = 0; i < rows_done; i++) {
        report_held();
    }
    if (why != NULL) {
        report_stop(why);
    } else {
        report_end();
    }
}

/** Require a row and print the operation that failed. */
static void check(int ok, const char *what)
{
    char detail[128];

    if (!ok) {
        snprintf(detail, sizeof(detail), "%s: errno %d", what, errno);
        printf("BINDHOLDMUSL FAIL (%s): errno %d\n", what, errno);
        report_rows(detail);
        exit(1);
    }
}

/** Copy this staged executable into a writable file on the boot root. */
static void copy(const char *to)
{
    char buf[4096];
    ssize_t n;
    int in = open("/musl/bindholdmusl", O_RDONLY);
    int out = open(to, O_CREAT | O_TRUNC | O_WRONLY, 0755);

    check(in >= 0 && out >= 0, "copy open");
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        check(write(out, buf, n) == n, "copy write");
    }
    check(n == 0, "copy read");
    close(out);
    close(in);
}

/** Check every byte of a surviving descriptor against the staged file. */
static void contents(int fd, const char *what)
{
    char want[4096], got[4096];
    off_t offset = 0;
    ssize_t n;
    int in = open("/musl/bindholdmusl", O_RDONLY);

    check(in >= 0, "reference open");
    while ((n = read(in, want, sizeof(want))) > 0) {
        check(pread(fd, got, sizeof(got), offset) == n && memcmp(want, got, n) == 0, what);
        offset += n;
    }
    check(n == 0 && pread(fd, got, 1, offset) == 0, what);
    close(in);
}

/** Return free KRFFS blocks, including space reclaimed from orphans. */
static unsigned long freeblocks(void)
{
    struct statfs st;

    check(statfs(original, &st) == 0 && st.f_type == 0x4b524646, "boot root statfs");
    return st.f_bfree;
}

/** Remove the original name, directly or by replacing it with another file. */
static void remove_name(int rename_over)
{
    int fd;

    if (rename_over) {
        copy(replacement);
        check(rename(bound_replacement, bound) == -1 && errno == EROFS, "readonly bind rename");
    } else {
        check(unlink(bound) == -1 && errno == EROFS, "readonly bind unlink");
    }
    if (remove_through_bind) {
        check(mount(NULL, alias, NULL, MS_BIND | MS_REMOUNT, NULL) == 0,
              "bind removal writable remount");
    }
    if (rename_over) {
        check(rename(remove_through_bind ? bound_replacement : replacement,
                     remove_through_bind ? bound : program) == 0,
              "rename over held file");
    } else {
        check(unlink(remove_through_bind ? bound : program) == 0, "unlink held file");
    }
    copy(churn);
    fd = open(churn, O_WRONLY);
    check(fd >= 0 && write(fd, "reused", 6) == 6, "replacement allocation");
    close(fd);
}

/** Remount the bind read-only while leaving the original mount writable. */
static void readonly(void)
{
    check(mount(NULL, alias, NULL, MS_BIND | MS_REMOUNT | MS_RDONLY, NULL) == 0,
          "bind readonly remount");
}

/** A duplicate keeps the removed file until the last descriptor closes. */
static void descriptor(int rename_over)
{
    unsigned long before = freeblocks(), held;
    int fd, duplicate;

    copy(program);
    fd = open(bound, O_RDONLY);
    check(fd >= 0, "bind descriptor open");
    duplicate = dup(fd);
    check(duplicate >= 0, "duplicate holder");
    readonly();
    remove_name(rename_over);
    contents(fd, "bind descriptor contents");
    contents(duplicate, "duplicate contents");
    held = freeblocks();
    close(fd);
    check(freeblocks() == held, "duplicate retains orphan");
    close(duplicate);
    check(freeblocks() > held, "last descriptor reclaims orphan");
    unlink(program);
    unlink(churn);
    check(freeblocks() == before, "descriptor space restored");
}

/** A mapping keeps the removed file after its descriptor closes. */
static void mapping(int rename_over)
{
    unsigned long before = freeblocks(), held;
    struct stat st;
    char want[4096];
    size_t offset = 0;
    ssize_t n;
    int fd, in;

    copy(program);
    fd = open(bound, O_RDONLY);
    check(fd >= 0 && fstat(fd, &st) == 0, "mapping open");
    mapped_len = st.st_size;
    mapped = mmap(NULL, mapped_len, PROT_READ, MAP_PRIVATE, fd, 0);
    check(mapped != MAP_FAILED, "bind mapping");
    close(fd);
    readonly();
    remove_name(rename_over);
    in = open("/musl/bindholdmusl", O_RDONLY);
    check(in >= 0, "mapping reference open");
    while ((n = read(in, want, sizeof(want))) > 0) {
        check(offset + n <= mapped_len && memcmp((char *)mapped + offset, want, n) == 0,
              "bind mapping contents");
        offset += n;
    }
    check(n == 0 && offset == mapped_len, "mapping length");
    close(in);
    held = freeblocks();
    check(munmap(mapped, mapped_len) == 0, "mapping release");
    mapped = NULL;
    check(freeblocks() > held, "last mapping reclaims orphan");
    unlink(program);
    unlink(churn);
    check(freeblocks() == before, "mapping space restored");
}

/** A descriptor in flight keeps the file until receipt and last close. */
static void flight(int rename_over)
{
    unsigned long before = freeblocks(), held;
    int pair[2], fd, received;
    char byte = 'x';
    struct iovec iov = {&byte, 1};
    struct msghdr msg;
    struct cmsghdr *cmsg;
    union {
        struct cmsghdr align;
        char bytes[CMSG_SPACE(sizeof(int))];
    } control;

    copy(program);
    fd = open(bound, O_RDONLY);
    check(fd >= 0 && socketpair(AF_UNIX, SOCK_DGRAM, 0, pair) == 0, "flight open");
    memset(&msg, 0, sizeof(msg));
    memset(&control, 0, sizeof(control));
    msg.msg_iov = &iov;
    msg.msg_iovlen = 1;
    msg.msg_control = control.bytes;
    msg.msg_controllen = sizeof(control.bytes);
    cmsg = CMSG_FIRSTHDR(&msg);
    cmsg->cmsg_level = SOL_SOCKET;
    cmsg->cmsg_type = SCM_RIGHTS;
    cmsg->cmsg_len = CMSG_LEN(sizeof(int));
    memcpy(CMSG_DATA(cmsg), &fd, sizeof(fd));
    check(sendmsg(pair[0], &msg, 0) == 1, "send bind descriptor");
    close(fd);
    readonly();
    remove_name(rename_over);
    held = freeblocks();
    memset(&control, 0, sizeof(control));
    msg.msg_controllen = sizeof(control.bytes);
    check(recvmsg(pair[1], &msg, 0) == 1, "receive bind descriptor");
    cmsg = CMSG_FIRSTHDR(&msg);
    check(cmsg && cmsg->cmsg_level == SOL_SOCKET && cmsg->cmsg_type == SCM_RIGHTS &&
              cmsg->cmsg_len == CMSG_LEN(sizeof(int)),
          "received rights");
    memcpy(&received, CMSG_DATA(cmsg), sizeof(received));
    contents(received, "bind flight contents");
    close(received);
    check(freeblocks() > held, "last received descriptor reclaims orphan");
    close(pair[0]);
    close(pair[1]);
    unlink(program);
    unlink(churn);
    check(freeblocks() == before, "flight space restored");
}

/** Start an executable through the bind, optionally keeping its forked
 * image. */
static int start(int forked)
{
    int ready[2], control[2];
    char a[24], b[24];
    pid_t running, launcher;
    int status;

    check(pipe(ready) == 0 && pipe(control) == 0, "image pipes");
    snprintf(a, sizeof(a), "%d", ready[1]);
    snprintf(b, sizeof(b), "%d", control[0]);
    launcher = fork();
    check(launcher >= 0, "image fork");
    if (launcher == 0) {
        char *args[] = {bound, forked ? "fork" : "hold", a, b, NULL};

        close(ready[0]);
        close(control[1]);
        execv(bound, args);
        _exit(111);
    }
    child = launcher;
    close(ready[1]);
    close(control[0]);
    check(read(ready[0], &running, sizeof(running)) == sizeof(running), "image ready");
    close(ready[0]);
    if (forked) {
        check(waitpid(launcher, &status, 0) == launcher && status == 0, "fork parent exit");
    }
    child = running;
    return control[1];
}

/** A running image keeps the file readable through /proc until its exit. */
static void image(int rename_over, int forked)
{
    unsigned long before = freeblocks(), held;
    char procpath[64];
    int control, fd, status;

    copy(program);
    control = start(forked);
    readonly();
    remove_name(rename_over);
    snprintf(procpath, sizeof(procpath), "/proc/%ld/exe", (long)child);
    fd = open(procpath, O_RDONLY);
    check(fd >= 0, "proc image open");
    contents(fd, "bind image contents");
    close(fd);
    held = freeblocks();
    check(write(control, "x", 1) == 1, "image release");
    close(control);
    check(waitpid(child, &status, 0) == child && status == 0, "image exit");
    child = 0;
    check(freeblocks() > held, "last image reclaims orphan");
    unlink(program);
    unlink(churn);
    check(freeblocks() == before, "image space restored");
}

/** Wait on a pipe after exec, or after exec and fork, without a time delay. */
static int child_mode(char **argv)
{
    pid_t pid;
    char command;

    if (strcmp(argv[1], "fork") == 0) {
        pid = fork();
        if (pid != 0) {
            return pid < 0 ? 112 : 0;
        }
    }
    pid = getpid();
    if (write(atoi(argv[2]), &pid, sizeof(pid)) != sizeof(pid)) {
        return 113;
    }
    close(atoi(argv[2]));
    if (read(atoi(argv[3]), &command, 1) != 1) {
        return 114;
    }
    close(atoi(argv[3]));
    return 0;
}

/** Check both name-removal paths with descriptors and direct or forked
 * images. */
int main(int argc, char **argv)
{
    int row;

    if (argc == 4) {
        return child_mode(argv);
    }
    atexit(cleanup);
    signal(SIGTERM, interrupted);
    signal(SIGINT, interrupted);
    signal(SIGHUP, interrupted);
    signal(SIGPIPE, interrupted);
    snprintf(work, sizeof(work), "%s/rach2-bindhold.XXXXXX",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    check(mkdtemp(work) != NULL, "scratch");
    created = 1;
    snprintf(original, sizeof(original), "%s/original", work);
    snprintf(alias, sizeof(alias), "%s/alias", work);
    snprintf(program, sizeof(program), "%s/program", original);
    snprintf(replacement, sizeof(replacement), "%s/replacement", original);
    snprintf(churn, sizeof(churn), "%s/churn", original);
    snprintf(bound, sizeof(bound), "%s/program", alias);
    snprintf(bound_replacement, sizeof(bound_replacement), "%s/replacement", alias);
    check(mkdir(original, 0755) == 0 && mkdir(alias, 0755) == 0, "directories");
    check(mount(original, alias, NULL, MS_BIND, NULL) == 0, "bind mount");
    mounted = 1;
    for (row = 0; row < 20; row++) {
        int kind = row % 10;

        remove_through_bind = row >= 10;
        check(mount(NULL, alias, NULL, MS_BIND | MS_REMOUNT, NULL) == 0, "bind writable remount");
        if (kind < 2) {
            descriptor(kind);
        } else if (kind < 4) {
            mapping(kind & 1);
        } else if (kind < 6) {
            flight(kind & 1);
        } else {
            image(kind & 1, kind >= 8);
        }
        rows_done++;
    }
    report_rows(NULL);
    puts("BINDHOLDMUSL OK");
    return 0;
}
