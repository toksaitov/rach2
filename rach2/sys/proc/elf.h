/*
 * elf.h - ELF32 executable format definitions
 *
 * Defines structures for parsing ELF32 executables used by the Rach2 kernel
 * to load and execute user programs from the file system.
 *
 * Reference: ELF specification (TIS Committee, v1.2)
 */

#ifndef _ELF_H_
#define _ELF_H_

#include "../lib/kint.h"
#include "../lib/kdef.h"

/*
 * ELF Identification (e_ident) indices
 */
#define EI_MAG0       0   /* File identification byte 0 */
#define EI_MAG1       1   /* File identification byte 1 */
#define EI_MAG2       2   /* File identification byte 2 */
#define EI_MAG3       3   /* File identification byte 3 */
#define EI_CLASS      4   /* File class */
#define EI_DATA       5   /* Data encoding */
#define EI_VERSION    6   /* File version */
#define EI_OSABI      7   /* OS/ABI identification */
#define EI_ABIVERSION 8   /* ABI version */
#define EI_PAD        9   /* Start of padding bytes */
#define EI_NIDENT     16  /* Size of e_ident[] */

/*
 * ELF Magic number bytes
 */
#define ELFMAG0 0x7F  /* e_ident[EI_MAG0] */
#define ELFMAG1 'E'   /* e_ident[EI_MAG1] */
#define ELFMAG2 'L'   /* e_ident[EI_MAG2] */
#define ELFMAG3 'F'   /* e_ident[EI_MAG3] */

/*
 * ELF Class (e_ident[EI_CLASS])
 */
#define ELFCLASSNONE 0  /* Invalid class */
#define ELFCLASS32   1  /* 32-bit objects */
#define ELFCLASS64   2  /* 64-bit objects */

/*
 * Data encoding (e_ident[EI_DATA])
 */
#define ELFDATANONE 0  /* Invalid data encoding */
#define ELFDATA2LSB 1  /* 2's complement, little endian */
#define ELFDATA2MSB 2  /* 2's complement, big endian */

/*
 * ELF Version (e_ident[EI_VERSION])
 */
#define EV_NONE    0  /* Invalid version */
#define EV_CURRENT 1  /* Current version */

/*
 * ELF Object file type (e_type)
 */
#define ET_NONE   0       /* No file type */
#define ET_REL    1       /* Relocatable file */
#define ET_EXEC   2       /* Executable file */
#define ET_DYN    3       /* Shared object file */
#define ET_CORE   4       /* Core file */
#define ET_LOPROC 0xFF00  /* Processor-specific */
#define ET_HIPROC 0xFFFF  /* Processor-specific */

/*
 * Machine type (e_machine)
 */
#define EM_NONE  0  /* No machine */
#define EM_386   3  /* Intel 80386 */

/*
 * Program header types (p_type)
 */
#define PT_NULL    0  /* Unused entry */
#define PT_LOAD    1  /* Loadable segment */
#define PT_DYNAMIC 2  /* Dynamic linking information */
#define PT_INTERP  3  /* Interpreter path */
#define PT_NOTE    4  /* Auxiliary information */
#define PT_SHLIB   5  /* Reserved */
#define PT_PHDR    6  /* Program header table itself */
#define PT_LOPROC  0x70000000  /* Processor-specific */
#define PT_HIPROC  0x7FFFFFFF  /* Processor-specific */

/*
 * Program header flags (p_flags)
 */
#define PF_X 0x1  /* Execute permission */
#define PF_W 0x2  /* Write permission */
#define PF_R 0x4  /* Read permission */

/*
 * ELF32 Header Structure (52 bytes)
 */
#pragma pack(push, 1)
typedef struct {
    uint8_t  e_ident[EI_NIDENT];  /* ELF identification */
    uint16_t e_type;              /* Object file type */
    uint16_t e_machine;           /* Machine type */
    uint32_t e_version;           /* Object file version */
    uint32_t e_entry;             /* Entry point address */
    uint32_t e_phoff;             /* Program header offset */
    uint32_t e_shoff;             /* Section header offset */
    uint32_t e_flags;             /* Processor-specific flags */
    uint16_t e_ehsize;            /* ELF header size */
    uint16_t e_phentsize;         /* Program header entry size */
    uint16_t e_phnum;             /* Number of program headers */
    uint16_t e_shentsize;         /* Section header entry size */
    uint16_t e_shnum;             /* Number of section headers */
    uint16_t e_shstrndx;          /* Section name string table index */
} Elf32_Ehdr;
#pragma pack(pop)

/*
 * ELF32 Program Header Structure (32 bytes)
 */
#pragma pack(push, 1)
typedef struct {
    uint32_t p_type;    /* Segment type */
    uint32_t p_offset;  /* Segment file offset */
    uint32_t p_vaddr;   /* Segment virtual address */
    uint32_t p_paddr;   /* Segment physical address (unused) */
    uint32_t p_filesz;  /* Segment size in file */
    uint32_t p_memsz;   /* Segment size in memory */
    uint32_t p_flags;   /* Segment flags */
    uint32_t p_align;   /* Segment alignment */
} Elf32_Phdr;
#pragma pack(pop)

/*
 * Validate ELF header
 * Returns true if the data is a valid ELF32 executable for i386
 */
bool elf_check(const void *data, uint32_t size);

/*
 * Get ELF entry point address
 * Returns the virtual address where execution should begin
 * Caller must verify the ELF is valid first using elf_check()
 */
uint32_t elf_entry_point(const void *data);

/*
 * Get pointer to program header table
 * Returns NULL if invalid or no program headers
 */
Elf32_Phdr *elf_program_headers(const void *data);

/*
 * Get number of program headers
 */
uint16_t elf_program_header_count(const void *data);

#endif /* _ELF_H_ */
