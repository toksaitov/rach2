/*
 * The files of /proc and /sys, made from the kernel's tables.
 *
 * Most files are a snapshot: the text is made into a buffer from the heap
 * when the file is opened and served from there, so a read sees one moment,
 * and made again by a read from offset 0 after the first. Dup and fork share
 * the snapshot until the open file description's last close. The text is
 * built by append functions that grow the buffer and write numbers
 * themselves, since the kernel has no snprintf and i686 has no 64-bit
 * division here.
 */

#if defined(__x86_64__) && !defined(RACH2_HOSTTEST)
#include "../kvm/stats.h"
#include "../vfio/vfio.h"
#include "../iommu.h"
#endif

#include "../fpu.h"
#include "../klog.h"
#include "../panic.h"
#include "../irq.h"
#include "procfs.h"

#include "../latency.h"
#include "../dmabuf/dmabuf.h"
#include "../network/ip/ip.h"
#include "../network/ip6/ip6.h"
#include "../network/ip6/route6.h"
#include "../network/ip6/nd6.h"
#include "../mm/filecache.h"
#include "../mm/filemap.h"
#include "../mm/kheap.h"
#include "../mm/mempolicy.h"
#include "../mm/pagecache.h"
#include "bcache.h"
#include "dcache.h"
#include "ext2.h"

#include "../block/loop.h"
#include "synthfs.h"
#include "sysfs.h"
#include "fd.h"
#include "file.h"
#include "devfs.h"
#include "krffs.h"
#include "../usb/core.h"
#include "../pci.h"
#include "../audio/audio_proc.h"
#include "../speaker.h"
#include "../usb/usb.h"
#include "../block/usbdisk.h"
#include "../tty/pty.h"
#include "../proc/tgroup.h"
#include "../proc/seccomp.h"
#include "../proc/ns.h"
#include "../proc/smp.h"
#include "../mm/mm.h"
#include "../mm/paging.h"
#include "../io/poll.h"
#include "../cmdline.h"
#include "../block/diskinfo.h"
#include "../timer.h"
#include "../vdso.h"
#include "vfs.h"
#include "name.h"
#include "lock.h"
#include "sleeplock.h"
#include "tmpfs.h"
#include "cpuinfo.h"
#include "../dmi.h"
#include "../ipc/sysv.h"
#include "../ipc/mqueue.h"
#include "pathbuf.h"
#include "../syscall.h"
#include "../network/ethernet/ethernet.h"
#include "../network/driver/netdev.h"
#include "../network/wireless/wiphy.h"
#include "../rfkill.h"
#include "../network/arp/arp.h"
#include "../network/tcp/tcp.h"
#include "../network/netconf.h"
#include "../network/net.h"
#include "../network/socket/socket.h"
#include "../network/socket/packet.h"
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
#include "../proc/smp.h"
#include "../mm/paging.h"
#include "../io/anon.h"
#include "../io/inotify.h"
#include "../lib/krandom.h"
#include "../lib/kstd.h"
#include "../mm/vm.h"
#include "../ipc/unix.h"
#include "../acpicall.h"
#include "../display/backlight.h"

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
#define PROCFS_INO_NET         0x120u /* and the other files of /proc/net after it, by slot */
#define PROCFS_INO_ACPI        0x1F0u /* /proc/acpi */
#define PROCFS_INO_ACPI_CALL   0x1F1u
#define PROCFS_INO_FS          0x1F2u /* /proc/fs */
#define PROCFS_INO_LOCKWAIT    0x1F3u

/* Inode namespace for /sys: a fixed directory by its node, with its
 * attribute files 256 apart above it; a PCI function's directory by its
 * key in bits 8 to 23 and its file in the low byte, and its link by its
 * key in a range of its own; a USB device's directory by
 * its key with the top bit set, its uevent with the next bit too, and its
 * link with that bit alone;
 * and an interface's, a disk node's and a processor's directory and files
 * each in a range of its own, by the key from bit 8 and the file in the low
 * byte. The keys are below 2^30 (synthfs.h). */
#define SYSFS_INO_FIXED     0x100u
#define SYSFS_INO_PCI       0x01000000u
#define SYSFS_INO_PCI_LINK  0x02000000u
#define SYSFS_INO_NET       0x03000000u
#define SYSFS_INO_BLOCK     0x04000000u
#define SYSFS_INO_CPU       0x05000000u
#define SYSFS_INO_BACKLIGHT 0x06000000u
#define SYSFS_INO_RFKILL    0x08000000u
#define SYSFS_INO_REG       0x09000000u
#define SYSFS_INO_CACHE     0x0A000000u
#define SYSFS_INO_USB       0x80000000u
#define SYSFS_INO_FILE      0x40000000u
#define PROCFS_INO_SYS      0x140u /* and the entries of /proc/sys after it */
#define PROCFS_INO_PID_BASE 0x200u

/* A file of /proc that is one text for the whole system: the kind of its
 * node, the origin its descriptor records, and its inode number. */
struct procfs_global {
    int kind;
    int origin;
    uint32_t ino;
};

static const struct procfs_global procfs_globals[] = {
    {VNODE_PROC_CMDLINE, SYNTH_PROC_BOOTLINE, PROCFS_INO_CMDLINE},
    {VNODE_PROC_DISK, SYNTH_PROC_DISKINFO, PROCFS_INO_DISK},
    {VNODE_PROC_NET_DEV, SYNTH_PROC_NETDEV, PROCFS_INO_NETDEV},
    {VNODE_PROC_NET_ROUTE, SYNTH_PROC_NETROUTE, PROCFS_INO_NETROUTE},
    {VNODE_PROC_NET_ARP, SYNTH_PROC_NETARP, PROCFS_INO_NETARP},
    {VNODE_PROC_NET_TCP, SYNTH_PROC_NETTCP, PROCFS_INO_NETTCP},
    {VNODE_PROC_ACPI_CALL, SYNTH_PROC_ACPI_CALL, PROCFS_INO_ACPI_CALL},
    {VNODE_PROC_FS_LOCKWAIT, SYNTH_PROC_LOCKWAIT, PROCFS_INO_LOCKWAIT},
    {VNODE_PROC_UPTIME, SYNTH_PROC_UPTIME, PROCFS_INO_UPTIME},
    {VNODE_PROC_STAT_SYS, SYNTH_PROC_STATSYS, PROCFS_INO_STATSYS},
    {VNODE_PROC_MEMINFO, SYNTH_PROC_MEMINFO, PROCFS_INO_MEMINFO},
    {VNODE_PROC_CPUINFO, SYNTH_PROC_CPUINFO, PROCFS_INO_CPUINFO},
    {VNODE_PROC_LOADAVG, SYNTH_PROC_LOADAVG, PROCFS_INO_LOADAVG},
    {VNODE_PROC_DISKSTATS, SYNTH_PROC_DISKSTATS, PROCFS_INO_DISKSTATS},
    {VNODE_PROC_VERSION, SYNTH_PROC_VERSION, PROCFS_INO_VERSION},
    {VNODE_PROC_PARTITIONS, SYNTH_PROC_PARTITIONS, PROCFS_INO_PARTITIONS},
    {VNODE_PROC_VMSTAT, SYNTH_PROC_VMSTAT, PROCFS_INO_VMSTAT},
    {VNODE_PROC_SLABINFO, SYNTH_PROC_SLABINFO, PROCFS_INO_SLABINFO},
    {VNODE_PROC_SWAPS, SYNTH_PROC_SWAPS, PROCFS_INO_SWAPS},
    {VNODE_PROC_LOCKS, SYNTH_PROC_LOCKS, PROCFS_INO_LOCKS},
    {VNODE_PROC_MOUNTS, SYNTH_PROC_MOUNTS, PROCFS_INO_MOUNTS},
    {VNODE_PROC_AUDIO, SYNTH_PROC_AUDIO, PROCFS_INO_AUDIO},
    {VNODE_PROC_USB, SYNTH_PROC_USB, PROCFS_INO_USB},
    {VNODE_PROC_FILESYSTEMS, SYNTH_PROC_FILESYSTEMS, PROCFS_INO_FILESYSTEMS},
    {VNODE_PROC_LOOP, SYNTH_PROC_LOOP, PROCFS_INO_LOOP},
    {VNODE_PROC_INTERRUPTS, SYNTH_PROC_INTERRUPTS, PROCFS_INO_INTERRUPTS},
    {VNODE_PROC_CONSOLE, SYNTH_PROC_CONSOLE, PROCFS_INO_CONSOLE}};

/** The system-wide file whose node is of `kind`, or whose descriptor
 * records `origin` when `kind` is -1; NULL for none. */
static const struct procfs_global *procfs_global_find(int kind, int origin)
{
    unsigned i;

    for (i = 0; i < sizeof(procfs_globals) / sizeof(procfs_globals[0]); i++) {
        if (kind >= 0 ? procfs_globals[i].kind == kind : procfs_globals[i].origin == origin) {
            return &procfs_globals[i];
        }
    }
    return NULL;
}

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
/* The IPv6 files' headers, whose address columns are 37 wide. */
#define PROCFS_NETTCP6_HEADER                                                \
    "  sl  local_address                         remote_address            " \
    "            st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout"   \
    " inode\n"
#define PROCFS_NETUDP6_HEADER                                                \
    "  sl  local_address                         remote_address            " \
    "            st tx_queue rx_queue tr tm->when retrnsmt   uid  timeout"   \
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
    if (vp->kind == VNODE_PROC_FS_LOCKWAIT) {
        return "/proc/fs/lockwait";
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
    /* A snapshot's path, with the process or thread `procfs_resolve_id`
     * gave at the open, by which a read from offset 0 makes it again
     * (`procfs_read`); `fresh` while no read has used the text since it was
     * made, and `gen` counts the times it was made. `stale` when the open
     * made the text only for its checks, and the first read, at any
     * offset, makes it again: /proc/<pid>/syscall's. */
    struct vpath vp;
    int gen_pid;
    int fresh;
    int stale;
    uint32_t gen;
    /* A file of /proc/<pid> or a descriptor's fdinfo file: `owned`, the node
     * as `procfs_node_of` names it, the owner the last lookup of that node
     * gave (`procfs_lookup_owner`), and the next such open file. */
    int owned;
    int node[4];
    uint32_t uid;
    uint32_t gid;
    struct procfs_file *owned_next;
};

/* The open files whose owner is their node's at its last lookup, as a
 * Linux inode of /proc/<pid> keeps the owner `pid_revalidate` and
 * `tid_fd_revalidate` set (fs/proc/base.c:2053, fs/proc/fd.c:145) and has
 * no getattr to work it out again (base.c:776, fd.c:103). */
static struct procfs_file *procfs_owned;

static void procfs_own(struct procfs_file *f, const struct vpath *vp, int id);

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

/** Append `v` in decimal, all 64 bits of it. */
static void ap_u64(struct procfs_text *t, uint64_t v)
{
    char tmp[24];

    (void)diskinfo_u64_dec(tmp, (int)sizeof(tmp), v);
    ap_str(t, tmp);
}

/** Append `v` as `ap_u64` does, right-aligned in `width` places. */
static void ap_u64_width(struct procfs_text *t, uint64_t v, int width)
{
    char tmp[24];
    int n = diskinfo_u64_dec(tmp, (int)sizeof(tmp), v);

    while (n++ < width) {
        ap_ch(t, ' ');
    }
    ap_str(t, tmp);
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
    p = proc_named((uint32_t)pid);
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
    t = proc_named((uint32_t)vp->tid);
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
 * are in the `clock_t` of USER_HZ that Linux's fields count, whatever the
 * tick's rate (`TICKS_TO_CLOCK`): the whole process's, every
 * thread's, for a process's file (`whole`), and the thread's own for a
 * thread's under /proc/<pid>/task, as Linux keeps them apart; the children's
 * are the process's in both. The nice value is the one setpriority keeps.
 * The start time is when the process or thread was made, in the same
 * units, which ps subtracts from the uptime for its elapsed time, and the
 * virtual size in
 * bytes and the resident pages are the address space's, as statm has them.
 * The fields end at the 24th, rss; the flags, the fault counts and
 * itrealvalue, which the kernel does not keep, are 0.
 */
static int procfs_gen_stat(struct procfs_text *t, int pid, int whole)
{
    process_t *p;
    const struct mm *mm;
    uint64_t user;
    uint64_t sys;
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
    ap_u64(t, TICKS_TO_CLOCK(user));
    ap_ch(t, ' ');
    ap_u64(t, TICKS_TO_CLOCK(sys));
    ap_ch(t, ' ');
    ap_u64(t, TICKS_TO_CLOCK(p->tg->cutime));
    ap_ch(t, ' ');
    ap_u64(t, TICKS_TO_CLOCK(p->tg->cstime));
    /* 18 priority, Linux's nice + 20 without real-time policy; 19 nice */
    ap_ch(t, ' ');
    ap_int(t, 20 + (int)p->nice);
    ap_ch(t, ' ');
    ap_int(t, (int)p->nice);
    ap_ch(t, ' ');
    ap_u32(t, p->tg->live); /* 20 num_threads */
    ap_str(t, " 0 ");       /* 21 itrealvalue */
    ap_u64(t, TICKS_TO_CLOCK(p->start_ticks));
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

/**
 * Whether the caller may see what /proc shows of `p` behind Linux's ptrace
 * check with PTRACE_MODE_FSCREDS, `__ptrace_may_access`: a thread of `p`'s
 * own process; a privileged caller, which stands for CAP_SYS_PTRACE; or a
 * caller whose file-system user and group IDs are each of `p`'s three, with
 * `p`'s address space dumpable and `p` not privileged, which stands for the
 * subset test of its permitted capabilities in `cap_ptrace_access_check`.
 * READ and ATTACH differ on Linux only by Yama, which Rach2 does not have,
 * so one test serves both: the memory, environment, auxiliary vector and
 * maps, the syscall and wchan files, the links and the descriptors.
 */
static int procfs_may_read_mem(const process_t *p)
{
    uint32_t uid, gid;

    if (!current_proc) {
        return 0;
    }
    if (p->tg == current_proc->tg || proc_privileged(current_proc)) {
        return 1;
    }
    uid = proc_fsuid(current_proc);
    gid = proc_fsgid(current_proc);
    return uid == p->ruid && uid == p->euid && uid == p->suid && gid == p->rgid && gid == p->egid &&
           gid == p->sgid && (!p->mm || p->mm->dumpable == 1u) && !proc_privileged(p);
}

uint32_t procfs_owner_mode(int procfile)
{
    if (procfile == SYNTH_PROC_ENVIRON || procfile == SYNTH_PROC_AUXV ||
        procfile == SYNTH_PROC_SYSCALL) {
        return 0400;
    }
    return procfile == SYNTH_PROC_MEM ? 0600u : 0u;
}

/**
 * Whether the caller may open, with the access mode of `flags`, the file
 * `procfile` of process `pid`, whose mode lets only its owner open it
 * (`procfs_owner_mode`), as Linux's generic_permission judges it: a caller
 * whose file-system user ID owns it (`procfs_oom_owner`), for an access its
 * mode gives the owner, or one with privilege over files, which stands for
 * CAP_DAC_OVERRIDE. 1 for a file of any other mode.
 */
static int procfs_owner_may_open(int pid, int procfile, int flags)
{
    uint32_t mode = procfs_owner_mode(procfile), uid, gid, want;

    if (mode == 0 || proc_fs_privileged(current_proc)) {
        return 1;
    }
    if (procfs_oom_owner(pid, &uid, &gid) != 0) {
        return 0;
    }
    want = (flags & 3) == 0 ? 0400u : (flags & 3) == 1 ? 0200u : 0600u;
    return proc_fsuid(current_proc) == uid && (mode & want) == want;
}

int procfs_may_read(int pid)
{
    process_t *p = procfs_proc(pid);

    return p && procfs_may_read_mem(p);
}

int procfs_same_group(int pid)
{
    process_t *p = procfs_proc(pid);

    return p && current_proc && p->tg == current_proc->tg;
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

struct vfs_name *procfs_exe_entry(int pid)
{
    process_t *p = procfs_proc(pid);

    return p && p->mm && p->mm->exe_file && procfs_may_read_mem(p)
               ? vfs_name_ref(p->mm->exe_file->name)
               : NULL;
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

/** A path from the mount table becomes relative to the reader's root.
 * A path outside that root remains absolute, as the saved exec path did. */
static void exe_relative(char *path)
{
    struct vfs_ref root;
    char *prefix;
    uint32_t n;
    int rc;

    if (!current_proc || !current_proc->fs || !current_proc->fs->root_mnt) {
        return;
    }
    prefix = kmalloc(VFS_PATH_MAX);
    if (!prefix) {
        return;
    }
    root.mnt = current_proc->fs->root_mnt;
    root.node = current_proc->fs->root_node;
    if (vfs_enter(root.mnt) != 0) {
        kfree(prefix);
        return;
    }
    vfs_ref_hold(&root);
    vfs_ns_lock(root.mnt);
    rc = vfs_node_path(root.mnt, root.node, prefix, VFS_PATH_MAX);
    vfs_ns_unlock(root.mnt);
    vfs_ref_release(&root);
    if (rc == 0) {
        n = 0;
        while (prefix[n] && path[n] == prefix[n]) {
            n++;
        }
        if (n > 1 && !prefix[n] && (path[n] == '/' || path[n] == '\0')) {
            k_memmove(path, path + n, k_strlen(path + n) + 1);
            if (!path[0]) {
                path[0] = '/';
                path[1] = '\0';
            }
        }
    }
    kfree(prefix);
}

/** Reconstruct the executable's present path under its mount's lock. The
 * description stays held across a sleeping path read and is released before
 * the reader's root is read, so two mount locks are never held together. */
static char *exe_file_path(struct open_file *file, int *deleted)
{
    krffs_node_t *node;
    struct vfs_stat st;
    char *path = NULL;

    *deleted = 0;
    if (!file || !file->vmnt) {
        return NULL;
    }
    fd_ofd_ref(file);
    vfs_ns_lock(file->vmnt);
    node = exe_root_node(file);
    if (node) {
        *deleted = node->name[0] == '\0';
        if (!*deleted) {
            path = exe_present_path(node);
        }
    } else if (file->name) {
        *deleted = vfs_name_deleted(file->name);
        path = kmalloc(VFS_PATH_MAX);
        if (path && vfs_name_path(file->name, file->vnode, path, VFS_PATH_MAX, deleted) != 0) {
            kfree(path);
            path = NULL;
        }
    } else {
        if (vfs_getattr(file->vmnt, file->vnode, &st) == 0) {
            *deleted = st.nlink == 0;
        }
        if (!*deleted && (path = kmalloc(VFS_PATH_MAX)) != NULL &&
            vfs_node_path(file->vmnt, file->vnode, path, VFS_PATH_MAX) != 0) {
            kfree(path);
            path = NULL;
        }
    }
    vfs_ns_unlock(file->vmnt);
    fd_ofd_unref(file);
    if (path) {
        exe_relative(path);
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
    deleted = 0;
    if (cap > 0) {
        now = exe_file_path(mm->exe_file, &deleted);
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

int procfs_link_target(const struct vpath *vp, struct vfs_ref *out, char *buf, int cap)
{
    int pid = procfs_resolve_id(vp);
    process_t *p = pid < 0 ? NULL : procfs_proc(pid);
    fd_entry_t *e;

    if (!p || !p->fs || !p->fds) {
        return -ENOENT;
    }
    if (!procfs_may_read_mem(p)) {
        return -EACCES;
    }
    if (vp->kind == VNODE_PROC_ROOT) {
        out->mnt = p->fs->root_mnt ? p->fs->root_mnt : vfs_root_mount();
        out->node = p->fs->root_mnt ? p->fs->root_node : out->mnt->root;
        return 0;
    }
    if (vp->kind == VNODE_PROC_CWD) {
        if (!p->fs->cwd_mnt) {
            return exe_put(buf, cap, 0, proc_cwd(p));
        }
        out->mnt = p->fs->cwd_mnt;
        out->node = p->fs->cwd_node;
        return 0;
    }
    if (vp->procfd < 0 || vp->procfd >= fd_table_cap(p->fds)) {
        return -ENOENT;
    }
    e = &p->fds->entries[vp->procfd];
    if (e->type == FD_TYPE_NONE || (e->flags & FD_FLAG_CLOSING)) {
        return -ENOENT;
    }
    if ((e->type == FD_TYPE_FILE || e->type == FD_TYPE_DIR) && e->ofd && e->ofd->vmnt) {
        out->mnt = e->ofd->vmnt;
        out->node = e->ofd->vnode;
        return 0;
    }
    if (e->type == FD_TYPE_SYNTH_DIR && e->priv) {
        const struct synth_dir *sd = e->priv;
        char path[64];

        synth_dir_path(sd->kind, sd->pid, sd->tid, path, (int)sizeof(path));
        return exe_put(buf, cap, 0, path);
    }
    return -ENOTDIR;
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
 * of its initial stack as the address space keeps them, 4 bytes each for an
 * i386 program, through its AT_NULL pair; -EACCES for a caller
 * `procfs_may_read_mem` refuses. */
static int procfs_gen_auxv(struct procfs_text *t, int pid)
{
    process_t *p = procfs_proc(pid);
    uint32_t word, pair, i;

    if (!p) {
        return -ENOENT;
    }
    if (!procfs_may_read_mem(p)) {
        return -EACCES;
    }
    if (!p->mm) {
        return t->len;
    }
    word = (uint32_t)sizeof(p->mm->auxv[0]);
    if (p->mm->compat) {
        word = 4u;
    }
    for (pair = 0; (pair + 1u) * 2u * word <= sizeof(p->mm->auxv); pair++) {
        const uint8_t *at = (const uint8_t *)p->mm->auxv + pair * 2u * word;
        uint8_t type = 0;

        for (i = 0; i < 2u * word; i++) {
            ap_ch(t, (char)at[i]);
            type |= i < word ? at[i] : 0u;
        }
        if (type == 0) {
            break;
        }
    }
    return t->len;
}

/**
 * /proc/<pid>/task/<tid>/children: the PID of each child of the process,
 * as the reader's PID namespace sees it, each followed by one space, in the
 * order of the process list. A child records the process that made it and
 * not the thread, so the file of the leader, whose id `tid` is the
 * process's, lists every child, and the file of any other thread is empty;
 * Linux lists each thread's own.
 */
static int procfs_gen_children(struct procfs_text *t, int tid)
{
    process_t *self = procfs_proc(tid);
    process_t *c;

    if (!self) {
        return -ENOENT;
    }
    if (self->pid != self->tgid) {
        return t->len;
    }
    for (c = proc_list; c; c = c->next) {
        if (c->state != PROC_UNUSED && c->pid == c->tgid && c->parent_pid == self->tgid &&
            pid_seen(c->pid) != 0) {
            ap_u32(t, pid_seen(c->pid));
            ap_ch(t, ' ');
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

/**
 * /proc/<pid>/syscall, in the forms of Linux's `proc_pid_syscall`:
 * "running" for a thread that is ready or on a processor, other than the
 * caller; otherwise, from the frame of the thread's entry to the kernel,
 * "NR A0 A1 A2 A3 A4 A5 SP PC" when that entry was a system call, with the
 * number as a tracer left it (`orig_nr`) and the arguments of the call's
 * ABI, i386's for an `int $0x80` on x86-64; "NR SP PC" when that number is
 * negative, and "-1 SP PC" when the entry was a fault or an interrupt, or
 * after a signal handler's return. A leader that
 * has ended reads "-1 0x0 0x0", as Linux's, whose stack is gone, does. A
 * sleeping kernel thread, one with no address space outside an exit, reads
 * number 0 and every other field 0x0: Linux's has a stack, and the frame
 * `copy_thread` zeroes for it gives `orig_ax` 0. An exiting thread past its
 * address space still has its user frame. All but the number are in hex.
 * -EPERM for a caller `procfs_may_read_mem` refuses, as Linux's
 * `lock_trace` answers its PTRACE_MODE_ATTACH_FSCREDS check; the file's
 * mode of 0400 is the open's (`procfs_owner_may_open`). The open makes the
 * text only to see that it can be made, and the first read makes it again
 * (`procfs_read`), as Linux's seq_file makes it, so the caller's own thread
 * shows the read, and a caller the ptrace check refuses opens the file and
 * gets EPERM from its reads.
 */
static int procfs_gen_syscall(struct procfs_text *t, int pid)
{
    process_t *p = procfs_proc(pid);
    const isr_params_t *tf;
    unsigned long args[6];
    int i, nr;

    if (!p) {
        return -ENOENT;
    }
    if (!procfs_may_read_mem(p)) {
        return -EPERM;
    }
    if (p != current_proc && (p->state == PROC_READY || p->state == PROC_RUNNING)) {
        ap_str(t, "running\n");
        return t->len;
    }
    if (p->state == PROC_ZOMBIE) {
        ap_str(t, "-1 0x0 0x0\n");
        return t->len;
    }
    if (p->space_root == 0 && !p->in_syscall) {
        ap_str(t, "0 0x0 0x0 0x0 0x0 0x0 0x0 0x0 0x0\n");
        return t->len;
    }
    tf = proc_user_frame(p);
    nr = FRAME_SYSCALL(tf) ? (int)p->orig_nr : -1;
    ap_int(t, nr);
    if (nr >= 0) {
        args[0] = SC_ARG(tf, 0);
        args[1] = SC_ARG(tf, 1);
        args[2] = SC_ARG(tf, 2);
        args[3] = SC_ARG(tf, 3);
        args[4] = SC_ARG(tf, 4);
        args[5] = SC_ARG(tf, 5);
        for (i = 0; i < 6; i++) {
            ap_str(t, " 0x");
            ap_xlow(t, args[i], 1);
        }
    }
    ap_str(t, " 0x");
    ap_xlow(t, (unsigned long)FRAME_SP(tf), 1);
    ap_str(t, " 0x");
    ap_xlow(t, (unsigned long)FRAME_PC(tf), 1);
    ap_ch(t, '\n');
    return t->len;
}

/**
 * The name of the function thread `p` sleeps in, as /proc/<pid>/wchan gives
 * it: the name its wait recorded while it is blocked, Linux's
 * `do_signal_stop` or `ptrace_stop` while it is stopped, and NULL while it
 * runs, is ready to, or has ended, as Linux's `get_wchan` gives 0 then.
 */
static const char *procfs_wchan(const process_t *p)
{
    if (p->state == PROC_BLOCKED) {
        return p->wchan;
    }
    if (p->state == PROC_STOPPED) {
        return p->ptrace_status != 0 ? "ptrace_stop" : "do_signal_stop";
    }
    return NULL;
}

/** /proc/<pid>/wchan: `procfs_wchan`'s name with no newline, or "0" for
 * none and for a caller `procfs_may_read_mem` refuses, as Linux gives 0 to
 * a reader that fails its PTRACE_MODE_READ check. */
static int procfs_gen_wchan(struct procfs_text *t, int pid)
{
    process_t *p = procfs_proc(pid);
    const char *name;

    if (!p) {
        return -ENOENT;
    }
    name = procfs_may_read_mem(p) ? procfs_wchan(p) : NULL;
    ap_str(t, name ? name : "0");
    return t->len;
}

/**
 * /proc/<pid>/schedstat, as Linux's `proc_pid_schedstat` prints the
 * thread's sched_info: its time on a processor in nanoseconds, here its
 * ticks of user and system time, then the nanoseconds it waited ready on a
 * run queue and the times it was switched onto a processor, which
 * `schedule` counts (process.h's `run_delay` and `pcount`).
 */
static int procfs_gen_schedstat(struct procfs_text *t, int pid)
{
    process_t *p = procfs_proc(pid);

    if (!p) {
        return -ENOENT;
    }
    ap_u64(t, (p->utime + p->stime) * TICK_NS);
    ap_ch(t, ' ');
    ap_u64(t, p->run_delay);
    ap_ch(t, ' ');
    ap_u64(t, (uint64_t)p->pcount);
    ap_ch(t, '\n');
    return t->len;
}

/** The path /proc/<pid>/maps gives region `r` of `mm`: the file it maps,
 * which for the vDSO's two regions is [vvar] or [vdso] (`vdso_map`),
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

/** Append the device and inode of the file region `r` of `mm` maps, as stat
 * gives them (`vfs_fill_stat64`): the major and minor numbers in hex, a space
 * and the inode in decimal, or "00:00 0" for a region of no file. A segment
 * of the program or the dynamic linker that was loaded and not mapped, as
 * the debug shell loads its program, has the identity of the file whose
 * path `maps_path` gives it. */
static void ap_maps_identity(struct procfs_text *t, const struct mm *mm, const vma_t *r)
{
    const struct open_file *ofd = filemap_ofd(r->file);
    const struct vfs_mount *base;
    uint32_t dev = 0;
    int i;

    if (!r->file && (r->type == VM_CODE || r->type == VM_DATA)) {
        ofd = r->end <= mm->vm.heap_start ? mm->exe_file : mm->interp_file;
    }
    if (!ofd || !ofd->vmnt) {
        ap_str(t, "00:00 0");
        return;
    }
    base = vfs_mount_base(ofd->vmnt);
    for (i = 0; i < vfs_mount_slots(); i++) {
        if (vfs_mount_at(i) == base) {
            dev = procfs_mount_dev(i, base);
        }
    }
    ap_xlow(t, (dev >> 8) & 0xfffu, 2);
    ap_ch(t, ':');
    ap_xlow(t, (dev & 0xffu) | ((dev >> 12) & 0xfff00u), 2);
    ap_ch(t, ' ');
    ap_u64(t, ofd->ino);
}

/** Whether region `r` is a shared mapping, Linux's VM_MAYSHARE: a VM_SHARED
 * region, or a read-only shared mapping served as a private one, VM_FILE
 * with VM_PROT_NOWRITE, which only a shared mapping is given; the vDSO's
 * data page, which Linux maps private, is not. */
static int maps_shared(const vma_t *r)
{
    return !vdso_region_name(r->file) &&
           (r->type == VM_SHARED || (r->type == VM_FILE && (r->prot & VM_PROT_NOWRITE)));
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
    ap_ch(t, VM_READABLE(r->prot) ? 'r' : '-');
    ap_ch(t, (r->prot & VM_PROT_WRITE) ? 'w' : '-');
    ap_ch(t, (r->prot & VM_PROT_EXEC) ? 'x' : '-');
    ap_ch(t, maps_shared(r) ? 's' : 'p');
    ap_ch(t, ' ');
    ap_xlow(t, r->file ? r->pgoff * PAGE_SIZE : 0u, 8);
    ap_ch(t, ' ');
    ap_maps_identity(t, mm, r);
    ap_ch(t, ' ');
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
 * rights with p or s for private or shared, p for the vDSO's two regions as
 * on Linux though every process shares their pages, the offset in the file, the
 * file's device and inode, 0 for a region of no file, a space, and the path
 * from column 73. A region with no path ends at that space, as on Linux, where
 * BusyBox's pmap counts on it to find the end of the fields. GNU libc's
 * pthread_getattr_np finds the stack by its [stack] line. -EACCES for a
 * caller `procfs_may_read_mem` refuses, as Linux refuses another user.
 */
static int procfs_gen_maps(struct procfs_text *t, int pid)
{
    process_t *p = procfs_proc(pid);
    const struct mm *mm;
    const vma_t *r;

    if (!p) {
        return -ENOENT;
    }
    if (!procfs_may_read_mem(p)) {
        return -EACCES;
    }
    mm = p->mm;
    for (r = mm ? vm_first(&mm->vm) : NULL; r; r = vm_next(r)) {
        ap_maps_line(t, mm, r);
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

/** Append the `ProtectionKey:` line of the region `r`, as Linux's smaps has
 * it where the protection keys are on; nothing where they are off. */
static void ap_smaps_key(struct procfs_text *t, const vma_t *r)
{
    if (fpu_pkeys()) {
        ap_str(t, "ProtectionKey:  ");
        ap_u32_width(t, VM_PROT_KEY(r->prot), 8);
        ap_ch(t, '\n');
    }
}

/**
 * Append the possible-access part of the VmFlags line of the region `r`:
 * sh, mr, mw, me and ms. A shared mapping is ms (`maps_shared`), and sh too
 * only when it may be made writable, as Linux clears VM_SHARED for a file
 * opened without write. The vDSO's pages have what Linux's special mappings
 * have: [vdso] mr mw me, and [vvar], which is VM_PROT_NOWRITE, mr.
 */
static void ap_smaps_may(struct procfs_text *t, const vma_t *r)
{
    int vvar = vdso_is_data(r->file);
    int shared = maps_shared(r);

    ap_str(t, shared && !(r->prot & VM_PROT_NOWRITE) ? " sh" : "");
    ap_str(t, " mr");
    ap_str(t, !(r->prot & VM_PROT_NOWRITE) ? " mw" : "");
    ap_str(t, vvar || (r->prot & VM_PROT_SECRET) ? "" : " me");
    ap_str(t, shared ? " ms" : "");
}

/**
 * Append the VmFlags line of smaps for the region `r`, the names Linux's
 * show_smap_vma_flags gives the flags this kernel keeps, in the order of
 * Linux's VM_ bits: access and possible access (`ap_smaps_may`), then gd
 * 0x100, pf 0x400, lo 0x2000, io 0x4000, dc 0x20000, de 0x40000, lf
 * 0x80000, ac 0x100000, nr 0x200000, wf 0x2000000, dd 0x4000000, then sl.
 * The vDSO's [vdso] is de,
 * and its [vvar] pf io de dd, as Linux's special mappings are.
 */
static void ap_smaps_flags(struct procfs_text *t, const vma_t *r)
{
    int vdso = vdso_region_name(r->file) != NULL;
    int io = (r->prot & VM_PROT_IO) || (vdso && vdso_is_data(r->file));

    ap_str(t, "VmFlags:");
    ap_str(t, VM_READABLE(r->prot) ? " rd" : "");
    ap_str(t, (r->prot & VM_PROT_WRITE) ? " wr" : "");
    ap_str(t, (r->prot & VM_PROT_EXEC) ? " ex" : "");
    ap_smaps_may(t, r);
    ap_str(t, r->type == VM_STACK ? " gd" : "");
    ap_str(t, io ? " pf" : "");
    ap_str(t, (r->prot & VM_PROT_LOCKED) ? " lo" : "");
    ap_str(t, io ? " io" : "");
    ap_str(t, (r->prot & VM_PROT_DONTFORK) ? " dc" : "");
    ap_str(t, io || vdso ? " de" : "");
    ap_str(t, (r->prot & VM_PROT_LOCKONFAULT) ? " lf" : "");
    ap_str(t, (r->prot & VM_PROT_ACCOUNT) ? " ac" : "");
    ap_str(t, (r->prot & VM_PROT_NORESERVE) ? " nr" : "");
    ap_str(t, (r->prot & VM_PROT_WIPEONFORK) ? " wf" : "");
    ap_str(t, io || (r->prot & VM_PROT_SECRET) ? " dd" : "");
    ap_str(t, (r->prot & VM_PROT_SEALED) ? " sl" : "");
    ap_ch(t, '\n');
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
 * also maps being counted whole to each; there is no swap and no huge page
 * of user memory, so those fields are 0. Locked is Rss for a locked region,
 * flagged `lo`, and `lf` when it is locked on fault; a mapping of
 * memfd_secret is flagged `dd`, left out of a core file, and a sealed region
 * `sl`, last, as Linux's highest bit. Where the protection keys are on, the
 * region's key comes before the flags, as `ProtectionKey:`.
 * -EACCES for a caller `procfs_may_read_mem` refuses, as for maps.
 */
static int procfs_gen_smaps(struct procfs_text *t, int pid)
{
    process_t *p = procfs_proc(pid);
    const struct mm *mm;
    const vma_t *r;

    if (!p) {
        return -ENOENT;
    }
    if (!procfs_may_read_mem(p)) {
        return -EACCES;
    }
    mm = p->mm;
    for (r = mm ? vm_first(&mm->vm) : NULL; r; r = vm_next(r)) {
        unsigned long present, clean, dirty, anon;
        int shared = r->type == VM_SHARED;
        int of_file = r->file || r->type == VM_CODE || r->type == VM_DATA;

        paging_user_count(mm->dir, r->start, r->end, &present, &dirty, &anon);
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
        ap_smaps_kb(t, "Anonymous:", !of_file ? clean + dirty : shared ? 0 : anon);
        ap_smaps_kb(t, "Swap:", 0);
        ap_smaps_kb(t, "SwapPss:", 0);
        ap_smaps_kb(t, "Locked:", (r->prot & VM_PROT_LOCKED) ? clean + dirty : 0);
        ap_smaps_key(t, r);
        ap_smaps_flags(t, r);
    }
    return t->len;
}

/** Append the policy `pol`, NULL for the default, as Linux's mpol_to_str
 * writes it in numa_maps: the mode's name; for a policy with a mode flag
 * "=" and "static" or "relative", then "balancing", after a "|" when it
 * follows one of the two; and ":0", the policy's nodes, for a mode that has
 * nodes. */
static void ap_numa_policy(struct procfs_text *t, const struct vm_policy *pol)
{
    static const char *const names[] = {
        "default", "prefer", "bind", "interleave", "local", "prefer (many)", "weighted interleave"};
    uint32_t mode = pol ? pol->mode : 0u;
    uint32_t flags = pol ? pol->flags : 0u;

    ap_str(t, mode < sizeof(names) / sizeof(names[0]) ? names[mode] : "unknown");
    if (flags) {
        ap_ch(t, '=');
        ap_str(t, (flags & MPOL_F_STATIC_NODES)     ? "static"
                  : (flags & MPOL_F_RELATIVE_NODES) ? "relative"
                                                    : "");
        if (flags & MPOL_F_NUMA_BALANCING) {
            ap_str(t, flags != MPOL_F_NUMA_BALANCING ? "|balancing" : "balancing");
        }
    }
    if (pol && pol->nodes) {
        ap_str(t, ":0");
    }
}

/** Append what region `r` of `mm` maps, as numa_maps names it: " heap",
 * " stack", or " file=" and the path maps gives the region, with a newline,
 * a tab, a space and an equals sign each written as a backslash and three
 * octal digits, as Linux's seq_path writes them; nothing for anonymous
 * memory and for the vDSO's two regions. */
static void ap_numa_place(struct procfs_text *t, const struct mm *mm, const vma_t *r)
{
    const char *path = maps_path(mm, r);

    if (r->type == VM_HEAP || r->type == VM_STACK) {
        ap_str(t, r->type == VM_HEAP ? " heap" : " stack");
        return;
    }
    if (!path || vdso_region_name(r->file)) {
        return;
    }
    ap_str(t, " file=");
    for (; *path; path++) {
        if (*path == '\n' || *path == '\t' || *path == ' ' || *path == '=') {
            ap_ch(t, '\\');
            ap_ch(t, (char)('0' + ((*path >> 6) & 3)));
            ap_ch(t, (char)('0' + ((*path >> 3) & 7)));
            ap_ch(t, (char)('0' + (*path & 7)));
        } else {
            ap_ch(t, *path);
        }
    }
}

/** Append `name` and the count `n`, a field of a numa_maps line. */
static void ap_numa_count(struct procfs_text *t, const char *name, unsigned long n)
{
    ap_str(t, name);
    ap_ulong(t, n);
}

/**
 * /proc/<pid>/numa_maps: a line for each region, in Linux's form: the start,
 * the policy that governs it, and what it maps (`ap_numa_place`); then, for
 * a region with pages of memory, the counts of them Linux gives: anon=, the
 * anonymous ones; dirty=, those written; mapped=, all of them, when they are
 * neither all anonymous nor all dirty; N0=, those on node 0, which is all;
 * and the page size in kilobytes. The policy is the one mbind gave the
 * region's first page (`mempolicy_at`), and where it gave none the policy
 * of the thread read, as on Linux. The pages are the frames of the region's
 * page tables (`paging_user_frames`): a page a read brought in zeroed is
 * Linux's zero page and is not one, nor is device memory. Memory of no
 * file is anonymous and counts as written, as Linux's anonymous pages are
 * from their first store; of a private region read from a file the pages
 * written since are the anonymous ones, as in smaps. Linux's mapmax=, the
 * most mappings a page has, and active=, the pages on the active list, are
 * not given: the kernel counts no mappings of a page and has no such list.
 * -EACCES for a caller `procfs_may_read_mem` refuses, as for maps.
 */
static int procfs_gen_numa_maps(struct procfs_text *t, int pid)
{
    process_t *p = procfs_proc(pid);
    const struct mm *mm;
    const vma_t *r;

    if (!p) {
        return -ENOENT;
    }
    if (!procfs_may_read_mem(p)) {
        return -EACCES;
    }
    mm = p->mm;
    for (r = mm ? vm_first(&mm->vm) : NULL; r; r = vm_next(r)) {
        const struct vm_policy *pol = mempolicy_at(r, r->start);
        int of_file = r->file || r->type == VM_CODE || r->type == VM_DATA;
        struct paging_frame_counts counts;
        unsigned long frames, dirty, anon;

        ap_xlow(t, r->start, 8);
        ap_ch(t, ' ');
        ap_numa_policy(t, pol ? pol : p->mempolicy);
        ap_numa_place(t, mm, r);
        paging_user_frames(mm->dir, r->start, r->end, &counts);
        frames = counts.frames;
        dirty = counts.dirty;
        anon = counts.anon;
        if (frames != 0) {
            dirty = of_file ? dirty : frames;
            anon = !of_file ? frames : r->type == VM_SHARED ? 0 : anon;
            if (anon) {
                ap_numa_count(t, " anon=", anon);
            }
            if (dirty) {
                ap_numa_count(t, " dirty=", dirty);
            }
            if (frames != anon && frames != dirty) {
                ap_numa_count(t, " mapped=", frames);
            }
            ap_numa_count(t, " N0=", frames);
            ap_numa_count(t, " kernelpagesize_kB=", PAGE_SIZE / 1024u);
        }
        ap_ch(t, '\n');
    }
    return t->len;
}

/** The bytes of `mm`'s regions of type `type`. */
static unsigned long mm_bytes(const struct mm *mm, vm_type_t type)
{
    unsigned long n = 0;
    const vma_t *r;

    for (r = vm_first(&mm->vm); r; r = vm_next(r)) {
        if (r->type == type) {
            n += r->end - r->start;
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

/** The processors started, a bit for each. */
static uint64_t cpu_online_bits(void)
{
    uint64_t mask = 0;
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        if (cpu_table[c].online) {
            mask |= (uint64_t)1 << c;
        }
    }
    return mask;
}

/** Append the processors of `mask` as Linux lists a set of them, ranges
 * with commas between: 0-3, or 0,2-3. */
static void ap_cpu_set(struct procfs_text *t, uint64_t mask)
{
    int c = 0, first = 1;

    while (c < NR_CPUS) {
        int end;

        if (!((mask >> c) & 1u)) {
            c++;
            continue;
        }
        for (end = c; end + 1 < NR_CPUS && ((mask >> (end + 1)) & 1u); end++) {
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
 * Append the processors of `mask` as Linux's `%*pb` writes a mask of
 * nr_cpu_ids bits, here one past the highest processor started: hex words
 * of 32 bits, the highest first and as wide as its bits need, the others
 * eight digits, with commas between, as in "3" for two processors or
 * "ff,ffffffff" for forty.
 */
static void ap_cpu_mask(struct procfs_text *t, uint64_t mask)
{
    uint64_t online = cpu_online_bits();
    int bits = 1, k;

    while (bits < NR_CPUS && (online >> bits) != 0) {
        bits++;
    }
    for (k = (bits - 1) / 32; k >= 0; k--) {
        int width = k == (bits - 1) / 32 ? bits - 32 * k : 32;
        uint32_t word = (uint32_t)(mask >> (32 * k));

        if (width < 32) {
            word &= ((uint32_t)1 << width) - 1u;
        }
        ap_xlow(t, word, (width + 3) / 4);
        if (k > 0) {
            ap_ch(t, ',');
        }
    }
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

/** Append "Name:" and the four ids of `ids`, a tab before each: the real,
 * effective, saved and file-system ones. */
static void ap_ids(struct procfs_text *t, const char *name, const uint32_t ids[4])
{
    int i;

    ap_str(t, name);
    ap_ch(t, ':');
    for (i = 0; i < 4; i++) {
        ap_ch(t, '\t');
        ap_u32(t, ids[i]);
    }
    ap_ch(t, '\n');
}

/** The Uid and Gid lines of `p`: its real, effective, saved and
 * file-system ids, as the reader's user namespace sees them. */
static void ap_cred_ids(struct procfs_text *t, const process_t *p)
{
    uint32_t ids[4];

    ids[0] = uid_seen(p->ruid);
    ids[1] = uid_seen(p->euid);
    ids[2] = uid_seen(p->suid);
    ids[3] = uid_seen(proc_fsuid(p));
    ap_ids(t, "Uid", ids);
    ids[0] = gid_seen(p->rgid);
    ids[1] = gid_seen(p->egid);
    ids[2] = gid_seen(p->sgid);
    ids[3] = gid_seen(proc_fsgid(p));
    ap_ids(t, "Gid", ids);
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

/** Append the completed timed wakes and their stage times, in microseconds.
 * The big lock keeps the record stable while this snapshot is built. */
static void procfs_waketime(struct procfs_text *t, const process_t *p)
{
    const struct wake_record *w = &p->wake_time;
    unsigned int i;

    ap_str(t, "TimedWakeCount:\t");
    ap_u32(t, w->count);
    ap_str(t, "\nTimedWakeLast:\t");
    for (i = 0; i < WAKE_FIELDS; i++) {
        if (i) {
            ap_ch(t, ' ');
        }
        ap_u32(t, w->last[i]);
    }
    ap_str(t, "\nTimedWakeWorst:\t");
    for (i = 0; i < WAKE_FIELDS; i++) {
        if (i) {
            ap_ch(t, ' ');
        }
        ap_u32(t, w->worst[i]);
    }
    ap_str(t, "\nTimedWakeSum:\t");
    for (i = 0; i < WAKE_FIELDS; i++) {
        if (i) {
            ap_ch(t, ' ');
        }
        ap_u64(t, w->sum[i]);
    }
    ap_ch(t, '\n');
}

/**
 * Count the resident pages of `mm` by Linux's three kinds, for the RssAnon,
 * RssFile and RssShmem of status, by the rule of smaps: a private region's
 * pages are anonymous, but for the unwritten ones of a region read from a
 * file, which are the file's still; a shared region's are the file's. A
 * file's pages are counted as shared memory when the file is in memory (a
 * tmpfs file, a memfd, shared anonymous memory), and as file pages
 * otherwise, device memory among them. This walks the regions' page
 * tables and each frame's anonymous mark, which fork and copy-on-write
 * preserve independently of the dirty bit; the three sum to VmRSS.
 */
static void rss_by_kind(const struct mm *mm, unsigned long *anon, unsigned long *file,
                        unsigned long *shmem)
{
    const vma_t *r;

    *anon = 0;
    *file = 0;
    *shmem = 0;
    for (r = vm_first(&mm->vm); r; r = vm_next(r)) {
        unsigned long present, dirty, own, copies;
        int of_file = r->file || r->type == VM_CODE || r->type == VM_DATA;

        paging_user_count(mm->dir, r->start, r->end, &present, &dirty, &copies);
        own = r->type == VM_SHARED ? 0 : of_file ? copies : present;
        *anon += own;
        if (filemap_in_memory(r->file)) {
            *shmem += present - own;
        } else {
            *file += present - own;
        }
    }
}

/** Append the memory sizes of an address space to its status record. */
static void procfs_status_memory(struct procfs_text *t, const struct mm *mm)
{
    unsigned long size = vm_total(&mm->vm);
    unsigned long rss = mm_resident(mm) * PAGE_SIZE;
    unsigned long anon, file, shmem;

    rss_by_kind(mm, &anon, &file, &shmem);
    ap_kb(t, "VmPeak", size);
    ap_kb(t, "VmSize", size);
    ap_kb(t, "VmLck", vm_locked(&mm->vm));
    ap_kb(t, "VmPin", mm->pinned * PAGE_SIZE);
    ap_kb(t, "VmHWM", mm_resident_peak(mm) * PAGE_SIZE);
    ap_kb(t, "VmRSS", rss);
    ap_kb(t, "RssAnon", anon * PAGE_SIZE);
    ap_kb(t, "RssFile", file * PAGE_SIZE);
    ap_kb(t, "RssShmem", shmem * PAGE_SIZE);
    ap_kb(t, "VmData", mm_bytes(mm, VM_DATA) + mm_bytes(mm, VM_HEAP) + mm_bytes(mm, VM_ANON));
    ap_kb(t, "VmStk", mm_bytes(mm, VM_STACK));
    ap_kb(t, "VmExe", mm_bytes(mm, VM_CODE));
    ap_kb(t, "VmLib", mm_bytes(mm, VM_FILE));
    ap_kb(t, "VmSwap", 0);
}

/**
 * /proc/<pid>/status: the lines of Linux's that programs read, among them
 * the name, the state, the ids, the tracer, the memory in kilobytes, the
 * thread count and the signal sets. The fourth Uid and Gid columns are the
 * file-system ids (`proc_fsuid`, `proc_fsgid`). The capability sets are
 * those capget(2) reports (`cap_word`, `cap_eff_word`).
 * VmPeak is the present size, since the kernel keeps no peak of it; VmHWM
 * is the most pages the address space has had resident. RssAnon, RssFile
 * and RssShmem divide VmRSS as `rss_by_kind` says; Wine's working set and
 * pagefile figures are read from them.
 * Cpus_allowed and its list are the thread's affinity among the processors
 * started, which sched_getaffinity(2) reports.
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
    ap_cred_ids(t, p);
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
        procfs_status_memory(t, mm);
    }
    ap_str(t, "Threads:\t");
    ap_u32(t, p->tg->live);
    ap_str(t, "\nSigQ:\t0/");
    ap_u32(t, KSIG_MAX);
    ap_ch(t, '\n');
    ap_signal_sets(t, p);
    caps = (uint64_t)cap_eff_word(p, 1) << 32 | cap_eff_word(p, 0);
    ap_set(t, "CapInh", (uint64_t)p->cap_inh[1] << 32 | p->cap_inh[0]);
    ap_set(t, "CapPrm", caps | (uint64_t)cap_word(p, 1) << 32 | cap_word(p, 0));
    ap_set(t, "CapEff", caps);
    ap_set(t, "CapBnd", (uint64_t)(CAP_HIGH_WORD & ~p->bset_lost[1]) << 32 | ~p->bset_lost[0]);
    ap_set(t, "CapAmb", (uint64_t)p->cap_amb[1] << 32 | p->cap_amb[0]);
    ap_str(t, "NoNewPrivs:\t");
    ap_u32(t, p->no_new_privs);
    ap_str(t, "\nSeccomp:\t");
    ap_u32(t, p->seccomp_mode);
    ap_str(t, "\nSeccomp_filters:\t");
    ap_u32(t, seccomp_filter_count(p));
    ap_str(t, "\nCpus_allowed:\t");
    ap_cpu_mask(t, p->affinity & cpu_online_bits());
    ap_str(t, "\nCpus_allowed_list:\t");
    ap_cpu_set(t, p->affinity & cpu_online_bits());
    ap_ch(t, '\n');
    ap_str(t, "voluntary_ctxt_switches:\t");
    ap_ulong(t, p->nvcsw);
    ap_str(t, "\nnonvoluntary_ctxt_switches:\t");
    ap_ulong(t, p->nivcsw);
    ap_ch(t, '\n');
    procfs_waketime(t, p);
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
 * columns. The nine the kernel keeps (`rlim`) are the process's, and the
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
                {"Max locked memory", RLIM_MEMLOCK, "bytes"},
                {"Max address space", RLIM_AS, "bytes"},
                {"Max file locks", -1, "locks"},
                {"Max pending signals", -1, "signals"},
                {"Max msgqueue size", -1, "bytes"},
                {"Max nice priority", RLIM_NICE, 0},
                {"Max realtime priority", RLIM_RTPRIO, 0},
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
    case SYNTH_PROC_NUMA_MAPS:
        return "numa_maps";
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
    case SYNTH_PROC_CHILDREN:
        return "children";
    case SYNTH_PROC_SYSCALL:
        return "syscall";
    case SYNTH_PROC_WCHAN:
        return "wchan";
    case SYNTH_PROC_SCHEDSTAT:
        return "schedstat";
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
    case SYNTH_PROC_ACPI_CALL:
        return "/proc/acpi/call";
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
    case SYNTH_PROC_LOCKWAIT:
        return "/proc/fs/lockwait";
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

/** Append to `t` the target of `e` when it is open on a file that no path
 * names, "/memfd:NAME (deleted)" for a memfd and "/secretmem (deleted)" for
 * a file of memfd_secret, as Linux shows them: 1, or 0 for any other. */
static int memfd_target(struct procfs_text *t, const fd_entry_t *e)
{
    if (!e->ofd || !e->ofd->vmnt || e->ofd->vmnt->type != &vfs_tmpfs_type) {
        return 0;
    }
    if (tmpfs_secret(e->ofd->vnode)) {
        ap_str(t, "/secretmem (deleted)");
        return 1;
    }
    if (!tmpfs_label(e->ofd->vnode)) {
        return 0;
    }
    ap_str(t, "/memfd:");
    ap_str(t, tmpfs_label(e->ofd->vnode));
    ap_str(t, " (deleted)");
    return 1;
}

/**
 * Append the /proc/<pid>/fd/<fd> target of a file or directory descriptor to
 * `t`: the absolute path of a KRFFS node, "/memfd:NAME (deleted)" for a memfd
 * and "/secretmem (deleted)" for a file of memfd_secret as Linux shows them,
 * the present path of a node of another type, and "/" for the root directory, a
 * stale descriptor or a path the heap had no room to record. A KRFFS path is
 * rebuilt, in a heap buffer of VFS_PATH_MAX bytes, only from a node still live
 * and reachable, since the node of a stale descriptor, unlinked or relocated,
 * may be on the free list; a heap with no room for the buffer sets `t`'s
 * `nomem`.
 *
 * The present path of a node of a mounted type is what Linux's `d_path` gives:
 * the entry the file was opened by as renames have moved it, or a directory's
 * path as the type rebuilds it, from the reader's root, with " (deleted)" once
 * the entry is gone (`exe_file_path`). A type that keeps neither gives the path
 * the file was opened by. Reading the present path may sleep; `e` is not read
 * after it.
 */
static void procfs_file_target(struct procfs_text *t, const fd_entry_t *e)
{
    struct open_file *file = e->ofd;
    int deleted = 0;
    char *now = NULL;

    if (e->node && e->node->type == KRFFS_RESERVED_NODE) {
        char *path = kmalloc(VFS_PATH_MAX);

        if (!path) {
            t->nomem = 1;
        } else if (krffs_node_path(e->node, path, VFS_PATH_MAX) == 0 &&
                   krffs_resolve(path) == e->node) {
            ap_str(t, path);
        } else {
            ap_ch(t, '/');
        }
        kfree(path);
    } else if (memfd_target(t, e)) {
    } else if (file && file->vmnt && (file->name || file->vmnt->type->ops->path) &&
               ((now = exe_file_path(file, &deleted)) != NULL || (deleted && file->dirpath))) {
        ap_str(t, now ? now : file->dirpath);
        if (deleted) {
            ap_str(t, " (deleted)");
        }
        kfree(now);
    } else if (file && file->vmnt && file->dirpath) {
        ap_str(t, file->dirpath);
    } else {
        ap_ch(t, '/');
    }
}

/**
 * Append to `t` the /proc link target of the anonymous descriptor `e`: for
 * a message queue /NAME, the path of its file within the mqueue file
 * system, with " (deleted)" once mq_unlink has taken the name; for a
 * namespace "KIND:[INUM]", as nsfs's; and anon_inode:NAME for any other.
 */
static void anon_target(struct procfs_text *t, const fd_entry_t *e)
{
    int unlinked;
    const char *qname = mq_fd_name(e, &unlinked);

    if (qname) {
        ap_ch(t, '/');
        ap_str(t, qname);
        if (unlinked) {
            ap_str(t, " (deleted)");
        }
        return;
    }
    if (!nsfs_ns(e)) {
        ap_str(t, "anon_inode:");
    }
    ap_str(t, e->priv ? ((const struct anon_obj *)e->priv)->name : "[unknown]");
}

static void mount_reader_root(char *prefix);
static const char *path_seen(const char *path, const char *prefix);

/**
 * Append to `t` the /proc link target of the synthetic directory `e`: the
 * path from the reader's root by which it was opened, when it was opened by
 * a path, and otherwise the path its kind and owner give.
 */
static void synth_dir_target(struct procfs_text *t, const fd_entry_t *e)
{
    struct synth_dir *sd = (struct synth_dir *)e->priv;
    char path[64];

    if (e->ofd && e->ofd->dirpath) {
        char prefix[VFS_COVERED_PATH_MAX];
        const char *seen;

        mount_reader_root(prefix);
        seen = path_seen(e->ofd->dirpath, prefix);
        ap_str(t, seen ? seen : e->ofd->dirpath);
    } else if (sd) {
        synth_dir_path(sd->kind, sd->pid, sd->tid, path, (int)sizeof(path));
        ap_str(t, path);
    }
}

/**
 * Append to `t` the /proc link target of the descriptor `e` of a file of
 * /proc or /sys, rebuilt from its origin: a global file's path, a file of
 * /proc/net, a sysfs attribute, a sysctl, or a file of a process's or a
 * task's directory, with the ids the reader sees.
 */
static void proc_file_target(struct procfs_text *t, const fd_entry_t *e)
{
    struct procfs_file *pf = (struct procfs_file *)e->priv;
    const char *path;

    if (!pf) {
        return;
    }
    path = procfs_global_fd_path(pf->origin_file);
    if (path) {
        ap_str(t, path);
        return;
    }
    if (synth_net_name(pf->origin_file)) {
        ap_str(t, "/proc/net/");
        ap_str(t, synth_net_name(pf->origin_file));
        return;
    }
    if (pf->origin_file == SYNTH_SYS_UEVENT) {
        char where[64];
        int node = pf->origin_task >> 8;

        synth_sys_path(node, pf->origin_pid, where, (int)sizeof(where));
        ap_str(t, where);
        ap_ch(t, '/');
        ap_str(t, sysfs_attr(node, pf->origin_task & 255) ? sysfs_attr(node, pf->origin_task & 255)
                                                          : "uevent");
        return;
    }
    if (pf->origin_file == SYNTH_PROC_SYSCTL) {
        char where[96];

        synth_dir_path(VNODE_PROC_SYS_DIR, pf->origin_pid, 0, where, (int)sizeof(where));
        ap_str(t, where);
        return;
    }
    ap_str(t, "/proc/");
    if (pf->origin_task != 0) {
        ap_u32(t, pid_seen((uint32_t)pf->origin_task));
        ap_str(t, "/task/");
    }
    ap_u32(t, pid_seen((uint32_t)pf->origin_pid));
    ap_ch(t, '/');
    ap_str(t, procfs_pid_file_name(pf->origin_file));
}

/**
 * The target of each kind of descriptor is the one Linux's /proc gives: the
 * path of a file or directory (`procfs_file_target`), /dev/console,
 * /dev/NAME for a device, /dev/pts/N, which musl's ttyname_r matches,
 * /dev/ptmx, pipe:[N], socket:[N], and for an anonymous descriptor the
 * target `anon_target` gives. A socket names its own inode, the same number
 * `/proc/net/tcp` and `fstat` report, so one socket is one thing to a
 * program reading all three. A synthetic directory has its path, one opened
 * through a bind of /proc or a proc mounted elsewhere the path from the
 * reader's root that reached it (`path_seen`), a descriptor of a file of
 * /proc or /sys the path of that file, rebuilt from its origin, and an O_PATH
 * descriptor of a /dev or /proc node the path it was opened by. The
 * framebuffer and the mouse have no name, and show "anon_inode:[device]". A
 * descriptor that is closed, whose release waits for another thread's call
 * or is under way, has no link, as it has no entry in the directory's
 * listing and no lookup finds it.
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
        anon_target(t, e);
        break;
    case FD_TYPE_FILE:
    case FD_TYPE_DIR:
        procfs_file_target(t, e);
        break;
    case FD_TYPE_SYNTH_DIR:
        synth_dir_target(t, e);
        break;
    case FD_TYPE_PROC:
        proc_file_target(t, e);
        break;
    case FD_TYPE_SYNTH_PATH:
        ap_str(t, e->ofd && e->ofd->dirpath ? e->ofd->dirpath : "/");
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
    return PROCFS_INO_NET + (uint32_t)synth_net_slot(origin);
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

uint32_t procfs_ino_acpidir(void)
{
    return PROCFS_INO_ACPI;
}

uint32_t procfs_ino_acpicall(void)
{
    return PROCFS_INO_ACPI_CALL;
}

uint32_t procfs_ino_fsdir(void)
{
    return PROCFS_INO_FS;
}

uint32_t procfs_ino_lockwait(void)
{
    return PROCFS_INO_LOCKWAIT;
}

uint32_t procfs_fd_ino(struct fd_entry *e)
{
    struct procfs_file *pf;
    const struct procfs_global *g;

    if (!e || e->type != FD_TYPE_PROC || !e->priv) {
        return 0;
    }
    pf = (struct procfs_file *)e->priv;
    g = procfs_global_find(-1, pf->origin_file);
    if (g) {
        return g->ino;
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
    if (pf->origin_file == SYNTH_PROC_SYSCTL) {
        return PROCFS_INO_SYS + (uint32_t)pf->origin_pid;
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
        ap_u64_width(t, kib, 11);
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
 * give it. They are 64 bits wide, copied inside `net_lock` (netdev.h) so
 * that none is read half updated. Seven more come from the card, through
 * the driver's `counters`, in the columns Linux fills from a card's own
 * tallies: frames the card missed add to the receive drops, alignment errors
 * are the frame column, multicasts their own, and on the transmit side
 * errors, underruns (fifo), collisions and the transmissions the card
 * aborted (carrier). A card that keeps no such counts leaves them zero. The
 * two compressed columns and the receive fifo are zero: nothing here has
 * them.
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
        uint64_t v[7]; /* the interface's counts, as netdev.h orders them */
        uint32_t flags;

        if (nd->ops->counters && !nd->ops->counters(nd->dev, &card)) {
            card.rx_missed = card.rx_frame = card.rx_multicast = 0;
            card.tx_errors = card.tx_fifo = card.tx_collisions = 0;
            card.tx_carrier = 0;
        }
        flags = net_lock();
        v[0] = nd->rx_packets;
        v[1] = nd->rx_bytes;
        v[2] = nd->rx_errors;
        v[3] = nd->rx_dropped;
        v[4] = nd->tx_packets;
        v[5] = nd->tx_bytes;
        v[6] = nd->tx_dropped;
        net_unlock(flags);
        ap_str(t, "  ");
        ap_str(t, nd->name);
        ap_str(t, ": ");
        ap_u64(t, v[1]);
        ap_ch(t, ' ');
        ap_u64(t, v[0]);
        ap_ch(t, ' ');
        ap_u64(t, v[2]);
        ap_ch(t, ' ');
        ap_u64(t, v[3] + card.rx_missed);
        /* fifo, frame, compressed, multicast */
        ap_str(t, " 0 ");
        ap_u32(t, card.rx_frame);
        ap_str(t, " 0 ");
        ap_u32(t, card.rx_multicast);
        ap_ch(t, ' ');
        ap_u64(t, v[5]);
        ap_ch(t, ' ');
        ap_u64(t, v[4]);
        ap_ch(t, ' ');
        ap_u32(t, card.tx_errors);
        ap_ch(t, ' ');
        ap_u64(t, v[6]);
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
 * has them, the hundredths cut, not rounded. */
static void ap_ticks_as_seconds(struct procfs_text *t, uint64_t ticks)
{
    uint32_t rest, hundredths;

    ap_u64(t, tick_div(ticks, TICK_HZ, &rest));
    hundredths = rest * 100u / TICK_HZ;
    ap_ch(t, '.');
    ap_ch(t, (char)('0' + hundredths / 10u));
    ap_ch(t, (char)('0' + hundredths % 10u));
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
    uint64_t user, busy, idle;

    sched_cpu_ticks(0, &user, &busy, &idle);
    ap_ticks_as_seconds(t, busy + idle);
    ap_ch(t, ' ');
    ap_ticks_as_seconds(t, sched_idle_ticks());
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

/** Append a /proc/stat processor line's columns after its name, in the
 * `clock_t` of USER_HZ that Linux's count: `user` ticks, no nice, `busy`
 * less `user` as system, and `idle`. */
static void ap_cpu_ticks(struct procfs_text *t, uint64_t user, uint64_t busy, uint64_t idle)
{
    ap_str(t, " ");
    ap_u64(t, TICKS_TO_CLOCK(user));
    ap_str(t, " 0 ");
    ap_u64(t, TICKS_TO_CLOCK(busy - user));
    ap_ch(t, ' ');
    ap_u64(t, TICKS_TO_CLOCK(idle));
    ap_str(t, " 0 0 0 0 0 0\n");
}

/** Render what the wireless queues counted, summed over the radios: the
 * frames a full transmit queue refused, by kind, and the looks at a full
 * receive queue; the longest wait of any radio's received frames; the
 * completions lost; the renewals of a pairwise key that ended in leaving the
 * AP, by cause; and the silent ones that the gateway's answer saved. */
static void procfs_wlan_refused(struct procfs_text *t)
{
    struct wlan_counters c;
    struct wlan *wlan;
    uint32_t tcp = 0, ack = 0, other = 0, held = 0, wait = 0, lost = 0;
    uint32_t rescues[WLAN_RESCUES] = {0, 0, 0}, saved = 0, far = 0;
    unsigned i, r;

    for (i = 0; (wlan = wlan_at(i)) != 0; ++i) {
        wlan_get_counters(wlan, &c);
        tcp += c.tx_refused_tcp;
        ack += c.tx_refused_ack;
        other += c.tx_refused_other;
        held += c.rx_held;
        wait = c.rx_wait_most > wait ? c.rx_wait_most : wait;
        lost += c.tx_status_lost;
        for (r = 0; r < WLAN_RESCUES; ++r)
            rescues[r] += c.rescues[r];
        saved += c.rescue_saved;
        far += c.rx_old_far;
    }
    ap_str(t, "\nwlan_refused_tcp ");
    ap_u32(t, tcp);
    ap_str(t, "\nwlan_refused_ack ");
    ap_u32(t, ack);
    ap_str(t, "\nwlan_refused_other ");
    ap_u32(t, other);
    ap_str(t, "\nwlan_rx_held ");
    ap_u32(t, held);
    ap_str(t, "\nwlan_rx_wait_most ");
    ap_u32(t, wait);
    ap_str(t, "\nwlan_tx_status_lost ");
    ap_u32(t, lost);
    ap_str(t, "\nwlan_rescue_key ");
    ap_u32(t, rescues[WLAN_RESCUE_KEY]);
    ap_str(t, "\nwlan_rescue_message4 ");
    ap_u32(t, rescues[WLAN_RESCUE_MESSAGE4]);
    ap_str(t, "\nwlan_rescue_silent ");
    ap_u32(t, rescues[WLAN_RESCUE_SILENT]);
    ap_str(t, "\nwlan_rescue_saved ");
    ap_u32(t, saved);
    ap_str(t, "\nwlan_rx_old_far ");
    ap_u32(t, far);
}

/* The entries `procfs_entry_holds` lists, the longest first. */
#define PROCFS_HOLD_ENTRIES 16

/** Append to `t` the `bkl_hold_entry` lines: the entries that held the big
 * lock longest since boot, as many as `PROCFS_HOLD_ENTRIES`, each with its
 * kind, its number, its holds and their sum in microseconds. */
static void procfs_entry_holds(struct procfs_text *t)
{
    static unsigned char shown[BKL_ENTRY_KEYS];
    uint64_t us, best_us;
    uint32_t holds, best_holds = 0;
    int n, key, best;

    k_memset(shown, 0, sizeof(shown));
    for (n = 0; n < PROCFS_HOLD_ENTRIES; n++) {
        best = -1;
        best_us = 0;
        for (key = 0; key < BKL_ENTRY_KEYS; key++) {
            smp_entry_hold(key, &us, &holds);
            if (!shown[key] && holds != 0 && (best < 0 || us > best_us)) {
                best = key;
                best_us = us;
                best_holds = holds;
            }
        }
        if (best < 0) {
            break;
        }
        shown[best] = 1;
        ap_str(t, "\nbkl_hold_entry ");
        if (best < BKL_ENTRY_SYSCALLS) {
            ap_str(t, "syscall ");
            ap_u32(t, (uint32_t)best);
        } else if (best < BKL_ENTRY_KEYS - 1) {
            ap_str(t, "vector ");
            ap_u32(t, (uint32_t)(best - BKL_ENTRY_SYSCALLS));
        } else {
            ap_str(t, "syscall -1");
        }
        ap_ch(t, ' ');
        ap_u32(t, best_holds);
        ap_ch(t, ' ');
        ap_u64(t, best_us);
    }
}

/** Append live KVM objects on x86-64, each counted under the big lock, and
 * the work KVM did for guests without an exit to their callers. */
static void procfs_kvm_counts(struct procfs_text *t)
{
#if defined(__x86_64__) && !defined(RACH2_HOSTTEST)
    static const char *const names[KVM_COUNT_TOTAL] = {
        "kvm_vms ",          "kvm_vcpus ",    "kvm_slots ",        "kvm_tdp_pages ",
        "kvm_run_mappings ", "kvm_gpa_maps ", "kvm_gpa_map_pages "};
    int i;

    for (i = 0; i < KVM_COUNT_TOTAL; i++) {
        ap_str(t, names[i]);
        ap_u32(t, kvm_resource_count((enum kvm_resource)i));
        ap_ch(t, '\n');
    }
    ap_str(t, "kvm_ioeventfd_signals ");
    ap_u32(t, kvm_event_count(KVM_EVENT_IOEVENTFD));
    ap_str(t, "\nkvm_irqfd_raises ");
    ap_u32(t, kvm_event_count(KVM_EVENT_IRQFD));
    ap_ch(t, '\n');
#else
    (void)t;
#endif
}

/** Append VT-d faults and live VFIO resources on x86-64, under the big lock. */
static void procfs_assignment_counts(struct procfs_text *t)
{
#if defined(__x86_64__) && !defined(RACH2_HOSTTEST)
    static const char *const names[5] = {"vfio_groups ", "vfio_devices ", "vfio_dma_mappings ",
                                         "vfio_dma_pages ", "vfio_dma_io_pages "};
    uint32_t counts[5];
    int i;

    vfio_resource_counts(counts);
    for (i = 0; i < 5; i++) {
        ap_str(t, names[i]);
        ap_u32(t, counts[i]);
        ap_ch(t, '\n');
    }
    ap_str(t, "kvm_exits ");
    ap_u64(t, kvm_exit_count());
    ap_str(t, "\niommu_dma_faults ");
    ap_u64(t, iommu_dma_faults());
    ap_str(t, "\niommu_irq_faults ");
    ap_u64(t, iommu_irq_faults());
    ap_str(t, "\nmsi_vectors_free ");
    ap_u32(t, (uint32_t)isr_free_vector_count());
    ap_ch(t, '\n');
#else
    (void)t;
#endif
}

/** Append the retained holds completed during the longest big-lock wait. */
static void procfs_wait_holds(struct procfs_text *t, uint32_t holds)
{
    uint32_t i, vector;
    uint64_t us;
    int32_t nr;

    for (i = 0; i < holds && i < BKL_WAIT_HOLDS; i++) {
        smp_wait_hold(i, &us, &vector, &nr);
        ap_str(t, "\nbkl_wait_max_hold ");
        ap_u32(t, vector);
        ap_ch(t, ' ');
        if (nr < 0) {
            ap_str(t, "-1");
        } else {
            ap_u32(t, (uint32_t)nr);
        }
        ap_ch(t, ' ');
        ap_u64(t, us);
    }
}

/** The counts of the kernel's own work for `/proc/stat`, from
 * `process_pid_steps` to the last `bkl_hold_entry`, each on a line of
 * its own. */
static void procfs_work_counts(struct procfs_text *t)
{
    uint64_t total, longest, held, age;
    uint32_t vector, holds;
    int32_t nr;
    uintptr_t pc[BKL_SAMPLE_PCS];
    int i, m;

    smp_wait_stats(&total, &longest, &holds);
    smp_hold_stats(&held, &vector, &nr);
    smp_hold_sample(&age, pc);
    ap_str(t, "process_pid_steps ");
    ap_u32(t, proc_work_count(0));
    ap_str(t, "\nprocess_uid_steps ");
    ap_u32(t, proc_work_count(1));
    ap_str(t, "\nprocess_timer_steps ");
    ap_u32(t, proc_work_count(2));
    ap_str(t, "\nprocess_timer_passes ");
    ap_u32(t, proc_work_count(3));
    ap_str(t, "\ntimer_passes_late ");
    ap_u32(t, timer_late_passes());
    ap_str(t, "\ntlb_shootdowns ");
    ap_u32(t, smp_tlb_count(0));
    ap_str(t, "\ntlb_shootdown_ipis ");
    ap_u32(t, smp_tlb_count(1));
    ap_str(t, "\nscheduler_picks ");
    ap_u32(t, sched_work_count(0));
    ap_str(t, "\nscheduler_steps ");
    ap_u32(t, sched_work_count(1));
    ap_str(t, "\nvm_gap_steps ");
    ap_u32(t, vm_gap_steps());
    ap_str(t, "\nclock_calls ");
    ap_u32(t, clock_calls());
    ap_str(t, "\nvdso_tsc ");
    ap_u32(t, (uint32_t)vclock_running());
    ap_str(t, "\nbkl_wait_us ");
    ap_u64(t, total);
    ap_str(t, "\nbkl_wait_max_us ");
    ap_u64(t, longest);
    ap_str(t, "\nbkl_wait_max_holds ");
    ap_u32(t, holds);
    procfs_wait_holds(t, holds);
    ap_str(t, "\nbkl_hold_max_us ");
    ap_u64(t, held);
    ap_str(t, "\nbkl_hold_max_entry ");
    ap_u32(t, vector);
    ap_ch(t, ' ');
    if (nr < 0) {
        ap_str(t, "-1");
    } else {
        ap_u32(t, (uint32_t)nr);
    }
    ap_str(t, "\nbkl_hold_sample_us ");
    ap_u64(t, age);
    ap_str(t, "\nbkl_hold_sample_pc");
    for (i = 0; i < BKL_SAMPLE_PCS && pc[i]; i++) {
        ap_ch(t, ' ');
        ap_xlow(t, (unsigned long)pc[i], 1);
    }
    for (m = 0; m < BKL_SAMPLE_MARKS; m++) {
        smp_hold_early(m, &age, pc);
        ap_str(t, "\nbkl_hold_max_mark ");
        ap_u32(t, 1u << m);
        ap_ch(t, ' ');
        ap_u64(t, age);
        for (i = 0; i < BKL_SAMPLE_PCS && pc[i]; i++) {
            ap_ch(t, ' ');
            ap_xlow(t, (unsigned long)pc[i], 1);
        }
    }
    procfs_entry_holds(t);
    ap_ch(t, '\n');
}

/**
 * Render /proc/stat: the tick counters, the interrupt total, the boot epoch,
 * the process counts, and this kernel's own counters: the sleeps a signal
 * cut short (`process_sleeps_cut_short`), the longest delays since boot
 * (latency.h), and the loopback ring's.
 *
 * `clock_calls` is the clock's system calls made since boot (vdso.h), and
 * `vdso_tsc` is 1 when the vDSO reads the clock from the TSC and those
 * calls are not needed, 0 when its functions make them.
 *
 * `loopback_dropped` is the datagrams the loopback ring had no room for, which
 * the ring logs once and counts thereafter, and `loopback_peak` is the most it
 * has held at once, which says how much room it has over what one system
 * call can put in it; the `loopback_delivered_` counts are its datagrams
 * delivered by the tick, by the idle wait and at the exit of a system call,
 * and `loopback_left_idle` is those a tick found on it while every processor
 * was halted in an idle wait, which waited for the timer.
 *
 * The `namecache_` counts are the name cache's (dcache.h): the resolver's
 * lookups it answered and those it passed to the file system, each a read of
 * a directory, since boot, and the entries it has now, and of those the
 * negative ones.
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
    struct dcache_counts names;
    uint64_t user = 0, busy = 0, idle = 0, cu, cb, ci;
    const process_t *p;
    uint32_t running = 0;
    uint32_t blocked = 0;
    int c;

    for (c = 0; c < NR_CPUS; c++) {
        sched_cpu_ticks(c, &cu, &cb, &ci);
        user += cu;
        busy += cb;
        idle += ci;
    }
    ap_str(t, "cpu ");
    ap_cpu_ticks(t, user, busy, idle);
    for (c = 0; c < NR_CPUS; c++) {
        if (cpu_table[c].online) {
            ap_str(t, "cpu");
            ap_u32(t, (uint32_t)c);
            sched_cpu_ticks(c, &cu, &cb, &ci);
            ap_cpu_ticks(t, cu, cb, ci);
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
    procfs_work_counts(t);
    procfs_kvm_counts(t);
    procfs_assignment_counts(t);
    ap_str(t, "latency_timer_us ");
    ap_u32(t, latency_max(LAT_TIMER));
    ap_str(t, "\nlatency_disk_us ");
    ap_u32(t, latency_max(LAT_DISK));
    ap_str(t, "\nlatency_diskwait_us ");
    ap_u32(t, latency_max(LAT_DISK_WAIT));
    ap_str(t, "\nlatency_console_us ");
    ap_u32(t, latency_max(LAT_CONSOLE));
    procfs_wlan_refused(t);
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
    ap_str(t, "\nloopback_left_idle ");
    ap_u32(t, ip_loop_left_idle());
    dcache_counts(&names);
    ap_str(t, "\nnamecache_hits ");
    ap_u32(t, names.hits);
    ap_str(t, "\nnamecache_misses ");
    ap_u32(t, names.misses);
    ap_str(t, "\nnamecache_entries ");
    ap_u32(t, names.entries);
    ap_str(t, "\nnamecache_negative ");
    ap_u32(t, names.negative);
    ap_str(t, "\next2_node_visits ");
    ap_u32(t, ext2_node_visits());
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
 * MemAvailable is the free count and the two together. SwapCached, which
 * IGT adds to Cached, is 0, as SwapTotal and SwapFree are, since nothing is
 * swapped. HeapPages and
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
    ap_str(t, "SwapCached:            0 kB\n");
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
 * with the root port it is on, its speed, what it says it is, the class it
 * was driven as and, for a keyboard or mouse, the reports it has delivered;
 * then one line for each stick that is a disk, with its node, what the drive
 * says it is, its size and whether it is still there; on a machine that has
 * had a stick, a line with how many names are free and how many sticks have
 * been given up since boot; and, while the stack has not finished with a
 * device present (`usb_core_busy`), a last line with how many. A machine with
 * no controller, or one booted without USB, has the file, empty.
 *
 * The count is taken before the lines. Each line is read with interrupts off
 * for that line alone, so the pump can run between two of them and finish a
 * device whose line was already passed; counted after, that device would be
 * neither listed nor busy. Counted first, a reading without the `busy` line
 * lists every device that was running when it began, since a device not in
 * progress then needs several passes of the pump before it runs.
 */
static int procfs_gen_usb(struct procfs_text *t)
{
    int i;
    int busy = usb_core_busy();
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
    if (busy > 0) {
        ap_str(t, "busy ");
        ap_u32(t, (uint32_t)busy);
        ap_str(t, "\n");
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

/** The path of the reader's root from the root of the mount table into
 * `prefix`, of VFS_COVERED_PATH_MAX bytes, as an empty string for the root
 * of the mount table, or when the path cannot be had, so that the table is
 * shown whole. */
static void mount_reader_root(char *prefix)
{
    struct vfs_ref root;
    int rc = -ENOENT;

    prefix[0] = '\0';
    if (!current_proc || !current_proc->fs || !current_proc->fs->root_mnt) {
        return;
    }
    root.mnt = current_proc->fs->root_mnt;
    root.node = current_proc->fs->root_node;
    if (root.node == root.mnt->root) {
        k_strncpy(prefix, root.mnt->covered_path, VFS_COVERED_PATH_MAX);
        rc = 0;
    } else if (vfs_enter(root.mnt) == 0) {
        vfs_ref_hold(&root);
        vfs_ns_lock(root.mnt);
        rc = vfs_node_path(root.mnt, root.node, prefix, VFS_COVERED_PATH_MAX);
        vfs_ns_unlock(root.mnt);
        vfs_ref_release(&root);
    }
    if (rc != 0 || (prefix[0] == '/' && prefix[1] == '\0')) {
        prefix[0] = '\0';
    }
}

/** The path `path`, from the root of the mount table, as a reader whose
 * root's path is `prefix` (`mount_reader_root`) sees it, or NULL for a place
 * outside that root. */
static const char *path_seen(const char *path, const char *prefix)
{
    uint32_t n = 0;

    if (prefix[0] == '\0') {
        return path;
    }
    while (prefix[n] && path[n] == prefix[n]) {
        n++;
    }
    if (prefix[n] || (path[n] != '/' && path[n] != '\0')) {
        return NULL;
    }
    return path[n] ? path + n : "/";
}

/** The mount point of `m` as a reader whose root's path is `prefix` sees it
 * (`path_seen`), or NULL for a mount outside that root, which the reader
 * cannot reach and the tables leave out, as Linux does. */
static const char *mount_point_seen(const struct vfs_mount *m, const char *prefix)
{
    return path_seen(m->covered_path, prefix);
}

/** The index of the listed mount made next after the one whose `seq` is
 * `after`, as `procfs_mount_listed` lists them, or -1 when none is; with
 * `first`, the one made first. The tables list the mounts in the order
 * they were made, so that a mount over another's root follows it. */
static int mount_next_made(uint32_t after, int first)
{
    int i, best = -1;

    for (i = 0; i < vfs_mount_slots(); i++) {
        struct vfs_mount *m = vfs_mount_at(i);

        if (procfs_mount_listed(i, m) && (first || m->seq > after) &&
            (best < 0 || m->seq < vfs_mount_at(best)->seq)) {
            best = i;
        }
    }
    return best;
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
 * that walks this file, then every other mount in the order they were made,
 * each with its source as the caller gave it, its mount point from the
 * reader's root, its type and its options. A mount outside the reader's
 * root is left out.
 */
static int procfs_gen_mounts(struct procfs_text *t)
{
    char prefix[VFS_COVERED_PATH_MAX];
    int i;

    mount_reader_root(prefix);
    for (i = mount_next_made(0, 1); i >= 0; i = mount_next_made(vfs_mount_at(i)->seq, 0)) {
        struct vfs_mount *m = vfs_mount_at(i);
        const char *point = mount_point_seen(m, prefix);

        if (!point) {
            continue;
        }
        ap_str(t, procfs_mount_source(i, m));
        ap_ch(t, ' ');
        ap_str(t, point);
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

uint32_t procfs_mount_id(const struct vfs_mount *m)
{
    int j;

    for (j = 0; m && j < vfs_mount_slots(); j++) {
        if (vfs_mount_at(j) == m) {
            return (uint32_t)j + 1u;
        }
    }
    return 0;
}

uint32_t procfs_synth_mount_id(uint64_t dev, const struct vfs_mount *hint)
{
    const struct vfs_type *type = dev == DEVFS_FSID    ? &vfs_devfs_type
                                  : dev == PROCFS_FSID ? &vfs_procfs_type
                                  : dev == SYSFS_FSID  ? &vfs_sysfs_type
                                                       : NULL;
    int j;

    if (!type) {
        return 0;
    }
    if (hint && hint->type == type) {
        return procfs_mount_id(hint);
    }
    for (j = 0; j < vfs_mount_slots(); j++) {
        const struct vfs_mount *m = vfs_mount_at(j);

        if (m && m->type == type && vfs_mount_seen(m)) {
            return (uint32_t)j + 1u;
        }
    }
    return 0;
}

/**
 * Render /proc/<pid>/mountinfo, the table util-linux's libmount reads, one
 * line for each mount of /proc/mounts in its order, which bubblewrap reads
 * to find the mount it has just made at the path a descriptor of it gives,
 * and so a mount point is a path from the reader's root: the mount's id, its
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
    char prefix[VFS_COVERED_PATH_MAX];
    int i, j;

    mount_reader_root(prefix);
    for (i = mount_next_made(0, 1); i >= 0; i = mount_next_made(vfs_mount_at(i)->seq, 0)) {
        struct vfs_mount *m = vfs_mount_at(i);
        const char *point = mount_point_seen(m, prefix);
        uint32_t dev;
        int parent = 1, base = i;

        if (!point) {
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
        ap_str(t, point);
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
 * Render /proc/fs/lockwait: a line for each kind of sleeping lock, in the
 * order the kinds were first used (`sleep_lock_records`), with its name, the
 * holds of it that crossed a disk wait, the longest of them in microseconds,
 * and that hold's thread ID and name, or 0 and `-` while there was none. The
 * name is last, since a process may put a space in it. The records change
 * only between sleeps, so one pass under the big lock reads them whole.
 */
static int procfs_gen_lockwait(struct procfs_text *t)
{
    const struct sleep_lock_record *r;
    char comm[sizeof(r->longest_comm) + 1];

    for (r = sleep_lock_records(); r; r = r->next) {
        k_memcpy(comm, r->longest_comm, sizeof(r->longest_comm));
        comm[sizeof(r->longest_comm)] = '\0';
        ap_str(t, r->kind);
        ap_ch(t, ' ');
        ap_u32(t, r->crossings);
        ap_ch(t, ' ');
        ap_u32(t, r->longest_us);
        ap_ch(t, ' ');
        ap_u32(t, r->longest_pid);
        ap_ch(t, ' ');
        ap_str(t, comm[0] ? comm : "-");
        ap_ch(t, '\n');
    }
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
 * machine rather than guessed; then the speaker's (`speaker_get_counters`),
 * so that a tone can be counted where it cannot be heard.
 */
static int procfs_gen_console(struct procfs_text *t)
{
    struct console_counters c;
    struct speaker_counters sp;

    tty_console_counters(&c);
    speaker_get_counters(&sp);
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
    ap_counter(t, "speaker-tones", sp.tones);
    ap_counter(t, "speaker-sounds", sp.sounds);
    ap_counter(t, "speaker-ends", sp.ends);
    ap_counter(t, "speaker-cuts", sp.cuts);
    ap_counter(t, "speaker-divisor", sp.divisor);
    ap_counter(t, "speaker-ms", sp.ms);
    ap_counter(t, "speaker-beeps", sp.beeps);
    ap_counter(t, "speaker-beep-divider", sp.beep_divider);
    ap_counter(t, "speaker-beep-retries", sp.beep_retries);
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

/* The types /proc/filesystems lists ahead of those mount(2) accepts. */
static const char *const procfs_fixed_types[] = {"devfs", "proc"};

/** The types /proc/filesystems lists: devfs and proc, then every type
 * mount(2) accepts (`vfs_type_at`), KRFFS volumes among them. The type of
 * the KRFFS root is not among them: the root is an image the kernel maps,
 * which mount(2) does not place. */
int procfs_filesystem_at(int idx, const char **name)
{
    const struct vfs_type *ty;
    int fixed = (int)(sizeof(procfs_fixed_types) / sizeof(procfs_fixed_types[0]));

    if (idx < 0) {
        return -1;
    }
    if (idx < fixed) {
        *name = procfs_fixed_types[idx];
        return 1;
    }
    ty = vfs_type_at(idx - fixed);
    if (!ty) {
        return -1;
    }
    *name = ty->name;
    return ty->nodev ? 1 : 0;
}

/** Render /proc/filesystems: a line for each type `procfs_filesystem_at`
 * gives, with `nodev` before one that stands on no device. */
static int procfs_gen_filesystems(struct procfs_text *t)
{
    const char *name;
    int nodev;
    int i;

    for (i = 0; (nodev = procfs_filesystem_at(i, &name)) >= 0; i++) {
        if (nodev) {
            ap_str(t, "nodev");
        }
        ap_ch(t, '\t');
        ap_str(t, name);
        ap_ch(t, '\n');
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

/**
 * Append the endpoint `a`, `port` of a socket of `family` as /proc/net's
 * socket files write one: for AF_INET the address as `ap_ip_hex` writes it,
 * and for AF_INET6 its four 32-bit words each as a little-endian x86 reads
 * it, so ::1 is 00000000000000000000000001000000, an IPv4 address in its
 * mapped form; then a colon and the port in four hex digits.
 */
static void ap_endpoint(struct procfs_text *t, int family, net_addr_t a, uint16_t port)
{
    uint8_t b[16];
    int w;

    if (family == AF_INET6) {
        net_addr_to6(a, b);
        for (w = 0; w < 16; w += 4) {
            ap_hex(t,
                   (uint32_t)b[w + 3] << 24 | (uint32_t)b[w + 2] << 16 | (uint32_t)b[w + 1] << 8 |
                       b[w],
                   8);
        }
    } else {
        ap_ip_hex(t, a.v4);
    }
    ap_ch(t, ':');
    ap_hex(t, port, 4);
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
 * Render /proc/net/tcp, or with `family` AF_INET6 /proc/net/tcp6: the
 * connections of sockets of that family, in the columns netstat parses, or
 * none for a caller outside the first network namespace; -ENOMEM when the
 * heap has no room for the snapshot of the connections. An AF_INET6
 * socket's connection with an IPv4 peer is in tcp6, its addresses mapped.
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
static int procfs_gen_nettcp(struct procfs_text *t, int family)
{
    struct tcp_row *rows;
    int n = tcp_snapshot(&rows);
    int i, sl = 0;

    if (n < 0) {
        return -ENOMEM;
    }
    if (!net_ns_seen()) {
        n = 0;
    }
    ap_str(t, family == AF_INET6 ? PROCFS_NETTCP6_HEADER : PROCFS_NETTCP_HEADER);
    for (i = 0; i < n; i++) {
        struct tcp_row row = rows[i];

        if ((row.family == AF_INET6) != (family == AF_INET6)) {
            continue;
        }
        ap_str(t, sl < 10 ? "   " : sl < 100 ? "  " : sl < 1000 ? " " : "");
        ap_u32(t, (uint32_t)sl++);
        ap_str(t, ": ");
        ap_endpoint(t, family, row.local_ip, row.local_port);
        ap_ch(t, ' ');
        ap_endpoint(t, family, row.remote_ip, row.remote_port);
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
 * Render /proc/net/udp or, for `type` SOCK_RAW, /proc/net/raw, or with
 * `family` AF_INET6 udp6 or raw6: the sockets of that family in the columns
 * of /proc/net/tcp, which netstat and ss parse the same way.
 *
 * The local address is the one the socket bound, 0.0.0.0 or :: for one
 * bound to the unspecified address or not bound at all. The state is
 * Linux's: 1, its TCP_ESTABLISHED, for a connected socket and 7, TCP_CLOSE,
 * for the rest. A raw socket's port is its protocol.
 */
static int procfs_gen_netdgram(struct procfs_text *t, int family, int type)
{
    struct socket_row *rows;
    int n = socket_snapshot(family, type, &rows);
    int i;

    if (n < 0) {
        return -ENOMEM;
    }
    ap_str(t, family == AF_INET6 ? PROCFS_NETUDP6_HEADER : PROCFS_NETUDP_HEADER);
    for (i = 0; i < n; i++) {
        uint16_t port = rows[i].local_port;

        if (type == SOCK_RAW && port == 0) {
            port = 255; /* IPPROTO_RAW, which the ioctl descriptor is opened with */
        }
        ap_str(t, i < 10 ? "    " : i < 100 ? "   " : i < 1000 ? "  " : " ");
        ap_u32(t, (uint32_t)i);
        ap_str(t, ": ");
        ap_endpoint(t, family, rows[i].local_ip, port);
        ap_ch(t, ' ');
        ap_endpoint(t, family, rows[i].connected ? rows[i].remote_ip : net_addr4(0),
                    (uint16_t)(rows[i].connected ? rows[i].remote_port : 0));
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
    int udp = socket_snapshot(AF_INET, SOCK_DGRAM, &rows);
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
              " InDiscards InDelivers OutRequests OutDiscards OutNoRoutes ReasmTimeout ReasmReqds"
              " ReasmOKs ReasmFails FragOKs FragFails FragCreates\nIp: 2");
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
    ap_mib(t, m->ip_reasm_timeout);
    ap_mib(t, m->ip_reasm_reqds);
    ap_mib(t, m->ip_reasm_oks);
    ap_mib(t, m->ip_reasm_fails);
    ap_mib(t, m->ip_frag_oks);
    ap_mib(t, m->ip_frag_fails);
    ap_mib(t, m->ip_frag_creates);
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
    ap_mib(t, TCP_RTO_MIN * 1000u / TICK_HZ); /* ticks as milliseconds */
    ap_mib(t, TCP_RTO_MAX * 1000u / TICK_HZ);
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
    ap_str(t, "\nUdp: InDatagrams NoPorts InErrors OutDatagrams RcvbufErrors\nUdp:");
    ap_mib(t, m->udp_in_datagrams);
    ap_mib(t, m->udp_no_ports);
    ap_mib(t, m->udp_in_errors);
    ap_mib(t, m->udp_out_datagrams);
    ap_mib(t, m->udp_rcvbuf_errors);
    ap_ch(t, '\n');
    return t->len;
}

/**
 * Render /proc/net/netstat: of the TcpExt counters Linux keeps, the ones
 * this stack has (`net_mib`), a line of names and a line of values, in
 * Linux's order, which `nstat` reads by name: the segments of data received
 * again whole, the retransmission timeouts, the connections ended by data
 * past the peer's FIN and by the timeouts, the challenge acknowledgements
 * and SYNs challenged, the answers to unacceptable segments held back, the
 * keepalive probes, and the probes of a larger segment lost and taken.
 */
static int procfs_gen_netnetstat(struct procfs_text *t)
{
    const struct net_mib *m = &net_mib;

    ap_str(t, "TcpExt: DelayedACKLost TCPTimeouts TCPAbortOnData TCPAbortOnTimeout "
              "TCPChallengeACK TCPSYNChallenge TCPACKSkippedSynRecv TCPACKSkippedSeq "
              "TCPACKSkippedChallenge TCPKeepAlive TCPMTUPFail TCPMTUPSuccess\n"
              "TcpExt:");
    ap_mib(t, m->tcp_delayed_ack_lost);
    ap_mib(t, m->tcp_timeouts);
    ap_mib(t, m->tcp_abort_on_data);
    ap_mib(t, m->tcp_abort_on_timeout);
    ap_mib(t, m->tcp_challenge_ack);
    ap_mib(t, m->tcp_syn_challenge);
    ap_mib(t, m->tcp_ack_skipped_syn_recv);
    ap_mib(t, m->tcp_ack_skipped_seq);
    ap_mib(t, m->tcp_ack_skipped_challenge);
    ap_mib(t, m->tcp_keepalive);
    ap_mib(t, m->tcp_mtup_fail);
    ap_mib(t, m->tcp_mtup_success);
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

/** Append `v` in decimal and spaces after it to `width` places: printf's
 * "%-Nu" for a column of N. */
static void ap_u32_left(struct procfs_text *t, uint32_t v, int width)
{
    uint32_t w = 1, rest;

    for (rest = v; rest >= 10u; rest /= 10u) {
        w++;
    }
    ap_u32(t, v);
    while ((int)w++ < width) {
        ap_ch(t, ' ');
    }
}

/**
 * Render /proc/net/packet: Linux's heading and a row for each bound packet
 * socket (`packet_snapshot`), in Linux's columns, which `ss -0` reads.
 *
 * The socket's kernel address is zeros, as Linux shows it to a reader
 * without privilege, as wide as a pointer. The type is SOCK_DGRAM, the only
 * one; the protocol is the EtherType in hexadecimal; R, running, is 1 for a
 * socket with a protocol, which is offered frames, and 0 for one without;
 * and Rmem is the bytes queued to be read. A socket not yet bound is not
 * listed, where Linux lists every packet socket.
 */
static int procfs_gen_netpacket(struct procfs_text *t)
{
    struct packet_row *rows;
    int n = packet_snapshot(&rows);
    int i;

    if (n < 0) {
        return -ENOMEM;
    }
    ap_left(t, "sk", (int)sizeof(void *) * 2 + 1);
    ap_str(t, "RefCnt Type Proto  Iface R Rmem   User   Inode\n");
    for (i = 0; i < n; i++) {
        ap_xlow(t, 0, (int)sizeof(void *) * 2);
        ap_ch(t, ' ');
        ap_u32_left(t, (uint32_t)rows[i].refs, 6);
        ap_ch(t, ' ');
        ap_u32_left(t, SOCK_DGRAM, 4);
        ap_ch(t, ' ');
        ap_xlow(t, rows[i].protocol, 4);
        ap_str(t, "   ");
        ap_u32_left(t, (uint32_t)rows[i].ifindex, 5);
        ap_str(t, rows[i].protocol != 0 ? " 1 " : " 0 ");
        ap_u32_left(t, rows[i].rmem, 6);
        ap_ch(t, ' ');
        ap_u32_left(t, rows[i].uid, 6);
        ap_ch(t, ' ');
        ap_u32_left(t, rows[i].inode, 6);
        ap_ch(t, '\n');
    }
    kfree(rows);
    return t->len;
}

/** Append the /proc/net/snmp6 line of the counter `name` of value `v`:
 * the name left-aligned in 32 columns, a tab and the value, as Linux
 * writes it. */
static void ap_mib6(struct procfs_text *t, const char *name, uint32_t v)
{
    ap_left(t, name, 32);
    ap_ch(t, '\t');
    ap_u32(t, v);
    ap_ch(t, '\n');
}

/**
 * Render /proc/net/snmp6: of the IPv6, ICMPv6 and UDP counters Linux keeps,
 * the ones this stack has (`net_mib6`), a line each, in Linux's order, which
 * `nstat` reads by name. Ip6OutForwDatagrams, Icmp6OutPktTooBigs and the
 * messages only a router sends are 0, since this machine forwards nothing.
 */
static int procfs_gen_netsnmp6(struct procfs_text *t)
{
    const struct net_mib6 *m = &net_mib6;

    ap_mib6(t, "Ip6InReceives", m->ip6_in_receives);
    ap_mib6(t, "Ip6InHdrErrors", m->ip6_in_hdr_errors);
    ap_mib6(t, "Ip6InAddrErrors", m->ip6_in_addr_errors);
    ap_mib6(t, "Ip6InUnknownProtos", m->ip6_in_unknown_protos);
    ap_mib6(t, "Ip6InDiscards", m->ip6_in_discards);
    ap_mib6(t, "Ip6InDelivers", m->ip6_in_delivers);
    ap_mib6(t, "Ip6OutForwDatagrams", 0);
    ap_mib6(t, "Ip6OutRequests", m->ip6_out_requests);
    ap_mib6(t, "Ip6OutDiscards", m->ip6_out_discards);
    ap_mib6(t, "Ip6OutNoRoutes", m->ip6_out_no_routes);
    ap_mib6(t, "Ip6ReasmTimeout", m->ip6_reasm_timeout);
    ap_mib6(t, "Ip6ReasmReqds", m->ip6_reasm_reqds);
    ap_mib6(t, "Ip6ReasmOKs", m->ip6_reasm_oks);
    ap_mib6(t, "Ip6ReasmFails", m->ip6_reasm_fails);
    ap_mib6(t, "Ip6FragOKs", m->ip6_frag_oks);
    ap_mib6(t, "Ip6FragFails", m->ip6_frag_fails);
    ap_mib6(t, "Ip6FragCreates", m->ip6_frag_creates);
    ap_mib6(t, "Ip6InMcastPkts", m->ip6_in_mcast_pkts);
    ap_mib6(t, "Ip6OutMcastPkts", m->ip6_out_mcast_pkts);
    ap_mib6(t, "Icmp6InMsgs", m->icmp6_in_msgs);
    ap_mib6(t, "Icmp6InErrors", m->icmp6_in_errors);
    ap_mib6(t, "Icmp6OutMsgs", m->icmp6_out_msgs);
    ap_mib6(t, "Icmp6InDestUnreachs", m->icmp6_in_dest_unreachs);
    ap_mib6(t, "Icmp6InPktTooBigs", m->icmp6_in_pkt_too_bigs);
    ap_mib6(t, "Icmp6InTimeExcds", m->icmp6_in_time_excds);
    ap_mib6(t, "Icmp6InParmProblems", m->icmp6_in_parm_problems);
    ap_mib6(t, "Icmp6InEchos", m->icmp6_in_echos);
    ap_mib6(t, "Icmp6InEchoReplies", m->icmp6_in_echo_replies);
    ap_mib6(t, "Icmp6InGroupMembQueries", m->icmp6_in_group_memb_queries);
    ap_mib6(t, "Icmp6InGroupMembResponses", m->icmp6_in_group_memb_responses);
    ap_mib6(t, "Icmp6InGroupMembReductions", m->icmp6_in_group_memb_reductions);
    ap_mib6(t, "Icmp6InRouterSolicits", m->icmp6_in_router_solicits);
    ap_mib6(t, "Icmp6InRouterAdvertisements", m->icmp6_in_router_advertisements);
    ap_mib6(t, "Icmp6InNeighborSolicits", m->icmp6_in_neighbor_solicits);
    ap_mib6(t, "Icmp6InNeighborAdvertisements", m->icmp6_in_neighbor_advertisements);
    ap_mib6(t, "Icmp6InRedirects", m->icmp6_in_redirects);
    ap_mib6(t, "Icmp6InMLDv2Reports", m->icmp6_in_mldv2_reports);
    ap_mib6(t, "Icmp6OutDestUnreachs", m->icmp6_out_dest_unreachs);
    ap_mib6(t, "Icmp6OutPktTooBigs", 0);
    ap_mib6(t, "Icmp6OutTimeExcds", m->icmp6_out_time_excds);
    ap_mib6(t, "Icmp6OutParmProblems", m->icmp6_out_parm_problems);
    ap_mib6(t, "Icmp6OutEchos", m->icmp6_out_echos);
    ap_mib6(t, "Icmp6OutEchoReplies", m->icmp6_out_echo_replies);
    ap_mib6(t, "Icmp6OutGroupMembQueries", 0);
    ap_mib6(t, "Icmp6OutGroupMembResponses", m->icmp6_out_group_memb_responses);
    ap_mib6(t, "Icmp6OutGroupMembReductions", m->icmp6_out_group_memb_reductions);
    ap_mib6(t, "Icmp6OutRouterSolicits", m->icmp6_out_router_solicits);
    ap_mib6(t, "Icmp6OutRouterAdvertisements", 0);
    ap_mib6(t, "Icmp6OutNeighborSolicits", m->icmp6_out_neighbor_solicits);
    ap_mib6(t, "Icmp6OutNeighborAdvertisements", m->icmp6_out_neighbor_advertisements);
    ap_mib6(t, "Icmp6OutRedirects", 0);
    ap_mib6(t, "Icmp6OutMLDv2Reports", m->icmp6_out_mldv2_reports);
    ap_mib6(t, "Udp6InDatagrams", m->udp6_in_datagrams);
    ap_mib6(t, "Udp6NoPorts", m->udp6_no_ports);
    ap_mib6(t, "Udp6InErrors", m->udp6_in_errors);
    ap_mib6(t, "Udp6OutDatagrams", m->udp6_out_datagrams);
    return t->len;
}

/** Append the IPv6 address `a` as /proc/net's IPv6 files write one: its
 * sixteen bytes as 32 lowercase hex digits, in network order. */
static void ap_ip6_hex(struct procfs_text *t, const uint8_t *a)
{
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = 0; i < IP6_ADDR_LEN; i++) {
        ap_ch(t, digits[a[i] >> 4]);
        ap_ch(t, digits[a[i] & 0xFu]);
    }
}

/** Append `v` as `width` lowercase hex digits and a space, as Linux's
 * IPv6 files write their numbers. */
static void ap_hex_fixed(struct procfs_text *t, uint32_t v, int width)
{
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = width - 1; i >= 0; i--) {
        ap_ch(t, digits[(v >> (i * 4)) & 0xFu]);
    }
    ap_ch(t, ' ');
}

/** Append `v` as two lowercase hex digits and a space. */
static void ap_hex2_lower(struct procfs_text *t, uint32_t v)
{
    ap_hex_fixed(t, v, 2);
}

/** Append the name of the interface of index `index` right-aligned in
 * eight columns, as Linux's `%8s`, and the line's end. */
static void ap_ifname8(struct procfs_text *t, int index)
{
    const char *name = "";
    int i;

    for (i = 0; i < netdev_seen_count(); i++) {
        if (netdev_at(i) && netdev_at(i)->index == index) {
            name = netdev_at(i)->name;
        }
    }
    for (i = (int)k_strlen(name); i < 8; i++) {
        ap_ch(t, ' ');
    }
    ap_str(t, name);
    ap_ch(t, '\n');
}

/**
 * Render /proc/net/if_inet6: an interface's IPv6 address a line, from a
 * copy taken under net_lock, in Linux's columns, which BusyBox's ifconfig
 * and net-tools read: the address, then in hex the interface's index, the
 * prefix's length, the scope as Linux's IPV6_ADDR_ bits (0x10 for ::1,
 * 0x20 link-local, 0x40 site-local and 0 global) and the IFA_F_ state,
 * then the interface's name.
 */
static int procfs_gen_netifinet6(struct procfs_text *t)
{
    struct netdev_addr6 addrs[NETDEV_ADDRS6];
    int i, k, n;

    for (i = 0; i < netdev_seen_count(); i++) {
        struct netdev *nd = netdev_at(i);
        uint32_t flags;

        if (!nd) {
            continue;
        }
        flags = net_lock();
        n = nd->naddr6;
        k_memcpy(addrs, nd->addr6, (uint32_t)n * sizeof(addrs[0]));
        net_unlock(flags);
        for (k = 0; k < n; k++) {
            uint8_t scope = addrs[k].scope;

            ap_ip6_hex(t, addrs[k].addr);
            ap_ch(t, ' ');
            ap_hex2_lower(t, (uint32_t)nd->index);
            ap_hex2_lower(t, addrs[k].prefix);
            ap_hex2_lower(t, scope == NETDEV_SCOPE_HOST   ? 0x10u
                             : scope == NETDEV_SCOPE_LINK ? 0x20u
                             : scope == NETDEV_SCOPE_SITE ? 0x40u
                                                          : 0u);
            ap_hex2_lower(t, addrs[k].flags);
            ap_ifname8(t, nd->index);
        }
    }
    return t->len;
}

/* The route flags of /proc/net/ipv6_route, Linux's RTF_ bits: up, through
 * a gateway, a default router's, learned from a router advertisement, and
 * with a lifetime. */
#define PROCFS_RTF_UP       0x00000001u
#define PROCFS_RTF_GATEWAY  0x00000002u
#define PROCFS_RTF_DEFAULT  0x00010000u
#define PROCFS_RTF_ADDRCONF 0x00040000u
#define PROCFS_RTF_EXPIRES  0x00400000u

/**
 * Render /proc/net/ipv6_route: the IPv6 routes, each copied out under
 * net_lock (`route6_at`), a line each in Linux's columns, which BusyBox's
 * `route -A inet6` reads: the destination and its prefix's length, the
 * source and its length, which are always :: and 0, the next hop, :: for
 * on the link, then in eight hex digits the metric, the reference and use
 * counts, kept as 0, and the flags, then the interface's name.
 */
static int procfs_gen_netipv6route(struct procfs_text *t)
{
    struct route6_row r;
    uint32_t flags, rtf;
    int i, more;

    for (i = 0; net_ns_seen(); i++) {
        flags = net_lock();
        more = route6_at(i, &r);
        net_unlock(flags);
        if (!more) {
            break;
        }
        rtf = PROCFS_RTF_UP;
        if (!ip6_is_unspecified(r.gw)) {
            rtf |= PROCFS_RTF_GATEWAY;
        }
        if (r.proto == ROUTE6_PROTO_RA) {
            rtf |= PROCFS_RTF_ADDRCONF | (r.plen == 0 ? PROCFS_RTF_DEFAULT : 0u);
        }
        if (r.expires != 0) {
            rtf |= PROCFS_RTF_EXPIRES;
        }
        ap_ip6_hex(t, r.dst);
        ap_ch(t, ' ');
        ap_hex2_lower(t, r.plen);
        ap_ip6_hex(t, ip6_any);
        ap_str(t, " 00 ");
        ap_ip6_hex(t, r.gw);
        ap_ch(t, ' ');
        ap_hex_fixed(t, r.metric, 8);
        ap_str(t, "00000000 00000000 ");
        ap_hex_fixed(t, rtf, 8);
        ap_ifname8(t, r.ifindex);
    }
    return t->len;
}

/** Render Linux's wireless statistics, with signed dBm signal and noise. */
static int procfs_gen_wireless(struct procfs_text *t)
{
    unsigned i;
    struct wlan *wlan;

    ap_str(t, "Inter-| sta-|   Quality        |   Discarded packets               | Missed | WE\n"
              " face | tus | link level noise |  nwid  crypt   frag  retry   misc | beacon | 22\n");
    for (i = 0; net_ns_seen() && (wlan = wlan_at(i)) != 0; ++i) {
        struct wlan_counters c;
        wlan_get_counters(wlan, &c);
        ap_str(t, wlan->netdev.name);
        ap_str(t, ": 0000 ");
        ap_u32(t, c.quality);
        ap_str(t, ". ");
        ap_int(t, c.signal / 100);
        ap_str(t, ". ");
        ap_int(t, c.noise / 100);
        ap_str(t, ". 0 ");
        ap_u32(t, c.rx_decrypt_errors);
        ap_ch(t, ' ');
        ap_u32(t, c.rx_fragments);
        ap_ch(t, ' ');
        ap_u32(t, c.tx_retries);
        ap_ch(t, ' ');
        ap_u32(t, c.rx_dropped);
        ap_ch(t, ' ');
        ap_u32(t, c.missed_beacons);
        ap_ch(t, '\n');
    }
    return t->nomem ? -1 : t->len;
}

/** Render the /proc/net file of origin `origin`, one with a name from
 * `synth_net_name`: those `synth_net_file` gives and the two files of
 * /proc/net/stat. */
static int procfs_gen_net(struct procfs_text *t, int origin)
{
    switch (origin) {
    case SYNTH_PROC_NETWIRELESS:
        return procfs_gen_wireless(t);
    case SYNTH_PROC_NETPACKET:
        return procfs_gen_netpacket(t);
    case SYNTH_PROC_NETSNMP6:
        return procfs_gen_netsnmp6(t);
    case SYNTH_PROC_NETIFINET6:
        return procfs_gen_netifinet6(t);
    case SYNTH_PROC_NETIPV6RT:
        return procfs_gen_netipv6route(t);
    case SYNTH_PROC_NETSTAT_RT:
    case SYNTH_PROC_NETSTAT_ARP:
        return procfs_gen_netstat(t, origin);
    case SYNTH_PROC_NETUDP:
        return procfs_gen_netdgram(t, AF_INET, SOCK_DGRAM);
    case SYNTH_PROC_NETRAW:
        return procfs_gen_netdgram(t, AF_INET, SOCK_RAW);
    case SYNTH_PROC_NETTCP6:
        return procfs_gen_nettcp(t, AF_INET6);
    case SYNTH_PROC_NETUDP6:
        return procfs_gen_netdgram(t, AF_INET6, SOCK_DGRAM);
    case SYNTH_PROC_NETRAW6:
        return procfs_gen_netdgram(t, AF_INET6, SOCK_RAW);
    case SYNTH_PROC_NETUNIX:
        return procfs_gen_netunix(t);
    case SYNTH_PROC_NETSOCKSTAT:
        return procfs_gen_netsockstat(t);
    case SYNTH_PROC_NETNETSTAT:
        return procfs_gen_netnetstat(t);
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
    if (node == SYSFS_REG) {
        return SYSFS_INO_REG + ((uint32_t)key & (SYSFS_NODES_MAX - 1u));
    }
    if (node >= SYSFS_WLAN_PHY && node <= SYSFS_WLAN_DRIVER)
        return 0x07000000u | ((uint32_t)(node - SYSFS_WLAN_PHY) << 20) | ((uint32_t)key << 8) |
               (uint32_t)file;
    if (SYSFS_IS_BLOCK(node)) {
        return SYSFS_INO_BLOCK | (uint32_t)(node - SYSFS_BLOCK) << 20 |
               ((uint32_t)key & 0xFFFu) << 8 | (uint32_t)file;
    }
    if (node == SYSFS_NET_IF || node == SYSFS_NET_STATS) {
        return SYSFS_INO_NET | (node == SYSFS_NET_STATS ? 0x800000u : 0u) |
               ((uint32_t)key & 0xFFFFu) << 8 | (uint32_t)file;
    }
    if (node == SYSFS_CPUN) {
        return SYSFS_INO_CPU | ((uint32_t)key & 0xFFFFu) << 8 | ((uint32_t)file & 0x7Fu);
    }
    if (node == SYSFS_CPU_TOPOLOGY) {
        return SYSFS_INO_CPU | ((uint32_t)key & 0xFFFFu) << 8 | 0x80u | ((uint32_t)file & 0x7Fu);
    }
    if (node == SYSFS_CPU_CACHE || node == SYSFS_CPU_CACHE_INDEX) {
        return SYSFS_INO_CACHE | (node == SYSFS_CPU_CACHE_INDEX ? 0x800000u : 0u) |
               ((uint32_t)key & 0x7FFFu) << 8 | ((uint32_t)file & 0xFFu);
    }
    if (node == SYSFS_BACKLIGHT) {
        return SYSFS_INO_BACKLIGHT | ((uint32_t)key & 0xFFFFu) << 8 | ((uint32_t)file & 0xFFu);
    }
    if (node == SYSFS_RFKILL) {
        return SYSFS_INO_RFKILL | ((uint32_t)key & 0xFFFFu) << 8 | ((uint32_t)file & 0xFFu);
    }
    if (node == SYSFS_PCI_DEVICE && file == 2) {
        return SYSFS_INO_PCI_LINK + (uint32_t)key;
    }
    if (node == SYSFS_PCI_DEVICE) {
        return SYSFS_INO_PCI + ((uint32_t)key << 8) + ((uint32_t)file & 0xFFu);
    }
    if (node == SYSFS_USB_DEVICE && file == 2) {
        return SYSFS_INO_FILE | (uint32_t)key;
    }
    if (node == SYSFS_USB_DEVICE) {
        return SYSFS_INO_USB | (file ? SYSFS_INO_FILE : 0u) | (uint32_t)key;
    }
    return SYSFS_INO_FIXED + (uint32_t)node + ((uint32_t)file << 8);
}

int sysfs_usb_key(const struct usb_sys_row *r)
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
static int sysfs_pci_real(int key)
{
    uint8_t bus = (uint8_t)(key >> 8), dev = (uint8_t)((key >> 3) & 31);

    return pci_read_config16(bus, dev, (uint8_t)(key & 7), 0x00) != 0xFFFFu &&
           ((key & 7) == 0 || (pci_read_config8(bus, dev, 0, 0x0E) & 0x80) != 0);
}

/** Whether the PCI function `key` is there: a real one, or one a driver
 * reports (`sysfs_pci_virtual`). */
static int sysfs_pci_present(int key)
{
    return sysfs_pci_real(key) || sysfs_pci_virtual(key, NULL);
}

/**
 * The identity of the PCI function `key` into `*id`: the one a driver
 * reports for it (`sysfs_pci_virtual`), or else from its configuration
 * space. boot_vga is 1 for the first VGA function in bus order that decodes
 * memory and I/O, the one the firmware's display ran on, as Linux's VGA
 * arbiter takes it. 0, or -ENOENT for a function that is not there.
 */
static int sysfs_pci_ident(int key, struct sysfs_pci_ident *id)
{
    uint8_t bus = (uint8_t)(key >> 8), dev = (uint8_t)((key >> 3) & 31), fn = (uint8_t)(key & 7);
    pci_device_t p;
    uint32_t sub = 0, ids;
    int n, k;

    if (sysfs_pci_virtual(key, id)) {
        return 0;
    }
    if (!sysfs_pci_real(key)) {
        return -ENOENT;
    }
    ids = pci_read_config32(bus, dev, fn, 0x00);
    if ((pci_read_config8(bus, dev, fn, 0x0E) & 0x7F) == 0) {
        sub = pci_read_config32(bus, dev, fn, 0x2C);
    }
    id->vendor = (uint16_t)(ids & 0xFFFFu);
    id->device = (uint16_t)(ids >> 16);
    id->subsystem_vendor = (uint16_t)(sub & 0xFFFFu);
    id->subsystem_device = (uint16_t)(sub >> 16);
    id->class = pci_read_config32(bus, dev, fn, 0x08) >> 8;
    id->revision = pci_read_config8(bus, dev, fn, 0x08);
    id->boot_vga = 0;
    for (n = 0; (id->class >> 8) == 0x0300u && pci_nth(n, &p); n++) {
        k = p.bus << 8 | p.device << 3 | p.function;
        if (p.class_code == 0x03 && p.subclass == 0x00 &&
            (pci_read_config16(p.bus, p.device, p.function, 0x04) & 3u) == 3u) {
            id->boot_vga = k == key;
            break;
        }
    }
    return 0;
}

int sysfs_pci_is_vga(int key)
{
    struct sysfs_pci_ident id;

    return sysfs_pci_ident(key, &id) == 0 && (id.class >> 8) == 0x0300u;
}

uint64_t sysfs_pci_bar_size(int key, int bar)
{
    struct pci_resource r;

    if (bar < 0 || bar >= SYSFS_PCI_BARS || !sysfs_pci_real(key)) {
        return 0;
    }
    pci_resource((uint8_t)(key >> 8), (uint8_t)((key >> 3) & 31), (uint8_t)(key & 7), bar, &r);
    return r.flags ? r.end - r.start + 1u : 0;
}

int sysfs_pci_attr_present(int key, int attr)
{
    struct pci_resource r;
    int bar;

    if (attr == SYSFS_PCI_BOOT_VGA) {
        return sysfs_pci_is_vga(key);
    }
    if (attr >= SYSFS_PCI_RESOURCE0 && attr < SYSFS_PCI_ATTRS) {
        bar = (attr - SYSFS_PCI_RESOURCE0) % SYSFS_PCI_BARS;
        if (sysfs_pci_bar_size(key, bar) == 0) {
            return 0;
        }
        if (attr >= SYSFS_PCI_RESOURCE0_WC) {
            pci_resource((uint8_t)(key >> 8), (uint8_t)((key >> 3) & 31), (uint8_t)(key & 7), bar,
                         &r);
            return arch_pte_wc() != PTE_NOCACHE &&
                   (r.flags & (PCI_IORESOURCE_MEM | PCI_IORESOURCE_PREFETCH)) ==
                       (PCI_IORESOURCE_MEM | PCI_IORESOURCE_PREFETCH);
        }
        return 1;
    }
    return 1;
}

int sysfs_device_key(int node, int n)
{
    struct usb_sys_row r;
    pci_device_t p;
    int i, key, seen = 0;

    if (node == SYSFS_PCI_DEVICE) {
        if (pci_nth(n, &p)) {
            return p.bus << 8 | p.device << 3 | p.function;
        }
        for (i = 0; pci_nth(i, &p); i++) {
        }
        n -= i;
        for (i = 0; (key = sysfs_pci_virtual_nth(i)) >= 0; i++) {
            if (!sysfs_pci_real(key) && n-- == 0) {
                return key;
            }
        }
        return -1;
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
        ap_u64(t, row.sectors);
    } else if (same_word(name, "start")) {
        ap_u64(t, row.start);
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

/* synthfs.h's net/ipv6/conf has a directory for each slot an interface can
 * hold. */
typedef char ipv6_slots_fit[(SYNTH_IPV6_IFACES == NETDEV_MAX + 1) ? 1 : -1];

const char *sysfs_net_slot_name(int slot)
{
    int i;

    for (i = 0; i < netdev_seen_count(); i++) {
        const struct netdev *nd = netdev_at(i);

        if (nd && netdev_slot(nd) == slot) {
            return nd->name;
        }
    }
    return "";
}

int sysfs_net_pci_key(int index)
{
    const struct netdev *nd = sysfs_netdev(index);

    return nd ? nd->pci_key : -1;
}

/** Return the radio number of a visible interface, or -1. */
int sysfs_net_wiphy(int index)
{
    const struct wlan *wlan = wlan_from_netdev(sysfs_netdev(index));
    return wlan ? (int)wlan->radio->index : -1;
}

/** Return a visible radio's static driver name, or NULL. */
const char *sysfs_wlan_driver(int index)
{
    const struct wlan *wlan = net_ns_seen() ? wlan_by_index((unsigned)index) : 0;
    return wlan ? wlan->radio->name : 0;
}

int sysfs_device_exists(int node, int key)
{
    struct usb_sys_row r;
    struct devfs_disk_row row;

    if (node >= SYSFS_WLAN_PHY && node <= SYSFS_WLAN_DRIVER)
        return sysfs_wlan_driver(key) != 0;
    if (SYSFS_IS_BLOCK(node)) {
        return devfs_disk_row(key, &row) == 1 && (node == SYSFS_BLOCK_PART) == (row.partno != 0) &&
               (node != SYSFS_BLOCK_LOOP || sysfs_block_loop(key) >= 0);
    }
    if (node == SYSFS_NET_IF || node == SYSFS_NET_STATS) {
        return sysfs_netdev(key) != NULL;
    }
    if (node == SYSFS_CPUN || node == SYSFS_CPU_TOPOLOGY) {
        return key >= 0 && key < NR_CPUS && cpu_table[key].online;
    }
    if (node == SYSFS_CPU_CACHE || node == SYSFS_CPU_CACHE_INDEX) {
        struct cpu_cache cache;
        int cpu = node == SYSFS_CPU_CACHE ? key : key >> 4;

        return cpu >= 0 && cpu < NR_CPUS && cpu_table[cpu].online &&
               cpuinfo_cache(cpu, node == SYSFS_CPU_CACHE ? 0 : key & 15, &cache);
    }
    if (node == SYSFS_BACKLIGHT) {
        return key >= 0 && key < backlight_count();
    }
    if (node == SYSFS_RFKILL) {
        return rfkill_present(key);
    }
    if (node == SYSFS_REG) {
        return sysfs_node_kind(key) != 0;
    }
    return node == SYSFS_PCI_DEVICE ? sysfs_pci_present(key) : sysfs_usb_find(key, &r) >= 0;
}

int sysfs_attr_present(int node, int attr)
{
    return node != SYSFS_DMI_ID || attr == SYSFS_DMI_MODALIAS || dmi_field(attr) != 0;
}

/**
 * Render the attribute `attr` of the cache directory `key`, processor
 * `key >> 4`'s cache `key & 15`, in the order `sysfs_attr` lists them, as
 * Linux's cacheinfo shows them: numbers, the sharing processors as a list
 * and a mask, the size in KB with a K, and the type by name. -ENOENT for a
 * cache or processor that is not there.
 */
static int procfs_gen_cache(struct procfs_text *t, int key, int attr)
{
    static const char *const types[] = {"", "Data", "Instruction", "Unified"};
    struct cpu_cache c;
    int cpu = key >> 4;

    if (cpu < 0 || cpu >= NR_CPUS || !cpu_table[cpu].online || !cpuinfo_cache(cpu, key & 15, &c) ||
        !sysfs_attr(SYSFS_CPU_CACHE_INDEX, attr)) {
        return -ENOENT;
    }
    if (attr == 5) {
        ap_cpu_set(t, c.shared);
    } else if (attr == 6) {
        ap_cpu_mask(t, c.shared);
    } else if (attr == 7) {
        ap_u64(t, (uint64_t)c.sets * c.line * c.partitions * c.ways >> 10);
        ap_ch(t, 'K');
    } else if (attr == 8) {
        ap_str(t, types[c.type & 3u]);
    } else {
        ap_u32(t, attr == 0   ? c.line
                  : attr == 1 ? c.id
                  : attr == 2 ? c.level
                  : attr == 3 ? c.sets
                  : attr == 4 ? c.partitions
                              : c.ways);
    }
    ap_ch(t, '\n');
    return t->len;
}

/**
 * Render /sys/class/dmi/id/modalias as Linux's dmi-id.c makes it: "dmi",
 * then for each field the table holds its prefix and its text with the
 * spaces, the colons and the bytes past ASCII taken out, each after a
 * colon, and a colon at the end.
 */
static void procfs_gen_modalias(struct procfs_text *t)
{
    static const struct {
        const char *prefix;
        int field;
    } fields[] = {
        {"bvn", DMI_BIOS_VENDOR}, {"bvr", DMI_BIOS_VERSION},        {"bd", DMI_BIOS_DATE},
        {"br", DMI_BIOS_RELEASE}, {"efr", DMI_EC_FIRMWARE_RELEASE}, {"svn", DMI_SYS_VENDOR},
        {"pn", DMI_PRODUCT_NAME}, {"pvr", DMI_PRODUCT_VERSION},     {"rvn", DMI_BOARD_VENDOR},
        {"rn", DMI_BOARD_NAME},   {"rvr", DMI_BOARD_VERSION},       {"cvn", DMI_CHASSIS_VENDOR},
        {"ct", DMI_CHASSIS_TYPE}, {"cvr", DMI_CHASSIS_VERSION},     {"sku", DMI_PRODUCT_SKU}};
    const char *v;
    int i, j;

    ap_str(t, "dmi");
    for (i = 0; i < (int)(sizeof(fields) / sizeof(fields[0])); i++) {
        if ((v = dmi_field(fields[i].field)) == 0) {
            continue;
        }
        ap_ch(t, ':');
        ap_str(t, fields[i].prefix);
        for (j = 0; v[j] != '\0'; j++) {
            if (v[j] > ' ' && v[j] < 127 && v[j] != ':') {
                ap_ch(t, v[j]);
            }
        }
    }
    ap_str(t, ":\n");
}

/**
 * Render the attribute `attr` of the NUMA node directories, the clock
 * source's or the DMI fields', `node` among SYSFS_NODE to SYSFS_DMI_ID: one
 * node, 0, which has every processor started and all the memory, at
 * distance 10 from itself; the clock sources Linux would name with a space
 * after each, and the one in use; and a DMI field's text. -ENOENT for a
 * field the firmware's table does not hold.
 */
static int procfs_gen_system(struct procfs_text *t, int node, int attr)
{
    const char *v;
    int i;

    if (node == SYSFS_NODE) {
        ap_str(t, "0\n");
    } else if (node == SYSFS_NODE0) {
        if (attr == 0) {
            ap_cpu_set(t, cpu_online_bits());
        } else if (attr == 1) {
            ap_cpu_mask(t, cpu_online_bits());
        } else {
            ap_str(t, "10");
        }
        ap_ch(t, '\n');
    } else if (node == SYSFS_CLOCKSOURCE0 && attr == 0) {
        for (i = 0; (v = timer_clocksource(i)) != 0; i++) {
            ap_str(t, v);
            ap_ch(t, ' ');
        }
        ap_ch(t, '\n');
    } else if (node == SYSFS_CLOCKSOURCE0) {
        ap_str(t, timer_clocksource(-1));
        ap_ch(t, '\n');
    } else if (attr == SYSFS_DMI_MODALIAS) {
        procfs_gen_modalias(t);
    } else if ((v = dmi_field(attr)) != 0) {
        ap_str(t, v);
        ap_ch(t, '\n');
    } else {
        return -ENOENT;
    }
    return t->len;
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

/* The policy words of /sys/kernel/mm/transparent_hugepage's `enabled`,
 * `defrag` and `shmem_enabled`, in Linux 6.12's order (huge_memory.c and
 * shmem.c). */
static const char *const thp_enabled_words[] = {"always", "madvise", "never", 0};
static const char *const thp_defrag_words[] = {"always",  "defer", "defer+madvise",
                                               "madvise", "never", 0};
static const char *const thp_shmem_words[] = {"always", "never", "within_size", "advise", "deny",
                                              "force",  0};

/* The order shmem_enabled_show lists its words in, as indexes above. */
static const int thp_shmem_order[] = {0, 2, 3, 1, 4, 5};

/* The policy as Linux 6.12 boots with CONFIG_TRANSPARENT_HUGEPAGE_NEVER:
 * indexes into the words above for the first three, and flags for the
 * huge zero page and the splitting of underused huge pages, which
 * `procfs_thp_boot` sets for a small machine. Root's writes change them; no
 * page is ever huge, which every policy permits. The big lock covers them. */
static int thp_enabled = 2, thp_defrag = 3, thp_shmem = 1;
static int thp_zero_page = 1, thp_shrink_underused = 1;

void procfs_thp_boot(void)
{
    if (frame_total_count() < (512u << 20) / PAGE_SIZE) {
        thp_enabled = 2;
        thp_defrag = 4;
        thp_zero_page = 0;
    }
}

/** Append the word `words[i]` of each `order[k]`, the chosen one in
 * brackets, separated by spaces and ended by a newline. */
static void ap_choice(struct procfs_text *t, const char *const *words, const int *order, int n,
                      int chosen)
{
    int k;

    for (k = 0; k < n; k++) {
        int i = order ? order[k] : k;

        if (k > 0) {
            ap_ch(t, ' ');
        }
        if (i == chosen) {
            ap_ch(t, '[');
        }
        ap_str(t, words[i]);
        if (i == chosen) {
            ap_ch(t, ']');
        }
    }
    ap_ch(t, '\n');
}

/**
 * Render the attribute `attr` of /sys/kernel/mm/transparent_hugepage, in
 * `sysfs_system_attrs`' order: the policy words of `enabled` and `defrag`,
 * the flag `use_zero_page`, the page-directory mapping size, the words of
 * `shmem_enabled` and the flag `shrink_underused`, as Linux 6.12 writes
 * them. The length of the text.
 */
static int procfs_gen_thp(struct procfs_text *t, int attr)
{
    if (attr == 0) {
        ap_choice(t, thp_enabled_words, 0, 3, thp_enabled);
    } else if (attr == 1) {
        ap_choice(t, thp_defrag_words, 0, 5, thp_defrag);
    } else if (attr == 3) {
        /* PMD_SIZE: 2 MiB with 4-level paging, 4 MiB with i686's 2-level. */
#if ELF_CLASS == 2
        ap_u32(t, 2097152u);
#else
        ap_u32(t, 4194304u);
#endif
        ap_ch(t, '\n');
    } else if (attr == 4) {
        ap_choice(t, thp_shmem_words, thp_shmem_order, 6, thp_shmem);
    } else {
        ap_ch(t, (attr == 2 ? thp_zero_page : thp_shrink_underused) ? '1' : '0');
        ap_ch(t, '\n');
    }
    return t->len;
}

/** Whether the string `s` is `word`, or, when `newline` is set, `word`
 * and one newline, as Linux's sysfs_streq compares them. */
static int thp_streq(const char *s, const char *word, int newline)
{
    while (*s != '\0' && *s == *word) {
        s++;
        word++;
    }
    return *word == '\0' && (*s == '\0' || (newline && *s == '\n' && s[1] == '\0'));
}

/** The index in `words` of the word `s` names as `thp_streq` compares it,
 * or -EINVAL. */
static int thp_word(const char *s, const char *const *words, int newline)
{
    int i;

    for (i = 0; words[i] != 0; i++) {
        if (thp_streq(s, words[i], newline)) {
            return i;
        }
    }
    return -EINVAL;
}

/** Whether the `n` decimal digits at `digits`, with no leading zero, name
 * a number above the one `max` spells. */
static int thp_over(const char *digits, int n, const char *max)
{
    int len = (int)k_strlen(max), i;

    if (n != len) {
        return n > len;
    }
    for (i = 0; i < n && digits[i] == max[i]; i++) {
    }
    return i < n && digits[i] > max[i];
}

/**
 * Parse `s` as Linux's kstrtoul in base 10: an optional `+`, decimal
 * digits and an optional newline. The value when it is 0 or 1; -ERANGE
 * when the digits pass 64 bits, which Linux's _kstrtoull finds before it
 * looks at what follows them, or when what is otherwise a number passes an
 * unsigned long; -EINVAL otherwise.
 */
static int thp_flag(const char *s)
{
    const char *digits;
    int n = 0;

    if (*s == '+') {
        s++;
    }
    while (*s == '0' && s[1] >= '0' && s[1] <= '9') {
        s++;
    }
    digits = s;
    while (s[n] >= '0' && s[n] <= '9') {
        n++;
    }
    if (thp_over(digits, n, "18446744073709551615")) {
        return -ERANGE;
    }
    if (n == 0 || !(s[n] == '\0' || (s[n] == '\n' && s[n + 1] == '\0'))) {
        return -EINVAL;
    }
#if ELF_CLASS != 2
    if (thp_over(digits, n, "4294967295")) {
        return -ERANGE;
    }
#endif
    return n == 1 && digits[0] <= '1' ? digits[0] - '0' : -EINVAL;
}

/** `text` as Linux's kstrtobool reads its first letters: 1 for y, t, 1 or
 * on, 0 for n, f, 0 or off, in either case, and -EINVAL otherwise. */
static int thp_bool(const char *text)
{
    char c = text[0] | 0x20, d = c == 'o' ? text[1] | 0x20 : 0;

    if (c == 'y' || c == 't' || text[0] == '1' || d == 'n') {
        return 1;
    }
    if (c == 'n' || c == 'f' || text[0] == '0' || d == 'f') {
        return 0;
    }
    return -EINVAL;
}

/** The index in `thp_shmem_words` of the `count` bytes of `text` as
 * Linux's shmem_enabled_store reads them: at most 15, a final newline
 * dropped, then one word exactly; -EINVAL otherwise. */
static int thp_shmem_word(char *text, uint32_t count)
{
    if (count + 1 > 16) {
        return -EINVAL;
    }
    if (count > 0 && text[count - 1] == '\n') {
        text[count - 1] = '\0';
    }
    return thp_word(text, thp_shmem_words, 0);
}

/**
 * Take root's write of `count` bytes, `text`, NUL-terminated after them,
 * to the attribute `attr` of /sys/kernel/mm/transparent_hugepage, as
 * Linux 6.12's stores parse it: `enabled` and `defrag` a policy word with
 * at most one newline after it, `use_zero_page` 0 or 1 as kstrtoul reads
 * it (`thp_flag`), `shmem_enabled` as `thp_shmem_word` reads it, and
 * `shrink_underused` as `thp_bool` does. `count`, or -EINVAL or -ERANGE
 * with nothing changed.
 */
static int32_t thp_store(int attr, char *text, uint32_t count)
{
    int *const flags[] = {&thp_enabled, &thp_defrag,          &thp_zero_page, 0,
                          &thp_shmem,   &thp_shrink_underused};
    int v;

    if (attr < 0 || attr > 5 || !flags[attr]) {
        return -EINVAL;
    }
    v = attr == 0   ? thp_word(text, thp_enabled_words, 1)
        : attr == 1 ? thp_word(text, thp_defrag_words, 1)
        : attr == 2 ? thp_flag(text)
        : attr == 4 ? thp_shmem_word(text, count)
                    : thp_bool(text);
    if (v < 0) {
        return v;
    }
    *flags[attr] = v;
    return (int32_t)count;
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
    if (node >= SYSFS_NODE && node <= SYSFS_DMI_ID) {
        return procfs_gen_system(t, node, attr);
    }
    if (node == SYSFS_THP) {
        return procfs_gen_thp(t, attr);
    }
    if (node == SYSFS_KERNEL) {
        ap_u32(t, sysfs_uevent_seqnum());
        ap_ch(t, '\n');
        return t->len;
    }
    if (node == SYSFS_CLASS_DRM) {
        /* Linux's drm_sysfs.c: the core's version and date. */
        ap_str(t, "drm 1.1.0 20060810\n");
        return t->len;
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
            ap_cpu_set(t, cpu_online_bits());
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
        uint64_t v[10];
        uint32_t flags = net_lock();

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
        net_unlock(flags);
        ap_u64(t, v[attr]);
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
        ap_u32(t, nd->mtu);
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
 * A PCI function has its driver when one has it, its class, IDs and
 * subsystem IDs, its slot and its module alias. A USB device has its
 * product, the numbers of its bus and of itself, and its type last: `lsusb`
 * takes a uevent whose first line is the type for an interface's, and
 * Linux's first lines, the device node's numbers and name, are left out,
 * since no node of the device exists here. -ENOENT for a device that is not
 * there.
 */
static int procfs_gen_uevent(struct procfs_text *t, int node, int key)
{
    struct usb_sys_row r;
    struct sysfs_pci_ident id;
    char name[16];
    const char *drv, *owner;
    uint32_t cls, ids, sub;

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
    if (sysfs_pci_ident(key, &id) != 0) {
        return -ENOENT;
    }
    cls = id.class;
    ids = (uint32_t)id.vendor | (uint32_t)id.device << 16;
    sub = (uint32_t)id.subsystem_vendor | (uint32_t)id.subsystem_device << 16;
    synth_sys_device_name(SYSFS_PCI_DEVICE, key, name, sizeof(name));
    drv = sysfs_pci_driver(key);
    owner = pci_owner((uint8_t)(key >> 8), (uint8_t)key);
    if (!drv && owner && !same_word(owner, PCI_OWNER_KERNEL)) {
        drv = owner; /* vfio-pci, which shows itself bound */
    }
    if (drv) {
        ap_str(t, "DRIVER=");
        ap_str(t, drv);
        ap_ch(t, '\n');
    }
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

/** Append attribute `attr` of backlight `key` (display/backlight.h): its
 * length, or -ENODEV for a backlight that is gone. */
static int procfs_gen_backlight(struct procfs_text *t, int key, int attr)
{
    char text[32];
    int rc = backlight_show(key, attr, text, (int)sizeof(text));

    if (rc < 0) {
        return rc;
    }
    ap_str(t, text);
    return t->len;
}

/** Append attribute `attr` of radio switch `key` (rfkill.h): its length, or
 * -ENOENT for a switch that is gone. */
static int procfs_gen_rfkill(struct procfs_text *t, int key, int attr)
{
    char text[160];
    int len = rfkill_show(key, attr, text, (int)sizeof(text) - 1);

    if (len < 0) {
        return len;
    }
    text[len] = '\0';
    ap_str(t, text);
    return t->len;
}

/**
 * Render the attribute `attr` of processor `cpu`'s topology directory, in
 * the order `sysfs_attr` lists them: Linux's masks and lists of the
 * processors that share its core or package, and its ids. A cluster is a
 * core here and a die is a package, as on a processor whose cores keep
 * their own L2 and whose package has one die. -ENOENT for a processor not
 * started.
 */
static int procfs_gen_topology(struct procfs_text *t, int cpu, int attr)
{
    struct cpu_place place;
    uint64_t set;

    if (cpu < 0 || cpu >= NR_CPUS || !cpu_table[cpu].online ||
        !sysfs_attr(SYSFS_CPU_TOPOLOGY, attr)) {
        return -ENOENT;
    }
    cpuinfo_place(cpu, &place);
    if (attr == 2 || attr == 5) {
        ap_u32(t, place.core);
    } else if (attr == 10) {
        ap_u32(t, 0);
    } else if (attr == 13) {
        ap_u32(t, place.package);
    } else {
        /* Of each pair of mask and list, the list is the second. */
        static const signed char level[] = {CPUINFO_CORE,
                                            CPUINFO_CORE,
                                            -1,
                                            CPUINFO_CORE,
                                            CPUINFO_CORE,
                                            -1,
                                            CPUINFO_PACKAGE,
                                            CPUINFO_PACKAGE,
                                            CPUINFO_PACKAGE,
                                            CPUINFO_PACKAGE,
                                            -1,
                                            CPUINFO_PACKAGE,
                                            CPUINFO_PACKAGE,
                                            -1,
                                            CPUINFO_CORE,
                                            CPUINFO_CORE};
        static const char list[] = {0, 1, 0, 0, 1, 0, 0, 1, 0, 1, 0, 0, 1, 0, 0, 1};

        set = cpuinfo_siblings(cpu, level[attr]);
        if (list[attr]) {
            ap_cpu_set(t, set);
        } else {
            ap_cpu_mask(t, set);
        }
    }
    ap_ch(t, '\n');
    return t->len;
}

/** Render the attributes of a registered radio, with Linux's spelling. */
static int procfs_gen_phy(struct procfs_text *t, int key, int attr)
{
    const struct wlan *wlan = net_ns_seen() ? wlan_by_index((unsigned)key) : 0;

    if (!wlan)
        return -ENOENT;
    switch (attr) {
    case 0:
        ap_u32(t, (unsigned)key);
        break;
    case 1:
        ap_str(t, "phy");
        ap_u32(t, (unsigned)key);
        break;
    case 2:
        ap_mac(t, wlan->radio->mac);
        break;
    case 3:
        ap_str(t, "DEVTYPE=wlan\nDRIVER=");
        ap_str(t, wlan->radio->name);
        break;
    default:
        return -ENOENT;
    }
    ap_ch(t, '\n');
    return t->nomem ? -1 : t->len;
}

/** Write the identity `id` into the configuration header `header` at the
 * offsets of its IDs, revision, class and subsystem IDs. */
static void pci_header_ident(uint8_t *header, const struct sysfs_pci_ident *id)
{
    header[0x00] = (uint8_t)id->vendor;
    header[0x01] = (uint8_t)(id->vendor >> 8);
    header[0x02] = (uint8_t)id->device;
    header[0x03] = (uint8_t)(id->device >> 8);
    header[0x08] = id->revision;
    header[0x09] = (uint8_t)id->class;
    header[0x0A] = (uint8_t)(id->class >> 8);
    header[0x0B] = (uint8_t)(id->class >> 16);
    header[0x2C] = (uint8_t)id->subsystem_vendor;
    header[0x2D] = (uint8_t)(id->subsystem_vendor >> 8);
    header[0x2E] = (uint8_t)id->subsystem_device;
    header[0x2F] = (uint8_t)(id->subsystem_device >> 8);
}

/**
 * Render the configuration space of the PCI function `key` as Linux's
 * `config` gives it: its 256 bytes, or 4096 when an ECAM window reaches its
 * extended space, read a dword at a time; only the 64 bytes of the header
 * to a reader without privilege, since some devices act on a read of their
 * own registers. A reported identity (`sysfs_pci_virtual`) replaces the
 * IDs in the header, and a reported function with no device behind it has
 * a type 0 header of zeros besides. -ENOENT for a function not there.
 */
static int procfs_gen_pciconfig(struct procfs_text *t, int key)
{
    uint8_t bus = (uint8_t)(key >> 8), dev = (uint8_t)((key >> 3) & 31), fn = (uint8_t)(key & 7);
    uint32_t size = 256, off, v;
    struct sysfs_pci_ident id;
    uint8_t header[256];
    int reported = sysfs_pci_virtual(key, &id), real = sysfs_pci_real(key);

    if (!real && !reported) {
        return -ENOENT;
    }
    if (real && pci_read_config32_ext(bus, dev, fn, 0x100) != 0xFFFFFFFFu) {
        size = 4096;
    }
    if (!proc_privileged(current_proc)) {
        size = 64;
    }
    k_memset(header, 0, sizeof(header));
    for (off = 0; real && off < size && off < 256; off += 4) {
        v = pci_read_config32(bus, dev, fn, (uint8_t)off);
        header[off] = (uint8_t)v;
        header[off + 1] = (uint8_t)(v >> 8);
        header[off + 2] = (uint8_t)(v >> 16);
        header[off + 3] = (uint8_t)(v >> 24);
    }
    if (reported) {
        pci_header_ident(header, &id);
    }
    for (off = 0; off < size && off < 256; off++) {
        ap_ch(t, (char)header[off]);
    }
    for (; off < size; off += 4) {
        v = pci_read_config32_ext(bus, dev, fn, (uint16_t)off);
        ap_ch(t, (char)(v & 0xFFu));
        ap_ch(t, (char)((v >> 8) & 0xFFu));
        ap_ch(t, (char)((v >> 16) & 0xFFu));
        ap_ch(t, (char)(v >> 24));
    }
    return t->len;
}

/**
 * Render the file `attr` of the PCI function `key`, a SYSFS_PCI_* past its
 * config, as Linux's pci-sysfs.c writes it: the IDs as 0x and four hex
 * digits, the class as 0x and six, the revision as 0x and two, and
 * boot_vga as 0 or 1, which only a VGA function has. -ENOENT for a
 * function, or a boot_vga, that is not there.
 */
static int procfs_gen_pciattr(struct procfs_text *t, int key, int attr)
{
    static const char digits[] = "0123456789abcdef";
    struct sysfs_pci_ident id;
    uint32_t v;
    int width = 4, i;

    if (sysfs_pci_ident(key, &id) != 0) {
        return -ENOENT;
    }
    if (attr == SYSFS_PCI_BOOT_VGA) {
        if ((id.class >> 8) != 0x0300u) {
            return -ENOENT;
        }
        ap_str(t, id.boot_vga ? "1\n" : "0\n");
        return t->len;
    }
    v = attr == SYSFS_PCI_VENDOR             ? id.vendor
        : attr == SYSFS_PCI_DEVICE_ID        ? id.device
        : attr == SYSFS_PCI_SUBSYSTEM_VENDOR ? id.subsystem_vendor
        : attr == SYSFS_PCI_SUBSYSTEM_DEVICE ? id.subsystem_device
        : attr == SYSFS_PCI_CLASS            ? id.class
                                             : id.revision;
    width = attr == SYSFS_PCI_CLASS ? 6 : attr == SYSFS_PCI_REVISION ? 2 : 4;
    ap_str(t, "0x");
    for (i = width - 1; i >= 0; i--) {
        ap_ch(t, digits[(v >> (i * 4)) & 0xFu]);
    }
    ap_ch(t, '\n');
    return t->len;
}

/** Append `v` to `t` as 0x and sixteen hex digits, as Linux's "0x%016llx". */
static void ap_hex64(struct procfs_text *t, uint64_t v)
{
    static const char digits[] = "0123456789abcdef";
    int i;

    ap_str(t, "0x");
    for (i = 15; i >= 0; i--) {
        ap_ch(t, digits[(unsigned)(v >> (i * 4)) & 0xFu]);
    }
}

/**
 * Render the `resource` file of the PCI function `key`, as Linux's
 * resource_show: a line for each resource, its start, end and flags in
 * hex (`pci_resource`), BARs 0 to 5 and the ROM, then the six of SR-IOV,
 * which no function here has, and for a bridge its four windows, which
 * Linux shows as 0 too. libpciaccess reads a BAR's size from it. A
 * function a driver reports has no BARs, so its lines are 0. -ENOENT for
 * a function that is not there.
 */
static int procfs_gen_pciresource(struct procfs_text *t, int key)
{
    uint8_t bus = (uint8_t)(key >> 8), dev = (uint8_t)((key >> 3) & 31), fn = (uint8_t)(key & 7);
    struct pci_resource r;
    int real = sysfs_pci_real(key), lines = 13, i;

    if (!sysfs_pci_present(key)) {
        return -ENOENT;
    }
    if (real && (pci_read_config8(bus, dev, fn, 0x0E) & 0x7F) != 0) {
        lines = 17;
    }
    for (i = 0; i < lines; i++) {
        r.start = 0;
        r.end = 0;
        r.flags = 0;
        if (real && i < PCI_RESOURCES) {
            pci_resource(bus, dev, fn, i, &r);
        }
        ap_hex64(t, r.start);
        ap_ch(t, ' ');
        ap_hex64(t, r.end);
        ap_ch(t, ' ');
        ap_hex64(t, r.flags);
        ap_ch(t, '\n');
    }
    return t->len;
}

/**
 * Render the registered attribute `handle` (sysfs.h) through its `show`,
 * into room that grows to what the show asks for, up to SYSFS_SHOW_MAX:
 * its length, or the show's error, -ENOMEM when the heap has no room.
 */
static int procfs_gen_regattr(struct procfs_text *t, int handle)
{
    int32_t want = 4096, got;

    for (;;) {
        if (procfs_text_room(t, t->len + want) != 0) {
            return -ENOMEM;
        }
        got = sysfs_node_show(handle, t->buf + t->len, (uint32_t)(t->cap - 1 - t->len));
        if (got < 0) {
            return got;
        }
        if (got <= t->cap - 1 - t->len) {
            t->len += got;
            return t->len;
        }
        if ((uint32_t)got > SYSFS_SHOW_MAX) {
            return -EFBIG;
        }
        want = got;
    }
}

/**
 * Render the /sys file at the path `vp`: an attribute of a backlight, of a
 * disk node, of an interface, of a processor's topology or cache, or of a
 * fixed directory, a device's uevent, or a PCI function's config. `*key`
 * gets the backlight, the disk node, the interface, the processor or cache
 * or the device the file is for, which its descriptor keeps.
 */
static int procfs_gen_sysfile(struct procfs_text *t, const struct vpath *vp, int *key)
{
    if (vp->sysent == SYSFS_WLAN_PHY) {
        *key = vp->syskey;
        return procfs_gen_phy(t, vp->syskey, vp->procfile);
    }
    if (vp->sysent == SYSFS_BACKLIGHT) {
        *key = vp->syskey;
        return procfs_gen_backlight(t, vp->syskey, vp->procfile);
    }
    if (vp->sysent == SYSFS_RFKILL) {
        *key = vp->syskey;
        return procfs_gen_rfkill(t, vp->syskey, vp->procfile);
    }
    if (SYSFS_IS_BLOCK(vp->sysent)) {
        *key = vp->syskey;
        return procfs_gen_blockattr(t, vp->sysent, vp->syskey, vp->procfile);
    }
    if (vp->sysent == SYSFS_NET_IF || vp->sysent == SYSFS_NET_STATS) {
        *key = vp->syskey;
        return procfs_gen_netattr(t, vp->sysent, vp->syskey, vp->procfile);
    }
    if (vp->sysent == SYSFS_CPU_TOPOLOGY) {
        *key = vp->syskey;
        return procfs_gen_topology(t, vp->syskey, vp->procfile);
    }
    if (vp->sysent == SYSFS_CPU_CACHE_INDEX) {
        *key = vp->syskey;
        return procfs_gen_cache(t, vp->syskey, vp->procfile);
    }
    if (sysfs_attr(vp->sysent, vp->procfile)) {
        return procfs_gen_sysattr(t, vp->sysent, vp->procfile);
    }
    *key = vp->syskey;
    if (vp->sysent == SYSFS_REG) {
        return procfs_gen_regattr(t, vp->syskey);
    }
    if (vp->sysent == SYSFS_PCI_DEVICE && vp->procfile == SYSFS_PCI_CONFIG) {
        return procfs_gen_pciconfig(t, vp->syskey);
    }
    if (vp->sysent == SYSFS_PCI_DEVICE && vp->procfile == SYSFS_PCI_RESOURCE) {
        return procfs_gen_pciresource(t, vp->syskey);
    }
    if (vp->sysent == SYSFS_PCI_DEVICE && vp->procfile >= SYSFS_PCI_VENDOR) {
        return procfs_gen_pciattr(t, vp->syskey, vp->procfile);
    }
    return procfs_gen_uevent(t, vp->sysent, vp->syskey);
}

/* The operations of a descriptor of /proc or /sys. */

static int procfs_render(struct procfs_text *t, const struct vpath *vp, int pid, int *key);
static int procfs_needs_pid(int kind);

/**
 * Make the snapshot of `f` again, from the path and the process its open
 * resolved: 0, or the generator's error, -ESRCH for a process that is gone
 * as Linux answers a read of its file, with the old text kept. Nothing that
 * sleeps comes between taking the old text and putting the new.
 */
static int procfs_regen(struct procfs_file *f)
{
    struct procfs_text text;
    int key = f->gen_pid;
    int len = procfs_render(&text, &f->vp, f->gen_pid, &key);

    if (len < 0) {
        return len == -ENOENT && procfs_needs_pid(f->vp.kind) ? -ESRCH : len;
    }
    kfree(f->data);
    f->data = text.buf;
    f->len = len;
    f->gen++;
    return 0;
}

/**
 * Read the snapshot from the description's offset, which moves past what
 * is read: the bytes read, 0 at its end, -EFAULT when none could be copied,
 * or the error of making it again. A read from offset 0 makes the snapshot
 * again unless it is the first read of the text an open or a read made, so
 * a reader that seeks back to the start, or reads with pread64 at 0, sees
 * the file as it is then, as Linux's seq_file files are made again. A
 * `stale` text is made again at the first read, from any offset.
 *
 * The text goes out through a buffer on the stack, a piece at a time, since
 * the copy to the caller may sleep in a fault and pread64 holds no lock: a
 * sharer may make the snapshot again meanwhile, which frees the old text.
 * The read then ends short, before a piece of the new.
 */
static int32_t procfs_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    char piece[256];
    uint32_t off, n, done = 0, gen;
    int rc;

    if (!f) {
        return 0;
    }
    if (f->stale || (e->ofd->offset == 0 && !f->fresh)) {
        rc = procfs_regen(f);
        if (rc != 0) {
            return rc;
        }
    }
    f->fresh = 0;
    f->stale = 0;
    if (e->ofd->offset >= (uint64_t)f->len) {
        return 0;
    }
    off = (uint32_t)e->ofd->offset;
    n = (uint32_t)f->len - off < count ? (uint32_t)f->len - off : count;
    gen = f->gen;
    while (done < n && f->gen == gen) {
        uint32_t k = n - done < sizeof(piece) ? n - done : (uint32_t)sizeof(piece);

        k_memcpy(piece, f->data + off + done, k);
        if (copy_to_user(buf + done, piece, k) != 0) {
            break;
        }
        done += k;
    }
    if (done == 0) {
        return -EFAULT;
    }
    e->ofd->offset = off + done;
    return (int32_t)done;
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

/** Free the descriptor's record and its text, take it off `procfs_owned`,
 * and drop a mem descriptor's hold on its address space; at the
 * description's last close. */
static void procfs_close(fd_entry_t *e)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    if (f) {
        struct procfs_file **pp = &procfs_owned;

        while (f->owned && *pp && *pp != f) {
            pp = &(*pp)->owned_next;
        }
        if (f->owned && *pp == f) {
            *pp = f->owned_next;
        }
        e->priv = NULL;
        mm_unhold(f->mm);
        kfree(f->data);
        kfree(f);
    }
}

static const struct file_ops procfs_file_ops = {
    procfs_read, procfs_write, procfs_poll, procfs_ioctl, procfs_close, NULL, NULL};

/** Read the PCI config file afresh at every offset, including pread. */
static int32_t procfs_pciconfig_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    struct procfs_text t;
    uint64_t off = e->ofd->offset;
    uint32_t n = 0;
    int rc;

    procfs_text_init(&t);
    rc = procfs_gen_pciconfig(&t, f->origin_pid);
    if (rc >= 0 && !t.nomem && off < (uint32_t)t.len) {
        n = count < (uint32_t)t.len - (uint32_t)off ? count : (uint32_t)t.len - (uint32_t)off;
        if (copy_to_user(buf, t.buf + (uint32_t)off, n)) {
            rc = -EFAULT;
        }
    }
    kfree(t.buf);
    if (rc < 0 || t.nomem) {
        return t.nomem ? -ENOMEM : rc;
    }
    e->ofd->offset += n;
    return (int32_t)n;
}

/** Write the range with aligned accesses of its own width. Never write
 * back a dword read to merge a byte: status bits may clear on a one. */
static void pciconfig_store(int key, uint32_t off, const uint8_t *data, uint32_t count)
{
    uint8_t bus = (uint8_t)(key >> 8), dev = (uint8_t)((key >> 3) & 31), fn = (uint8_t)(key & 7);
    uint32_t n, value;

    while (count) {
        n = !(off & 3u) && count >= 4 ? 4u : !(off & 1u) && count >= 2 ? 2u : 1u;
        value = data[0];
        if (n >= 2) {
            value |= (uint32_t)data[1] << 8;
        }
        if (n == 4) {
            value |= (uint32_t)data[2] << 16 | (uint32_t)data[3] << 24;
            pci_write_config32_ext(bus, dev, fn, (uint16_t)off, value);
        } else if (n == 2) {
            pci_write_config16_ext(bus, dev, fn, (uint16_t)off, (uint16_t)value);
        } else {
            pci_write_config8_ext(bus, dev, fn, (uint16_t)off, (uint8_t)value);
        }
        off += n;
        data += n;
        count -= n;
    }
}

/** Write raw PCI config, cut at the function's end as Linux does. The
 * open checked privilege. Copy the range before touching any register. */
static int32_t procfs_pciconfig_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    int key = f->origin_pid;
    uint8_t bus = (uint8_t)(key >> 8), dev = (uint8_t)((key >> 3) & 31), fn = (uint8_t)(key & 7);
    uint32_t size = 256, n;
    uint64_t off = e->ofd->offset;
    uint8_t *data;

    if (!sysfs_pci_real(key)) {
        return -ENODEV;
    }
    if (pci_read_config32_ext(bus, dev, fn, 0x100) != 0xFFFFFFFFu) {
        size = 4096;
    }
    if (off >= size || !count) {
        return 0;
    }
    n = count < size - (uint32_t)off ? count : size - (uint32_t)off;
    data = kmalloc(n);
    if (!data) {
        return -ENOMEM;
    }
    if (copy_from_user(data, buf, n)) {
        kfree(data);
        return -EFAULT;
    }
    pciconfig_store(key, (uint32_t)off, data, n);
    kfree(data);
    e->ofd->offset = off + n;
    return (int32_t)n;
}

static const struct file_ops procfs_pciconfig_ops = {procfs_pciconfig_read,
                                                     procfs_pciconfig_write,
                                                     procfs_poll,
                                                     procfs_ioctl,
                                                     procfs_close,
                                                     NULL,
                                                     NULL};

/**
 * Whether the caller may open a PCI function's file, root's with the mode
 * bits `mode`, for access `acc` (0 read, 1 write, 2 both), as Linux's
 * generic_permission judges a sysfs file: by the owner's bits when its
 * file-system user id is root's, else by the others' bits, which
 * CAP_DAC_OVERRIDE passes, and for a read CAP_DAC_READ_SEARCH, while root's
 * ids are mapped in its user namespace. Root's group is no help, since
 * these files give it no more than the others.
 */
static int pci_file_may(uint32_t mode, int acc)
{
    uint32_t want = acc == 0 ? 4u : acc == 1 ? 2u : 6u;

    if (proc_fsuid(current_proc) == 0) {
        return (want & ~(mode >> 6) & 7u) == 0;
    }
    if ((want & ~mode & 7u) == 0) {
        return 1;
    }
    if (!ns_ids_mapped(proc_userns(current_proc), 0, 0)) {
        return 0;
    }
    return cap_holds(current_proc, CAP_DAC_OVERRIDE) ||
           (acc == 0 && cap_holds(current_proc, CAP_DAC_READ_SEARCH));
}

/** Open raw PCI config, root's with mode 0644 as Linux's, which any caller
 * reads and a write needs `pci_file_may` to allow. Each access reaches the
 * current registers, including a read at a nonzero offset. */
static int procfs_open_pciconfig(const struct vpath *vp, int flags)
{
    struct procfs_file *f;
    fd_entry_t *e;
    int acc = flags & 3, fd;

    if (acc == 3 || !pci_file_may(0644, acc)) {
        return -EACCES;
    }
    if (!sysfs_pci_present(vp->syskey)) {
        return -ENOENT;
    }
    f = kzalloc(sizeof(*f));
    if (!f) {
        return -ENOMEM;
    }
    f->origin_file = SYNTH_SYS_UEVENT;
    f->origin_task = SYSFS_PCI_DEVICE << 8 | SYSFS_PCI_CONFIG;
    f->origin_pid = vp->syskey;
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(f);
        return fd;
    }
    e = &current_proc->fds->entries[fd];
    e->type = FD_TYPE_PROC;
    e->ops = &procfs_pciconfig_ops;
    e->priv = f;
    e->flags = acc == 0 ? FD_FLAG_READ : acc == 1 ? FD_FLAG_WRITE : FD_FLAG_READ | FD_FLAG_WRITE;
    if (flags & PROCFS_O_CLOEXEC) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    return fd;
}

/* The resourceN and resourceN_wc files of a PCI function, as in pci-sysfs:
 * the descriptor's `procfs_file` has the function's key in `origin_pid` and
 * the file's `procfile` in the low byte of `origin_task`. */

/** The BAR of the resourceN descriptor `e` into `*r`, as `pci_resource`
 * reads it now, and its function's key into `*key`: its number, or -1. */
static int pcibar_of(fd_entry_t *e, struct pci_resource *r, int *key)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    int attr = (int)(f->origin_task & 0xFFu);
    int bar = (attr - SYSFS_PCI_RESOURCE0) % SYSFS_PCI_BARS;

    *key = f->origin_pid;
    r->flags = 0;
    if (attr < SYSFS_PCI_RESOURCE0 || attr >= SYSFS_PCI_ATTRS ||
        !sysfs_pci_attr_present(*key, attr)) {
        return -1;
    }
    pci_resource((uint8_t)(*key >> 8), (uint8_t)((*key >> 3) & 31), (uint8_t)(*key & 7), bar, r);
    return bar;
}

/**
 * The bytes a read or write of `count` bytes at the description's offset in
 * the resourceN descriptor `e` reaches, as Linux's sysfs and pci_resource_io
 * judge them together, with the BAR into `*r`. sysfs clips the access to the
 * file, as long as the BAR: a read of none, or one at or past the end, gives
 * 0, a write at or past it -EFBIG, and a write clipped to none 0. A memory
 * BAR then has no read or write, -EIO, and pci_resource_io takes one port
 * access of 1, 2 or 4 bytes within the BAR as it reads now, -EINVAL for
 * another width or one past its end. The bytes to move, more than 0, or
 * the call's result: 0, -ENODEV for a function or BAR that is gone, -EFBIG,
 * -EIO or -EINVAL.
 */
static int32_t pcibar_span(fd_entry_t *e, uint32_t count, int write, struct pci_resource *r)
{
    uint64_t off = e->ofd->offset, size;
    int key, bar = pcibar_of(e, r, &key);

    if (bar < 0) {
        return -ENODEV;
    }
    if (count == 0 && !write) {
        return 0;
    }
    size = sysfs_pci_bar_size(key, bar);
    if (off >= size) {
        return write ? -EFBIG : 0;
    }
    if (count > size - off) {
        count = (uint32_t)(size - off);
    }
    if (count == 0) {
        return 0;
    }
    if (!(r->flags & PCI_IORESOURCE_IO)) {
        return -EIO;
    }
    if (off > r->end - r->start) {
        return 0;
    }
    if (count - 1u > r->end - r->start - off || (count != 1 && count != 2 && count != 4)) {
        return -EINVAL;
    }
    return (int32_t)count;
}

/**
 * Read or write `count` bytes, 1, 2 or 4, of the I/O BAR `r` at the
 * offset of the resourceN descriptor `e`, which `pcibar_span` has judged,
 * one port access of that width, as Linux's pci_resource_io: from `*value`
 * for a write, into it for a read. Moves the offset past them.
 */
static void pcibar_io(fd_entry_t *e, const struct pci_resource *r, uint32_t count, uint32_t *value,
                      int write)
{
    uint16_t port = (uint16_t)(r->start + e->ofd->offset);

    if (write) {
        if (count == 1) {
            pci_io_write8(port, (uint8_t)*value);
        } else if (count == 2) {
            pci_io_write16(port, (uint16_t)*value);
        } else {
            pci_io_write32(port, *value);
        }
    } else {
        *value = count == 1   ? pci_io_read8(port)
                 : count == 2 ? pci_io_read16(port)
                              : pci_io_read32(port);
    }
    e->ofd->offset += count;
}

/** read of a resourceN file, its bytes little-endian: the count, clipped
 * to the file, or `pcibar_span`'s result. */
static int32_t procfs_pcibar_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct pci_resource r;
    uint32_t value = 0;
    uint8_t bytes[4];
    int32_t n = pcibar_span(e, count, 0, &r);

    if (n <= 0) {
        return n;
    }
    pcibar_io(e, &r, (uint32_t)n, &value, 0);
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
    bytes[2] = (uint8_t)(value >> 16);
    bytes[3] = (uint8_t)(value >> 24);
    if (copy_to_user(buf, bytes, (uint32_t)n)) {
        e->ofd->offset -= (uint32_t)n;
        return -EFAULT;
    }
    return n;
}

/**
 * write of a resourceN file, its bytes little-endian: the count, clipped
 * to the file, `pcibar_span`'s result, or -EFAULT. As Linux's kernfs, the
 * first `count` bytes, up to a page, are copied before anything else is
 * judged, so that a source not readable to that length is -EFAULT with no
 * port written, though the access the file clips it to would read less.
 */
static int32_t procfs_pcibar_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct pci_resource r;
    uint8_t bytes[4] = {0, 0, 0, 0};
    uint8_t chunk[64];
    uint32_t value, done, len = count < PAGE_SIZE ? count : PAGE_SIZE;
    int32_t n;

    for (done = 0; done < len; done += sizeof(chunk)) {
        uint32_t piece = len - done < sizeof(chunk) ? len - done : sizeof(chunk);

        if (copy_from_user(chunk, buf + done, piece)) {
            return -EFAULT;
        }
        if (done == 0) {
            k_memcpy(bytes, chunk, piece < sizeof(bytes) ? piece : sizeof(bytes));
        }
    }
    n = pcibar_span(e, len, 1, &r);
    if (n <= 0) {
        return n;
    }
    value = (uint32_t)bytes[0] | (uint32_t)bytes[1] << 8 | (uint32_t)bytes[2] << 16 |
            (uint32_t)bytes[3] << 24;
    pcibar_io(e, &r, (uint32_t)n, &value, 1);
    return n;
}

/** The `page` of a resourceN mapping, whose object is the function's key
 * shifted left by 3 with the BAR's number: the physical address of the
 * BAR's page `pgoff` as the BAR reads now, or -1, and SIGBUS, once the
 * function or the BAR is gone or for a page past its end. */
static int pcibar_page(void *obj, uint32_t pgoff, uintptr_t *phys)
{
    uintptr_t v = (uintptr_t)obj;
    int key = (int)(v >> 3), bar = (int)(v & 7u);
    uint64_t size = sysfs_pci_bar_size(key, bar), at;
    struct pci_resource r;

    if ((uint64_t)pgoff * PAGE_SIZE >= size) {
        return -1;
    }
    pci_resource((uint8_t)(key >> 8), (uint8_t)((key >> 3) & 31), (uint8_t)(key & 7), bar, &r);
    at = r.start + (uint64_t)pgoff * PAGE_SIZE;
    if (!(r.flags & PCI_IORESOURCE_MEM) || at != (uint64_t)(uintptr_t)at) {
        return -1;
    }
    *phys = (uintptr_t)at;
    return 0;
}

/** The `put` of a resourceN mapping: the mapping holds no reference. */
static void pcibar_put(void *obj)
{
    (void)obj;
}

static const struct vm_device_ops pcibar_vm_ops = {pcibar_page, pcibar_put, NULL};

/**
 * mmap of a resourceN or resourceN_wc file, as Linux's pci_mmap_resource:
 * a shared mapping of the memory BAR's pages from the page offset,
 * uncached for resourceN and write-combining for resourceN_wc, mapped as
 * they are touched. -ENODEV for an I/O BAR, which x86 does not map, or a
 * function that is gone; -EINVAL for a range past the BAR's end, as
 * pci_mmap_fits refuses it, or a private mapping; -ENOMEM.
 */
static int32_t procfs_pcibar_mmap(fd_entry_t *e, struct vma *vma)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    uint64_t pages = (vma->end - vma->start) / PAGE_SIZE, size;
    struct pci_resource r;
    int key, bar = pcibar_of(e, &r, &key);

    if (bar < 0 || !(r.flags & PCI_IORESOURCE_MEM)) {
        return -ENODEV;
    }
    size = (r.end - r.start) / PAGE_SIZE + 1u;
    if (vma->pgoff >= size || size - vma->pgoff < pages) {
        return -EINVAL;
    }
    return vm_device_map(
        vma, &pcibar_vm_ops, (void *)(uintptr_t)((uintptr_t)key << 3 | (uintptr_t)bar),
        (f->origin_task & 0xFFu) >= SYSFS_PCI_RESOURCE0_WC ? VM_PROT_WC : VM_PROT_UC);
}

static const struct file_ops procfs_pcibar_ops = {
    procfs_pcibar_read, procfs_pcibar_write, procfs_poll, procfs_ioctl,
    procfs_close,       procfs_pcibar_mmap,  NULL};

/** Open a resourceN or resourceN_wc file, root's with mode 0600 as
 * Linux's, for the access `pci_file_may` allows. -EACCES for another caller,
 * -ENOENT for a BAR without a length or a function that is not there,
 * -ENOMEM, or the descriptor's error. */
static int procfs_open_pcibar(const struct vpath *vp, int flags)
{
    struct procfs_file *f;
    fd_entry_t *e;
    int acc = flags & 3, fd;

    if (acc == 3 || !pci_file_may(0600, acc)) {
        return -EACCES;
    }
    if (!sysfs_pci_attr_present(vp->syskey, vp->procfile)) {
        return -ENOENT;
    }
    f = kzalloc(sizeof(*f));
    if (!f) {
        return -ENOMEM;
    }
    f->origin_file = SYNTH_SYS_UEVENT;
    f->origin_task = SYSFS_PCI_DEVICE << 8 | vp->procfile;
    f->origin_pid = vp->syskey;
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(f);
        return fd;
    }
    e = &current_proc->fds->entries[fd];
    e->type = FD_TYPE_PROC;
    e->ops = &procfs_pcibar_ops;
    e->priv = f;
    e->flags = acc == 0 ? FD_FLAG_READ : acc == 1 ? FD_FLAG_WRITE : FD_FLAG_READ | FD_FLAG_WRITE;
    if (flags & PROCFS_O_CLOEXEC) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    return fd;
}

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
static int32_t procfs_mem_access(fd_entry_t *e, char __user *dst, const char __user *src,
                                 uint32_t count, int write)
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
        procfs_mm_put(mm);
        return -EIO;
    }
    while (done < count) {
        uint32_t n = count - done < sizeof(buf) ? count - done : (uint32_t)sizeof(buf);

        if (n > PAGE_SIZE - (addr & (PAGE_SIZE - 1u))) {
            n = (uint32_t)(PAGE_SIZE - (addr & (PAGE_SIZE - 1u)));
        }
        if (write) {
            if (copy_from_user(buf, src + done, n) != 0) {
                rc = -EFAULT;
                break;
            }
            rc = paging_access(mm, addr, buf, n, 1);
        } else {
            rc = paging_access(mm, addr, buf, n, 0);
            if (rc == 0 && copy_to_user(dst + done, buf, n) != 0) {
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
    return procfs_mem_access(e, buf, NULL, count, 0);
}

/** The write of a mem descriptor (`procfs_mem_access`). */
static int32_t procfs_mem_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    return procfs_mem_access(e, NULL, buf, count, 1);
}

static const struct file_ops procfs_mem_ops = {
    procfs_mem_read, procfs_mem_write, procfs_poll, procfs_ioctl, procfs_close, NULL, NULL};

int procfs_is_mem(const fd_entry_t *e)
{
    return e->type == FD_TYPE_PROC && e->priv &&
           ((const struct procfs_file *)e->priv)->origin_file == SYNTH_PROC_MEM;
}

/**
 * Open /proc/<pid>/mem for reading, writing or both: the caller is one
 * `procfs_may_read_mem` allows, as Linux's mm_access with
 * PTRACE_MODE_ATTACH_FSCREDS, past the file's mode of 0600. The
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
    if (!procfs_may_read_mem(p)) {
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
    procfs_own(f, vp, pid);
    e->ofd->offset = 0;
    e->flags = acc == 0 ? FD_FLAG_READ : acc == 1 ? FD_FLAG_WRITE : (FD_FLAG_READ | FD_FLAG_WRITE);
    if (flags & PROCFS_O_CLOEXEC) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    return fd;
}

/**
 * Render /proc/audio, whose text `audio_proc_text` writes from one snapshot:
 * its length, or -1 with `nomem` set when the heap has no room. The text
 * published is as long as the pass that wrote it said, and a pass that
 * needed more room than the snapshot's first measure runs again with it.
 */
static int procfs_gen_audio(struct procfs_text *t)
{
    int n = audio_proc_text(NULL, 0);
    int m;

    for (;;) {
        if (procfs_text_room(t, t->len + n) < 0)
            return -1;
        m = audio_proc_text(t->buf + t->len, n);
        if (m <= n)
            break;
        n = m;
    }
    t->len += m;
    return t->len;
}

/* The range of an OOM score adjustment, Linux's OOM_SCORE_ADJ_MIN and
 * OOM_SCORE_ADJ_MAX. */
#define OOM_SCORE_ADJ_MIN (-1000)
#define OOM_SCORE_ADJ_MAX 1000

/** Give a process node Linux's task_dump_owner ownership (procfs.h). */
int procfs_pid_owner(int pid, uint32_t mode, uint32_t *uid, uint32_t *gid)
{
    process_t *p = procfs_proc(pid);

    if (!p) {
        return -ENOENT;
    }
    *uid = 0;
    *gid = 0;
    if (mode == 0040555u || (p->mm && p->mm->dumpable == 1u)) {
        *uid = p->euid;
        *gid = p->egid;
    }
    return 0;
}

/** The owner of oom_score_adj and the other process files, mode 0644. */
int procfs_oom_owner(int pid, uint32_t *uid, uint32_t *gid)
{
    return procfs_pid_owner(pid, 0100644u, uid, gid);
}

/**
 * Name in `node` the file `vp` names whose owner its lookup sets, a file of
 * /proc/<pid> other than one of its net directory, or a descriptor's fdinfo
 * file: its kind, its file or descriptor, the process or thread `id` that
 * `procfs_resolve_id` gave, and for a thread's file its process. Returns 1,
 * or 0 for any other node.
 */
static int procfs_node_of(const struct vpath *vp, int id, int node[4])
{
    if (vp->kind == VNODE_PROC_FILE ? synth_net_name(vp->procfile) != NULL
                                    : vp->kind != VNODE_PROC_FDINFO) {
        return 0;
    }
    node[0] = vp->kind;
    node[1] = vp->kind == VNODE_PROC_FDINFO ? vp->procfd : vp->procfile;
    node[2] = id;
    node[3] = vp->tid != 0 ? procfs_resolve_pid(vp->pid) : 0;
    return 1;
}

/**
 * Give every open file of the node `node` (`procfs_node_of`) the owner
 * Linux's lookup gives its inode, task_dump_owner's for a file: the
 * effective IDs of the process or thread `node[2]` while it is dumpable,
 * and root otherwise (`procfs_oom_owner`). Linux sets it when the lookup
 * makes the inode and again at each walk that finds the dentry
 * (fs/proc/base.c:2693 and 2066, fs/proc/fd.c:393 and 161).
 */
static void procfs_set_owner(const int node[4])
{
    struct procfs_file *f;
    uint32_t uid = 0, gid = 0;

    if (procfs_oom_owner(node[2], &uid, &gid) != 0) {
        return;
    }
    for (f = procfs_owned; f; f = f->owned_next) {
        if (f->node[0] == node[0] && f->node[1] == node[1] && f->node[2] == node[2] &&
            f->node[3] == node[3]) {
            f->uid = uid;
            f->gid = gid;
        }
    }
}

/**
 * Put the record `f` of a descriptor just opened on `vp`, for the process
 * or thread `id`, on `procfs_owned` when its node's lookup sets its owner,
 * and give it and the node's other open files the owner of this lookup.
 */
static void procfs_own(struct procfs_file *f, const struct vpath *vp, int id)
{
    if (!procfs_node_of(vp, id, f->node)) {
        return;
    }
    f->owned = 1;
    f->owned_next = procfs_owned;
    procfs_owned = f;
    procfs_set_owner(f->node);
}

void procfs_lookup_owner(const struct vpath *vp)
{
    int node[4];
    int id = procfs_resolve_id(vp);

    if (id >= 0 && procfs_node_of(vp, id, node)) {
        procfs_set_owner(node);
    }
}

int procfs_fd_owner(struct fd_entry *e, uint32_t *uid, uint32_t *gid)
{
    const struct procfs_file *f;

    if (!e || e->type != FD_TYPE_PROC || !e->priv) {
        return -1;
    }
    f = (const struct procfs_file *)e->priv;
    if (!f->owned) {
        return -1;
    }
    *uid = f->uid;
    *gid = f->gid;
    return f->origin_file == SYNTH_PROC_OOM_ADJ || f->origin_file == SYNTH_PROC_COMM ||
           procfs_is_idmap(f->origin_file);
}

int procfs_fd_writable(struct fd_entry *e)
{
    const struct procfs_file *f;

    if (!e || e->type != FD_TYPE_PROC || !e->priv) {
        return 0;
    }
    f = (const struct procfs_file *)e->priv;
    if (f->origin_file == SYNTH_SYS_UEVENT) {
        if (f->origin_task >> 8 == SYSFS_REG) {
            return sysfs_node_writable(f->origin_pid) ? (int)sysfs_node_mode(f->origin_pid) : 0;
        }
        return sysfs_attr_writable(f->origin_task >> 8, f->origin_task & 255) ? 0644 : 0;
    }
    if (f->origin_file == SYNTH_PROC_ACPI_CALL) {
        return 0600;
    }
    return f->origin_file == SYNTH_PROC_SYSCTL && synth_sys_writable(f->origin_pid) ? 0644 : 0;
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

static const struct file_ops procfs_oom_ops = {
    procfs_oom_read, procfs_oom_write, procfs_poll, procfs_ioctl, procfs_close, NULL, NULL};

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
    char *text;

    if (!p) {
        return -ENOENT;
    }
    text = kmalloc(NS_MAP_TEXT_MAX);
    if (!text) {
        t->nomem = 1;
        return t->len;
    }
    ns_map_text(proc_userns(p), idmap_which(file), current_proc, text, NS_MAP_TEXT_MAX);
    ap_str(t, text);
    kfree(text);
    return t->len;
}

/** Read a user namespace's file or a `comm` opened for writing as well:
 * its text at this read, from the description's offset; -ESRCH once the
 * process has been reaped. */
static int32_t procfs_idmap_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    uint32_t off = e->ofd->offset > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)e->ofd->offset;
    struct procfs_text t;
    uint32_t n = 0;
    int rc;

    procfs_text_init(&t);
    rc = f->origin_file == SYNTH_PROC_COMM ? procfs_gen_comm(&t, f->origin_pid)
                                           : procfs_gen_idmap(&t, f->origin_pid, f->origin_file);
    if (rc < 0 || t.nomem) {
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

/* The bytes a write of a map must stay under, a page, as on Linux. */
#define IDMAP_WRITE_MAX 4096u

/** Write a user namespace's map or its setgroups, as `ns_map_write` takes
 * it, from the `count` bytes of `buf`, fewer than IDMAP_WRITE_MAX, at
 * offset 0: what `ns_map_write` returns, -EINVAL at another offset or for
 * a page or more, -EFAULT, -ENOMEM, or -ESRCH once the process has been
 * reaped. */
static int32_t procfs_idmap_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    process_t *p = procfs_proc(f->origin_pid);
    char *text;
    int32_t rc;

    if (!p) {
        return -ESRCH;
    }
    if (e->ofd->offset != 0 || count >= IDMAP_WRITE_MAX) {
        return -EINVAL;
    }
    text = kmalloc(count + 1u);
    if (!text) {
        return -ENOMEM;
    }
    rc = copy_from_user(text, buf, count) != 0
             ? -EFAULT
             : ns_map_write(proc_userns(p), idmap_which(f->origin_file), text, count, current_proc);
    kfree(text);
    return rc;
}

static const struct file_ops procfs_idmap_ops = {
    procfs_idmap_read, procfs_idmap_write, procfs_poll, procfs_ioctl, procfs_close, NULL, NULL};

/**
 * Write /proc/<pid>/comm or /proc/<pid>/task/<tid>/comm as Linux's
 * `comm_write` does: the first 15 bytes of `buf`, NUL-padded, become the
 * thread's name, whatever they hold, a newline included. Only a thread of
 * the writer's own thread group is renamed. `count`, all of it taken even
 * when cut, or -EFAULT, -EINVAL for a thread of another group, or -ESRCH
 * once the thread has been reaped.
 */
static int32_t procfs_comm_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    char name[sizeof(((process_t *)0)->comm)];
    uint32_t n = count < sizeof(name) - 1u ? count : (uint32_t)sizeof(name) - 1u;
    process_t *p;

    k_memset(name, 0, sizeof(name));
    if (copy_from_user(name, buf, n) != 0) {
        return -EFAULT;
    }
    p = procfs_proc(f->origin_pid);
    if (!p) {
        return -ESRCH;
    }
    if (p->tg != current_proc->tg) {
        return -EINVAL;
    }
    k_memcpy(p->comm, name, sizeof(name));
    return (int32_t)count;
}

static const struct file_ops procfs_comm_ops = {
    procfs_idmap_read, procfs_comm_write, procfs_poll, procfs_ioctl, procfs_close, NULL, NULL};

/**
 * Open /proc/<pid>/uid_map, gid_map, setgroups or comm, `file` of the path
 * `vp`, for writing, or for reading and writing. Who may write a map is the
 * namespace's rule (`ns_map_write`), checked at the write, which must be at
 * the start; a `comm`, whose mode is 0644, takes a writer that owns it
 * (`procfs_oom_owner`) or a privileged one, checked here. -EACCES for an
 * access mode of 3 or a writer who may not, and -ENOENT for no such process.
 */
static int procfs_open_idmap(const struct vpath *vp, int file, int pid, int flags)
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
    if (file == SYNTH_PROC_COMM && !proc_fs_privileged(current_proc) &&
        proc_fsuid(current_proc) != uid) {
        return -EACCES;
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
    e->ops = file == SYNTH_PROC_COMM ? &procfs_comm_ops : &procfs_idmap_ops;
    e->priv = f;
    procfs_own(f, vp, pid);
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
    if (acc != 0 && !proc_fs_privileged(current_proc) && proc_fsuid(current_proc) != uid) {
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
    procfs_own(f, vp, pid);
    e->ofd->offset = 0;
    e->flags = acc == 0 ? FD_FLAG_READ : acc == 1 ? FD_FLAG_WRITE : (FD_FLAG_READ | FD_FLAG_WRITE);
    if (flags & PROCFS_O_CLOEXEC) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    return fd;
}

/* The most seconds a /proc/sys file of TCP's takes: as many as the kernel's
 * clock counts in 31 bits. */
#define SYSCTL_SECONDS_MAX (0x7FFFFFFF / TICK_HZ)

/* The /proc/sys files whose values are their owners' variables, and the
 * range a write must fall in, as Linux's `proc_dointvec_minmax` takes one;
 * kernel/panic and kernel/panic_on_oops take any int, as Linux's
 * `proc_dointvec` does (kernel/sysctl.c). */
static const struct {
    int entry;
    int *var;
    int min;
    int max;
} sysctl_ints[] = {{SYNTH_SYS_INOTIFY_QUEUED, &inotify_max_queued_events, 0, 0x7FFFFFFF},
                   {SYNTH_SYS_INOTIFY_INSTANCES, &inotify_max_user_instances, 0, 0x7FFFFFFF},
                   {SYNTH_SYS_INOTIFY_WATCHES, &inotify_max_user_watches, 0, 0x7FFFFFFF},
                   {SYNTH_SYS_MAX_MAP_COUNT, &vm_max_map_count, 0, 0x7FFFFFFF},
                   {SYNTH_SYS_OVERFLOWGID, &ns_overflowgid, 0, 65535},
                   {SYNTH_SYS_OVERFLOWUID, &ns_overflowuid, 0, 65535},
                   {SYNTH_SYS_NR_OPEN, &fd_nr_open, FD_NR_OPEN_MIN, FD_NR_OPEN_MAX},
                   {SYNTH_SYS_RANDOMIZE_VA_SPACE, &randomize_va_space, 0, 2},
                   {SYNTH_SYS_TCP_KEEPALIVE_TIME, &tcp_keepalive_time, 1, SYSCTL_SECONDS_MAX},
                   {SYNTH_SYS_TCP_KEEPALIVE_TIME + 1, &tcp_keepalive_intvl, 1, SYSCTL_SECONDS_MAX},
                   {SYNTH_SYS_TCP_KEEPALIVE_TIME + 2, &tcp_keepalive_probes, 0, 255},
                   {SYNTH_SYS_TCP_KEEPALIVE_TIME + 3, &tcp_mtu_probing, 0, 2},
                   {SYNTH_SYS_TCP_RETRIES2, &tcp_retries2, 0, 255},
                   {SYNTH_SYS_PANIC, &panic_timeout, -0x7FFFFFFF - 1, 0x7FFFFFFF},
                   {SYNTH_SYS_PANIC_ON_OOPS, &panic_on_oops, -0x7FFFFFFF - 1, 0x7FFFFFFF}};

/** The index in `sysctl_ints` of the /proc/sys file `entry`, or -1. */
static int sysctl_int(int entry)
{
    int i;

    for (i = 0; i < (int)(sizeof(sysctl_ints) / sizeof(sysctl_ints[0])); i++) {
        if (sysctl_ints[i].entry == entry) {
            return i;
        }
    }
    return -1;
}

/** Append a random UUID of version 4 as Linux's kernel/random files give
 * one: 36 characters in five groups of hexadecimal digits. */
static void ap_uuid(struct procfs_text *t, const uint8_t *b)
{
    static const char digits[] = "0123456789abcdef";
    int i;

    for (i = 0; i < 16; i++) {
        uint8_t v = b[i];

        if (i == 6) {
            v = (uint8_t)((v & 0x0Fu) | 0x40u);
        } else if (i == 8) {
            v = (uint8_t)((v & 0x3Fu) | 0x80u);
        }
        if (i == 4 || i == 6 || i == 8 || i == 10) {
            ap_ch(t, '-');
        }
        ap_ch(t, digits[v >> 4]);
        ap_ch(t, digits[v & 15u]);
    }
}

/** The `which` of nd6.h for the net/ipv6/conf directory `dir`, a name
 * `synth_sys_ipv6` gave: `all`, `default`, or the index of the interface
 * of that name, -ENODEV when there is none. */
static int sysctl_ipv6_which(const char *dir)
{
    int len = (int)k_strlen(dir), index;

    if (len == 3 && dir[0] == 'a') {
        return ND6_CONF_ALL;
    }
    if (len == 7 && dir[0] == 'd') {
        return ND6_CONF_DEFAULT;
    }
    index = sysfs_net_index(dir, len);
    return index > 0 ? index : -ENODEV;
}

/* The ND6_ setting of each SYNTH_IPV6_ file but stable_secret. */
static const int sysctl_ipv6_keys[SYNTH_IPV6_KEYS] = {
    ND6_ACCEPT_RA,        ND6_AUTOCONF, ND6_DISABLE_IPV6, ND6_FORWARDING,
    ND6_RA_DEFRTR_METRIC, -1,           ND6_USE_TEMPADDR};

/**
 * The text of the net/ipv6/conf file `key` of the directory `dir`,
 * appended to `t`: the setting, or the stable secret as an IPv6 address.
 * Returns 0, -EIO for a secret not written, as Linux reads one, or -ENODEV
 * for an interface gone.
 */
static int sysctl_ipv6_text(struct procfs_text *t, const char *dir, int key)
{
    int which = sysctl_ipv6_which(dir);
    uint8_t secret[IP6_ADDR_LEN];
    char text[40];
    int32_t v;
    int rc;

    if (which == -ENODEV) {
        return which;
    }
    if (key == SYNTH_IPV6_STABLE_SECRET) {
        rc = nd6_secret_get(which, secret);
        if (rc == 0) {
            ip6_format(secret, text);
            ap_str(t, text);
            ap_ch(t, '\n');
        }
        return rc;
    }
    rc = nd6_conf_get(which, sysctl_ipv6_keys[key], &v);
    if (rc == 0) {
        ap_int(t, (int)v);
        ap_ch(t, '\n');
    }
    return rc;
}

/**
 * The text a read of the /proc/sys file `entry` gives, appended to `t`.
 *
 * fs/file-nr is Linux's three figures: the open file descriptions, 0 for
 * the freed ones Linux keeps, and fs/file-max. kernel/random/boot_id is
 * drawn once, at its first read, and stays for the
 * boot; kernel/random/uuid is drawn anew for each open, as Linux draws it
 * for each read. Both come from the kernel's generator, whatever its seed,
 * since neither is a key. A file of net/ipv6/conf is `sysctl_ipv6_text`'s.
 *
 * Returns 0, or a negative errno: -EIO for a stable secret not written.
 */
static int procfs_sysctl_text(struct procfs_text *t, int entry)
{
    static uint8_t boot_id[16];
    static int boot_id_drawn;
    uint8_t uuid[16];
    int i = sysctl_int(entry);
    const char *dir;
    int key;

    if (synth_sys_ipv6(entry, &dir, &key) && key >= 0) {
        return sysctl_ipv6_text(t, dir, key);
    }
    if (entry == SYNTH_SYS_HOSTNAME || entry == SYNTH_SYS_DOMAINNAME) {
        ap_str(t, entry == SYNTH_SYS_HOSTNAME ? uts_hostname() : uts_domainname());
        ap_ch(t, '\n');
    } else if (entry == SYNTH_SYS_MAX_MNT_NAMESPACES) {
        ap_int(t, ns_max_mnt(proc_userns(current_proc)));
        ap_ch(t, '\n');
    } else if (i >= 0) {
        ap_int(t, *sysctl_ints[i].var);
        ap_ch(t, '\n');
    } else if (entry == SYNTH_SYS_FILE_MAX) {
        ap_ulong(t, fd_file_max);
        ap_ch(t, '\n');
    } else if (entry == SYNTH_SYS_PIPE_MAX_SIZE) {
        ap_ulong(t, pipe_max_size);
        ap_ch(t, '\n');
    } else if (entry == SYNTH_SYS_PIPE_USER_PAGES_HARD || entry == SYNTH_SYS_PIPE_USER_PAGES_SOFT) {
        ap_ulong(t, entry == SYNTH_SYS_PIPE_USER_PAGES_HARD ? pipe_user_pages_hard
                                                            : pipe_user_pages_soft);
        ap_ch(t, '\n');
    } else if (entry == SYNTH_SYS_FILE_NR) {
        ap_ulong(t, fd_files_open());
        ap_str(t, "\t0\t");
        ap_ulong(t, fd_file_max);
        ap_ch(t, '\n');
    } else if (entry == SYNTH_SYS_BOOT_ID || entry == SYNTH_SYS_UUID) {
        if (entry == SYNTH_SYS_BOOT_ID && !boot_id_drawn) {
            krandom_bytes(boot_id, sizeof(boot_id));
            boot_id_drawn = 1;
        }
        if (entry == SYNTH_SYS_UUID) {
            krandom_bytes(uuid, sizeof(uuid));
        }
        ap_uuid(t, entry == SYNTH_SYS_BOOT_ID ? boot_id : uuid);
        ap_ch(t, '\n');
    } else {
        ap_str(t, synth_sys_text(entry));
    }
    return 0;
}

/** Read a /proc/sys file opened for writing as well: its value at this
 * read, from the description's offset. */
static int32_t procfs_sysctl_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    uint32_t off = e->ofd->offset > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)e->ofd->offset;
    struct procfs_text t;
    uint32_t n = 0;
    int rc;

    procfs_text_init(&t);
    rc = procfs_sysctl_text(&t, f->origin_pid);
    if (rc != 0 || t.nomem) {
        kfree(t.buf);
        return rc != 0 ? rc : -ENOMEM;
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
 * Store the number of the NUL-ended text `s` in `*v`, as Linux's
 * `proc_doulongvec_minmax` takes a write to fs/file-max and the pipe page
 * limits and `proc_douintvec` one to fs/pipe-max-size: spaces stripped at
 * both ends, then a number in base 0, as `oom_parse` reads one, from 0 to
 * `limit`. 0, or -EINVAL for a sign, `+` or `-`, as Linux's `proc_get_long`
 * and its callers refuse either, text that is no number, or a number out of
 * range, which leaves `*v` as it was.
 */
static int sysctl_parse_ulong(const char *s, unsigned long limit, unsigned long *v)
{
    const char *end = s + k_strlen(s);
    unsigned long base = 10, mag = 0, d;
    int digits = 0;

    while (oom_space(*s)) {
        s++;
    }
    while (end > s && oom_space(end[-1])) {
        end--;
    }
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X') && oom_digit(s[2]) < 16u) {
        base = 16;
        s += 2;
    } else if (s[0] == '0') {
        base = 8;
    }
    for (; s < end; s++, digits++) {
        d = oom_digit(*s);
        if (d >= base || mag > (limit - d) / base) {
            return -EINVAL;
        }
        mag = mag * base + d;
    }
    if (digits == 0) {
        return -EINVAL;
    }
    *v = mag;
    return 0;
}

/**
 * Write the NUL-ended text `text` to the /proc/sys file `entry` when its
 * value is an unsigned long: fs/file-max, up to LONG_MAX; fs/pipe-max-size,
 * up to 2^31 and rounded (`pipe_round_size`); fs/pipe-user-pages-hard and
 * fs/pipe-user-pages-soft, any. Returns 0 for any other file, and otherwise
 * 1, with 0 or -EINVAL in `*rc`.
 */
static int sysctl_ulong_write(int entry, const char *text, int *rc)
{
    unsigned long v;

    switch (entry) {
    case SYNTH_SYS_FILE_MAX:
        *rc = sysctl_parse_ulong(text, ~0ul >> 1, &fd_file_max);
        return 1;
    case SYNTH_SYS_PIPE_MAX_SIZE:
        *rc = sysctl_parse_ulong(text, ~0ul, &v);
        if (*rc == 0 && v > 0x80000000ul) {
            *rc = -EINVAL;
        }
        if (*rc == 0) {
            pipe_max_size = pipe_round_size((uint32_t)v);
        }
        return 1;
    case SYNTH_SYS_PIPE_USER_PAGES_HARD:
        *rc = sysctl_parse_ulong(text, ~0ul, &pipe_user_pages_hard);
        return 1;
    case SYNTH_SYS_PIPE_USER_PAGES_SOFT:
        *rc = sysctl_parse_ulong(text, ~0ul, &pipe_user_pages_soft);
        return 1;
    default:
        return 0;
    }
}

/** Set the caller's user namespace's user/max_mnt_namespaces to `v`:
 * `count`, or -EINVAL out of its range. */
static int32_t sysctl_max_mnt_write(int32_t v, uint32_t count)
{
    int rc = ns_set_max_mnt(proc_userns(current_proc), v);

    return rc != 0 ? rc : (int32_t)count;
}

/**
 * Write a /proc/sys file, as Linux's sysctl handlers take one.
 *
 * kernel/hostname and kernel/domainname become the text up to its first
 * newline, at most 64 bytes, as sethostname(2) and setdomainname(2) would
 * set them; the whole count is taken. fs/file-max and the pipe limits take
 * an unsigned long (`sysctl_ulong_write`), and fs/pipe-max-size rounds it
 * as Linux's `round_pipe_size` does, so that 1 sets a page. A write to
 * another file is an integer, read as `oom_parse` reads one. A file of
 * `sysctl_ints` stores it in its owner's variable, which the owner reads at
 * its next use, and a value out of the file's range is -EINVAL.
 * user/max_mnt_namespaces is the
 * limit of the caller's user namespace, from 0 to INT_MAX
 * (`ns_set_max_mnt`). To net/ipv4/ip_forward 0, its
 * value, is taken, and any other value is -EINVAL, since the stack forwards
 * nothing between interfaces, where Linux would begin to. A file of
 * net/ipv6/conf is `nd6_conf_set`'s, with its checks, and stable_secret
 * takes an IPv6 address up to the first space or newline
 * (`nd6_secret_set`), -EINVAL for text that is none. -EFAULT for a buffer
 * the caller cannot read.
 */
static int32_t procfs_sysctl_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    struct procfs_file *f = (struct procfs_file *)e->priv;
    char text[65];
    uint32_t n = count < 64u ? count : 64u;
    uint32_t len = 0;
    int32_t v;
    int rc;
    int i;
    const char *dir;
    int key;

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
    if (sysctl_ulong_write(f->origin_pid, text, &rc)) {
        return rc != 0 ? rc : (int32_t)count;
    }
    if (synth_sys_ipv6(f->origin_pid, &dir, &key) && key == SYNTH_IPV6_STABLE_SECRET) {
        uint8_t secret[IP6_ADDR_LEN];
        int which = sysctl_ipv6_which(dir);

        while (len < n && text[len] != '\n' && text[len] != ' ' && text[len] != '\0') {
            len++;
        }
        if (which == -ENODEV) {
            return which;
        }
        if (ip6_parse(text, len, secret) != 0) {
            return -EINVAL;
        }
        rc = nd6_secret_set(which, secret);
        return rc != 0 ? rc : (int32_t)count;
    }
    text[12] = '\0';
    rc = oom_parse(text, &v);
    if (rc != 0) {
        return rc;
    }
    if (synth_sys_ipv6(f->origin_pid, &dir, &key)) {
        int which = sysctl_ipv6_which(dir);

        rc = which == -ENODEV ? which : nd6_conf_set(which, sysctl_ipv6_keys[key], v);
        return rc != 0 ? rc : (int32_t)count;
    }
    if (f->origin_pid == SYNTH_SYS_MAX_MNT_NAMESPACES) {
        return sysctl_max_mnt_write(v, count);
    }
    i = sysctl_int(f->origin_pid);
    if (i >= 0) {
        if (v < sysctl_ints[i].min || v > sysctl_ints[i].max) {
            return -EINVAL;
        }
        *sysctl_ints[i].var = (int)v;
        return (int32_t)count;
    }
    return v == 0 ? (int32_t)count : -EINVAL;
}

static const struct file_ops procfs_sysctl_ops = {
    procfs_sysctl_read, procfs_sysctl_write, procfs_poll, procfs_ioctl, procfs_close, NULL, NULL};

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
    len = f->origin_task >> 8 == SYSFS_BACKLIGHT
              ? procfs_gen_backlight(&t, f->origin_pid, f->origin_task & 255)
          : f->origin_task >> 8 == SYSFS_RFKILL
              ? procfs_gen_rfkill(&t, f->origin_pid, f->origin_task & 255)
          : f->origin_task >> 8 == SYSFS_REG
              ? procfs_gen_regattr(&t, f->origin_pid)
              : procfs_gen_sysattr(&t, f->origin_task >> 8, f->origin_task & 255);
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

/**
 * Take a write of the `count` bytes at `text`, NUL-terminated, to rtc0's
 * wakealarm, as Linux's wakealarm_store (drivers/rtc/sysfs.c:152-213):
 * the clock read first; then seconds since 1970, `+N` for N seconds from
 * now, or `+=N` to move a set alarm N seconds later, the number read as
 * kstrtos64 with base 0 reads it (`k_strtoll`). A time after now, or any
 * push, sets the alarm: -EBUSY while one is set but for a push, and
 * -EINVAL for a push with none set or to a time before 1970, which
 * Linux's rtc_valid_tm refuses (drivers/rtc/lib.c:161-172); a push to a
 * time not after now raises the alarm at once. Any other time disables it.
 * The count, -EIO when the clock cannot be read, the parse's error, or the
 * error of `devfs_rtc_set_wakealarm`.
 */
static int32_t wakealarm_store(const char *text, uint32_t count)
{
    const char *p = text;
    rtc_time_t now_tm;
    long long alarm;
    int64_t now, set;
    int adjust = 0, push = 0;
    int32_t rc;

    if (rtc_read_clock(&now_tm) != 0) {
        return -EIO;
    }
    now = rtc_seconds_of(&now_tm);
    if (*p == '+') {
        p++;
        if (*p == '=') {
            p++;
            push = 1;
        } else {
            adjust = 1;
        }
    }
    rc = k_strtoll(p, count - (uint32_t)(p - text), &alarm);
    if (rc != 0) {
        return rc;
    }
    /* The sums wrap, as Linux's time64_t arithmetic does. */
    if (adjust) {
        alarm = (long long)((uint64_t)alarm + (uint64_t)now);
    }
    if (alarm > now || push) {
        set = devfs_rtc_wakealarm();
        if (set >= 0 && !push) {
            return -EBUSY;
        }
        if (set < 0 && push) {
            return -EINVAL;
        }
        if (push) {
            alarm = (long long)((uint64_t)alarm + (uint64_t)set);
            if (alarm < 0) {
                return -EINVAL;
            }
        }
        rc = devfs_rtc_set_wakealarm(alarm);
    } else {
        rc = devfs_rtc_set_wakealarm(-1);
    }
    return rc != 0 ? rc : (int32_t)count;
}

/**
 * Take a write of the `count` bytes at `text` to the uevent `attr` of the
 * device `key` of `node` (`sysfs_attr_uevent`), as Linux's
 * dev_uevent_store: the action and its arguments (`sysfs_uevent_parse`) are
 * sent as that uevent with the device's variables, so that udev's coldplug
 * hears the device. `count`, -EINVAL, -ENOENT for a device that is gone,
 * or -ENOMEM.
 */
static int32_t procfs_uevent_write(int node, int key, int attr, const char *text, uint32_t count)
{
    char env[SYSFS_SYNTH_ENV_MAX], action[8], path[96];
    const char *subsystem = node == SYSFS_PCI_DEVICE   ? "pci"
                            : node == SYSFS_USB_DEVICE ? "usb"
                            : node == SYSFS_NET_IF     ? "net"
                            : node == SYSFS_WLAN_PHY   ? "ieee80211"
                            : node == SYSFS_RFKILL     ? "rfkill"
                                                       : "block";
    struct procfs_text vars;
    struct vpath vp;
    int32_t rc;
    int got = key;

    rc = sysfs_uevent_parse(text, count, action, env);
    if (rc != 0) {
        return rc;
    }
    k_memset(&vp, 0, sizeof(vp));
    vp.kind = VNODE_SYS_FILE;
    vp.sysent = node;
    vp.syskey = key;
    vp.procfile = attr;
    procfs_text_init(&vars);
    rc = procfs_gen_sysfile(&vars, &vp, &got);
    if (rc >= 0 && vars.nomem) {
        rc = -ENOMEM;
    }
    if (rc >= 0) {
        synth_sys_path(node, key, path, (int)sizeof(path));
        rc = sysfs_uevent_path(path, subsystem, action, env, vars.buf, (uint32_t)vars.len);
    }
    kfree(vars.buf);
    return rc < 0 ? rc : (int32_t)count;
}

/**
 * Give the `count` bytes at `text`, NUL-terminated, to the store of the
 * writable /sys attribute of `f`, each of which parses them with its own
 * limits. A registered attribute (sysfs.h) takes them as written, through
 * its `store`, and a uevent file as `procfs_uevent_write` does; the
 * transparent_hugepage files go to `thp_store`. A backlight's brightness
 * and bl_power take what `backlight_store` takes, and a radio switch's
 * soft and state what `rfkill_store` takes, with their errors, given the
 * text whole, as Linux gives the buffer to kstrtoul, which takes one
 * newline; rtc0's wakealarm goes whole to `wakealarm_store`.
 * /sys/power/state has its final newlines and spaces cut, then takes
 * `freeze` or
 * `mem`, both suspend to idle, which the write waits in until a wake
 * event, or has the error of `suspend_to_idle`; another state is -EINVAL.
 * The count taken, or -errno.
 */
static int32_t procfs_sysattr_store(const struct procfs_file *f, char *text, uint32_t count)
{
    int node = f->origin_task >> 8, attr = f->origin_task & 255;
    uint32_t n = count;
    int32_t rc;

    if (node == SYSFS_REG) {
        return sysfs_node_store(f->origin_pid, text, count);
    }
    if (sysfs_attr_uevent(node, attr)) {
        return procfs_uevent_write(node, f->origin_pid, attr, text, count);
    }
    if (node == SYSFS_THP) {
        return thp_store(attr, text, count);
    }
    if (node == SYSFS_BACKLIGHT) {
        rc = backlight_store(f->origin_pid, attr, text, (int)count);
        return rc != 0 ? rc : (int32_t)count;
    }
    if (node == SYSFS_RFKILL) {
        rc = rfkill_store(f->origin_pid, attr, text, (int)count);
        return rc != 0 ? rc : (int32_t)count;
    }
    if (node != SYSFS_POWER) {
        return wakealarm_store(text, count);
    }
    while (n > 0 && (text[n - 1] == '\n' || text[n - 1] == ' ')) {
        text[--n] = '\0';
    }
    if (!sysattr_is(text, n, "freeze") && !sysattr_is(text, n, "mem")) {
        return -EINVAL;
    }
    rc = suspend_to_idle();
    return rc != 0 ? rc : (int32_t)count;
}

/**
 * Write a writable /sys attribute, as Linux's kernfs writes a sysfs file:
 * the first PAGE_SIZE bytes at most of the `count` at `buf` go to the
 * attribute's store (`procfs_sysattr_store`), NUL-terminated, so that a
 * longer write is taken in part. An empty write is 0 with no store
 * called, as Linux's sysfs_kf_write answers it. The count taken, or
 * -EFAULT, -ENOMEM or the store's error.
 */
static int32_t procfs_sysattr_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    char *text;
    int32_t rc;

    if (count == 0) {
        return 0;
    }
    if (count > PAGE_SIZE) {
        count = PAGE_SIZE;
    }
    text = kmalloc(count + 1u);
    if (!text) {
        return -ENOMEM;
    }
    if (copy_from_user(text, buf, count) != 0) {
        rc = -EFAULT;
    } else {
        text[count] = '\0';
        rc = procfs_sysattr_store((const struct procfs_file *)e->priv, text, count);
    }
    kfree(text);
    return rc;
}

static const struct file_ops procfs_sysattr_ops = {
    procfs_sysattr_read, procfs_sysattr_write, procfs_poll, procfs_ioctl, procfs_close, NULL, NULL};

/**
 * Open the writable attribute `attr` of the /sys directory `node` of the
 * device `key` for writing, or for reading and writing, which needs a
 * privileged caller, as its mode, 0644 and owned by root, says. Each read
 * and write reaches the value as it is then. The descriptor is a sysfs one,
 * with the node and the attribute in `origin_task` and the key in
 * `origin_pid`, as a snapshot of a /sys file has them.
 */
static int procfs_open_sysattr(int node, int key, int attr, int flags)
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
    f->origin_pid = key;
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

/** Read the result of the last ACPI call (`acpi_call_result`) and its NUL,
 * as Linux's acpi_call gives them, from the description's offset, which
 * moves past what is read: the bytes read, 0 at the end, or -EFAULT. */
static int32_t procfs_acpicall_read(fd_entry_t *e, char __user *buf, uint32_t count)
{
    char text[ACPI_CALL_MAX];
    uint32_t off = e->ofd->offset > 0xFFFFFFFFu ? 0xFFFFFFFFu : (uint32_t)e->ofd->offset;
    uint32_t len = (uint32_t)acpi_call_result(text, (int)sizeof(text)) + 1;
    uint32_t n = 0;

    if (off < len) {
        n = len - off < count ? len - off : count;
        if (copy_to_user(buf, text + off, n) != 0) {
            return -EFAULT;
        }
    }
    e->ofd->offset = off + n;
    return (int32_t)n;
}

/** Evaluate the method a write names (`acpi_call_write`), the whole count
 * taken: the count, -EINVAL for a request of ACPI_CALL_MAX bytes or more,
 * -EFAULT, or the call's error. */
static int32_t procfs_acpicall_write(fd_entry_t *e, const char __user *buf, uint32_t count)
{
    char text[ACPI_CALL_MAX];
    int rc;

    (void)e;
    if (count >= sizeof(text)) {
        return -EINVAL;
    }
    if (copy_from_user(text, buf, count) != 0) {
        return -EFAULT;
    }
    text[count] = '\0';
    rc = acpi_call_write(text, count);
    return rc != 0 ? rc : (int32_t)count;
}

static const struct file_ops procfs_acpicall_ops = {procfs_acpicall_read,
                                                    procfs_acpicall_write,
                                                    procfs_poll,
                                                    procfs_ioctl,
                                                    procfs_close,
                                                    NULL,
                                                    NULL};

/**
 * Open /proc/acpi/call, which needs a privileged caller, as its mode, 0600
 * and owned by root, says. O_CREAT and O_TRUNC are taken as they are for a
 * file that exists, which a shell's redirection asks for. A write evaluates
 * a method and a read gives the result of the last.
 */
static int procfs_open_acpicall(int flags)
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
    f->origin_file = SYNTH_PROC_ACPI_CALL;
    fd = fd_alloc(current_proc->fds);
    if (fd < 0) {
        kfree(f);
        return fd;
    }
    e = &current_proc->fds->entries[fd];
    e->type = FD_TYPE_PROC;
    e->node = NULL;
    e->sock = NULL;
    e->ops = &procfs_acpicall_ops;
    e->priv = f;
    e->ofd->offset = 0;
    e->flags = acc == 0 ? FD_FLAG_READ : acc == 1 ? FD_FLAG_WRITE : (FD_FLAG_READ | FD_FLAG_WRITE);
    if (flags & PROCFS_O_CLOEXEC) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    return fd;
}

/**
 * Open a writable /proc/sys file for writing, or for reading and writing, which
 * needs a privileged caller, as the file's mode, 0644 and owned by root, says;
 * the names of the UTS namespace need privilege over it, and a limit of
 * /proc/sys/user privilege in the caller's user namespace, whose limit it
 * is, as Linux lets the root of a user namespace write its own. O_CREAT and
 * O_TRUNC are taken as they are for a file that exists, which is what a
 * shell's redirection and BusyBox's `sysctl -w` ask for. Each read and
 * write reaches the value as it is then.
 */
static int procfs_open_sysctl(int entry, int flags)
{
    struct procfs_file *f;
    fd_entry_t *e;
    int acc = flags & 3;
    int fd;

    if (acc == 3 ||
        !((entry == SYNTH_SYS_HOSTNAME || entry == SYNTH_SYS_DOMAINNAME) ? uts_privileged()
          : entry == SYNTH_SYS_MAX_MNT_NAMESPACES
              ? ns_privileged(current_proc, proc_userns(current_proc))
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

#if defined(__x86_64__) && !defined(RACH2_HOSTTEST)
/**
 * Append the interrupt counts of the VM `vm` for its descriptor's fdinfo:
 * the controller in the kernel, then a line for each I/O APIC pin and each
 * vCPU's vector with a count, the counts in the order of enum
 * kvm_pin_step and enum kvm_vec_step. Under the big lock.
 */
static void procfs_kvm_irqs(struct procfs_text *t, void *vm)
{
    static const char *const pin_names[KVM_PIN_STEPS] = {" raised ", " coalesced ", " held ",
                                                         " delivered ", " missed "};
    static const char *const vec_names[KVM_VEC_STEPS] = {" accepted ", " dropped ", " acked ",
                                                         " injected ", " saved ",   " eoi "};
    struct kvm_irq_row row;
    uint32_t pos = 0, i;

    ap_str(t, "kvm_irqchip:\t");
    ap_str(t, kvm_vm_irqchip_name(vm));
    ap_ch(t, '\n');
    while (kvm_vm_irq_row(vm, &pos, &row)) {
        if (row.pin) {
            ap_str(t, "kvm_ioapic_pin:\t");
            ap_u32(t, row.index);
        } else {
            ap_str(t, "kvm_vcpu_vector:\t");
            ap_u32(t, row.vcpu);
            ap_ch(t, ' ');
            ap_u32(t, row.index);
        }
        for (i = 0; i < (row.pin ? (uint32_t)KVM_PIN_STEPS : (uint32_t)KVM_VEC_STEPS); i++) {
            ap_str(t, row.pin ? pin_names[i] : vec_names[i]);
            ap_u32(t, row.counts[i]);
        }
        ap_ch(t, '\n');
    }
}
#endif

/**
 * The text of /proc/PID/fdinfo/N, descriptor `fd` of process `pid`: its
 * offset, its flags in octal, the access mode with O_APPEND, O_NONBLOCK,
 * O_PATH and O_CLOEXEC, the id of its mount, 0 for an object on none, and
 * its inode number, the four lines Linux gives first. Its length, or
 * -ENOENT for a descriptor that is not open. A dma-buf also gives its
 * supplied name, copied before appending text or the stat can sleep, and a
 * KVM VM its interrupt counts, from a reference taken then.
 */
static int procfs_gen_fdinfo(struct procfs_text *t, int pid, int fd)
{
    process_t *p = get_process((uint32_t)pid);
    const fd_entry_t *e;
    char pos[24], name[DMA_BUF_NAME_LEN];
    uint32_t rw, flags, mnt_id;
    uint64_t ino;
    int named = 0;
#if defined(__x86_64__) && !defined(RACH2_HOSTTEST)
    void *vm;
#endif

    if (!p || p->state == PROC_UNUSED || fd < 0 || fd >= fd_table_cap(p->fds)) {
        return -ENOENT;
    }
    e = &p->fds->entries[fd];
    if (e->type == FD_TYPE_NONE || (e->flags & FD_FLAG_CLOSING)) {
        return -ENOENT;
    }
    if (is_dma_buf_file(e)) {
        const struct dma_buf *b = e->priv;

        named = b->named;
        if (named) {
            k_memcpy(name, b->name, sizeof(name));
        }
    }
#if defined(__x86_64__) && !defined(RACH2_HOSTTEST)
    vm = kvm_vm_irq_hold(e);
#endif
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
    /* The stat may sleep; `e` is not read past this point. */
    if (fd_stat_ids(pid, fd, &mnt_id, &ino) == 0) {
        ap_str(t, "mnt_id:\t");
        ap_u32(t, mnt_id);
        diskinfo_u64_dec(pos, (int)sizeof(pos), ino);
        ap_str(t, "\nino:\t");
        ap_str(t, pos);
        ap_ch(t, '\n');
    }
    if (named) {
        ap_str(t, "name:\t");
        ap_str(t, name);
        ap_ch(t, '\n');
    }
#if defined(__x86_64__) && !defined(RACH2_HOSTTEST)
    if (vm) {
        procfs_kvm_irqs(t, vm);
        kvm_vm_irq_done(vm);
    }
#endif
    return t->nomem ? -1 : t->len;
}

/**
 * Write into `t` the text of the file of a process that `vp` names, the
 * process or thread `pid` as `procfs_resolve_id` gave it: its `cmdline` for
 * a kind with no generator of its own. Returns the text's length, or a
 * negative error from the generator.
 */
static int procfs_gen_pid_file(struct procfs_text *t, int pid, const struct vpath *vp)
{
    switch (vp->procfile) {
    case SYNTH_PROC_STAT:
        return procfs_gen_stat(t, pid, vp->tid == 0);
    case SYNTH_PROC_MAPS:
        return procfs_gen_maps(t, pid);
    case SYNTH_PROC_SMAPS:
        return procfs_gen_smaps(t, pid);
    case SYNTH_PROC_NUMA_MAPS:
        return procfs_gen_numa_maps(t, pid);
    case SYNTH_PROC_STATUS:
        return procfs_gen_status(t, pid);
    case SYNTH_PROC_STATM:
        return procfs_gen_statm(t, pid);
    case SYNTH_PROC_AUXV:
        return procfs_gen_auxv(t, pid);
    case SYNTH_PROC_ENVIRON:
        return procfs_gen_environ(t, pid);
    case SYNTH_PROC_PIDMOUNTS:
        return procfs_gen_mounts(t);
    case SYNTH_PROC_MOUNTINFO:
        return procfs_gen_mountinfo(t);
    case SYNTH_PROC_COMM:
        return procfs_gen_comm(t, pid);
    case SYNTH_PROC_LIMITS:
        return procfs_gen_limits(t, pid);
    case SYNTH_PROC_CHILDREN:
        return procfs_gen_children(t, pid);
    case SYNTH_PROC_SYSCALL:
        return procfs_gen_syscall(t, pid);
    case SYNTH_PROC_WCHAN:
        return procfs_gen_wchan(t, pid);
    case SYNTH_PROC_SCHEDSTAT:
        return procfs_gen_schedstat(t, pid);
    default:
        return procfs_is_idmap(vp->procfile) ? procfs_gen_idmap(t, pid, vp->procfile)
                                             : procfs_gen_cmdline(t, pid);
    }
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
        VNODE_PROC_SYSVIPC,   VNODE_PROC_ACPI_CALL,  VNODE_PROC_FS_LOCKWAIT};
    uint32_t i;

    for (i = 0; i < sizeof(global) / sizeof(global[0]); i++) {
        if (kind == global[i]) {
            return 0;
        }
    }
    return 1;
}

/**
 * Write into `t` the text of the /proc or /sys file `vp` names, the process
 * or thread `pid` as `procfs_resolve_id` gave it for a file of a process.
 * `*key` gets what the descriptor's `origin_pid` keeps when it is not `pid`:
 * the entry of a /proc/sys file, or the key of a /sys file. Returns the
 * text's length, -EOVERFLOW for a /proc/disk past its bound, or a negative
 * error from the generator; the caller frees the text either way.
 */
static int procfs_gen_text(struct procfs_text *t, const struct vpath *vp, int pid, int *key)
{
    int len;

    if (vp->kind == VNODE_PROC_CMDLINE) {
        /* The boot line and a newline, as Linux's /proc/cmdline has it. */
        const char *line = kcmdline_get();
        ap_str(t, line);
        ap_ch(t, '\n');
        len = t->len;
    } else if (vp->kind == VNODE_PROC_DISK) {
        len = 0;
        if (procfs_text_room(t, PROCFS_MAX_DISK) == 0) {
            len = diskinfo_format(diskinfo_get(), t->buf, PROCFS_MAX_DISK);
            len += diskinfo_format_disks(t->buf + len, PROCFS_MAX_DISK - len);
            if (len >= PROCFS_MAX_DISK - 1) {
                klog(KLOG_ERR, "procfs", "/proc/disk: text exceeds its %d-byte bound, refusing",
                     PROCFS_MAX_DISK);
                return -EOVERFLOW;
            }
        }
    } else if (vp->kind == VNODE_PROC_FDINFO) {
        len = procfs_gen_fdinfo(t, pid, vp->procfd);
    } else if (vp->kind == VNODE_PROC_NET_DEV) {
        len = procfs_gen_netdev(t);
    } else if (vp->kind == VNODE_PROC_UPTIME) {
        len = procfs_gen_uptime(t);
    } else if (vp->kind == VNODE_PROC_STAT_SYS) {
        len = procfs_gen_statsys(t);
    } else if (vp->kind == VNODE_PROC_MEMINFO) {
        len = procfs_gen_meminfo(t);
    } else if (vp->kind == VNODE_PROC_LOADAVG) {
        len = procfs_gen_loadavg(t);
    } else if (vp->kind == VNODE_PROC_DISKSTATS) {
        len = procfs_gen_diskstats(t);
    } else if (vp->kind == VNODE_PROC_PARTITIONS) {
        len = procfs_gen_partitions(t);
    } else if (vp->kind == VNODE_PROC_VMSTAT) {
        len = procfs_gen_vmstat(t);
    } else if (vp->kind == VNODE_PROC_SLABINFO) {
        len = procfs_gen_slabinfo(t);
    } else if (vp->kind == VNODE_PROC_LOCKS) {
        len = procfs_gen_locks(t);
    } else if (vp->kind == VNODE_PROC_FS_LOCKWAIT) {
        len = procfs_gen_lockwait(t);
    } else if (vp->kind == VNODE_PROC_SWAPS) {
        /* Linux's header, and no area under it, there being no swap. */
        ap_str(t, "Filename\t\t\t\tType\t\tSize\t\tUsed\t\tPriority\n");
        len = t->len;
    } else if (vp->kind == VNODE_PROC_VERSION) {
        /* Linux's "Linux version", then uname's release and version. */
        ap_str(t, UTS_SYSNAME " version " UTS_RELEASE " " UTS_VERSION "\n");
        len = t->len;
    } else if (vp->kind == VNODE_PROC_CPUINFO) {
        len = procfs_gen_cpuinfo(t);
    } else if (vp->kind == VNODE_PROC_SYS_FILE) {
        len = procfs_sysctl_text(t, vp->sysent);
        len = len < 0 ? len : t->len;
        *key = vp->sysent;
    } else if (vp->kind == VNODE_PROC_MOUNTS) {
        len = procfs_gen_mounts(t);
    } else if (vp->kind == VNODE_PROC_AUDIO) {
        len = procfs_gen_audio(t);
    } else if (vp->kind == VNODE_PROC_USB) {
        len = procfs_gen_usb(t);
    } else if (vp->kind == VNODE_PROC_LOOP) {
        len = procfs_gen_loop(t);
    } else if (vp->kind == VNODE_PROC_INTERRUPTS) {
        len = procfs_gen_interrupts(t);
    } else if (vp->kind == VNODE_PROC_CONSOLE) {
        len = procfs_gen_console(t);
    } else if (vp->kind == VNODE_PROC_FILESYSTEMS) {
        len = procfs_gen_filesystems(t);
    } else if (vp->kind == VNODE_PROC_NET_ROUTE) {
        len = procfs_gen_netroute(t);
    } else if (vp->kind == VNODE_PROC_NET_ARP) {
        len = procfs_gen_netarp(t);
    } else if (vp->kind == VNODE_PROC_NET_TCP) {
        len = procfs_gen_nettcp(t, AF_INET);
    } else if (vp->kind == VNODE_PROC_NET_FILE) {
        len = procfs_gen_net(t, vp->procfile);
    } else if (vp->kind == VNODE_PROC_SYSVIPC) {
        len = procfs_gen_sysvipc(t, vp->procfile);
    } else if (vp->kind == VNODE_SYS_FILE) {
        len = procfs_gen_sysfile(t, vp, key);
    } else {
        len = procfs_gen_pid_file(t, pid, vp);
    }
    return len;
}

/**
 * Render the file `vp` names into `t`, as `procfs_gen_text` does, which this
 * starts: its length, with the text in `t->buf` for the caller to free, or
 * a negative error with nothing to free, -ENOMEM when the heap ran out.
 */
static int procfs_render(struct procfs_text *t, const struct vpath *vp, int pid, int *key)
{
    int len;

    procfs_text_init(t);
    len = procfs_gen_text(t, vp, pid, key);
    /* -1 from a generator that ran out of heap is not -EPERM. */
    if (len < 0 || t->nomem) {
        kfree(t->buf);
        t->buf = NULL;
    }
    if (t->nomem) {
        klog(KLOG_ERR, "procfs", "%s: no memory for the text", procfs_kind_name(vp));
        return -ENOMEM;
    }
    return len;
}

/**
 * Open the file `vp` names, of process `pid` when it is one of a process,
 * through the open of its own when it has one: oom_score_adj, a write of an
 * ID map or comm, mem, a write of a /proc/sys file, the ACPI call file, a
 * PCI function's config and BAR files, and a write of a /sys attribute.
 * Returns 1 with the open's answer in `*fd`, or 0 for a file with none.
 */
static int procfs_open_own(const struct vpath *vp, int pid, int flags, int *fd)
{
    int acc = flags & 3;

    if (vp->kind == VNODE_PROC_FILE && vp->procfile == SYNTH_PROC_OOM_ADJ) {
        *fd = procfs_open_oom(vp, pid, flags);
    } else if (vp->kind == VNODE_PROC_FILE &&
               (procfs_is_idmap(vp->procfile) || vp->procfile == SYNTH_PROC_COMM) && acc != 0) {
        *fd = procfs_open_idmap(vp, vp->procfile, pid, flags);
    } else if (vp->kind == VNODE_PROC_FILE && vp->procfile == SYNTH_PROC_MEM) {
        *fd = procfs_open_mem(vp, pid, flags);
    } else if (vp->kind == VNODE_PROC_SYS_FILE && synth_sys_writable(vp->sysent) && acc != 0) {
        *fd = procfs_open_sysctl(vp->sysent, flags);
    } else if (vp->kind == VNODE_PROC_ACPI_CALL) {
        *fd = procfs_open_acpicall(flags);
    } else if (vp->kind == VNODE_SYS_FILE && vp->sysent == SYSFS_PCI_DEVICE &&
               (vp->procfile == SYSFS_PCI_CONFIG || vp->procfile >= SYSFS_PCI_RESOURCE0)) {
        *fd = vp->procfile == SYSFS_PCI_CONFIG ? procfs_open_pciconfig(vp, flags)
                                               : procfs_open_pcibar(vp, flags);
    } else if (vp->kind == VNODE_SYS_FILE &&
               sysfs_file_writable(vp->sysent, vp->syskey, vp->procfile) && acc != 0) {
        *fd = procfs_open_sysattr(vp->sysent, vp->syskey, vp->procfile, flags);
    } else {
        return 0;
    }
    return 1;
}

/**
 * The checks of the flags are in the order that gives the errno of a
 * regular file that exists: O_CREAT with O_EXCL is -EEXIST first, then
 * O_DIRECTORY -ENOTDIR, then -EACCES for a file of a process whose mode
 * the caller lacks (`procfs_owner_may_open`). The files a write reaches
 * then go to opens of their own (`procfs_open_own`), and any other write or
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
    const struct procfs_global *g;
    struct procfs_text text;
    fd_entry_t *e;
    int fd, len, key;

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
    if (vp->kind == VNODE_PROC_FILE && !procfs_owner_may_open(pid, vp->procfile, flags)) {
        return -EACCES;
    }
    if (procfs_open_own(vp, pid, flags, &fd)) {
        return fd;
    }
    if ((flags & 3) != 0 || (flags & 0100 /* O_CREAT */)) {
        return -EACCES;
    }
    if (vp->kind == VNODE_SYS_FILE && sysfs_attr_private(vp->sysent, vp->procfile) &&
        !proc_privileged(current_proc)) {
        return -EACCES;
    }

    f = kzalloc(sizeof(*f));
    if (!f) {
        return -ENOMEM;
    }
    key = pid;
    f->stale = vp->kind == VNODE_PROC_FILE && vp->procfile == SYNTH_PROC_SYSCALL;
    len = procfs_render(&text, vp, pid, &key);
    if (len == -EPERM && f->stale) {
        /* The ptrace check is the read's, as Linux's lock_trace. */
        text.buf = NULL;
        len = 0;
    }
    if (len < 0) {
        kfree(f);
        return len;
    }
    f->data = text.buf;
    f->len = len;
    f->fresh = 1;
    f->vp = *vp;
    f->gen_pid = pid;
    f->origin_pid = key;
    f->origin_task = vp->tid != 0 ? procfs_resolve_pid(vp->pid) : 0;
    g = procfs_global_find(vp->kind, 0);
    if (g) {
        f->origin_file = g->origin;
    } else if (vp->kind == VNODE_PROC_SYS_FILE) {
        f->origin_file = SYNTH_PROC_SYSCTL;
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
    procfs_own(f, vp, pid);
    e->ofd->offset = 0;
    e->flags = FD_FLAG_READ;
    if (flags & PROCFS_O_CLOEXEC) {
        e->flags |= FD_FLAG_CLOEXEC;
    }
    return fd;
}
