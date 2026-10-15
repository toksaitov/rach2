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

/*
 * The ELF header, a program header and a section header, and the offsets of
 * the fields this file sets, in the class the architecture's programs have.
 * A file of PN_XNUM program headers or more has PN_XNUM as the header's
 * count, and the count itself in the one section header it then has: ELF's
 * extended numbering, which Linux's core files use.
 */
#if ELF_CLASS == ELFCLASS64
#define EHDR_SIZE 64u
#define PHDR_SIZE 56u
#define SHDR_SIZE 64u
#define E_SHOFF   40u
#define E_EHSIZE  52u
#define E_PHOFF   32u
#define P_FLAGS   4u
#define P_OFFSET  8u
#define P_VADDR   16u
#define P_FILESZ  32u
#define P_MEMSZ   40u
#define P_ALIGN   48u
#define S_SIZE    32u
#define S_INFO    44u
#else
#define EHDR_SIZE 52u
#define PHDR_SIZE 32u
#define SHDR_SIZE 40u
#define E_SHOFF   32u
#define E_EHSIZE  40u
#define E_PHOFF   28u
#define P_FLAGS   24u
#define P_OFFSET  4u
#define P_VADDR   8u
#define P_FILESZ  16u
#define P_MEMSZ   20u
#define P_ALIGN   28u
#define S_SIZE    20u
#define S_INFO    28u
#endif
#define E_PHENTSIZE (E_EHSIZE + 2u)
#define E_PHNUM     (E_EHSIZE + 4u)
#define E_SHENTSIZE (E_EHSIZE + 6u)
#define E_SHNUM     (E_EHSIZE + 8u)
#define PN_XNUM     0xFFFFu

/* What is being written and how much may be. */
struct core_out {
    core_write_fn write;
    void *ctx;
    uint64_t done; /* bytes written, holes not counted */
    uint64_t limit;
    int32_t err; /* the first error, after which nothing is written */
};

/* What the file has of a region: its range and protection, and whether it
 * maps a file, taken once before anything is written. */
struct core_region {
    uintptr_t start;
    uintptr_t end;
    uint32_t prot;
    int of_file;
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

/** Put an address, an offset or a size of the ELF class: 32 or 64 bits. */
static void put_word(uint8_t *p, uintptr_t v)
{
#if ELF_CLASS == ELFCLASS64
    uint64_t w = v;
#else
    uint32_t w = (uint32_t)v;
#endif

    k_memcpy(p, &w, sizeof(w));
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

/** Put a time in ticks, 100 a second, as the timeval of an elf_prstatus. */
static void put_timeval(long *tv, uint32_t ticks)
{
    tv[0] = (long)(ticks / 100u);
    tv[1] = (long)((ticks % 100u) * 10000u);
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
 * saved when it parked. */
uint8_t *core_notes(int signo, uint32_t *size)
{
    struct core_prstatus status;
    struct core_prpsinfo info;
    uint32_t auxv_size = (uint32_t)sizeof(current_proc->mm->auxv);
    uint32_t threads = 0;
    uint32_t each = core_note_bytes(sizeof(status)) + arch_core_fp_size();
    uint32_t fixed = core_note_bytes(sizeof(info)) + core_note_bytes(auxv_size);
    uint8_t *buf;
    uint8_t *p;
    process_t *t;

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
    prstatus_fill(&status, current_proc, signo);
    core_note_put(&p, "CORE", NT_PRSTATUS, &status, sizeof(status));
    prpsinfo_fill(&info);
    core_note_put(&p, "CORE", NT_PRPSINFO, &info, sizeof(info));
    core_note_put(&p, "CORE", NT_AUXV, current_proc->mm->auxv, auxv_size);
    fpu_save(current_proc->fpu);
    arch_core_fp_notes(&p, current_proc);
    for (t = proc_list; t; t = t->next) {
        if (t == current_proc || !core_thread(t)) {
            continue;
        }
        prstatus_fill(&status, t, signo);
        core_note_put(&p, "CORE", NT_PRSTATUS, &status, sizeof(status));
        arch_core_fp_notes(&p, t);
    }
    *size = (uint32_t)(p - buf);
    return buf;
}

/** The bytes of region `r` the file has: all of a readable region, none of
 * one that is not, as Linux leaves a PROT_NONE reservation out, and none of
 * device memory, which Linux leaves out too. */
static uintptr_t region_file_size(const struct core_region *r)
{
    return (r->prot & VM_PROT_READ) && !(r->prot & VM_PROT_IO) ? r->end - r->start : 0u;
}

/**
 * Write the pages of region `r`: a page the process has, read through its
 * address space; a page of a file it never touched, brought in from the
 * file as a read of /proc/PID/mem brings it, as Linux's dump brings it,
 * since a debugger reads the file's bytes from the core, among them the
 * version of libc that libthread_db checks; and a hole for a page of
 * anonymous memory it never touched. Each page is looked up in the address
 * space as it is read, so a page another process sharing the space has
 * unmapped since the regions were taken is a hole.
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
        if ((!r->of_file && paging_user_phys(mm->dir, addr) == 0) ||
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
    uint32_t i;

    if (!regions) {
        return NULL;
    }
    for (i = 0; i < vm->count; i++) {
        regions[i].start = vm->vmas[i].start;
        regions[i].end = vm->vmas[i].end;
        regions[i].prot = vm->vmas[i].prot;
        regions[i].of_file = vm->vmas[i].file != NULL;
    }
    *count = vm->count;
    return regions;
}

/**
 * Write the ELF header of a core file of `phnum` program headers, the first
 * at the header's end. With PN_XNUM or more, the header's count is PN_XNUM
 * and the file has one section header, at `shoff`, which has the count.
 */
static void ehdr_write(struct core_out *o, uint32_t phnum, uintptr_t shoff)
{
    uint8_t ehdr[EHDR_SIZE];

    k_memset(ehdr, 0, sizeof(ehdr));
    ehdr[EI_MAG0] = ELFMAG0;
    ehdr[EI_MAG1] = ELFMAG1;
    ehdr[EI_MAG2] = ELFMAG2;
    ehdr[EI_MAG3] = ELFMAG3;
    ehdr[EI_CLASS] = ELF_CLASS;
    ehdr[EI_DATA] = ELFDATA2LSB;
    ehdr[EI_VERSION] = EV_CURRENT;
    put16(ehdr + 16, ET_CORE);
    put16(ehdr + 18, ELF_MACHINE);
    put32(ehdr + 20, EV_CURRENT);
    put_word(ehdr + E_PHOFF, EHDR_SIZE);
    put16(ehdr + E_EHSIZE, EHDR_SIZE);
    put16(ehdr + E_PHENTSIZE, PHDR_SIZE);
    put16(ehdr + E_PHNUM, (uint16_t)(phnum < PN_XNUM ? phnum : PN_XNUM));
    if (phnum >= PN_XNUM) {
        put_word(ehdr + E_SHOFF, shoff);
        put16(ehdr + E_SHENTSIZE, SHDR_SIZE);
        put16(ehdr + E_SHNUM, 1);
    }
    (void)out(o, ehdr, sizeof(ehdr));
}

/** Write the program header of the load of region `r`, whose bytes are at
 * `off` in the file. */
static void load_write(struct core_out *o, const struct core_region *r, uintptr_t off)
{
    uint8_t phdr[PHDR_SIZE];
    uint32_t flags = 0;

    if (r->prot & VM_PROT_READ) {
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
    put_word(phdr + P_OFFSET, off);
    put_word(phdr + P_VADDR, r->start);
    put_word(phdr + P_FILESZ, region_file_size(r));
    put_word(phdr + P_MEMSZ, r->end - r->start);
    put32(phdr + P_FLAGS, flags);
    put_word(phdr + P_ALIGN, PAGE_SIZE);
    (void)out(o, phdr, sizeof(phdr));
}

/** Write the one section header of extended numbering: a null section whose
 * sh_size is the count of section headers, 1, and whose sh_info is the
 * count of program headers, `phnum`, as Linux's `fill_extnum_info` has it. */
static void shdr_write(struct core_out *o, uint32_t phnum)
{
    uint8_t shdr[SHDR_SIZE];

    k_memset(shdr, 0, sizeof(shdr));
    put_word(shdr + S_SIZE, 1);
    put32(shdr + S_INFO, phnum);
    (void)out(o, shdr, sizeof(shdr));
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
    struct core_region *regions;
    struct core_out o;
    uint8_t phdr[PHDR_SIZE];
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
    data = (EHDR_SIZE + phnum * PHDR_SIZE + notes_size + PAGE_SIZE - 1u) & ~(PAGE_SIZE - 1u);
    end = data;
    for (i = 0; i < count; i++) {
        end += region_file_size(&regions[i]);
    }

    ehdr_write(&o, phnum, end);
    k_memset(phdr, 0, sizeof(phdr));
    put32(phdr, PT_NOTE);
    put_word(phdr + P_OFFSET, EHDR_SIZE + phnum * PHDR_SIZE);
    put_word(phdr + P_FILESZ, notes_size);
    (void)out(&o, phdr, sizeof(phdr));
    off = data;
    for (i = 0; i < count; i++) {
        load_write(&o, &regions[i], off);
        off += region_file_size(&regions[i]);
    }
    (void)out(&o, notes, notes_size);
    (void)out(&o, NULL, data - (EHDR_SIZE + phnum * PHDR_SIZE + notes_size));
    for (i = 0; i < count && o.err == 0; i++) {
        if (region_file_size(&regions[i]) != 0) {
            region_write(&o, mm, &regions[i]);
        }
    }
    if (phnum >= PN_XNUM) {
        shdr_write(&o, phnum);
    }
    kfree(regions);
    return o.err;
}
