/**
 * The type getdents64 reports in d_type, which GNU libc's and musl's readdir
 * hand on and programs such as find and ls act on without a stat; the seeks
 * of a directory's listing; and the answer each call that takes a path
 * gives one that names a directory by its end.
 *
 * Every entry of /dev, /dev/pts, /proc, /proc/self, /proc/self/fd, /proc/net
 * and a directory the probe makes must report the type lstat gives it, the
 * mode's type bits shifted down, or DT_UNKNOWN, and never DT_REG for
 * anything but a regular file; /dev/null, /proc/self, /proc/meminfo and
 * /proc/net have their own types by name. Each listing must end without an
 * error from readdir and check at least one entry, and lstat may fail only
 * with ENOENT, only in /proc and /dev/pts, whose entries come and go. A path
 * that names a directory by its end is followed there, by lstat and by the
 * old lstat, and a file named that way is ENOTDIR. The probe's own
 * directory and /proc/self, read to their end, list as many entries again
 * after rewinddir, and seekdir to the place telldir gave after the first
 * entry lists the second next.
 *
 * The other calls that take a path answer one that names a directory by its
 * end as Linux does, in the probe's own directory and in one of /dev/shm: a
 * regular file so named is ENOTDIR to open, access, chmod, chown, lchown,
 * truncate, utimensat, readlink, statfs, link, unlink and rename, and a new
 * name so named is EISDIR to open with O_CREAT and ENOENT or EEXIST to link,
 * symlink, mknod and mkdir, which make nothing; rmdir is EINVAL for a last
 * "." and ENOTEMPTY for a last "..", and rename EBUSY for either, and so do
 * unlinkat and renameat through a directory descriptor. execve of a file so
 * named is ENOTDIR and of a directory EACCES, and so is a script whose
 * interpreter is named with a trailing slash ENOTDIR, and an ELF image whose
 * PT_INTERP names the file or the directory so ENOTDIR or EACCES; bind to a
 * name so named makes nothing, connect to a socket so named is ENOTDIR, and
 * so is umount2 of a file so named. rmdir and rename of the root are EBUSY,
 * link, symlink and mknod with the root as the new name EEXIST and bind to
 * it EADDRINUSE. open with O_CREAT is EISDIR for an existing directory, one
 * of the probe's, one of /dev/shm and /proc, even to read, and with
 * O_DIRECTORY EINVAL before the path is read, unless O_PATH is given too,
 * which drops O_CREAT. A symbolic link to a directory named with a trailing
 * slash is followed by the calls that look it up, O_NOFOLLOW and
 * AT_SYMLINK_NOFOLLOW notwithstanding, and is ENOTDIR to unlink and rename,
 * in /dev/shm and on an ext2 root.
 *
 * With a directory argument, which tests/system/suites/ext2.sh gives as /tmp
 * on an ext2 root, the probe's own directory is made there and must hold a
 * symbolic link reported as DT_LNK, and / and /bin of that root are checked
 * too.
 *
 * Prints "DIRENTMUSL OK" on success and "DIRENTMUSL FAIL (detail)" otherwise.
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
#include <unistd.h>

/* The directory the probe makes, and its entries. */
static char base[256], file[300], sub[300], link_name[300];

/** Report one failed check and return 1, the probe's failure status. */
static int fail(const char *what)
{
    printf("DIRENTMUSL FAIL (%s)\n", what);

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
 * to stat and lstat. 0, or 1 once the failure is reported.
 */
static int dir_ends(const char *file)
{
    unsigned char old[64];
    char path[520];
    struct stat st;

    if (lstat("/proc/self/.", &st) != 0 || !S_ISDIR(st.st_mode)) {
        return fail("lstat of /proc/self/. did not reach the directory");
    }
    memset(old, 0, sizeof(old));
    if (syscall(SYS_lstat, "/proc/self/.", old) != 0 ||
        !S_ISDIR((mode_t)(old[8] | (old[9] << 8)))) {
        return fail("the old lstat of /proc/self/. did not reach the directory");
    }
    snprintf(path, sizeof(path), "%s/", file);
    if (stat(path, &st) != -1 || errno != ENOTDIR || lstat(path, &st) != -1 || errno != ENOTDIR) {
        return fail("a file named with a trailing slash was not ENOTDIR");
    }
    snprintf(path, sizeof(path), "%s/.", file);
    if (lstat(path, &st) != -1 || errno != ENOTDIR) {
        return fail("a file named with a last . was not ENOTDIR");
    }
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
 * what differs.
 */
static int rewinds(const char *dir)
{
    char second[256];
    struct dirent *de = NULL;
    DIR *d = opendir(dir);
    int count = 0;
    int again = -1;

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
        }
    }
    if (d) {
        closedir(d);
    }
    if (count < 2 || again != count) {
        printf("direntmusl: %s: %d entries, %d after rewinddir\n", dir, count, again);
        return 1;
    }
    if (!de || strcmp(de->d_name, second) != 0) {
        printf("direntmusl: %s: seekdir did not list %s again\n", dir, second);
        return 2;
    }
    return 0;
}

/** Whether a call returned -1 with `err`. */
static int refused(long rc, int err)
{
    return rc == -1 && errno == err;
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

    if (!refused(open(p->fs, O_RDONLY), ENOTDIR) || !refused(open(p->fdot, O_RDONLY), ENOTDIR) ||
        !refused(open(p->fs, O_PATH), ENOTDIR)) {
        return fail("open of a file named with a trailing slash or a last . was not ENOTDIR");
    }
    if (!refused(access(p->fs, F_OK), ENOTDIR)) {
        return fail("access of a file named with a trailing slash was not ENOTDIR");
    }
    if (!refused(chmod(p->fs, 0644), ENOTDIR)) {
        return fail("chmod of a file named with a trailing slash was not ENOTDIR");
    }
    if (!refused(chown(p->fs, (uid_t)-1, (gid_t)-1), ENOTDIR) ||
        !refused(lchown(p->fs, (uid_t)-1, (gid_t)-1), ENOTDIR)) {
        return fail("chown or lchown of a file named with a trailing slash was not ENOTDIR");
    }
    if (!refused(truncate(p->fs, 0), ENOTDIR)) {
        return fail("truncate of a file named with a trailing slash was not ENOTDIR");
    }
    if (!refused(utimensat(AT_FDCWD, p->fs, NULL, 0), ENOTDIR)) {
        return fail("utimensat of a file named with a trailing slash was not ENOTDIR");
    }
    if (!refused(readlink(p->fs, buf, sizeof(buf)), ENOTDIR) ||
        !refused(readlink(p->ss, buf, sizeof(buf)), EINVAL)) {
        return fail("readlink of a name with a trailing slash was not ENOTDIR or EINVAL");
    }
    if (!refused(statfs(p->fs, &sf), ENOTDIR)) {
        return fail("statfs of a file named with a trailing slash was not ENOTDIR");
    }
    if (!refused(link(p->fs, p->n), ENOTDIR)) {
        return fail("link from a file named with a trailing slash was not ENOTDIR");
    }
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

    if (!refused(open(p->s, O_RDONLY | O_CREAT | O_DIRECTORY, 0644), EINVAL) ||
        !refused(open(p->n, O_RDONLY | O_CREAT | O_DIRECTORY, 0644), EINVAL) ||
        !refused(openat(AT_FDCWD, "", O_RDONLY | O_CREAT | O_DIRECTORY, 0644), EINVAL) ||
        lstat(p->n, &st) == 0) {
        return fail("an open with O_CREAT and O_DIRECTORY was not EINVAL");
    }
    fd = open(p->s, O_PATH | O_CREAT | O_DIRECTORY, 0644);
    if (fd < 0) {
        return fail("an open with O_PATH, O_CREAT and O_DIRECTORY did not name the directory");
    }
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
 * them makes n. open with O_CREAT, which asks for a file, is EISDIR for the
 * directory s and for /proc too, even to read, and with O_DIRECTORY EINVAL
 * (`create_dir_pair`). 0, or 1 once the failure is reported.
 */
static int make_ends(const struct ends *p)
{
    struct stat st;

    if (!refused(open(p->ns, O_WRONLY | O_CREAT, 0644), EISDIR) ||
        !refused(open(p->sdot, O_WRONLY | O_CREAT, 0644), EISDIR) ||
        !refused(open(p->sdot, O_WRONLY | O_CREAT | O_EXCL, 0644), EEXIST) ||
        !refused(open(p->sdotdot, O_RDONLY | O_CREAT | O_EXCL, 0644), EEXIST) ||
        lstat(p->n, &st) == 0) {
        return fail("an open with O_CREAT of a path that names a directory by its end");
    }
    if (!refused(open(p->s, O_RDONLY | O_CREAT, 0644), EISDIR) ||
        !refused(open("/proc", O_RDONLY | O_CREAT, 0644), EISDIR)) {
        return fail("an open with O_CREAT of an existing directory was not EISDIR");
    }
    if (!refused(link(p->f, p->ns), ENOENT) || !refused(link(p->f, p->sdot), EEXIST)) {
        return fail("link to a new name with a trailing slash or a last .");
    }
    if (!refused(symlink("f", p->ns), ENOENT) || !refused(symlink("f", p->sdot), EEXIST)) {
        return fail("symlink to a new name with a trailing slash or a last .");
    }
    if (!refused(mknod(p->ns, S_IFIFO | 0644, 0), ENOENT) ||
        !refused(mknod(p->sdot, S_IFIFO | 0644, 0), EEXIST) ||
        !refused(mknod(p->fdot, S_IFIFO | 0644, 0), ENOTDIR)) {
        return fail("mknod of a new name with a trailing slash or a last .");
    }
    if (!refused(mkdir(p->sdot, 0755), EEXIST) || !refused(mkdir("/proc/.", 0755), EEXIST) ||
        !refused(mkdir(p->ndot, 0755), ENOENT) || lstat(p->n, &st) == 0) {
        return fail("mkdir of a path whose last name is .");
    }
    return create_dir_pair(p);
}

/**
 * The calls that remove or move a name, given one as a directory by its
 * end: unlink is ENOTDIR for the file with a slash or a last "." after it
 * and EISDIR for a last "." after a directory; rmdir is EINVAL for a last
 * "." and ENOTEMPTY for a last "..";
 * rename is EBUSY for either as the last component of either name, and
 * ENOTDIR for the file with a slash after either name; and none of them
 * takes f or e away. 0, or 1 once the failure is reported.
 */
static int remove_ends(const struct ends *p)
{
    struct stat st;

    if (!refused(unlink(p->fs), ENOTDIR) || !refused(unlink(p->fdot), ENOTDIR) ||
        !refused(unlink(p->sdot), EISDIR) || lstat(p->f, &st) != 0) {
        return fail("unlink of a file named with a trailing slash or of a last .");
    }
    if (!refused(rmdir(p->edot), EINVAL) || !refused(rmdir(p->edotdot), ENOTEMPTY) ||
        lstat(p->e, &st) != 0) {
        return fail("rmdir of a directory's . or ..");
    }
    if (!refused(rename(p->sdot, p->n), EBUSY) || !refused(rename(p->f, p->sdot), EBUSY) ||
        !refused(rename(p->sdotdot, p->n), EBUSY)) {
        return fail("rename of a name whose last component is . or ..");
    }
    if (!refused(rename(p->fs, p->n), ENOTDIR) || !refused(rename(p->f, p->ns), ENOTDIR) ||
        lstat(p->f, &st) != 0) {
        return fail("rename of a file named with a trailing slash");
    }
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
    if (!renamed) {
        return fail("renameat of a name whose last component is . or .. through a descriptor");
    }
    return 0;
}

/* A script whose interpreter is named with a trailing slash: the kernel
 * refuses it, and a kernel that ran the interpreter would print the line. */
static const char slash_script[] =
    "#!/bin/sh/\n"
    "echo 'DIRENTMUSL FAIL (a script whose interpreter is named with a trailing slash ran)'\n";

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

/**
 * Write the ELF image `path`, mode 0755, whose PT_INTERP is `interp`: the
 * ELF header, the PT_INTERP, and one PT_LOAD at 0x08048000 that covers the
 * whole file, headers and interpreter path included, which the kernel loads
 * before it opens the interpreter. 0, or -1 when the file could not be
 * written.
 */
static int write_elf(const char *path, const char *interp)
{
    unsigned char img[sizeof(Elf32_Ehdr) + 2 * sizeof(Elf32_Phdr) + 320];
    Elf32_Ehdr eh;
    Elf32_Phdr ph[2];
    size_t at = sizeof(eh) + sizeof(ph);
    size_t size = at + strlen(interp) + 1;
    int fd;
    int ok;

    if (size > sizeof(img)) {
        return -1;
    }
    memset(&eh, 0, sizeof(eh));
    memcpy(eh.e_ident, ELFMAG, SELFMAG);
    eh.e_ident[EI_CLASS] = ELFCLASS32;
    eh.e_ident[EI_DATA] = ELFDATA2LSB;
    eh.e_ident[EI_VERSION] = EV_CURRENT;
    eh.e_type = ET_EXEC;
    eh.e_machine = EM_386;
    eh.e_version = EV_CURRENT;
    eh.e_entry = 0x08048000 + at;
    eh.e_phoff = sizeof(eh);
    eh.e_ehsize = sizeof(eh);
    eh.e_phentsize = sizeof(Elf32_Phdr);
    eh.e_phnum = 2;
    memset(ph, 0, sizeof(ph));
    ph[0].p_type = PT_INTERP;
    ph[0].p_offset = at;
    ph[0].p_vaddr = 0x08048000 + at;
    ph[0].p_filesz = size - at;
    ph[0].p_memsz = size - at;
    ph[0].p_flags = PF_R;
    ph[1].p_type = PT_LOAD;
    ph[1].p_vaddr = 0x08048000;
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
    static char *const args[] = {"direntmusl", NULL};
    int ok = write_elf(p->x, p->fs) == 0 && refused(execve(p->x, args, args + 1), ENOTDIR) &&
             write_elf(p->x, p->fdot) == 0 && refused(execve(p->x, args, args + 1), ENOTDIR) &&
             write_elf(p->x, p->ss) == 0 && refused(execve(p->x, args, args + 1), EACCES) &&
             write_elf(p->x, p->sdotdot) == 0 && refused(execve(p->x, args, args + 1), EACCES);

    unlink(p->x);
    if (!ok) {
        return fail("an ELF image whose PT_INTERP names a directory by its end was not refused");
    }
    return 0;
}

/**
 * execve given a path that names a directory by its end: the file so named
 * is ENOTDIR and the directory EACCES, and a script whose interpreter is
 * named with a trailing slash is ENOTDIR, and so is an ELF image whose
 * PT_INTERP is (`interp_ends`). 0, or 1 once the failure is reported.
 */
static int exec_ends(const struct ends *p)
{
    static char *const args[] = {"direntmusl", NULL};
    int fd;
    int ok;

    if (!refused(execve(p->fs, args, args + 1), ENOTDIR) ||
        !refused(execve(p->fdot, args, args + 1), ENOTDIR) ||
        !refused(execve(p->ss, args, args + 1), EACCES) ||
        !refused(execve(p->sdot, args, args + 1), EACCES)) {
        return fail("execve of a path that names a directory by its end was not ENOTDIR or EACCES");
    }
    fd = open(p->x, O_WRONLY | O_CREAT | O_TRUNC, 0755);
    ok = fd >= 0 &&
         write(fd, slash_script, sizeof(slash_script) - 1) == (ssize_t)(sizeof(slash_script) - 1);
    if (fd >= 0) {
        close(fd);
    }
    ok = ok && refused(execve(p->x, args, args + 1), ENOTDIR);
    unlink(p->x);
    if (!ok) {
        return fail("a script whose interpreter has a trailing slash was not ENOTDIR");
    }
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
    if (!connected) {
        return fail("connect to a socket named with a trailing slash was not ENOTDIR");
    }
    if (!refused(umount2(p->fs, 0), ENOTDIR)) {
        return fail("umount2 of a file named with a trailing slash was not ENOTDIR");
    }
    return 0;
}

/**
 * A symbolic link to a directory named with a trailing slash names the
 * directory, and the calls that look the path up follow it, as Linux's do,
 * even where they would not follow a final link otherwise: open with
 * O_NOFOLLOW, lchown, utimensat with AT_SYMLINK_NOFOLLOW, and readlink,
 * which is EINVAL for the directory. unlink and rename judge the name
 * itself, a link and no directory, and are ENOTDIR, leaving it. 0, or 1 once
 * the failure is reported.
 */
static int link_ends(const struct ends *p)
{
    struct stat st;
    char buf[16];
    int fd = open(p->lds, O_RDONLY | O_NOFOLLOW);
    int ok = fd >= 0;

    if (fd >= 0) {
        close(fd);
    }
    ok = ok && lchown(p->lds, (uid_t)-1, (gid_t)-1) == 0 &&
         utimensat(AT_FDCWD, p->lds, NULL, AT_SYMLINK_NOFOLLOW) == 0 &&
         refused(readlink(p->lds, buf, sizeof(buf)), EINVAL);
    if (!ok) {
        return fail("a symbolic link to a directory named with a trailing slash was not followed");
    }
    if (!refused(unlink(p->lds), ENOTDIR) || !refused(rename(p->lds, p->n), ENOTDIR) ||
        lstat(p->ld, &st) != 0 || !S_ISLNK(st.st_mode)) {
        return fail("unlink or rename of a link named with a trailing slash was not ENOTDIR");
    }
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
    if (!refused(link(file, "/"), EEXIST) || !refused(symlink("f", "/"), EEXIST) ||
        !refused(mknod("/", S_IFIFO | 0644, 0), EEXIST)) {
        return fail("link, symlink or mknod with the root as the new name was not EEXIST");
    }
    s = socket(AF_UNIX, SOCK_STREAM, 0);
    bound = s >= 0 && refused(bind_path(s, "/"), EADDRINUSE);
    if (s >= 0) {
        close(s);
    }
    if (!bound) {
        return fail("a bind to the root was not EADDRINUSE");
    }
    return 0;
}

/**
 * The rows that read the probe's own directory beyond the types of its
 * listing: the path ends of `dir_ends`, rewinddir and seekdir of it and of
 * /proc/self (`rewinds`), the root as a name (`root_rows`), and the other
 * calls given a path that names a directory by its end, there and in
 * /dev/shm (`dir_end_calls`). 0, or 1 once the failure is reported.
 */
static int own_dir_rows(void)
{
    int seek;

    if (dir_ends(file) != 0) {
        return 1;
    }
    seek = rewinds(base);
    if (seek != 0) {
        return fail(seek == 1 ? "rewinddir of the probe's own directory"
                              : "seekdir of the probe's own directory to a place telldir gave");
    }
    if (rewinds("/proc/self") != 0) {
        return fail("rewinddir and seekdir of /proc/self");
    }
    if (root_rows() != 0) {
        return 1;
    }
    return dir_end_calls(base, 0) != 0 || shm_dir_end_calls() != 0;
}

/** Remove what the probe made. */
static void tidy(void)
{
    unlink(link_name);
    unlink(file);
    rmdir(sub);
    rmdir(base);
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "/";
    const char *sep = dir[strlen(dir) - 1] == '/' ? "" : "/";
    int links;
    int fd;

    snprintf(base, sizeof(base), "%s%sdirent.d", dir, sep);
    snprintf(file, sizeof(file), "%s/f", base);
    snprintf(sub, sizeof(sub), "%s/s", base);
    snprintf(link_name, sizeof(link_name), "%s/l", base);
    tidy();
    if (mkdir(base, 0755) != 0 || mkdir(sub, 0755) != 0 ||
        (fd = open(file, O_WRONLY | O_CREAT, 0644)) < 0) {
        return fail("make the directory");
    }
    close(fd);
    links = symlink("f", link_name) == 0;
    if (!links && (argc > 1 || errno != EPERM)) {
        tidy();
        return fail("a symbolic link where the file system has them");
    }
    if (type_of(base, "f") != DT_REG || type_of(base, "s") != DT_DIR ||
        (links && type_of(base, "l") != DT_LNK) || !types_agree(base, 0)) {
        tidy();
        return fail("the probe's own directory");
    }
    if (own_dir_rows() != 0) {
        tidy();
        return 1;
    }
    tidy();
    if (type_of("/dev", "null") != DT_CHR || !types_agree("/dev", 0) ||
        !types_agree("/dev/pts", 1)) {
        return fail("/dev");
    }
    if (type_of("/proc", "self") != DT_LNK || type_of("/proc", "meminfo") != DT_REG ||
        type_of("/proc", "net") != DT_DIR || !types_agree("/proc", 1) ||
        !types_agree("/proc/self", 0) || !types_agree("/proc/self/fd", 0) ||
        !types_agree("/proc/net", 0)) {
        return fail("/proc");
    }
    if (argc > 1 && (!types_agree("/", 0) || !types_agree("/bin", 0))) {
        return fail("the root and /bin");
    }
    printf("DIRENTMUSL OK%s\n", links ? " (links)" : "");
    return 0;
}
