/*
 * vdso.c: the x86-64's vDSO image, a shared object of one page that the
 * kernel lays out at boot around the code of vdsocode.s, as a linker would lay
 * out Linux's linux-vdso.so.1, so that a C library, GDB and readelf read it
 * as they read that one (../../vdso.h); and the dispatch to the 32-bit image
 * of vdso32.c, which the boot builds with it.
 */

#include "../../vdso.h"
#include "vdsoimg.h"

#include "../../lib/kstring.h"
#include "../../mm/paging.h"
#include "../../proc/elf.h"
#include "../x86/kvmclock.h"
#include "../x86/tsc.h"

/*
 * The ELF structures the image has that the loader's elf.h does not: a
 * section header, a dynamic entry, a symbol, and a version's definition and
 * its name. Each has the layout of Linux's <elf.h> and no padding.
 */
typedef struct {
    uint32_t sh_name, sh_type;
    uint64_t sh_flags, sh_addr, sh_offset, sh_size;
    uint32_t sh_link, sh_info;
    uint64_t sh_addralign, sh_entsize;
} vdso_shdr_t;

typedef struct {
    int64_t d_tag;
    uint64_t d_val;
} vdso_dyn_t;

typedef struct {
    uint32_t st_name;
    uint8_t st_info, st_other;
    uint16_t st_shndx;
    uint64_t st_value, st_size;
} vdso_sym_t;

typedef struct {
    uint16_t vd_version, vd_flags, vd_ndx, vd_cnt;
    uint32_t vd_hash, vd_aux, vd_next;
} vdso_verdef_t;

typedef struct {
    uint32_t vda_name, vda_next;
} vdso_verdaux_t;

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
#define STB_WEAK       2
#define STT_OBJECT     1
#define STT_FUNC       2
#define VER_FLG_BASE   1
#define ST_INFO(b, t)  ((uint8_t)(((b) << 4) | (t)))

/* The code of vdsocode.s: its bounds, and each function's. */
extern const char vdso_text_start[], vdso_text_end[];
extern const char vdso_clock_gettime[], vdso_clock_gettime_end[];
extern const char vdso_gettimeofday[], vdso_gettimeofday_end[];
extern const char vdso_time[], vdso_time_end[];
extern const char vdso_getcpu[], vdso_getcpu_end[];
extern const char vdso_clock_getres[], vdso_clock_getres_end[];

/*
 * The functions the image gives, each under two names, as Linux's x86-64
 * vDSO gives them: `__vdso_` and the call's name, which is global, and the
 * call's name alone, which is weak. Both are of the version LINUX_2.6,
 * which musl and GNU libc ask for.
 */
static const struct vdso_fn {
    const char *name; /* with the `__vdso_` prefix */
    const char *start, *end;
} vdso_fns[] = {
    {"__vdso_clock_gettime", vdso_clock_gettime, vdso_clock_gettime_end},
    {"__vdso_gettimeofday", vdso_gettimeofday, vdso_gettimeofday_end},
    {"__vdso_time", vdso_time, vdso_time_end},
    {"__vdso_getcpu", vdso_getcpu, vdso_getcpu_end},
    {"__vdso_clock_getres", vdso_clock_getres, vdso_clock_getres_end},
};
#define VDSO_FNS    (sizeof(vdso_fns) / sizeof(vdso_fns[0]))
#define VDSO_PREFIX 7u /* the length of `__vdso_` */

/* The symbols: the null one, the version's, and two for each function;
 * and the hash table's buckets, of which any count works. */
#define VDSO_SYMS    (2u + 2u * VDSO_FNS)
#define VDSO_BUCKETS 7u

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

uint32_t vdso_elf_hash(const char *name)
{
    uint32_t h = 0, g;

    while (*name) {
        h = (h << 4) + (uint8_t)*name++;
        g = h & 0xf0000000u;
        if (g) {
            h ^= g >> 24;
        }
        h &= ~g;
    }
    return h;
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

/** Fill section header `i` at `shoff`. */
static void section(uint32_t shoff, int i, uint32_t name, uint32_t type, uint64_t flags,
                    uint32_t off, uint32_t size, uint32_t link, uint32_t info, uint32_t align,
                    uint32_t entsize)
{
    vdso_shdr_t sh;

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

/** The ELF header and the two program headers: the one segment, read and
 * execute, which is the whole page, and the dynamic table, read-only, as
 * GNU libc requires of a vDSO it cannot write. */
static void headers(uint32_t dynoff, uint32_t dynsize, uint32_t shoff)
{
    Elf64_Ehdr eh;
    Elf64_Phdr ph;

    k_memset(&eh, 0, sizeof(eh));
    eh.e_ident[0] = 0x7f;
    eh.e_ident[1] = 'E';
    eh.e_ident[2] = 'L';
    eh.e_ident[3] = 'F';
    eh.e_ident[4] = 2; /* ELFCLASS64 */
    eh.e_ident[5] = 1; /* ELFDATA2LSB */
    eh.e_ident[6] = 1; /* EV_CURRENT */
    eh.e_type = ET_DYN;
    eh.e_machine = EM_X86_64;
    eh.e_version = 1;
    eh.e_phoff = sizeof(eh);
    eh.e_shoff = shoff;
    eh.e_ehsize = sizeof(eh);
    eh.e_phentsize = sizeof(ph);
    eh.e_phnum = 2;
    eh.e_shentsize = sizeof(vdso_shdr_t);
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
    ph.p_align = 8;
    put(sizeof(eh) + sizeof(ph), &ph, sizeof(ph));
}

/** The two versions: the object's own, linux-vdso.so.1, index 1, and
 * LINUX_2.6, index 2, which every symbol has; `names` are their offsets in
 * the string table. */
static void versions(uint32_t off, const uint32_t names[2])
{
    static const char *const text[2] = {"linux-vdso.so.1", "LINUX_2.6"};
    vdso_verdef_t vd;
    vdso_verdaux_t va;
    uint32_t i, each = sizeof(vd) + sizeof(va);

    for (i = 0; i < 2u; i++) {
        vd.vd_version = 1;
        vd.vd_flags = i == 0 ? VER_FLG_BASE : 0;
        vd.vd_ndx = (uint16_t)(i + 1u);
        vd.vd_cnt = 1;
        vd.vd_hash = vdso_elf_hash(text[i]);
        vd.vd_aux = sizeof(vd);
        vd.vd_next = i == 0 ? each : 0;
        va.vda_name = names[i];
        va.vda_next = 0;
        put(off + i * each, &vd, sizeof(vd));
        put(off + i * each + sizeof(vd), &va, sizeof(va));
    }
}

/** The hash table's words: the counts of buckets and of chains, the
 * buckets, then a chain for each symbol. */
#define HASH_WORDS (2u + VDSO_BUCKETS + VDSO_SYMS)

/** Put symbol `n`, `sym`, at `symoff`, of version LINUX_2.6, and link it into
 * the hash table `hash`, whose names are in the string table at `stroff`:
 * its chain gets the symbol last in its bucket, and it becomes the last, so
 * a lookup walks a bucket from the last symbol back. */
static void symbol(uint32_t symoff, uint32_t stroff, uint32_t *hash, uint16_t *versym, uint32_t n,
                   const vdso_sym_t *sym)
{
    uint32_t b = vdso_elf_hash((const char *)image + stroff + sym->st_name) % VDSO_BUCKETS;

    put(symoff + n * (uint32_t)sizeof(*sym), sym, sizeof(*sym));
    versym[n] = 2;
    hash[2u + VDSO_BUCKETS + n] = hash[2u + b];
    hash[2u + b] = n;
}

/**
 * The symbols, their versions and their hash table: the null symbol, the
 * version's, absolute, then for each function its two names at its place in
 * the code, which is at VDSO_TEXT_OFFSET. `names` are the offsets of the
 * functions' names in the string table at `stroff`, and `version_name`
 * that of LINUX_2.6. The name without the prefix is the tail of the one
 * with it, as a linker merges them.
 */
static void symbols(uint32_t symoff, uint32_t veroff, uint32_t hashoff, uint32_t stroff,
                    uint32_t version_name, const uint32_t *names)
{
    uint32_t hash[HASH_WORDS];
    uint16_t versym[VDSO_SYMS];
    vdso_sym_t sym;
    uint32_t i, n = 1;

    k_memset(hash, 0, sizeof(hash));
    k_memset(&sym, 0, sizeof(sym));
    put(symoff, &sym, sizeof(sym));
    versym[0] = 0;
    sym.st_name = version_name;
    sym.st_info = ST_INFO(STB_GLOBAL, STT_OBJECT);
    sym.st_shndx = SHN_ABS;
    symbol(symoff, stroff, hash, versym, n++, &sym);
    for (i = 0; i < VDSO_FNS; i++) {
        const struct vdso_fn *f = &vdso_fns[i];

        sym.st_name = names[i];
        sym.st_info = ST_INFO(STB_GLOBAL, STT_FUNC);
        sym.st_shndx = SEC_TEXT;
        sym.st_value = VDSO_TEXT_OFFSET + ((uintptr_t)f->start - (uintptr_t)vdso_text_start);
        sym.st_size = (uintptr_t)f->end - (uintptr_t)f->start;
        symbol(symoff, stroff, hash, versym, n++, &sym);
        sym.st_name = names[i] + VDSO_PREFIX;
        sym.st_info = ST_INFO(STB_WEAK, STT_FUNC);
        symbol(symoff, stroff, hash, versym, n++, &sym);
    }
    hash[0] = VDSO_BUCKETS;
    hash[1] = VDSO_SYMS;
    put(hashoff, hash, sizeof(hash));
    put(veroff, versym, sizeof(versym));
}

/**
 * The layout is that of a linked shared object: the headers, the hash
 * table, the symbols, their names, their versions and the versions'
 * definitions, the dynamic table, the code at VDSO_TEXT_OFFSET, then the
 * section names and the section headers, all inside the one page, whose
 * address is the image's base, so that every address in it is its offset.
 * The 32-bit image is built after it (`vdso32_build`), on a kernel with the
 * i386 ABI (RACH2_IA32).
 */
int vdso_build(void)
{
    static const char *const sec_names[SEC_COUNT] = {"",         ".hash",        ".dynsym",
                                                     ".dynstr",  ".gnu.version", ".gnu.version_d",
                                                     ".dynamic", ".text",        ".shstrtab"};
    uint32_t text_len = (uint32_t)((uintptr_t)vdso_text_end - (uintptr_t)vdso_text_start);
    uint32_t hashoff, symoff, veroff, verdefoff, dynoff, shoff, names[2], shname[SEC_COUNT];
    uint32_t fn_names[VDSO_FNS];
    struct strtab str, shstr;
    vdso_dyn_t dyn[10];
    int i;

    if (image) {
        return 0;
    }
    image = (uint8_t *)alloc_kernel_frame();
    if (!image) {
        return -1;
    }
    k_memset(image, 0, PAGE_SIZE);
    image_phys = virt_to_phys(image);
    cursor = sizeof(Elf64_Ehdr) + 2u * sizeof(Elf64_Phdr);
    hashoff = reserve(HASH_WORDS * 4u, 8);
    symoff = reserve(VDSO_SYMS * sizeof(vdso_sym_t), 8);
    str.off = reserve(0, 1);
    str.len = 0;
    (void)str_add(&str, "");
    names[0] = str_add(&str, "linux-vdso.so.1");
    names[1] = str_add(&str, "LINUX_2.6");
    for (i = 0; i < (int)VDSO_FNS; i++) {
        fn_names[i] = str_add(&str, vdso_fns[i].name);
    }
    cursor = str.off + str.len;
    veroff = reserve(VDSO_SYMS * 2u, 2);
    verdefoff = reserve(2u * (sizeof(vdso_verdef_t) + sizeof(vdso_verdaux_t)), 4);
    dynoff = reserve(sizeof(dyn), 8);
    if (cursor > VDSO_TEXT_OFFSET) {
        return -1;
    }
    symbols(symoff, veroff, hashoff, str.off, names[1], fn_names);
    versions(verdefoff, names);

    dyn[0].d_tag = DT_HASH;
    dyn[0].d_val = hashoff;
    dyn[1].d_tag = DT_STRTAB;
    dyn[1].d_val = str.off;
    dyn[2].d_tag = DT_SYMTAB;
    dyn[2].d_val = symoff;
    dyn[3].d_tag = DT_STRSZ;
    dyn[3].d_val = str.len;
    dyn[4].d_tag = DT_SYMENT;
    dyn[4].d_val = sizeof(vdso_sym_t);
    dyn[5].d_tag = DT_SONAME;
    dyn[5].d_val = names[0];
    dyn[6].d_tag = DT_VERSYM;
    dyn[6].d_val = veroff;
    dyn[7].d_tag = DT_VERDEF;
    dyn[7].d_val = verdefoff;
    dyn[8].d_tag = DT_VERDEFNUM;
    dyn[8].d_val = 2;
    dyn[9].d_tag = DT_NULL;
    dyn[9].d_val = 0;
    put(dynoff, dyn, sizeof(dyn));

    cursor = VDSO_TEXT_OFFSET;
    put(reserve(text_len, 16), vdso_text_start, text_len);
    shstr.off = reserve(0, 1);
    shstr.len = 0;
    for (i = 0; i < SEC_COUNT; i++) {
        shname[i] = str_add(&shstr, sec_names[i]);
    }
    cursor = shstr.off + shstr.len;
    shoff = reserve(SEC_COUNT * sizeof(vdso_shdr_t), 8);
    if (cursor > PAGE_SIZE) {
        return -1;
    }
    k_memset(image + shoff, 0, sizeof(vdso_shdr_t));
    section(shoff, SEC_HASH, shname[SEC_HASH], SHT_HASH, SHF_ALLOC, hashoff, HASH_WORDS * 4u,
            SEC_DYNSYM, 0, 8, 4);
    section(shoff, SEC_DYNSYM, shname[SEC_DYNSYM], SHT_DYNSYM, SHF_ALLOC, symoff,
            VDSO_SYMS * sizeof(vdso_sym_t), SEC_DYNSTR, 1, 8, sizeof(vdso_sym_t));
    section(shoff, SEC_DYNSTR, shname[SEC_DYNSTR], SHT_STRTAB, SHF_ALLOC, str.off, str.len, 0, 0, 1,
            0);
    section(shoff, SEC_VERSYM, shname[SEC_VERSYM], SHT_GNU_versym, SHF_ALLOC, veroff,
            VDSO_SYMS * 2u, SEC_DYNSYM, 0, 2, 2);
    section(shoff, SEC_VERDEF, shname[SEC_VERDEF], SHT_GNU_verdef, SHF_ALLOC, verdefoff,
            2u * (sizeof(vdso_verdef_t) + sizeof(vdso_verdaux_t)), SEC_DYNSTR, 2, 4, 0);
    section(shoff, SEC_DYNAMIC, shname[SEC_DYNAMIC], SHT_DYNAMIC, SHF_ALLOC, dynoff, sizeof(dyn),
            SEC_DYNSTR, 0, 8, sizeof(vdso_dyn_t));
    section(shoff, SEC_TEXT, shname[SEC_TEXT], SHT_PROGBITS, SHF_ALLOC | SHF_EXECINSTR,
            VDSO_TEXT_OFFSET, text_len, 0, 0, 16, 0);
    section(shoff, SEC_SHSTRTAB, shname[SEC_SHSTRTAB], SHT_STRTAB, 0, shstr.off, shstr.len, 0, 0, 1,
            0);
    headers(dynoff, sizeof(dyn), shoff);
#if RACH2_IA32
    return vdso32_build();
#else
    return 0;
#endif
}

uint32_t vdso_image_pages(int abi)
{
#if RACH2_IA32
    if (abi == VDSO_IA32) {
        return vdso32_image_pages();
    }
#else
    (void)abi;
#endif
    return image ? 1u : 0u;
}

uintptr_t vdso_frame(int abi, uint32_t page)
{
    if (page == VDSO_DATA_PAGE) {
        return vdso_data_frame();
    }
    if (page == VDSO_KVM_PAGE) {
        return kvmclock_page_frame();
    }
#if RACH2_IA32
    return abi == VDSO_IA32 ? vdso32_image_frame() : image_phys;
#else
    (void)abi;
    return image_phys;
#endif
}
