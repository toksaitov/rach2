/*
 * elf.c - ELF32 executable parser
 *
 * Implements ELF header validation and parsing for loading user programs.
 */

#include "elf.h"
#include "../mm/paging.h"
#include "../lib/kstring.h"
#include "../serial.h"

/* The user image window: a PT_LOAD segment must fit between the user/kernel
 * boundary and the bottom of the user stack (matching the kernel's user_access_ok
 * range). Segments outside it are rejected so a hostile ELF cannot request
 * kernel- or stack-range mappings. The floor is below the conventional ELF base
 * (a freestanding -Ttext=0x08048000 binary maps its header page at 0x08047000). */
#define ELF_USER_MIN 0x02000000u
#define ELF_USER_MAX 0xBFFFF000u

/*
 * Validate ELF header
 * Returns true if the data is a valid ELF32 executable for i386
 *
 * Checks performed:
 * 1. Size is large enough for ELF header
 * 2. Magic number is correct (0x7F 'E' 'L' 'F')
 * 3. Class is 32-bit (ELFCLASS32)
 * 4. Data encoding is little-endian (ELFDATA2LSB)
 * 5. Version is current (EV_CURRENT)
 * 6. Type is executable (ET_EXEC)
 * 7. Machine is i386 (EM_386)
 * 8. Program header offset is within file bounds
 */
bool elf_check(const void *data, uint32_t size)
{
    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)data;

    /* Check minimum size for ELF header */
    if (size < sizeof(Elf32_Ehdr)) {
        return false;
    }

    /* Check ELF magic number: 0x7F 'E' 'L' 'F' */
    if (ehdr->e_ident[EI_MAG0] != ELFMAG0 ||
        ehdr->e_ident[EI_MAG1] != ELFMAG1 ||
        ehdr->e_ident[EI_MAG2] != ELFMAG2 ||
        ehdr->e_ident[EI_MAG3] != ELFMAG3) {
        return false;
    }

    /* Check 32-bit class */
    if (ehdr->e_ident[EI_CLASS] != ELFCLASS32) {
        return false;
    }

    /* Check little-endian encoding */
    if (ehdr->e_ident[EI_DATA] != ELFDATA2LSB) {
        return false;
    }

    /* Check ELF version */
    if (ehdr->e_ident[EI_VERSION] != EV_CURRENT) {
        return false;
    }

    /* Check file type is executable */
    if (ehdr->e_type != ET_EXEC) {
        return false;
    }

    /* Check machine type is i386 */
    if (ehdr->e_machine != EM_386) {
        return false;
    }

    /* Check program header offset is within bounds */
    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        return false;
    }

    /* Verify program header table fits within file */
    uint32_t phdr_end = ehdr->e_phoff + (ehdr->e_phnum * ehdr->e_phentsize);
    if (phdr_end > size) {
        return false;
    }

    /* Check entry point is non-zero */
    if (ehdr->e_entry == 0) {
        return false;
    }

    return true;
}

/*
 * Get ELF entry point address
 * Returns the virtual address where execution should begin
 * Caller must verify the ELF is valid first using elf_check()
 */
uint32_t elf_entry_point(const void *data)
{
    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)data;
    return ehdr->e_entry;
}

/*
 * Get pointer to program header table
 * Returns NULL if invalid or no program headers
 */
Elf32_Phdr *elf_program_headers(const void *data)
{
    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)data;

    if (ehdr->e_phoff == 0 || ehdr->e_phnum == 0) {
        return NULL;
    }

    return (Elf32_Phdr *)((uint8_t *)data + ehdr->e_phoff);
}

/*
 * Get number of program headers
 */
uint16_t elf_program_header_count(const void *data)
{
    const Elf32_Ehdr *ehdr = (const Elf32_Ehdr *)data;
    return ehdr->e_phnum;
}

/*
 * Load ELF segments into user memory
 *
 * For each PT_LOAD segment:
 * 1. Allocate user pages covering vaddr to vaddr + memsz
 * 2. Copy filesz bytes from file offset to virtual address
 * 3. Zero-fill remaining bytes if memsz > filesz (BSS)
 *
 * Since alloc_user_page() returns a physical address within the
 * identity-mapped region, we can write directly to it.
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

        /* Only process PT_LOAD segments */
        if (phdr->p_type != PT_LOAD) {
            continue;
        }

        /* Validate segment fits in file */
        if (phdr->p_offset + phdr->p_filesz > size) {
            return -1;
        }

        /* Calculate virtual address range */
        vaddr = phdr->p_vaddr;
        vaddr_end = vaddr + phdr->p_memsz;

        /* Allocate pages for the entire segment */
        for (page_addr = (vaddr & ~(PAGE_SIZE - 1));
             page_addr < vaddr_end;
             page_addr += PAGE_SIZE) {

            uintptr_t phys_addr;

            phys_addr = alloc_user_page(dir, page_addr);
            if (phys_addr == 0) {
                return -1;
            }

            /* Zero the entire page first (for BSS and alignment) */
            k_memset((void *)phys_addr, 0, PAGE_SIZE);
        }

        /* Copy file content to memory */
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

            /* Calculate how many bytes to copy in this page */
            chunk_size = PAGE_SIZE - page_offset;
            if (chunk_size > bytes_to_copy - bytes_copied) {
                chunk_size = bytes_to_copy - bytes_copied;
            }

            /* Find physical address for this virtual page */
            pde_idx = page_base / PAGE_SIZE / 1024;
            pte_idx = (page_base / PAGE_SIZE) % 1024;

            pt = dir->logical_page_tables[pde_idx];

            if (!pt) {
                return -1;
            }

            phys_base = pt->pages[pte_idx] & ~0xFFF;

            /* Copy data to physical address */
            k_memcpy((void *)(phys_base + page_offset),
                     file_data + file_offset + bytes_copied,
                     chunk_size);

            bytes_copied += chunk_size;
        }
    }

    return 0;
}

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

uint32_t elf_phdr_vaddr(const Elf32_Phdr *phdrs, uint16_t phnum, uint32_t phoff)
{
    uint16_t i;

    for (i = 0; i < phnum; i++) {
        const Elf32_Phdr *p = &phdrs[i];

        /* phoff - p_offset avoids the p_offset + p_filesz overflow. */
        if (p->p_type == PT_LOAD && phoff >= p->p_offset &&
            phoff - p->p_offset < p->p_filesz) {
            return p->p_vaddr + (phoff - p->p_offset);
        }
    }
    return 0;
}

int elf_load_image(elf_read_fn readfn, void *ctx, page_dir_t *dir, elf_image_t *out)
{
    static Elf32_Phdr phdrs[ELF_MAX_PHNUM];
    Elf32_Ehdr ehdr;
    uint16_t i;
    uint32_t ph_bytes;
    uint32_t prev_end = ELF_USER_MIN; /* segments must ascend, no overlap */
    int nload = 0;

    if (!readfn || !dir || !out) {
        return -1;
    }

    /* ELF header */
    if (readfn(ctx, &ehdr, sizeof(ehdr), 0) != (int)sizeof(ehdr)) {
        return -1;
    }
    if (!elf_valid_exec(&ehdr, sizeof(ehdr)) || ehdr.e_phnum > ELF_MAX_PHNUM) {
        return -1;
    }

    /* Program headers */
    ph_bytes = (uint32_t)ehdr.e_phnum * sizeof(Elf32_Phdr);
    if (readfn(ctx, phdrs, ph_bytes, ehdr.e_phoff) != (int)ph_bytes) {
        return -1;
    }

    /* Each PT_LOAD: allocate and zero its pages, then stream filesz bytes in
     * directly from the file -- no whole-file buffer, so size is unbounded. */
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
         * segments that overlap or precede an earlier one -- valid static ELFs
         * have ascending, disjoint PT_LOADs. */
        if (vaddr_end < vaddr || ph->p_offset + ph->p_filesz < ph->p_offset) {
            return -1;
        }
        if (vaddr < ELF_USER_MIN || vaddr_end > ELF_USER_MAX) {
            return -1;
        }
        if (vaddr < prev_end) {
            return -1;
        }
        prev_end = vaddr_end;
        nload++;

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

    /* A loadable image needs at least one PT_LOAD, its program headers must lie
     * inside one (so AT_PHDR is real -- Phase 2 TLS depends on it), and the
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
