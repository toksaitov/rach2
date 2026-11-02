/**
 * inotifymusl: check inotify's events and errors as Linux reports them.
 *
 * A watch of a directory reports a file made, opened, written twice (one
 * IN_MODIFY, since the second merges with the first), closed, its mode
 * changed, read and closed again, moved away with a cookie that pairs its two events, linked,
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
 * The directory watched is /dev/shm/inotifymusl, or the one the argument
 * `dir=PATH` names, which must be on a tmpfs, as a native run gives it under
 * the case's own (tests/native/Readme.md).
 *
 * Prints `INOTIFYMUSL OK` and exits 0 on success, and prints
 * `INOTIFYMUSL FAIL (detail)` and exits 1 otherwise. Writes etude's report
 * records (../probe_report.h) for the case ETUDE_CASE names, an assertion for
 * each check `checks` lists; a failure leaves the checks after it not run.
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

#include "../probe_args.h"
#include "../probe_report.h"

extern char **environ;

/* On tmpfs, which has hard links, where the KRFFS root has none. */
static const char *dir_path = "/dev/shm/inotifymusl";
#define DIR_PATH dir_path

/**
 * The path of `name` in `DIR_PATH`, in one of four buffers used in turn, so
 * that the paths of the arguments of one call stay apart.
 */
static const char *at(const char *name)
{
    static char paths[4][256];
    static int next;
    char *p = paths[next++ % 4];

    snprintf(p, sizeof(paths[0]), "%s/%s", DIR_PATH, name);
    return p;
}

/* An event a read must return: its watch, its mask, and its name or "". */
struct want {
    int wd;
    unsigned int mask;
    const char *name;
};

/* The checks, one assertion each, in the order the program makes them, and
 * the text it prints for each that fails. */
enum {
    INIT1,
    REGISTER_KEPT,
    WATCH_DIR,
    FILE_TO_WATCH,
    WATCH_FILE,
    CALLS_WATCHED,
    EVENTS,
    SMALL_READ,
    SMALL_READ_EVENTS,
    EMPTY_EAGAIN,
    RM_WATCH_EINVAL,
    WATCH_ENOENT,
    ONLYDIR_ENOTDIR,
    NO_EVENTS_EINVAL,
    NOT_INSTANCE_EINVAL,
    SECOND_WATCH,
    MASK_CREATE_EEXIST,
    RM_WATCH,
    RM_WATCH_IGNORED,
    POLL_EMPTY,
    ONESHOT_WATCH,
    POLL_READABLE,
    ONESHOT_EVENTS,
    CHECKS
};

static const struct {
    const char *id;
    const char *text;
} checks[CHECKS] = {
    {"init1", "inotify_init1"},
    {"register-kept", "inotify_init changed the caller's register"},
    {"watch-dir", "inotify_add_watch of a directory"},
    {"file-to-watch", "a file to watch"},
    {"watch-file", "inotify_add_watch of a file"},
    {"calls-watched", "the calls watched"},
    {"events", "the events of a directory and a file"},
    {"small-read", "a read too small for an event was not EINVAL"},
    {"small-read-events", "the events after a small read"},
    {"empty-eagain", "an empty queue was not EAGAIN"},
    {"rm-watch-einval", "inotify_rm_watch of no watch was not EINVAL"},
    {"watch-enoent", "a watch of no file was not ENOENT"},
    {"onlydir-enotdir", "IN_ONLYDIR of a file was not ENOTDIR"},
    {"no-events-einval", "a watch of no events was not EINVAL"},
    {"not-instance-einval", "a watch on a descriptor of no instance was not EINVAL"},
    {"second-watch", "a second watch of a directory did not return the first's descriptor"},
    {"mask-create-eexist", "IN_MASK_CREATE of a watched directory was not EEXIST"},
    {"rm-watch", "inotify_rm_watch"},
    {"rm-watch-ignored", "the IN_IGNORED of inotify_rm_watch"},
    {"poll-empty", "an empty instance polled readable"},
    {"oneshot-watch", "a one-shot watch"},
    {"poll-readable", "an instance with an event did not poll readable"},
    {"oneshot-events", "a one-shot watch"},
};

/** In the process that began the report, report every check before `check`
 * as held, `check` as failed with `why`, those after it as not run, and the
 * report's end; with `check` CHECKS, every check held. */
static void report_at(int check, const char *why)
{
    int i;

    if (report_ended || report_getpid() != report_owner) {
        return;
    }
    for (i = 0; i < CHECKS; i++) {
        if (i < check) {
            report_pass(checks[i].id);
        } else if (i == check) {
            report_fail(checks[i].id, why);
        } else {
            report_notrun(checks[i].id, "an earlier check failed");
        }
    }
    report_end();
}

/** Report that the check `check` failed as `what`, with errno, and return 1,
 * the failure status. */
static int fail_as(int check, const char *what)
{
    printf("INOTIFYMUSL FAIL (%s: errno %d)\n", what, errno);
    report_at(check, what);
    return 1;
}

/** Report that the check `check` failed, with errno, and return 1. */
static int fail(int check)
{
    return fail_as(check, checks[check].text);
}

/**
 * Read the queue of `fd` and compare it with the `n` events of `w`, the
 * cookies of each IN_MOVED_FROM and the IN_MOVED_TO after it equal and not
 * zero, every other cookie zero, as the check `check`. Returns 0, or 1 after
 * naming the event that differed.
 */
static int expect(int fd, const struct want *w, int n, int check)
{
    const char *what = checks[check].text;
    char buf[4096] __attribute__((aligned(8)));
    char *p;
    int avail = -1, i = 0;
    unsigned int cookie = 0;
    ssize_t got;

    if (ioctl(fd, FIONREAD, &avail) != 0) {
        return fail_as(check, "FIONREAD");
    }
    got = read(fd, buf, sizeof buf);
    if (got < 0 || got != avail) {
        return fail(check);
    }
    for (p = buf; p < buf + got; i++) {
        struct inotify_event *ev = (struct inotify_event *)p;
        const char *name = ev->len ? ev->name : "";

        if (i == n || ev->wd != w[i].wd || ev->mask != w[i].mask || strcmp(name, w[i].name) != 0) {
            printf("INOTIFYMUSL FAIL (%s: event %d is wd %d mask %#x name \"%s\")\n", what, i,
                   ev->wd, ev->mask, name);
            report_at(check, what);
            return 1;
        }
        if (ev->mask & IN_MOVED_FROM) {
            cookie = ev->cookie;
        }
        if ((ev->mask & (IN_MOVED_FROM | IN_MOVED_TO)) ? ev->cookie == 0 || ev->cookie != cookie
                                                       : ev->cookie != 0) {
            return fail_as(check, "a cookie did not pair a move's two events");
        }
        p += sizeof *ev + ev->len;
    }
    if (i != n) {
        return fail(check);
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
        return fail(WATCH_DIR);
    }
    f = open(at("a"), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    if (f < 0 || write(f, "x", 1) != 1 || write(f, "y", 1) != 1 || close(f) != 0) {
        return fail(FILE_TO_WATCH);
    }
    wf = inotify_add_watch(fd, at("a"), IN_ALL_EVENTS);
    if (wf != 2) {
        return fail(WATCH_FILE);
    }
    f = open(at("a"), O_RDONLY);
    if (chmod(at("a"), 0600) != 0 || f < 0 || read(f, &c, 1) != 1 || close(f) != 0 ||
        rename(at("a"), at("b")) != 0 || link(at("b"), at("c")) != 0 || unlink(at("c")) != 0 ||
        unlink(at("b")) != 0 || mkdir(at("d"), 0755) != 0 || rmdir(at("d")) != 0) {
        return fail(CALLS_WATCHED);
    }
    {
        static const struct want w[] = {{1, IN_CREATE, "a"},
                                        {1, IN_OPEN, "a"},
                                        {1, IN_MODIFY, "a"},
                                        {1, IN_CLOSE_WRITE, "a"},
                                        {2, IN_OPEN, ""},
                                        {1, IN_OPEN, "a"},
                                        {2, IN_ATTRIB, ""},
                                        {1, IN_ATTRIB, "a"},
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

        return expect(fd, w, (int)(sizeof w / sizeof w[0]), EVENTS);
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

    close(open(at("e"), O_CREAT | O_WRONLY, 0644));
    if (read(fd, small, sizeof small) != -1 || errno != EINVAL) {
        return fail(SMALL_READ);
    }
    {
        static const struct want w[] = {
            {1, IN_CREATE, "e"}, {1, IN_OPEN, "e"}, {1, IN_CLOSE_WRITE, "e"}};

        if (expect(fd, w, 3, SMALL_READ_EVENTS)) {
            return 1;
        }
    }
    if (read(fd, small, sizeof small) != -1 || errno != EAGAIN) {
        return fail(EMPTY_EAGAIN);
    }
    if (inotify_rm_watch(fd, 999) != -1 || errno != EINVAL) {
        return fail(RM_WATCH_EINVAL);
    }
    if (inotify_add_watch(fd, "/nonexistent", IN_MODIFY) != -1 || errno != ENOENT) {
        return fail(WATCH_ENOENT);
    }
    if (inotify_add_watch(fd, at("e"), IN_MODIFY | IN_ONLYDIR) != -1 || errno != ENOTDIR) {
        return fail(ONLYDIR_ENOTDIR);
    }
    if (inotify_add_watch(fd, DIR_PATH, 0) != -1 || errno != EINVAL) {
        return fail(NO_EVENTS_EINVAL);
    }
    if (inotify_add_watch(0, DIR_PATH, IN_MODIFY) != -1 || errno != EINVAL) {
        return fail(NOT_INSTANCE_EINVAL);
    }
    if (inotify_add_watch(fd, DIR_PATH, IN_CREATE) != wd) {
        return fail(SECOND_WATCH);
    }
    if (inotify_add_watch(fd, DIR_PATH, IN_CREATE | IN_MASK_CREATE) != -1 || errno != EEXIST) {
        return fail(MASK_CREATE_EEXIST);
    }
    if (inotify_rm_watch(fd, wd) != 0) {
        return fail(RM_WATCH);
    }
    {
        static const struct want w[] = {{1, IN_IGNORED, ""}};

        if (expect(fd, w, 1, RM_WATCH_IGNORED)) {
            return 1;
        }
    }
    pf.fd = fd;
    pf.events = POLLIN;
    if (poll(&pf, 1, 0) != 0) {
        return fail(POLL_EMPTY);
    }
    if (inotify_add_watch(fd, DIR_PATH, IN_CREATE | IN_ONESHOT) != 3) {
        return fail(ONESHOT_WATCH);
    }
    close(open(at("f"), O_CREAT | O_WRONLY, 0644));
    close(open(at("g"), O_CREAT | O_WRONLY, 0644));
    if (poll(&pf, 1, 0) != 1 || !(pf.revents & POLLIN)) {
        return fail(POLL_READABLE);
    }
    {
        static const struct want w[] = {{3, IN_CREATE, "f"}, {3, IN_IGNORED, ""}};

        return expect(fd, w, 2, ONESHOT_EVENTS);
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
        return fail_as(REGISTER_KEPT, "inotify_init changed the caller's RDI");
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
        return fail_as(REGISTER_KEPT, "inotify_init changed the caller's EBX");
    }
    close(r);
    return 0;
}
#endif

int main(int argc, char **argv)
{
    int fd, wd;

    probe_args(argc, argv);
    report_begin(report_case(environ, "guest.default.inotifymusl"));
    dir_path = probe_arg("dir", dir_path);
    mkdir(DIR_PATH, 0755);
    fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0 || fcntl(fd, F_GETFD) != FD_CLOEXEC) {
        return fail(INIT1);
    }
    if (register_kept() || events(fd, &wd) || refusals(fd, wd)) {
        return 1;
    }
    close(fd);
    unlink(at("e"));
    unlink(at("f"));
    unlink(at("g"));
    rmdir(DIR_PATH);
    report_at(CHECKS, NULL);
    printf("INOTIFYMUSL OK\n");
    return 0;
}
