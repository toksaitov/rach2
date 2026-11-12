/**
 * direntmusl: check d_type, the seeks of a listing, and paths that end as
 * directories.
 *
 * getdents64 reports a type in d_type, which GNU libc's and musl's readdir
 * pass on and programs such as find and ls act on without a stat. The probe
 * checks that type, the seeks of a directory's listing, and the answer each
 * call that takes a path gives one that names a directory by its end.
 *
 * Every entry of /dev, /dev/pts, /proc, /proc/self, /proc/self/fd, /proc/net,
 * /proc/sys and /proc/sys/fs/inotify and a directory the probe makes must
 * report the type lstat gives it, the mode's type bits shifted down, or
 * DT_UNKNOWN, and never DT_REG for anything but a regular file; /dev/null,
 * /proc/self, /proc/meminfo, /proc/cpuinfo, /proc/net and /proc/sys have
 * their own types by name. /proc/cpuinfo is a record of processor 0 with its
 * flags, /proc/self/mounts reads as /proc/mounts, /proc/loadavg has three
 * loads with two decimals, the running and all tasks, the probe among them,
 * and a last process ID no lower than its own, /proc has no process 0,
 * /proc/sys/fs/inotify/max_user_watches reads 8192,
 * /proc/sys/kernel/core_pattern the name of a core file, and a working
 * directory of /proc/sys/kernel is named so by getcwd. Each listing must end
 * without an error from readdir and check at least one entry, and lstat may
 * fail only with ENOENT, only in /proc and /dev/pts, whose entries come and
 * go. A path that names a directory by its end is followed there, by lstat
 * and by the old lstat, and a file named that way is ENOTDIR. The probe's own
 * directory and /proc/self, read to their end, list as many entries again
 * after rewinddir, and seekdir to the place telldir gave after the first
 * entry lists the second next. In a directory of ext2, whose places are
 * bytes, the place a listing gave after a, once a and b are removed and a
 * name of twelve bytes is made in their space, is taken by lseek and by
 * seekdir and lists c and then the end, as Linux's ext2 does; lseek there
 * also takes SEEK_END and a place past 4 GiB, from which nothing is listed,
 * and the volume's largest file offset, and refuses a byte past it with
 * EINVAL from each origin; an empty ext2 directory removed while open is
 * ENOENT to getdents64 past 4 GiB.
 *
 * The other calls that take a path answer one that names a directory by its end
 * as Linux does, in the probe's own directory and in one of /dev/shm: a regular
 * file so named is ENOTDIR to open, access, chmod, chown, lchown, truncate,
 * utimensat, readlink, statfs, link, unlink and rename, where renameat2 with
 * RENAME_NOREPLACE onto a name that exists is EEXIST, and a new name so named
 * is EISDIR to open with O_CREAT and ENOENT or EEXIST to link, symlink, mknod
 * and mkdir, which make nothing, and symlink, link and mknod give those answers
 * in /dev and /proc too, before their EROFS, and bind EADDRINUSE there, and a
 * name that exists, named plainly, is EEXIST to them and to mkdir, and
 * EADDRINUSE to bind, before EROFS, EPERM or EXDEV; rmdir is EINVAL for a last
 * "." and ENOTEMPTY for a last "..", and rename EBUSY for either, in /proc too,
 * where nothing is renamed, and so do unlinkat and renameat through a directory
 * descriptor. execve of a file so named is ENOTDIR and of a directory EACCES,
 * and so is a script whose interpreter is named with a trailing slash ENOTDIR,
 * and an ELF image whose PT_INTERP names the file or the directory so ENOTDIR
 * or EACCES; bind to a name so named makes nothing, connect to a socket so
 * named is ENOTDIR, and so is umount2 of a file so named, and open of /fb and
 * /mouse so named or with O_DIRECTORY, which opens neither device. rmdir and
 * rename of the root are EBUSY, link, symlink and mknod with the root as the
 * new name EEXIST and bind to it EADDRINUSE; a mount point, /dev or /proc, is
 * EEXIST as a new name with a trailing slash and as a RENAME_NOREPLACE
 * rename's, and so is another user's /proc/PID/exe to symlink, though its
 * link may not be read. open with O_CREAT is EISDIR for an
 * existing directory, one of the probe's, one of /dev/shm and /proc, even to
 * read, and with O_DIRECTORY EINVAL before the path is read, unless O_PATH is
 * given too, which drops O_CREAT. A symbolic link to a directory named with a
 * trailing slash is followed by the calls that look it up, O_NOFOLLOW and
 * AT_SYMLINK_NOFOLLOW notwithstanding, lchown and utimensat changing the
 * directory and not the link, and is ENOTDIR to unlink and rename, in /dev/shm
 * and on an ext2 root.
 *
 * With a directory argument, which the machine.ext2 cases give as /tmp
 * on an ext2 root, the probe's own directory is made there and must have a
 * symbolic link reported as DT_LNK, and / and /bin of that root are checked
 * too. A relative directory is taken from the working directory, as a native
 * run gives `.`, its workspace (tests/native/Readme.md). The other user is
 * 1000, or the NEW of 1000 in `ids=OLD:NEW,...`; the directory is the first
 * argument without `=`.
 *
 * The program runs as root. It prints `DIRENTMUSL OK`, followed by
 * " (links)" when the file system has symbolic links, and exits 0 on success,
 * and prints `DIRENTMUSL FAIL (detail)` and exits 1 otherwise.
 *
 * etude's report (probe_report.h), for the case ETUDE_CASE names, has an
 * assertion for each row, a setup step joined to the row after it, in the
 * groups of `groups`, each checked in order until one fails; the groups of
 * `own_dir_rows` from `root_rows` on run whether or not one before failed,
 * and a row of a group not reached is not run. The calls of `dir_end_calls`
 * have a row each in the probe's own directory, `own-*`, and in /dev/shm,
 * `shm-*`. Three groups of branches say which rows ran: `arch`, `i686` with
 * `proc-self-dot-old` or `x86-64` without it; `dir`, `given` with
 * `root-bin-types` or `default` without it; `seek`, `bytes` with
 * `ext2-lseek-reuse`, `ext2-seekdir-reuse`, `ext2-lseek-limit` and
 * `ext2-getdents-removed` on ext2, or `places` without them; and `links`,
 * `links` with
 * `own-link-followed` and `own-link-unlink-enotdir`, or `none`, for a file
 * system with no symbolic links. A failure before the probe has tried a
 * symbolic link reports no branch of `links`.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/syscall.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../probe_args.h"
#include "../probe_report.h"

/* The renameat2 flag, which musl does not declare. */
#define RENAME_NOREPLACE 1

/* The directory the probe makes, and its entries. */
static char base[256], file[300], sub[300], link_name[300];

/* The directory of `reuse_rows`, in `base`, and the names it holds. */
static char reuse_dir[300];
static const char *const reuse_names[] = {"a", "b", "c", "abcdefghijkl"};

/* ext2's f_type, which statfs gives a directory of ext2. */
#define EXT2_MAGIC 0xEF53

/*
 * A group of rows of etude's report, checked in order until one fails: its
 * assertions in order, how many of them the run has, and how many are
 * reported. The groups of `own_dir_rows` from `root_rows` on each run
 * whether or not one before failed.
 */
struct group {
    const char *const *ids;
    int n;
    int reported;
};

/* The group whose rows the probe checks now. */
static struct group *current;

/** Make `g` the group whose rows the probe checks now. */
static void group_start(struct group *g)
{
    current = g;
}

/** In the process that began the report, report that the current group's
 * next row held. */
static void held(void)
{
    if (report_getpid() == report_owner && current->reported < current->n) {
        report_pass(current->ids[current->reported++]);
    }
}

/** In the process that began the report, report that the current group's
 * next row failed with `why`, and the rest of the group not run. */
static void stop(const char *why)
{
    struct group *g = current;

    if (report_owner == 0 || report_ended || report_getpid() != report_owner) {
        return;
    }
    if (g->reported < g->n) {
        report_fail(g->ids[g->reported++], why);
    }
    while (g->reported < g->n) {
        report_notrun(g->ids[g->reported++], "an earlier check failed");
    }
}

/* The groups of rows, in the order the probe checks them. */
static const char *const start_ids[] = {"symlink-or-eperm",   "own-dir-types",    "proc-self-dot",
#if !defined __x86_64__
                                        "proc-self-dot-old",
#endif
                                        "file-slash-enotdir", "file-dot-enotdir", "own-rewind",
                                        "own-seekdir",        "proc-self-rewind"};
static const char *const reuse_ids[] = {"ext2-lseek-reuse", "ext2-seekdir-reuse",
                                        "ext2-lseek-limit", "ext2-getdents-removed"};
static const char *const root_ids[] = {"root-ebusy", "root-eexist", "root-bind"};
static const char *const synth_ids[] = {"proc-dot-rename", "dev-noreplace", "synth-new-slash"};
static const char *const taken_ids[] = {"synth-taken"};
static const char *const unreadable_ids[] = {"unreadable-taken"};
static const char *const devices_ids[] = {"device-enotdir"};
static const char *const own_ids[] = {"own-open-enotdir",
                                      "own-access-enotdir",
                                      "own-chmod-enotdir",
                                      "own-chown-enotdir",
                                      "own-truncate-enotdir",
                                      "own-utimensat-enotdir",
                                      "own-readlink-enotdir",
                                      "own-statfs-enotdir",
                                      "own-link-from-enotdir",
                                      "own-creat-ends",
                                      "own-creat-eisdir",
                                      "own-link-to-ends",
                                      "own-symlink-to-ends",
                                      "own-link-onto-eexist",
                                      "own-mknod-ends",
                                      "own-mkdir-dot",
                                      "own-creat-directory-einval",
                                      "own-path-creat-directory",
                                      "own-unlink-ends",
                                      "own-rmdir-dot",
                                      "own-rename-dot",
                                      "own-rename-slash",
                                      "own-noreplace-eexist",
                                      "own-unlinkat-dot",
                                      "own-renameat-dot",
                                      "own-execve-ends",
                                      "own-script-interp-slash",
                                      "own-elf-interp",
                                      "own-bind-ends",
                                      "own-connect-slash",
                                      "own-umount-slash",
                                      "own-link-followed",
                                      "own-link-unlink-enotdir"};
static const char *const shm_ids[] = {"shm-open-enotdir",
                                      "shm-access-enotdir",
                                      "shm-chmod-enotdir",
                                      "shm-chown-enotdir",
                                      "shm-truncate-enotdir",
                                      "shm-utimensat-enotdir",
                                      "shm-readlink-enotdir",
                                      "shm-statfs-enotdir",
                                      "shm-link-from-enotdir",
                                      "shm-creat-ends",
                                      "shm-creat-eisdir",
                                      "shm-link-to-ends",
                                      "shm-symlink-to-ends",
                                      "shm-link-onto-eexist",
                                      "shm-mknod-ends",
                                      "shm-mkdir-dot",
                                      "shm-creat-directory-einval",
                                      "shm-path-creat-directory",
                                      "shm-unlink-ends",
                                      "shm-rmdir-dot",
                                      "shm-rename-dot",
                                      "shm-rename-slash",
                                      "shm-noreplace-eexist",
                                      "shm-unlinkat-dot",
                                      "shm-renameat-dot",
                                      "shm-execve-ends",
                                      "shm-script-interp-slash",
                                      "shm-elf-interp",
                                      "shm-bind-ends",
                                      "shm-connect-slash",
                                      "shm-umount-slash",
                                      "shm-link-followed",
                                      "shm-link-unlink-enotdir"};
static const char *const types_ids[] = {"dev-types", "proc-types"};
static const char *const given_ids[] = {"root-bin-types"};
static const char *const texts_ids[] = {"cpuinfo",     "self-mounts",     "loadavg",
                                        "no-proc-0",   "inotify-watches", "core-pattern",
                                        "proc-sys-cwd"};

#define GROUP(ids) {ids, (int)(sizeof(ids) / sizeof(ids[0])), 0}

static struct group group_start_rows = GROUP(start_ids);
static struct group group_reuse = GROUP(reuse_ids);
static struct group group_root = GROUP(root_ids);
static struct group group_synth = GROUP(synth_ids);
static struct group group_taken = GROUP(taken_ids);
static struct group group_unreadable = GROUP(unreadable_ids);
static struct group group_devices = GROUP(devices_ids);
static struct group group_own = GROUP(own_ids);
static struct group group_shm = GROUP(shm_ids);
static struct group group_types = GROUP(types_ids);
static struct group group_given = GROUP(given_ids);
static struct group group_texts = GROUP(texts_ids);

/* Every group, for the rows `finish` reports not run. */
static struct group *const groups[] = {
    &group_start_rows, &group_reuse, &group_root, &group_synth, &group_taken, &group_unreadable,
    &group_devices,    &group_own,   &group_shm,  &group_types, &group_given, &group_texts};

/* The rows of `link_ends`, the last of `dir_end_calls`, which the probe's own
 * directory has only where the file system has symbolic links. */
#define LINK_ROWS 2

/** Report every row not yet reported not run, and end the report. */
static void finish(void)
{
    unsigned i;

    for (i = 0; i < sizeof(groups) / sizeof(groups[0]); i++) {
        while (groups[i]->reported < groups[i]->n) {
            report_notrun(groups[i]->ids[groups[i]->reported++], "an earlier check failed");
        }
    }
    report_end();
}

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("DIRENTMUSL FAIL (%s)\n", what);
    stop(what);

    return 1;
}

/**
 * Whether every entry of `dir` reports the type lstat gives it or
 * DT_UNKNOWN, readdir ends without an error, and at least one entry was
 * checked. With `vanish`, an entry lstat no longer finds, a process that
 * exited or a terminal that closed, is passed over; any other lstat error
 * fails. Prints the first entry or error that disagrees.
 */
static int types_agree(const char *dir, int vanish)
{
    char path[512];
    struct dirent *de;
    struct stat st;
    DIR *d = opendir(dir);
    int checked = 0;
    int ok = d != NULL;

    while (ok) {
        errno = 0;
        de = readdir(d);
        if (de == NULL) {
            if (errno != 0) {
                printf("direntmusl: %s: readdir: errno %d\n", dir, errno);
                ok = 0;
            }
            break;
        }
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (lstat(path, &st) != 0) {
            if (!vanish || errno != ENOENT) {
                printf("direntmusl: %s: lstat: errno %d\n", path, errno);
                ok = 0;
            }
            continue;
        }
        checked++;
        if (de->d_type != DT_UNKNOWN && de->d_type != IFTODT(st.st_mode)) {
            printf("direntmusl: %s: d_type %d, lstat type %d\n", path, de->d_type,
                   (int)IFTODT(st.st_mode));
            ok = 0;
        }
    }
    if (d) {
        closedir(d);
    }
    if (ok && checked == 0) {
        printf("direntmusl: %s: no entry checked\n", dir);
        ok = 0;
    }
    return ok;
}

/** The d_type `dir` reports for `name`, or -1 when it has no such entry. */
static int type_of(const char *dir, const char *name)
{
    struct dirent *de;
    DIR *d = opendir(dir);
    int type = -1;

    while (d && (de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, name) == 0) {
            type = de->d_type;
            break;
        }
    }
    if (d) {
        closedir(d);
    }
    return type;
}

/**
 * Paths that name a directory by their end: lstat of /proc/self/., and the
 * old lstat, call 107, of it, follow the link to the directory, and the
 * regular file `file` named with a trailing slash or a last "." is ENOTDIR
 * to stat and lstat. 0, or 1 once the failure is reported. x86-64 has no old
 * lstat, and its row is i686's alone.
 */
static int dir_ends(const char *file)
{
#if !defined __x86_64__
    unsigned char old[64];
#endif
    char path[520];
    struct stat st;

    if (lstat("/proc/self/.", &st) != 0 || !S_ISDIR(st.st_mode)) {
        return fail("lstat of /proc/self/. did not reach the directory");
    }
    held();
#if !defined __x86_64__
    memset(old, 0, sizeof(old));
    if (syscall(SYS_lstat, "/proc/self/.", old) != 0 ||
        !S_ISDIR((mode_t)(old[8] | (old[9] << 8)))) {
        return fail("the old lstat of /proc/self/. did not reach the directory");
    }
    held();
#endif
    snprintf(path, sizeof(path), "%s/", file);
    if (stat(path, &st) != -1 || errno != ENOTDIR || lstat(path, &st) != -1 || errno != ENOTDIR) {
        return fail("a file named with a trailing slash was not ENOTDIR");
    }
    held();
    snprintf(path, sizeof(path), "%s/.", file);
    if (lstat(path, &st) != -1 || errno != ENOTDIR) {
        return fail("a file named with a last . was not ENOTDIR");
    }
    held();
    return 0;
}

/** The entries `d` lists from where it is to its end. */
static int count_rest(DIR *d)
{
    int n = 0;

    while (readdir(d) != NULL) {
        n++;
    }
    return n;
}

/**
 * Whether `dir`, read to its end, lists as many entries again after
 * rewinddir, and whether seekdir to the place telldir gave after its first
 * entry then lists its second entry: 0, 1 when the rewind differs or the
 * directory lists fewer than two entries, or 2 when the seek differs. Prints
 * what differs. The entry the seek lists is compared before closedir, which
 * frees the buffer it is in.
 */
static int rewinds(const char *dir)
{
    char second[256];
    struct dirent *de;
    DIR *d = opendir(dir);
    int count = 0;
    int again = -1;
    int listed = 0;

    if (d && readdir(d)) {
        long place = telldir(d);

        de = readdir(d);
        if (de) {
            snprintf(second, sizeof(second), "%s", de->d_name);
            count = 2 + count_rest(d);
            rewinddir(d);
            again = count_rest(d);
            seekdir(d, place);
            de = readdir(d);
            listed = de != NULL && strcmp(de->d_name, second) == 0;
        }
    }
    if (d) {
        closedir(d);
    }
    if (count < 2 || again != count) {
        printf("direntmusl: %s: %d entries, %d after rewinddir\n", dir, count, again);
        return 1;
    }
    if (!listed) {
        printf("direntmusl: %s: seekdir did not list %s again\n", dir, second);
        return 2;
    }
    return 0;
}

/** Remove `reuse_dir` and what `reuse_make` and `reuse_change` made in it. */
static void reuse_tidy(void)
{
    char path[400];
    unsigned i;

    for (i = 0; i < sizeof(reuse_names) / sizeof(reuse_names[0]); i++) {
        snprintf(path, sizeof(path), "%s/%s", reuse_dir, reuse_names[i]);
        unlink(path);
    }
    rmdir(reuse_dir);
}

/** Make the regular file `name` in `reuse_dir`. 0 or -1. */
static int reuse_file(const char *name)
{
    char path[400];
    int fd;

    snprintf(path, sizeof(path), "%s/%s", reuse_dir, name);
    fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        return -1;
    }
    close(fd);
    return 0;
}

/** Make `reuse_dir` anew with a, b and c, which ext2 lists in that order
 * after . and .., each in a record of 12 bytes. 0 or -1. */
static int reuse_make(void)
{
    reuse_tidy();
    if (mkdir(reuse_dir, 0755) != 0 || reuse_file("a") != 0 || reuse_file("b") != 0 ||
        reuse_file("c") != 0) {
        return -1;
    }
    return 0;
}

/**
 * Remove a and b and make a name of twelve bytes, which ext2 puts in the 24
 * bytes they held, so that the place a listing gave after a, the start of
 * b's record, now lies inside the new name's record. 0 or -1.
 */
static int reuse_change(void)
{
    char path[400];

    snprintf(path, sizeof(path), "%s/a", reuse_dir);
    if (unlink(path) != 0) {
        return -1;
    }
    snprintf(path, sizeof(path), "%s/b", reuse_dir);
    if (unlink(path) != 0) {
        return -1;
    }
    return reuse_file("abcdefghijkl");
}

/**
 * Read one getdents64 batch of `fd` into `buf`, and the names it holds into
 * `names`, separated by spaces. Returns the batch's length, or -1.
 */
static long dents_names(int fd, char *buf, size_t cap, char *names, size_t ncap)
{
    long n = syscall(SYS_getdents64, fd, buf, cap);
    long at;

    names[0] = '\0';
    for (at = 0; at < n; at += ((struct dirent *)(buf + at))->d_reclen) {
        snprintf(names + strlen(names), ncap - strlen(names), "%s%s", names[0] ? " " : "",
                 ((struct dirent *)(buf + at))->d_name);
    }
    return n;
}

/**
 * Whether lseek(2) of an ext2 directory takes the d_off that getdents64
 * gave after a, once a and b are gone and a new name covers that byte, and
 * the next getdents64 lists c alone, as Linux's ext2 does; whether
 * SEEK_END lands at the directory's size and a place past 4 GiB is taken,
 * each listing nothing. 0, or 1 once the failure is reported.
 */
static int lseek_reuse(void)
{
    char buf[2048], names[512];
    struct stat st;
    long long place = -1;
    long n, at;
    int fd;

    if (reuse_make() != 0 || (fd = open(reuse_dir, O_RDONLY | O_DIRECTORY)) < 0) {
        return fail("make the directory of the reused place");
    }
    n = dents_names(fd, buf, sizeof(buf), names, sizeof(names));
    for (at = 0; at < n; at += ((struct dirent *)(buf + at))->d_reclen) {
        if (strcmp(((struct dirent *)(buf + at))->d_name, "a") == 0) {
            place = ((struct dirent *)(buf + at))->d_off;
        }
    }
    if (place <= 0 || reuse_change() != 0) {
        close(fd);
        printf("direntmusl: %s lists %s\n", reuse_dir, names);
        return fail("remove a and b and reuse their space");
    }
    if (lseek(fd, place, SEEK_SET) != place) {
        printf("direntmusl: lseek to %lld: %s\n", place, strerror(errno));
        close(fd);
        return fail("lseek of an ext2 directory to a place a listing gave before reuse");
    }
    n = dents_names(fd, buf, sizeof(buf), names, sizeof(names));
    if (n <= 0 || strcmp(names, "c") != 0 || syscall(SYS_getdents64, fd, buf, sizeof(buf)) != 0) {
        printf("direntmusl: from %lld: %ld bytes, '%s'\n", place, n, names);
        close(fd);
        return fail("getdents64 from a reused place lists c alone");
    }
    if (fstat(fd, &st) != 0 || lseek(fd, 0, SEEK_END) != st.st_size ||
        syscall(SYS_getdents64, fd, buf, sizeof(buf)) != 0 ||
        lseek(fd, 0x100000000LL, SEEK_SET) != 0x100000000LL ||
        syscall(SYS_getdents64, fd, buf, sizeof(buf)) != 0) {
        printf("direntmusl: SEEK_END or 4 GiB: %s\n", strerror(errno));
        close(fd);
        return fail("lseek of an ext2 directory to its end and past 4 GiB");
    }
    close(fd);
    held();
    return 0;
}

/**
 * Whether seekdir of an ext2 directory to the place telldir gave after a,
 * once a and b are gone and a new name covers that byte, lists c and then
 * the end, as musl's seekdir and Linux's ext2 give. 0, or 1 once the failure
 * is reported.
 */
static int seekdir_reuse(void)
{
    struct dirent *de;
    long place = -1;
    int ok;
    DIR *d;

    if (reuse_make() != 0 || (d = opendir(reuse_dir)) == NULL) {
        return fail("make the directory of the reused place");
    }
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, "a") == 0) {
            place = telldir(d);
        }
    }
    if (place <= 0 || reuse_change() != 0) {
        closedir(d);
        return fail("remove a and b and reuse their space");
    }
    seekdir(d, place);
    de = readdir(d);
    ok = de != NULL && strcmp(de->d_name, "c") == 0;
    if (!ok) {
        printf("direntmusl: seekdir to %ld lists %s\n", place, de ? de->d_name : strerror(errno));
    }
    ok = ok && readdir(d) == NULL;
    closedir(d);
    if (!ok) {
        return fail("seekdir to a place telldir gave before reuse lists c and the end");
    }
    held();
    return 0;
}

/**
 * Whether lseek(2) of an ext2 directory takes the volume's largest file
 * offset, Linux's `s_maxbytes` (`ext2_max_size`, fs/ext2/super.c:730) for
 * the block size statfs gives, by SEEK_SET and SEEK_END, and refuses a
 * byte past it with EINVAL by SEEK_SET, SEEK_CUR and SEEK_END, leaving the
 * offset, as Linux's `generic_file_llseek` does. musl's lseek is _llseek on
 * i386, native or compat by the kernel, and lseek on x86-64. 0, or 1 once
 * the failure is reported.
 */
static int lseek_limit(void)
{
    static const struct {
        long bsize;
        long long most;
    } limits[] = {{1024, 17247252480LL}, {2048, 275415851008LL}, {4096, 2196873666560LL}};
    struct statfs sf;
    struct stat st;
    long long most = -1;
    unsigned i;
    int fd, ok;

    if (reuse_make() != 0 || (fd = open(reuse_dir, O_RDONLY | O_DIRECTORY)) < 0) {
        return fail("make the directory of the seek limit");
    }
    if (fstatfs(fd, &sf) != 0 || fstat(fd, &st) != 0) {
        close(fd);
        return fail("fstatfs and fstat of an ext2 directory");
    }
    for (i = 0; i < sizeof(limits) / sizeof(limits[0]); i++) {
        if ((long)sf.f_bsize == limits[i].bsize) {
            most = limits[i].most;
        }
    }
    if (most < 0) {
        close(fd);
        printf("direntmusl: ext2 block size %ld\n", (long)sf.f_bsize);
        return fail("statfs gives an ext2 block size of 1, 2 or 4 KiB");
    }
    ok = lseek(fd, most, SEEK_SET) == most;
    ok = ok && lseek(fd, 1, SEEK_CUR) == -1 && errno == EINVAL && lseek(fd, 0, SEEK_CUR) == most;
    ok = ok && lseek(fd, most + 1, SEEK_SET) == -1 && errno == EINVAL;
    ok = ok && lseek(fd, most - st.st_size + 1, SEEK_END) == -1 && errno == EINVAL;
    ok = ok && lseek(fd, 0, SEEK_CUR) == most && lseek(fd, 0, SEEK_SET) == 0;
    ok = ok && lseek(fd, most - st.st_size, SEEK_END) == most;
    if (!ok) {
        printf("direntmusl: limit %lld, size %lld: %s\n", most, (long long)st.st_size,
               strerror(errno));
        close(fd);
        return fail("lseek of an ext2 directory to its volume's largest file offset and past it");
    }
    close(fd);
    held();
    return 0;
}

/**
 * Whether getdents64 of an empty ext2 directory at a place past 4 GiB lists
 * nothing, and once the directory is removed fails with ENOENT, as Linux's
 * `iterate_dir` checks a removed directory before the listing's end
 * (fs/readdir.c:105). 0, or 1 once the failure is reported.
 */
static int getdents_removed(void)
{
    char buf[256];
    long n;
    int fd;

    reuse_tidy();
    if (mkdir(reuse_dir, 0755) != 0 || (fd = open(reuse_dir, O_RDONLY | O_DIRECTORY)) < 0) {
        return fail("make the empty directory of the removed listing");
    }
    if (lseek(fd, 0x100000000LL, SEEK_SET) != 0x100000000LL ||
        syscall(SYS_getdents64, fd, buf, sizeof(buf)) != 0) {
        printf("direntmusl: 4 GiB: %s\n", strerror(errno));
        close(fd);
        return fail("getdents64 of an empty ext2 directory past 4 GiB lists nothing");
    }
    if (rmdir(reuse_dir) != 0) {
        close(fd);
        return fail("remove the empty directory of the removed listing");
    }
    n = syscall(SYS_getdents64, fd, buf, sizeof(buf));
    if (n != -1 || errno != ENOENT) {
        printf("direntmusl: getdents64: %ld, %s\n", n, strerror(errno));
        close(fd);
        return fail("getdents64 past 4 GiB of a removed ext2 directory is ENOENT");
    }
    close(fd);
    held();
    return 0;
}

/**
 * The rows of a place in an ext2 directory that a removal and a new name
 * have covered (`lseek_reuse`, `seekdir_reuse`), of the volume's largest
 * file offset (`lseek_limit`) and of a removed directory read past 4 GiB
 * (`getdents_removed`), which a directory of another file system does not
 * have. 0, or 1 once a failure is reported.
 */
static int reuse_rows(void)
{
    int rc = 0;

    if (group_reuse.n > 0) {
        rc = lseek_reuse();
        rc = rc ? rc : seekdir_reuse();
        rc = rc ? rc : lseek_limit();
        rc = rc ? rc : getdents_removed();
    }
    reuse_tidy();
    return rc;
}

/** Whether a call returned -1 with `err`. */
static int refused(long rc, int err)
{
    return rc == -1 && errno == err;
}

/**
 * Whether an open returned -1 with `err`, given what it returned, `fd`. A
 * descriptor an open returns in error is closed at once, so it keeps no
 * directory the probe removes busy and no device open.
 */
static int fd_refused(int fd, int err)
{
    if (fd >= 0) {
        close(fd);
        return 0;
    }
    return errno == err;
}

/* The paths the rows of `dir_end_calls` name in one directory: the regular
 * file f and the directory s, which the directory has, the empty directory
 * e, which the rows make, the name n, which nothing has, the socket file k,
 * the program x, a script and then an ELF image, and the symbolic link ld to
 * s, which the rows make and remove, each also with a slash after it, "s",
 * or a last "." or "..". */
struct ends {
    char f[300], fs[304], fdot[304];
    char s[300], ss[304], sdot[304], sdotdot[304];
    char e[300], edot[304], edotdot[304];
    char n[300], ns[304], ndot[304];
    char k[300], ks[304], x[300];
    char ld[300], lds[304];
};

/** Fill `p` with the paths of the rows in `dir`. */
static void ends_in(struct ends *p, const char *dir)
{
    snprintf(p->f, sizeof(p->f), "%s/f", dir);
    snprintf(p->fs, sizeof(p->fs), "%s/f/", dir);
    snprintf(p->fdot, sizeof(p->fdot), "%s/f/.", dir);
    snprintf(p->s, sizeof(p->s), "%s/s", dir);
    snprintf(p->ss, sizeof(p->ss), "%s/s/", dir);
    snprintf(p->sdot, sizeof(p->sdot), "%s/s/.", dir);
    snprintf(p->sdotdot, sizeof(p->sdotdot), "%s/s/..", dir);
    snprintf(p->e, sizeof(p->e), "%s/e", dir);
    snprintf(p->edot, sizeof(p->edot), "%s/e/.", dir);
    snprintf(p->edotdot, sizeof(p->edotdot), "%s/e/..", dir);
    snprintf(p->n, sizeof(p->n), "%s/n", dir);
    snprintf(p->ns, sizeof(p->ns), "%s/n/", dir);
    snprintf(p->ndot, sizeof(p->ndot), "%s/n/.", dir);
    snprintf(p->k, sizeof(p->k), "%s/k", dir);
    snprintf(p->ks, sizeof(p->ks), "%s/k/", dir);
    snprintf(p->x, sizeof(p->x), "%s/x", dir);
    snprintf(p->ld, sizeof(p->ld), "%s/ld", dir);
    snprintf(p->lds, sizeof(p->lds), "%s/ld/", dir);
}

/**
 * The calls that look a path up, given a regular file named as a directory
 * by its end: open, with O_PATH too, access, chmod, chown, lchown, truncate,
 * utimensat, statfs and link's source are ENOTDIR, and so is readlink,
 * which is EINVAL for a directory so named. 0, or 1 once the failure is
 * reported.
 */
static int lookup_ends(const struct ends *p)
{
    struct statfs sf;
    char buf[16];

    if (!fd_refused(open(p->fs, O_RDONLY), ENOTDIR) ||
        !fd_refused(open(p->fdot, O_RDONLY), ENOTDIR) ||
        !fd_refused(open(p->fs, O_PATH), ENOTDIR)) {
        return fail("open of a file named with a trailing slash or a last . was not ENOTDIR");
    }
    held();
    if (!refused(access(p->fs, F_OK), ENOTDIR)) {
        return fail("access of a file named with a trailing slash was not ENOTDIR");
    }
    held();
    if (!refused(chmod(p->fs, 0644), ENOTDIR)) {
        return fail("chmod of a file named with a trailing slash was not ENOTDIR");
    }
    held();
    if (!refused(chown(p->fs, (uid_t)-1, (gid_t)-1), ENOTDIR) ||
        !refused(lchown(p->fs, (uid_t)-1, (gid_t)-1), ENOTDIR)) {
        return fail("chown or lchown of a file named with a trailing slash was not ENOTDIR");
    }
    held();
    if (!refused(truncate(p->fs, 0), ENOTDIR)) {
        return fail("truncate of a file named with a trailing slash was not ENOTDIR");
    }
    held();
    if (!refused(utimensat(AT_FDCWD, p->fs, NULL, 0), ENOTDIR)) {
        return fail("utimensat of a file named with a trailing slash was not ENOTDIR");
    }
    held();
    if (!refused(readlink(p->fs, buf, sizeof(buf)), ENOTDIR) ||
        !refused(readlink(p->ss, buf, sizeof(buf)), EINVAL)) {
        return fail("readlink of a name with a trailing slash was not ENOTDIR or EINVAL");
    }
    held();
    if (!refused(statfs(p->fs, &sf), ENOTDIR)) {
        return fail("statfs of a file named with a trailing slash was not ENOTDIR");
    }
    held();
    if (!refused(link(p->fs, p->n), ENOTDIR)) {
        return fail("link from a file named with a trailing slash was not ENOTDIR");
    }
    held();
    return 0;
}

/**
 * open with O_CREAT and O_DIRECTORY, which ask for a file and for a
 * directory at once: EINVAL before the path is read, for the directory s,
 * for the name n, which it does not make, and for the empty path, by open
 * and by openat; with O_PATH, which drops O_CREAT, a descriptor on s. 0, or
 * 1 once the failure is reported.
 */
static int create_dir_pair(const struct ends *p)
{
    struct stat st;
    int fd;

    if (!fd_refused(open(p->s, O_RDONLY | O_CREAT | O_DIRECTORY, 0644), EINVAL) ||
        !fd_refused(open(p->n, O_RDONLY | O_CREAT | O_DIRECTORY, 0644), EINVAL) ||
        !fd_refused(openat(AT_FDCWD, "", O_RDONLY | O_CREAT | O_DIRECTORY, 0644), EINVAL) ||
        lstat(p->n, &st) == 0) {
        return fail("an open with O_CREAT and O_DIRECTORY was not EINVAL");
    }
    held();
    fd = open(p->s, O_PATH | O_CREAT | O_DIRECTORY, 0644);
    if (fd < 0) {
        return fail("an open with O_PATH, O_CREAT and O_DIRECTORY did not name the directory");
    }
    held();
    close(fd);
    return 0;
}

/**
 * The calls that make a node, given a new name as a directory by its end:
 * open with O_CREAT is EISDIR, and with O_EXCL too EEXIST for a last "." or
 * "..", which names a directory that exists; link, symlink and mknod are
 * ENOENT for a name nothing has with a slash after it, and EEXIST for a
 * last "." after a directory, and mknod ENOTDIR for one after the file;
 * mkdir is EEXIST for a last "." after a directory, in /proc too, where
 * nothing is made, and ENOENT for one after a name nothing has; and none of
 * them makes n. symlink onto f and link onto s, names that exist, are
 * EEXIST, before the EPERM of the KRFFS root, which has neither kind of
 * link. open with O_CREAT, which asks for a file, is EISDIR for the
 * directory s and for /proc too, even to read, and with O_DIRECTORY EINVAL
 * (`create_dir_pair`). 0, or 1 once the failure is reported.
 */
static int make_ends(const struct ends *p)
{
    struct stat st;

    if (!fd_refused(open(p->ns, O_WRONLY | O_CREAT, 0644), EISDIR) ||
        !fd_refused(open(p->sdot, O_WRONLY | O_CREAT, 0644), EISDIR) ||
        !fd_refused(open(p->sdot, O_WRONLY | O_CREAT | O_EXCL, 0644), EEXIST) ||
        !fd_refused(open(p->sdotdot, O_RDONLY | O_CREAT | O_EXCL, 0644), EEXIST) ||
        lstat(p->n, &st) == 0) {
        return fail("an open with O_CREAT of a path that names a directory by its end");
    }
    held();
    if (!fd_refused(open(p->s, O_RDONLY | O_CREAT, 0644), EISDIR) ||
        !fd_refused(open("/proc", O_RDONLY | O_CREAT, 0644), EISDIR)) {
        return fail("an open with O_CREAT of an existing directory was not EISDIR");
    }
    held();
    if (!refused(link(p->f, p->ns), ENOENT) || !refused(link(p->f, p->sdot), EEXIST)) {
        return fail("link to a new name with a trailing slash or a last .");
    }
    held();
    if (!refused(symlink("f", p->ns), ENOENT) || !refused(symlink("f", p->sdot), EEXIST)) {
        return fail("symlink to a new name with a trailing slash or a last .");
    }
    held();
    if (!refused(symlink("f", p->f), EEXIST) || !refused(link(p->f, p->s), EEXIST)) {
        return fail("symlink or link onto a name that exists was not EEXIST");
    }
    held();
    if (!refused(mknod(p->ns, S_IFIFO | 0644, 0), ENOENT) ||
        !refused(mknod(p->sdot, S_IFIFO | 0644, 0), EEXIST) ||
        !refused(mknod(p->fdot, S_IFIFO | 0644, 0), ENOTDIR)) {
        return fail("mknod of a new name with a trailing slash or a last .");
    }
    held();
    if (!refused(mkdir(p->sdot, 0755), EEXIST) || !refused(mkdir("/proc/.", 0755), EEXIST) ||
        !refused(mkdir(p->ndot, 0755), ENOENT) || lstat(p->n, &st) == 0) {
        return fail("mkdir of a path whose last name is .");
    }
    held();
    return create_dir_pair(p);
}

/**
 * The calls that remove or move a name, given one as a directory by its
 * end: unlink is ENOTDIR for the file with a slash or a last "." after it
 * and EISDIR for a last "." after a directory; rmdir is EINVAL for a last
 * "." and ENOTEMPTY for a last "..";
 * rename is EBUSY for either as the last component of either name, and
 * ENOTDIR for the file with a slash after either name, unless
 * RENAME_NOREPLACE finds the new name s there, which is EEXIST first, as on
 * Linux; and none of them takes f or e away. 0, or 1 once the failure is
 * reported.
 */
static int remove_ends(const struct ends *p)
{
    struct stat st;

    if (!refused(unlink(p->fs), ENOTDIR) || !refused(unlink(p->fdot), ENOTDIR) ||
        !refused(unlink(p->sdot), EISDIR) || lstat(p->f, &st) != 0) {
        return fail("unlink of a file named with a trailing slash or of a last .");
    }
    held();
    if (!refused(rmdir(p->edot), EINVAL) || !refused(rmdir(p->edotdot), ENOTEMPTY) ||
        lstat(p->e, &st) != 0) {
        return fail("rmdir of a directory's . or ..");
    }
    held();
    if (!refused(rename(p->sdot, p->n), EBUSY) || !refused(rename(p->f, p->sdot), EBUSY) ||
        !refused(rename(p->sdotdot, p->n), EBUSY)) {
        return fail("rename of a name whose last component is . or ..");
    }
    held();
    if (!refused(rename(p->fs, p->n), ENOTDIR) || !refused(rename(p->f, p->ns), ENOTDIR) ||
        lstat(p->f, &st) != 0) {
        return fail("rename of a file named with a trailing slash");
    }
    held();
    if (!refused(syscall(SYS_renameat2, AT_FDCWD, p->fs, AT_FDCWD, p->s, RENAME_NOREPLACE),
                 EEXIST) ||
        !refused(syscall(SYS_renameat2, AT_FDCWD, p->f, AT_FDCWD, p->ss, RENAME_NOREPLACE),
                 EEXIST)) {
        return fail("a RENAME_NOREPLACE rename onto a name that exists was not EEXIST first");
    }
    held();
    return 0;
}

/**
 * unlinkat with AT_REMOVEDIR and renameat through a descriptor of `dir`,
 * given a relative path whose last component is "." or "..": EINVAL and
 * ENOTEMPTY, and EBUSY, as rmdir and rename answer, leaving e and s where
 * they are. 0, or 1 once the failure is reported.
 */
static int at_ends(const char *dir, const struct ends *p)
{
    struct stat st;
    int dfd = open(dir, O_RDONLY | O_DIRECTORY);
    int removed, renamed;

    removed = dfd >= 0 && refused(unlinkat(dfd, "e/.", AT_REMOVEDIR), EINVAL) &&
              refused(unlinkat(dfd, "e/..", AT_REMOVEDIR), ENOTEMPTY) && lstat(p->e, &st) == 0;
    renamed = dfd >= 0 && refused(renameat(dfd, "s/.", dfd, "n"), EBUSY) &&
              refused(renameat(dfd, "f", dfd, "s/.."), EBUSY) && lstat(p->s, &st) == 0;
    if (dfd >= 0) {
        close(dfd);
    }
    if (!removed) {
        return fail("unlinkat with AT_REMOVEDIR of a directory's . or .. through a descriptor");
    }
    held();
    if (!renamed) {
        return fail("renameat of a name whose last component is . or .. through a descriptor");
    }
    held();
    return 0;
}

/* A script whose interpreter is named with a trailing slash, which execve
 * fails with ENOTDIR. The row judges by the child's status (`exec_refused`),
 * since the default image's /bin/sh reads its commands from its input and not
 * from the script; a shell that read the script would print the line. */
static const char slash_script[] =
    "#!/bin/sh/\n"
    "echo 'DIRENTMUSL FAIL (a script whose interpreter is named with a trailing slash ran)'\n";

/**
 * Whether execve of `path` returned -1 with `err`. The call is made by a
 * child, so a kernel that wrongly runs the program leaves this process to
 * report the row and remove what the rows made: the child exits with 3 when
 * the call returned -1 with `err` and with 4 when it returned otherwise, and
 * any other end, a program that ran, is a failure. The child's input is
 * /dev/null, so a shell that runs ends at once and does not wait for a line.
 */
static int exec_refused(const char *path, int err)
{
    static char *const args[] = {"direntmusl", NULL};
    pid_t pid = fork();
    int status;
    int null;

    if (pid == 0) {
        null = open("/dev/null", O_RDONLY);
        if (null < 0 || dup2(null, 0) != 0) {
            _exit(5);
        }
        _exit(refused(execve(path, args, args + 1), err) ? 3 : 4);
    }
    return pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
           WEXITSTATUS(status) == 3;
}

/** bind(2) of the socket `fd` to the path `path`. */
static int bind_path(int fd, const char *path)
{
    struct sockaddr_un a;

    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    return bind(fd, (struct sockaddr *)&a, sizeof(a));
}

/** connect(2) of the socket `fd` to the path `path`. */
static int connect_path(int fd, const char *path)
{
    struct sockaddr_un a;

    memset(&a, 0, sizeof(a));
    a.sun_family = AF_UNIX;
    snprintf(a.sun_path, sizeof(a.sun_path), "%s", path);
    return connect(fd, (struct sockaddr *)&a, sizeof(a));
}

/*
 * The ELF image of a program of this architecture, which the kernel loads:
 * ELF32 i386 at 0x08048000, or ELF64 x86-64 at 0x400000, where the linker
 * places a program's text.
 */
#if defined __x86_64__
typedef Elf64_Ehdr Ehdr;
typedef Elf64_Phdr Phdr;
#define ELF_CLASS   ELFCLASS64
#define ELF_MACHINE EM_X86_64
#define LOAD_BASE   0x400000
#else
typedef Elf32_Ehdr Ehdr;
typedef Elf32_Phdr Phdr;
#define ELF_CLASS   ELFCLASS32
#define ELF_MACHINE EM_386
#define LOAD_BASE   0x08048000
#endif

/**
 * Write the ELF image `path`, mode 0755, whose PT_INTERP is `interp`: the
 * ELF header, the PT_INTERP, and one PT_LOAD at LOAD_BASE that covers the
 * whole file, headers and interpreter path included, which the kernel loads
 * before it opens the interpreter. 0, or -1 when the file could not be
 * written.
 */
static int write_elf(const char *path, const char *interp)
{
    unsigned char img[sizeof(Ehdr) + 2 * sizeof(Phdr) + 320];
    Ehdr eh;
    Phdr ph[2];
    size_t at = sizeof(eh) + sizeof(ph);
    size_t size = at + strlen(interp) + 1;
    int fd;
    int ok;

    if (size > sizeof(img)) {
        return -1;
    }
    memset(&eh, 0, sizeof(eh));
    memcpy(eh.e_ident, ELFMAG, SELFMAG);
    eh.e_ident[EI_CLASS] = ELF_CLASS;
    eh.e_ident[EI_DATA] = ELFDATA2LSB;
    eh.e_ident[EI_VERSION] = EV_CURRENT;
    eh.e_type = ET_EXEC;
    eh.e_machine = ELF_MACHINE;
    eh.e_version = EV_CURRENT;
    eh.e_entry = LOAD_BASE + at;
    eh.e_phoff = sizeof(eh);
    eh.e_ehsize = sizeof(eh);
    eh.e_phentsize = sizeof(Phdr);
    eh.e_phnum = 2;
    memset(ph, 0, sizeof(ph));
    ph[0].p_type = PT_INTERP;
    ph[0].p_offset = at;
    ph[0].p_vaddr = LOAD_BASE + at;
    ph[0].p_filesz = size - at;
    ph[0].p_memsz = size - at;
    ph[0].p_flags = PF_R;
    ph[1].p_type = PT_LOAD;
    ph[1].p_vaddr = LOAD_BASE;
    ph[1].p_filesz = size;
    ph[1].p_memsz = size;
    ph[1].p_flags = PF_R | PF_X;
    ph[1].p_align = 4096;
    memcpy(img, &eh, sizeof(eh));
    memcpy(img + sizeof(eh), ph, sizeof(ph));
    memcpy(img + at, interp, size - at);
    fd = open(path, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    ok = fd >= 0 && write(fd, img, size) == (ssize_t)size;
    if (fd >= 0) {
        close(fd);
    }
    return ok ? 0 : -1;
}

/**
 * execve of an ELF image, x, whose PT_INTERP names a directory by its end,
 * which is held to the rule for the program's own path: the file f so named
 * is ENOTDIR, and the directory s, which is no program, EACCES. 0, or 1 once
 * the failure is reported.
 */
static int interp_ends(const struct ends *p)
{
    int ok = write_elf(p->x, p->fs) == 0 && exec_refused(p->x, ENOTDIR) &&
             write_elf(p->x, p->fdot) == 0 && exec_refused(p->x, ENOTDIR) &&
             write_elf(p->x, p->ss) == 0 && exec_refused(p->x, EACCES) &&
             write_elf(p->x, p->sdotdot) == 0 && exec_refused(p->x, EACCES);

    unlink(p->x);
    if (!ok) {
        return fail("an ELF image whose PT_INTERP names a directory by its end was not refused");
    }
    held();
    return 0;
}

/**
 * execve given a path that names a directory by its end: the file so named
 * is ENOTDIR and the directory EACCES, and a script whose interpreter is
 * named with a trailing slash is ENOTDIR, and so is an ELF image whose
 * PT_INTERP is (`interp_ends`). Each exec is a child's (`exec_refused`). 0,
 * or 1 once the failure is reported.
 */
static int exec_ends(const struct ends *p)
{
    int fd;
    int ok;

    if (!exec_refused(p->fs, ENOTDIR) || !exec_refused(p->fdot, ENOTDIR) ||
        !exec_refused(p->ss, EACCES) || !exec_refused(p->sdot, EACCES)) {
        return fail("execve of a path that names a directory by its end was not ENOTDIR or EACCES");
    }
    held();
    fd = open(p->x, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    ok = fd >= 0 &&
         write(fd, slash_script, sizeof(slash_script) - 1) == (ssize_t)(sizeof(slash_script) - 1);
    if (fd >= 0) {
        close(fd);
    }
    ok = ok && exec_refused(p->x, ENOTDIR);
    unlink(p->x);
    if (!ok) {
        return fail("a script whose interpreter has a trailing slash was not ENOTDIR");
    }
    held();
    return interp_ends(p);
}

/**
 * The AF_UNIX calls and umount2 given a path that names a directory by its
 * end: bind is ENOENT after a name nothing has, EADDRINUSE after the file
 * and for a last "." after the directory, and ENOTDIR for one after the
 * file, making nothing; connect to a bound socket named with a trailing
 * slash is ENOTDIR; and umount2 of the file so named is ENOTDIR. 0, or 1
 * once the failure is reported.
 */
static int socket_ends(const struct ends *p)
{
    struct stat st;
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    int c = socket(AF_UNIX, SOCK_STREAM, 0);
    int bound, connected;

    bound = s >= 0 && refused(bind_path(s, p->ns), ENOENT) &&
            refused(bind_path(s, p->fs), EADDRINUSE) &&
            refused(bind_path(s, p->sdot), EADDRINUSE) && refused(bind_path(s, p->fdot), ENOTDIR) &&
            lstat(p->n, &st) != 0;
    connected = bound && bind_path(s, p->k) == 0 && listen(s, 1) == 0 && c >= 0 &&
                refused(connect_path(c, p->ks), ENOTDIR);
    if (s >= 0) {
        close(s);
    }
    if (c >= 0) {
        close(c);
    }
    unlink(p->k);
    if (!bound) {
        return fail("a bind to a name ending in a slash or . was not refused as on Linux");
    }
    held();
    if (!connected) {
        return fail("connect to a socket named with a trailing slash was not ENOTDIR");
    }
    held();
    if (!refused(umount2(p->fs, 0), ENOTDIR)) {
        return fail("umount2 of a file named with a trailing slash was not ENOTDIR");
    }
    held();
    return 0;
}

/**
 * A symbolic link to a directory named with a trailing slash names the
 * directory, and the calls that look the path up follow it, as Linux's do,
 * even where they would not follow a final link otherwise: open with
 * O_NOFOLLOW; lchown and utimensat with AT_SYMLINK_NOFOLLOW, which must set
 * the group and the times of the directory s and leave the link's; and
 * readlink, which is EINVAL for the directory. unlink and rename judge the
 * name itself, a link and no directory, and are ENOTDIR, leaving it. 0, or 1
 * once the failure is reported.
 */
static int link_ends(const struct ends *p)
{
    static const struct timespec when[2] = {{1000000, 0}, {2000000, 0}};
    struct stat st, lst;
    char buf[16];
    int fd = open(p->lds, O_RDONLY | O_NOFOLLOW);
    int ok = fd >= 0;

    if (fd >= 0) {
        close(fd);
    }
    ok = ok && lchown(p->lds, (uid_t)-1, 1) == 0 &&
         utimensat(AT_FDCWD, p->lds, when, AT_SYMLINK_NOFOLLOW) == 0 &&
         refused(readlink(p->lds, buf, sizeof(buf)), EINVAL) && stat(p->s, &st) == 0 &&
         lstat(p->ld, &lst) == 0 && st.st_gid == 1 && st.st_mtime == 2000000 && lst.st_gid != 1 &&
         lst.st_mtime != 2000000;
    if (!ok) {
        return fail("a symbolic link to a directory named with a trailing slash was not followed");
    }
    held();
    if (!refused(unlink(p->lds), ENOTDIR) || !refused(rename(p->lds, p->n), ENOTDIR) ||
        lstat(p->ld, &st) != 0 || !S_ISLNK(st.st_mode)) {
        return fail("unlink or rename of a link named with a trailing slash was not ENOTDIR");
    }
    held();
    return 0;
}

/**
 * The calls that take a path, other than the stat calls, given one that
 * names a directory by its end, in `dir`, which has the regular file f and
 * the directory s: `lookup_ends`, `make_ends`, `remove_ends`, `at_ends`,
 * `exec_ends` and `socket_ends`, and `link_ends` where the symbolic link ld
 * can be made, which `need_links` requires, with the empty directory e and
 * the link made for them and taken away after, as is whatever a failing row
 * made at n. 0, or 1 once the failure is reported.
 */
static int dir_end_calls(const char *dir, int need_links)
{
    struct ends p;
    int links;
    int rc;

    ends_in(&p, dir);
    if (mkdir(p.e, 0755) != 0) {
        return fail("make an empty directory");
    }
    links = symlink("s", p.ld) == 0;
    if (!links && need_links) {
        rmdir(p.e);
        return fail("make a symbolic link to a directory");
    }
    rc = lookup_ends(&p);
    if (rc == 0) {
        rc = make_ends(&p);
    }
    if (rc == 0) {
        rc = remove_ends(&p);
    }
    if (rc == 0) {
        rc = at_ends(dir, &p);
    }
    if (rc == 0) {
        rc = exec_ends(&p);
    }
    if (rc == 0) {
        rc = socket_ends(&p);
    }
    if (rc == 0 && links) {
        rc = link_ends(&p);
    }
    unlink(p.ld);
    rmdir(p.e);
    unlink(p.n);
    rmdir(p.n);
    return rc;
}

/**
 * `dir_end_calls` in a directory of /dev/shm, a tmpfs, whose calls take a
 * mounted type's paths where the probe's own directory on the KRFFS root
 * takes the root's. 0, or 1 once the failure is reported.
 */
static int shm_dir_end_calls(void)
{
    static const char dir[] = "/dev/shm/dirent.d";
    char f[64], s[64];
    int rc = 1;
    int fd;

    snprintf(f, sizeof(f), "%s/f", dir);
    snprintf(s, sizeof(s), "%s/s", dir);
    unlink(f);
    rmdir(s);
    rmdir(dir);
    if (mkdir(dir, 0755) != 0 || mkdir(s, 0755) != 0 ||
        (fd = open(f, O_WRONLY | O_CREAT, 0644)) < 0) {
        fail("make a directory in /dev/shm");
    } else {
        close(fd);
        rc = dir_end_calls(dir, 1);
    }
    unlink(f);
    rmdir(s);
    rmdir(dir);
    return rc;
}

/**
 * The root as the name a call removes, moves or makes: rmdir, and rename
 * with it as either name, are EBUSY, with both names on the root's mount;
 * link, symlink and mknod are EEXIST, as for any name that exists, and bind
 * EADDRINUSE. 0, or 1 once the failure is reported.
 */
static int root_rows(void)
{
    int s;
    int bound;

    if (!refused(rmdir("/"), EBUSY) || !refused(rename("/", sub), EBUSY) ||
        !refused(rename(file, "/"), EBUSY)) {
        return fail("rmdir or rename of the root was not EBUSY");
    }
    held();
    if (!refused(link(file, "/"), EEXIST) || !refused(symlink("f", "/"), EEXIST) ||
        !refused(mknod("/", S_IFIFO | 0644, 0), EEXIST)) {
        return fail("link, symlink or mknod with the root as the new name was not EEXIST");
    }
    held();
    s = socket(AF_UNIX, SOCK_STREAM, 0);
    bound = s >= 0 && refused(bind_path(s, "/"), EADDRINUSE);
    if (s >= 0) {
        close(s);
    }
    if (!bound) {
        return fail("a bind to the root was not EADDRINUSE");
    }
    held();
    return 0;
}

/**
 * Names in /proc and /dev, where nothing is made or renamed (EROFS), which
 * the calls answer first as Linux does, before it asks for write access:
 * "/proc/." as the old name of a rename and "/proc/.." as the new are
 * EBUSY, and "/proc/." as the new name under RENAME_NOREPLACE EEXIST, as is
 * the mount point /dev; a new name with a slash after it is ENOENT to
 * symlink and link when the namespace does not have it, and EEXIST to
 * symlink and mknod, and EADDRINUSE to bind, when it does, a mount point
 * among them. 0, or 1 once the failure is reported.
 */
static int synth_name_rows(void)
{
    int s;
    int made;

    if (!refused(rename("/proc/.", "/proc/new"), EBUSY) ||
        !refused(rename("/proc/meminfo", "/proc/.."), EBUSY) ||
        !refused(syscall(SYS_renameat2, AT_FDCWD, "/proc/meminfo", AT_FDCWD, "/proc/.",
                         RENAME_NOREPLACE),
                 EEXIST)) {
        return fail("a rename of a last . or .. in /proc was not EBUSY or EEXIST");
    }
    held();
    if (!refused(syscall(SYS_renameat2, AT_FDCWD, file, AT_FDCWD, "/dev", RENAME_NOREPLACE),
                 EEXIST) ||
        !refused(syscall(SYS_renameat2, AT_FDCWD, "/proc", AT_FDCWD, "/dev", RENAME_NOREPLACE),
                 EEXIST)) {
        return fail("a rename onto the mount point /dev under RENAME_NOREPLACE was not EEXIST");
    }
    held();
    s = socket(AF_UNIX, SOCK_STREAM, 0);
    made = s < 0 || !refused(symlink("x", "/proc/nonexistent/"), ENOENT) ||
           !refused(symlink("x", "/proc/meminfo/"), EEXIST) ||
           !refused(mknod("/dev/null/", S_IFIFO | 0644, 0), EEXIST) ||
           !refused(link(file, "/proc/nonexistent/"), ENOENT) ||
           !refused(bind_path(s, "/proc/meminfo/"), EADDRINUSE) ||
           !refused(symlink("x", "/dev/"), EEXIST) ||
           !refused(mknod("/proc/", S_IFIFO | 0644, 0), EEXIST);
    if (s >= 0) {
        close(s);
    }
    if (made) {
        return fail("a new name in /dev or /proc with a trailing slash was not ENOENT or EEXIST");
    }
    held();
    return 0;
}

/**
 * Names that exist in /proc and /dev, given without a path ending, to the
 * calls that make a node: EEXIST to symlink, mknod, mkdir and link, and
 * EADDRINUSE to bind, before the EROFS of those namespaces and the EXDEV of
 * a link from another mount, as Linux answers. 0, or 1 once the failure is
 * reported.
 */
static int synth_taken_rows(void)
{
    int s = socket(AF_UNIX, SOCK_STREAM, 0);
    int taken = s >= 0 && refused(symlink("x", "/proc/meminfo"), EEXIST) &&
                refused(mknod("/dev/null", S_IFIFO | 0644, 0), EEXIST) &&
                refused(mkdir("/proc/self", 0755), EEXIST) &&
                refused(mkdir("/dev/shm", 0755), EEXIST) &&
                refused(link(file, "/proc/meminfo"), EEXIST) &&
                refused(bind_path(s, "/proc/meminfo"), EADDRINUSE);

    if (s >= 0) {
        close(s);
    }
    if (!taken) {
        return fail("a name that exists in /dev or /proc was not EEXIST before EROFS or EXDEV");
    }
    held();
    return 0;
}

/**
 * A name that exists in /proc whose link the caller may not read: symlink
 * onto the exe link of this process, which runs as root, is EEXIST, plainly
 * and with a trailing slash, for a child that runs as user 1000. 0, or 1
 * once the failure is reported.
 */
static int unreadable_taken_rows(void)
{
    char exe[40], exe_slash[40];
    pid_t pid;
    int status;

    snprintf(exe, sizeof(exe), "/proc/%d/exe", (int)getpid());
    snprintf(exe_slash, sizeof(exe_slash), "%s/", exe);
    pid = fork();
    if (pid == 0) {
        if (setgid((gid_t)probe_id(1000)) != 0 || setuid((uid_t)probe_id(1000)) != 0) {
            _exit(2);
        }
        _exit(refused(symlink("x", exe), EEXIST) && refused(symlink("x", exe_slash), EEXIST) ? 0
                                                                                             : 1);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFEXITED(status) ||
        WEXITSTATUS(status) != 0) {
        return fail("a name in /proc whose link the caller may not read was not EEXIST");
    }
    held();
    return 0;
}

/**
 * The names /fb and /mouse, which open takes for the framebuffer and the
 * mouse before any walk, given as directories: a trailing slash, a last "."
 * or "..", and O_DIRECTORY are ENOTDIR, and neither device is opened. 0, or
 * 1 once the failure is reported.
 */
static int device_ends(void)
{
    if (!fd_refused(open("/fb/", O_RDONLY), ENOTDIR) ||
        !fd_refused(open("/fb/.", O_RDONLY), ENOTDIR) ||
        !fd_refused(open("/mouse/", O_RDONLY), ENOTDIR) ||
        !fd_refused(open("/mouse/..", O_RDONLY), ENOTDIR) ||
        !fd_refused(open("/fb", O_RDONLY | O_DIRECTORY), ENOTDIR)) {
        return fail("an open of /fb or /mouse as a directory was not ENOTDIR");
    }
    held();
    return 0;
}

/**
 * The rows that read the probe's own directory beyond the types of its listing:
 * the path ends of `dir_ends`, rewinddir and seekdir of it and of /proc/self
 * (`rewinds`), the root as a name (`root_rows`), names in /proc and /dev that
 * end as directories (`synth_name_rows`) or exist (`synth_taken_rows`,
 * `unreadable_taken_rows`), /fb
 * and /mouse as directories (`device_ends`), and the other calls given a
 * path that names a directory by its end, there and in /dev/shm
 * (`dir_end_calls`). The groups from `root_rows` on each run whether or not
 * one before failed, so a kernel fault that breaks several reports each. 0,
 * or 1 once a failure is reported.
 */
static int own_dir_rows(void)
{
    int seek;
    int rc;

    if (dir_ends(file) != 0) {
        return 1;
    }
    seek = rewinds(base);
    if (seek == 2) {
        held();
    }
    if (seek != 0) {
        return fail(seek == 1 ? "rewinddir of the probe's own directory"
                              : "seekdir of the probe's own directory to a place telldir gave");
    }
    held();
    held();
    if (rewinds("/proc/self") != 0) {
        return fail("rewinddir and seekdir of /proc/self");
    }
    held();
    group_start(&group_reuse);
    rc = reuse_rows();
    group_start(&group_root);
    rc |= root_rows();
    group_start(&group_synth);
    rc |= synth_name_rows();
    group_start(&group_taken);
    rc |= synth_taken_rows();
    group_start(&group_unreadable);
    rc |= unreadable_taken_rows();
    group_start(&group_devices);
    rc |= device_ends();
    group_start(&group_own);
    rc |= dir_end_calls(base, 0);
    group_start(&group_shm);
    return rc | shm_dir_end_calls();
}

/** Remove what the probe made. */
static void tidy(void)
{
    unlink(link_name);
    unlink(file);
    rmdir(sub);
    reuse_tidy();
    rmdir(base);
}

/**
 * Read the small file `path` into `buf` of `cap` bytes, terminated with a
 * NUL. Returns its length, or -1.
 */
static int slurp(const char *path, char *buf, int cap)
{
    int fd = open(path, O_RDONLY);
    int n = fd < 0 ? -1 : (int)read(fd, buf, (size_t)cap - 1);

    if (fd >= 0) {
        close(fd);
    }
    if (n >= 0) {
        buf[n] = '\0';
    }
    return n;
}

/**
 * Whether `s` begins with three loads of two decimals each, as /proc/loadavg
 * writes them, each followed by a space; the text after them is left in
 * `*rest`.
 */
static int three_loads(const char *s, const char **rest)
{
    int i;

    for (i = 0; i < 3; i++) {
        if (*s < '0' || *s > '9') {
            return 0;
        }
        while (*s >= '0' && *s <= '9') {
            s++;
        }
        if (s[0] != '.' || s[1] < '0' || s[1] > '9' || s[2] < '0' || s[2] > '9' || s[3] != ' ') {
            return 0;
        }
        s += 4;
    }
    *rest = s;
    return 1;
}

/**
 * Check /proc/cpuinfo's record, /proc/self/mounts, /proc/loadavg and that
 * there is no process 0, two /proc/sys files, and a working directory in
 * /proc/sys. Returns 0, or 1 once the failure is reported.
 */
static int proc_texts(void)
{
    char buf[4096];
    char cwd[64];

    if (slurp("/proc/cpuinfo", buf, sizeof(buf)) <= 0 ||
        strncmp(buf, "processor\t: 0\n", 14) != 0 || !strstr(buf, "\nflags\t\t:")) {
        return fail("/proc/cpuinfo is not a record of processor 0 with its flags");
    }
    held();
    {
        char mounts[4096];

        if (slurp("/proc/mounts", mounts, sizeof(mounts)) <= 0 ||
            slurp("/proc/self/mounts", buf, sizeof(buf)) <= 0 || strcmp(buf, mounts) != 0) {
            return fail("/proc/self/mounts is not /proc/mounts");
        }
        held();
    }
    {
        unsigned running = 0, total = 0, last = 0;
        const char *rest = buf;
        char end = 0;
        struct stat st;

        if (slurp("/proc/loadavg", buf, sizeof(buf)) <= 0 || !three_loads(buf, &rest) ||
            sscanf(rest, "%u/%u %u%c", &running, &total, &last, &end) != 4 || end != '\n' ||
            running < 1 || total < running || last < (unsigned)getpid()) {
            return fail("/proc/loadavg is not three loads, the running and all tasks, and the "
                        "last process ID");
        }
        held();
        if (stat("/proc/0", &st) == 0 || errno != ENOENT || type_of("/proc", "0") != -1) {
            return fail("/proc has a process 0");
        }
        held();
    }
    if (slurp("/proc/sys/fs/inotify/max_user_watches", buf, sizeof(buf)) < 0 ||
        strcmp(buf, "8192\n") != 0) {
        return fail("/proc/sys/fs/inotify/max_user_watches");
    }
    held();
    if (slurp("/proc/sys/kernel/core_pattern", buf, sizeof(buf)) < 0 ||
        strcmp(buf, "core\n") != 0) {
        return fail("/proc/sys/kernel/core_pattern is not the name of a core file, core");
    }
    held();
    if (chdir("/proc/sys/kernel") != 0 || !getcwd(cwd, sizeof(cwd)) ||
        strcmp(cwd, "/proc/sys/kernel") != 0 || chdir("/") != 0) {
        return fail("a working directory in /proc/sys");
    }
    held();
    return 0;
}

/**
 * Name the probe's directory and its entries under `dir`, which is taken from
 * the working directory when it is relative. Returns 0, or -1 when the working
 * directory cannot be read.
 */
static int name_files(const char *dir)
{
    char where[200];
    const char *sep;

    if (dir[0] != '/') {
        if (getcwd(where, sizeof(where)) == NULL) {
            return -1;
        }
        snprintf(where + strlen(where), sizeof(where) - strlen(where), "/%s", dir);
        dir = where;
    }
    sep = dir[strlen(dir) - 1] == '/' ? "" : "/";
    snprintf(base, sizeof(base), "%s%sdirent.d", dir, sep);
    snprintf(file, sizeof(file), "%s/f", base);
    snprintf(sub, sizeof(sub), "%s/s", base);
    snprintf(link_name, sizeof(link_name), "%s/l", base);
    snprintf(reuse_dir, sizeof(reuse_dir), "%s/r", base);
    return 0;
}

/**
 * Make the probe's directory, in the directory argument when `given`, and
 * check what it lists, then the rows of `own_dir_rows`. Reports the branch
 * of the group `links`, whether the file system has symbolic links, which
 * `*links` receives. Returns 0, or 1 once the failure is reported.
 */
static int own_rows(int given, int *links)
{
    struct statfs sfs;
    int fd;

    group_start(&group_start_rows);
    if (name_files(given ? probe_word(1) : "/") != 0) {
        return fail("the working directory");
    }
    tidy();
    if (mkdir(base, 0755) != 0 || mkdir(sub, 0755) != 0 ||
        (fd = open(file, O_WRONLY | O_CREAT, 0644)) < 0) {
        return fail("make the directory");
    }
    close(fd);
    group_reuse.n = statfs(base, &sfs) == 0 && sfs.f_type == EXT2_MAGIC ? group_reuse.n : 0;
    report_branch("seek", group_reuse.n > 0 ? "bytes" : "places");
    *links = symlink("f", link_name) == 0;
    if (!*links && (given || errno != EPERM)) {
        tidy();
        return fail("a symbolic link where the file system has them");
    }
    held();
    report_branch("links", *links ? "links" : "none");
    group_own.n = *links ? group_own.n : group_own.n - LINK_ROWS;
    if (type_of(base, "f") != DT_REG || type_of(base, "s") != DT_DIR ||
        (*links && type_of(base, "l") != DT_LNK) || !types_agree(base, 0)) {
        tidy();
        return fail("the probe's own directory");
    }
    held();
    if (own_dir_rows() != 0) {
        tidy();
        return 1;
    }
    tidy();
    return 0;
}

/**
 * Check the types /dev and /proc list, those of the root and /bin when the
 * directory is `given`, and the texts of /proc (`proc_texts`). Returns 0, or
 * 1 once the failure is reported.
 */
static int system_rows(int given)
{
    group_start(&group_types);
    if (type_of("/dev", "null") != DT_CHR || !types_agree("/dev", 0) ||
        !types_agree("/dev/pts", 1)) {
        return fail("/dev");
    }
    held();
    if (type_of("/proc", "self") != DT_LNK || type_of("/proc", "meminfo") != DT_REG ||
        type_of("/proc", "cpuinfo") != DT_REG || type_of("/proc", "sys") != DT_DIR ||
        type_of("/proc", "net") != DT_DIR || !types_agree("/proc", 1) ||
        !types_agree("/proc/sys", 0) || !types_agree("/proc/sys/fs/inotify", 0) ||
        !types_agree("/proc/self", 0) || !types_agree("/proc/self/fd", 0) ||
        !types_agree("/proc/net", 0)) {
        return fail("/proc");
    }
    held();
    group_start(&group_given);
    if (given && (!types_agree("/", 0) || !types_agree("/bin", 0))) {
        return fail("the root and /bin");
    }
    if (given) {
        held();
    }
    group_start(&group_texts);
    return proc_texts();
}

int main(int argc, char **argv)
{
    int given;
    int links = 0;
    int rc;

    probe_args(argc, argv);
    given = probe_words() > 0;
    report_begin(report_case(environ, "guest.default.direntmusl"));
#if defined __x86_64__
    report_branch("arch", "x86-64");
#else
    report_branch("arch", "i686");
#endif
    report_branch("dir", given ? "given" : "default");
    group_given.n = given ? 1 : 0;
    rc = own_rows(given, &links);
    if (rc == 0) {
        rc = system_rows(given);
    }
    finish();
    if (rc != 0) {
        return 1;
    }
    printf("DIRENTMUSL OK%s\n", links ? " (links)" : "");
    return 0;
}
