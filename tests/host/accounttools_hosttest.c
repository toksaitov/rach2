/**
 * Check the policy file of `sudo`, the name `login` reads, and the descriptors
 * `login`, `su` and `sudo` close before they change identity.
 *
 * The three programs are included whole, each `main` renamed. Their file,
 * read, close, identity and exec calls are replaced by models, so the host's
 * identity never changes and no program is run.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <pwd.h>
#include <termios.h>
#include <stdarg.h>
#include <sys/types.h>
#include <sys/stat.h>
#include "pwauth.h"

/*
 * `Open_fds` is 1 for each descriptor the model holds open. `Drop_calls`
 * counts the identity changes since `reset_fds`, and `Drop_dirty` those made
 * while a descriptor above 2 was open.
 */
static int Failed, Open_fds[1024], Drop_calls, Drop_dirty;
static uid_t Caller;
static const char *Policy; /* the bytes of /etc/sudoers, or NULL for none */
static size_t Policy_size;
static const char *Input; /* standard input, which `account_read` gives out */
static size_t Input_size, Input_at;
static int Read_error; /* nonzero to fail every read with `EIO` */

/** Count a failed check and print its name. */
static void check(int ok, const char *name)
{
    if (!ok) {
        fprintf(stderr, "accounttools-hosttest: fail: %s\n", name);
        Failed++;
    }
}

/**
 * Open `Policy` as a temporary stdio stream with every byte in it, or return
 * NULL for a missing file when `Policy` is NULL. Exits 2 if it cannot.
 */
static FILE *account_fopen(const char *path, const char *mode)
{
    FILE *f;

    (void)path;
    (void)mode;
    if (!Policy) {
        return NULL;
    }
    f = tmpfile();
    if (!f || fwrite(Policy, 1, Policy_size, f) != Policy_size) {
        exit(2);
    }
    rewind(f);
    return f;
}

/**
 * Read the next byte of `Input`, one per call; return 0 at its end, or -1
 * with `EIO` while `Read_error` is set.
 */
static ssize_t account_read(int fd, void *buf, size_t len)
{
    (void)fd;
    (void)len;
    if (Read_error) {
        errno = EIO;
        return -1;
    }
    if (Input_at == Input_size) {
        return 0;
    }
    *(char *)buf = Input[Input_at++];
    return 1;
}

/**
 * Mark `fd` closed in the model, which covers every descriptor Rach2 allows,
 * from 0 to 1023.
 */
static int account_close(int fd)
{
    if (fd >= 0 && fd < 1024) {
        Open_fds[fd] = 0;
    }
    return 0;
}

/** Return 1 when only descriptors 0, 1 and 2 are open, and 0 otherwise. */
static int descriptors_clean(void)
{
    int i;
    for (i = 3; i < 1024; i++) {
        if (Open_fds[i]) {
            return 0;
        }
    }
    return Open_fds[0] && Open_fds[1] && Open_fds[2];
}

/** Count an identity change, and whether a descriptor above 2 was open. */
static int drop_identity(void)
{
    Drop_calls++;
    if (!descriptors_clean()) {
        Drop_dirty++;
    }
    return 0;
}

/** Return `Caller`, the real uid the row chooses. */
static uid_t account_getuid(void)
{
    return Caller;
}

/** Record an identity change with `drop_identity` and succeed. */
static int account_setuid(uid_t uid)
{
    (void)uid;
    return drop_identity();
}

/** As `account_setuid`. */
static int account_setgid(gid_t gid)
{
    (void)gid;
    return drop_identity();
}

/** As `account_setuid`. */
static int account_setgroups(size_t n, const gid_t *groups)
{
    (void)n;
    (void)groups;
    return drop_identity();
}

/** As `account_setuid`. */
static int account_setresuid(uid_t a, uid_t b, uid_t c)
{
    (void)a;
    (void)b;
    (void)c;
    return drop_identity();
}

/** As `account_setuid`. */
static int account_setresgid(gid_t a, gid_t b, gid_t c)
{
    (void)a;
    (void)b;
    (void)c;
    return drop_identity();
}

/** Return root's entry, with `/test-shell` as its shell, for any name. */
static struct passwd *account_getpwnam(const char *name)
{
    static struct passwd pw;
    (void)name;
    pw.pw_name = (char *)"root";
    pw.pw_dir = (char *)"/";
    pw.pw_shell = (char *)"/test-shell";
    pw.pw_uid = 0;
    pw.pw_gid = 0;
    return &pw;
}

/** Fail the exec, so that `su` and `sudo` return 1 to the test. */
static int account_execve(const char *path, char *const args[], char *const env[])
{
    (void)path;
    (void)args;
    (void)env;
    return -1;
}

/** Fail the exec, as `account_execve` does. */
static int account_execle(const char *path, const char *arg, ...)
{
    (void)path;
    (void)arg;
    return -1;
}

/**
 * Fail the password read. No row reaches authentication, so the `pwauth.h`
 * calls the programs make are stand-ins.
 */
int pwauth_read_password(const char *prompt, char *buf, size_t size)
{
    (void)prompt;
    (void)buf;
    (void)size;
    return -1;
}

/** Refuse every password, as for a wrong one. */
int pwauth_verify(const char *name, const char *password)
{
    (void)name;
    (void)password;
    return 0;
}

/** Refuse every password, as for a wrong one. */
int pwauth_check(const char *stored, const char *password)
{
    (void)stored;
    (void)password;
    return 0;
}

#define close     account_close
#define read      account_read
#define fopen     account_fopen
#define getuid    account_getuid
#define getpwnam  account_getpwnam
#define setuid    account_setuid
#define setgid    account_setgid
#define setgroups account_setgroups
#define setresuid account_setresuid
#define setresgid account_setresgid
#define execve    account_execve
#define execle    account_execle
#define main      login_main
#include "login.c"
#undef main
#define main su_main
#include "su.c"
#undef main
#undef MAX_FD
#define main sudo_main
#include "sudo.c"
#undef main
#undef close
#undef read
#undef fopen
#undef getuid
#undef getpwnam
#undef setuid
#undef setgid
#undef setgroups
#undef setresuid
#undef setresgid
#undef execve
#undef execle

/**
 * Require `permitted(1000)`, over `len` bytes of policy file `data`, to
 * return `want`.
 */
static void policy_case(const char *data, size_t len, int want, const char *name)
{
    Policy = data;
    Policy_size = len;
    check(permitted(1000) == want, name);
}

/**
 * Read `len` bytes of `data` with `read_line` into a buffer of `size` bytes,
 * at most 7. Require it to return `want`, and on 0 a terminated name equal
 * to `text`; the byte after the buffer must stay untouched.
 */
static void line_case(const char *data, size_t len, size_t size, int want, const char *text,
                      const char *name)
{
    char buf[8];
    int rc;

    memset(buf, 'x', sizeof(buf));
    Input = data;
    Input_size = len;
    Input_at = 0;
    rc = read_line(buf, size);
    check(rc == want && (rc != 0 || (memchr(buf, 0, size) && strcmp(buf, text) == 0)), name);
    check(buf[size] == 'x', "login buffer boundary preserved");
}

/** Mark every descriptor open and clear the identity-change counts. */
static void reset_fds(void)
{
    int i;
    for (i = 0; i < 1024; i++) {
        Open_fds[i] = 1;
    }
    Drop_calls = Drop_dirty = 0;
}

/** Run the rows and return 1 if any failed, 0 otherwise. */
int main(void)
{
    char comment[133], line[133];
    char *su_args[] = {(char *)"su", NULL};
    char *sudo_args[] = {(char *)"sudo", (char *)"/test-command", NULL};

    memset(comment, 'x', sizeof(comment));
    comment[0] = '#';
    memcpy(comment + 127, "1000\n", 5);
    policy_case(comment, 132, 0, "long comment cannot authorize uid");
    memset(line, ' ', sizeof(line));
    memcpy(line + 127, "1000\n", 5);
    policy_case(line, 132, 0, "overlong line rejected");
    policy_case("#1000\n\n1000\n", 12, 1, "complete uid line accepted");
    policy_case("1000", 4, 1, "final uid line accepted");
    policy_case("1000x\n", 6, 0, "uid suffix rejected");
    policy_case("1000\0junk\n", 10, 0, "uid NUL suffix rejected");
    policy_case("+1000\n", 6, 0, "uid sign rejected");
    policy_case(NULL, 0, 0, "missing policy rejected");
    line_case("ab", 2, 5, 0, "ab", "partial EOF terminates name");
    line_case("abcd", 4, 5, 0, "abcd", "exact capacity EOF terminates name");
    line_case("abcdef", 6, 5, 0, "", "overlong EOF discarded");
    line_case("abcdef\n", 7, 5, 0, "", "overlong newline discarded");
    line_case("a", 1, 1, 0, "", "one-byte buffer discards partial line");
    line_case("", 0, 5, -1, "", "empty EOF refused");
    Read_error = 1;
    line_case("ab", 2, 5, -1, "", "name read error refused");
    Read_error = 0;
    reset_fds();
    close_inherited();
    check(descriptors_clean(), "login closes high descriptors");
    reset_fds();
    Caller = 0;
    (void)su_main(1, su_args);
    check(Drop_calls == 3 && Drop_dirty == 0, "su closes high descriptors before identity");
    reset_fds();
    Caller = 1000;
    Policy = "1000\n";
    Policy_size = 5;
    (void)sudo_main(2, sudo_args);
    check(Drop_calls == 2 && Drop_dirty == 0, "sudo closes high descriptors before identity");
    printf("accounttools-hosttest: result: %s\n", Failed ? "fail" : "pass");
    return Failed ? 1 : 0;
}
