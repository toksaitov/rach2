/** Check account input and descriptor cleanup before identity changes. */
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

static int Failed, Open_fds[1024], Drop_calls, Drop_dirty;
static uid_t Caller;
static const char *Policy;
static size_t Policy_size;
static const char *Input;
static size_t Input_size, Input_at;
static int Read_error;

/** Record failed assertions. */
static void check(int ok, const char *name)
{
    if (!ok) {
        fprintf(stderr, "accounttools-hosttest: fail: %s\n", name);
        Failed++;
    }
}

/** Supply complete policy bytes through a real stdio stream. */
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

/** Supply login bytes and distinguish read failure from partial EOF. */
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

/** Model every descriptor permitted by Rach2, including the last one. */
static int account_close(int fd)
{
    if (fd >= 0 && fd < 1024) {
        Open_fds[fd] = 0;
    }
    return 0;
}
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

/** Observe descriptor state at each identity-changing call. */
static int drop_identity(void)
{
    Drop_calls++;
    if (!descriptors_clean()) {
        Drop_dirty++;
    }
    return 0;
}
static uid_t account_getuid(void)
{
    return Caller;
}
static int account_setuid(uid_t uid)
{
    (void)uid;
    return drop_identity();
}
static int account_setgid(gid_t gid)
{
    (void)gid;
    return drop_identity();
}
static int account_setgroups(size_t n, const gid_t *groups)
{
    (void)n;
    (void)groups;
    return drop_identity();
}
static int account_setresuid(uid_t a, uid_t b, uid_t c)
{
    (void)a;
    (void)b;
    (void)c;
    return drop_identity();
}
static int account_setresgid(gid_t a, gid_t b, gid_t c)
{
    (void)a;
    (void)b;
    (void)c;
    return drop_identity();
}
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
static int account_execve(const char *path, char *const args[], char *const env[])
{
    (void)path;
    (void)args;
    (void)env;
    return -1;
}
static int account_execle(const char *path, const char *arg, ...)
{
    (void)path;
    (void)arg;
    return -1;
}

/** Authentication is outside these parser and descriptor tests. */
int pwauth_read_password(const char *prompt, char *buf, size_t size)
{
    (void)prompt;
    (void)buf;
    (void)size;
    return -1;
}
int pwauth_verify(const char *name, const char *password)
{
    (void)name;
    (void)password;
    return 0;
}
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

/** Pass exact physical lines to sudo's authorization function. */
static void policy_case(const char *data, size_t len, int want, const char *name)
{
    Policy = data;
    Policy_size = len;
    check(permitted(1000) == want, name);
}

/** Check newline and EOF through the same bounded login buffer. */
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

/** Fill all descriptors before invoking each program's cleanup path. */
static void reset_fds(void)
{
    int i;
    for (i = 0; i < 1024; i++) {
        Open_fds[i] = 1;
    }
    Drop_calls = Drop_dirty = 0;
}

/** Exercise source entry points without changing the host identity. */
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
