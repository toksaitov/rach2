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
#define PROCFS_INO_PID_BASE    0x200u

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
    struct mm *mm;   /* /proc/<pid>/mem: the address space the open allowed */
};

/**
 * Drop a reference to `mm` that /proc took, loading the kernel's directory
 * first when this is the last reference and its directory is the one in
 * use: a process a signal ends drops its own reference before it closes
 * its descriptors, and its /proc/self/mem descriptor may hold the last one.
 */
static void procfs_mm_put(struct mm *mm)
{
    if (mm->refs == 1 && _curr_dir == mm->dir) {
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
    case PROC_STOPPED:
    case PROC_UNUSED:
    default:
        return 'R';
    }
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
 * are the process's in both. The nice value is the one setpriority keeps;
 * the accounting fields the kernel does not keep (faults, sizes) are zero,
 * but every field is present and in order.
 */
static int procfs_gen_stat(struct procfs_text *t, int pid, int whole)
{
    process_t *p;
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
    ap_ch(t, proc_state_char(p->state)); /* 3 state */
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
    ap_str(t, " 0 0");      /* 21 itrealvalue, 22 starttime */
    ap_str(t, " 0 0");      /* 23 vsize, 24 rss */
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

/** Whether the caller may read what /proc shows of `p`'s memory, its
 * environment and its auxiliary vector: effective user ID 0, or a real user
 * ID that is `p`'s, as Linux's ptrace check asks at its simplest. */
static int procfs_may_read_mem(const process_t *p)
{
    return current_proc && (current_proc->euid == 0 || current_proc->ruid == p->ruid);
}

int procfs_readlink_exe(int pid, char *buf, int cap)
{
    process_t *p = procfs_proc(pid);
    int n;

    if (!p || !p->mm || !p->mm->exe) {
        return -ENOENT;
    }
    if (!procfs_may_read_mem(p)) {
        return -EACCES;
    }
    n = (int)k_strlen(p->mm->exe);
    if (n > cap) {
        n = cap;
    }
    k_memcpy(buf, p->mm->exe, (uint32_t)n);
    return n;
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

/**
 * /proc/<pid>/maps: a line for each region, in Linux's form: the range, the
 * rights with p or s for private or shared, the offset in the file, a device
 * and inode of 0, which the kernel does not keep, and the path from column
 * 73. GNU libc's pthread_getattr_np finds the stack by its [stack] line.
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
        const vma_t *r = &mm->vm.vmas[i];
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
        ap_str(t, " 00:00 0");
        if (path) {
            do {
                ap_ch(t, ' ');
            } while (t->len - start < 73 && !t->nomem);
            ap_str(t, path);
        }
        ap_ch(t, '\n');
    }
    return t->len;
}

/** The pages of `mm` in memory: the present user pages of its page tables. */
static uint32_t mm_resident(const struct mm *mm)
{
    uint32_t n = 0;
    uint32_t i;

    for (i = 0; i < mm->vm.count; i++) {
        uint32_t a;

        for (a = mm->vm.vmas[i].start; a < mm->vm.vmas[i].end; a += PAGE_SIZE) {
            if (paging_user_phys(mm->dir, a) != 0) {
                n++;
            }
        }
    }
    return n;
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
    caps = p->euid == 0 ? 0x000001FFFFFFFFFFULL : 0;
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
    case SYNTH_PROC_MEM:
        return "mem";
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
 * not listed, as on Linux, though /proc/<tid> still opens.
 */
int procfs_nth_pid(int n)
{
    const process_t *p;
    int seen = 0;

    if (n < 0) {
        return -1;
    }
    for (p = proc_list; p; p = p->next) {
        if (p->pid != p->tgid) {
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
 * started.
 */
static int procfs_gen_meminfo(struct procfs_text *t)
{
    uint32_t total = frame_total_count();
    uint32_t free_frames = frame_free_count();
    uint32_t buffers = bcache_resident();
    uint32_t cached = pc_resident();

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
    {
        uint32_t pages = 0, bytes = 0;

        kheap_stats(&pages, &bytes);
        ap_str(t, "HeapPages:      ");
        ap_u32(t, pages);
        ap_str(t, "\n");
        ap_str(t, "HeapUsed:       ");
        ap_u32(t, bytes);
        ap_str(t, "\n");
    }
    ap_str(t, "SwapTotal:             0 kB\n");
    ap_str(t, "SwapFree:              0 kB\n");
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
 * cleared while a connection is still closing.
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
        if (f->mm) {
            procfs_mm_put(f->mm);
        }
        kfree(f->data);
        kfree(f);
    }
}

static const struct file_ops procfs_file_ops = {procfs_read, procfs_write, procfs_poll,
                                                procfs_ioctl, procfs_close};

/**
 * The address space a /proc/<pid>/mem descriptor may reach, with a reference
 * of the caller's own, or NULL: the one the open allowed, while the process
 * it was opened for still runs in it. Once the process has another, which
 * an execve gives it, or none, the descriptor's reference goes, as on Linux,
 * where the file keeps the address space it was opened on and reads end
 * when that has no user left; a program the process executes, one of other
 * ids among them, is never reached through a descriptor opened before.
 */
static struct mm *procfs_mem_mm(struct procfs_file *f)
{
    struct mm *mm = f->mm;
    process_t *p = mm ? get_process((uint32_t)f->origin_pid) : NULL;

    if (!mm) {
        return NULL;
    }
    if (!p || p->state == PROC_UNUSED || p->mm != mm) {
        f->mm = NULL;
        procfs_mm_put(mm);
        return NULL;
    }
    mm_get(mm);
    return mm;
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
 * be, and 0 once the address space is not the process's. The address space
 * is held for the length of the call, which may sleep bringing a page in
 * while the process ends.
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
 * as `procfs_resolve_id` gave it for the path `vp`, a reference to the
 * address space those checks allowed, and no text; its offset is an
 * address.
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
    if (current_proc->euid != 0 && (current_proc->ruid != p->ruid || !p->mm->dumpable)) {
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
        return -EMFILE;
    }
    mm_get(p->mm);
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

/** Distinguish global snapshots from files whose process must still exist. */
static int procfs_needs_pid(int kind)
{
    return kind != VNODE_PROC_CMDLINE && kind != VNODE_PROC_DISK && kind != VNODE_PROC_NET_DEV &&
           kind != VNODE_PROC_UPTIME && kind != VNODE_PROC_STAT_SYS && kind != VNODE_PROC_MEMINFO &&
           kind != VNODE_PROC_MOUNTS && kind != VNODE_PROC_USB && kind != VNODE_PROC_AUDIO &&
           kind != VNODE_PROC_FILESYSTEMS && kind != VNODE_PROC_LOOP &&
           kind != VNODE_PROC_INTERRUPTS && kind != VNODE_PROC_CONSOLE &&
           kind != VNODE_PROC_NET_ROUTE && kind != VNODE_PROC_NET_ARP && kind != VNODE_PROC_NET_TCP;
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
 * intent is -EACCES.
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
    if (vp->kind == VNODE_PROC_FILE && vp->procfile == SYNTH_PROC_MEM) {
        return procfs_open_mem(vp, pid, flags);
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
    } else if (vp->procfile == SYNTH_PROC_STAT) {
        len = procfs_gen_stat(&text, pid, vp->tid == 0);
    } else if (vp->procfile == SYNTH_PROC_MAPS) {
        len = procfs_gen_maps(&text, pid);
    } else if (vp->procfile == SYNTH_PROC_STATUS) {
        len = procfs_gen_status(&text, pid);
    } else if (vp->procfile == SYNTH_PROC_STATM) {
        len = procfs_gen_statm(&text, pid);
    } else if (vp->procfile == SYNTH_PROC_AUXV) {
        len = procfs_gen_auxv(&text, pid);
    } else if (vp->procfile == SYNTH_PROC_ENVIRON) {
        len = procfs_gen_environ(&text, pid);
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
    } else {
        f->origin_file = vp->procfile;
    }

    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(f->data);
        kfree(f);
        return -EMFILE;
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
