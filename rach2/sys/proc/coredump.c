/*
 * coredump.c: the core file of a process a signal ends, as coredump.h
 * describes it.
 */

#include "coredump.h"

#include "elf.h"
#include "../fpu.h"
#include "process.h"
#include "tgroup.h"
#include "wait_queue.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/mm.h"
#include "../mm/paging.h"
#include "../mm/vm.h"
#include "../timer.h"
#include "../vdso.h"

/* The notes every architecture's core file has; the floating-point notes
 * are the architecture's (`arch_core_fp_notes`). */
#define NT_PRSTATUS 1u
#define NT_PRPSINFO 3u
#define NT_AUXV     6u

/*
 * Linux's `struct elf_prstatus` and `struct elf_prpsinfo`, declared with
 * their C types, so that the compiler gives them the layout of the
 * processor's ABI, as it gives a program's: 144 and 124 bytes on i686, 336
 * and 136 on x86-64.
 */
struct core_prstatus {
    int si_signo;
    int si_code;
    int si_errno;
    short cursig;
    unsigned long sigpend;
    unsigned long sighold;
    int pid;
    int ppid;
    int pgrp;
    int sid;
    long utime[2];
    long stime[2];
    long cutime[2];
    long cstime[2];
    unsigned long regs[ARCH_GREGS];
    int fpvalid;
};

struct core_prpsinfo {
    char state;
    char sname;
    char zomb;
    char nice;
    unsigned long flag;
    core_uid_t uid;
    core_uid_t gid;
    int pid;
    int ppid;
    int pgrp;
    int sid;
    char fname[16];
    char psargs[80];
};

#if ELF_IA32
/*
 * The i386 forms of the two structures, which an i386 program's core file
 * on x86-64 has, as Linux's compat_elf_prstatus and compat_elf_prpsinfo:
 * 144 and 124 bytes, the same as i686's.
 */
struct core_prstatus32 {
    int32_t si_signo;
    int32_t si_code;
    int32_t si_errno;
    int16_t cursig;
    uint32_t sigpend;
    uint32_t sighold;
    int32_t pid;
    int32_t ppid;
    int32_t pgrp;
    int32_t sid;
    int32_t utime[2];
    int32_t stime[2];
    int32_t cutime[2];
    int32_t cstime[2];
    uint32_t regs[IA32_GREGS];
    int32_t fpvalid;
};

struct core_prpsinfo32 {
    char state;
    char sname;
    char zomb;
    char nice;
    uint32_t flag;
    uint16_t uid;
    uint16_t gid;
    int32_t pid;
    int32_t ppid;
    int32_t pgrp;
    int32_t sid;
    char fname[16];
    char psargs[80];
};

typedef char core_prstatus32_size[sizeof(struct core_prstatus32) == 144u ? 1 : -1];
typedef char core_prpsinfo32_size[sizeof(struct core_prpsinfo32) == 124u ? 1 : -1];
#endif

/*
 * The ELF header, a program header and a section header of a core file's
 * class, and the offsets of the fields this file sets: the class of the
 * kernel's programs, or ELF32 for an i386 program's on x86-64. A file of
 * PN_XNUM program headers or more has PN_XNUM as the header's count, and
 * the count itself in the one section header it then has: ELF's extended
 * numbering, which Linux's core files use.
 */
struct core_elf {
    uint8_t cls;
    uint16_t machine;
    uint32_t ehdr_size, phdr_size, shdr_size;
    uint32_t e_shoff, e_ehsize, e_phoff;
    uint32_t p_flags, p_offset, p_vaddr, p_filesz, p_memsz, p_align;
    uint32_t s_size, s_info;
};

#define CORE_ELF32(machine) \
    {ELFCLASS32, machine, 52, 32, 40, 32, 40, 28, 24, 4, 8, 16, 20, 28, 20, 28}
#define CORE_ELF64(machine) \
    {ELFCLASS64, machine, 64, 56, 64, 40, 52, 32, 4, 8, 16, 32, 40, 48, 32, 44}
#define CORE_HDR_MAX 64u /* the largest of the three headers */

#if ELF_CLASS == ELFCLASS64
static const struct core_elf core_native = CORE_ELF64(ELF_MACHINE);
#else
static const struct core_elf core_native = CORE_ELF32(ELF_MACHINE);
#endif
#if ELF_IA32
static const struct core_elf core_ia32 = CORE_ELF32(EM_386);
#endif

#define E_PHENTSIZE(f) ((f)->e_ehsize + 2u)
#define E_PHNUM(f)     ((f)->e_ehsize + 4u)
#define E_SHENTSIZE(f) ((f)->e_ehsize + 6u)
#define E_SHNUM(f)     ((f)->e_ehsize + 8u)
#define PN_XNUM        0xFFFFu

#if ELF_IA32
/** Whether the current process is an i386 program on x86-64, whose core
 * file is ELF32 with i386's notes. */
static int core_compat(void)
{
    return current_proc->mm->compat != 0;
}
#endif

/** The headers of the current process's core file. */
static const struct core_elf *core_elf(void)
{
#if ELF_IA32
    if (core_compat()) {
        return &core_ia32;
    }
#endif
    return &core_native;
}

/* What is being written and how much may be. */
struct core_out {
    core_write_fn write;
    void *ctx;
    uint64_t done; /* bytes written, holes not counted */
    uint64_t limit;
    int32_t err; /* the first error, after which nothing is written */
};

/* What the file has of a region: its range and protection, whether it
 * maps a file, and whether its pages are all holes, taken once before
 * anything is written. */
struct core_region {
    uintptr_t start;
    uintptr_t end;
    uint32_t prot;
    int of_file;
    int holes; /* the vDSO's data page, [vvar] */
};

/** Put `v` at `p` little-endian, the processor's order and ELFDATA2LSB. */
static void put16(uint8_t *p, uint16_t v)
{
    k_memcpy(p, &v, sizeof(v));
}

/** Put `v` at `p` as `put16` does. */
static void put32(uint8_t *p, uint32_t v)
{
    k_memcpy(p, &v, sizeof(v));
}

/** Put an address, an offset or a size of the class of `f`: 32 or 64
 * bits. */
static void put_word(const struct core_elf *f, uint8_t *p, uintptr_t v)
{
    uint64_t w = v;

    if (f->cls == ELFCLASS64) {
        k_memcpy(p, &w, sizeof(w));
    } else {
        put32(p, (uint32_t)v);
    }
}

/** Write `len` bytes of `buf`, or a hole of `len` zeros when `buf` is null,
 * unless an earlier write failed or the bytes pass the limit, which a hole
 * does not count toward; 0, or -1 with `err` set. */
static int out(struct core_out *o, const void *buf, uint32_t len)
{
    int32_t rc;

    if (o->err != 0) {
        return -1;
    }
    if (buf && len > o->limit - o->done) {
        o->err = -EFBIG;
        return -1;
    }
    rc = o->write(o->ctx, buf, len);
    if (rc < 0) {
        o->err = rc;
        return -1;
    }
    if (buf) {
        o->done += len;
    }
    return 0;
}

uint32_t core_note_bytes(uint32_t descsz)
{
    return 12u + 8u + ((descsz + 3u) & ~3u);
}

void core_note_put(uint8_t **p, const char *name, uint32_t type, const void *desc, uint32_t descsz)
{
    uint8_t *q = *p;
    uint32_t namesz = (uint32_t)k_strlen(name) + 1u;

    put32(q, namesz);
    put32(q + 4, descsz);
    put32(q + 8, type);
    k_memset(q + 12, 0, 8);
    k_memcpy(q + 12, name, namesz);
    k_memcpy(q + 20, desc, descsz);
    k_memset(q + 20 + descsz, 0, ((descsz + 3u) & ~3u) - descsz);
    *p = q + core_note_bytes(descsz);
}

/** Put a time in ticks as the timeval of an elf_prstatus. */
static void put_timeval(long *tv, uint64_t ticks)
{
    uint32_t rest;

    tv[0] = (long)tick_div(ticks, TICK_HZ, &rest);
    tv[1] = (long)(rest * TICK_US);
}

/**
 * Fill the `elf_prstatus` of thread `t`, with `signo` the signal that ends
 * the process: the signal, the pending and blocked sets, the ids, the times,
 * the registers in `user_regs_struct`'s order, and a valid floating-point
 * mark.
 */
static void prstatus_fill(struct core_prstatus *d, const process_t *t, int signo)
{
    k_memset(d, 0, sizeof(*d));
    d->si_signo = signo;
    d->cursig = (short)signo;
    d->sigpend = (unsigned long)t->sig_pending;
    d->sighold = (unsigned long)t->sig_blocked;
    d->pid = (int)t->pid;
    d->ppid = (int)t->parent_pid;
    d->pgrp = (int)t->pgid;
    d->sid = (int)t->sid;
    put_timeval(d->utime, t->utime);
    put_timeval(d->stime, t->stime);
    put_timeval(d->cutime, t->tg->cutime);
    put_timeval(d->cstime, t->tg->cstime);
    arch_regs_get(t, d->regs);
    d->fpvalid = 1;
}

/**
 * Fill the `elf_prpsinfo` of the current process: its state, nice value,
 * ids, program name, and the start of its arguments, NULs made spaces, as
 * `ps` shows them, read from its stack.
 */
static void prpsinfo_fill(struct core_prpsinfo *d)
{
    const process_t *p = current_proc;
    struct mm *mm = p->mm;
    uintptr_t len = mm->arg_end - mm->arg_start;
    uintptr_t i;

    k_memset(d, 0, sizeof(*d));
    d->sname = 'R';
    d->nice = (char)p->nice;
    d->uid = (core_uid_t)p->ruid;
    d->gid = (core_uid_t)p->rgid;
    d->pid = (int)p->tgid;
    d->ppid = (int)p->parent_pid;
    d->pgrp = (int)p->pgid;
    d->sid = (int)p->sid;
    k_memcpy(d->fname, p->comm, sizeof(d->fname));
    if (len > sizeof(d->psargs) - 1u) {
        len = sizeof(d->psargs) - 1u;
    }
    if (mm->arg_end > mm->arg_start &&
        paging_access(mm, mm->arg_start, d->psargs, (uint32_t)len, 0) == 0) {
        for (i = 0; i + 1u < len; i++) {
            if (d->psargs[i] == 0) {
                d->psargs[i] = ' ';
            }
        }
    }
    d->psargs[sizeof(d->psargs) - 1u] = 0;
}

#if ELF_IA32
/** Put the NT_PRSTATUS of thread `t` of an i386 program, with `signo` the
 * signal that ends the process, in i386's form: `prstatus_fill`'s, its
 * words cut to 32 bits, with the registers of `ia32_regs_get`. */
static void prstatus32_put(uint8_t **p, const process_t *t, int signo)
{
    struct core_prstatus n;
    struct core_prstatus32 d;
    int i;

    prstatus_fill(&n, t, signo);
    k_memset(&d, 0, sizeof(d));
    d.si_signo = n.si_signo;
    d.cursig = n.cursig;
    d.sigpend = (uint32_t)n.sigpend;
    d.sighold = (uint32_t)n.sighold;
    d.pid = n.pid;
    d.ppid = n.ppid;
    d.pgrp = n.pgrp;
    d.sid = n.sid;
    for (i = 0; i < 2; i++) {
        d.utime[i] = (int32_t)n.utime[i];
        d.stime[i] = (int32_t)n.stime[i];
        d.cutime[i] = (int32_t)n.cutime[i];
        d.cstime[i] = (int32_t)n.cstime[i];
    }
    ia32_regs_get(t, d.regs);
    d.fpvalid = n.fpvalid;
    core_note_put(p, "CORE", NT_PRSTATUS, &d, sizeof(d));
}

/** Put the NT_PRPSINFO of the current process, an i386 program, in i386's
 * form, with 16-bit ids as Linux's compat_elf_prpsinfo has them. */
static void prpsinfo32_put(uint8_t **p)
{
    struct core_prpsinfo n;
    struct core_prpsinfo32 d;

    prpsinfo_fill(&n);
    k_memset(&d, 0, sizeof(d));
    d.state = n.state;
    d.sname = n.sname;
    d.zomb = n.zomb;
    d.nice = n.nice;
    d.flag = (uint32_t)n.flag;
    d.uid = (uint16_t)n.uid;
    d.gid = (uint16_t)n.gid;
    d.pid = n.pid;
    d.ppid = n.ppid;
    d.pgrp = n.pgrp;
    d.sid = n.sid;
    k_memcpy(d.fname, n.fname, sizeof(d.fname));
    k_memcpy(d.psargs, n.psargs, sizeof(d.psargs));
    core_note_put(p, "CORE", NT_PRPSINFO, &d, sizeof(d));
}

/** The bytes of an i386 program's auxiliary vector, packed in 4-byte words
 * in its address space's copy, through its AT_NULL pair, as Linux's
 * `fill_auxv_note` writes it. */
static uint32_t auxv32_bytes(const struct mm *mm)
{
    const uint32_t *w = (const uint32_t *)mm->auxv;
    uint32_t n = (uint32_t)sizeof(mm->auxv) / 8u;
    uint32_t i;

    for (i = 0; i < n; i++) {
        if (w[2u * i] == 0) {
            return (i + 1u) * 2u * (uint32_t)sizeof(uint32_t);
        }
    }
    return n * 2u * (uint32_t)sizeof(uint32_t);
}
#endif

/** Put the NT_PRSTATUS of thread `t`, in the form of the core file's class. */
static void prstatus_put(uint8_t **p, const process_t *t, int signo)
{
    struct core_prstatus status;

#if ELF_IA32
    if (core_compat()) {
        prstatus32_put(p, t, signo);
        return;
    }
#endif
    prstatus_fill(&status, t, signo);
    core_note_put(p, "CORE", NT_PRSTATUS, &status, sizeof(status));
}

/** Put the floating-point notes of thread `t`, and for an i386 program its
 * TLS note. */
static void fp_notes_put(uint8_t **p, const process_t *t)
{
#if ELF_IA32
    if (core_compat()) {
        ia32_core_fp_notes(p, t);
        return;
    }
#endif
    arch_core_fp_notes(p, t);
}

/** Whether `t` is a thread of the dumping process the core describes: the
 * dumping thread and every other thread of its process that has not ended,
 * each of which has a frame on its kernel stack. */
static int core_thread(const process_t *t)
{
    return t->tg == current_proc->tg && t->state != PROC_UNUSED &&
           (t->state != PROC_ZOMBIE || t == current_proc) && t->kernel_stack != 0;
}

/** Whether every other thread of the process `arg` the core describes has
 * parked, or an execve of one of them waits for the rest to end. */
static int core_parked_all(void *arg)
{
    const struct tgroup *tg = (const struct tgroup *)arg;
    const process_t *t;

    if (tg->exec_task) {
        return 1;
    }
    for (t = proc_list; t; t = t->next) {
        if (t != current_proc && core_thread(t) && !t->core_parked) {
            return 0;
        }
    }
    return 1;
}

/** Whether the process `arg` has written or given up its core. */
static int core_done(void *arg)
{
    return !((const struct tgroup *)arg)->core_dumping;
}

void core_park(void)
{
    struct tgroup *tg = current_proc->tg;

    fpu_save(current_proc->fpu);
    current_proc->core_parked = 1;
    wq_wake_all(&tg->gone);
    (void)wq_wait_event_nointr(&tg->gone, core_done, tg);
    current_proc->core_parked = 0;
}

void core_release(void)
{
    current_proc->tg->core_dumping = 0;
    wq_wake_all(&current_proc->tg->gone);
}

/** The notes are in Linux's order: the dumping thread's status, the
 * process's information, the auxiliary vector and the thread's
 * floating-point state, which is in the unit's registers until it is saved
 * here, then each other thread's status and the floating-point state it
 * saved when it parked. An i386 program's are in i386's forms, its vector
 * in 4-byte words, with a TLS note after each thread's floating-point
 * ones. */
uint8_t *core_notes(int signo, uint32_t *size)
{
    struct core_prpsinfo info;
    uint32_t auxv_size = (uint32_t)sizeof(current_proc->mm->auxv);
    uint32_t threads = 0;
    uint32_t each = core_note_bytes(sizeof(struct core_prstatus)) + arch_core_fp_size();
    uint32_t fixed;
    uint8_t *buf;
    uint8_t *p;
    process_t *t;

#if ELF_IA32
    if (core_compat()) {
        auxv_size = auxv32_bytes(current_proc->mm);
        each = core_note_bytes(sizeof(struct core_prstatus32)) + ia32_core_fp_size();
    }
#endif
    fixed = core_note_bytes(sizeof(info)) + core_note_bytes(auxv_size);

    (void)wq_wait_event_nointr(&current_proc->tg->gone, core_parked_all, current_proc->tg);
    for (t = proc_list; t; t = t->next) {
        if (core_thread(t)) {
            threads++;
        }
    }
    if (threads > (0xFFFFFFFFu - fixed) / each) {
        return NULL;
    }
    *size = threads * each + fixed;
    buf = kmalloc(*size);
    if (!buf) {
        return NULL;
    }
    p = buf;
    prstatus_put(&p, current_proc, signo);
#if ELF_IA32
    if (core_compat()) {
        prpsinfo32_put(&p);
    } else
#endif
    {
        prpsinfo_fill(&info);
        core_note_put(&p, "CORE", NT_PRPSINFO, &info, sizeof(info));
    }
    core_note_put(&p, "CORE", NT_AUXV, current_proc->mm->auxv, auxv_size);
    fpu_save(current_proc->fpu);
    fp_notes_put(&p, current_proc);
    for (t = proc_list; t; t = t->next) {
        if (t == current_proc || !core_thread(t)) {
            continue;
        }
        prstatus_put(&p, t, signo);
        fp_notes_put(&p, t);
    }
    *size = (uint32_t)(p - buf);
    return buf;
}

/** The bytes of region `r` the file has: all of a readable region, none of
 * one that is not, as Linux leaves a PROT_NONE reservation out, and none of
 * device memory or of memfd_secret's, which Linux leaves out too. */
static uintptr_t region_file_size(const struct core_region *r)
{
    return (r->prot & VM_PROT_READ) && !(r->prot & (VM_PROT_IO | VM_PROT_SECRET))
               ? r->end - r->start
               : 0u;
}

/**
 * Write the pages of region `r`: a page the process has, read through its
 * address space; a page of a file it never touched, brought in from the
 * file as a read of /proc/PID/mem brings it, as Linux's dump brings it,
 * since a debugger reads the file's bytes from the core, among them the
 * version of libc that libthread_db checks; and a hole for a page of
 * anonymous memory it never touched. Each page is looked up in the address
 * space as it is read, so a page another process sharing the space has
 * unmapped since the regions were taken is a hole. The vDSO's data page is
 * a hole, as Linux leaves it, since Linux's dump cannot take a page of that
 * mapping; it holds the kernel's clock, which says nothing of the process.
 */
static void region_write(struct core_out *o, struct mm *mm, const struct core_region *r)
{
    uint8_t *page = kmalloc(PAGE_SIZE);
    uintptr_t addr;

    if (!page) {
        o->err = -ENOMEM;
        return;
    }
    for (addr = r->start; addr < r->end && o->err == 0; addr += PAGE_SIZE) {
        if (r->holes || (!r->of_file && paging_user_phys(mm->dir, addr) == 0) ||
            paging_access(mm, addr, page, PAGE_SIZE, 0) != 0) {
            (void)out(o, NULL, PAGE_SIZE);
        } else {
            (void)out(o, page, PAGE_SIZE);
        }
    }
    kfree(page);
}

/** The regions of `vm` as the file records them, `*count` of them, in an
 * array from the heap the caller frees; NULL when the heap has no room. */
static struct core_region *regions_take(const vm_space_t *vm, uint32_t *count)
{
    struct core_region *regions = kmalloc((vm->count + 1u) * sizeof(*regions));
    uint32_t i = 0;
    const vma_t *r;

    if (!regions) {
        return NULL;
    }
    for (r = vm_first(vm); r; r = vm_next(r), i++) {
        regions[i].start = r->start;
        regions[i].end = r->end;
        regions[i].prot = r->prot;
        regions[i].of_file = r->file != NULL;
        regions[i].holes = vdso_is_data(r->file);
    }
    *count = vm->count;
    return regions;
}

/**
 * Write the ELF header of a core file of `phnum` program headers, the first
 * at the header's end. With PN_XNUM or more, the header's count is PN_XNUM
 * and the file has one section header, at `shoff`, which has the count.
 */
static void ehdr_write(struct core_out *o, const struct core_elf *f, uint32_t phnum,
                       uintptr_t shoff)
{
    uint8_t ehdr[CORE_HDR_MAX];

    k_memset(ehdr, 0, sizeof(ehdr));
    ehdr[EI_MAG0] = ELFMAG0;
    ehdr[EI_MAG1] = ELFMAG1;
    ehdr[EI_MAG2] = ELFMAG2;
    ehdr[EI_MAG3] = ELFMAG3;
    ehdr[EI_CLASS] = f->cls;
    ehdr[EI_DATA] = ELFDATA2LSB;
    ehdr[EI_VERSION] = EV_CURRENT;
    put16(ehdr + 16, ET_CORE);
    put16(ehdr + 18, f->machine);
    put32(ehdr + 20, EV_CURRENT);
    put_word(f, ehdr + f->e_phoff, f->ehdr_size);
    put16(ehdr + f->e_ehsize, (uint16_t)f->ehdr_size);
    put16(ehdr + E_PHENTSIZE(f), (uint16_t)f->phdr_size);
    put16(ehdr + E_PHNUM(f), (uint16_t)(phnum < PN_XNUM ? phnum : PN_XNUM));
    if (phnum >= PN_XNUM) {
        put_word(f, ehdr + f->e_shoff, shoff);
        put16(ehdr + E_SHENTSIZE(f), (uint16_t)f->shdr_size);
        put16(ehdr + E_SHNUM(f), 1);
    }
    (void)out(o, ehdr, f->ehdr_size);
}

/** Write the program header of the load of region `r`, whose bytes are at
 * `off` in the file. */
static void load_write(struct core_out *o, const struct core_elf *f, const struct core_region *r,
                       uintptr_t off)
{
    uint8_t phdr[CORE_HDR_MAX];
    uint32_t flags = 0;

    if (VM_READABLE(r->prot)) {
        flags |= PF_R;
    }
    if (r->prot & VM_PROT_WRITE) {
        flags |= PF_W;
    }
    if (r->prot & VM_PROT_EXEC) {
        flags |= PF_X;
    }
    k_memset(phdr, 0, sizeof(phdr));
    put32(phdr, PT_LOAD);
    put_word(f, phdr + f->p_offset, off);
    put_word(f, phdr + f->p_vaddr, r->start);
    put_word(f, phdr + f->p_filesz, region_file_size(r));
    put_word(f, phdr + f->p_memsz, r->end - r->start);
    put32(phdr + f->p_flags, flags);
    put_word(f, phdr + f->p_align, PAGE_SIZE);
    (void)out(o, phdr, f->phdr_size);
}

/** Write the one section header of extended numbering: a null section whose
 * sh_size is the count of section headers, 1, and whose sh_info is the
 * count of program headers, `phnum`, as Linux's `fill_extnum_info` has it. */
static void shdr_write(struct core_out *o, const struct core_elf *f, uint32_t phnum)
{
    uint8_t shdr[CORE_HDR_MAX];

    k_memset(shdr, 0, sizeof(shdr));
    put_word(f, shdr + f->s_size, 1);
    put32(shdr + f->s_info, phnum);
    (void)out(o, shdr, f->shdr_size);
}

/**
 * The file is the ELF header, the program headers with the note's first,
 * the notes, zeros to the next page, the pages of each readable region in
 * the order of the headers, and with extended numbering the one section
 * header at the end, where the ELF header's `e_shoff` points.
 */
int32_t core_write(core_write_fn write, void *ctx, const uint8_t *notes, uint32_t notes_size,
                   uint64_t limit)
{
    struct mm *mm = current_proc->mm;
    const struct core_elf *f = core_elf();
    struct core_region *regions;
    struct core_out o;
    uint8_t phdr[CORE_HDR_MAX];
    uint32_t heads;
    uint32_t count;
    uint32_t phnum;
    uint32_t data;
    uintptr_t end;
    uintptr_t off;
    uint32_t i;

    regions = regions_take(&mm->vm, &count);
    if (!regions) {
        return -ENOMEM;
    }
    phnum = count + 1u;
    o.write = write;
    o.ctx = ctx;
    o.done = 0;
    o.limit = limit;
    o.err = 0;
    heads = f->ehdr_size + phnum * f->phdr_size;
    data = (heads + notes_size + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    end = data;
    for (i = 0; i < count; i++) {
        end += region_file_size(&regions[i]);
    }

    ehdr_write(&o, f, phnum, end);
    k_memset(phdr, 0, sizeof(phdr));
    put32(phdr, PT_NOTE);
    put_word(f, phdr + f->p_offset, heads);
    put_word(f, phdr + f->p_filesz, notes_size);
    (void)out(&o, phdr, f->phdr_size);
    off = data;
    for (i = 0; i < count; i++) {
        load_write(&o, f, &regions[i], off);
        off += region_file_size(&regions[i]);
    }
    (void)out(&o, notes, notes_size);
    (void)out(&o, NULL, data - (heads + notes_size));
    for (i = 0; i < count && o.err == 0; i++) {
        if (region_file_size(&regions[i]) != 0) {
            region_write(&o, mm, &regions[i]);
        }
    }
    if (phnum >= PN_XNUM) {
        shdr_write(&o, f, phnum);
    }
    kfree(regions);
    return o.err;
}
