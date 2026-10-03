/**
 * elfcheck: judge ELF artifacts against what this system will actually load.
 *
 * Two commands in one tool, because they share one parser. `elfcheck
 * [--arch ARCH] FILE...` judges user executables the way `exec` does: the
 * LOADER half is `elf_load_image`'s contract (rach2/sys/proc/elf.c) rule for
 * rule, and the POLICY half is this project's, refusing `PT_INTERP` and
 * `PT_DYNAMIC`, since the musl roots these artifacts are staged in have no
 * loader for a dynamic program, and exec refuses one whose loader it cannot
 * find. ARCH is `i686`, the default, for ELF32 i386 programs and the window
 * the i686 kernel loads into, or `x86_64` for ELF64 x86-64 programs and the
 * x86-64 user window of milestone 57's plan, [64 KiB, 0x00007FFFFFFFF000),
 * which the x86-64 loader is to have. `elfcheck
 * --kernel [--tcc] FILE` checks a kernel against what the multiboot loaders
 * (QEMU's -kernel and GRUB) read: the first checksum-valid multiboot header
 * in the search window with exactly the declared flags, exactly two
 * ascending non-overlapping PT_LOAD segments at or above 1 MiB with
 * `p_paddr == p_vaddr`, the entry inside file-backed bytes, and, where a
 * symbol table exists, `_start` equal to the entry and `_end` equal to the
 * image end. `--tcc` accepts the symbol-free TinyCC layout and adds its
 * first-load rules (file offset 0, address exactly 1 MiB).
 *
 * `--selftest` runs both batteries: the user checker's literal window
 * boundary cases for each architecture (the numbers rach2/sys/proc/elf.c
 * actually has, written out, so a case built out of a wrong constant cannot
 * pass by moving with it), and the kernel checker's synthesized fixture with
 * every malformed variant refused for its own rule, matched against the
 * diagnostic text.
 */
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ELF_MAX_PHNUM 32
#define ELF_MAX_SEGS  8
#define PAGE          0x1000u

#define MULTIBOOT_MAGIC 0x1BADB002u
#define MULTIBOOT_FLAGS 0x3u
#define LOAD_FLOOR      0x100000u
#define SEARCH_END      (8192 - 48)

#define ET_EXEC    2
#define EM_386     3
#define EM_X86_64  62
#define PT_LOAD    1
#define PT_DYNAMIC 2
#define PT_INTERP  3

static uint32_t r16(const unsigned char *p)
{
    return p[0] | p[1] << 8;
}
static uint32_t r32(const unsigned char *p)
{
    return (uint32_t)p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24;
}
static uint64_t r64(const unsigned char *p)
{
    return (uint64_t)r32(p) | (uint64_t)r32(p + 4) << 32;
}

/* ---- the user-executable checker (check_elf semantics) ---- */

/* What an architecture's loader takes: the ELF class and machine, the sizes
   of the header and of a program header, and the window exec loads into,
   from its floor up to but not including its ceiling. */
struct user_arch {
    const char *name;
    unsigned elf_class, bits, machine;
    const char *machine_name;
    unsigned ehsize, phentsize;
    uint64_t user_min, user_max;
};

static const struct user_arch user_arches[] = {
    {"i686", 1, 32, EM_386, "EM_386", 52, 32, 0x02000000u, 0xBFFFF000u},
    {"x86_64", 2, 64, EM_X86_64, "EM_X86_64", 64, 56, 0x10000u, 0x00007FFFFFFFF000ull},
};
static const struct user_arch *const arch_i686 = &user_arches[0];
static const struct user_arch *const arch_x86_64 = &user_arches[1];

static int user_problems;

static void problem(const char *name, const char *fmt, ...)
{
    va_list ap;

    fprintf(stderr, "  - %s: ", name);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    user_problems++;
}

/* Returns the number of findings; `quiet` suppresses the report so the
   selftest can count without printing. The fields are read at the offsets of
   `a`'s ELF class, so one walk serves both. */
static int user_check(const struct user_arch *a, const char *name, const unsigned char *d, size_t n,
                      int quiet)
{
    uint64_t e_entry, e_phoff, end, wrap;
    uint32_t e_type, e_machine, e_phentsize, e_phnum;
    uint32_t i, loads, phdr_covered;
    int wide = a->elf_class == 2;
    int before = user_problems;
    FILE *hold = NULL;

    if (quiet) {
        FILE *sink = fopen("/dev/null", "w");

        if (sink != NULL) {
            hold = stderr;
            stderr = sink;
        }
    }
    if (n < a->ehsize) {
        problem(name, "shorter than an ELF header");
        goto out;
    }
    if (memcmp(d,
               "\x7f"
               "ELF",
               4) != 0) {
        problem(name, "no ELF signature");
        goto out;
    }
    if (d[4] != a->elf_class)
        problem(name, "not ELFCLASS%u", a->bits);
    if (d[5] != 1)
        problem(name, "not ELFDATA2LSB");
    if (d[6] != 1)
        problem(name, "not EV_CURRENT");
    e_type = r16(d + 16);
    e_machine = r16(d + 18);
    e_entry = wide ? r64(d + 24) : r32(d + 24);
    e_phoff = wide ? r64(d + 32) : r32(d + 28);
    e_phentsize = r16(d + (wide ? 54 : 42));
    e_phnum = r16(d + (wide ? 56 : 44));
    if (e_type != ET_EXEC)
        problem(name,
                "e_type is %u, not ET_EXEC; this kernel loads no "
                "other kind",
                e_type);
    if (e_machine != a->machine)
        problem(name, "e_machine is %u, not %s", e_machine, a->machine_name);
    if (e_phoff == 0 || e_phnum == 0) {
        problem(name, "no program header table");
        goto out;
    }
    if (e_phentsize != a->phentsize) {
        problem(name, "e_phentsize is %u, not %u", e_phentsize, a->phentsize);
        goto out;
    }
    if (e_phnum > ELF_MAX_PHNUM)
        problem(name,
                "%u program headers, more than the %u this kernel "
                "reads",
                e_phnum, ELF_MAX_PHNUM);
    if (e_phoff > n || (uint64_t)e_phnum * a->phentsize > n - e_phoff) {
        problem(name, "the program header table runs past the file");
        goto out;
    }

    /* The address space a segment must not wrap: 32 bits for ELF32, and
       for ELF64 the 64 bits a sum of two fields can overflow. */
    wrap = wide ? UINT64_MAX : 0xFFFFFFFFull;
    loads = 0;
    end = 0;
    phdr_covered = 0;
    for (i = 0; i < e_phnum; i++) {
        const unsigned char *p = d + e_phoff + (uint64_t)a->phentsize * i;
        uint32_t p_type = r32(p);
        uint64_t p_offset = wide ? r64(p + 8) : r32(p + 4);
        uint64_t p_vaddr = wide ? r64(p + 16) : r32(p + 8);
        uint64_t p_filesz = wide ? r64(p + 32) : r32(p + 16);
        uint64_t p_memsz = wide ? r64(p + 40) : r32(p + 20);
        uint64_t page_start;

        if (p_type == PT_INTERP)
            problem(name, "has a PT_INTERP, so it wants a loader, which "
                          "a musl root does not have");
        if (p_type == PT_DYNAMIC)
            problem(name, "has a PT_DYNAMIC, so it expects relocation "
                          "no loader here performs");
        if (p_type != PT_LOAD)
            continue;
        loads++;
        if (loads > ELF_MAX_SEGS) {
            /* Reported once, after the loop, with the count. */
        }
        if (p_filesz > p_memsz)
            problem(name, "a segment's file size exceeds its memory size");
        if (p_offset > wrap - p_filesz || p_vaddr > wrap - p_memsz) {
            problem(name, "a segment wraps the %u-bit address space", a->bits);
            continue;
        }
        if (p_offset + p_filesz > n)
            problem(name, "a segment runs past the file");
        if (p_vaddr < a->user_min || p_vaddr + p_memsz > a->user_max) {
            if (wide)
                problem(name,
                        "a segment lies outside the window exec loads "
                        "into, [0x%llx, 0x%llx)",
                        (unsigned long long)a->user_min, (unsigned long long)a->user_max);
            else
                problem(name,
                        "a segment lies outside the window exec loads "
                        "into, [0x%08x, 0x%08x)",
                        (uint32_t)a->user_min, (uint32_t)a->user_max);
        }
        page_start = p_vaddr & ~(uint64_t)(PAGE - 1);
        if (page_start < end)
            problem(name, "the PT_LOAD segments are not ascending and "
                          "page-disjoint");
        end = (p_vaddr + p_memsz + PAGE - 1) & ~(uint64_t)(PAGE - 1);
        if (p_offset <= e_phoff &&
            e_phoff + (uint64_t)e_phnum * a->phentsize <= p_offset + p_filesz)
            phdr_covered = 1;
    }
    if (loads == 0) {
        problem(name, "no PT_LOAD segment");
        goto out;
    }
    if (loads > ELF_MAX_SEGS)
        problem(name,
                "%u PT_LOAD segments, more than the %u this kernel "
                "has slots for",
                loads, ELF_MAX_SEGS);
    if (!phdr_covered)
        problem(name, "the program header table lies outside every "
                      "PT_LOAD, so AT_PHDR would name nothing");
    if (e_entry < a->user_min || e_entry >= a->user_max)
        problem(name, "the entry point is outside the window exec loads "
                      "into");
out:
    if (hold != NULL) {
        fclose(stderr);
        stderr = hold;
    }
    return user_problems - before;
}

/* ---- the kernel checker (check_kernel semantics) ---- */

static char kfail[256];

static int kf(const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(kfail, sizeof(kfail), fmt, ap);
    va_end(ap);
    return 1;
}

struct load {
    uint32_t off, vaddr, paddr, filesz, memsz, align;
};

/**
 * Reports whether the string table entry at `off` is exactly `want`.
 *
 * A string table is a byte range inside the file and its last entry need
 * not be terminated, so the comparison stops at the end of the file as
 * well as at a NUL. A truncated or malformed table then reads as a name
 * that does not match rather than as a read past the buffer.
 */
static int name_is(const unsigned char *d, size_t n, uint64_t off, const char *want)
{
    size_t k;

    for (k = 0; want[k] != '\0'; k++) {
        if (off + k >= n || d[off + k] != (unsigned char)want[k])
            return 0;
    }
    return off + k < n && d[off + k] == '\0';
}

/* Returns 0 and fills `detail` on success; nonzero with `kfail` set on
   the first violation. */
static int kernel_check(const unsigned char *d, size_t n, int tcc, char *detail, size_t detail_len)
{
    uint32_t e_type, e_machine, e_entry, e_phoff, e_phentsize, e_phnum;
    uint32_t shoff, e_shentsize, e_shnum;
    struct load loads[64];
    uint32_t i, nloads = 0;
    uint32_t mb_off = 0, mb_flags = 0;
    int mb_found = 0;
    uint32_t start_val = 0, end_val = 0;
    int have_start = 0, have_end = 0, have_symtab = 0;
    uint32_t image_end, entry_vaddr = 0, entry_filesz = 0;
    int entry_found = 0;
    struct load tmp;

    if (n < 52 || memcmp(d,
                         "\x7f"
                         "ELF",
                         4) != 0)
        return kf("not an ELF file");
    if (d[4] != 1 || d[5] != 1)
        return kf("not a 32-bit little-endian ELF");
    e_type = r16(d + 16);
    e_machine = r16(d + 18);
    e_entry = r32(d + 24);
    e_phoff = r32(d + 28);
    shoff = r32(d + 32);
    e_phentsize = r16(d + 42);
    e_phnum = r16(d + 44);
    e_shentsize = r16(d + 46);
    e_shnum = r16(d + 48);
    if (e_type != 2)
        return kf("type is %u, not ET_EXEC", e_type);
    if (e_machine != 3)
        return kf("machine is %u, not EM_386", e_machine);
    if (e_phentsize != 32)
        return kf("program header entry size is %u, not 32", e_phentsize);
    if ((uint64_t)e_phoff + (uint64_t)e_phnum * 32 > n)
        return kf("truncated or malformed ELF headers");
    for (i = 0; i < e_phnum; i++) {
        const unsigned char *p = d + e_phoff + 32 * i;

        if (r32(p) != PT_LOAD)
            continue;
        if (nloads < 64) {
            loads[nloads].off = r32(p + 4);
            loads[nloads].vaddr = r32(p + 8);
            loads[nloads].paddr = r32(p + 12);
            loads[nloads].filesz = r32(p + 16);
            loads[nloads].memsz = r32(p + 20);
            loads[nloads].align = r32(p + 28);
        }
        nloads++;
    }
    if (nloads != 2)
        return kf("%u PT_LOAD segments, not 2", nloads);

    for (i = 0; i < 2; i++) {
        struct load *l = &loads[i];

        if (l->paddr != l->vaddr)
            return kf("load at 0x%x has p_paddr 0x%x; the loaders place "
                      "segments at the physical address",
                      l->vaddr, l->paddr);
        if (l->vaddr < LOAD_FLOOR)
            return kf("load at 0x%x is below 0x%x", l->vaddr, LOAD_FLOOR);
        if (l->filesz > l->memsz)
            return kf("load at 0x%x has p_filesz 0x%x > p_memsz 0x%x", l->vaddr, l->filesz,
                      l->memsz);
        if ((uint64_t)l->vaddr + l->memsz > 0xFFFFFFFFu)
            return kf("load at 0x%x: 0x%x bytes run past the end of the "
                      "32-bit address space",
                      l->vaddr, l->memsz);
        if ((uint64_t)l->off + l->filesz > n)
            return kf("load at 0x%x: file range 0x%x+0x%x runs past the end "
                      "of the file",
                      l->vaddr, l->off, l->filesz);
        if (l->align > 1 && (l->off % l->align) != (l->vaddr % l->align))
            return kf("load at 0x%x: offset 0x%x not congruent to the "
                      "address modulo 0x%x",
                      l->vaddr, l->off, l->align);
    }
    if (loads[1].vaddr < loads[0].vaddr) {
        tmp = loads[0];
        loads[0] = loads[1];
        loads[1] = tmp;
    }
    if (loads[1].vaddr < loads[0].vaddr + loads[0].memsz)
        return kf("loads overlap: 0x%x < 0x%x", loads[1].vaddr, loads[0].vaddr + loads[0].memsz);

    for (i = 0; i + 12 <= n && i < SEARCH_END; i += 4) {
        if (r32(d + i) == MULTIBOOT_MAGIC) {
            uint32_t flags = r32(d + i + 4), sum = r32(d + i + 8);

            if (((MULTIBOOT_MAGIC + flags + sum) & 0xFFFFFFFFu) == 0) {
                mb_found = 1;
                mb_off = i;
                mb_flags = flags;
                break;
            }
        }
    }
    if (!mb_found)
        return kf("no multiboot header with a valid checksum in the "
                  "loader's search window");
    if (mb_flags != MULTIBOOT_FLAGS)
        return kf("the first valid multiboot header (file offset 0x%x) has "
                  "flags 0x%x, not 0x%x",
                  mb_off, mb_flags, MULTIBOOT_FLAGS);

    if (shoff != 0 && e_shentsize != 0) {
        if (e_shentsize < 40)
            return kf("section header entry size is %u, under the 40 bytes "
                      "of an ELF32 section header",
                      e_shentsize);
        if ((uint64_t)shoff + (uint64_t)e_shentsize * e_shnum > n)
            return kf("truncated or malformed symbol table");
        for (i = 0; i < e_shnum; i++) {
            const unsigned char *sh = d + shoff + (uint64_t)e_shentsize * i;
            uint32_t sh_type = r32(sh + 4), sh_offset = r32(sh + 16);
            uint32_t sh_size = r32(sh + 20), sh_link = r32(sh + 24);
            uint32_t sh_entsize = r32(sh + 36);
            uint32_t str_offset, count, j;

            if (sh_type != 2)
                continue;
            have_symtab = 1;
            if (sh_link >= e_shnum || sh_entsize < 16)
                return kf("truncated or malformed symbol table");
            if ((uint64_t)sh_offset + sh_size > n)
                return kf("truncated or malformed symbol table");
            str_offset = r32(d + shoff + (uint64_t)e_shentsize * sh_link + 16);
            if (str_offset >= n)
                return kf("truncated or malformed symbol table");
            count = sh_size / sh_entsize;
            for (j = 0; j < count; j++) {
                const unsigned char *sym = d + sh_offset + (uint64_t)sh_entsize * j;
                uint32_t name_off = r32(sym), value = r32(sym + 4);

                if (name_is(d, n, (uint64_t)str_offset + name_off, "_start")) {
                    have_start = 1;
                    start_val = value;
                } else if (name_is(d, n, (uint64_t)str_offset + name_off, "_end")) {
                    have_end = 1;
                    end_val = value;
                }
            }
        }
    }
    if (!have_symtab && !tcc)
        return kf("no symbol table (the GNU-linked kernel has one)");
    if (!tcc) {
        if (!have_start)
            return kf("no _start symbol");
        if (e_entry != start_val)
            return kf("entry 0x%x is not _start 0x%x", e_entry, start_val);
    }
    for (i = 0; i < 2; i++) {
        if (loads[i].vaddr <= e_entry && e_entry < loads[i].vaddr + loads[i].memsz) {
            entry_found = 1;
            entry_vaddr = loads[i].vaddr;
            entry_filesz = loads[i].filesz;
        }
    }
    if (!entry_found)
        return kf("entry 0x%x is outside every load", e_entry);
    if (e_entry >= entry_vaddr + entry_filesz)
        return kf("entry 0x%x is in the zero-fill of its load, not in "
                  "file-backed bytes",
                  e_entry);

    image_end = loads[0].vaddr + loads[0].memsz;
    if (loads[1].vaddr + loads[1].memsz > image_end)
        image_end = loads[1].vaddr + loads[1].memsz;
    if (!tcc) {
        if (!have_end)
            return kf("no _end symbol");
        if (end_val != image_end)
            return kf("_end 0x%x is not the image end 0x%x", end_val, image_end);
    }
    if (tcc) {
        if (loads[0].off != 0)
            return kf("tcc profile: first load file offset is 0x%x, not 0", loads[0].off);
        if (loads[0].vaddr != LOAD_FLOOR)
            return kf("tcc profile: first load at 0x%x, not 0x%x", loads[0].vaddr, LOAD_FLOOR);
    }
    snprintf(detail, detail_len,
             "%s: 2 loads, entry 0x%x, _end 0x%x, multiboot header at file "
             "offset 0x%x",
             tcc ? "tcc profile" : "common profile", e_entry, image_end, mb_off);
    return 0;
}

/* ---- fixtures and the selftest ---- */

static void w32(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
    p[2] = (unsigned char)(v >> 16);
    p[3] = (unsigned char)(v >> 24);
}
static void w16(unsigned char *p, uint32_t v)
{
    p[0] = (unsigned char)v;
    p[1] = (unsigned char)(v >> 8);
}

static void w64(unsigned char *p, uint64_t v)
{
    w32(p, (uint32_t)v);
    w32(p + 4, (uint32_t)(v >> 32));
}

/* One PT_LOAD user image with the header table inside it, for the window
   boundary cases. */
static size_t craft_user(unsigned char *b, uint32_t entry, uint32_t vaddr, uint32_t memsz)
{
    uint32_t filesz = 52 + 32;

    memset(b, 0, 52 + 32);
    memcpy(b,
           "\x7f"
           "ELF\x01\x01\x01",
           7);
    w16(b + 16, ET_EXEC);
    w16(b + 18, EM_386);
    w32(b + 20, 1);
    w32(b + 24, entry);
    w32(b + 28, 52);
    w16(b + 40, 52);
    w16(b + 42, 32);
    w16(b + 44, 1);
    w32(b + 52, PT_LOAD);
    w32(b + 56, 0);
    w32(b + 60, vaddr);
    w32(b + 64, vaddr);
    w32(b + 68, filesz);
    w32(b + 72, memsz > filesz ? memsz : filesz);
    w32(b + 76, 5);
    w32(b + 80, PAGE);
    return filesz;
}

/* The same image as ELF64 x86-64, for the x86-64 window's cases. */
static size_t craft_user64(unsigned char *b, uint64_t entry, uint64_t vaddr, uint64_t memsz)
{
    uint64_t filesz = 64 + 56;

    memset(b, 0, 64 + 56);
    memcpy(b,
           "\x7f"
           "ELF\x02\x01\x01",
           7);
    w16(b + 16, ET_EXEC);
    w16(b + 18, EM_X86_64);
    w32(b + 20, 1);
    w64(b + 24, entry);
    w64(b + 32, 64);
    w16(b + 52, 64);
    w16(b + 54, 56);
    w16(b + 56, 1);
    w32(b + 64, PT_LOAD);
    w32(b + 64 + 4, 5);
    w64(b + 64 + 8, 0);
    w64(b + 64 + 16, vaddr);
    w64(b + 64 + 24, vaddr);
    w64(b + 64 + 32, filesz);
    w64(b + 64 + 40, memsz > filesz ? memsz : filesz);
    w64(b + 64 + 48, PAGE);
    return (size_t)filesz;
}

/* A minimal kernel-shaped fixture: a text load at 1 MiB, a data+bss load
   after it, one multiboot header, and a symtab with `_start` and `_end`.
   `first_off` 0 models the tcc layout. */
static size_t craft_kernel(unsigned char *b, uint32_t first_off, uint32_t mb_at, uint32_t text_size)
{
    uint32_t hdr_size = 52 + 2 * 32;
    uint32_t text_off, first_file_start, first_filesz, text_base;
    uint32_t entry_at, entry, data_off, data_vaddr, image_end;
    uint32_t sym_off, str_off, shoff, pos;
    static const char strtab[] = "\0_start\0_end";
    uint32_t strtab_len = 13;
    uint32_t data_len = 8, bss_size = 0x20;
    uint32_t i;

    if (first_off == 0) {
        text_off = hdr_size;
        first_file_start = 0;
        first_filesz = text_off + text_size;
        text_base = LOAD_FLOOR + text_off;
    } else {
        text_off = first_off;
        first_file_start = first_off;
        first_filesz = text_size;
        text_base = LOAD_FLOOR;
    }
    entry_at = mb_at + 12;
    entry = text_base + entry_at;
    data_off = ((first_file_start + first_filesz) + 0xFFF) & ~0xFFFu;
    data_vaddr = LOAD_FLOOR + (data_off - first_file_start);
    image_end = data_vaddr + data_len + bss_size;
    sym_off = data_off + data_len;
    str_off = sym_off + 3 * 16;
    shoff = str_off + strtab_len;

    memset(b, 0, shoff + 3 * 40);
    memcpy(b,
           "\x7f"
           "ELF\x01\x01\x01",
           7);
    w16(b + 16, 2);
    w16(b + 18, 3);
    w32(b + 20, 1);
    w32(b + 24, entry);
    w32(b + 28, 52);
    w32(b + 32, shoff);
    w16(b + 40, 52);
    w16(b + 42, 32);
    w16(b + 44, 2);
    w16(b + 46, 40);
    w16(b + 48, 3);
    /* ph1: the text load; ph2: data+bss. */
    w32(b + 52, PT_LOAD);
    w32(b + 56, first_file_start);
    w32(b + 60, LOAD_FLOOR);
    w32(b + 64, LOAD_FLOOR);
    w32(b + 68, first_filesz);
    w32(b + 72, first_filesz);
    w32(b + 76, 5);
    w32(b + 80, 0x1000);
    w32(b + 84, PT_LOAD);
    w32(b + 88, data_off);
    w32(b + 92, data_vaddr);
    w32(b + 96, data_vaddr);
    w32(b + 100, data_len);
    w32(b + 104, data_len + bss_size);
    w32(b + 108, 6);
    w32(b + 112, 0x1000);
    /* The multiboot header and a hlt loop at the entry. */
    pos = text_off + mb_at;
    w32(b + pos, MULTIBOOT_MAGIC);
    w32(b + pos + 4, MULTIBOOT_FLAGS);
    w32(b + pos + 8, (uint32_t)(0 - (MULTIBOOT_MAGIC + MULTIBOOT_FLAGS)));
    b[text_off + entry_at] = 0xF4;
    b[text_off + entry_at + 1] = 0xEB;
    b[text_off + entry_at + 2] = 0xFD;
    for (i = 0; i < data_len; i++)
        b[data_off + i] = 0x11;
    /* Three symbols: null, _start, _end. */
    w32(b + sym_off + 16, 1);
    w32(b + sym_off + 16 + 4, entry);
    b[sym_off + 16 + 12] = 0x10;
    w16(b + sym_off + 16 + 14, 1);
    w32(b + sym_off + 32, 8);
    w32(b + sym_off + 32 + 4, image_end);
    b[sym_off + 32 + 12] = 0x10;
    w16(b + sym_off + 32 + 14, 2);
    memcpy(b + str_off, strtab, strtab_len);
    /* Three sections: null, symtab, strtab. */
    w32(b + shoff + 40 + 4, 2);
    w32(b + shoff + 40 + 16, sym_off);
    w32(b + shoff + 40 + 20, 3 * 16);
    w32(b + shoff + 40 + 24, 2);
    w32(b + shoff + 40 + 32, 4);
    w32(b + shoff + 40 + 36, 16);
    w32(b + shoff + 80 + 4, 3);
    w32(b + shoff + 80 + 16, str_off);
    w32(b + shoff + 80 + 20, strtab_len);
    w32(b + shoff + 80 + 32, 1);
    return shoff + 3 * 40;
}

static int st_bad;

static void st_expect_kernel(const unsigned char *d, size_t n, int tcc, const char *why,
                             const char *want)
{
    char detail[160];

    if (kernel_check(d, n, tcc, detail, sizeof(detail)) == 0) {
        fprintf(stderr, "elfcheck: selftest: accepted %s\n", why);
        st_bad++;
    } else if (strstr(kfail, want) == NULL) {
        fprintf(stderr,
                "elfcheck: selftest: %s: refused for the wrong "
                "rule: %s\n",
                why, kfail);
        st_bad++;
    }
}

static int selftest(void)
{
    static const struct {
        const char *name;
        uint32_t entry, vaddr, memsz;
        int want_ok;
    } cases[] = {
        {"a segment ending exactly at the ceiling", 0x08048000, 0xBFFFE000, 0x1000, 1},
        {"a segment ending one byte past the ceiling", 0x08048000, 0xBFFFE000, 0x1001, 0},
        {"a segment inside the page exec leaves out", 0x08048000, 0xBFFFF000, 0x1000, 0},
        {"an entry point at the ceiling", 0xBFFFF000, 0x08048000, 0x1000, 0},
        {"an entry point one byte below it", 0xBFFFEFFF, 0x08048000, 0x1000, 1},
        {"a segment starting below the floor", 0x08048000, 0x01FFF000, 0x1000, 0},
        {"a segment starting exactly at the floor", 0x02000000, 0x02000000, 0x1000, 1},
        {"a segment starting one page below the floor", 0x08048000, 0x01FFF000, 0x2000, 0},
        {"an entry point exactly at the floor", 0x02000000, 0x02000000, 0x1000, 1},
        {"an entry point one byte below the floor", 0x01FFFFFF, 0x02000000, 0x1000, 0},
    };
    static const struct {
        const char *name;
        uint64_t entry, vaddr, memsz;
        int want_ok;
    } cases64[] = {
        {"x86_64: a segment ending at the ceiling", 0x400000, 0x7FFFFFFFE000ull, 0x1000, 1},
        {"x86_64: a segment ending a byte past it", 0x400000, 0x7FFFFFFFE000ull, 0x1001, 0},
        {"x86_64: a segment in the page exec leaves out", 0x400000, 0x7FFFFFFFF000ull, 0x1000, 0},
        {"x86_64: an entry point at the ceiling", 0x7FFFFFFFF000ull, 0x400000, 0x1000, 0},
        {"x86_64: an entry point one byte below it", 0x7FFFFFFFEFFFull, 0x400000, 0x1000, 1},
        {"x86_64: a segment starting below the floor", 0x400000, 0xF000, 0x1000, 0},
        {"x86_64: a segment starting exactly at the floor", 0x10000, 0x10000, 0x1000, 1},
        {"x86_64: a segment wrapping the address space", 0x400000, ~0xFFFull, 0x2000, 0},
        {"x86_64: an entry point exactly at the floor", 0x10000, 0x10000, 0x1000, 1},
        {"x86_64: an entry point one byte below the floor", 0xFFFF, 0x10000, 0x1000, 0},
    };
    unsigned char user[128];
    unsigned char good[16384], gnu[16384], mut[16384], big[32768];
    size_t goodn, gnun, bign, n32, n64;
    char detail[160];
    unsigned i;
    uint32_t hdr = 52 + 64, mb = 52 + 64 + 16, last;

    st_bad = 0;
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        size_t n = craft_user(user, cases[i].entry, cases[i].vaddr, cases[i].memsz);
        int got = user_check(arch_i686, "case", user, n, 1);

        if (cases[i].want_ok && got) {
            fprintf(stderr, "elfcheck: selftest: %s: expected a clean run\n", cases[i].name);
            st_bad++;
        } else if (!cases[i].want_ok && !got) {
            fprintf(stderr,
                    "elfcheck: selftest: %s: expected a finding, "
                    "the run was clean\n",
                    cases[i].name);
            st_bad++;
        }
    }
    for (i = 0; i < sizeof(cases64) / sizeof(cases64[0]); i++) {
        size_t n = craft_user64(user, cases64[i].entry, cases64[i].vaddr, cases64[i].memsz);
        int got = user_check(arch_x86_64, "case", user, n, 1);

        if (cases64[i].want_ok && got) {
            fprintf(stderr, "elfcheck: selftest: %s: expected a clean run\n", cases64[i].name);
            st_bad++;
        } else if (!cases64[i].want_ok && !got) {
            fprintf(stderr,
                    "elfcheck: selftest: %s: expected a finding, "
                    "the run was clean\n",
                    cases64[i].name);
            st_bad++;
        }
    }
    n64 = craft_user64(user, 0x400000, 0x400000, 0x1000);
    if (user_check(arch_i686, "case", user, n64, 1) == 0) {
        fprintf(stderr, "elfcheck: selftest: an x86_64 program passed for i686\n");
        st_bad++;
    }
    n32 = craft_user(user, 0x08048000, 0x08048000, 0x1000);
    if (user_check(arch_x86_64, "case", user, n32, 1) == 0) {
        fprintf(stderr, "elfcheck: selftest: an i686 program passed for x86_64\n");
        st_bad++;
    }

    goodn = craft_kernel(good, 0, 16, 0x100);
    if (kernel_check(good, goodn, 1, detail, sizeof(detail)) != 0 ||
        kernel_check(good, goodn, 0, detail, sizeof(detail)) != 0) {
        fprintf(stderr,
                "elfcheck: selftest: the good fixture was "
                "refused: %s\n",
                kfail);
        st_bad++;
    }
    gnun = craft_kernel(gnu, 0x1000, 16, 0x100);
    if (kernel_check(gnu, gnun, 0, detail, sizeof(detail)) != 0) {
        fprintf(stderr,
                "elfcheck: selftest: the GNU-shaped fixture was "
                "refused: %s\n",
                kfail);
        st_bad++;
    }

    memcpy(mut, good, goodn);
    w32(mut + mb + 8, 0);
    st_expect_kernel(mut, goodn, 0, "a corrupted multiboot checksum", "no multiboot header");
    memcpy(mut, good, goodn);
    w32(mut + 24, 0x90000);
    st_expect_kernel(mut, goodn, 0, "an entry that is not _start", "is not _start");
    memcpy(mut, good, goodn);
    w32(mut + 52 + 12, 0);
    st_expect_kernel(mut, goodn, 0, "a zero physical address", "p_paddr");
    memcpy(mut, good, goodn);
    w32(mut + 52 + 32 + 4, (uint32_t)goodn);
    st_expect_kernel(mut, goodn, 0, "a load past the end of the file", "past the end");
    memcpy(mut, good, goodn);
    w32(mut + 52 + 16, mb);
    st_expect_kernel(mut, goodn, 0, "an entry in its load's zero-fill", "zero-fill");
    memcpy(mut, good, goodn);
    w32(mut + mb + 4, 0x10003);
    w32(mut + mb + 8, (uint32_t)(0 - (MULTIBOOT_MAGIC + 0x10003)));
    st_expect_kernel(mut, goodn, 0, "an address-override multiboot header", "flags");
    memcpy(mut, good, goodn);
    w32(mut + hdr, MULTIBOOT_MAGIC);
    w32(mut + hdr + 4, 0x10000);
    w32(mut + hdr + 8, (uint32_t)(0 - (MULTIBOOT_MAGIC + 0x10000)));
    st_expect_kernel(mut, goodn, 0, "an earlier valid header with foreign flags", "flags");

    last = SEARCH_END - 4;
    bign = craft_kernel(big, 0, last - hdr, 0x2000);
    if (kernel_check(big, bign, 1, detail, sizeof(detail)) != 0) {
        fprintf(stderr,
                "elfcheck: selftest: a header at the last searched "
                "offset was refused: %s\n",
                kfail);
        st_bad++;
    }
    bign = craft_kernel(big, 0, (last + 4) - hdr, 0x2000);
    st_expect_kernel(big, bign, 1, "a header past the loader's search bound",
                     "no multiboot header");

    st_expect_kernel(gnu, gnun, 1, "the GNU layout under the tcc profile", "file offset");

    memcpy(mut, good, goodn);
    w32(mut + 32, 0);
    if (kernel_check(mut, goodn, 1, detail, sizeof(detail)) != 0) {
        fprintf(stderr,
                "elfcheck: selftest: the symbol-free fixture was "
                "refused under the tcc profile: %s\n",
                kfail);
        st_bad++;
    }
    st_expect_kernel(mut, goodn, 0,
                     "a kernel without a symbol table under the common "
                     "profile",
                     "no symbol table");

    memcpy(mut, good, goodn);
    w32(mut + 52 + 20, 0xFFF00000u);
    st_expect_kernel(mut, goodn, 0, "a load that wraps the address space", "32-bit address space");
    memcpy(mut, good, goodn);
    w16(mut + 46, 20);
    st_expect_kernel(mut, goodn, 0, "a short section header entry size",
                     "section header entry size");
    {
        uint32_t shoff_of = r32(good + 32);
        uint32_t symtab = shoff_of + 40, strtab = shoff_of + 80;

        memcpy(mut, good, goodn);
        w32(mut + strtab + 16, (uint32_t)goodn);
        w32(mut + r32(good + symtab + 16) + 16, 0);
        memcpy(mut + goodn, "_start", 6);
        mut[goodn + 6] = 0;
        st_expect_kernel(mut, goodn + 6, 0, "a symbol name unterminated at the end of the file",
                         "no _start symbol");
    }

    if (st_bad) {
        fprintf(stderr, "elfcheck: selftest: %d case(s) failed\n", st_bad);
        return 1;
    }
    printf("elfcheck: selftest: ok: 10 i686 and 10 x86_64 window cases, 5 accepted kernels, "
           "13 refused, each for its own rule\n");
    return 0;
}

/* ---- entry ---- */

static unsigned char *read_file(const char *path, size_t *n)
{
    FILE *f = fopen(path, "rb");
    unsigned char *buf;
    long len;

    if (f == NULL)
        return NULL;
    if (fseek(f, 0, SEEK_END) != 0 || (len = ftell(f)) < 0 || fseek(f, 0, SEEK_SET) != 0) {
        fclose(f);
        return NULL;
    }
    buf = malloc((size_t)len + 1);
    if (buf == NULL || fread(buf, 1, (size_t)len, f) != (size_t)len) {
        free(buf);
        fclose(f);
        return NULL;
    }
    fclose(f);
    *n = (size_t)len;
    return buf;
}

int main(int argc, char **argv)
{
    int i, kernel_mode = 0, tcc = 0, first = 1;
    const struct user_arch *arch = arch_i686;

    if (argc >= 2 && strcmp(argv[1], "--selftest") == 0)
        return selftest();
    if (argc >= 3 && strcmp(argv[1], "--arch") == 0) {
        size_t k;

        arch = NULL;
        for (k = 0; k < sizeof(user_arches) / sizeof(user_arches[0]); k++)
            if (strcmp(argv[2], user_arches[k].name) == 0)
                arch = &user_arches[k];
        if (arch == NULL) {
            fprintf(stderr,
                    "check-elf: unknown architecture '%s'; the architectures are "
                    "i686 and x86_64\n",
                    argv[2]);
            return 2;
        }
        first = 3;
    }
    if (argc >= 2 && strcmp(argv[1], "--kernel") == 0) {
        kernel_mode = 1;
        first = 2;
        if (argc >= 3 && strcmp(argv[2], "--tcc") == 0) {
            tcc = 1;
            first = 3;
        }
    }
    if (first >= argc) {
        fprintf(stderr, "usage: elfcheck [--arch i686|x86_64] FILE... | elfcheck --kernel "
                        "[--tcc] KERNEL | elfcheck --selftest\n");
        return 2;
    }
    if (kernel_mode) {
        size_t n;
        unsigned char *d;
        char detail[160];

        if (first != argc - 1) {
            fprintf(stderr,
                    "check-kernel: --kernel takes one kernel, not "
                    "%d\n",
                    argc - first);
            return 2;
        }
        d = read_file(argv[first], &n);
        if (d == NULL) {
            fprintf(stderr, "check-kernel: cannot read %s\n", argv[first]);
            return 2;
        }
        if (kernel_check(d, n, tcc, detail, sizeof(detail)) != 0) {
            fprintf(stderr, "check-kernel: fail: %s\n", kfail);
            free(d);
            return 1;
        }
        printf("check-kernel: ok: %s\n", detail);
        free(d);
        return 0;
    }
    for (i = first; i < argc; i++) {
        size_t n;
        unsigned char *d = read_file(argv[i], &n);

        if (d == NULL) {
            problem(argv[i], "cannot read");
            continue;
        }
        if (n == 0)
            problem(argv[i], "empty");
        else
            user_check(arch, argv[i], d, n, 0);
        free(d);
    }
    if (user_problems) {
        fprintf(stderr, "check-elf: FAIL (%d finding(s))\n", user_problems);
        return 1;
    }
    printf("check-elf: OK (loadable by this kernel, and static)\n");
    return 0;
}
