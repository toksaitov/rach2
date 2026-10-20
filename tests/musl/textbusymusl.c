/* Check executable write exclusion, its lifetime, and the ELF interpreter.
 *
 * The copy that ends by exec runs /musl/statushelper, or the path that
 * `helper=PATH` gives, as a native run's private root has the helper at /
 * (tests/native/Readme.md); the copies are passed the same argument. Other
 * NAME=VALUE arguments, such as `root=PATH`, are ignored. */
#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../probe_args.h"

/* The path `helper=PATH` gives, or null for /musl/statushelper. */
static const char *helper;
static char work[256];
static pid_t children[8];
static int nchildren;
static int mounted;
static int created;

/** Reap children and remove every fixture, including after a failed row. */
static void cleanup(void)
{
    int i;
    char path[320];
    const char *names[] = {"program", "moved", "link", "interp", "dynamic", "bad", NULL};

    for (i = 0; i < nchildren; i++) {
        if (children[i] > 0) {
            kill(children[i], SIGKILL);
            waitpid(children[i], NULL, 0);
        }
    }
    for (i = 0; created && names[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", work, names[i]);
        unlink(path);
    }
    if (mounted) {
        umount(work);
    }
    if (created) {
        rmdir(work);
        created = 0;
    }
}

/** Exit through the cleanup registered before any fixture was made. */
static void interrupted(int sig)
{
    exit(128 + sig);
}

/** Require a row, preserving errno for the diagnostic. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("TEXTBUSYMUSL FAIL (%s): errno %d\n", what, errno);
        exit(1);
    }
}

/** Copy a staged executable into a disposable writable file. */
static void copy(const char *from, const char *to)
{
    char buf[4096];
    ssize_t n;
    int in = open(from, O_RDONLY);
    int out = open(to, O_CREAT | O_TRUNC | O_WRONLY, 0755);

    check(in >= 0 && out >= 0, "copy open");
    while ((n = read(in, buf, sizeof(buf))) > 0) {
        check(write(out, buf, n) == n, "copy write");
    }
    check(n == 0, "copy read");
    close(in);
    close(out);
}

/** Start a copy and wait for its post-exec pipe message, without a delay. */
static pid_t start(const char *path, int forked, int *control)
{
    int ready[2], hold[2];
    char a[24], b[24], given[300];
    pid_t pid, running;

    check(pipe(ready) == 0 && pipe(hold) == 0, "pipes");
    snprintf(a, sizeof(a), "%d", ready[1]);
    snprintf(b, sizeof(b), "%d", hold[0]);
    snprintf(given, sizeof(given), "helper=%s", helper ? helper : "");
    pid = fork();
    check(pid >= 0, "fork");
    if (pid == 0) {
        char *args[] = {(char *)path, forked ? "fork" : "hold", a, b, helper ? given : NULL, NULL};
        close(ready[0]);
        close(hold[1]);
        execv(path, args);
        _exit(111);
    }
    children[nchildren++] = pid;
    close(ready[1]);
    close(hold[0]);
    check(read(ready[0], &running, sizeof(running)) == sizeof(running), "exec ready");
    close(ready[0]);
    if (forked) {
        check(waitpid(pid, NULL, 0) == pid, "fork parent exit");
        children[nchildren - 1] = running;
    }
    *control = hold[1];
    return running;
}

/** End or exec a child, then wait until its address space is released. */
static void finish(pid_t pid, int control, char command)
{
    int status, i;

    check(write(control, &command, 1) == 1, "release child");
    close(control);
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "child status");
    for (i = 0; i < nchildren; i++) {
        if (children[i] == pid) {
            children[i] = 0;
        }
    }
}

/** A running file rejects writing and truncation, including through aliases. */
static void busy(const char *path)
{
    int fd;

    errno = 0;
    fd = open(path, O_WRONLY);
    if (fd >= 0) {
        close(fd);
    }
    check(fd == -1 && errno == ETXTBSY, "write open");
    check(open(path, O_RDWR) == -1 && errno == ETXTBSY, "read write open");
    check(open(path, O_RDONLY | O_TRUNC) == -1 && errno == ETXTBSY, "read truncate open");
    check(open(path, O_WRONLY | O_TRUNC) == -1 && errno == ETXTBSY, "write truncate open");
    check(truncate(path, 0) == -1 && errno == ETXTBSY, "truncate");
    fd = open(path, O_RDONLY);
    check(fd >= 0, "read running file");
    check(ftruncate(fd, 0) == -1 && errno == EINVAL, "readonly ftruncate");
    check(fallocate(fd, 0, 0, 1) == -1 && errno == EBADF, "readonly fallocate");
    close(fd);
}

/** With the last executable reference gone, a writer and truncation succeed. */
static void writable(const char *path)
{
    int fd = open(path, O_RDWR);
    struct stat st;

    check(fd >= 0 && fstat(fd, &st) == 0, "last release open");
    check(ftruncate(fd, st.st_size) == 0, "last release ftruncate");
    check(fallocate(fd, 0, 0, 1) == 0 || errno == EOPNOTSUPP, "last release fallocate");
    close(fd);
    check(truncate(path, st.st_size) == 0, "last release truncate");
}

/** An exec is refused while any duplicate of a writer remains open. */
static void writer(const char *path, const char *target)
{
    int fd = open(path, O_WRONLY | O_CLOEXEC);
    int alias, status;
    pid_t pid;

    check(fd >= 0, "writer open");
    alias = dup(fd);
    check(alias >= 0, "writer duplicate");
    close(fd);
    pid = fork();
    check(pid >= 0, "writer fork");
    if (pid == 0) {
        char *args[] = {(char *)target, "unexpected", NULL};
        execv(target, args);
        _exit(errno == ETXTBSY ? 0 : 112);
    }
    check(waitpid(pid, &status, 0) == pid && status == 0, "exec writer");
    close(alias);
}

/** Build a small main ELF whose interpreter is a copy of this static probe. */
static void dynamic(const char *path, const char *interp)
{
#if defined(__x86_64__)
    struct {
        Elf64_Ehdr eh;
        Elf64_Phdr ph[2];
        char name[320];
    } image;
    int fd;

    memset(&image, 0, sizeof(image));
    memcpy(image.eh.e_ident, ELFMAG, SELFMAG);
    image.eh.e_ident[EI_CLASS] = ELFCLASS64;
    image.eh.e_ident[EI_DATA] = ELFDATA2LSB;
    image.eh.e_ident[EI_VERSION] = EV_CURRENT;
    image.eh.e_type = ET_EXEC;
    image.eh.e_machine = EM_X86_64;
    image.eh.e_version = EV_CURRENT;
    image.eh.e_entry = 0x40000000;
    image.eh.e_phoff = sizeof(image.eh);
    image.eh.e_ehsize = sizeof(image.eh);
    image.eh.e_phentsize = sizeof(image.ph[0]);
    image.eh.e_phnum = 2;
    image.ph[0].p_type = PT_LOAD;
    image.ph[0].p_vaddr = 0x40000000;
    image.ph[0].p_filesz = sizeof(image);
    image.ph[0].p_memsz = sizeof(image);
    image.ph[0].p_flags = PF_R | PF_X;
    image.ph[0].p_align = 4096;
    image.ph[1].p_type = PT_INTERP;
    image.ph[1].p_offset = sizeof(image.eh) + sizeof(image.ph);
    image.ph[1].p_filesz = strlen(interp) + 1;
    strcpy(image.name, interp);
    fd = open(path, O_CREAT | O_WRONLY | O_TRUNC, 0755);
    check(fd >= 0 && write(fd, &image, sizeof(image)) == sizeof(image), "dynamic fixture");
    close(fd);
#else
    (void)path;
    (void)interp;
#endif
}

/** Run the executable child, optionally leaving a forked image behind. */
static void child_mode(char **argv)
{
    char command;
    pid_t pid;
    if (strcmp(argv[1], "fork") == 0) {
        pid = fork();
        if (pid != 0) {
            _exit(pid < 0 ? 113 : 0);
        }
    }
    pid = getpid();
    if (write(atoi(argv[2]), &pid, sizeof(pid)) != sizeof(pid)) {
        _exit(114);
    }
    close(atoi(argv[2]));
    if (read(atoi(argv[3]), &command, 1) != 1) {
        _exit(115);
    }
    close(atoi(argv[3]));
    if (command == 'x') {
        char *args[] = {(char *)(helper ? helper : "/musl/statushelper"), "0", NULL};
        execv(args[0], args);
        _exit(116);
    }
    return;
}

/** Check lifetimes and aliases on one writable file system. */
static void filesystem(int tmpfs)
{
    char program[320], moved[320], linkpath[320], interp[320], dyn[320], bad[320];
    int a, b, fd;
    pid_t one, two;
    snprintf(work, sizeof(work), "%s/rach2-textbusy.XXXXXX",
             getenv("TMPDIR") ? getenv("TMPDIR") : "/tmp");
    check(mkdtemp(work) != NULL, "scratch");
    created = 1;
    if (tmpfs) {
        check(mount("none", work, "tmpfs", 0, NULL) == 0, "tmpfs");
        mounted = 1;
    }
    snprintf(program, sizeof(program), "%s/program", work);
    snprintf(moved, sizeof(moved), "%s/moved", work);
    snprintf(linkpath, sizeof(linkpath), "%s/link", work);
    snprintf(interp, sizeof(interp), "%s/interp", work);
    snprintf(dyn, sizeof(dyn), "%s/dynamic", work);
    snprintf(bad, sizeof(bad), "%s/bad", work);
    copy("/musl/textbusymusl", program);
    writer(program, program);
    one = start(program, 0, &a);
    two = start(program, 0, &b);
    busy(program);
    check(rename(program, moved) == 0, "rename running");
    busy(moved);
    if (tmpfs) {
        check(link(moved, linkpath) == 0, "link running");
        busy(linkpath);
        unlink(linkpath);
    }
    finish(one, a, 'e');
    busy(moved);
    finish(two, b, 'x');
    writable(moved);
    one = start(moved, 1, &a);
    busy(moved);
    finish(one, a, 'e');
    writable(moved);
    one = start(moved, 0, &a);
    check(unlink(moved) == 0, "unlink running");
    finish(one, a, 'e');
    fd = open(bad, O_CREAT | O_WRONLY, 0755);
    check(fd >= 0 && write(fd, "bad", 3) == 3, "bad fixture");
    close(fd);
    {
        char *args[] = {bad, NULL};
        check(execv(bad, args) == -1 && errno == ENOEXEC, "failed exec");
    }
    writable(bad);
    copy("/musl/textbusymusl", interp);
    dynamic(dyn, interp);
    writer(interp, dyn);
    one = start(dyn, 0, &a);
    busy(interp);
    busy(dyn);
    finish(one, a, 'x');
    writable(interp);
    writable(dyn);
    one = start(dyn, 1, &a);
    check(rename(interp, moved) == 0, "rename interpreter");
    busy(moved);
    check(unlink(moved) == 0, "unlink interpreter");
    finish(one, a, 'e');
    cleanup();
    mounted = 0;
    work[0] = 0;
    nchildren = 0;
}

/** Run the exclusion rows on the boot root and a tmpfs. */
int main(int argc, char **argv)
{
    probe_args(argc, argv);
    helper = probe_arg("helper", NULL);
    if (probe_words() == 1) {
        return 117;
    }
    if (probe_words() == 3) {
        child_mode(argv);
        return 0;
    }
    atexit(cleanup);
    signal(SIGTERM, interrupted);
    signal(SIGINT, interrupted);
    signal(SIGHUP, interrupted);
    signal(SIGPIPE, interrupted);
    filesystem(0);
    filesystem(1);
    puts("TEXTBUSYMUSL OK");
    return 0;
}
