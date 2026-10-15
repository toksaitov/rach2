/**
 * inotifymusl: check inotify's events and errors as Linux reports them.
 *
 * A watch of a directory reports a file made, opened, written twice (one
 * IN_MODIFY, since the second merges with the first), closed, read and
 * closed again, moved away with a cookie that pairs its two events, linked,
 * unlinked, and a directory made and removed with IN_ISDIR. A watch of the
 * file itself reports its attribute change, its opens and closes, the move of
 * it, the changes of its links, and its removal, after which the watch is
 * gone with IN_IGNORED. A read too small for the next event is EINVAL, an
 * empty queue of a nonblocking instance is EAGAIN, FIONREAD gives the bytes a
 * read returns, and poll shows when an event is queued. A second watch of the
 * same file changes the first and returns its descriptor, with IN_MASK_CREATE
 * it fails with EEXIST, a one-shot watch is removed after its first event,
 * and inotify_rm_watch queues IN_IGNORED. inotify_init made with inline
 * assembly leaves the caller's EBX, which i386 code keeps its global offset
 * table in, as it was, and on x86-64 its RDI, where inotify_init1 takes its
 * flags.
 *
 * Prints `INOTIFYMUSL OK` and exits 0 on success, and prints
 * `INOTIFYMUSL FAIL (detail)` and exits 1 otherwise.
 */

#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

/* On tmpfs, which has hard links, where the KRFFS root has none. */
#define DIR_PATH "/dev/shm/inotifymusl"

/* An event a read must return: its watch, its mask, and its name or "". */
struct want {
    int wd;
    unsigned int mask;
    const char *name;
};

/** Report one failed check with errno and return 1, the failure status. */
static int fail(const char *what)
{
    printf("INOTIFYMUSL FAIL (%s: errno %d)\n", what, errno);
    return 1;
}

/**
 * Read the queue of `fd` and compare it with the `n` events of `w`, the
 * cookies of each IN_MOVED_FROM and the IN_MOVED_TO after it equal and not
 * zero, every other cookie zero. Returns 0, or 1 after naming the event
 * that differed.
 */
static int expect(int fd, const struct want *w, int n, const char *what)
{
    char buf[4096] __attribute__((aligned(8)));
    char *p;
    int avail = -1, i = 0;
    unsigned int cookie = 0;
    ssize_t got;

    if (ioctl(fd, FIONREAD, &avail) != 0) {
        return fail("FIONREAD");
    }
    got = read(fd, buf, sizeof buf);
    if (got < 0 || got != avail) {
        return fail(what);
    }
    for (p = buf; p < buf + got; i++) {
        struct inotify_event *ev = (struct inotify_event *)p;
        const char *name = ev->len ? ev->name : "";

        if (i == n || ev->wd != w[i].wd || ev->mask != w[i].mask || strcmp(name, w[i].name) != 0) {
            printf("INOTIFYMUSL FAIL (%s: event %d is wd %d mask %#x name \"%s\")\n", what, i,
                   ev->wd, ev->mask, name);
            return 1;
        }
        if (ev->mask & IN_MOVED_FROM) {
            cookie = ev->cookie;
        }
        if ((ev->mask & (IN_MOVED_FROM | IN_MOVED_TO)) ? ev->cookie == 0 || ev->cookie != cookie
                                                       : ev->cookie != 0) {
            return fail("a cookie did not pair a move's two events");
        }
        p += sizeof *ev + ev->len;
    }
    if (i != n) {
        return fail(what);
    }
    return 0;
}

/**
 * Watch `DIR_PATH` and a file in it on the instance `fd`, make the calls
 * watched, and compare the events queued. The directory's watch descriptor,
 * which must be 1, is left in `*wd`. Returns 0, or 1 once the failure is
 * reported.
 */
static int events(int fd, int *wd)
{
    int f, wf;
    char c;

    *wd = inotify_add_watch(fd, DIR_PATH, IN_ALL_EVENTS);
    if (*wd != 1) {
        return fail("inotify_add_watch of a directory");
    }
    f = open(DIR_PATH "/a", O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (f < 0 || write(f, "x", 1) != 1 || write(f, "y", 1) != 1 || close(f) != 0) {
        return fail("a file to watch");
    }
    wf = inotify_add_watch(fd, DIR_PATH "/a", IN_ALL_EVENTS);
    if (wf != 2) {
        return fail("inotify_add_watch of a file");
    }
    f = open(DIR_PATH "/a", O_RDONLY);
    if (chmod(DIR_PATH "/a", 0600) != 0 || f < 0 || read(f, &c, 1) != 1 || close(f) != 0 ||
        rename(DIR_PATH "/a", DIR_PATH "/b") != 0 || link(DIR_PATH "/b", DIR_PATH "/c") != 0 ||
        unlink(DIR_PATH "/c") != 0 || unlink(DIR_PATH "/b") != 0 ||
        mkdir(DIR_PATH "/d", 0755) != 0 || rmdir(DIR_PATH "/d") != 0) {
        return fail("the calls watched");
    }
    {
        static const struct want w[] = {{1, IN_CREATE, "a"},
                                        {1, IN_OPEN, "a"},
                                        {1, IN_MODIFY, "a"},
                                        {1, IN_CLOSE_WRITE, "a"},
                                        {2, IN_OPEN, ""},
                                        {1, IN_OPEN, "a"},
                                        {2, IN_ATTRIB, ""},
                                        {2, IN_ACCESS, ""},
                                        {1, IN_ACCESS, "a"},
                                        {2, IN_CLOSE_NOWRITE, ""},
                                        {1, IN_CLOSE_NOWRITE, "a"},
                                        {1, IN_MOVED_FROM, "a"},
                                        {1, IN_MOVED_TO, "b"},
                                        {2, IN_MOVE_SELF, ""},
                                        {2, IN_ATTRIB, ""},
                                        {1, IN_CREATE, "c"},
                                        {2, IN_ATTRIB, ""},
                                        {1, IN_DELETE, "c"},
                                        {2, IN_ATTRIB, ""},
                                        {1, IN_DELETE, "b"},
                                        {2, IN_DELETE_SELF, ""},
                                        {2, IN_IGNORED, ""},
                                        {1, IN_CREATE | IN_ISDIR, "d"},
                                        {1, IN_DELETE | IN_ISDIR, "d"}};

        return expect(fd, w, (int)(sizeof w / sizeof w[0]), "the events of a directory and a file");
    }
}

/**
 * Check the errors of read, inotify_add_watch and inotify_rm_watch on the
 * instance `fd`, a second watch of the directory watched as `wd`, poll, and a
 * one-shot watch. Returns 0, or 1 once the failure is reported.
 */
static int refusals(int fd, int wd)
{
    char small[8];
    struct pollfd pf;

    close(open(DIR_PATH "/e", O_CREAT | O_WRONLY, 0644));
    if (read(fd, small, sizeof small) != -1 || errno != EINVAL) {
        return fail("a read too small for an event was not EINVAL");
    }
    {
        static const struct want w[] = {
            {1, IN_CREATE, "e"}, {1, IN_OPEN, "e"}, {1, IN_CLOSE_WRITE, "e"}};

        if (expect(fd, w, 3, "the events after a small read")) {
            return 1;
        }
    }
    if (read(fd, small, sizeof small) != -1 || errno != EAGAIN) {
        return fail("an empty queue was not EAGAIN");
    }
    if (inotify_rm_watch(fd, 999) != -1 || errno != EINVAL) {
        return fail("inotify_rm_watch of no watch was not EINVAL");
    }
    if (inotify_add_watch(fd, "/nonexistent", IN_MODIFY) != -1 || errno != ENOENT) {
        return fail("a watch of no file was not ENOENT");
    }
    if (inotify_add_watch(fd, DIR_PATH "/e", IN_MODIFY | IN_ONLYDIR) != -1 || errno != ENOTDIR) {
        return fail("IN_ONLYDIR of a file was not ENOTDIR");
    }
    if (inotify_add_watch(fd, DIR_PATH, 0) != -1 || errno != EINVAL) {
        return fail("a watch of no events was not EINVAL");
    }
    if (inotify_add_watch(0, DIR_PATH, IN_MODIFY) != -1 || errno != EINVAL) {
        return fail("a watch on a descriptor of no instance was not EINVAL");
    }
    if (inotify_add_watch(fd, DIR_PATH, IN_CREATE) != wd) {
        return fail("a second watch of a directory did not return the first's descriptor");
    }
    if (inotify_add_watch(fd, DIR_PATH, IN_CREATE | IN_MASK_CREATE) != -1 || errno != EEXIST) {
        return fail("IN_MASK_CREATE of a watched directory was not EEXIST");
    }
    if (inotify_rm_watch(fd, wd) != 0) {
        return fail("inotify_rm_watch");
    }
    {
        static const struct want w[] = {{1, IN_IGNORED, ""}};

        if (expect(fd, w, 1, "the IN_IGNORED of inotify_rm_watch")) {
            return 1;
        }
    }
    pf.fd = fd;
    pf.events = POLLIN;
    if (poll(&pf, 1, 0) != 0) {
        return fail("an empty instance polled readable");
    }
    if (inotify_add_watch(fd, DIR_PATH, IN_CREATE | IN_ONESHOT) != 3) {
        return fail("a one-shot watch");
    }
    close(open(DIR_PATH "/f", O_CREAT | O_WRONLY, 0644));
    close(open(DIR_PATH "/g", O_CREAT | O_WRONLY, 0644));
    if (poll(&pf, 1, 0) != 1 || !(pf.revents & POLLIN)) {
        return fail("an instance with an event did not poll readable");
    }
    {
        static const struct want w[] = {{3, IN_CREATE, "f"}, {3, IN_IGNORED, ""}};

        return expect(fd, w, 2, "a one-shot watch");
    }
}

#if defined __x86_64__
/**
 * Check that inotify_init (253), which the kernel serves as inotify_init1
 * without flags, leaves the caller's RDI, where inotify_init1 takes them, as
 * it was: `syscall` keeps every register but RAX, RCX and R11. Returns 0, or
 * 1 once the failure is reported.
 */
static int register_kept(void)
{
    unsigned long di;
    long r;

    __asm__ __volatile__("syscall"
                         : "=a"(r), "=D"(di)
                         : "0"(253L), "1"(0xA5A5A5A5A5A5A5A5UL)
                         : "rcx", "r11", "memory");
    if (r < 0 || di != 0xA5A5A5A5A5A5A5A5UL) {
        return fail("inotify_init changed the caller's RDI");
    }
    close((int)r);
    return 0;
}
#else
/**
 * Check that inotify_init (291), which the kernel serves as inotify_init1
 * without flags, leaves the caller's EBX as it was. Returns 0, or 1 once the
 * failure is reported.
 */
static int register_kept(void)
{
    unsigned int b;
    int r;

    __asm__ __volatile__("int $0x80" : "=a"(r), "=b"(b) : "0"(291), "1"(0xA5A5A5A5u) : "memory");
    if (r < 0 || b != 0xA5A5A5A5u) {
        return fail("inotify_init changed the caller's EBX");
    }
    close(r);
    return 0;
}
#endif

int main(void)
{
    int fd, wd;

    mkdir(DIR_PATH, 0755);
    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0 || fcntl(fd, F_GETFD) != FD_CLOEXEC) {
        return fail("inotify_init1");
    }
    if (register_kept() || events(fd, &wd) || refusals(fd, wd)) {
        return 1;
    }
    close(fd);
    unlink(DIR_PATH "/e");
    unlink(DIR_PATH "/f");
    unlink(DIR_PATH "/g");
    rmdir(DIR_PATH);
    printf("INOTIFYMUSL OK\n");
    return 0;
}
