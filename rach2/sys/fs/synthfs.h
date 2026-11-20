#ifndef _SYNTHFS_H_
#define _SYNTHFS_H_

#include "../lib/kint.h"

/*
 * synthfs: the classifier of the paths of the three synthetic namespaces,
 * /dev, /proc and /sys, and the tables their names come from. The VFS's
 * synthetic mount types (vfs.c) classify the spelling of a path inside the
 * mount, the namespace's name first, and devfs, procfs and the system calls
 * act on the kind and the coordinates it gives. The classifier is string
 * logic; what it asks of the machine, whether an interface, a disk node or
 * a backlight of a name exists, it asks of procfs.c and
 * display/backlight.c, for which tests/host/synthfs_hosttest.c has
 * stand-ins, so that the test runs it on the host under ASan and UBSan.
 */

/* What a path is: one outside the three namespaces, or a node of one. */
enum vnode_kind {
    VNODE_KRFFS = 0,     /* not under /dev, /proc or /sys */
    VNODE_DEV_DIR,       /* /dev */
    VNODE_DEV,           /* /dev/<name>: a character device */
    VNODE_PTS_DIR,       /* /dev/pts: the pty slave directory */
    VNODE_PTS,           /* /dev/pts/N: a pty slave (pts_index = N) */
    VNODE_PROC_DIR,      /* /proc */
    VNODE_PROC_PID,      /* /proc/<pid> (or /proc/self) */
    VNODE_PROC_FD_DIR,   /* /proc/<pid>/fd */
    VNODE_PROC_FILE,     /* /proc/<pid>/<file>, by `procfile` */
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
    VNODE_PROC_USB,           /* /proc/usb: the USB devices this kernel enumerated */
    VNODE_PROC_NET_ROUTE,     /* /proc/net/route: the on-link subnet and gateway */
    VNODE_PROC_NET_ARP,       /* /proc/net/arp: the address cache */
    VNODE_PROC_NET_TCP,       /* /proc/net/tcp: the connection pool */
    VNODE_PROC_FILESYSTEMS,   /* /proc/filesystems: what mount(2) accepts */
    VNODE_PROC_LOOP,          /* /proc/loop: the files attached to devices */
    VNODE_PROC_INTERRUPTS,    /* /proc/interrupts: the count on each vector */
    VNODE_PROC_CONSOLE,       /* /proc/console: the serial receive path's counters */
    VNODE_PROC_EXE,           /* /proc/<pid>/exe: a link to the program's file */
    VNODE_PROC_TASK_DIR,      /* /proc/<pid>/task: the directory of the threads */
    VNODE_PROC_CPUINFO,       /* /proc/cpuinfo: what cpuid says of the processor */
    VNODE_PROC_SYS_DIR,       /* /proc/sys and a directory of its tree */
    VNODE_PROC_SYS_FILE,      /* a file of /proc/sys: a limit or a name */
    VNODE_PROC_LOADAVG,       /* /proc/loadavg: the load averages and task counts */
    VNODE_PROC_NET_FILE,      /* /proc/net's other files, by `procfile` */
    VNODE_SYS_DIR,            /* /sys and a directory of its tree, by `sysent` and `syskey` */
    VNODE_SYS_LINK,           /* /sys/bus/{pci,usb}/devices/NAME, a link to the device */
    VNODE_SYS_FILE,           /* a device's uevent, by `sysent` and `syskey` */
    VNODE_PROC_CWD,           /* /proc/<pid>/cwd: a link to the working directory */
    VNODE_PROC_ROOT,          /* /proc/<pid>/root: a link to the root directory */
    VNODE_PROC_DISKSTATS,     /* /proc/diskstats: the transfers of each disk node */
    VNODE_PROC_VERSION,       /* /proc/version: the kernel's name and release */
    VNODE_PROC_PARTITIONS,    /* /proc/partitions: the size of each disk node */
    VNODE_PROC_VMSTAT,        /* /proc/vmstat: the memory and paging counters */
    VNODE_PROC_SLABINFO,      /* /proc/slabinfo: the kernel heap's size classes */
    VNODE_PROC_SWAPS,         /* /proc/swaps: the swap areas, of which there are none */
    VNODE_PROC_LOCKS,         /* /proc/locks: the advisory file locks held */
    VNODE_DEV_LINK,           /* /dev/log: a link to the syslog daemon's socket */
    VNODE_PROC_SYSVIPC_DIR,   /* /proc/sysvipc: the System V objects' directory */
    VNODE_PROC_SYSVIPC,       /* /proc/sysvipc/{shm,sem,msg}, by `procfile` */
    VNODE_PROC_NET_STAT_DIR,  /* /proc/net/stat: the route and ARP counters' directory */
    VNODE_PROC_NS_DIR,        /* /proc/<pid>/ns: the links to its namespaces */
    VNODE_PROC_NS_LINK,       /* /proc/<pid>/ns/<kind>, by `procfd` (proc/ns.h's NS_) */
    VNODE_PROC_FDINFO_DIR,    /* /proc/<pid>/fdinfo: a file for each descriptor */
    VNODE_PROC_FDINFO,        /* /proc/<pid>/fdinfo/<n>: its offset and flags, by `procfd` */
    VNODE_DEV_SUBDIR,         /* /dev/input, /dev/dri: a row of /dev's directories, by `devdir` */
    VNODE_PROC_ACPI_DIR,      /* /proc/acpi: the directory of call */
    VNODE_PROC_ACPI_CALL,     /* /proc/acpi/call: an ACPI method evaluated by its path */
    VNODE_PROC_FS_DIR,        /* /proc/fs: the directory of lockwait */
    VNODE_PROC_FS_LOCKWAIT,   /* /proc/fs/lockwait: each kind of lock's longest disk wait */
    VNODE_PROC_LOCKDEP,       /* /proc/lockdep: the lock-order checker's classes */
    VNODE_PROC_LOCKDEP_STATS, /* /proc/lockdep_stats: its pools and reports */
    VNODE_DEV_MADE,      /* a directory or link root made under /dev, by `syskey` (devtree.h) */
    VNODE_PROC_IRQ_DIR,  /* /proc/irq, pid = -1, or /proc/irq/N, pid = N */
    VNODE_PROC_IRQ_FILE, /* pid = vector; procfile = requested (0), effective (1) */
    /* /proc/asound, pid = -1, or its cardK, pid = K (audio/asound.h) */
    VNODE_PROC_ASOUND_DIR,
    /* a file of /proc/asound, pid = its ASOUND_FILE number */
    VNODE_PROC_ASOUND,
    /* /proc/bus, pid = -1, or its input, pid = 0 */
    VNODE_PROC_BUS_DIR,
    /* a file of /proc/bus/input, pid = evdev.h's EVDEV_PROC_ */
    VNODE_PROC_BUS_INPUT,
    VNODE_ENOENT, /* under a synthetic mount, but no such entry */
    VNODE_ENOTDIR /* under a synthetic mount, a non-dir used as a directory */
};

/** The name of /proc/<pid>/ns's link of the namespace kind `kind`, NULL
 * past the last. */
const char *synth_ns_name(int kind);

/* The files of /proc by their origin: the `procfile` of a classified path,
 * and the origin of the text procfs takes of it at an open. */
#define SYNTH_PROC_STAT          1
#define SYNTH_PROC_CMDLINE       2
#define SYNTH_PROC_BOOTLINE      3
#define SYNTH_PROC_DISKINFO      4
#define SYNTH_PROC_NETDEV        5
#define SYNTH_PROC_UPTIME        6
#define SYNTH_PROC_STATSYS       7
#define SYNTH_PROC_MEMINFO       8
#define SYNTH_PROC_MOUNTS        9
#define SYNTH_PROC_NETROUTE      10
#define SYNTH_PROC_NETARP        11
#define SYNTH_PROC_NETTCP        12
#define SYNTH_PROC_USB           13
#define SYNTH_PROC_FILESYSTEMS   14
#define SYNTH_PROC_LOOP          15
#define SYNTH_PROC_INTERRUPTS    16
#define SYNTH_PROC_CONSOLE       17
#define SYNTH_PROC_AUDIO         18
#define SYNTH_PROC_MAPS          19
#define SYNTH_PROC_STATUS        20
#define SYNTH_PROC_STATM         21
#define SYNTH_PROC_AUXV          22
#define SYNTH_PROC_ENVIRON       23
#define SYNTH_PROC_COMM          24
#define SYNTH_PROC_MEM           25
#define SYNTH_PROC_OOM_ADJ       26
#define SYNTH_PROC_CPUINFO       27
#define SYNTH_PROC_SYSCTL        28
#define SYNTH_PROC_PIDMOUNTS     29
#define SYNTH_PROC_LOADAVG       30
/* The files of /proc/net after the first four, SYNTH_PROC_NETUDP to
 * SYNTH_PROC_NETSNMP, SYNTH_PROC_NETPACKET and SYNTH_PROC_NETSNMP6, in the
 * order they are listed (`synth_net_file`). */
#define SYNTH_PROC_NETUDP        31
#define SYNTH_PROC_NETRAW        32
#define SYNTH_PROC_NETUNIX       33
#define SYNTH_PROC_NETSOCKSTAT   34
#define SYNTH_PROC_NETSNMP       35
/* A device's uevent under /sys, as the origin of its snapshot. */
#define SYNTH_SYS_UEVENT         36
#define SYNTH_PROC_SMAPS         37
#define SYNTH_PROC_DISKSTATS     38
#define SYNTH_PROC_VERSION       39
#define SYNTH_PROC_PARTITIONS    40
#define SYNTH_PROC_LIMITS        41
#define SYNTH_PROC_MOUNTINFO     42
/* /proc/sysvipc/shm, sem and msg, in that order (ipc/sysv.h's kinds). */
#define SYNTH_PROC_SYSVIPC       43
/* /proc/net/stat/rt_cache and arp_cache, two more files of /proc/net by
 * their origin (`synth_net_name`). */
#define SYNTH_PROC_NETSTAT_RT    46
#define SYNTH_PROC_NETSTAT_ARP   47
#define SYNTH_PROC_VMSTAT        48
/* /proc/<pid>/uid_map, gid_map and setgroups, of its user namespace. */
#define SYNTH_PROC_UID_MAP       49
#define SYNTH_PROC_GID_MAP       50
#define SYNTH_PROC_SETGROUPS     51
#define SYNTH_PROC_SLABINFO      52
#define SYNTH_PROC_SWAPS         53
#define SYNTH_PROC_LOCKS         54
/* /proc/<pid>/task/<tid>/children, which a thread's directory alone has. */
#define SYNTH_PROC_CHILDREN      55
#define SYNTH_PROC_NETPACKET     56
/* /proc/acpi/call, read and written as the ACPI method calls it makes. */
#define SYNTH_PROC_ACPI_CALL     57
#define SYNTH_PROC_NETSNMP6      58
/* /proc/net/if_inet6 and ipv6_route, two more of `synth_net_file`'s. */
#define SYNTH_PROC_NETIFINET6    59
#define SYNTH_PROC_NETIPV6RT     60
/* /proc/net/tcp6, udp6 and raw6, three more of `synth_net_file`'s. */
#define SYNTH_PROC_NETTCP6       61
#define SYNTH_PROC_NETUDP6       62
#define SYNTH_PROC_NETRAW6       63
/* /proc/net/netstat, the last of `synth_net_file`'s. */
#define SYNTH_PROC_NETNETSTAT    64
#define SYNTH_PROC_NETWIRELESS   65
/* /proc/fs/lockwait, the record of the sleeping locks (fs/sleeplock.h). */
#define SYNTH_PROC_LOCKWAIT      66
/* /proc/<pid>/numa_maps, each region's memory policy and pages by node. */
#define SYNTH_PROC_NUMA_MAPS     67
/* /proc/<pid>/syscall and wchan, the call and the function a thread sleeps
 * in. */
#define SYNTH_PROC_SYSCALL       68
#define SYNTH_PROC_WCHAN         69
/* /proc/<pid>/schedstat, the thread's run time, its wait for a processor
 * and its switches onto one. */
#define SYNTH_PROC_SCHEDSTAT     70
/* /proc/lockdep and /proc/lockdep_stats, in the lock-order checker's build
 * alone (lib/lockdep.h). */
#define SYNTH_PROC_LOCKDEP       71
#define SYNTH_PROC_LOCKDEP_STATS 72
#define SYNTH_PROC_IRQ_AFFINITY  73
/* /proc/net/softnet_stat, a file of `synth_net_file`'s. */
#define SYNTH_PROC_NETSOFTNET    74
/* A file of /proc/asound, which the descriptor's `origin_pid` names. */
#define SYNTH_PROC_ASOUND        75
/* A file of /proc/bus/input, which the descriptor's `origin_pid` names. */
#define SYNTH_PROC_BUS_INPUT     76
/* /proc/<pid>/sched, the scheduler's record of a thread, whose
 * statistics a write clears. */
#define SYNTH_PROC_SCHED         77
/* /proc/<pid>/autogroup, the id and nice value of the process's session
 * group, whose nice value a write sets. */
#define SYNTH_PROC_AUTOGROUP     78

/* The pid of /proc/self and /dev/fd, which procfs takes as the caller's
 * (`procfs_resolve_pid`). */
#define SYNTH_PID_SELF (-1)

/*
 * A classified path: its kind, and the coordinates of its node, each
 * meaningful for the kinds its comment names. A VNODE_ENOTDIR path keeps
 * those of the file it goes on past, as far as the classifier read them,
 * and the rest stay as they began: an empty name, a pid of 0, a pty number
 * of -1, a row of -1 and a node of 0.
 */
struct vpath {
    int kind;
    char name[269]; /* VNODE_DEV: device name, "input/eventN" below /dev/input,
                     * of up to DEVFS_NODE_NAME_MAX bytes with its NUL */
    int devdir;     /* VNODE_DEV_SUBDIR: the row of /dev's directories (`synth_dev_dir_at`) */
    int pid;        /* VNODE_PROC_*: SYNTH_PID_SELF for /proc/self */
    int procfd;     /* PROC_FD_LINK, PROC_FDINFO: the descriptor number; NS_LINK: the kind */
    int procfile;   /* PROC_FILE, NET_FILE, SYSVIPC: SYNTH_PROC_*; SYS_FILE: the attribute */
    int pts_index;  /* VNODE_PTS: the pty slave number N in /dev/pts/N */
    int tid;        /* VNODE_PROC_*: TID in /proc/<pid>/task/TID, or 0 */
    int sysent;     /* VNODE_PROC_SYS_*: the entry of the /proc/sys table; VNODE_SYS_*: the node */
    int syskey;     /* VNODE_SYS_*: the device of a device's node (`synth_sys_device_name`);
                     * VNODE_DEV_MADE: the entry's id (devtree.h) */
    int sysreg;     /* VNODE_SYS_LINK: the registered link (sysfs.h) the path ends on, or 0 */
};

/**
 * Classify `path`, with or without its leading slash and with `.` and `..`
 * resolved, into `vp`, and return its kind. Empty components are skipped. A
 * path outside /dev, /proc and /sys is VNODE_KRFFS. A name of /dev that is
 * not a special one is VNODE_DEV whether or not the device exists, which
 * devfs decides, and so is a name in a directory of /dev's table, as the
 * directory, a slash and the name, such as "input/event0".
 * Whether a process, a descriptor, a processor or a PCI or USB device
 * exists is for the caller to ask, while an interface, a disk node and a
 * backlight are looked up (`sysfs_net_index`, `sysfs_block_find`,
 * `backlight_find`). A path that goes on past a file is VNODE_ENOTDIR, and
 * the caller asks in the same way whether that file exists, since a path
 * past a missing one is ENOENT. Past /proc/<pid>/cwd, root or fd/<n>,
 * which may lead to a directory, a path is VNODE_ENOENT, as is any other
 * path that names nothing.
 */
int synth_classify(const char *path, struct vpath *vp);

/**
 * Whether `path`, as `synth_classify` takes it, ends on a link of /sys a
 * driver added (sysfs.h), for a walk whose root is the directory of the
 * first `root` bytes of `path`, "sys" or the directory a bind of /sys
 * shows: 1 with the path of the directory the link leads to, "/sys/...", in
 * `out` of `cap` bytes, when that directory lies below the root; -1, with
 * the same in `out`, when it does not; 0 for any other path. The walk takes
 * a `..` after a link of 1 from that directory, as Linux's walk takes it
 * from the link's target, so that libdrm's /sys/dev/char/226:N/device/.. is
 * the parent of the card's device. A link of -1 the VFS follows by its text
 * from the link's directory, through the mounts and the root above the
 * bind, as Linux does: /sys/bus/virtio/devices bound at /mnt has no
 * /mnt/virtio0/.., since its target, ../../../devices/..., climbs out of
 * /mnt.
 */
int synth_sys_link_dir(const char *path, int root, char *out, int cap);

/*
 * The owner the last lookup gave a node of /proc whose Linux inode keeps
 * it, held by each descriptor open on that node (`procfs_owner_keep`): a
 * file of /proc/<pid> outside its net directory, a descriptor's fdinfo
 * file, a process's fd, fdinfo or task directory, or its exe, cwd, root,
 * descriptor or namespace link. Linux sets the owner
 * when a lookup makes or revalidates the inode, and the inode's getattr and
 * permission read it as it is.
 */
struct synth_owner {
    int kept;     /* 1 while on procfs's list of kept owners */
    int node[4];  /* kind, file or descriptor, process or
                     thread, and a thread's process or 0 */
    uint32_t uid; /* the owner the last lookup gave the node */
    uint32_t gid;
    struct synth_owner *next; /* the next kept owner */
};

/*
 * The record from the heap that the `priv` of a descriptor open on a
 * synthetic directory points at, since its coordinates do not fit in the
 * pointer: the directory's kind, its process's pid and, for a thread's
 * directory under /proc/<pid>/task, the thread's id, 0 for any other. A
 * directory of /proc/sys has its entry of the table (`synth_sys_name`) in
 * `pid`, and a directory of /sys its node in `pid` and its key in `tid`. It
 * is defined here so that procfs can give the path /proc/<pid>/fd shows.
 */
struct synth_dir {
    int kind;
    int pid;
    int used; /* 1 from its allocation; nothing reads it */
    int tid;
    struct synth_owner own; /* a /proc/PID/fd, fdinfo or task directory's
                               owner, which fstat and fchdir read */
};

/**
 * Write into `out`, of `cap` bytes, the path of the synthetic directory of
 * `kind` with the coordinates struct synth_dir has: `pid`, and `tid` when it
 * is not 0, for a process's or a thread's directory; the entry in `pid` for
 * a directory of /proc/sys; the node and the key in `pid` and `tid` for one
 * of /sys; the row in `pid` for a directory of /dev's table, and the id for
 * one made under /dev (`devtree_path`). An unknown kind gives "/". The path
 * is NUL-terminated and cut to fit; nothing is written when `cap` is not
 * positive.
 */
void synth_dir_path(int kind, int pid, int tid, char *out, int cap);

/*
 * The directories of /dev, apart from pts, which devpts owns: a row each,
 * in the order /dev lists them. A row's devices are devfs's nodes named
 * "DIR/NAME" (devfs.h), and a row may be in another, as /dev/cpu/0 is in
 * /dev/cpu, with its devices named "cpu/0/NAME". A row marked `always` is
 * present on every machine, as /dev/input is, even when empty; any other
 * only while devfs has a device in it or in a row in it
 * (`devfs_subdir_present`), as /dev/dri is while a display device is
 * bound. A descriptor open on a row's directory has the row in its
 * record's `pid`.
 */
struct synth_dev_dir {
    const char *name; /* its path below /dev: "input", or "cpu/0" in a row */
    uint32_t ino;     /* its inode, apart from devfs's and devpts's numbers */
    int always;       /* 1: present when it has no device */
    int parent;       /* the row it is in, or -1 for one of /dev itself */
};

/** The number of rows of /dev's directories. */
int synth_dev_dir_count(void);

/** Row `n` of /dev's directories, or NULL past the last. */
const struct synth_dev_dir *synth_dev_dir_at(int n);

/** The row whose name is the `len` bytes at `name`, a path below /dev
 * such as "cpu/0", or -1 for none. */
int synth_dev_dir_find(const char *name, int len);

/** The row named by the `len` bytes at `leaf` in row `parent`, or in /dev
 * itself when `parent` is -1: its index, or -1 for none. */
int synth_dev_dir_child(int parent, const char *leaf, int len);

/** The last name of the path of row `d`, "0" for "cpu/0". */
const char *synth_dev_dir_leaf(const struct synth_dev_dir *d);

/*
 * /proc/sys: a few of Linux's sysctl files, in a table of entries, of which
 * those `synth_sys_writable` names take a write. Entry 0 is /proc/sys
 * itself; every other entry has its directory as a `parent` entry, and a
 * file has its text, empty for one whose value procfs reads. Each accessor
 * answers NULL or -1 for an index past the table.
 */

/** The number of entries of the table. */
int synth_sys_count(void);
/* The entries of kernel/hostname and domainname, whose texts are the names
 * of the moment, which procfs reads, and not the table's. */
/* The entry of kernel/pid_max, whose value is proc/process.h's `pid_max`. */
#define SYNTH_SYS_PID_MAX                9
#define SYNTH_SYS_HOSTNAME               11
#define SYNTH_SYS_DOMAINNAME             12
/* The directories net and user, whose files' permissions Linux's
 * net_ctl_permissions and set_permissions give. */
#define SYNTH_SYS_NET                    20
#define SYNTH_SYS_USER                   52
/* The entry of net/ipv4/ip_forward, which takes a write of 0. */
#define SYNTH_SYS_IP_FORWARD             22
/* The entries of fs/inotify's max_queued_events, max_user_instances and
 * max_user_watches, of vm/max_map_count, of kernel/overflowgid and
 * overflowuid, and of kernel/random's boot_id and uuid, whose values procfs
 * reads from the variables of their owners or makes. */
#define SYNTH_SYS_INOTIFY_QUEUED         3
#define SYNTH_SYS_INOTIFY_INSTANCES      4
#define SYNTH_SYS_INOTIFY_WATCHES        5
#define SYNTH_SYS_MAX_MAP_COUNT          32
#define SYNTH_SYS_OVERFLOWGID            33
#define SYNTH_SYS_OVERFLOWUID            34
#define SYNTH_SYS_BOOT_ID                36
#define SYNTH_SYS_UUID                   37
/* The entries of fs/nr_open, fs/file-max and fs/file-nr, and of
 * kernel/randomize_va_space, whose values procfs reads from fs/fd.h's and
 * mm/vm.h's variables; file-nr alone takes no write. */
#define SYNTH_SYS_NR_OPEN                38
#define SYNTH_SYS_FILE_MAX               39
#define SYNTH_SYS_FILE_NR                40
#define SYNTH_SYS_RANDOMIZE_VA_SPACE     41
/* net/ipv6/conf, the directory of IPv6's settings, which holds those of
 * all interfaces, of the default for a new one, and of each interface the
 * caller sees, `lo`, the cards' and the radios', one directory for each of
 * the SYNTH_IPV6_IFACES slots an interface holds while it is there
 * (`netdev_slot`, NETDEV_MAX + 1 of them); and the files of each, in this
 * order, which procfs reads and writes through nd6.h. */
#define SYNTH_SYS_IPV6_CONF              43
#define SYNTH_IPV6_IFACES                5
#define SYNTH_IPV6_DIRS                  (2 + SYNTH_IPV6_IFACES)
#define SYNTH_IPV6_ACCEPT_RA             0
#define SYNTH_IPV6_AUTOCONF              1
#define SYNTH_IPV6_DISABLE               2
#define SYNTH_IPV6_FORWARDING            3
#define SYNTH_IPV6_RA_DEFRTR_METRIC      4
#define SYNTH_IPV6_STABLE_SECRET         5
#define SYNTH_IPV6_USE_TEMPADDR          6
#define SYNTH_IPV6_KEYS                  7
/* The entries of net/ipv4's tcp_keepalive_time, tcp_keepalive_intvl,
 * tcp_keepalive_probes, tcp_mtu_probing and tcp_retries2, in that order,
 * whose values procfs reads from tcp.h's variables. */
#define SYNTH_SYS_TCP_KEEPALIVE_TIME     44
#define SYNTH_SYS_TCP_RETRIES2           48
/* The entry of user/max_mnt_namespaces, whose value is the limit of the
 * reader's user namespace, which procfs reads and writes through ns.h. */
#define SYNTH_SYS_MAX_MNT_NAMESPACES     53
/* The entry of fs/pipe-max-size, whose value procfs reads from and writes
 * to ipc/pipe.h's `pipe_max_size`. */
#define SYNTH_SYS_PIPE_MAX_SIZE          54
/* The entries of fs/pipe-user-pages-hard and fs/pipe-user-pages-soft, whose
 * values are ipc/pipe.h's `pipe_user_pages_hard` and `_soft`. */
#define SYNTH_SYS_PIPE_USER_PAGES_HARD   55
#define SYNTH_SYS_PIPE_USER_PAGES_SOFT   56
/* The entries of kernel/panic and kernel/panic_on_oops, whose values are
 * panic.h's `panic_timeout` and `panic_on_oops`. */
#define SYNTH_SYS_PANIC                  57
#define SYNTH_SYS_PANIC_ON_OOPS          58
/* The entries of the lockup detectors' settings under kernel, from
 * watchdog to hung_task_warnings in the table's order, whose values procfs
 * reads from and writes to watchdog.h's variables; nmi_watchdog, and
 * hung_task_timeout_secs and hung_task_check_interval_secs, whose writes
 * procfs passes on. */
#define SYNTH_SYS_WATCHDOG               59
#define SYNTH_SYS_NMI_WATCHDOG           61
#define SYNTH_SYS_HUNG_TASK_TIMEOUT      70
#define SYNTH_SYS_HUNG_TASK_INTERVAL     71
#define SYNTH_SYS_HUNG_TASK_WARNINGS     72
/* The entries of fs/protected_fifos, protected_hardlinks, protected_regular
 * and protected_symlinks, in that order, whose values are vfs.h's
 * `vfs_protected_*`. */
#define SYNTH_SYS_PROTECTED_FIFOS        73
#define SYNTH_SYS_PROTECTED_SYMLINKS     76
/* The entries of vm's write-back settings, from dirty_background_bytes to
 * dirtytime_expire_seconds in the table's order, whose values are bcache.h's
 * and writeback.h's variables; they follow the protected files', and every
 * entry from protected_fifos to dirtytime_expire_seconds takes a write. */
#define SYNTH_SYS_DIRTY_BACKGROUND_BYTES 77
#define SYNTH_SYS_DIRTY_BACKGROUND_RATIO 78
#define SYNTH_SYS_DIRTY_BYTES            79
#define SYNTH_SYS_DIRTY_EXPIRE           80
#define SYNTH_SYS_DIRTY_RATIO            81
#define SYNTH_SYS_DIRTY_WRITEBACK        82
#define SYNTH_SYS_DIRTYTIME_EXPIRE       83
/* The entry of kernel/lock_stat, whose value is fs/sleeplock.h's
 * `sleep_lock_stat`. */
#define SYNTH_SYS_LOCK_STAT              84
/* The entry of net/core/netdev_max_backlog, whose value is ip.h's
 * `netdev_max_backlog`. */
#define SYNTH_SYS_NETDEV_MAX_BACKLOG     86
/* The entry of kernel/ns_last_pid, the last process ID of the reader's PID
 * namespace (`pidns_last_pid` in proc/ns.h), which anyone may open and
 * only a writer privileged over the namespace may write, as on Linux. */
#define SYNTH_SYS_NS_LAST_PID            87
/* The entries of net/core/netdev_budget and net/core/netdev_budget_usecs,
 * whose values are ip.h's `netdev_budget` and `netdev_budget_usecs`. */
#define SYNTH_SYS_NETDEV_BUDGET          88
#define SYNTH_SYS_NETDEV_BUDGET_USECS    89
/* The entry of kernel/sched_schedstats, whose value is proc/fair.h's
 * `fair_schedstats`. */
#define SYNTH_SYS_SCHED_SCHEDSTATS       90
/* The entries of kernel/sched_autogroup_enabled (proc/autogroup.h's
 * `autogroup_enabled`), sched_rr_timeslice_ms, sched_rt_period_us and
 * sched_rt_runtime_us (proc/sched.h's `sched_rr_timeslice_ms` and
 * `sched_rt_bandwidth`). */
#define SYNTH_SYS_SCHED_AUTOGROUP        91
#define SYNTH_SYS_SCHED_RR_TIMESLICE     92
#define SYNTH_SYS_SCHED_RT_PERIOD        93
#define SYNTH_SYS_SCHED_RT_RUNTIME       94
/**
 * Whether `entry` is under net/ipv6/conf: 1 with the name of its directory
 * in `*dir`, `all`, `default` or an interface's, and in `*key` the
 * SYNTH_IPV6_* of its file, or -1 for the directory itself; 0 for any
 * other entry. The directory of a slot that holds no interface the caller
 * sees is named by the empty string and has no parent, and so is not found
 * or listed.
 */
int synth_sys_ipv6(int entry, const char **dir, int *key);
/** Whether a privileged caller may write the file entry `entry`: not
 * nmi_watchdog where the hard-lockup detector cannot run, whose file Linux
 * makes 0444 (kernel/watchdog.c:1162 and :1173-1174), so that an open for
 * writing fails with EACCES. */
int synth_sys_writable(int entry);
/** The mode of the file entry `entry`, as Linux's tables give it, each
 * owned by root: 0600 for net/ipv6/conf's stable_secret
 * (net/ipv6/addrconf.c), 0666 for kernel/ns_last_pid, 0644 for any other
 * file that takes a write and 0444 for the rest. */
unsigned synth_sys_mode(int entry);

/** The name of the entry `entry`, the last component of its path. */
const char *synth_sys_name(int entry);

/** The entry of the directory `entry` is in, -1 for /proc/sys itself. */
int synth_sys_parent(int entry);

/** The text of a file entry, NULL for a directory. */
const char *synth_sys_text(int entry);

/** The path in /proc/net of the file of origin `origin`: one that
 * `synth_net_file` gives, or SYNTH_PROC_NETSTAT_RT or SYNTH_PROC_NETSTAT_ARP,
 * whose two are under stat; NULL for another. */
const char *synth_net_name(int origin);
/** The origin of file `n`, from 0, of /proc/net after the first four, or 0
 * past the last. */
int synth_net_file(int n);
/** The place, from 0, of the file of origin `origin` among those
 * `synth_net_name` names, the two under stat last; -1 for another. */
int synth_net_slot(int origin);
/** The name of the /proc/sysvipc file of origin `origin`, or NULL. */
const char *synth_sysvipc_name(int origin);

/*
 * /sys: the parts of Linux's sysfs that programs here read: the PCI
 * functions and USB devices with their uevent files, the network interfaces
 * and their statistics, the clock and its wake alarm, the backlights, the
 * sleep states, the disk nodes, the processors with their caches, the NUMA
 * node, the clock sources, the firmware's DMI fields, the memory block, and
 * the entries drivers add (sysfs.h) below these and below the empty
 * directories they need, /sys/class/drm, /sys/dev/char, /sys/kernel and
 * /sys/devices/platform. Each
 * directory is a numbered node, below; a node of which there is one for each
 * device, interface, backlight, processor or disk node has a `syskey` that
 * says which. A backlight's key is its index (display/backlight.h). A PCI
 * function's key is its bus, device and function as the configuration
 * address has them, below 2^16; a USB device's is its controller's number
 * less 1 in bits 28 and 29, its root port in the eight below, and the ports
 * of up to five hubs below it in four bits each, as its name gives them, at
 * least 2^20.
 */
#define SYSFS_ROOT            0  /* /sys */
#define SYSFS_BUS             1  /* /sys/bus */
#define SYSFS_BUS_PCI         2  /* /sys/bus/pci */
#define SYSFS_BUS_USB         3  /* /sys/bus/usb */
#define SYSFS_PCI_LINKS       4  /* /sys/bus/pci/devices */
#define SYSFS_USB_LINKS       5  /* /sys/bus/usb/devices */
#define SYSFS_DEVICES         6  /* /sys/devices */
#define SYSFS_PCI_ROOT        7  /* /sys/devices/pci0000:00 */
#define SYSFS_USB_ROOT        8  /* /sys/devices/usb */
#define SYSFS_PCI_DEVICE      9  /* /sys/devices/pci0000:00/0000:BB:DD.F */
#define SYSFS_USB_DEVICE      10 /* /sys/devices/usb/B-P[.P...] */
/* The `procfile` of a PCI function's `config`, its configuration space;
 * its uevent's is 0. The file numbers of sysfs_ino are one more, and 2 is
 * the function's link. */
#define SYSFS_PCI_CONFIG      2
#define SYSFS_CLASS           11 /* /sys/class */
#define SYSFS_CLASS_NET       12 /* /sys/class/net */
#define SYSFS_NET_IF          13 /* /sys/class/net/IF, whose key is its index */
#define SYSFS_NET_STATS       14 /* /sys/class/net/IF/statistics */
#define SYSFS_CLASS_RTC       15 /* /sys/class/rtc */
#define SYSFS_RTC0            16 /* /sys/class/rtc/rtc0, the CMOS clock */
#define SYSFS_RTC_DEVICE      17 /* /sys/class/rtc/rtc0/device */
#define SYSFS_RTC_POWER       18 /* /sys/class/rtc/rtc0/device/power */
#define SYSFS_POWER           19 /* /sys/power, the sleep states */
#define SYSFS_BLOCK           20 /* /sys/block */
#define SYSFS_BLOCK_DISK      21 /* /sys/block/DISK, whose key is its disk node's index */
#define SYSFS_BLOCK_PART      22 /* /sys/block/DISK/PART, the same of the partition */
#define SYSFS_BLOCK_QUEUE     23 /* /sys/block/DISK/queue */
#define SYSFS_BLOCK_DEV       24 /* /sys/block/DISK/device */
#define SYSFS_DEV             25 /* /sys/dev */
#define SYSFS_DEV_BLOCK       26 /* /sys/dev/block, a link for each disk node */
#define SYSFS_SYSTEM          27 /* /sys/devices/system */
#define SYSFS_CPU             28 /* /sys/devices/system/cpu, the processors */
#define SYSFS_CPUN            29 /* /sys/devices/system/cpu/cpuN, whose key is N */
#define SYSFS_MEMORY          30 /* /sys/devices/system/memory, the memory blocks */
#define SYSFS_MEMORY0         31 /* /sys/devices/system/memory/memory0 */
#define SYSFS_BLOCK_LOOP      32 /* /sys/block/loopN/loop, while a file backs it */
#define SYSFS_CLASS_BACKLIGHT 33 /* /sys/class/backlight */
#define SYSFS_BACKLIGHT       34 /* /sys/class/backlight/NAME, whose key is its index */
#define SYSFS_CPU_TOPOLOGY    35 /* /sys/devices/system/cpu/cpuN/topology, whose key is N */

#define SYSFS_CLASS_IEEE80211 36 /* /sys/class/ieee80211 */
#define SYSFS_WLAN_PHY        37 /* phyN, key is the radio index */
#define SYSFS_WLAN_DEVICE     38 /* phyN/device */
#define SYSFS_WLAN_DRIVER     39 /* phyN/device/DRIVER */
#define SYSFS_CLASS_RFKILL    40 /* /sys/class/rfkill */
#define SYSFS_RFKILL          41 /* /sys/class/rfkill/rfkillN, whose key is N */
/* An entry a driver added (sysfs.h), whose key is its handle. */
#define SYSFS_REG             42
#define SYSFS_KERNEL          43 /* /sys/kernel */
#define SYSFS_KERNEL_DEBUG    44 /* /sys/kernel/debug, where debugfs is mounted */
#define SYSFS_CLASS_DRM       45 /* /sys/class/drm, a link for each DRM node and connector */
#define SYSFS_DEV_CHAR        46 /* /sys/dev/char, a link for each registered device */
#define SYSFS_PLATFORM        47 /* /sys/devices/platform */
#define SYSFS_BUS_PLATFORM    48 /* /sys/bus/platform */
#define SYSFS_PLATFORM_LINKS  49 /* /sys/bus/platform/devices */
#define SYSFS_CPU_CACHE       50 /* /sys/devices/system/cpu/cpuN/cache, whose key is N */
/* /sys/devices/system/cpu/cpuN/cache/indexM, whose key is N << 4 | M: the
 * processor's cache M (fs/cpuinfo.h's `cpuinfo_cache`). */
#define SYSFS_CPU_CACHE_INDEX 51
#define SYSFS_NODE            52 /* /sys/devices/system/node, the NUMA nodes */
#define SYSFS_NODE0           53 /* /sys/devices/system/node/node0, the one node */
#define SYSFS_CLOCKSOURCE     54 /* /sys/devices/system/clocksource */
#define SYSFS_CLOCKSOURCE0    55 /* /sys/devices/system/clocksource/clocksource0 */
#define SYSFS_CLASS_DMI       56 /* /sys/class/dmi */
/* /sys/class/dmi/id, the firmware's SMBIOS fields: its files are numbered
 * as dmi.h's DMI_ fields, and modalias follows them. */
#define SYSFS_DMI_ID          57
#define SYSFS_DMI_MODALIAS    22
#define SYSFS_KERNEL_MM       58 /* /sys/kernel/mm */
#define SYSFS_THP             59 /* /sys/kernel/mm/transparent_hugepage */
#define SYSFS_THP_PMD_SIZE    3  /* its hpage_pmd_size, the one read-only file */
/* /sys/kernel/mm/transparent_hugepage/khugepaged, whose two counts are
 * read-only. */
#define SYSFS_THP_KHUGEPAGED  60
#define SYSFS_KHP_FULL_SCANS  2 /* full_scans */
#define SYSFS_KHP_COLLAPSED   6 /* pages_collapsed */

/* The `procfile` of each file of a PCI function's directory, by Linux's
 * names (`sysfs_pci_attr`); 1 has no file, since 1 + 1 is the inode number
 * of the function's link. boot_vga is there only for a VGA function and a
 * resourceN file only for a BAR with a length; resourceN_wc also needs a
 * prefetchable memory BAR and write-combining (`sysfs_pci_attr_present`). */
#define SYSFS_PCI_UEVENT           0
#define SYSFS_PCI_VENDOR           3
#define SYSFS_PCI_DEVICE_ID        4
#define SYSFS_PCI_SUBSYSTEM_VENDOR 5
#define SYSFS_PCI_SUBSYSTEM_DEVICE 6
#define SYSFS_PCI_CLASS            7
#define SYSFS_PCI_REVISION         8
#define SYSFS_PCI_BOOT_VGA         9
#define SYSFS_PCI_RESOURCE         10
/* resource0 to resource5, and resource0_wc to resource5_wc, as Linux's
 * pci_create_resource_files makes them. */
#define SYSFS_PCI_RESOURCE0        11
#define SYSFS_PCI_BARS             6
#define SYSFS_PCI_RESOURCE0_WC     17
#define SYSFS_PCI_ATTRS            23

/** The name of the file `attr` of a PCI function's directory, or NULL for a
 * number with none. */
const char *sysfs_pci_attr(int attr);

/** Whether the file `attr` of the /sys directory `node` of device `key`
 * takes a privileged write: one `sysfs_attr_writable` names, or a
 * registered attribute with a `store`, whose handle is `key`. */
int sysfs_file_writable(int node, int key, int attr);

/** Return the radio index of a network interface, or -1. */
int sysfs_net_wiphy(int index);
/** Return a radio's static driver name, or NULL if absent. */
const char *sysfs_wlan_driver(int index);

/** Whether the /sys directory `node` is one of a device, an interface, a
 * processor, a disk node or a registered entry, which `syskey` says, and
 * which exists only while that does. */
#define SYSFS_KEYED(node)                                                                         \
    ((node) == SYSFS_PCI_DEVICE || (node) == SYSFS_USB_DEVICE || (node) == SYSFS_NET_IF ||        \
     (node) == SYSFS_NET_STATS || (node) == SYSFS_CPUN || (node) == SYSFS_CPU_TOPOLOGY ||         \
     (node) == SYSFS_CPU_CACHE || (node) == SYSFS_CPU_CACHE_INDEX || (node) == SYSFS_BACKLIGHT || \
     SYSFS_IS_BLOCK(node) || (node) == SYSFS_RFKILL ||                                            \
     ((node) >= SYSFS_WLAN_PHY && (node) <= SYSFS_WLAN_DRIVER) || (node) == SYSFS_REG)

/** Whether `node` is a directory of a disk node: its own, its queue's, its
 * device's or a loop device's `loop`. */
#define SYSFS_IS_BLOCK(node) \
    (((node) >= SYSFS_BLOCK_DISK && (node) <= SYSFS_BLOCK_DEV) || (node) == SYSFS_BLOCK_LOOP)

/** Whether `node` is a directory of the radios or of the radio switches,
 * /sys/class/ieee80211 and /sys/class/rfkill and what lies below them. */
#define SYSFS_IS_RADIO(node) ((node) >= SYSFS_CLASS_IEEE80211 && (node) <= SYSFS_RFKILL)

/** The name of the `n`th attribute file of the /sys directory `node`, in the
 * order the directory lists them, or NULL past the last, and for a node
 * with none. */
const char *sysfs_attr(int node, int n);

/** Whether the attribute `attr` of `node` is root's alone, mode 0400 as
 * Linux gives them: the serial numbers and the UUID of /sys/class/dmi/id. */
int sysfs_attr_private(int node, int attr);

/** The mode bits of the attribute `attr` of the fixed /sys directory `node`,
 * owned by root as stat(2) reports them: 0644 for a writable one, 0400 for a
 * private one (`sysfs_attr_private`) and 0444 for the rest. */
uint32_t sysfs_attr_mode(int node, int attr);

/** Whether the attribute `attr` of the fixed /sys directory `node` is
 * there, in procfs.c: a field of /sys/class/dmi/id only when the firmware's
 * table holds it, as Linux makes only those files; every other is. */
int sysfs_attr_present(int node, int attr);

/** Whether the attribute `attr` of `node` is a device's uevent: a PCI
 * function's, a USB device's, an interface's, a disk's, a partition's, a
 * radio's or a radio switch's. */
int sysfs_attr_uevent(int node, int attr);

/** Whether the attribute `attr` of `node` takes a privileged write:
 * rtc0's wakealarm, /sys/power's state, a backlight's brightness and
 * bl_power, a radio switch's soft and state, a device's uevent
 * (`sysfs_attr_uevent`), and each transparent-hugepage policy file. */
int sysfs_attr_writable(int node, int attr);

/** The interfaces' part, in procfs.c: the index of the interface named by
 * the `len` bytes at `name`, or -1. */
int sysfs_net_index(const char *name, int len);

/** The name of the interface `index`, NUL-terminated, into `out` of `cap`
 * bytes. */
void sysfs_net_name(int index, char *out, int cap);

/** The name of the interface the caller sees in the slot `slot`, 1 to
 * SYNTH_IPV6_IFACES (`netdev_slot`), or the empty string when it sees none
 * there. The name is the interface's own, valid while it stays registered. */
const char *sysfs_net_slot_name(int slot);

/** The PCI key of the card of the interface `index`, or -1. */
int sysfs_net_pci_key(int index);

/** The disk nodes' part, in procfs.c, each node known by its index in
 * devfs's table: the node named by the `len` bytes at `name`, a whole
 * disk's when `disk` is -1 and else a partition of the disk whose node is
 * `disk`, or -1. */
int sysfs_block_find(const char *name, int len, int disk);

/** The node of the numbers MAJOR:MINOR the `len` bytes at `name` give, or
 * -1. */
int sysfs_block_devno(const char *name, int len);

/** The name of the node `key`, NUL-terminated, into `out` of `cap` bytes. */
void sysfs_block_name(int key, char *out, int cap);

/** The index of the whole disk the node `key` is on, itself for a whole
 * disk, or -1 for none. */
int sysfs_block_whole(int key);

/** The loop device the disk node `key` is while a file backs it, or -1:
 * the one disk whose directory has `loop`, as Linux gives a bound loop
 * device. */
int sysfs_block_loop(int key);

struct usb_sys_row;

/** The USB part, in procfs.c: the key of the running USB device `r`
 * (usb/core.h's `usb_core_row`), or -1 when a port of it does not fit
 * one. */
int sysfs_usb_key(const struct usb_sys_row *r);

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

/* Whether a classified kind is a node of /dev, /proc or /sys. */
#define VNODE_IS_SYNTH(kind) ((kind) != VNODE_KRFFS)

#endif /* _SYNTHFS_H_ */
