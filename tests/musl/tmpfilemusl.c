/**
 * tmpfilemusl: check O_TMPFILE and linkat's ways of naming its file.
 *
 * On each of the tmpfs of /dev/shm and an ext2 root that it finds, the
 * program makes a work directory and checks, as Linux 6.12 answers them:
 * open with O_TMPFILE makes a regular file with no link whose
 * /proc/self/fd link reads "DIR/#INO (deleted)"; linkat with AT_EMPTY_PATH
 * names it, and names it again once named, but not once its names are gone
 * again; AT_EMPTY_PATH names a file opened by its name and refuses with
 * ENOENT an unlinked file O_TMPFILE did not make; linkat of /proc/self/fd/N
 * names the file with AT_SYMLINK_FOLLOW and is EXDEV without it; a file
 * made with O_EXCL takes no name either way; and the last close of a file
 * never named frees its inode on ext2 and its pages on tmpfs.
 *
 * Then the rule on the opener's credentials, which AT_EMPTY_PATH applies to
 * a descriptor: a thread of the opener names the file; a child of fork,
 * whose credentials are new, names it with CAP_DAC_READ_SEARCH and gets
 * ENOENT without it; a process without the capability names a file it
 * opened itself, but not once setuid to its own uid has given it new
 * credentials with the same ids, and still names that file through
 * /proc/self/fd, which takes no such rule.
 *
 * Runs as root, which holds CAP_DAC_READ_SEARCH. Prints `TMPFILEMUSL OK`
 * and exits 0 on success, and prints `TMPFILEMUSL FAIL (detail)` and exits
 * 1 otherwise. Writes etude's report records (../probe_report.h) for the
 * case ETUDE_CASE names: `caps` and `found`, then the rows of each file
 * system it found, `tmpfs-` and `ext2-` before each row's name. The branch
 * of the group `tmpfs` is `run`, by the type of /dev/shm, or `absent`, and
 * that of `ext2` is `run`, by the type of /, or `absent`. A failure leaves
 * the checks after it not run, and the work directory removed.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../probe_report.h"

#define TMPFS_MAGIC 0x01021994
#define EXT2_MAGIC  0xEF53

/* _LINUX_CAPABILITY_VERSION_3, whose data is two words of each set. */
#define VERSION_3   0x20080522u
/* CAP_DAC_READ_SEARCH as a bit of the sets' first word. */
#define READ_SEARCH (1u << 2)

/* The size of the file the last-close row writes: 16 pages. */
#define FILL 65536

/* One 32-bit word of each set, as Linux's __user_cap_data_struct. */
struct cap_data {
    unsigned effective;
    unsigned permitted;
    unsigned inheritable;
};

/* The rows each file system runs, in order, and the text printed for each
 * that fails. */
enum {
    DIR_MADE,
    OPEN,
    NAMELESS,
    EMPTY_PATH,
    NAMED_AGAIN,
    RENAMED_ENOENT,
    NAMED_FILE,
    UNLINKED_ENOENT,
    PROC_FD,
    PROC_FD_EXDEV,
    EXCL,
    THREAD,
    CAP_CHILD,
    NOCAP_CHILD,
    NOCAP_OWN,
    NOCAP_MOVED,
    NOCAP_PROC_FD,
    LAST_CLOSE,
    CLEANUP,
    ROWS
};

static const struct {
    const char *stem;
    const char *text;
} rows[ROWS] = {
    {"dir", "the work directory could not be made"},
    {"open", "open O_TMPFILE of the work directory failed"},
    {"nameless", "the O_TMPFILE file has a link, another mode or no deleted /proc/self/fd name"},
    {"empty-path", "linkat AT_EMPTY_PATH did not name the O_TMPFILE file"},
    {"named-again", "linkat AT_EMPTY_PATH did not give the named O_TMPFILE file a second name"},
    {"renamed-enoent", "linkat AT_EMPTY_PATH named the O_TMPFILE file again once its names were "
                       "gone, or was not ENOENT"},
    {"named-file", "linkat AT_EMPTY_PATH did not name a file opened by its name"},
    {"unlinked-enoent", "linkat AT_EMPTY_PATH of an unlinked file O_TMPFILE did not make was not "
                        "ENOENT"},
    {"proc-fd", "linkat AT_SYMLINK_FOLLOW of /proc/self/fd/N did not name the O_TMPFILE file"},
    {"proc-fd-exdev", "linkat of /proc/self/fd/N without AT_SYMLINK_FOLLOW was not EXDEV"},
    {"excl", "an O_TMPFILE file made with O_EXCL took a name, or the refusal was not ENOENT"},
    {"thread", "a thread of the opener could not name the file with AT_EMPTY_PATH"},
    {"cap-child", "a child with CAP_DAC_READ_SEARCH could not name its parent's file with "
                  "AT_EMPTY_PATH"},
    {"nocap-child", "a child without CAP_DAC_READ_SEARCH named its parent's file with "
                    "AT_EMPTY_PATH, or the refusal was not ENOENT"},
    {"nocap-own", "a process without CAP_DAC_READ_SEARCH could not name a file it opened with "
                  "AT_EMPTY_PATH"},
    {"nocap-moved", "a process without CAP_DAC_READ_SEARCH named a file it opened before "
                    "setuid gave it new credentials, or the refusal was not ENOENT"},
    {"nocap-proc-fd", "a process without CAP_DAC_READ_SEARCH could not name its file through "
                      "/proc/self/fd"},
    {"last-close", "the last close of an O_TMPFILE file never named did not free it"},
    {"cleanup", "the work directory could not be removed"},
};

/* The file systems, their work directories and the prefix of their rows. */
enum { TMPFS, EXT2, KINDS };
static const char *const prefixes[KINDS] = {"tmpfs", "ext2"};
static const char *const work_dirs[KINDS] = {"/dev/shm/tmpfilemusl", "/tmpfilemusl"};

/* The checks of the report: `caps`, `found`, then ROWS for each file system
 * found, by identity, with the text of each. */
#define CHECKS_MAX (2 + KINDS * ROWS)
static char ids[CHECKS_MAX][40];
static const char *texts[CHECKS_MAX];
static int checks;

/* The work directory of the rows under way. */
static const char *dir;

/** In the process that began the report, report every check before `check`
 * as held, `check` as failed, those after it as not run, and the report's
 * end; with `check` `checks`, every check held. */
static void report_at(int check)
{
    int i;

    if (report_ended || report_getpid() != report_owner) {
        return;
    }
    for (i = 0; i < checks; i++) {
        if (i < check) {
            report_pass(ids[i]);
        } else if (i == check) {
            report_fail(ids[i], texts[i]);
        } else {
            report_notrun(ids[i], "an earlier check failed");
        }
    }
    report_end();
}

/** Add the check `id`, whose failure prints `text`. */
static void check_add(const char *prefix, const char *id, const char *text)
{
    if (prefix) {
        snprintf(ids[checks], sizeof(ids[0]), "%s-%s", prefix, id);
    } else {
        snprintf(ids[checks], sizeof(ids[0]), "%s", id);
    }
    texts[checks++] = text;
}

/** Whether a call returned -1 with `err`. */
static int refused(long rc, int err)
{
    return rc == -1 && errno == err;
}

/** The path `name` in the work directory, in one of four buffers used in
 * turn. */
static const char *at(const char *name)
{
    static char paths[4][320];
    static int next;
    char *p = paths[next++ & 3];

    snprintf(p, sizeof(paths[0]), "%s/%s", dir, name);
    return p;
}

/** Make a file with no name in the work directory with `extra` among its
 * flags and `text` in it. The descriptor, or -1. */
static int tmpfile_with(int extra, const char *text)
{
    int fd = open(dir, O_TMPFILE | O_RDWR | extra, 0600);
    size_t len = strlen(text);

    if (fd >= 0 && write(fd, text, len) != (ssize_t)len) {
        close(fd);
        return -1;
    }
    return fd;
}

/** Whether the file at `path` has exactly `text` in it. */
static int has_text(const char *path, const char *text)
{
    char buf[32];
    int fd = open(path, O_RDONLY);
    ssize_t n;

    if (fd < 0) {
        return 0;
    }
    n = read(fd, buf, sizeof(buf));
    close(fd);
    return n == (ssize_t)strlen(text) && memcmp(buf, text, (size_t)n) == 0;
}

/** Whether `path` names the file `fd` is open on, with `nlink` links. */
static int names_fd(const char *path, int fd, nlink_t nlink)
{
    struct stat a, b;

    return stat(path, &a) == 0 && fstat(fd, &b) == 0 && a.st_ino == b.st_ino &&
           a.st_dev == b.st_dev && b.st_nlink == nlink;
}

/** Whether the /proc/self/fd link of `fd` reads "DIR/#INO (deleted)", INO
 * the file's inode number, as Linux's d_tmpfile names such a file. */
static int nameless(int fd)
{
    char link[32], target[128], want[128];
    struct stat st;
    ssize_t n;

    if (fstat(fd, &st) != 0) {
        return 0;
    }
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    snprintf(want, sizeof(want), "%s/#%llu (deleted)", dir, (unsigned long long)st.st_ino);
    n = readlink(link, target, sizeof(target) - 1);
    if (n < 0) {
        return 0;
    }
    target[n] = '\0';
    return strcmp(target, want) == 0;
}

/** Name the file `fd` is open on as `name` through its /proc/self/fd link,
 * the link followed when `follow`. linkat's result. */
static int link_proc(int fd, const char *name, int follow)
{
    char link[32];

    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    return linkat(AT_FDCWD, link, AT_FDCWD, at(name), follow ? AT_SYMLINK_FOLLOW : 0);
}

/** Read the caller's sets into `d`. 0, or -1 with errno set. */
static int get_caps(struct cap_data *d)
{
    unsigned h[2] = {VERSION_3, 0};

    return (int)syscall(SYS_capget, h, d);
}

/** Take the capabilities of the bits `bits` out of every set of the
 * caller's first word. 0, or -1 with errno set. */
static int drop_caps(unsigned bits)
{
    unsigned h[2] = {VERSION_3, 0};
    struct cap_data d[2];

    if (get_caps(d) != 0) {
        return -1;
    }
    d[0].effective &= ~bits;
    d[0].permitted &= ~bits;
    d[0].inheritable &= ~bits;
    return (int)syscall(SYS_capset, h, d);
}

/** Remove every entry of the work directory and the directory. 1, or 0
 * when one stays. */
static int remove_dir(void)
{
    DIR *d = opendir(dir);
    struct dirent *e;
    int ok = 1;

    if (!d) {
        return errno == ENOENT;
    }
    while ((e = readdir(d)) != NULL) {
        if (strcmp(e->d_name, ".") != 0 && strcmp(e->d_name, "..") != 0 &&
            unlink(at(e->d_name)) != 0) {
            ok = 0;
        }
    }
    closedir(d);
    return rmdir(dir) == 0 && ok;
}

/** The descriptor a thread names (`thread_link`) and its result. */
static int thread_fd, thread_rc;

/** A thread's work: name `thread_fd` as "thread" with AT_EMPTY_PATH. */
static void *thread_link(void *unused)
{
    (void)unused;
    thread_rc = linkat(thread_fd, "", AT_FDCWD, at("thread"), AT_EMPTY_PATH);
    return NULL;
}

/** Run `child` in a child of fork and return its exit status, or 99 when
 * it could not be had. */
static int in_child(int (*child)(int), int fd)
{
    pid_t pid = fork();
    int status;

    if (pid == 0) {
        _exit(child(fd));
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status)) {
        return 99;
    }
    return WEXITSTATUS(status);
}

/** In a child that keeps its capabilities: name the parent's `fd` as
 * "cap" with AT_EMPTY_PATH. 0, or 1. */
static int cap_child(int fd)
{
    return linkat(fd, "", AT_FDCWD, at("cap"), AT_EMPTY_PATH) == 0 && names_fd(at("cap"), fd, 2)
               ? 0
               : 1;
}

/**
 * In a child that gives up CAP_DAC_READ_SEARCH: the parent's `fd` takes no
 * name (1 otherwise), a file the child opens takes one (2), a file opened
 * before setuid(getuid()) takes none after it (3), and takes one through
 * /proc/self/fd (4). 0 when all hold, or the first that does not.
 */
static int nocap_child(int fd)
{
    int own, moved;

    if (drop_caps(READ_SEARCH) != 0) {
        return 1;
    }
    if (!refused(linkat(fd, "", AT_FDCWD, at("nocap"), AT_EMPTY_PATH), ENOENT)) {
        return 1;
    }
    own = tmpfile_with(0, "own");
    if (own < 0 || linkat(own, "", AT_FDCWD, at("own"), AT_EMPTY_PATH) != 0 ||
        !names_fd(at("own"), own, 1)) {
        return 2;
    }
    moved = tmpfile_with(0, "moved");
    if (moved < 0 || setuid(getuid()) != 0 ||
        !refused(linkat(moved, "", AT_FDCWD, at("moved"), AT_EMPTY_PATH), ENOENT)) {
        return 3;
    }
    if (link_proc(moved, "moved", 1) != 0 || !has_text(at("moved"), "moved")) {
        return 4;
    }
    return 0;
}

/** The free count the last-close row watches on the file system of the
 * work directory: inodes on ext2, blocks on tmpfs, or -1. */
static long long free_count(int kind)
{
    struct statfs sfs;

    if (statfs(dir, &sfs) != 0) {
        return -1;
    }
    return kind == EXT2 ? (long long)sfs.f_ffree : (long long)sfs.f_bfree;
}

/**
 * Check that a file O_TMPFILE makes and never names takes an inode on ext2,
 * or 16 pages on tmpfs once written, and gives it back at its last close.
 * 1, or 0.
 */
static int last_close(int kind)
{
    static char fill[FILL];
    long long before = free_count(kind), during;
    int fd = open(dir, O_TMPFILE | O_RDWR, 0600);

    if (fd < 0) {
        return 0;
    }
    memset(fill, 'x', sizeof(fill));
    if (write(fd, fill, sizeof(fill)) != (ssize_t)sizeof(fill) || fsync(fd) != 0) {
        close(fd);
        return 0;
    }
    during = free_count(kind);
    close(fd);
    if (kind == EXT2 && during != before - 1) {
        return 0;
    }
    if (kind == TMPFS && during > before - FILL / 4096) {
        return 0;
    }
    return before >= 0 && free_count(kind) == before;
}

/* The descriptors the rows of one file system keep open, -1 for none: the
 * O_TMPFILE file the rows name, a file opened by its name, an unlinked file
 * O_TMPFILE did not make, the file named through /proc/self/fd, and the
 * file made with O_EXCL. */
static int fd_t = -1, fd_named = -1, fd_gone = -1, fd_proc = -1, fd_excl = -1;

/* The file system the rows run on, and the exit status of `nocap_child`. */
static int kind_now, nocap_rc;

/** Row `dir`: make the work directory. */
static int row_dir(void)
{
    return mkdir(dir, 0700) == 0;
}

/** Row `open`: make a file with no name. */
static int row_open(void)
{
    fd_t = tmpfile_with(0, "t");
    return fd_t >= 0;
}

/** Row `nameless`: the file has no link, mode 0600 and its deleted name. */
static int row_nameless(void)
{
    struct stat st;

    return fstat(fd_t, &st) == 0 && st.st_nlink == 0 && S_ISREG(st.st_mode) &&
           (st.st_mode & 07777) == 0600 && nameless(fd_t);
}

/** Row `empty-path`: AT_EMPTY_PATH names the file. */
static int row_empty_path(void)
{
    return linkat(fd_t, "", AT_FDCWD, at("a"), AT_EMPTY_PATH) == 0 && names_fd(at("a"), fd_t, 1) &&
           has_text(at("a"), "t");
}

/** Row `named-again`: AT_EMPTY_PATH gives the named file a second name. */
static int row_named_again(void)
{
    return linkat(fd_t, "", AT_FDCWD, at("b"), AT_EMPTY_PATH) == 0 && names_fd(at("b"), fd_t, 2);
}

/** Row `renamed-enoent`: with both names gone, neither way names it. */
static int row_renamed_enoent(void)
{
    return unlink(at("a")) == 0 && unlink(at("b")) == 0 &&
           refused(linkat(fd_t, "", AT_FDCWD, at("c"), AT_EMPTY_PATH), ENOENT) &&
           refused(link_proc(fd_t, "c", 1), ENOENT);
}

/** Row `named-file`: AT_EMPTY_PATH names a file opened by its name. */
static int row_named_file(void)
{
    fd_named = open(at("f"), O_RDWR | O_CREAT | O_EXCL, 0644);
    return fd_named >= 0 && linkat(fd_named, "", AT_FDCWD, at("f2"), AT_EMPTY_PATH) == 0 &&
           names_fd(at("f2"), fd_named, 2);
}

/** Row `unlinked-enoent`: an unlinked file O_TMPFILE did not make takes no
 * name. */
static int row_unlinked_enoent(void)
{
    fd_gone = open(at("g"), O_RDWR | O_CREAT | O_EXCL, 0644);
    return fd_gone >= 0 && unlink(at("g")) == 0 &&
           refused(linkat(fd_gone, "", AT_FDCWD, at("g2"), AT_EMPTY_PATH), ENOENT);
}

/** Row `proc-fd`: the followed /proc/self/fd link names a second file. */
static int row_proc_fd(void)
{
    fd_proc = tmpfile_with(0, "p");
    return fd_proc >= 0 && link_proc(fd_proc, "p", 1) == 0 && names_fd(at("p"), fd_proc, 1) &&
           has_text(at("p"), "p");
}

/** Row `proc-fd-exdev`: the link of /proc itself is on another mount. */
static int row_proc_fd_exdev(void)
{
    return refused(link_proc(fd_proc, "q", 0), EXDEV);
}

/** Row `excl`: a file made with O_EXCL takes no name either way. */
static int row_excl(void)
{
    fd_excl = tmpfile_with(O_EXCL, "x");
    return fd_excl >= 0 && refused(linkat(fd_excl, "", AT_FDCWD, at("x"), AT_EMPTY_PATH), ENOENT) &&
           refused(link_proc(fd_excl, "x", 1), ENOENT);
}

/** Row `thread`: a thread of the opener names a new file with no name. */
static int row_thread(void)
{
    pthread_t t;

    close(fd_t);
    fd_t = tmpfile_with(0, "c");
    thread_fd = fd_t;
    thread_rc = -1;
    return fd_t >= 0 && pthread_create(&t, NULL, thread_link, NULL) == 0 &&
           pthread_join(t, NULL) == 0 && thread_rc == 0 && names_fd(at("thread"), fd_t, 1);
}

/** Row `cap-child`: a child with CAP_DAC_READ_SEARCH names it. */
static int row_cap_child(void)
{
    return in_child(cap_child, fd_t) == 0;
}

/** Row `nocap-child`: run `nocap_child`, which must pass its first step. */
static int row_nocap_child(void)
{
    nocap_rc = in_child(nocap_child, fd_t);
    return nocap_rc != 1 && nocap_rc != 99;
}

/** Row `nocap-own`: `nocap_child` passed its second step. */
static int row_nocap_own(void)
{
    return nocap_rc != 2;
}

/** Row `nocap-moved`: `nocap_child` passed its third step. */
static int row_nocap_moved(void)
{
    return nocap_rc != 3;
}

/** Row `nocap-proc-fd`: `nocap_child` passed its fourth step. */
static int row_nocap_proc_fd(void)
{
    return nocap_rc != 4;
}

/** Row `last-close`: `last_close` on the file system under way. */
static int row_last_close(void)
{
    return last_close(kind_now);
}

/* The rows before `cleanup`, in the order of `rows`. */
static int (*const row_runs[CLEANUP])(void) = {
    row_dir,           row_open,           row_nameless,   row_empty_path,
    row_named_again,   row_renamed_enoent, row_named_file, row_unlinked_enoent,
    row_proc_fd,       row_proc_fd_exdev,  row_excl,       row_thread,
    row_cap_child,     row_nocap_child,    row_nocap_own,  row_nocap_moved,
    row_nocap_proc_fd, row_last_close,
};

/** Close `*fd` when it is open and mark it closed. */
static void close_fd(int *fd)
{
    if (*fd >= 0) {
        close(*fd);
    }
    *fd = -1;
}

/**
 * Run the rows on the file system `kind`, whose first check is `base`, up
 * to the first that fails, and then close their descriptors and remove the
 * work directory, row `cleanup`. Returns the check that failed, or -1 when
 * every row held.
 */
static int fs_rows(int kind, int base)
{
    int i, failed = -1;

    kind_now = kind;
    dir = work_dirs[kind];
    (void)remove_dir();
    for (i = 0; i < CLEANUP && failed < 0; i++) {
        if (!row_runs[i]()) {
            failed = i;
        }
    }
    close_fd(&fd_t);
    close_fd(&fd_named);
    close_fd(&fd_gone);
    close_fd(&fd_proc);
    close_fd(&fd_excl);
    if (!remove_dir() && failed < 0) {
        failed = CLEANUP;
    }
    return failed < 0 ? -1 : base + failed;
}

int main(void)
{
    struct cap_data d[2];
    struct statfs sfs;
    int run[KINDS], base[KINDS];
    int kind, failed = -1;

    run[TMPFS] = statfs("/dev/shm", &sfs) == 0 && sfs.f_type == TMPFS_MAGIC;
    run[EXT2] = statfs("/", &sfs) == 0 && sfs.f_type == EXT2_MAGIC;
    umask(022);
    report_begin(report_case(environ, "guest.default.tmpfilemusl"));
    report_branch("tmpfs", run[TMPFS] ? "run" : "absent");
    report_branch("ext2", run[EXT2] ? "run" : "absent");
    check_add(NULL, "caps", "the program does not hold CAP_DAC_READ_SEARCH");
    check_add(NULL, "found", "neither /dev/shm is tmpfs nor / ext2");
    for (kind = 0; kind < KINDS; kind++) {
        int i;

        base[kind] = checks;
        for (i = 0; run[kind] && i < ROWS; i++) {
            check_add(prefixes[kind], rows[i].stem, rows[i].text);
        }
    }
    if (get_caps(d) != 0 || !(d[0].effective & READ_SEARCH)) {
        failed = 0;
    } else if (!run[TMPFS] && !run[EXT2]) {
        failed = 1;
    }
    for (kind = 0; failed < 0 && kind < KINDS; kind++) {
        if (run[kind]) {
            failed = fs_rows(kind, base[kind]);
        }
    }
    if (failed >= 0) {
        printf("TMPFILEMUSL FAIL (%s: %s)\n", ids[failed], texts[failed]);
        report_at(failed);
        return 1;
    }
    report_at(checks);
    printf("TMPFILEMUSL OK\n");
    return 0;
}
