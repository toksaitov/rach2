/*
 * coredump.c: the core file of a process a signal ends, as coredump.h
 * describes it.
 */

#include "coredump.h"

#include "elf.h"
#include "fpu.h"
#include "process.h"
#include "tgroup.h"
#include "wait_queue.h"
#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/mm.h"
#include "../mm/paging.h"
#include "../mm/vm.h"

/* The notes Linux writes for an i386 process, and their sizes. */
#define NT_PRSTATUS 1u
#define NT_FPREGSET 2u
#define NT_PRPSINFO 3u
#define NT_AUXV     6u
#define NT_PRXFPREG 0x46E62B7Fu

#define PRSTATUS_SIZE 144u
#define PRPSINFO_SIZE 124u
#define FPREGSET_SIZE 108u
#define PRXFPREG_SIZE 512u

/* The ELF header, a program header and a section header. A file of PN_XNUM
 * program headers or more has PN_XNUM as the header's count, and the count
 * itself in the one section header it then has: ELF's extended numbering,
 * which Linux's core files use. */
#define EHDR_SIZE 52u
#define PHDR_SIZE 32u
#define SHDR_SIZE 40u
#define PN_XNUM   0xFFFFu

/* What is being written and how much may be. */
struct core_out {
    core_write_fn write;
    void *ctx;
    uint32_t done;
    uint32_t limit;
    int32_t err;
};

/* What the file has of a region: its range and protection, and whether it
 * maps a file, taken once before anything is written. */
struct core_region {
    uint32_t start;
    uint32_t end;
    uint32_t prot;
    int of_file;
};

static void put16(uint8_t *p, uint16_t v)
{
    k_memcpy(p, &v, sizeof(v));
}

static void put32(uint8_t *p, uint32_t v)
{
    k_memcpy(p, &v, sizeof(v));
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

/** The bytes a note of `descsz` bytes takes, its name "CORE" or "LINUX"
 * padded to eight bytes and its description to a word. */
static uint32_t note_bytes(uint32_t descsz)
{
    return 12u + 8u + ((descsz + 3u) & ~3u);
}

/** Put the note `type` named `name` with `desc` at `*p` and step past it. */
static void note_put(uint8_t **p, const char *name, uint32_t type, const void *desc,
                     uint32_t descsz)
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
    *p = q + note_bytes(descsz);
}

/** Put a time in ticks as the timeval of an elf_prstatus. */
static void put_timeval(uint8_t *p, uint32_t ticks)
{
    put32(p, ticks / 100u);
    put32(p + 4, (ticks % 100u) * 10000u);
}

/**
 * Fill the i386 `elf_prstatus` of thread `t`, whose user registers are the
 * frame at the top of its kernel stack, with `signo` the signal that ends
 * the process: the signal, the pending and blocked sets' low words, the ids,
 * the times, the 17 registers in `user_regs_struct`'s order, and a valid
 * floating-point mark.
 */
static void prstatus_fill(uint8_t *d, const process_t *t, int signo)
{
    const isr_params_t *tf = (const isr_params_t *)(t->kernel_stack - sizeof(isr_params_t));
    uint32_t regs[17];
    int i;

    regs[0] = tf->ebx;
    regs[1] = tf->ecx;
    regs[2] = tf->edx;
    regs[3] = tf->esi;
    regs[4] = tf->edi;
    regs[5] = tf->ebp;
    regs[6] = tf->eax;
    regs[7] = tf->ds & 0xFFFFu;
    regs[8] = tf->es & 0xFFFFu;
    regs[9] = tf->fs & 0xFFFFu;
    regs[10] = tf->gs & 0xFFFFu;
    regs[11] = (uint32_t)t->orig_eax;
    regs[12] = tf->eip;
    regs[13] = tf->cs & 0xFFFFu;
    regs[14] = tf->eflags;
    regs[15] = tf->user_esp;
    regs[16] = tf->user_ss & 0xFFFFu;
    k_memset(d, 0, PRSTATUS_SIZE);
    put32(d, (uint32_t)signo);
    put16(d + 12, (uint16_t)signo);
    put32(d + 16, (uint32_t)t->sig_pending);
    put32(d + 20, (uint32_t)t->sig_blocked);
    put32(d + 24, t->pid);
    put32(d + 28, t->parent_pid);
    put32(d + 32, t->pgid);
    put32(d + 36, t->sid);
    put_timeval(d + 40, t->utime);
    put_timeval(d + 48, t->stime);
    put_timeval(d + 56, t->tg->cutime);
    put_timeval(d + 64, t->tg->cstime);
    for (i = 0; i < 17; i++) {
        put32(d + 72 + 4 * i, regs[i]);
    }
    put32(d + 140, 1);
}

/**
 * Fill the i386 `elf_prpsinfo` of the current process: its state, nice
 * value, ids, program name, and the start of its arguments, NULs made
 * spaces, as `ps` shows them, read from its stack.
 */
static void prpsinfo_fill(uint8_t *d)
{
    const process_t *p = current_proc;
    struct mm *mm = p->mm;
    uint32_t len = mm->arg_end - mm->arg_start;
    uint32_t i;

    k_memset(d, 0, PRPSINFO_SIZE);
    d[1] = 'R';
    d[3] = (uint8_t)p->nice;
    put16(d + 8, (uint16_t)p->ruid);
    put16(d + 10, (uint16_t)p->rgid);
    put32(d + 12, p->tgid);
    put32(d + 16, p->parent_pid);
    put32(d + 20, p->pgid);
    put32(d + 24, p->sid);
    k_memcpy(d + 28, p->comm, 16);
    if (len > 79u) {
        len = 79u;
    }
    if (mm->arg_end > mm->arg_start && paging_access(mm, mm->arg_start, d + 44, len, 0) == 0) {
        for (i = 0; i + 1u < len; i++) {
            if (d[44 + i] == 0) {
                d[44 + i] = ' ';
            }
        }
    }
    d[44 + 79] = 0;
}

/** Whether `t` is a thread of the dumping process the core describes: its
 * leader and live threads, which have a frame on their kernel stacks. */
static int core_thread(const process_t *t)
{
    return t->tg == current_proc->tg && t->state != PROC_UNUSED &&
           (t->state != PROC_ZOMBIE || t == current_proc) && t->kernel_stack != 0;
}

/* The notes are in Linux's order: the dumping thread's status, the
 * process's information, the auxiliary vector and the thread's
 * floating-point state, which is in the unit's registers until it is saved
 * here, then each other thread's status and the floating-point state its
 * last switch saved. */
uint8_t *core_notes(int signo, uint32_t *size)
{
    uint8_t status[PRSTATUS_SIZE];
    uint8_t info[PRPSINFO_SIZE];
    uint8_t fp[FPU_LINUX_SIZE];
    uint32_t threads = 0;
    uint32_t each =
        note_bytes(PRSTATUS_SIZE) + note_bytes(FPREGSET_SIZE) + note_bytes(PRXFPREG_SIZE);
    uint8_t *buf;
    uint8_t *p;
    process_t *t;

    for (t = proc_list; t; t = t->next) {
        if (core_thread(t)) {
            threads++;
        }
    }
    *size = threads * each + note_bytes(PRPSINFO_SIZE) + note_bytes(MM_AUXV_WORDS * 4u);
    buf = kmalloc(*size);
    if (!buf) {
        return NULL;
    }
    p = buf;
    prstatus_fill(status, current_proc, signo);
    note_put(&p, "CORE", NT_PRSTATUS, status, PRSTATUS_SIZE);
    prpsinfo_fill(info);
    note_put(&p, "CORE", NT_PRPSINFO, info, PRPSINFO_SIZE);
    note_put(&p, "CORE", NT_AUXV, current_proc->mm->auxv, MM_AUXV_WORDS * 4u);
    fpu_save(current_proc->fpu);
    fpu_to_linux(current_proc->fpu, fp);
    note_put(&p, "CORE", NT_FPREGSET, fp, FPREGSET_SIZE);
    note_put(&p, "LINUX", NT_PRXFPREG, fp + 112, PRXFPREG_SIZE);
    for (t = proc_list; t; t = t->next) {
        if (t == current_proc || !core_thread(t)) {
            continue;
        }
        prstatus_fill(status, t, signo);
        note_put(&p, "CORE", NT_PRSTATUS, status, PRSTATUS_SIZE);
        fpu_to_linux(t->fpu, fp);
        note_put(&p, "CORE", NT_FPREGSET, fp, FPREGSET_SIZE);
        note_put(&p, "LINUX", NT_PRXFPREG, fp + 112, PRXFPREG_SIZE);
    }
    *size = (uint32_t)(p - buf);
    return buf;
}

/** The bytes of region `r` the file has: all of a readable region, none of
 * one that is not, as Linux leaves a PROT_NONE reservation out, and none of
 * device memory, which Linux leaves out too. */
static uint32_t region_file_size(const struct core_region *r)
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
    uint32_t addr;

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

/** Whether the core file of the process `arg` may be written: its other
 * threads have ended, or an execve of one of them waits for the rest to. */
static int core_quiet(void *arg)
{
    const struct tgroup *tg = (const struct tgroup *)arg;

    return tg->live == 1 || tg->exec_task != NULL;
}

/** The regions of `vm` as the file records them, from the heap, `*count`
 * of them; NULL when the heap has no room. */
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
static void ehdr_write(struct core_out *o, uint32_t phnum, uint32_t shoff)
{
    uint8_t ehdr[EHDR_SIZE];

    k_memset(ehdr, 0, sizeof(ehdr));
    ehdr[EI_MAG0] = ELFMAG0;
    ehdr[EI_MAG1] = ELFMAG1;
    ehdr[EI_MAG2] = ELFMAG2;
    ehdr[EI_MAG3] = ELFMAG3;
    ehdr[EI_CLASS] = ELFCLASS32;
    ehdr[EI_DATA] = ELFDATA2LSB;
    ehdr[EI_VERSION] = EV_CURRENT;
    put16(ehdr + 16, ET_CORE);
    put16(ehdr + 18, EM_386);
    put32(ehdr + 20, EV_CURRENT);
    put32(ehdr + 28, EHDR_SIZE);
    put16(ehdr + 40, EHDR_SIZE);
    put16(ehdr + 42, PHDR_SIZE);
    put16(ehdr + 44, (uint16_t)(phnum < PN_XNUM ? phnum : PN_XNUM));
    if (phnum >= PN_XNUM) {
        put32(ehdr + 32, shoff);
        put16(ehdr + 46, SHDR_SIZE);
        put16(ehdr + 48, 1);
    }
    (void)out(o, ehdr, sizeof(ehdr));
}

/** Write the program header of the load of region `r`, whose bytes are at
 * `off` in the file. */
static void load_write(struct core_out *o, const struct core_region *r, uint32_t off)
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
    put32(phdr + 4, off);
    put32(phdr + 8, r->start);
    put32(phdr + 16, region_file_size(r));
    put32(phdr + 20, r->end - r->start);
    put32(phdr + 24, flags);
    put32(phdr + 28, PAGE_SIZE);
    (void)out(o, phdr, sizeof(phdr));
}

/** Write the one section header of extended numbering: a null section whose
 * sh_size is the count of section headers, 1, and whose sh_info is the
 * count of program headers, `phnum`, as Linux's `fill_extnum_info` has it. */
static void shdr_write(struct core_out *o, uint32_t phnum)
{
    uint8_t shdr[SHDR_SIZE];

    k_memset(shdr, 0, sizeof(shdr));
    put32(shdr + 20, 1);
    put32(shdr + 28, phnum);
    (void)out(o, shdr, sizeof(shdr));
}

int32_t core_write(core_write_fn write, void *ctx, const uint8_t *notes, uint32_t notes_size,
                   uint32_t limit)
{
    struct mm *mm = current_proc->mm;
    struct core_region *regions;
    struct core_out o;
    uint8_t phdr[PHDR_SIZE];
    uint32_t count;
    uint32_t phnum;
    uint32_t data;
    uint32_t end;
    uint32_t off;
    uint32_t i;

    (void)wq_wait_event_nointr(&current_proc->tg->gone, core_quiet, current_proc->tg);
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
    put32(phdr + 4, EHDR_SIZE + phnum * PHDR_SIZE);
    put32(phdr + 16, notes_size);
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
