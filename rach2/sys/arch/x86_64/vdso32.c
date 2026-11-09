/*
 * vdso32.c: the 32-bit vDSO an i386 program is given, an ELF32 shared
 * object of one page that the kernel lays out at boot around the code of
 * vdso32code.s, as a linker lays out Linux's linux-gate.so.1, beside the
 * 64-bit image of vdso.c, whose data page it shares.
 */

#include "ia32.h"
#include "vdsoimg.h"

#include "../../lib/kstring.h"
#include "../../mm/mm.h"
#include "../../mm/paging.h"
#include "../../proc/elf.h"
#include "../../proc/process.h"
#include "../../vdso.h"

/*
 * The ELF32 structures the image has that the loader's elf.h does not: a
 * section header, a dynamic entry, a symbol, and a version's definition and
 * its name, each with the layout of Linux's <elf.h> and no padding.
 */
typedef struct {
    uint32_t sh_name, sh_type, sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info, sh_addralign, sh_entsize;
} vdso32_shdr_t;

typedef struct {
    int32_t d_tag;
    uint32_t d_val;
} vdso32_dyn_t;

typedef struct {
    uint32_t st_name, st_value, st_size;
    uint8_t st_info, st_other;
    uint16_t st_shndx;
} vdso32_sym_t;

typedef struct {
    uint16_t vd_version, vd_flags, vd_ndx, vd_cnt;
    uint32_t vd_hash, vd_aux, vd_next;
} vdso32_verdef_t;

typedef struct {
    uint32_t vda_name, vda_next;
} vdso32_verdaux_t;

/* The constants of <elf.h> the image uses. */
#define SHT_PROGBITS   1
#define SHT_STRTAB     3
#define SHT_HASH       5
#define SHT_DYNAMIC    6
#define SHT_DYNSYM     11
#define SHT_GNU_verdef 0x6ffffffd
#define SHT_GNU_versym 0x6fffffff
#define SHF_ALLOC      0x2
#define SHF_EXECINSTR  0x4
#define SHN_ABS        0xfff1
#define DT_NULL        0
#define DT_HASH        4
#define DT_STRTAB      5
#define DT_SYMTAB      6
#define DT_STRSZ       10
#define DT_SYMENT      11
#define DT_SONAME      14
#define DT_VERSYM      0x6ffffff0
#define DT_VERDEF      0x6ffffffc
#define DT_VERDEFNUM   0x6ffffffd
#define STB_GLOBAL     1
#define STT_OBJECT     1
#define STT_FUNC       2
#define VER_FLG_BASE   1
#define ST_INFO(b, t)  ((uint8_t)(((b) << 4) | (t)))

/* The code of vdso32code.s: its bounds, and each function's. */
extern const char vdso32_text_start[], vdso32_text_end[];
extern const char vdso32_vsyscall[], vdso32_vsyscall_end[];
extern const char vdso32_sigreturn[], vdso32_sigreturn_end[];
extern const char vdso32_rt_sigreturn[], vdso32_rt_sigreturn_end[];
extern const char vdso32_clock_gettime[], vdso32_clock_gettime_end[];
extern const char vdso32_clock_gettime64[], vdso32_clock_gettime64_end[];
extern const char vdso32_gettimeofday[], vdso32_gettimeofday_end[];
extern const char vdso32_time[], vdso32_time_end[];
extern const char vdso32_clock_getres[], vdso32_clock_getres_end[];
extern const char vdso32_getcpu[], vdso32_getcpu_end[];

/*
 * The versions, by their index in the version table: the object's own,
 * linux-gate.so.1, then LINUX_2.6, which the time functions have, and
 * LINUX_2.5, which the kernel's entry and its signal returns have, as
 * Linux's vdso32.lds gives them. A function has one name, its global one.
 */
#define VER_BASE   1
#define VER_2_6    2
#define VER_2_5    3
#define VDSO32_VER 3u
static const char *const vdso32_versions[VDSO32_VER] = {"linux-gate.so.1", "LINUX_2.6",
                                                        "LINUX_2.5"};

static const struct vdso32_fn {
    const char *name;
    const char *start, *end;
    uint16_t version;
} vdso32_fns[] = {
    {"__vdso_clock_gettime", vdso32_clock_gettime, vdso32_clock_gettime_end, VER_2_6},
    {"__vdso_gettimeofday", vdso32_gettimeofday, vdso32_gettimeofday_end, VER_2_6},
    {"__vdso_time", vdso32_time, vdso32_time_end, VER_2_6},
    {"__vdso_clock_getres", vdso32_clock_getres, vdso32_clock_getres_end, VER_2_6},
    {"__vdso_clock_gettime64", vdso32_clock_gettime64, vdso32_clock_gettime64_end, VER_2_6},
    {"__vdso_getcpu", vdso32_getcpu, vdso32_getcpu_end, VER_2_6},
    {"__kernel_vsyscall", vdso32_vsyscall, vdso32_vsyscall_end, VER_2_5},
    {"__kernel_sigreturn", vdso32_sigreturn, vdso32_sigreturn_end, VER_2_5},
    {"__kernel_rt_sigreturn", vdso32_rt_sigreturn, vdso32_rt_sigreturn_end, VER_2_5},
};
#define VDSO32_FNS (sizeof(vdso32_fns) / sizeof(vdso32_fns[0]))

/* The symbols: the null one, one for each version but the object's own,
 * and one for each function; and the hash table's buckets. */
#define VDSO32_SYMS    (1u + (VDSO32_VER - 1u) + VDSO32_FNS)
#define VDSO32_BUCKETS 7u
#define HASH_WORDS     (2u + VDSO32_BUCKETS + VDSO32_SYMS)

/* The sections, in the order of their headers. */
enum {
    SEC_NULL,
    SEC_HASH,
    SEC_DYNSYM,
    SEC_DYNSTR,
    SEC_VERSYM,
    SEC_VERDEF,
    SEC_DYNAMIC,
    SEC_TEXT,
    SEC_SHSTRTAB,
    SEC_COUNT
};

/* The page the image is built in, its frame, and the place the next table
 * goes. */
static uint8_t *image;
static uintptr_t image_phys;
static uint32_t cursor;

/** Copy `len` bytes of `src` to the image at `off`. */
static void put(uint32_t off, const void *src, uint32_t len)
{
    k_memcpy(image + off, src, len);
}

/** Reserve `len` bytes at the cursor, aligned to `align`, and return their
 * offset. */
static uint32_t reserve(uint32_t len, uint32_t align)
{
    uint32_t off = (cursor + align - 1u) & ~(align - 1u);

    cursor = off + len;
    return off;
}

/** A string table being built: its offset in the image and its length. */
struct strtab {
    uint32_t off, len;
};

/** Add `s` to the table `t` and return its offset in the table. */
static uint32_t str_add(struct strtab *t, const char *s)
{
    uint32_t at = t->len, n = (uint32_t)k_strlen(s) + 1u;

    put(t->off + at, s, n);
    t->len += n;
    return at;
}

/** Fill section header `i` at `shoff`; an allocated section's address is
 * its offset, since the image is placed at its base. */
static void section(uint32_t shoff, int i, uint32_t name, uint32_t type, uint32_t flags,
                    uint32_t off, uint32_t size, uint32_t link, uint32_t info, uint32_t align,
                    uint32_t entsize)
{
    vdso32_shdr_t sh;

    sh.sh_name = name;
    sh.sh_type = type;
    sh.sh_flags = flags;
    sh.sh_addr = (flags & SHF_ALLOC) ? off : 0;
    sh.sh_offset = off;
    sh.sh_size = size;
    sh.sh_link = link;
    sh.sh_info = info;
    sh.sh_addralign = align;
    sh.sh_entsize = entsize;
    put(shoff + (uint32_t)i * sizeof(sh), &sh, sizeof(sh));
}

/** The ELF header and the two program headers, as the 64-bit image has
 * them: the page, read and execute, and the dynamic table, read-only. */
static void headers(uint32_t dynoff, uint32_t dynsize, uint32_t shoff)
{
    Elf32_Ehdr eh;
    Elf32_Phdr ph;

    k_memset(&eh, 0, sizeof(eh));
    eh.e_ident[EI_MAG0] = ELFMAG0;
    eh.e_ident[EI_MAG1] = ELFMAG1;
    eh.e_ident[EI_MAG2] = ELFMAG2;
    eh.e_ident[EI_MAG3] = ELFMAG3;
    eh.e_ident[EI_CLASS] = ELFCLASS32;
    eh.e_ident[EI_DATA] = ELFDATA2LSB;
    eh.e_ident[EI_VERSION] = EV_CURRENT;
    eh.e_type = ET_DYN;
    eh.e_machine = EM_386;
    eh.e_version = EV_CURRENT;
    eh.e_phoff = sizeof(eh);
    eh.e_shoff = shoff;
    eh.e_ehsize = sizeof(eh);
    eh.e_phentsize = sizeof(ph);
    eh.e_phnum = 2;
    eh.e_shentsize = sizeof(vdso32_shdr_t);
    eh.e_shnum = SEC_COUNT;
    eh.e_shstrndx = SEC_SHSTRTAB;
    put(0, &eh, sizeof(eh));

    k_memset(&ph, 0, sizeof(ph));
    ph.p_type = PT_LOAD;
    ph.p_flags = PF_R | PF_X;
    ph.p_filesz = PAGE_SIZE;
    ph.p_memsz = PAGE_SIZE;
    ph.p_align = PAGE_SIZE;
    put(sizeof(eh), &ph, sizeof(ph));
    ph.p_type = PT_DYNAMIC;
    ph.p_flags = PF_R;
    ph.p_offset = dynoff;
    ph.p_vaddr = dynoff;
    ph.p_paddr = dynoff;
    ph.p_filesz = dynsize;
    ph.p_memsz = dynsize;
    ph.p_align = 4;
    put(sizeof(eh) + sizeof(ph), &ph, sizeof(ph));
}

/** The version definitions at `off`, whose names are at `names` in the
 * string table: the object's own, marked as the base, then the two
 * versions. */
static void versions(uint32_t off, const uint32_t *names)
{
    vdso32_verdef_t vd;
    vdso32_verdaux_t va;
    uint32_t i, each = sizeof(vd) + sizeof(va);

    for (i = 0; i < VDSO32_VER; i++) {
        vd.vd_version = 1;
        vd.vd_flags = i == 0 ? VER_FLG_BASE : 0;
        vd.vd_ndx = (uint16_t)(i + 1u);
        vd.vd_cnt = 1;
        vd.vd_hash = vdso_elf_hash(vdso32_versions[i]);
        vd.vd_aux = sizeof(vd);
        vd.vd_next = i + 1u < VDSO32_VER ? each : 0;
        va.vda_name = names[i];
        va.vda_next = 0;
        put(off + i * each, &vd, sizeof(vd));
        put(off + i * each + sizeof(vd), &va, sizeof(va));
    }
}

/** Put symbol `n`, `sym`, at `symoff`, and link it into the hash table
 * `hash`, whose names are in the string table at `stroff`, as the 64-bit
 * image's `symbol` links one. */
static void symbol(uint32_t symoff, uint32_t stroff, uint32_t *hash, uint32_t n,
                   const vdso32_sym_t *sym)
{
    uint32_t b = vdso_elf_hash((const char *)image + stroff + sym->st_name) % VDSO32_BUCKETS;

    put(symoff + n * (uint32_t)sizeof(*sym), sym, sizeof(*sym));
    hash[2u + VDSO32_BUCKETS + n] = hash[2u + b];
    hash[2u + b] = n;
}

/**
 * The symbols, their versions and their hash table: the null symbol, an
 * absolute one for each version, then each function at its place in the
 * code, which is at VDSO_TEXT_OFFSET. `vnames` are the offsets of the
 * versions' names in the string table at `stroff` and `fnames` those of
 * the functions'.
 */
static void symbols(uint32_t symoff, uint32_t veroff, uint32_t hashoff, uint32_t stroff,
                    const uint32_t *vnames, const uint32_t *fnames)
{
    uint32_t hash[HASH_WORDS];
    uint16_t versym[VDSO32_SYMS];
    vdso32_sym_t sym;
    uint32_t i, n = 1;

    k_memset(hash, 0, sizeof(hash));
    k_memset(&sym, 0, sizeof(sym));
    put(symoff, &sym, sizeof(sym));
    versym[0] = 0;
    for (i = 1; i < VDSO32_VER; i++) {
        sym.st_name = vnames[i];
        sym.st_info = ST_INFO(STB_GLOBAL, STT_OBJECT);
        sym.st_shndx = SHN_ABS;
        versym[n] = (uint16_t)(i + 1u);
        symbol(symoff, stroff, hash, n++, &sym);
    }
    for (i = 0; i < VDSO32_FNS; i++) {
        const struct vdso32_fn *f = &vdso32_fns[i];

        sym.st_name = fnames[i];
        sym.st_info = ST_INFO(STB_GLOBAL, STT_FUNC);
        sym.st_shndx = SEC_TEXT;
        sym.st_value =
            VDSO_TEXT_OFFSET + (uint32_t)((uintptr_t)f->start - (uintptr_t)vdso32_text_start);
        sym.st_size = (uint32_t)((uintptr_t)f->end - (uintptr_t)f->start);
        versym[n] = f->version;
        symbol(symoff, stroff, hash, n++, &sym);
    }
    hash[0] = VDSO32_BUCKETS;
    hash[1] = VDSO32_SYMS;
    put(hashoff, hash, sizeof(hash));
    put(veroff, versym, sizeof(versym));
}

/**
 * The layout is the 64-bit image's (vdso.c): the headers, the hash table,
 * the symbols, their names, their versions and the versions' definitions,
 * the dynamic table, the code at VDSO_TEXT_OFFSET, then the section names
 * and the section headers, all inside the one page.
 */
int vdso32_build(void)
{
    static const char *const sec_names[SEC_COUNT] = {"",         ".hash",        ".dynsym",
                                                     ".dynstr",  ".gnu.version", ".gnu.version_d",
                                                     ".dynamic", ".text",        ".shstrtab"};
    uint32_t text_len = (uint32_t)((uintptr_t)vdso32_text_end - (uintptr_t)vdso32_text_start);
    uint32_t hashoff, symoff, veroff, verdefoff, dynoff, shoff, shname[SEC_COUNT];
    uint32_t vnames[VDSO32_VER], fnames[VDSO32_FNS];
    struct strtab str, shstr;
    vdso32_dyn_t dyn[10];
    uint32_t i;

    if (image) {
        return 0;
    }
    image = (uint8_t *)alloc_kernel_frame();
    if (!image) {
        return -1;
    }
    k_memset(image, 0, PAGE_SIZE);
    image_phys = virt_to_phys(image);
    cursor = sizeof(Elf32_Ehdr) + 2u * sizeof(Elf32_Phdr);
    hashoff = reserve(HASH_WORDS * 4u, 4);
    symoff = reserve(VDSO32_SYMS * sizeof(vdso32_sym_t), 4);
    str.off = reserve(0, 1);
    str.len = 0;
    (void)str_add(&str, "");
    for (i = 0; i < VDSO32_VER; i++) {
        vnames[i] = str_add(&str, vdso32_versions[i]);
    }
    for (i = 0; i < VDSO32_FNS; i++) {
        fnames[i] = str_add(&str, vdso32_fns[i].name);
    }
    cursor = str.off + str.len;
    veroff = reserve(VDSO32_SYMS * 2u, 2);
    verdefoff = reserve(VDSO32_VER * (sizeof(vdso32_verdef_t) + sizeof(vdso32_verdaux_t)), 4);
    dynoff = reserve(sizeof(dyn), 4);
    if (cursor > VDSO_TEXT_OFFSET) {
        return -1;
    }
    symbols(symoff, veroff, hashoff, str.off, vnames, fnames);
    versions(verdefoff, vnames);

    dyn[0].d_tag = DT_HASH;
    dyn[0].d_val = hashoff;
    dyn[1].d_tag = DT_STRTAB;
    dyn[1].d_val = str.off;
    dyn[2].d_tag = DT_SYMTAB;
    dyn[2].d_val = symoff;
    dyn[3].d_tag = DT_STRSZ;
    dyn[3].d_val = str.len;
    dyn[4].d_tag = DT_SYMENT;
    dyn[4].d_val = sizeof(vdso32_sym_t);
    dyn[5].d_tag = DT_SONAME;
    dyn[5].d_val = vnames[0];
    dyn[6].d_tag = DT_VERSYM;
    dyn[6].d_val = veroff;
    dyn[7].d_tag = DT_VERDEF;
    dyn[7].d_val = verdefoff;
    dyn[8].d_tag = DT_VERDEFNUM;
    dyn[8].d_val = VDSO32_VER;
    dyn[9].d_tag = DT_NULL;
    dyn[9].d_val = 0;
    put(dynoff, dyn, sizeof(dyn));

    cursor = VDSO_TEXT_OFFSET;
    put(reserve(text_len, 16), vdso32_text_start, text_len);
    shstr.off = reserve(0, 1);
    shstr.len = 0;
    for (i = 0; i < SEC_COUNT; i++) {
        shname[i] = str_add(&shstr, sec_names[i]);
    }
    cursor = shstr.off + shstr.len;
    shoff = reserve(SEC_COUNT * sizeof(vdso32_shdr_t), 4);
    if (cursor > PAGE_SIZE) {
        return -1;
    }
    section(shoff, SEC_HASH, shname[SEC_HASH], SHT_HASH, SHF_ALLOC, hashoff, HASH_WORDS * 4u,
            SEC_DYNSYM, 0, 4, 4);
    section(shoff, SEC_DYNSYM, shname[SEC_DYNSYM], SHT_DYNSYM, SHF_ALLOC, symoff,
            VDSO32_SYMS * sizeof(vdso32_sym_t), SEC_DYNSTR, 1, 4, sizeof(vdso32_sym_t));
    section(shoff, SEC_DYNSTR, shname[SEC_DYNSTR], SHT_STRTAB, SHF_ALLOC, str.off, str.len, 0, 0, 1,
            0);
    section(shoff, SEC_VERSYM, shname[SEC_VERSYM], SHT_GNU_versym, SHF_ALLOC, veroff,
            VDSO32_SYMS * 2u, SEC_DYNSYM, 0, 2, 2);
    section(shoff, SEC_VERDEF, shname[SEC_VERDEF], SHT_GNU_verdef, SHF_ALLOC, verdefoff,
            VDSO32_VER * (sizeof(vdso32_verdef_t) + sizeof(vdso32_verdaux_t)), SEC_DYNSTR,
            VDSO32_VER, 4, 0);
    section(shoff, SEC_DYNAMIC, shname[SEC_DYNAMIC], SHT_DYNAMIC, SHF_ALLOC, dynoff, sizeof(dyn),
            SEC_DYNSTR, 0, 4, sizeof(vdso32_dyn_t));
    section(shoff, SEC_TEXT, shname[SEC_TEXT], SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR,
            VDSO_TEXT_OFFSET, text_len, 0, 0, 16, 0);
    section(shoff, SEC_SHSTRTAB, shname[SEC_SHSTRTAB], SHT_STRTAB, 0, shstr.off, shstr.len, 0, 0, 1,
            0);
    headers(dynoff, sizeof(dyn), shoff);
    return 0;
}

uint32_t vdso32_image_pages(void)
{
    return image ? 1u : 0u;
}

uintptr_t vdso32_image_frame(void)
{
    return image_phys;
}

/** The address in the image at `base` of the code at `fn` in vdso32code.s. */
static uintptr_t vdso32_at(uintptr_t base, const char *fn)
{
    return base + VDSO_TEXT_OFFSET + (uintptr_t)(fn - vdso32_text_start);
}

uintptr_t vdso32_vsyscall_at(uintptr_t base)
{
    return vdso32_at(base, vdso32_vsyscall);
}

uintptr_t ia32_vdso_sigreturn(int rt)
{
    uintptr_t base = current_proc && current_proc->mm ? current_proc->mm->vdso : 0;

    if (base == 0 || !current_proc->mm->compat) {
        return 0;
    }
    return vdso32_at(base, rt ? vdso32_rt_sigreturn : vdso32_sigreturn);
}
