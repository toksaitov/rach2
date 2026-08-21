/**
 * Host test for the credential library (utils/pwauth.c).
 *
 * host-deps: utils/pwauth.c vendor/musl/musl-1.2.5/src/crypt/crypt_sha512.c
 *
 * It covers what is pure or can be made pure: the password comparison and its
 * deny cases against the published SHA-crypt vectors, the salt mapping, the
 * replacement idiom inside a temporary directory, and the lock protocol through
 * the `_at` forms, whose lock path and retry budget are parameters so this test
 * neither touches `/etc` nor waits ten seconds to watch an acquire refuse.
 * Owner inheritance is asserted in the guest instead, where root and uid 1000
 * differ; here it would be a tautology.
 *
 * The lock cases all assert the same thing twice over: the refusal, and that
 * the file is still there afterwards with its bytes unchanged. A lock is
 * reported and never broken, so every one of them is also a check that nothing
 * was removed, and the control that makes the refusals mean something is the
 * last case, where removing the file by hand lets the next acquire succeed.
 *
 * The library is a guest source, so three symbols it leans on are supplied
 * here, not by the host: `crypt`, over the vendored musl SHA-512 the guest
 * verifies with; `getspnam`, whose reader path is proved in the guest against a
 * real `/etc/shadow`; and `getrandom`, which no host call can stand in for,
 * since the property that matters is the kernel's refusal to hand out untrusted
 * entropy. Their declarations come from `tests/host/compat`, on the include
 * path of `tools/hosttest.sh` alone.
 *
 * A fourth, `fsync`, is supplied for a different reason: it is the one step of
 * the replacement whose failure cannot be provoked by arranging the file
 * system, and the step that decides whether a caller may say "unchanged". The
 * stand-in fails on a chosen call, which is what lets the two sides of the
 * rename be checked apart: the flush before it is a refusal that changed
 * nothing, the flush after it is a replacement that is already live, and the
 * release's own flush is a lock gone from the file system that may come back
 * with the next boot.
 *
 * The last two are interposed calls, not stand-ins, and both read the
 * signal mask from inside a stretch the library blocks it across, since
 * restoring a mask and never taking one look the same from outside a function.
 * `snprintf` forwards to `vsnprintf` and is the only call inside either window
 * where the library publishes state a death handler acts on; `getpid` goes to
 * the raw system call and is the only one inside the release registered with
 * `atexit`, which runs after the last case here and can be watched no other
 * way. The mask is a property of the thread, so reading it at a call reads it
 * for the statement.
 *
 * Returns the failed-check count (0 == pass).
 */

#define _GNU_SOURCE /* mkdtemp, utimensat, posix_openpt */
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <termios.h>
#include <time.h>
#include <crypt.h>
#include <shadow.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>

#include "pwauth.h"

/* The published SHA-crypt vector, the one mkhash_hosttest mints and the guest
 * verifies against its committed /etc/shadow line. */
#define VECTOR_PASSWORD "Hello world!"
#define VECTOR_HASH                                      \
    "$6$saltstring$svn8UoSVapNtMuq1ukKS4tPQd8iKwSMHWjl/" \
    "O817G3uBnIFNjnQJuesI68u4OTLiBFdcbYEdFCoEOfaS35inz1"

/* The salt alphabet, repeated here, not reached for inside the library:
 * a mapping tested against its own table would agree with itself. */
#define SALT_ALPHABET "./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz"

/* The replaced file's base name, and what every temporary of it begins with. A
 * temporary has the writer's pid, so "is the directory clean" is a question
 * about a prefix and not about one name. */
#define TARGET_BASE "shadow"
#define TEMP_PREFIX TARGET_BASE ".tmp"

char *__crypt_sha512(const char *key, const char *setting, char *output);

static int Failed;
static int Saved_stderr = -1;
static int Pipe_fd[2] = {-1, -1};
static int Pipe_saved = -1;
static char Dir[128];
static char Target[192];
static char Target_tmp[224];
static char Lock[192];
static char Log[192];
static char Self[512];

/* The arguments this program runs itself under. The first reports one
 * inherited signal disposition from the far side of an exec; the second reports
 * the signal mask from after the registered release has run, which is a moment
 * no case inside this process can reach. */
#define HELPER_PIPE "report-sigpipe"
#define HELPER_EXIT "report-exit-mask"
#define HELPER_KEEP "report-pipe-kept"
#define HELPER_HEIR "report-heir-teardown"
#define HELPER_TTY  "report-restore-count"
#define HELPER_BG   "report-background-prompt"

/**
 * Record one failed check, naming the case that failed.
 *
 * Every case names itself, so a run that fails says which contract broke rather
 * than only how many did.
 */
static void fail(const char *what)
{
    fprintf(stderr, "pwauth_hosttest: %s\n", what);
    Failed++;
}

/**
 * True when all four of a transaction's death signals are blocked right now, 0
 * when any is deliverable, -1 when the mask cannot be read.
 */
static int deaths_blocked(void)
{
    static const int deaths[] = {SIGINT, SIGTERM, SIGHUP, SIGQUIT};
    sigset_t current;
    size_t i;

    if (sigprocmask(SIG_BLOCK, NULL, &current) != 0) {
        return -1;
    }
    for (i = 0; i < sizeof(deaths) / sizeof(deaths[0]); i++) {
        if (!sigismember(&current, deaths[i])) {
            return 0;
        }
    }

    return 1;
}

/**
 * The host's `crypt(3)`: the vendored musl SHA-512 the guest verifies with,
 * over a static buffer as `crypt` returns one.
 */
char *crypt(const char *key, const char *setting)
{
    static char output[256];

    return __crypt_sha512(key, setting, output);
}

/**
 * The host's `getspnam`: no shadow database exists here, so every account is
 * absent.
 *
 * That is enough to check the one piece of `pwauth_verify` that is the
 * library's own, that a failed lookup denies, not falls through to the
 * comparison. Resolving a real account is the guest's assertion.
 */
struct spwd *getspnam(const char *name)
{
    (void)name;

    return NULL;
}

/**
 * The host's `getrandom(2)`: a refusal, matching the kernel's answer on a CPU
 * that exposes neither RDSEED nor RDRAND.
 *
 * The draw is deliberately not exercised from here. A positive result would be
 * this stub agreeing with itself, and the fail-closed half needs a kernel that
 * really has no trusted entropy, which is what `make test-noentropy` boots.
 */
ssize_t getrandom(void *buffer, size_t length, unsigned int flags)
{
    (void)buffer;
    (void)length;
    (void)flags;
    errno = EIO;

    return -1;
}

/**
 * The host's `fsync(2)`: a success, or `-EIO` on the call the case under way
 * asked to fail.
 *
 * Nothing here needs a real flush, since every file this test writes lives in a
 * temporary directory it removes. What it needs is a writeback error on demand:
 * `Fsync_fail_at` counts from the start of each case, so in a replacement it is
 * 1 for the flush before the rename and 2 for the flush after it, and in a
 * transaction that ends without one it is 1 for the release's own flush, which
 * is then the only flush the writer makes.
 *
 * It is also the one moment from which a replacement's temporary file can be
 * seen. The first flush is the last step before the rename, so the file is on
 * disk under the name the library chose and gone a few lines later; with
 * `Temp_watch` set, the name is recorded for the case that asks whose it is.
 *
 * And in a release it is the one call inside the window between the unlink and
 * the flush, so with `Fsync_mask_watch` set it reads the signal mask there, the
 * way the interposed `snprintf` reads it in the other two windows. Restoring a
 * mask and never taking it are indistinguishable from outside a function; this
 * is what tells them apart.
 */
static int Fsync_fail_at;
static int Fsync_calls;
static int Temp_watch;
static char Temp_seen[128];
static int Fsync_mask_watch;
static int Fsync_mask_obs;

int fsync(int fd)
{
    (void)fd;
    Fsync_calls++;
    if (Fsync_mask_watch) {
        Fsync_mask_obs = deaths_blocked();
    }
    if (Temp_watch && Fsync_calls == 1) {
        DIR *dir = opendir(Dir);
        struct dirent *entry;

        Temp_seen[0] = '\0';
        if (dir != NULL) {
            while ((entry = readdir(dir)) != NULL) {
                if (strncmp(entry->d_name, TEMP_PREFIX, sizeof(TEMP_PREFIX) - 1) == 0) {
                    snprintf(Temp_seen, sizeof(Temp_seen), "%s", entry->d_name);
                }
            }
            closedir(dir);
        }
    }
    if (Fsync_fail_at != 0 && Fsync_calls == Fsync_fail_at) {
        errno = EIO;

        return -1;
    }

    return 0;
}

/**
 * The number of the first signal whose blocked state differs between `before`
 * and the mask in force now, 0 when every one of them agrees, and -1 when the
 * mask cannot be read.
 *
 * The whole mask and not just the four, because "the caller's mask came back"
 * is the claim being checked and a test of the four alone would pass a library
 * that put three of them back and kept the fourth, or that blocked something it
 * was never asked to. It is also the difference between a restore and a mask
 * that was never taken, which is why the interposed calls above read the mask
 * from inside each window, not only around it.
 */
#ifndef NSIG
#define NSIG 32
#endif

static int mask_changed(const sigset_t *before)
{
    sigset_t now;
    int sig;

    if (sigprocmask(SIG_BLOCK, NULL, &now) != 0) {
        return -1;
    }
    for (sig = 1; sig < NSIG; sig++) {
        if (!sigismember(before, sig) != !sigismember(&now, sig)) {
            return sig;
        }
    }

    return 0;
}

/**
 * The host's `tcsetattr(3)`, and the interposed call that delivers a death
 * inside a restore.
 *
 * `pwauth_tty_restore` puts the saved modes back and clears the flag that says
 * it must. A handler runs with only its own signal blocked, so a second death
 * can land between those two statements, and it enters the restore again on its
 * way out; whether the terminal ends up usable depends entirely on which of the
 * two comes first. That window is a couple of instructions wide and cannot be
 * hit by sending signals from outside, so it is entered from within: armed with
 * a call number, this raises one from inside that call. The number matters,
 * since the prompt's own echo-off is a `tcsetattr` too; the restore is the one
 * after it. The signal raised is deliberately not the one the case sends: a
 * handler runs with its own signal blocked, so raising that one from inside it
 * would only mark it pending and no second handler would run at all.
 *
 * It forwards, through the `ioctl` the libc's own `tcsetattr` is a wrapper for,
 * because two other cases here turn echo off over a pseudo-terminal and read it
 * back: a stand-in that quietly did nothing would leave the terminal untouched
 * and both of them passing for the wrong reason. What this case asks is how
 * many restores reached this point, which is one if the second death skipped
 * the work and two if it did it.
 */
static volatile sig_atomic_t Tcs_raise_at;
static volatile sig_atomic_t Tcs_raise_sig = SIGHUP;
static volatile sig_atomic_t Tcs_calls;

int tcsetattr(int fd, int actions, const struct termios *modes)
{
/* unsigned long, not int: these request numbers carry the high bit, and an int
 * array would sign-extend them into a request no driver answers, so every
 * forwarded call would fail and the interposition would quietly stop
 * working. */
#ifdef TIOCSETA
    static const unsigned long how[] = {TIOCSETA, TIOCSETAW, TIOCSETAF};
#else
    static const unsigned long how[] = {TCSETS, TCSETSW, TCSETSF};
#endif

    int rc;

    Tcs_calls++;
    if (actions < 0 || actions > 2) {
        errno = EINVAL;

        return -1;
    }
    rc = ioctl(fd, how[actions], modes);
    /* After the modes are in force, not before. The window being entered is the
     * one between a restore taking effect and the flag that records it being
     * cleared; a signal raised ahead of the `ioctl` would run while the
     * terminal was still quiet, which is a different moment and not the one at
     * issue. */
    if (Tcs_raise_at != 0 && Tcs_calls == Tcs_raise_at) {
        raise(Tcs_raise_sig);
    }

    return rc;
}

/**
 * The host's `getpid(2)`, and the interposed call that reaches inside the
 * registered release.
 *
 * That release blocks the four deaths for its own length so a death cannot call
 * `exit` from within `exit`, and it runs after the last case in this process,
 * so nothing here can watch it from outside. The first thing it does is ask
 * whether this process holds the transaction, which is a `getpid`, and that is
 * the only call it makes before the release's own mask takes over. Arming this
 * just before leaving `main` therefore reads the mask at the one instant that
 * says whether the teardown took it.
 *
 * The real answer comes from the raw system call, which is what musl's own
 * `getpid` is; going through the libc entry point here would be this function
 * calling itself. It has no sanitizer instrumentation, because the
 * library's own death handler reaches `getpid` through `cred_txn_mine`, and a
 * function a signal handler reaches must not enter the sanitizer runtime to
 * allocate a shadow frame on its way to a system call. Everything it shares
 * with that context is `sig_atomic_t` for the same reason.
 */
static volatile sig_atomic_t Getpid_watch;
static volatile sig_atomic_t Getpid_mask_obs = -2;
static volatile sig_atomic_t Getpid_raise;
static char Getpid_raise_when[192];

/* cppcheck cannot evaluate __has_attribute, so the support test is by compiler,
 * not by feature; an unrecognized compiler simply keeps the
 * instrumentation, which costs correctness nothing here. */
#if defined(__clang__) || (defined(__GNUC__) && __GNUC__ >= 8)
#define INTERPOSED_UNINSTRUMENTED __attribute__((no_sanitize("address", "undefined")))
#else
#define INTERPOSED_UNINSTRUMENTED
#endif

INTERPOSED_UNINSTRUMENTED
pid_t getpid(void)
{
    pid_t self = (pid_t)syscall(SYS_getpid);

    if (Getpid_watch) {
        Getpid_watch = 0;
        Getpid_mask_obs = deaths_blocked();
    }
    /* Armed only by the case that needs a signal delivered inside the acquire.
     * The named file is the lock, so the raise happens on the first call after
     * the exclusive create has succeeded, which is exactly the window in which
     * SIGPIPE is still fatal and a lock already exists to be stranded. */
    if (Getpid_raise && access(Getpid_raise_when, F_OK) == 0) {
        Getpid_raise = 0;
        raise(SIGPIPE);
    }

    return self;
}

/**
 * The host's `snprintf(3)`, and the interposed call that watches the signal
 * mask.
 * The library publishes two pieces of state a death handler then acts on: the
 * lock file's path and the temporary file's name. Neither is one store, so both
 * are written with the four deaths blocked, and this is where that is checked.
 * A formatting call is the only thing inside either window that a test can
 * interpose, and it is enough: the mask is a property of the thread, so reading
 * it here reads it for the statement.
 *
 * `Mask_match` arms the interposed call on the calls that produce one exact
 * string, since the replacement formats the same temporary name twice, once
 * into a local before the mask is taken and once into the state the handler
 * reads. Recording both in order is what makes a blocked reading mean
 * something: the first must come back deliverable and the second blocked, so an
 * interposed call that had stopped looking would fail the case rather than
 * pass it.
 */
#define MASK_OBS_MAX 4
static const char *Mask_match;
static int Mask_obs[MASK_OBS_MAX];
static int Mask_obs_n;

int snprintf(char *buffer, size_t size, const char *format, ...)
{
    va_list args;
    int n;

    va_start(args, format);
    n = vsnprintf(buffer, size, format, args);
    va_end(args);
    if (Mask_match != NULL && buffer != NULL && n > 0 && Mask_obs_n < MASK_OBS_MAX &&
        strcmp(buffer, Mask_match) == 0) {
        Mask_obs[Mask_obs_n++] = deaths_blocked();
    }

    return n;
}

/**
 * Read at most `size - 1` bytes of `path` into `buffer`, terminating it.
 * Returns the byte count, or -1 if the file could not be read.
 */
static ssize_t slurp(const char *path, char *buffer, size_t size)
{
    ssize_t n;
    int fd = open(path, O_RDONLY);

    if (fd < 0) {
        return -1;
    }
    n = read(fd, buffer, size - 1);
    close(fd);
    if (n < 0) {
        return -1;
    }
    buffer[n] = '\0';

    return n;
}

/**
 * Write `text` to `path` at mode `mode`, replacing whatever was there. Returns
 * 0 on success and -1 otherwise.
 */
static int spill(const char *path, const char *text, mode_t mode)
{
    size_t len = strlen(text);
    ssize_t n = 0;
    int fd;

    unlink(path);
    fd = open(path, O_CREAT | O_WRONLY, mode);
    if (fd < 0) {
        return -1;
    }
    if (len > 0) {
        n = write(fd, text, len);
    }
    close(fd);
    chmod(path, mode);

    return (n == (ssize_t)len) ? 0 : -1;
}

/** True if `path` names something that exists. */
static int present(const char *path)
{
    struct stat st;

    return stat(path, &st) == 0;
}

/**
 * How many temporary files of the replaced file's the directory still has.
 *
 * The name a replacement writes through has the writer's pid, so a case
 * that
 * checked one fixed name and called the directory clean would miss a temporary
 * left under any other. This counts every name that begins the way one of them
 * does, which is also what proves the library never writes through the shared
 * name a case may have planted a directory at.
 */
static int temps_left(void)
{
    DIR *dir = opendir(Dir);
    struct dirent *entry;
    int n = 0;

    if (dir == NULL) {
        fail("could not read the directory the temporary files go in");

        return 0;
    }
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, TEMP_PREFIX, sizeof(TEMP_PREFIX) - 1) == 0) {
            n++;
        }
    }
    closedir(dir);

    return n;
}

/**
 * Push the file's modification time back by `seconds`.
 *
 * The backdated cases exist because an age bound was the rule this phase
 * deleted: a lock an hour old must be refused exactly as a fresh one is.
 */
static void backdate(const char *path, long seconds)
{
    struct timespec ts[2];
    time_t now = time(NULL);

    ts[0].tv_sec = now - seconds;
    ts[0].tv_nsec = 0;
    ts[1] = ts[0];
    if (utimensat(AT_FDCWD, path, ts, 0) != 0) {
        fail("utimensat could not backdate a lock file");
    }
}

/** Redirect file descriptor 2 into `Log`, keeping the old one to put back. */
static void capture_start(void)
{
    int fd;

    fflush(stderr);
    Saved_stderr = dup(2);
    fd = open(Log, O_CREAT | O_TRUNC | O_WRONLY, 0600);
    if (fd >= 0) {
        dup2(fd, 2);
        close(fd);
    }
}

/** Restore file descriptor 2 and read what the library printed into `buffer`.
 * */
static void capture_stop(char *buffer, size_t size)
{
    fflush(stderr);
    if (Saved_stderr >= 0) {
        dup2(Saved_stderr, 2);
        close(Saved_stderr);
        Saved_stderr = -1;
    }
    if (slurp(Log, buffer, size) < 0) {
        buffer[0] = '\0';
    }
}

/**
 * Redirect file descriptor 2 into a pipe, keeping the old one to put back.
 *
 * The file capture cannot serve the one case that shrinks `RLIMIT_FSIZE`, since
 * the limit falls on the log as hard as on the lock file and would leave the
 * report empty. A pipe is not a regular file, so the limit does not reach it.
 */
static void pipe_capture_start(void)
{
    fflush(stderr);
    if (pipe(Pipe_fd) != 0) {
        fail("could not make the pipe a capture needs");

        return;
    }
    Pipe_saved = dup(2);
    dup2(Pipe_fd[1], 2);
    close(Pipe_fd[1]);
    Pipe_fd[1] = -1;
}

/** Restore file descriptor 2 and read what the library printed into `buffer`.
 * */
static void pipe_capture_stop(char *buffer, size_t size)
{
    ssize_t n;

    fflush(stderr);
    if (Pipe_saved >= 0) {
        dup2(Pipe_saved, 2);
        close(Pipe_saved);
        Pipe_saved = -1;
    }
    buffer[0] = '\0';
    if (Pipe_fd[0] >= 0) {
        n = read(Pipe_fd[0], buffer, size - 1);
        buffer[n > 0 ? n : 0] = '\0';
        close(Pipe_fd[0]);
        Pipe_fd[0] = -1;
    }
}

/** Sleep `ms` milliseconds, this test's one way of letting time pass. */
static void nap(long ms)
{
    struct timespec ts;

    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;
    nanosleep(&ts, NULL);
}

/**
 * Wait up to `ms` milliseconds for `pid`, filling `*status`. Returns 1 when it
 * was reaped, 0 on a timeout, having killed it.
 *
 * The cases below hand a child a descriptor it is meant to block on, so a
 * blocking wait would turn a regression into a hung gate, not a failed
 * one.
 */
static int wait_bounded(pid_t pid, int *status, long ms)
{
    long waited = 0;

    while (waited < ms) {
        pid_t got = waitpid(pid, status, WNOHANG);

        if (got == pid) {
            return 1;
        }
        if (got < 0) {
            return 0;
        }
        nap(50);
        waited += 50;
    }
    kill(pid, SIGKILL);
    waitpid(pid, status, 0);

    return 0;
}

/**
 * Read from `fd` until what has arrived has `needle` or `ms` milliseconds
 * pass. Returns 1 when the text arrived.
 *
 * `fd` must be non-blocking: a prompt that never comes has to fail the case,
 * not stop the run.
 */
static int read_until(int fd, const char *needle, long ms)
{
    char buffer[512];
    size_t filled = 0;
    long waited = 0;

    buffer[0] = '\0';
    while (waited < ms) {
        ssize_t n = read(fd, buffer + filled, sizeof(buffer) - 1 - filled);

        if (n > 0) {
            filled += (size_t)n;
            buffer[filled] = '\0';
            if (strstr(buffer, needle) != NULL) {
                return 1;
            }
            if (filled + 1 >= sizeof(buffer)) {
                return 0;
            }
            continue;
        }
        if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
            return 0;
        }
        nap(50);
        waited += 50;
    }

    return 0;
}

/**
 * Open a pseudo-terminal and write its slave's name into `name`. Returns the
 * master, or -1.
 *
 * The prompt cases need a real terminal, since everything they are about is
 * `tcsetattr` and the process group that owns it, and a pipe on file descriptor
 * 0 would take the library's other path entirely.
 */
static int pty_make(char *name, size_t size)
{
    const char *slave;
    int master = posix_openpt(O_RDWR | O_NOCTTY);

    if (master < 0) {
        return -1;
    }
    if (grantpt(master) != 0 || unlockpt(master) != 0) {
        close(master);

        return -1;
    }
    slave = ptsname(master);
    if (slave == NULL) {
        close(master);

        return -1;
    }
    snprintf(name, size, "%s", slave);

    return master;
}

/** How many times `needle` occurs in `haystack`, without overlaps. */
static int occurrences(const char *haystack, const char *needle)
{
    const char *p = haystack;
    int n = 0;

    while ((p = strstr(p, needle)) != NULL) {
        n++;
        p += strlen(needle);
    }

    return n;
}

/**
 * The pure comparison and its deny cases.
 *
 * The published vector proves the algorithm; the deny cases prove the one rule
 * that matters more, that a field which is not a `crypt(3)` setting denies
 * everyone. A locked `!`, a `*`, an empty field and a missing entry must all
 * refuse alike, since a login server reports them as one refusal.
 */
static void check_compare(void)
{
    if (!pwauth_check(VECTOR_HASH, VECTOR_PASSWORD)) {
        fail("pwauth_check rejects the password its stored hash was minted from");
    }
    if (pwauth_check(VECTOR_HASH, "hello world!")) {
        fail("pwauth_check accepts a wrong password");
    }
    if (pwauth_check(VECTOR_HASH, "")) {
        fail("pwauth_check accepts an empty password against a real hash");
    }
    if (pwauth_check(NULL, VECTOR_PASSWORD)) {
        fail("pwauth_check accepts a NULL stored field");
    }
    if (pwauth_check(VECTOR_HASH, NULL)) {
        fail("pwauth_check accepts a NULL password");
    }
    if (pwauth_check("", "")) {
        fail("pwauth_check accepts an empty stored field");
    }
    if (pwauth_check("!", "!")) {
        fail("pwauth_check accepts a locked account");
    }
    if (pwauth_check("!" VECTOR_HASH, VECTOR_PASSWORD)) {
        fail("pwauth_check accepts a hash locked with a leading !");
    }
    if (pwauth_check("*", "*")) {
        fail("pwauth_check accepts a starred account");
    }
    if (pwauth_check("x", "x")) {
        fail("pwauth_check accepts a field that is not a crypt setting");
    }
    if (pwauth_check("$1$notsha", "anything")) {
        fail("pwauth_check accepts a setting crypt itself rejects");
    }
    if (pwauth_verify("nobody", VECTOR_PASSWORD)) {
        fail("pwauth_verify accepts an account the database does not hold");
    }
}

/**
 * The salt mapping: the alphabet, the length, and dependence on the bytes
 * alone.
 *
 * Every byte value is mapped, so a table indexed past its end or a sign
 * extension shows up here, not as a salt the guest cannot reproduce.
 * Two bytes that differ only above the low six bits must map alike, which is
 * what "the low six bits and nothing else" means.
 */
static void check_salt_map(void)
{
    unsigned char bytes[256];
    char out[PWAUTH_SALT_LEN + 1];
    char high[PWAUTH_SALT_LEN + 1];
    char full[257];
    size_t i;

    for (i = 0; i < sizeof(bytes); i++) {
        bytes[i] = (unsigned char)i;
    }
    memset(full, 'Z', sizeof(full));
    pwauth_salt_map(bytes, sizeof(bytes), full);
    if (full[256] != '\0') {
        fail("pwauth_salt_map does not terminate its output");
    }
    for (i = 0; i < 256; i++) {
        if (strchr(SALT_ALPHABET, full[i]) == NULL || full[i] == '\0') {
            fail("pwauth_salt_map produced a character outside the alphabet");
            break;
        }
    }
    for (i = 0; i < 64; i++) {
        if (full[i] != SALT_ALPHABET[i]) {
            fail("pwauth_salt_map does not follow the crypt alphabet in order");
            break;
        }
        if (full[i] != full[i + 64] || full[i] != full[i + 128] || full[i] != full[i + 192]) {
            fail("pwauth_salt_map reads more than the low six bits of a byte");
            break;
        }
    }

    memset(out, 'Z', sizeof(out));
    pwauth_salt_map(bytes, PWAUTH_SALT_LEN, out);
    if (strlen(out) != PWAUTH_SALT_LEN) {
        fail("pwauth_salt_map wrote a salt of the wrong length");
    }
    memset(high, 'Z', sizeof(high));
    for (i = 0; i < PWAUTH_SALT_LEN; i++) {
        bytes[i] = (unsigned char)(i + 64);
    }
    pwauth_salt_map(bytes, PWAUTH_SALT_LEN, high);
    if (strcmp(out, high) != 0) {
        fail("pwauth_salt_map is not a function of the low six bits alone");
    }
}

/**
 * The replacement idiom, under a transaction, in a directory of this test's
 * own.
 *
 * The cases divide on the rename, which is what the three returns divide on.
 * Before it every failure removes the temporary file and leaves the target byte
 * for byte as it was, so each failing case checks the target as well as the
 * return; after it the replacement is live, and a flush that fails there is
 * reported as published, not as unchanged. The refusal with no
 * transaction held is first, because a caller that forgot the lock must not be
 * able to publish.
 */
static void check_rewrite(void)
{
    char content[256];
    char report[512];
    char absent[224];
    char blocked[224];
    char blocked_tmp[256];
    struct stat st;
    int rc;

    if (spill(Target, "root:x:0:0:root:/root:/bin/sh\n", 0600) != 0) {
        fail("could not create the file the rewrite cases replace");
        return;
    }

    /* The capture window has the library call and nothing else: a `fail`
     * inside it would be written into the file the report is read from. */
    capture_start();
    rc = cred_rewrite(Target, "usurped\n", 8);
    capture_stop(report, sizeof(report));
    if (rc == 0) {
        fail("cred_rewrite published with no transaction held");
    }
    if (strstr(report, "no transaction held") == NULL) {
        fail("cred_rewrite refused without a transaction but did not say so");
    }
    if (slurp(Target, content, sizeof(content)) < 0 ||
        strcmp(content, "root:x:0:0:root:/root:/bin/sh\n") != 0) {
        fail("cred_rewrite touched the target with no transaction held");
    }

    if (cred_txn_begin_at("pwauth_hosttest", Lock, 0) != 0) {
        fail("could not take the transaction the rewrite cases run under");
        return;
    }

    if (cred_rewrite(Target, "one\ntwo\n", 8) != 0) {
        fail("cred_rewrite refused a plain replacement");
    }
    if (slurp(Target, content, sizeof(content)) < 0 || strcmp(content, "one\ntwo\n") != 0) {
        fail("cred_rewrite left the target holding something else");
    }
    if (temps_left() != 0) {
        fail("cred_rewrite left its temporary file behind");
    }

    if (chmod(Target, 0640) != 0) {
        fail("could not set the mode the replacement must inherit");
    }
    if (cred_rewrite(Target, "three\n", 6) != 0) {
        fail("cred_rewrite refused the replacement whose mode must be inherited");
    }
    if (stat(Target, &st) != 0 || (st.st_mode & 07777) != 0640) {
        fail("cred_rewrite did not take the mode from the file it replaced");
    }

    if (spill(Target_tmp, "a temporary no live writer can own\n", 0600) != 0) {
        fail("could not plant the stale temporary file");
    }
    if (cred_rewrite(Target, "four\n", 5) != 0) {
        fail("cred_rewrite refused instead of removing a stale temporary file");
    }
    if (slurp(Target, content, sizeof(content)) < 0 || strcmp(content, "four\n") != 0) {
        fail("cred_rewrite published the stale temporary file's content");
    }
    if (temps_left() != 0) {
        fail("cred_rewrite left the stale temporary file behind");
    }

    /* An uncreatable temporary, made so by a directory standing at its name:
     * the unlink cannot remove it and the exclusive create then finds it there,
     * which is the EEXIST the library reports, not retries. */
    if (mkdir(Target_tmp, 0700) != 0) {
        fail("could not stand a directory where the temporary file goes");
    }
    capture_start();
    rc = cred_rewrite(Target, "five\n", 5);
    capture_stop(report, sizeof(report));
    if (rc == 0) {
        fail("cred_rewrite reported success with no temporary file to write");
    }
    if (strstr(report, "cannot create the replacement") == NULL) {
        fail("cred_rewrite failed to create its temporary file and did not say so");
    }
    if (slurp(Target, content, sizeof(content)) < 0 || strcmp(content, "four\n") != 0) {
        fail("cred_rewrite changed the target after failing before the rename");
    }
    rmdir(Target_tmp);

    snprintf(absent, sizeof(absent), "%s/absent", Dir);
    capture_start();
    rc = cred_rewrite(absent, "six\n", 4);
    capture_stop(report, sizeof(report));
    if (rc == 0) {
        fail("cred_rewrite invented a mode and owner for a file that does not exist");
    }
    if (strstr(report, "cannot read the file being replaced") == NULL) {
        fail("cred_rewrite refused a missing target but did not say why");
    }
    if (present(absent)) {
        fail("cred_rewrite created the target it said it could not read");
    }

    /* A rename that cannot happen, made so by a directory standing at the
     * destination. It is the one failure that reaches the abort path with a
     * complete temporary file in hand, so it is what proves the abort is an
     * abort: a body that reported success and left the temporary file where it
     * was would pass every other case here. */
    snprintf(blocked, sizeof(blocked), "%s/blocked", Dir);
    snprintf(blocked_tmp, sizeof(blocked_tmp), "%s.tmp.%d", blocked, (int)getpid());
    if (mkdir(blocked, 0700) != 0) {
        fail("could not stand a directory where a rename must fail");
    }
    capture_start();
    rc = cred_rewrite(blocked, "seven\n", 6);
    capture_stop(report, sizeof(report));
    if (rc != CRED_UNCHANGED) {
        fail("cred_rewrite reported a rename that failed as a replacement");
    }
    if (strstr(report, "cannot rename the replacement into place") == NULL) {
        fail("cred_rewrite could not rename its temporary file and did not say so");
    }
    if (stat(blocked, &st) != 0 || !S_ISDIR(st.st_mode)) {
        fail("a failed rename did not leave the destination as it was");
    }
    if (present(blocked_tmp)) {
        fail("a failed rename left its temporary file behind");
    }
    rmdir(blocked);

    /* The two flushes, told apart by which one fails. The first is a refusal
     * that changed nothing; the second is a replacement that is already live,
     * and the caller must not be able to report it as unchanged. */
    Fsync_calls = 0;
    Fsync_fail_at = 1;
    capture_start();
    rc = cred_rewrite(Target, "eight\n", 6);
    capture_stop(report, sizeof(report));
    Fsync_fail_at = 0;
    if (rc != CRED_UNCHANGED) {
        fail("a replacement whose first flush failed did not report the target unchanged");
    }
    if (strstr(report, "cannot flush the replacement") == NULL) {
        fail("a replacement whose first flush failed did not say so");
    }
    if (slurp(Target, content, sizeof(content)) < 0 || strcmp(content, "four\n") != 0) {
        fail("a replacement whose first flush failed changed the target");
    }
    if (temps_left() != 0) {
        fail("a replacement whose first flush failed left its temporary file behind");
    }

    Fsync_calls = 0;
    Fsync_fail_at = 2;
    capture_start();
    rc = cred_rewrite(Target, "nine\n", 5);
    capture_stop(report, sizeof(report));
    Fsync_fail_at = 0;
    if (rc != CRED_UNFLUSHED) {
        fail("a replacement published but not flushed was not reported as published");
    }
    if (strstr(report, "published but not flushed") == NULL) {
        fail("a replacement published but not flushed did not say so");
    }
    if (slurp(Target, content, sizeof(content)) < 0 || strcmp(content, "nine\n") != 0) {
        fail("a replacement reported as published but not flushed did not publish");
    }
    if (temps_left() != 0) {
        fail("a replacement published but not flushed left its temporary file behind");
    }

    if (cred_txn_end() != 0) {
        fail("cred_txn_end refused the transaction the rewrite cases held");
    }
    if (present(Lock)) {
        fail("cred_txn_end left the lock file behind");
    }
}

/**
 * What a `fork` under a held transaction does, which is nothing.
 *
 * The state is file-scope, so a child inherits it while holding nothing at all:
 * the lock file names its parent. Both ordinary ways out of that child ran the
 * release before the owner was recorded, and one keystroke reaches them, since
 * `^C` at a terminal is delivered to every process in the foreground group. So
 * the cases are the two ways out and the three entry points: a child that exits
 * and a child that is killed must leave the lock where it is, and its own
 * `cred_txn_end`, `cred_rewrite` and `cred_txn_begin_at` must all decline to
 * act on a transaction that is not its.
 */
static void check_fork(void)
{
    char content[256];
    char report[512];
    char before[64];
    char after[64];
    pid_t child;
    ssize_t n;
    int status;

    unlink(Lock);
    if (spill(Target, "before the fork\n", 0600) != 0) {
        fail("could not create the file the fork cases guard");

        return;
    }
    if (cred_txn_begin_at("pwauth_hosttest", Lock, 0) != 0) {
        fail("could not take the transaction the fork cases run under");

        return;
    }
    n = slurp(Lock, before, sizeof(before));

    child = fork();
    if (child == 0) {
        exit(0); /* the release registered with atexit runs here */
    }
    if (child < 0 || waitpid(child, &status, 0) != child) {
        fail("could not fork the child that exits under its parent's transaction");
    }
    if (!present(Lock)) {
        fail("a child's exit removed its parent's lock");
        cred_txn_end();

        return;
    }

    child = fork();
    if (child == 0) {
        for (;;) {
            pause(); /* the death handler the transaction installed is inherited */
        }
    }
    if (child < 0) {
        fail("could not fork the child that a signal kills under its parent's transaction");
    } else {
        kill(child, SIGTERM);
        if (waitpid(child, &status, 0) != child) {
            fail("could not reap the child a signal killed");
        }
        if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM) {
            fail("a child under its parent's transaction did not die of the signal it was sent");
        }
        if (!present(Lock)) {
            fail("a child's death removed its parent's lock");
        }
    }

    capture_start();
    child = fork();
    if (child == 0) {
        int wrong = 0;

        if (cred_txn_end() == 0) {
            wrong |= 1;
        }
        if (cred_rewrite(Target, "usurped\n", 8) != CRED_UNCHANGED) {
            wrong |= 2;
        }
        if (cred_txn_begin_at("pwauth_hosttest", Lock, 0) == 0) {
            wrong |= 4;
        }
        _exit(wrong);
    }
    if (child < 0 || waitpid(child, &status, 0) != child) {
        fail("could not fork the child that tries the transaction it inherited");
    }
    capture_stop(report, sizeof(report));
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail("a child acted on a transaction it does not hold");
    }
    if (strstr(report, "no transaction held") == NULL) {
        fail("a child's replacement was refused without saying the transaction is not its");
    }
    if (strstr(report, "credential database busy") == NULL) {
        fail("a child's acquire was not refused by the ordinary contention path");
    }
    if (strstr(report, "already locked by this program") != NULL) {
        fail("a child was told it holds the lock its parent holds");
    }
    if (slurp(Target, content, sizeof(content)) < 0 || strcmp(content, "before the fork\n") != 0) {
        fail("a child replaced the file its parent's transaction protects");
    }
    if (slurp(Lock, after, sizeof(after)) != n || memcmp(before, after, (size_t)n) != 0) {
        fail("a child rewrote the lock its parent holds");
    }

    if (cred_txn_end() != 0) {
        fail("cred_txn_end refused the transaction the fork cases held");
    }
    if (present(Lock)) {
        fail("cred_txn_end left the lock file behind");
    }
}

/**
 * What a release and a publish do about a lock that is no longer this
 * transaction's, and what an acquire does when it cannot record the holder.
 *
 * A name is not proof of a file: root removes a lock the library reported as
 * stale, the next writer creates its own at the same name, and an unconditional
 * unlink at the end of the first transaction then takes that live writer's
 * lock, and the writer after it, indefinitely. There is nothing to compare but
 * the recorded pid, since two files that carried one name in turn share an
 * inode number on the guest's file system. The last case is the state that
 * starts the cascade: a lock held with no pid written into it is what every
 * later acquire reports as stale and tells an operator to remove.
 */
static void check_lock_ownership(void)
{
    char content[256];
    char report[512];
    char before[64];
    char after[64];
    char other[64];
    struct rlimit limit, saved_limit;
    ssize_t n;
    int rc;

    unlink(Lock);
    if (spill(Target, "the file a lost lock protects\n", 0600) != 0) {
        fail("could not create the file the lost-lock cases guard");

        return;
    }
    if (cred_txn_begin_at("pwauth_hosttest", Lock, 0) != 0) {
        fail("could not take the transaction the lost-lock cases run under");

        return;
    }
    snprintf(other, sizeof(other), "%d\n", (int)getpid() + 1);
    unlink(Lock);
    if (spill(Lock, other, 0600) != 0) {
        fail("could not stand another writer's lock at the name");
    }
    n = slurp(Lock, before, sizeof(before));

    capture_start();
    rc = cred_rewrite(Target, "usurped\n", 8);
    capture_stop(report, sizeof(report));
    if (rc != CRED_UNCHANGED) {
        fail("a replacement published with the transaction's lock gone");
    }
    if (strstr(report, "the transaction's lock is gone") == NULL) {
        fail("a replacement refused for a lost lock did not say so");
    }
    if (slurp(Target, content, sizeof(content)) < 0 ||
        strcmp(content, "the file a lost lock protects\n") != 0) {
        fail("a replacement with the transaction's lock gone changed the target");
    }
    if (temps_left() != 0) {
        fail("a replacement refused for a lost lock left its temporary file behind");
    }

    if (cred_txn_end() != 0) {
        fail("cred_txn_end refused the transaction the lost-lock cases held");
    }
    if (!present(Lock)) {
        fail("a release removed a lock another writer created at the same name");
    } else if (slurp(Lock, after, sizeof(after)) != n || memcmp(before, after, (size_t)n) != 0) {
        fail("a release rewrote another writer's lock");
    }
    unlink(Lock);

    signal(SIGXFSZ, SIG_IGN);
    limit.rlim_cur = 0;
    limit.rlim_max = RLIM_INFINITY;
    if (getrlimit(RLIMIT_FSIZE, &saved_limit) != 0 || setrlimit(RLIMIT_FSIZE, &limit) != 0) {
        fail("could not shrink the file size limit the pid-write case needs");

        return;
    }
    pipe_capture_start();
    rc = cred_txn_begin_at("pwauth_hosttest", Lock, 0);
    pipe_capture_stop(report, sizeof(report));
    setrlimit(RLIMIT_FSIZE, &saved_limit);
    if (rc == 0) {
        fail("an acquire that could not record the holder pid reported success");
        cred_txn_end();
    }
    if (present(Lock)) {
        fail("an acquire that could not record the holder pid held an empty lock");
        unlink(Lock);
    }
    if (strstr(report, "cannot record the holder pid") == NULL) {
        fail("an acquire that could not record the holder pid did not say so");
    }
}

/**
 * One refusal against a lock this test wrote: the acquire fails, the file is
 * still there with its bytes unchanged, and the diagnostic is the one the
 * recorded pid calls for.
 *
 * `expected` is a fragment the report must carry. The busy line is required
 * exactly once whatever the outcome, since it announces a wait and there is one
 * wait; and every refusal must name the removal, since none of them is anything
 * the library will do for the operator. The two forms differ in what has to be
 * true first: a stale lock is theirs to remove outright, a live pid only if it
 * is not a writer, which is the distinction a reboot's reused pid needs.
 */
static void check_refusal(const char *what, const char *body, const char *expected, int stale)
{
    char before[256];
    char after[256];
    char report[512];
    ssize_t n;

    if (spill(Lock, body, 0600) != 0) {
        fail("could not write the lock file a refusal case needs");
        return;
    }
    n = slurp(Lock, before, sizeof(before));

    capture_start();
    if (cred_txn_begin_at("pwauth_hosttest", Lock, 2) == 0) {
        capture_stop(report, sizeof(report));
        fail(what);
        cred_txn_end();

        return;
    }
    capture_stop(report, sizeof(report));

    if (occurrences(report, "credential database busy, waiting") != 1) {
        fail("an acquire that waited did not announce it exactly once");
    }
    if (strstr(report, expected) == NULL) {
        fprintf(stderr, "pwauth_hosttest: report was: %s", report);
        fail(what);
    }
    if (stale && strstr(report, "remove it as root and retry") == NULL) {
        fail("a stale lock was reported without saying how to clear it");
    }
    if (!stale && strstr(report, "remove it as root and retry") != NULL) {
        fail("a live holder was reported as a lock to remove outright");
    }
    if (!stale && strstr(report, "is not a writer, remove") == NULL) {
        fail("a live holder was reported with no way out of a pid a reboot reused");
    }
    if (!present(Lock)) {
        fail("a refused acquire removed the lock file");

        return;
    }
    if (slurp(Lock, after, sizeof(after)) != n || memcmp(before, after, (size_t)n) != 0) {
        fail("a refused acquire rewrote the lock file it reported");
    }
}

/**
 * That the set the library actually installs is the set it means to.
 *
 * A loop installing dispositions has no obvious reason to read what `sigaction`
 * returned, and this libc refuses three signals outright, so a signal the
 * library believes it covers but never installed is the worst of both: the
 * contract says a death is handled, and the writer dies with its lock held. The
 * question is therefore asked of the dispositions themselves, not of the
 * predicate that chose them.
 *
 * Every signal is set to a known disposition first, and whatever the acquire
 * changes is what it claimed. The expected set is written out here rather than
 * borrowed from the library, since a test that asked the library what it
 * intended and then checked it did that would agree with any intention. The
 * work happens in a child because it rewrites every disposition in the process,
 * which nothing else in this file would survive.
 *
 * Only the signals this host has are asked about, which is fewer than the guest
 * has: Darwin stops at 32 where the guest goes to 64, so a signal the host will
 * not accept at all is skipped, not counted as unclaimed. What that
 * leaves unchecked here is the upper range, and it is checked where it matters,
 * in the guest, by `tests/musl/pwauthmusl.c`.
 */
static void check_death_set_installed(void)
{
    static const int not_claimed[] = {SIGKILL, SIGSTOP, SIGCHLD, SIGURG, SIGWINCH, SIGCONT, SIGTSTP,
                                      SIGTTIN, SIGTTOU, SIGPIPE, 32,     33,       34};
    int fds[2];
    pid_t child;
    int status;
    char said[32];
    ssize_t got;

    unlink(Lock);
    if (pipe(fds) != 0) {
        fail("could not make the pipe the installed-set case reports through");

        return;
    }
    child = fork();
    if (child == 0) {
        char available[65];
        int sig;

        close(fds[0]);
        for (sig = 1; sig < 65; sig++) {
            struct sigaction quiet;

            memset(&quiet, 0, sizeof(quiet));
            quiet.sa_handler = SIG_IGN;
            sigemptyset(&quiet.sa_mask);
            quiet.sa_flags = 0;
            available[sig] = sigaction(sig, &quiet, NULL) == 0;
        }
        if (cred_txn_begin_at("pwauth_hosttest", Lock, 0) != 0) {
            _exit(70);
        }
        for (sig = 1; sig < 65; sig++) {
            struct sigaction now;
            int expected = 1;
            size_t k;
            int taken;

            if (!available[sig]) {
                continue;
            }
            for (k = 0; k < sizeof(not_claimed) / sizeof(not_claimed[0]); k++) {
                if (not_claimed[k] == sig) {
                    expected = 0;
                }
            }
            if (sigaction(sig, NULL, &now) != 0) {
                taken = 0;
            } else {
                taken = now.sa_handler != SIG_IGN;
            }
            /* SIGPIPE is claimed, but by a disposition of its own, so it is
             * listed as unclaimed here and checked apart. */
            if (sig == SIGPIPE) {
                expected = 1;
            }
            if (taken != expected) {
                char report = (char)sig;

                (void)write(fds[1], &report, 1);
            }
        }
        (void)cred_txn_end();
        _exit(0);
    }
    close(fds[1]);
    if (child < 0) {
        fail("could not fork the writer the installed-set case inspects");
        close(fds[0]);

        return;
    }
    while ((got = read(fds[0], said, sizeof(said))) > 0) {
        ssize_t n;

        for (n = 0; n < got; n++) {
            char line[96];

            snprintf(line, sizeof(line),
                     "signal %d is not handled the way the library means it to be",
                     (int)(unsigned char)said[n]);
            fail(line);
        }
    }
    close(fds[0]);
    if (!wait_bounded(child, &status, 20000)) {
        kill(child, SIGKILL);
        (void)wait_bounded(child, &status, 20000);
        fail("the writer the installed-set case inspects never finished");
    } else if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fail("the writer the installed-set case inspects could not take its transaction");
    }
    unlink(Lock);
}

/**
 * A password reader started in the background, and the terminal it does not
 * own.
 *
 * `su &` is one keystroke, and this kernel raises nothing for a `tcsetattr`
 * from a background process group: only the read is guarded, by `SIGTTIN`. So a
 * reader that took echo away before reading would silence a terminal belonging
 * to the shell that started it and then stop at the first byte, leaving that
 * shell echoing nothing and no program running that knows how to put it back.
 * Killing the stopped job does not undo it either, since the modes are the
 * terminal's and not the process's.
 *
 * The reader must therefore leave the modes alone until it owns the
 * terminal, and pick them up on the resume that a `fg` sends, which is the
 * path that already asks the question. The case builds the shape a shell
 * builds: a session leader holding the terminal, and the reader in a process
 * group of its own. Both halves are asserted, because either alone would
 * pass for the wrong reason: the terminal still echoes, and the reader
 * stopped on `SIGTTIN` instead of getting a byte it should not have been
 * given.
 */
static void check_background_prompt(void)
{
    char slave[128];
    char answer[2];
    int master;
    int fds[2];
    pid_t child;
    int status;

    master = pty_make(slave, sizeof(slave));
    if (master < 0) {
        fail("could not make the terminal the background-prompt case shares");

        return;
    }
    if (pipe(fds) != 0) {
        fail("could not make the pipe the background-prompt case reports through");
        close(master);

        return;
    }
    child = fork();
    if (child == 0) {
        close(fds[0]);
        close(master);
        if (dup2(fds[1], 1) < 0) {
            _exit(90);
        }
        close(fds[1]);
        execl(Self, Self, HELPER_BG, slave, (char *)NULL);
        _exit(90);
    }
    close(fds[1]);
    if (child < 0) {
        fail("could not fork the session the background-prompt case watches");
        close(fds[0]);
        close(master);

        return;
    }
    answer[0] = '\0';
    answer[1] = '\0';
    {
        struct pollfd waiting;

        waiting.fd = fds[0];
        waiting.events = POLLIN;
        waiting.revents = 0;
        if (poll(&waiting, 1, 20000) != 1 ||
            read(fds[0], answer, sizeof(answer)) != (ssize_t)sizeof(answer)) {
            answer[0] = '\0';
            answer[1] = '\0';
        }
    }
    close(fds[0]);
    if (!wait_bounded(child, &status, 20000)) {
        kill(child, SIGKILL);
        (void)wait_bounded(child, &status, 20000);
        fail("the session the background-prompt case watches never finished");
    } else if (WIFEXITED(status) && WEXITSTATUS(status) >= 90) {
        fail("the session the background-prompt case watches could not set itself up");
    }
    if (answer[1] != 1) {
        fail("a reader in the background did not stop on SIGTTIN as it should have");
    }
    if (answer[0] != 1) {
        fail("a reader in the background took echo from a terminal it did not own");
    }
    close(master);
}

/**
 * A death arriving inside the restore that another death started.
 *
 * `pwauth_tty_restore` does two things, and the order decides whether a console
 * survives. A handler runs with only its own signal blocked, so a second death
 * can land between them and enters the restore again on its way out. If the
 * flag that says "echo is off" is cleared first, that second death finds it
 * down, skips the work, and leaves; the restore it interrupted never resumes
 * and the terminal keeps the echo-off modes with no program left to undo them.
 * If the modes go back first, the second death finds the flag still up and does
 * the restore itself, at the cost of one repeated `tcsetattr`.
 *
 * The window is two instructions wide, so the signal comes from inside it: the
 * interposed `tcsetattr` raises one on the call the restore makes. It is run
 * twice, with a death and with a resume, and the second is the harder half. A
 * resume is not another restore: `pwauth_cont_handler` exists to put a prompt
 * back after a stop, so it re-applies the echo-off modes, and delivered inside
 * the fatal restore it silences the terminal the dying process was in the
 * middle of giving back. The verdict is therefore the terminal's own `ECHO` bit
 * once the reader is gone, read from the slave while this process still holds
 * the master, not a count of calls that a resume would inflate.
 *
 * The helper takes the terminal as its controlling one and its own session,
 * because the resume touches the modes only while the process is the
 * foreground group; without that it would return early and prove nothing.
 */
static void check_restore_reentry(const char *inject)
{
    char slave[128];
    char answer = '\0';
    int master;
    int fds[2];
    pid_t child;
    int status;

    unlink(Lock);
    master = pty_make(slave, sizeof(slave));
    if (master < 0) {
        fail("could not make the terminal the restore-reentry case prompts on");

        return;
    }
    if (pipe(fds) != 0) {
        fail("could not make the pipe the restore-reentry case reports through");
        close(master);

        return;
    }
    child = fork();
    if (child == 0) {
        close(fds[0]);
        close(master);
        if (dup2(fds[1], 1) < 0) {
            _exit(90);
        }
        close(fds[1]);
        execl(Self, Self, HELPER_TTY, slave, Lock, inject, (char *)NULL);
        _exit(90);
    }
    close(fds[1]);
    if (child < 0) {
        fail("could not fork the reader the restore-reentry case interrupts");
        close(fds[0]);
        close(master);

        return;
    }
    /* The prompt on the master says the reader has taken echo away, which is
     * the first tcsetattr; the restore the signal provokes is the second. */
    if (!read_until(master, "Password: ", 20000)) {
        fail("the reader the restore-reentry case interrupts never prompted");
        kill(child, SIGKILL);
        (void)wait_bounded(child, &status, 20000);
        close(fds[0]);
        close(master);
        unlink(Lock);

        return;
    }
    kill(child, SIGTERM);
    {
        struct pollfd waiting;

        waiting.fd = fds[0];
        waiting.events = POLLIN;
        waiting.revents = 0;
        if (poll(&waiting, 1, 20000) != 1 || read(fds[0], &answer, 1) != 1) {
            answer = '\0';
        }
    }
    close(fds[0]);
    if (!wait_bounded(child, &status, 20000)) {
        kill(child, SIGKILL);
        (void)wait_bounded(child, &status, 20000);
        fail("the reader the restore-reentry case interrupts never finished");
    } else if (WIFEXITED(status) && WEXITSTATUS(status) >= 90) {
        char said[96];

        snprintf(said, sizeof(said), "the %s reader could not set itself up (status %d)", inject,
                 WEXITSTATUS(status));
        fail(said);
    }
    if (answer == '\0') {
        fail("the restore-reentry case never heard from its reader");
    } else if (strcmp(inject, "death") == 0 && (int)answer < 3) {
        char said[128];

        /* Three: the prompt's echo-off, the restore the death began, and the
         * restore the second death did because the flag was still up. Two means
         * the second death found the flag already down and skipped it, which is
         * the terminal left quiet with nobody to fix it. */
        snprintf(said, sizeof(said),
                 "a death inside a restore skipped it (%d calls where three were due)",
                 (int)answer);
        fail(said);
    } else if (strcmp(inject, "cont") == 0 && (int)answer != 2) {
        char said[128];

        /* Two: the echo-off and the restore. A third would be the resume
         * putting echo back off inside the dying process's restore, which is
         * what disarming it before the restore exists to prevent. */
        snprintf(said, sizeof(said),
                 "a resume inside a restore touched the terminal (%d calls where two were due)",
                 (int)answer);
        fail(said);
    }
    /* The terminal itself is the verdict, not the number of calls: a resume
     * reaches `tcsetattr` too and puts echo back OFF, so a case that counted
     * would call that a restore. The modes are read from the slave after the
     * reader is gone; the master is still open, so the line still exists. */
    {
        int check = open(slave, O_RDWR | O_NOCTTY);
        struct termios ended;

        if (check < 0) {
            fail("could not reopen the terminal the restore-reentry case watched");
        } else {
            if (tcgetattr(check, &ended) != 0) {
                fail("could not read back the modes the restore-reentry case watched");
            } else if ((ended.c_lflag & ECHO) == 0) {
                char said[128];

                snprintf(said, sizeof(said),
                         "a %s inside a restore left the terminal with echo off", inject);
                fail(said);
            }
            close(check);
        }
    }
    close(master);
    unlink(Lock);
}

/**
 * A signal outside the four a writer expects, sent while it holds a
 * transaction.
 *
 * This kernel terminates a process for every signal but four it ignores and
 * four it stops for, and `cred_may_signal` lets a caller signal a process whose
 * real uid matches its own, which a setuid program keeps from whoever ran it.
 * So `SIGUSR1` is as fatal to a writer as `SIGTERM`, and as easy for an
 * unprivileged user to send. Handling only the four that arrive by habit would
 * leave every other signal as a way to strand the credential database: the
 * writer dies where no registered function runs, and the lock stays in `/etc`
 * until root removes it.
 *
 * The child holds a transaction and takes the signal. It must die of it, since
 * a death is still a death, and the lock must be gone, which only happens if
 * the handler ran and left through `exit` to reach the registered release.
 */
static void check_unlisted_death(void)
{
    static const int sent[] = {SIGUSR1, SIGUSR2, SIGALRM, SIGABRT, SIGSYS};
    size_t i;

    for (i = 0; i < sizeof(sent) / sizeof(sent[0]); i++) {
        int ready[2];
        pid_t child;
        int status;
        char token = '\0';

        unlink(Lock);
        if (pipe(ready) != 0) {
            fail("could not make the pipe the unlisted-death case waits on");

            return;
        }
        child = fork();
        if (child == 0) {
            close(ready[0]);
            if (cred_txn_begin_at("pwauth_hosttest", Lock, 0) != 0) {
                _exit(70);
            }
            if (write(ready[1], "r", 1) != 1) {
                _exit(71);
            }
            for (;;) {
                nap(50);
            }
        }
        close(ready[1]);
        if (child < 0) {
            fail("could not fork the writer the unlisted-death case signals");
            close(ready[0]);

            return;
        }
        if (read(ready[0], &token, 1) != 1 || token != 'r') {
            fail("the writer the unlisted-death case signals never took its transaction");
            kill(child, SIGKILL);
            (void)wait_bounded(child, &status, 20000);
            close(ready[0]);
            unlink(Lock);
            continue;
        }
        close(ready[0]);
        kill(child, sent[i]);
        if (!wait_bounded(child, &status, 20000)) {
            kill(child, SIGKILL);
            (void)wait_bounded(child, &status, 20000);
            fail("a writer sent a signal outside the usual four did not die of it");
        } else if (WIFSIGNALED(status)) {
            fail("a writer sent a signal outside the usual four took the default action");
        } else if (!WIFEXITED(status)) {
            fail("a writer sent a signal outside the usual four ended in no known way");
        }
        if (present(Lock)) {
            fail("a writer killed by a signal outside the usual four left its lock behind");
            unlink(Lock);
        }
    }
}

/**
 * A `SIGPIPE` delivered between the lock's creation and its handler.
 *
 * The four deaths are blocked across the acquire, but `SIGPIPE` has a fatal
 * default too, and its handler does not go in until several calls after the
 * lock exists. A signal in that window would kill the writer where no
 * registered function runs, leaving the lock behind for an operator. It is not
 * a theoretical window: this kernel lets a process signal one whose real uid
 * matches its own, and a setuid program keeps the real uid of whoever ran it,
 * so the user who started the writer may send it as many as they please.
 *
 * The interposed `getpid` is what makes the moment reachable: the acquire
 * records the holder pid through it, and the case arms it to raise `SIGPIPE` on
 * the first call for which the lock file already exists. A child takes the
 * signal so that the run survives to report it, and it must come back alive
 * with the lock released, not killed with the lock left.
 */
static void check_pipe_during_acquire(void)
{
    pid_t child;
    int status;

    unlink(Lock);
    child = fork();
    if (child == 0) {
        int taken;

        snprintf(Getpid_raise_when, sizeof(Getpid_raise_when), "%s", Lock);
        Getpid_raise = 1;
        taken = cred_txn_begin_at("pwauth_hosttest", Lock, 0);
        Getpid_raise = 0;
        if (taken != 0) {
            _exit(70);
        }
        _exit(cred_txn_end() == 0 ? 0 : 71);
    }
    if (child < 0 || !wait_bounded(child, &status, 20000)) {
        fail("could not run the writer the acquire-signal case watches");
    } else if (WIFSIGNALED(status)) {
        fail("a SIGPIPE inside the acquire killed the writer");
    } else if (!WIFEXITED(status)) {
        fail("the writer the acquire-signal case watches did not exit normally");
    } else if (WEXITSTATUS(status) == 70) {
        fail("a SIGPIPE inside the acquire made it refuse the lock");
    } else if (WEXITSTATUS(status) != 0) {
        fail("the writer the acquire-signal case watches could not end its transaction");
    }
    if (present(Lock)) {
        fail("a SIGPIPE inside the acquire left the lock behind");
        unlink(Lock);
    }
}

/**
 * A refused acquire that cannot report, and must repair itself anyway.
 *
 * Every diagnostic is a write, and a write to a standard error that is a full
 * pipe with a reader that is not reading BLOCKS: `SIGPIPE` answers a reader
 * that has gone, not one that is merely slow. The library has the four death
 * signals across the acquire, so a report issued before the repair would leave
 * the writer stopped inside that write, holding a lock it had already decided
 * to give up, and deaf to everything but `SIGKILL`, which is the one signal
 * that leaves the lock exactly where it is.
 *
 * Both halves are forced at once: `RLIMIT_FSIZE` of zero makes the write of the
 * holder pid fail, and a pipe filled to capacity, whose read end this process
 * holds open and never reads, makes the diagnostic block. The writer is
 * expected to still be stuck in that write, which is not the fault; what is
 * asserted is that it is stuck having already put things right. So the lock
 * must already be gone while it is still stuck, which is only true if the
 * unlink came first, and it must then die of an ordinary `SIGTERM` rather than
 * needing `SIGKILL`, which is only true if the mask went back first. That the
 * pid-write path creates a lock at all, and removes it, is `check_lock`'s; this
 * case is about the order of the last three steps.
 *
 * The child says when it is ready, on a pipe of its own, and it says so after
 * the filling and immediately before the acquire. A fixed pause instead would
 * be a test that can pass without testing anything: on a loaded host the child
 * could still be filling when the signal arrived, die of it as any process
 * would, leave no lock because it had not made one yet, and satisfy both
 * assertions on an implementation with the fault in it.
 */
static void check_report_blocks(void)
{
    int stuck[2];
    int ready[2];
    pid_t child;
    int status;

    unlink(Lock);
    if (pipe(stuck) != 0) {
        fail("could not make the pipe the blocked-report case fills");

        return;
    }
    if (pipe(ready) != 0) {
        fail("could not make the pipe the blocked-report case waits on");
        close(stuck[0]);
        close(stuck[1]);

        return;
    }
    child = fork();
    if (child == 0) {
        close(ready[0]);
        char filler[4096];
        struct rlimit limit;
        long wrote = 0;

        close(stuck[0]);
        if (dup2(stuck[1], 2) < 0) {
            _exit(80);
        }
        close(stuck[1]);
        signal(SIGXFSZ, SIG_IGN);
        limit.rlim_cur = 0;
        limit.rlim_max = RLIM_INFINITY;
        if (setrlimit(RLIMIT_FSIZE, &limit) != 0) {
            _exit(81);
        }
        /* Fill it without blocking, then put the descriptor back the way the
         * library will find it. Filling through a blocking descriptor would
         * simply stop here, and the case would never reach the acquire it
         * exists to watch. */
        if (fcntl(2, F_SETFL, O_NONBLOCK) != 0) {
            _exit(82);
        }
        memset(filler, 'x', sizeof(filler));
        for (;;) {
            ssize_t n = write(2, filler, sizeof(filler));

            if (n <= 0) {
                break;
            }
            wrote += n;
            if (wrote > (long)(1 << 22)) {
                _exit(82);
            }
        }
        if (errno != EAGAIN || fcntl(2, F_SETFL, 0) != 0) {
            _exit(82);
        }
        if (write(ready[1], "r", 1) != 1) {
            _exit(82);
        }
        close(ready[1]);
        _exit(cred_txn_begin_at("pwauth_hosttest", Lock, 0) == 0 ? 83 : 84);
    }
    close(stuck[1]);
    close(ready[1]);
    if (child < 0) {
        fail("could not fork the writer the blocked-report case watches");
        close(stuck[0]);
        close(ready[0]);

        return;
    }
    {
        struct pollfd waiting;
        char token = '\0';

        waiting.fd = ready[0];
        waiting.events = POLLIN;
        waiting.revents = 0;
        if (poll(&waiting, 1, 20000) != 1 || read(ready[0], &token, 1) != 1 || token != 'r') {
            fail("the writer the blocked-report case watches never reached its acquire");
            kill(child, SIGKILL);
            (void)wait_bounded(child, &status, 20000);
            close(stuck[0]);
            close(ready[0]);

            return;
        }
    }
    close(ready[0]);
    /* Past the ready token the child has only the acquire left, and the acquire
     * ends stuck in a write that never completes, so this settles rather than
     * races: what follows must find it alive and blocked. */
    nap(500);
    if (waitpid(child, &status, WNOHANG) == child) {
        fail("the writer the blocked-report case watches finished instead of blocking");
        close(stuck[0]);

        return;
    }
    /* The ordering assertion, made while it is still stuck: the repair happened
     * before the report it is stuck inside. */
    if (present(Lock)) {
        fail("an acquire still reporting its refusal has not yet removed its lock");
    }
    kill(child, SIGTERM);
    if (!wait_bounded(child, &status, 10000)) {
        kill(child, SIGKILL);
        (void)wait_bounded(child, &status, 10000);
        fail("a writer blocked reporting its refusal could not be killed by SIGTERM");
    } else if (WIFEXITED(status) && WEXITSTATUS(status) >= 80 && WEXITSTATUS(status) <= 82) {
        fail("the writer the blocked-report case watches could not set itself up");
    } else if (WIFEXITED(status) && WEXITSTATUS(status) == 83) {
        fail("an acquire that could not record the holder pid reported success");
    } else if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGTERM) {
        fail("a writer blocked reporting its refusal did not die of the signal sent");
    }
    close(stuck[0]);
    if (present(Lock)) {
        fail("an acquire that could not report its refusal left the lock behind");
        unlink(Lock);
    }
}

/**
 * The lock protocol: what an acquire does when the file is already there, and
 * what it never does.
 *
 * The retry budget is two here, not the compiled-in ten, so the refusal
 * arrives in about two seconds; that the wait is bounded at all is checked with
 * the clock, since a lock that never waited would pass every other assertion.
 */
static void check_lock(void)
{
    char self[32];
    char reaped[32];
    char before[256];
    char after[256];
    char report[512];
    struct stat st_before, st_after;
    time_t started, elapsed;
    pid_t child;
    int status;
    ssize_t n;

    unlink(Lock);
    snprintf(self, sizeof(self), "%d\n", (int)getpid());

    started = time(NULL);
    check_refusal("an acquire took a lock naming a live pid", self, "credential database busy (pid",
                  0);
    elapsed = time(NULL) - started;
    if (elapsed < 2) {
        fail("an acquire with a budget of two tries did not wait two seconds");
    }
    if (elapsed > 8) {
        fail("an acquire with a budget of two tries waited far past its budget");
    }

    child = fork();
    if (child == 0) {
        _exit(0);
    }
    if (child < 0 || waitpid(child, &status, 0) != child) {
        fail("could not mint a reaped pid for the stale-lock case");
    } else {
        snprintf(reaped, sizeof(reaped), "%d\n", (int)child);
        check_refusal("an acquire took a lock naming a reaped pid", reaped, "is gone)", 1);
        if (spill(Lock, reaped, 0600) == 0) {
            backdate(Lock, 3600);
            check_refusal("an acquire took an hour-old lock naming a reaped pid", reaped,
                          "is gone)", 1);
        }
    }

    if (spill(Lock, self, 0600) == 0) {
        backdate(Lock, 3600);
        check_refusal("an acquire took an hour-old lock naming a live pid", self,
                      "credential database busy (pid", 0);
    }

    if (spill(Lock, "", 0600) == 0) {
        backdate(Lock, 3600);
        check_refusal("an acquire took a lock recording no holder", "", "(no holder recorded)", 1);
    }
    check_refusal("an acquire took a lock whose contents do not parse", "not a pid\n",
                  "(no holder recorded)", 1);

    /* The control the five refusals rest on: with the file gone, the same call
     * succeeds. */
    unlink(Lock);
    if (cred_txn_begin_at("pwauth_hosttest", Lock, 2) != 0) {
        fail("an acquire failed with no lock file in the way");

        return;
    }
    if (slurp(Lock, before, sizeof(before)) <= 0 || atoi(before) != (int)getpid()) {
        fail("a held lock does not record the holder's pid");
    }

    n = slurp(Lock, before, sizeof(before));
    stat(Lock, &st_before);
    started = time(NULL);
    capture_start();
    if (cred_txn_begin_at("pwauth_hosttest", Lock, 2) == 0) {
        fail("a second transaction in one process was granted");
    }
    capture_stop(report, sizeof(report));
    if (time(NULL) - started > 1) {
        fail("a second transaction in one process waited instead of refusing");
    }
    if (strstr(report, "already locked by this program") == NULL) {
        fail("a second transaction was refused as contention, not as a mistake");
    }
    if (slurp(Lock, after, sizeof(after)) != n || memcmp(before, after, (size_t)n) != 0 ||
        stat(Lock, &st_after) != 0 || st_before.st_mtime != st_after.st_mtime) {
        fail("a second transaction in one process touched the lock file");
    }

    if (cred_txn_end() != 0) {
        fail("cred_txn_end refused a held transaction");
    }
    if (present(Lock)) {
        fail("cred_txn_end left the lock file behind");
    }
    if (cred_txn_end() == 0) {
        fail("cred_txn_end reported success with no transaction held");
    }
}

/**
 * A standard error that is a pipe with no reader, which must kill neither a
 * prompt nor a writer.
 *
 * The prompt is written to file descriptor 2 after echo has been taken away, so
 * the default `SIGPIPE` action ends the process at exactly the point where the
 * terminal is silenced and no program is left to restore it. A transaction is
 * the same death with a lock on top: a writer reports as it goes, and a
 * diagnostic into a pipe nobody reads would end it where no `atexit` runs, with
 * the lock file left in `/etc` for every later writer to wait ten seconds and
 * be
 * refused by. So there are two cases, one per lifetime, and both are children,
 * since what is being guarded against is a death.
 */
static void check_sigpipe(void)
{
    static const char line[] = "secret\n";
    int broken[2];
    int feed[2];
    pid_t child;
    int status;

    unlink(Lock);
    if (pipe(broken) != 0) {
        fail("could not make the pipe with no reader");

        return;
    }
    if (pipe(feed) != 0) {
        close(broken[0]);
        close(broken[1]);
        fail("could not make the pipe the prompt case reads its line from");

        return;
    }
    if (write(feed[1], line, sizeof(line) - 1) != (ssize_t)sizeof(line) - 1) {
        fail("could not script the line the prompt case reads");
    }
    close(feed[1]);
    close(broken[0]); /* from here the write end has no reader anywhere */

    child = fork();
    if (child == 0) {
        char password[64];

        if (dup2(feed[0], 0) != 0 || dup2(broken[1], 2) != 2) {
            _exit(70);
        }
        if (pwauth_read_password("Password: ", password, sizeof(password)) != 0) {
            _exit(71);
        }
        _exit(strcmp(password, "secret") == 0 ? 0 : 72);
    }
    close(feed[0]);
    if (child < 0 || !wait_bounded(child, &status, 10000)) {
        fail("could not run the prompt whose standard error nobody reads");
    } else if (!WIFEXITED(status)) {
        fail("a prompt written into a pipe with no reader killed the reader");
    } else if (WEXITSTATUS(status) != 0) {
        fail("a prompt written into a pipe with no reader did not read its line");
    }

    child = fork();
    if (child == 0) {
        if (cred_txn_begin_at("pwauth_hosttest", Lock, 0) != 0) {
            _exit(73);
        }
        if (dup2(broken[1], 2) != 2) {
            _exit(74);
        }
        cred_rewrite(NULL, NULL, 0); /* one line, into a pipe with no reader */
        exit(0);                     /* the release registered with atexit must still run */
    }
    if (child < 0 || !wait_bounded(child, &status, 10000)) {
        fail("could not run the writer whose standard error nobody reads");
    } else if (!WIFEXITED(status)) {
        fail("a writer reporting into a pipe with no reader was killed holding its lock");
    } else if (WEXITSTATUS(status) != 0) {
        fail("the writer whose standard error nobody reads could not take its transaction");
    }
    if (present(Lock)) {
        fail("a writer killed at its own diagnostic left its lock behind");
        unlink(Lock);
    }
    close(broken[1]);
}

/**
 * A release with no descriptor to spare, and a release that cannot finish.
 *
 * `cred_txn_end` must need no descriptor of its own. The transaction already
 * holds the lock's, so closing it frees the slot the ownership check needs; an
 * ownership check that opened a second one read every failure, `EMFILE`
 * included, as another writer's lock, which is the one answer that declines to
 * unlink. A process holding the lock in its last free slot would then strand it
 * for everyone and report success. The child is the one that fills its table,
 * since a test that filled its own could not say what it found.
 *
 * The second case is what the caller is told. A lock that cannot be looked at
 * is not a lock that is gone, and a release that reported success over one
 * would send the next writer into a wait and a refusal nobody warned it of. The
 * unreadable lock is made here with a symbolic link that resolves to itself,
 * which is an `open` no retry helps, standing in for the exhausted table the
 * first case arranges for real.
 *
 * The third is the answer between those two. A removal that did not reach the
 * disk is neither a clean finish nor a lock left behind: the lock is gone from
 * the running system, which the case asserts, and may be back after a power
 * cut, which is why it is its own return and not a printed line.
 */
static void check_release(void)
{
    char report[512];
    pid_t child;
    int status;
    int rc;
    int fd;

    unlink(Lock);
    child = fork();
    if (child == 0) {
        struct rlimit limit;
        int wrong = 0;

        if (cred_txn_begin_at("pwauth_hosttest", Lock, 0) != 0) {
            _exit(80);
        }
        if (getrlimit(RLIMIT_NOFILE, &limit) != 0) {
            _exit(81);
        }
        limit.rlim_cur = 16;
        if (setrlimit(RLIMIT_NOFILE, &limit) != 0) {
            _exit(82);
        }
        while (open("/dev/null", O_RDONLY) >= 0) {
            ;
        }
        if (errno != EMFILE) {
            _exit(83);
        }
        if (cred_txn_end() != 0) {
            wrong |= 1;
        }
        if (present(Lock)) {
            wrong |= 2;
        }
        _exit(wrong);
    }
    if (child < 0 || !wait_bounded(child, &status, 20000) || !WIFEXITED(status)) {
        fail("could not run the release under a full descriptor table");
    } else if (WEXITSTATUS(status) >= 80) {
        fail("the child that fills its descriptor table could not set the case up");
    } else if ((WEXITSTATUS(status) & 2) != 0) {
        fail("a release with no descriptor to spare left the lock behind");
    } else if ((WEXITSTATUS(status) & 1) != 0) {
        fail("a release with no descriptor to spare reported a failure");
    }
    if (present(Lock)) {
        fail("the lock of a writer that filled its descriptor table is still there");
        unlink(Lock);
    }

    if (cred_txn_begin_at("pwauth_hosttest", Lock, 0) != 0) {
        fail("could not take the transaction the unreadable-lock case releases");

        return;
    }
    if (unlink(Lock) != 0 || symlink(Lock, Lock) != 0) {
        fail("could not put a lock in the way of its own release");
        cred_txn_end();
        unlink(Lock);

        return;
    }
    fd = open(Lock, O_RDONLY);
    if (fd >= 0) {
        close(fd);
        fail("the lock the release cannot read can be read after all");
    } else if (errno != ELOOP) {
        fail("the unreadable lock fails to open for the wrong reason");
    }
    capture_start();
    rc = cred_txn_end();
    capture_stop(report, sizeof(report));
    if (rc == 0) {
        fail("a release that could not look at the lock reported success");
    }
    if (strstr(report, "cannot read the lock") == NULL) {
        fail("a release that could not look at the lock did not say so");
    }
    unlink(Lock);

    if (cred_txn_begin_at("pwauth_hosttest", Lock, 0) != 0) {
        fail("could not take the transaction the unflushed-release case ends");

        return;
    }
    Fsync_calls = 0;
    Fsync_fail_at = 1;
    capture_start();
    rc = cred_txn_end();
    capture_stop(report, sizeof(report));
    Fsync_fail_at = 0;
    if (rc != CRED_UNFLUSHED) {
        fail("a release whose flush failed did not report the lock as unflushed");
    }
    if (present(Lock)) {
        fail("a release whose flush failed left the lock behind");
        unlink(Lock);
    }
    if (strstr(report, "not flushed to disk") == NULL) {
        fail("a release whose flush failed did not say so");
    }
}

/**
 * What a program exec'd by a child of a writer inherits.
 *
 * A transaction catches `SIGPIPE` so that a report into a pipe with no reader
 * fails the write instead of killing the writer with its lock still in `/etc`.
 * The disposition is this library's business and no other program's, and the
 * two ways to arrange it part company at exactly that point: an exec resets a
 * caught signal to the default and carries an ignored one into the new image.
 * A child forked under a transaction holds none of it and has no way to give
 * back what it inherited, so with `SIG_IGN` the next program it runs would find
 * `SIGPIPE` ignored, having linked nothing that asked for it, and a pipeline
 * whose reader went away would spin where it should have ended.
 *
 * The program runs itself to ask, since the far side of an exec is the only
 * place the question can be answered and the answer must come from a different
 * image than the one that set the disposition.
 */
static void check_sigpipe_exec(void)
{
    pid_t child;
    int status;

    unlink(Lock);
    if (cred_txn_begin_at("pwauth_hosttest", Lock, 0) != 0) {
        fail("could not take the transaction the exec case forks under");

        return;
    }
    child = fork();
    if (child == 0) {
        execl(Self, Self, HELPER_PIPE, (char *)NULL);
        _exit(90);
    }
    if (child < 0 || !wait_bounded(child, &status, 20000) || !WIFEXITED(status)) {
        fail("could not run the program a writer's child execs");
    } else if (WEXITSTATUS(status) == 90) {
        fail("the program a writer's child execs could not be run");
    } else if (WEXITSTATUS(status) == 91) {
        fail("the program a writer's child execs could not read its own disposition");
    } else if (WEXITSTATUS(status) != 0) {
        fail("a writer's SIGPIPE disposition survived into the program its child exec'd");
    }
    if (cred_txn_end() != 0) {
        fail("could not end the transaction the exec case held");
    }
    unlink(Lock);
}

/* The mask this program started with. Every case must give it back, and it is
 * captured, not assumed empty: the mask is inherited, so a harness that
 * ran this test with something blocked would otherwise turn every comparison
 * into a failure blaming the library for its own caller's doing. */
static sigset_t Start_mask;
static char Exit_lock[192];

/** The disposition the kept-SIGPIPE helper installs after ending its
 * transaction, recognizable only by its address. */
static void keep_marker(int sig)
{
    (void)sig;
}

/**
 * Report how many restores reached the terminal, from after the death that
 * interrupted one.
 */
static void report_restore_count(void)
{
    char answer = (char)Tcs_calls;

    (void)write(1, &answer, 1);
}

/**
 * Report, from the heir, what the registered release left of the state it
 * inherited.
 *
 * Two bytes: whether the disposition this image installed before the acquire is
 * back, and whether the death signals are back to the default. Registered
 * before the library's, so it runs after them, and it runs in the child because
 * a `fork` has the registrations across while the parent leaves through
 * `_exit` and runs none of them.
 */
static void report_heir(void)
{
    struct sigaction pipe_now, death_now;
    char answer[2];

    answer[0] = (sigaction(SIGPIPE, NULL, &pipe_now) == 0 && pipe_now.sa_handler == keep_marker)
                    ? 'y'
                    : 'n';
    answer[1] =
        (sigaction(SIGINT, NULL, &death_now) == 0 && death_now.sa_handler == SIG_DFL) ? 'y' : 'n';
    (void)write(1, answer, sizeof(answer));
}

/**
 * Report, from after the registered release has run, whether the disposition
 * this program installed for itself is still the one in force.
 *
 * Registered before the library's, so it runs after them. A writer that ended
 * its transaction has already put `SIGPIPE` back, and whatever it installed
 * afterwards belongs to it; the release still to come must not reach back and
 * overwrite that with the copy it saved at the acquire.
 */
static void report_pipe_kept(void)
{
    struct sigaction now;
    char answer = 'n';

    if (sigaction(SIGPIPE, NULL, &now) == 0 && now.sa_handler == keep_marker) {
        answer = 'y';
    }
    (void)write(1, &answer, 1);
}

/**
 * Report, from after the registered release has run, whether it gave the mask
 * back. Registered with `atexit` before the library's own, so it runs last.
 */
static void report_exit_mask(void)
{
    char answer[3];

    answer[0] = mask_changed(&Start_mask) == 0 ? 'y' : 'n';
    answer[1] = Getpid_mask_obs == 1 ? 'y' : (Getpid_mask_obs == -2 ? '?' : 'n');
    /* Registered before the library's two, so this runs after both: the last
     * moment of the process, and the one a function registered by an
     * application before it ever called the library would also occupy. An
     * acquire here can never be released, since the walk of registered
     * functions only moves downwards and the release is already behind it, so
     * the library must refuse it. */
    answer[2] = cred_txn_begin_at("pwauth_hosttest", Exit_lock, 0) != 0 ? 'y' : 'n';
    (void)write(1, answer, sizeof(answer));
}

/**
 * What the release registered with `atexit` leaves behind on the way out.
 *
 * It blocks the four deaths for its own length, so that a death served during
 * the teardown cannot call `exit` from inside `exit`, and it must give the mask
 * back before returning: what follows it is the rest of `exit`, other
 * registered functions and the flush of every stream, and a writer that left
 * the four blocked through a flush onto a pipe with no reader would be
 * unkillable by anything short of `SIGKILL`.
 *
 * No case inside this process can watch that, since the release runs after the
 * last of them. The answer therefore comes from a fresh image: a child execs
 * this program, which registers its own reporter before taking a transaction,
 * so the library's own registrations (its exit marker and then its release,
 * both later than the reporter) run first and the reporter runs last and sees
 * what they left. The same case proves the
 * registered release does its job at all, since the child exits holding a
 * transaction it never ends and the lock must be gone anyway.
 */
static void check_exit_mask(void)
{
    char answer[3];
    int fds[2];
    pid_t child;
    int status;

    unlink(Lock);
    if (pipe(fds) != 0) {
        fail("could not make the pipe the exit-mask case reports through");

        return;
    }
    child = fork();
    if (child == 0) {
        close(fds[0]);
        if (dup2(fds[1], 1) < 0) {
            _exit(95);
        }
        close(fds[1]);
        execl(Self, Self, HELPER_EXIT, Lock, (char *)NULL);
        _exit(90);
    }
    close(fds[1]);
    if (child < 0) {
        fail("could not fork the writer the exit-mask case watches");
        close(fds[0]);

        return;
    }
    answer[0] = '?';
    answer[1] = '?';
    answer[2] = '?';
    /* Bounded, and not merely followed by a bounded wait. The failure this case
     * exists to catch is a writer that wedges inside the registered release,
     * and such a writer still holds the pipe's write end, so a blocking read
     * would never return and the wait below would never be reached. Nothing in
     * tools/hosttest.sh imposes an outer timeout, so an unbounded read here
     * turns a detected fault into a hung gate. */
    {
        struct pollfd waiting;

        waiting.fd = fds[0];
        waiting.events = POLLIN;
        waiting.revents = 0;
        if (poll(&waiting, 1, 20000) == 1 &&
            read(fds[0], answer, sizeof(answer)) != (ssize_t)sizeof(answer)) {
            answer[0] = '?';
        }
    }
    close(fds[0]);
    if (!wait_bounded(child, &status, 20000)) {
        kill(child, SIGKILL);
        (void)wait_bounded(child, &status, 20000);
        fail("the writer the exit-mask case watches never finished");
    } else if (!WIFEXITED(status)) {
        fail("the writer the exit-mask case watches did not exit normally");
    } else if (WEXITSTATUS(status) >= 90) {
        fail("the writer the exit-mask case watches could not set itself up");
    }
    if (answer[0] == '?') {
        fail("the exit-mask case never heard from the writer's reporter");
    } else {
        if (answer[0] != 'y') {
            fail("the registered release left a signal blocked for the rest of exit");
        }
        if (answer[1] == '?') {
            fail("the registered release never reached the interposed call inside it");
        } else if (answer[1] != 'y') {
            fail("the registered release ran with a death signal deliverable");
        }
        if (answer[2] != 'y') {
            fail("a transaction was taken after the release had already run");
        }
    }
    if (present(Lock)) {
        fail("a writer that exited holding a transaction left its lock behind");
        unlink(Lock);
    }
}

/**
 * What a child forked under a transaction leaves behind on its way out.
 *
 * The library supports a `fork` under a held transaction and says what an heir
 * owes: nothing to the lock, which is its parent's, and it must not release it.
 * What it does owe is everything local. It inherited a writable descriptor on
 * the lock file and five dispositions its own program never asked for, and if
 * the registered release skips those along with the release itself, then every
 * later registered function and the flush of every stream in that child runs
 * under this library's signal policy, and one of sixteen descriptors is held by
 * a file the child has no business in.
 *
 * The heir is a grandchild of this process: the middle image takes the
 * transaction and leaves through `_exit`, which runs no registered function, so
 * the reporter runs once and in the child that inherited. The parent's lock
 * must still be there afterwards, which is the half the older fork case already
 * covers and is repeated here because this case would otherwise pass on a
 * library that released it.
 */
static void check_heir(void)
{
    char answer[2];
    int fds[2];
    pid_t child;
    int status;

    unlink(Lock);
    if (pipe(fds) != 0) {
        fail("could not make the pipe the heir case reports through");

        return;
    }
    child = fork();
    if (child == 0) {
        close(fds[0]);
        if (dup2(fds[1], 1) < 0) {
            _exit(90);
        }
        close(fds[1]);
        execl(Self, Self, HELPER_HEIR, Lock, (char *)NULL);
        _exit(90);
    }
    close(fds[1]);
    if (child < 0) {
        fail("could not fork the writer the heir case watches");
        close(fds[0]);

        return;
    }
    answer[0] = '?';
    answer[1] = '?';
    {
        struct pollfd waiting;

        waiting.fd = fds[0];
        waiting.events = POLLIN;
        waiting.revents = 0;
        if (poll(&waiting, 1, 20000) != 1 ||
            read(fds[0], answer, sizeof(answer)) != (ssize_t)sizeof(answer)) {
            answer[0] = '?';
        }
    }
    close(fds[0]);
    if (!wait_bounded(child, &status, 20000)) {
        kill(child, SIGKILL);
        (void)wait_bounded(child, &status, 20000);
        fail("the writer the heir case watches never finished");
    } else if (!WIFEXITED(status)) {
        fail("the writer the heir case watches did not exit normally");
    } else if (WEXITSTATUS(status) >= 90) {
        fail("the writer the heir case watches could not set itself up");
    }
    if (answer[0] == '?') {
        fail("the heir case never heard from its reporter");
    } else {
        if (answer[0] != 'y') {
            fail("an heir carried the library's SIGPIPE disposition through its own exit");
        }
        if (answer[1] != 'y') {
            fail("an heir carried the library's death dispositions through its own exit");
        }
    }
    if (!present(Lock)) {
        fail("an heir released a lock that was its parent's");
    }
    unlink(Lock);
}

/**
 * A disposition installed after a transaction ended, and the release that must
 * not take it back.
 *
 * The library saves the caller's `SIGPIPE` disposition at the acquire and puts
 * it back at the end, and the release registered with `atexit` puts it back too
 * for the writer that never got that far. Those two must not both happen: a
 * writer that ended its transaction has already restored the caller's, and
 * anything installed after that belongs to the program, not to a copy the
 * library took earlier. Overwriting it at the last moment of the process
 * changes the policy under the remaining registered functions and the flush of
 * every stream, in whichever direction the program did not ask for.
 *
 * The helper is a fresh image so that its reporter, registered before the
 * library's two, runs after them and can see what they left.
 */
static void check_pipe_kept(void)
{
    char answer = '?';
    int fds[2];
    pid_t child;
    int status;

    unlink(Lock);
    if (pipe(fds) != 0) {
        fail("could not make the pipe the kept-disposition case reports through");

        return;
    }
    child = fork();
    if (child == 0) {
        close(fds[0]);
        if (dup2(fds[1], 1) < 0) {
            _exit(90);
        }
        close(fds[1]);
        execl(Self, Self, HELPER_KEEP, Lock, (char *)NULL);
        _exit(90);
    }
    close(fds[1]);
    if (child < 0) {
        fail("could not fork the writer the kept-disposition case watches");
        close(fds[0]);

        return;
    }
    {
        struct pollfd waiting;

        waiting.fd = fds[0];
        waiting.events = POLLIN;
        waiting.revents = 0;
        if (poll(&waiting, 1, 20000) != 1 || read(fds[0], &answer, 1) != 1) {
            answer = '?';
        }
    }
    close(fds[0]);
    if (!wait_bounded(child, &status, 20000)) {
        kill(child, SIGKILL);
        (void)wait_bounded(child, &status, 20000);
        fail("the writer the kept-disposition case watches never finished");
    } else if (!WIFEXITED(status)) {
        fail("the writer the kept-disposition case watches did not exit normally");
    } else if (WEXITSTATUS(status) >= 90) {
        fail("the writer the kept-disposition case watches could not set itself up");
    }
    if (answer == '?') {
        fail("the kept-disposition case never heard from the writer's reporter");
    } else if (answer != 'y') {
        fail(
            "the registered release overwrote a disposition installed after the transaction ended");
    }
    if (present(Lock)) {
        fail("the kept-disposition case left a lock behind");
        unlink(Lock);
    }
}

/**
 * Where a death signal may not be served, and that the caller's mask comes back
 * from every one of them.
 *
 * The lock file becomes this process's when the exclusive create returns, and
 * the state that says so goes in several calls later; the temporary file's name
 * reaches the handler's reach one byte at a time. A death served inside either
 * window acts on state that describes neither situation. The first strands a
 * lock file in `/etc` that no `atexit` runs to remove and that every later
 * writer refuses to remove for it. The second is worse and is why the mask is
 * here at all: the temporary's name is the target's with a suffix, so the
 * truncation at one exact length is the credential database itself, and the
 * handler unlinks whatever name it reads.
 *
 * Neither window can be entered on demand, so what is checked is the mask that
 * makes them uninhabitable, read from inside each one. The caller's mask must
 * also come back, which is the second half of every case here: a library that
 * blocked a signal for the length of a program would be a worse bug than the
 * one it closed. That half is checked on the paths that fail as well as the
 * ones that succeed, since an early return is exactly where a restore is
 * forgotten: a refused acquire, an acquire that cannot create its lock at all,
 * a replacement that aborts, and the release.
 *
 * The release is here for a third reason. Its unlink and the flush that makes
 * the unlink durable are one step, and a death between them would leave through
 * the registered release, which finds the lock already gone and reports a clean
 * finish, so the flush would be owed to nobody.
 */
static void check_masked(void)
{
    char temp[sizeof(Target) + 32];
    char report[512];
    sigset_t caller;
    int rc;

    /* The baseline is the one this program started with, not whatever is in
     * force on arrival. Reading it here would let a leak from an earlier case
     * become the thing every comparison below measures against, and every one
     * of them would then pass on a library that never gave the mask back;
     * assuming it empty would instead blame the library whenever a harness ran
     * this test with a signal already blocked. So it is captured once, in main,
     * and a discrepancy on arrival is an earlier case's fault and is said so.
     */
    caller = Start_mask;
    if (mask_changed(&caller) != 0) {
        fail("a case before this one left the signal mask altered");
        sigprocmask(SIG_SETMASK, &caller, NULL);
    }
    unlink(Lock);
    Mask_obs_n = 0;
    Mask_match = Lock;
    rc = cred_txn_begin_at("pwauth_hosttest", Lock, 0);
    Mask_match = NULL;
    if (rc != 0) {
        fail("could not take the transaction the signal-mask cases watch");

        return;
    }
    if (Mask_obs_n == 0) {
        fail("the acquire recorded no matching signal-mask observation");
    } else if (Mask_obs[0] != 1) {
        fail("the acquire recorded its lock with a death signal still deliverable");
    }
    if (mask_changed(&caller) != 0) {
        fail("the acquire returned with the caller's signal mask altered");
    }

    if (spill(Target, "before\n", 0600) != 0) {
        fail("could not write the file the masked replacement replaces");
        cred_txn_end();

        return;
    }
    snprintf(temp, sizeof(temp), "%s.tmp.%d", Target, (int)getpid());
    Mask_obs_n = 0;
    Mask_match = temp;
    rc = cred_rewrite(Target, "after\n", 6);
    Mask_match = NULL;
    if (rc != CRED_PUBLISHED) {
        fail("the replacement the signal-mask case watches did not publish");
    }
    if (Mask_obs_n < 2) {
        fail("the replacement recorded fewer than two matching signal-mask "
             "observations");
    } else {
        if (Mask_obs[0] != 0) {
            fail("the first observation was already blocked, so a blocked "
                 "reading proves nothing");
        }
        if (Mask_obs[1] != 1) {
            fail("the replacement published the temporary's name with a death deliverable");
        }
    }
    if (mask_changed(&caller) != 0) {
        fail("the replacement returned with the caller's signal mask altered");
    }

    Fsync_calls = 0;
    Fsync_fail_at = 1;
    capture_start();
    rc = cred_rewrite(Target, "aborted\n", 8);
    capture_stop(report, sizeof(report));
    Fsync_fail_at = 0;
    if (rc != CRED_UNCHANGED) {
        fail("the replacement the mask case aborts did not refuse");
    }
    if (mask_changed(&caller) != 0) {
        fail("a replacement that aborted left the caller's signal mask altered");
    }
    Fsync_mask_watch = 1;
    Fsync_mask_obs = -2;
    rc = cred_txn_end();
    Fsync_mask_watch = 0;
    if (rc != 0) {
        fail("could not end the transaction the signal-mask cases held");
    }
    if (Fsync_mask_obs == -2) {
        fail("the release never reached the flush the mask case watches");
    } else if (Fsync_mask_obs != 1) {
        fail("the release flushed its own unlink with a death signal deliverable");
    }
    if (mask_changed(&caller) != 0) {
        fail("the release left the caller's signal mask altered");
    }

    if (spill(Lock, "1\n", 0600) != 0) {
        fail("could not put a lock in the way of the refused acquire");

        return;
    }
    capture_start();
    rc = cred_txn_begin_at("pwauth_hosttest", Lock, 0);
    capture_stop(report, sizeof(report));
    if (rc == 0) {
        fail("the acquire the mask case refuses took the lock instead");
        cred_txn_end();
    }
    if (mask_changed(&caller) != 0) {
        fail("an acquire refused for a lock already there altered the caller's mask");
    }
    unlink(Lock);

    capture_start();
    rc = cred_txn_begin_at("pwauth_hosttest", "/pwauth_hosttest_nodir/lock", 0);
    capture_stop(report, sizeof(report));
    if (rc == 0) {
        fail("the acquire that cannot create its lock took one instead");
        cred_txn_end();
    }
    if (mask_changed(&caller) != 0) {
        fail("an acquire that could not create its lock altered the caller's mask");
    }
}

/**
 * Whose temporary file a replacement writes through, and when it may touch one
 * at all.
 *
 * The name has the writer's pid, and that is what keeps two writers apart.
 * A
 * fixed `<path>.tmp` is shared, so a writer whose lock had been removed by hand
 * could unlink and recreate the file a successor already had open and had
 * written; the successor's rename is by name, and it would publish the other
 * one's bytes over the credential database. With the pid in the name there is
 * no
 * such file to meet on. The second case is the other half: a replacement
 * refused
 * because the lock is not this transaction's must change nothing at all, its
 * own
 * leftover temporary included, which means the lock is checked before the
 * create
 * begins by removing whatever stands at the name.
 */
static void check_temp_name(void)
{
    char content[256];
    char report[512];
    char expected[128];
    char shared[224];
    char other[64];
    int rc;

    unlink(Lock);
    if (spill(Target, "the file a temporary stands beside\n", 0600) != 0) {
        fail("could not create the file the temporary-name cases replace");

        return;
    }
    if (cred_txn_begin_at("pwauth_hosttest", Lock, 0) != 0) {
        fail("could not take the transaction the temporary-name cases run under");

        return;
    }

    snprintf(expected, sizeof(expected), "%s.%d", TEMP_PREFIX, (int)getpid());
    Fsync_calls = 0;
    Temp_seen[0] = '\0';
    Temp_watch = 1;
    rc = cred_rewrite(Target, "one\n", 4);
    Temp_watch = 0;
    if (rc != CRED_PUBLISHED) {
        fail("cred_rewrite refused the replacement whose temporary file is watched");
    }
    if (strcmp(Temp_seen, expected) != 0) {
        fprintf(stderr, "pwauth_hosttest: the temporary file was named %s\n", Temp_seen);
        fail("a replacement's temporary file is not named for the writer that made it");
    }

    /* A directory at the shared name, which the library must have no use for.
     * */
    snprintf(shared, sizeof(shared), "%s.tmp", Target);
    if (mkdir(shared, 0700) != 0) {
        fail("could not stand a directory at the shared temporary name");
    }
    if (cred_rewrite(Target, "two\n", 4) != CRED_PUBLISHED) {
        fail("a replacement was stopped by a name it has no business using");
    }
    if (slurp(Target, content, sizeof(content)) < 0 || strcmp(content, "two\n") != 0) {
        fail("the replacement that ignored the shared name did not publish");
    }
    rmdir(shared);

    if (spill(Target_tmp, "a temporary this call must not touch\n", 0600) != 0) {
        fail("could not plant the temporary file the refused replacement must leave");
    }
    snprintf(other, sizeof(other), "%d\n", (int)getpid() + 1);
    unlink(Lock);
    if (spill(Lock, other, 0600) != 0) {
        fail("could not stand another writer's lock at the name");
    }
    capture_start();
    rc = cred_rewrite(Target, "three\n", 6);
    capture_stop(report, sizeof(report));
    if (rc != CRED_UNCHANGED) {
        fail("a replacement published with the transaction's lock gone");
    }
    if (strstr(report, "the transaction's lock is gone") == NULL) {
        fail("a replacement refused for a lost lock did not say so");
    }
    if (slurp(Target_tmp, content, sizeof(content)) < 0 ||
        strcmp(content, "a temporary this call must not touch\n") != 0) {
        fail("a replacement refused for a lost lock destroyed a temporary file first");
    }
    if (slurp(Target, content, sizeof(content)) < 0 || strcmp(content, "two\n") != 0) {
        fail("a replacement refused for a lost lock changed the target");
    }
    unlink(Target_tmp);

    if (cred_txn_end() != 0) {
        fail("cred_txn_end refused the transaction the temporary-name cases held");
    }
    unlink(Lock);
}

/**
 * A continue that arrives while the prompt is being taken down.
 *
 * The teardown is a sequence: the modes go back, a newline is written, and only
 * then are the stop and continue handlers removed. A `SIGCONT` served inside it
 * runs a handler that re-applies echo-off and reprints the prompt, and the call
 * then returns with the terminal silenced and nothing installed that would ever
 * undo it. Any process in the same session may send that signal, so the window
 * is forced, not waited for: the reader's standard error is a pipe this
 * test fills, so the newline in the middle of the teardown blocks and has the
 * reader inside the window until the signal has been delivered.
 *
 * The verdict is the reader's own, read off the terminal after the call returns
 * and reported as an exit status, because only the reader can take it without a
 * race: this terminal belongs to the reader's session, and a driver that waited
 * for the reader to be gone would be asking a terminal that no longer exists.
 */
static void check_teardown(void)
{
    char filler[1024];
    char name[128];
    pid_t child;
    long waited;
    int report[2];
    int master;
    int status;
    int chunks;
    int filled = 0;
    int reaped = 0;

    master = pty_make(name, sizeof(name));
    if (master < 0) {
        fail("could not open a pseudo-terminal for the teardown case");

        return;
    }
    if (pipe(report) != 0) {
        close(master);
        fail("could not make the pipe the teardown case holds the reader on");

        return;
    }

    child = fork();
    if (child == 0) {
        struct termios modes;
        char password[64];
        int tty = open(name, O_RDWR);

        close(master);
        close(report[0]);
        if (tty < 0 || dup2(tty, 0) != 0 || dup2(report[1], 2) != 2) {
            _exit(90);
        }
        if (tty > 2) {
            close(tty);
        }
        if (report[1] > 2) {
            close(report[1]);
        }
        if (setsid() < 0 || ioctl(0, TIOCSCTTY, 0) != 0) {
            _exit(91);
        }
        if (pwauth_read_password("Password: ", password, sizeof(password)) != 0) {
            _exit(92);
        }
        if (tcgetattr(0, &modes) != 0) {
            _exit(93);
        }
        if ((modes.c_lflag & ECHO) == 0) {
            _exit(94);
        }
        _exit(strcmp(password, "secret") == 0 ? 0 : 95);
    }
    if (child < 0) {
        close(master);
        close(report[0]);
        close(report[1]);
        fail("could not fork the reader the teardown case interrupts");

        return;
    }
    fcntl(report[0], F_SETFL, O_NONBLOCK);
    if (!read_until(report[0], "Password: ", 10000)) {
        fail("the prompt never reached the pipe the teardown case reads");
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        close(report[0]);
        close(report[1]);
        close(master);

        return;
    }

    /* The flags are shared with the reader's own file descriptor 2, so blocking
     * goes back on before the reader can reach the write this fill is for. */
    memset(filler, 'x', sizeof(filler));
    fcntl(report[1], F_SETFL, O_NONBLOCK);
    for (chunks = 0; chunks < 65536; chunks++) {
        if (write(report[1], filler, sizeof(filler)) < 0) {
            filled = (errno == EAGAIN || errno == EWOULDBLOCK);
            break;
        }
    }
    fcntl(report[1], F_SETFL, 0);
    if (!filled) {
        fail("could not fill the pipe the teardown case holds the reader on");
    }

    if (write(master, "secret\n", 7) != 7) {
        fail("could not type the password the teardown case ends with");
    }
    nap(500);
    if (kill(child, SIGCONT) != 0) {
        fail("could not continue the reader inside its teardown");
    }
    nap(300);

    /* Draining is what lets the reader finish the newline it is blocked on. */
    for (waited = 0; waited < 10000; waited += 50) {
        char sink[512];
        pid_t got;

        while (read(report[0], sink, sizeof(sink)) > 0) {
            ;
        }
        got = waitpid(child, &status, WNOHANG);
        if (got == child) {
            reaped = 1;
            break;
        }
        if (got < 0) {
            break;
        }
        nap(50);
    }
    if (!reaped) {
        kill(child, SIGKILL);
        waitpid(child, &status, 0);
        fail("the reader never came out of its teardown");
    } else if (!WIFEXITED(status)) {
        fail("the reader interrupted in its teardown died of a signal");
    } else if (WEXITSTATUS(status) == 94) {
        fail("a continue during the teardown left the terminal with echo off");
    } else if (WEXITSTATUS(status) == 95) {
        fail("the reader interrupted in its teardown did not read the line it was given");
    } else if (WEXITSTATUS(status) != 0) {
        fail("could not run the reader the teardown case interrupts");
    }
    close(report[0]);
    close(report[1]);
    close(master);
}

int main(int argc, char **argv)
{
    char template[] = "/tmp/pwauth_hosttest.XXXXXX";
    char *dir;

    /* Run as the helper of check_sigpipe_exec, which is this same program on
     * the far side of an exec: report whether SIGPIPE arrived at the new image
     * still carrying the disposition a transaction installed. */
    if (argc > 1 && strcmp(argv[1], HELPER_PIPE) == 0) {
        struct sigaction inherited;

        if (sigaction(SIGPIPE, NULL, &inherited) != 0) {
            return 91;
        }

        return inherited.sa_handler == SIG_DFL ? 0 : 92;
    }
    /* Run as the helper of check_background_prompt: become the session leader
     * that owns the terminal, then run the reader in a process group of its
     * own, which is what a shell's `&` produces. The reader must leave the
     * terminal alone and stop on SIGTTIN at its first read. */
    if (argc > 2 && strcmp(argv[1], HELPER_BG) == 0) {
        struct termios seen;
        char answer[2];
        pid_t reader;
        int slave;
        int status;

        (void)setsid();
        slave = open(argv[2], O_RDWR);
        if (slave < 0) {
            return 94;
        }
#ifdef TIOCSCTTY
        (void)ioctl(slave, TIOCSCTTY, 0);
#endif
        if (dup2(slave, 0) < 0 || dup2(slave, 2) < 0) {
            return 94;
        }
        if (slave > 2) {
            close(slave);
        }
        reader = fork();
        if (reader == 0) {
            char secret[64];

            (void)setpgid(0, 0);
            (void)pwauth_read_password("Password: ", secret, sizeof(secret));
            _exit(0);
        }
        if (reader < 0) {
            return 97;
        }
        if (waitpid(reader, &status, WUNTRACED) != reader) {
            return 97;
        }
        answer[0] = (char)(tcgetattr(0, &seen) == 0 && (seen.c_lflag & ECHO) != 0 ? 1 : 0);
        answer[1] = (char)(WIFSTOPPED(status) && WSTOPSIG(status) == SIGTTIN ? 1 : 0);
        (void)write(1, answer, sizeof(answer));
        kill(reader, SIGKILL);
        (void)waitpid(reader, &status, 0);

        return 0;
    }
    /* Run as the helper of check_restore_reentry: take a transaction so the
     * death handler leaves through exit and the reporter runs, prompt on the
     * pseudo-terminal named in argv[2] so echo is taken away, and arm the
     * interposed tcsetattr to fire a second death from inside the restore. */
    if (argc > 3 && strcmp(argv[1], HELPER_TTY) == 0) {
        char secret[64];
        int slave;

        if (atexit(report_restore_count) != 0) {
            return 93;
        }
        (void)setsid();
        slave = open(argv[2], O_RDWR);
        if (slave < 0) {
            return 94;
        }
#ifdef TIOCSCTTY
        (void)ioctl(slave, TIOCSCTTY, 0);
#endif
        /* Standard input and standard error, not standard output: the prompt
         * goes to fd 2 and must reach the terminal the case is watching, while
         * fd 1 stays the pipe the reporter answers on. */
        if (dup2(slave, 0) < 0 || dup2(slave, 2) < 0) {
            return 94;
        }
        if (slave > 2) {
            close(slave);
        }
        if (cred_txn_begin_at("pwauth_hosttest", argv[3], 0) != 0) {
            return 95;
        }
        /* The resume only touches the terminal while this process is its
         * foreground group, so the case that injects one must arrange that. */
        if (argc > 4 && strcmp(argv[4], "cont") == 0) {
            Tcs_raise_sig = SIGCONT;
        }
        Tcs_raise_at = 2;
        (void)pwauth_read_password("Password: ", secret, sizeof(secret));

        return 0;
    }
    /* Run as the helper of check_heir: take a transaction, fork, and let the
     * child leave through exit while this image leaves through _exit, so the
     * registered functions run once and in the heir. The heir must drop what it
     * inherited and must not touch the lock, which is this image's. */
    if (argc > 2 && strcmp(argv[1], HELPER_HEIR) == 0) {
        struct sigaction mine;
        pid_t heir;
        int status;

        if (atexit(report_heir) != 0) {
            return 93;
        }
        memset(&mine, 0, sizeof(mine));
        mine.sa_handler = keep_marker;
        sigemptyset(&mine.sa_mask);
        mine.sa_flags = 0;
        if (sigaction(SIGPIPE, &mine, NULL) != 0) {
            return 96;
        }
        if (cred_txn_begin_at("pwauth_hosttest", argv[2], 0) != 0) {
            return 94;
        }
        heir = fork();
        if (heir == 0) {
            exit(0);
        }
        if (heir < 0) {
            _exit(97);
        }
        (void)waitpid(heir, &status, 0);
        _exit(0);
    }
    /* Run as the helper of check_pipe_kept: end the transaction, then install a
     * disposition of this program's own, and let the reporter registered before
     * the library's say whether it survived the release that runs after it. */
    if (argc > 2 && strcmp(argv[1], HELPER_KEEP) == 0) {
        struct sigaction mine;

        if (atexit(report_pipe_kept) != 0) {
            return 93;
        }
        if (cred_txn_begin_at("pwauth_hosttest", argv[2], 0) != 0) {
            return 94;
        }
        if (cred_txn_end() != 0) {
            return 95;
        }
        memset(&mine, 0, sizeof(mine));
        mine.sa_handler = keep_marker;
        sigemptyset(&mine.sa_mask);
        mine.sa_flags = 0;
        if (sigaction(SIGPIPE, &mine, NULL) != 0) {
            return 96;
        }

        return 0;
    }
    /* Run as the helper of check_exit_mask: register the reporter first, so the
     * library's release (registered second, run first) has finished by the time
     * it looks, then leave through exit holding a transaction. */
    if (argc > 2 && strcmp(argv[1], HELPER_EXIT) == 0) {
        if (sigprocmask(SIG_BLOCK, NULL, &Start_mask) != 0) {
            return 96;
        }
        snprintf(Exit_lock, sizeof(Exit_lock), "%s", argv[2]);
        {
            int quiet = open("/dev/null", O_WRONLY);

            if (quiet >= 0) {
                dup2(quiet, 2);
                close(quiet);
            }
        }
        if (atexit(report_exit_mask) != 0) {
            return 93;
        }
        if (cred_txn_begin_at("pwauth_hosttest", argv[2], 0) != 0) {
            return 94;
        }
        Getpid_watch = 1;

        return 0;
    }
    snprintf(Self, sizeof(Self), "%s", argv[0]);
    if (sigprocmask(SIG_BLOCK, NULL, &Start_mask) != 0) {
        fprintf(stderr, "pwauth_hosttest: cannot read the signal mask to start from\n");

        return 1;
    }

    dir = mkdtemp(template);
    if (dir == NULL) {
        fprintf(stderr, "pwauth_hosttest: no temporary directory: %s\n", strerror(errno));

        return 1;
    }
    snprintf(Dir, sizeof(Dir), "%s", dir);
    snprintf(Target, sizeof(Target), "%s/" TARGET_BASE, Dir);
    snprintf(Target_tmp, sizeof(Target_tmp), "%s.tmp.%d", Target, (int)getpid());
    snprintf(Lock, sizeof(Lock), "%s/.pwlock", Dir);
    snprintf(Log, sizeof(Log), "%s/stderr", Dir);

    check_compare();
    check_salt_map();
    check_rewrite();
    check_masked();
    check_exit_mask();
    check_pipe_kept();
    check_heir();
    check_fork();
    check_lock_ownership();
    check_lock();
    check_report_blocks();
    check_pipe_during_acquire();
    check_unlisted_death();
    check_background_prompt();
    check_restore_reentry("death");
    check_restore_reentry("cont");
    check_death_set_installed();
    check_sigpipe();
    check_sigpipe_exec();
    check_release();
    check_temp_name();
    check_teardown();

    /* No case may end with a signal blocked. The mask is process-wide and
     * outlives the case that changed it, so a leak here is both a defect in the
     * library and the thing that would quietly disarm the comparisons in
     * check_masked; asserting it once at the end names whichever case is at
     * fault by the fact that the run got this far without saying so. */
    if (mask_changed(&Start_mask) != 0) {
        fail("the suite ended with the signal mask altered");
    }

    unlink(Target);
    unlink(Target_tmp);
    unlink(Lock);
    unlink(Log);
    rmdir(Dir);

    return Failed > 100 ? 100 : Failed;
}
