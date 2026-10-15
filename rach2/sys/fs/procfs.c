/*
 * The files of /proc and /sys, made from the kernel's tables.
 *
 * Most files are a snapshot: the text is made into a buffer from the heap
 * when the file is opened and served from there, so a read sees one moment,
 * and dup and fork share the snapshot until the open file description's
 * last close. The text is built by append functions that grow the buffer
 * and write numbers themselves, since the kernel has no snprintf and i686
 * has no 64-bit division here.
 */

#include "../klog.h"
#include "../irq.h"
#include "procfs.h"

#include "../latency.h"
#include "../network/ip/ip.h"
#include "../mm/filecache.h"
#include "../mm/kheap.h"
#include "../mm/pagecache.h"
#include "bcache.h"

#include "../block/loop.h"
#include "synthfs.h"
#include "fd.h"
#include "file.h"
#include "devfs.h"
#include "krffs.h"
#include "../usb/core.h"
#include "../pci.h"
#include "../audio/hda.h"
#include "../usb/usb.h"
#include "../block/usbdisk.h"
#include "../tty/pty.h"
#include "../proc/tgroup.h"
#include "../proc/seccomp.h"
#include "../proc/ns.h"
#include "../mm/mm.h"
#include "../mm/paging.h"
#include "../io/poll.h"
#include "../cmdline.h"
#include "../block/diskinfo.h"
#include "vfs.h"
#include "lock.h"
#include "tmpfs.h"
#include "cpuinfo.h"
#include "../ipc/sysv.h"
#include "pathbuf.h"
#include "../syscall.h"
#include "../network/ethernet/ethernet.h"
#include "../network/driver/netdev.h"
#include "../network/arp/arp.h"
#include "../network/tcp/tcp.h"
#include "../network/netconf.h"
#include "../network/net.h"
#include "../network/socket/socket.h"
#include "../ipc/unix.h"
#include "../proc/suspend.h"
#include "../rtc.h"
#include "../ipc/pipe.h"
#include "../proc/process.h"
#include "../mm/usercopy.h"
#include "../lib/kerrno.h"
#include "../serial.h"
#include "../rtc.h"
#include "../proc/sched.h"
#include "../mm/paging.h"
#include "../io/anon.h"
#include "../ipc/unix.h"

/* O_CLOEXEC, Linux's value on i386 and x86-64. */
#define PROCFS_O_CLOEXEC 02000000

/* Inode namespace for /proc: the directory and the global files, then a
 * per-pid base. */
#define PROCFS_INO_DIR         0x100u
#define PROCFS_INO_CMDLINE     0x101u
#define PROCFS_INO_DISK        0x102u
#define PROCFS_INO_NETDIR      0x103u
#define PROCFS_INO_NETDEV      0x104u
#define PROCFS_INO_UPTIME      0x105u
#define PROCFS_INO_STATSYS     0x106u
#define PROCFS_INO_MEMINFO     0x107u
#define PROCFS_INO_MOUNTS      0x108u
#define PROCFS_INO_AUDIO       0x111u
#define PROCFS_INO_USB         0x10Cu
#define PROCFS_INO_FILESYSTEMS 0x10Du
#define PROCFS_INO_LOOP        0x10Eu
#define PROCFS_INO_INTERRUPTS  0x10Fu
#define PROCFS_INO_CONSOLE     0x110u
#define PROCFS_INO_NETROUTE    0x109u
#define PROCFS_INO_NETARP      0x10Au
#define PROCFS_INO_NETTCP      0x10Bu
#define PROCFS_INO_CPUINFO     0x112u
#define PROCFS_INO_LOADAVG     0x113u
#define PROCFS_INO_DISKSTATS   0x114u
#define PROCFS_INO_VERSION     0x115u
#define PROCFS_INO_PARTITIONS  0x116u
#define PROCFS_INO_SYSVIPC     0x117u /* and its three files after it */
#define PROCFS_INO_NETSTAT     0x11Bu
#define PROCFS_INO_VMSTAT      0x11Cu
#define PROCFS_INO_SLABINFO    0x11Du
#define PROCFS_INO_SWAPS       0x11Eu
#define PROCFS_INO_LOCKS       0x11Fu
#define PROCFS_INO_NET         0x120u /* and the other files of /proc/net after it */

/* Inode namespace for /sys: a fixed directory by its node, with its
 * attribute files 256 apart above it; a PCI function's directory by its
 * key, doubled, with its uevent one more, and its link by its key in a
 * range of its own; a USB device's directory by its key with the top bit
 * set, its uevent with the next bit too, and its link with that bit alone;
 * and an interface's, a disk node's and a processor's directory and files
 * each in a range of its own, by the key from bit 8 and the file in the low
 * byte. The keys are below 2^30 (synthfs.h). */
#define SYSFS_INO_FIXED     0x100u
#define SYSFS_INO_PCI       0x01000000u
#define SYSFS_INO_PCI_LINK  0x02000000u
#define SYSFS_INO_NET       0x03000000u
#define SYSFS_INO_BLOCK     0x04000000u
#define SYSFS_INO_CPU       0x05000000u
#define SYSFS_INO_USB       0x80000000u
#define SYSFS_INO_FILE      0x40000000u
#define PROCFS_INO_SYS      0x140u /* and the entries of /proc/sys after it */
#define PROCFS_INO_PID_BASE 0x200u

/*
 * A text is built in the heap and grows as it is appended to, so a report
 * lists what there is: every mount, every connection, every vector. The one
 * fixed width is `/proc/disk`, which `diskinfo_format` writes into a buffer
 * of the size it is given, truncating; a text it filled to the end is
 * refused with `-EOVERFLOW` rather than served short.
 */
#define PROCFS_MAX_DISK \
    (DISKINFO_RECORD_BYTES + DISKINFO_DISKS_BYTES) /* the widest record, then the disks */

/* The header line of each network table. */
#define PROCFS_NETROUTE_HEADER                    \
    "Iface\tDestination\tGateway \tFlags\tRefCnt" \
    "\tUse\tMetric\tMask\t\tMTU\tWindow\tIRTT\n"
#define PROCFS_NETARP_HEADER                    \
    "IP address       HW type     Flags       " \
    "HW address            Mask     Device\n"
#define PROCFS_NETTCP_HEADER                        \
    "  sl  local_address rem_address   st tx_queue" \
    " rx_queue tr tm->when retrnsmt   uid  timeout" \
    " inode\n"
#define PROCFS_NETUDP_HEADER                         \
    "   sl  local_address rem_address   st tx_queue" \
    " rx_queue tr tm->when retrnsmt   uid  timeout"  \
    " inode ref pointer drops\n"
#define PROCFS_NETUNIX_HEADER "Num       RefCount Protocol Flags    Type St Inode Path\n"

/** The file's name, for the diagnostic of a text that could not be built. */
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
    if (vp->kind == VNODE_PROC_CPUINFO) {
        return "/proc/cpuinfo";
    }
    if (vp->kind == VNODE_PROC_LOADAVG) {
        return "/proc/loadavg";
    }
    if (vp->kind == VNODE_PROC_DISKSTATS) {
        return "/proc/diskstats";
    }
    if (vp->kind == VNODE_PROC_VERSION) {
        return "/proc/version";
    }
    if (vp->kind == VNODE_PROC_PARTITIONS) {
        return "/proc/partitions";
    }
    if (vp->kind == VNODE_PROC_VMSTAT) {
        return "/proc/vmstat";
    }
    if (vp->kind == VNODE_PROC_SLABINFO) {
        return "/proc/slabinfo";
    }
    if (vp->kind == VNODE_PROC_SWAPS) {
        return "/proc/swaps";
    }
    if (vp->kind == VNODE_PROC_LOCKS) {
        return "/proc/locks";
    }
    if (vp->kind == VNODE_PROC_SYS_FILE) {
        return "/proc/sys";
    }
    if (vp->kind == VNODE_PROC_MOUNTS) {
        return "/proc/mounts";
    }
    if (vp->kind == VNODE_PROC_AUDIO) {
        return "/proc/audio";
    }
    if (vp->kind == VNODE_PROC_USB) {
        return "/proc/usb";
    }
    if (vp->kind == VNODE_PROC_INTERRUPTS) {
        return "/proc/interrupts";
    }
    if (vp->kind == VNODE_PROC_CONSOLE) {
        return "/proc/console";
    }
    if (vp->kind == VNODE_PROC_FILESYSTEMS) {
        return "/proc/filesystems";
    }
    if (vp->kind == VNODE_PROC_LOOP) {
        return "/proc/loop";
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
    if (vp->kind == VNODE_PROC_NET_FILE) {
        return "/proc/net";
    }
    if (vp->kind == VNODE_PROC_SYSVIPC) {
        return "/proc/sysvipc";
    }
    if (vp->kind == VNODE_SYS_FILE) {
        return "/sys";
    }
    if (vp->procfile == SYNTH_PROC_STAT) {
        return "/proc/<pid>/stat";
    }
    return "/proc/<pid>/cmdline";
}

/*
 * The private record of a descriptor of /proc or /sys, from the heap at the
 * open and freed at the last close. Its origin is the file it was opened
 * on, for the descriptor's link, its inode and its owner: `origin_pid` is
 * the kernel's id of the process or thread for a file under /proc/<pid>,
 * the entry of a /proc/sys file, and the key of a /sys file's device,
 * interface or disk node.
 */
struct procfs_file {
    char *data; /* the text, from the heap; NULL for a file with no snapshot */
    int len;
    int origin_pid;  /* the process or thread, or the entry or key above */
    int origin_file; /* SYNTH_PROC_*, or SYNTH_SYS_UEVENT for any /sys file */
    int origin_task; /* the process whose task/<origin_pid> it came from, or 0;
                      * for a /sys file, its node << 8 | its attribute */
    struct mm *mm;   /* /proc/<pid>/mem: a hold on the address space it allowed */
};

/**
 * Drop a user's count of `mm` that a read of /proc took for its length,
 * loading the kernel's directory first when this is the last user and its
 * directory is the one in use, as `mm_drop` does.
 */
static void procfs_mm_put(struct mm *mm)
{
    if (mm->users == 1 && _curr_dir == mm->dir) {
        switch_page_directory(_kernel_dir);
    }
    mm_put(mm);
}

/*
 * One text under construction: its buffer in the heap, the buffer's size,
 * how much is written, and whether the heap ran out. The buffer doubles
 * when an append does not fit; a text the heap could not hold is refused
 * whole, never served short.
 */
struct procfs_text {
    char *buf;
    int cap;
    int len;
    int nomem;
};

#define PROCFS_TEXT_FIRST 256

/** Make `t` an empty text with no buffer. */
static void procfs_text_init(struct procfs_text *t)
{
    t->buf = NULL;
    t->cap = 0;
    t->len = 0;
    t->nomem = 0;
}

/** Grow `t`'s buffer to hold `want` bytes and a NUL: 0, or -1 with `nomem`
 * set when the heap has no room. */
static int procfs_text_room(struct procfs_text *t, int want)
{
    int cap = t->cap ? t->cap : PROCFS_TEXT_FIRST;
    char *grown;

    if (want < t->cap) {
        return 0;
    }
    while (cap <= want) {
        cap *= 2;
    }
    grown = kmalloc((size_t)cap);
    if (!grown) {
        t->nomem = 1;
        return -1;
    }
    if (t->len) {
        k_memcpy(grown, t->buf, (size_t)t->len);
    }
    kfree(t->buf);
    t->buf = grown;
    t->cap = cap;
    return 0;
}

/* The append functions. Each leaves room for a NUL after the text. */

/** Append `c`; nothing, with `nomem` set, when the heap has no room. */
static void ap_ch(struct procfs_text *t, char c)
{
    if (t->len + 1 >= t->cap && procfs_text_room(t, t->len + 1) != 0) {
        return;
    }
    t->buf[t->len] = c;
    t->len++;
}

/** Append the string `s`. */
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

/** Append `v` as unsigned decimal, a count of bytes or an address as wide
 * as a pointer. */
static void ap_ulong(struct procfs_text *t, unsigned long v)
{
    char tmp[24];
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

/** Append `v` as `ap_u32` does, right-aligned in `width` places. */
static void ap_u32_width(struct procfs_text *t, uint32_t v, int width)
{
    uint32_t w = 1, rest;

    for (rest = v; rest >= 10u; rest /= 10u) {
        w++;
    }
    while ((int)w++ < width) {
        ap_ch(t, ' ');
    }
    ap_u32(t, v);
}

/** Append `v` as signed decimal; INT_MIN is negated in unsigned arithmetic,
 * where it has a value. */
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

/* The queries over the process table. */

int procfs_resolve_pid(int pid)
{
    process_t *p;

    if (pid == SYNTH_PID_SELF) {
        return current_proc ? (int)current_proc->tgid : -1;
    }
    if (pid < 0) {
        return -1;
    }
    p = get_process(pid_named((uint32_t)pid));
    if (!p || p->state == PROC_UNUSED) {
        return -1;
    }
    return (int)p->pid;
}

int procfs_resolve_id(const struct vpath *vp)
{
    int pid = procfs_resolve_pid(vp->pid);
    process_t *p;
    process_t *t;

    if (pid < 0 || vp->tid == 0) {
        return pid;
    }
    p = get_process((uint32_t)pid);
    t = get_process(pid_named((uint32_t)vp->tid));
    if (!p || !t || t->state == PROC_UNUSED || t->tg != p->tg ||
        (t->state == PROC_ZOMBIE && t != t->tg->leader)) {
        return -1;
    }
    return (int)t->pid;
}

/** The state letter of `p` in its stat file, which the State line of its
 * status file gives with its name: a ptrace stop is 't', a stop of its
 * process 'T', and a sleep no signal ends, the disk's waits among them, 'D',
 * as in Linux. */
static char proc_state_char(const process_t *p)
{
    if (p->state == PROC_ZOMBIE) {
        return 'Z';
    }
    if (p->ptrace_status != 0) {
        return 't';
    }
    if (p->state == PROC_STOPPED) {
        return 'T';
    }
    if (p->state == PROC_BLOCKED) {
        return p->uninterruptible ? 'D' : 'S';
    }
    return 'R';
}

/** Append the device number of `p`'s controlling terminal and its
 * foreground process group, as the caller's PID namespace sees it, or 0 and
 * -1 for a process with no terminal, as /proc/<pid>/stat's fields 7 and 8. */
static void ap_tty(struct procfs_text *t, const process_t *p)
{
    uint32_t dev;
    int fg;

    if (!tty_controlling((int)p->sid, &dev, &fg)) {
        ap_str(t, " 0 -1");
        return;
    }
    ap_ch(t, ' ');
    ap_u32(t, dev);
    ap_ch(t, ' ');
    ap_int(t, fg > 0 ? (int)pid_seen((uint32_t)fg) : -1);
}

/**
 * Format /proc/<pid>/stat into `t`: its length, or -ENOENT for no such
 * process.
 *
 * The fields are separated by spaces, as ps and top parse them. comm, field
 * 2, is in parentheses, with each parenthesis in it made '_', so that a
 * parser that scans for the closing ')' finds the right one. The CPU times
 * are in the 100 Hz ticks the fields count: the whole process's, every
 * thread's, for a process's file (`whole`), and the thread's own for a
 * thread's under /proc/<pid>/task, as Linux keeps them apart; the children's
 * are the process's in both. The nice value is the one setpriority keeps.
 * The start time is the tick the process or thread was made at, which ps
 * subtracts from the uptime for its elapsed time, and the virtual size in
 * bytes and the resident pages are the address space's, as statm has them.
 * The fields end at the 24th, rss; the flags, the fault counts and
 * itrealvalue, which the kernel does not keep, are 0.
 */
static int procfs_gen_stat(struct procfs_text *t, int pid, int whole)
{
    process_t *p;
    const struct mm *mm;
    uint32_t user;
    uint32_t sys;
    int i;

    p = get_process((uint32_t)pid);
    if (!p || p->state == PROC_UNUSED) {
        return -ENOENT;
    }

    ap_int(t, (int)pid_seen((uint32_t)pid)); /* 1 pid */
    ap_str(t, " (");                         /* 2 comm */
    if (p->comm[0]) {
        for (i = 0; i < (int)sizeof(p->comm) && p->comm[i]; i++) {
            char c = p->comm[i];
            ap_ch(t, (c == '(' || c == ')') ? '_' : c);
        }
    } else {
        ap_ch(t, '?');
    }
    ap_str(t, ") ");
    ap_ch(t, proc_state_char(p)); /* 3 state */
    ap_ch(t, ' ');
    ap_int(t, (int)pid_seen(p->parent_pid)); /* 4 ppid */
    ap_ch(t, ' ');
    ap_int(t, (int)pid_seen(p->pgid)); /* 5 pgrp */
    ap_ch(t, ' ');
    ap_int(t, (int)pid_seen(p->sid)); /* 6 session */
    ap_tty(t, p);                     /* 7 tty_nr, 8 tpgid */
    /* 9-13 flags, minflt, cminflt, majflt, cmajflt; 14-17 the CPU times */
    ap_str(t, " 0 0 0 0 0 ");
    if (whole) {
        proc_cpu_time(p, &user, &sys);
    } else {
        user = p->utime;
        sys = p->stime;
    }
    ap_u32(t, user);
    ap_ch(t, ' ');
    ap_u32(t, sys);
    ap_ch(t, ' ');
    ap_u32(t, p->tg->cutime);
    ap_ch(t, ' ');
    ap_u32(t, p->tg->cstime);
    /* 18 priority, Linux's nice + 20 without real-time policy; 19 nice */
    ap_ch(t, ' ');
    ap_int(t, 20 + (int)p->nice);
    ap_ch(t, ' ');
    ap_int(t, (int)p->nice);
    ap_ch(t, ' ');
    ap_u32(t, p->tg->live); /* 20 num_threads */
    ap_str(t, " 0 ");       /* 21 itrealvalue */
    ap_u32(t, p->start_ticks);
    ap_ch(t, ' ');
    mm = p->mm;
    ap_ulong(t, mm ? vm_total(&mm->vm) : 0);
    ap_ch(t, ' ');
    ap_ulong(t, mm ? mm_resident(mm) : 0);
    ap_ch(t, '\n');
    if (!t->nomem) {
        t->buf[t->len] = '\0';
    }
    return t->len;
}

/** Whether the /proc/<pid> file `file` is one of the user namespace's. */
static int procfs_is_idmap(int file)
{
    return file == SYNTH_PROC_UID_MAP || file == SYNTH_PROC_GID_MAP || file == SYNTH_PROC_SETGROUPS;
}

/** The process `pid` names for one of its /proc files, or NULL. */
static process_t *procfs_proc(int pid)
{
    process_t *p = get_process((uint32_t)pid);

    return (p && p->state != PROC_UNUSED) ? p : NULL;
}

/**
 * The count is Linux's: the directory's entry in its parent, its own `.`,
 * and the `..` of each live thread's directory. Chromium's zygote takes a
 * count of 3 as the proof that it runs one thread, and stops when it is less.
 */
uint32_t procfs_task_links(int pid)
{
    process_t *p = procfs_proc(pid);

    return 2u + (p ? p->tg->live : 0u);
}

/** Whether the caller may read what /proc shows of `p`'s memory, its
 * environment and its auxiliary vector: a thread of `p`'s own process, a
 * privileged caller, or one whose real user ID is `p`'s when `p` is not
 * privileged, as Linux's ptrace check asks at its simplest. */
static int procfs_may_read_mem(const process_t *p)
{
    return current_proc && (p->tg == current_proc->tg || proc_privileged(current_proc) ||
                            (current_proc->ruid == p->ruid && !proc_privileged(p)));
}

int procfs_may_read(int pid)
{
    process_t *p = procfs_proc(pid);

    return p && procfs_may_read_mem(p);
}

int procfs_exe_file(int pid, struct vfs_ref *out)
{
    process_t *p = procfs_proc(pid);

    if (!p || !p->mm || !p->mm->exe_file) {
        return -ENOENT;
    }
    if (!procfs_may_read_mem(p)) {
        return -EACCES;
    }
    out->mnt = p->mm->exe_file->vmnt;
    out->node = p->mm->exe_file->vnode;
    return 0;
}

/** The node of the program's file `ofd` describes when that is a file of
 * the KRFFS root, whose node has a path of its own; NULL otherwise. */
static krffs_node_t *exe_root_node(const struct open_file *ofd)
{
    if (!ofd || ofd->vmnt != vfs_root_mount() || ofd->vmnt->type != &vfs_krffs_type ||
        ofd->vnode == krffs_fs->node) {
        return NULL;
    }
    return ofd->vnode;
}

/** The present path of `node`, a file of the KRFFS root that has a name, in
 * a buffer from the heap the caller frees; NULL when no path resolves back
 * to it, or the heap has no room. */
static char *exe_present_path(krffs_node_t *node)
{
    char *path = kmalloc(VFS_PATH_MAX);

    if (path && (krffs_node_path(node, path, VFS_PATH_MAX) != 0 || krffs_resolve(path) != node)) {
        kfree(path);
        path = NULL;
    }
    return path;
}

/** Copy the string `s` into `buf` from `n` on, as far as `cap` allows; the
 * length after. */
static int exe_put(char *buf, int cap, int n, const char *s)
{
    while (*s && n < cap) {
        buf[n++] = *s++;
    }
    return n;
}

/**
 * The text of process `pid`'s /proc/<pid>/exe into `buf` of `cap` bytes, as
 * `procfs_readlink_exe` says, with " (deleted)" after the path of a file that
 * lost its name only when `mark` is set. The record of the address space is
 * held while its path is read, since reading the node may sleep in the page
 * cache while the process ends.
 */
static int exe_text(int pid, char *buf, int cap, int mark)
{
    process_t *p = procfs_proc(pid);
    krffs_node_t *node;
    struct mm *mm;
    char *now = NULL;
    int deleted;
    int n;

    if (!p || !p->mm || (!p->mm->exe && !p->mm->exe_file)) {
        return -ENOENT;
    }
    if (!procfs_may_read_mem(p)) {
        return -EACCES;
    }
    mm = p->mm;
    mm_hold(mm);
    node = exe_root_node(mm->exe_file);
    deleted = node && node->name[0] == '\0';
    if (node && !deleted && cap > 0) {
        now = exe_present_path(node);
    }
    n = exe_put(buf, cap, 0, now ? now : mm->exe ? mm->exe : "");
    if (deleted && mark) {
        n = exe_put(buf, cap, n, " (deleted)");
    }
    kfree(now);
    mm_unhold(mm);
    return n;
}

int procfs_readlink_exe(int pid, char *buf, int cap)
{
    return exe_text(pid, buf, cap, 1);
}

int procfs_exe_name(int pid, char *buf, int cap)
{
    return exe_text(pid, buf, cap, 0);
}

int procfs_readlink_ns(int pid, int kind, char *buf, int cap)
{
    process_t *p = procfs_proc(pid);
    struct ns_common *c;
    char text[24];
    int n;

    if (!p) {
        return -ENOENT;
    }
    if (!procfs_may_read_mem(p)) {
        return -EACCES;
    }
    c = ns_of(p, kind);
    if (!c) {
        return -ENOENT;
    }
    ns_link_name(c, text);
    ns_put(c);
    n = (int)k_strlen(text);
    n = n < cap ? n : cap;
    k_memcpy(buf, text, (uint32_t)(n > 0 ? n : 0));
    return n;
}

uint32_t procfs_ns_ino(int pid, int kind)
{
    process_t *p = procfs_proc(pid);
    struct ns_common *c = p ? ns_of(p, kind) : NULL;
    uint32_t inum = c ? c->inum : 0;

    ns_put(c);
    return inum;
}

int procfs_open_ns(int pid, int kind, int flags)
{
    process_t *p = procfs_proc(pid);
    struct ns_common *c;
    int fd;

    if (!p) {
        return -ENOENT;
    }
    if (!procfs_may_read_mem(p)) {
        return -EACCES;
    }
    if ((flags & 3) != 0) {
        return -EACCES;
    }
    c = ns_of(p, kind);
    if (!c) {
        return -ENOENT;
    }
    fd = nsfs_open(c, (uint32_t)flags);
    ns_put(c);
    return fd;
}

int procfs_readlink_dir(int pid, int root, char *buf, int cap)
{
    process_t *p = procfs_proc(pid);

    if (!p || !p->fs) {
        return -ENOENT;
    }
    if (!procfs_may_read_mem(p)) {
        return -EACCES;
    }
    if (root && p->fs->root_mnt) {
        int rc = vfs_node_path(p->fs->root_mnt, p->fs->root_node, buf, (uint32_t)cap);

        return rc != 0 ? rc : (int)k_strlen(buf);
    }
    return exe_put(buf, cap, 0, root ? "/" : proc_cwd(p));
}

/**
 * Append the bytes of `p`'s memory from `start` to `end`, read through its
 * address space, at most 128 KiB: the strings of its arguments or its
 * environment, as its initial stack has them.
 *
 * A read may sleep, bringing in a page of a file the process maps, and the
 * process may end and be reaped meanwhile, so the address space is held for
 * the length of the reads, and `p`, whose record the reap frees, is read
 * only before the first; the caller took `start` and `end` from it before.
 */
static void ap_memory(struct procfs_text *t, const process_t *p, uintptr_t start, uintptr_t end)
{
    struct mm *mm = p->mm;
    char chunk[128];

    if (end > start + 131072u) {
        end = start + 131072u;
    }
    mm_get(mm);
    while (start < end) {
        uint32_t n =
            end - start < sizeof(chunk) ? (uint32_t)(end - start) : (uint32_t)sizeof(chunk);
        uint32_t i;

        if (paging_access(mm, start, chunk, n, 0) != 0) {
            break;
        }
        for (i = 0; i < n; i++) {
            ap_ch(t, chunk[i]);
        }
        start += n;
    }
    procfs_mm_put(mm);
}

/**
 * Format /proc/<pid>/cmdline into `t`, returning its length or -ENOENT.
 *
 * The arguments as the program's initial stack has them, each ended by a
 * NUL, read through its address space: a program that rewrote them shows
 * what it wrote, as on Linux. A process with no address space, or none of
 * its arguments kept, shows its name as the one argument.
 */
static int procfs_gen_cmdline(struct procfs_text *t, int pid)
{
    process_t *p = procfs_proc(pid);
    int i;

    if (!p) {
        return -ENOENT;
    }
    if (p->mm && p->mm->arg_end > p->mm->arg_start) {
        uintptr_t start = p->mm->arg_start;
        uintptr_t end = p->mm->arg_end;

        ap_memory(t, p, start, end);
        return t->len;
    }
    for (i = 0; i < (int)sizeof(p->comm) && p->comm[i]; i++) {
        ap_ch(t, p->comm[i]);
    }
    if (t->len > 0) {
        ap_ch(t, '\0'); /* argv[0] terminator */
    }
    return t->len;
}

/** /proc/<pid>/environ: the environment strings of the initial stack, each
 * ended by a NUL; -EACCES for a caller `procfs_may_read_mem` refuses. */
static int procfs_gen_environ(struct procfs_text *t, int pid)
{
    process_t *p = procfs_proc(pid);

    if (!p) {
        return -ENOENT;
    }
    if (!procfs_may_read_mem(p)) {
        return -EACCES;
    }
    if (p->mm && p->mm->env_end > p->mm->env_start) {
        uintptr_t start = p->mm->env_start;
        uintptr_t end = p->mm->env_end;

        ap_memory(t, p, start, end);
    }
    return t->len;
}

/** /proc/<pid>/auxv: the auxiliary vector the program was given, the words
 * of its initial stack as the address space keeps them, through its AT_NULL
 * pair; -EACCES for a caller `procfs_may_read_mem` refuses. */
static int procfs_gen_auxv(struct procfs_text *t, int pid)
{
    process_t *p = procfs_proc(pid);
    uint32_t i;
    int b;

    if (!p) {
        return -ENOENT;
    }
    if (!procfs_may_read_mem(p)) {
        return -EACCES;
    }
    for (i = 0; p->mm && i + 1 < MM_AUXV_WORDS; i += 2) {
        const uint8_t *pair = (const uint8_t *)&p->mm->auxv[i];

        for (b = 0; b < (int)(2 * sizeof(p->mm->auxv[0])); b++) {
            ap_ch(t, (char)pair[b]);
        }
        if (p->mm->auxv[i] == 0) {
            break;
        }
    }
    return t->len;
}

/** /proc/<pid>/comm: the program name and a newline. */
static int procfs_gen_comm(struct procfs_text *t, int pid)
{
    process_t *p = procfs_proc(pid);
    int i;

    if (!p) {
        return -ENOENT;
    }
    for (i = 0; i < (int)sizeof(p->comm) && p->comm[i]; i++) {
        ap_ch(t, p->comm[i]);
    }
    ap_ch(t, '\n');
    return t->len;
}

/** Append `v` as lowercase hex digits, at least `width` of them, as
 * /proc/<pid>/maps and status write numbers. */
static void ap_xlow(struct procfs_text *t, unsigned long v, int width)
{
    static const char digits[] = "0123456789abcdef";
    int i;

    while (width < (int)sizeof(v) * 2 && (v >> (width * 4)) != 0) {
        width++;
    }
    for (i = width - 1; i >= 0; i--) {
        ap_ch(t, digits[(v >> (i * 4)) & 0xFu]);
    }
}

/** The path /proc/<pid>/maps gives region `r` of `mm`: the file it maps,
 * [heap], [stack], the program for the segments of its image, which lie
 * below the heap, the dynamic linker for the segments of its own, above it,
 * or none. */
static const char *maps_path(const struct mm *mm, const vma_t *r)
{
    if (r->file && r->file->path) {
        return r->file->path;
    }
    if (r->type == VM_HEAP) {
        return "[heap]";
    }
    if (r->type == VM_STACK) {
        return "[stack]";
    }
    if (r->type == VM_CODE || r->type == VM_DATA) {
        return r->end <= mm->vm.heap_start ? mm->exe : mm->interp;
    }
    return NULL;
}

/** Append region `r` of `mm` as its line of /proc/<pid>/maps. */
static void ap_maps_line(struct procfs_text *t, const struct mm *mm, const vma_t *r)
{
    const char *path = maps_path(mm, r);
    int start = t->len;

    ap_xlow(t, r->start, 8);
    ap_ch(t, '-');
    ap_xlow(t, r->end, 8);
    ap_ch(t, ' ');
    ap_ch(t, (r->prot & VM_PROT_READ) ? 'r' : '-');
    ap_ch(t, (r->prot & VM_PROT_WRITE) ? 'w' : '-');
    ap_ch(t, (r->prot & VM_PROT_EXEC) ? 'x' : '-');
    ap_ch(t, r->type == VM_SHARED ? 's' : 'p');
    ap_ch(t, ' ');
    ap_xlow(t, r->file ? r->pgoff * PAGE_SIZE : 0u, 8);
    ap_str(t, " 00:00 0 ");
    if (path) {
        do {
            ap_ch(t, ' ');
        } while (t->len - start < 73 && !t->nomem);
        ap_str(t, path);
    }
    ap_ch(t, '\n');
}

/**
 * /proc/<pid>/maps: a line for each region, in Linux's form: the range, the
 * rights with p or s for private or shared, the offset in the file, a device
 * and inode of 0, which the kernel does not keep, a space, and the path from
 * column 73. A region with no path ends at that space, as on Linux, where
 * BusyBox's pmap counts on it to find the end of the fields. GNU libc's
 * pthread_getattr_np finds the stack by its [stack] line. -EACCES for a
 * caller `procfs_may_read_mem` refuses, as Linux refuses another user.
 */
static int procfs_gen_maps(struct procfs_text *t, int pid)
{
    process_t *p = procfs_proc(pid);
    const struct mm *mm;
    uint32_t i;

    if (!p) {
        return -ENOENT;
    }
    if (!procfs_may_read_mem(p)) {
        return -EACCES;
    }
    mm = p->mm;
    for (i = 0; mm && i < mm->vm.count; i++) {
        ap_maps_line(t, mm, &mm->vm.vmas[i]);
    }
    return t->len;
}

/** Append one field of /proc/<pid>/smaps: `name` padded to 16 places, then
 * the kilobytes of `pages` in eight and " kB", as Linux writes them. The
 * count is a long, which holds the kilobytes of any user range: 3 GiB's on
 * i686, and on x86-64 a region past 4 TiB, whose pages times four pass 32
 * bits. */
static void ap_smaps_kb(struct procfs_text *t, const char *name, unsigned long pages)
{
    unsigned long kb = pages * (PAGE_SIZE / 1024u);
    unsigned long w = 1;
    unsigned long v;
    int start = t->len;

    ap_str(t, name);
    while (t->len - start < 16 && !t->nomem) {
        ap_ch(t, ' ');
    }
    for (v = kb; v >= 10u; v /= 10u) {
        w++;
    }
    while (w++ < 8u) {
        ap_ch(t, ' ');
    }
    ap_ulong(t, kb);
    ap_str(t, " kB\n");
}

/**
 * /proc/<pid>/smaps: each region's maps line and Linux's fields for it, which
 * BusyBox's pmap and smemcap read. The pages in memory are the present pages
 * of the region's page tables, counted past the tables it lacks
 * (`paging_user_count`), clean or dirty by the entry's dirty bit, and
 * shared or private by the region's type. A private region read from a file,
 * a mapping or a segment of the image, has as anonymous memory the pages
 * written since, the ones Linux has copied from the file; gdb's gcore leaves
 * the others out of a core and reads them from the file. The kernel does not
 * count the processes that map a page, so Pss is Rss, a page another process
 * also maps being counted whole to each; there is no swap, no huge page and
 * no locking, so those fields are 0.
 * -EACCES for a caller `procfs_may_read_mem` refuses, as for maps.
 */
static int procfs_gen_smaps(struct procfs_text *t, int pid)
{
    process_t *p = procfs_proc(pid);
    const struct mm *mm;
    uint32_t i;

    if (!p) {
        return -ENOENT;
    }
    if (!procfs_may_read_mem(p)) {
        return -EACCES;
    }
    mm = p->mm;
    for (i = 0; mm && i < mm->vm.count; i++) {
        const vma_t *r = &mm->vm.vmas[i];
        unsigned long present, clean, dirty;
        int shared = r->type == VM_SHARED;
        int of_file = r->file || r->type == VM_CODE || r->type == VM_DATA;

        paging_user_count(mm->dir, r->start, r->end, &present, &dirty);
        clean = present - dirty;
        ap_maps_line(t, mm, r);
        ap_smaps_kb(t, "Size:", (r->end - r->start) / PAGE_SIZE);
        ap_smaps_kb(t, "KernelPageSize:", 1);
        ap_smaps_kb(t, "MMUPageSize:", 1);
        ap_smaps_kb(t, "Rss:", clean + dirty);
        ap_smaps_kb(t, "Pss:", clean + dirty);
        ap_smaps_kb(t, "Shared_Clean:", shared ? clean : 0);
        ap_smaps_kb(t, "Shared_Dirty:", shared ? dirty : 0);
        ap_smaps_kb(t, "Private_Clean:", shared ? 0 : clean);
        ap_smaps_kb(t, "Private_Dirty:", shared ? 0 : dirty);
        ap_smaps_kb(t, "Referenced:", clean + dirty);
        ap_smaps_kb(t, "Anonymous:", !of_file ? clean + dirty : shared ? 0 : dirty);
        ap_smaps_kb(t, "Swap:", 0);
        ap_smaps_kb(t, "SwapPss:", 0);
        ap_smaps_kb(t, "Locked:", 0);
        ap_str(t, "VmFlags:");
        ap_str(t, (r->prot & VM_PROT_READ) ? " rd" : "");
        ap_str(t, (r->prot & VM_PROT_WRITE) ? " wr" : "");
        ap_str(t, (r->prot & VM_PROT_EXEC) ? " ex" : "");
        ap_str(t, shared ? " sh" : "");
        ap_str(t, r->type == VM_STACK ? " gd" : "");
        ap_str(t, (r->prot & VM_PROT_IO) ? " io pf" : "");
        ap_ch(t, '\n');
    }
    return t->len;
}

/** The bytes of `mm`'s regions of type `type`. */
static unsigned long mm_bytes(const struct mm *mm, vm_type_t type)
{
    unsigned long n = 0;
    uint32_t i;

    for (i = 0; i < mm->vm.count; i++) {
        if (mm->vm.vmas[i].type == type) {
            n += mm->vm.vmas[i].end - mm->vm.vmas[i].start;
        }
    }
    return n;
}

/** Append "Name:\t" and the kilobytes of `bytes`, right-aligned in eight
 * places as Linux writes them, and " kB". */
static void ap_kb(struct procfs_text *t, const char *name, unsigned long bytes)
{
    unsigned long kb = bytes / 1024u;
    uint32_t w = 1;
    unsigned long v;

    for (v = kb; v >= 10u; v /= 10u) {
        w++;
    }
    ap_str(t, name);
    ap_str(t, ":\t");
    while (w++ < 8u) {
        ap_ch(t, ' ');
    }
    ap_ulong(t, kb);
    ap_str(t, " kB\n");
}

/** Append "Name:\t" and the 64-bit set `set` as sixteen hex digits. */
static void ap_set(struct procfs_text *t, const char *name, uint64_t set)
{
    ap_str(t, name);
    ap_str(t, ":\t");
    ap_xlow(t, (uint32_t)(set >> 32), 8);
    ap_xlow(t, (uint32_t)set, 8);
    ap_ch(t, '\n');
}

/** Append "Name:\t" and four ids separated by tabs: the real, effective and
 * saved ones, and the effective one again as the file-system id. */
static void ap_ids(struct procfs_text *t, const char *name, uint32_t r, uint32_t e, uint32_t s)
{
    ap_str(t, name);
    ap_str(t, ":\t");
    ap_u32(t, r);
    ap_ch(t, '\t');
    ap_u32(t, e);
    ap_ch(t, '\t');
    ap_u32(t, s);
    ap_ch(t, '\t');
    ap_u32(t, e);
    ap_ch(t, '\n');
}

/** `head`, then the numbers of `p` in each PID namespace from the reader's
 * down to its own, as the NSpid line has them. */
static void ap_ns_pids(struct procfs_text *t, const char *head, const process_t *p)
{
    uint32_t from = proc_pidns(current_proc)->level, to = proc_pidns(p)->level, l;

    ap_str(t, head);
    ap_u32(t, pid_seen(p->pid));
    for (l = from + 1u; l <= to && pid_seen(p->pid) != 0; l++) {
        ap_ch(t, '\t');
        ap_u32(t, p->nspid[l]);
    }
}

/** The five signal sets of `p`'s status: pending for the thread and for
 * the process, blocked, ignored and caught. */
static void ap_signal_sets(struct procfs_text *t, const process_t *p)
{
    uint64_t ignored = 0;
    uint64_t caught = 0;
    int sig;

    for (sig = 1; sig <= KSIG_MAX; sig++) {
        if (p->sigacts[sig].handler == KSIG_IGN) {
            ignored |= KSIG_BIT(sig);
        } else if (p->sigacts[sig].handler != KSIG_DFL) {
            caught |= KSIG_BIT(sig);
        }
    }
    ap_set(t, "SigPnd", p->sig_pending);
    ap_set(t, "ShdPnd", p->tg->pending);
    ap_set(t, "SigBlk", p->sig_blocked);
    ap_set(t, "SigIgn", ignored);
    ap_set(t, "SigCgt", caught);
}

/**
 * /proc/<pid>/status: the lines of Linux's that programs read, among them
 * the name, the state, the ids, the tracer, the memory in kilobytes, the
 * thread count and the signal sets. The file-system ids are the effective
 * ones. The capability sets are full for a process `proc_privileged` passes
 * and empty otherwise, since the kernel has no capabilities but that test.
 * VmPeak and VmHWM are the present sizes, since the kernel keeps no peak.
 */
static int procfs_gen_status(struct procfs_text *t, int pid)
{
    process_t *p = procfs_proc(pid);
    const struct mm *mm;
    uint64_t caps;
    uint32_t i;

    if (!p) {
        return -ENOENT;
    }
    mm = p->mm;
    ap_str(t, "Name:\t");
    for (i = 0; i < sizeof(p->comm) && p->comm[i]; i++) {
        ap_ch(t, p->comm[i]);
    }
    if (p->fs) {
        ap_str(t, "\nUmask:\t0");
        ap_ch(t, (char)('0' + ((p->fs->umask >> 6) & 7u)));
        ap_ch(t, (char)('0' + ((p->fs->umask >> 3) & 7u)));
        ap_ch(t, (char)('0' + (p->fs->umask & 7u)));
    }
    ap_str(t, "\nState:\t");
    ap_str(t, p->state == PROC_ZOMBIE    ? "Z (zombie)"
              : p->ptrace_status != 0    ? "t (tracing stop)"
              : p->state == PROC_STOPPED ? "T (stopped)"
              : p->state == PROC_BLOCKED ? (p->uninterruptible ? "D (disk sleep)" : "S (sleeping)")
                                         : "R (running)");
    ap_str(t, "\nTgid:\t");
    ap_u32(t, pid_seen(p->tgid));
    ap_str(t, "\nNgid:\t0\nPid:\t");
    ap_u32(t, pid_seen(p->pid));
    ap_str(t, "\nPPid:\t");
    ap_u32(t, pid_seen(p->parent_pid));
    ap_str(t, "\nTracerPid:\t");
    ap_u32(t, p->ptrace ? pid_seen(p->tracer) : 0u);
    ap_ch(t, '\n');
    ap_ids(t, "Uid", uid_seen(p->ruid), uid_seen(p->euid), uid_seen(p->suid));
    ap_ids(t, "Gid", gid_seen(p->rgid), gid_seen(p->egid), gid_seen(p->sgid));
    ap_str(t, "FDSize:\t");
    ap_u32(t, (uint32_t)fd_table_cap(p->fds));
    ap_str(t, "\nGroups:\t");
    for (i = 0; i < p->ngroups; i++) {
        ap_u32(t, gid_seen(p->groups[i]));
        ap_ch(t, ' ');
    }
    ap_ns_pids(t, "\nNStgid:\t", p->tg->leader ? p->tg->leader : p);
    ap_ns_pids(t, "\nNSpid:\t", p);
    ap_str(t, "\nNSpgid:\t");
    ap_u32(t, pid_seen(p->pgid));
    ap_str(t, "\nNSsid:\t");
    ap_u32(t, pid_seen(p->sid));
    ap_ch(t, '\n');
    if (mm) {
        unsigned long size = vm_total(&mm->vm);
        unsigned long rss = mm_resident(mm) * PAGE_SIZE;

        ap_kb(t, "VmPeak", size);
        ap_kb(t, "VmSize", size);
        ap_kb(t, "VmLck", 0);
        ap_kb(t, "VmPin", 0);
        ap_kb(t, "VmHWM", rss);
        ap_kb(t, "VmRSS", rss);
        ap_kb(t, "VmData", mm_bytes(mm, VM_DATA) + mm_bytes(mm, VM_HEAP) + mm_bytes(mm, VM_ANON));
        ap_kb(t, "VmStk", mm_bytes(mm, VM_STACK));
        ap_kb(t, "VmExe", mm_bytes(mm, VM_CODE));
        ap_kb(t, "VmLib", mm_bytes(mm, VM_FILE));
        ap_kb(t, "VmSwap", 0);
    }
    ap_str(t, "Threads:\t");
    ap_u32(t, p->tg->live);
    ap_str(t, "\nSigQ:\t0/");
    ap_u32(t, KSIG_MAX);
    ap_ch(t, '\n');
    ap_signal_sets(t, p);
    caps = proc_privileged(p) ? 0x000001FFFFFFFFFFULL : 0;
    ap_set(t, "CapInh", 0);
    ap_set(t, "CapPrm", caps);
    ap_set(t, "CapEff", caps);
    ap_set(t, "CapBnd", 0x000001FFFFFFFFFFULL);
    ap_set(t, "CapAmb", 0);
    ap_str(t, "NoNewPrivs:\t");
    ap_u32(t, p->no_new_privs);
    ap_str(t, "\nSeccomp:\t");
    ap_u32(t, p->seccomp_mode);
    ap_str(t, "\nSeccomp_filters:\t");
    ap_u32(t, seccomp_filter_count(p));
    ap_str(t, "\nCpus_allowed:\t1\nCpus_allowed_list:\t0\n");
    ap_str(t, "voluntary_ctxt_switches:\t0\nnonvoluntary_ctxt_switches:\t0\n");
    return t->len;
}

/** /proc/<pid>/statm: the sizes in pages, total, resident, shared, text,
 * library, data with stack, and dirty, of which the kernel keeps the first,
 * second, fourth and sixth. */
static int procfs_gen_statm(struct procfs_text *t, int pid)
{
    process_t *p = procfs_proc(pid);
    const struct mm *mm;

    if (!p) {
        return -ENOENT;
    }
    mm = p->mm;
    if (!mm) {
        ap_str(t, "0 0 0 0 0 0 0\n");
        return t->len;
    }
    ap_ulong(t, vm_total(&mm->vm) / PAGE_SIZE);
    ap_ch(t, ' ');
    ap_ulong(t, mm_resident(mm));
    ap_str(t, " 0 ");
    ap_ulong(t, mm_bytes(mm, VM_CODE) / PAGE_SIZE);
    ap_str(t, " 0 ");
    ap_ulong(t, (mm_bytes(mm, VM_DATA) + mm_bytes(mm, VM_HEAP) + mm_bytes(mm, VM_ANON) +
                 mm_bytes(mm, VM_STACK)) /
                    PAGE_SIZE);
    ap_str(t, " 0\n");
    return t->len;
}

/** Append `s` and spaces to `width` bytes, or `s` and one space when it is
 * as wide: printf's "%-Ns " for a column of N. */
static void ap_left(struct procfs_text *t, const char *s, int width)
{
    int n = (int)k_strlen(s);

    ap_str(t, s);
    do {
        ap_ch(t, ' ');
    } while (++n < width);
}

/** Append a limit's value in its column of 21: "unlimited", or the number. */
static void ap_limit(struct procfs_text *t, unsigned long v)
{
    char digits[24];
    int i = (int)sizeof(digits) - 1;

    if (v == RLIM_INFINITY) {
        ap_left(t, "unlimited", 21);
        return;
    }
    digits[i] = '\0';
    do {
        digits[--i] = (char)('0' + v % 10);
        v /= 10;
    } while (v != 0);
    ap_left(t, digits + i, 21);
}

/**
 * /proc/<pid>/limits: Linux's table of the sixteen resource limits, each a
 * name, the soft and hard values or "unlimited", and a unit, in Linux's
 * columns. The six the kernel keeps (`rlim`) are the process's, and the
 * others unlimited, as getrlimit reports them.
 */
static int procfs_gen_limits(struct procfs_text *t, int pid)
{
    static const struct {
        const char *name;
        int slot;
        const char *unit;
    } rows[] = {{"Max cpu time", -1, "seconds"},
                {"Max file size", RLIM_FSIZE, "bytes"},
                {"Max data size", -1, "bytes"},
                {"Max stack size", RLIM_STACK, "bytes"},
                {"Max core file size", RLIM_CORE, "bytes"},
                {"Max resident set", -1, "bytes"},
                {"Max processes", RLIM_NPROC, "processes"},
                {"Max open files", RLIM_NOFILE, "files"},
                {"Max locked memory", -1, "bytes"},
                {"Max address space", RLIM_AS, "bytes"},
                {"Max file locks", -1, "locks"},
                {"Max pending signals", -1, "signals"},
                {"Max msgqueue size", -1, "bytes"},
                {"Max nice priority", -1, 0},
                {"Max realtime priority", -1, 0},
                {"Max realtime timeout", -1, "us"}};
    process_t *p = procfs_proc(pid);
    uint32_t i;

    if (!p) {
        return -ENOENT;
    }
    ap_left(t, "Limit", 26);
    ap_left(t, "Soft Limit", 21);
    ap_left(t, "Hard Limit", 21);
    ap_left(t, "Units", 10);
    for (i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        int s = rows[i].slot;

        ap_ch(t, '\n');
        ap_left(t, rows[i].name, 26);
        ap_limit(t, s < 0 ? RLIM_INFINITY : p->rlim[s].cur);
        ap_limit(t, s < 0 ? RLIM_INFINITY : p->rlim[s].max);
        if (rows[i].unit) {
            ap_left(t, rows[i].unit, 10);
        }
    }
    ap_ch(t, '\n');
    return t->len;
}

/** The name of the /proc/<pid> file of kind `kind`, for a descriptor's link. */
static const char *procfs_pid_file_name(int kind)
{
    switch (kind) {
    case SYNTH_PROC_CMDLINE:
        return "cmdline";
    case SYNTH_PROC_MAPS:
        return "maps";
    case SYNTH_PROC_SMAPS:
        return "smaps";
    case SYNTH_PROC_STATUS:
        return "status";
    case SYNTH_PROC_STATM:
        return "statm";
    case SYNTH_PROC_AUXV:
        return "auxv";
    case SYNTH_PROC_ENVIRON:
        return "environ";
    case SYNTH_PROC_COMM:
        return "comm";
    case SYNTH_PROC_PIDMOUNTS:
        return "mounts";
    case SYNTH_PROC_MOUNTINFO:
        return "mountinfo";
    case SYNTH_PROC_MEM:
        return "mem";
    case SYNTH_PROC_OOM_ADJ:
        return "oom_score_adj";
    case SYNTH_PROC_LIMITS:
        return "limits";
    case SYNTH_PROC_UID_MAP:
        return "uid_map";
    case SYNTH_PROC_GID_MAP:
        return "gid_map";
    case SYNTH_PROC_SETGROUPS:
        return "setgroups";
    default:
        return "stat";
    }
}

/** The path of the file of origin `origin`, for a descriptor's link; NULL
 * for a file of /proc/<pid>, /proc/sys or /sys, and for the files of
 * /proc/net with a name from `synth_net_name`. */
static const char *procfs_global_fd_path(int origin)
{
    switch (origin) {
    case SYNTH_PROC_BOOTLINE:
        return "/proc/cmdline";
    case SYNTH_PROC_DISKINFO:
        return "/proc/disk";
    case SYNTH_PROC_NETDEV:
        return "/proc/net/dev";
    case SYNTH_PROC_NETROUTE:
        return "/proc/net/route";
    case SYNTH_PROC_NETARP:
        return "/proc/net/arp";
    case SYNTH_PROC_NETTCP:
        return "/proc/net/tcp";
    case SYNTH_PROC_CONSOLE:
        return "/proc/console";
    case SYNTH_PROC_AUDIO:
        return "/proc/audio";
    case SYNTH_PROC_UPTIME:
        return "/proc/uptime";
    case SYNTH_PROC_STATSYS:
        return "/proc/stat";
    case SYNTH_PROC_MEMINFO:
        return "/proc/meminfo";
    case SYNTH_PROC_MOUNTS:
        return "/proc/mounts";
    case SYNTH_PROC_USB:
        return "/proc/usb";
    case SYNTH_PROC_FILESYSTEMS:
        return "/proc/filesystems";
    case SYNTH_PROC_LOOP:
        return "/proc/loop";
    case SYNTH_PROC_INTERRUPTS:
        return "/proc/interrupts";
    case SYNTH_PROC_CPUINFO:
        return "/proc/cpuinfo";
    case SYNTH_PROC_LOADAVG:
        return "/proc/loadavg";
    case SYNTH_PROC_DISKSTATS:
        return "/proc/diskstats";
    case SYNTH_PROC_VERSION:
        return "/proc/version";
    case SYNTH_PROC_PARTITIONS:
        return "/proc/partitions";
    case SYNTH_PROC_VMSTAT:
        return "/proc/vmstat";
    case SYNTH_PROC_SLABINFO:
        return "/proc/slabinfo";
    case SYNTH_PROC_SWAPS:
        return "/proc/swaps";
    case SYNTH_PROC_LOCKS:
        return "/proc/locks";
    case SYNTH_PROC_SYSVIPC:
        return "/proc/sysvipc/shm";
    case SYNTH_PROC_SYSVIPC + 1:
        return "/proc/sysvipc/sem";
    case SYNTH_PROC_SYSVIPC + 2:
        return "/proc/sysvipc/msg";
    default:
        return NULL;
    }
}

/**
 * Append the /proc/<pid>/fd/<fd> target of a file or directory descriptor to
 * `t`: the absolute path of a KRFFS node, "/memfd:NAME (deleted)" for a memfd
 * as Linux shows one, the path a node of another type was opened by, and "/"
 * for the root directory, a stale descriptor or a path that does not fit. A
 * KRFFS path is rebuilt, in 256 bytes, only from a node still live and
 * reachable, since the node of a stale descriptor, unlinked or relocated,
 * may be on the free list.
 */
static void procfs_file_target(struct procfs_text *t, const fd_entry_t *e)
{
    char path[256];

    if (e->node && e->node->type == KRFFS_RESERVED_NODE) {
        if (krffs_node_path(e->node, path, sizeof(path)) == 0 && krffs_resolve(path) == e->node) {
            ap_str(t, path);
        } else {
            ap_ch(t, '/');
        }
    } else if (e->ofd && e->ofd->vmnt && e->ofd->vmnt->type == &vfs_tmpfs_type &&
               tmpfs_label(e->ofd->vnode)) {
        ap_str(t, "/memfd:");
        ap_str(t, tmpfs_label(e->ofd->vnode));
        ap_str(t, " (deleted)");
    } else if (e->ofd && e->ofd->vmnt && e->ofd->dirpath[0]) {
        ap_str(t, e->ofd->dirpath);
    } else {
        ap_ch(t, '/');
    }
}

/**
 * The target of each kind of descriptor is the one Linux's /proc gives: the
 * path of a file or directory (`procfs_file_target`), /dev/console,
 * /dev/NAME for a device, /dev/pts/N, which musl's ttyname_r matches,
 * /dev/ptmx, pipe:[N], socket:[N], and anon_inode:NAME. A socket names its
 * own inode, the same number `/proc/net/tcp` and `fstat` report, so one
 * socket is one thing to a program reading all three. A synthetic directory
 * has its path, a descriptor of a file of /proc or /sys the path of that
 * file, rebuilt from its origin, and an O_PATH descriptor of a /dev or /proc
 * node the path it was opened by. The framebuffer and the mouse have no
 * name, and show "anon_inode:[device]". A descriptor that is closed, whose
 * release waits for another thread's call or is under way, has no link, as
 * it has no entry in the directory's listing and no lookup finds it.
 */
int procfs_readlink_fd(int pid, int fd, char *buf, int cap)
{
    struct procfs_text text;
    struct procfs_text *t = &text;
    process_t *p;
    fd_entry_t *e;
    int n;

    procfs_text_init(t);

    p = get_process((uint32_t)pid);
    if (!p || p->state == PROC_UNUSED) {
        return -ENOENT;
    }
    if (fd < 0 || fd >= fd_table_cap(p->fds)) {
        return -ENOENT;
    }
    e = &p->fds->entries[fd];
    if (e->type == FD_TYPE_NONE || (e->flags & FD_FLAG_CLOSING)) {
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
    case FD_TYPE_UNIX:
        ap_str(t, "socket:[");
        ap_u32(t, e->priv ? unix_inode(e) : 0u);
        ap_ch(t, ']');
        break;
    case FD_TYPE_ANON:
        if (!nsfs_ns(e)) {
            ap_str(t, "anon_inode:"); /* a namespace's is "KIND:[INUM]", as nsfs's */
        }
        ap_str(t, e->priv ? ((const struct anon_obj *)e->priv)->name : "[unknown]");
        break;
    case FD_TYPE_FILE:
    case FD_TYPE_DIR:
        procfs_file_target(t, e);
        break;
    case FD_TYPE_SYNTH_DIR: {
        struct synth_dir *sd = (struct synth_dir *)e->priv;
        char path[64];
        if (sd) {
            synth_dir_path(sd->kind, sd->pid, sd->tid, path, (int)sizeof(path));
            ap_str(t, path);
        }
        break;
    }
    case FD_TYPE_PROC: {
        struct procfs_file *pf = (struct procfs_file *)e->priv;
        if (pf) {
            const char *path = procfs_global_fd_path(pf->origin_file);
            if (path) {
                ap_str(t, path);
                break;
            }
            if (synth_net_name(pf->origin_file)) {
                ap_str(t, "/proc/net/");
                ap_str(t, synth_net_name(pf->origin_file));
                break;
            }
            if (pf->origin_file == SYNTH_SYS_UEVENT) {
                char where[64];
                int node = pf->origin_task >> 8;

                synth_sys_path(node, pf->origin_pid, where, (int)sizeof(where));
                ap_str(t, where);
                ap_ch(t, '/');
                ap_str(t, sysfs_attr(node, pf->origin_task & 255)
                              ? sysfs_attr(node, pf->origin_task & 255)
                              : "uevent");
                break;
            }
            if (pf->origin_file == SYNTH_PROC_SYSCTL) {
                char where[96];

                synth_dir_path(VNODE_PROC_SYS_DIR, pf->origin_pid, 0, where, (int)sizeof(where));
                ap_str(t, where);
                break;
            }
            ap_str(t, "/proc/");
            if (pf->origin_task != 0) {
                ap_int(t, pf->origin_task);
                ap_str(t, "/task/");
            }
            ap_int(t, pf->origin_pid);
            ap_ch(t, '/');
            ap_str(t, procfs_pid_file_name(pf->origin_file));
        }
        break;
    }
    case FD_TYPE_SYNTH_PATH:
        ap_str(t, e->ofd && e->ofd->dirpath[0] ? e->ofd->dirpath : "/");
        break;
    case FD_TYPE_FRAMEBUFFER:
    case FD_TYPE_MOUSE:
    case FD_TYPE_NONE:
    default:
        ap_str(t, "anon_inode:[device]");
        break;
    }
    if (t->nomem) {
        kfree(t->buf);
        return -ENOMEM;
    }
    n = t->len < cap - 1 ? t->len : cap - 1;
    if (n > 0) {
        k_memcpy(buf, t->buf, (size_t)n);
    }
    if (cap > 0) {
        buf[n] = '\0';
    }
    kfree(t->buf);
    return n;
}

/**
 * The processes are counted in the order of the process list. A process is
 * listed once, by its leader; the ids of its other threads are not listed,
 * as on Linux, though /proc/<tid> still opens. The idle process, pid 0, is
 * not listed, as Linux lists no pid 0, nor is a process outside the
 * caller's PID namespace.
 */
int procfs_nth_pid(int n)
{
    const process_t *p;
    int seen = 0;

    if (n < 0) {
        return -1;
    }
    for (p = proc_list; p; p = p->next) {
        if (p->pid != p->tgid || p == proc_idle || pid_seen(p->pid) == 0) {
            continue;
        }
        if (seen == n) {
            return (int)pid_seen(p->pid);
        }
        seen++;
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

uint32_t procfs_ino_cpuinfo(void)
{
    return PROCFS_INO_CPUINFO;
}

uint32_t procfs_ino_diskstats(void)
{
    return PROCFS_INO_DISKSTATS;
}

uint32_t procfs_ino_version(void)
{
    return PROCFS_INO_VERSION;
}

uint32_t procfs_ino_partitions(void)
{
    return PROCFS_INO_PARTITIONS;
}

uint32_t procfs_ino_vmstat(void)
{
    return PROCFS_INO_VMSTAT;
}

uint32_t procfs_ino_slabinfo(void)
{
    return PROCFS_INO_SLABINFO;
}

uint32_t procfs_ino_swaps(void)
{
    return PROCFS_INO_SWAPS;
}

uint32_t procfs_ino_locks(void)
{
    return PROCFS_INO_LOCKS;
}

uint32_t procfs_ino_loadavg(void)
{
    return PROCFS_INO_LOADAVG;
}

uint32_t procfs_ino_sys(int entry)
{
    return PROCFS_INO_SYS + (uint32_t)entry;
}

uint32_t procfs_ino_sysdir(void)
{
    return PROCFS_INO_SYS;
}

uint32_t procfs_ino_mounts(void)
{
    return PROCFS_INO_MOUNTS;
}

uint32_t procfs_ino_audio(void)
{
    return PROCFS_INO_AUDIO;
}

uint32_t procfs_ino_usb(void)
{
    return PROCFS_INO_USB;
}

uint32_t procfs_ino_loop(void)
{
    return PROCFS_INO_LOOP;
}

uint32_t procfs_ino_filesystems(void)
{
    return PROCFS_INO_FILESYSTEMS;
}

uint32_t procfs_ino_interrupts(void)
{
    return PROCFS_INO_INTERRUPTS;
}

uint32_t procfs_ino_console(void)
{
    return PROCFS_INO_CONSOLE;
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

uint32_t procfs_ino_net(int origin)
{
    return PROCFS_INO_NET + (uint32_t)(origin - SYNTH_PROC_NETUDP);
}

uint32_t procfs_ino_netstatdir(void)
{
    return PROCFS_INO_NETSTAT;
}

uint32_t procfs_ino_sysvipcdir(void)
{
    return PROCFS_INO_SYSVIPC;
}

uint32_t procfs_ino_sysvipc(int origin)
{
    return PROCFS_INO_SYSVIPC + 1u + (uint32_t)(origin - SYNTH_PROC_SYSVIPC);
}

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
    if (synth_net_name(pf->origin_file)) {
        return procfs_ino_net(pf->origin_file);
    }
    if (synth_sysvipc_name(pf->origin_file)) {
        return procfs_ino_sysvipc(pf->origin_file);
    }
    if (pf->origin_file == SYNTH_SYS_UEVENT) {
        return sysfs_ino(pf->origin_task >> 8, pf->origin_pid, 1 + (pf->origin_task & 255));
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
    if (pf->origin_file == SYNTH_PROC_CPUINFO) {
        return PROCFS_INO_CPUINFO;
    }
    if (pf->origin_file == SYNTH_PROC_LOADAVG) {
        return PROCFS_INO_LOADAVG;
    }
    if (pf->origin_file == SYNTH_PROC_DISKSTATS) {
        return PROCFS_INO_DISKSTATS;
    }
    if (pf->origin_file == SYNTH_PROC_VERSION) {
        return PROCFS_INO_VERSION;
    }
    if (pf->origin_file == SYNTH_PROC_PARTITIONS) {
        return PROCFS_INO_PARTITIONS;
    }
    if (pf->origin_file == SYNTH_PROC_VMSTAT) {
        return PROCFS_INO_VMSTAT;
    }
    if (pf->origin_file == SYNTH_PROC_SLABINFO) {
        return PROCFS_INO_SLABINFO;
    }
    if (pf->origin_file == SYNTH_PROC_SWAPS) {
        return PROCFS_INO_SWAPS;
    }
    if (pf->origin_file == SYNTH_PROC_LOCKS) {
        return PROCFS_INO_LOCKS;
    }
    if (pf->origin_file == SYNTH_PROC_SYSCTL) {
        return PROCFS_INO_SYS + (uint32_t)pf->origin_pid;
    }
    if (pf->origin_file == SYNTH_PROC_MOUNTS) {
        return PROCFS_INO_MOUNTS;
    }
    if (pf->origin_file == SYNTH_PROC_AUDIO) {
        return PROCFS_INO_AUDIO;
    }
    if (pf->origin_file == SYNTH_PROC_USB) {
        return PROCFS_INO_USB;
    }
    if (pf->origin_file == SYNTH_PROC_FILESYSTEMS) {
        return PROCFS_INO_FILESYSTEMS;
    }
    if (pf->origin_file == SYNTH_PROC_LOOP) {
        return PROCFS_INO_LOOP;
    }
    if (pf->origin_file == SYNTH_PROC_INTERRUPTS) {
        return PROCFS_INO_INTERRUPTS;
    }
    if (pf->origin_file == SYNTH_PROC_CONSOLE) {
        return PROCFS_INO_CONSOLE;
    }
    return procfs_ino_pid(pf->origin_pid);
}

uint32_t procfs_ino_pid(int pid)
{
    return PROCFS_INO_PID_BASE + (uint32_t)(pid & 0xffff);
}

/**
 * Render /proc/diskstats: a line for each disk node, the whole disk and each
 * partition, in Linux's twenty fields: the device numbers and name, then the
 * reads that ended well, none merged, their sectors and milliseconds, the
 * same of the writes, the requests in progress, the milliseconds the device
 * was busy and the sum of the transfers' milliseconds, and zero for the
 * discards and the flushes, which are not counted per device. A whole disk's
 * counts are of every transfer to it, through any of its nodes, as Linux's
 * are (`block_request_flags`); a partition's are its view's and those of the
 * kernel's own view of a boot root on it (`devfs_window_alias`).
 */
static int procfs_gen_diskstats(struct procfs_text *t)
{
    const block_device_t *dev, *alias;
    const char *name;
    uint32_t major, minor;
    int i, got;

    for (i = 0; (got = devfs_disk_node_at(i, &name, &major, &minor, &dev, &alias)) >= 0; i++) {
        struct block_stats sum;
        const struct block_stats *s = &sum;

        if (got == 0) {
            continue;
        }
        sum = dev->stats;
        if (alias) {
            sum.reads += alias->stats.reads;
            sum.read_sectors += alias->stats.read_sectors;
            sum.read_ms += alias->stats.read_ms;
            sum.writes += alias->stats.writes;
            sum.write_sectors += alias->stats.write_sectors;
            sum.write_ms += alias->stats.write_ms;
            sum.in_flight += alias->stats.in_flight;
            sum.busy_ms += alias->stats.busy_ms;
        }
        ap_u32_width(t, major, 4);
        ap_u32_width(t, minor, 8);
        ap_ch(t, ' ');
        ap_str(t, name);
        ap_ch(t, ' ');
        ap_u32(t, s->reads);
        ap_str(t, " 0 ");
        ap_u32(t, s->read_sectors);
        ap_ch(t, ' ');
        ap_u32(t, s->read_ms);
        ap_ch(t, ' ');
        ap_u32(t, s->writes);
        ap_str(t, " 0 ");
        ap_u32(t, s->write_sectors);
        ap_ch(t, ' ');
        ap_u32(t, s->write_ms);
        ap_ch(t, ' ');
        ap_u32(t, s->in_flight);
        ap_ch(t, ' ');
        ap_u32(t, s->busy_ms);
        ap_ch(t, ' ');
        ap_u32(t, s->read_ms + s->write_ms);
        ap_str(t, " 0 0 0 0 0 0\n");
    }
    return t->nomem ? -1 : t->len;
}

/**
 * Render /proc/partitions: the header Linux prints, then a line for each disk
 * node /proc/diskstats lists, with its device numbers, its size in kibibytes
 * and its name, in Linux's columns, which BusyBox's `fdisk -l` reads to find
 * the disks.
 */
static int procfs_gen_partitions(struct procfs_text *t)
{
    const block_device_t *dev, *alias;
    const char *name;
    uint32_t major, minor;
    int i, got;

    ap_str(t, "major minor  #blocks  name\n\n");
    for (i = 0; (got = devfs_disk_node_at(i, &name, &major, &minor, &dev, &alias)) >= 0; i++) {
        uint64_t kib = dev ? dev->sector_count : 0;
        uint32_t size = dev ? dev->sector_size : 0;

        if (got == 0) {
            continue;
        }
        for (; size > 1024; size >>= 1) {
            kib <<= 1;
        }
        for (; size != 0 && size < 1024; size <<= 1) {
            kib >>= 1;
        }
        ap_u32_width(t, major, 4);
        ap_u32_width(t, minor, 9);
        ap_u32_width(t, kib > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)kib, 11);
        ap_ch(t, ' ');
        ap_str(t, name);
        ap_ch(t, '\n');
    }
    return t->nomem ? -1 : t->len;
}

/**
 * Render /proc/net/dev: the two header lines and one line for each interface
 * the caller sees (`netdev_seen_count`), in the order and with the sixteen
 * counters Linux prints, since `ifconfig` and the programs like it parse
 * this file by counting fields.
 *
 * Seven of the sixteen come from the interface: bytes and packets each way,
 * receive errors, receive drops, and transmit drops. The receive errors and
 * drops are the ones the drivers note (`ethernet_note_rx_error`,
 * `ethernet_note_rx_drop`) and the frames `ethernet_rx` drops, and the
 * transmit drops are the frames a driver refused or `ethernet_tx` did not
 * give it. Seven more come from the card, through the driver's `counters`,
 * in the columns Linux fills from a card's own tallies: frames the card
 * missed add to the receive drops, alignment errors are the frame column,
 * multicasts their own, and on the transmit side errors, underruns (fifo),
 * collisions and the transmissions the card aborted (carrier). A card that
 * keeps no such counts leaves them zero. The two compressed columns and the
 * receive fifo are zero: nothing here has them.
 */
static int procfs_gen_netdev(struct procfs_text *t)
{
    int i;

    ap_str(t, "Inter-|   Receive                                                |"
              "  Transmit\n"
              " face |bytes    packets errs drop fifo frame compressed multicast|"
              "bytes    packets errs drop fifo colls carrier compressed\n");
    for (i = 0; i < netdev_seen_count(); i++) {
        const struct netdev *nd = netdev_at(i);
        struct netdev_counters card = {0, 0, 0, 0, 0, 0, 0};

        if (nd->ops->counters && !nd->ops->counters(nd->dev, &card)) {
            card.rx_missed = card.rx_frame = card.rx_multicast = 0;
            card.tx_errors = card.tx_fifo = card.tx_collisions = 0;
            card.tx_carrier = 0;
        }
        ap_str(t, "  ");
        ap_str(t, nd->name);
        ap_str(t, ": ");
        ap_u32(t, nd->rx_bytes);
        ap_ch(t, ' ');
        ap_u32(t, nd->rx_packets);
        ap_ch(t, ' ');
        ap_u32(t, nd->rx_errors);
        ap_ch(t, ' ');
        ap_u32(t, nd->rx_dropped + card.rx_missed);
        /* fifo, frame, compressed, multicast */
        ap_str(t, " 0 ");
        ap_u32(t, card.rx_frame);
        ap_str(t, " 0 ");
        ap_u32(t, card.rx_multicast);
        ap_ch(t, ' ');
        ap_u32(t, nd->tx_bytes);
        ap_ch(t, ' ');
        ap_u32(t, nd->tx_packets);
        ap_ch(t, ' ');
        ap_u32(t, card.tx_errors);
        ap_ch(t, ' ');
        ap_u32(t, nd->tx_dropped);
        /* fifo, colls, carrier, compressed */
        ap_ch(t, ' ');
        ap_u32(t, card.tx_fifo);
        ap_ch(t, ' ');
        ap_u32(t, card.tx_collisions);
        ap_ch(t, ' ');
        ap_u32(t, card.tx_carrier);
        ap_str(t, " 0\n");
    }
    return t->len;
}

/*
 * The machine's own files. Every number below is one the kernel keeps; a
 * field of Linux's that the kernel has no count for is 0.
 */

/** Append a count of ticks as seconds with two decimals, as /proc/uptime
 * has them. Ticks are 100 Hz, so the fraction is the remainder. */
static void ap_ticks_as_seconds(struct procfs_text *t, uint32_t ticks)
{
    ap_u32(t, ticks / 100u);
    ap_ch(t, '.');
    ap_ch(t, (char)('0' + (ticks / 10u) % 10u));
    ap_ch(t, (char)('0' + ticks % 10u));
}

/** Append a 64-bit count of 100 Hz ticks as seconds with two decimals, by
 * its decimal digits split before the last two, since i686 has no 64-bit
 * division here. */
static void ap_ticks64_as_seconds(struct procfs_text *t, uint64_t ticks)
{
    char digits[24];
    int n = diskinfo_u64_dec(digits, (int)sizeof(digits), ticks);
    int i;

    for (i = 0; i < n - 2; i++) {
        ap_ch(t, digits[i]);
    }
    if (n < 3) {
        ap_ch(t, '0');
    }
    ap_ch(t, '.');
    ap_ch(t, n < 2 ? '0' : digits[n - 2]);
    ap_ch(t, digits[n - 1]);
}

/**
 * Render /proc/uptime: seconds since boot, and the seconds the processors
 * idled, summed over them as Linux sums them.
 *
 * Both are counted in ticks (`sched_account_tick`): the uptime is the boot
 * processor's idle and busy ticks together, and the idle time the idle
 * ticks of every processor.
 */
static int procfs_gen_uptime(struct procfs_text *t)
{
    uint32_t up = cpu_table[0].idle_ticks + cpu_table[0].busy_ticks;
    uint64_t idle = sched_idle_ticks();

    ap_ticks_as_seconds(t, up);
    ap_ch(t, ' ');
    ap_ticks64_as_seconds(t, idle);
    ap_ch(t, '\n');
    return t->len;
}

/**
 * Render /proc/loadavg as Linux does: the load averages over one, five and
 * fifteen minutes with two decimals (`sched_loadavg`), the processes running
 * or ready to run and all the processes, threads counted, with a slash, and
 * the last process ID given out.
 */
static int procfs_gen_loadavg(struct procfs_text *t)
{
    uint32_t avg[3], active, total;
    int i;

    sched_loadavg(avg);
    sched_task_counts(&active, &total);
    for (i = 0; i < 3; i++) {
        uint32_t frac = ((avg[i] & ((1u << SCHED_LOAD_SHIFT) - 1u)) * 100u) >> SCHED_LOAD_SHIFT;

        ap_u32(t, avg[i] >> SCHED_LOAD_SHIFT);
        ap_ch(t, '.');
        ap_ch(t, (char)('0' + frac / 10u));
        ap_ch(t, (char)('0' + frac % 10u));
        ap_ch(t, ' ');
    }
    ap_u32(t, active);
    ap_ch(t, '/');
    ap_u32(t, total);
    ap_ch(t, ' ');
    ap_u32(t, proc_last_pid());
    ap_ch(t, '\n');
    return t->len;
}

/**
 * Render /proc/cpuinfo: a block for each processor online, in the order of
 * their numbers, as the architecture writes one (`cpuinfo_text`).
 */
static int procfs_gen_cpuinfo(struct procfs_text *t)
{
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        if (!cpu_table[c].online) {
            continue;
        }
        if (procfs_text_room(t, t->len + CPUINFO_TEXT_MAX) != 0) {
            return -1;
        }
        t->len += (int)cpuinfo_text(t->buf + t->len, CPUINFO_TEXT_MAX, c);
    }
    return t->len;
}

/** Append `v` in decimal, all 64 bits of it. */
static void ap_u64(struct procfs_text *t, uint64_t v)
{
    char tmp[24];

    (void)diskinfo_u64_dec(tmp, (int)sizeof(tmp), v);
    ap_str(t, tmp);
}

/** Append a /proc/stat processor line's columns after its name: `user`,
 * no nice, `busy` less `user` as system, and `idle`. The columns are 64
 * bits for the `cpu` line's sums: one processor's ticks, each a word, wrap
 * after 497 days at 100 Hz, but their sum over 64 processors after eight. */
static void ap_cpu_ticks(struct procfs_text *t, uint64_t user, uint64_t busy, uint64_t idle)
{
    ap_str(t, " ");
    ap_u64(t, user);
    ap_str(t, " 0 ");
    ap_u64(t, busy - user);
    ap_ch(t, ' ');
    ap_u64(t, idle);
    ap_str(t, " 0 0 0 0 0 0\n");
}

/**
 * Render /proc/stat: the tick counters, the interrupt total, the boot epoch,
 * the process counts, and this kernel's own counters: the sleeps a signal
 * cut short (`process_sleeps_cut_short`), the longest delays since boot
 * (latency.h), and the loopback ring's.
 *
 * `loopback_dropped` is the datagrams the loopback ring had no room for, which
 * the ring logs once and counts thereafter, and `loopback_peak` is the most it
 * has held at once, which says how much room it has over what one system
 * call can put in it; the `loopback_delivered_` counts are its datagrams
 * delivered by the tick, by the idle wait and at the exit of a system call.
 *
 * The `cpu` line, and the `cpuN` line of each processor online, have user,
 * nice, system and idle in that order, then six columns this kernel does not
 * count, which are zero: a processor's ticks in user mode, in the kernel and
 * in its idle wait, with the `cpu` line their sums. Nice is zero, since no
 * tick is told apart by the nice value; an interrupt's time is the
 * interrupted code's, as `sched_account_tick` charges it.
 *
 * `processes` is what Linux means by it, the count of processes created
 * since boot, not of those alive now. `procs_running` and `procs_blocked`
 * are counted from the process list; a process is blocked, as Linux counts
 * it, while it sleeps in a wait no signal ends, the disk's among them, and
 * not in a wait a signal ends.
 */
static int procfs_gen_statsys(struct procfs_text *t)
{
    uint64_t user = 0, busy = 0, idle = 0;
    const process_t *p;
    uint32_t running = 0;
    uint32_t blocked = 0;
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        user += cpu_table[c].user_ticks;
        busy += cpu_table[c].busy_ticks;
        idle += cpu_table[c].idle_ticks;
    }
    ap_str(t, "cpu ");
    ap_cpu_ticks(t, user, busy, idle);
    for (c = 0; c < NR_CPUS; c++) {
        if (cpu_table[c].online) {
            ap_str(t, "cpu");
            ap_u32(t, (uint32_t)c);
            ap_cpu_ticks(t, cpu_table[c].user_ticks, cpu_table[c].busy_ticks,
                         cpu_table[c].idle_ticks);
        }
    }
    ap_str(t, "intr ");
    ap_u32(t, isr_interrupt_total());
    ap_ch(t, '\n');
    ap_str(t, "ctxt 0\n");
    ap_str(t, "btime ");
    ap_u32(t, (uint32_t)rtc_boot_epoch());
    ap_ch(t, '\n');
    ap_str(t, "processes ");
    ap_u32(t, process_forks_since_boot());
    ap_str(t, "\nsleeps_interrupted ");
    ap_u32(t, process_sleeps_cut_short());
    ap_ch(t, '\n');
    ap_str(t, "latency_timer_us ");
    ap_u32(t, latency_max(LAT_TIMER));
    ap_str(t, "\nlatency_disk_us ");
    ap_u32(t, latency_max(LAT_DISK));
    ap_str(t, "\nlatency_diskwait_us ");
    ap_u32(t, latency_max(LAT_DISK_WAIT));
    ap_str(t, "\nlatency_console_us ");
    ap_u32(t, latency_max(LAT_CONSOLE));
    ap_str(t, "\nloopback_dropped ");
    ap_u32(t, ip_loop_dropped());
    ap_str(t, "\nloopback_peak ");
    ap_u32(t, ip_loop_peak());
    ap_str(t, "\nloopback_delivered_tick ");
    ap_u32(t, ip_loop_delivered(IP_LOOP_FROM_TICK));
    ap_str(t, "\nloopback_delivered_idle ");
    ap_u32(t, ip_loop_delivered(IP_LOOP_FROM_IDLE));
    ap_str(t, "\nloopback_delivered_call ");
    ap_u32(t, ip_loop_delivered(IP_LOOP_FROM_CALL));
    ap_ch(t, '\n');
    for (p = proc_list; p; p = p->next) {
        if (p->state == PROC_RUNNING || p->state == PROC_READY) {
            running++;
        } else if (p->state == PROC_BLOCKED && p->uninterruptible) {
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
static void ap_frames_as_kb(struct procfs_text *t, size_t frames)
{
    ap_ulong(t, (unsigned long)frames * (PAGE_SIZE / 1024u));
    ap_str(t, " kB\n");
}

/**
 * Render /proc/meminfo from the physical frame pool.
 *
 * The pool is the whole of what this machine manages, so MemTotal and MemFree
 * are the two counts outright. Buffers and Cached are what the buffer cache
 * and the page caches hold, the disk window's and the file page cache's,
 * which reclaim gives back under pressure, so
 * MemAvailable is the free count and the two together. HeapPages and
 * HeapUsed are the kernel heap's pages and the bytes in use inside it, for a
 * test that makes and frees objects and requires the heap back where it
 * started. Slab, last, is the heap's pages but the buffer cache's, which
 * the heap holds and Buffers counts, in kilobytes: the kernel's own objects
 * as Linux's slab holds them, which BusyBox's nmeter adds to MemFree,
 * Buffers and Cached, and subtracts from MemTotal, for the memory in use.
 * Every count is taken before the first append, which may take a page for
 * the heap.
 */
static int procfs_gen_meminfo(struct procfs_text *t)
{
    size_t total = frame_total_count();
    size_t free_frames = frame_free_count();
    uint32_t buffers = bcache_resident();
    uint32_t cached = pc_resident() + filecache_pages();
    uint32_t heap_pages = 0;
    size_t heap_bytes = 0;

    kheap_stats(&heap_pages, &heap_bytes);

    ap_str(t, "MemTotal:       ");
    ap_frames_as_kb(t, total);
    ap_str(t, "MemFree:        ");
    ap_frames_as_kb(t, free_frames);
    ap_str(t, "MemAvailable:   ");
    ap_frames_as_kb(t, free_frames + buffers + cached);
    ap_str(t, "Buffers:        ");
    ap_frames_as_kb(t, buffers);
    ap_str(t, "Cached:         ");
    ap_frames_as_kb(t, cached);
    ap_str(t, "HeapPages:      ");
    ap_u32(t, heap_pages);
    ap_str(t, "\n");
    ap_str(t, "HeapUsed:       ");
    ap_ulong(t, (unsigned long)heap_bytes);
    ap_str(t, "\n");
    ap_str(t, "SwapTotal:             0 kB\n");
    ap_str(t, "SwapFree:              0 kB\n");
    ap_str(t, "Slab:           ");
    ap_frames_as_kb(t, heap_pages > buffers ? heap_pages - buffers : 0);
    return t->len;
}

/**
 * Render /proc/vmstat: the counters of Linux's file that procps's vmstat reads,
 * of those the kernel keeps. nr_free_pages and nr_file_pages are the free pages
 * and those of the page caches and the buffer cache; pgpgin and pgpgout the
 * kilobytes read from and written to the disks since boot, counted over the
 * whole disks, as Linux counts them; and pswpin and pswpout 0, since nothing is
 * swapped. A counter Linux has and the kernel does not keep is not listed, and
 * procps reads it as 0.
 */
static int procfs_gen_vmstat(struct procfs_text *t)
{
    size_t free_frames = frame_free_count();
    uint32_t file_pages = bcache_resident() + pc_resident() + filecache_pages();
    uint32_t in = 0, out = 0, major, minor;
    const block_device_t *dev, *alias;
    const char *name;
    int i, got;

    for (i = 0; (got = devfs_disk_node_at(i, &name, &major, &minor, &dev, &alias)) >= 0; i++) {
        struct devfs_disk_row row;

        if (got == 0 || devfs_disk_row(i, &row) <= 0 || row.partno != 0) {
            continue;
        }
        in += dev->stats.read_sectors / 2u + (alias ? alias->stats.read_sectors / 2u : 0);
        out += dev->stats.write_sectors / 2u + (alias ? alias->stats.write_sectors / 2u : 0);
    }
    ap_str(t, "nr_free_pages ");
    ap_ulong(t, (unsigned long)free_frames);
    ap_str(t, "\nnr_file_pages ");
    ap_u32(t, file_pages);
    ap_str(t, "\npgpgin ");
    ap_u32(t, in);
    ap_str(t, "\npgpgout ");
    ap_u32(t, out);
    ap_str(t, "\npswpin 0\npswpout 0\n");
    return t->len;
}

/**
 * Render /proc/usb: one line for each device the USB stack has configured,
 * with the root port it is on, its speed, what it says it is, and the class
 * it was driven as; then one line for each stick that is a disk, with its
 * node, what the drive says it is, its size and whether it is still there;
 * and, on a machine that has had a stick, a last line with how many names
 * are free and how many sticks have been given up since boot. A machine with
 * no controller, or one booted without USB, has the file, empty.
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
    if (usbdisk_free() != USBDISK_MAX || usbdisk_teardowns() != 0) {
        ap_str(t, "disks ");
        ap_u32(t, (uint32_t)usbdisk_free());
        ap_str(t, " free ");
        ap_u32(t, usbdisk_teardowns());
        ap_str(t, " reclaimed\n");
    }
    return t->nomem ? -1 : t->len;
}

/** Whether entry `i` of the mount table, `m`, is listed in /proc/mounts:
 * the root, /dev and /proc always, and every other mount that is active,
 * whose device is present, that has a place in the tree, which the memfd
 * mount lacks, and that the reader's mount namespace sees. */
static int procfs_mount_listed(int i, const struct vfs_mount *m)
{
    if (i < 3) {
        return m != NULL;
    }
    return m && m->state == VFS_ACTIVE && !vfs_mount_gone(m) && m->parent && vfs_mount_seen(m);
}

/** The source of entry `i`, `m`, as /proc/mounts names it; a bind's is that
 * of the mount it shows. */
static const char *procfs_mount_source(int i, const struct vfs_mount *m)
{
    int j;

    for (j = 0; m->bind_of && j < vfs_mount_slots(); j++) {
        if (vfs_mount_at(j) == m->bind_of) {
            i = j;
            m = m->bind_of;
        }
    }
    if (i == 0 && m->type == &vfs_krffs_type) {
        return krffs_mount_source();
    }
    return i == 1 ? "devfs" : i == 2 ? "proc" : m->source;
}

/**
 * Append the options of entry `i`, `m`, as /proc/mounts gives them: a KRFFS
 * root's as its mount recorded them (`krffs_mount_options`), since a RAM
 * root and a disk root differ in ways the mount table does not keep; rw or
 * ro with relatime for another of the first three entries, the root, /dev
 * and /proc; and another mount's flags as words.
 */
static void ap_mount_options(struct procfs_text *t, int i, const struct vfs_mount *m)
{
    static const struct {
        uint32_t flag;
        const char *word;
    } words[] = {{VFS_MS_SYNCHRONOUS, ",sync"},      {VFS_MS_DIRSYNC, ",dirsync"},
                 {VFS_MS_NOSUID, ",nosuid"},         {VFS_MS_NODEV, ",nodev"},
                 {VFS_MS_NOEXEC, ",noexec"},         {VFS_MS_NOATIME, ",noatime"},
                 {VFS_MS_NODIRATIME, ",nodiratime"}, {VFS_MS_RELATIME, ",relatime"}};
    uint32_t w;

    if (i == 0 && m->type == &vfs_krffs_type) {
        ap_str(t, krffs_mount_options());
        return;
    }
    ap_str(t, (m->flags & VFS_MS_RDONLY) ? "ro" : "rw");
    if (i < 3) {
        ap_str(t, ",relatime");
        return;
    }
    for (w = 0; w < sizeof(words) / sizeof(words[0]); w++) {
        if (m->flags & words[w].flag) {
            ap_str(t, words[w].word);
        }
    }
}

/**
 * Render /proc/mounts in the six-field fstab shape `df` and `mount` parse:
 * the root, then /dev and /proc, which are real mounts to every program
 * that walks this file, then every other mount in table order, each with
 * its source as the caller gave it, its mount point, its type and its
 * options.
 */
static int procfs_gen_mounts(struct procfs_text *t)
{
    int i;

    for (i = 0; i < vfs_mount_slots(); i++) {
        struct vfs_mount *m = vfs_mount_at(i);

        if (!procfs_mount_listed(i, m)) {
            continue;
        }
        ap_str(t, procfs_mount_source(i, m));
        ap_ch(t, ' ');
        ap_str(t, m->covered_path);
        ap_ch(t, ' ');
        ap_str(t, m->type->name);
        ap_ch(t, ' ');
        ap_mount_options(t, i, m);
        ap_str(t, " 0 0\n");
    }
    return t->nomem ? -1 : t->len;
}

uint32_t procfs_mount_dev(int i, const struct vfs_mount *m)
{
    uint32_t major, minor;

    if (i == 0 && m->type == &vfs_krffs_type) {
        return 0;
    }
    if (devfs_numbers(m->source, &major, &minor)) {
        return (minor & 0xFFu) | major << 8 | (minor & ~0xFFu) << 12;
    }
    if (m->type == &vfs_devfs_type) {
        return DEVFS_FSID;
    }
    if (m->type == &vfs_procfs_type) {
        return PROCFS_FSID;
    }
    if (m->type == &vfs_sysfs_type) {
        return SYSFS_FSID;
    }
    return 0x10u + (uint32_t)i;
}

/**
 * Render /proc/<pid>/mountinfo, the table util-linux's libmount reads, one
 * line for each mount of /proc/mounts in its order: the mount's id, its
 * parent's, the major and minor numbers of the device stat gives its files,
 * the root of the mount within its file system, its mount point, its
 * options, a hyphen where Linux puts its propagation fields, its type, its
 * source and the file system's options. The id is the entry's index plus
 * one, and the root is its own parent. A bind mount has the device of the
 * mount it shows and the path of its root within that file system. The
 * table is the reader's, whatever process the file is of: it leaves out the
 * mounts the reader's mount namespace does not see (`procfs_mount_listed`).
 */
static int procfs_gen_mountinfo(struct procfs_text *t)
{
    int i, j;

    for (i = 0; i < vfs_mount_slots(); i++) {
        struct vfs_mount *m = vfs_mount_at(i);
        uint32_t dev;
        int parent = 1, base = i;

        if (!procfs_mount_listed(i, m)) {
            continue;
        }
        for (j = 0; j < vfs_mount_slots(); j++) {
            if (i >= 3 && vfs_mount_at(j) == m->parent) {
                parent = j + 1;
            }
            if (vfs_mount_at(j) == vfs_mount_base(m)) {
                base = j;
            }
        }
        dev = procfs_mount_dev(base, vfs_mount_base(m));
        ap_u32(t, (uint32_t)i + 1);
        ap_ch(t, ' ');
        ap_u32(t, i == 0 ? 1 : (uint32_t)parent);
        ap_ch(t, ' ');
        ap_u32(t, (dev >> 8) & 0xfff);
        ap_ch(t, ':');
        ap_u32(t, (dev & 0xff) | ((dev >> 12) & 0xfff00));
        ap_ch(t, ' ');
        ap_str(t, m->bind_of ? m->bind_root : "/");
        ap_ch(t, ' ');
        ap_str(t, m->covered_path);
        ap_ch(t, ' ');
        ap_mount_options(t, i, m);
        ap_str(t, " - ");
        ap_str(t, m->type->name);
        ap_ch(t, ' ');
        ap_str(t, procfs_mount_source(i, m));
        ap_str(t, (m->flags & VFS_MS_RDONLY) ? " ro\n" : " rw\n");
    }
    return t->nomem ? -1 : t->len;
}

/** Append `v` right-aligned in `width` places, in decimal with its sign, or
 * in octal with `octal`, as printf's %*d and %*o give it. */
static void ap_num_width(struct procfs_text *t, int32_t v, int width, int octal)
{
    char tmp[12];
    uint32_t base = octal ? 8u : 10u;
    uint32_t u = !octal && v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
    int n = 0;

    do {
        tmp[n++] = (char)('0' + u % base);
        u /= base;
    } while (u != 0 && n < (int)sizeof(tmp) - 1);
    if (!octal && v < 0) {
        tmp[n++] = '-';
    }
    while (width-- > n) {
        ap_ch(t, ' ');
    }
    while (n > 0) {
        ap_ch(t, tmp[--n]);
    }
}

/** Append `v` as Linux's `%02x` writes it: lowercase, at least two digits. */
static void ap_hex_two(struct procfs_text *t, uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    int shift = 28;

    while (shift > 4 && (v >> shift) == 0) {
        shift -= 4;
    }
    for (; shift >= 0; shift -= 4) {
        ap_ch(t, digits[(v >> shift) & 15u]);
    }
}

/** Append the file of the lock key `key` as Linux's /proc/locks gives it,
 * the major and minor numbers of its mount's device in hexadecimal and its
 * inode number, which stat(2) reports for the file too. */
static void ap_lock_file(struct procfs_text *t, const struct lock_key *key)
{
    char ino[24];
    uint32_t dev = 0;
    int i;

    for (i = 0; key->mnt && i < vfs_mount_slots(); i++) {
        if (vfs_mount_at(i) == key->mnt) {
            dev = procfs_mount_dev(i, vfs_mount_at(i));
        }
    }
    ap_hex_two(t, (dev >> 8) & 0xFFFu);
    ap_ch(t, ':');
    ap_hex_two(t, (dev & 0xFFu) | ((dev >> 12) & 0xFFF00u));
    ap_ch(t, ':');
    diskinfo_u64_dec(ino, (int)sizeof(ino), key->shown);
    ap_str(t, ino);
}

/**
 * Render /proc/locks, which util-linux's lslocks reads: a line for each lock
 * held, the newest first, in Linux's columns. An open file description's
 * record lock is OFDLCK with the process -1, a flock(2) lock covers the
 * whole file, `0 EOF`, and a lock to the end of the file ends at EOF. The
 * waits for a lock are not listed.
 */
static int procfs_gen_locks(struct procfs_text *t)
{
    static const char *const kind[] = {"POSIX  ADVISORY  ", "OFDLCK ADVISORY  ",
                                       "FLOCK  ADVISORY  "};
    struct lock_row *rows;
    char num[24];
    int n = lock_count();
    int i;

    rows = kmalloc(sizeof(*rows) * (size_t)(n ? n : 1));
    if (!rows) {
        return -ENOMEM;
    }
    n = lock_rows(rows, n);
    for (i = 0; i < n; i++) {
        ap_int(t, i + 1);
        ap_str(t, ": ");
        ap_str(t, kind[rows[i].kind]);
        ap_str(t, rows[i].type == F_WRLCK ? "WRITE " : "READ  ");
        ap_int(t, rows[i].pid);
        ap_ch(t, ' ');
        ap_lock_file(t, &rows[i].key);
        diskinfo_u64_dec(num, (int)sizeof(num), rows[i].kind == LOCK_ROW_FLOCK ? 0 : rows[i].start);
        ap_ch(t, ' ');
        ap_str(t, num);
        ap_ch(t, ' ');
        if (rows[i].kind == LOCK_ROW_FLOCK || rows[i].end == LOCK_EOF) {
            ap_str(t, "EOF\n");
        } else {
            diskinfo_u64_dec(num, (int)sizeof(num), rows[i].end);
            ap_str(t, num);
            ap_ch(t, '\n');
        }
    }
    kfree(rows);
    return t->nomem ? -ENOMEM : t->len;
}

/**
 * Render /proc/slabinfo in Linux's version 2.1 format, which slabtop reads:
 * one line for each size class of the kernel heap, named kmalloc-SIZE as
 * Linux names its general caches. A page of the class is a slab of one
 * page: its objects are the blocks the page holds after its header, and the
 * active ones those in use. The tunables and the shared count are 0, since
 * the heap has no per-CPU caches.
 */
static int procfs_gen_slabinfo(struct procfs_text *t)
{
    struct kheap_class c;
    uint32_t cls;
    int width;

    ap_str(t, "slabinfo - version: 2.1\n# name            <active_objs> <num_objs> <objsize> "
              "<objperslab> <pagesperslab> : tunables <limit> <batchcount> <sharedfactor> : "
              "slabdata <active_slabs> <num_slabs> <sharedavail>\n");
    for (cls = 0; kheap_class(cls, &c) == 0; cls++) {
        ap_str(t, "kmalloc-");
        ap_u32(t, c.size);
        for (width = c.size >= 1000 ? 12 : c.size >= 100 ? 11 : 10; width < 18; width++) {
            ap_ch(t, ' ');
        }
        ap_num_width(t, (int32_t)c.live, 6, 0);
        ap_ch(t, ' ');
        ap_num_width(t, (int32_t)(c.pages * c.per_page), 6, 0);
        ap_ch(t, ' ');
        ap_num_width(t, (int32_t)c.size, 6, 0);
        ap_ch(t, ' ');
        ap_num_width(t, (int32_t)c.per_page, 4, 0);
        ap_str(t, "    1 : tunables    0    0    0 : slabdata ");
        ap_num_width(t, (int32_t)c.pages, 6, 0);
        ap_ch(t, ' ');
        ap_num_width(t, (int32_t)c.pages, 6, 0);
        ap_str(t, "      0\n");
    }
    return t->len;
}

/**
 * Render /proc/sysvipc/shm, sem or msg, for `origin`: Linux's heading and a
 * line for each object of that kind, in its columns, from the key, the id
 * and the bits in octal to the times, which ipcs reads. A segment's
 * resident and swapped bytes are its size and 0, since nothing is swapped.
 */
static int procfs_gen_sysvipc(struct procfs_text *t, int origin)
{
    static const char *const heads[] = {
        "       key      shmid perms                  size  cpid  lpid nattch   uid   gid  cuid  "
        "cgid      atime      dtime      ctime                   rss                  swap\n",
        "       key      semid perms      nsems   uid   gid  cuid  cgid      otime      ctime\n",
        "       key      msqid perms      cbytes       qnum lspid lrpid   uid   gid  cuid  cgid    "
        "  "
        "stime      rtime      ctime\n"};
    int kind = origin - SYNTH_PROC_SYSVIPC;
    struct sysv_row r;
    int i, got;

    ap_str(t, heads[kind]);
    for (i = 0; (got = sysv_row_at(kind, i, &r)) >= 0; i++) {
        if (got == 0) {
            continue;
        }
        ap_num_width(t, r.key, 10, 0);
        ap_ch(t, ' ');
        ap_num_width(t, r.id, 10, 0);
        ap_str(t, "  ");
        ap_num_width(t, (int32_t)r.mode, 4, 1);
        if (kind == SYSV_SHM) {
            ap_u32_width(t, r.a, 22);
            ap_u32_width(t, r.b, 6);
            ap_u32_width(t, r.c, 6);
            ap_u32_width(t, r.d, 7);
        } else if (kind == SYSV_SEM) {
            ap_u32_width(t, r.a, 11);
        } else {
            ap_u32_width(t, r.a, 12);
            ap_u32_width(t, r.b, 11);
            ap_u32_width(t, r.c, 6);
            ap_u32_width(t, r.e, 6);
        }
        ap_u32_width(t, r.uid, 6);
        ap_u32_width(t, r.gid, 6);
        ap_u32_width(t, r.cuid, 6);
        ap_u32_width(t, r.cgid, 6);
        ap_u32_width(t, (uint32_t)r.t0, 11);
        ap_u32_width(t, (uint32_t)r.t1, 11);
        if (kind != SYSV_SEM) {
            ap_u32_width(t, (uint32_t)r.t2, 11);
        }
        if (kind == SYSV_SHM) {
            ap_u32_width(t, r.a, 22);
            ap_u32_width(t, 0, 22);
        }
        ap_ch(t, '\n');
    }
    return t->nomem ? -1 : t->len;
}

/** Append one counter of /proc/console: `name value`. */
static void ap_counter(struct procfs_text *t, const char *name, uint32_t v)
{
    ap_str(t, name);
    ap_ch(t, ' ');
    ap_u32(t, v);
    ap_ch(t, '\n');
}

/**
 * Render /proc/console: the serial receive path's counters, from the UART
 * to the readers, so where a typed byte was lost can be read off the
 * machine rather than guessed.
 */
static int procfs_gen_console(struct procfs_text *t)
{
    struct console_counters c;

    tty_console_counters(&c);
    ap_counter(t, "uart-bytes", c.uart_bytes);
    ap_counter(t, "uart-overruns", c.uart_overruns);
    ap_counter(t, "irq-drains", c.irq_drains);
    ap_counter(t, "irq-bytes", c.irq_bytes);
    ap_counter(t, "tick-drains", c.tick_drains);
    ap_counter(t, "reader-drains", c.reader_drains);
    ap_counter(t, "ring-full", c.ring_full);
    ap_counter(t, "ring-max", c.ring_max);
    ap_counter(t, "shell-bytes", c.shell_bytes);
    ap_counter(t, "line-drops", c.ldisc.line_drops);
    ap_counter(t, "queue-drops", c.ldisc.queue_drops);
    ap_counter(t, "flush-bytes", c.ldisc.flush_bytes);
    ap_counter(t, "partial-moved", c.ldisc.partial_moved);
    ap_counter(t, "read-bytes", c.ldisc.read_bytes);
    ap_counter(t, "queue-max", c.ldisc.queue_max);
    ap_counter(t, "line-max", c.ldisc.line_max);
    return t->len;
}

/**
 * Render /proc/interrupts: a line for each vector that has taken an
 * interrupt, with the vector, its count, where it comes from and the names
 * of the devices on it, separated by commas, or "none"; then the count of
 * the APIC's spurious vector, SPU, and FBK, the times the timer's drain of
 * the USB controllers found work with no USB interrupt pending
 * (`usb_fallback_count`): work an interrupt should have announced, or one
 * still in transit.
 */
static int procfs_gen_interrupts(struct procfs_text *t)
{
    int v;

    for (v = ISR_DEVICE_FIRST; v <= ISR_DEVICE_LAST; v++) {
        uint32_t n = isr_vector_count(v);
        int slot;
        int named = 0;

        if (n == 0) {
            continue;
        }
        ap_u32(t, (uint32_t)v);
        ap_str(t, ":\t");
        ap_u32(t, n);
        ap_str(t, "\t");
        ap_str(t, isr_vector_source(v));
        ap_str(t, "\t");
        for (slot = 0; slot < ISR_MAX_VECTOR_HANDLERS; slot++) {
            const char *name = isr_vector_name(v, slot);

            if (name == 0) {
                continue;
            }
            if (named++) {
                ap_str(t, ", ");
            }
            ap_str(t, name);
        }
        if (!named) {
            ap_str(t, "none");
        }
        ap_ch(t, '\n');
    }
    ap_str(t, "SPU:\t");
    ap_u32(t, isr_spurious_count());
    ap_str(t, "\tspurious\n");
    ap_str(t, "FBK:\t");
    ap_u32(t, usb_fallback_count());
    ap_str(t, "\tusb fallback\n");
    return t->nomem ? -1 : t->len;
}

/** Render /proc/loop: a line for each attached device, with its size in
 * bytes, whether it takes writes, and the path it was attached by
 * (`loop_line`). A device nothing is attached to has no line, so the file
 * of a machine with no device attached is empty. */
static int procfs_gen_loop(struct procfs_text *t)
{
    char line[128];
    int i;

    for (i = 0; i < LOOP_DEVICES; i++) {
        if (loop_line(i, line, sizeof(line)) > 0) {
            ap_str(t, line);
        }
    }
    return t->nomem ? -1 : t->len;
}

/** Render /proc/filesystems: devfs and proc, each after `nodev`, then every
 * type mount(2) accepts (`vfs_type_at`), KRFFS volumes among them, with
 * `nodev` before one that stands on no device. The type of the KRFFS root
 * is not among them: the root is an image the kernel maps, which mount(2)
 * does not place. */
static int procfs_gen_filesystems(struct procfs_text *t)
{
    int i;

    ap_str(t, "nodev\tdevfs\n");
    ap_str(t, "nodev\tproc\n");
    for (i = 0; vfs_type_at(i); i++) {
        const struct vfs_type *ty = vfs_type_at(i);

        if (ty) {
            if (ty->nodev) {
                ap_str(t, "nodev");
            }
            ap_ch(t, '\t');
            ap_str(t, ty->name);
            ap_ch(t, '\n');
        }
    }
    return t->nomem ? -1 : t->len;
}

/*
 * The network files, in the forms the programs that read them parse:
 * BusyBox's `route` reads eleven whitespace-separated fields with `fscanf`
 * and skips one header line; its `arp` reads six with `sscanf` and skips one
 * header line; and its `netstat` reads nine and rejects a line that yields
 * fewer. The columns Linux prints after the ninth are there too, for a
 * program that counts the fields of a line.
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
 * Append the IPv4 address `ip`, in host order, as Linux's /proc writes one:
 * the hex of the word that has the address's bytes in network order, read
 * as a little-endian x86 reads it, so 127.0.0.1 is 0100007F. `route` and
 * `netstat` undo it the same way.
 */
static void ap_ip_hex(struct procfs_text *t, uint32_t ip)
{
    ap_hex(t,
           ((ip & 0xFFu) << 24) | ((ip & 0xFF00u) << 8) | ((ip >> 8) & 0xFF00u) |
               ((ip >> 24) & 0xFFu),
           8);
}

/** Append the address `ip`, in host order, as dotted decimal, which
 * /proc/net/arp uses in place of hex. */
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

/** One line of /proc/net/route for `r`: the interface, the destination,
 * the gateway, RTF_UP with RTF_GATEWAY for a route through one, the metric
 * and the mask. */
static void ap_route(struct procfs_text *t, const struct netdev_rt *r)
{
    int i;

    for (i = 0; i < netdev_seen_count(); i++) {
        if (netdev_at(i)->index == r->ifindex) {
            ap_str(t, netdev_at(i)->name);
        }
    }
    ap_ch(t, '\t');
    ap_ip_hex(t, r->dst);
    ap_ch(t, '\t');
    ap_ip_hex(t, r->gw);
    ap_str(t, r->gw ? "\t0003\t0\t0\t" : "\t0001\t0\t0\t");
    ap_u32(t, r->metric);
    ap_ch(t, '\t');
    ap_ip_hex(t, r->mask);
    ap_str(t, "\t0\t0\t0\n");
}

/**
 * Render /proc/net/route: each interface's default route, then the subnets
 * its addresses imply, then the routes programs added, in the columns
 * `route` and BusyBox's `ip` read.
 */
static int procfs_gen_netroute(struct procfs_text *t)
{
    struct netdev_rt r;
    int i, n, kind;

    ap_str(t, PROCFS_NETROUTE_HEADER);
    for (i = 0; i <= netdev_seen_count(); i++) {
        for (kind = NETDEV_RT_DEFAULT; kind >= NETDEV_RT_LINK; kind--) {
            for (n = 0; i < netdev_seen_count() && netdev_route_at(n, &r); n++) {
                if (r.ifindex == netdev_at(i)->index && r.kind == kind) {
                    ap_route(t, &r);
                }
            }
        }
    }
    for (n = 0; net_ns_seen() && netdev_route_at(n, &r); n++) {
        if (r.kind == NETDEV_RT_ADDED) {
            ap_route(t, &r);
        }
    }
    return t->len;
}

/**
 * Render /proc/net/arp: what the cache holds, one line each, in Linux's
 * columns, the address padded to 16 characters and the hardware address in
 * lowercase.
 *
 * The hardware type is 1, Ethernet, and the flag is 2, which is what `arp`
 * prints as a complete entry, or 6 for one set by hand as permanent; an
 * entry this cache holds at all is complete, since a pending one is not
 * stored.
 */
static int procfs_gen_netarp(struct procfs_text *t)
{
    static const char digits[] = "0123456789abcdef";
    static struct arp_row rows[ARP_CACHE_SIZE];
    int n, i, b, start;

    ap_str(t, PROCFS_NETARP_HEADER);
    n = net_ns_seen() ? arp_snapshot(rows, ARP_CACHE_SIZE) : 0;
    for (i = 0; i < n; i++) {
        start = t->len;
        ap_ip_dotted(t, rows[i].ip);
        while (t->len - start < 16 && !t->nomem) {
            ap_ch(t, ' ');
        }
        ap_str(t, rows[i].permanent ? " 0x1         0x6         " : " 0x1         0x2         ");
        for (b = 0; b < 6; b++) {
            if (b) {
                ap_ch(t, ':');
            }
            ap_ch(t, digits[rows[i].mac[b] >> 4]);
            ap_ch(t, digits[rows[i].mac[b] & 0xF]);
        }
        ap_str(t, "     *        ");
        ap_str(t, rows[i].iface);
        ap_ch(t, '\n');
    }
    return t->len;
}

/**
 * Render /proc/net/tcp: the connections, in the columns netstat parses, or
 * none for a caller outside the first network namespace; -ENOMEM when the
 * heap has no room for the snapshot of the connections.
 *
 * The state numbers are Linux's, which this stack's own enumeration does not
 * match, so they are mapped (`tcp_state_to_linux`): a reader prints the name
 * for the number it is given. `uid` and `inode` come from the block rather
 * than from its socket, which is cleared while a connection is still
 * closing. A listener's local address is the one it was bound to, 0.0.0.0
 * for one bound to INADDR_ANY. After Linux's columns each line has this
 * stack's own, as `struct tcp_row` describes them: the peer's window, the
 * congestion window, the bytes in flight and unsent, the two timers, and the
 * transmits the driver refused and for how long.
 */
static int procfs_gen_nettcp(struct procfs_text *t)
{
    struct tcp_row *rows;
    int n = tcp_snapshot(&rows);
    int i;

    if (n < 0) {
        return -ENOMEM;
    }
    if (!net_ns_seen()) {
        n = 0;
    }
    ap_str(t, PROCFS_NETTCP_HEADER);
    for (i = 0; i < n; i++) {
        struct tcp_row row = rows[i];

        ap_str(t, "   ");
        ap_u32(t, (uint32_t)i);
        ap_str(t, ": ");
        ap_ip_hex(t, row.local_ip);
        ap_ch(t, ':');
        ap_hex(t, row.local_port, 4);
        ap_ch(t, ' ');
        ap_ip_hex(t, row.remote_ip);
        ap_ch(t, ':');
        ap_hex(t, row.remote_port, 4);
        ap_ch(t, ' ');
        ap_hex(t, (uint32_t)tcp_state_to_linux(row.state), 2);
        ap_ch(t, ' ');
        ap_hex(t, row.txq, 8);
        ap_ch(t, ':');
        ap_hex(t, row.rxq, 8);
        ap_str(t, " 00:00000000 ");
        ap_hex(t, row.retransmits, 8); /* Linux's retrnsmt column */
        ap_ch(t, ' ');
        ap_u32(t, row.uid);
        ap_str(t, "        0 ");
        ap_u32(t, row.inode);
        ap_str(t, " 1 0000000000000000 100 0 0 10 0 wnd=");
        ap_u32(t, row.snd_wnd);
        ap_str(t, " cwnd=");
        ap_u32(t, row.cwnd);
        ap_str(t, " flight=");
        ap_u32(t, row.in_flight);
        ap_str(t, " unsent=");
        ap_u32(t, row.unsent);
        ap_str(t, " rto=");
        ap_u32(t, row.rto_timer);
        ap_str(t, " zwp=");
        ap_u32(t, row.zwp_timer);
        ap_str(t, " refused=");
        ap_u32(t, row.tx_refused);
        ap_str(t, " for=");
        ap_u32(t, row.refused_for);
        ap_ch(t, '\n');
    }
    kfree(rows);
    return t->len;
}

/**
 * Render /proc/net/udp or, for `type` SOCK_RAW, /proc/net/raw: the sockets in
 * the columns of /proc/net/tcp, which netstat and ss parse the same way.
 *
 * The local address is the one the socket bound, 0.0.0.0 for one bound to
 * INADDR_ANY or not bound at all. The state is Linux's: 1, its
 * TCP_ESTABLISHED, for a connected socket and 7, TCP_CLOSE, for the rest. A
 * raw socket's port is its protocol.
 */
static int procfs_gen_netdgram(struct procfs_text *t, int type)
{
    struct socket_row *rows;
    int n = socket_snapshot(type, &rows);
    int i;

    if (n < 0) {
        return -ENOMEM;
    }
    ap_str(t, PROCFS_NETUDP_HEADER);
    for (i = 0; i < n; i++) {
        uint16_t port = rows[i].local_port;

        if (type == SOCK_RAW && port == 0) {
            port = 255; /* IPPROTO_RAW, which the ioctl descriptor is opened with */
        }
        ap_str(t, i < 10 ? "    " : i < 100 ? "   " : i < 1000 ? "  " : " ");
        ap_u32(t, (uint32_t)i);
        ap_str(t, ": ");
        ap_ip_hex(t, rows[i].local_ip);
        ap_ch(t, ':');
        ap_hex(t, port, 4);
        ap_ch(t, ' ');
        ap_ip_hex(t, rows[i].connected ? rows[i].remote_ip : 0);
        ap_ch(t, ':');
        ap_hex(t, rows[i].connected ? rows[i].remote_port : 0, 4);
        ap_str(t, rows[i].connected ? " 01 00000000:" : " 07 00000000:");
        ap_hex(t, rows[i].rxq, 8);
        ap_str(t, " 00:00000000 00000000 ");
        ap_u32(t, rows[i].uid);
        ap_str(t, "        0 ");
        ap_u32(t, rows[i].inode);
        ap_str(t, " 2 00000000 0\n");
    }
    kfree(rows);
    return t->len;
}

/**
 * Render /proc/net/unix: the AF_UNIX sockets with a descriptor open, in
 * Linux's columns, which netstat reads with `sscanf`.
 *
 * The first column is the socket's kernel address, which Linux hides as
 * zeros from a reader without privilege; here it is zeros for everyone. A
 * listener has the flag `__SO_ACCEPTCON`, 0x10000, and the state is 3,
 * SS_CONNECTED, for a connected socket and 1, SS_UNCONNECTED, for the rest.
 * An abstract name is written with `@` for its leading NUL byte.
 */
static int procfs_gen_netunix(struct procfs_text *t)
{
    struct unix_row *rows;
    int n = unix_snapshot(&rows);
    int i;
    uint32_t k;

    if (n < 0) {
        return -ENOMEM;
    }
    ap_str(t, PROCFS_NETUNIX_HEADER);
    for (i = 0; i < n; i++) {
        ap_str(t, "00000000: ");
        ap_hex(t, rows[i].refs, 8);
        ap_str(t, " 00000000 ");
        ap_hex(t, rows[i].listening ? 0x10000u : 0, 8);
        ap_ch(t, ' ');
        ap_hex(t, (uint32_t)rows[i].type, 4);
        ap_str(t, rows[i].connected ? " 03 " : " 01 ");
        ap_u32(t, rows[i].inode);
        if (rows[i].name_len > 0) {
            int abstract = rows[i].path[0] == '\0';

            ap_ch(t, ' ');
            for (k = 0; k < rows[i].name_len && (abstract || rows[i].path[k] != '\0'); k++) {
                ap_ch(t, rows[i].path[k] != '\0' ? rows[i].path[k] : '@');
            }
        }
        ap_ch(t, '\n');
    }
    kfree(rows);
    return t->len;
}

/**
 * Render /proc/net/sockstat: the sockets in use, counted as Linux counts
 * them, which `ss -s` reads.
 *
 * `mem` is in pages: for TCP the control blocks and their receive rings, and
 * for UDP the datagrams queued to be read. Fragments are dropped on arrival,
 * so none is ever held.
 */
static int procfs_gen_netsockstat(struct procfs_text *t)
{
    struct tcp_counts tc;
    struct socket_row *rows;
    int udp = socket_snapshot(SOCK_DGRAM, &rows);
    uint32_t queued = 0;
    int i;

    if (udp < 0) {
        return -ENOMEM;
    }
    for (i = 0; i < udp; i++) {
        queued += rows[i].rxq;
    }
    kfree(rows);
    tcp_count(&tc);
    ap_str(t, "sockets: used ");
    ap_int(t, socket_count(0) + unix_count());
    ap_str(t, "\nTCP: inuse ");
    ap_int(t, tc.inuse);
    ap_str(t, " orphan ");
    ap_int(t, tc.orphan);
    ap_str(t, " tw ");
    ap_int(t, tc.tw);
    ap_str(t, " alloc ");
    ap_int(t, tc.alloc);
    ap_str(t, " mem ");
    ap_u32(t, (tc.bytes + PAGE_SIZE - 1) / PAGE_SIZE);
    ap_str(t, "\nUDP: inuse ");
    ap_int(t, udp);
    ap_str(t, " mem ");
    ap_u32(t, (queued + PAGE_SIZE - 1) / PAGE_SIZE);
    ap_str(t, "\nUDPLITE: inuse 0\nRAW: inuse ");
    ap_int(t, socket_count(SOCK_RAW));
    ap_str(t, "\nFRAG: inuse 0 memory 0\n");
    return t->len;
}

/** Append a space and `v`, one value of a /proc/net/snmp line. */
static void ap_mib(struct procfs_text *t, uint32_t v)
{
    ap_ch(t, ' ');
    ap_u32(t, v);
}

/**
 * Render /proc/net/snmp: of the MIB counters Linux keeps, the ones this
 * stack has (`net_mib`), a line of names and a line of values for each
 * group, in Linux's order, which `nstat` reads by name.
 *
 * Forwarding 2 is "not a router". The retransmission bounds are Linux's
 * milliseconds; MaxConn is -1, since the number of connections has no fixed
 * limit, and CurrEstab counts ESTABLISHED and CLOSE_WAIT, as Linux does.
 */
static int procfs_gen_netsnmp(struct procfs_text *t)
{
    const struct net_mib *m = &net_mib;
    struct tcp_counts tc;

    tcp_count(&tc);
    ap_str(t, "Ip: Forwarding DefaultTTL InReceives InHdrErrors InAddrErrors InUnknownProtos"
              " InDiscards InDelivers OutRequests OutDiscards OutNoRoutes\nIp: 2");
    ap_mib(t, IP_TTL_DEFAULT);
    ap_mib(t, m->ip_in_receives);
    ap_mib(t, m->ip_in_hdr_errors);
    ap_mib(t, m->ip_in_addr_errors);
    ap_mib(t, m->ip_in_unknown_protos);
    ap_mib(t, m->ip_in_discards);
    ap_mib(t, m->ip_in_delivers);
    ap_mib(t, m->ip_out_requests);
    ap_mib(t, m->ip_out_discards);
    ap_mib(t, m->ip_out_no_routes);
    ap_str(t, "\nIcmp: InMsgs InErrors InDestUnreachs InEchos InEchoReps OutMsgs"
              " OutDestUnreachs OutEchos OutEchoReps\nIcmp:");
    ap_mib(t, m->icmp_in_msgs);
    ap_mib(t, m->icmp_in_errors);
    ap_mib(t, m->icmp_in_dest_unreachs);
    ap_mib(t, m->icmp_in_echos);
    ap_mib(t, m->icmp_in_echo_reps);
    ap_mib(t, m->icmp_out_msgs);
    ap_mib(t, m->icmp_out_dest_unreachs);
    ap_mib(t, m->icmp_out_echos);
    ap_mib(t, m->icmp_out_echo_reps);
    ap_str(t, "\nTcp: RtoAlgorithm RtoMin RtoMax MaxConn ActiveOpens PassiveOpens AttemptFails"
              " EstabResets CurrEstab InSegs OutSegs RetransSegs InErrs OutRsts\nTcp: 1");
    ap_mib(t, TCP_RTO_MIN * 10); /* ticks of 10 ms */
    ap_mib(t, TCP_RTO_MAX * 10);
    ap_str(t, " -1");
    ap_mib(t, m->tcp_active_opens);
    ap_mib(t, m->tcp_passive_opens);
    ap_mib(t, m->tcp_attempt_fails);
    ap_mib(t, m->tcp_estab_resets);
    ap_mib(t, (uint32_t)tc.estab);
    ap_mib(t, m->tcp_in_segs);
    ap_mib(t, m->tcp_out_segs);
    ap_mib(t, m->tcp_retrans_segs);
    ap_mib(t, m->tcp_in_errs);
    ap_mib(t, m->tcp_out_rsts);
    ap_str(t, "\nUdp: InDatagrams NoPorts InErrors OutDatagrams\nUdp:");
    ap_mib(t, m->udp_in_datagrams);
    ap_mib(t, m->udp_no_ports);
    ap_mib(t, m->udp_in_errors);
    ap_mib(t, m->udp_out_datagrams);
    ap_ch(t, '\n');
    return t->len;
}

/** Append `v` as eight lowercase hex digits and `sep`, as /proc/net/stat's
 * files have each counter. */
static void ap_stat(struct procfs_text *t, uint32_t v, const char *sep)
{
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = 7; i >= 0; i--) {
        ap_ch(t, digits[(v >> (i * 4)) & 0xFu]);
    }
    ap_str(t, sep);
}

/**
 * Render /proc/net/stat/rt_cache and arp_cache, which iproute2's lnstat, rtstat
 * among its names, reads: Linux's heading and a line of hex counters for the
 * one processor. Of the route counters, the routes in the table, the datagrams
 * received and those for another host, and the lookups of the output path;
 * the hits are 0, since there is no cache of routes, and the broadcasts are
 * not counted apart. Of the neighbour counters, the mappings held, stored and
 * dropped, the lookups and hits, the resolutions that got no answer and the
 * datagrams dropped waiting for one, and the mappings refused for a full table.
 */
static int procfs_gen_netstat(struct procfs_text *t, int origin)
{
    static struct arp_row rows[ARP_CACHE_SIZE];
    struct netdev_rt r;
    int n;

    if (origin == SYNTH_PROC_NETSTAT_RT) {
        for (n = 0; netdev_route_at(n, &r); n++) {
        }
        ap_str(t, "entries  in_hit   in_slow_tot in_slow_mc in_no_route in_brd   in_martian_dst "
                  "in_martian_src out_hit  out_slow_tot out_slow_mc gc_total gc_ignored "
                  "gc_goal_miss gc_dst_overflow in_hlist_search out_hlist_search\n");
        ap_stat(t, (uint32_t)n, "  ");
        ap_stat(t, 0, " ");
        ap_stat(t, net_mib.ip_in_receives, " ");
        ap_stat(t, 0, " ");
        ap_stat(t, net_mib.ip_in_addr_errors, " ");
        ap_stat(t, 0, " ");
        ap_stat(t, 0, " ");
        ap_stat(t, 0, "  ");
        ap_stat(t, 0, " ");
        ap_stat(t, netdev_route_lookups, " ");
        for (n = 0; n < 6; n++) {
            ap_stat(t, 0, " ");
        }
        ap_stat(t, 0, " \n");
        return t->len;
    }
    ap_str(t, "entries  allocs   destroys hash_grows lookups  hits     res_failed "
              "rcv_probes_mcast rcv_probes_ucast periodic_gc_runs forced_gc_runs "
              "unresolved_discards table_fulls\n");
    ap_stat(t, (uint32_t)arp_snapshot(rows, ARP_CACHE_SIZE), " ");
    ap_stat(t, arp_stats.allocs, " ");
    ap_stat(t, arp_stats.destroys, " ");
    ap_stat(t, 0, "   ");
    ap_stat(t, arp_stats.lookups, " ");
    ap_stat(t, arp_stats.hits, " ");
    ap_stat(t, arp_stats.res_failed, "   ");
    ap_stat(t, 0, " ");
    ap_stat(t, 0, "   ");
    ap_stat(t, 0, " ");
    ap_stat(t, 0, " ");
    ap_stat(t, arp_stats.unres_discards, " ");
    ap_stat(t, arp_stats.table_fulls, "\n");
    return t->len;
}

/** Render the /proc/net file of origin `origin`, one with a name from
 * `synth_net_name`: SYNTH_PROC_NETUDP to SYNTH_PROC_NETSNMP and the two
 * files of /proc/net/stat. */
static int procfs_gen_net(struct procfs_text *t, int origin)
{
    switch (origin) {
    case SYNTH_PROC_NETSTAT_RT:
    case SYNTH_PROC_NETSTAT_ARP:
        return procfs_gen_netstat(t, origin);
    case SYNTH_PROC_NETUDP:
        return procfs_gen_netdgram(t, SOCK_DGRAM);
    case SYNTH_PROC_NETRAW:
        return procfs_gen_netdgram(t, SOCK_RAW);
    case SYNTH_PROC_NETUNIX:
        return procfs_gen_netunix(t);
    case SYNTH_PROC_NETSOCKSTAT:
        return procfs_gen_netsockstat(t);
    default:
        return procfs_gen_netsnmp(t);
    }
}

/* The files of /sys: the uevents of the PCI and USB devices, and the
 * attributes of the interfaces, the disk nodes, the clock, the processors
 * and the memory. */

int procfs_fd_is_sys(struct fd_entry *e)
{
    const struct procfs_file *pf = e ? (const struct procfs_file *)e->priv : 0;

    return pf && pf->origin_file == SYNTH_SYS_UEVENT;
}

uint32_t sysfs_ino(int node, int key, int file)
{
    if (SYSFS_IS_BLOCK(node)) {
        return SYSFS_INO_BLOCK | (uint32_t)(node - SYSFS_BLOCK) << 20 |
               ((uint32_t)key & 0xFFFu) << 8 | (uint32_t)file;
    }
    if (node == SYSFS_NET_IF || node == SYSFS_NET_STATS) {
        return SYSFS_INO_NET | (node == SYSFS_NET_STATS ? 0x800000u : 0u) |
               ((uint32_t)key & 0xFFFFu) << 8 | (uint32_t)file;
    }
    if (node == SYSFS_CPUN) {
        return SYSFS_INO_CPU | ((uint32_t)key & 0xFFFFu) << 8 | ((uint32_t)file & 0xFFu);
    }
    if (node == SYSFS_PCI_DEVICE && file == 2) {
        return SYSFS_INO_PCI_LINK + (uint32_t)key;
    }
    if (node == SYSFS_PCI_DEVICE) {
        return SYSFS_INO_PCI + ((uint32_t)key << 1) + (file ? 1u : 0u);
    }
    if (node == SYSFS_USB_DEVICE && file == 2) {
        return SYSFS_INO_FILE | (uint32_t)key;
    }
    if (node == SYSFS_USB_DEVICE) {
        return SYSFS_INO_USB | (file ? SYSFS_INO_FILE : 0u) | (uint32_t)key;
    }
    return SYSFS_INO_FIXED + (uint32_t)node + ((uint32_t)file << 8);
}

/** The key of USB device `r`, or -1 when a port of it does not fit one. */
static int sysfs_usb_key(const struct usb_sys_row *r)
{
    int key, k;

    if (r->bus < 1 || r->bus > 4 || r->depth < 1 || r->depth > 6 || r->ports[0] > 255) {
        return -1;
    }
    key = (r->bus - 1) << 28 | r->ports[0] << 20;
    for (k = 1; k < r->depth; k++) {
        if (r->ports[k] > 15) {
            return -1;
        }
        key |= r->ports[k] << (4 * (5 - k));
    }
    return key;
}

/** The entry of the USB device `key` in the USB stack's table, with its row
 * in `*r`, or -1 when the device is not there. */
static int sysfs_usb_find(int key, struct usb_sys_row *r)
{
    int i;

    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (usb_core_row(i, r) == 0 && sysfs_usb_key(r) == key) {
            return i;
        }
    }
    return -1;
}

/** Whether the PCI function `key` answers: its vendor is not all ones, and
 * a function past the first is one of a device that has several. */
static int sysfs_pci_present(int key)
{
    uint8_t bus = (uint8_t)(key >> 8), dev = (uint8_t)((key >> 3) & 31);

    return pci_read_config16(bus, dev, (uint8_t)(key & 7), 0x00) != 0xFFFFu &&
           ((key & 7) == 0 || (pci_read_config8(bus, dev, 0, 0x0E) & 0x80) != 0);
}

int sysfs_device_key(int node, int n)
{
    struct usb_sys_row r;
    pci_device_t p;
    int i, key, seen = 0;

    if (node == SYSFS_PCI_DEVICE) {
        return pci_nth(n, &p) ? p.bus << 8 | p.device << 3 | p.function : -1;
    }
    if (node == SYSFS_CPUN) {
        for (i = 0; i < NR_CPUS; i++) {
            if (cpu_table[i].online && seen++ == n) {
                return i;
            }
        }
        return -1;
    }
    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (usb_core_row(i, &r) == 0 && (key = sysfs_usb_key(&r)) >= 0 && seen++ == n) {
            return key;
        }
    }
    return -1;
}

/** The interface whose index is `index`, or NULL. */
static struct netdev *sysfs_netdev(int index)
{
    int i;

    for (i = 0; i < netdev_seen_count(); i++) {
        if (netdev_at(i) && netdev_at(i)->index == index) {
            return netdev_at(i);
        }
    }
    return NULL;
}

int sysfs_block_find(const char *name, int len, int disk)
{
    struct devfs_disk_row row;
    int i, k, got;

    for (i = 0; (got = devfs_disk_row(i, &row)) >= 0; i++) {
        if (got == 0 || (disk < 0 ? row.partno != 0 : row.partno == 0 || row.whole != disk)) {
            continue;
        }
        for (k = 0; k < len && row.name[k] == name[k]; k++) {
        }
        if (k == len && row.name[k] == '\0') {
            return i;
        }
    }
    return -1;
}

int sysfs_block_devno(const char *name, int len)
{
    struct devfs_disk_row row;
    uint32_t major = 0, minor = 0, *at = &major;
    int i, got;

    for (i = 0; i < len; i++) {
        if (name[i] == ':' && at == &major && i > 0) {
            at = &minor;
        } else if (name[i] >= '0' && name[i] <= '9' && *at < 100000u) {
            *at = *at * 10u + (uint32_t)(name[i] - '0');
        } else {
            return -1;
        }
    }
    for (i = 0; at == &minor && (got = devfs_disk_row(i, &row)) >= 0; i++) {
        if (got == 1 && row.major == major && row.minor == minor) {
            return i;
        }
    }
    return -1;
}

void sysfs_block_name(int key, char *out, int cap)
{
    struct devfs_disk_row row;
    int k;

    for (k = 0; devfs_disk_row(key, &row) == 1 && row.name[k] && k < cap - 1; k++) {
        out[k] = row.name[k];
    }
    if (cap > 0) {
        out[k] = '\0';
    }
}

int sysfs_block_whole(int key)
{
    struct devfs_disk_row row;

    return devfs_disk_row(key, &row) == 1 ? row.whole : -1;
}

int sysfs_block_loop(int key)
{
    struct devfs_disk_row row;
    const char *p;
    int index = 0, autoclear;

    if (devfs_disk_row(key, &row) != 1 || row.partno || row.name[0] != 'l' || row.name[1] != 'o' ||
        row.name[2] != 'o' || row.name[3] != 'p' || !row.name[4]) {
        return -1;
    }
    for (p = row.name + 4; *p >= '0' && *p <= '9' && index < 1000; p++) {
        index = index * 10 + (*p - '0');
    }
    return !*p && loop_backing(index, &autoclear) ? index : -1;
}

/** Whether the strings `a` and `b` are the same. */
static int same_word(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return *a == *b;
}

/** Append the attribute `name` of a bound loop device's `loop` directory:
 * the backing file's path, its autoclear flag, and 0 for the offset, the
 * size limit, partition scanning and direct I/O, which this driver does not
 * have. */
static void ap_loopattr(struct procfs_text *t, int key, const char *name)
{
    int autoclear = 0;
    const char *path = loop_backing(sysfs_block_loop(key), &autoclear);

    if (same_word(name, "backing_file")) {
        ap_str(t, path ? path : "");
    } else {
        ap_u32(t, same_word(name, "autoclear") ? (uint32_t)autoclear : 0u);
    }
}

/**
 * Render the attribute `attr` of the /sys directory `node` of the disk node
 * `key`, as Linux's files of the same names give it for lsblk and the
 * programs like it: a disk's or a partition's numbers, size in 512-byte
 * sectors, first sector and number, a disk's range of minor numbers and
 * whether it is a removable stick, its uevent; its queue's sizes, a transfer
 * of at most 128 KiB, no discard and no scheduler; its device's model,
 * serial number and vendor, "USB" or "ATA"; and a loop device's `loop`
 * attributes (`ap_loopattr`). Another attribute of a disk, a partition or
 * a queue is 0, and a device's revision is empty. -ENOENT for no such
 * attribute or disk node.
 */
static int procfs_gen_blockattr(struct procfs_text *t, int node, int key, int attr)
{
    const char *name = sysfs_attr(node, attr);
    struct devfs_disk_row row;

    if (!name || devfs_disk_row(key, &row) != 1) {
        return -ENOENT;
    }
    if (same_word(name, "dev")) {
        ap_u32(t, row.major);
        ap_ch(t, ':');
        ap_u32(t, row.minor);
    } else if (same_word(name, "size")) {
        ap_u32(t, row.sectors > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)row.sectors);
    } else if (same_word(name, "start")) {
        ap_u32(t, (uint32_t)row.start);
    } else if (same_word(name, "partition")) {
        ap_u32(t, (uint32_t)row.partno);
    } else if (same_word(name, "removable")) {
        ap_u32(t, (uint32_t)row.removable);
    } else if (same_word(name, "uevent")) {
        ap_str(t, "MAJOR=");
        ap_u32(t, row.major);
        ap_str(t, "\nMINOR=");
        ap_u32(t, row.minor);
        ap_str(t, "\nDEVNAME=");
        ap_str(t, row.name);
        ap_str(t, row.partno ? "\nDEVTYPE=partition\nPARTN=" : "\nDEVTYPE=disk");
        if (row.partno) {
            ap_u32(t, (uint32_t)row.partno);
        }
    } else if (node == SYSFS_BLOCK_DEV) {
        ap_str(t, same_word(name, "model")    ? row.model
                  : same_word(name, "serial") ? row.serial
                  : same_word(name, "vendor") ? (row.removable ? "USB" : "ATA")
                                              : "");
    } else if (node == SYSFS_BLOCK_LOOP) {
        ap_loopattr(t, key, name);
    } else {
        static const char *const fixed[][2] = {{"range", "16"},
                                               {"ext_range", "256"},
                                               {"capability", "50"},
                                               {"scheduler", "[none]"},
                                               {"zoned", "none"},
                                               {"max_sectors_kb", "128"},
                                               {"nr_requests", "4"},
                                               {"read_ahead_kb", "128"},
                                               {"rotational", "1"},
                                               {"hw_sector_size", "512"},
                                               {"logical_block_size", "512"},
                                               {"physical_block_size", "512"},
                                               {"minimum_io_size", "512"}};
        int i;
        const char *text = "0";

        for (i = 0; i < (int)(sizeof(fixed) / sizeof(fixed[0])); i++) {
            if (same_word(name, fixed[i][0])) {
                text = fixed[i][1];
            }
        }
        ap_str(t, text);
    }
    ap_ch(t, '\n');
    return t->nomem ? -1 : t->len;
}

int sysfs_net_index(const char *name, int len)
{
    int i, k;

    for (i = 0; i < netdev_seen_count(); i++) {
        const struct netdev *nd = netdev_at(i);

        for (k = 0; nd && k < len && k < (int)sizeof(nd->name) && nd->name[k] == name[k]; k++) {
        }
        if (nd && k == len && (k == (int)sizeof(nd->name) || nd->name[k] == '\0')) {
            return nd->index;
        }
    }
    return -1;
}

void sysfs_net_name(int index, char *out, int cap)
{
    const struct netdev *nd = sysfs_netdev(index);
    int k;

    for (k = 0; nd && k < cap - 1 && k < (int)sizeof(nd->name) && nd->name[k] != '\0'; k++) {
        out[k] = nd->name[k];
    }
    if (cap > 0) {
        out[k] = '\0';
    }
}

int sysfs_net_pci_key(int index)
{
    const struct netdev *nd = sysfs_netdev(index);

    return nd ? nd->pci_key : -1;
}

int sysfs_device_exists(int node, int key)
{
    struct usb_sys_row r;
    struct devfs_disk_row row;

    if (SYSFS_IS_BLOCK(node)) {
        return devfs_disk_row(key, &row) == 1 && (node == SYSFS_BLOCK_PART) == (row.partno != 0) &&
               (node != SYSFS_BLOCK_LOOP || sysfs_block_loop(key) >= 0);
    }
    if (node == SYSFS_NET_IF || node == SYSFS_NET_STATS) {
        return sysfs_netdev(key) != NULL;
    }
    if (node == SYSFS_CPUN) {
        return key >= 0 && key < NR_CPUS && cpu_table[key].online;
    }
    return node == SYSFS_PCI_DEVICE ? sysfs_pci_present(key) : sysfs_usb_find(key, &r) >= 0;
}

/** Append `v`, 0 to 99, as two decimal digits. */
static void ap_two(struct procfs_text *t, int32_t v)
{
    ap_ch(t, (char)('0' + v / 10 % 10));
    ap_ch(t, (char)('0' + v % 10));
}

/** Append `v` in hexadecimal, lowercase and without leading zeros, as
 * Linux's `%x` writes it. */
static void ap_hex_lower(struct procfs_text *t, uint32_t v)
{
    static const char digits[] = "0123456789abcdef";
    int shift = 28;

    while (shift > 0 && (v >> shift) == 0) {
        shift -= 4;
    }
    for (; shift >= 0; shift -= 4) {
        ap_ch(t, digits[(v >> shift) & 15u]);
    }
}

/** Append the processors online as Linux lists a set of them, ranges with
 * commas between: 0-3, or 0,2-3. */
static void ap_cpu_list(struct procfs_text *t)
{
    int c = 0, first = 1;

    while (c < NR_CPUS) {
        int end;

        if (!cpu_table[c].online) {
            c++;
            continue;
        }
        for (end = c; end + 1 < NR_CPUS && cpu_table[end + 1].online; end++) {
        }
        if (!first) {
            ap_ch(t, ',');
        }
        ap_u32(t, (uint32_t)c);
        if (end > c) {
            ap_ch(t, '-');
            ap_u32(t, (uint32_t)end);
        }
        first = 0;
        c = end + 1;
    }
}

/**
 * Render the attribute `attr` of /sys/class/rtc/rtc0, of its device's power
 * directory, of /sys/power, of /sys/devices/system/cpu or of
 * /sys/devices/system/memory and its block, `node`: the clock's date and
 * time in UTC and its seconds since 1970, its driver's name, that it set the
 * system's time at boot, and the wake alarm in seconds since 1970, nothing
 * when none is set, as Linux writes them; that the clock may wake the
 * machine; the one sleep state, suspend to idle, by its two names; the
 * highest processor number the kernel takes, NR_CPUS less one, and the
 * processors that may be, are and are online, which are the ones started,
 * as a list of ranges, with none offline; and the one memory block,
 * memory0, the size of all the memory the kernel manages, online and not
 * removable, which lsmem reads. -ENOENT for no such attribute, and -EIO
 * when the clock cannot be read.
 */
static int procfs_gen_sysattr(struct procfs_text *t, int node, int attr)
{
    rtc_time_t now;

    if (!sysfs_attr(node, attr)) {
        return -ENOENT;
    }
    if (node == SYSFS_RTC_POWER) {
        ap_str(t, "enabled\n");
        return t->len;
    }
    if (node == SYSFS_POWER) {
        ap_str(t, attr == 0 ? "[s2idle]\n" : "freeze mem\n");
        return t->len;
    }
    if (node == SYSFS_CPU) {
        if (attr == 0) {
            ap_u32(t, NR_CPUS - 1);
        } else if (attr != 1) {
            ap_cpu_list(t);
        }
        ap_ch(t, '\n');
        return t->len;
    }
    if (node == SYSFS_MEMORY) {
        ap_xlow(t, (unsigned long)frame_total_count(), 1);
        ap_str(t, "000\n"); /* a frame is 0x1000 bytes */
        return t->len;
    }
    if (node == SYSFS_MEMORY0) {
        static const char *const value[] = {"1\n", "00000000\n", "0\n", "online\n"};

        ap_str(t, value[attr]);
        return t->len;
    }
    if (attr == 2 || attr == 1) {
        ap_str(t, attr == 2 ? "rtc_cmos\n" : "1\n");
        return t->len;
    }
    if (attr == 5) {
        if (devfs_rtc_wakealarm() >= 0) {
            ap_u32(t, (uint32_t)devfs_rtc_wakealarm());
            ap_ch(t, '\n');
        }
        return t->len;
    }
    if (rtc_read_clock(&now) != 0) {
        return -EIO;
    }
    if (attr == 0) {
        ap_u32(t, (uint32_t)now.year);
        ap_ch(t, '-');
        ap_two(t, now.month);
        ap_ch(t, '-');
        ap_two(t, now.day);
    } else if (attr == 4) {
        ap_two(t, now.hour);
        ap_ch(t, ':');
        ap_two(t, now.minute);
        ap_ch(t, ':');
        ap_two(t, now.second);
    } else {
        ap_u32(t, (uint32_t)rtc_seconds_of(&now));
    }
    ap_ch(t, '\n');
    return t->len;
}

/** Append the hardware address `mac` as Linux's sysfs writes it, in
 * lowercase. */
static void ap_mac(struct procfs_text *t, const uint8_t *mac)
{
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 6; i++) {
        if (i) {
            ap_ch(t, ':');
        }
        ap_ch(t, digits[mac[i] >> 4]);
        ap_ch(t, digits[mac[i] & 15u]);
    }
}

/**
 * Render the attribute `attr` of /sys/class/net/IF, or of its `statistics`
 * when `node` is SYSFS_NET_STATS, of the interface `index`: what Linux's
 * files of the same names give. The link comes from the card where it can
 * say, and an interface down has no carrier, as on Linux; the MTU is the
 * Ethernet frame's, which the interface ioctls report too. `lo` has Linux's
 * loopback type, flags and MTU, and a broadcast address of zeros. -ENOENT
 * for no such interface or attribute.
 */
static int procfs_gen_netattr(struct procfs_text *t, int node, int index, int attr)
{
    static const uint8_t broadcast[6] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    struct netdev *nd = sysfs_netdev(index);
    struct netdev_counters c;
    struct netdev_link link;
    int known;

    if (!nd || !sysfs_attr(node, attr)) {
        return -ENOENT;
    }
    k_memset(&c, 0, sizeof(c));
    if (nd->ops && nd->ops->counters) {
        nd->ops->counters(nd->dev, &c);
    }
    known = netdev_link(nd, &link);
    if (node == SYSFS_NET_STATS) {
        uint32_t v[10];

        v[0] = c.tx_collisions;
        v[1] = c.rx_multicast;
        v[2] = nd->rx_bytes;
        v[3] = nd->rx_dropped;
        v[4] = nd->rx_errors;
        v[5] = nd->rx_packets;
        v[6] = nd->tx_bytes;
        v[7] = nd->tx_dropped;
        v[8] = c.tx_errors;
        v[9] = nd->tx_packets;
        ap_u32(t, v[attr]);
        ap_ch(t, '\n');
        return t->len;
    }
    switch (attr) {
    case 0:
        ap_mac(t, nd->mac);
        break;
    case 1:
        ap_str(t, "6");
        break;
    case 2:
        ap_mac(t, nd->loopback ? nd->mac : broadcast);
        break;
    case 3:
        ap_str(t, nd->up && netdev_running(nd) ? "1" : "0");
        break;
    case 4:
        ap_str(t, "0x0");
        break;
    case 5:
        ap_str(t, !known || !nd->up ? "unknown" : link.full_duplex ? "full" : "half");
        break;
    case 6:
        ap_str(t, nd->loopback ? "0x9" : nd->up ? "0x1003" : "0x1002");
        break;
    case 7:
    case 8:
        ap_u32(t, (uint32_t)nd->index);
        break;
    case 9:
        ap_u32(t, nd->loopback ? NETDEV_LOOPBACK_MTU : 1500u);
        break;
    case 10:
        ap_str(t, !nd->up ? "down" : !known ? "unknown" : link.up ? "up" : "down");
        break;
    case 11:
        if (known && nd->up && link.up) {
            ap_u32(t, link.mbps);
        } else {
            ap_str(t, "-1");
        }
        break;
    case 12:
        ap_str(t, "1000");
        break;
    case 13:
        ap_u32(t, nd->loopback ? NETDEV_LOOPBACK_TYPE : 1u);
        break;
    default:
        ap_str(t, "INTERFACE=");
        ap_str(t, nd->name);
        ap_str(t, "\nIFINDEX=");
        ap_u32(t, (uint32_t)nd->index);
        break;
    }
    ap_ch(t, '\n');
    return t->len;
}

/** Append `v` in decimal with leading zeros to three digits, as `%03d`. */
static void ap_u32_3(struct procfs_text *t, uint32_t v)
{
    if (v < 100) {
        ap_ch(t, '0');
    }
    if (v < 10) {
        ap_ch(t, '0');
    }
    ap_u32(t, v);
}

/**
 * Render the uevent of the device `key` of `node`, the variables Linux
 * gives a device of that bus that the readers here use.
 *
 * A PCI function has its class, IDs and subsystem IDs, its slot and its
 * module alias. A USB device has its product, the numbers of its bus and
 * of itself, and its type last: `lsusb` takes a uevent whose first line is
 * the type for an interface's, and Linux's first lines, the device node's
 * numbers and name, are left out, since no node of the device exists here.
 * -ENOENT for a device that is not there.
 */
static int procfs_gen_uevent(struct procfs_text *t, int node, int key)
{
    struct usb_sys_row r;
    char name[16];
    uint8_t bus = (uint8_t)(key >> 8), dev = (uint8_t)((key >> 3) & 31), fn = (uint8_t)(key & 7);
    uint32_t cls, ids, sub = 0;

    if (node == SYSFS_USB_DEVICE) {
        if (sysfs_usb_find(key, &r) < 0) {
            return -ENOENT;
        }
        ap_str(t, "PRODUCT=");
        ap_hex_lower(t, r.vendor);
        ap_ch(t, '/');
        ap_hex_lower(t, r.product);
        ap_ch(t, '/');
        ap_hex_lower(t, r.release);
        ap_str(t, "\nBUSNUM=");
        ap_u32_3(t, (uint32_t)r.bus);
        ap_str(t, "\nDEVNUM=");
        ap_u32_3(t, (uint32_t)r.devnum);
        ap_str(t, "\nDEVTYPE=usb_device\n");
        return t->len;
    }
    if (!sysfs_pci_present(key)) {
        return -ENOENT;
    }
    cls = pci_read_config32(bus, dev, fn, 0x08) >> 8;
    ids = pci_read_config32(bus, dev, fn, 0x00);
    if ((pci_read_config8(bus, dev, fn, 0x0E) & 0x7F) == 0) {
        sub = pci_read_config32(bus, dev, fn, 0x2C);
    }
    synth_sys_device_name(SYSFS_PCI_DEVICE, key, name, sizeof(name));
    ap_str(t, "PCI_CLASS=");
    ap_hex(t, cls, cls > 0xFFFFFu ? 6 : cls > 0xFFFFu ? 5 : 4);
    ap_str(t, "\nPCI_ID=");
    ap_hex(t, ids & 0xFFFFu, 4);
    ap_ch(t, ':');
    ap_hex(t, ids >> 16, 4);
    ap_str(t, "\nPCI_SUBSYS_ID=");
    ap_hex(t, sub & 0xFFFFu, 4);
    ap_ch(t, ':');
    ap_hex(t, sub >> 16, 4);
    ap_str(t, "\nPCI_SLOT_NAME=");
    ap_str(t, name);
    ap_str(t, "\nMODALIAS=pci:v");
    ap_hex(t, ids & 0xFFFFu, 8);
    ap_ch(t, 'd');
    ap_hex(t, ids >> 16, 8);
    ap_str(t, "sv");
    ap_hex(t, sub & 0xFFFFu, 8);
    ap_str(t, "sd");
    ap_hex(t, sub >> 16, 8);
    ap_str(t, "bc");
    ap_hex(t, cls >> 16, 2);
    ap_str(t, "sc");
    ap_hex(t, (cls >> 8) & 0xFFu, 2);
    ap_ch(t, 'i');
    ap_hex(t, cls & 0xFFu, 2);
    ap_ch(t, '\n');
    return t->len;
}

/**
 * Render the /sys file at the path `vp`: an attribute of a disk node, of an
 * interface or of a fixed directory, or a device's uevent. `*key` gets the
 * disk node, the interface or the device the file is for, which its
 * descriptor keeps.
 */
static int procfs_gen_sysfile(struct procfs_text *t, const struct vpath *vp, int *key)
{
    if (SYSFS_IS_BLOCK(vp->sysent)) {
        *key = vp->syskey;
        return procfs_gen_blockattr(t, vp->sysent, vp->syskey, vp->procfile);
    }
    if (vp->sysent == SYSFS_NET_IF || vp->sysent == SYSFS_NET_STATS) {
        *key = vp->syskey;
        return procfs_gen_netattr(t, vp->sysent, vp->syskey, vp->procfile);
    }
    if (sysfs_attr(vp->sysent, vp->procfile)) {
        return procfs_gen_sysattr(t, vp->sysent, vp->procfile);
    }
    *key = vp->syskey;
    return procfs_gen_uevent(t, vp->sysent, vp->syskey);
}

/* The operations of a descriptor of /proc or /sys. */

/** Read the snapshot from the description's offset, which moves past what
 * is read: the bytes read, 0 at its end, or -EFAULT. */
static int32_t procfs_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    uint32_t off, avail, n;

    if (!f || e->ofd->offset >= (uint64_t)f->len) {
        return 0;
    }
    off = (uint32_t)e->ofd->offset;
    avail = (uint32_t)f->len - off;
    n = count < avail ? count : avail;
    if (copy_to_user(buf, f->data + off, n) != 0) {
        return -EFAULT;
    }
    e->ofd->offset = off + n;
    return (int32_t)n;
}

/** The write of a snapshot, which takes none: -EBADF. */
static int32_t procfs_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    (void)e;
    (void)buf;
    (void)count;
    return -EBADF;
}

/** POLLIN of `events`, with no wait queue: a read never waits. */
static short procfs_poll(fd_entry_t *e, short events, struct wait_queue **wq)
{
    (void)e;
    if (wq) {
        *wq = NULL;
    }
    return (short)(events & POLLIN);
}

/** -ENOTTY for every request. */
static int32_t procfs_ioctl(fd_entry_t *e, uint32_t request, uintptr_t arg)
{
    (void)e;
    (void)request;
    (void)arg;
    return -ENOTTY;
}

/** Free the descriptor's record and its text, and drop a mem descriptor's
 * hold on its address space; at the description's last close. */
static void procfs_close(fd_entry_t *e)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    if (f) {
        e->priv = NULL;
        mm_unhold(f->mm);
        kfree(f->data);
        kfree(f);
    }
}

static const struct file_ops procfs_file_ops = {procfs_read, procfs_write, procfs_poll,
                                                procfs_ioctl, procfs_close};

/**
 * The address space a /proc/<pid>/mem descriptor may reach, with a user's
 * count taken for the caller, who drops it (`procfs_mm_put`), or NULL: the
 * one the open allowed, while anything still runs in it. The descriptor
 * keeps only its record (`mm_hold`), as Linux's file keeps the mm_struct
 * and a read takes mm_users only while it is not 0: the pages go when the
 * last thread that ran in them ends or executes another program, and a
 * program the process executes, one of other ids among them, is never
 * reached through a descriptor opened before.
 */
static struct mm *procfs_mem_mm(const struct procfs_file *f)
{
    return f->mm && mm_get_live(f->mm) ? f->mm : NULL;
}

/**
 * Read or write `count` bytes of /proc/<pid>/mem at the descriptor's offset,
 * which is the address, through the address space the open allowed
 * (`procfs_mem_mm`), and move the offset past them, as GDB reads and writes
 * the memory of a tracee with pread64 and pwrite64. A write reaches
 * read-only program text, as a tracer's does. A transfer goes no further
 * than a page at a time, and all of a page or none of it is done, so one
 * that meets an address no region allows has done every byte before it:
 * those are returned and the offset moves past them. When none could be,
 * -EIO, -EFAULT for the caller's buffer, or -ENOMEM (`paging_access`); 0
 * once nothing runs in the address space; and -EIO for an offset past the
 * machine's addresses. It has a user's count for the length of the call,
 * which may sleep bringing a page in while the process ends.
 */
static int32_t procfs_mem_access(fd_entry_t *e, char __user *ubuf, uint32_t count, int write)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    struct mm *mm = f ? procfs_mem_mm(f) : NULL;
    uintptr_t addr = (uintptr_t)e->ofd->offset;
    uint32_t done = 0;
    char buf[256];
    int rc = 0;

    if (!mm) {
        return 0;
    }
    if ((uint64_t)addr != e->ofd->offset) {
        return -EIO;
    }
    while (done < count) {
        uint32_t n = count - done < sizeof(buf) ? count - done : (uint32_t)sizeof(buf);

        if (n > PAGE_SIZE - (addr & (PAGE_SIZE - 1u))) {
            n = (uint32_t)(PAGE_SIZE - (addr & (PAGE_SIZE - 1u)));
        }
        if (write) {
            if (copy_from_user(buf, ubuf + done, n) != 0) {
                rc = -EFAULT;
                break;
            }
            rc = paging_access(mm, addr, buf, n, 1);
        } else {
            rc = paging_access(mm, addr, buf, n, 0);
            if (rc == 0 && copy_to_user(ubuf + done, buf, n) != 0) {
                rc = -EFAULT;
            }
        }
        if (rc != 0) {
            break;
        }
        done += n;
        addr += n;
    }
    procfs_mm_put(mm);
    e->ofd->offset = addr;
    return done > 0 ? (int32_t)done : rc;
}

/** The read of a mem descriptor (`procfs_mem_access`). */
static int32_t procfs_mem_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    return procfs_mem_access(e, buf, count, 0);
}

/** The write of a mem descriptor (`procfs_mem_access`). */
static int32_t procfs_mem_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    return procfs_mem_access(e, (char __user *)(uintptr_t)buf, count, 1);
}

static const struct file_ops procfs_mem_ops = {procfs_mem_read, procfs_mem_write, procfs_poll,
                                               procfs_ioctl, procfs_close};

int procfs_is_mem(const fd_entry_t *e)
{
    return e->type == FD_TYPE_PROC && e->priv &&
           ((const struct procfs_file *)e->priv)->origin_file == SYNTH_PROC_MEM;
}

/**
 * Open /proc/<pid>/mem for reading, writing or both: the caller is a thread
 * of the process or privileged, or has the process's real user ID while the
 * process is dumpable and not privileged, as a tracer needs to attach. The
 * descriptor keeps the process's id, `pid` as `procfs_resolve_id` gave it
 * for the path `vp`, a hold on the record of the address space those checks
 * allowed, which keeps none of its memory, and no text; its offset is an
 * address. Returns the descriptor, -ENOENT for no process or address space,
 * -EACCES for an access mode of 3, O_CREAT or a caller refused, -ENOMEM, or
 * the error of allocating the descriptor.
 */
static int procfs_open_mem(const struct vpath *vp, int pid, int flags)
{
    process_t *p = get_process((uint32_t)pid);
    struct procfs_file *f;
    fd_entry_t *e;
    int acc = flags & 3;
    int fd;

    if (!p || !p->mm) {
        return -ENOENT;
    }
    if (acc == 3 || (flags & 0100 /* O_CREAT */)) {
        return -EACCES;
    }
    if (p->tg != current_proc->tg && !proc_privileged(current_proc) &&
        (current_proc->ruid != p->ruid || !p->mm->dumpable || proc_privileged(p))) {
        return -EACCES;
    }
    f = kzalloc(sizeof(*f));
    if (!f) {
        return -ENOMEM;
    }
    f->origin_pid = pid;
    f->origin_file = SYNTH_PROC_MEM;
    f->origin_task = vp->tid != 0 ? procfs_resolve_pid(vp->pid) : 0;
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(f);
        return fd;
    }
    mm_hold(p->mm);
    f->mm = p->mm;
    e = &current_proc->fds->entries[fd];
    e->type = FD_TYPE_PROC;
    e->node = NULL;
    e->sock = NULL;
    e->ops = &procfs_mem_ops;
    e->priv = f;
    e->ofd->offset = 0;
    e->flags = acc == 0 ? FD_FLAG_READ : acc == 1 ? FD_FLAG_WRITE : (FD_FLAG_READ | FD_FLAG_WRITE);
    if (flags & PROCFS_O_CLOEXEC) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    return fd;
}

/** Append `name` and `value` in decimal, one field of /proc/audio. */
static void audio_field(struct procfs_text *t, const char *name, uint32_t value)
{
    ap_str(t, name);
    ap_u32(t, value);
}

/** Render /proc/audio: the audio driver's name and the HDA driver's report
 * (`hda_get_report`), the codec and the widgets it chose, the rates and
 * formats of the last stream it programmed, the converters' capabilities and
 * the connection lists it read. */
static int procfs_gen_audio(struct procfs_text *t)
{
    const hda_report_t *r = hda_get_report();
    int i, j;

    ap_str(t, "driver ");
    ap_str(t, audio_get_driver_name());
    audio_field(t, "\nhda ready ", r->ready);
    audio_field(t, " playing ", r->playing);
    audio_field(t, "\ncodec ", r->codec);
    ap_str(t, " vendor 0x");
    ap_hex(t, r->vendor, 8);
    audio_field(t, " afg ", r->afg);
    audio_field(t, "\nprimary pin ", r->pin);
    audio_field(t, " index ", r->pin_index);
    audio_field(t, " mixer ", r->mixer);
    audio_field(t, " index ", r->mixer_index);
    audio_field(t, " dac ", r->dac);
    audio_field(t, " enabled ", r->primary_enabled);
    audio_field(t, "\nheadphone pin ", r->hp_pin);
    audio_field(t, " index ", r->hp_index);
    audio_field(t, " dac ", r->hp_dac);
    audio_field(t, " enabled ", r->hp_enabled);
    audio_field(t, "\nstream requested ", r->requested_rate);
    audio_field(t, " programmed ", r->programmed_rate);
    ap_str(t, " format 0x");
    ap_hex(t, r->stream_format, 4);
    ap_str(t, " dac-format 0x");
    ap_hex(t, r->dac_format, 4);
    ap_str(t, " hp-format 0x");
    ap_hex(t, r->hp_format, 4);
    ap_ch(t, '\n');
    for (i = 0; i < 2; i++) {
        const hda_caps_report_t *c = &r->caps[i];
        if (!c->nid)
            continue;
        audio_field(t, "caps dac ", c->nid);
        audio_field(t, " source ", c->source);
        ap_str(t, " pcm 0x");
        ap_hex(t, c->pcm, 8);
        ap_str(t, " formats 0x");
        ap_hex(t, c->formats, 8);
        ap_ch(t, '\n');
    }
    for (i = 0; i < 3; i++) {
        const hda_connection_report_t *c = &r->connections[i];
        if (!c->nid)
            continue;
        audio_field(t, "connections nid ", c->nid);
        audio_field(t, " long ", c->long_form);
        ap_str(t, c->count < 0 ? " invalid raw" : " raw");
        for (j = 0; j < c->raw_count; j++) {
            ap_str(t, " 0x");
            ap_hex(t, c->raw[j], c->long_form ? 4 : 2);
        }
        ap_str(t, "\nexpanded");
        for (j = 0; j < c->count; j++)
            audio_field(t, " ", c->nodes[j]);
        ap_ch(t, '\n');
    }
    return t->nomem ? -1 : t->len;
}

/* The range of an OOM score adjustment, Linux's OOM_SCORE_ADJ_MIN and
 * OOM_SCORE_ADJ_MAX. */
#define OOM_SCORE_ADJ_MIN (-1000)
#define OOM_SCORE_ADJ_MAX 1000

int procfs_oom_owner(int pid, uint32_t *uid, uint32_t *gid)
{
    process_t *p = procfs_proc(pid);

    if (!p) {
        return -ENOENT;
    }
    *uid = 0;
    *gid = 0;
    if (p->mm && p->mm->dumpable) {
        *uid = p->euid;
        *gid = p->egid;
    }
    return 0;
}

int procfs_fd_owner(struct fd_entry *e, uint32_t *uid, uint32_t *gid)
{
    const struct procfs_file *f;

    if (!e || e->type != FD_TYPE_PROC || !e->priv) {
        return -1;
    }
    f = (const struct procfs_file *)e->priv;
    if (procfs_global_fd_path(f->origin_file) || synth_net_name(f->origin_file) ||
        f->origin_file == SYNTH_SYS_UEVENT || f->origin_file == SYNTH_PROC_SYSCTL ||
        procfs_oom_owner(f->origin_pid, uid, gid) != 0) {
        return -1;
    }
    return f->origin_file == SYNTH_PROC_OOM_ADJ || procfs_is_idmap(f->origin_file);
}

int procfs_fd_writable(struct fd_entry *e)
{
    const struct procfs_file *f;

    if (!e || e->type != FD_TYPE_PROC || !e->priv) {
        return 0;
    }
    f = (const struct procfs_file *)e->priv;
    if (f->origin_file == SYNTH_SYS_UEVENT) {
        return sysfs_attr_writable(f->origin_task >> 8, f->origin_task & 255);
    }
    return f->origin_file == SYNTH_PROC_SYSCTL && synth_sys_writable(f->origin_pid);
}

/** Whether `c` is a space as Linux's `isspace` has it: a space, a tab, a
 * newline, a vertical tab, a form feed, a carriage return, or the byte 0xa0,
 * the no-break space of Latin-1, whose classes Linux's table has. */
static int oom_space(char c)
{
    return c == ' ' || (c >= '\t' && c <= '\r') || (unsigned char)c == 0xA0;
}

/** The value of the digit `c` in any base up to 16, or 16 for none. */
static uint32_t oom_digit(char c)
{
    if (c >= '0' && c <= '9') {
        return (uint32_t)(c - '0');
    }
    if (c >= 'a' && c <= 'f') {
        return (uint32_t)(c - 'a') + 10u;
    }
    if (c >= 'A' && c <= 'F') {
        return (uint32_t)(c - 'A') + 10u;
    }
    return 16u;
}

/**
 * Read the integer of the NUL-ended text `s` into `*v` as Linux's
 * oom_score_adj reads a write, `kstrtoint` in base 0 of the text with its
 * spaces stripped at both ends: a sign, then a decimal number, a hexadecimal
 * one after `0x` or an octal one after `0`. 0, -EINVAL for text that is no
 * such number, or -ERANGE for a number an int does not hold.
 *
 * The whole text is read before the number is judged, as `kstrtoint` reads
 * it through a conversion wider than an int, which a write's 12 bytes cannot
 * pass: text that is no number is EINVAL however many digits it begins with.
 */
static int oom_parse(const char *s, int32_t *v)
{
    const char *end = s + k_strlen(s);
    uint32_t base = 10, mag = 0, limit = 0x7FFFFFFFu, d;
    int neg = 0, digits = 0, over = 0;

    while (oom_space(*s)) {
        s++;
    }
    while (end > s && oom_space(end[-1])) {
        end--;
    }
    if (*s == '-' || *s == '+') {
        neg = *s == '-';
        s++;
    }
    if (neg) {
        limit = 0x80000000u;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && oom_digit(s[2]) < 16u) {
        base = 16;
        s += 2;
    } else if (s[0] == '0') {
        base = 8;
    }
    for (; s < end; s++, digits++) {
        d = oom_digit(*s);
        if (d >= base) {
            return -EINVAL;
        }
        if (over || mag > (limit - d) / base) {
            over = 1;
        } else {
            mag = mag * base + d;
        }
    }
    if (digits == 0) {
        return -EINVAL;
    }
    if (over) {
        return -ERANGE;
    }
    *v = neg ? (int32_t)(0u - mag) : (int32_t)mag;
    return 0;
}

/**
 * Read /proc/<pid>/oom_score_adj: the adjustment of the process the
 * descriptor was opened on, as it is at this read, and a newline, from the
 * descriptor's offset, which moves past what is read. -ESRCH once the
 * process has been reaped.
 */
static int32_t procfs_oom_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    process_t *p = f ? procfs_proc(f->origin_pid) : NULL;
    uint32_t off = e->ofd->offset > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)e->ofd->offset;
    struct procfs_text t;
    uint32_t n = 0;

    if (!p) {
        return -ESRCH;
    }
    procfs_text_init(&t);
    ap_int(&t, p->tg->oom_score_adj);
    ap_ch(&t, '\n');
    if (t.nomem) {
        kfree(t.buf);
        return -ENOMEM;
    }
    if (off < (uint32_t)t.len) {
        n = (uint32_t)t.len - off < count ? (uint32_t)t.len - off : count;
        if (copy_to_user(buf, t.buf + off, n) != 0) {
            kfree(t.buf);
            return -EFAULT;
        }
    }
    kfree(t.buf);
    e->ofd->offset = off + n;
    return (int32_t)n;
}

/**
 * Write /proc/<pid>/oom_score_adj: the first 12 bytes of `buf` are an
 * integer from -1000 to 1000, which becomes the adjustment of the process's
 * thread group, as `oom_parse` reads it. A value below the group's floor,
 * `oom_score_adj_min`, needs a privileged caller, whose write sets the floor
 * as well, as on Linux, where the privilege is CAP_SYS_RESOURCE. The bytes
 * taken, at most 12, or -EFAULT, -EINVAL for text that is no integer or one
 * out of the range, -ERANGE for one an int does not hold, -ESRCH once the
 * process has been reaped, or -EACCES.
 */
static int32_t procfs_oom_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    char text[13];
    uint32_t n = count < sizeof(text) - 1u ? count : (uint32_t)sizeof(text) - 1u;
    process_t *p;
    int32_t v;
    int rc;

    k_memset(text, 0, sizeof(text));
    if (copy_from_user(text, buf, n) != 0) {
        return -EFAULT;
    }
    rc = oom_parse(text, &v);
    if (rc != 0) {
        return rc;
    }
    if (v < OOM_SCORE_ADJ_MIN || v > OOM_SCORE_ADJ_MAX) {
        return -EINVAL;
    }
    p = f ? procfs_proc(f->origin_pid) : NULL;
    if (!p) {
        return -ESRCH;
    }
    if (v < p->tg->oom_score_adj_min && !proc_privileged(current_proc)) {
        return -EACCES;
    }
    p->tg->oom_score_adj = v;
    if (proc_privileged(current_proc)) {
        p->tg->oom_score_adj_min = v;
    }
    return (int32_t)n;
}

static const struct file_ops procfs_oom_ops = {procfs_oom_read, procfs_oom_write, procfs_poll,
                                               procfs_ioctl, procfs_close};

/** Which of the user namespace's files `file` is, NS_MAP_ (proc/ns.h). */
static int idmap_which(int file)
{
    return file == SYNTH_PROC_GID_MAP     ? NS_MAP_GID
           : file == SYNTH_PROC_SETGROUPS ? NS_MAP_SETGROUPS
                                          : NS_MAP_UID;
}

/** /proc/<pid>/uid_map, gid_map or setgroups, `file`: the maps of the
 * process's user namespace as the reader sees them (`ns_map_text`). */
static int procfs_gen_idmap(struct procfs_text *t, int pid, int file)
{
    process_t *p = procfs_proc(pid);
    char text[NS_EXTENTS * 33 + 1];

    if (!p) {
        return -ENOENT;
    }
    ns_map_text(proc_userns(p), idmap_which(file), current_proc, text, (int)sizeof(text));
    ap_str(t, text);
    return t->len;
}

/** Read a user namespace's file opened for writing as well: its text at
 * this read, from the description's offset; -ESRCH once the process has
 * been reaped. */
static int32_t procfs_idmap_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    uint32_t off = e->ofd->offset > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)e->ofd->offset;
    struct procfs_text t;
    uint32_t n = 0;

    procfs_text_init(&t);
    if (procfs_gen_idmap(&t, f->origin_pid, f->origin_file) < 0 || t.nomem) {
        kfree(t.buf);
        return t.nomem ? -ENOMEM : -ESRCH;
    }
    if (off < (uint32_t)t.len) {
        n = (uint32_t)t.len - off < count ? (uint32_t)t.len - off : count;
        if (copy_to_user(buf, t.buf + off, n) != 0) {
            kfree(t.buf);
            return -EFAULT;
        }
    }
    kfree(t.buf);
    e->ofd->offset = off + n;
    return (int32_t)n;
}

/** Write a user namespace's map or its setgroups, as `ns_map_write` takes
 * it, from at most 256 bytes of `buf`, at offset 0: what `ns_map_write`
 * returns, -EINVAL at another offset, -EFAULT, or -ESRCH once the process
 * has been reaped. */
static int32_t procfs_idmap_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    process_t *p = procfs_proc(f->origin_pid);
    char text[256];
    uint32_t n = count < sizeof(text) ? count : (uint32_t)sizeof(text);

    if (!p) {
        return -ESRCH;
    }
    if (e->ofd->offset != 0) {
        return -EINVAL;
    }
    if (copy_from_user(text, buf, n) != 0) {
        return -EFAULT;
    }
    return ns_map_write(proc_userns(p), idmap_which(f->origin_file), text, n, current_proc);
}

static const struct file_ops procfs_idmap_ops = {procfs_idmap_read, procfs_idmap_write, procfs_poll,
                                                 procfs_ioctl, procfs_close};

/**
 * Open /proc/<pid>/uid_map, gid_map or setgroups, `file`, for writing, or
 * for reading and writing; who may write it is the namespace's rule
 * (`ns_map_write`), checked at the write, which must be at the start.
 * -EACCES for an access mode of 3, and -ENOENT for no such process.
 */
static int procfs_open_idmap(int file, int pid, int flags)
{
    struct procfs_file *f;
    fd_entry_t *e;
    int acc = flags & 3;
    int fd;

    if (acc == 3) {
        return -EACCES;
    }
    if (!procfs_proc(pid)) {
        return -ENOENT;
    }
    f = kzalloc(sizeof(*f));
    if (!f) {
        return -ENOMEM;
    }
    f->origin_pid = pid;
    f->origin_file = file;
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(f);
        return fd;
    }
    e = &current_proc->fds->entries[fd];
    e->type = FD_TYPE_PROC;
    e->node = NULL;
    e->sock = NULL;
    e->ops = &procfs_idmap_ops;
    e->priv = f;
    e->ofd->offset = 0;
    e->flags = acc == 1 ? FD_FLAG_WRITE : (FD_FLAG_READ | FD_FLAG_WRITE);
    if (flags & PROCFS_O_CLOEXEC) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    return fd;
}

/**
 * Open /proc/<pid>/oom_score_adj for reading, writing or both; O_CREAT and
 * O_TRUNC are taken as they are for a file that exists, which is what a
 * shell's redirection asks for. Writing needs a privileged caller or one
 * whose effective user ID owns the file (`procfs_oom_owner`), whose mode is
 * 0644. The descriptor keeps the id of the process, `pid` as
 * `procfs_resolve_id` gave it for the path `vp`, and no text: each read and
 * write reaches the adjustment as it is then.
 */
static int procfs_open_oom(const struct vpath *vp, int pid, int flags)
{
    struct procfs_file *f;
    fd_entry_t *e;
    uint32_t uid, gid;
    int acc = flags & 3;
    int fd;

    if (acc == 3) {
        return -EACCES;
    }
    if (procfs_oom_owner(pid, &uid, &gid) != 0) {
        return -ENOENT;
    }
    if (acc != 0 && !proc_privileged(current_proc) && current_proc->euid != uid) {
        return -EACCES;
    }
    f = kzalloc(sizeof(*f));
    if (!f) {
        return -ENOMEM;
    }
    f->origin_pid = pid;
    f->origin_file = SYNTH_PROC_OOM_ADJ;
    f->origin_task = vp->tid != 0 ? procfs_resolve_pid(vp->pid) : 0;
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(f);
        return fd;
    }
    e = &current_proc->fds->entries[fd];
    e->type = FD_TYPE_PROC;
    e->node = NULL;
    e->sock = NULL;
    e->ops = &procfs_oom_ops;
    e->priv = f;
    e->ofd->offset = 0;
    e->flags = acc == 0 ? FD_FLAG_READ : acc == 1 ? FD_FLAG_WRITE : (FD_FLAG_READ | FD_FLAG_WRITE);
    if (flags & PROCFS_O_CLOEXEC) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    return fd;
}

/** The text a read of the /proc/sys file `entry` gives, appended to `t`. */
static void procfs_sysctl_text(struct procfs_text *t, int entry)
{
    if (entry == SYNTH_SYS_HOSTNAME || entry == SYNTH_SYS_DOMAINNAME) {
        ap_str(t, entry == SYNTH_SYS_HOSTNAME ? uts_hostname() : uts_domainname());
        ap_ch(t, '\n');
    } else {
        ap_str(t, synth_sys_text(entry));
    }
}

/** Read a /proc/sys file opened for writing as well: its value at this
 * read, from the description's offset. */
static int32_t procfs_sysctl_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    uint32_t off = e->ofd->offset > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)e->ofd->offset;
    struct procfs_text t;
    uint32_t n = 0;

    procfs_text_init(&t);
    procfs_sysctl_text(&t, f->origin_pid);
    if (t.nomem) {
        kfree(t.buf);
        return -ENOMEM;
    }
    if (off < (uint32_t)t.len) {
        n = (uint32_t)t.len - off < count ? (uint32_t)t.len - off : count;
        if (copy_to_user(buf, t.buf + off, n) != 0) {
            kfree(t.buf);
            return -EFAULT;
        }
    }
    kfree(t.buf);
    e->ofd->offset = off + n;
    return (int32_t)n;
}

/**
 * Write a /proc/sys file, as Linux's sysctl handlers take one.
 *
 * kernel/hostname and kernel/domainname become the text up to its first
 * newline, at most 64 bytes, as sethostname(2) and setdomainname(2) would
 * set them; the whole count is taken. A write to net/ipv4/ip_forward is an
 * integer, read as `oom_parse` reads one, and 0, its value, is taken; any
 * other value is -EINVAL, since the stack forwards nothing between
 * interfaces, where Linux would begin to. -EFAULT for a buffer the caller
 * cannot read.
 */
static int32_t procfs_sysctl_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    char text[65];
    uint32_t n = count < 64u ? count : 64u;
    uint32_t len = 0;
    int32_t v;
    int rc;

    k_memset(text, 0, sizeof(text));
    if (copy_from_user(text, buf, n) != 0) {
        return -EFAULT;
    }
    if (f->origin_pid == SYNTH_SYS_HOSTNAME || f->origin_pid == SYNTH_SYS_DOMAINNAME) {
        while (len < n && text[len] != '\n' && text[len] != '\0') {
            len++;
        }
        if (f->origin_pid == SYNTH_SYS_HOSTNAME) {
            uts_set_hostname(text, len);
        } else {
            uts_set_domainname(text, len);
        }
        return (int32_t)count;
    }
    text[12] = '\0';
    rc = oom_parse(text, &v);
    if (rc != 0) {
        return rc;
    }
    return v == 0 ? (int32_t)count : -EINVAL;
}

static const struct file_ops procfs_sysctl_ops = {procfs_sysctl_read, procfs_sysctl_write,
                                                  procfs_poll, procfs_ioctl, procfs_close};

/** Read a writable /sys attribute opened for writing as well: its value at
 * this read, from the description's offset. */
static int32_t procfs_sysattr_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    uint32_t off = e->ofd->offset > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)e->ofd->offset;
    struct procfs_text t;
    uint32_t n = 0;
    int len;

    procfs_text_init(&t);
    len = procfs_gen_sysattr(&t, f->origin_task >> 8, f->origin_task & 255);
    if (len < 0 || t.nomem) {
        kfree(t.buf);
        return len < 0 ? len : -ENOMEM;
    }
    if (off < (uint32_t)t.len) {
        n = (uint32_t)t.len - off < count ? (uint32_t)t.len - off : count;
        if (copy_to_user(buf, t.buf + off, n) != 0) {
            kfree(t.buf);
            return -EFAULT;
        }
    }
    kfree(t.buf);
    e->ofd->offset = off + n;
    return (int32_t)n;
}

/** Whether the `len` bytes at `s` are the word `word`. */
static int sysattr_is(const char *s, uint32_t len, const char *word)
{
    uint32_t i;

    for (i = 0; i < len && word[i] == s[i]; i++) {
    }
    return i == len && word[i] == '\0';
}

/** The decimal number at the start of the `len` bytes at `s`, with a
 * leading `+` noted in `*relative`; -1 when no digit begins them. Reading
 * stops at the first other byte, or once the value reaches 2^31 - 1. */
static int64_t sysattr_number(const char *s, uint32_t len, int *relative)
{
    int64_t v = 0;
    uint32_t i = 0;

    *relative = len > 0 && s[0] == '+';
    i = (uint32_t)*relative;
    if (i == len || s[i] < '0' || s[i] > '9') {
        return -1;
    }
    for (; i < len && s[i] >= '0' && s[i] <= '9' && v < 0x7FFFFFFF; i++) {
        v = v * 10 + (s[i] - '0');
    }
    return v;
}

/**
 * Write a writable /sys attribute, as Linux's do. rtc0's wakealarm takes
 * seconds since 1970, or `+N` for N seconds from now, and 0 to clear it; a
 * new time while an alarm is set is -EBUSY, as on Linux, which wants the
 * alarm cleared first, and a time `devfs_rtc_set_wakealarm` does not set
 * has its error; text that is no number is -EINVAL.
 * /sys/power/state takes `freeze` or `mem`, both suspend to idle, which the
 * write waits in until a wake event, or has the error of `suspend_to_idle`;
 * another state is -EINVAL. The whole count is taken, or the write fails
 * with -EFAULT, -EIO for a clock that cannot be read or cleared, or one of
 * the errors above.
 */
static int32_t procfs_sysattr_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    char text[32];
    uint32_t n = count < sizeof(text) - 1 ? count : (uint32_t)sizeof(text) - 1;
    rtc_time_t now;
    int64_t v;
    int relative;
    int32_t rc;

    k_memset(text, 0, sizeof(text));
    if (copy_from_user(text, buf, n) != 0) {
        return -EFAULT;
    }
    while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == ' ')) {
        text[--n] = '\0';
    }
    if (f->origin_task >> 8 == SYSFS_POWER) {
        if (!sysattr_is(text, n, "freeze") && !sysattr_is(text, n, "mem")) {
            return -EINVAL;
        }
        rc = suspend_to_idle();
        return rc != 0 ? rc : (int32_t)count;
    }
    v = sysattr_number(text, n, &relative);
    if (v < 0) {
        return -EINVAL;
    }
    if (v == 0) {
        return devfs_rtc_set_wakealarm(-1) == 0 ? (int32_t)count : -EIO;
    }
    if (devfs_rtc_wakealarm() >= 0) {
        return -EBUSY;
    }
    if (relative) {
        if (rtc_read_clock(&now) != 0) {
            return -EIO;
        }
        v += rtc_seconds_of(&now);
    }
    rc = devfs_rtc_set_wakealarm(v);
    return rc != 0 ? rc : (int32_t)count;
}

static const struct file_ops procfs_sysattr_ops = {procfs_sysattr_read, procfs_sysattr_write,
                                                   procfs_poll, procfs_ioctl, procfs_close};

/**
 * Open the writable attribute `attr` of the /sys directory `node` for
 * writing, or for reading and writing, which needs a privileged caller, as
 * its mode, 0644 and owned by root, says. Each read and write reaches the
 * value as it is then. The descriptor is a sysfs one, with the node and the
 * attribute in `origin_task`, as a snapshot of a /sys file has them.
 */
static int procfs_open_sysattr(int node, int attr, int flags)
{
    struct procfs_file *f;
    fd_entry_t *e;
    int acc = flags & 3;
    int fd;

    if (acc == 3 || !proc_privileged(current_proc)) {
        return -EACCES;
    }
    f = kzalloc(sizeof(*f));
    if (!f) {
        return -ENOMEM;
    }
    f->origin_file = SYNTH_SYS_UEVENT;
    f->origin_task = node << 8 | attr;
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(f);
        return fd;
    }
    e = &current_proc->fds->entries[fd];
    e->type = FD_TYPE_PROC;
    e->node = NULL;
    e->sock = NULL;
    e->ops = &procfs_sysattr_ops;
    e->priv = f;
    e->ofd->offset = 0;
    e->flags = acc == 1 ? FD_FLAG_WRITE : (FD_FLAG_READ | FD_FLAG_WRITE);
    if (flags & PROCFS_O_CLOEXEC) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    return fd;
}

/**
 * Open a writable /proc/sys file for writing, or for reading and writing, which
 * needs a privileged caller, as the file's mode, 0644 and owned by root, says;
 * the names of the UTS namespace need privilege over it. O_CREAT and O_TRUNC
 * are taken as they are for a file that exists, which is what a shell's
 * redirection and BusyBox's `sysctl -w` ask for. Each read and write reaches
 * the value as it is then.
 */
static int procfs_open_sysctl(int entry, int flags)
{
    struct procfs_file *f;
    fd_entry_t *e;
    int acc = flags & 3;
    int fd;

    if (acc == 3 || !((entry == SYNTH_SYS_HOSTNAME || entry == SYNTH_SYS_DOMAINNAME)
                          ? uts_privileged()
                          : proc_privileged(current_proc))) {
        return -EACCES;
    }
    f = kzalloc(sizeof(*f));
    if (!f) {
        return -ENOMEM;
    }
    f->origin_pid = entry;
    f->origin_file = SYNTH_PROC_SYSCTL;
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(f);
        return fd;
    }
    e = &current_proc->fds->entries[fd];
    e->type = FD_TYPE_PROC;
    e->node = NULL;
    e->sock = NULL;
    e->ops = &procfs_sysctl_ops;
    e->priv = f;
    e->ofd->offset = 0;
    e->flags = acc == 1 ? FD_FLAG_WRITE : (FD_FLAG_READ | FD_FLAG_WRITE);
    if (flags & PROCFS_O_CLOEXEC) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    return fd;
}

/**
 * The text of /proc/PID/fdinfo/N, descriptor `fd` of process `pid`: its
 * offset, and its flags in octal, the access mode with O_APPEND,
 * O_NONBLOCK, O_PATH and O_CLOEXEC, the first two lines Linux gives. Its
 * length, or -ENOENT for a descriptor that is not open.
 */
static int procfs_gen_fdinfo(struct procfs_text *t, int pid, int fd)
{
    process_t *p = get_process((uint32_t)pid);
    const fd_entry_t *e;
    char pos[24];
    uint32_t rw, flags;

    if (!p || p->state == PROC_UNUSED || fd < 0 || fd >= fd_table_cap(p->fds)) {
        return -ENOENT;
    }
    e = &p->fds->entries[fd];
    if (e->type == FD_TYPE_NONE || (e->flags & FD_FLAG_CLOSING)) {
        return -ENOENT;
    }
    rw = e->flags & (FD_FLAG_READ | FD_FLAG_WRITE);
    flags = rw == (FD_FLAG_READ | FD_FLAG_WRITE) ? 2u : (rw == FD_FLAG_WRITE ? 1u : 0u);
    if (e->ofd) {
        flags |= e->ofd->status_flags & (OFD_APPEND | OFD_NONBLOCK);
    }
    if (e->flags & FD_FLAG_PATH) {
        flags |= 010000000u;
    }
    if (e->flags & FD_FLAG_CLOEXEC) {
        flags |= 02000000u;
    }
    diskinfo_u64_dec(pos, (int)sizeof(pos), e->ofd ? e->ofd->offset : 0);
    ap_str(t, "pos:\t");
    ap_str(t, pos);
    ap_str(t, "\nflags:\t0");
    ap_num_width(t, (int32_t)flags, 0, 1);
    ap_ch(t, '\n');
    return t->nomem ? -1 : t->len;
}

/** Whether a file of kind `kind` is one of a process, whose process must be
 * there at the open; 0 for the files of /proc outside /proc/<pid> and for
 * those of /sys. */
static int procfs_needs_pid(int kind)
{
    static const int global[] = {
        VNODE_PROC_CMDLINE,   VNODE_PROC_DISK,       VNODE_PROC_NET_DEV,    VNODE_PROC_UPTIME,
        VNODE_PROC_STAT_SYS,  VNODE_PROC_MEMINFO,    VNODE_PROC_CPUINFO,    VNODE_PROC_LOADAVG,
        VNODE_PROC_DISKSTATS, VNODE_PROC_VERSION,    VNODE_PROC_PARTITIONS, VNODE_PROC_VMSTAT,
        VNODE_PROC_SLABINFO,  VNODE_PROC_SWAPS,      VNODE_PROC_LOCKS,      VNODE_PROC_SYS_FILE,
        VNODE_PROC_MOUNTS,    VNODE_PROC_USB,        VNODE_PROC_AUDIO,      VNODE_PROC_FILESYSTEMS,
        VNODE_PROC_LOOP,      VNODE_PROC_INTERRUPTS, VNODE_PROC_CONSOLE,    VNODE_PROC_NET_ROUTE,
        VNODE_PROC_NET_ARP,   VNODE_PROC_NET_TCP,    VNODE_PROC_NET_FILE,   VNODE_SYS_FILE,
        VNODE_PROC_SYSVIPC};
    uint32_t i;

    for (i = 0; i < sizeof(global) / sizeof(global[0]); i++) {
        if (kind == global[i]) {
            return 0;
        }
    }
    return 1;
}

/**
 * The checks of the flags are in the order that gives the errno of a
 * regular file that exists: O_CREAT with O_EXCL is -EEXIST first, then
 * O_DIRECTORY -ENOTDIR. The files a write reaches then go to opens of their
 * own (`procfs_open_oom`, `procfs_open_idmap`, `procfs_open_mem`,
 * `procfs_open_sysctl`, `procfs_open_sysattr`), and any other write or
 * create is -EACCES.
 *
 * `/proc/disk` is rendered from the record taken at boot, so opening it
 * issues no command to the device. `diskinfo_format` truncates to the size
 * it is given, so a text that reached it is refused with -EOVERFLOW.
 */
int procfs_open(const struct vpath *vp, int flags)
{
    int pid = 0;
    struct procfs_file *f = NULL;
    struct procfs_text text;
    fd_entry_t *e;
    int fd, len;

    if (procfs_needs_pid(vp->kind)) {
        pid = procfs_resolve_id(vp);
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
    if (vp->kind == VNODE_PROC_FILE && vp->procfile == SYNTH_PROC_OOM_ADJ) {
        return procfs_open_oom(vp, pid, flags);
    }
    if (vp->kind == VNODE_PROC_FILE && procfs_is_idmap(vp->procfile) && (flags & 3) != 0) {
        return procfs_open_idmap(vp->procfile, pid, flags);
    }
    if (vp->kind == VNODE_PROC_FILE && vp->procfile == SYNTH_PROC_MEM) {
        return procfs_open_mem(vp, pid, flags);
    }
    if (vp->kind == VNODE_PROC_SYS_FILE && synth_sys_writable(vp->sysent) && (flags & 3) != 0) {
        return procfs_open_sysctl(vp->sysent, flags);
    }
    if (vp->kind == VNODE_SYS_FILE && sysfs_attr_writable(vp->sysent, vp->procfile) &&
        (flags & 3) != 0) {
        return procfs_open_sysattr(vp->sysent, vp->procfile, flags);
    }
    if ((flags & 3) != 0 || (flags & 0100 /* O_CREAT */)) {
        return -EACCES;
    }

    f = kzalloc(sizeof(*f));
    if (!f) {
        return -ENOMEM;
    }
    procfs_text_init(&text);
    if (vp->kind == VNODE_PROC_CMDLINE) {
        /* The boot line and a newline, as Linux's /proc/cmdline has it. */
        const char *line = kcmdline_get();
        ap_str(&text, line);
        ap_ch(&text, '\n');
        len = text.len;
    } else if (vp->kind == VNODE_PROC_DISK) {
        len = 0;
        if (procfs_text_room(&text, PROCFS_MAX_DISK) == 0) {
            len = diskinfo_format(diskinfo_get(), text.buf, PROCFS_MAX_DISK);
            len += diskinfo_format_disks(text.buf + len, PROCFS_MAX_DISK - len);
            if (len >= PROCFS_MAX_DISK - 1) {
                kfree(text.buf);
                kfree(f);
                klog(KLOG_ERR, "procfs", "/proc/disk: text exceeds its %d-byte bound, refusing",
                     PROCFS_MAX_DISK);
                return -EOVERFLOW;
            }
        }
    } else if (vp->kind == VNODE_PROC_FDINFO) {
        len = procfs_gen_fdinfo(&text, pid, vp->procfd);
    } else if (vp->kind == VNODE_PROC_NET_DEV) {
        len = procfs_gen_netdev(&text);
    } else if (vp->kind == VNODE_PROC_UPTIME) {
        len = procfs_gen_uptime(&text);
    } else if (vp->kind == VNODE_PROC_STAT_SYS) {
        len = procfs_gen_statsys(&text);
    } else if (vp->kind == VNODE_PROC_MEMINFO) {
        len = procfs_gen_meminfo(&text);
    } else if (vp->kind == VNODE_PROC_LOADAVG) {
        len = procfs_gen_loadavg(&text);
    } else if (vp->kind == VNODE_PROC_DISKSTATS) {
        len = procfs_gen_diskstats(&text);
    } else if (vp->kind == VNODE_PROC_PARTITIONS) {
        len = procfs_gen_partitions(&text);
    } else if (vp->kind == VNODE_PROC_VMSTAT) {
        len = procfs_gen_vmstat(&text);
    } else if (vp->kind == VNODE_PROC_SLABINFO) {
        len = procfs_gen_slabinfo(&text);
    } else if (vp->kind == VNODE_PROC_LOCKS) {
        len = procfs_gen_locks(&text);
    } else if (vp->kind == VNODE_PROC_SWAPS) {
        /* Linux's header, and no area under it, there being no swap. */
        ap_str(&text, "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n");
        len = text.len;
    } else if (vp->kind == VNODE_PROC_VERSION) {
        /* Linux's "Linux version", then uname's release and version. */
        ap_str(&text, UTS_SYSNAME " version " UTS_RELEASE " " UTS_VERSION "\n");
        len = text.len;
    } else if (vp->kind == VNODE_PROC_CPUINFO) {
        len = procfs_gen_cpuinfo(&text);
    } else if (vp->kind == VNODE_PROC_SYS_FILE) {
        procfs_sysctl_text(&text, vp->sysent);
        len = text.len;
        pid = vp->sysent;
    } else if (vp->kind == VNODE_PROC_MOUNTS) {
        len = procfs_gen_mounts(&text);
    } else if (vp->kind == VNODE_PROC_AUDIO) {
        len = procfs_gen_audio(&text);
    } else if (vp->kind == VNODE_PROC_USB) {
        len = procfs_gen_usb(&text);
    } else if (vp->kind == VNODE_PROC_LOOP) {
        len = procfs_gen_loop(&text);
    } else if (vp->kind == VNODE_PROC_INTERRUPTS) {
        len = procfs_gen_interrupts(&text);
    } else if (vp->kind == VNODE_PROC_CONSOLE) {
        len = procfs_gen_console(&text);
    } else if (vp->kind == VNODE_PROC_FILESYSTEMS) {
        len = procfs_gen_filesystems(&text);
    } else if (vp->kind == VNODE_PROC_NET_ROUTE) {
        len = procfs_gen_netroute(&text);
    } else if (vp->kind == VNODE_PROC_NET_ARP) {
        len = procfs_gen_netarp(&text);
    } else if (vp->kind == VNODE_PROC_NET_TCP) {
        len = procfs_gen_nettcp(&text);
    } else if (vp->kind == VNODE_PROC_NET_FILE) {
        len = procfs_gen_net(&text, vp->procfile);
    } else if (vp->kind == VNODE_PROC_SYSVIPC) {
        len = procfs_gen_sysvipc(&text, vp->procfile);
    } else if (vp->kind == VNODE_SYS_FILE) {
        len = procfs_gen_sysfile(&text, vp, &pid);
    } else if (vp->procfile == SYNTH_PROC_STAT) {
        len = procfs_gen_stat(&text, pid, vp->tid == 0);
    } else if (vp->procfile == SYNTH_PROC_MAPS) {
        len = procfs_gen_maps(&text, pid);
    } else if (vp->procfile == SYNTH_PROC_SMAPS) {
        len = procfs_gen_smaps(&text, pid);
    } else if (vp->procfile == SYNTH_PROC_STATUS) {
        len = procfs_gen_status(&text, pid);
    } else if (vp->procfile == SYNTH_PROC_STATM) {
        len = procfs_gen_statm(&text, pid);
    } else if (vp->procfile == SYNTH_PROC_AUXV) {
        len = procfs_gen_auxv(&text, pid);
    } else if (vp->procfile == SYNTH_PROC_ENVIRON) {
        len = procfs_gen_environ(&text, pid);
    } else if (vp->procfile == SYNTH_PROC_PIDMOUNTS) {
        len = procfs_gen_mounts(&text);
    } else if (vp->procfile == SYNTH_PROC_MOUNTINFO) {
        len = procfs_gen_mountinfo(&text);
    } else if (vp->procfile == SYNTH_PROC_COMM) {
        len = procfs_gen_comm(&text, pid);
    } else if (vp->procfile == SYNTH_PROC_LIMITS) {
        len = procfs_gen_limits(&text, pid);
    } else if (procfs_is_idmap(vp->procfile)) {
        len = procfs_gen_idmap(&text, pid, vp->procfile);
    } else {
        len = procfs_gen_cmdline(&text, pid);
    }
    /* -1 from a generator that ran out of heap is not -EPERM. */
    if (len < 0 && !text.nomem) {
        kfree(text.buf);
        kfree(f);
        return len;
    }
    if (text.nomem) {
        kfree(text.buf);
        kfree(f);
        klog(KLOG_ERR, "procfs", "%s: no memory for the text", procfs_kind_name(vp));
        return -ENOMEM;
    }
    f->data = text.buf;
    f->len = len;
    f->origin_pid = pid;
    f->origin_task = vp->tid != 0 ? procfs_resolve_pid(vp->pid) : 0;
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
    } else if (vp->kind == VNODE_PROC_CPUINFO) {
        f->origin_file = SYNTH_PROC_CPUINFO;
    } else if (vp->kind == VNODE_PROC_LOADAVG) {
        f->origin_file = SYNTH_PROC_LOADAVG;
    } else if (vp->kind == VNODE_PROC_DISKSTATS) {
        f->origin_file = SYNTH_PROC_DISKSTATS;
    } else if (vp->kind == VNODE_PROC_VERSION) {
        f->origin_file = SYNTH_PROC_VERSION;
    } else if (vp->kind == VNODE_PROC_PARTITIONS) {
        f->origin_file = SYNTH_PROC_PARTITIONS;
    } else if (vp->kind == VNODE_PROC_VMSTAT) {
        f->origin_file = SYNTH_PROC_VMSTAT;
    } else if (vp->kind == VNODE_PROC_SLABINFO) {
        f->origin_file = SYNTH_PROC_SLABINFO;
    } else if (vp->kind == VNODE_PROC_SWAPS) {
        f->origin_file = SYNTH_PROC_SWAPS;
    } else if (vp->kind == VNODE_PROC_LOCKS) {
        f->origin_file = SYNTH_PROC_LOCKS;
    } else if (vp->kind == VNODE_PROC_SYS_FILE) {
        f->origin_file = SYNTH_PROC_SYSCTL;
    } else if (vp->kind == VNODE_PROC_MOUNTS) {
        f->origin_file = SYNTH_PROC_MOUNTS;
    } else if (vp->kind == VNODE_PROC_AUDIO) {
        f->origin_file = SYNTH_PROC_AUDIO;
    } else if (vp->kind == VNODE_PROC_USB) {
        f->origin_file = SYNTH_PROC_USB;
    } else if (vp->kind == VNODE_PROC_INTERRUPTS) {
        f->origin_file = SYNTH_PROC_INTERRUPTS;
    } else if (vp->kind == VNODE_PROC_CONSOLE) {
        f->origin_file = SYNTH_PROC_CONSOLE;
    } else if (vp->kind == VNODE_PROC_FILESYSTEMS) {
        f->origin_file = SYNTH_PROC_FILESYSTEMS;
    } else if (vp->kind == VNODE_PROC_LOOP) {
        f->origin_file = SYNTH_PROC_LOOP;
    } else if (vp->kind == VNODE_PROC_NET_ROUTE) {
        f->origin_file = SYNTH_PROC_NETROUTE;
    } else if (vp->kind == VNODE_PROC_NET_ARP) {
        f->origin_file = SYNTH_PROC_NETARP;
    } else if (vp->kind == VNODE_PROC_NET_TCP) {
        f->origin_file = SYNTH_PROC_NETTCP;
    } else if (vp->kind == VNODE_SYS_FILE) {
        f->origin_file = SYNTH_SYS_UEVENT;
        f->origin_task = vp->sysent << 8 | vp->procfile;
    } else {
        f->origin_file = vp->procfile;
    }

    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(f->data);
        kfree(f);
        return fd;
    }
    e = &current_proc->fds->entries[fd];
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
