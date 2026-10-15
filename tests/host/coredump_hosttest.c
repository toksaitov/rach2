/**
 * coredump_hosttest.c: the core file's regions, the wait for a process's
 * other threads to park, a thread's parking, and extended numbering.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 * host-cflags: -Wno-int-to-pointer-cast -Wno-pointer-to-int-cast
 *
 * `proc/coredump.c` is included whole. The writer keeps the file in memory,
 * holes as zeros. On a chosen call it replaces the address space's array of
 * regions with a longer one and frees the old, as another process sharing
 * the address space can with an mmap while the writer sleeps: the program
 * headers must still describe the regions as they were taken, each once,
 * and nothing may be read from the freed array, which AddressSanitizer
 * judges.
 *
 * The wait for the other threads is a model that parks them. The notes must
 * be built after it, with a status for the parked thread, whose registers are
 * read only once it has parked, and the file written after must record the
 * regions as the parked thread left them. An execve waiting for the dumping
 * thread ends the wait at once. A thread that parks saves its floating-point
 * state, marks itself parked and wakes the dumping thread before it waits,
 * and the wait goes on until the core is written.
 *
 * A page of a mapped file the process never touched is read for the file,
 * where one of anonymous memory is a hole.
 *
 * A process of 65533 inaccessible regions has 65534 program headers, which
 * the header's 16-bit field holds; one of 65534 or 65535 regions has
 * PN_XNUM there and its count in the sh_info of the one section header, at
 * the file's end, as ELF's extended numbering has it.
 */
#include "proc/coredump.c"

extern void *calloc(unsigned long count, unsigned long size);
extern void *realloc(void *ptr, unsigned long size);
extern void free(void *ptr);
extern int printf(const char *format, ...);

static process_t self;
static process_t sibling;
static struct tgroup group;
static struct mm space;
struct percpu cpu_table[NR_CPUS] = {{.current = &self}};
process_t *proc_list = &self;
/** The core file `put` has written, `file_len` bytes in `file_cap`. */
static uint8_t *file;
static uint32_t file_len, file_cap;
/* Calls of put, the call that grows the regions (0 for none), and counts. */
static int writes, grow_at, waits, wakes, fpu_saves, failures;

/** Count and print a failure when `c` is false. */
#define CHECK(c, detail)                                 \
    do {                                                 \
        if (!(c)) {                                      \
            printf("coredump-test: fail: %s\n", detail); \
            failures++;                                  \
        }                                                \
    } while (0)

/** The kernel's allocator, from the C library, zeroed. */
void *kmalloc(size_t size)
{
    return calloc(1, size);
}

/** Free a block from `kmalloc`. */
void kfree(void *p)
{
    free(p);
}

/** Count a save of the floating-point state; nothing is saved. */
void fpu_save(void *area)
{
    (void)area;
    fpu_saves++;
}

/** Count a wake, which must be on the process's `gone` queue. */
void wq_wake_all(wait_queue_t *wq)
{
    CHECK(wq == &group.gone, "a wake of the core is on its process's queue");
    wakes++;
}

/** Zero the registers of `t`. The rows judge the regions, so the registers
 * are zeros and no floating-point note is written. Another thread's
 * registers are read only while it is parked, or waits in an execve for the
 * dumping thread, when its saved state does not change. */
void arch_regs_get(const struct process *t, unsigned long *r)
{
    CHECK(t != &sibling || sibling.core_parked || group.exec_task == &sibling,
          "another thread's registers are read only once it has parked");
    k_memset(r, 0, ARCH_GREGS * sizeof(unsigned long));
}

/** The size of the floating-point notes of a thread: none. */
uint32_t arch_core_fp_size(void)
{
    return 0;
}

/** Write no floating-point notes. */
void arch_core_fp_notes(uint8_t **p, const struct process *t)
{
    (void)p;
    (void)t;
}

/** The physical address of the page at `addr`: 0x100000 for every page of a
 * region but the second page of a longer one, and 0, no page, for that page
 * and outside every region. */
uintptr_t paging_user_phys(page_dir_t *dir, uintptr_t addr)
{
    uint32_t i;

    (void)dir;
    for (i = 0; i < space.vm.count; i++) {
        if (addr >= space.vm.vmas[i].start && addr < space.vm.vmas[i].end) {
            return addr - space.vm.vmas[i].start == PAGE_SIZE ? 0 : 0x100000u;
        }
    }
    return 0;
}

/** A page's bytes are the low byte of its page number. */
int paging_access(struct mm *mm, uintptr_t addr, void *buf, uint32_t len, int write)
{
    (void)mm;
    (void)write;
    k_memset(buf, (int)((addr >> 12) & 0xFFu), len);
    return 0;
}

/** Put `count` regions of a page each, readable and writable, from
 * 0x08048000 a page apart, the third of two pages and the fourth
 * inaccessible, in the address space's array. */
static void regions_set(uint32_t count)
{
    uint32_t i;
    uint32_t at = 0x08048000u;

    free(space.vm.vmas);
    space.vm.vmas = calloc(count + 1u, sizeof(vma_t));
    space.vm.cap = count + 1u;
    space.vm.count = count;
    for (i = 0; i < count; i++) {
        space.vm.vmas[i].start = at;
        space.vm.vmas[i].end = at + (i == 2 ? 2u : 1u) * PAGE_SIZE;
        space.vm.vmas[i].prot = i == 3 ? 0u : VM_PROT_READ | VM_PROT_WRITE;
        space.vm.vmas[i].type = VM_ANON;
        at = space.vm.vmas[i].end + PAGE_SIZE;
    }
}

/** Grow the array by one region, into a new array, freeing the old one. */
static void regions_grow(void)
{
    uint32_t count = space.vm.count;
    vma_t *more = calloc(count + 2u, sizeof(vma_t));

    k_memcpy(more, space.vm.vmas, count * sizeof(vma_t));
    more[count].start = 0x70000000u;
    more[count].end = 0x70001000u;
    more[count].prot = VM_PROT_READ;
    more[count].type = VM_ANON;
    free(space.vm.vmas);
    space.vm.vmas = more;
    space.vm.cap = count + 2u;
    space.vm.count = count + 1u;
}

/** The writer: append `len` bytes of `buf` to `file`, or zeros for a hole
 * when `buf` is NULL, growing the regions on the call `grow_at`. Returns 0,
 * or -ENOMEM when the file cannot grow. */
static int32_t put(void *ctx, const void *buf, uint32_t len)
{
    uint8_t *more;

    (void)ctx;
    if (++writes == grow_at) {
        regions_grow();
    }
    if (file_len + len > file_cap) {
        uint32_t cap = file_cap ? file_cap : 4096u;

        while (cap < file_len + len) {
            cap *= 2u;
        }
        more = realloc(file, cap);
        if (!more) {
            return -ENOMEM;
        }
        file = more;
        file_cap = cap;
    }
    if (buf) {
        k_memcpy(file + file_len, buf, len);
    } else {
        k_memset(file + file_len, 0, len);
    }
    file_len += len;
    return 0;
}

/** The waits of the core: the dumping thread's for the other threads, which
 * the model parks, the one that finishes an mmap first; and a parked
 * thread's for the core, which the model writes. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    CHECK(wq == &group.gone, "a wait of the core is on its process's queue");
    waits++;
    if (cond == core_parked_all) {
        if (!cond(arg)) {
            regions_grow();
            sibling.core_parked = 1;
        }
        CHECK(cond(arg), "the wait ends when the other threads have parked");
        return 0;
    }
    CHECK(cond == core_done && self.core_parked && fpu_saves == 1 && wakes == 1,
          "a parked thread has saved its state and woken the dumping thread");
    CHECK(!cond(arg), "its wait goes on while the core is being written");
    group.core_dumping = 0;
    CHECK(cond(arg), "its wait ends when the core is written");
    return 0;
}

/** The little-endian 16-bit field at `off` of the file. */
static uint32_t get16(uint32_t off)
{
    uint16_t v;

    k_memcpy(&v, file + off, sizeof(v));
    return v;
}

/** The little-endian 32-bit field at `off` of the file. */
static uint32_t get32(uint32_t off)
{
    uint32_t v;

    k_memcpy(&v, file + off, sizeof(v));
    return v;
}

/** Empty the file and grow the regions at write `grow`. */
static void file_reset(int grow)
{
    free(file);
    file = NULL;
    file_len = file_cap = 0;
    writes = 0;
    grow_at = grow;
}

/** Write a core file of the model with notes of 20 bytes, the regions
 * growing at write `grow`; its result. */
static int32_t dump(int grow)
{
    static const uint8_t notes[20] = {5};

    file_reset(grow);
    return core_write(put, NULL, notes, sizeof(notes), 0xFFFFFFFFu);
}

/** Whether program header `i` of the file is a load of the model's region
 * `r` as it was set, at file offset `off`, and the offset after it. */
static int load_is(uint32_t i, uint32_t r, uint32_t *off)
{
    uint32_t ph = 52u + 32u * i;
    uint32_t start = 0x08048000u + 2u * PAGE_SIZE * r + (r > 2 ? PAGE_SIZE : 0u);
    uint32_t mem = (r == 2 ? 2u : 1u) * PAGE_SIZE;
    uint32_t filesz = r == 3 ? 0u : mem;
    int ok = get32(ph) == PT_LOAD && get32(ph + 4) == *off && get32(ph + 8) == start &&
             get32(ph + 16) == filesz && get32(ph + 20) == mem &&
             get32(ph + 24) == (r == 3 ? 0u : (uint32_t)(PF_R | PF_W));

    *off += filesz;
    return ok;
}

/**
 * The regions are taken once: the array grows and is freed at the first
 * program header of a load, and the file still has four loads, with the
 * pages of the three readable regions, the missing page a hole, and none of
 * the inaccessible one.
 */
static void regions_taken_once(void)
{
    uint32_t off = 4096u;
    uint32_t i;

    regions_set(4);
    CHECK(dump(3) == 0, "the file is written while the regions grow");
    CHECK(get16(44) == 5, "the header counts the note and the four regions taken");
    for (i = 0; i < 4; i++) {
        CHECK(load_is(i + 1u, i, &off), "each load describes its region as it was taken");
    }
    CHECK(file_len == off, "the file ends with the last readable region's pages");
    CHECK(file[4096] == 0x48 && file[8192] == 0x4A && file[12288] == 0x4C,
          "a page the process has is written");
    CHECK(file[16384] == 0 && file[20479] == 0, "and a page it never touched is a hole");
}

/** A page of a mapped file the process never touched is read for the file,
 * where the same page of anonymous memory is a hole. */
static void file_pages_read(void)
{
    static struct vm_file mapped;

    regions_set(4);
    space.vm.vmas[2].file = &mapped;
    CHECK(dump(0) == 0, "the file of a region of a mapped file is written");
    CHECK(file[16384] == 0x4D && file[20479] == 0x4D,
          "a page of a mapped file the process never touched is read from the file");
}

/**
 * The notes are built after the other threads park: the wait comes first,
 * the notes have a status for the parked thread, and a region it mapped
 * before it parked is in the file written after. An execve waiting for the
 * dumping thread ends the wait at once.
 */
static void threads_park_first(void)
{
    uint32_t one = core_note_bytes(sizeof(struct core_prstatus));
    uint32_t rest =
        core_note_bytes(sizeof(struct core_prpsinfo)) + core_note_bytes(sizeof(space.auxv));
    uint8_t *notes;
    uint32_t size = 0;

    regions_set(4);
    self.next = &sibling;
    sibling.state = PROC_BLOCKED;
    sibling.kernel_stack = 0x1000u;
    sibling.core_parked = 0;
    group.exec_task = NULL;
    waits = 0;
    notes = core_notes(SIGABRT, &size);
    CHECK(notes && waits == 1 && sibling.core_parked,
          "the dumping thread waits for the other threads to park");
    CHECK(size == 2u * one + rest, "and the notes have a status for each thread");
    file_reset(0);
    CHECK(notes && core_write(put, NULL, notes, size, 0xFFFFFFFFu) == 0 && get16(44) == 6,
          "the file has the region a thread mapped before it parked");
    kfree(notes);
    regions_set(4);
    sibling.core_parked = 0;
    group.exec_task = &sibling;
    waits = 0;
    notes = core_notes(SIGABRT, &size);
    CHECK(notes && waits == 1 && !sibling.core_parked && space.vm.count == 4,
          "an execve waiting for the dumping thread ends the wait at once");
    kfree(notes);
    self.next = NULL;
    group.exec_task = NULL;
}

/** A thread parks for the core and goes on once it is written, and a release
 * lets the parked threads go. */
static void thread_parks(void)
{
    group.core_dumping = 1;
    fpu_saves = wakes = waits = 0;
    core_park();
    CHECK(waits == 1 && !self.core_parked && !group.core_dumping,
          "a parked thread is no longer parked once the core is written");
    group.core_dumping = 1;
    wakes = 0;
    core_release();
    CHECK(!group.core_dumping && wakes == 1, "a release clears the dump and wakes the threads");
}

/** Put `count` inaccessible regions of a page each, a page apart, in the
 * address space's array; the file has no bytes of them. */
static void regions_none(uint32_t count)
{
    uint32_t i;

    free(space.vm.vmas);
    space.vm.vmas = calloc(count, sizeof(vma_t));
    space.vm.cap = count;
    space.vm.count = count;
    for (i = 0; i < count; i++) {
        space.vm.vmas[i].start = 0x08048000u + 2u * PAGE_SIZE * i;
        space.vm.vmas[i].end = space.vm.vmas[i].start + PAGE_SIZE;
        space.vm.vmas[i].type = VM_ANON;
    }
}

/**
 * The ELF header's count of program headers, its section headers, and the
 * size of the file, of a process of `count` inaccessible regions: the note's
 * header and a load's each, the section header at the file's end with the
 * whole count when there are PN_XNUM or more, and nothing more.
 */
static void numbering(uint32_t count)
{
    uint32_t phnum = count + 1u;
    uint32_t data = (52u + 32u * phnum + 20u + 4095u) & ~4095u;
    int ext = phnum >= 0xFFFFu;

    regions_none(count);
    CHECK(dump(0) == 0, "a process of many regions leaves its file");
    CHECK(get16(44) == (ext ? 0xFFFFu : phnum), "the header's count of program headers");
    CHECK(get32(32) == (ext ? data : 0u) && get16(46) == (ext ? 40u : 0u) &&
              get16(48) == (ext ? 1u : 0u),
          "the header has a section header exactly when the count does not fit");
    CHECK(file_len == data + (ext ? 40u : 0u), "the file ends after the section header it has");
    if (ext && file_len == data + 40u) {
        CHECK(get32(data + 4) == 0 && get32(data + 20) == 1 && get32(data + 28) == phnum,
              "the section header is a null one with the count in sh_info");
    }
}

/** Set up the dumping thread and its process and run the rows; 0 when every
 * check passed, 1 otherwise. */
int main(void)
{
    self.pid = self.tgid = 3;
    self.tg = &group;
    self.mm = &space;
    self.state = PROC_RUNNING;
    self.kernel_stack = 0x2000u;
    group.leader = &self;
    sibling.tg = &group;
    regions_taken_once();
    file_pages_read();
    threads_park_first();
    thread_parks();
    numbering(65533);
    numbering(65534);
    numbering(65535);
    free(file);
    free(space.vm.vmas);
    if (failures) {
        return 1;
    }
    printf("coredump-test: pass: the regions taken once, the pages of a mapped file, the wait "
           "for the threads to park, a thread's parking and the extended numbering\n");
    return 0;
}
