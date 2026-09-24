/*
 * elf.c - ELF32 executable validation and loading for user programs.
 */

#include "elf.h"
#include "../mm/paging.h"
#include "../mm/vm.h"
#include "../lib/kstring.h"
#include "../serial.h"

/* The user image window: a PT_LOAD segment must fit between USER_ADDR_MIN,
 * the bottom of user space as `user_range_ok` has it, and the user stack's
 * first page at 0xBFFFF000. Segments outside it are rejected so a hostile ELF
 * cannot request kernel- or stack-range mappings. The floor is below the
 * conventional ELF base (a freestanding -Ttext=0x08048000 binary maps its
 * header page at 0x08047000). */
#define ELF_USER_MIN 0x02000000u
#define ELF_USER_MAX 0xBFFFF000u

/**
 * Report whether `data` is a valid ELF32 executable for i386.
 *
 * The image must begin with the ELF magic `0x7F 'E' 'L' 'F'`, be 32-bit
 * little-endian of the current version, type `ET_EXEC` for machine `EM_386`,
 * and have a non-zero entry point and a program header table of entries this
 * kernel's size that lies wholly within the first `size` bytes. The bound is
 * taken by division and not by addition, since an offset near 2^32 and a
 * table length would wrap a sum and pass a table that starts past the image;
 * the caller reads the table straight after this returns.
 */
bool elf_check(const void *data, uint32_t size)
{
    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)data;

    if (size < sizeof(Elf32_Ehdr)) {
        return false;
    }

    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 || ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 || ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        return false;
    }

    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) {
        return false;
    }

    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        return false;
    }

    if (ehdr->e_ident[EI_VERSION] != EV_CURRENT) {
        return false;
    }

    if (ehdr->e_type != ET_EXEC) {
        return false;
    }

    if (ehdr->e_machine != EM_386) {
        return false;
    }

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        return false;
    }

    if (ehdr->e_phentsize != sizeof(Elf32_Phdr)) {
        return false;
    }
    if ((uint32_t)ehdr->e_phoff > size ||
        (uint32_t)ehdr->e_phnum > (size - (uint32_t)ehdr->e_phoff) / sizeof(Elf32_Phdr)) {
        return false;
    }

    if (ehdr->e_entry == 0) {
        return false;
    }

    return true;
}

/**
 * Return the virtual address where execution begins.
 *
 * The caller must have already validated the image with `elf_check`.
 */
uint32_t elf_entry_point(const void *data)
{
    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)data;
    return ehdr->e_entry;
}

/**
 * Return a pointer to the program header table, or NULL if there is none.
 *
 * The table is not bounded here; the caller runs `elf_check` first.
 */
const Elf32_Phdr *elf_program_headers(const void *data)
{
    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)data;

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        return NULL;
    }

    return (const Elf32_Phdr *)((const uint8_t *)data + ehdr->e_phoff);
}

uint16_t elf_program_header_count(const void *data)
{
    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)data;
    return ehdr->e_phnum;
}

/**
 * Validate just the ELF header of a streamed image.
 *
 * The checks of `elf_check` that the header alone can answer: the program
 * header table is not bounded against the file, and the entry point is not
 * checked. `elf_load_image`, which reads the rest of the file on demand,
 * fails a short read of the table and checks the entry against the user
 * image window; `sys_execve` uses this to tell an executable from a script
 * before it destroys the old image.
 */
bool elf_valid_exec(const void *ehdr, uint32_t len)
{
    const Elf32_Ehdr *e = (const Elf32_Ehdr *)ehdr;

    if (len < sizeof(Elf32_Ehdr)) {
        return false;
    }
    if (e->e_ident[EI_MAG0] != ELFMAG0 || e->e_ident[EI_MAG1] != ELFMAG1 ||
        e->e_ident[EI_MAG2] != ELFMAG2 || e->e_ident[EI_MAG3] != ELFMAG3) {
        return false;
    }
    if (e->e_ident[EI_CLASS] != ELFCLASS32 || e->e_ident[EI_DATA] != ELFDATA2LSB ||
        e->e_ident[EI_VERSION] != EV_CURRENT) {
        return false;
    }
    if (e->e_type != ET_EXEC || e->e_machine != EM_386) {
        return false;
    }
    if (e->e_phoff == 0 || e->e_phnum == 0 || e->e_phentsize != sizeof(Elf32_Phdr)) {
        return false;
    }
    return true;
}

/**
 * Map the program header table's file offset to its run-time virtual address.
 *
 * The kernel passes `AT_PHDR` to the program, the in-memory address of its
 * program headers, where a static C library finds its PT_TLS. Find the
 * PT_LOAD segment whose file bytes contain the whole table, not just its
 * first byte, so `AT_PHDR` names a complete table, and translate. Return 0
 * when no segment covers it. The subtractions run only after the bounds that
 * keep them from underflowing, and `table_bytes` cannot overflow, since
 * `phnum` has 16 bits.
 */
uint32_t elf_phdr_vaddr(const Elf32_Phdr *phdrs, uint16_t phnum, uint32_t phoff)
{
    uint32_t table_bytes = (uint32_t)phnum * (uint32_t)sizeof(Elf32_Phdr);
    uint16_t i;

    for (i = 0; i < phnum; i++) {
        const Elf32_Phdr *p = &phdrs[i];

        if (p->p_type == PT_LOAD && phoff >= p->p_offset && p->p_filesz >= table_bytes &&
            phoff - p->p_offset <= p->p_filesz - table_bytes) {
            return p->p_vaddr + (phoff - p->p_offset);
        }
    }
    return 0;
}

/**
 * Stream an ELF executable into `dir` and describe its layout in `out`.
 *
 * The image is never held whole in memory: `readfn` pulls the header, the
 * program headers, and each segment's bytes on demand, so the file size is
 * unbounded. The program headers are on this call's stack: a read can sleep
 * and let another process load a different executable.
 *
 * Each PT_LOAD is validated before its pages are allocated. Segments must be
 * well formed and lie within the user image window: non-wrapping, clear of
 * the kernel and stack ranges, and ascending by page, each starting at or
 * after the page-aligned end of the one before, since two that shared a page
 * would allocate it twice, leaking a frame and losing the first segment's
 * bytes. Each is recorded in `out` with its page-aligned bounds and flags,
 * from which exec builds the VMAs; a PT_LOAD past `ELF_MAX_SEGS` fails the
 * load, since a segment without a record would stay writable and have no
 * VMA. Once every segment's bytes are in place, a page covered only by
 * segments without PF_W is made read-only, so a user write to it faults and
 * the process is killed; a page shared with a writable segment stays
 * writable. The image needs at least one PT_LOAD, a program header table
 * that lives inside one (so `AT_PHDR` is real), and an entry point inside
 * the window.
 *
 * @param out filled with the segment table, image end (the heap base), entry
 *            point, and program header geometry on success.
 * @return 0 on success, -1 on any malformed or out-of-policy image, a read
 *         short of the requested length, an allocation failure, more program
 *         headers than `ELF_MAX_PHNUM` or more PT_LOADs than `ELF_MAX_SEGS`.
 *         On failure the pages already mapped stay in `dir`, for the caller
 *         to free with the address space.
 */
int elf_load_image(elf_read_fn readfn, void *ctx, page_dir_t *dir, elf_image_t *out)
{
    Elf32_Phdr phdrs[ELF_MAX_PHNUM];
    Elf32_Ehdr ehdr;
    uint16_t i;
    uint32_t ph_bytes;
    uint32_t prev_end = ELF_USER_MIN; /* page-aligned end of the previous PT_LOAD */
    int nload = 0;

    if (!readfn || !dir || !out) {
        return -1;
    }

    out->nsegs = 0;
    out->image_end = 0;

    if (readfn(ctx, &ehdr, sizeof(ehdr), 0) != (int)sizeof(ehdr)) {
        return -1;
    }
    if (!elf_valid_exec(&ehdr, sizeof(ehdr)) || ehdr.e_phnum > ELF_MAX_PHNUM) {
        return -1;
    }

    ph_bytes = (uint32_t)ehdr.e_phnum * sizeof(Elf32_Phdr);
    if (readfn(ctx, phdrs, ph_bytes, ehdr.e_phoff) != (int)ph_bytes) {
        return -1;
    }

    for (i = 0; i < ehdr.e_phnum; i++) {
        const Elf32_Phdr *ph = &phdrs[i];
        uint32_t vaddr, vaddr_end, page_addr;
        uint32_t file_offset, copied, to_copy;

        if (ph->p_type != PT_LOAD) {
            continue;
        }
        if (ph->p_filesz > ph->p_memsz) {
            return -1;
        }

        vaddr = ph->p_vaddr;
        vaddr_end = vaddr + ph->p_memsz;

        if (vaddr_end < vaddr || ph->p_offset + ph->p_filesz < ph->p_offset) {
            return -1;
        }
        if (vaddr < ELF_USER_MIN || vaddr_end > ELF_USER_MAX) {
            return -1;
        }
        if ((vaddr & ~(PAGE_SIZE - 1)) < prev_end) {
            return -1;
        }
        prev_end = (vaddr_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);
        nload++;

        {
            uint32_t seg_start = vaddr & ~(PAGE_SIZE - 1);
            uint32_t seg_end = (vaddr_end + PAGE_SIZE - 1) & ~(PAGE_SIZE - 1);

            if (out->nsegs >= ELF_MAX_SEGS) {
                return -1;
            }
            out->segs[out->nsegs].start = seg_start;
            out->segs[out->nsegs].end = seg_end;
            out->segs[out->nsegs].flags = ph->p_flags;
            out->nsegs++;
            if (seg_end > out->image_end) {
                out->image_end = seg_end;
            }
        }

        for (page_addr = (vaddr & ~(PAGE_SIZE - 1)); page_addr < vaddr_end;
             page_addr += PAGE_SIZE) {
            uintptr_t phys_addr = alloc_user_page(dir, page_addr);

            if (phys_addr == 0) {
                return -1;
            }
            if (paging_zero_frame(phys_addr) != 0) {
                return -1;
            }
        }

        file_offset = ph->p_offset;
        copied = 0;
        to_copy = ph->p_filesz;

        while (copied < to_copy) {
            uint32_t curr_vaddr = vaddr + copied;
            uint32_t page_base = curr_vaddr & ~(PAGE_SIZE - 1);
            uint32_t page_offset = curr_vaddr & (PAGE_SIZE - 1);
            uint32_t chunk_size = PAGE_SIZE - page_offset;
            uint32_t pde_idx, pte_idx;
            page_table_t *pt;
            uintptr_t phys_base;

            if (chunk_size > to_copy - copied) {
                chunk_size = to_copy - copied;
            }

            pde_idx = page_base / PAGE_SIZE / 1024;
            pte_idx = (page_base / PAGE_SIZE) % 1024;
            pt = dir->logical_page_tables[pde_idx];
            if (!pt) {
                return -1;
            }
            phys_base = pt->pages[pte_idx] & ~0xFFF;

            {
                uint8_t *page = kmap_temp(phys_base);
                int got;

                if (!page) {
                    return -1;
                }
                got = readfn(ctx, page + page_offset, chunk_size, file_offset + copied);
                kunmap_temp(page);
                if (got != (int)chunk_size) {
                    return -1;
                }
            }
            copied += chunk_size;
        }
    }

    {
        int si, sj;

        for (si = 0; si < out->nsegs; si++) {
            uint32_t pg;

            if (out->segs[si].flags & PF_W) {
                continue;
            }
            for (pg = out->segs[si].start; pg < out->segs[si].end; pg += PAGE_SIZE) {
                int writable = 0;

                for (sj = 0; sj < out->nsegs; sj++) {
                    if ((out->segs[sj].flags & PF_W) && pg >= out->segs[sj].start &&
                        pg < out->segs[sj].end) {
                        writable = 1;
                        break;
                    }
                }
                if (!writable) {
                    paging_set_writable(dir, pg, false);
                }
            }
        }
    }

    out->phdr_vaddr = elf_phdr_vaddr(phdrs, ehdr.e_phnum, ehdr.e_phoff);
    if (nload == 0 || out->phdr_vaddr == 0) {
        return -1;
    }
    if (ehdr.e_entry < ELF_USER_MIN || ehdr.e_entry >= ELF_USER_MAX) {
        return -1;
    }
    out->entry = ehdr.e_entry;
    out->phent = ehdr.e_phentsize;
    out->phnum = ehdr.e_phnum;
    return 0;
}

/**
 * Build the VMA map of a fresh address space from a loaded ELF image.
 *
 * Lay down one VMA per loaded segment with protection from its `p_flags`, an
 * empty heap just past the image, the anonymous-mmap window, and a one-page
 * stack at `stack_top` that may grow down to `stack_top - stack_growth`.
 * 0, or -1 when a region could not be recorded, which is the heap having
 * no room for the array.
 */
int elf_setup_vmas(struct vm_space *vm, const elf_image_t *img, uint32_t stack_top,
                   uint32_t stack_growth)
{
    int si;

    vm_init(vm);

    /* One VMA per loaded segment, protection from the ELF p_flags. */
    for (si = 0; si < img->nsegs; si++) {
        uint32_t flags = img->segs[si].flags;
        uint32_t prot = 0;
        vm_type_t vt;

        if (flags & PF_R) {
            prot |= VM_PROT_READ;
        }
        if (flags & PF_W) {
            prot |= VM_PROT_WRITE;
        }
        if (flags & PF_X) {
            prot |= VM_PROT_EXEC;
        }
        vt = (flags & PF_X) ? VM_CODE : VM_DATA;
        if (vm_insert(vm, img->segs[si].start, img->segs[si].end, prot, vt) != 0) {
            return -1;
        }
    }

    /* Heap starts empty just past the image; brk grows it on demand. */
    vm->heap_start = img->image_end;
    vm->brk = img->image_end;

    /* Anonymous mmaps are bumped down from a fixed base between heap and
     * stack. */
    vm->mmap_top = VM_MMAP_BASE;

    /* The stack: one page mapped at the top, growable downward to
     * stack_limit. */
    vm->stack_limit = stack_top - stack_growth;
    return vm_insert(vm, stack_top, stack_top + PAGE_SIZE, VM_PROT_READ | VM_PROT_WRITE, VM_STACK);
}
