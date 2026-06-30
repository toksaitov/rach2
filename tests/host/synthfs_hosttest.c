/**
 * Dual-target seam test for the synthetic-path classifier (fs/synthfs.c).
 *
 * host-deps: rach2/sys/fs/synthfs.c
 *
 * The component splitting and the /proc/<pid>/fd/<n> parsing are the
 * fence-post-prone part every path syscall leans on, so they run natively here
 * under ASan/UBSan: a path resolves to the right vnode kind with the right
 * pid/fd, leading/trailing/doubled slashes are tolerated, a non-pid or unknown
 * /proc entry is ENOENT, and an ordinary name stays KRFFS-bound. Returns the
 * failed-check count (0 == pass).
 */

#include "fs/synthfs.h"

static int fails;

#define CHECK(cond)    \
    do {               \
        if (!(cond)) { \
            fails++;   \
        }              \
    } while (0)

/* Equal NUL-terminated strings? */
static int eq(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

int main(void)
{
    struct vpath vp;

    /* Root and ordinary KRFFS names are not synthetic. */
    CHECK(synth_classify("", &vp) == VNODE_KRFFS);
    CHECK(synth_classify("hello.txt", &vp) == VNODE_KRFFS);
    CHECK(synth_classify("etc/passwd", &vp) == VNODE_KRFFS);
    CHECK(synth_classify("devices/x", &vp) == VNODE_KRFFS); /* not "dev" */
    CHECK(synth_classify("proconsul", &vp) == VNODE_KRFFS); /* not "proc" */

    /* /dev. */
    CHECK(synth_classify("dev", &vp) == VNODE_DEV_DIR);
    CHECK(synth_classify("dev/null", &vp) == VNODE_DEV && eq(vp.name, "null"));
    CHECK(synth_classify("dev/urandom", &vp) == VNODE_DEV && eq(vp.name, "urandom"));
    CHECK(synth_classify("dev/null/x", &vp) == VNODE_ENOENT); /* device is a leaf */

    /* /proc directories. */
    CHECK(synth_classify("proc", &vp) == VNODE_PROC_DIR);
    CHECK(synth_classify("proc/self", &vp) == VNODE_PROC_PID && vp.pid == SYNTH_PID_SELF);
    CHECK(synth_classify("proc/7", &vp) == VNODE_PROC_PID && vp.pid == 7);
    CHECK(synth_classify("proc/12/fd", &vp) == VNODE_PROC_FD_DIR && vp.pid == 12);

    /* /proc files. */
    CHECK(synth_classify("proc/7/stat", &vp) == VNODE_PROC_FILE && vp.procfile == SYNTH_PROC_STAT &&
          vp.pid == 7);
    CHECK(synth_classify("proc/9/cmdline", &vp) == VNODE_PROC_FILE &&
          vp.procfile == SYNTH_PROC_CMDLINE);

    /* /proc/<pid>/fd/<n> magic links: the ttyname path. */
    CHECK(synth_classify("proc/self/fd/0", &vp) == VNODE_PROC_FD_LINK && vp.pid == SYNTH_PID_SELF &&
          vp.procfd == 0);
    CHECK(synth_classify("proc/7/fd/3", &vp) == VNODE_PROC_FD_LINK && vp.pid == 7 &&
          vp.procfd == 3);

    /* Bad /proc entries are ENOENT, never KRFFS (so they don't
     * shadow-resolve). */
    CHECK(synth_classify("proc/abc", &vp) == VNODE_ENOENT);     /* not a pid */
    CHECK(synth_classify("proc/7/bogus", &vp) == VNODE_ENOENT); /* unknown file */
    CHECK(synth_classify("proc/7/fd/xy", &vp) == VNODE_ENOENT); /* not an fd number */

    /* Slash tolerance: leading, trailing, and doubled slashes. */
    CHECK(synth_classify("/dev/null", &vp) == VNODE_DEV && eq(vp.name, "null"));
    CHECK(synth_classify("dev/", &vp) == VNODE_DEV_DIR);
    CHECK(synth_classify("proc/"
                         "/7/"
                         "/stat",
                         &vp) == VNODE_PROC_FILE &&
          vp.pid == 7);

    /* The classifier requires a lexically pre-normalized path: a '.' or '..'
     * component is NOT collapsed here (only the KRFFS resolver does that), so a
     * dot path transiting a synthetic prefix deliberately fails closed rather
     * than reaching the wrong object. */
    CHECK(synth_classify("proc/7/../9/stat", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/../etc/passwd", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("dev/../dev/null", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/7/.", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("dev/./null", &vp) == VNODE_ENOENT);

    /* A pid that would overflow or wrap a 32-bit int is rejected, never folded to
     * a small pid another process might own; the boundary is 2147483639. */
    CHECK(synth_classify("proc/99999999999", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/4294967297", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/2147483639", &vp) == VNODE_PROC_PID && vp.pid == 2147483639);
    CHECK(synth_classify("proc/2147483640", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/7/fd/99999999999", &vp) == VNODE_ENOENT);

    /* A leading '-' is not a digit, so a negative token cannot be read as the
     * SYNTH_PID_SELF (-1) sentinel. */
    CHECK(synth_classify("proc/-1", &vp) == VNODE_ENOENT);
    CHECK(synth_classify("proc/-1/stat", &vp) == VNODE_ENOENT);

    return fails;
}
