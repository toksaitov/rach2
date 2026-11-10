/**
 * fsidprobe: setfsuid(2) and setfsgid(2), run as root, as Linux 6.12
 * answers them.
 *
 * * query: an id of -1 changes nothing and returns the present file-system
 *   id, and a new id returns the one before; on i386 the 16-bit calls,
 *   setfsuid16 and setfsgid16, take 0xFFFF for -1 and return the id whole.
 * * create-owner: with both file-system ids at 1000, a new file in a
 *   directory of mode 0777 is owned by 1000 and group 1000.
 * * open-denied: root's file of mode 0600 no longer opens for reading.
 * * access-real: access(2), which judges by the real ids, still allows it.
 * * status-ids: /proc/self/status gives 1000 as the fourth column of its Uid
 *   and Gid lines, after the real, effective and saved 0.
 * * capeff: CapEff has lost the capabilities that go by the file-system
 *   user id, Linux's CAP_FS_SET, and kept the rest.
 * * restore: setfsuid(0) brings them back, the file opens again, and the
 *   status lines end in 0.
 * * setuid-resets: setuid(0) and setgid(0) make the file-system ids the
 *   effective ones again, though the effective ids do not change.
 * * cleanup: the files and the directory are removed.
 *
 * The files are fsid.d, fsid.d/own and fsid.d/r600 in the working
 * directory. In a native run, `ids=1000:NEW` (probe_args.h) gives the id
 * that stands for 1000. Prints "FSIDPROBE OK" or "FSIDPROBE FAIL: why".
 * etude's report (probe_report.h), for the case ETUDE_CASE names, has an
 * assertion for each row above in order; the first failure fails its
 * assertion and leaves the rest not run. Freestanding; direct system calls
 * (probe.h).
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

#define O_RDONLY 0
#define O_WRONLY 1
#define O_CREAT  0100
#define O_EXCL   0200
#define R_OK     4
#define EACCES   13

/* Linux's CAP_FS_SET: CHOWN, DAC_OVERRIDE, DAC_READ_SEARCH, FOWNER, FSETID,
 * LINUX_IMMUTABLE, MKNOD and MAC_OVERRIDE. */
#define CAP_FS_SET 0x10800021Full

static int made_dir, made_own, made_r600;

/** The length of the string `s`. */
static size_t strlen(const char *s)
{
    size_t n = 0;

    while (s[n]) {
        n++;
    }
    return n;
}

/** Copy `n` bytes from `src` to `dst`. */
static void copy(void *dst, const void *src, size_t n)
{
    char *d = dst;
    const char *s = src;

    while (n--) {
        *d++ = *s++;
    }
}

/** Write `head`, `s` and a newline to standard output, in one write. */
static void say(const char *head, const char *s)
{
    char line[160];
    size_t n = strlen(head), m = strlen(s);

    if (n + m > sizeof(line) - 1) {
        m = sizeof(line) - 1 - n;
    }
    copy(line, head, n);
    copy(line + n, s, m);
    line[n + m] = '\n';
    syscall3(SYS_WRITE, 1, (long)line, (long)(n + m + 1));
}

/** Put the ids back to root's and remove what the probe made; 0, or -1
 * when a removal failed. */
static int cleanup(void)
{
    int bad = 0;

    syscall1(SYS_SETFSUID, 0);
    syscall1(SYS_SETFSGID, 0);
    if (made_own) {
        bad |= syscall1(SYS_UNLINK, (long)"fsid.d/own") != 0;
        made_own = 0;
    }
    if (made_r600) {
        bad |= syscall1(SYS_UNLINK, (long)"fsid.d/r600") != 0;
        made_r600 = 0;
    }
    if (made_dir) {
        bad |= syscall1(SYS_RMDIR, (long)"fsid.d") != 0;
        made_dir = 0;
    }
    return bad ? -1 : 0;
}

/** Print the failure `why`, end the report, clean up and exit 1. */
static void fail(const char *why)
{
    say("FSIDPROBE FAIL: ", why);
    report_stop(why);
    cleanup();
    syscall1(SYS_EXIT, 1);
    __builtin_unreachable();
}

/** Fail with `why` unless `got` is `want`. */
static void want(long got, long want_value, const char *why)
{
    if (got != want_value) {
        fail(why);
    }
}

/** Write the decimal `v` into `out`, ended with a NUL; return the end. */
static char *decimal(char *out, unsigned long v)
{
    char digits[24];
    int n = 0;

    do {
        digits[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        *out++ = digits[--n];
    }
    *out = '\0';
    return out;
}

/** Read /proc/self/status into `buf` of `cap` bytes, ended with a NUL;
 * return its length, or 0 when it cannot be read. */
static int read_status(char *buf, int cap)
{
    long fd = syscall3(SYS_OPEN, (long)"/proc/self/status", O_RDONLY, 0);
    int len = 0;
    long n;

    if (fd < 0) {
        return 0;
    }
    while (len < cap - 1 && (n = syscall3(SYS_READ, fd, (long)(buf + len), cap - 1 - len)) > 0) {
        len += (int)n;
    }
    syscall1(SYS_CLOSE, fd);
    buf[len] = '\0';
    return len;
}

/** Whether `needle` occurs in the string `hay`. */
static int contains(const char *hay, const char *needle)
{
    size_t nl = strlen(needle);
    size_t i, j;

    for (i = 0; hay[i]; i++) {
        for (j = 0; j < nl && hay[i + j] == needle[j]; j++) {
        }
        if (j == nl) {
            return 1;
        }
    }
    return 0;
}

/** Whether /proc/self/status has "Name:\t0\t0\t0\tID\n" for both Uid and
 * Gid. */
static int status_has_fsid(unsigned long id)
{
    static char status[4096];
    char line[48];
    char *end;

    if (read_status(status, sizeof(status)) == 0) {
        return 0;
    }
    copy(line, "Uid:\t0\t0\t0\t", 11);
    end = decimal(line + 11, id);
    end[0] = '\n';
    end[1] = '\0';
    if (!contains(status, line)) {
        return 0;
    }
    line[0] = 'G';
    return contains(status, line);
}

/** The CapEff set of /proc/self/status, or ~0 when it cannot be read. */
static unsigned long long cap_eff(void)
{
    static char status[4096];
    unsigned long long v = 0;
    const char *p;
    int i;

    if (read_status(status, sizeof(status)) == 0) {
        return ~0ull;
    }
    for (p = status; *p; p++) {
        if (p[0] == 'C' && p[1] == 'a' && p[2] == 'p' && p[3] == 'E' && p[4] == 'f' &&
            p[5] == 'f' && p[6] == ':') {
            break;
        }
    }
    if (!*p) {
        return ~0ull;
    }
    p += 8;
    for (i = 0; i < 16; i++, p++) {
        int d = *p >= '0' && *p <= '9' ? *p - '0' : *p >= 'a' && *p <= 'f' ? *p - 'a' + 10 : -1;

        if (d < 0) {
            return ~0ull;
        }
        v = v << 4 | (unsigned long long)d;
    }
    return v;
}

/** The owner and group of `fd` into `*uid` and `*gid`; 0, or -1. */
static int owner_of(long fd, unsigned long *uid, unsigned long *gid)
{
    unsigned char st[KSTAT_SIZE];
    unsigned int u, g;

    if (syscall2(SYS_FSTAT, fd, (long)st) != 0) {
        return -1;
    }
    copy(&u, st + KSTAT_UID, sizeof(u));
    copy(&g, st + KSTAT_GID, sizeof(g));
    *uid = u;
    *gid = g;
    return 0;
}

/** Make fsid.d, of mode 0777, and root's fsid.d/r600. */
static void make_files(void)
{
    long fd;

    if (syscall2(SYS_MKDIR, (long)"fsid.d", 0777) != 0) {
        fail("make fsid.d");
    }
    made_dir = 1;
    fd = syscall3(SYS_OPEN, (long)"fsid.d/r600", O_WRONLY | O_CREAT | O_EXCL, 0600);
    if (fd < 0) {
        fail("make fsid.d/r600");
    }
    made_r600 = 1;
    syscall2(SYS_FCHMOD, fd, 0600);
    syscall1(SYS_CLOSE, fd);
    if (syscall2(SYS_CHMOD, (long)"fsid.d", 0777) != 0) {
        fail("make fsid.d mode 0777");
    }
}

/** The query row, which leaves both file-system ids at `id`. */
static void query_row(unsigned long id)
{
    want(syscall1(SYS_SETFSUID, -1), 0, "query: setfsuid(-1) did not give 0");
    want(syscall1(SYS_SETFSUID, (long)id), 0, "query: setfsuid(1000) did not give 0");
    want(syscall1(SYS_SETFSUID, -1), (long)id, "query: setfsuid(-1) did not give 1000");
    want(syscall1(SYS_SETFSGID, -1), 0, "query: setfsgid(-1) did not give 0");
    want(syscall1(SYS_SETFSGID, (long)id), 0, "query: setfsgid(1000) did not give 0");
    want(syscall1(SYS_SETFSGID, -1), (long)id, "query: setfsgid(-1) did not give 1000");
#ifdef SYS_SETFSUID16
    want(syscall1(SYS_SETFSUID16, 0xFFFF), (long)id, "query: setfsuid16(0xFFFF) did not give 1000");
    want(syscall1(SYS_SETFSGID16, 0xFFFF), (long)id, "query: setfsgid16(0xFFFF) did not give 1000");
#endif
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/**
 * Run the rows in order, as root. `_start` calls it with the initial stack.
 */
void probe_main(unsigned long *sp)
{
    static const char *const plan[] = {"query",       "create-owner",  "open-denied",
                                       "access-real", "status-ids",    "capeff",
                                       "restore",     "setuid-resets", "cleanup"};
    unsigned long id, uid, gid;
    unsigned long long caps;
    long fd;
    int i;

    report_begin(report_case((char **)(sp + 1) + sp[0] + 1, "guest.default.fsidprobe"));
    for (i = 0; i < (int)(sizeof(plan) / sizeof(plan[0])); i++) {
        report_plan(plan[i]);
    }
    probe_args((int)sp[0], (char **)(sp + 1));
    id = (unsigned long)probe_id(1000);
    if (syscall0(SYS_GETEUID) != 0 || syscall0(SYS_GETUID) != 0) {
        fail("the probe must run as root");
    }
    make_files();
    caps = cap_eff();
    if (caps == ~0ull) {
        fail("read CapEff");
    }

    query_row(id);
    report_held();

    fd = syscall3(SYS_OPEN, (long)"fsid.d/own", O_WRONLY | O_CREAT | O_EXCL, 0644);
    if (fd < 0) {
        fail("create-owner: fsid.d/own was not made");
    }
    made_own = 1;
    if (owner_of(fd, &uid, &gid) != 0 || uid != id || gid != id) {
        fail("create-owner: fsid.d/own is not owned by 1000:1000");
    }
    syscall1(SYS_CLOSE, fd);
    report_held();

    want(syscall3(SYS_OPEN, (long)"fsid.d/r600", O_RDONLY, 0), -EACCES,
         "open-denied: root's 0600 file was not EACCES");
    report_held();
    want(syscall2(SYS_ACCESS, (long)"fsid.d/r600", R_OK), 0,
         "access-real: access(R_OK) did not judge by the real id");
    report_held();
    if (!status_has_fsid(id)) {
        fail("status-ids: the Uid and Gid lines do not end in 1000");
    }
    report_held();
    want(cap_eff() == (caps & ~CAP_FS_SET), 1, "capeff: CapEff is not the set less CAP_FS_SET");
    report_held();

    want(syscall1(SYS_SETFSUID, 0), (long)id, "restore: setfsuid(0) did not give 1000");
    want(syscall1(SYS_SETFSGID, 0), (long)id, "restore: setfsgid(0) did not give 1000");
    want(cap_eff() == caps, 1, "restore: CapEff did not come back");
    fd = syscall3(SYS_OPEN, (long)"fsid.d/r600", O_RDONLY, 0);
    if (fd < 0) {
        fail("restore: root's 0600 file did not open");
    }
    syscall1(SYS_CLOSE, fd);
    if (!status_has_fsid(0)) {
        fail("restore: the Uid and Gid lines do not end in 0");
    }
    report_held();

    syscall1(SYS_SETFSUID, (long)id);
    want(syscall1(SYS_SETUID, 0), 0, "setuid-resets: setuid(0) failed");
    want(syscall1(SYS_SETFSUID, -1), 0, "setuid-resets: setuid(0) left the file-system user id");
    want(syscall1(SYS_SETGID, 0), 0, "setuid-resets: setgid(0) failed");
    want(syscall1(SYS_SETFSGID, -1), 0, "setuid-resets: setgid(0) left the file-system group id");
    report_held();

    if (cleanup() != 0) {
        fail("cleanup: the files were not removed");
    }
    report_held();
    report_end();
    say("FSIDPROBE OK", "");
    syscall1(SYS_EXIT, 0);
    __builtin_unreachable();
}
