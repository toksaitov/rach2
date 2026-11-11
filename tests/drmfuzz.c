/**
 * drmfuzz: random ioctls on the display stack's device nodes from an
 * ordinary user, for a fixed count, with the kernel's own records of the run
 * and its heap read after; or on /dev/kvm, a VM and a vCPU.
 *
 *   drmfuzz [COUNT [SEED [TRACE]]]
 *   drmfuzz kvm [COUNT [SEED [TRACE]]]
 *
 * As root, the parent opens every node 76 brings that exists: the DRM cards
 * and render nodes, /dev/udmabuf, the evdev nodes and sw_sync's debugfs
 * file, as seatd opens a session's devices for a desktop that runs as an
 * ordinary user. It clears the kernel's log and reads the heap's live
 * objects from /proc/slabinfo. A child keeps the descriptors, drops to uid
 * and gid 65534 with no supplementary groups, opens what that user may
 * open itself, and makes COUNT (20000) calls of a generator seeded with
 * SEED (1): ioctls of the stack's types (`d`, `E`, `>`, `b`, `W` and `u`)
 * with random numbers, directions and sizes, whose arguments hold random
 * bytes, small integers, descriptors and pointers to buffers of its own;
 * reads and polls that never block; dups, closes, and maps of a page that
 * are never touched. The argument and the pointed-to buffers lie between
 * unmapped guard pages, so a call that copies past them faults instead of
 * changing the fuzzer's own state, and one argument in 64 is an address
 * where nothing of the fuzzer's is. Every descriptor a call gives back, a
 * dma-buf, a sync file or a syncobj's file, joins the set the calls choose
 * from. A timer interrupts the child every 50 ms, so that no wait a call
 * makes can hold it, and every descriptor is nonblocking. From call TRACE
 * on, each ioctl is printed with its descriptor, number and first words
 * before it is made, and each call's kind and result after, so that a run
 * that finds a fault names the call.
 *
 * Once the child has exited, the parent reads the log since the clear and
 * the heap again. It prints `drmfuzz: node: PATH` for each node opened, and
 *
 *   drmfuzz: result: CALLS calls on NODES nodes, MADE descriptors made;
 *       ok A einval B enotty C efault D eperm E other F
 *   drmfuzz: kernel: F faults, E errors, W warnings; heap objects X before,
 *       Y after
 *
 * with each record of error level or worse as `drmfuzz: record: TEXT`. F
 * counts the records of critical level or worse, which only a fault of the
 * kernel writes, as etude's kernel check reads them; E counts error
 * records, such as the heap's for a request it has no room for, which a
 * caller answers with ENOMEM. "DRMFUZZ OK" ends a run whose child made
 * every call and exited by itself and whose kernel recorded no fault;
 * "DRMFUZZ FAIL: what" any other, with status 1. A fault that panics the
 * machine ends the run with neither line. etude's report (probe_report.h),
 * for the case ETUDE_CASE names, has the assertions `open` (a node opened),
 * `calls` (the child made them all and exited by itself) and `records` (no
 * fault recorded). Freestanding; direct system calls (probe.h).
 *
 * With `kvm`, the only node is /dev/kvm, which an ordinary user opens. The
 * child, as nobody, makes a VM with a memory slot of 64 KiB at guest address
 * 0 over pages of HLT and a vCPU, which the calls never close, and the
 * ioctls are KVM's, type 0xAE: three in four with the number of one of the
 * interface's ioctls, the rest with a random number, direction and size.
 * KVM_SET_SIGNAL_MASK is not among the first, since a mask that blocks the
 * timer's signal lets a guest that never exits hold KVM_RUN, as on Linux.
 * The parent reads /proc/stat's live KVM counts (`kvm_vms` to
 * `kvm_gpa_map_pages`) before the fork and after the child is reaped, prints
 *
 *   drmfuzz: kvm: before V C S T R G P after V C S T R G P
 *
 * and fails the run, and the case's assertion `leak`, when any differs. The
 * default case is machine.kvm.fuzz.
 */

#include "probe.h"
#include "probe_report.h"

#define O_RDWR        2
#define O_NONBLOCK    04000
#define F_GETFD       1
#define F_SETFL       4
#define POLLIN        0x001
#define POLLOUT       0x004
#define PROT_READ     0x1
#define PROT_WRITE    0x2
#define MAP_SHARED    0x01
#define MAP_PRIVATE   0x02
#define MAP_ANONYMOUS 0x20
#define PROT_NONE     0x0

#define EPERM  1
#define EFAULT 14
#define EACCES 13
#define EINVAL 22
#define ENOTTY 25

#define SIGALRM         14
#define SA_SIGINFO      0x00000004u
#define SA_RESTORER     0x04000000u
#define ITIMER_REAL     0
#define SYSLOG_READ_ALL 3
#define SYSLOG_CLEAR    5

#define NOBODY 65534

/* The descriptors the calls choose from: the nodes and what calls made. */
#define FDS_MAX  256
#define FDS_KEEP 160
static int fds[FDS_MAX], nfds, nodes;

/*
 * The argument and the buffers its pointers name, in one private mapping:
 * the argument's 16 KiB, the largest size an ioctl number gives, then an
 * unmapped page, the pool's four pages and another unmapped page. A call
 * that copies past either part faults on its guard page, as it would past
 * any buffer of a program's, and leaves the fuzzer's own state alone.
 */
#define POOL      4
#define BUF_SIZE  4096
#define ARG_SIZE  16384
#define MAP_BYTES (ARG_SIZE + BUF_SIZE + POOL * BUF_SIZE + BUF_SIZE)
static unsigned char *arg, *pool[POOL], *guard;
static char text[16384];

/* `drmfuzz kvm`: the node is /dev/kvm and the ioctls are KVM's. */
static int kvm_mode;

/* KVM's ioctl numbers, built as <linux/kvm.h> builds them: the direction
 * (1 write, 2 read), the argument's size, type 0xAE and the number. */
#define KVMIO 0xAEu
#define K(dir, nr, size) \
    ((unsigned long)(dir) << 30 | (unsigned long)(size) << 16 | KVMIO << 8 | (nr))
#define KVM_CREATE_VM              K(0, 0x01, 0)
#define KVM_CREATE_VCPU            K(0, 0x41, 0)
#define KVM_SET_USER_MEMORY_REGION K(1, 0x46, 32)

/* Each ioctl of rach2/sys/kvm/kvm_abi.h but KVM_SET_SIGNAL_MASK: the
 * system's, the VM's, the devices' and the vCPU's. */
static const unsigned long kvm_cmds[] = {
    K(0, 0x00, 0),    K(0, 0x01, 0),    K(3, 0x02, 4),   K(0, 0x03, 0),    K(0, 0x04, 0),
    K(3, 0x05, 8),    K(3, 0x09, 8),    K(3, 0x0A, 4),   K(2, 0x9D, 8),    K(3, 0xC1, 8),
    K(0, 0x41, 0),    K(1, 0x42, 16),   K(0, 0x44, 0),   K(0, 0x45, 0),    K(1, 0x46, 32),
    K(0, 0x47, 0),    K(1, 0x48, 8),    K(1, 0x49, 160), K(0, 0x60, 0),    K(1, 0x61, 8),
    K(3, 0x62, 520),  K(2, 0x63, 520),  K(0, 0x64, 0),   K(3, 0x65, 72),   K(2, 0x66, 72),
    K(3, 0x67, 8),    K(1, 0x67, 16),   K(1, 0x68, 16),  K(1, 0x6A, 8),    K(0, 0x71, 0),
    K(1, 0x76, 32),   K(1, 0x77, 64),   K(0, 0x78, 0),   K(1, 0x79, 64),   K(1, 0x7B, 48),
    K(2, 0x7C, 48),   K(2, 0x9F, 112),  K(1, 0xA0, 112), K(1, 0xA3, 104),  K(1, 0xA5, 32),
    K(1, 0xBD, 24),   K(3, 0xC0, 24),   K(3, 0xE0, 12),  K(0, 0xCE, 0),    K(1, 0xE1, 24),
    K(1, 0xE2, 24),   K(1, 0xE3, 24),   K(0, 0x80, 0),   K(2, 0x81, 144),  K(1, 0x82, 144),
    K(2, 0x83, 312),  K(1, 0x84, 312),  K(3, 0x85, 24),  K(1, 0x86, 4),    K(3, 0x88, 8),
    K(1, 0x89, 8),    K(2, 0x8C, 416),  K(1, 0x8D, 416), K(2, 0x8E, 1024), K(1, 0x8F, 1024),
    K(1, 0x90, 8),    K(3, 0x91, 8),    K(1, 0x93, 8),   K(2, 0x98, 4),    K(1, 0x99, 4),
    K(0, 0x9A, 0),    K(0, 0xA2, 0),    K(0, 0xA3, 0),   K(1, 0x9B, 72),   K(1, 0x9C, 8),
    K(1, 0x9E, 64),   K(2, 0x9F, 64),   K(1, 0xA0, 64),  K(2, 0xA1, 128),  K(1, 0xA2, 128),
    K(2, 0xA4, 4096), K(1, 0xA5, 4096), K(2, 0xA6, 392), K(1, 0xA7, 392),  K(0, 0xAD, 0),
    K(3, 0xD5, 64)};

/* The guest memory of the child's VM: 64 KiB of HLT, so that a vCPU sent
 * into it exits at once. */
#define KVM_SLOT_BYTES 65536

struct k_sigaction {
    void (*handler)(int);
    unsigned long flags;
    void (*restorer)(void);
    unsigned mask[2];
};

struct ktimeval {
    long sec, usec;
};

struct kitimerval {
    struct ktimeval interval, value;
};

struct kpollfd {
    int fd;
    short events, revents;
};

extern void tick_restorer(void);
PROBE_RT_RESTORER(tick_restorer);

/** The timer's handler: nothing, since the signal's only work is to end a
 * wait. */
static void tick(int sig)
{
    (void)sig;
}

/* The generator, xorshift64*. */
static unsigned long long state;

/** Return the next value of a 64-bit xorshift generator. */
static unsigned long long rnd(void)
{
    state ^= state >> 12;
    state ^= state << 25;
    state ^= state >> 27;
    return state * 2685821657736338717ULL;
}

/** A number below `n`, which is not 0. */
static unsigned below(unsigned n)
{
    return (unsigned)(rnd() >> 33) % n;
}

/** Write S to standard output. */
static void put(const char *s)
{
    long n = 0;

    while (s[n]) {
        n++;
    }
    syscall3(SYS_WRITE, 1, (long)s, n);
}

/** `v` in decimal into `buf`, which it returns. */
static char *dec(char *buf, unsigned long v)
{
    char t[24];
    int n = 0, i = 0;

    do {
        t[n++] = (char)('0' + v % 10);
        v /= 10;
    } while (v);
    while (n) {
        buf[i++] = t[--n];
    }
    buf[i] = 0;
    return buf;
}

/* A line built a piece at a time and written with one write(2). */
static char line[256];
static int line_len;

/** Append `s` and, unless it is 0xFFFFFFFF, `v` in decimal. */
static void line_add(const char *s, unsigned long v)
{
    char num[24];
    int i;

    for (i = 0; s[i] && line_len < (int)sizeof(line) - 1; i++) {
        line[line_len++] = s[i];
    }
    if (v != 0xFFFFFFFFUL) {
        dec(num, v);
        for (i = 0; num[i] && line_len < (int)sizeof(line) - 1; i++) {
            line[line_len++] = num[i];
        }
    }
}

/** Write the line with its newline, and begin a new one. */
static void line_end(void)
{
    line[line_len++] = '\n';
    syscall3(SYS_WRITE, 1, (long)line, line_len);
    line_len = 0;
}

/** End the process with STATUS. */
static void sys_exit(int status)
{
    for (;;) {
        syscall1(SYS_EXIT_GROUP, status);
    }
}

/** Add `fd` to the set, unless it is there or the set is full. */
static void add_fd(int fd)
{
    int i;

    for (i = 0; i < nfds; i++) {
        if (fds[i] == fd) {
            return;
        }
    }
    if (nfds < FDS_MAX) {
        fds[nfds++] = fd;
        syscall3(SYS_FCNTL, fd, F_SETFL, O_NONBLOCK);
    }
}

/** Open `path` and add it as a node; 1 when it opened. */
static int open_node(const char *path)
{
    int fd = (int)syscall3(SYS_OPEN, (long)path, O_RDWR | O_NONBLOCK, 0);

    if (fd < 0) {
        fd = (int)syscall3(SYS_OPEN, (long)path, O_NONBLOCK, 0);
    }
    if (fd < 0) {
        return 0;
    }
    add_fd(fd);
    nodes++;
    probe_line("drmfuzz: node: ", path, syscall0(SYS_GETUID) == 0 ? "" : " (as nobody)");
    return 1;
}

/** Open `prefix` with the decimal `n` after it as a node. */
static void open_numbered(const char *prefix, unsigned n)
{
    char path[40], num[12];
    int k = 0, i;

    for (i = 0; prefix[i]; i++) {
        path[k++] = prefix[i];
    }
    dec(num, n);
    for (i = 0; num[i]; i++) {
        path[k++] = num[i];
    }
    path[k] = 0;
    open_node(path);
}

/** Open every node of 76's that exists and the caller may open, or
 * /dev/kvm alone in KVM's mode. */
static void open_nodes(void)
{
    unsigned i;

    if (kvm_mode) {
        open_node("/dev/kvm");
        return;
    }
    for (i = 0; i < 8; i++) {
        open_numbered("/dev/dri/card", i);
        open_numbered("/dev/dri/renderD", 128 + i);
    }
    for (i = 0; i < 16; i++) {
        open_numbered("/dev/input/event", i);
    }
    open_node("/dev/udmabuf");
    open_node("/sys/kernel/debug/sync/sw_sync");
}

/** Take into the set each descriptor above the highest known that a call
 * made, and close made ones beyond FDS_KEEP. */
static int made;

/** Add the descriptors that the last call made to the set, up to FDS_KEEP. */
static void find_new(void)
{
    int top = 2, i, fd;

    for (i = 0; i < nfds; i++) {
        if (fds[i] > top) {
            top = fds[i];
        }
    }
    for (fd = 3; fd <= top + 8 && fd < 1024; fd++) {
        if (syscall2(SYS_FCNTL, fd, F_GETFD) >= 0) {
            int known = 0;

            for (i = 0; i < nfds; i++) {
                known |= fds[i] == fd;
            }
            if (!known) {
                add_fd(fd);
                made++;
            }
        }
    }
    while (nfds > FDS_KEEP) {
        syscall1(SYS_CLOSE, fds[nfds - 1]);
        nfds--;
    }
}

/** A word for an argument: random bits, a small integer, an object id, a
 * descriptor of the set, a size, or a pointer into the pool. */
static unsigned long long word(void)
{
    switch (below(8)) {
    case 0:
    case 1:
        return rnd();
    case 2:
        return below(9);
    case 3:
        return 30 + below(40);
    case 4:
        return nfds ? (unsigned long long)fds[below((unsigned)nfds)] : 0;
    case 5:
        return 1ULL << below(32);
    default:
        return (unsigned long long)(unsigned long)(pool[below(POOL)] + below(BUF_SIZE / 8) * 8);
    }
}

/** Fill `buf` of `n` bytes with words, each in its own place. */
static void fill(unsigned char *buf, unsigned n)
{
    unsigned i;

    for (i = 0; i + 8 <= n; i += 8) {
        unsigned long long w = word();

        if (below(3) == 0) {
            *(unsigned *)(buf + i) = (unsigned)w;
            *(unsigned *)(buf + i + 4) = (unsigned)word();
        } else {
            *(unsigned long long *)(buf + i) = w;
        }
    }
}

/** An address no buffer of the fuzzer's is at: in the first page, in a
 * guard page, at the top of the address space or in the kernel's part,
 * above 0xC0000000 on i686 and in the upper half on x86-64. */
static long bad_address(void)
{
    switch (below(4)) {
    case 0:
        return (long)below(4096);
    case 1:
        return (long)(guard + below(BUF_SIZE));
    case 2:
        return (long)(~0UL - below(4096));
    default:
#if defined __x86_64__
        return (long)(0xFFFF800000000000UL + below(1u << 20) * 4096UL);
#else
        return (long)(0xC0000000UL + below(0x40000u) * 4096UL);
#endif
    }
}

/** An ioctl number of the stack's types with a random number, direction
 * and size, the number most often among those the type's decoders take. */
static unsigned long command(void)
{
    static const unsigned short kvm_sizes[] = {0,  4,   8,   12,  16,  24,  32,  48,  64,
                                               72, 104, 112, 144, 160, 312, 392, 416, 520};
    static const unsigned char types[] = {'d', 'd', 'd', 'E', '>', 'b', 'W', 'u'};
    static const unsigned short sizes[] = {0,  4,  8,  12, 16, 24, 32,  36,  40,
                                           48, 56, 64, 68, 72, 80, 104, 128, 256};
    unsigned type = types[below(sizeof(types))], nr, size, dir = below(4);

    if (kvm_mode) {
        if (below(4) != 0) {
            return kvm_cmds[below(sizeof(kvm_cmds) / sizeof(kvm_cmds[0]))];
        }
        size = kvm_sizes[below(sizeof(kvm_sizes) / sizeof(kvm_sizes[0]))];
        return (unsigned long)dir << 30 | (unsigned long)size << 16 | KVMIO << 8 | below(0x100);
    }
    switch (type) {
    case 'd':
        nr = below(4) == 0 ? below(0x40) : 0xA0 + below(0x40);
        break;
    case 'E':
        nr = below(0x100);
        break;
    default:
        nr = below(8);
        break;
    }
    size = below(4) == 0 ? below(512) : sizes[below(sizeof(sizes) / sizeof(sizes[0]))];
    if (below(32) == 0) {
        return (unsigned long)(unsigned)rnd();
    }
    return (unsigned long)dir << 30 | (unsigned long)size << 16 | (unsigned long)type << 8 | nr;
}

static unsigned long calls, ok, einval, enotty, efault, eperm, other;
static unsigned long trace_from = 0xFFFFFFFFUL;

/** Count a call's result by kind. */
static void count(long r)
{
    calls++;
    if (r >= 0) {
        ok++;
    } else if (r == -EINVAL) {
        einval++;
    } else if (r == -ENOTTY) {
        enotty++;
    } else if (r == -EFAULT) {
        efault++;
    } else if (r == -EPERM || r == -EACCES) {
        eperm++;
    } else {
        other++;
    }
}

/** One call of the generator's choosing on a descriptor of the set. */
static void step(void)
{
    int fd = nfds ? fds[below((unsigned)nfds)] : -1;
    unsigned k = below(100), i;
    struct kpollfd p;
    long r, addr;

    if (k < 80) {
        for (i = 0; i < POOL; i++) {
            if (below(16) == 0) {
                fill(pool[i], BUF_SIZE);
            }
        }
        unsigned long cmd;

        fill(arg, below(4) == 0 ? ARG_SIZE : 256);
        cmd = command();
        if (calls >= trace_from) {
            line_add("drmfuzz: call ", calls);
            line_add(": fd ", (unsigned long)fd);
            line_add(" ioctl ", cmd);
            line_add(" words", 0xFFFFFFFFUL);
            for (i = 0; i < 8; i++) {
                line_add(" ", ((unsigned *)arg)[i]);
            }
            line_end();
        }
        r = syscall3(SYS_IOCTL, fd, (long)cmd, below(64) ? (long)arg : bad_address());
    } else if (k < 86) {
        r = syscall3(SYS_READ, fd, (long)arg, (long)below(ARG_SIZE));
    } else if (k < 91) {
        p.fd = fd;
        p.events = POLLIN | POLLOUT;
        p.revents = 0;
        r = syscall3(SYS_POLL, (long)&p, 1, 0);
    } else if (k < 94) {
        r = syscall1(SYS_DUP, fd);
    } else if (k < 97) {
        addr = probe_mmap(0, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                          (long)(below(4096) * 4096UL));
        r = sys_failed(addr) ? addr : 0;
        if (!sys_failed(addr)) {
            syscall2(SYS_MUNMAP, addr, 4096);
        }
    } else {
        /* A descriptor a call made, never a node. */
        r = -EINVAL;
        if (nfds > nodes) {
            i = (unsigned)nodes + below((unsigned)(nfds - nodes));
            r = syscall1(SYS_CLOSE, fds[i]);
            fds[i] = fds[--nfds];
        }
    }
    if (calls >= trace_from) {
        line_add("drmfuzz: call ", calls);
        line_add(": kind ", k);
        line_add(" result ", (unsigned long)(r < 0 ? -r : r));
        line_add(r < 0 ? " (negated)" : "", 0xFFFFFFFFUL);
        line_end();
    }
    count(r);
    find_new();
}

/* KVM_SET_USER_MEMORY_REGION's argument. */
struct kvm_region {
    unsigned slot, flags;
    unsigned long long guest_phys_addr, memory_size, userspace_addr;
};

/** Make a VM of the /dev/kvm the child opened, with a slot over 64 KiB of
 * HLT and a vCPU, and keep both among the nodes, which no call closes. */
static void kvm_setup(void)
{
    struct kvm_region r;
    long mem, vm, vcpu;
    int i, sys = -1;

    for (i = 0; i < nfds; i++) {
        if (syscall3(SYS_IOCTL, fds[i], (long)K(0, 0x00, 0), 0) == 12) {
            sys = fds[i];
        }
    }
    mem = probe_mmap(0, KVM_SLOT_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (sys < 0 || sys_failed(mem)) {
        probe_line("DRMFUZZ FAIL: ", "nobody could not open /dev/kvm or map the guest's memory", 0);
        sys_exit(1);
    }
    for (i = 0; i < KVM_SLOT_BYTES; i++) {
        ((unsigned char *)mem)[i] = 0xF4;
    }
    vm = syscall3(SYS_IOCTL, sys, (long)KVM_CREATE_VM, 0);
    r.slot = 0;
    r.flags = 0;
    r.guest_phys_addr = 0;
    r.memory_size = KVM_SLOT_BYTES;
    r.userspace_addr = (unsigned long long)(unsigned long)mem;
    if (vm < 0 || syscall3(SYS_IOCTL, vm, (long)KVM_SET_USER_MEMORY_REGION, (long)&r) < 0) {
        probe_line("DRMFUZZ FAIL: ", "nobody could not make a VM with a memory slot", 0);
        sys_exit(1);
    }
    vcpu = syscall3(SYS_IOCTL, vm, (long)KVM_CREATE_VCPU, 0);
    if (vcpu < 0) {
        probe_line("DRMFUZZ FAIL: ", "nobody could not make a vCPU", 0);
        sys_exit(1);
    }
    add_fd((int)vm);
    add_fd((int)vcpu);
    nodes += 2;
    probe_line("drmfuzz: node: ", "a VM and a vCPU", " (as nobody)");
}

/** The child: drop to nobody, open what nobody may, and make the calls;
 * exits 0 having made them all. */
static void child(unsigned long n)
{
    struct k_sigaction ka;
    struct kitimerval it;
    if (syscall2(SYS_SETGROUPS, 0, 0) < 0 && syscall0(SYS_GETUID) == 0) {
        probe_line("DRMFUZZ FAIL: ", "setgroups", 0);
        sys_exit(1);
    }
    if (syscall1(SYS_SETGID, NOBODY) < 0 || syscall1(SYS_SETUID, NOBODY) < 0 ||
        syscall0(SYS_GETUID) != NOBODY) {
        probe_line("DRMFUZZ FAIL: ", "the child could not become an ordinary user", 0);
        sys_exit(1);
    }
    open_nodes();
    if (kvm_mode) {
        kvm_setup();
    }
    ka.handler = tick;
    /* SA_SIGINFO: i386 gives a handler without it the old frame, which
     * the restorer's rt_sigreturn does not read. */
    ka.flags = SA_SIGINFO | SA_RESTORER;
    ka.restorer = tick_restorer;
    ka.mask[0] = ka.mask[1] = 0;
    syscall4(SYS_RT_SIGACTION, SIGALRM, (long)&ka, 0, 8);
    it.interval.sec = it.value.sec = 0;
    it.interval.usec = it.value.usec = 50000;
    syscall3(SYS_SETITIMER, ITIMER_REAL, (long)&it, 0);
    while (calls < n) {
        step();
    }
    it.interval.usec = it.value.usec = 0;
    syscall3(SYS_SETITIMER, ITIMER_REAL, (long)&it, 0);
    line_add("drmfuzz: result: ", calls);
    line_add(" calls on ", (unsigned long)nodes);
    line_add(" nodes, ", (unsigned long)made);
    line_add(" descriptors made; ok ", ok);
    line_add(" einval ", einval);
    line_add(" enotty ", enotty);
    line_add(" efault ", efault);
    line_add(" eperm ", eperm);
    line_add(" other ", other);
    line_end();
    sys_exit(0);
}

/** The heap's live objects: the sum of /proc/slabinfo's active objects. */
static unsigned long heap_objects(void)
{
    int fd = (int)syscall3(SYS_OPEN, (long)"/proc/slabinfo", 0, 0);
    long n, i;
    unsigned long sum = 0;
    int line = 0;

    if (fd < 0) {
        return 0;
    }
    n = syscall3(SYS_READ, fd, (long)text, (long)sizeof(text) - 1);
    syscall1(SYS_CLOSE, fd);
    if (n <= 0) {
        return 0;
    }
    text[n] = 0;
    for (i = 0; i < n; i++) {
        if (i == 0 || text[i - 1] == '\n') {
            line++;
            if (line > 2) {
                long j = i;
                unsigned long v = 0;

                while (j < n && text[j] != ' ') {
                    j++;
                }
                while (j < n && text[j] == ' ') {
                    j++;
                }
                while (j < n && text[j] >= '0' && text[j] <= '9') {
                    v = v * 10 + (unsigned long)(text[j] - '0');
                    j++;
                }
                sum += v;
            }
        }
    }
    return sum;
}

static unsigned long number(const char *s, unsigned long def);

/* /proc/stat's live KVM counts, in this order. */
static const char *const kvm_counts[] = {"kvm_vms",          "kvm_vcpus",        "kvm_slots",
                                         "kvm_tdp_pages",    "kvm_run_mappings", "kvm_gpa_maps",
                                         "kvm_gpa_map_pages"};
#define KVM_COUNTS 7

/** Read /proc/stat's KVM counts into `v`; 0 when a count is missing. */
static int kvm_live(unsigned long *v)
{
    int fd = (int)syscall3(SYS_OPEN, (long)"/proc/stat", 0, 0), k, found = 0;
    long n, i;

    if (fd < 0) {
        return 0;
    }
    n = syscall3(SYS_READ, fd, (long)text, (long)sizeof(text) - 1);
    syscall1(SYS_CLOSE, fd);
    if (n <= 0) {
        return 0;
    }
    text[n] = 0;
    for (k = 0; k < KVM_COUNTS; k++) {
        const char *name = kvm_counts[k];

        for (i = 0; i < n; i++) {
            long j = 0;

            if (i != 0 && text[i - 1] != '\n') {
                continue;
            }
            while (name[j] && text[i + j] == name[j]) {
                j++;
            }
            if (name[j] || text[i + j] != ' ') {
                continue;
            }
            v[k] = number(text + i + j + 1, 0);
            found++;
            break;
        }
    }
    return found == KVM_COUNTS;
}

/** Fail the path's next assertion and the run with `why`. */
static void fail(const char *why)
{
    report_stop(why);
    probe_line("DRMFUZZ FAIL: ", why, 0);
    sys_exit(1);
}

/** Return S read as a decimal number, or DEF when S is absent or empty. */
static unsigned long number(const char *s, unsigned long def)
{
    unsigned long v = 0;

    if (!s || !*s) {
        return def;
    }
    while (*s >= '0' && *s <= '9') {
        v = v * 10 + (unsigned long)(*s++ - '0');
    }
    return v;
}

/**
 * Map the argument buffer and the pool, each fenced by a PROT_NONE page, so
 * that a kernel write past an ioctl's buffer faults instead of reaching the
 * generator's state.
 */
static void map_region(void)
{
    long m = probe_mmap(0, MAP_BYTES, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    int i;

    if (sys_failed(m)) {
        probe_line("DRMFUZZ FAIL: ", "mmap", 0);
        sys_exit(1);
    }
    arg = (unsigned char *)m;
    guard = arg + ARG_SIZE;
    for (i = 0; i < POOL; i++) {
        pool[i] = guard + BUF_SIZE * (i + 1);
    }
    if (syscall3(SYS_MPROTECT, (long)guard, BUF_SIZE, PROT_NONE) < 0 ||
        syscall3(SYS_MPROTECT, (long)(pool[POOL - 1] + BUF_SIZE), BUF_SIZE, PROT_NONE) < 0) {
        probe_line("DRMFUZZ FAIL: ", "mprotect", 0);
        sys_exit(1);
    }
}

/**
 * Count the kernel's records in the LEN bytes of text by level: 0 to 2 are
 * faults, 3 errors and 4 warnings. The first 8 records at level 3 or worse
 * are printed.
 */
static void count_records(long len, int *faults, int *errors, int *warnings)
{
    long i, j;
    int shown = 0;

    for (i = 0; i + 2 < len; i++) {
        if ((i != 0 && text[i - 1] != '\n') || text[i] != '<' || text[i + 2] != '>') {
            continue;
        }
        if (text[i + 1] == '4') {
            (*warnings)++;
        }
        if (text[i + 1] < '0' || text[i + 1] > '3') {
            continue;
        }
        *faults += text[i + 1] <= '2';
        *errors += text[i + 1] == '3';
        if (shown++ < 8) {
            j = i;
            while (j < len && text[j] != '\n') {
                j++;
            }
            text[j] = 0;
            probe_line("drmfuzz: record: ", text + i, 0);
            text[j] = '\n';
        }
    }
}

/** Print the live KVM counts `before` the fork and now, and fail the run
 * and the assertion `leak` when any differs. */
static void kvm_judge(const unsigned long *before)
{
    unsigned long after[KVM_COUNTS];
    int k, leaked = 0;

    if (!kvm_live(after)) {
        fail("/proc/stat has no KVM counts");
    }
    line_add("drmfuzz: kvm: before", 0xFFFFFFFFUL);
    for (k = 0; k < KVM_COUNTS; k++) {
        line_add(" ", before[k]);
    }
    line_add(" after", 0xFFFFFFFFUL);
    for (k = 0; k < KVM_COUNTS; k++) {
        line_add(" ", after[k]);
        leaked |= after[k] != before[k];
    }
    line_end();
    if (leaked) {
        fail("a KVM object outlived the child");
    }
    report_held();
}

PROBE_SP_ENTRY;

void probe_main(unsigned long *sp);

/** Open the nodes, run the child and judge what the kernel recorded. */
void probe_main(unsigned long *sp)
{
    char **argv = (char **)(sp + 1), **env = argv + sp[0] + 1;
    unsigned long n, before, after, kvm_before[KVM_COUNTS];
    unsigned long argc = sp[0];
    int status = 0, faults = 0, errors = 0, warnings = 0;
    long pid, len, i;

    if (argc > 1 && argv[1][0] == 'k' && argv[1][1] == 'v' && argv[1][2] == 'm' && !argv[1][3]) {
        kvm_mode = 1;
        argv++;
        argc--;
    }
    n = number(argc > 1 ? argv[1] : 0, 20000);
    state = number(argc > 2 ? argv[2] : 0, 1) * 0x9E3779B97F4A7C15ULL + 1;
    trace_from = number(argc > 3 ? argv[3] : 0, 0xFFFFFFFFUL);
    map_region();
    report_begin(report_case(env, kvm_mode ? "machine.kvm.fuzz" : "guest.dmabuf.drmfuzz"));
    report_plan("open");
    report_plan("calls");
    report_plan("records");
    if (kvm_mode) {
        report_plan("leak");
    }
    open_nodes();
    if (nodes == 0) {
        fail(kvm_mode ? "/dev/kvm did not open" : "no node of the display stack opened");
    }
    report_held();
    if (kvm_mode && !kvm_live(kvm_before)) {
        fail("/proc/stat has no KVM counts");
    }
    syscall3(SYS_SYSLOG, SYSLOG_CLEAR, 0, 0);
    before = heap_objects();
    pid = syscall0(SYS_FORK);
    if (pid == 0) {
        child(n);
    }
    if (pid < 0) {
        fail("fork");
    }
    for (i = 0; i < nfds; i++) {
        syscall1(SYS_CLOSE, fds[i]);
    }
    if (syscall3(SYS_WAITPID, pid, (long)&status, 0) != pid) {
        fail("wait");
    }
    after = heap_objects();
    len = syscall3(SYS_SYSLOG, SYSLOG_READ_ALL, (long)text, (long)sizeof(text) - 1);
    count_records(len, &faults, &errors, &warnings);
    line_add("drmfuzz: kernel: ", (unsigned long)faults);
    line_add(" faults, ", (unsigned long)errors);
    line_add(" errors, ", (unsigned long)warnings);
    line_add(" warnings; heap objects ", before);
    line_add(" before, ", after);
    line_add(" after", 0xFFFFFFFFUL);
    line_end();
    if ((status & 0x7f) != 0) {
        fail("the child was killed by a signal");
    }
    if (((status >> 8) & 0xff) != 0) {
        fail("the child did not make its calls");
    }
    report_held();
    if (faults) {
        fail("the kernel recorded a fault");
    }
    report_held();
    if (kvm_mode) {
        kvm_judge(kvm_before);
    }
    report_end();
    put("DRMFUZZ OK\n");
    sys_exit(0);
}
