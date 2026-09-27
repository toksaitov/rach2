/**
 * The type getdents64 reports in d_type, which GNU libc's and musl's readdir
 * hand on and programs such as find and ls act on without a stat.
 *
 * Every entry of /dev, /dev/pts, /proc, /proc/self, /proc/self/fd, /proc/net
 * and a directory the probe makes must report the type lstat gives it, the
 * mode's type bits shifted down, or DT_UNKNOWN, and never DT_REG for
 * anything but a regular file; /dev/null, /proc/self, /proc/meminfo and
 * /proc/net have their own types by name. With a directory argument, which
 * tests/system/suites/ext2.sh gives as /tmp on an ext2 root, the probe's own
 * directory is made there and must hold a symbolic link reported as DT_LNK,
 * and / and /bin of that root are checked too.
 *
 * Prints "DIRENTMUSL OK" on success and "DIRENTMUSL FAIL (detail)" otherwise.
 */

#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
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
 * DT_UNKNOWN. An entry that is gone by the time of the lstat, a process that
 * exited among them, is passed over. Prints the first entry that disagrees.
 */
static int types_agree(const char *dir)
{
    char path[512];
    struct dirent *de;
    struct stat st;
    DIR *d = opendir(dir);
    int ok = d != NULL;

    while (ok && (de = readdir(d)) != NULL) {
        snprintf(path, sizeof(path), "%s/%s", dir, de->d_name);
        if (lstat(path, &st) != 0) {
            continue;
        }
        if (de->d_type != DT_UNKNOWN && de->d_type != IFTODT(st.st_mode)) {
            printf("direntmusl: %s: d_type %d, lstat type %d\n", path, de->d_type,
                   (int)IFTODT(st.st_mode));
            ok = 0;
        }
    }
    if (d) {
        closedir(d);
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
    fd = -1;
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
        (links && type_of(base, "l") != DT_LNK) || !types_agree(base)) {
        tidy();
        return fail("the probe's own directory");
    }
    tidy();
    if (type_of("/dev", "null") != DT_CHR || !types_agree("/dev") || !types_agree("/dev/pts")) {
        return fail("/dev");
    }
    if (type_of("/proc", "self") != DT_LNK || type_of("/proc", "meminfo") != DT_REG ||
        type_of("/proc", "net") != DT_DIR || !types_agree("/proc") ||
        !types_agree("/proc/self") || !types_agree("/proc/self/fd") ||
        !types_agree("/proc/net")) {
        return fail("/proc");
    }
    if (argc > 1 && (!types_agree("/") || !types_agree("/bin"))) {
        return fail("the root and /bin");
    }
    printf("DIRENTMUSL OK%s\n", links ? " (links)" : "");
    return 0;
}
