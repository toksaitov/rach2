/**
 * pkeymusl: check memory protection keys against Linux 6.12: pkey_alloc,
 * pkey_free and pkey_mprotect, PKRU through faults, signals, fork, threads,
 * exec and ptrace, the kernel's own access to keyed memory, the
 * execute-only key, and what cpuid and /proc show.
 *
 * What it expects follows the kernel, whose architecture ETUDE_KERNEL in the
 * environment names, or the program's own where it is unset, and the
 * processor. On the x86-64 kernel with OSPKE in cpuid the keys are on, as on
 * Rach2's and Linux's x86-64 kernels under QEMU's max processor; on the
 * x86-64 kernel without it they are off, and the calls answer as on a
 * processor without keys; on the i686 kernel the three calls are ENOSYS and
 * no region has a key. An i386 program on the x86-64 kernel gets the 64-bit
 * kernel's answers, as on Linux. Each part is one assertion of etude's
 * report (probe_report.h), for the case ETUDE_CASE names, and runs in a
 * child of fork, so that the keys a part allocates and the PKRU it sets do
 * not reach the next. With the keys on:
 *
 * `cpu`: cpuid has OSPKE, /proc/cpuinfo `pku` and `ospke`, XCR0 PKRU, the
 * XSAVE size for XCR0 reaches past PKRU, and PKRU starts as 0x55555554, key
 * 0 open and every other key denied. With the keys off, neither OSPKE nor
 * `ospke`.
 *
 * `alloc`: pkey_alloc refuses a flag and an unknown right with EINVAL, gives
 * keys 1 to 15 in order with the rights asked written into PKRU, and then
 * ENOSPC; a key freed is given again, and freed twice is EINVAL, as are 16
 * and -1; key 0 may be freed and is then given again. With the keys off
 * pkey_alloc is ENOSPC and pkey_free EINVAL, 0 too.
 *
 * `mprotect`: pkey_mprotect gives a page in the middle of three a key, which
 * splits the region in smaps' ProtectionKey; a key not allocated, 16 and -2
 * are EINVAL, but 0 for an empty range, and an unaligned start is EINVAL;
 * mprotect keeps the key; key 0 again merges the three; mremap and fork
 * keep the key, and the child may free it. With the keys off pkey_mprotect
 * is mprotect with key -1 and EINVAL with key 0, and smaps has no key.
 *
 * `fault`: a read of a page under a key PKRU denies, present or not, raises
 * SIGSEGV with SEGV_PKUERR, si_pkey the key and si_addr the address; the
 * handler runs with PKRU 0x55555554 and finds the interrupted PKRU in the
 * frame's XSAVE image, PKRU marked in use, and the access runs again on the
 * PKRU the handler writes there; a key denied writes alone faults a write and
 * not a read; a write to a read-only page under a denied key is SEGV_PKUERR
 * and under an open key SEGV_ACCERR.
 *
 * `signal`: a handler of SIGUSR1 runs with PKRU 0x55555554, the frame holds
 * the interrupted PKRU, and sigreturn gives that back, or what the handler
 * wrote in the frame.
 *
 * `kernel`: read into and write from a page whose key PKRU denies are
 * EFAULT, and read into one whose writes it denies, while write from it
 * works; /proc/self/mem, process_vm_readv and PTRACE_PEEKDATA reach a
 * denied page, as Linux's access to another address space ignores the keys.
 *
 * `exec-only`: mprotect and mmap with PROT_EXEC alone give the execute-only
 * key, the lowest free one, which PKRU then denies: the page runs, a read of
 * it is SEGV_PKUERR with the key, and a later PROT_EXEC denies it again;
 * pkey_alloc passes it over, pkey_free and pkey_mprotect refuse it, and
 * PROT_READ|PROT_EXEC moves the page back to key 0. With the keys off the
 * page runs and reads.
 *
 * `inherit`: a child of fork and a new thread start with the creator's
 * PKRU; a program exec starts has PKRU 0x55555554 and none of the keys.
 *
 * `ptrace`: NT_X86_XSTATE of a stopped tracee holds its PKRU, marked in
 * use, and the tracee runs on the PKRU a tracer writes there.
 *
 * `proc`: each region of /proc/self/smaps has a ProtectionKey line before
 * its VmFlags, and none has one with the keys off.
 *
 * Prints `PKEYMUSL OK` and exits 0 when every part held, and prints
 * `PKEYMUSL FAIL (detail)` for each failed check and exits 1 otherwise.
 */

#define _GNU_SOURCE
#include <elf.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/ptrace.h>
#include <sys/syscall.h>
#include <sys/uio.h>
#include <sys/wait.h>
#include <ucontext.h>
#include <unistd.h>

#include "../probe_args.h"
#include "../probe_report.h"

/* The key calls, whose x86-64 numbers are 329 to 331 and i386's 380 to 382. */
#if defined __x86_64__
#define NR_PKEY_MPROTECT 329
#define NR_PKEY_ALLOC    330
#define NR_PKEY_FREE     331
#else
#define NR_PKEY_MPROTECT 380
#define NR_PKEY_ALLOC    381
#define NR_PKEY_FREE     382
#endif

#define PAGE                4096ul
#define PKEY_DISABLE_ACCESS 1ul
#define PKEY_DISABLE_WRITE  2ul
#define PKRU_AD(k)          (1u << (2 * (k)))
#define PKRU_WD(k)          (2u << (2 * (k)))
#define PKRU_INIT           0x55555554u
#define SEGV_PKUERR_CODE    4
#define XFEATURE_PKRU       0x200u
#define FP_XSTATE_MAGIC1    0x46505853u

/* What the kernel and the processor give: the keys on, the calls without
 * keys, or no calls, on the i686 kernel. */
enum mode { KEYS, NOKEYS, NOCALLS };
static enum mode mode;

/* PKRU's offset in the XSAVE image, from cpuid leaf 0xD. */
static unsigned pkru_off;

/* The failed checks of the parent, which decide the exit status, and of
 * the child that runs a part. */
static int failures;
static int bad;

/* What the SIGSEGV and SIGUSR1 handler saw, and what it does: the bits it
 * clears from the frame's PKRU, and whether it leaves by siglongjmp. */
static volatile int faults;
static volatile int fault_code;
static volatile unsigned fault_pkey;
static volatile uintptr_t fault_addr;
static volatile uint32_t handler_pkru;
static volatile uint32_t frame_pkru;
static volatile int frame_bv;
static volatile uint32_t fix_mask;
static volatile int escape_on;
static sigjmp_buf escape;

/* mov $42, %eax; ret: the same bytes in 32-bit and 64-bit code. */
static const unsigned char code42[] = {0xb8, 0x2a, 0x00, 0x00, 0x00, 0xc3};

/** cpuid of `leaf` and `sub` into r[0..3], EAX to EDX. */
static void cpuid(unsigned leaf, unsigned sub, unsigned r[4])
{
    __asm__ __volatile__("cpuid"
                         : "=a"(r[0]), "=b"(r[1]), "=c"(r[2]), "=d"(r[3])
                         : "a"(leaf), "c"(sub));
}

/** The low half of XCR0: `xgetbv` with ECX 0. */
static unsigned xcr0_low(void)
{
    unsigned lo, hi;

    __asm__ __volatile__(".byte 0x0f, 0x01, 0xd0" : "=a"(lo), "=d"(hi) : "c"(0u));
    return lo;
}

/** PKRU: `rdpkru`. */
static uint32_t rdpkru(void)
{
    uint32_t lo, hi;

    __asm__ __volatile__(".byte 0x0f, 0x01, 0xee" : "=a"(lo), "=d"(hi) : "c"(0u));
    return lo;
}

/** Write PKRU: `wrpkru`. */
static void wrpkru(uint32_t v)
{
    __asm__ __volatile__(".byte 0x0f, 0x01, 0xef" : : "a"(v), "c"(0u), "d"(0u) : "memory");
}

/** pkey_alloc by its number, which musl 1.2.5 has no wrapper for. */
static long pkey_alloc_(unsigned long flags, unsigned long rights)
{
    return syscall(NR_PKEY_ALLOC, flags, rights);
}

/** pkey_free by its number. */
static long pkey_free_(long key)
{
    return syscall(NR_PKEY_FREE, key);
}

/** pkey_mprotect by its number. */
static long pkey_mprotect_(void *at, unsigned long len, long prot, long key)
{
    return syscall(NR_PKEY_MPROTECT, at, len, prot, key);
}

/** Record that the check `what` of the parent's open part failed. */
static void miss(const char *what)
{
    printf("PKEYMUSL FAIL (%s)\n", what);
    fflush(stdout);
    report_miss(what);
    failures++;
}

/** In a part's child, record the check `what` unless `ok`. */
static void check(int ok, const char *what)
{
    if (!ok) {
        printf("PKEYMUSL FAIL (%s: errno %d)\n", what, errno);
        fflush(stdout);
        bad++;
    }
}

/** In a part's child, check that the call `what` answered `rc`, -1 with
 * `err` when `err` is not 0, or `want` otherwise. */
static void expect(const char *what, long rc, long want, int err)
{
    char why[160];

    if (err != 0 ? (rc != -1 || errno != err) : rc != want) {
        snprintf(why, sizeof(why), "%s gave %ld", what, rc);
        check(0, why);
    }
}

/** A new private anonymous mapping of `pages` pages with protection `prot`,
 * or NULL. */
static char *anon(unsigned long pages, int prot)
{
    void *p = mmap(NULL, pages * PAGE, prot, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);

    return p == MAP_FAILED ? NULL : (char *)p;
}

/** A read of the byte at `p`, which the compiler must make. */
static int peek(const volatile char *p)
{
    return *p;
}

/** A write of `v` to the byte at `p`. */
static void poke(volatile char *p, char v)
{
    *p = v;
}

/**
 * Find the region of /proc/self/smaps that holds `at`: its ProtectionKey in
 * `*key`, -1 when it has no such line, and its bytes in `*span`. 0, or -1
 * when no region holds it.
 */
static int smaps_region(const void *at, long *key, unsigned long *span)
{
    char line[512];
    FILE *f = fopen("/proc/self/smaps", "r");
    int in = 0, found = -1;

    *key = -1;
    *span = 0;
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        unsigned long lo, hi, k;

        if (sscanf(line, "%lx-%lx ", &lo, &hi) == 2) {
            if (found == 0) {
                break;
            }
            in = lo <= (unsigned long)(uintptr_t)at && (unsigned long)(uintptr_t)at < hi;
            if (in) {
                found = 0;
                *span = hi - lo;
            }
        } else if (in && sscanf(line, "ProtectionKey: %lu", &k) == 1) {
            *key = (long)k;
        }
    }
    fclose(f);
    return found;
}

/** The ProtectionKey of the region that holds `at`, -1 for none, or -2 when
 * no region holds it. */
static long smaps_key(const void *at)
{
    long key;
    unsigned long span;

    return smaps_region(at, &key, &span) == 0 ? key : -2;
}

/**
 * Read /proc/self/smaps in one pass: the regions in `*regions`, and in
 * `*keyed` those whose ProtectionKey line comes before their VmFlags line.
 * Returns the ProtectionKey lines, or -1 when the file cannot be read.
 */
static int smaps_keys(int *regions, int *keyed)
{
    char line[512];
    FILE *f = fopen("/proc/self/smaps", "r");
    int keys = 0, seen = 0;

    *regions = 0;
    *keyed = 0;
    if (f == NULL) {
        return -1;
    }
    while (fgets(line, sizeof(line), f) != NULL) {
        if (strncmp(line, "ProtectionKey:", 14) == 0) {
            keys++;
            seen = 1;
        } else if (strncmp(line, "VmFlags:", 8) == 0) {
            ++*regions;
            *keyed += seen;
            seen = 0;
        }
    }
    fclose(f);
    return keys;
}

/** Whether a flags line of /proc/cpuinfo has the flag `name`. */
static int cpuinfo_has(const char *name)
{
    char line[4096];
    FILE *f = fopen("/proc/cpuinfo", "r");
    size_t n = strlen(name);
    int has = 0;

    if (f == NULL) {
        return 0;
    }
    while (!has && fgets(line, sizeof(line), f) != NULL) {
        char *p = line;

        if (strncmp(line, "flags", 5) != 0) {
            continue;
        }
        while ((p = strstr(p, name)) != NULL) {
            if (p[-1] == ' ' && (p[n] == ' ' || p[n] == '\n' || p[n] == '\0')) {
                has = 1;
                break;
            }
            p += n;
        }
    }
    fclose(f);
    return has;
}

/** The XSAVE image of the signal frame `uc`, or NULL when its software bytes
 * do not announce one: at `fpregs` on x86-64, past the 112 bytes of the x87
 * part, the status word and the magic word on i386. */
static unsigned char *frame_image(ucontext_t *uc)
{
    unsigned char *img = (unsigned char *)uc->uc_mcontext.fpregs;
    uint32_t magic;

    if (img == NULL) {
        return NULL;
    }
#if !defined __x86_64__
    img += 112;
#endif
    memcpy(&magic, img + 464, 4);
    return magic == FP_XSTATE_MAGIC1 ? img : NULL;
}

/** Record the signal and the PKRU it was delivered with and that its frame
 * holds; clear `fix_mask` from the frame's PKRU, which sigreturn then
 * loads; leave by siglongjmp when `escape_on` is set. More than eight
 * faults in a row are a loop, and end the child. */
static void on_signal(int sig, siginfo_t *si, void *ctx)
{
    unsigned char *img = frame_image((ucontext_t *)ctx);
    uint64_t bv;
    uint32_t v;

    (void)sig;
    if (++faults > 8) {
        _exit(99);
    }
    handler_pkru = mode == KEYS ? rdpkru() : 0;
    fault_code = si->si_code;
    fault_pkey = si->si_pkey;
    fault_addr = (uintptr_t)si->si_addr;
    frame_bv = 0;
    frame_pkru = 0;
    if (img != NULL && mode == KEYS) {
        memcpy(&bv, img + 512, 8);
        frame_bv = (bv & XFEATURE_PKRU) != 0;
        memcpy(&v, img + pkru_off, 4);
        frame_pkru = v;
        v &= ~fix_mask;
        memcpy(img + pkru_off, &v, 4);
    }
    if (escape_on) {
        siglongjmp(escape, 1);
    }
}

/** Catch SIGSEGV and SIGUSR1 with `on_signal`. */
static void catch_signals(void)
{
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));
    sa.sa_sigaction = on_signal;
    sa.sa_flags = SA_SIGINFO | SA_NODEFER;
    sigaction(SIGSEGV, &sa, NULL);
    sigaction(SIGUSR1, &sa, NULL);
}

/** The `cpu` part: what cpuid, XCR0 and /proc/cpuinfo say, and the first
 * PKRU. */
static void cpu_rows(void)
{
    unsigned r[4];

    cpuid(7, 0, r);
    check(((r[2] >> 4) & 1u) == (mode == KEYS), "OSPKE in cpuid");
    check(cpuinfo_has("ospke") == (mode == KEYS), "ospke in /proc/cpuinfo");
    if (mode != KEYS) {
        return;
    }
    check(cpuinfo_has("pku"), "pku in /proc/cpuinfo");
    check((xcr0_low() & XFEATURE_PKRU) != 0, "PKRU in XCR0");
    cpuid(0xD, 0, r);
    check(r[1] >= pkru_off + 8, "the XSAVE size of XCR0 holds PKRU");
    check(rdpkru() == PKRU_INIT, "the first PKRU");
}

/** The `alloc` part: pkey_alloc and pkey_free. */
static void alloc_rows(void)
{
    long k;

    if (mode == NOCALLS) {
        expect("pkey_alloc", pkey_alloc_(0, 0), 0, ENOSYS);
        expect("pkey_free", pkey_free_(1), 0, ENOSYS);
        return;
    }
    expect("pkey_alloc with a flag", pkey_alloc_(1, 0), 0, EINVAL);
    expect("pkey_alloc with an unknown right", pkey_alloc_(0, 4), 0, EINVAL);
    if (mode == NOKEYS) {
        expect("pkey_alloc", pkey_alloc_(0, 0), 0, ENOSPC);
        expect("pkey_free of 1", pkey_free_(1), 0, EINVAL);
        expect("pkey_free of 0", pkey_free_(0), 0, EINVAL);
        return;
    }
    expect("pkey_alloc", pkey_alloc_(0, 0), 1, 0);
    check((rdpkru() & (PKRU_AD(1) | PKRU_WD(1))) == 0, "key 1 open in PKRU");
    expect("pkey_alloc denying writes", pkey_alloc_(0, PKEY_DISABLE_WRITE), 2, 0);
    check((rdpkru() & (PKRU_AD(2) | PKRU_WD(2))) == PKRU_WD(2), "key 2 denied writes in PKRU");
    expect("pkey_alloc denying access", pkey_alloc_(0, PKEY_DISABLE_ACCESS), 3, 0);
    check((rdpkru() & (PKRU_AD(3) | PKRU_WD(3))) == PKRU_AD(3), "key 3 denied access in PKRU");
    for (k = 4; k < 16; k++) {
        expect("pkey_alloc of the next key", pkey_alloc_(0, 0), k, 0);
    }
    expect("pkey_alloc of a seventeenth key", pkey_alloc_(0, 0), 0, ENOSPC);
    expect("pkey_free", pkey_free_(2), 0, 0);
    expect("pkey_free again", pkey_free_(2), 0, EINVAL);
    expect("pkey_alloc after pkey_free", pkey_alloc_(0, 0), 2, 0);
    expect("pkey_free of 16", pkey_free_(16), 0, EINVAL);
    expect("pkey_free of -1", pkey_free_(-1), 0, EINVAL);
    expect("pkey_free of 0", pkey_free_(0), 0, 0);
    expect("pkey_alloc after freeing 0", pkey_alloc_(0, 0), 0, 0);
}

/** The key rows of pkey_mprotect over `m`, three read-write pages between
 * two inaccessible ones, with the keys on. */
static void mprotect_key_rows(char *m)
{
    long k = pkey_alloc_(0, 0), key;
    unsigned long span;
    char *n;
    pid_t pid;
    int status;

    expect("pkey_mprotect of the middle page",
           pkey_mprotect_(m + PAGE, PAGE, PROT_READ | PROT_WRITE, k), 0, 0);
    check(smaps_key(m) == 0 && smaps_key(m + PAGE) == k && smaps_key(m + 2 * PAGE) == 0,
          "the keys of the three pages in smaps");
    expect("pkey_mprotect with key 16", pkey_mprotect_(m, PAGE, PROT_READ, 16), 0, EINVAL);
    expect("pkey_mprotect with a key not allocated", pkey_mprotect_(m, PAGE, PROT_READ, 15), 0,
           EINVAL);
    expect("pkey_mprotect with key -2", pkey_mprotect_(m, PAGE, PROT_READ, -2), 0, EINVAL);
    expect("pkey_mprotect of no bytes", pkey_mprotect_(m, 0, PROT_READ, 15), 0, 0);
    expect("pkey_mprotect of an unaligned start", pkey_mprotect_(m + 1, PAGE, PROT_READ, k), 0,
           EINVAL);
    expect("mprotect of the keyed page", mprotect(m + PAGE, PAGE, PROT_READ), 0, 0);
    check(smaps_key(m + PAGE) == k, "mprotect keeps the key");
    expect("pkey_mprotect back to key 0", pkey_mprotect_(m, 3 * PAGE, PROT_READ | PROT_WRITE, 0), 0,
           0);
    check(smaps_region(m, &key, &span) == 0 && key == 0 && span == 3 * PAGE,
          "the three pages merge under key 0");
    expect("pkey_mprotect of the three", pkey_mprotect_(m, 3 * PAGE, PROT_READ | PROT_WRITE, k), 0,
           0);
    n = mremap(m, 3 * PAGE, 6 * PAGE, MREMAP_MAYMOVE);
    check(n != MAP_FAILED && smaps_key(n) == k && smaps_key(n + 5 * PAGE) == k,
          "mremap keeps the key");
    if (n == MAP_FAILED) {
        return;
    }
    pid = fork();
    if (pid == 0) {
        _exit(smaps_key(n) == k && pkey_free_(k) == 0 ? 0 : 1);
    }
    check(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          "a child of fork keeps the key and may free it");
    munmap(n, 6 * PAGE);
}

/** The `mprotect` part: pkey_mprotect over three pages between two
 * inaccessible ones. */
static void mprotect_rows(void)
{
    char *m = anon(5, PROT_NONE);

    if (m == NULL || mprotect(m + PAGE, 3 * PAGE, PROT_READ | PROT_WRITE) != 0) {
        check(0, "mmap");
        return;
    }
    if (mode == NOCALLS) {
        expect("pkey_mprotect", pkey_mprotect_(m + PAGE, PAGE, PROT_READ, -1), 0, ENOSYS);
    } else if (mode == NOKEYS) {
        expect("pkey_mprotect with key -1", pkey_mprotect_(m + PAGE, PAGE, PROT_READ, -1), 0, 0);
        expect("pkey_mprotect with key 0", pkey_mprotect_(m + PAGE, PAGE, PROT_READ, 0), 0, EINVAL);
    } else {
        mprotect_key_rows(m + PAGE);
    }
    if (mode != KEYS) {
        check(smaps_key(m + PAGE) == -1, "no ProtectionKey in smaps");
    }
    munmap(m, 5 * PAGE);
}

/** The `fault` part: SEGV_PKUERR, its siginfo, and the PKRU of the handler
 * and of its frame. */
static void fault_rows(void)
{
    long k;
    char *p, *r;
    uint32_t before;

    if (mode != KEYS) {
        return;
    }
    catch_signals();
    k = pkey_alloc_(0, PKEY_DISABLE_ACCESS);
    p = anon(2, PROT_READ | PROT_WRITE);
    r = anon(1, PROT_READ);
    if (k < 0 || p == NULL || r == NULL) {
        check(0, "pkey_alloc or mmap");
        return;
    }
    poke(p, 1);
    expect("pkey_mprotect", pkey_mprotect_(p, 2 * PAGE, PROT_READ | PROT_WRITE, k), 0, 0);
    before = rdpkru();
    fix_mask = PKRU_AD(k) | PKRU_WD(k);
    faults = 0;
    check(peek(p) == 1, "the read after the handler opened the key");
    check(faults == 1 && fault_code == SEGV_PKUERR_CODE && fault_pkey == (unsigned)k &&
              fault_addr == (uintptr_t)p,
          "a read of a denied page is SEGV_PKUERR with the key and the address");
    check(handler_pkru == PKRU_INIT, "the handler's PKRU");
    check(frame_pkru == before && frame_bv, "the frame's PKRU, marked in use");
    check(rdpkru() == (before & ~fix_mask), "sigreturn takes the frame's PKRU");

    wrpkru(before);
    faults = 0;
    poke(p + PAGE, 2);
    check(faults == 1 && fault_code == SEGV_PKUERR_CODE && fault_addr == (uintptr_t)(p + PAGE),
          "a write of a denied page not yet present is SEGV_PKUERR");

    wrpkru((before & ~PKRU_AD(k)) | PKRU_WD(k));
    faults = 0;
    (void)peek(p);
    check(faults == 0, "a read under a key denied writes");
    poke(p, 3);
    check(faults == 1 && fault_code == SEGV_PKUERR_CODE, "a write under a key denied writes");

    expect("pkey_mprotect of a read-only page", pkey_mprotect_(r, PAGE, PROT_READ, k), 0, 0);
    wrpkru(before);
    fix_mask = 0;
    escape_on = 1;
    faults = 0;
    if (sigsetjmp(escape, 1) == 0) {
        poke(r, 1);
    }
    check(faults == 1 && fault_code == SEGV_PKUERR_CODE,
          "a write of a read-only page under a denied key");
    wrpkru(before & ~(PKRU_AD(k) | PKRU_WD(k)));
    faults = 0;
    if (sigsetjmp(escape, 1) == 0) {
        poke(r, 1);
    }
    check(faults == 1 && fault_code == SEGV_ACCERR,
          "a write of a read-only page under an open key");
    escape_on = 0;
}

/** The `signal` part: PKRU through a handler of SIGUSR1 and sigreturn. */
static void signal_rows(void)
{
    long k;
    uint32_t v;

    if (mode != KEYS) {
        return;
    }
    catch_signals();
    k = pkey_alloc_(0, 0);
    if (k < 0) {
        check(0, "pkey_alloc");
        return;
    }
    v = (rdpkru() & ~PKRU_AD(k)) | PKRU_WD(k);
    wrpkru(v);
    fix_mask = 0;
    faults = 0;
    raise(SIGUSR1);
    check(faults == 1 && handler_pkru == PKRU_INIT, "the handler's PKRU");
    check(frame_pkru == v && frame_bv, "the frame's PKRU, marked in use");
    check(rdpkru() == v, "sigreturn gives the PKRU back");
    fix_mask = PKRU_WD(k);
    raise(SIGUSR1);
    check(rdpkru() == (v & ~PKRU_WD(k)), "sigreturn takes the PKRU the handler wrote");
}

/** The rows of a tracer's PTRACE_PEEKDATA of `b`, which the child it forks
 * inherits under a key PKRU denies. */
static void peek_child_rows(char *b)
{
    pid_t pid = fork();
    int status;
    long w;

    if (pid == 0) {
        ptrace(PTRACE_TRACEME, 0, NULL, NULL);
        raise(SIGSTOP);
        _exit(0);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status)) {
        check(0, "a traced child");
        return;
    }
    errno = 0;
    w = ptrace(PTRACE_PEEKDATA, pid, b, NULL);
    check(errno == 0 && (w & 0xff) == 'y', "PTRACE_PEEKDATA of a denied page");
    ptrace(PTRACE_CONT, pid, NULL, NULL);
    waitpid(pid, &status, 0);
}

/** The `kernel` part: the copies of read and write against a denied page,
 * and the access to another address space that ignores the keys. */
static void kernel_rows(void)
{
    long k;
    char *b, c = 0;
    int fds[2], mem;
    struct iovec local, remote;
    uint32_t denied;

    if (mode != KEYS) {
        return;
    }
    k = pkey_alloc_(0, PKEY_DISABLE_ACCESS);
    b = anon(1, PROT_READ | PROT_WRITE);
    if (k < 0 || b == NULL || pipe(fds) != 0 || write(fds[1], "abcd", 4) != 4) {
        check(0, "pkey_alloc, mmap or pipe");
        return;
    }
    denied = rdpkru();
    wrpkru(denied & ~PKRU_AD(k));
    poke(b, 'x');
    wrpkru(denied);
    expect("pkey_mprotect", pkey_mprotect_(b, PAGE, PROT_READ | PROT_WRITE, k), 0, 0);
    expect("read into a denied page", read(fds[0], b, 1), 0, EFAULT);
    expect("write from a denied page", write(fds[1], b, 1), 0, EFAULT);
    wrpkru((denied & ~PKRU_AD(k)) | PKRU_WD(k));
    expect("read into a page denied writes", read(fds[0], b, 1), 0, EFAULT);
    expect("write from a page denied writes", write(fds[1], b, 1), 1, 0);
    wrpkru(denied);
    mem = open("/proc/self/mem", O_RDWR);
    check(mem >= 0, "open /proc/self/mem");
    expect("/proc/self/mem read of a denied page", pread(mem, &c, 1, (off_t)(uintptr_t)b), 1, 0);
    check(c == 'x', "/proc/self/mem reads the denied page's byte");
    expect("/proc/self/mem write of a denied page", pwrite(mem, "y", 1, (off_t)(uintptr_t)b), 1, 0);
    close(mem);
    c = 0;
    local.iov_base = &c;
    local.iov_len = 1;
    remote.iov_base = b;
    remote.iov_len = 1;
    expect("process_vm_readv of a denied page",
           process_vm_readv(getpid(), &local, 1, &remote, 1, 0), 1, 0);
    check(c == 'y', "process_vm_readv reads the denied page's byte");
    peek_child_rows(b);
}

/** The rows of the execute-only key, with the keys on. */
static void exec_key_rows(char *c)
{
    long xo = smaps_key(c), key;
    char *d;

    check(xo == 1, "PROT_EXEC alone takes the lowest free key");
    check((rdpkru() & PKRU_AD(xo)) != 0, "PKRU denies the execute-only key");
    check(((int (*)(void))(uintptr_t)c)() == 42, "an execute-only page runs");
    catch_signals();
    fix_mask = PKRU_AD(xo) | PKRU_WD(xo);
    faults = 0;
    check(peek(c) == 0xb8 - 256 || peek(c) == 0xb8, "the read after the handler opened the key");
    check(faults == 1 && fault_code == SEGV_PKUERR_CODE && fault_pkey == (unsigned)xo,
          "a read of an execute-only page is SEGV_PKUERR with its key");
    expect("mprotect to PROT_EXEC again", mprotect(c, PAGE, PROT_EXEC), 0, 0);
    check((rdpkru() & PKRU_AD(xo)) != 0, "PKRU denies the execute-only key again");
    key = pkey_alloc_(0, 0);
    check(key == 2, "pkey_alloc passes the execute-only key over");
    expect("pkey_free of the execute-only key", pkey_free_(xo), 0, EINVAL);
    expect("pkey_mprotect with the execute-only key", pkey_mprotect_(c, PAGE, PROT_EXEC, xo), 0,
           EINVAL);
    expect("mprotect to PROT_READ|PROT_EXEC", mprotect(c, PAGE, PROT_READ | PROT_EXEC), 0, 0);
    check(smaps_key(c) == 0, "PROT_READ|PROT_EXEC moves the page to key 0");
    faults = 0;
    (void)peek(c);
    check(faults == 0, "the page reads under key 0");
    d = mmap(NULL, PAGE, PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    check(d != MAP_FAILED && smaps_key(d) == xo, "mmap of PROT_EXEC alone takes the key");
}

/** The `exec-only` part: a page of PROT_EXEC alone. */
static void exec_only_rows(void)
{
    char *c = anon(1, PROT_READ | PROT_WRITE);

    if (c == NULL) {
        check(0, "mmap");
        return;
    }
    memcpy(c, code42, sizeof(code42));
    expect("mprotect to PROT_EXEC", mprotect(c, PAGE, PROT_EXEC), 0, 0);
    if (mode == KEYS) {
        exec_key_rows(c);
        return;
    }
    check(smaps_key(c) == -1, "no ProtectionKey in smaps");
    check(((int (*)(void))(uintptr_t)c)() == 42, "an execute-only page runs");
    check(peek(c) == (char)0xb8, "an execute-only page reads");
}

/** The PKRU of a new thread, for `inherit_rows`. */
static void *thread_pkru(void *arg)
{
    *(uint32_t *)arg = rdpkru();
    return NULL;
}

/** The `inherit` part: PKRU across fork, a new thread and exec. */
static void inherit_rows(void)
{
    uint32_t v, seen = 0;
    pthread_t t;
    pid_t pid;
    int status;

    if (mode != KEYS) {
        return;
    }
    if (pkey_alloc_(0, PKEY_DISABLE_WRITE) != 1) {
        check(0, "pkey_alloc");
        return;
    }
    v = rdpkru();
    pid = fork();
    if (pid == 0) {
        _exit(rdpkru() == v ? 0 : 1);
    }
    check(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          "a child of fork starts with its parent's PKRU");
    check(pthread_create(&t, NULL, thread_pkru, &seen) == 0 && pthread_join(t, NULL) == 0 &&
              seen == v,
          "a new thread starts with its creator's PKRU");
    pid = fork();
    if (pid == 0) {
        execl("/proc/self/exe", "pkeymusl", "exec-child", (char *)NULL);
        _exit(2);
    }
    check(pid > 0 && waitpid(pid, &status, 0) == pid && WIFEXITED(status) &&
              WEXITSTATUS(status) == 0,
          "an exec starts with PKRU 0x55555554 and no keys");
}

/** The program run by `inherit_rows`' exec: 0 when it started with the
 * first PKRU, key 1 not its own, and key 1 the first pkey_alloc gives. */
static int exec_child(void)
{
    return rdpkru() == PKRU_INIT && pkey_free_(1) == -1 && pkey_alloc_(0, 0) == 1 ? 0 : 1;
}

/** The tracee of `ptrace_rows`, which stops with PKRU `v` and ends with 0
 * when it runs on `want` after. */
static void ptrace_child(uint32_t v, uint32_t want)
{
    ptrace(PTRACE_TRACEME, 0, NULL, NULL);
    wrpkru(v);
    raise(SIGSTOP);
    _exit(rdpkru() == want ? 0 : 1);
}

/** The `ptrace` part: a tracer reads and writes a tracee's PKRU through
 * NT_X86_XSTATE. */
static void ptrace_rows(void)
{
    static unsigned char img[8192] __attribute__((aligned(64)));
    uint32_t v = PKRU_INIT & ~PKRU_AD(1), want = v & ~PKRU_AD(7), got;
    struct iovec iov;
    uint64_t bv;
    pid_t pid;
    int status;

    if (mode != KEYS) {
        return;
    }
    pid = fork();
    if (pid == 0) {
        ptrace_child(v, want);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid || !WIFSTOPPED(status)) {
        check(0, "a traced child");
        return;
    }
    iov.iov_base = img;
    iov.iov_len = sizeof(img);
    expect("PTRACE_GETREGSET of NT_X86_XSTATE",
           ptrace(PTRACE_GETREGSET, pid, (void *)NT_X86_XSTATE, &iov), 0, 0);
    memcpy(&got, img + pkru_off, 4);
    memcpy(&bv, img + 512, 8);
    check(iov.iov_len >= pkru_off + 8 && got == v && (bv & XFEATURE_PKRU),
          "NT_X86_XSTATE holds the tracee's PKRU, marked in use");
    memcpy(img + pkru_off, &want, 4);
    expect("PTRACE_SETREGSET of NT_X86_XSTATE",
           ptrace(PTRACE_SETREGSET, pid, (void *)NT_X86_XSTATE, &iov), 0, 0);
    ptrace(PTRACE_CONT, pid, NULL, NULL);
    check(waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "the tracee runs on the PKRU the tracer wrote");
}

/** The `proc` part: the ProtectionKey lines of smaps. */
static void proc_rows(void)
{
    int regions, keyed, keys = smaps_keys(&regions, &keyed);

    if (mode == KEYS) {
        check(regions > 0 && keys == regions && keyed == regions,
              "a ProtectionKey line before the VmFlags of each region of smaps");
    } else {
        check(regions > 0 && keys == 0, "no ProtectionKey line in smaps");
    }
}

/** Run the part `id` by `rows` in a child of fork, which reports its failed
 * checks itself, and record the part failed when the child does not end
 * with 0. */
static void part(const char *id, void (*rows)(void))
{
    char why[96];
    pid_t pid;
    int status;

    report_part(id);
    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        bad = 0;
        rows();
        fflush(stdout);
        _exit(bad != 0);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid) {
        miss("fork");
        return;
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        snprintf(why, sizeof(why), "%s: the child ended with status 0x%x", id, status);
        miss(why);
    }
}

int main(int argc, char **argv)
{
    const char *kernel = getenv("ETUDE_KERNEL");
    unsigned r[4];
    int kernel_x86_64;

    if (argc == 2 && strcmp(argv[1], "exec-child") == 0) {
        return exec_child();
    }
#if defined __x86_64__
    kernel_x86_64 = kernel == NULL || strcmp(kernel, "x86_64") == 0;
#else
    kernel_x86_64 = kernel != NULL && strcmp(kernel, "x86_64") == 0;
#endif
    cpuid(7, 0, r);
    mode = !kernel_x86_64 ? NOCALLS : ((r[2] >> 4) & 1u) ? KEYS : NOKEYS;
    if (mode == KEYS) {
        cpuid(0xD, 9, r);
        pkru_off = r[1];
    }
    probe_args(argc, argv);
    printf("pkeymusl: kernel: %s, keys: %s\n", kernel_x86_64 ? "x86_64" : "i686",
           mode == KEYS ? "on" : "off");
    fflush(stdout);
    report_begin(report_case(environ, "guest.default.pkeymusl"));
    part("cpu", cpu_rows);
    part("alloc", alloc_rows);
    part("mprotect", mprotect_rows);
    part("fault", fault_rows);
    part("signal", signal_rows);
    part("kernel", kernel_rows);
    part("exec-only", exec_only_rows);
    part("inherit", inherit_rows);
    part("ptrace", ptrace_rows);
    part("proc", proc_rows);
    report_finish();
    if (failures != 0) {
        return 1;
    }
    printf("PKEYMUSL OK\n");
    return 0;
}
