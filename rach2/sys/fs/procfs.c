/*
 * procfs.c: the synthetic /proc, generated from the process table.
 *
 * A /proc text file is snapshotted into a small pooled buffer when opened and
 * served from there (so a read sees a consistent moment, and dup/fork share the
 * snapshot until the last descriptor closes; the per-open-file-description
 * lifetime). The text is built with bounded append helpers and hand-rolled
 * integer formatting (the kernel has no snprintf and avoids 64-bit division).
 */

#include "../klog.h"
#include "../isr.h"
#include "../lapic.h"
#include "procfs.h"
#include "synthfs.h"
#include "fd.h"
#include "file.h"
#include "devfs.h"
#include "krffs.h"
#include "../usb/core.h"
#include "../usb/usb.h"
#include "../block/usbdisk.h"
#include "../tty/pty.h"
#include "../io/poll.h"
#include "../cmdline.h"
#include "../block/diskinfo.h"
#include "vfs.h"
#include "../network/ethernet/ethernet.h"
#include "../network/driver/netdev.h"
#include "../network/arp/arp.h"
#include "../network/tcp/tcp.h"
#include "../network/netconf.h"
#include "../network/net.h"
#include "../network/socket/socket.h"
#include "../ipc/pipe.h"
#include "../proc/process.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"
#include "../serial.h"
#include "../rtc.h"
#include "../proc/sched.h"
#include "../mm/paging.h"

#define PROCFS_O_CLOEXEC 02000000

/* Inode namespace for /proc: the directory, the boot command line, then a
 * per-pid base. */
#define PROCFS_INO_DIR 0x100u
#define PROCFS_INO_CMDLINE 0x101u
#define PROCFS_INO_DISK    0x102u
#define PROCFS_INO_NETDIR  0x103u
#define PROCFS_INO_NETDEV  0x104u
#define PROCFS_INO_UPTIME  0x105u
#define PROCFS_INO_STATSYS 0x106u
#define PROCFS_INO_MEMINFO 0x107u
#define PROCFS_INO_MOUNTS  0x108u
#define PROCFS_INO_USB     0x10Cu
#define PROCFS_INO_FILESYSTEMS 0x10Du
#define PROCFS_INO_INTERRUPTS 0x10Eu
#define PROCFS_INO_NETROUTE 0x109u
#define PROCFS_INO_NETARP  0x10Au
#define PROCFS_INO_NETTCP  0x10Bu
#define PROCFS_INO_PID_BASE 0x200u

/*
 * The room a snapshot is built in, and the bound each file is allowed of it.
 *
 * Every file declares a worst case that can be argued from a fixed pool: the
 * process table, the ARP cache and the tcb pool all have a size the kernel
 * chose, so the largest text each can produce is a number and not a guess. The
 * buffer is the largest of those bounds, written out rather than derived so
 * that declaring a larger file without raising it stops the build instead of
 * borrowing another file's room. A text that would exceed its own bound is
 * reported and refused rather than served short, which is what the appenders
 * used to do: a sixteen-row table read back as a nine-row one with nothing
 * anywhere saying so.
 */
#define PROCFS_TEXT_MAX 3072

/* Per-file bounds. Each is the file's own worst case, and each is checked
   against the buffer below at compile time. */
#define PROCFS_MAX_BOOTLINE 320  /* the 256-byte boot line, a newline, slack */
#define PROCFS_MAX_DISK     768  /* diskinfo_format's widest record */
#define PROCFS_MAX_NETDEV   512  /* two header lines and one interface */
#define PROCFS_MAX_PIDSTAT  256  /* one line of small fields plus comm */
#define PROCFS_MAX_PIDCMD    64  /* comm and its terminator */
#define PROCFS_MAX_UPTIME    64  /* two decimal numbers */
#define PROCFS_MAX_STATSYS  256  /* the cpu lines, btime, and the counters */
#define PROCFS_MAX_MEMINFO  256  /* seven sized lines */
#define PROCFS_MAX_MOUNTS   256  /* the root, and the two synthetic mounts */
/* Four devices and two sticks, one line each, and the line that counts the
 * names. A stick's line has two names a drive chose the text of. */
#define PROCFS_MAX_USB      576
#define PROCFS_MAX_FILESYSTEMS 128 /* two nodev lines and four types */
/* Thirty-two vectors at most, each a line of its number, its count, which of
   the two roads it arrives on, and up to four names of at most a dozen
   characters, plus the two lines that belong to no vector. */
#define PROCFS_MAX_INTERRUPTS 2304
#define PROCFS_MAX_NETROUTE 512  /* a header and at most two routes */
#define PROCFS_MAX_NETARP  1024  /* a header and the eight-entry cache */
#define PROCFS_MAX_NETTCP  3072  /* a header and the sixteen-block pool */

/* The header line of each network table, named so the same text is printed and
   measured, and the widest row each can print. A row is a fixed-width shape:
   the decimal fields are uint32 at their widest, the addresses and queues are
   hex of a known width, and the rest is literal. */
#define PROCFS_NETROUTE_HEADER "Iface\tDestination\tGateway \tFlags\tRefCnt" \
                               "\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
#define PROCFS_NETARP_HEADER   "IP address       HW type     Flags       " \
                               "HW address            Mask     Device\n"
#define PROCFS_NETTCP_HEADER   "  sl  local_address rem_address   st tx_queue" \
                               " rx_queue tr tm->when retrnsmt   uid  timeout" \
                               " inode\n"
#define PROCFS_DEC32       10 /* a uint32 in decimal, at its widest */
#define PROCFS_IFNAME_LEN  (sizeof(NETDEV_IFNAME) - 1)
#define PROCFS_NETROUTE_ROW (PROCFS_IFNAME_LEN + 8 + 8 + 34)
#define PROCFS_NETARP_ROW   (15 + 32 + 17 + 14 + PROCFS_IFNAME_LEN + 1)
#define PROCFS_NETTCP_ROW   (3 + PROCFS_DEC32 + 2 + 13 + 1 + 13 + 1 + 2 + 1 + \
                             17 + 22 + PROCFS_DEC32 + 10 + PROCFS_DEC32 + 33)

/* C89 has no static assert: a bound that does not fit makes this array size
   negative and the build stops here rather than truncating at run time. The
   network tables are checked against the tables behind them, so growing the
   connection pool or the ARP cache fails the build instead of truncating. */
typedef char procfs_bounds_fit[(PROCFS_MAX_BOOTLINE <= PROCFS_TEXT_MAX &&
                                PROCFS_MAX_DISK <= PROCFS_TEXT_MAX &&
                                PROCFS_MAX_NETDEV <= PROCFS_TEXT_MAX &&
                                PROCFS_MAX_PIDSTAT <= PROCFS_TEXT_MAX &&
                                PROCFS_MAX_PIDCMD <= PROCFS_TEXT_MAX &&
                                PROCFS_MAX_UPTIME <= PROCFS_TEXT_MAX &&
                                PROCFS_MAX_STATSYS <= PROCFS_TEXT_MAX &&
                                PROCFS_MAX_MEMINFO <= PROCFS_TEXT_MAX &&
                                PROCFS_MAX_MOUNTS <= PROCFS_TEXT_MAX &&
                                PROCFS_MAX_USB <= PROCFS_TEXT_MAX &&
                                PROCFS_MAX_FILESYSTEMS <= PROCFS_TEXT_MAX &&
                                PROCFS_MAX_INTERRUPTS <= PROCFS_TEXT_MAX &&
                                PROCFS_MAX_NETROUTE <= PROCFS_TEXT_MAX &&
                                PROCFS_MAX_NETARP <= PROCFS_TEXT_MAX &&
                                PROCFS_MAX_NETTCP <= PROCFS_TEXT_MAX &&
                                sizeof(PROCFS_NETROUTE_HEADER) - 1 +
                                        2 * PROCFS_NETROUTE_ROW <=
                                    PROCFS_MAX_NETROUTE &&
                                sizeof(PROCFS_NETARP_HEADER) - 1 +
                                        ARP_CACHE_SIZE * PROCFS_NETARP_ROW <=
                                    PROCFS_MAX_NETARP &&
                                sizeof(PROCFS_NETTCP_HEADER) - 1 +
                                        MAX_TCP_CONNS * PROCFS_NETTCP_ROW <=
                                    PROCFS_MAX_NETTCP)
                                   ? 1
                                   : -1];

/** The file's name, for the one diagnostic a text over its bound prints. */
static const char *procfs_kind_name(const struct vpath *vp)
{
    if (vp->kind == VNODE_PROC_CMDLINE) {
        return "/proc/cmdline";
    }
    if (vp->kind == VNODE_PROC_DISK) {
        return "/proc/disk";
    }
    if (vp->kind == VNODE_PROC_NET_DEV) {
        return "/proc/net/dev";
    }
    if (vp->kind == VNODE_PROC_UPTIME) {
        return "/proc/uptime";
    }
    if (vp->kind == VNODE_PROC_STAT_SYS) {
        return "/proc/stat";
    }
    if (vp->kind == VNODE_PROC_MEMINFO) {
        return "/proc/meminfo";
    }
    if (vp->kind == VNODE_PROC_MOUNTS) {
        return "/proc/mounts";
    }
    if (vp->kind == VNODE_PROC_USB) {
        return "/proc/usb";
    }
    if (vp->kind == VNODE_PROC_INTERRUPTS) {
        return "/proc/interrupts";
    }
    if (vp->kind == VNODE_PROC_FILESYSTEMS) {
        return "/proc/filesystems";
    }
    if (vp->kind == VNODE_PROC_NET_ROUTE) {
        return "/proc/net/route";
    }
    if (vp->kind == VNODE_PROC_NET_ARP) {
        return "/proc/net/arp";
    }
    if (vp->kind == VNODE_PROC_NET_TCP) {
        return "/proc/net/tcp";
    }
    if (vp->procfile == SYNTH_PROC_STAT) {
        return "/proc/<pid>/stat";
    }
    return "/proc/<pid>/cmdline";
}

/* A /proc text file, snapshotted at open and served through file_ops. */
struct procfs_file {
    char data[PROCFS_TEXT_MAX];
    int len;
    int used;
    int origin_pid;  /* the /proc/<pid> this snapshot came from (for fd readlink) */
    int origin_file; /* SYNTH_PROC_STAT or SYNTH_PROC_CMDLINE */
};

/*
 * One text under construction: where it goes, how much of it this file is
 * allowed, how much is written, and whether an append did not fit.
 *
 * `cap` is the file's declared bound rather than the buffer's size, so a file
 * that outgrows what was argued for it fails on its own terms instead of
 * quietly borrowing another file's room.
 */
struct procfs_text {
    char *buf;
    int cap;
    int len;
    int over;
};

static void procfs_text_init(struct procfs_text *t, char *buf, int cap)
{
    t->buf = buf;
    t->cap = cap;
    t->len = 0;
    t->over = 0;
}

/* bounded text builders (no snprintf; cap leaves room for a NUL) */

static void ap_ch(struct procfs_text *t, char c)
{
    if (t->len < t->cap - 1) {
        t->buf[t->len] = c;
        t->len++;
    } else {
        t->over = 1;
    }
}

static void ap_str(struct procfs_text *t, const char *s)
{
    while (*s) {
        ap_ch(t, *s++);
    }
}

/** Append `v` as unsigned decimal; the counters are 32-bit and wrap. */
static void ap_u32(struct procfs_text *t, uint32_t v)
{
    char tmp[12];
    int n = 0;

    if (v == 0) {
        ap_ch(t, '0');
        return;
    }
    while (v && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + (v % 10u));
        v /= 10u;
    }
    while (n > 0) {
        ap_ch(t, tmp[--n]);
    }
}

/* Append a signed decimal. INT_MIN is handled by negating in unsigned space. */
static void ap_int(struct procfs_text *t, int v)
{
    char tmp[12];
    int n = 0;
    unsigned int uv;

    if (v < 0) {
        ap_ch(t, '-');
        uv = (unsigned int)(-(v + 1)) + 1u;
    } else {
        uv = (unsigned int)v;
    }
    if (uv == 0) {
        ap_ch(t, '0');
        return;
    }
    while (uv) {
        tmp[n++] = (char)('0' + (uv % 10u));
        uv /= 10u;
    }
    while (n) {
        ap_ch(t, tmp[--n]);
    }
}

/* queries over the process table */

/**
 * Resolve a /proc pid token to a live pid, or -1.
 *
 * SYNTH_PID_SELF maps to the caller's pid. A numeric pid must name a process
 * that exists and is not PROC_UNUSED.
 */
int procfs_resolve_pid(int pid)
{
    process_t *p;

    if (pid == SYNTH_PID_SELF) {
        return current_proc ? (int)current_proc->pid : -1;
    }
    if (pid < 0) {
        return -1;
    }
    p = get_process((uint32_t)pid);
    if (!p || p->state == PROC_UNUSED) {
        return -1;
    }
    return pid;
}

static char proc_state_char(proc_state_t s)
{
    switch (s) {
        case PROC_RUNNING:
        case PROC_READY:
            return 'R';
        case PROC_BLOCKED:
            return 'S';
        case PROC_ZOMBIE:
            return 'Z';
        default:
            return 'R';
    }
}

/**
 * Format /proc/<pid>/stat into `t`, returning its length or -ENOENT.
 *
 * Emits the space-separated fields ps and friends parse. comm (field 2) is
 * wrapped in parentheses, with any stray parens scrubbed so a naive parser that
 * scans for the closing ')' is not fooled. Accounting fields we do not track
 * (faults, cpu time, sizes) are zero, but every field is present and in order.
 */
static int procfs_gen_stat(struct procfs_text *t, int pid)
{
    process_t *p;
    int i;

    p = get_process((uint32_t)pid);
    if (!p || p->state == PROC_UNUSED) {
        return -ENOENT;
    }

    ap_int(t, pid);             /* 1 pid */
    ap_str(t, " (");            /* 2 comm */
    if (p->comm[0]) {
        for (i = 0; i < (int)sizeof(p->comm) && p->comm[i]; i++) {
            char c = p->comm[i];
            ap_ch(t, (c == '(' || c == ')') ? '_' : c);
        }
    } else {
        ap_ch(t, '?');
    }
    ap_str(t, ") ");
    ap_ch(t, proc_state_char(p->state)); /* 3 state */
    ap_ch(t, ' ');
    ap_int(t, (int)p->parent_pid); /* 4 ppid */
    ap_ch(t, ' ');
    ap_int(t, (int)p->pgid); /* 5 pgrp */
    ap_ch(t, ' ');
    ap_int(t, (int)p->sid); /* 6 session */
    ap_str(t, " 0 -1"); /* 7 tty_nr, 8 tpgid (not modeled) */
    /* 9..13 flags,minflt,cminflt,majflt,cmajflt;
       14..17 utime,stime,cutime,cstime */
    ap_str(t, " 0 0 0 0 0 0 0 0 0");
    ap_str(t, " 20 0"); /* 18 priority, 19 nice */
    ap_str(t, " 1 0 0"); /* 20 num_threads, 21 itrealvalue, 22 starttime */
    ap_str(t, " 0 0"); /* 23 vsize, 24 rss */
    ap_ch(t, '\n');
    t->buf[t->len] = '\0';
    return t->len;
}

/**
 * Format /proc/<pid>/cmdline into `t`, returning its length or -ENOENT.
 *
 * argv is NUL-separated. We do not retain the full argv, so the program name is
 * emitted as the single argument (busybox falls back to comm).
 */
static int procfs_gen_cmdline(struct procfs_text *t, int pid)
{
    process_t *p;
    int i;

    p = get_process((uint32_t)pid);
    if (!p || p->state == PROC_UNUSED) {
        return -ENOENT;
    }
    for (i = 0; i < (int)sizeof(p->comm) && p->comm[i]; i++) {
        ap_ch(t, p->comm[i]);
    }
    if (t->len > 0) {
        ap_ch(t, '\0'); /* argv[0] terminator */
    }
    return t->len;
}

/**
 * Render the /proc/<pid>/fd/<fd> symlink target into `buf`, or -ENOENT.
 *
 * The target string matches what each descriptor type reports through the real
 * Linux /proc: a path for files and dirs, /dev/console, /dev/pts/N, /dev/ptmx,
 * and the pipe:[1]/socket:[N]/anon_inode forms. musl ttyname_r relies on the
 * /dev/pts/N spelling. A socket names its own inode, the same number
 * `/proc/net/tcp` and `fstat` report, so one socket is one thing to a program
 * reading all three; it was a constant before, which made every socket on the
 * machine look like the same one.
 */
int procfs_readlink_fd(int pid, int fd, char *buf, int cap)
{
    struct procfs_text text;
    struct procfs_text *t = &text;
    process_t *p;
    fd_entry_t *e;

    procfs_text_init(t, buf, cap);

    p = get_process((uint32_t)pid);
    if (!p || p->state == PROC_UNUSED) {
        return -ENOENT;
    }
    if (fd < 0 || fd >= MAX_FDS) {
        return -ENOENT;
    }
    e = &p->fds.entries[fd];
    if (e->type == FD_TYPE_NONE) {
        return -ENOENT;
    }

    switch (e->type) {
        case FD_TYPE_CONSOLE:
            ap_str(t, "/dev/console");
            break;
        case FD_TYPE_DEVICE:
            ap_str(t, "/dev/");
            ap_str(t, devfs_fd_name(e));
            break;
        case FD_TYPE_PIPE: {
            pipe_t *pp = (pipe_t *)e->priv;
            ap_str(t, "pipe:[");
            ap_u32(t, pp ? pp->inode : 0u);
            ap_ch(t, ']');
            break;
        }
        case FD_TYPE_PTS:
            /* a pty slave: /dev/pts/N (so musl ttyname_r can match it). */
            ap_str(t, "/dev/pts/");
            ap_int(t, pty_slave_index(e));
            break;
        case FD_TYPE_PTM:
            ap_str(t, "/dev/ptmx");
            break;
        case FD_TYPE_SOCKET:
            ap_str(t, "socket:[");
            ap_u32(t, e->sock ? e->sock->inode : 0u);
            ap_ch(t, ']');
            break;
        case FD_TYPE_FILE:
        case FD_TYPE_DIR:
            if (e->node && e->node->type == KRFFS_RESERVED_NODE) {
                char path[256]; /* the same path capacity the syscall layer uses */
                /* Reconstruct the path only from a node still live and
                 * reachable; a stale fd (its node unlinked or relocated) would
                 * otherwise parse freed free-list bytes into the link target.
                 */
                if (krffs_node_path(e->node, path, sizeof(path)) == 0 &&
                    krffs_resolve(path) == e->node) {
                    ap_str(t, path); /* the full absolute path */
                } else {
                    ap_ch(t, '/'); /* unresolvable or stale */
                }
            } else if (e->ofd && e->ofd->vmnt && e->ofd->dirpath[0]) {
                ap_str(t, e->ofd->dirpath); /* a node of another type, by the path it was opened by */
            } else {
                ap_ch(t, '/'); /* the root directory, or a stale fd */
            }
            break;
        case FD_TYPE_SYNTH_DIR: {
            /* a /dev or /proc directory fd: its reconstructed path */
            struct synth_dir *sd = (struct synth_dir *)e->priv;
            char path[64];
            if (sd) {
                synth_dir_path(sd->kind, sd->pid, path, (int)sizeof(path));
                ap_str(t, path);
            }
            break;
        }
        case FD_TYPE_PROC: {
            /* a /proc snapshot fd: its origin path (a pid file or the global
             * boot command line) */
            struct procfs_file *pf = (struct procfs_file *)e->priv;
            if (pf) {
                if (pf->origin_file == SYNTH_PROC_BOOTLINE) {
                    ap_str(t, "/proc/cmdline");
                    break;
                }
                if (pf->origin_file == SYNTH_PROC_DISKINFO) {
                    ap_str(t, "/proc/disk");
                    break;
                }
                if (pf->origin_file == SYNTH_PROC_NETDEV) {
                    ap_str(t, "/proc/net/dev");
                    break;
                }
                if (pf->origin_file == SYNTH_PROC_NETROUTE) {
                    ap_str(t, "/proc/net/route");
                    break;
                }
                if (pf->origin_file == SYNTH_PROC_NETARP) {
                    ap_str(t, "/proc/net/arp");
                    break;
                }
                if (pf->origin_file == SYNTH_PROC_NETTCP) {
                    ap_str(t, "/proc/net/tcp");
                    break;
                }
                ap_str(t, "/proc/");
                ap_int(t, pf->origin_pid);
                ap_ch(t, '/');
                ap_str(t, pf->origin_file == SYNTH_PROC_CMDLINE ? "cmdline" : "stat");
            }
            break;
        }
        default:
            ap_str(t, "anon_inode:[device]"); /* fb/mouse: best effort */
            break;
    }
    return t->len;
}

/**
 * Return the pid of the n-th live process in process-table order, or -1.
 *
 * Lets a /proc directory read walk the table by index without exposing slots.
 */
int procfs_nth_pid(int n)
{
    int i;
    int seen = 0;

    if (n < 0) {
        return -1;
    }
    for (i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state != PROC_UNUSED) {
            if (seen == n) {
                return (int)proc_table[i].pid;
            }
            seen++;
        }
    }
    return -1;
}

uint32_t procfs_ino_dir(void)
{
    return PROCFS_INO_DIR;
}

uint32_t procfs_ino_cmdline(void)
{
    return PROCFS_INO_CMDLINE;
}

uint32_t procfs_ino_disk(void)
{
    return PROCFS_INO_DISK;
}

uint32_t procfs_ino_netdir(void)
{
    return PROCFS_INO_NETDIR;
}

uint32_t procfs_ino_netdev(void)
{
    return PROCFS_INO_NETDEV;
}

uint32_t procfs_ino_uptime(void)
{
    return PROCFS_INO_UPTIME;
}

uint32_t procfs_ino_statsys(void)
{
    return PROCFS_INO_STATSYS;
}

uint32_t procfs_ino_meminfo(void)
{
    return PROCFS_INO_MEMINFO;
}

uint32_t procfs_ino_mounts(void)
{
    return PROCFS_INO_MOUNTS;
}

uint32_t procfs_ino_usb(void)
{
    return PROCFS_INO_USB;
}

uint32_t procfs_ino_filesystems(void)
{
    return PROCFS_INO_FILESYSTEMS;
}

uint32_t procfs_ino_interrupts(void)
{
    return PROCFS_INO_INTERRUPTS;
}


uint32_t procfs_ino_netroute(void)
{
    return PROCFS_INO_NETROUTE;
}

uint32_t procfs_ino_netarp(void)
{
    return PROCFS_INO_NETARP;
}

uint32_t procfs_ino_nettcp(void)
{
    return PROCFS_INO_NETTCP;
}

/**
 * The inode of an open /proc snapshot descriptor, from its origin, so fstat
 * reports the same identity as the stat of the path it was opened by.
 * Returns 0 if `e` is not a /proc snapshot fd.
 */
uint32_t procfs_fd_ino(struct fd_entry *e)
{
    struct procfs_file *pf;

    if (!e || e->type != FD_TYPE_PROC || !e->priv) {
        return 0;
    }
    pf = (struct procfs_file *)e->priv;
    if (pf->origin_file == SYNTH_PROC_BOOTLINE) {
        return PROCFS_INO_CMDLINE;
    }
    if (pf->origin_file == SYNTH_PROC_DISKINFO) {
        return PROCFS_INO_DISK;
    }
    if (pf->origin_file == SYNTH_PROC_NETDEV) {
        return PROCFS_INO_NETDEV;
    }
    if (pf->origin_file == SYNTH_PROC_NETROUTE) {
        return PROCFS_INO_NETROUTE;
    }
    if (pf->origin_file == SYNTH_PROC_NETARP) {
        return PROCFS_INO_NETARP;
    }
    if (pf->origin_file == SYNTH_PROC_NETTCP) {
        return PROCFS_INO_NETTCP;
    }
    if (pf->origin_file == SYNTH_PROC_UPTIME) {
        return PROCFS_INO_UPTIME;
    }
    if (pf->origin_file == SYNTH_PROC_STATSYS) {
        return PROCFS_INO_STATSYS;
    }
    if (pf->origin_file == SYNTH_PROC_MEMINFO) {
        return PROCFS_INO_MEMINFO;
    }
    if (pf->origin_file == SYNTH_PROC_MOUNTS) {
        return PROCFS_INO_MOUNTS;
    }
    if (pf->origin_file == SYNTH_PROC_USB) {
        return PROCFS_INO_USB;
    }
    if (pf->origin_file == SYNTH_PROC_FILESYSTEMS) {
        return PROCFS_INO_FILESYSTEMS;
    }
    if (pf->origin_file == SYNTH_PROC_INTERRUPTS) {
        return PROCFS_INO_INTERRUPTS;
    }
    return procfs_ino_pid(pf->origin_pid);
}

uint32_t procfs_ino_pid(int pid)
{
    return PROCFS_INO_PID_BASE + (uint32_t)(pid & 0xffff);
}


/**
 * Render /proc/net/dev.
 *
 * The two header lines and one line per interface, in the order and with the
 * sixteen counters Linux prints, because `ifconfig` and everything like it
 * parses this file by counting fields. This machine has one interface and
 * lists it only when a device was found: an interface that is not there is not
 * one a program should be told about.
 *
 * Seven of the sixteen are kept: bytes and packets each way, receive errors,
 * receive drops, and transmit drops. The other nine are zero because this
 * machine has nothing to put in them. The three refusal columns were zero too
 * until the drivers could report a refusal: a transmit verdict for the last
 * of them, and two receive-note calls for the first two. Every driver here
 * refuses a frame under some condition, so zero was never the truth.
 * Collisions and carrier remain zero: no driver reports either.
 */
static int procfs_gen_netdev(struct procfs_text *t)
{
    const struct eth_counts *c = ethernet_counts();

    ap_str(t,
           "Inter-|   Receive                                                |"
           "  Transmit\n"
           " face |bytes    packets errs drop fifo frame compressed multicast|"
           "bytes    packets errs drop fifo colls carrier compressed\n");
    if (!netdev) {
        return t->len;
    }
    ap_str(t, "  ");
    ap_str(t, NETDEV_IFNAME);
    ap_str(t, ": ");
    ap_u32(t, c->rx_bytes);
    ap_ch(t, ' ');
    ap_u32(t, c->rx_packets);
    ap_ch(t, ' ');
    ap_u32(t, c->rx_errors);
    ap_ch(t, ' ');
    ap_u32(t, c->rx_dropped);
    /* fifo, frame, compressed, multicast */
    ap_str(t, " 0 0 0 0 ");
    ap_u32(t, c->tx_bytes);
    ap_ch(t, ' ');
    ap_u32(t, c->tx_packets);
    /* errs */
    ap_str(t, " 0 ");
    ap_u32(t, c->tx_dropped);
    /* fifo, colls, carrier, compressed */
    ap_str(t, " 0 0 0 0\n");
    return t->len;
}

/*
 * The machine's own files.
 *
 * Every number below is one the kernel already holds. Where Linux prints a
 * field this system has no equivalent for, the field is zero and the applet
 * that needs it is left not working rather than shown a plausible invention:
 * `top` reads several CPU categories and a load average, and neither is here.
 */

/** Append a count of ticks as seconds with two decimals, the /proc/uptime
 * shape. Ticks are 100 Hz, so the fraction is the remainder outright. */
static void ap_ticks_as_seconds(struct procfs_text *t, uint32_t ticks)
{
    ap_u32(t, ticks / 100u);
    ap_ch(t, '.');
    ap_ch(t, (char)('0' + (ticks / 10u) % 10u));
    ap_ch(t, (char)('0' + ticks % 10u));
}

/**
 * Render /proc/uptime: seconds since boot, and seconds the processor idled.
 *
 * Both are counted from delivered ticks rather than timed. Interrupts are off
 * in several paths here and a tick can be lost, so what these say is what the
 * timer actually delivered, which is the honest number and the one that makes
 * the two add up to each other. Both counters are read once, into locals: a
 * tick landing between two reads of the same file would report a total and an
 * idle from different moments, which is what the snapshot exists to prevent.
 */
static int procfs_gen_uptime(struct procfs_text *t)
{
    uint32_t idle = sched_idle_ticks();
    uint32_t busy = sched_busy_ticks();

    ap_ticks_as_seconds(t, idle + busy);
    ap_ch(t, ' ');
    ap_ticks_as_seconds(t, idle);
    ap_ch(t, '\n');
    return t->len;
}

/**
 * Render /proc/stat: the tick counters, the boot epoch, and the process count.
 *
 * The `cpu` line carries user, nice, system and idle in that order. This
 * kernel does not separate user from system time, so what it knows is one
 * busy figure and one idle figure: busy goes in the system column, since that
 * is where a kernel's own time belongs, and user and nice are zero rather than
 * a share invented to look plausible.
 *
 * `processes` is what Linux means by it, the count of processes CREATED since
 * boot and not the count alive now, which is a different number and the one an
 * earlier draft printed. `procs_running` and `procs_blocked` are counted from
 * the table rather than fixed, since a machine with two runnable processes
 * reporting one is wrong the moment anything looks.
 */
static int procfs_gen_statsys(struct procfs_text *t)
{
    uint32_t busy = sched_busy_ticks();
    uint32_t idle = sched_idle_ticks();
    int i;
    uint32_t running = 0;
    uint32_t blocked = 0;

    ap_str(t, "cpu  0 0 ");
    ap_u32(t, busy);
    ap_ch(t, ' ');
    ap_u32(t, idle);
    ap_str(t, " 0 0 0 0 0 0\n");
    ap_str(t, "cpu0 0 0 ");
    ap_u32(t, busy);
    ap_ch(t, ' ');
    ap_u32(t, idle);
    ap_str(t, " 0 0 0 0 0 0\n");
    ap_str(t, "intr ");
    ap_u32(t, isr_interrupt_total());
    ap_ch(t, '\n');
    ap_str(t, "ctxt 0\n");
    ap_str(t, "btime ");
    ap_u32(t, (uint32_t)rtc_boot_epoch());
    ap_ch(t, '\n');
    ap_str(t, "processes ");
    ap_u32(t, process_forks_since_boot());
    ap_ch(t, '\n');
    for (i = 0; i < MAX_PROCS; i++) {
        if (proc_table[i].state == PROC_RUNNING || proc_table[i].state == PROC_READY) {
            running++;
        } else if (proc_table[i].state == PROC_BLOCKED) {
            blocked++;
        }
    }
    ap_str(t, "procs_running ");
    ap_u32(t, running);
    ap_str(t, "\nprocs_blocked ");
    ap_u32(t, blocked);
    ap_ch(t, '\n');
    return t->len;
}

/** Append a frame count as kibibytes, which is what /proc/meminfo prints. */
static void ap_frames_as_kb(struct procfs_text *t, uint32_t frames)
{
    ap_u32(t, frames * (PAGE_SIZE / 1024u));
    ap_str(t, " kB\n");
}

/**
 * Render /proc/meminfo from the physical frame pool.
 *
 * The pool is the whole of what this machine manages, so MemTotal and MemFree
 * are the two counts outright. MemAvailable is the free count as well and not
 * an estimate: there is no reclaimable cache to add to it, since the page cache
 * here is a fixed reservation the pool never gets back.
 */
static int procfs_gen_meminfo(struct procfs_text *t)
{
    uint32_t total = frame_total_count();
    uint32_t free_frames = frame_free_count();

    ap_str(t, "MemTotal:       ");
    ap_frames_as_kb(t, total);
    ap_str(t, "MemFree:        ");
    ap_frames_as_kb(t, free_frames);
    ap_str(t, "MemAvailable:   ");
    ap_frames_as_kb(t, free_frames);
    ap_str(t, "Buffers:               0 kB\n");
    ap_str(t, "Cached:                0 kB\n");
    ap_str(t, "SwapTotal:             0 kB\n");
    ap_str(t, "SwapFree:              0 kB\n");
    return t->len;
}

/**
 * Render /proc/mounts in the six-field fstab shape `df` and `mount` parse.
 *
 * The root's source and options are what the mount recorded when it succeeded,
 * because the three branches that can mount this file system differ in ways
 * nothing afterwards reports: a fixed line would call a RAM root a disk one on
 * two boots out of three. The two synthetic mounts are named as well, since
 * they are real mounts to every program that walks this file.
 */
/**
 * Render /proc/usb.
 *
 * One line per device the USB stack has configured: the root port it is on,
 * its speed, what it says it is, and the class it was driven as. Then one
 * line per stick that is a disk, naming its node, what the drive says it
 * is, its size and whether it is still there, and, on a machine that has had
 * a stick, a last line with how many names are free and how many sticks have
 * been given up since boot. A machine with no controller, or one booted
 * without USB, has this file and it is empty, which is a different answer
 * from the file not being there and is the one a program that opens it can
 * act on.
 */
static int procfs_gen_usb(struct procfs_text *t)
{
    int i;
    char line[96];

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        int n = usb_core_describe(i, line, (int)sizeof(line));
        int j;

        for (j = 0; j < n; j++) {
            ap_ch(t, line[j]);
        }
    }
    for (i = 0; i < USBDISK_MAX; i++) {
        int n = usbdisk_describe(i, line, (int)sizeof(line));
        int j;

        for (j = 0; j < n; j++) {
            ap_ch(t, line[j]);
        }
    }
    /* The counting line is written only by a machine that has had a stick,
     * so a boot with no controller, or one with a controller and nothing
     * plugged into it, has this file empty as it always had. A reader that
     * finds no such line has every name free and nothing given up, which is
     * what a machine that never had a stick has. */
    if (usbdisk_free() != USBDISK_MAX || usbdisk_teardowns() != 0) {
        ap_str(t, "disks ");
        ap_u32(t, (uint32_t)usbdisk_free());
        ap_str(t, " free ");
        ap_u32(t, usbdisk_teardowns());
        ap_str(t, " reclaimed\n");
    }
    return t->over ? -1 : t->len;
}

static int procfs_gen_mounts(struct procfs_text *t)
{
    int i;

    if (vfs_root_mount()->type == &vfs_krffs_type) {
        ap_str(t, krffs_mount_source());
        ap_str(t, " / krffs ");
        ap_str(t, krffs_mount_options());
    } else {
        const struct vfs_mount *root = vfs_root_mount();

        ap_str(t, root->source);
        ap_str(t, " / ");
        ap_str(t, root->type->name);
        ap_str(t, (root->flags & VFS_MS_RDONLY) ? " ro,relatime" : " rw,relatime");
    }
    ap_str(t, " 0 0\n");
    ap_str(t, "devfs /dev devfs rw,relatime 0 0\n");
    ap_str(t, "proc /proc proc rw,relatime 0 0\n");
    /* Then every mount mount(2) placed, in table order: its source as the
     * caller spelled it, its target, its type, and the flags as words. */
    for (i = 3; i < VFS_MOUNTS; i++) {
        struct vfs_mount *m = vfs_mount_at(i);

        if (!m || m->state != VFS_ACTIVE) {
            continue;
        }
        ap_str(t, m->source);
        ap_ch(t, ' ');
        ap_str(t, m->covered_path);
        ap_ch(t, ' ');
        ap_str(t, m->type->name);
        ap_ch(t, ' ');
        ap_str(t, (m->flags & VFS_MS_RDONLY) ? "ro" : "rw");
        if (m->flags & VFS_MS_NOSUID) {
            ap_str(t, ",nosuid");
        }
        if (m->flags & VFS_MS_NODEV) {
            ap_str(t, ",nodev");
        }
        if (m->flags & VFS_MS_NOEXEC) {
            ap_str(t, ",noexec");
        }
        if (m->flags & VFS_MS_NOATIME) {
            ap_str(t, ",noatime");
        }
        if (m->flags & VFS_MS_NODIRATIME) {
            ap_str(t, ",nodiratime");
        }
        if (m->flags & VFS_MS_RELATIME) {
            ap_str(t, ",relatime");
        }
        ap_str(t, " 0 0\n");
    }
    return t->over ? -1 : t->len;
}

/* /proc/filesystems: `nodev` for the two synthetic namespaces, then every
 * type mount(2) accepts. KRFFS is not listed, because the root is not
 * mounted through mount(2) and cannot be while it lives in the window. */
/* /proc/interrupts: one line per vector that has taken an interrupt, the
 * vector, its count and where it comes from, then the counts the APIC's
 * spurious vector and the timer's fallback pump keep. */
static int procfs_gen_interrupts(struct procfs_text *t)
{
    int v;

    for (v = 32; v < 64; v++) {
        uint32_t n = isr_vector_count(v);
        int slot;
        int named = 0;

        if (n == 0) {
            continue;
        }
        ap_u32(t, (uint32_t)v);
        ap_str(t, ":\t");
        ap_u32(t, n);
        ap_str(t, v < 48 ? "\tPIC\t" : "\tMSI\t");
        for (slot = 0; slot < ISR_MAX_VECTOR_HANDLERS; slot++) {
            const char *name = isr_vector_name(v, slot);

            if (name == 0) {
                continue;
            }
            if (named++) {
                ap_str(t, ", "); /* two devices on one line are two names */
            }
            ap_str(t, name);
        }
        if (!named) {
            ap_str(t, "none");
        }
        ap_ch(t, '\n');
    }
    ap_str(t, "SPU:\t");
    ap_u32(t, lapic_spurious_count());
    ap_str(t, "\tspurious\n");
    /* What the timer's pump found that a message should have brought. It is
     * zero on a machine whose messages arrive, so a number here is a count of
     * lost edges and not of work done. */
    ap_str(t, "FBK:\t");
    ap_u32(t, usb_fallback_count());
    ap_str(t, "\tusb fallback\n");
    return t->over ? -1 : t->len;
}

static int procfs_gen_filesystems(struct procfs_text *t)
{
    int i;

    ap_str(t, "nodev\tdevfs\n");
    ap_str(t, "nodev\tproc\n");
    for (i = 0; i < 4; i++) {
        const struct vfs_type *ty = vfs_type_at(i);

        if (ty) {
            ap_ch(t, '\t');
            ap_str(t, ty->name);
            ap_ch(t, '\n');
        }
    }
    return t->over ? -1 : t->len;
}

/*
 * The network files, in the shapes the programs that read them parse.
 *
 * Each format was taken from the parser rather than from a description of it:
 * `route` reads eleven whitespace-separated fields with `fscanf` and skips one
 * header line; `arp` reads six with `sscanf` and skips one header line; and
 * `netstat` reads nine and rejects a line that yields fewer. A file that is
 * nearly right fails in the reader, not here, so the numbers below are what
 * those three call for and not what looks tidy. The columns Linux prints after
 * the ninth are kept for fidelity: no parser here needs them, and a program
 * that counts fields on a line would.
 */

/** Append `v` as `width` uppercase hex digits, most significant first. */
static void ap_hex(struct procfs_text *t, uint32_t v, int width)
{
    static const char digits[] = "0123456789ABCDEF";
    int i;

    for (i = width - 1; i >= 0; i--) {
        ap_ch(t, digits[(v >> (i * 4)) & 0xFu]);
    }
}

/**
 * An IPv4 address as /proc writes it: the four bytes in memory order, which on
 * this machine reads as the address reversed. Both readers undo it the same
 * way, so writing it the human way would put every address backwards.
 */
static void ap_ip_hex(struct procfs_text *t, uint32_t ip)
{
    ap_hex(t, ((ip & 0xFFu) << 24) | ((ip & 0xFF00u) << 8) | ((ip >> 8) & 0xFF00u) |
                  ((ip >> 24) & 0xFFu),
           8);
}

/** An address as dotted decimal, which /proc/net/arp uses instead of hex. */
static void ap_ip_dotted(struct procfs_text *t, uint32_t ip)
{
    ap_u32(t, (ip >> 24) & 0xFFu);
    ap_ch(t, '.');
    ap_u32(t, (ip >> 16) & 0xFFu);
    ap_ch(t, '.');
    ap_u32(t, (ip >> 8) & 0xFFu);
    ap_ch(t, '.');
    ap_u32(t, ip & 0xFFu);
}

/**
 * Render /proc/net/route: the on-link subnet, and the default route if there
 * is one.
 *
 * This stack knows exactly one subnet and at most one gateway, so the table has
 * one or two rows and both come out of `netconf`. The flags are the ones
 * `route` prints from: 1 is up, and 2 more says the destination is a gateway.
 */
static int procfs_gen_netroute(struct procfs_text *t)
{
    ap_str(t, PROCFS_NETROUTE_HEADER);
    if (!netdev || net_ip_addr == 0) {
        return t->len;
    }
    if (net_ip_gateway != 0) {
        ap_str(t, NETDEV_IFNAME);
        ap_str(t, "\t00000000\t");
        ap_ip_hex(t, net_ip_gateway);
        ap_str(t, "\t0003\t0\t0\t0\t00000000\t0\t0\t0\n");
    }
    ap_str(t, NETDEV_IFNAME);
    ap_ch(t, '\t');
    ap_ip_hex(t, net_ip_addr & net_ip_mask);
    ap_str(t, "\t00000000\t0001\t0\t0\t0\t");
    ap_ip_hex(t, net_ip_mask);
    ap_str(t, "\t0\t0\t0\n");
    return t->len;
}

/**
 * Render /proc/net/arp: what the cache holds, one line each.
 *
 * The hardware type is 1, Ethernet, and the flag is 2, which is what `arp`
 * prints as a complete entry; an entry this cache holds at all is complete,
 * since a pending one is not stored.
 */
static int procfs_gen_netarp(struct procfs_text *t)
{
    static struct arp_row rows[ARP_CACHE_SIZE];
    int n, i, b;

    ap_str(t, PROCFS_NETARP_HEADER);
    n = arp_snapshot(rows, ARP_CACHE_SIZE);
    for (i = 0; i < n; i++) {
        ap_ip_dotted(t, rows[i].ip);
        ap_str(t, "        0x1         0x2         ");
        for (b = 0; b < 6; b++) {
            if (b) {
                ap_ch(t, ':');
            }
            ap_hex(t, rows[i].mac[b], 2);
        }
        ap_str(t, "     *        ");
        ap_str(t, NETDEV_IFNAME);
        ap_ch(t, '\n');
    }
    return t->len;
}

/**
 * Render /proc/net/tcp: the connection pool, in the columns netstat parses.
 *
 * The state numbers are Linux's, which this stack's own enumeration does not
 * match, so they are mapped rather than cast: a reader prints the name for the
 * number it is given, and an unmapped value would name the wrong state.
 * `uid` and `inode` come from the block rather than from its socket, which is
 * cleared while a connection is still closing.
 */
static int procfs_gen_nettcp(struct procfs_text *t)
{
    static struct tcp_row rows[MAX_TCP_CONNS];
    int n, i;

    ap_str(t, PROCFS_NETTCP_HEADER);
    n = tcp_snapshot(rows, MAX_TCP_CONNS);
    for (i = 0; i < n; i++) {
        ap_str(t, "   ");
        ap_u32(t, (uint32_t)i);
        ap_str(t, ": ");
        ap_ip_hex(t, rows[i].local_ip);
        ap_ch(t, ':');
        ap_hex(t, rows[i].local_port, 4);
        ap_ch(t, ' ');
        ap_ip_hex(t, rows[i].remote_ip);
        ap_ch(t, ':');
        ap_hex(t, rows[i].remote_port, 4);
        ap_ch(t, ' ');
        ap_hex(t, (uint32_t)tcp_state_to_linux(rows[i].state), 2);
        ap_ch(t, ' ');
        ap_hex(t, rows[i].txq, 8);
        ap_ch(t, ':');
        ap_hex(t, rows[i].rxq, 8);
        ap_str(t, " 00:00000000 00000000 ");
        ap_u32(t, rows[i].uid);
        ap_str(t, "        0 ");
        ap_u32(t, rows[i].inode);
        ap_str(t, " 1 0000000000000000 100 0 0 10 0\n");
    }
    return t->len;
}

/* the /proc file backing: a snapshot served through file_ops */

static struct procfs_file proc_files[8];

static int32_t procfs_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    uint32_t off, avail, n;

    if (!f) {
        return 0;
    }
    off = e->ofd->offset;
    if (off >= (uint32_t)f->len) {
        return 0; /* end of the snapshot */
    }
    avail = (uint32_t)f->len - off;
    n = count < avail ? count : avail;
    if (copy_to_user(buf, f->data + off, n) != 0) {
        return -EFAULT;
    }
    e->ofd->offset = off + n;
    return (int32_t)n;
}

static int32_t procfs_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    (void)e;
    (void)buf;
    (void)count;
    return -EBADF; /* these /proc files are read-only */
}

static short procfs_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    (void)e;
    if (wq) {
        *wq = NULL;
    }
    return (short)(events & POLLIN); /* always has data (or EOF) ready */
}

static int32_t procfs_ioctl(fd_entry_t *e, uint32_t request, uint32_t arg)
{
    (void)e;
    (void)request;
    (void)arg;
    return -ENOTTY;
}

static void procfs_close(fd_entry_t *e)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    if (f) {
        f->used = 0; /* return the snapshot slot to the pool */
    }
}

static const struct file_ops procfs_file_ops = {procfs_read, procfs_write, procfs_poll,
                                                procfs_ioctl, procfs_close};

/**
 * Open a /proc/<pid>/{stat,cmdline} file, returning a descriptor or -errno.
 *
 * The text is generated and snapshotted into a pooled slot at open, then served
 * read-only through procfs_file_ops; the slot returns to the pool on the last
 * close. These files are read-only regular files, so the open-flag checks are
 * ordered to report the conventional errno: O_CREAT|O_EXCL on an existing file
 * is -EEXIST first, then O_DIRECTORY is -ENOTDIR, then any write or create
 * intent is -EACCES.
 *
 * The text is built once, here, against the bound its file declared. A text
 * that did not fit is refused with `-EOVERFLOW` and one line on the serial log
 * naming the file: a table read back with rows missing and nothing saying so is
 * the failure the bounds exist to prevent. `/proc/disk` is rendered from the
 * record taken at boot, so opening it issues no command to the device, and
 * `diskinfo_format` truncates to the cap it is given rather than reporting, so
 * its bound is tested against what it returned.
 */
int procfs_open(const struct vpath *vp, int flags)
{
    int pid = 0;
    struct procfs_file *f = NULL;
    struct procfs_text text;
    fd_entry_t *e;
    int slot, fd, len;

    /* /proc/cmdline and /proc/disk are global; every other proc file lives
     * under a pid. */
    if (vp->kind != VNODE_PROC_CMDLINE && vp->kind != VNODE_PROC_DISK &&
        vp->kind != VNODE_PROC_NET_DEV && vp->kind != VNODE_PROC_UPTIME &&
        vp->kind != VNODE_PROC_STAT_SYS && vp->kind != VNODE_PROC_MEMINFO &&
        vp->kind != VNODE_PROC_MOUNTS && vp->kind != VNODE_PROC_USB &&
        vp->kind != VNODE_PROC_FILESYSTEMS && vp->kind != VNODE_PROC_INTERRUPTS &&
        vp->kind != VNODE_PROC_NET_ROUTE &&
        vp->kind != VNODE_PROC_NET_ARP && vp->kind != VNODE_PROC_NET_TCP) {
        pid = procfs_resolve_pid(vp->pid);
        if (pid < 0) {
            return -ENOENT;
        }
    }
    if ((flags & 0100 /* O_CREAT */) && (flags & 0200 /* O_EXCL */)) {
        return -EEXIST;
    }
    if (flags & 0200000 /* O_DIRECTORY */) {
        return -ENOTDIR;
    }
    if ((flags & 3) != 0 || (flags & 0100 /* O_CREAT */)) {
        return -EACCES;
    }

    for (slot = 0; slot < 8; slot++) {
        if (!proc_files[slot].used) {
            f = &proc_files[slot];
            break;
        }
    }
    if (!f) {
        return -ENFILE;
    }

    if (vp->kind == VNODE_PROC_CMDLINE) {
        /* The captured boot line and a newline, the Linux shape. */
        const char *line = kcmdline_get();
        procfs_text_init(&text, f->data, PROCFS_MAX_BOOTLINE);
        ap_str(&text, line);
        ap_ch(&text, '\n');
        len = text.len;
    } else if (vp->kind == VNODE_PROC_DISK) {
        procfs_text_init(&text, f->data, PROCFS_MAX_DISK);
        len = diskinfo_format(diskinfo_get(), f->data, PROCFS_MAX_DISK);
        if (len >= PROCFS_MAX_DISK - 1) {
            text.over = 1;
        }
    } else if (vp->kind == VNODE_PROC_NET_DEV) {
        procfs_text_init(&text, f->data, PROCFS_MAX_NETDEV);
        len = procfs_gen_netdev(&text);
    } else if (vp->kind == VNODE_PROC_UPTIME) {
        procfs_text_init(&text, f->data, PROCFS_MAX_UPTIME);
        len = procfs_gen_uptime(&text);
    } else if (vp->kind == VNODE_PROC_STAT_SYS) {
        procfs_text_init(&text, f->data, PROCFS_MAX_STATSYS);
        len = procfs_gen_statsys(&text);
    } else if (vp->kind == VNODE_PROC_MEMINFO) {
        procfs_text_init(&text, f->data, PROCFS_MAX_MEMINFO);
        len = procfs_gen_meminfo(&text);
    } else if (vp->kind == VNODE_PROC_MOUNTS) {
        procfs_text_init(&text, f->data, PROCFS_MAX_MOUNTS);
        len = procfs_gen_mounts(&text);
    } else if (vp->kind == VNODE_PROC_USB) {
        procfs_text_init(&text, f->data, PROCFS_MAX_USB);
        len = procfs_gen_usb(&text);
    } else if (vp->kind == VNODE_PROC_INTERRUPTS) {
        procfs_text_init(&text, f->data, PROCFS_MAX_INTERRUPTS);
        len = procfs_gen_interrupts(&text);
    } else if (vp->kind == VNODE_PROC_FILESYSTEMS) {
        procfs_text_init(&text, f->data, PROCFS_MAX_FILESYSTEMS);
        len = procfs_gen_filesystems(&text);
    } else if (vp->kind == VNODE_PROC_NET_ROUTE) {
        procfs_text_init(&text, f->data, PROCFS_MAX_NETROUTE);
        len = procfs_gen_netroute(&text);
    } else if (vp->kind == VNODE_PROC_NET_ARP) {
        procfs_text_init(&text, f->data, PROCFS_MAX_NETARP);
        len = procfs_gen_netarp(&text);
    } else if (vp->kind == VNODE_PROC_NET_TCP) {
        procfs_text_init(&text, f->data, PROCFS_MAX_NETTCP);
        len = procfs_gen_nettcp(&text);
    } else if (vp->procfile == SYNTH_PROC_STAT) {
        procfs_text_init(&text, f->data, PROCFS_MAX_PIDSTAT);
        len = procfs_gen_stat(&text, pid);
    } else {
        procfs_text_init(&text, f->data, PROCFS_MAX_PIDCMD);
        len = procfs_gen_cmdline(&text, pid);
    }
    if (len < 0) {
        return len;
    }
    if (text.over) {
        klog(KLOG_ERR, "procfs", "%s: text exceeds its %d-byte bound, refusing",
                      procfs_kind_name(vp), text.cap);
        return -EOVERFLOW;
    }
    f->len = len;
    f->used = 1;
    f->origin_pid = pid;
    if (vp->kind == VNODE_PROC_CMDLINE) {
        f->origin_file = SYNTH_PROC_BOOTLINE;
    } else if (vp->kind == VNODE_PROC_DISK) {
        f->origin_file = SYNTH_PROC_DISKINFO;
    } else if (vp->kind == VNODE_PROC_NET_DEV) {
        f->origin_file = SYNTH_PROC_NETDEV;
    } else if (vp->kind == VNODE_PROC_UPTIME) {
        f->origin_file = SYNTH_PROC_UPTIME;
    } else if (vp->kind == VNODE_PROC_STAT_SYS) {
        f->origin_file = SYNTH_PROC_STATSYS;
    } else if (vp->kind == VNODE_PROC_MEMINFO) {
        f->origin_file = SYNTH_PROC_MEMINFO;
    } else if (vp->kind == VNODE_PROC_MOUNTS) {
        f->origin_file = SYNTH_PROC_MOUNTS;
    } else if (vp->kind == VNODE_PROC_USB) {
        f->origin_file = SYNTH_PROC_USB;
    } else if (vp->kind == VNODE_PROC_INTERRUPTS) {
        f->origin_file = SYNTH_PROC_INTERRUPTS;
    } else if (vp->kind == VNODE_PROC_FILESYSTEMS) {
        f->origin_file = SYNTH_PROC_FILESYSTEMS;
    } else if (vp->kind == VNODE_PROC_NET_ROUTE) {
        f->origin_file = SYNTH_PROC_NETROUTE;
    } else if (vp->kind == VNODE_PROC_NET_ARP) {
        f->origin_file = SYNTH_PROC_NETARP;
    } else if (vp->kind == VNODE_PROC_NET_TCP) {
        f->origin_file = SYNTH_PROC_NETTCP;
    } else {
        f->origin_file = vp->procfile;
    }

    fd = fd_alloc(&current_proc->fds);
    if (fd < 0) {
        f->used = 0;
        return -EMFILE;
    }
    e = &current_proc->fds.entries[fd];
    e->type = FD_TYPE_PROC;
    e->node = NULL;
    e->sock = NULL;
    e->ops = &procfs_file_ops;
    e->priv = f;
    e->ofd->offset = 0;
    e->flags = FD_FLAG_READ;
    if (flags & PROCFS_O_CLOEXEC) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    return fd;
}
