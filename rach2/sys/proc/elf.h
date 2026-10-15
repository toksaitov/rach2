/*
 * elf.h: the ELF structures and constants the kernel reads to load a
 * program and writes in a core file, and the interface of the loader.
 *
 * The programs are ELF32 on i686 and ELF64 on x86-64, the class `ELF_CLASS`
 * names (arch/arch.h), and the types `Elf_Ehdr` and `Elf_Phdr` are that
 * class's headers.
 *
 * Reference: ELF specification (TIS Committee, v1.2), and the System V ABI's
 * ELF-64 Object File Format.
 */

#ifndef _ELF_H_
#define _ELF_H_

#include "../arch/arch.h"
#include "../lib/kint.h"
#include "../lib/kdef.h"

/*
 * ELF Identification (e_ident) indices
 */
#define EI_MAG0       0  /* File identification byte 0 */
#define EI_MAG1       1  /* File identification byte 1 */
#define EI_MAG2       2  /* File identification byte 2 */
#define EI_MAG3       3  /* File identification byte 3 */
#define EI_CLASS      4  /* File class */
#define EI_DATA       5  /* Data encoding */
#define EI_VERSION    6  /* File version */
#define EI_OSABI      7  /* OS/ABI identification */
#define EI_ABIVERSION 8  /* ABI version */
#define EI_PAD        9  /* Start of padding bytes */
#define EI_NIDENT     16 /* Size of e_ident[] */

/*
 * ELF Magic number bytes
 */
#define ELFMAG0 0x7F /* e_ident[EI_MAG0] */
#define ELFMAG1 'E'  /* e_ident[EI_MAG1] */
#define ELFMAG2 'L'  /* e_ident[EI_MAG2] */
#define ELFMAG3 'F'  /* e_ident[EI_MAG3] */

/*
 * ELF Class (e_ident[EI_CLASS])
 */
#define ELFCLASSNONE 0 /* Invalid class */
#define ELFCLASS32   1 /* 32-bit objects */
#define ELFCLASS64   2 /* 64-bit objects */

/*
 * Data encoding (e_ident[EI_DATA])
 */
#define ELFDATANONE 0 /* Invalid data encoding */
#define ELFDATA2LSB 1 /* 2's complement, little endian */
#define ELFDATA2MSB 2 /* 2's complement, big endian */

/*
 * ELF Version (e_ident[EI_VERSION])
 */
#define EV_NONE    0 /* Invalid version */
#define EV_CURRENT 1 /* Current version */

/*
 * ELF Object file type (e_type)
 */
#define ET_NONE   0      /* No file type */
#define ET_REL    1      /* Relocatable file */
#define ET_EXEC   2      /* Executable file */
#define ET_DYN    3      /* Shared object file */
#define ET_CORE   4      /* Core file */
#define ET_LOPROC 0xFF00 /* Processor-specific */
#define ET_HIPROC 0xFFFF /* Processor-specific */

/*
 * Machine type (e_machine)
 */
#define EM_NONE   0  /* No machine */
#define EM_386    3  /* Intel 80386 */
#define EM_X86_64 62 /* AMD x86-64 */

/*
 * Program header types (p_type)
 */
#define PT_NULL    0          /* Unused entry */
#define PT_LOAD    1          /* Loadable segment */
#define PT_DYNAMIC 2          /* Dynamic linking information */
#define PT_INTERP  3          /* Interpreter path */
#define PT_NOTE    4          /* Auxiliary information */
#define PT_SHLIB   5          /* Reserved */
#define PT_PHDR    6          /* Program header table itself */
#define PT_LOPROC  0x70000000 /* Processor-specific */
#define PT_HIPROC  0x7FFFFFFF /* Processor-specific */

/*
 * Program header flags (p_flags)
 */
#define PF_X 0x1 /* Execute permission */
#define PF_W 0x2 /* Write permission */
#define PF_R 0x4 /* Read permission */

/*
 * ELF32 Header Structure (52 bytes)
 */
#pragma pack(push, 1)
typedef struct {
    uint8_t e_ident[EI_NIDENT]; /* ELF identification */
    uint16_t e_type;            /* Object file type */
    uint16_t e_machine;         /* Machine type */
    uint32_t e_version;         /* Object file version */
    uint32_t e_entry;           /* Entry point address */
    uint32_t e_phoff;           /* Program header offset */
    uint32_t e_shoff;           /* Section header offset */
    uint32_t e_flags;           /* Processor-specific flags */
    uint16_t e_ehsize;          /* ELF header size */
    uint16_t e_phentsize;       /* Program header entry size */
    uint16_t e_phnum;           /* Number of program headers */
    uint16_t e_shentsize;       /* Section header entry size */
    uint16_t e_shnum;           /* Number of section headers */
    uint16_t e_shstrndx;        /* Section name string table index */
} Elf32_Ehdr;
#pragma pack(pop)

/*
 * ELF32 Program Header Structure (32 bytes)
 */
#pragma pack(push, 1)
typedef struct {
    uint32_t p_type;   /* Segment type */
    uint32_t p_offset; /* Segment file offset */
    uint32_t p_vaddr;  /* Segment virtual address */
    uint32_t p_paddr;  /* Segment physical address (unused) */
    uint32_t p_filesz; /* Segment size in file */
    uint32_t p_memsz;  /* Segment size in memory */
    uint32_t p_flags;  /* Segment flags */
    uint32_t p_align;  /* Segment alignment */
} Elf32_Phdr;
#pragma pack(pop)

/*
 * ELF64 Header Structure (64 bytes)
 */
#pragma pack(push, 1)
typedef struct {
    uint8_t e_ident[EI_NIDENT]; /* ELF identification */
    uint16_t e_type;            /* Object file type */
    uint16_t e_machine;         /* Machine type */
    uint32_t e_version;         /* Object file version */
    uint64_t e_entry;           /* Entry point address */
    uint64_t e_phoff;           /* Program header offset */
    uint64_t e_shoff;           /* Section header offset */
    uint32_t e_flags;           /* Processor-specific flags */
    uint16_t e_ehsize;          /* ELF header size */
    uint16_t e_phentsize;       /* Program header entry size */
    uint16_t e_phnum;           /* Number of program headers */
    uint16_t e_shentsize;       /* Section header entry size */
    uint16_t e_shnum;           /* Number of section headers */
    uint16_t e_shstrndx;        /* Section name string table index */
} Elf64_Ehdr;
#pragma pack(pop)

/*
 * ELF64 Program Header Structure (56 bytes), whose flags come second
 */
#pragma pack(push, 1)
typedef struct {
    uint32_t p_type;   /* Segment type */
    uint32_t p_flags;  /* Segment flags */
    uint64_t p_offset; /* Segment file offset */
    uint64_t p_vaddr;  /* Segment virtual address */
    uint64_t p_paddr;  /* Segment physical address (unused) */
    uint64_t p_filesz; /* Segment size in file */
    uint64_t p_memsz;  /* Segment size in memory */
    uint64_t p_align;  /* Segment alignment */
} Elf64_Phdr;
#pragma pack(pop)

/* The headers of the class the architecture's programs have. */
#if ELF_CLASS == ELFCLASS64
typedef Elf64_Ehdr Elf_Ehdr;
typedef Elf64_Phdr Elf_Phdr;
#else
typedef Elf32_Ehdr Elf_Ehdr;
typedef Elf32_Phdr Elf_Phdr;
#endif

/**
 * Whether the `size` bytes at `data` are an ELF program of type ET_EXEC
 * this kernel runs: the magic, the class `ELF_CLASS`, little-endian, the
 * current version, the machine `ELF_MACHINE`, a nonzero entry point, and a
 * nonempty program header table of entries of this class's size that lies
 * wholly within the bytes. The debug shell's `elf` command reads a
 * program held in memory with it and the three functions below.
 */
bool elf_check(const void *data, uint32_t size);

/** Entry point virtual address; valid only after `elf_check`. */
uintptr_t elf_entry_point(const void *data);

/** Pointer to the program header table, or NULL if there is none. The table
 * is not bounded here; `elf_check` bounds it. */
const Elf_Phdr *elf_program_headers(const void *data);

/** Number of program headers (`e_phnum`). */
uint16_t elf_program_header_count(const void *data);

struct page_dir;

/* The most program headers `elf_read_layout` and the loader take: the
 * length of the array of them each has on its stack. */
#define ELF_MAX_PHNUM 32

/*
 * Byte-range reader for the segment loader: copy `count` bytes from source
 * offset `off` into `dst`. Returns the bytes copied, fewer than `count` only
 * at the end of the file, or a negative value when the read failed. execve
 * reads through the VFS, and the boot from a KRFFS file or through the
 * root's mount (kernel.c), so the file is never read into one buffer.
 */
typedef int (*elf_read_fn)(void *ctx, void *dst, uint32_t count, uint32_t off);

/** The context of a reader of a program on the KRFFS root: its name. */
struct exec_krffs_ctx {
    const char *name;
};

/* Maximum PT_LOAD segments recorded per image. */
#define ELF_MAX_SEGS 8

/* One recorded PT_LOAD segment, page-aligned, so exec can build a VMA for it
 * with the right protection. */
typedef struct elf_seg {
    uintptr_t start; /* page-aligned segment start (inclusive) */
    uintptr_t end;   /* page-aligned segment end (exclusive)   */
    uint32_t flags;  /* ELF p_flags (PF_R/PF_W/PF_X)           */
} elf_seg_t;

/*
 * Metadata the segment loader records for the auxiliary vector and the
 * address-space map (VMAs).
 */
typedef struct {
    uintptr_t entry;      /* AT_ENTRY: program entry point */
    uintptr_t phdr_vaddr; /* AT_PHDR: vaddr of the program header table */
    uint32_t phent;       /* AT_PHENT: size in bytes of one program header */
    uint32_t phnum;       /* AT_PHNUM: number of program headers */
    uintptr_t image_end;  /* page-aligned end of all loaded segments (heap base) */
    int nsegs;            /* number of recorded PT_LOAD segments */
    elf_seg_t segs[ELF_MAX_SEGS];
} elf_image_t;

/**
 * Whether the `len` bytes at `ehdr` begin with the ELF header of an ET_EXEC
 * or ET_DYN program this kernel loads: the checks of `elf_check` the header
 * alone can answer, with a program header table offset that fits in 32
 * bits. The table is not bounded against the file and the entry point is
 * not checked; the loader, which reads the rest of the file on demand,
 * fails a short read of the table and checks the entry against the user
 * image window. execve uses it to tell a program from a script.
 */
bool elf_valid_exec(const void *ehdr, uint32_t len);

/* The longest interpreter path PT_INTERP may name, its NUL included. */
#define ELF_INTERP_MAX 256

/*
 * What exec reads of an image before it loads it: its type, the page span
 * of its PT_LOAD segments, and the interpreter PT_INTERP names, "" for none.
 */
typedef struct {
    uint16_t type;
    uintptr_t lo;
    uintptr_t hi;
    char interp[ELF_INTERP_MAX];
} elf_layout_t;

/** Read an image's layout, and with `interp` the interpreter its PT_INTERP
 * names; without it a PT_INTERP is not read, as for an interpreter, whose
 * own one is ignored. 0, -ENOEXEC for an image this kernel does not load, an
 * interpreter path that is not an absolute path within the limit or a file
 * that ends before its headers, or -EIO when a read fails. */
int elf_read_layout(elf_read_fn readfn, void *ctx, int interp, elf_layout_t *out);

/** The address at run time of the program header table at file offset
 * `phoff`, AT_PHDR, where a static C library finds its PT_TLS: its place in
 * the PT_LOAD whose file bytes contain the whole table, or 0 for none. */
uintptr_t elf_phdr_vaddr(const Elf_Phdr *phdrs, uint16_t phnum, uint64_t phoff);

/**
 * Load an ELF program into `dir`, reading it through `read` with `ctx`, and
 * describe it in `out`: the segment table, the image's end (the heap's
 * base), the entry point and the program header geometry.
 *
 * The file is read on demand and never held whole, so its size has no
 * bound. Each PT_LOAD must be well formed and lie in the user image window,
 * clear of the kernel and of the stack, and start at or after the
 * page-aligned end of the one before. A page that no segment with PF_W
 * covers is made read-only, so a user write to it faults and kills the
 * process, and one that no segment with PF_X covers is made not
 * executable. The image needs at least one PT_LOAD, a program header table
 * inside one, for AT_PHDR, and an entry point inside the window.
 *
 * 0; -ENOEXEC for a malformed image or one outside these rules, one with
 * more than ELF_MAX_PHNUM program headers or ELF_MAX_SEGS PT_LOADs, or a
 * file that ends before the bytes its headers name; -EIO when a read
 * fails; -ENOMEM when a page, a page table or a temporary mapping could not
 * be had; -EINVAL without a reader, a directory or `out`. On failure the
 * pages already mapped stay in `dir`, for the caller to free with the
 * address space.
 */
int elf_load_image(elf_read_fn read, void *ctx, struct page_dir *dir, elf_image_t *out);

/** The same with every address moved by `bias`, page-aligned, which must be
 * 0 for an ET_EXEC image: an ET_DYN program or interpreter at its base.
 * Another `bias` is -ENOEXEC. */
int elf_load_image_at(elf_read_fn read, void *ctx, struct page_dir *dir, uintptr_t bias,
                      elf_image_t *out);

struct vm_space; /* mm/vm.h */

/** Add the regions of a loaded image's segments to `vm`, each with the
 * protection of its `p_flags`, a code region for one with PF_X and a data
 * region for another: an interpreter's, beside its program's. 0, or -1 when
 * a region could not be recorded (`vm_insert`). */
int elf_add_vmas(struct vm_space *vm, const elf_image_t *img);

/**
 * Start the regions of a new address space `vm` from a loaded image: the
 * segments' (`elf_add_vmas`), an empty heap at the image's end, which brk
 * grows, and a stack region of one page at `stack_top` that may grow down
 * to `stack_top - stack_growth`. 0, or -1 when a region could not be
 * recorded (`vm_insert`).
 */
int elf_setup_vmas(struct vm_space *vm, const elf_image_t *img, uintptr_t stack_top,
                   uintptr_t stack_growth);

#endif /* _ELF_H_ */
