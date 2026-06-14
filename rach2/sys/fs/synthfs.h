#ifndef _SYNTHFS_H_
#define _SYNTHFS_H_

#include "../lib/kint.h"

/*
 * synthfs -- the one place that knows /dev and /proc are synthetic. Every
 * path-taking syscall classifies its (already absolute, leading-'/'-stripped) path
 * through synth_classify(); a VNODE_KRFFS result means "ordinary file -- resolve
 * through KRFFS", anything else routes to the devfs/procfs glue. Keeping the
 * classification here (instead of scattering "does the path start with dev/?"
 * checks before each krffs_resolve) is what stops the two namespaces from rotting
 * into special cases. The classifier is pure string logic -- no kernel state -- so
 * the fence-post-prone component parsing is exercised natively under ASan in
 * tests/host/synthfs_hosttest.c.
 */

/* What a path resolves to. KRFFS is the ordinary case; the rest are synthetic. */
enum vnode_kind {
    VNODE_KRFFS = 0,    /* not synthetic: resolve through KRFFS */
    VNODE_DEV_DIR,      /* /dev */
    VNODE_DEV,          /* /dev/<name> -- a character device */
    VNODE_PROC_DIR,     /* /proc */
    VNODE_PROC_PID,     /* /proc/<pid> (or /proc/self) */
    VNODE_PROC_FD_DIR,  /* /proc/<pid>/fd */
    VNODE_PROC_FILE,    /* /proc/<pid>/{stat,cmdline} */
    VNODE_PROC_FD_LINK, /* /proc/<pid>/fd/<n> -- a magic symlink */
    VNODE_ENOENT,       /* under a synthetic mount, but no such entry */
    VNODE_ENOTDIR       /* under a synthetic mount, a non-dir used as a directory */
};

/* /proc/<pid>/<file> kinds. */
#define SYNTH_PROC_STAT    1
#define SYNTH_PROC_CMDLINE 2

/* PID sentinel meaning "the calling process" (/proc/self), resolved by the glue. */
#define SYNTH_PID_SELF (-1)

/*
 * The classified path. `name` is a NUL-terminated copy of the device leaf (for
 * VNODE_DEV); pid/procfd/procfile carry the /proc coordinates.
 */
struct vpath {
    int kind;
    char name[64]; /* VNODE_DEV: device leaf name */
    int pid;       /* VNODE_PROC_*: SYNTH_PID_SELF for /proc/self */
    int procfd;    /* VNODE_PROC_FD_LINK: the linked descriptor number */
    int procfile;  /* VNODE_PROC_FILE: SYNTH_PROC_* */
};

/*
 * Classify an absolute path with the leading '/' already stripped ("" is the
 * root, "dev/null", "proc/7/stat", "proc/self/fd/0"). Fills *vp and returns its
 * kind. Pure -- no kernel state, no allocation.
 */
int synth_classify(const char *path, struct vpath *vp);

/*
 * A synthetic directory fd's identity (kind + full pid). It does not fit packed in
 * the fd's priv pointer, so it lives in a pool (in syscall.c) and priv points at
 * the slot; defined here so /proc/<pid>/fd readlink can render it.
 */
struct synth_dir {
    int kind;
    int pid;
    int used;
};

/*
 * Reconstruct the absolute path of a synthetic directory ("/dev", "/proc",
 * "/proc/<pid>", "/proc/<pid>/fd") from its kind/pid into out (NUL-terminated).
 * Pure. Used to resolve a path relative to a synthetic directory fd.
 */
void synth_dir_path(int kind, int pid, char *out, int cap);

/* True if a classified kind is one of the synthetic (non-KRFFS) namespaces. */
#define VNODE_IS_SYNTH(kind) ((kind) != VNODE_KRFFS)

#endif /* _SYNTHFS_H_ */
