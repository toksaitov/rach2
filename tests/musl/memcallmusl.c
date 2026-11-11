/**
 * memcallmusl: check the memory calls of Linux 6.10 to 6.12 that Rach2
 * adds, mseal, process_madvise, process_mrelease, cachestat and
 * memfd_secret, and its answers for memory keys and shadow stacks, against
 * what Linux 6.12 gives.
 *
 * What it expects follows the kernel, whose architecture ETUDE_KERNEL in the
 * environment names, or the program's own where it is unset: an i386
 * program on the x86-64 kernel gets the 64-bit kernel's answers, as on
 * Linux. Each part is one assertion of etude's report (probe_report.h), for
 * the case ETUDE_CASE names, in this order:
 *
 * `mseal`: on the x86-64 kernel, a flag, an unaligned start and a range with
 * a hole are EINVAL, EINVAL and ENOMEM, and an empty range is 0; a sealed
 * page refuses munmap, which unmaps nothing, mremap, MAP_FIXED and
 * mprotect, which changes the page below it first, with EPERM, refuses
 * MADV_DONTNEED when it is read-only, and MADV_GUARD_INSTALL, which keeps
 * its bytes, as Linux 6.13 does, shows `sl` in smaps, may be sealed
 * again and stays sealed in a child of fork. An mprotect to PROT_EXEC over a
 * populated anonymous page and a sealed page of memfd_secret above it is
 * EACCES, and has already made the first page read-only, so that a write
 * there raises SIGSEGV. On the i686 kernel every call is EPERM, as Linux's
 * 32-bit kernel answers.
 *
 * `pkey`: on the x86-64 kernel pkey_alloc refuses a flag and an unknown
 * access right with EINVAL, pkey_free of a key not allocated is EINVAL,
 * and pkey_mprotect with key -1 is mprotect, while a key not allocated is
 * EINVAL, but 0 for an empty range; pkey_alloc is then ENOSPC on a
 * processor without the keys, and gives key 1 where cpuid has OSPKE, which
 * pkeymusl checks further. On the i686 kernel the three are ENOSYS.
 *
 * `shadow-stack`: map_shadow_stack is EOPNOTSUPP on the x86-64 kernel and
 * ENOSYS on the i686 one.
 *
 * `cachestat`: a memfd of three pages written in its first and last counts
 * two pages in memory and none dirty, a range of its middle page none, and a
 * range past its end none, and grown by ftruncate to 0xFFFFFFFF000 bytes
 * it still counts two; EBADF for a descriptor that is not open and an
 * O_PATH one, EFAULT for a range it cannot read and EINVAL for a flag.
 *
 * `cachestat-perm`: a process that neither owns nor may write a file it
 * opened for reading, its own program, is EPERM; the program becomes user
 * 65534, or the id a native run maps it to (probe_args.h), when it is
 * root.
 *
 * `secret-file`: memfd_secret refuses a flag but O_CLOEXEC with EINVAL and
 * gives a close-on-exec descriptor for O_CLOEXEC; the file is regular, mode
 * 0600, the caller's, one link, size 0, named "/secretmem (deleted)" under
 * /proc/self/fd. read, write and F_GET_SEALS are EINVAL, fallocate
 * EOPNOTSUPP, and ftruncate sets the size once and is EINVAL after.
 *
 * `secret-map`: a private mapping is EINVAL and one with PROT_EXEC EPERM; a
 * second shared one reads what a first stored, which is named
 * "/secretmem (deleted)" in /proc/self/maps, shows `dd` in smaps, refuses
 * mprotect to PROT_EXEC with EACCES, raises SIGBUS past the size, and
 * cachestat counts its touched page as in memory and not dirty.
 *
 * `secret-refused`: /proc/self/mem cannot read the mapping (EIO),
 * process_vm_readv and get_mempolicy cannot (EFAULT), the last of a page
 * touched and of one not, which it leaves unfilled, so that cachestat still
 * counts one page; mlock fails with ENOMEM and leaves no lock mark in smaps,
 * after which munlock succeeds.
 *
 * `secret-memlock`: under an RLIMIT_MEMLOCK of one page, a child may touch
 * one page of memfd_secret and is killed by SIGBUS at the second, whether or
 * not it is root.
 *
 * `process-madvise`: a flag, advice outside WILLNEED, COLD, PAGEOUT and
 * COLLAPSE, and a descriptor that is no pidfd are EINVAL, EINVAL and EBADF;
 * MADV_COLD and MADV_PAGEOUT over a page of the caller give its 4096 bytes,
 * and over no mapping ENOMEM; over a page of a child they give 4096 for
 * root and EPERM for anyone else, who lacks CAP_SYS_NICE. As root, over a
 * process of user 1000, a caller with that user's file-system IDs and user
 * 1001's real and effective ones passes the access check, which Linux makes
 * with the file-system IDs, and is EPERM, where real or effective IDs would
 * make it EACCES.
 *
 * `process-mrelease`: a flag is EINVAL, a descriptor that is no pidfd
 * EBADF, the caller and a child that is not dying EINVAL; a child killed
 * with SIGKILL is 0, or ESRCH when it has already left its memory, and once
 * reaped ESRCH.
 *
 * `wipe-on-fork`: after MADV_WIPEONFORK over the first of two written pages,
 * which shows `wf` in smaps, a child of fork reads zeros there and the
 * second page's byte, and so does its own child after it wrote the first;
 * the parent keeps its byte. After MADV_KEEPONFORK a child reads the byte.
 * MADV_WIPEONFORK over shared anonymous memory and over a private mapping
 * of a memfd is EINVAL, as Linux takes it on private anonymous memory alone.
 *
 * `account`: private anonymous read-write pages show `ac` in smaps; after
 * mprotect(PROT_READ) the one never touched and the one only read lose it,
 * and the one written and then dropped with MADV_DONTNEED keeps it, as does
 * the unwritten second of two pages split by MADV_DONTFORK from the written
 * first, as Linux drops VM_ACCOUNT only from a region with no anon_vma and
 * a split shares it. A written MADV_WIPEONFORK page made read-only keeps it,
 * and so does a child of fork through an mprotect that changes nothing.
 *
 * Prints `MEMCALLMUSL OK` and exits 0 when every part held, and prints
 * `MEMCALLMUSL FAIL (detail)` for each failed check and exits 1 otherwise.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/fsuid.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../probe_args.h"
#include "../probe_report.h"

/* The numbers this musl does not name, the same for i386 and x86-64 but the
 * memory keys', whose x86-64 numbers are 329 to 331 and i386's 380 to 382. */
#define NR_PIDFD_OPEN       434
#define NR_PROCESS_MADVISE  440
#define NR_MEMFD_SECRET     447
#define NR_PROCESS_MRELEASE 448
#define NR_CACHESTAT        451
#define NR_MAP_SHADOW_STACK 453
#define NR_MSEAL            462
#if defined __x86_64__
#define NR_GET_MEMPOLICY 239
#else
#define NR_GET_MEMPOLICY 275
#endif
#if defined __x86_64__
#define NR_PKEY_MPROTECT 329
#define NR_PKEY_ALLOC    330
#define NR_PKEY_FREE     331
#else
#define NR_PKEY_MPROTECT 380
#define NR_PKEY_ALLOC    381
#define NR_PKEY_FREE     382
#endif

/* madvise's advice of Linux 5.4 and 6.1, and fcntl's seal command. */
#define ADV_COLD          20
#define ADV_PAGEOUT       21
#define ADV_DONTNEED      4
#define ADV_WIPEONFORK    18
#define ADV_KEEPONFORK    19
#define ADV_GUARD_INSTALL 102
#define FCNTL_GET_SEALS   1034

#define PAGE 4096ul

/* Linux's struct cachestat_range and struct cachestat, of 64-bit members
 * alone and so of one layout for both ABIs. */
struct range {
    uint64_t off;
    uint64_t len;
};
struct counts {
    uint64_t cache;
    uint64_t dirty;
    uint64_t writeback;
    uint64_t evicted;
    uint64_t recently_evicted;
};

/* Whether the kernel is x86-64's, as ETUDE_KERNEL says, or where it is unset
 * the program's own architecture. */
static int kernel_x86_64;

/* The failed checks, which decide the exit status. */
static int failures;

/** Record that the check `what` of the open part failed, with errno. */
static void miss(const char *what)
{
    char why[160];

    snprintf(why, sizeof(why), "%s: errno %d", what, errno);
    printf("MEMCALLMUSL FAIL (%s)\n", why);
    report_miss(why);
    failures++;
}

/** Check that the call `what` answered `rc`, -1 with `err` when `err` is not
 * 0, or `want` otherwise. */
static void expect(const char *what, long rc, long want, int err)
{
    if (err != 0 ? (rc != -1 || errno != err) : rc != want) {
        char why[128];

        snprintf(why, sizeof(why), "%s gave %ld", what, rc);
        miss(why);
    }
}

/** A new private anonymous mapping of `pages` pages with protection `prot`,
 * or NULL. */
static char *anon(unsigned long pages, int prot)
{
    void *p = mmap(NULL, pages * PAGE, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    return p == MAP_FAILED ? NULL : (char *)p;
}

/**
 * Find the line of /proc/self/maps for the region that holds `at`, which may
 * have merged with a neighbour, and with `smaps` the VmFlags line of
 * /proc/self/smaps after it: 0 with the line in `out` of `cap` bytes, or -1.
 */
static int region_line(const void *at, int smaps, char *out, size_t cap)
{
    char line[512];
    FILE *f = fopen(smaps ? "/proc/self/smaps" : "/proc/self/maps", "r");
    int in = 0, found = -1;

    if (f == NULL) {
        return -1;
    }
    while (found != 0 && fgets(line, sizeof(line), f) != NULL) {
        unsigned long lo, hi;

        if (sscanf(line, "%lx-%lx ", &lo, &hi) == 2) {
            in = lo <= (unsigned long)(uintptr_t)at && (unsigned long)(uintptr_t)at < hi;
            if (in && !smaps) {
                found = 0;
            }
        } else if (in && strncmp(line, "VmFlags:", 8) == 0) {
            found = 0;
        }
        if (found == 0) {
            snprintf(out, cap, "%s", line);
        }
    }
    fclose(f);
    return found;
}

/** Whether the smaps flags of the region at `at` have the flag `flag`. */
static int has_flag(const void *at, const char *flag)
{
    char line[512], word[8];

    if (region_line(at, 1, line, sizeof(line)) != 0) {
        return 0;
    }
    snprintf(word, sizeof(word), " %s", flag);
    return strstr(line, word) != NULL;
}

/** Wait for the child `pid`: its wait status, or -1. */
static int reap(pid_t pid)
{
    int status;

    return waitpid(pid, &status, 0) == pid ? status : -1;
}

/** Whether the child `pid` ends killed by the signal `sig`. */
static int killed_by(pid_t pid, int sig)
{
    int status = reap(pid);

    return status != -1 && WIFSIGNALED(status) && WTERMSIG(status) == sig;
}

/** In a child, take core files out of a death by signal. */
static void no_core(void)
{
    struct rlimit none = {0, 0};

    (void)setrlimit(RLIMIT_CORE, &none);
}

/** Protect a populated prefix before a sealed secret suffix returns EACCES.
 * Run in a child: its final write must raise SIGSEGV, and its exit frees
 * the sealed mapping. Return only after a setup failure. */
static void protect_prefix(void)
{
    int fd = (int)syscall(NR_MEMFD_SECRET, 0ul);
    char *m = anon(2, PROT_READ | PROT_WRITE);

    no_core();
    if (fd < 0 || m == NULL || ftruncate(fd, PAGE) != 0 ||
        mmap(m + PAGE, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) != m + PAGE) {
        _exit(2);
    }
    close(fd);
    *(volatile char *)m = 1;
    if (syscall(NR_MSEAL, m + PAGE, PAGE, 0ul) != 0 ||
        mprotect(m, 2 * PAGE, PROT_READ | PROT_EXEC) != -1 || errno != EACCES) {
        _exit(3);
    }
    *(volatile char *)m = 2;
    _exit(4);
}

/** The x86-64 kernel's mseal, on a mapping of three pages whose middle one
 * is sealed. */
static void seal_rows(void)
{
    char *m = anon(3, PROT_READ | PROT_WRITE), *ro = anon(1, PROT_READ), *hole = anon(4, PROT_READ);
    char *kept = anon(1, PROT_READ | PROT_WRITE);
    char line[512];
    pid_t pid;

    if (m == NULL || ro == NULL || hole == NULL || kept == NULL ||
        munmap(hole + 3 * PAGE, PAGE) != 0) {
        miss("mmap of the regions");
        return;
    }
    kept[0] = 'k';
    expect("mseal with a flag", syscall(NR_MSEAL, m, PAGE, 1ul), 0, EINVAL);
    expect("mseal of an unaligned start", syscall(NR_MSEAL, m + 1, PAGE, 0ul), 0, EINVAL);
    expect("mseal of an empty range", syscall(NR_MSEAL, m, 0ul, 0ul), 0, 0);
    expect("mseal over a hole", syscall(NR_MSEAL, hole, 4 * PAGE, 0ul), 0, ENOMEM);
    expect("mseal", syscall(NR_MSEAL, m + PAGE, PAGE, 0ul), 0, 0);
    expect("mseal again", syscall(NR_MSEAL, m + PAGE, PAGE, 0ul), 0, 0);
    expect("munmap through the seal", munmap(m, 3 * PAGE), 0, EPERM);
    expect("msync of the page below after the munmap", msync(m, PAGE, MS_ASYNC), 0, 0);
    expect("mremap of the sealed page",
           (long)(intptr_t)mremap(m + PAGE, PAGE, 2 * PAGE, MREMAP_MAYMOVE), 0, EPERM);
    expect("MAP_FIXED over the sealed page",
           (long)(intptr_t)mmap(m + PAGE, PAGE, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                                -1, 0),
           0, EPERM);
    expect("mprotect through the seal", mprotect(m, 3 * PAGE, PROT_READ), 0, EPERM);
    if (region_line(m, 0, line, sizeof(line)) != 0 || strstr(line, " r--p ") == NULL) {
        miss("the page below the seal is not read-only after the mprotect");
    }
    expect("MADV_DONTNEED of the writable sealed page", madvise(m + PAGE, PAGE, ADV_DONTNEED), 0,
           0);
    expect("mseal of a read-only page", syscall(NR_MSEAL, ro, PAGE, 0ul), 0, 0);
    expect("MADV_DONTNEED of the read-only sealed page", madvise(ro, PAGE, ADV_DONTNEED), 0, EPERM);
    expect("mprotect of a written page to read-only", mprotect(kept, PAGE, PROT_READ), 0, 0);
    expect("mseal of the written page", syscall(NR_MSEAL, kept, PAGE, 0ul), 0, 0);
    expect("MADV_GUARD_INSTALL of the sealed page", madvise(kept, PAGE, ADV_GUARD_INSTALL), 0,
           EPERM);
    if (kept[0] != 'k') {
        miss("MADV_GUARD_INSTALL dropped the sealed page's byte");
    }
    if (!has_flag(m + PAGE, "sl")) {
        miss("smaps shows no sl for the sealed page");
    }
    pid = fork();
    if (pid == 0) {
        _exit(munmap(m + PAGE, PAGE) == -1 && errno == EPERM ? 0 : 1);
    }
    if (pid < 0 || reap(pid) != 0) {
        miss("a child of fork could unmap the sealed page");
    }
    munmap(hole, 3 * PAGE);
    pid = fork();
    if (pid == 0) {
        protect_prefix();
    }
    if (pid < 0 || !killed_by(pid, SIGSEGV)) {
        miss("mprotect left a populated prefix writable before a sealed secret suffix");
    }
}

/** In a child of fork: 0 when the page `w` reads zero and the page after
 * it `q`, also in a child of its own after it wrote `w`; 1 otherwise. */
static int wiped_child(char *w)
{
    pid_t pid;

    if (w[0] != 0 || w[PAGE] != 'q') {
        return 1;
    }
    w[0] = 'c';
    pid = fork();
    if (pid == 0) {
        _exit(w[0] == 0 && w[PAGE] == 'q' ? 0 : 1);
    }
    return pid < 0 || reap(pid) != 0 || w[0] != 'c';
}

/** MADV_WIPEONFORK and MADV_KEEPONFORK through fork, and the regions
 * WIPEONFORK refuses. */
static void wipe_rows(void)
{
    char *w = anon(2, PROT_READ | PROT_WRITE);
    char *shared = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    int fd = memfd_create("memcallmusl", 0);
    char *file = fd >= 0 && ftruncate(fd, (off_t)PAGE) == 0
                     ? mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0)
                     : MAP_FAILED;
    pid_t pid;

    if (w == NULL || shared == MAP_FAILED || file == MAP_FAILED) {
        miss("mmap of the regions");
        return;
    }
    w[0] = 'p';
    w[PAGE] = 'q';
    expect("MADV_WIPEONFORK", madvise(w, PAGE, ADV_WIPEONFORK), 0, 0);
    if (!has_flag(w, "wf") || has_flag(w + PAGE, "wf")) {
        miss("smaps shows wf on the wiped page alone");
    }
    pid = fork();
    if (pid == 0) {
        _exit(wiped_child(w));
    }
    if (pid < 0 || reap(pid) != 0 || w[0] != 'p') {
        miss("a child of fork read the wiped page's byte");
    }
    expect("MADV_KEEPONFORK", madvise(w, PAGE, ADV_KEEPONFORK), 0, 0);
    pid = fork();
    if (pid == 0) {
        _exit(w[0] == 'p' ? 0 : 1);
    }
    if (pid < 0 || reap(pid) != 0) {
        miss("a child of fork read zeros after MADV_KEEPONFORK");
    }
    expect("MADV_WIPEONFORK of shared memory", madvise(shared, PAGE, ADV_WIPEONFORK), 0, EINVAL);
    expect("MADV_WIPEONFORK of a file", madvise(file, PAGE, ADV_WIPEONFORK), 0, EINVAL);
    munmap(w, 2 * PAGE);
    munmap(shared, PAGE);
    munmap(file, PAGE);
    close(fd);
}

/**
 * Private anonymous read-write pages, each between pages of PROT_NONE so
 * that none merges with a neighbour, show `ac` in smaps, and keep it after
 * mprotect(PROT_READ) only once a page of their region was written, as
 * Linux keeps VM_ACCOUNT on a region with an anon_vma: not when never
 * touched, nor when only read, which maps Linux's zero page; but still when
 * the written page was dropped with MADV_DONTNEED, since Linux keeps a
 * region's anon_vma after its pages go; and on the unwritten second page of
 * two split from the written first by MADV_DONTFORK, since a split gives
 * both parts the region's anon_vma.
 */
static void account_rows(void)
{
    static const char *const how[] = {"never touched", "only read", "written and dropped",
                                      "split from a written page"};
    static const int slot[] = {1, 3, 5, 8};
    char *res = anon(12, PROT_NONE), *m[4], why[96];
    int i;

    if (res == NULL) {
        miss("mmap of the reservation");
        return;
    }
    for (i = 0; i < 4; i++) {
        m[i] = mmap(res + slot[i] * PAGE, PAGE, PROT_READ | PROT_WRITE,
                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
        if (m[i] != res + slot[i] * PAGE) {
            miss("mmap of a page in the reservation");
            munmap(res, 12 * PAGE);
            return;
        }
        if (!has_flag(m[i], "ac")) {
            miss("a private read-write page shows ac");
        }
    }
    if (mmap(res + 7 * PAGE, 2 * PAGE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) != res + 7 * PAGE) {
        miss("mmap of two pages in the reservation");
        munmap(res, 12 * PAGE);
        return;
    }
    (void)*(volatile char *)m[1];
    *(volatile char *)m[2] = 1;
    expect("MADV_DONTNEED of the written page", madvise(m[2], PAGE, MADV_DONTNEED), 0, 0);
    res[7 * PAGE] = 1;
    expect("MADV_DONTFORK of the second page", madvise(m[3], PAGE, MADV_DONTFORK), 0, 0);
    for (i = 0; i < 4; i++) {
        expect("mprotect to PROT_READ", mprotect(m[i], PAGE, PROT_READ), 0, 0);
        if (has_flag(m[i], "ac") != (i >= 2)) {
            snprintf(why, sizeof(why), "a page %s %s ac after mprotect(PROT_READ)", how[i],
                     i >= 2 ? "keeps" : "drops");
            miss(why);
        }
    }
    munmap(res, 12 * PAGE);
}

/**
 * A written page made MADV_WIPEONFORK and read-only keeps `ac`, and a child
 * of fork, whose copy of the region has no pages and no anon_vma on Linux,
 * keeps it through an mprotect(PROT_READ) that changes nothing, as Linux's
 * mprotect_fixup returns before it looks at the flags.
 */
static void account_fork_row(void)
{
    char *m = anon(1, PROT_READ | PROT_WRITE);
    pid_t pid;

    if (m == NULL) {
        miss("mmap");
        return;
    }
    *(volatile char *)m = 1;
    expect("MADV_WIPEONFORK", madvise(m, PAGE, ADV_WIPEONFORK), 0, 0);
    expect("mprotect to PROT_READ", mprotect(m, PAGE, PROT_READ), 0, 0);
    if (!has_flag(m, "ac")) {
        miss("a written page made read-only keeps ac");
    }
    pid = fork();
    if (pid == 0) {
        _exit(mprotect(m, PAGE, PROT_READ) == 0 && has_flag(m, "ac") ? 0 : 1);
    }
    if (pid < 0 || reap(pid) != 0) {
        miss("a child keeps ac through an mprotect that changes nothing");
    }
    munmap(m, PAGE);
}

/** pkey_alloc with no flag: ENOSPC on a processor without the keys, and
 * with them, where cpuid has OSPKE, key 1, which pkey_free gives back. */
static void alloc_row(void)
{
    unsigned a, b, c, d;
    long key;

    __asm__ __volatile__("cpuid" : "=a"(a), "=b"(b), "=c"(c), "=d"(d) : "a"(7u), "c"(0u));
    if ((c & 0x10u) == 0) {
        expect("pkey_alloc", syscall(NR_PKEY_ALLOC, 0ul, 1ul), 0, ENOSPC);
        return;
    }
    key = syscall(NR_PKEY_ALLOC, 0ul, 1ul);
    expect("pkey_alloc with the keys on", key, 1, 0);
    if (key > 0) {
        expect("pkey_free of the key", syscall(NR_PKEY_FREE, key), 0, 0);
    }
}

/** The memory keys, whose calls only the x86-64 kernel has, and the shadow
 * stack, which neither kernel has. */
static void key_rows(void)
{
    char *m = anon(1, PROT_READ | PROT_WRITE);

    report_part("pkey");
    if (m == NULL) {
        miss("mmap");
        return;
    }
    if (!kernel_x86_64) {
        expect("pkey_alloc", syscall(NR_PKEY_ALLOC, 0ul, 0ul), 0, ENOSYS);
        expect("pkey_free", syscall(NR_PKEY_FREE, 1ul), 0, ENOSYS);
        expect("pkey_mprotect", syscall(NR_PKEY_MPROTECT, m, PAGE, (long)PROT_READ, -1l), 0,
               ENOSYS);
    } else {
        expect("pkey_alloc with a flag", syscall(NR_PKEY_ALLOC, 1ul, 0ul), 0, EINVAL);
        expect("pkey_alloc with an unknown right", syscall(NR_PKEY_ALLOC, 0ul, 4ul), 0, EINVAL);
        expect("pkey_free", syscall(NR_PKEY_FREE, 1ul), 0, EINVAL);
        expect("pkey_mprotect with key -1",
               syscall(NR_PKEY_MPROTECT, m, PAGE, (long)PROT_READ, -1l), 0, 0);
        expect("pkey_mprotect with key 1", syscall(NR_PKEY_MPROTECT, m, PAGE, (long)PROT_READ, 1l),
               0, EINVAL);
        expect("pkey_mprotect of no bytes with key 1",
               syscall(NR_PKEY_MPROTECT, m, 0ul, (long)PROT_READ, 1l), 0, 0);
        expect("pkey_mprotect of an unaligned start with key 1",
               syscall(NR_PKEY_MPROTECT, m + 1, PAGE, (long)PROT_READ, 1l), 0, EINVAL);
        alloc_row();
    }
    report_part("shadow-stack");
    expect("map_shadow_stack", syscall(NR_MAP_SHADOW_STACK, 0ul, PAGE, 0ul), 0,
           kernel_x86_64 ? EOPNOTSUPP : ENOSYS);
    munmap(m, PAGE);
}

/** Count the pages of `fd` in `r` into `c`: cachestat's answer. */
static long count(int fd, uint64_t off, uint64_t len, struct counts *c)
{
    struct range r;

    r.off = off;
    r.len = len;
    memset(c, 0xff, sizeof(*c));
    return syscall(NR_CACHESTAT, fd, &r, c, 0ul);
}

/** Check that `c` counts `cache` pages in memory, `dirty` of them dirty,
 * and nothing else, after the call `what`. */
static void counted(const char *what, long rc, const struct counts *c, uint64_t cache,
                    uint64_t dirty)
{
    char why[160];

    if (rc != 0 || c->cache != cache || c->dirty != dirty || c->writeback != 0 || c->evicted != 0 ||
        c->recently_evicted != 0) {
        snprintf(why, sizeof(why), "%s gave %ld: cache %llu dirty %llu writeback %llu", what, rc,
                 (unsigned long long)c->cache, (unsigned long long)c->dirty,
                 (unsigned long long)c->writeback);
        miss(why);
    }
}

/** cachestat over a memfd, and its refusals. */
static void cachestat_rows(void)
{
    static const char byte = 'x';
    struct counts c;
    struct range r = {0, 0};
    int fd = memfd_create("memcallmusl", 0), opath = open("/", O_PATH);
    char *gone = anon(1, PROT_READ);
    pid_t pid;

    report_part("cachestat");
    if (fd < 0 || opath < 0 || gone == NULL || munmap(gone, PAGE) != 0 ||
        pwrite(fd, &byte, 1, 0) != 1 || pwrite(fd, &byte, 1, 2 * PAGE) != 1) {
        miss("a memfd of three pages");
        return;
    }
    counted("cachestat of the whole file", count(fd, 0, 0, &c), &c, 2, 0);
    counted("cachestat of the middle page", count(fd, PAGE, PAGE, &c), &c, 0, 0);
    counted("cachestat past the end", count(fd, 8 * PAGE, PAGE, &c), &c, 0, 0);
    if (ftruncate(fd, (off_t)0xFFFFFFFF000ull) != 0) {
        miss("grow the memfd to its sparse maximum");
    } else {
        counted("cachestat of a maximum sparse file", count(fd, 0, 0, &c), &c, 2, 0);
    }
    expect("cachestat of a closed descriptor", syscall(NR_CACHESTAT, 999, &r, &c, 0ul), 0, EBADF);
    expect("cachestat of an O_PATH descriptor", syscall(NR_CACHESTAT, opath, &r, &c, 0ul), 0,
           EBADF);
    expect("cachestat of an unreadable range", syscall(NR_CACHESTAT, fd, gone, &c, 0ul), 0, EFAULT);
    expect("cachestat with a flag", syscall(NR_CACHESTAT, fd, &r, &c, 1ul), 0, EINVAL);
    close(fd);
    close(opath);

    report_part("cachestat-perm");
    pid = fork();
    if (pid == 0) {
        int exe = open("/proc/self/exe", O_RDONLY);

        if (exe < 0 || (geteuid() == 0 && setuid((uid_t)probe_id(65534)) != 0)) {
            _exit(2);
        }
        _exit(syscall(NR_CACHESTAT, exe, &r, &c, 0ul) == -1 && errno == EPERM ? 0 : 1);
    }
    if (pid < 0 || reap(pid) != 0) {
        miss("cachestat of a file the caller may not write was not EPERM");
    }
}

/** The descriptor and the file of memfd_secret. */
static void secret_file_rows(void)
{
    char link[64], name[64], byte = 0;
    struct stat st;
    int fd;
    ssize_t n;

    report_part("secret-file");
    expect("memfd_secret with O_NONBLOCK", syscall(NR_MEMFD_SECRET, (unsigned long)O_NONBLOCK), 0,
           EINVAL);
    fd = (int)syscall(NR_MEMFD_SECRET, (unsigned long)O_CLOEXEC);
    if (fd < 0) {
        miss("memfd_secret");
        return;
    }
    if (fcntl(fd, F_GETFD) != FD_CLOEXEC || (fcntl(fd, F_GETFL) & O_ACCMODE) != O_RDWR) {
        miss("the descriptor is not close-on-exec and read-write");
    }
    if (fstat(fd, &st) != 0 || st.st_mode != (S_IFREG | 0600) || st.st_uid != geteuid() ||
        st.st_nlink != 1 || st.st_size != 0) {
        miss("fstat of the file");
    }
    snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
    n = readlink(link, name, sizeof(name) - 1);
    name[n > 0 ? n : 0] = '\0';
    if (strcmp(name, "/secretmem (deleted)") != 0) {
        miss("the descriptor's link is not /secretmem (deleted)");
    }
    expect("write", write(fd, &byte, 1), 0, EINVAL);
    expect("read", read(fd, &byte, 1), 0, EINVAL);
    expect("F_GET_SEALS", fcntl(fd, FCNTL_GET_SEALS), 0, EINVAL);
    expect("fallocate", fallocate(fd, 0, 0, PAGE), 0, EOPNOTSUPP);
    expect("ftruncate to two pages", ftruncate(fd, 2 * PAGE), 0, 0);
    expect("ftruncate to one page after", ftruncate(fd, PAGE), 0, EINVAL);
    expect("ftruncate to two pages again", ftruncate(fd, 2 * PAGE), 0, EINVAL);
    close(fd);
}

/** A shared mapping of memfd_secret and what refuses its pages. */
static void secret_map_rows(void)
{
    char line[512], byte;
    struct counts c;
    struct iovec local, remote;
    int fd = (int)syscall(NR_MEMFD_SECRET, 0ul), mem = open("/proc/self/mem", O_RDONLY);
    char *m, *twin;
    pid_t pid;

    report_part("secret-map");
    if (fd < 0 || mem < 0 || ftruncate(fd, 2 * PAGE) != 0) {
        miss("memfd_secret of two pages");
        return;
    }
    expect("a private mapping",
           (long)(intptr_t)mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0), 0, EINVAL);
    expect("a mapping with PROT_EXEC",
           (long)(intptr_t)mmap(NULL, PAGE, PROT_READ | PROT_EXEC, MAP_SHARED, fd, 0), 0, EPERM);
    m = mmap(NULL, 3 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    twin = mmap(NULL, PAGE, PROT_READ, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        miss("a shared mapping");
        return;
    }
    m[5] = 'y';
    if (twin == MAP_FAILED || twin[5] != 'y') {
        miss("a second mapping does not read what the first stored");
    }
    if (region_line(m, 0, line, sizeof(line)) != 0 ||
        strstr(line, "/secretmem (deleted)") == NULL) {
        miss("maps does not name the mapping /secretmem (deleted)");
    }
    if (!has_flag(m, "dd")) {
        miss("smaps shows no dd for the mapping");
    }
    expect("mprotect to PROT_EXEC", mprotect(m, PAGE, PROT_READ | PROT_EXEC), 0, EACCES);
    counted("cachestat of the file", count(fd, 0, 0, &c), &c, 1, 0);
    pid = fork();
    if (pid == 0) {
        no_core();
        m[2 * PAGE] = 'z';
        _exit(0);
    }
    if (pid < 0 || !killed_by(pid, SIGBUS)) {
        miss("a touch past the size did not raise SIGBUS");
    }

    report_part("secret-refused");
    expect("a read of /proc/self/mem", pread(mem, &byte, 1, (off_t)(uintptr_t)m), 0, EIO);
    local.iov_base = &byte;
    local.iov_len = 1;
    remote.iov_base = m;
    remote.iov_len = 1;
    expect("process_vm_readv", process_vm_readv(getpid(), &local, 1, &remote, 1, 0), 0, EFAULT);
    {
        int node = -1;

        expect("get_mempolicy of a populated secret page",
               syscall(NR_GET_MEMPOLICY, &node, NULL, 0ul, m, 3ul), 0, EFAULT);
        expect("get_mempolicy of an untouched secret page",
               syscall(NR_GET_MEMPOLICY, &node, NULL, 0ul, m + PAGE, 3ul), 0, EFAULT);
        counted("secret page query did not populate the file", count(fd, 0, 0, &c), &c, 1, 0);
    }
    expect("mlock", mlock(m, PAGE), 0, ENOMEM);
    if (has_flag(m, "lo")) {
        miss("mlock marked the secret mapping locked");
    }
    expect("munlock", munlock(m, PAGE), 0, 0);
    munmap(m, 3 * PAGE);
    if (twin != MAP_FAILED) {
        munmap(twin, PAGE);
    }
    close(mem);
    close(fd);
}

/** A child under an RLIMIT_MEMLOCK of one page touches two pages of
 * memfd_secret; run while the program holds no page of its own, since the
 * count is its user's. */
static void secret_memlock_rows(void)
{
    int sync[2];
    char got = 0;
    pid_t pid;

    report_part("secret-memlock");
    if (pipe(sync) != 0) {
        miss("pipe");
        return;
    }
    pid = fork();
    if (pid == 0) {
        struct rlimit one = {PAGE, PAGE};
        int fd;
        char *m;

        no_core();
        close(sync[0]);
        fd = (int)syscall(NR_MEMFD_SECRET, 0ul);
        if (setrlimit(RLIMIT_MEMLOCK, &one) != 0 || fd < 0 || ftruncate(fd, 2 * PAGE) != 0) {
            _exit(2);
        }
        m = mmap(NULL, 2 * PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (m == MAP_FAILED) {
            _exit(3);
        }
        m[0] = 1;
        (void)write(sync[1], "1", 1);
        m[PAGE] = 1;
        _exit(0);
    }
    close(sync[1]);
    if (pid < 0 || read(sync[0], &got, 1) != 1) {
        miss("the child could not touch its first page");
    }
    if (pid < 0 || !killed_by(pid, SIGBUS)) {
        miss("the second page did not raise SIGBUS");
    }
    close(sync[0]);
}

/** Start a child that waits to be killed: its pid, with a pidfd of it in
 * `*fd`, or -1. */
static pid_t sleeper(int *fd)
{
    pid_t pid = fork();

    if (pid == 0) {
        for (;;) {
            pause();
        }
    }
    *fd = pid > 0 ? (int)syscall(NR_PIDFD_OPEN, pid, 0ul) : -1;
    return *fd >= 0 ? pid : -1;
}

/** In an isolated caller whose file-system IDs alone are the target's, its
 * real and effective ones another user's, require EPERM at CAP_SYS_NICE:
 * a kernel that judged the access by real or effective IDs would refuse it
 * first with EACCES. The target waits on a pipe and ends when the caller
 * closes it, since the caller, no longer root and none of the target's
 * users, may not kill it, as Linux's kill_ok_by_cred refuses; it is reaped
 * on every path. 0 on success, or 1 on a setup or result failure. */
static int madvise_fsids(void)
{
    uid_t uid = (uid_t)probe_id(1000), other = (uid_t)probe_id(1001);
    int ready[2], hold[2], fd = -1, ok = 0;
    char byte;
    pid_t pid;
    long rc;

    if (pipe(ready) != 0) {
        return 1;
    }
    if (pipe(hold) != 0) {
        close(ready[0]);
        close(ready[1]);
        return 1;
    }
    pid = fork();
    if (pid == 0) {
        close(ready[0]);
        close(hold[1]);
        if (setgid((gid_t)uid) != 0 || setuid(uid) != 0 || prctl(PR_SET_DUMPABLE, 1) != 0 ||
            write(ready[1], "r", 1) != 1) {
            _exit(2);
        }
        close(ready[1]);
        while (read(hold[0], &byte, 1) != 0) {
        }
        _exit(0);
    }
    close(ready[1]);
    close(hold[0]);
    if (pid <= 0 || read(ready[0], &byte, 1) != 1) {
        goto out;
    }
    fd = (int)syscall(NR_PIDFD_OPEN, pid, 0ul);
    if (fd < 0 || setresgid((gid_t)other, (gid_t)other, (gid_t)uid) != 0 ||
        setresuid(other, other, uid) != 0) {
        goto out;
    }
    setfsgid((gid_t)uid);
    setfsuid(uid);
    if ((uid_t)setfsuid((uid_t)-1) != uid || (gid_t)setfsgid((gid_t)-1) != (gid_t)uid) {
        goto out;
    }
    rc = syscall(NR_PROCESS_MADVISE, fd, NULL, 0ul, (long)ADV_COLD, 0ul);
    ok = rc == -1 && errno == EPERM;
out:
    close(ready[0]);
    close(hold[1]);
    if (fd >= 0) {
        close(fd);
    }
    if (pid > 0 && reap(pid) < 0) {
        ok = 0;
    }
    return ok ? 0 : 1;
}

/** process_madvise over the caller's pages and a child's. */
static void madvise_rows(void)
{
    char *m = anon(1, PROT_READ | PROT_WRITE), *gone = anon(1, PROT_READ);
    int self = (int)syscall(NR_PIDFD_OPEN, getpid(), 0ul), fd;
    struct iovec v;
    pid_t pid;

    report_part("process-madvise");
    if (m == NULL || gone == NULL || self < 0 || munmap(gone, PAGE) != 0) {
        miss("the mappings and the pidfd");
        return;
    }
    m[0] = 1;
    v.iov_base = m;
    v.iov_len = PAGE;
    expect("a flag", syscall(NR_PROCESS_MADVISE, self, &v, 1ul, (long)ADV_COLD, 1ul), 0, EINVAL);
    expect("a descriptor that is no pidfd",
           syscall(NR_PROCESS_MADVISE, 0, &v, 1ul, (long)ADV_COLD, 0ul), 0, EBADF);
    expect("MADV_DONTNEED", syscall(NR_PROCESS_MADVISE, self, &v, 1ul, (long)ADV_DONTNEED, 0ul), 0,
           EINVAL);
    expect("MADV_COLD", syscall(NR_PROCESS_MADVISE, self, &v, 1ul, (long)ADV_COLD, 0ul), (long)PAGE,
           0);
    expect("MADV_PAGEOUT", syscall(NR_PROCESS_MADVISE, self, &v, 1ul, (long)ADV_PAGEOUT, 0ul),
           (long)PAGE, 0);
    v.iov_base = gone;
    expect("MADV_COLD over no mapping",
           syscall(NR_PROCESS_MADVISE, self, &v, 1ul, (long)ADV_COLD, 0ul), 0, ENOMEM);
    v.iov_base = m;
    pid = sleeper(&fd);
    if (pid < 0) {
        miss("a child to advise");
    } else {
        long rc = syscall(NR_PROCESS_MADVISE, fd, &v, 1ul, (long)ADV_COLD, 0ul);

        expect("MADV_COLD of a child's page", rc, geteuid() == 0 ? (long)PAGE : 0,
               geteuid() == 0 ? 0 : EPERM);
        kill(pid, SIGKILL);
        reap(pid);
        close(fd);
    }
    close(self);
    munmap(m, PAGE);
    if (geteuid() == 0) {
        pid = fork();
        if (pid == 0) {
            _exit(madvise_fsids());
        }
        if (pid < 0 || reap(pid) != 0) {
            miss("process_madvise did not use filesystem IDs before CAP_SYS_NICE");
        }
    }
}

/** process_mrelease of the caller, a child alive and one killed. */
static void mrelease_rows(void)
{
    int self = (int)syscall(NR_PIDFD_OPEN, getpid(), 0ul), fd;
    pid_t pid = sleeper(&fd);
    long rc;

    report_part("process-mrelease");
    if (self < 0 || pid < 0) {
        miss("the pidfds");
        return;
    }
    expect("a flag", syscall(NR_PROCESS_MRELEASE, fd, 1ul), 0, EINVAL);
    expect("a descriptor that is no pidfd", syscall(NR_PROCESS_MRELEASE, 0, 0ul), 0, EBADF);
    expect("the caller", syscall(NR_PROCESS_MRELEASE, self, 0ul), 0, EINVAL);
    expect("a child alive", syscall(NR_PROCESS_MRELEASE, fd, 0ul), 0, EINVAL);
    kill(pid, SIGKILL);
    rc = syscall(NR_PROCESS_MRELEASE, fd, 0ul);
    if (rc != 0 && !(rc == -1 && errno == ESRCH)) {
        miss("a child killed was neither 0 nor ESRCH");
    }
    if (!killed_by(pid, SIGKILL)) {
        miss("the child did not die of SIGKILL");
    }
    expect("a child reaped", syscall(NR_PROCESS_MRELEASE, fd, 0ul), 0, ESRCH);
    close(fd);
    close(self);
}

int main(int argc, char **argv)
{
    const char *kernel = getenv("ETUDE_KERNEL");
    char *m;

#if defined __x86_64__
    kernel_x86_64 = kernel == NULL || strcmp(kernel, "x86_64") == 0;
#else
    kernel_x86_64 = kernel != NULL && strcmp(kernel, "x86_64") == 0;
#endif
    probe_args(argc, argv);
    printf("memcallmusl: kernel: %s\n", kernel_x86_64 ? "x86_64" : "i686");
    fflush(stdout);
    report_begin(report_case(environ, "guest.default.memcallmusl"));
    report_part("mseal");
    if (kernel_x86_64) {
        seal_rows();
    } else if ((m = anon(1, PROT_READ)) == NULL) {
        miss("mmap");
    } else {
        expect("mseal", syscall(NR_MSEAL, m, PAGE, 0ul), 0, EPERM);
        expect("mseal with a flag", syscall(NR_MSEAL, m, PAGE, 1ul), 0, EPERM);
        expect("munmap after mseal", munmap(m, PAGE), 0, 0);
    }
    key_rows();
    cachestat_rows();
    secret_file_rows();
    secret_map_rows();
    secret_memlock_rows();
    madvise_rows();
    mrelease_rows();
    report_part("wipe-on-fork");
    wipe_rows();
    report_part("account");
    account_rows();
    account_fork_row();
    report_finish();
    if (failures != 0) {
        return 1;
    }
    printf("MEMCALLMUSL OK\n");
    return 0;
}
