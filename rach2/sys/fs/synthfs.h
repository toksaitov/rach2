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
    VNODE_KRFFS = 0,     /* not synthetic: resolve through KRFFS */
    VNODE_DEV_DIR,       /* /dev */
    VNODE_DEV,           /* /dev/<name>: a character device */
    VNODE_PTS_DIR,       /* /dev/pts: the pty slave directory */
    VNODE_PTS,           /* /dev/pts/N: a pty slave (pts_index = N) */
    VNODE_PROC_DIR,      /* /proc */
    VNODE_PROC_PID,      /* /proc/<pid> (or /proc/self) */
    VNODE_PROC_FD_DIR,   /* /proc/<pid>/fd */
    VNODE_PROC_FILE,     /* /proc/<pid>/{stat,cmdline} */
    VNODE_PROC_FD_LINK,  /* /proc/<pid>/fd/<n>: a magic symlink */
    VNODE_PROC_CMDLINE,  /* /proc/cmdline: the boot command line */
    VNODE_PROC_DISK,     /* /proc/disk: what the disk says about itself */
    VNODE_PROC_NET_DIR,  /* /proc/net: the directory the interface file is in */
    VNODE_PROC_NET_DEV,  /* /proc/net/dev: the interface and what it carried */
    VNODE_PROC_UPTIME,   /* /proc/uptime: seconds up, and seconds idle */
    VNODE_PROC_STAT_SYS, /* /proc/stat: the machine's tick counters and btime */
    VNODE_PROC_MEMINFO,  /* /proc/meminfo: the physical frame pool */
    VNODE_PROC_MOUNTS,   /* /proc/mounts: what was mounted, and how */
    VNODE_PROC_AUDIO,
    VNODE_PROC_USB,         /* /proc/usb: the USB devices this kernel enumerated */
    VNODE_PROC_NET_ROUTE,   /* /proc/net/route: the on-link subnet and gateway */
    VNODE_PROC_NET_ARP,     /* /proc/net/arp: the address cache */
    VNODE_PROC_NET_TCP,     /* /proc/net/tcp: the connection pool */
    VNODE_PROC_FILESYSTEMS, /* /proc/filesystems: what mount(2) accepts */
    VNODE_PROC_LOOP,        /* /proc/loop: the files attached to devices */
    VNODE_PROC_INTERRUPTS,  /* /proc/interrupts: the count on each vector */
    VNODE_PROC_CONSOLE,     /* /proc/console: the serial receive path's counters */
    VNODE_PROC_EXE,         /* /proc/<pid>/exe: a link to the program's file */
    VNODE_PROC_TASK_DIR,    /* /proc/<pid>/task: the directory of the threads */
    VNODE_PROC_CPUINFO,     /* /proc/cpuinfo: what cpuid says of the processor */
    VNODE_PROC_SYS_DIR,     /* /proc/sys and a directory of its tree */
    VNODE_PROC_SYS_FILE,    /* a file of /proc/sys: a limit or a name */
    VNODE_PROC_LOADAVG,     /* /proc/loadavg: the load averages and task counts */
    VNODE_PROC_NET_FILE,    /* /proc/net/{udp,raw,unix,sockstat,snmp}, by `procfile` */
    VNODE_ENOENT,           /* under a synthetic mount, but no such entry */
    VNODE_ENOTDIR           /* under a synthetic mount, a non-dir used as a directory */
};

/* /proc/<pid>/<file> kinds, and the global boot line (a procfs_file origin). */
#define SYNTH_PROC_STAT        1
#define SYNTH_PROC_CMDLINE     2
#define SYNTH_PROC_BOOTLINE    3
#define SYNTH_PROC_DISKINFO    4
#define SYNTH_PROC_NETDEV      5
#define SYNTH_PROC_UPTIME      6
#define SYNTH_PROC_STATSYS     7
#define SYNTH_PROC_MEMINFO     8
#define SYNTH_PROC_MOUNTS      9
#define SYNTH_PROC_NETROUTE    10
#define SYNTH_PROC_NETARP      11
#define SYNTH_PROC_NETTCP      12
#define SYNTH_PROC_USB         13
#define SYNTH_PROC_FILESYSTEMS 14
#define SYNTH_PROC_LOOP        15
#define SYNTH_PROC_INTERRUPTS  16
#define SYNTH_PROC_CONSOLE     17
#define SYNTH_PROC_AUDIO       18
#define SYNTH_PROC_MAPS        19
#define SYNTH_PROC_STATUS      20
#define SYNTH_PROC_STATM       21
#define SYNTH_PROC_AUXV        22
#define SYNTH_PROC_ENVIRON     23
#define SYNTH_PROC_COMM        24
#define SYNTH_PROC_MEM         25
#define SYNTH_PROC_OOM_ADJ     26
#define SYNTH_PROC_CPUINFO     27
#define SYNTH_PROC_SYSCTL      28
#define SYNTH_PROC_PIDMOUNTS   29
#define SYNTH_PROC_LOADAVG     30
/* The files of /proc/net after the first four, in the order they are listed
 * (`synth_net_name`). */
#define SYNTH_PROC_NETUDP      31
#define SYNTH_PROC_NETRAW      32
#define SYNTH_PROC_NETUNIX     33
#define SYNTH_PROC_NETSOCKSTAT 34
#define SYNTH_PROC_NETSNMP     35

/* PID sentinel meaning "the calling process" (/proc/self), resolved by the
 * glue. */
#define SYNTH_PID_SELF (-1)

/*
 * The classified path. `name` is a NUL-terminated copy of the device leaf (for
 * VNODE_DEV); pid/procfd/procfile/tid carry the /proc coordinates.
 */
struct vpath {
    int kind;
    char name[64]; /* VNODE_DEV: device leaf name */
    int pid;       /* VNODE_PROC_*: SYNTH_PID_SELF for /proc/self */
    int procfd;    /* VNODE_PROC_FD_LINK: the linked descriptor number */
    int procfile;  /* VNODE_PROC_FILE: SYNTH_PROC_* */
    int pts_index; /* VNODE_PTS: the pty slave number N in /dev/pts/N */
    int tid;       /* VNODE_PROC_*: TID in /proc/<pid>/task/TID, or 0 */
    int sysent;    /* VNODE_PROC_SYS_*: the entry of the /proc/sys table */
};

/** Classify a leading-'/'-stripped path; fills *vp and returns its kind. */
int synth_classify(const char *path, struct vpath *vp);

/*
 * A synthetic directory fd's identity: its kind, the process's pid, and
 * for a thread's directory under /proc/<pid>/task the thread's id, 0 for
 * any other. A directory of /proc/sys has its entry of the table
 * (`synth_sys_name`) in `pid`, since it has no process. It does not fit
 * packed in the fd's priv pointer, so it is a record from the heap that
 * priv points at; defined here so /proc/<pid>/fd readlink can render it.
 */
struct synth_dir {
    int kind;
    int pid;
    int used;
    int tid;
};

/** Render a synthetic directory's absolute path from its kind, pid and, for
 * a thread's directory, tid (0 for none) into out; for a directory of
 * /proc/sys, `pid` is its entry. */
void synth_dir_path(int kind, int pid, int tid, char *out, int cap);

/*
 * /proc/sys: a few of Linux's sysctl files, read-only, in a table of
 * entries. Entry 0 is /proc/sys itself; every other entry has its directory
 * as a `parent` entry, and a file has its text. Each accessor answers NULL
 * or -1 for an index past the table.
 */
int synth_sys_count(void);
const char *synth_sys_name(int entry);
int synth_sys_parent(int entry);
/** The text of a file entry, NULL for a directory. */
const char *synth_sys_text(int entry);

/** The name in /proc/net of the file `origin`, SYNTH_PROC_NETUDP to
 * SYNTH_PROC_NETSNMP, or NULL for another. */
const char *synth_net_name(int origin);

/* True if a classified kind is one of the synthetic (non-KRFFS) namespaces. */
#define VNODE_IS_SYNTH(kind) ((kind) != VNODE_KRFFS)

#endif /* _SYNTHFS_H_ */
