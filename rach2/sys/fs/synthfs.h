#ifndef _SYNTHFS_H_
#define _SYNTHFS_H_

#include "../lib/kint.h"

/*
 * synthfs: the one place that knows /dev and /proc are synthetic. Every
 * path-taking syscall classifies its (already absolute,
 * leading-'/'-stripped) path through synth_classify(); a VNODE_KRFFS result
 * means "ordinary file, resolve through KRFFS", anything else routes to the
 * devfs/procfs glue. Keeping the classification here (instead of scattering
 * "does the path start with dev/?" checks before each krffs_resolve) is what
 * stops the two namespaces from rotting into special cases. The classifier
 * is pure string logic with no kernel state, so the fence-post-prone
 * component parsing is exercised natively under ASan in
 * tests/host/synthfs_hosttest.c.
 */

/* What a path resolves to. KRFFS is the ordinary case; the rest are
 * synthetic. */
enum vnode_kind {
    VNODE_KRFFS = 0,    /* not synthetic: resolve through KRFFS */
    VNODE_DEV_DIR,      /* /dev */
    VNODE_DEV,          /* /dev/<name>: a character device */
    VNODE_PTS_DIR,      /* /dev/pts: the pty slave directory */
    VNODE_PTS,          /* /dev/pts/N: a pty slave (pts_index = N) */
    VNODE_PROC_DIR,     /* /proc */
    VNODE_PROC_PID,     /* /proc/<pid> (or /proc/self) */
    VNODE_PROC_FD_DIR,  /* /proc/<pid>/fd */
    VNODE_PROC_FILE,    /* /proc/<pid>/{stat,cmdline} */
    VNODE_PROC_FD_LINK, /* /proc/<pid>/fd/<n>: a magic symlink */
    VNODE_PROC_CMDLINE, /* /proc/cmdline: the boot command line */
    VNODE_PROC_DISK,    /* /proc/disk: what the disk says about itself */
    VNODE_PROC_NET_DIR, /* /proc/net: the directory the interface file is in */
    VNODE_PROC_NET_DEV, /* /proc/net/dev: the interface and what it carried */
    VNODE_PROC_UPTIME,  /* /proc/uptime: seconds up, and seconds idle */
    VNODE_PROC_STAT_SYS,/* /proc/stat: the machine's tick counters and btime */
    VNODE_PROC_MEMINFO, /* /proc/meminfo: the physical frame pool */
    VNODE_PROC_MOUNTS,  /* /proc/mounts: what was mounted, and how */
    VNODE_PROC_USB,     /* /proc/usb: the USB devices this kernel enumerated */
    VNODE_PROC_NET_ROUTE, /* /proc/net/route: the on-link subnet and gateway */
    VNODE_PROC_NET_ARP, /* /proc/net/arp: the address cache */
    VNODE_PROC_NET_TCP, /* /proc/net/tcp: the connection pool */
    VNODE_ENOENT,       /* under a synthetic mount, but no such entry */
    VNODE_ENOTDIR       /* under a synthetic mount, a non-dir used as a directory */
};

/* /proc/<pid>/<file> kinds, and the global boot line (a procfs_file origin). */
#define SYNTH_PROC_STAT     1
#define SYNTH_PROC_CMDLINE  2
#define SYNTH_PROC_BOOTLINE 3
#define SYNTH_PROC_DISKINFO 4
#define SYNTH_PROC_NETDEV   5
#define SYNTH_PROC_UPTIME   6
#define SYNTH_PROC_STATSYS  7
#define SYNTH_PROC_MEMINFO  8
#define SYNTH_PROC_MOUNTS   9
#define SYNTH_PROC_NETROUTE 10
#define SYNTH_PROC_NETARP   11
#define SYNTH_PROC_NETTCP   12
#define SYNTH_PROC_USB      13

/* PID sentinel meaning "the calling process" (/proc/self), resolved by the
 * glue. */
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
    int pts_index; /* VNODE_PTS: the pty slave number N in /dev/pts/N */
};

/** Classify a leading-'/'-stripped path; fills *vp and returns its kind. */
int synth_classify(const char *path, struct vpath *vp);

/*
 * A synthetic directory fd's identity (kind + full pid). It does not fit
 * packed in the fd's priv pointer, so it lives in a pool (in syscall.c) and
 * priv points at the slot; defined here so /proc/<pid>/fd readlink can
 * render it.
 */
struct synth_dir {
    int kind;
    int pid;
    int used;
};

/** Render a synthetic directory's absolute path from its kind/pid into out. */
void synth_dir_path(int kind, int pid, char *out, int cap);

/* True if a classified kind is one of the synthetic (non-KRFFS) namespaces. */
#define VNODE_IS_SYNTH(kind) ((kind) != VNODE_KRFFS)

#endif /* _SYNTHFS_H_ */
