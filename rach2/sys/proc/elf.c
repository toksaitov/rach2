/*
 * elf.c - ELF32 executable parser
 *
 * Implements ELF header validation and parsing for loading user programs.
 */

#include "elf.h"

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
