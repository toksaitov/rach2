/*
 * ledger.c: the durable claims of one machine's resources, kept in a file
 * that each change rewrites whole under the ledger's lock, and the recovery
 * that releases a claim only when its processes are proved to have ended.
 */
#include <errno.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include "win/winexec.h"
#else
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <unistd.h>
#if defined(__linux__) || defined(__rach2__)
#include <sys/utsname.h>
#include "boot_identity.h"
#endif
#if defined(__APPLE__)
#include <sys/sysctl.h>
#endif
#endif

#include "etude/ledger.h"
#include "authority.h"
#include "sha256.h"

/* The first line of a ledger of this format. */
#define LEDGER_MAGIC "etude-ledger 1"

/* The largest ledger file read, in bytes. */
#define LEDGER_FILE_MAX (64u << 20)

/* The longest boot identity, system name, state, resource and account. */
#define BOOT_MAX     128
#define SYSTEM_MAX   16
#define STATE_MAX    16
#define RES_NAME_MAX 64
#define ACCOUNT_MAX  64

/* The longest claim identity, as the history's identities. */
#define ID_MAX 255

/*
 * A claim's forecast, kept as two lines of the resource form so that an
 * older reader keeps them as resources that no limit names: the expected
 * elapsed time, and the time of day of the admission, both in microseconds.
 */
#define FORECAST_RES "expected_us"
#define ADMITTED_RES "admitted_utc_us"

/* The operating system whose processes this ledger's process can see. */
#if defined(_WIN32)
#define LEDGER_SYSTEM "win32"
#elif defined(__linux__)
#define LEDGER_SYSTEM "linux"
#elif defined(__APPLE__)
#define LEDGER_SYSTEM "darwin"
#else
#define LEDGER_SYSTEM "posix"
#endif

/* The roles of a claim's processes. */
static const char *const ROLES[] = {"keeper", "case", "child"};
enum { ROLE_KEEPER, ROLE_CASE, ROLE_CHILD, ROLES_N };

/* One process of a claim. */
struct lproc {
    int role;
    struct etude_pid id;
};

/*
 * One claim, as the ledger file records it, or a request that waits to
 * become one, whose state is waiting or blocked. A request holds nothing.
 */
struct claim {
    char *id;
    char system[SYSTEM_MAX];
    char boot[BOOT_MAX]; /* "-" where the system gives no boot identity */
    struct etude_pid owner;
    char account[ACCOUNT_MAX]; /* "" for a claim that etude_ledger_reserve made */
    char state[STATE_MAX];     /* reserved, running, uncertain, waiting or blocked */
    uint64_t ticket;           /* a request's place in the order of arrival; 0 for a claim */
    struct etude_ledger_res *res;
    size_t nres;
    struct lproc *proc;
    size_t nproc;
};

/* The claims of the ledger file, as one change reads and writes them. */
struct table {
    struct claim *c;
    size_t n;
};

/* What a caller asks for: claim `id` of `res` within the machine's `cap`. */
struct ask {
    const char *id;
    const char *account; /* "" for etude_ledger_reserve */
    const struct etude_ledger_res *res, *cap;
    size_t nres, ncap;
    int wait;             /* a request refused for now waits in the ledger */
    uint64_t forecast_us; /* its expected elapsed time; 0 when unknown */
    uint64_t now;         /* the time of day in microseconds; 0 when unknown */
};

struct etude_ledger {
    char *dir;
    uint64_t (*clock)(void *ctx); /* the time of day in microseconds; NULL for the system's */
    void *clock_ctx;
    struct etude_authority *authority;
    char boot[BOOT_MAX];
    struct etude_pid self;
#ifdef _WIN32
    HANDLE lock;
#else
    int lock;
#endif
};

/*
 * What recovery finds of a claim. J_ABANDONED is uncertain too: its owner
 * and its processes have ended, its cleanup unconfirmed, and only a person
 * can release it.
 */
enum judged { J_FREE, J_OWNED, J_LIVE, J_UNCERTAIN, J_ABANDONED };

/* What a check finds of one process. */
enum { P_GONE, P_RUNS, P_UNKNOWN };

/* ---- Names and paths. ---- */

/** Say whether `c` may be in a token of the class that `extra` widens. */
static int token_char(int c, const char *extra)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
           (c != '\0' && strchr(extra, c) != NULL);
}

/** Say whether `s` is a token of at most `max` characters of the class. */
static int is_token(const char *s, size_t max, const char *extra)
{
    size_t n;

    for (n = 0; s[n] != '\0'; n++)
        if (n == max || !token_char((unsigned char)s[n], extra))
            return 0;
    return n > 0;
}

/** Say whether `s` is a claim identity: the history's form of an identity. */
static int is_id(const char *s)
{
    return token_char((unsigned char)s[0], "") && is_token(s, ID_MAX, "._:/-");
}

/** Say whether `s` is a resource's name. */
static int is_name(const char *s)
{
    return is_token(s, RES_NAME_MAX - 1, "._:-");
}

/** Say whether `s` is an account's name. */
static int is_account(const char *s)
{
    return is_token(s, ACCOUNT_MAX - 1, "._@-");
}

/** Say whether state `s` is a request's, which holds nothing. */
static int is_request_state(const char *s)
{
    return strcmp(s, "waiting") == 0 || strcmp(s, "blocked") == 0;
}

/** Give a copy of `a`, `/` and `b` from malloc, or NULL. */
static char *join(const char *a, const char *b)
{
    size_t na = strlen(a), nb = strlen(b);
    char *s = malloc(na + nb + 2);

    if (s == NULL)
        return NULL;
    memcpy(s, a, na);
    s[na] = '/';
    memcpy(s + na + 1, b, nb + 1);
    return s;
}

/* ---- The system: files, the lock, the boot and processes. ---- */

#ifdef _WIN32

/** Give the errno value nearest the Win32 error `err`. */
static int win_errno(DWORD err)
{
    switch (err) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
        return ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_SHARING_VIOLATION:
        return EACCES;
    case ERROR_NOT_ENOUGH_MEMORY:
    case ERROR_OUTOFMEMORY:
        return ENOMEM;
    default:
        return EIO;
    }
}

/* How long a file that another process holds open is tried again, in ms. */
#define HELD_MS 5000

/**
 * Wait before another try at a file that Windows refused with `err`, after
 * `*waited` milliseconds of waiting. Another process, such as a virus scanner,
 * an indexer or a reader, may hold a ledger file open for a moment without
 * sharing it, and Windows then refuses to open, replace or remove it with
 * ERROR_ACCESS_DENIED or ERROR_SHARING_VIOLATION. Return 1 after the wait,
 * or 0 for another error or once HELD_MS have passed.
 */
static int held_wait(DWORD err, DWORD *waited)
{
    DWORD step = *waited < 10 ? 10 : *waited > 250 ? 250 : *waited;

    if ((err != ERROR_ACCESS_DENIED && err != ERROR_SHARING_VIOLATION) || *waited >= HELD_MS)
        return 0;
    Sleep(step);
    *waited += step;
    return 1;
}

/** Open `path` as CreateFileW does; the error is in errno. */
static HANDLE open_file(const char *path, DWORD access, DWORD share, DWORD how)
{
    wchar_t *w = etude_win_long_path(path);
    DWORD waited = 0;
    HANDLE h;

    if (w == NULL) {
        errno = ENOMEM;
        return INVALID_HANDLE_VALUE;
    }
    do
        h = CreateFileW(w, access, share, NULL, how, FILE_ATTRIBUTE_NORMAL, NULL);
    while (h == INVALID_HANDLE_VALUE && held_wait(GetLastError(), &waited));
    if (h == INVALID_HANDLE_VALUE)
        errno = win_errno(GetLastError());
    free(w);
    return h;
}

/** Make directory `path` unless it is there; 0 or an error number. */
static int dir_make(const char *path)
{
    wchar_t *w = etude_win_long_path(path);
    DWORD err = 0;

    if (w == NULL)
        return ENOMEM;
    if (!CreateDirectoryW(w, NULL) && GetLastError() != ERROR_ALREADY_EXISTS)
        err = GetLastError();
    free(w);
    return err == 0 ? 0 : win_errno(err);
}

/** Read all of `path` into `*data`, NUL-terminated; 0 or an error number. */
static int file_read(const char *path, char **data, size_t *len)
{
    HANDLE h = open_file(path, GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                         OPEN_EXISTING);
    LARGE_INTEGER size;
    DWORD got;
    size_t n = 0;
    char *buf;

    if (h == INVALID_HANDLE_VALUE)
        return errno;
    if (!GetFileSizeEx(h, &size) || size.QuadPart > (LONGLONG)LEDGER_FILE_MAX) {
        CloseHandle(h);
        return EFBIG;
    }
    buf = malloc((size_t)size.QuadPart + 1);
    if (buf == NULL) {
        CloseHandle(h);
        return ENOMEM;
    }
    while (n < (size_t)size.QuadPart) {
        if (!ReadFile(h, buf + n, (DWORD)((size_t)size.QuadPart - n), &got, NULL) || got == 0)
            break;
        n += got;
    }
    CloseHandle(h);
    buf[n] = '\0';
    *data = buf;
    *len = n;
    return 0;
}

/**
 * Replace `path` with `len` bytes of `data`, written to `path.new`, flushed,
 * and moved over it; 0 or an error number.
 */
static int file_replace(const char *path, const char *data, size_t len)
{
    size_t np = strlen(path);
    char *tmp = malloc(np + 5);
    wchar_t *wt = NULL, *wp = NULL;
    DWORD put, err = 0, waited = 0;
    HANDLE h;
    size_t n = 0;

    if (tmp == NULL)
        return ENOMEM;
    memcpy(tmp, path, np);
    memcpy(tmp + np, ".new", 5);
    h = open_file(tmp, GENERIC_WRITE, 0, CREATE_ALWAYS);
    if (h == INVALID_HANDLE_VALUE) {
        free(tmp);
        return errno;
    }
    while (n < len && err == 0) {
        if (!WriteFile(h, data + n, (DWORD)(len - n), &put, NULL))
            err = GetLastError();
        n += put;
    }
    if (err == 0 && !FlushFileBuffers(h))
        err = GetLastError();
    CloseHandle(h);
    wt = etude_win_long_path(tmp);
    wp = etude_win_long_path(path);
    if (err == 0 && (wt == NULL || wp == NULL))
        err = ERROR_NOT_ENOUGH_MEMORY;
    while (err == 0 && !MoveFileExW(wt, wp, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        err = GetLastError();
        if (held_wait(err, &waited))
            err = 0;
    }
    free(wt);
    free(wp);
    free(tmp);
    return err == 0 ? 0 : win_errno(err);
}

/** Remove `path` if it is there; 0 or an error number. */
static int file_remove(const char *path)
{
    wchar_t *w = etude_win_long_path(path);
    DWORD err = 0, waited = 0;

    if (w == NULL)
        return ENOMEM;
    while (!DeleteFileW(w) && (err = GetLastError()) != ERROR_FILE_NOT_FOUND) {
        if (!held_wait(err, &waited))
            break;
        err = 0;
    }
    if (err == ERROR_FILE_NOT_FOUND)
        err = 0;
    free(w);
    return err == 0 ? 0 : win_errno(err);
}

/** Open the lock file of ledger `l`; 0 or an error number. */
static int lock_open(struct etude_ledger *l, const char *path)
{
    l->lock = open_file(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
                        OPEN_ALWAYS);
    return l->lock == INVALID_HANDLE_VALUE ? errno : 0;
}

/** Take the ledger's lock, waiting for it; 0 or an error number. */
static int lock_take(struct etude_ledger *l)
{
    OVERLAPPED ov;

    memset(&ov, 0, sizeof(ov));
    return LockFileEx(l->lock, LOCKFILE_EXCLUSIVE_LOCK, 0, 1, 0, &ov) ? 0
                                                                      : win_errno(GetLastError());
}

/** Give the ledger's lock back. */
static void lock_give(struct etude_ledger *l)
{
    OVERLAPPED ov;

    memset(&ov, 0, sizeof(ov));
    UnlockFileEx(l->lock, 0, 1, 0, &ov);
}

/** Close the lock file. */
static void lock_close(struct etude_ledger *l)
{
    if (l->lock != INVALID_HANDLE_VALUE)
        CloseHandle(l->lock);
}

/** Fill `boot` with the identity of this boot; Windows gives none. */
static void boot_read(char *boot)
{
    strcpy(boot, "-");
}

#else

/** Open `path` as open(2) does, closed on exec. */
static int open_file(const char *path, int flags, mode_t mode)
{
    int fd;

    do {
        fd = open(path, flags | O_CLOEXEC, mode);
    } while (fd < 0 && errno == EINTR);
    return fd;
}

/** Make directory `path` unless it is there; 0 or an error number. */
static int dir_make(const char *path)
{
    return mkdir(path, 0777) == 0 || errno == EEXIST ? 0 : errno;
}

/** Read all of `path` into `*data`, NUL-terminated; 0 or an error number. */
static int file_read(const char *path, char **data, size_t *len)
{
    int fd = open_file(path, O_RDONLY, 0), e;
    struct stat st;
    size_t n = 0;
    ssize_t got;
    char *buf;

    if (fd < 0)
        return errno;
    if (fstat(fd, &st) != 0 || st.st_size < 0 || (uint64_t)st.st_size > LEDGER_FILE_MAX) {
        close(fd);
        return EFBIG;
    }
    buf = malloc((size_t)st.st_size + 1);
    if (buf == NULL) {
        close(fd);
        return ENOMEM;
    }
    while (n < (size_t)st.st_size) {
        got = read(fd, buf + n, (size_t)st.st_size - n);
        if (got < 0 && errno == EINTR)
            continue;
        if (got <= 0)
            break;
        n += (size_t)got;
    }
    e = errno;
    close(fd);
    if (n < (size_t)st.st_size) {
        free(buf);
        return e != 0 ? e : EIO;
    }
    buf[n] = '\0';
    *data = buf;
    *len = n;
    return 0;
}

/** Write `len` bytes of `data` to `fd`; 0 or an error number. */
static int write_all(int fd, const char *data, size_t len)
{
    ssize_t n;

    while (len > 0) {
        n = write(fd, data, len);
        if (n < 0 && errno == EINTR)
            continue;
        if (n <= 0)
            return n < 0 ? errno : EIO;
        data += n;
        len -= (size_t)n;
    }
    return 0;
}

/**
 * Replace `path` with `len` bytes of `data`, written to `path.new`, synced,
 * and renamed over it, with the directory synced after; 0 or an error number.
 */
static int file_replace(const char *path, const char *data, size_t len)
{
    size_t np = strlen(path);
    char *tmp = malloc(np + 5), *slash;
    int fd, e;

    if (tmp == NULL)
        return ENOMEM;
    memcpy(tmp, path, np);
    memcpy(tmp + np, ".new", 5);
    fd = open_file(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0666);
    if (fd < 0) {
        e = errno;
        free(tmp);
        return e;
    }
    e = write_all(fd, data, len);
    if (e == 0 && fsync(fd) != 0)
        e = errno;
    if (close(fd) != 0 && e == 0)
        e = errno;
    if (e == 0 && rename(tmp, path) != 0)
        e = errno;
    if (e == 0) {
        memcpy(tmp, path, np + 1);
        slash = strrchr(tmp, '/');
        if (slash != NULL)
            *slash = '\0';
        fd = open_file(slash != NULL ? tmp : ".", O_RDONLY, 0);
        if (fd >= 0) {
            if (fsync(fd) != 0 && errno != EINVAL)
                e = errno;
            close(fd);
        }
    }
    free(tmp);
    return e;
}

/** Remove `path` if it is there; 0 or an error number. */
static int file_remove(const char *path)
{
    return unlink(path) == 0 || errno == ENOENT ? 0 : errno;
}

/** Open the lock file of ledger `l`; 0 or an error number. */
static int lock_open(struct etude_ledger *l, const char *path)
{
    l->lock = open_file(path, O_RDWR | O_CREAT, 0666);
    return l->lock < 0 ? errno : 0;
}

/** Lock or unlock the whole lock file as `type` says; 0 or an error number. */
static int lock_set(struct etude_ledger *l, short type, int cmd)
{
    struct flock fl;

    memset(&fl, 0, sizeof(fl));
    fl.l_type = type;
    fl.l_whence = SEEK_SET;
    while (fcntl(l->lock, cmd, &fl) != 0)
        if (errno != EINTR)
            return errno;
    return 0;
}

/** Take the ledger's lock, waiting for it; 0 or an error number. */
static int lock_take(struct etude_ledger *l)
{
    return lock_set(l, F_WRLCK, F_SETLKW);
}

/** Give the ledger's lock back. */
static void lock_give(struct etude_ledger *l)
{
    lock_set(l, F_UNLCK, F_SETLK);
}

/** Close the lock file. */
static void lock_close(struct etude_ledger *l)
{
    if (l->lock >= 0)
        close(l->lock);
}

/**
 * Fill `boot` with the identity of this boot, which another boot does not
 * share: Linux's random boot_id, Rach2's boot-local token, or macOS's boot
 * session UUID. Elsewhere it is "-", which matches no boot.
 */
static void boot_read(char *boot)
{
#if defined(__linux__) || defined(__rach2__)
    struct utsname kernel;
    if (uname(&kernel)) {
        boot[0] = '\0';
    } else if (etude_boot_is_rach2(kernel.sysname, kernel.release, kernel.version)) {
        if (etude_boot_file_read("/dev/shm/etude-guest-boot", boot, BOOT_MAX))
            boot[0] = '\0';
    } else {
        FILE *f = fopen("/proc/sys/kernel/random/boot_id", "r");
        if (f == NULL || fgets(boot, BOOT_MAX, f) == NULL)
            boot[0] = '\0';
        if (f != NULL)
            fclose(f);
        boot[strcspn(boot, "\n")] = '\0';
    }
#elif defined(__APPLE__)
    size_t len = BOOT_MAX;

    if (sysctlbyname("kern.bootsessionuuid", boot, &len, NULL, 0) != 0)
        len = 0;
    boot[len < BOOT_MAX ? len : BOOT_MAX - 1] = '\0';
#else
    boot[0] = '\0';
#endif
    if (!is_token(boot, BOOT_MAX - 1, "-"))
        strcpy(boot, "-");
}

/**
 * Say whether process group `pgid` may have a member. A group number is not
 * reused while the group has one, so a group that answers is the case's or
 * a later one; either keeps the claim.
 */
static int group_runs(long pgid)
{
    return kill((pid_t)-pgid, 0) == 0 || errno != ESRCH;
}

#endif

/** Give the time of day in microseconds since the epoch, or 0. */
static uint64_t wall_us(void)
{
#ifdef _WIN32
    FILETIME ft;
    ULARGE_INTEGER u;

    GetSystemTimeAsFileTime(&ft);
    u.LowPart = ft.dwLowDateTime;
    u.HighPart = ft.dwHighDateTime;
    return u.QuadPart / 10u > 11644473600000000ull ? u.QuadPart / 10u - 11644473600000000ull : 0;
#else
    struct timeval tv;

    if (gettimeofday(&tv, NULL) != 0 || tv.tv_sec < 0)
        return 0;
    return (uint64_t)tv.tv_sec * 1000000u + (uint64_t)tv.tv_usec;
#endif
}

/**
 * Check process `id`: P_RUNS when the same process runs, P_GONE when it has
 * ended or its PID now names another process, P_UNKNOWN when neither can be
 * told.
 */
static int process_check(const struct etude_pid *id)
{
    struct etude_pid now;
    int state;

    memset(&now, 0, sizeof(now));
    state = etude_process_state(id->pid, &now);
    if (state == 0)
        return P_GONE;
    if (state < 0 || !id->start_known || !now.start_known)
        return P_UNKNOWN;
    return now.start == id->start ? P_RUNS : P_GONE;
}

/**
 * Check process `p` of a claim as process_check does, a case counting as
 * running on POSIX systems while its process group has a member. A Windows
 * job holds the case's descendants, and ends with the case.
 */
static int member_check(const struct lproc *p)
{
#ifndef _WIN32
    if (p->role == ROLE_CASE && group_runs(p->id.pid))
        return P_RUNS;
#endif
    return process_check(&p->id);
}

/* ---- The ledger file. ---- */

/** Free the memory of claim `c`. */
static void claim_free(struct claim *c)
{
    size_t i;

    free(c->id);
    for (i = 0; i < c->nres; i++)
        free((char *)c->res[i].name);
    free(c->res);
    free(c->proc);
    memset(c, 0, sizeof(*c));
}

/** Free the claims of table `t`. */
static void table_free(struct table *t)
{
    size_t i;

    for (i = 0; i < t->n; i++)
        claim_free(&t->c[i]);
    free(t->c);
    t->c = NULL;
    t->n = 0;
}

/** Give the claim of table `t` whose identity is `id`, or NULL. */
static struct claim *table_find(struct table *t, const char *id)
{
    size_t i;

    for (i = 0; i < t->n; i++)
        if (strcmp(t->c[i].id, id) == 0)
            return &t->c[i];
    return NULL;
}

/** Remove claim `c` from table `t`. */
static void table_remove(struct table *t, struct claim *c)
{
    size_t i = (size_t)(c - t->c);

    claim_free(c);
    memmove(&t->c[i], &t->c[i + 1], (t->n - i - 1) * sizeof(t->c[0]));
    t->n--;
}

/** Add an empty claim to table `t`; NULL when memory ran out. */
static struct claim *table_add(struct table *t)
{
    struct claim *c = realloc(t->c, (t->n + 1) * sizeof(*c));

    if (c == NULL)
        return NULL;
    t->c = c;
    memset(&c[t->n], 0, sizeof(c[0]));
    return &c[t->n++];
}

/** Add process `id` in role `role` to claim `c`; 0 or ENOMEM. */
static int claim_add_proc(struct claim *c, int role, const struct etude_pid *id)
{
    struct lproc *p = realloc(c->proc, (c->nproc + 1) * sizeof(*p));

    if (p == NULL)
        return ENOMEM;
    c->proc = p;
    p[c->nproc].role = role;
    p[c->nproc++].id = *id;
    return 0;
}

/** Add resource `name` of `amount` to claim `c`; 0 or an error number. */
static int claim_add_res(struct claim *c, const char *name, uint64_t amount)
{
    struct etude_ledger_res *r;
    size_t i;
    char *copy;

    if (!is_name(name))
        return EINVAL;
    for (i = 0; i < c->nres; i++)
        if (strcmp(c->res[i].name, name) == 0)
            return EINVAL;
    r = realloc(c->res, (c->nres + 1) * sizeof(*r));
    if (r == NULL)
        return ENOMEM;
    c->res = r;
    copy = malloc(strlen(name) + 1);
    if (copy == NULL)
        return ENOMEM;
    strcpy(copy, name);
    r[c->nres].name = copy;
    r[c->nres++].amount = amount;
    return 0;
}

/**
 * Split line `s` at single spaces into at most `max` fields.
 *
 * @return the count of fields, or -1 for an empty field or too many.
 */
static int split(char *s, char **f, int max)
{
    int n = 0;

    for (;;) {
        if (n == max || *s == '\0' || *s == ' ')
            return -1;
        f[n++] = s;
        s = strchr(s, ' ');
        if (s == NULL)
            return n;
        *s++ = '\0';
    }
}

/** Read decimal `s` into `*v`; 0, or -1 when it is not one. */
static int number(const char *s, uint64_t *v)
{
    uint64_t n = 0;

    if (*s == '\0' || (s[0] == '0' && s[1] != '\0'))
        return -1;
    for (; *s != '\0'; s++) {
        if (*s < '0' || *s > '9' || n > (UINT64_MAX - (uint64_t)(*s - '0')) / 10u)
            return -1;
        n = n * 10u + (uint64_t)(*s - '0');
    }
    *v = n;
    return 0;
}

/** Read the three fields of a process's identity; 0 or -1. */
static int read_pid(char **f, struct etude_pid *id)
{
    uint64_t pid, start, known;

    if (number(f[0], &pid) != 0 || number(f[1], &start) != 0 || number(f[2], &known) != 0 ||
        pid == 0 || pid > 0x7fffffffu || known > 1)
        return -1;
    id->pid = (long)pid;
    id->start = start;
    id->start_known = (int)known;
    return 0;
}

/** Say whether claim `c`, read whole, has every part and sound values. */
static int claim_sound(const struct claim *c)
{
    if (c->system[0] == '\0' || c->boot[0] == '\0' || c->owner.pid == 0)
        return 0;
    if (is_request_state(c->state))
        return c->nproc == 0 && c->ticket != 0 && c->account[0] != '\0';
    return c->ticket == 0 &&
           (strcmp(c->state, "reserved") == 0
                ? c->nproc == 0
                : strcmp(c->state, "running") == 0 || strcmp(c->state, "uncertain") == 0);
}

/**
 * Set the text field `field`, of `max` bytes, to `value`, a token or `extra`,
 * once.
 *
 * @return 0, or -1 for a field already set or a value that is not a token.
 */
static int set_text(char *field, size_t max, const char *value, const char *extra)
{
    if (field[0] != '\0' || !is_token(value, max - 1, extra))
        return -1;
    strcpy(field, value);
    return 0;
}

/**
 * Read the fields `f` of a process line, its role and its identity, into the
 * claim `c` being read.
 *
 * @return 0, or -1 for a line that does not belong.
 */
static int process_line(struct claim *c, char **f)
{
    struct etude_pid id;
    int role;

    for (role = 0; role < ROLES_N; role++)
        if (strcmp(f[0], ROLES[role]) == 0)
            break;
    if (role == ROLES_N || read_pid(f + 1, &id) != 0)
        return -1;
    return claim_add_proc(c, role, &id) != 0 ? -1 : 0;
}

/**
 * Read one line, `f` its `n` fields, of the claim `c` being read.
 *
 * @return 0, or -1 for a line that does not belong.
 */
static int claim_line(struct claim *c, char **f, int n)
{
    uint64_t amount;

    if (n == 2 && strcmp(f[0], "system") == 0)
        return set_text(c->system, SYSTEM_MAX, f[1], "");
    if (n == 2 && strcmp(f[0], "boot") == 0)
        return set_text(c->boot, BOOT_MAX, f[1], "-");
    if (n == 2 && strcmp(f[0], "state") == 0)
        return set_text(c->state, STATE_MAX, f[1], "");
    if (n == 2 && strcmp(f[0], "account") == 0)
        return set_text(c->account, ACCOUNT_MAX, f[1], "._@-");
    if (n == 2 && strcmp(f[0], "ticket") == 0)
        return c->ticket != 0 || number(f[1], &c->ticket) != 0 || c->ticket == 0 ? -1 : 0;
    if (n == 4 && strcmp(f[0], "owner") == 0)
        return c->owner.pid != 0 ? -1 : read_pid(f + 1, &c->owner);
    if (n == 3 && strcmp(f[0], "resource") == 0)
        return number(f[2], &amount) != 0 || claim_add_res(c, f[1], amount) != 0 ? -1 : 0;
    if (n == 5 && strcmp(f[0], "process") == 0)
        return process_line(c, f + 1);
    return -1;
}

/**
 * Parse the ledger file's text `s` into table `t`.
 *
 * @return 0, or EBADMSG for a file that does not parse or has two blocked
 *         requests, or ENOMEM.
 */
static int table_parse(struct table *t, char *s)
{
    struct claim *c = NULL;
    char *line, *nl, *f[5];
    int n, first = 1, blocked = 0;

    for (line = s; *line != '\0'; line = nl + 1) {
        nl = strchr(line, '\n');
        if (nl == NULL)
            return EBADMSG;
        *nl = '\0';
        if (first) {
            if (strcmp(line, LEDGER_MAGIC) != 0)
                return EBADMSG;
            first = 0;
            continue;
        }
        n = split(line, f, 5);
        if (c == NULL) {
            if (n != 2 || strcmp(f[0], "claim") != 0 || !is_id(f[1]) || table_find(t, f[1]) != NULL)
                return EBADMSG;
            c = table_add(t);
            if (c == NULL || (c->id = malloc(strlen(f[1]) + 1)) == NULL)
                return ENOMEM;
            strcpy(c->id, f[1]);
        } else if (n == 1 && strcmp(f[0], "end") == 0) {
            if (!claim_sound(c) || (strcmp(c->state, "blocked") == 0 && blocked++ > 0))
                return EBADMSG;
            c = NULL;
        } else if (n < 0 || claim_line(c, f, n) != 0) {
            return EBADMSG;
        }
    }
    return first || c != NULL ? EBADMSG : 0;
}

/* A growing text. */
struct text {
    char *s;
    size_t len, size;
    int err;
};

/** Append to text `b` as printf does. */
static void text_add(struct text *b, const char *fmt, ...)
{
    va_list ap;
    size_t want;
    char *s;
    int n;

    if (b->err != 0)
        return;
    va_start(ap, fmt);
    n = vsnprintf(b->s + b->len, b->size - b->len, fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->err = EIO;
        return;
    }
    if ((size_t)n < b->size - b->len) {
        b->len += (size_t)n;
        return;
    }
    want = b->size * 2 + (size_t)n + 1;
    s = realloc(b->s, want);
    if (s == NULL) {
        b->err = ENOMEM;
        return;
    }
    b->s = s;
    b->size = want;
    va_start(ap, fmt);
    vsnprintf(b->s + b->len, b->size - b->len, fmt, ap);
    va_end(ap);
    b->len += (size_t)n;
}

/** Append process identity `id` to text `b`. */
static void text_pid(struct text *b, const struct etude_pid *id)
{
    text_add(b, " %ld %llu %d\n", id->pid, (unsigned long long)id->start, id->start_known);
}

/** Write table `t` as the ledger file `path`; 0 or an error number. */
static int table_write(const struct table *t, const char *path)
{
    struct text b;
    size_t i, j;
    int e;

    memset(&b, 0, sizeof(b));
    b.s = malloc(256);
    if (b.s == NULL)
        return ENOMEM;
    b.size = 256;
    text_add(&b, "%s\n", LEDGER_MAGIC);
    for (i = 0; i < t->n; i++) {
        const struct claim *c = &t->c[i];

        text_add(&b, "claim %s\nsystem %s\nboot %s\nowner", c->id, c->system, c->boot);
        text_pid(&b, &c->owner);
        if (c->account[0] != '\0')
            text_add(&b, "account %s\n", c->account);
        text_add(&b, "state %s\n", c->state);
        if (c->ticket != 0)
            text_add(&b, "ticket %llu\n", (unsigned long long)c->ticket);
        for (j = 0; j < c->nres; j++)
            text_add(&b, "resource %s %llu\n", c->res[j].name,
                     (unsigned long long)c->res[j].amount);
        for (j = 0; j < c->nproc; j++) {
            text_add(&b, "process %s", ROLES[c->proc[j].role]);
            text_pid(&b, &c->proc[j].id);
        }
        text_add(&b, "end\n");
    }
    e = b.err != 0 ? b.err : file_replace(path, b.s, b.len);
    free(b.s);
    return e;
}

/* ---- Changes under the lock. ---- */

/** Give the path of a file of the ledger, from malloc, or NULL. */
static char *ledger_path(const struct etude_ledger *l, const char *name)
{
    return join(l->dir, name);
}

/**
 * Take the lock and read the ledger file into `t`.
 *
 * @return 0 with the lock held, or an error number with it given back.
 */
static int begin(struct etude_ledger *l, struct table *t)
{
    char *path = ledger_path(l, "claims"), *data = NULL;
    size_t len;
    int e;

    memset(t, 0, sizeof(*t));
    if (path == NULL)
        return ENOMEM;
    e = lock_take(l);
    if (e == 0) {
        e = file_read(path, &data, &len);
        if (e == ENOENT)
            e = 0;
        else if (e == 0 && strlen(data) != len)
            e = EBADMSG;
        else if (e == 0)
            e = table_parse(t, data);
        free(data);
        if (e != 0) {
            table_free(t);
            lock_give(l);
        }
    }
    free(path);
    return e;
}

/** Write `t` as the ledger file when `e` is 0, free it, give the lock back. */
static int end(struct etude_ledger *l, struct table *t, int e, int changed)
{
    char *path;

    if (e == 0 && changed) {
        path = ledger_path(l, "claims");
        e = path == NULL ? ENOMEM : table_write(t, path);
        free(path);
    }
    table_free(t);
    lock_give(l);
    return e;
}

/** Say whether claim `c` is owned by this process. */
static int is_mine(const struct etude_ledger *l, const struct claim *c)
{
    return strcmp(c->system, LEDGER_SYSTEM) == 0 && c->owner.pid == l->self.pid &&
           c->owner.start == l->self.start && c->owner.start_known == l->self.start_known;
}

/**
 * Give the path of the verdict file of claim `id`, from malloc, or NULL: the
 * first 32 hexadecimal digits of the SHA-256 of the identity, under
 * `verdicts/`, since an identity may have characters a file name cannot.
 */
static char *verdict_file(const struct etude_ledger *l, const char *id)
{
    unsigned char digest[32];
    struct sha256 s;
    char hex[65], name[48];

    sha256_init(&s);
    sha256_update(&s, (const unsigned char *)id, strlen(id));
    sha256_final(&s, digest);
    sha256_hex(digest, hex);
    snprintf(name, sizeof(name), "verdicts/%.32s", hex);
    return ledger_path(l, name);
}

/** Say whether the verdict file of claim `id` records a confirmed cleanup. */
static int verdict_confirmed(const struct etude_ledger *l, const char *id)
{
    char *path = verdict_file(l, id), *data = NULL;
    size_t len;
    int ok;

    if (path == NULL || file_read(path, &data, &len) != 0) {
        free(path);
        return 0;
    }
    ok = len == strlen(ETUDE_VERDICT_CONFIRMED) && memcmp(data, ETUDE_VERDICT_CONFIRMED, len) == 0;
    free(data);
    free(path);
    return ok;
}

/** Remove the verdict file of claim `id`. */
static void verdict_remove(const struct etude_ledger *l, const char *id)
{
    char *path = verdict_file(l, id);

    if (path != NULL)
        file_remove(path);
    free(path);
}

/**
 * Judge claim `c`: J_FREE when it is proved free, J_OWNED when its owner
 * runs, and otherwise J_LIVE, J_UNCERTAIN or J_ABANDONED with the reason in
 * `*why`.
 */
static enum judged judge(const struct etude_ledger *l, const struct claim *c, const char **why)
{
    int runs = 0, unknown = 0;
    size_t i;

    *why = "";
    if (strcmp(c->system, LEDGER_SYSTEM) != 0) {
        *why = "another operating system recorded it";
        return J_UNCERTAIN;
    }
    if (strcmp(c->boot, "-") != 0 && strcmp(l->boot, "-") != 0 && strcmp(c->boot, l->boot) != 0)
        return J_FREE;
    switch (process_check(&c->owner)) {
    case P_RUNS:
        return J_OWNED;
    case P_UNKNOWN:
        *why = "its owner cannot be identified";
        return J_UNCERTAIN;
    }
    if (strcmp(c->state, "reserved") == 0 || is_request_state(c->state))
        return J_FREE;
    for (i = 0; i < c->nproc; i++) {
        switch (member_check(&c->proc[i])) {
        case P_RUNS:
            runs = 1;
            break;
        case P_UNKNOWN:
            unknown = 1;
            break;
        }
    }
    if (runs) {
        *why = "a process of it runs";
        return J_LIVE;
    }
    if (unknown) {
        *why = "a process of it cannot be identified";
        return J_UNCERTAIN;
    }
    if (strcmp(c->state, "uncertain") == 0) {
        *why = "its owner found the cleanup unconfirmed";
        return J_ABANDONED;
    }
    if (strcmp(c->system, "win32") != 0 && !verdict_confirmed(l, c->id)) {
        *why = "its keeper did not confirm the cleanup";
        return J_ABANDONED;
    }
    return J_FREE;
}

/**
 * Give in `v` what keeps live claim `c` live: a process of it that runs, or
 * when none does, the process group of its case, which has a member.
 */
static void note_running(const struct claim *c, struct etude_ledger_claim *v)
{
    size_t i;

    for (i = 0; i < c->nproc && v->running == 0; i++)
        if (process_check(&c->proc[i].id) == P_RUNS)
            v->running = c->proc[i].id.pid;
    for (i = 0; i < c->nproc && v->running == 0 && v->group == 0; i++)
        if (c->proc[i].role == ROLE_CASE && member_check(&c->proc[i]) == P_RUNS)
            v->group = c->proc[i].id.pid;
}

/**
 * Remove from `t` every claim that judge finds free, giving their identities
 * in `*gone` for swept; a claim that memory does not allow to note stays.
 *
 * @return the count of claims removed.
 */
static size_t sweep(const struct etude_ledger *l, struct table *t, char ***gone)
{
    const char *why;
    size_t i = 0, n = 0;
    char **ids = NULL, **more;

    while (i < t->n) {
        if (judge(l, &t->c[i], &why) != J_FREE ||
            (more = realloc(ids, (n + 1) * sizeof(*ids))) == NULL) {
            i++;
            continue;
        }
        ids = more;
        ids[n++] = t->c[i].id;
        t->c[i].id = NULL;
        table_remove(t, &t->c[i]);
    }
    *gone = ids;
    return n;
}

/**
 * Free the identities of the `n` claims `ids` that sweep removed, and remove
 * their verdict files when the ledger file without them is `written`.
 */
static void swept(const struct etude_ledger *l, char **ids, size_t n, int written)
{
    size_t i;

    for (i = 0; i < n; i++) {
        if (written)
            verdict_remove(l, ids[i]);
        free(ids[i]);
    }
    free(ids);
}

/** Give `e` as errno and -1, or 0 for no error. */
static int fail(int e)
{
    if (e == 0)
        return 0;
    errno = e;
    return -1;
}

/* ---- The interface. ---- */

struct etude_ledger *etude_ledger_open(const char *dir)
{
    struct etude_ledger *l = calloc(1, sizeof(*l));
    char *path = NULL;
    int e;

    if (l == NULL)
        return NULL;
#ifdef _WIN32
    l->lock = INVALID_HANDLE_VALUE;
#else
    l->lock = -1;
#endif
    if (etude_authority_open(dir, &l->authority)) {
        e = errno;
        goto out;
    }
    l->dir = malloc(strlen(dir) + 1);
    if (l->dir == NULL) {
        e = ENOMEM;
        goto out;
    }
    strcpy(l->dir, dir);
    boot_read(l->boot);
#ifdef _WIN32
    l->self.pid = (long)GetCurrentProcessId();
#else
    l->self.pid = (long)getpid();
#endif
    if (etude_process_state(l->self.pid, &l->self) != 1)
        l->self.start_known = 0;
    e = dir_make(dir);
    if (e == 0 && (path = ledger_path(l, "verdicts")) == NULL)
        e = ENOMEM;
    if (e == 0)
        e = dir_make(path);
    free(path);
    path = NULL;
    if (e == 0 && (path = ledger_path(l, "lock")) == NULL)
        e = ENOMEM;
    if (e == 0)
        e = lock_open(l, path);
    free(path);
out:
    if (e == 0)
        return l;
    etude_ledger_close(l);
    errno = e;
    return NULL;
}

void etude_ledger_close(struct etude_ledger *l)
{
    if (l == NULL)
        return;
    lock_close(l);
    etude_authority_close(l->authority);
    free(l->dir);
    free(l);
}

int etude_ledger_verdict_path(const struct etude_ledger *l, const char *id, char *buf, size_t size)
{
    char *path = verdict_file(l, id);
    int e = path == NULL ? ENOMEM : strlen(path) >= size ? ERANGE : 0;

    if (e == 0)
        strcpy(buf, path);
    free(path);
    return fail(e);
}

/** Give the amount of resource `name` in `res`, or 0 when it has none. */
static uint64_t amount_of(const struct etude_ledger_res *res, size_t nres, const char *name)
{
    size_t j;

    for (j = 0; j < nres; j++)
        if (strcmp(res[j].name, name) == 0)
            return res[j].amount;
    return 0;
}

/** Add `b` to `*a`; 0, or -1 when the sum overflows. */
static int add(uint64_t *a, uint64_t b)
{
    if (*a > UINT64_MAX - b)
        return -1;
    *a += b;
    return 0;
}

/**
 * Give in `*sum` how much of resource `name` the claims of `t` hold, only
 * those that judge finds abandoned when `stuck` is set; requests hold none.
 *
 * @return 0, or -1 when the sum overflows.
 */
static int held(const struct etude_ledger *l, const struct table *t, const char *name, int stuck,
                uint64_t *sum)
{
    const char *why;
    size_t i;

    *sum = 0;
    for (i = 0; i < t->n; i++) {
        const struct claim *c = &t->c[i];

        if (is_request_state(c->state))
            continue;
        if (stuck && judge(l, c, &why) != J_ABANDONED)
            continue;
        if (add(sum, amount_of(c->res, c->nres, name)) != 0)
            return -1;
    }
    return 0;
}

/**
 * Give the time of day, in microseconds, at which claim `c` is expected to
 * end: its admission plus its expected elapsed time, or, once that has
 * passed, `now` plus that time again, so that a claim running late still
 * leaves a window ahead of it. A claim with no forecast, or uncertain, is
 * taken never to end: UINT64_MAX.
 */
static uint64_t claim_end(const struct claim *c, uint64_t now)
{
    uint64_t span = amount_of(c->res, c->nres, FORECAST_RES);
    uint64_t from = amount_of(c->res, c->nres, ADMITTED_RES);

    if (span == 0 || from == 0 || strcmp(c->state, "uncertain") == 0)
        return UINT64_MAX;
    if (from <= now && span <= now - from)
        return span > UINT64_MAX - now ? UINT64_MAX : now + span;
    return from > UINT64_MAX - span ? UINT64_MAX : from + span;
}

/**
 * Give in `*sum` how much of resource `name` the claims of `t` that are
 * expected to run past time `at` hold, as claim_end judges at `now`.
 *
 * @return 0, or -1 when the sum overflows.
 */
static int held_after(const struct table *t, const char *name, uint64_t at, uint64_t now,
                      uint64_t *sum)
{
    size_t i;

    *sum = 0;
    for (i = 0; i < t->n; i++) {
        const struct claim *c = &t->c[i];

        if (!is_request_state(c->state) && claim_end(c, now) > at &&
            add(sum, amount_of(c->res, c->nres, name)) != 0)
            return -1;
    }
    return 0;
}

/**
 * Say whether `extra`, when not NULL, and request `keep` fit within the
 * limits of `a` beside the claims of `t` expected to run past time `at`;
 * `extra` is counted only on the resources it takes.
 */
static int fits_at(const struct table *t, const struct ask *a, const struct claim *keep,
                   const struct ask *extra, uint64_t at)
{
    const struct etude_ledger_res *cap = a->cap;
    uint64_t sum, want;
    size_t k;

    for (k = 0; k < a->ncap; k++) {
        want = extra != NULL ? amount_of(extra->res, extra->nres, cap[k].name) : 0;
        if (extra != NULL && want == 0)
            continue;
        if (held_after(t, cap[k].name, at, a->now, &sum) != 0 || add(&sum, want) != 0 ||
            add(&sum, amount_of(keep->res, keep->nres, cap[k].name)) != 0 || sum > cap[k].amount)
            return 0;
    }
    return 1;
}

/**
 * Say whether what `a` asks for, which fits now but takes what the protected
 * request `keep` needs, may still be admitted beside it, so that the machine
 * is used while `keep` waits without delaying it on the claims' forecasts.
 *
 * `keep` is expected to fit at the earliest time, now or a claim's expected
 * end, by which the claims still running leave it room. `a` is admitted when
 * it is expected to end by then, or when it fits beside `keep` and the claims
 * expected to run past then. With no such time, as when `keep` waits for a
 * claim that has no forecast, nothing is admitted beside it.
 */
static int backfills(const struct table *t, const struct ask *a, const struct claim *keep)
{
    uint64_t at = UINT64_MAX, end;
    size_t i;

    if (a->now == 0)
        return 0;
    for (i = 0; i <= t->n; i++) {
        if (i < t->n && is_request_state(t->c[i].state))
            continue;
        end = i == t->n ? a->now : claim_end(&t->c[i], a->now);
        if (end < at && fits_at(t, a, keep, NULL, end))
            at = end;
    }
    if (at == UINT64_MAX)
        return 0;
    if (a->forecast_us != 0 && a->forecast_us <= at - a->now)
        return 1;
    return fits_at(t, a, keep, a, at);
}

/**
 * Say whether what `a` asks for fits beside the claims of `t` within the
 * limits, and leaves free, of each resource it takes, what request `keep`
 * needs of it, or else backfills beside `keep` as backfills() says. A
 * resource that `a` does not take is not kept, so a request blocked on
 * memory does not hold back a claim that takes processors alone.
 */
static int fits(const struct etude_ledger *l, const struct table *t, const struct ask *a,
                const struct claim *keep)
{
    const struct etude_ledger_res *cap = a->cap;
    uint64_t sum, want;
    size_t k;
    int blocked = 0;

    for (k = 0; k < a->ncap; k++) {
        want = amount_of(a->res, a->nres, cap[k].name);
        if (held(l, t, cap[k].name, 0, &sum) != 0 || add(&sum, want) != 0 || sum > cap[k].amount)
            return 0;
        if (keep != NULL && want > 0 &&
            (add(&sum, amount_of(keep->res, keep->nres, cap[k].name)) != 0 || sum > cap[k].amount))
            blocked = 1;
    }
    return !blocked || backfills(t, a, keep);
}

/**
 * Say whether `res` could ever fit in `cap`: within it alone, and beside the
 * claims that only a person can release, those judge finds abandoned.
 * Return 0 when it could, or ERANGE or ENOSPC when it could not.
 */
static int possible(const struct etude_ledger *l, const struct table *t,
                    const struct etude_ledger_res *res, size_t nres,
                    const struct etude_ledger_res *cap, size_t ncap)
{
    uint64_t sum;
    size_t k;

    for (k = 0; k < ncap; k++)
        if (amount_of(res, nres, cap[k].name) > cap[k].amount)
            return ERANGE;
    for (k = 0; k < ncap; k++)
        if (held(l, t, cap[k].name, 1, &sum) != 0 ||
            add(&sum, amount_of(res, nres, cap[k].name)) != 0 || sum > cap[k].amount)
            return ENOSPC;
    return 0;
}

/** Count the claims of `t` that account `account` holds. */
static size_t held_by(const struct table *t, const char *account)
{
    size_t i, n = 0;

    for (i = 0; i < t->n; i++)
        if (!is_request_state(t->c[i].state) && strcmp(t->c[i].account, account) == 0)
            n++;
    return n;
}

/**
 * Give the request of `t` that comes first between accounts: of those that
 * could ever fit, the one whose account holds the fewest claims, and of
 * those, the one that arrived first; NULL when there is none.
 */
static struct claim *fairest(const struct etude_ledger *l, struct table *t,
                             const struct etude_ledger_res *cap, size_t ncap)
{
    struct claim *best = NULL;
    size_t i, n, best_n = 0;

    for (i = 0; i < t->n; i++) {
        struct claim *c = &t->c[i];

        if (!is_request_state(c->state) || possible(l, t, c->res, c->nres, cap, ncap) != 0)
            continue;
        n = held_by(t, c->account);
        if (best == NULL || n < best_n || (n == best_n && c->ticket < best->ticket)) {
            best = c;
            best_n = n;
        }
    }
    return best;
}

/**
 * Give the machine's protected request, the one blocked request whose needs
 * every other admission leaves free: the request already blocked, unless it
 * could never fit, and otherwise the fairest request, which becomes blocked
 * and stays so until it is admitted or withdrawn. Set `*changed` when the
 * table changes; NULL when no request waits.
 */
static struct claim *protect(const struct etude_ledger *l, struct table *t,
                             const struct etude_ledger_res *cap, size_t ncap, int *changed)
{
    struct claim *p = NULL;
    size_t i;

    for (i = 0; i < t->n; i++)
        if (strcmp(t->c[i].state, "blocked") == 0)
            p = &t->c[i];
    if (p != NULL && possible(l, t, p->res, p->nres, cap, ncap) != 0) {
        strcpy(p->state, "waiting");
        *changed = 1;
        p = NULL;
    }
    if (p == NULL && (p = fairest(l, t, cap, ncap)) != NULL) {
        strcpy(p->state, "blocked");
        *changed = 1;
    }
    return p;
}

/**
 * Say whether the `nres` resources `res` have names, each once, none of them
 * a name the ledger keeps for a claim's forecast.
 */
static int res_sound(const struct etude_ledger_res *res, size_t nres)
{
    size_t i, j;

    for (i = 0; i < nres; i++) {
        if (res[i].name == NULL || !is_name(res[i].name) ||
            strcmp(res[i].name, FORECAST_RES) == 0 || strcmp(res[i].name, ADMITTED_RES) == 0)
            return 0;
        for (j = 0; j < i; j++)
            if (strcmp(res[i].name, res[j].name) == 0)
                return 0;
    }
    return 1;
}

/** Give claim `c` the resources `res` in place of its own; 0 or ENOMEM. */
static int claim_set_res(struct claim *c, const struct etude_ledger_res *res, size_t nres)
{
    size_t i;
    int e = 0;

    for (i = 0; i < c->nres; i++)
        free((char *)c->res[i].name);
    free(c->res);
    c->res = NULL;
    c->nres = 0;
    for (i = 0; i < nres && e == 0; i++)
        e = claim_add_res(c, res[i].name, res[i].amount);
    return e;
}

/**
 * Add to `t` a claim of this process in state `state` of what `a` asks for,
 * a request getting the next ticket; NULL when memory ran out.
 */
static struct claim *claim_new(const struct etude_ledger *l, struct table *t, const struct ask *a,
                               const char *state)
{
    uint64_t ticket = 0;
    struct claim *c;
    size_t i;

    for (i = 0; i < t->n; i++)
        if (t->c[i].ticket > ticket)
            ticket = t->c[i].ticket;
    c = table_add(t);
    if (c == NULL)
        return NULL;
    if ((c->id = malloc(strlen(a->id) + 1)) == NULL || claim_set_res(c, a->res, a->nres) != 0) {
        table_remove(t, c);
        return NULL;
    }
    strcpy(c->id, a->id);
    strcpy(c->system, LEDGER_SYSTEM);
    strcpy(c->boot, l->boot);
    c->owner = l->self;
    strcpy(c->account, a->account);
    strcpy(c->state, state);
    c->ticket = is_request_state(state) ? ticket + 1 : 0;
    return c;
}

/** Give ledger `l`'s time of day in microseconds, or 0 when it has none. */
static uint64_t ledger_now(const struct etude_ledger *l)
{
    return l->clock != NULL ? l->clock(l->clock_ctx) : wall_us();
}

void etude_ledger_clock(struct etude_ledger *l, uint64_t (*now)(void *ctx), void *ctx)
{
    l->clock = now;
    l->clock_ctx = ctx;
}

/** Record on claim `c`, just admitted, the forecast of `a`; 0 or an error. */
static int claim_forecast(struct claim *c, const struct ask *a)
{
    int e;

    if (a->forecast_us == 0 || a->now == 0)
        return 0;
    e = claim_add_res(c, FORECAST_RES, a->forecast_us);
    return e != 0 ? e : claim_add_res(c, ADMITTED_RES, a->now);
}

int etude_ledger_reserve(struct etude_ledger *l, const char *id, const struct etude_ledger_res *res,
                         size_t nres, const struct etude_ledger_res *cap, size_t ncap)
{
    const struct ask a = {.id = id,
                          .account = "",
                          .res = res,
                          .cap = cap,
                          .nres = nres,
                          .ncap = ncap,
                          .wait = 0,
                          .forecast_us = 0,
                          .now = ledger_now(l)};
    struct claim *p;
    struct table t;
    char **gone;
    size_t n;
    int e, w, changed;

    if (!is_id(id) || !res_sound(res, nres))
        return fail(EINVAL);
    if (etude_authority_claim(l->authority, id, cap, ncap))
        return -1;
    e = begin(l, &t);
    if (e != 0)
        return fail(e);
    n = sweep(l, &t, &gone);
    changed = n > 0;
    p = protect(l, &t, cap, ncap, &changed);
    if (table_find(&t, id) != NULL)
        e = EEXIST;
    else if (!fits(l, &t, &a, p))
        e = EAGAIN;
    else if (claim_new(l, &t, &a, "reserved") == NULL)
        e = ENOMEM;
    if (e == 0) {
        verdict_remove(l, id);
        changed = 1;
    }
    w = end(l, &t, 0, changed);
    swept(l, gone, n, w == 0);
    return fail(w != 0 ? w : e);
}

/**
 * Decide what `a` asks for in table `t`, as etude_ledger_admit says, under
 * the lock, and set `*changed` when `t` changes.
 *
 * @return 0 for a claim admitted, EAGAIN, ERANGE, ENOSPC or EEXIST as
 *         etude_ledger_admit gives them, or ENOMEM.
 */
static int admit_in(const struct etude_ledger *l, struct table *t, const struct ask *a,
                    int *changed)
{
    struct claim *c = table_find(t, a->id), *p;
    int e;

    if (c != NULL && (!is_request_state(c->state) || !is_mine(l, c)))
        return EEXIST;
    e = possible(l, t, a->res, a->nres, a->cap, a->ncap);
    if (e != 0 && c != NULL) {
        table_remove(t, c);
        *changed = 1;
    }
    if (e != 0)
        return e;
    if (c != NULL) {
        strcpy(c->account, a->account);
        *changed = 1;
        if (claim_set_res(c, a->res, a->nres) != 0)
            return ENOMEM;
    } else if (a->wait) {
        *changed = 1;
        if ((c = claim_new(l, t, a, "waiting")) == NULL)
            return ENOMEM;
    }
    p = protect(l, t, a->cap, a->ncap, changed);
    if (!fits(l, t, a, p == c ? NULL : p)) {
        if (c != NULL && !a->wait) {
            table_remove(t, c);
            *changed = 1;
        }
        return EAGAIN;
    }
    if (c == NULL && (c = claim_new(l, t, a, "reserved")) == NULL)
        return ENOMEM;
    strcpy(c->state, "reserved");
    c->ticket = 0;
    *changed = 1;
    return claim_forecast(c, a);
}

int etude_ledger_admit(struct etude_ledger *l, const char *id, const char *account,
                       const struct etude_ledger_res *res, size_t nres,
                       const struct etude_ledger_res *cap, size_t ncap, int wait)
{
    return etude_ledger_admit_for(l, id, account, res, nres, cap, ncap, wait, 0);
}

int etude_ledger_admit_for(struct etude_ledger *l, const char *id, const char *account,
                           const struct etude_ledger_res *res, size_t nres,
                           const struct etude_ledger_res *cap, size_t ncap, int wait,
                           uint64_t forecast_us)
{
    const struct ask a = {.id = id,
                          .account = account,
                          .res = res,
                          .cap = cap,
                          .nres = nres,
                          .ncap = ncap,
                          .wait = wait,
                          .forecast_us = forecast_us,
                          .now = ledger_now(l)};
    struct table t;
    char **gone;
    size_t n;
    int e, w, changed;

    if (!is_id(id) || !is_account(account) || !res_sound(res, nres))
        return fail(EINVAL);
    if (etude_authority_claim(l->authority, id, cap, ncap))
        return -1;
    e = begin(l, &t);
    if (e != 0)
        return fail(e);
    n = sweep(l, &t, &gone);
    changed = n > 0;
    e = admit_in(l, &t, &a, &changed);
    if (e == 0)
        verdict_remove(l, id);
    w = end(l, &t, e == ENOMEM ? e : 0, changed);
    swept(l, gone, n, w == 0);
    return fail(w != 0 ? w : e);
}

int etude_ledger_withdraw(struct etude_ledger *l, const char *id)
{
    struct claim *c;
    struct table t;
    int e = begin(l, &t);

    if (e != 0)
        return fail(e);
    c = table_find(&t, id);
    if (c == NULL)
        e = ENOENT;
    else if (!is_mine(l, c))
        e = EPERM;
    else if (!is_request_state(c->state))
        e = EINVAL;
    else
        table_remove(&t, c);
    return fail(end(l, &t, e, 1));
}

/**
 * Add the processes of an attempt to its claim: the keeper and the case's
 * process when `child` is NULL, and the descendant `child` otherwise.
 */
static int add_processes(struct etude_ledger *l, const char *id, const struct etude_pid *keeper,
                         const struct etude_pid *process, const struct etude_pid *child)
{
    struct claim *c;
    struct table t;
    size_t i;
    int e = begin(l, &t);

    if (e != 0)
        return e;
    c = table_find(&t, id);
    if (c == NULL)
        e = ENOENT;
    else if (!is_mine(l, c))
        e = EPERM;
    else if (is_request_state(c->state) || (child == NULL && strcmp(c->state, "reserved") != 0))
        e = EINVAL;
    if (e == 0 && child == NULL) {
        e = claim_add_proc(c, ROLE_KEEPER, keeper);
        if (e == 0)
            e = claim_add_proc(c, ROLE_CASE, process);
        strcpy(c->state, "running");
    } else if (e == 0) {
        for (i = 0; i < c->nproc; i++)
            if (c->proc[i].id.pid == child->pid && c->proc[i].id.start == child->start)
                return end(l, &t, 0, 0);
        e = claim_add_proc(c, ROLE_CHILD, child);
    }
    return end(l, &t, e, 1);
}

int etude_ledger_register(void *ledger, const char *attempt_id, const struct etude_pid *keeper,
                          const struct etude_pid *process)
{
    return add_processes(ledger, attempt_id, keeper, process, NULL);
}

int etude_ledger_child(void *ledger, const char *attempt_id, const struct etude_pid *child)
{
    return add_processes(ledger, attempt_id, NULL, NULL, child);
}

int etude_ledger_finish(struct etude_ledger *l, const char *id, enum etude_cleanup cleanup)
{
    struct claim *c;
    struct table t;
    int e;

    if (cleanup != ETUDE_CLEANUP_CONFIRMED && cleanup != ETUDE_CLEANUP_UNCONFIRMED)
        return fail(EINVAL);
    e = begin(l, &t);
    if (e != 0)
        return fail(e);
    c = table_find(&t, id);
    if (c == NULL)
        e = ENOENT;
    else if (!is_mine(l, c))
        e = EPERM;
    else if (is_request_state(c->state))
        e = EINVAL;
    else if (cleanup == ETUDE_CLEANUP_CONFIRMED)
        table_remove(&t, c);
    else
        strcpy(c->state, "uncertain");
    e = end(l, &t, e, 1);
    if (e == 0 && cleanup == ETUDE_CLEANUP_CONFIRMED)
        verdict_remove(l, id);
    return fail(e);
}

int etude_ledger_recover(struct etude_ledger *l, size_t *freed)
{
    struct table t;
    char **gone;
    size_t n;
    int e = begin(l, &t);

    if (e != 0)
        return fail(e);
    n = sweep(l, &t, &gone);
    e = end(l, &t, 0, n > 0);
    swept(l, gone, n, e == 0);
    if (e == 0 && freed != NULL)
        *freed = n;
    return fail(e);
}

int etude_ledger_list(struct etude_ledger *l,
                      int (*visit)(const struct etude_ledger_claim *c, void *ctx), void *ctx)
{
    struct etude_ledger_claim v;
    struct table t;
    char **gone;
    size_t i, n;
    int e = begin(l, &t), rc = 0;

    if (e != 0)
        return fail(e);
    n = sweep(l, &t, &gone);
    for (i = 0; i < t.n && rc == 0; i++) {
        const struct claim *c = &t.c[i];

        memset(&v, 0, sizeof(v));
        v.id = c->id;
        v.system = c->system;
        v.owner = c->owner;
        v.account = c->account;
        v.state = c->state;
        switch (judge(l, c, &v.why)) {
        case J_LIVE:
            v.status = ETUDE_CLAIM_LIVE;
            note_running(c, &v);
            break;
        case J_OWNED:
            v.status = ETUDE_CLAIM_OWNED;
            break;
        default:
            v.status = ETUDE_CLAIM_UNCERTAIN;
            break;
        }
        v.res = c->res;
        v.nres = c->nres;
        v.nproc = c->nproc;
        rc = visit(&v, ctx);
    }
    e = end(l, &t, 0, n > 0);
    swept(l, gone, n, e == 0);
    return e != 0 ? fail(e) : rc;
}

int etude_ledger_drop(struct etude_ledger *l, const char *id)
{
    const char *why;
    struct claim *c;
    struct table t;
    int e = begin(l, &t);

    if (e != 0)
        return fail(e);
    c = table_find(&t, id);
    if (c == NULL)
        e = ENOENT;
    else if (judge(l, c, &why) == J_OWNED)
        e = EBUSY;
    else
        table_remove(&t, c);
    e = end(l, &t, e, e == 0);
    if (e == 0)
        verdict_remove(l, id);
    return fail(e);
}
