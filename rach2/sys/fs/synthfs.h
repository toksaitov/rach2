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
    VNODE_PROC_USB,          /* /proc/usb: the USB devices this kernel enumerated */
    VNODE_PROC_NET_ROUTE,    /* /proc/net/route: the on-link subnet and gateway */
    VNODE_PROC_NET_ARP,      /* /proc/net/arp: the address cache */
    VNODE_PROC_NET_TCP,      /* /proc/net/tcp: the connection pool */
    VNODE_PROC_FILESYSTEMS,  /* /proc/filesystems: what mount(2) accepts */
    VNODE_PROC_LOOP,         /* /proc/loop: the files attached to devices */
    VNODE_PROC_INTERRUPTS,   /* /proc/interrupts: the count on each vector */
    VNODE_PROC_CONSOLE,      /* /proc/console: the serial receive path's counters */
    VNODE_PROC_EXE,          /* /proc/<pid>/exe: a link to the program's file */
    VNODE_PROC_TASK_DIR,     /* /proc/<pid>/task: the directory of the threads */
    VNODE_PROC_CPUINFO,      /* /proc/cpuinfo: what cpuid says of the processor */
    VNODE_PROC_SYS_DIR,      /* /proc/sys and a directory of its tree */
    VNODE_PROC_SYS_FILE,     /* a file of /proc/sys: a limit or a name */
    VNODE_PROC_LOADAVG,      /* /proc/loadavg: the load averages and task counts */
    VNODE_PROC_NET_FILE,     /* /proc/net/{udp,raw,unix,sockstat,snmp}, by `procfile` */
    VNODE_SYS_DIR,           /* /sys and a directory of its tree, by `sysent` and `syskey` */
    VNODE_SYS_LINK,          /* /sys/bus/{pci,usb}/devices/NAME, a link to the device */
    VNODE_SYS_FILE,          /* a device's uevent, by `sysent` and `syskey` */
    VNODE_PROC_CWD,          /* /proc/<pid>/cwd: a link to the working directory */
    VNODE_PROC_ROOT,         /* /proc/<pid>/root: a link to the root directory */
    VNODE_PROC_DISKSTATS,    /* /proc/diskstats: the transfers of each disk node */
    VNODE_PROC_VERSION,      /* /proc/version: the kernel's name and release */
    VNODE_PROC_PARTITIONS,   /* /proc/partitions: the size of each disk node */
    VNODE_PROC_VMSTAT,       /* /proc/vmstat: the memory and paging counters */
    VNODE_DEV_LINK,          /* /dev/log: a link to the syslog daemon's socket */
    VNODE_PROC_SYSVIPC_DIR,  /* /proc/sysvipc: the System V objects' directory */
    VNODE_PROC_SYSVIPC,      /* /proc/sysvipc/{shm,sem,msg}, by `procfile` */
    VNODE_PROC_NET_STAT_DIR, /* /proc/net/stat: the route and ARP counters' directory */
    VNODE_PROC_NS_DIR,       /* /proc/<pid>/ns: the links to its namespaces */
    VNODE_PROC_NS_LINK,      /* /proc/<pid>/ns/<kind>, by `procfd` (proc/ns.h's NS_) */
    VNODE_PROC_FDINFO_DIR,   /* /proc/<pid>/fdinfo: a file for each descriptor */
    VNODE_PROC_FDINFO,       /* /proc/<pid>/fdinfo/<n>: its offset and flags, by `procfd` */
    VNODE_ENOENT,            /* under a synthetic mount, but no such entry */
    VNODE_ENOTDIR            /* under a synthetic mount, a non-dir used as a directory */
};

/** The name of /proc/<pid>/ns's link of the namespace kind `kind`, NULL
 * past the last. */
const char *synth_ns_name(int kind);

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
/* A device's uevent under /sys, as the origin of its snapshot. */
#define SYNTH_SYS_UEVENT       36
#define SYNTH_PROC_SMAPS       37
#define SYNTH_PROC_DISKSTATS   38
#define SYNTH_PROC_VERSION     39
#define SYNTH_PROC_PARTITIONS  40
#define SYNTH_PROC_LIMITS      41
#define SYNTH_PROC_MOUNTINFO   42
/* /proc/sysvipc/shm, sem and msg, in that order (ipc/sysv.h's kinds). */
#define SYNTH_PROC_SYSVIPC     43
/* /proc/net/stat/rt_cache and arp_cache, two more files of /proc/net by
 * their origin (`synth_net_name`). */
#define SYNTH_PROC_NETSTAT_RT  46
#define SYNTH_PROC_NETSTAT_ARP 47
#define SYNTH_PROC_VMSTAT      48
/* /proc/<pid>/uid_map, gid_map and setgroups, of its user namespace. */
#define SYNTH_PROC_UID_MAP     49
#define SYNTH_PROC_GID_MAP     50
#define SYNTH_PROC_SETGROUPS   51

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
    int procfd;    /* PROC_FD_LINK, PROC_FDINFO: the descriptor number; NS_LINK: the kind */
    int procfile;  /* VNODE_PROC_FILE: SYNTH_PROC_* */
    int pts_index; /* VNODE_PTS: the pty slave number N in /dev/pts/N */
    int tid;       /* VNODE_PROC_*: TID in /proc/<pid>/task/TID, or 0 */
    int sysent;    /* VNODE_PROC_SYS_*: the entry of the /proc/sys table; VNODE_SYS_*: the node */
    int syskey;    /* VNODE_SYS_*: the device of a device's node (`synth_sys_device_name`) */
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
 * /proc/sys: a few of Linux's sysctl files, in a table of entries, of which
 * kernel/hostname, kernel/domainname and net/ipv4/ip_forward take a write.
 * Entry 0 is /proc/sys itself; every other entry has its directory as a
 * `parent` entry, and a file has its text. Each accessor answers NULL or -1
 * for an index past the table.
 */
int synth_sys_count(void);
/* The entries of kernel/hostname and domainname, whose texts are the names
 * of the moment, which procfs reads, and not the table's. */
#define SYNTH_SYS_HOSTNAME   11
#define SYNTH_SYS_DOMAINNAME 12
/* The entry of net/ipv4/ip_forward, which takes a write of 0. */
#define SYNTH_SYS_IP_FORWARD 22
/** Whether a privileged caller may write the file entry `entry`. */
int synth_sys_writable(int entry);
const char *synth_sys_name(int entry);
int synth_sys_parent(int entry);
/** The text of a file entry, NULL for a directory. */
const char *synth_sys_text(int entry);

/** The name in /proc/net of the file `origin`, SYNTH_PROC_NETUDP to
 * SYNTH_PROC_NETSNMP, or NULL for another. */
const char *synth_net_name(int origin);
/** The name of the /proc/sysvipc file of origin `origin`, or NULL. */
const char *synth_sysvipc_name(int origin);

/*
 * /sys: the PCI functions and USB devices, as Linux's sysfs has them for
 * the readers of `uevent`. The nodes of the tree are numbered: /sys, its
 * `bus` and `devices`, the two buses and their `devices` of links, the two
 * directories the links lead to, and a device's own directory, whose
 * `syskey` says which device it is. A PCI function's key is its bus,
 * device and function as the configuration address has them, below 2^16; a
 * USB device's is its controller's number less 1 in bits 28 and 29, its
 * root port in the eight below, and the ports of up to five hubs below it
 * in four bits each, as its name gives them, at least 2^20.
 */
#define SYSFS_ROOT        0  /* /sys */
#define SYSFS_BUS         1  /* /sys/bus */
#define SYSFS_BUS_PCI     2  /* /sys/bus/pci */
#define SYSFS_BUS_USB     3  /* /sys/bus/usb */
#define SYSFS_PCI_LINKS   4  /* /sys/bus/pci/devices */
#define SYSFS_USB_LINKS   5  /* /sys/bus/usb/devices */
#define SYSFS_DEVICES     6  /* /sys/devices */
#define SYSFS_PCI_ROOT    7  /* /sys/devices/pci0000:00 */
#define SYSFS_USB_ROOT    8  /* /sys/devices/usb */
#define SYSFS_PCI_DEVICE  9  /* /sys/devices/pci0000:00/0000:BB:DD.F */
#define SYSFS_USB_DEVICE  10 /* /sys/devices/usb/B-P[.P...] */
#define SYSFS_CLASS       11 /* /sys/class */
#define SYSFS_CLASS_NET   12 /* /sys/class/net */
#define SYSFS_NET_IF      13 /* /sys/class/net/IF, whose key is its index */
#define SYSFS_NET_STATS   14 /* /sys/class/net/IF/statistics */
#define SYSFS_CLASS_RTC   15 /* /sys/class/rtc */
#define SYSFS_RTC0        16 /* /sys/class/rtc/rtc0, the CMOS clock */
#define SYSFS_RTC_DEVICE  17 /* /sys/class/rtc/rtc0/device */
#define SYSFS_RTC_POWER   18 /* /sys/class/rtc/rtc0/device/power */
#define SYSFS_POWER       19 /* /sys/power, the sleep states */
#define SYSFS_BLOCK       20 /* /sys/block */
#define SYSFS_BLOCK_DISK  21 /* /sys/block/DISK, whose key is its disk node's index */
#define SYSFS_BLOCK_PART  22 /* /sys/block/DISK/PART, the same of the partition */
#define SYSFS_BLOCK_QUEUE 23 /* /sys/block/DISK/queue */
#define SYSFS_BLOCK_DEV   24 /* /sys/block/DISK/device */
#define SYSFS_DEV         25 /* /sys/dev */
#define SYSFS_DEV_BLOCK   26 /* /sys/dev/block, a link for each disk node */
#define SYSFS_SYSTEM      27 /* /sys/devices/system */
#define SYSFS_CPU         28 /* /sys/devices/system/cpu, the processors */
#define SYSFS_CPU0        29 /* /sys/devices/system/cpu/cpu0 */

/** Whether the /sys directory `node` is one of a device or an interface,
 * which `syskey` says, and which exists only while that does. */
#define SYSFS_KEYED(node)                                                                  \
    ((node) == SYSFS_PCI_DEVICE || (node) == SYSFS_USB_DEVICE || (node) == SYSFS_NET_IF || \
     (node) == SYSFS_NET_STATS || SYSFS_IS_BLOCK(node))

/** Whether `node` is a directory of a disk node: its own, its queue's or
 * its device's. */
#define SYSFS_IS_BLOCK(node) ((node) >= SYSFS_BLOCK_DISK && (node) <= SYSFS_BLOCK_DEV)

/** The name of the `n`th attribute file of the /sys directory `node`, in the
 * order the directory lists them, or NULL past the last, and for a node
 * with none. */
const char *sysfs_attr(int node, int n);

/** Whether the attribute `attr` of `node` takes a privileged write:
 * rtc0's wakealarm and /sys/power's state. */
int sysfs_attr_writable(int node, int attr);

/* The interfaces' part, in procfs.c: the index of the interface named by
 * the `len` bytes at `name`, or -1; its name, NUL-terminated, into `out` of
 * `cap` bytes; and its card's PCI key, or -1. */
int sysfs_net_index(const char *name, int len);
void sysfs_net_name(int index, char *out, int cap);
int sysfs_net_pci_key(int index);

/* The disk nodes' part, in procfs.c, each known by its index in devfs's
 * table: the node named by the `len` bytes at `name`, a whole disk's when
 * `disk` is -1 and else a partition of the disk whose node is `disk`, or
 * -1; the node of the numbers MAJOR:MINOR the `len` bytes at `name` give,
 * or -1; a node's name into `out` of `cap` bytes; and the index of the
 * whole disk a node is on, itself for a whole disk, or -1 for none. */
int sysfs_block_find(const char *name, int len, int disk);
int sysfs_block_devno(const char *name, int len);
void sysfs_block_name(int key, char *out, int cap);
int sysfs_block_whole(int key);

/** Write the name of the device `key` of `node`, SYSFS_PCI_DEVICE or
 * SYSFS_USB_DEVICE, into `out` of `cap` bytes, NUL-terminated. */
void synth_sys_device_name(int node, int key, char *out, int cap);

/** The path of the /sys directory `node`, with its device `key`, into `out`
 * of `cap` bytes, NUL-terminated. */
void synth_sys_path(int node, int key, char *out, int cap);

/* The target of /dev/log, where a syslog daemon binds its socket: a
 * directory every system image has writable, as systemd's /dev/log leads
 * under /run. BusyBox's syslogd follows the link before it binds, and a
 * client's connect and send follow it to the socket. */
#define SYNTH_DEV_LOG_TARGET "/var/run/log"

/* True if a classified kind is one of the synthetic (non-KRFFS) namespaces. */
#define VNODE_IS_SYNTH(kind) ((kind) != VNODE_KRFFS)

#endif /* _SYNTHFS_H_ */
