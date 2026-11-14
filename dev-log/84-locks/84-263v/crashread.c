/* crashread: read a live x86-64 process as crashpad's handler reads a crashed
 * one, through ptrace, /proc/PID/maps, /proc/PID/auxv and /proc/PID/mem, and
 * print what each read gave, for comparison between two kernels. */
#define _GNU_SOURCE
#define _FILE_OFFSET_BITS 64
#include <dirent.h>
#include <elf.h>
#ifndef AT_MINSIGSTKSZ
#define AT_MINSIGSTKSZ 51
#endif
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <signal.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ptrace.h>
#include <sys/uio.h>
#include <sys/user.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#ifndef NT_X86_XSTATE
#define NT_X86_XSTATE 0x202
#endif
#ifndef NT_386_TLS
#define NT_386_TLS 0x200
#endif
#define PAGE 4096u
#define MAXMAPS 65536
#define MAXTHREADS 1024

struct map {
    uint64_t start, end, off, ino;
    unsigned dmaj, dmin;
    char perms[5];
    char name[256];
};

static struct map *maps;
static int nmaps;
static int memfd = -1;
static pid_t pid;
static long nshort, nerr;

/** Milliseconds of the monotonic clock, for the record only. */
static double now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1e3 + ts.tv_nsec / 1e6;
}

/** Read and parse /proc/PID/maps as crashpad's MemoryMap does. */
static void read_maps(void)
{
    char path[64], *line = NULL;
    size_t cap = 0;
    ssize_t len;
    long lines = 0, bad = 0, bytes = 0;
    FILE *f;

    snprintf(path, sizeof(path), "/proc/%d/maps", (int)pid);
    f = fopen(path, "r");
    if (!f) {
        printf("maps: open errno %d\n", errno);
        return;
    }
    maps = calloc(MAXMAPS, sizeof(*maps));
    while ((len = getline(&line, &cap, f)) > 0) {
        struct map *m = &maps[nmaps];
        int n = 0;

        lines++;
        bytes += len;
        if (line[len - 1] == '\n')
            line[len - 1] = '\0';
        if (nmaps >= MAXMAPS ||
            sscanf(line, "%" SCNx64 "-%" SCNx64 " %4s %" SCNx64 " %x:%x %" SCNu64 " %n", &m->start,
                   &m->end, m->perms, &m->off, &m->dmaj, &m->dmin, &m->ino, &n) != 7 ||
            n == 0) {
            bad++;
            printf("maps-bad: %s\n", line);
            continue;
        }
        snprintf(m->name, sizeof(m->name), "%s", line + n);
        nmaps++;
    }
    free(line);
    fclose(f);
    printf("maps: lines %ld parsed %d unparsed %ld bytes %ld\n", lines, nmaps, bad, bytes);
}

/** The index of the mapping that holds `a`, or -1. */
static int find_map(uint64_t a)
{
    int i;

    for (i = 0; i < nmaps; i++)
        if (a >= maps[i].start && a < maps[i].end)
            return i;
    return -1;
}

/** Read `n` bytes at `a` as crashpad's ProcessMemory::Read does: 0, or -1
 *  after a short read (a read of 0) or an error, which are counted. */
static int read_all(uint64_t a, void *buf, size_t n)
{
    char *p = buf;

    while (n > 0) {
        ssize_t r = pread(memfd, p, n, (off_t)a);

        if (r < 0) {
            nerr++;
            return -1;
        }
        if (r == 0) {
            nshort++;
            return -1;
        }
        p += r;
        a += (uint64_t)r;
        n -= (size_t)r;
    }
    return 0;
}

/** Parse the ELF image whose header is at `base`: its program header
 *  table's address and its dynamic array's, as ElfImageReader finds them. */
static int parse_elf(uint64_t base, uint64_t *phdr_at, uint64_t *dyn_at)
{
    Elf64_Ehdr eh;
    Elf64_Phdr ph[64];
    uint64_t lo = UINT64_MAX, bias;
    int i, dyn = -1;

    if (read_all(base, &eh, sizeof(eh)) != 0 || memcmp(eh.e_ident, ELFMAG, SELFMAG) != 0 ||
        eh.e_ident[EI_CLASS] != ELFCLASS64 || eh.e_phnum == 0 || eh.e_phnum > 64 ||
        eh.e_phentsize != sizeof(Elf64_Phdr))
        return -1;
    if (read_all(base + eh.e_phoff, ph, eh.e_phnum * sizeof(ph[0])) != 0)
        return -1;
    for (i = 0; i < eh.e_phnum; i++) {
        if (ph[i].p_type == PT_LOAD && ph[i].p_vaddr < lo)
            lo = ph[i].p_vaddr & ~(uint64_t)(PAGE - 1);
        if (ph[i].p_type == PT_DYNAMIC)
            dyn = i;
    }
    if (lo == UINT64_MAX)
        return -1;
    bias = base - lo;
    *phdr_at = base + eh.e_phoff;
    *dyn_at = dyn < 0 ? 0 : bias + ph[dyn].p_vaddr;
    return 0;
}

/** Find the image that a mapping belongs to, as crashpad's
 *  FindFilePossibleMmapStarts and its caller do: the mappings of the same
 *  file at offset 0 up to `idx`, last first, or `idx` alone for an anonymous
 *  one, whose image has `want` as its dynamic array (or, with `phdr`, its
 *  program header table). Returns the start's index, or -1; `starts` gets
 *  the number of candidates. */
static int find_image(int idx, uint64_t want, int phdr, int *starts)
{
    int i;

    *starts = 0;
    for (i = idx; i >= 0; i--) {
        uint64_t p, d;

        if (maps[idx].dmaj == 0 && maps[idx].dmin == 0 && maps[idx].ino == 0) {
            if (i != idx)
                break;
        } else if (maps[i].dmaj != maps[idx].dmaj || maps[i].dmin != maps[idx].dmin ||
                   maps[i].ino != maps[idx].ino || maps[i].off != 0) {
            continue;
        }
        (*starts)++;
        if (parse_elf(maps[i].start, &p, &d) == 0 && (phdr ? p : d) == want)
            return i;
    }
    return -1;
}

/** Read the auxiliary vector, then the executable and every module of the
 *  dynamic linker's list, as ProcessReaderLinux::InitializeModules does. */
static void read_modules(void)
{
    char path[64], name[512];
    uint64_t av[2 * 64], phdr = 0, base = 0, ehdr = 0, dyn = 0, dbg = 0, lm;
    Elf64_Dyn d[512];
    struct {
        int32_t version, pad;
        uint64_t map, brk;
        int32_t state, pad2;
        uint64_t ldbase;
    } rd;
    struct {
        uint64_t addr, name, ld, next, prev;
    } e;
    long found = 0, nomap = 0, nomodule = 0, entries = 0;
    int fd, i, n, idx, starts, ex;
    ssize_t r;

    snprintf(path, sizeof(path), "/proc/%d/auxv", (int)pid);
    fd = open(path, O_RDONLY);
    r = fd < 0 ? -1 : read(fd, av, sizeof(av));
    if (fd >= 0)
        close(fd);
    printf("auxv: bytes %zd\n", r);
    for (i = 0; r > 0 && i < r / 16; i++) {
        if (av[2 * i] == AT_PHDR)
            phdr = av[2 * i + 1];
        if (av[2 * i] == AT_BASE)
            base = av[2 * i + 1];
        if (av[2 * i] == AT_SYSINFO_EHDR)
            ehdr = av[2 * i + 1];
    }
    printf("auxv: types");
    for (i = 0; r > 0 && i < r / 16; i++)
        printf(" %" PRIu64, av[2 * i]);
    printf("\n");
    for (i = 0; r > 0 && i < r / 16; i++)
        if (av[2 * i] == AT_HWCAP || av[2 * i] == AT_HWCAP2 || av[2 * i] == AT_FLAGS ||
            av[2 * i] == AT_PAGESZ || av[2 * i] == AT_CLKTCK || av[2 * i] == AT_MINSIGSTKSZ ||
            av[2 * i] == 27 || av[2 * i] == 28)
            printf("auxv: type %" PRIu64 " value %#" PRIx64 "\n", av[2 * i], av[2 * i + 1]);
    printf("auxv: phdr %s base %s sysinfo_ehdr %s\n", phdr ? "set" : "none", base ? "set" : "none",
           ehdr ? "set" : "none");
    if (ehdr) {
        uint64_t p, dd;

        idx = find_map(ehdr);
        printf("vdso: map %s parse %s\n", idx < 0 ? "none" : maps[idx].name,
               parse_elf(ehdr, &p, &dd) == 0 ? "ok" : "fail");
    }
    idx = find_map(phdr);
    if (idx < 0) {
        printf("exe: no mapping holds the program headers\n");
        return;
    }
    ex = find_image(idx, phdr, 1, &starts);
    printf("exe: %s starts %d %s\n", ex < 0 ? "noexe" : "found", starts, maps[idx].name);
    if (ex < 0)
        return;
    {
        uint64_t p;

        parse_elf(maps[ex].start, &p, &dyn);
    }
    for (n = 0; dyn && n < 512; n++) {
        if (read_all(dyn + n * sizeof(d[0]), &d[n], sizeof(d[0])) != 0) {
            printf("exe: dynamic array unreadable at entry %d\n", n);
            return;
        }
        if (d[n].d_tag == DT_NULL)
            break;
        if (d[n].d_tag == DT_DEBUG)
            dbg = d[n].d_un.d_ptr;
    }
    printf("exe: dynamic entries %d debug %s\n", n, dbg ? "set" : "none");
    if (!dbg || read_all(dbg, &rd, sizeof(rd)) != 0) {
        printf("exe: r_debug unreadable\n");
        return;
    }
    printf("r_debug: version %d state %d\n", rd.version, rd.state);
    for (lm = rd.map; lm && entries < 2000; lm = e.next) {
        if (read_all(lm, &e, sizeof(e)) != 0) {
            printf("module: link_map unreadable\n");
            break;
        }
        name[0] = '\0';
        if (e.name)
            for (i = 0; i < (int)sizeof(name) - 1; i++) {
                if (read_all(e.name + i, &name[i], 1) != 0) {
                    name[i] = '\0';
                    break;
                }
                if (name[i] == '\0')
                    break;
            }
        name[sizeof(name) - 1] = '\0';
        if (entries++ == 0)
            continue; /* the executable, which crashpad takes from AT_PHDR */
        idx = find_map(e.ld);
        if (idx < 0) {
            nomap++;
            printf("module: nomap %s\n", name);
            continue;
        }
        if (find_image(idx, e.ld, 0, &starts) < 0) {
            nomodule++;
            printf("module: nomodule starts %d %s map %s\n", starts, name, maps[idx].name);
            continue;
        }
        found++;
        printf("module: found starts %d %s\n", starts, name[0] ? name : maps[idx].name);
    }
    printf("modules: entries %ld found %ld nomap %ld nomodule %ld\n", entries, found, nomap,
           nomodule);
}

/** The kind of a mapping for the summary: anon, file, or its bracketed name. */
static const char *kind(const struct map *m)
{
    if (m->name[0] == '[')
        return m->name;
    if (m->name[0] == '\0')
        return "anon";
    if (strstr(m->name, "(deleted)"))
        return "deleted";
    if (strncmp(m->name, "/memfd:", 7) == 0)
        return "memfd";
    if (strncmp(m->name, "/dev/", 5) == 0)
        return m->name;
    return "file";
}

/** Read each mapping a page at a time, every page of a small one and a
 *  sample of a large one, through /proc/PID/mem, and once across its end;
 *  print a line for each mapping and a sum for each kind and permission. */
static void read_memory(void)
{
    static char buf[2 * PAGE];
    struct sum {
        char key[300];
        long maps, pages, full, shorts, partial, err, errnos[4];
    } *s = calloc(512, sizeof(*s));
    int ns = 0, i, j;
    double t0 = now_ms();
    long total = 0;

    for (i = 0; i < nmaps; i++) {
        const struct map *m = &maps[i];
        uint64_t pages = (m->end - m->start) / PAGE, k, pg;
        long full = 0, shorts = 0, partial = 0, err = 0, last_errno = 0, sampled = 0;
        ssize_t cross;
        char key[300];
        struct sum *t;

        for (k = 0; k < pages; k++) {
            ssize_t r;

            if (pages > 512 && k >= 128 && k < pages - 128) {
                /* 256 more pages evenly between the first and last 128 */
                uint64_t step = (pages - 256) / 256 + 1;

                if ((k - 128) % step != 0)
                    continue;
            }
            pg = m->start + k * PAGE;
            sampled++;
            r = pread(memfd, buf, PAGE, (off_t)pg);
            if (r == PAGE) {
                full++;
                total += r;
            } else if (r == 0) {
                shorts++;
            } else if (r > 0) {
                partial++;
            } else {
                err++;
                last_errno = errno;
            }
        }
        cross = pread(memfd, buf, 2 * PAGE, (off_t)(m->end - PAGE));
        printf("mem: %" PRIx64 "-%" PRIx64 " %s %s pages %" PRIu64 " sampled %ld full %ld short %ld "
               "partial %ld err %ld errno %ld cross %zd/%d next %s\n",
               m->start, m->end, m->perms, m->name[0] ? m->name : "-", pages, sampled, full, shorts,
               partial, err, last_errno, cross, cross < 0 ? errno : 0,
               i + 1 < nmaps && maps[i + 1].start == m->end ? "adjacent" : "gap");
        snprintf(key, sizeof(key), "%s %s", m->perms, kind(m));
        for (j = 0; j < ns; j++)
            if (strcmp(s[j].key, key) == 0)
                break;
        if (j == ns && ns < 512)
            snprintf(s[ns++].key, sizeof(s[0].key), "%s", key);
        if (j >= 512)
            continue;
        t = &s[j];
        t->maps++;
        t->pages += sampled;
        t->full += full;
        t->shorts += shorts;
        t->partial += partial;
        t->err += err;
        if (err && last_errno == EIO)
            t->errnos[0]++;
        else if (err && last_errno == EFAULT)
            t->errnos[1]++;
        else if (err)
            t->errnos[2]++;
    }
    for (j = 0; j < ns; j++)
        printf("mem-sum: %s maps %ld pages %ld full %ld short %ld partial %ld err %ld "
               "(mappings with EIO %ld EFAULT %ld other %ld)\n",
               s[j].key, s[j].maps, s[j].pages, s[j].full, s[j].shorts, s[j].partial, s[j].err,
               s[j].errnos[0], s[j].errnos[1], s[j].errnos[2]);
    printf("mem-time: %ld bytes in %.0f ms\n", total, now_ms() - t0);
    free(s);
}

/** One register set of `tid` by PTRACE_GETREGSET into a buffer of `len`
 *  bytes: the length the kernel gave back, or -errno. */
static long regset(pid_t tid, int type, void *buf, size_t len)
{
    struct iovec iov;

    iov.iov_base = buf;
    iov.iov_len = len;
    if (ptrace(PTRACE_GETREGSET, tid, (void *)(long)type, &iov) != 0)
        return -errno;
    return (long)iov.iov_len;
}

/** The register reads crashpad's Ptracer makes of `tid`, and the older
 *  requests beside them, compared where they read the same registers. */
static void read_thread(pid_t tid)
{
    struct user_regs_struct rs, rg;
    static char big[16384];
    long gp, gp_big, fp, xs, tls, peek[8];
    int gr, gf, i, dr = 0, fs;
    unsigned long fsbase = 0;
    siginfo_t si;
    struct user_fpregs_struct fpr;
    int sinfo;

    memset(&rs, 0, sizeof(rs));
    memset(&rg, 0, sizeof(rg));
    gp = regset(tid, NT_PRSTATUS, &rs, sizeof(rs));
    gp_big = regset(tid, NT_PRSTATUS, big, sizeof(big));
    fp = regset(tid, NT_PRFPREG, big, 512);
    xs = regset(tid, NT_X86_XSTATE, big, sizeof(big));
    tls = regset(tid, NT_386_TLS, big, sizeof(big));
    gr = ptrace(PTRACE_GETREGS, tid, NULL, &rg) == 0 ? 0 : -errno;
    gf = ptrace(PTRACE_GETFPREGS, tid, NULL, &fpr) == 0 ? 0 : -errno;
    for (i = 0; i < 8; i++) {
        errno = 0;
        peek[i] = ptrace(PTRACE_PEEKUSER, tid,
                         (void *)(offsetof(struct user, u_debugreg) + i * sizeof(long)), NULL);
        if (errno)
            dr = -errno;
    }
    fs = ptrace(PTRACE_ARCH_PRCTL, tid, &fsbase, (void *)0x1003 /* ARCH_GET_FS */) == 0 ? 0 : -errno;
    if (tid == pid)
        printf("debugregs: %lx %lx %lx %lx %lx %lx %lx %lx\n", (unsigned long)peek[0],
               (unsigned long)peek[1], (unsigned long)peek[2], (unsigned long)peek[3],
               (unsigned long)peek[4], (unsigned long)peek[5], (unsigned long)peek[6],
               (unsigned long)peek[7]);
    sinfo = ptrace(PTRACE_GETSIGINFO, tid, NULL, &si) == 0 ? si.si_signo : -errno;
    printf("regs: tid-%s prstatus %ld prstatus-big %ld prfpreg %ld xstate %ld tls %ld getregs %d "
           "same %s getfpregs %d debugregs %d dr7 %lx arch_prctl %d fs-same %s siginfo %d cs %llx "
           "ss %llx rip %s rsp %s fs_base %s\n",
           tid == pid ? "main" : "other", gp, gp_big, fp, xs, tls, gr,
           memcmp(&rs, &rg, sizeof(rs)) == 0 ? "yes" : "no", gf, dr, (unsigned long)peek[7], fs,
           fsbase == rs.fs_base ? "yes" : "no", sinfo, rs.cs, rs.ss,
           find_map(rs.rip) >= 0 && strchr(maps[find_map(rs.rip)].perms, 'x') ? "in-exec" : "OUT",
           find_map(rs.rsp) >= 0 ? "in-map" : "OUT", find_map(rs.fs_base) >= 0 ? "in-map" : "OUT");
    if (find_map(rs.rsp) >= 0) {
        /* crashpad captures the stack from the pointer, less the red zone,
         * to the end of its mapping, at most 64 KiB here */
        uint64_t from = rs.rsp - 128, to = maps[find_map(rs.rsp)].end;

        if (to - from > 65536)
            to = from + 65536;
        printf("stack: tid-%s bytes %" PRIu64 " read %s\n", tid == pid ? "main" : "other",
               to - from, read_all(from, big, (size_t)(to - from)) == 0 ? "ok" : "FAIL");
    }
}

int main(int argc, char **argv)
{
    pid_t tids[MAXTHREADS];
    int nt = 0, i, st;
    char path[64];
    struct dirent *de;
    DIR *d;
    double t0;

    if (argc == 3 && strcmp(argv[1], "-f") == 0) {
        struct user_regs_struct r;

        /* Fault the thread `pid` as a wild jump would: an instruction fetch
         * at an address no region holds, a SIGSEGV of SEGV_MAPERR. */
        pid = (pid_t)atoi(argv[2]);
        if (ptrace(PTRACE_ATTACH, pid, NULL, NULL) != 0 || waitpid(pid, &st, __WALL) != pid ||
            ptrace(PTRACE_GETREGS, pid, NULL, &r) != 0) {
            printf("fault: attach errno %d\n", errno);
            return 1;
        }
        r.rip = 0x10;
        r.orig_rax = (unsigned long long)-1;
        if (ptrace(PTRACE_SETREGS, pid, NULL, &r) != 0 || ptrace(PTRACE_DETACH, pid, NULL, NULL) != 0) {
            printf("fault: errno %d\n", errno);
            return 1;
        }
        printf("fault: sent\n");
        return 0;
    }
    if (argc != 2) {
        fprintf(stderr, "usage: crashread [-f] PID\n");
        return 2;
    }
    pid = (pid_t)atoi(argv[1]);
    snprintf(path, sizeof(path), "/proc/%d/task", (int)pid);
    d = opendir(path);
    if (!d) {
        printf("task: open errno %d\n", errno);
        return 1;
    }
    while ((de = readdir(d)) && nt < MAXTHREADS)
        if (de->d_name[0] != '.')
            tids[nt++] = (pid_t)atoi(de->d_name);
    closedir(d);
    /* the thread group's leader first, as crashpad attaches it first */
    for (i = 0; i < nt; i++)
        if (tids[i] == pid) {
            tids[i] = tids[0];
            tids[0] = pid;
        }
    t0 = now_ms();
    for (i = 0; i < nt; i++) {
        long rc = ptrace(PTRACE_ATTACH, tids[i], NULL, NULL);
        int e = rc ? errno : 0;
        pid_t w = rc ? 0 : waitpid(tids[i], &st, __WALL);

        if (rc || w != tids[i] || !WIFSTOPPED(st) || i == 0)
            printf("attach: tid-%s rc %ld errno %d wait %s status %#x\n", i ? "other" : "main", rc,
                   e, w == tids[i] ? "ok" : "fail", rc ? 0 : st);
        if (rc)
            tids[i] = -1;
    }
    printf("attach: threads %d in %.0f ms\n", nt, now_ms() - t0);
    snprintf(path, sizeof(path), "/proc/%d/mem", (int)pid);
    memfd = open(path, O_RDONLY | O_CLOEXEC);
    printf("mem: open %s errno %d\n", memfd < 0 ? "fail" : "ok", memfd < 0 ? errno : 0);
    t0 = now_ms();
    read_maps();
    printf("maps-time: %.0f ms\n", now_ms() - t0);
    if (memfd >= 0) {
        t0 = now_ms();
        read_modules();
        printf("modules-time: %.0f ms shortreads %ld errors %ld\n", now_ms() - t0, nshort, nerr);
    }
    t0 = now_ms();
    for (i = 0; i < nt; i++)
        if (tids[i] > 0)
            read_thread(tids[i]);
    printf("regs-time: %d threads in %.0f ms\n", nt, now_ms() - t0);
    if (memfd >= 0)
        read_memory();
    for (i = 0; i < nt; i++)
        if (tids[i] > 0 && ptrace(PTRACE_DETACH, tids[i], NULL, NULL) != 0)
            printf("detach: errno %d\n", errno);
    return 0;
}
