/**
 * Check complete lock parsing, echo failures and failed credential cleanup.
 * System-call substitutes inject failures into the included library. Files
 * and captured diagnostics use a private directory on the host file system.
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <termios.h>
#include <crypt.h>
#include <shadow.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <sys/types.h>

static int Failed, Read_calls, Echo_calls, Echo_fail, Resume_read;
static int Pid_write_fail, Flush_fail, Read_fail, Read_short;
static const char *Unlink_fail, *Unlink_after_flush;
static const unsigned char *Input;
static size_t Input_len, Input_pos;
static void resume_prompt(void);

/** Inject read errors, short reads and a continue during password input. */
static ssize_t fault_read(int fd, void *buf, size_t n)
{
    if (fd == 0) {
        Read_calls++;
        if (Resume_read) {
            Resume_read = 0;
            Echo_fail = 1;
            resume_prompt();
            errno = EINTR;
            return -1;
        }
        if (Input_pos == Input_len) {
            return 0;
        }
        *(unsigned char *)buf = Input[Input_pos++];
        return 1;
    }
    if (Read_fail) {
        Read_fail = 0;
        errno = EIO;
        return -1;
    }
    return read(fd, buf, Read_short && n > 1 ? 1 : n);
}

/** Fail the acquisition write without intercepting diagnostic writes. */
static ssize_t fault_write(int fd, const void *buf, size_t n)
{
    if (Pid_write_fail && fd != 2) {
        errno = EIO;
        return -1;
    }
    return write(fd, buf, n);
}

/** Leave the selected file present when cleanup is attempted. */
static int fault_unlink(const char *path)
{
    if (Unlink_fail && strcmp(path, Unlink_fail) == 0) {
        errno = EIO;
        return -1;
    }
    return unlink(path);
}

/** Fail a replacement flush before publication. */
static int fault_fsync(int fd)
{
    if (Flush_fail) {
        if (Unlink_after_flush) {
            Unlink_fail = Unlink_after_flush;
        }
        errno = EIO;
        return -1;
    }
    return fsync(fd);
}

/** Model a foreground terminal and count echo-setting attempts. */
static int fault_isatty(int fd)
{
    (void)fd;
    return 1;
}
static pid_t fault_tcgetpgrp(int fd)
{
    (void)fd;
    return getpgrp();
}
static int fault_tcgetattr(int fd, struct termios *modes)
{
    (void)fd;
    memset(modes, 0, sizeof(*modes));
    modes->c_lflag = ECHO;
    return 0;
}
static int fault_tcsetattr(int fd, int action, const struct termios *modes)
{
    (void)fd;
    (void)action;
    (void)modes;
    Echo_calls++;
    if (Echo_fail) {
        errno = EIO;
        return -1;
    }
    return 0;
}

/** Keep the guest signal policy separate from the host's signal numbers. */
static int fault_sigaction(int sig, const struct sigaction *act, struct sigaction *old)
{
    (void)sig;
    (void)act;
    if (old) {
        memset(old, 0, sizeof(*old));
    }
    return 0;
}
static int fault_sigprocmask(int how, const sigset_t *set, sigset_t *old)
{
    (void)how;
    (void)set;
    if (old) {
        sigemptyset(old);
    }
    return 0;
}

/** Authentication and entropy are outside these failure-path tests. */
static char *fault_crypt(const char *key, const char *salt)
{
    (void)key;
    (void)salt;
    return NULL;
}
static struct spwd *fault_getspnam(const char *name)
{
    (void)name;
    return NULL;
}
static ssize_t fault_getrandom(void *buf, size_t n, unsigned flags)
{
    (void)buf;
    (void)n;
    (void)flags;
    errno = EIO;
    return -1;
}

#define read        fault_read
#define write       fault_write
#define unlink      fault_unlink
#define fsync       fault_fsync
#define isatty      fault_isatty
#define tcgetpgrp   fault_tcgetpgrp
#define tcgetattr   fault_tcgetattr
#define tcsetattr   fault_tcsetattr
#define sigaction   fault_sigaction
#define sigprocmask fault_sigprocmask
#define crypt       fault_crypt
#define getspnam    fault_getspnam
#define getrandom   fault_getrandom
#undef sigaction
#define sigaction(...) fault_sigaction(__VA_ARGS__)
#include "pwauth.c"
#undef read
#undef write
#undef unlink
#undef fsync
#undef sigaction
#undef sigprocmask
#undef crypt
#undef getspnam
#undef getrandom
#undef isatty
#undef tcgetpgrp
#undef tcgetattr
#undef tcsetattr

/** Deliver a continue at the injected read boundary. */
static void resume_prompt(void)
{
    pwauth_cont_handler(SIGCONT);
}

/** Record a failed assertion. */
static void check(int ok, const char *name)
{
    if (!ok) {
        fprintf(stderr, "pwauthfault-hosttest: fail: %s\n", name);
        Failed++;
    }
}

/** Check the parser over exact file bytes, including embedded NULs. */
static void parse_case(const void *data, size_t len, int want, long pid, const char *name)
{
    FILE *f = tmpfile();
    long value = -1;
    int got;

    if (!f) {
        exit(2);
    }
    if (fwrite(data, 1, len, f) != len || fflush(f) != 0 || fseek(f, 0, SEEK_SET) != 0) {
        exit(2);
    }
    got = cred_parse_pid(fileno(f), &value);
    check(got == want && (got != 1 || value == pid), name);
    fclose(f);
}

/** Check that neither an initial nor a resumed echo failure accepts input. */
static void echo_cases(void)
{
    char password[32];

    Input = (const unsigned char *)"secret\n";
    Input_len = 7;
    Input_pos = 0;
    Echo_fail = 1;
    Read_calls = 0;
    check(pwauth_read_password("", password, sizeof(password)) == -1 && password[0] == 0 &&
              Read_calls == 0,
          "initial echo failure stops input");
    Echo_fail = 0;
    Input_pos = 0;
    Resume_read = 1;
    Read_calls = 0;
    check(pwauth_read_password("", password, sizeof(password)) == -1 && password[0] == 0 &&
              Read_calls == 1,
          "resume echo failure stops input");
    Echo_fail = 0;
    Input_pos = 0;
    check(pwauth_read_password("", password, sizeof(password)) == 0 &&
              strcmp(password, "secret") == 0,
          "working echo permits input");
}

/** Capture a library diagnostic without intercepting stdio internals. */
static FILE *capture(int *saved)
{
    FILE *f = tmpfile();

    fflush(stderr);
    *saved = dup(2);
    if (!f || *saved < 0 || dup2(fileno(f), 2) < 0) {
        exit(2);
    }
    return f;
}
static int captured(FILE *f, int saved, const char *path, const char *detail)
{
    char buf[2048];
    size_t n;

    fflush(stderr);
    if (dup2(saved, 2) < 0) {
        exit(2);
    }
    close(saved);
    rewind(f);
    n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = 0;
    fclose(f);
    return strstr(buf, path) != NULL && strstr(buf, detail) != NULL;
}

/**
 * Require failed rollback to name the remaining file and preserve the target.
 */
static void cleanup_cases(void)
{
    char dir[] = "/tmp/pwauthfault.XXXXXX";
    char lock[128], target[128], temp[160], buf[16];
    int saved, fd, rc, end;
    FILE *log;

    if (!mkdtemp(dir)) {
        exit(2);
    }
    snprintf(lock, sizeof(lock), "%s/lock", dir);
    snprintf(target, sizeof(target), "%s/shadow", dir);
    snprintf(temp, sizeof(temp), "%s.tmp.%d", target, (int)getpid());
    Pid_write_fail = 1;
    Unlink_fail = lock;
    log = capture(&saved);
    rc = cred_txn_begin_at("test", lock, 0);
    check(captured(log, saved, lock, "cannot remove lock"), "lock rollback reports unlink failure");
    check(rc == -1 && access(lock, F_OK) == 0, "failed acquisition leaves reported lock");
    Pid_write_fail = 0;
    Unlink_fail = NULL;
    unlink(lock);
    fd = open(target, O_CREAT | O_RDWR | O_EXCL, 0600);
    if (fd < 0 || write(fd, "old", 3) != 3) {
        exit(2);
    }
    close(fd);
    check(cred_txn_begin_at("test", lock, 0) == 0, "acquire for failed cleanup");
    Flush_fail = 1;
    Unlink_after_flush = temp;
    log = capture(&saved);
    rc = cred_rewrite(target, "new", 3);
    end = cred_txn_end();
    check(captured(log, saved, temp, "cannot remove temporary file"),
          "temporary cleanup reports unlink failure");
    check(rc == CRED_UNCHANGED && end == -1 && access(temp, F_OK) == 0,
          "failed temporary removal is returned");
    fd = open(target, O_RDONLY);
    memset(buf, 0, sizeof(buf));
    check(fd >= 0 && read(fd, buf, sizeof(buf)) == 3 && strcmp(buf, "old") == 0,
          "cleanup failure leaves target unchanged");
    close(fd);
    Flush_fail = 0;
    Unlink_after_flush = NULL;
    Unlink_fail = NULL;
    unlink(temp);
    unlink(target);
    unlink(lock);
    rmdir(dir);
}

/** Run each failure-path group under the host sanitizers. */
int main(void)
{
    char late[96];

    parse_case("123\0junk", 8, 0, 0, "embedded NUL rejected");
    memset(late, ' ', sizeof(late));
    memcpy(late, "123", 3);
    late[95] = 'x';
    parse_case(late, sizeof(late), 0, 0, "suffix beyond first buffer rejected");
    late[95] = '\n';
    parse_case(late, sizeof(late), 1, 123, "long whitespace suffix accepted");
    parse_case("999999999\r\n\t ", 13, 1, 999999999, "nine digits accepted");
    parse_case("1234567890", 10, 0, 0, "ten digits rejected");
    parse_case("0", 1, 0, 0, "zero rejected");
    parse_case(" 123", 4, 0, 0, "leading whitespace rejected");
    parse_case("12 3", 4, 0, 0, "split digits rejected");
    parse_case("", 0, 0, 0, "empty file rejected");
    Read_short = 1;
    parse_case("123\n", 4, 1, 123, "short reads completed");
    Read_short = 0;
    Read_fail = 1;
    parse_case("123", 3, -1, 0, "read error distinguished");
    echo_cases();
    cleanup_cases();
    printf("pwauthfault-hosttest: result: %s\n", Failed ? "fail" : "pass");
    return Failed ? 1 : 0;
}
