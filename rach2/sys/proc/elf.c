/*
 * elf.c - ELF32 executable validation and loading for user programs.
 */

#include "elf.h"
#include "../mm/paging.h"
#include "../mm/vm.h"
#include "../lib/kstring.h"
#include "../serial.h"

/* The user image window: a PT_LOAD segment must fit between the user/kernel
 * boundary and the bottom of the user stack (matching the kernel's
 * user_access_ok range). Segments outside it are rejected so a hostile ELF
 * cannot request kernel- or stack-range mappings. The floor is below the
 * conventional ELF base (a freestanding -Ttext=0x08048000 binary maps its
 * header page at 0x08047000). */
#define ELF_USER_MIN 0x02000000u
#define ELF_USER_MAX 0xBFFFF000u

/**
 * Report whether `data` is a valid ELF32 executable for i386.
 *
 * The image must carry the ELF magic `0x7F 'E' 'L' 'F'`, be 32-bit
 * little-endian of the current version, type `ET_EXEC` for machine `EM_386`,
 * and have a non-zero entry point and a program header table that lies wholly
 * within the first `size` bytes.
 */
bool elf_check(const void *data, uint32_t size)
{
    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)data;

    if (size < sizeof(Elf32_Ehdr)) {
        return false;
    }

    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
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

    uint32_t phdr_end = ehdr->e_phoff + (ehdr->e_phnum * ehdr->e_phentsize);
    if (phdr_end > size) {
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
 */
Elf32_Phdr *elf_program_headers(const void *data)
{
    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)data;

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        return NULL;
    }

    return (Elf32_Phdr *)((uint8_t *)data + ehdr->e_phoff);
}

uint16_t elf_program_header_count(const void *data)
{
    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)data;
    return ehdr->e_phnum;
}

/**
 * Load every PT_LOAD segment of an in-memory ELF image into `dir`.
 *
 * For each loadable segment, allocate the user pages spanning `p_vaddr` to
 * `p_vaddr + p_memsz`, copy `p_filesz` bytes from the file, and leave the rest
 * zero for the BSS. `alloc_user_page` returns a physical address inside the
 * identity-mapped region, so the copy writes through it directly.
 *
 * This is a self-test helper for a trusted, project-built image: it does not
 * apply the full hostile-image hardening (user-window bounds, segment overlap,
 * `p_filesz <= p_memsz`, protect-after-copy) that `elf_load_image` carries. The
 * real `execve` path streams through `elf_load_image`; an untrusted image must
 * never be loaded here.
 *
 * @return 0 on success, -1 on a bad directory, a segment that runs past the
 *         image, or an allocation failure.
 */
int elf_load(const void *data, uint32_t size, page_dir_t *dir)
{
    Elf32_Phdr *phdrs;
    uint16_t phdr_count;
    uint16_t i;
    const uint8_t *file_data = (const uint8_t *)data;

    if (!dir) {
        return -1;
    }

    phdrs = elf_program_headers(data);
    phdr_count = elf_program_header_count(data);

    if (!phdrs || phdr_count == 0) {
        return -1;
    }

    for (i = 0; i < phdr_count; i++) {
        Elf32_Phdr *phdr = &phdrs[i];
        uint32_t vaddr, vaddr_end, page_addr;
        uint32_t bytes_copied, bytes_to_copy;
        uint32_t file_offset;

        if (phdr->p_type != PT_LOAD) {
            continue;
        }

        /* Reject a segment that runs past the image, rejecting the wrap first
         * so a crafted p_offset cannot pass the bound by overflowing. */
        if (phdr->p_offset + phdr->p_filesz < phdr->p_offset ||
            phdr->p_offset + phdr->p_filesz > size) {
            return -1;
        }

        vaddr = phdr->p_vaddr;
        vaddr_end = vaddr + phdr->p_memsz;

        for (page_addr = (vaddr & ~(PAGE_SIZE - 1));
             page_addr < vaddr_end;
             page_addr += PAGE_SIZE) {

            uintptr_t phys_addr;

            phys_addr = alloc_user_page(dir, page_addr);
            if (phys_addr == 0) {
                return -1;
            }

            /* Zero each page so the BSS tail and any unwritten bytes read 0. */
            k_memset((void *)phys_addr, 0, PAGE_SIZE);
        }

        file_offset = phdr->p_offset;
        bytes_copied = 0;
        bytes_to_copy = phdr->p_filesz;

        while (bytes_copied < bytes_to_copy) {
            uint32_t curr_vaddr = vaddr + bytes_copied;
            uint32_t page_base = curr_vaddr & ~(PAGE_SIZE - 1);
            uint32_t page_offset = curr_vaddr & (PAGE_SIZE - 1);
            uint32_t chunk_size;
            uintptr_t phys_base;
            uint32_t pde_idx, pte_idx;
            page_table_t *pt;

            chunk_size = PAGE_SIZE - page_offset;
            if (chunk_size > bytes_to_copy - bytes_copied) {
                chunk_size = bytes_to_copy - bytes_copied;
            }

            pde_idx = page_base / PAGE_SIZE / 1024;
            pte_idx = (page_base / PAGE_SIZE) % 1024;

            pt = dir->logical_page_tables[pde_idx];

            if (!pt) {
                return -1;
            }

            phys_base = pt->pages[pte_idx] & ~0xFFF;

            k_memcpy((void *)(phys_base + page_offset),
                     file_data + file_offset + bytes_copied,
                     chunk_size);

            bytes_copied += chunk_size;
        }
    }

    return 0;
}

/**
 * Validate just the ELF header of a streamed image.
 *
 * Like `elf_check`, but it sees only the first `len` bytes (the header), so it
 * cannot bound the program header table against the whole file; instead it
 * insists `e_phentsize` equals `sizeof(Elf32_Phdr)`. Used by `elf_load_image`,
 * which reads the rest of the file on demand.
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
 * The kernel passes `AT_PHDR` to the loader, which is the in-memory address of
 * the program headers. Find the PT_LOAD segment that contains `phoff` and
 * translate. Return 0 when no segment covers it.
 */
uint32_t elf_phdr_vaddr(const Elf32_Phdr *phdrs, uint16_t phnum, uint32_t phoff)
{
    uint32_t table_bytes = (uint32_t)phnum * (uint32_t)sizeof(Elf32_Phdr);
    uint16_t i;

    for (i = 0; i < phnum; i++) {
        const Elf32_Phdr *p = &phdrs[i];

        /* The WHOLE program header table, not just its first byte, must lie
         * within one PT_LOAD, so AT_PHDR names a real, complete table the
         * loader can read. The subtractions run only after the bounds that keep
         * them from underflowing; `table_bytes` cannot overflow because `phnum`
         * is already capped at ELF_MAX_PHNUM. */
        if (p->p_type == PT_LOAD && phoff >= p->p_offset &&
            p->p_filesz >= table_bytes &&
            phoff - p->p_offset <= p->p_filesz - table_bytes) {
            return p->p_vaddr + (phoff - p->p_offset);
        }
    }
    return 0;
}

/**
 * Stream an ELF executable into `dir` and describe its layout in `out`.
 *
 * Unlike `elf_load`, the image is never held whole in memory: `readfn` pulls
 * the header, the program headers, and each segment's bytes on demand, so the
 * file size is unbounded. Every PT_LOAD is validated before any page is
 * allocated, then recorded in `out` with its page-aligned bounds and flags so
 * exec can build VMAs and so the read-only pages can be protected.
 *
 * Segments must be well formed and lie within the user image window: ascending,
 * page-disjoint, non-wrapping, and clear of the kernel and stack ranges. The
 * image needs at least one PT_LOAD, a program header table that lives inside
 * one (so `AT_PHDR` is real), and an entry point inside the window.
 *
 * @param out filled with the segment table, image end (the heap base), entry
 *            point, and program header geometry on success.
 * @return 0 on success, -1 on any malformed or out-of-policy image, a read
 *         short of the requested length, an allocation failure, or more
 *         PT_LOADs than `ELF_MAX_PHNUM`/`ELF_MAX_SEGS` slots.
 */
int elf_load_image(elf_read_fn readfn, void *ctx, page_dir_t *dir, elf_image_t *out)
{
    static Elf32_Phdr phdrs[ELF_MAX_PHNUM];
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
        Elf32_Phdr *ph = &phdrs[i];
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

        /* Reject hostile or malformed segment geometry BEFORE allocating any
         * page: wrapping address/file ranges, anything outside the user image
         * window (so a hostile ELF cannot map kernel or stack range), and
         * segments that overlap or precede an earlier one; valid static ELFs
         * have ascending, disjoint PT_LOADs. The overlap test is page-granular:
         * two PT_LOADs that merely share a page would have their common page
         * allocated twice (leaking a frame and losing the first segment's
         * bytes), so a segment must start on or after the previous one's
         * page-aligned end. */
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

        /* Record the page-aligned segment so exec can build a VMA for it with
         * the right protection, and track the top of the image (the heap base).
         * Every PT_LOAD must get a slot: an untracked segment would be loaded
         * present+writable with no VMA, so a read-only segment past the cap
         * would be silently writable. Reject rather than under-protect: the
         * static binaries we target have only a few PT_LOADs. */
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
            k_memset((void *)phys_addr, 0, PAGE_SIZE);
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

            if (readfn(ctx, (void *)(phys_base + page_offset), chunk_size,
                       file_offset + copied) != (int)chunk_size) {
                return -1;
            }
            copied += chunk_size;
        }
    }

    /* Apply real PTE permissions now that every segment's bytes are in place.
     * alloc_user_page mapped each page writable; a page covered only by
     * non-writable segments (text: read + execute) is now made read-only, so a
     * user write to it faults as a protection violation, which the fault
     * handler turns into a segfault and which copy-on-write keys on to tell a
     * write fault from a demand fault. A page shared with any writable segment
     * stays writable (writable wins), so a text/data boundary page is never
     * wrongly protected. Only the recorded (capped) segments participate; an
     * image with more PT_LOADs than slots simply leaves the rest writable. */
    {
        int si, sj;

        for (si = 0; si < out->nsegs; si++) {
            uint32_t pg;

            if (out->segs[si].flags & PF_W) {
                continue;
            }
            for (pg = out->segs[si].start; pg < out->segs[si].end;
                 pg += PAGE_SIZE) {
                int writable = 0;

                for (sj = 0; sj < out->nsegs; sj++) {
                    if ((out->segs[sj].flags & PF_W) &&
                        pg >= out->segs[sj].start && pg < out->segs[sj].end) {
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

    /* A loadable image needs at least one PT_LOAD, its program headers must lie
     * inside one (so AT_PHDR is real, which TLS setup depends on), and the
     * entry point must be in the user window. */
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
 */
void elf_setup_vmas(struct vm_space *vm, const elf_image_t *img,
                    uint32_t stack_top, uint32_t stack_growth)
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
        vm_insert(vm, img->segs[si].start, img->segs[si].end, prot, vt);
    }

    /* Heap starts empty just past the image; brk grows it on demand. */
    vm->heap_start = img->image_end;
    vm->brk = img->image_end;

    /* Anonymous mmaps are bumped down from a fixed base between heap and stack. */
    vm->mmap_top = VM_MMAP_BASE;

    /* The stack: one page mapped at the top, growable downward to stack_limit. */
    vm->stack_limit = stack_top - stack_growth;
    vm_insert(vm, stack_top, stack_top + PAGE_SIZE,
              VM_PROT_READ | VM_PROT_WRITE, VM_STACK);
}
