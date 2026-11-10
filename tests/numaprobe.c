/**
 * numaprobe: the NUMA memory policy calls, set_mempolicy, get_mempolicy,
 * mbind, set_mempolicy_home_node, move_pages and migrate_pages, and
 * /proc/PID/numa_maps, answered as Linux 6.12 answers them on a machine of
 * one memory node whose kernel knows 1024 nodes, as Debian 13's does. Each
 * row's expected answer is Linux's, from mm/mempolicy.c, mm/migrate.c and
 * fs/proc/task_mmu.c, and the same on a 32-bit and a 64-bit kernel and for
 * a 32-bit program on a 64-bit one, with the one exception `move-many`
 * names.
 *
 * The rows, in order, each an assertion of etude's report (probe_report.h)
 * for the case ETUDE_CASE names:
 *
 * `thread-default`: the thread starts with MPOL_DEFAULT and an empty mask,
 * written as 8 bytes for a mask of 64 nodes.
 * `thread-modes`: each of the six other modes set with node 0 reads back
 * with node 0; MPOL_BIND of nodes 0 and 1 reads node 0; MPOL_LOCAL, and
 * MPOL_PREFERRED with an empty mask, read MPOL_LOCAL and no node; and
 * MPOL_DEFAULT takes the policy away.
 * `thread-flags`: a policy set with MPOL_F_STATIC_NODES,
 * MPOL_F_RELATIVE_NODES or MPOL_F_NUMA_BALANCING reads back with its flag
 * and the mask its caller gave, which under MPOL_F_RELATIVE_NODES need not
 * hold node 0.
 * `thread-errors`: an unknown mode, the two node flags together,
 * MPOL_F_NUMA_BALANCING with MPOL_INTERLEAVE, a mask for MPOL_DEFAULT or
 * MPOL_LOCAL, a node flag with MPOL_LOCAL or an empty MPOL_PREFERRED, and
 * an empty mask or one without node 0 for MPOL_BIND are EINVAL; an
 * unreadable mask is EFAULT; and the policy set before them stays.
 * `mask-maxnode`: a maxnode of 0 with a mask is EINVAL and without one the
 * empty mask; 1 is the empty mask; the bits past maxnode - 1 in the last
 * long are dropped; a mask of 32768 bits is read and one of 32769 EINVAL;
 * node 1023 is dropped, and a bit past it, in the long that holds the
 * mask's last bit, is EINVAL, but not one in a long past that.
 * `get-node`: MPOL_F_NODE alone gives node 0 to an interleaving thread and
 * EINVAL to a binding one; MPOL_F_MEMS_ALLOWED gives node 0 whatever the
 * policy.
 * `maps-names`: numa_maps shows a region without a policy by the thread's,
 * in Linux's words: default, prefer:0, bind:0, interleave:0, local,
 * prefer (many):0, weighted interleave:0, and bind=static:0,
 * bind=relative:0, bind=balancing:0 and bind=static|balancing:0.
 * `mbind-split`: a policy bound to the middle of a region reads back there
 * with MPOL_F_ADDR and not beside it, where the answer is MPOL_DEFAULT
 * though the thread interleaves; numa_maps has a line for each of the three
 * parts; and a length of one byte binds a page.
 * `mbind-pages`: numa_maps counts the two written pages of the bound part
 * as anon=2 dirty=2 N0=2 with kernelpagesize_kB=4, not the page that was
 * only read, and gives a part without pages no counts.
 * `mbind-merge`: MPOL_DEFAULT over the bound part joins the three parts
 * into one line again, and two neighbours bound alike are one region.
 * `mbind-mremap`: a bound region that mremap moves and grows has its policy
 * over the whole new range, with its pages.
 * `mbind-fork`: a forked child has the region's policy and the thread's,
 * and what it changes of them leaves the parent's as they were.
 * `exec-keeps`: a program started by execve has the thread's policy, flag
 * and mask included, and none on its stack.
 * `mbind-shared`: a policy bound through one mapping of a memfd reads back
 * through another, lasts when both are unmapped, and shows in numa_maps on
 * a new mapping's line with file=/memfd:numaprobe\040(deleted) and, once a
 * page is written, dirty=1 N0=1.
 * `mbind-strict`: MPOL_MF_STRICT passes a mask with node 0, and without it
 * a range whose pages were only read or never touched; it is EIO, and binds
 * nothing, over a written page under MPOL_LOCAL or a relative mask without
 * node 0; with a move flag the page is moved and the policy set; and
 * MPOL_DEFAULT ignores it.
 * `mbind-errors`: an address off a page boundary, an unknown flag or mode
 * and an empty mask for MPOL_BIND are EINVAL; an empty range is 0 before
 * the policy is looked at; an unreadable mask is EFAULT; a range with a
 * hole in it is EFAULT and binds nothing, but 0 for MPOL_DEFAULT; and a
 * range that is all hole is EFAULT for MPOL_DEFAULT too.
 * `home-node`: set_mempolicy_home_node is ENOENT over a region without a
 * policy; gives part of an MPOL_BIND region its home, which splits it and
 * leaves get_mempolicy's answer; is EINVAL for node 1, a flag and an
 * address off a page boundary, and 0 for an empty range; and is EOPNOTSUPP
 * at an MPOL_PREFERRED region, with the regions before it changed.
 * `move-status`: move_pages without nodes reports 0 for a written page,
 * -EFAULT for a page only read and an address in no mapping, 0 for a
 * written page of a memfd, and -ENOENT for an untouched page of anonymous
 * memory or a memfd; with node 0 for each it reports the
 * same.
 * `move-many`: of 20 addresses the one in no mapping is -EFAULT and the 19
 * of a written page 0. A 32-bit program on 64-bit Linux 6.12 reads -EFAULT
 * for entries 16 to 19, since do_pages_stat steps over its 32-bit pointers
 * by 64-bit ones (dev-log/80-games/upstream.md); Rach2 reads each entry.
 * `move-errors`: MPOL_MF_STRICT is EINVAL; node 1 is ENODEV, after the
 * status of the entry before it is written; an unreadable array of
 * addresses and an unwritable one of statuses are EFAULT; no pages is 0.
 * `move-other`: root reads the status of a child's page; an ordinary user
 * is refused the root child's pages by both calls, MPOL_MF_MOVE_ALL by
 * move_pages and mbind, and new nodes other than node 0 by migrate_pages,
 * each with EPERM, and moves its own; a reaped child's ID is ESRCH.
 * `migrate`: migrate_pages from node 0, or nodes 0 and 1, to node 0 moves
 * nothing and returns 0; new nodes without node 0 are EINVAL, an unreadable
 * mask EFAULT, and a mask of 32769 bits EINVAL.
 *
 * Prints "NUMAPROBE OK" and exits 0 when every row holds, and otherwise
 * "NUMAPROBE FAIL: what" and exits 1 at the first failure. Runs as root,
 * with user ID 1000 for the ordinary user, and starts itself again by
 * /numaprobe, its path on the test image, or by its own absolute path, with
 * the word `policy`, as the program `exec-keeps` starts. Freestanding;
 * direct system calls (probe.h).
 */

#include "probe.h"
#include "probe_args.h"
#include "probe_report.h"

#define EPERM      1
#define ENOENT     2
#define ESRCH      3
#define EIO        5
#define EFAULT     14
#define ENODEV     19
#define EINVAL     22
#define EOPNOTSUPP 95

#define PROT_NONE      0x0
#define PROT_READ      0x1
#define PROT_WRITE     0x2
#define MAP_SHARED     0x01
#define MAP_PRIVATE    0x02
#define MAP_ANONYMOUS  0x20
#define MREMAP_MAYMOVE 1

#define MPOL_DEFAULT             0
#define MPOL_PREFERRED           1
#define MPOL_BIND                2
#define MPOL_INTERLEAVE          3
#define MPOL_LOCAL               4
#define MPOL_PREFERRED_MANY      5
#define MPOL_WEIGHTED_INTERLEAVE 6

#define MPOL_F_STATIC_NODES   0x8000
#define MPOL_F_RELATIVE_NODES 0x4000
#define MPOL_F_NUMA_BALANCING 0x2000

#define MPOL_F_NODE         1
#define MPOL_F_ADDR         2
#define MPOL_F_MEMS_ALLOWED 4

#define MPOL_MF_STRICT   1
#define MPOL_MF_MOVE     2
#define MPOL_MF_MOVE_ALL 4

/* The ordinary user the rows that need one become. */
#define USER_ID 1000

#define PAGE 4096L

/* The maxnode of a mask of 64 nodes, which the rows pass with their masks
 * of 16 bytes. */
#define MAXNODE 65

/* The exit statuses of the program `exec-keeps` starts. */
#define EXEC_KEPT 42
#define EXEC_LOST 41

#if defined __x86_64__
#define SYS_MBIND                   237
#define SYS_SET_MEMPOLICY           238
#define SYS_GET_MEMPOLICY           239
#define SYS_MIGRATE_PAGES           256
#define SYS_MOVE_PAGES              279
#define SYS_MEMFD_CREATE            319
#define SYS_SET_MEMPOLICY_HOME_NODE 450
#else
#define SYS_MBIND                   274
#define SYS_GET_MEMPOLICY           275
#define SYS_SET_MEMPOLICY           276
#define SYS_MIGRATE_PAGES           294
#define SYS_MOVE_PAGES              317
#define SYS_MEMFD_CREATE            356
#define SYS_SET_MEMPOLICY_HOME_NODE 450
#endif

/* Node masks of 64 nodes: none, node 0, node 1, nodes 0 and 1, and every
 * node. */
static const unsigned char m_none[16];
static const unsigned char m_0[16] = {1};
static const unsigned char m_1[16] = {2};
static const unsigned char m_01[16] = {3};
static const unsigned char m_all[16] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                        0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};

/* A mask of a page of bits and a long more, for `mask-maxnode`. */
static unsigned char m_big[PAGE + 16];

/* numa_maps as `maps_read` last read it. */
static char maps_text[65536];

/* An address in no mapping: a page unmapped between two pages without
 * access, where no later mapping of the probe's, each longer, fits. */
static long hole;

/** End the process with `status`; it does not return. */
static void sys_exit(int status)
{
    syscall1(SYS_EXIT_GROUP, (long)status);
    __builtin_unreachable();
}

/** Say `what` failed, fail the current row with it, and exit 1. */
static void fail(const char *what)
{
    probe_line("NUMAPROBE FAIL: ", what, 0);
    report_stop(what);
    sys_exit(1);
}

/** Fail with `what` unless `ok`. */
static void check(int ok, const char *what)
{
    if (!ok) {
        fail(what);
    }
}

/** Set the `n` bytes at `p` to `v`. */
static void fill(void *p, int v, unsigned long n)
{
    unsigned char *q = p;

    while (n-- > 0) {
        *q++ = (unsigned char)v;
    }
}

/** Store into the page at `addr`. */
static void store(long addr)
{
    *(volatile char *)addr = 1;
}

/** Read the page at `addr`. */
static void load(long addr)
{
    (void)*(volatile char *)addr;
}

/** Fork; the child's 0, or the parent's child pid. */
static long fork_child(void)
{
    long pid = syscall0(SYS_FORK);

    check(pid >= 0, "fork");
    return pid;
}

/** Reap `pid` and return its exit status, or -1 if it did not exit. */
static int reap(long pid)
{
    int status = -1;

    check(syscall4(SYS_WAITPID, pid, (long)&status, 0, 0) == pid, "wait");
    return (status & 0x7F) == 0 ? (status >> 8) & 0xFF : -1;
}

/** set_mempolicy of `mode` with the mask `mask` of `maxnode`. */
static long set_policy(int mode, const void *mask, unsigned long maxnode)
{
    return syscall3(SYS_SET_MEMPOLICY, mode, (long)mask, (long)maxnode);
}

/** mbind of `len` bytes at `addr` to `mode` with the mask `mask` of 64
 * nodes, or with no mask when `mask` is null, and `flags`. */
static long bind(long addr, long len, int mode, const void *mask, long flags)
{
    return syscall6(SYS_MBIND, addr, len, mode, (long)mask, mask ? MAXNODE : 0, flags);
}

/** set_mempolicy_home_node of `len` bytes at `addr`. */
static long home(long addr, long len, long node, long flags)
{
    return syscall4(SYS_SET_MEMPOLICY_HOME_NODE, addr, len, node, flags);
}

/** move_pages of `n` addresses of process `pid`. */
static long move(long pid, long n, const void *pages, const int *nodes, int *status, long flags)
{
    return syscall6(SYS_MOVE_PAGES, pid, n, (long)pages, (long)nodes, (long)status, flags);
}

/** migrate_pages of process `pid` with masks of `maxnode`. */
static long migrate(long pid, long maxnode, const void *from, const void *to)
{
    return syscall4(SYS_MIGRATE_PAGES, pid, maxnode, (long)from, (long)to);
}

/**
 * get_mempolicy of `addr` with `flags` and a mask of 64 nodes: the mode
 * goes to `*mode`, and the mask's low byte is returned. The call must write
 * the mask's first 8 bytes, the 7 past the first as zeros, and no more.
 */
static int get_policy(long addr, long flags, int *mode)
{
    unsigned char mask[16];
    int i;

    fill(mask, 0xFF, sizeof(mask));
    *mode = -1;
    check(syscall5(SYS_GET_MEMPOLICY, (long)mode, (long)mask, MAXNODE, addr, flags) == 0,
          "get_mempolicy");
    for (i = 1; i < 16; i++) {
        check(mask[i] == (i < 8 ? 0 : 0xFF), "get_mempolicy does not write a mask of 8 bytes");
    }
    return mask[0];
}

/** Whether the thread's policy reads `mode` with the mask byte `nodes`. */
static int thread_is(int mode, int nodes)
{
    int m;
    int n = get_policy(0, 0, &m);

    return m == mode && n == nodes;
}

/** Whether the policy of `addr` reads `mode` with the mask byte `nodes`. */
static int addr_is(long addr, int mode, int nodes)
{
    int m;
    int n = get_policy(addr, MPOL_F_ADDR, &m);

    return m == mode && n == nodes;
}

/** Read /proc/self/numa_maps whole into `maps_text`. */
static void maps_read(void)
{
    long fd = syscall2(SYS_OPEN, (long)"/proc/self/numa_maps", 0);
    long n, len = 0;

    check(!sys_failed(fd), "open /proc/self/numa_maps");
    while ((n = syscall3(SYS_READ, fd, (long)(maps_text + len),
                         (long)sizeof(maps_text) - 1 - len)) > 0) {
        len += n;
    }
    check(n == 0 && len > 0, "read /proc/self/numa_maps");
    maps_text[len] = '\0';
    syscall1(SYS_CLOSE, fd);
}

/** The line of `maps_text` for the region that starts at `addr`, from its
 * policy on, or NULL when no region starts there. The address is in hex
 * of 8 digits at least, as Linux writes it. */
static const char *maps_line(long addr)
{
    char want[20];
    const char *p = maps_text;
    unsigned long v = (unsigned long)addr;
    unsigned long rest = v >> 16 >> 16;
    int digits = 8, n, i;

    for (; rest != 0; rest >>= 4) {
        digits++;
    }
    for (i = digits - 1; i >= 0; i--) {
        want[i] = "0123456789abcdef"[v & 15];
        v >>= 4;
    }
    n = digits;
    want[n++] = ' ';
    while (*p != '\0') {
        for (i = 0; i < n && p[i] == want[i]; i++) {
        }
        if (i == n) {
            return p + n;
        }
        while (*p != '\0' && *p++ != '\n') {
        }
    }
    return 0;
}

/** Whether `line`, from `maps_line`, begins with the policy `policy`, with
 * a space or the line's end after it. */
static int line_policy(const char *line, const char *policy)
{
    if (!line) {
        return 0;
    }
    while (*policy != '\0' && *line == *policy) {
        line++;
        policy++;
    }
    return *policy == '\0' && (*line == ' ' || *line == '\n');
}

/** Whether `line` has the field `field` after a space, with a space or the
 * line's end after it. */
static int line_has(const char *line, const char *field)
{
    int i;

    for (; line && *line != '\0' && *line != '\n'; line++) {
        if (*line != ' ') {
            continue;
        }
        for (i = 0; field[i] != '\0' && line[1 + i] == field[i]; i++) {
        }
        if (field[i] == '\0' && (line[1 + i] == ' ' || line[1 + i] == '\n')) {
            return 1;
        }
    }
    return 0;
}

/** Whether `line` is the policy `policy` and nothing more, as the line of
 * a region with no page of memory is. */
static int line_bare(const char *line, const char *policy)
{
    int n = 0;

    while (policy[n] != '\0') {
        n++;
    }
    return line_policy(line, policy) && line[n] == '\n';
}

/** Read numa_maps and say whether the region that starts at `addr` shows
 * the policy `policy`. */
static int region_shows(long addr, const char *policy)
{
    maps_read();
    return line_policy(maps_line(addr), policy);
}

/**
 * Map `pages` pages of private anonymous memory between two pages without
 * access, so that a region begins at the first of them and ends at the last
 * on any kernel, whatever is mapped beside them. The first page's address.
 */
static long area(long pages)
{
    long base = probe_mmap(0, (pages + 2) * PAGE, PROT_READ | PROT_WRITE,
                           MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    check(!sys_failed(base), "mmap");
    check(syscall3(SYS_MPROTECT, base, PAGE, PROT_NONE) == 0 &&
              syscall3(SYS_MPROTECT, base + (pages + 1) * PAGE, PAGE, PROT_NONE) == 0,
          "mprotect");
    return base + PAGE;
}

/** Make a memfd named numaprobe of `pages` pages. */
static long memfd(long pages)
{
    long fd = syscall2(SYS_MEMFD_CREATE, (long)"numaprobe", 0);

    check(!sys_failed(fd), "memfd_create");
    check(probe_ftruncate64((int)fd, (long long)(pages * PAGE)) == 0, "ftruncate");
    return fd;
}

/** Map `pages` pages of the memfd `fd` from its page `at`, shared. */
static long map_fd(long fd, long at, long pages)
{
    long a = probe_mmap(0, pages * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, at * PAGE);

    check(!sys_failed(a), "mmap of the memfd");
    return a;
}

/* A policy to set, and how it reads back or shows. */
struct policy_row {
    int mode;
    const unsigned char *mask;
    int reads;         /* the mode get_mempolicy gives */
    int nodes;         /* the mask's low byte */
    const char *shows; /* numa_maps' words, for `maps-names` */
};

/** thread-default, thread-modes and thread-flags. */
static void thread_rows(void)
{
    static const struct policy_row modes[] = {
        {MPOL_PREFERRED, m_0, MPOL_PREFERRED, 1, 0},
        {MPOL_BIND, m_0, MPOL_BIND, 1, 0},
        {MPOL_INTERLEAVE, m_0, MPOL_INTERLEAVE, 1, 0},
        {MPOL_PREFERRED_MANY, m_0, MPOL_PREFERRED_MANY, 1, 0},
        {MPOL_WEIGHTED_INTERLEAVE, m_0, MPOL_WEIGHTED_INTERLEAVE, 1, 0},
        {MPOL_BIND, m_01, MPOL_BIND, 1, 0},
        {MPOL_LOCAL, m_none, MPOL_LOCAL, 0, 0},
        {MPOL_PREFERRED, m_none, MPOL_LOCAL, 0, 0},
        {MPOL_DEFAULT, m_none, MPOL_DEFAULT, 0, 0}};
    static const struct policy_row flags[] = {
        {MPOL_BIND | MPOL_F_STATIC_NODES, m_01, MPOL_BIND | MPOL_F_STATIC_NODES, 3, 0},
        {MPOL_BIND | MPOL_F_RELATIVE_NODES, m_1, MPOL_BIND | MPOL_F_RELATIVE_NODES, 2, 0},
        {MPOL_BIND | MPOL_F_NUMA_BALANCING, m_0, MPOL_BIND | MPOL_F_NUMA_BALANCING, 1, 0},
        {MPOL_PREFERRED_MANY | MPOL_F_NUMA_BALANCING, m_01,
         MPOL_PREFERRED_MANY | MPOL_F_NUMA_BALANCING, 3, 0},
        {MPOL_INTERLEAVE | MPOL_F_STATIC_NODES, m_0, MPOL_INTERLEAVE | MPOL_F_STATIC_NODES, 1, 0},
        {MPOL_PREFERRED | MPOL_F_RELATIVE_NODES, m_1, MPOL_PREFERRED | MPOL_F_RELATIVE_NODES, 2,
         0}};
    unsigned i;

    check(thread_is(MPOL_DEFAULT, 0), "the thread does not start with MPOL_DEFAULT");
    report_held();
    for (i = 0; i < sizeof(modes) / sizeof(modes[0]); i++) {
        check(set_policy(modes[i].mode, modes[i].mask, MAXNODE) == 0, "set_mempolicy of a mode");
        check(thread_is(modes[i].reads, modes[i].nodes), "a mode does not read back");
    }
    report_held();
    for (i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        check(set_policy(flags[i].mode, flags[i].mask, MAXNODE) == 0, "set_mempolicy with a flag");
        check(thread_is(flags[i].reads, flags[i].nodes),
              "a policy with a flag does not read back with it and its caller's mask");
    }
    report_held();
}

/** thread-errors. */
static void thread_errors(void)
{
    static const struct {
        int mode;
        const unsigned char *mask;
    } bad[] = {{7, m_0},
               {MPOL_BIND | MPOL_F_STATIC_NODES | MPOL_F_RELATIVE_NODES, m_0},
               {MPOL_INTERLEAVE | MPOL_F_NUMA_BALANCING, m_0},
               {MPOL_DEFAULT, m_0},
               {MPOL_LOCAL, m_0},
               {MPOL_LOCAL | MPOL_F_STATIC_NODES, m_none},
               {MPOL_PREFERRED | MPOL_F_RELATIVE_NODES, m_none},
               {MPOL_BIND, m_none},
               {MPOL_BIND, m_1},
               {MPOL_BIND | MPOL_F_STATIC_NODES, m_1},
               {MPOL_BIND, 0}};
    unsigned i;

    check(set_policy(MPOL_INTERLEAVE, m_0, MAXNODE) == 0, "set_mempolicy");
    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        check(set_policy(bad[i].mode, bad[i].mask, MAXNODE) == -EINVAL,
              "a mode, flag or mask Linux refuses is not EINVAL");
    }
    check(set_policy(MPOL_BIND, (const void *)hole, MAXNODE) == -EFAULT,
          "an unreadable mask is not EFAULT");
    check(thread_is(MPOL_INTERLEAVE, 1), "a refused policy replaced the thread's");
    report_held();
}

/** Set or clear bit `n` of `m_big`. */
static void big_bit(unsigned n, int on)
{
    if (on) {
        m_big[n / 8] |= (unsigned char)(1u << (n % 8));
    } else {
        m_big[n / 8] &= (unsigned char)~(1u << (n % 8));
    }
}

/** mask-maxnode. */
static void mask_maxnode(void)
{
    check(set_policy(MPOL_DEFAULT, m_0, 0) == -EINVAL, "maxnode 0 with a mask is not EINVAL");
    check(set_policy(MPOL_DEFAULT, 0, 0) == 0, "maxnode 0 without a mask is not the empty mask");
    check(set_policy(MPOL_DEFAULT, m_0, 1) == 0 && set_policy(MPOL_BIND, m_0, 1) == -EINVAL,
          "maxnode 1 is not the empty mask");
    check(set_policy(MPOL_BIND | MPOL_F_STATIC_NODES, m_all, 2) == 0 &&
              thread_is(MPOL_BIND | MPOL_F_STATIC_NODES, 1),
          "maxnode 2 is not a mask of one bit");
    check(set_policy(MPOL_BIND | MPOL_F_STATIC_NODES, m_all, 3) == 0 &&
              thread_is(MPOL_BIND | MPOL_F_STATIC_NODES, 3),
          "maxnode 3 is not a mask of two bits");
    big_bit(0, 1);
    check(set_policy(MPOL_BIND, m_big, 8 * PAGE + 1) == 0, "a mask of a page of bits is refused");
    check(set_policy(MPOL_BIND, m_big, 8 * PAGE + 2) == -EINVAL,
          "a mask of more than a page of bits is not EINVAL");
    big_bit(1023, 1);
    check(set_policy(MPOL_BIND, m_big, 1025) == 0 && thread_is(MPOL_BIND, 1),
          "node 1023 is not dropped from a mask");
    big_bit(1040, 1);
    check(set_policy(MPOL_BIND, m_big, 1025) == 0, "a bit past the mask's end is read");
    check(set_policy(MPOL_BIND, m_big, 1026) == -EINVAL,
          "a bit past node 1023 in the mask's last long is not EINVAL");
    big_bit(1040, 0);
    big_bit(3000, 1);
    check(set_policy(MPOL_BIND, m_big, 4097) == -EINVAL, "node 3000 is not EINVAL");
    check(set_policy(MPOL_BIND, m_big, 2049) == 0, "a bit in a long past the mask's end is read");
    report_held();
}

/** get-node and maps-names. */
static void get_rows(void)
{
    static const struct policy_row names[] = {
        {MPOL_DEFAULT, m_none, 0, 0, "default"},
        {MPOL_PREFERRED, m_0, 0, 0, "prefer:0"},
        {MPOL_BIND, m_0, 0, 0, "bind:0"},
        {MPOL_INTERLEAVE, m_0, 0, 0, "interleave:0"},
        {MPOL_LOCAL, m_none, 0, 0, "local"},
        {MPOL_PREFERRED_MANY, m_0, 0, 0, "prefer (many):0"},
        {MPOL_WEIGHTED_INTERLEAVE, m_0, 0, 0, "weighted interleave:0"},
        {MPOL_BIND | MPOL_F_STATIC_NODES, m_01, 0, 0, "bind=static:0"},
        {MPOL_BIND | MPOL_F_RELATIVE_NODES, m_1, 0, 0, "bind=relative:0"},
        {MPOL_BIND | MPOL_F_NUMA_BALANCING, m_0, 0, 0, "bind=balancing:0"},
        {MPOL_BIND | MPOL_F_STATIC_NODES | MPOL_F_NUMA_BALANCING, m_0, 0, 0,
         "bind=static|balancing:0"}};
    long a = area(1);
    int mode = -1;
    unsigned i;

    check(set_policy(MPOL_INTERLEAVE, m_0, MAXNODE) == 0, "set_mempolicy");
    check(syscall5(SYS_GET_MEMPOLICY, (long)&mode, 0, 0, 0, MPOL_F_NODE) == 0 && mode == 0,
          "MPOL_F_NODE does not give an interleaving thread node 0");
    check(set_policy(MPOL_BIND, m_0, MAXNODE) == 0, "set_mempolicy");
    check(syscall5(SYS_GET_MEMPOLICY, (long)&mode, 0, 0, 0, MPOL_F_NODE) == -EINVAL,
          "MPOL_F_NODE alone is not EINVAL for a binding thread");
    check(get_policy(0, MPOL_F_MEMS_ALLOWED, &mode) == 1 && mode == 0,
          "the allowed nodes are not node 0 whatever the policy");
    report_held();
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        check(set_policy(names[i].mode, names[i].mask, MAXNODE) == 0, "set_mempolicy");
        check(region_shows(a, names[i].shows), "numa_maps does not show the thread's policy");
    }
    report_held();
    check(syscall2(SYS_MUNMAP, a - PAGE, 3 * PAGE) == 0, "munmap");
}

/** mbind-split, mbind-pages, mbind-merge and mbind-mremap, under a thread
 * that interleaves. */
static void mbind_rows(void)
{
    long r = area(8);
    const char *line;
    long n;

    check(set_policy(MPOL_INTERLEAVE, m_0, MAXNODE) == 0, "set_mempolicy");
    check(bind(r + 2 * PAGE, 4 * PAGE, MPOL_BIND, m_0, 0) == 0, "mbind");
    check(addr_is(r + 2 * PAGE, MPOL_BIND, 1) && addr_is(r + 5 * PAGE, MPOL_BIND, 1),
          "the bound pages do not read MPOL_BIND");
    check(addr_is(r + PAGE, MPOL_DEFAULT, 0) && addr_is(r + 6 * PAGE, MPOL_DEFAULT, 0),
          "the pages beside the bound ones do not read MPOL_DEFAULT");
    maps_read();
    check(line_policy(maps_line(r), "interleave:0") &&
              line_policy(maps_line(r + 2 * PAGE), "bind:0") &&
              line_policy(maps_line(r + 6 * PAGE), "interleave:0"),
          "numa_maps does not show the three parts");
    check(bind(r, 1, MPOL_PREFERRED, m_0, 0) == 0 && region_shows(r, "prefer:0") &&
              line_policy(maps_line(r + PAGE), "interleave:0"),
          "a length of one byte does not bind one page");
    check(bind(r, 1, MPOL_DEFAULT, 0, 0) == 0, "mbind of MPOL_DEFAULT");
    report_held();

    store(r + 2 * PAGE);
    store(r + 3 * PAGE);
    load(r + 4 * PAGE);
    maps_read();
    line = maps_line(r + 2 * PAGE);
    check(line_has(line, "anon=2") && line_has(line, "dirty=2") && line_has(line, "N0=2") &&
              line_has(line, "kernelpagesize_kB=4") && !line_has(line, "mapped=2"),
          "numa_maps does not count the two written pages alone");
    check(line_bare(maps_line(r + 6 * PAGE), "interleave:0"),
          "numa_maps counts pages in a part that has none");
    report_held();

    check(bind(r + 2 * PAGE, 4 * PAGE, MPOL_DEFAULT, 0, 0) == 0, "mbind of MPOL_DEFAULT");
    maps_read();
    line = maps_line(r);
    check(!maps_line(r + 2 * PAGE) && !maps_line(r + 6 * PAGE) &&
              line_policy(line, "interleave:0") && line_has(line, "N0=2"),
          "MPOL_DEFAULT does not join the parts again");
    check(bind(r, 2 * PAGE, MPOL_BIND, m_0, 0) == 0 &&
              bind(r + 2 * PAGE, 2 * PAGE, MPOL_BIND, m_0, 0) == 0,
          "mbind");
    maps_read();
    check(line_policy(maps_line(r), "bind:0") && !maps_line(r + 2 * PAGE) &&
              line_policy(maps_line(r + 4 * PAGE), "interleave:0"),
          "two neighbours bound alike are not one region");
    report_held();

    n = syscall4(SYS_MREMAP, r, 4 * PAGE, 12 * PAGE, MREMAP_MAYMOVE);
    check(!sys_failed(n) && n != r, "mremap does not move the region");
    check(addr_is(n, MPOL_BIND, 1) && addr_is(n + 11 * PAGE, MPOL_BIND, 1),
          "the moved region does not read MPOL_BIND throughout");
    maps_read();
    line = maps_line(n);
    check(line_policy(line, "bind:0") && line_has(line, "N0=2"),
          "numa_maps does not show the moved region with its policy and pages");
    report_held();
    check(set_policy(MPOL_DEFAULT, 0, 0) == 0, "set_mempolicy");
}

/** The body of `mbind-fork`'s child: 0 when it has the policies of the
 * region `q` and of the thread and can change its own. */
static int fork_child_holds(long q)
{
    return addr_is(q, MPOL_PREFERRED_MANY, 1) && thread_is(MPOL_INTERLEAVE, 1) &&
                   bind(q, PAGE, MPOL_DEFAULT, 0, 0) == 0 && set_policy(MPOL_DEFAULT, 0, 0) == 0 &&
                   addr_is(q, MPOL_DEFAULT, 0) && thread_is(MPOL_DEFAULT, 0)
               ? 0
               : 1;
}

/** The program `exec-keeps` starts: EXEC_KEPT when the thread's policy is
 * MPOL_BIND with MPOL_F_STATIC_NODES over nodes 0 and 1 and the stack has
 * none, and else EXEC_LOST. */
static void exec_child(void)
{
    int local = 0;

    sys_exit(thread_is(MPOL_BIND | MPOL_F_STATIC_NODES, 3) && addr_is((long)&local, MPOL_DEFAULT, 0)
                 ? EXEC_KEPT
                 : EXEC_LOST);
}

/** mbind-fork and exec-keeps. */
static void inherit_rows(void)
{
    long q = area(2);
    char *argv[3];
    long pid;

    check(set_policy(MPOL_INTERLEAVE, m_0, MAXNODE) == 0 &&
              bind(q, 2 * PAGE, MPOL_PREFERRED_MANY, m_0, 0) == 0,
          "set_mempolicy and mbind");
    pid = fork_child();
    if (pid == 0) {
        sys_exit(fork_child_holds(q));
    }
    check(reap(pid) == 0, "a forked child has not its parent's policies, or cannot change its own");
    check(addr_is(q, MPOL_PREFERRED_MANY, 1) && thread_is(MPOL_INTERLEAVE, 1),
          "the child's changes reached the parent");
    report_held();

    check(set_policy(MPOL_BIND | MPOL_F_STATIC_NODES, m_01, MAXNODE) == 0, "set_mempolicy");
    pid = fork_child();
    if (pid == 0) {
        argv[0] = (char *)"numaprobe";
        argv[1] = (char *)"policy";
        argv[2] = 0;
        syscall3(SYS_EXECVE, (long)probe_self("/numaprobe"), (long)argv, 0);
        sys_exit(2);
    }
    check(reap(pid) == EXEC_KEPT, "execve does not keep the thread's policy");
    report_held();
    check(set_policy(MPOL_DEFAULT, 0, 0) == 0, "set_mempolicy");
    check(syscall2(SYS_MUNMAP, q - PAGE, 4 * PAGE) == 0, "munmap");
}

/** mbind-shared. */
static void shared_row(void)
{
    long fd = memfd(4);
    long a = map_fd(fd, 0, 4), b = map_fd(fd, 0, 4), c;
    const char *line;

    check(bind(a + PAGE, 2 * PAGE, MPOL_BIND, m_0, 0) == 0, "mbind of a memfd's pages");
    check(addr_is(b + PAGE, MPOL_BIND, 1) && addr_is(b + 2 * PAGE, MPOL_BIND, 1) &&
              addr_is(b, MPOL_DEFAULT, 0) && addr_is(b + 3 * PAGE, MPOL_DEFAULT, 0),
          "another mapping of the memfd does not read the policy of its pages");
    check(syscall2(SYS_MUNMAP, a, 4 * PAGE) == 0 && syscall2(SYS_MUNMAP, b, 4 * PAGE) == 0,
          "munmap");
    c = map_fd(fd, 1, 2);
    check(addr_is(c, MPOL_BIND, 1) && addr_is(c + PAGE, MPOL_BIND, 1),
          "the policy of a memfd's pages does not last without a mapping");
    maps_read();
    line = maps_line(c);
    check(line_policy(line, "bind:0") && line_has(line, "file=/memfd:numaprobe\\040(deleted)"),
          "numa_maps does not show the memfd's mapping with its policy and file");
    store(c);
    maps_read();
    line = maps_line(c);
    check(line_has(line, "dirty=1") && line_has(line, "N0=1") && !line_has(line, "anon=1"),
          "numa_maps does not count the memfd's written page as a file's");
    report_held();
    check(syscall2(SYS_MUNMAP, c, 2 * PAGE) == 0 && syscall1(SYS_CLOSE, fd) == 0, "munmap");
}

/** mbind-strict. */
static void strict_row(void)
{
    long s = area(4);

    store(s);
    load(s + PAGE);
    check(bind(s, 4 * PAGE, MPOL_BIND, m_0, MPOL_MF_STRICT) == 0,
          "MPOL_MF_STRICT refuses a mask with node 0");
    check(bind(s + PAGE, 3 * PAGE, MPOL_LOCAL, 0, MPOL_MF_STRICT) == 0 &&
              addr_is(s + PAGE, MPOL_LOCAL, 0),
          "MPOL_MF_STRICT counts a page only read or never touched");
    check(bind(s, PAGE, MPOL_LOCAL, 0, MPOL_MF_STRICT) == -EIO && addr_is(s, MPOL_BIND, 1),
          "MPOL_MF_STRICT under MPOL_LOCAL is not EIO over a written page, or binds it");
    check(bind(s, PAGE, MPOL_BIND | MPOL_F_RELATIVE_NODES, m_1, MPOL_MF_STRICT) == -EIO &&
              addr_is(s, MPOL_BIND, 1),
          "MPOL_MF_STRICT with a relative mask without node 0 is not EIO, or binds");
    check(bind(s, PAGE, MPOL_LOCAL, 0, MPOL_MF_STRICT | MPOL_MF_MOVE) == 0 &&
              addr_is(s, MPOL_LOCAL, 0),
          "MPOL_MF_MOVE does not move the page and set the policy");
    check(bind(s, PAGE, MPOL_BIND | MPOL_F_RELATIVE_NODES, m_1,
               MPOL_MF_STRICT | MPOL_MF_MOVE_ALL) == 0 &&
              addr_is(s, MPOL_BIND | MPOL_F_RELATIVE_NODES, 2),
          "MPOL_MF_MOVE_ALL does not move root's page and set the policy");
    check(bind(s, 4 * PAGE, MPOL_DEFAULT, 0, MPOL_MF_STRICT) == 0 && addr_is(s, MPOL_DEFAULT, 0),
          "MPOL_DEFAULT does not ignore MPOL_MF_STRICT");
    report_held();
    check(syscall2(SYS_MUNMAP, s - PAGE, 6 * PAGE) == 0, "munmap");
}

/** mbind-errors. */
static void mbind_errors(void)
{
    long e = area(4);

    check(bind(e + 1, PAGE, MPOL_BIND, m_0, 0) == -EINVAL,
          "an address off a page boundary is not EINVAL");
    check(bind(e, PAGE, MPOL_BIND, m_0, 8) == -EINVAL, "an unknown flag is not EINVAL");
    check(bind(e, PAGE, 7, m_0, 0) == -EINVAL, "an unknown mode is not EINVAL");
    check(bind(e, 0, MPOL_BIND, 0, 0) == 0, "an empty range is not 0 before the policy is made");
    check(bind(e, PAGE, MPOL_BIND, 0, 0) == -EINVAL, "MPOL_BIND without a mask is not EINVAL");
    check(bind(e, PAGE, MPOL_BIND, (const void *)hole, 0) == -EFAULT,
          "an unreadable mask is not EFAULT");
    check(syscall2(SYS_MUNMAP, e + PAGE, PAGE) == 0, "munmap");
    check(bind(e, 3 * PAGE, MPOL_BIND, m_0, 0) == -EFAULT && addr_is(e, MPOL_DEFAULT, 0),
          "a range with a hole is not EFAULT, or is bound in part");
    check(bind(e + PAGE, 2 * PAGE, MPOL_BIND, m_0, 0) == -EFAULT,
          "a range that begins in a hole is not EFAULT");
    check(bind(e, 3 * PAGE, MPOL_DEFAULT, 0, 0) == 0,
          "MPOL_DEFAULT does not pass over a hole in its range");
    check(bind(e + PAGE, PAGE, MPOL_DEFAULT, 0, 0) == -EFAULT,
          "a range that is all hole is not EFAULT for MPOL_DEFAULT");
    check(bind(e + 2 * PAGE, PAGE, MPOL_BIND, m_0, 0) == 0, "mbind past the hole");
    report_held();
    check(syscall2(SYS_MUNMAP, e - PAGE, PAGE * 2) == 0 &&
              syscall2(SYS_MUNMAP, e + 2 * PAGE, 3 * PAGE) == 0,
          "munmap");
}

/** home-node. */
static void home_row(void)
{
    long h = area(4);

    check(home(h, 4 * PAGE, 0, 0) == -ENOENT, "a region without a policy is not ENOENT");
    check(bind(h, 4 * PAGE, MPOL_BIND, m_0, 0) == 0, "mbind");
    check(home(h + PAGE, 2 * PAGE, 0, 0) == 0 && addr_is(h + PAGE, MPOL_BIND, 1),
          "an MPOL_BIND region does not take a home node");
    maps_read();
    check(line_policy(maps_line(h), "bind:0") && line_policy(maps_line(h + PAGE), "bind:0") &&
              line_policy(maps_line(h + 3 * PAGE), "bind:0"),
          "the home node does not split the region in three");
    check(home(h, 4 * PAGE, 1, 0) == -EINVAL, "home node 1 is not EINVAL");
    check(home(h, 4 * PAGE, 0, 1) == -EINVAL, "a flag is not EINVAL");
    check(home(h + 1, PAGE, 0, 0) == -EINVAL, "an address off a page boundary is not EINVAL");
    check(home(h, 0, 0, 0) == 0, "an empty range is not 0");
    check(bind(h + 3 * PAGE, PAGE, MPOL_PREFERRED, m_0, 0) == 0, "mbind");
    check(home(h, 4 * PAGE, 0, 0) == -EOPNOTSUPP, "an MPOL_PREFERRED region is not EOPNOTSUPP");
    maps_read();
    check(line_policy(maps_line(h), "bind:0") && !maps_line(h + PAGE) &&
              line_policy(maps_line(h + 3 * PAGE), "prefer:0"),
          "the regions before the refused one did not take their home");
    report_held();
    check(syscall2(SYS_MUNMAP, h - PAGE, 6 * PAGE) == 0, "munmap");
}

/** Whether the `n` statuses at `got` are those at `want`. */
static int statuses(const int *got, const int *want, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        if (got[i] != want[i]) {
            return 0;
        }
    }
    return 1;
}

/** move-status and move-many, over the area `v`, whose first page is
 * written and second read, and whose third is not touched. */
static void move_rows(long v)
{
    static const int want[6] = {0, -EFAULT, -ENOENT, -EFAULT, 0, -ENOENT};
    static const int nodes[6];
    unsigned long pages[6], many[48];
    int status[21];
    long fd = memfd(2);
    long w = map_fd(fd, 0, 2);
    int i;

    store(w);
    pages[0] = (unsigned long)v;
    pages[1] = (unsigned long)(v + PAGE);
    pages[2] = (unsigned long)(v + 2 * PAGE);
    pages[3] = (unsigned long)hole;
    pages[4] = (unsigned long)w;
    pages[5] = (unsigned long)(w + PAGE);
    fill(status, 0x55, sizeof(status));
    check(move(0, 6, pages, 0, status, 0) == 0 && statuses(status, want, 6),
          "move_pages does not report where the six pages are");
    fill(status, 0x55, sizeof(status));
    check(move(0, 6, pages, nodes, status, MPOL_MF_MOVE) == 0 && statuses(status, want, 6),
          "move_pages to node 0 does not report the six pages");
    report_held();

    for (i = 0; i < 48; i++) {
        many[i] = (unsigned long)(i < 20 && i != 17 ? v : hole);
    }
    fill(status, 0x55, sizeof(status));
    check(move(0, 20, many, 0, status, 0) == 0, "move_pages of 20 pages");
    for (i = 0; i < 20; i++) {
        check(status[i] == (i == 17 ? -EFAULT : 0), "move_pages misreads an address past the 16th");
    }
    check(status[20] == 0x55555555, "move_pages writes a status past the last");
    report_held();
    check(syscall2(SYS_MUNMAP, w, 2 * PAGE) == 0 && syscall1(SYS_CLOSE, fd) == 0, "munmap");
}

/** move-errors, over the written page `v`. */
static void move_errors(long v)
{
    static const int nodes[2] = {0, 1};
    unsigned long pages[2];
    int status[2];

    pages[0] = pages[1] = (unsigned long)v;
    check(move(0, 1, pages, 0, status, MPOL_MF_STRICT) == -EINVAL,
          "MPOL_MF_STRICT is not EINVAL to move_pages");
    fill(status, 0x55, sizeof(status));
    check(move(0, 2, pages, nodes, status, MPOL_MF_MOVE) == -ENODEV && status[0] == 0 &&
              status[1] == 0x55555555,
          "node 1 is not ENODEV after the status before it");
    check(move(0, 1, (const void *)hole, 0, status, 0) == -EFAULT &&
              move(0, 1, (const void *)hole, nodes, status, 0) == -EFAULT,
          "an unreadable array of addresses is not EFAULT");
    check(move(0, 1, pages, 0, (int *)hole, 0) == -EFAULT &&
              move(0, 1, pages, nodes, (int *)hole, 0) == -EFAULT,
          "an unwritable array of statuses is not EFAULT");
    check(move(0, 0, 0, 0, 0, 0) == 0, "move_pages of no pages is not 0");
    report_held();
}

/** What an ordinary user is refused, for `move-other`: 0 when each call
 * about the root process `other` and each privileged request is EPERM and
 * the user's own pages can be asked about, or the number of the first
 * check that fails. */
static int user_refused(long other, long v)
{
    unsigned long page = (unsigned long)v;
    int status = 0x55;

    if (syscall1(SYS_SETUID, probe_id(USER_ID)) != 0) {
        return 1;
    }
    if (move(other, 1, &page, 0, &status, 0) != -EPERM) {
        return 2;
    }
    if (migrate(other, MAXNODE, m_0, m_0) != -EPERM) {
        return 3;
    }
    if (move(0, 1, &page, 0, &status, MPOL_MF_MOVE_ALL) != -EPERM) {
        return 4;
    }
    if (bind(v, PAGE, MPOL_BIND, m_0, MPOL_MF_MOVE_ALL) != -EPERM) {
        return 5;
    }
    if (migrate(0, MAXNODE, m_0, m_01) != -EPERM) {
        return 6;
    }
    if (migrate(0, MAXNODE, m_0, m_0) != 0) {
        return 7;
    }
    return move(0, 1, &page, 0, &status, 0) == 0 && status == 0 ? 0 : 8;
}

/** move-other, over the written page `v`. */
static void move_other(long v)
{
    unsigned long page = (unsigned long)v;
    int status = 0x55;
    int fds[2];
    long other, user;
    char c = 0;

    fds[0] = -1;
    fds[1] = -1;
    check(syscall1(SYS_PIPE, (long)fds) == 0, "pipe");
    other = fork_child();
    if (other == 0) {
        syscall1(SYS_CLOSE, fds[1]);
        syscall3(SYS_READ, fds[0], (long)&c, 1);
        sys_exit(0);
    }
    check(move(other, 1, &page, 0, &status, 0) == 0 && status == 0,
          "root does not read the status of a child's page");
    user = fork_child();
    if (user == 0) {
        sys_exit(user_refused(other, v));
    }
    check(reap(user) == 0, "an ordinary user is not refused, or cannot ask about its own pages");
    syscall1(SYS_CLOSE, fds[0]);
    syscall1(SYS_CLOSE, fds[1]);
    check(reap(other) == 0, "the child did not exit");
    check(move(other, 1, &page, 0, &status, 0) == -ESRCH &&
              migrate(other, MAXNODE, m_0, m_0) == -ESRCH,
          "a reaped child's ID is not ESRCH");
    report_held();
}

/** migrate. */
static void migrate_row(void)
{
    check(migrate(0, MAXNODE, m_0, m_0) == 0, "migrate_pages from node 0 to node 0 is not 0");
    check(migrate(0, MAXNODE, m_01, m_0) == 0, "migrate_pages from nodes 0 and 1 is not 0");
    check(migrate(0, MAXNODE, m_0, m_01) == 0, "root's migrate_pages to nodes 0 and 1 is not 0");
    check(migrate(0, MAXNODE, m_0, m_1) == -EINVAL, "new nodes without node 0 are not EINVAL");
    check(migrate(0, MAXNODE, m_0, 0) == -EINVAL, "no new nodes are not EINVAL");
    check(migrate(0, MAXNODE, (const void *)hole, m_0) == -EFAULT,
          "an unreadable mask is not EFAULT");
    check(migrate(0, 8 * PAGE + 2, m_big, m_big) == -EINVAL,
          "a mask of more than a page of bits is not EINVAL");
    report_held();
}

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Run the rows in order, or with the word `policy` the program
 * `exec-keeps` starts; `_start` calls it with the initial stack. */
void probe_main(unsigned long *sp)
{
    static const char *const ids[] = {
        "thread-default", "thread-modes", "thread-flags", "thread-errors", "mask-maxnode",
        "get-node",       "maps-names",   "mbind-split",  "mbind-pages",   "mbind-merge",
        "mbind-mremap",   "mbind-fork",   "exec-keeps",   "mbind-shared",  "mbind-strict",
        "mbind-errors",   "home-node",    "move-status",  "move-many",     "move-errors",
        "move-other",     "migrate"};
    int argc = (int)sp[0];
    char **argv = (char **)(sp + 1);
    const char *word;
    unsigned k;
    long v;

    probe_args(argc, argv);
    word = probe_word(1);
    if (word && word[0] == 'p') {
        exec_child();
    }
    report_begin(report_case(argv + argc + 1, "guest.default.numaprobe"));
    for (k = 0; k < sizeof(ids) / sizeof(ids[0]); k++) {
        report_plan(ids[k]);
    }
    hole = area(1);
    check(syscall2(SYS_MUNMAP, hole, PAGE) == 0, "a hole");
    thread_rows();
    thread_errors();
    mask_maxnode();
    get_rows();
    mbind_rows();
    inherit_rows();
    shared_row();
    strict_row();
    mbind_errors();
    home_row();
    check(set_policy(MPOL_DEFAULT, 0, 0) == 0, "set_mempolicy");
    v = area(3);
    store(v);
    load(v + PAGE);
    move_rows(v);
    move_errors(v);
    move_other(v);
    migrate_row();
    report_end();
    probe_line("NUMAPROBE OK", 0, 0);
    sys_exit(0);
}
