/*
 * procfs.c: the synthetic /proc, generated from the process table.
 *
 * A /proc text file is snapshotted into a buffer from the heap when opened and
 * served from there (so a read sees a consistent moment, and dup/fork share the
 * snapshot until the last descriptor closes; the per-open-file-description
 * lifetime). The text is built with append helpers that grow the buffer and
 * hand-rolled integer formatting (the kernel has no snprintf and avoids 64-bit
 * division).
 */

#include "../klog.h"
#include "../isr.h"
#include "../lapic.h"
#include "procfs.h"

#include "../latency.h"
#include "../network/ip/ip.h"
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
#include "../mm/mm.h"
#include "../mm/paging.h"
#include "../io/poll.h"
#include "../cmdline.h"
#include "../block/diskinfo.h"
#include "vfs.h"
#include "tmpfs.h"
#include "cpuinfo.h"
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
#define PROCFS_INO_NET         0x120u /* and the other files of /proc/net after it */

/* Inode namespace for /sys: the fixed directories by their node; a PCI
 * function's directory by its key, doubled, with its uevent one more, and
 * its link by its key in a range of its own; and a USB device's directory
 * by its key with the top bit set, its uevent with the next bit too, and
 * its link with that bit alone. The keys are below 2^30 (synthfs.h). */
#define SYSFS_INO_FIXED     0x100u
#define SYSFS_INO_PCI       0x01000000u
#define SYSFS_INO_PCI_LINK  0x02000000u
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
    if (vp->kind == VNODE_SYS_FILE) {
        return "/sys";
    }
    if (vp->procfile == SYNTH_PROC_STAT) {
        return "/proc/<pid>/stat";
    }
    return "/proc/<pid>/cmdline";
}

/* A /proc text file, snapshotted at open and served through file_ops. */
struct procfs_file {
    char *data; /* the text, from the heap, freed at the last close */
    int len;
    int origin_pid;  /* the /proc/<pid> this snapshot came from (for fd readlink) */
    int origin_file; /* SYNTH_PROC_STAT or SYNTH_PROC_CMDLINE */
    int origin_task; /* the process whose task/<origin_pid> it came from, or 0 */
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

static void procfs_text_init(struct procfs_text *t)
{
    t->buf = NULL;
    t->cap = 0;
    t->len = 0;
    t->nomem = 0;
}

/* Room for `want` bytes and a NUL; -1 when the heap has none. */
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

/* text builders (no snprintf; the buffer keeps room for a NUL) */

static void ap_ch(struct procfs_text *t, char c)
{
    if (t->len + 1 >= t->cap && procfs_text_room(t, t->len + 1) != 0) {
        return;
    }
    t->buf[t->len] = c;
    t->len++;
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
        return current_proc ? (int)current_proc->tgid : -1;
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

/**
 * Resolve the ids of the /proc path `vp` to the process or thread whose
 * files it shows, or -1. A path under /proc/<pid>/task/<tid> names both,
 * and needs both: the process <pid>, and the thread <tid> among the threads
 * its task directory lists, which is what the path shows. Any other path
 * is its pid's (`procfs_resolve_pid`).
 */
int procfs_resolve_id(const struct vpath *vp)
{
    int pid = procfs_resolve_pid(vp->pid);
    process_t *p;
    process_t *t;

    if (pid < 0 || vp->tid == 0) {
        return pid;
    }
    p = get_process((uint32_t)pid);
    t = get_process((uint32_t)vp->tid);
    if (!p || !t || t->state == PROC_UNUSED || t->tg != p->tg ||
        (t->state == PROC_ZOMBIE && t != t->tg->leader)) {
        return -1;
    }
    return vp->tid;
}

/** The state letter of `p` in its stat file, which the State line of its
 * status file spells out: a ptrace stop is 't', a stop of its process 'T',
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
    return p->state == PROC_BLOCKED ? 'S' : 'R';
}

/**
 * Format /proc/<pid>/stat into `t`, returning its length or -ENOENT.
 *
 * Emits the space-separated fields ps and friends parse. comm (field 2) is
 * wrapped in parentheses, with any stray parens scrubbed so a naive parser that
 * scans for the closing ')' is not fooled. The CPU times are ticks, which are
 * the 100 Hz clock ticks the fields count: the whole process's, every
 * thread's, for a process's file (`whole`), and the thread's own for a
 * thread's under /proc/<pid>/task, as Linux keeps them apart; the children's
 * are the process's in both. The nice value is the one setpriority keeps.
 * The start time is the tick the process or thread was made at, which ps
 * subtracts from the uptime for its elapsed time, and the virtual size in
 * bytes and the resident pages are the address space's, as statm has them.
 * The accounting fields the kernel does not keep (faults) are zero, but
 * every field is present and in order.
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

    ap_int(t, pid);  /* 1 pid */
    ap_str(t, " ("); /* 2 comm */
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
    ap_int(t, (int)p->parent_pid); /* 4 ppid */
    ap_ch(t, ' ');
    ap_int(t, (int)p->pgid); /* 5 pgrp */
    ap_ch(t, ' ');
    ap_int(t, (int)p->sid); /* 6 session */
    ap_str(t, " 0 -1");     /* 7 tty_nr, 8 tpgid (not modeled) */
    /* 9..13 flags,minflt,cminflt,majflt,cmajflt;
       14..17 utime,stime,cutime,cstime */
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
    /* 18 priority, which Linux reports as 20 more than the nice value for a
       process of no real-time policy; 19 nice */
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
    ap_u32(t, mm ? vm_total(&mm->vm) : 0);
    ap_ch(t, ' ');
    ap_u32(t, mm ? mm_resident(mm) : 0);
    ap_ch(t, '\n');
    if (!t->nomem) {
        t->buf[t->len] = '\0';
    }
    return t->len;
}

/** The process `pid` names for one of its /proc files, or NULL. */
static process_t *procfs_proc(int pid)
{
    process_t *p = get_process((uint32_t)pid);

    return (p && p->state != PROC_UNUSED) ? p : NULL;
}

/**
 * The link count of process `pid`'s task directory, as Linux gives it: 2,
 * for the directory's entry in its parent and its own `.`, and one for each
 * live thread, whose directory's `..` names it. Chromium's zygote takes a
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
static void ap_memory(struct procfs_text *t, const process_t *p, uint32_t start, uint32_t end)
{
    struct mm *mm = p->mm;
    char chunk[128];

    if (end > start + 131072u) {
        end = start + 131072u;
    }
    mm_get(mm);
    while (start < end) {
        uint32_t n = end - start < sizeof(chunk) ? end - start : (uint32_t)sizeof(chunk);
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
        uint32_t start = p->mm->arg_start;
        uint32_t end = p->mm->arg_end;

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
        uint32_t start = p->mm->env_start;
        uint32_t end = p->mm->env_end;

        ap_memory(t, p, start, end);
    }
    return t->len;
}

/** /proc/<pid>/auxv: the auxiliary vector the program was given, as the
 * words of its initial stack, through its AT_NULL pair. */
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
        for (b = 0; b < 8; b++) {
            ap_ch(t, (char)(p->mm->auxv[i + (uint32_t)b / 4u] >> (8 * (b % 4))));
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

/** Append `v` as `width` lowercase hex digits, as /proc/<pid>/maps and
 * status write numbers. */
static void ap_xlow(struct procfs_text *t, uint32_t v, int width)
{
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = width - 1; i >= 0; i--) {
        ap_ch(t, digits[(v >> (i * 4)) & 0xFu]);
    }
}

/** The path /proc/<pid>/maps gives region `r` of `mm`: the file it maps,
 * [heap], [stack], the program for the segments of its image, which lie
 * below the heap, or none. */
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
    if ((r->type == VM_CODE || r->type == VM_DATA) && r->end <= mm->vm.heap_start) {
        return mm->exe;
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
 * pthread_getattr_np finds the stack by its [stack] line.
 */
static int procfs_gen_maps(struct procfs_text *t, int pid)
{
    process_t *p = procfs_proc(pid);
    const struct mm *mm;
    uint32_t i;

    if (!p) {
        return -ENOENT;
    }
    mm = p->mm;
    for (i = 0; mm && i < mm->vm.count; i++) {
        ap_maps_line(t, mm, &mm->vm.vmas[i]);
    }
    return t->len;
}

/** Append one field of /proc/<pid>/smaps: `name` padded to 16 places, then
 * the kilobytes of `pages` in eight and " kB", as Linux writes them. */
static void ap_smaps_kb(struct procfs_text *t, const char *name, uint32_t pages)
{
    uint32_t kb = pages * (PAGE_SIZE / 1024u);
    uint32_t w = 1;
    uint32_t v;
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
    ap_u32(t, kb);
    ap_str(t, " kB\n");
}

/**
 * /proc/<pid>/smaps: each region's maps line and Linux's fields for it, which
 * BusyBox's pmap and smemcap read. The pages in memory are the present pages
 * of the region's page tables, clean or dirty by the entry's dirty bit, and
 * shared or private by the region's type. The kernel does not count the
 * processes that map a page, so Pss is Rss, a page another process also maps
 * being counted whole to each; there is no swap, no huge page and no locking,
 * so those fields are 0.
 */
static int procfs_gen_smaps(struct procfs_text *t, int pid)
{
    process_t *p = procfs_proc(pid);
    const struct mm *mm;
    uint32_t i;

    if (!p) {
        return -ENOENT;
    }
    mm = p->mm;
    for (i = 0; mm && i < mm->vm.count; i++) {
        const vma_t *r = &mm->vm.vmas[i];
        uint32_t clean = 0, dirty = 0;
        uint32_t a;
        int shared = r->type == VM_SHARED;

        for (a = r->start; a < r->end; a += PAGE_SIZE) {
            int state = paging_user_state(mm->dir, a);

            clean += state == 1;
            dirty += state == 2;
        }
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
        ap_smaps_kb(t, "Anonymous:", r->file ? 0 : clean + dirty);
        ap_smaps_kb(t, "Swap:", 0);
        ap_smaps_kb(t, "SwapPss:", 0);
        ap_smaps_kb(t, "Locked:", 0);
        ap_str(t, "VmFlags:");
        ap_str(t, (r->prot & VM_PROT_READ) ? " rd" : "");
        ap_str(t, (r->prot & VM_PROT_WRITE) ? " wr" : "");
        ap_str(t, (r->prot & VM_PROT_EXEC) ? " ex" : "");
        ap_str(t, shared ? " sh" : "");
        ap_str(t, r->type == VM_STACK ? " gd" : "");
        ap_ch(t, '\n');
    }
    return t->len;
}

/** The bytes of `mm`'s regions of type `type`. */
static uint32_t mm_bytes(const struct mm *mm, vm_type_t type)
{
    uint32_t n = 0;
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
static void ap_kb(struct procfs_text *t, const char *name, uint32_t bytes)
{
    uint32_t kb = bytes / 1024u;
    uint32_t w = 1;
    uint32_t v;

    for (v = kb; v >= 10u; v /= 10u) {
        w++;
    }
    ap_str(t, name);
    ap_str(t, ":\t");
    while (w++ < 8u) {
        ap_ch(t, ' ');
    }
    ap_u32(t, kb);
    ap_str(t, " kB\n");
}

/** Append a 64-bit set as sixteen hex digits. */
static void ap_set(struct procfs_text *t, const char *name, uint64_t set)
{
    ap_str(t, name);
    ap_str(t, ":\t");
    ap_xlow(t, (uint32_t)(set >> 32), 8);
    ap_xlow(t, (uint32_t)set, 8);
    ap_ch(t, '\n');
}

/** Append "Name:\t" and four ids separated by tabs. */
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

/**
 * /proc/<pid>/status: the lines of Linux's that programs read, among them
 * the name, the state, the ids, the tracer, the memory in kilobytes, the
 * thread count and the signal sets. The file-system ids are the effective
 * ones, and the capability sets full for effective user ID 0 and empty
 * otherwise, as this kernel has no capabilities apart from root. VmPeak and
 * VmHWM are the present sizes, since the kernel keeps no peak.
 */
static int procfs_gen_status(struct procfs_text *t, int pid)
{
    process_t *p = procfs_proc(pid);
    const struct mm *mm;
    uint64_t ignored = 0;
    uint64_t caught = 0;
    uint64_t caps;
    uint32_t i;
    int sig;

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
              : p->state == PROC_BLOCKED ? "S (sleeping)"
                                         : "R (running)");
    ap_str(t, "\nTgid:\t");
    ap_u32(t, p->tgid);
    ap_str(t, "\nNgid:\t0\nPid:\t");
    ap_u32(t, p->pid);
    ap_str(t, "\nPPid:\t");
    ap_u32(t, p->parent_pid);
    ap_str(t, "\nTracerPid:\t");
    ap_u32(t, p->ptrace ? p->tracer : 0u);
    ap_ch(t, '\n');
    ap_ids(t, "Uid", p->ruid, p->euid, p->suid);
    ap_ids(t, "Gid", p->rgid, p->egid, p->sgid);
    ap_str(t, "FDSize:\t");
    ap_u32(t, (uint32_t)fd_table_cap(p->fds));
    ap_str(t, "\nGroups:\t");
    for (i = 0; i < p->ngroups; i++) {
        ap_u32(t, p->groups[i]);
        ap_ch(t, ' ');
    }
    ap_str(t, "\nNStgid:\t");
    ap_u32(t, p->tgid);
    ap_str(t, "\nNSpid:\t");
    ap_u32(t, p->pid);
    ap_str(t, "\nNSpgid:\t");
    ap_u32(t, p->pgid);
    ap_str(t, "\nNSsid:\t");
    ap_u32(t, p->sid);
    ap_ch(t, '\n');
    if (mm) {
        uint32_t size = vm_total(&mm->vm);
        uint32_t rss = mm_resident(mm) * PAGE_SIZE;

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
    caps = proc_privileged(p) ? 0x000001FFFFFFFFFFULL : 0;
    ap_set(t, "CapInh", 0);
    ap_set(t, "CapPrm", caps);
    ap_set(t, "CapEff", caps);
    ap_set(t, "CapBnd", 0x000001FFFFFFFFFFULL);
    ap_set(t, "CapAmb", 0);
    ap_str(t, "NoNewPrivs:\t");
    ap_u32(t, p->no_new_privs);
    ap_str(t, "\nSeccomp:\t0\nCpus_allowed:\t1\nCpus_allowed_list:\t0\n");
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
    ap_u32(t, vm_total(&mm->vm) / PAGE_SIZE);
    ap_ch(t, ' ');
    ap_u32(t, mm_resident(mm));
    ap_str(t, " 0 ");
    ap_u32(t, mm_bytes(mm, VM_CODE) / PAGE_SIZE);
    ap_str(t, " 0 ");
    ap_u32(t, (mm_bytes(mm, VM_DATA) + mm_bytes(mm, VM_HEAP) + mm_bytes(mm, VM_ANON) +
               mm_bytes(mm, VM_STACK)) /
                  PAGE_SIZE);
    ap_str(t, " 0\n");
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
    case SYNTH_PROC_MEM:
        return "mem";
    case SYNTH_PROC_OOM_ADJ:
        return "oom_score_adj";
    default:
        return "stat";
    }
}

/** Return the fixed path for a global snapshot with a named fd link. */
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
    default:
        return NULL;
    }
}

/**
 * Append the /proc/<pid>/fd/<fd> target of a file or directory descriptor to
 * `t`: the absolute path of a KRFFS node, "/memfd:NAME (deleted)" for a memfd
 * as Linux shows one, the path a node of another type was opened by, and "/"
 * for the root directory or a stale descriptor. A KRFFS path is rebuilt only
 * from a node still live and reachable, since a stale descriptor's node
 * (unlinked or relocated) may be free-list bytes by now; 256 bytes is the
 * path capacity the syscall layer uses.
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
 * Render the /proc/<pid>/fd/<fd> symlink target into `buf`, or -ENOENT.
 *
 * The target string matches what each descriptor type reports through the real
 * Linux /proc: a path for files and dirs (`procfs_file_target`), /dev/console,
 * /dev/pts/N, /dev/ptmx, and the pipe:[N]/socket:[N]/anon_inode forms. musl
 * ttyname_r relies on the /dev/pts/N spelling. A socket names its own inode,
 * the same number `/proc/net/tcp` and `fstat` report, so one socket is one
 * thing to a program reading all three. A descriptor that is closed, whose
 * release waits for another thread's call or is under way, has no link, as
 * it has no entry in the directory's listing and no lookup finds it.
 *
 * @return the length written, -ENOENT, or -ENOMEM.
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
    case FD_TYPE_UNIX:
        ap_str(t, "socket:[");
        ap_u32(t, e->priv ? unix_inode(e) : 0u);
        ap_ch(t, ']');
        break;
    case FD_TYPE_ANON:
        ap_str(t, "anon_inode:");
        ap_str(t, e->priv ? ((const struct anon_obj *)e->priv)->name : "[unknown]");
        break;
    case FD_TYPE_FILE:
    case FD_TYPE_DIR:
        procfs_file_target(t, e);
        break;
    case FD_TYPE_SYNTH_DIR: {
        /* a /dev or /proc directory fd: its reconstructed path */
        struct synth_dir *sd = (struct synth_dir *)e->priv;
        char path[64];
        if (sd) {
            synth_dir_path(sd->kind, sd->pid, sd->tid, path, (int)sizeof(path));
            ap_str(t, path);
        }
        break;
    }
    case FD_TYPE_PROC: {
        /* a /proc snapshot fd: a global path procfs_global_fd_path knows, else
         * the pid file */
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

                synth_sys_path(sysfs_node_of(pf->origin_pid), pf->origin_pid, where,
                               (int)sizeof(where));
                ap_str(t, where);
                ap_str(t, "/uevent");
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
        /* an O_PATH fd on a /dev or /proc node: the path it was opened by */
        ap_str(t, e->ofd && e->ofd->dirpath[0] ? e->ofd->dirpath : "/");
        break;
    case FD_TYPE_FRAMEBUFFER:
    case FD_TYPE_MOUSE:
    case FD_TYPE_NONE:
    default:
        ap_str(t, "anon_inode:[device]"); /* fb/mouse: best effort */
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
 * Return the pid of the n-th live process in process-table order, or -1.
 *
 * Lets a /proc directory read walk the table by index without exposing slots.
 * A process is listed once, by its leader; the ids of its other threads are
 * not listed, as on Linux, though /proc/<tid> still opens. The idle process,
 * pid 0, is not listed, as Linux lists no pid 0.
 */
int procfs_nth_pid(int n)
{
    const process_t *p;
    int seen = 0;

    if (n < 0) {
        return -1;
    }
    for (p = proc_list; p; p = p->next) {
        if (p->pid != p->tgid || p == proc_idle) {
            continue;
        }
        if (seen == n) {
            return (int)p->pid;
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

/** Return the stable inode of the audio report. */
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
    if (synth_net_name(pf->origin_file)) {
        return procfs_ino_net(pf->origin_file);
    }
    if (pf->origin_file == SYNTH_SYS_UEVENT) {
        return sysfs_ino(sysfs_node_of(pf->origin_pid), pf->origin_pid, 1);
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
 * Render /proc/net/dev.
 *
 * The two header lines and one line per interface, in the order and with the
 * sixteen counters Linux prints, because `ifconfig` and everything like it
 * parses this file by counting fields. Only the interfaces the probe brought
 * up are listed: an interface that is not there is not one a program should
 * be told about.
 *
 * Seven of the sixteen come from the interface: bytes and packets each way,
 * receive errors, receive drops, and transmit drops. The three refusal
 * columns are what the drivers report: a transmit verdict for the transmit
 * drops, and the two receive-note calls for the receive errors and drops.
 * Every driver here refuses a frame under some condition. Seven more come
 * from the card, through the driver's `counters`, in the columns Linux fills
 * from a card's own tallies: frames the card missed add to the receive drops,
 * alignment errors are the frame column, multicasts their own, and on the
 * transmit side errors, underruns (fifo), collisions and the transmissions
 * the card aborted (carrier). A card that keeps no such counts leaves them
 * zero. The two compressed columns and the receive fifo are zero: nothing
 * here has them.
 */
static int procfs_gen_netdev(struct procfs_text *t)
{
    int i;

    ap_str(t, "Inter-|   Receive                                                |"
              "  Transmit\n"
              " face |bytes    packets errs drop fifo frame compressed multicast|"
              "bytes    packets errs drop fifo colls carrier compressed\n");
    for (i = 0; i < netdev_count(); i++) {
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
 * timer actually delivered, which is the number that makes the two add up
 * to each other. Both counters are read once, into locals: a
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
 * Render /proc/stat: the tick counters, the interrupt total, the boot epoch,
 * the process counts, and this kernel's own latency and loopback counters.
 *
 * `loopback_dropped` is the datagrams the loopback ring had no room for, which
 * the ring logs once and counts thereafter, and `loopback_peak` is the most it
 * has held at once, which is what says how much room it has over a window's
 * worth.
 *
 * The `cpu` line has user, nice, system and idle in that order. The
 * machine-wide counters do not separate user from system time, which only the
 * per-process ones do. This line reports busy and idle ticks: busy goes in
 * the system column, since that is where a kernel's own time belongs, and
 * user and nice are zero rather than a share invented to look plausible.
 *
 * `processes` is what Linux means by it, the count of processes CREATED since
 * boot and not the count alive now. `procs_running` and `procs_blocked` are
 * counted from the table rather than fixed, since a machine with two runnable
 * processes reporting one is wrong the moment anything looks.
 */
static int procfs_gen_statsys(struct procfs_text *t)
{
    uint32_t busy = sched_busy_ticks();
    uint32_t idle = sched_idle_ticks();
    const process_t *p;
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
    ap_str(t, "\nsleeps_interrupted ");
    ap_u32(t, process_sleeps_cut_short());
    ap_ch(t, '\n');
    /* The longest delays since boot, for the load row; see latency.h. */
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
        } else if (p->state == PROC_BLOCKED) {
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
 * are the two counts outright. Buffers and Cached are what the buffer cache
 * and the page cache hold, which reclaim gives back under pressure, so
 * MemAvailable is the free count and the two together. HeapPages and
 * HeapUsed are the kernel heap's pages and the bytes in use inside it, for a
 * test that makes and frees objects and requires the heap back where it
 * started. Slab, last, is the heap's pages but the buffer cache's, which
 * the heap holds and Buffers counts, in kilobytes: the kernel's own objects
 * as Linux's slab holds them, which BusyBox's nmeter adds to MemFree,
 * Buffers and Cached, and subtracts from MemTotal, for the memory in use.
 */
static int procfs_gen_meminfo(struct procfs_text *t)
{
    uint32_t total = frame_total_count();
    uint32_t free_frames = frame_free_count();
    uint32_t buffers = bcache_resident();
    uint32_t cached = pc_resident();
    uint32_t heap_pages = 0, heap_bytes = 0;

    /* Every count before the first append, which may take a heap page. */
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
    ap_u32(t, heap_bytes);
    ap_str(t, "\n");
    ap_str(t, "SwapTotal:             0 kB\n");
    ap_str(t, "SwapFree:              0 kB\n");
    ap_str(t, "Slab:           ");
    ap_frames_as_kb(t, heap_pages > buffers ? heap_pages - buffers : 0);
    return t->len;
}

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
    if (usbdisk_free() != USBDISK_MAX || usbdisk_teardowns() != 0) {
        ap_str(t, "disks ");
        ap_u32(t, (uint32_t)usbdisk_free());
        ap_str(t, " free ");
        ap_u32(t, usbdisk_teardowns());
        ap_str(t, " reclaimed\n");
    }
    return t->nomem ? -1 : t->len;
}

/**
 * Render /proc/mounts in the six-field fstab shape `df` and `mount` parse.
 *
 * A KRFFS root's source and options are what the mount recorded when it
 * succeeded, because the three branches that can mount that file system
 * differ in ways nothing afterwards reports: a fixed line would call a RAM
 * root a disk one on two boots out of three. The two synthetic mounts are
 * named as well, since they are real mounts to every program that walks this
 * file, and then every mount mount(2) placed.
 */
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
     * caller gave it, its target, its type, and the flags as words. */
    for (i = 3; i < vfs_mount_slots(); i++) {
        struct vfs_mount *m = vfs_mount_at(i);

        if (!m || m->state != VFS_ACTIVE || vfs_mount_gone(m) || !m->parent) {
            continue; /* the memfd mount has no parent and no path */
        }
        ap_str(t, m->source);
        ap_ch(t, ' ');
        ap_str(t, m->covered_path);
        ap_ch(t, ' ');
        ap_str(t, m->type->name);
        ap_ch(t, ' ');
        ap_str(t, (m->flags & VFS_MS_RDONLY) ? "ro" : "rw");
        if (m->flags & VFS_MS_SYNCHRONOUS) {
            ap_str(t, ",sync");
        }
        if (m->flags & VFS_MS_DIRSYNC) {
            ap_str(t, ",dirsync");
        }
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
    return t->nomem ? -1 : t->len;
}

/* One counter of /proc/console: `name value`. */
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

/*
 * /proc/interrupts: one line per vector that has taken an interrupt, the
 * vector, its count and where it comes from, then the counts the APIC's
 * spurious vector and the timer's fallback pump keep. The fallback count is
 * what the pump found that a message should have brought: zero on a machine
 * whose messages arrive, so a number there is a count of lost edges and not
 * of work done.
 */
static int procfs_gen_interrupts(struct procfs_text *t)
{
    int v;

    for (v = 32; v <= ISR_MSI_LAST; v++) {
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
    ap_str(t, "FBK:\t");
    ap_u32(t, usb_fallback_count());
    ap_str(t, "\tusb fallback\n");
    return t->nomem ? -1 : t->len;
}

/* /proc/loop: one line per attached device, its size in bytes, whether it
 * takes writes, and the path it was attached by. A device nothing is attached
 * to has no line, so an empty file is a machine with no image mounted through
 * one. */
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

/* /proc/filesystems: `nodev` for the two synthetic namespaces, then every
 * type mount(2) accepts, KRFFS among them, with `nodev` before one that
 * stands on no device. The KRFFS root's own type is not one of these: it is
 * a mapped image, not a volume placed by mount(2). */
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
    ap_hex(t,
           ((ip & 0xFFu) << 24) | ((ip & 0xFF00u) << 8) | ((ip >> 8) & 0xFF00u) |
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
 * Render /proc/net/route: for each interface with an address, its default
 * route if it has a gateway, then its on-link subnet.
 *
 * Each interface has one subnet and at most one gateway, so it has one or two
 * rows, from its own address, mask and gateway. The flags are the ones
 * `route` prints from: 1 is up, and 2 more says the destination is a gateway.
 */
static int procfs_gen_netroute(struct procfs_text *t)
{
    int i;

    ap_str(t, PROCFS_NETROUTE_HEADER);
    for (i = 0; i < netdev_count(); i++) {
        const struct netdev *nd = netdev_at(i);

        if (nd->ip == 0) {
            continue; /* an interface with no address has no route */
        }
        if (nd->gateway != 0) {
            ap_str(t, nd->name);
            ap_str(t, "\t00000000\t");
            ap_ip_hex(t, nd->gateway);
            ap_str(t, "\t0003\t0\t0\t0\t00000000\t0\t0\t0\n");
        }
        ap_str(t, nd->name);
        ap_ch(t, '\t');
        ap_ip_hex(t, nd->ip & nd->mask);
        ap_str(t, "\t00000000\t0001\t0\t0\t0\t");
        ap_ip_hex(t, nd->mask);
        ap_str(t, "\t0\t0\t0\n");
    }
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
        ap_str(t, rows[i].iface);
        ap_ch(t, '\n');
    }
    return t->len;
}

/**
 * Render /proc/net/tcp: the connections, in the columns netstat parses.
 *
 * The state numbers are Linux's, which this stack's own enumeration does not
 * match, so they are mapped rather than cast: a reader prints the name for the
 * number it is given, and an unmapped value would name the wrong state.
 * `uid` and `inode` come from the block rather than from its socket, which is
 * cleared while a connection is still closing. A listener here takes its port
 * on every address of the machine, as one bound to INADDR_ANY does on Linux,
 * so its local address is 0.0.0.0, as Linux gives that one.
 */
static int procfs_gen_nettcp(struct procfs_text *t)
{
    struct tcp_row *rows;
    int n = tcp_snapshot(&rows);
    int i;

    if (n < 0) {
        return -ENOMEM;
    }
    ap_str(t, PROCFS_NETTCP_HEADER);
    for (i = 0; i < n; i++) {
        struct tcp_row row = rows[i];

        ap_str(t, "   ");
        ap_u32(t, (uint32_t)i);
        ap_str(t, ": ");
        ap_ip_hex(t, row.state == TCP_LISTEN ? 0 : row.local_ip);
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
 * A socket has no address of its own here, so the local address is 0.0.0.0,
 * as a socket bound to INADDR_ANY has it. The state is Linux's: 1, its
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
        ap_str(t, ": 00000000:");
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

/** Render the /proc/net file `origin`, SYNTH_PROC_NETUDP to
 * SYNTH_PROC_NETSNMP. */
static int procfs_gen_net(struct procfs_text *t, int origin)
{
    switch (origin) {
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

/* /sys: the devices of the PCI and USB buses, and each one's uevent. */

int procfs_fd_is_sys(struct fd_entry *e)
{
    const struct procfs_file *pf = e ? (const struct procfs_file *)e->priv : 0;

    return pf && pf->origin_file == SYNTH_SYS_UEVENT;
}

int sysfs_node_of(int key)
{
    return key < 0x10000 ? SYSFS_PCI_DEVICE : SYSFS_USB_DEVICE;
}

uint32_t sysfs_ino(int node, int key, int file)
{
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
    return SYSFS_INO_FIXED + (uint32_t)node;
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

/** The table entry of the running USB device `key`, or -1. */
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
    for (i = 0; i < USB_MAX_DEVICES; i++) {
        if (usb_core_row(i, &r) == 0 && (key = sysfs_usb_key(&r)) >= 0 && seen++ == n) {
            return key;
        }
    }
    return -1;
}

int sysfs_device_exists(int node, int key)
{
    struct usb_sys_row r;

    return node == SYSFS_PCI_DEVICE ? sysfs_pci_present(key) : sysfs_usb_find(key, &r) >= 0;
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

/* the /proc file backing: a snapshot served through file_ops */

/* A snapshot and its text are allocated from the heap at open and freed at
 * the last close. */

static int32_t procfs_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    uint32_t off, avail, n;

    if (!f) {
        return 0;
    }
    off = (uint32_t)e->ofd->offset;
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
 * count of the caller's own, or NULL: the one the open allowed, while
 * anything still runs in it. The descriptor keeps only its record
 * (`mm_hold`), as Linux's file keeps the mm_struct and a read takes
 * mm_users only while it is not 0: the pages go when the last thread that
 * ran in them ends or executes another program, and a program the process
 * executes, one of other ids among them, is never reached through a
 * descriptor opened before.
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
 * those are returned and the offset moves past them. -EIO when none could
 * be, and 0 once nothing runs in the address space. It has a user's count
 * for the length of the call, which may sleep bringing a page in while the
 * process ends.
 */
static int32_t procfs_mem_access(fd_entry_t *e, char __user *ubuf, uint32_t count, int write)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    struct mm *mm = f ? procfs_mem_mm(f) : NULL;
    uint32_t addr = (uint32_t)e->ofd->offset;
    uint32_t done = 0;
    char buf[256];
    int rc = 0;

    if (!mm) {
        return 0;
    }
    while (done < count) {
        uint32_t n = count - done < sizeof(buf) ? count - done : (uint32_t)sizeof(buf);

        if (n > PAGE_SIZE - (addr & (PAGE_SIZE - 1u))) {
            n = PAGE_SIZE - (addr & (PAGE_SIZE - 1u));
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

static int32_t procfs_mem_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    return procfs_mem_access(e, buf, count, 0);
}

static int32_t procfs_mem_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    return procfs_mem_access(e, (char __user *)(uintptr_t)buf, count, 1);
}

static const struct file_ops procfs_mem_ops = {procfs_mem_read, procfs_mem_write, procfs_poll,
                                               procfs_ioctl, procfs_close};

/**
 * Open /proc/<pid>/mem for reading, writing or both: the caller needs
 * effective user ID 0, or the process's real user ID and a dumpable process,
 * as a tracer does to attach. The descriptor keeps the process's id, `pid`
 * as `procfs_resolve_id` gave it for the path `vp`, a hold on the record of
 * the address space those checks allowed, which keeps none of its memory,
 * and no text; its offset is an address.
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

/** Append one decimal audio field. */
static void audio_field(struct procfs_text *t, const char *name, uint32_t value)
{
    ap_str(t, name);
    ap_u32(t, value);
}

/** Render cached codec enumeration and the last programmed stream. */
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

int procfs_fd_oom_owner(struct fd_entry *e, uint32_t *uid, uint32_t *gid)
{
    const struct procfs_file *f;

    if (!e || e->type != FD_TYPE_PROC || !e->priv) {
        return -1;
    }
    f = (const struct procfs_file *)e->priv;
    if (f->origin_file != SYNTH_PROC_OOM_ADJ || procfs_oom_owner(f->origin_pid, uid, gid) != 0) {
        return -1;
    }
    return 0;
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
    uint32_t off = (uint32_t)e->ofd->offset;
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
    if (entry == SYNTH_SYS_HOSTNAME) {
        ap_str(t, uts_hostname());
        ap_ch(t, '\n');
    } else {
        ap_str(t, synth_sys_text(entry));
    }
}

/** Read a /proc/sys file opened for writing as well: its value now. */
static int32_t procfs_sysctl_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    uint32_t off = (uint32_t)e->ofd->offset;
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
 * kernel/hostname becomes the text up to its first newline, at most 64
 * bytes, as sethostname(2) would set it; the whole count is taken. A write
 * to net/ipv4/ip_forward is an integer, and 0, which it is, is taken; any
 * other value is -EINVAL, since the stack forwards nothing between
 * interfaces, where Linux would begin to.
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
    if (f->origin_pid == SYNTH_SYS_HOSTNAME) {
        while (len < n && text[len] != '\n' && text[len] != '\0') {
            len++;
        }
        uts_set_hostname(text, len);
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

/**
 * Open a writable /proc/sys file for writing, or for reading and writing,
 * which needs a privileged caller, as the file's mode, 0644 and owned by
 * root, says. O_CREAT and O_TRUNC are taken as they are for a file that
 * exists, which is what a shell's redirection and BusyBox's `sysctl -w` ask
 * for. Each read and write reaches the value as it is then.
 */
static int procfs_open_sysctl(int entry, int flags)
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

/** Distinguish global snapshots from files whose process must still exist. */
static int procfs_needs_pid(int kind)
{
    return kind != VNODE_PROC_CMDLINE && kind != VNODE_PROC_DISK && kind != VNODE_PROC_NET_DEV &&
           kind != VNODE_PROC_UPTIME && kind != VNODE_PROC_STAT_SYS && kind != VNODE_PROC_MEMINFO &&
           kind != VNODE_PROC_CPUINFO && kind != VNODE_PROC_LOADAVG &&
           kind != VNODE_PROC_DISKSTATS && kind != VNODE_PROC_VERSION &&
           kind != VNODE_PROC_PARTITIONS && kind != VNODE_PROC_SYS_FILE &&
           kind != VNODE_PROC_MOUNTS && kind != VNODE_PROC_USB && kind != VNODE_PROC_AUDIO &&
           kind != VNODE_PROC_FILESYSTEMS && kind != VNODE_PROC_LOOP &&
           kind != VNODE_PROC_INTERRUPTS && kind != VNODE_PROC_CONSOLE &&
           kind != VNODE_PROC_NET_ROUTE && kind != VNODE_PROC_NET_ARP &&
           kind != VNODE_PROC_NET_TCP && kind != VNODE_PROC_NET_FILE && kind != VNODE_SYS_FILE;
}

/**
 * Open a /proc text file, global or per process, returning a descriptor or
 * -errno.
 *
 * The text is generated into a snapshot at open, then served read-only
 * through procfs_file_ops; the snapshot is freed on the last close. These
 * files are read-only regular files, so the open-flag checks are ordered to
 * report the conventional errno: O_CREAT|O_EXCL on an existing file is
 * -EEXIST first, then O_DIRECTORY is -ENOTDIR, then any write or create
 * intent is -EACCES. After the first two, /proc/<pid>/oom_score_adj and
 * /proc/<pid>/mem, which have no snapshot, go to opens of their own
 * (`procfs_open_oom`, `procfs_open_mem`).
 *
 * The snapshot grows as text is appended. If allocation fails, discard
 * the partial text and return -ENOMEM.
 *
 * `/proc/disk` is rendered from the record taken at boot, so opening it issues
 * no command to the device. `diskinfo_format` truncates to the cap it is
 * given rather than reporting,
 * so a text that reached the cap is refused with `-EOVERFLOW`.
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
    if (vp->kind == VNODE_PROC_FILE && vp->procfile == SYNTH_PROC_MEM) {
        return procfs_open_mem(vp, pid, flags);
    }
    if (vp->kind == VNODE_PROC_SYS_FILE && synth_sys_writable(vp->sysent) && (flags & 3) != 0) {
        return procfs_open_sysctl(vp->sysent, flags);
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
        /* The captured boot line and a newline, the Linux shape. */
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
    } else if (vp->kind == VNODE_PROC_VERSION) {
        /* Linux's "Linux version 6.8.0 ...", with uname's version after. */
        ap_str(&text, UTS_SYSNAME " version " UTS_RELEASE " (" UTS_VERSION ")\n");
        len = text.len;
    } else if (vp->kind == VNODE_PROC_CPUINFO) {
        len = procfs_text_room(&text, CPUINFO_TEXT_MAX) == 0
                  ? (int)cpuinfo_text(text.buf, CPUINFO_TEXT_MAX)
                  : -1;
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
    } else if (vp->kind == VNODE_SYS_FILE) {
        len = procfs_gen_uevent(&text, vp->sysent, vp->syskey);
        pid = vp->syskey;
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
    } else if (vp->procfile == SYNTH_PROC_COMM) {
        len = procfs_gen_comm(&text, pid);
    } else {
        len = procfs_gen_cmdline(&text, pid);
    }
    /* No memory first: a generator that ran out says so with -1, which is
       not an error of its own. */
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
