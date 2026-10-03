/**
 * Check the core file's layout against an address space that changes while
 * the file is written, the wait for the process's other threads, and the
 * count of program headers past what the ELF header's field holds.
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
 * judges. The wait for the other threads is a model that ends them: it must
 * come before anything is written, and the regions it leaves are the ones
 * the file records; an execve waiting for the dumping thread ends it at once.
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
struct percpu cpu_table[NR_CPUS] = {{&self}};
process_t *proc_list = &self;
static uint8_t *file;
static uint32_t file_len, file_cap;
static int writes, grow_at, waits, writes_at_wait, grows_at_wait, failures;

#define CHECK(c, detail)                                 \
    do {                                                 \
        if (!(c)) {                                      \
            printf("coredump-test: fail: %s\n", detail); \
            failures++;                                  \
        }                                                \
    } while (0)

void *kmalloc(size_t size)
{
    return calloc(1, size);
}

void kfree(void *p)
{
    free(p);
}

void fpu_save(void *area)
{
    (void)area;
}

/** The rows judge the regions, not the notes: the registers are zeros and
 * no floating-point note is written. */
void arch_regs_get(const struct process *t, unsigned long *r)
{
    (void)t;
    k_memset(r, 0, ARCH_GREGS * sizeof(unsigned long));
}

uint32_t arch_core_fp_size(void)
{
    return 0;
}

void arch_core_fp_notes(uint8_t **p, const struct process *t)
{
    (void)p;
    (void)t;
}

/** The pages the process has: every page of a region but the second page
 * of one that is longer. */
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

/** The writer: keep the bytes, holes as zeros, growing the regions on the
 * call `grow_at`. */
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

/** The wait for the other threads: note that it came and when, and end
 * them, one of which finished an mmap first. */
int wq_wait_event_nointr(wait_queue_t *wq, wq_cond_fn cond, void *arg)
{
    CHECK(wq == &group.gone, "the dumping thread waits on its process's queue of ends");
    waits++;
    writes_at_wait = writes;
    grows_at_wait = cond(arg);
    if (!cond(arg)) {
        regions_grow();
        group.live = 1;
    }
    CHECK(cond(arg), "the wait ends when the other threads have");
    return 0;
}

static uint32_t get16(uint32_t off)
{
    uint16_t v;

    k_memcpy(&v, file + off, sizeof(v));
    return v;
}

static uint32_t get32(uint32_t off)
{
    uint32_t v;

    k_memcpy(&v, file + off, sizeof(v));
    return v;
}

/** Write a core file of the model with `live` threads and an execve
 * waiting when `exec`, the regions growing at write `grow`; its result. */
static int32_t dump(uint32_t live, int exec, int grow)
{
    static const uint8_t notes[20] = {5};

    free(file);
    file = NULL;
    file_len = file_cap = 0;
    writes = waits = 0;
    writes_at_wait = -1;
    grow_at = grow;
    group.live = live;
    group.exec_task = exec ? &sibling : NULL;
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
    CHECK(dump(1, 0, 3) == 0, "the file is written while the regions grow");
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
    CHECK(dump(1, 0, 0) == 0, "the file of a region of a mapped file is written");
    CHECK(file[16384] == 0x4D && file[20479] == 0x4D,
          "a page of a mapped file the process never touched is read from the file");
}

/**
 * The file is written after the other threads end: none is written before
 * the wait, and a region one of them mapped before it ended is in the file.
 * An execve waiting for the dumping thread ends the wait at once.
 */
static void threads_end_first(void)
{
    regions_set(4);
    CHECK(dump(2, 0, 0) == 0 && waits == 1, "the dumping thread waits for the other threads");
    CHECK(writes_at_wait == 0 && !grows_at_wait, "before anything is written");
    CHECK(get16(44) == 6, "and the file has the region a thread mapped before it ended");
    regions_set(4);
    CHECK(dump(2, 1, 0) == 0 && waits == 1 && group.live == 2 && get16(44) == 5,
          "an execve waiting for the dumping thread ends the wait at once");
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
    CHECK(dump(1, 0, 0) == 0, "a process of many regions leaves its file");
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

int main(void)
{
    self.pid = self.tgid = 3;
    self.tg = &group;
    self.mm = &space;
    self.state = PROC_RUNNING;
    group.leader = &self;
    sibling.tg = &group;
    regions_taken_once();
    file_pages_read();
    threads_end_first();
    numbering(65533);
    numbering(65534);
    numbering(65535);
    free(file);
    free(space.vm.vmas);
    if (failures) {
        return 1;
    }
    printf("coredump-test: pass: the regions taken once, the pages of a mapped file, the wait "
           "for the threads and the extended numbering\n");
    return 0;
}
