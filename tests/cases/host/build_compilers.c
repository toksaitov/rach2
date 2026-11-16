/* Compare prepared TinyCC with GNU binutils, retaining byte and layout rows. */
#include "build_support.h"
#include <elf.h>
#include <stdint.h>

static const char *tcc(struct fixture *f, int bits)
{
    return product_path(f, bits == 32 ? "tcc/i386-tcc" : "tcc/x86_64-tcc");
}
static int command(struct fixture *f, const char *const args[])
{
    return fx_check(f, fx_run(f, args, f->work, NULL, NULL) == 0, args[0]);
}
static int bytes(struct fixture *f, const char *elf, const char *out)
{
    const char *args[] = {"objcopy", "-O", "binary", "-j", ".text", elf, out, NULL};
    return command(f, args);
}

static char *instructions(const char *text)
{
    char *result = calloc(strlen(text) + 1, 1), *at = result;
    if (!result)
        abort();
    for (const char *p = text; *p;) {
        const char *end = strchr(p, '\n');
        if (!end)
            end = p + strlen(p);
        const char *first = memchr(p, '\t', (size_t)(end - p));
        const char *second = first ? memchr(first + 1, '\t', (size_t)(end - first - 1)) : NULL;
        if (second) {
            const char *last = memchr(second + 1, '\t', (size_t)(end - second - 1));
            if (!last)
                last = end;
            while (last > second + 1 && last[-1] == ' ')
                last--;
            size_t n = (size_t)(last - second - 1);
            memcpy(at, second + 1, n);
            at += n;
            *at++ = '\n';
        }
        p = *end ? end + 1 : end;
    }
    return result;
}

static int assembly(struct fixture *f, const char *name, const char *source, int bits,
                    const char *mode)
{
    char gnu[128], tiny[128], output[128];
    snprintf(gnu, sizeof(gnu), "%s.gnu.o", name);
    snprintf(tiny, sizeof(tiny), "%s.tcc.o", name);
    const char *as[] = {"as", bits == 32 ? "--32" : "--64", "-o", gnu, source, NULL};
    const char *cc[] = {tcc(f, bits), "-c", "-o", tiny, source, NULL};
    if (!command(f, as) || !command(f, cc))
        return 0;
    const char *gd[] = {"objdump", "-d", mode, gnu, NULL};
    const char *td[] = {"objdump", "-d", mode, tiny, NULL};
    if (!command(f, gd))
        return 0;
    char *expected = instructions(f->out ? f->out : "");
    int ok = command(f, td);
    char *actual = instructions(f->out ? f->out : "");
    fx_check(f, ok && !strcmp(expected, actual), name);
    free(expected);
    free(actual);
    snprintf(output, sizeof(output), "%s.bin", name);
    return bytes(f, tiny, output);
}

static int asm_sources(struct fixture *f)
{
    char *text = fx_read(bp(f->tree, "vendor/tcc/asm-test/insns.s"));
    if (!text)
        return 0;
    FILE *a = fopen(bp(f->work, "code16.s"), "w"), *b = fopen(bp(f->work, "code32.s"), "w");
    if (!a || !b) {
        if (a)
            fclose(a);
        if (b)
            fclose(b);
        free(text);
        return 0;
    }
    fputs(".code16\n", a);
    fputs(text, a);
    fputs(".code32\n", b);
    const char *drop[] = {"    movw %ax, %ds", "    movw %ax, %es", "    movw %ax, %ss",
                          "    sldt %ax",      "    smsw %ax",      "    str %ax",
                          "    jmp *%ax",      "    call *%ax",     NULL};
    char *save;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        int keep = 1;
        for (size_t i = 0; drop[i]; i++)
            keep &= strcmp(line, drop[i]) != 0;
        if (keep)
            fprintf(b, "%s\n", line);
    }
    free(text);
    int ok = fclose(a) == 0;
    ok &= fclose(b) == 0;
    return ok;
}

static int link_matrix(struct fixture *f, const char *name, const char *source, int bits,
                       int direct)
{
    char go[128], to[128], start[128], ge[128], te[128], ce[128], gb[128], tb[128], cb[128];
    snprintf(go, sizeof(go), "%s.gnu.o", name);
    snprintf(to, sizeof(to), "%s.tcc.o", name);
    snprintf(start, sizeof(start), "start%d.o", bits);
    snprintf(ge, sizeof(ge), "%s.gnu.elf", name);
    snprintf(te, sizeof(te), "%s.tcc.elf", name);
    snprintf(ce, sizeof(ce), "%s.tcclink.elf", name);
    snprintf(gb, sizeof(gb), "%s.gnu.bin", name);
    snprintf(tb, sizeof(tb), "%s.tcc.bin", name);
    snprintf(cb, sizeof(cb), "%s.tcclink.bin", name);
    const char *entry = strstr(name, "pcrel") ? "pcrel_start" : "trampoline_start";
    const char *as[] = {"as", bits == 32 ? "--32" : "--64", "-o", go, source, NULL};
    const char *cc[] = {tcc(f, bits), "-c", "-o", to, source, NULL};
    const char *sc[] = {tcc(f, bits), "-c", "-o", start, "start.s", NULL};
    const char *gl[] = {"ld",
                        bits == 32 ? "-melf_i386" : "-melf_x86_64",
                        "-o",
                        ge,
                        "-Ttext=0x100000",
                        "-e",
                        entry,
                        go,
                        NULL};
    const char *tl[] = {"ld",
                        bits == 32 ? "-melf_i386" : "-melf_x86_64",
                        "-o",
                        te,
                        "-Ttext=0x100000",
                        "-e",
                        entry,
                        to,
                        NULL};
    const char *cl[] = {tcc(f, bits), "-nostdlib", "-static", "-Wl,-Ttext=0x100000", "-o", ce,
                        to,           start,       NULL};
    if (!command(f, as) || !command(f, cc) || !command(f, sc) || !command(f, gl) ||
        !command(f, tl) || !command(f, cl) || !bytes(f, ge, gb) || !bytes(f, te, tb) ||
        !bytes(f, ce, cb))
        return 0;
    int ok = fx_same(bp(f->work, gb), bp(f->work, tb)) && fx_same(bp(f->work, gb), bp(f->work, cb));
    if (direct) {
        const char *dc[] = {tcc(f, bits), "-nostdlib",  "-static", "-Wl,-Ttext=0x100000",
                            "-o",         "direct.elf", source,    "start.s",
                            NULL};
        ok &= command(f, dc) && bytes(f, "direct.elf", "direct.bin") &&
              fx_same(bp(f->work, gb), bp(f->work, "direct.bin"));
    }
    return fx_check(f, ok, name);
}

static int tramp32_source(struct fixture *f)
{
    char *text = fx_read(bp(f->tree, "vendor/tcc/asm-test/trampoline.s")), *save;
    FILE *out = fopen(bp(f->work, "tramp32.s"), "w");
    if (!text || !out) {
        free(text);
        if (out)
            fclose(out);
        return 0;
    }
    int in64 = 0;
    for (char *line = strtok_r(text, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strncmp(line, ".code64", 7))
            in64 = 1;
        if (in64 && !strncmp(line, ".balign", 7))
            in64 = 0;
        if (in64 || strstr(line, ".quad trampoline_end - trampoline_start"))
            continue;
        if (!strncmp(line, "    ljmp $0x18", 14))
            line = "    hlt";
        fprintf(out, "%s\n", line);
    }
    free(text);
    return fclose(out) == 0;
}

int build_asm(struct fixture *f)
{
    if (!asm_sources(f) || !fx_text(bp(f->work, "start.s"), ".globl _start\n_start:\n"))
        return 0;
    assembly(f, "code16-x86_64", "code16.s", 64, "-Mi8086");
    assembly(f, "code16-i386", "code16.s", 32, "-Mi8086");
    assembly(f, "code32-x86_64", "code32.s", 64, "-Mi386");
    assembly(f, "code32-i386", "code32.s", 32, "-Mi386");
    assembly(f, "sys64", bp(f->tree, "vendor/tcc/asm-test/sys64.s"), 64, "-Mx86-64");
    fx_check(f, fx_same(bp(f->work, "code16-x86_64.bin"), bp(f->work, "code16-i386.bin")),
             "both TinyCCs write the same 16-bit bytes");
    fx_check(f, fx_same(bp(f->work, "code32-x86_64.bin"), bp(f->work, "code32-i386.bin")),
             "both TinyCCs write the same 32-bit bytes");
    link_matrix(f, "tramp", bp(f->tree, "vendor/tcc/asm-test/trampoline.s"), 64, 0);
    link_matrix(f, "pcrel", bp(f->tree, "vendor/tcc/asm-test/pcrel.s"), 64, 1);
    if (!tramp32_source(f))
        return 0;
    link_matrix(f, "tramp32", "tramp32.s", 32, 0);
    return !f->failed;
}

struct elf_layout {
    unsigned long long offset, base, data, mem, text_offset, data_address, target, top, end,
        headers, marker;
    unsigned found;
};
static void read_segments(struct fixture *f, const char *elf, struct elf_layout *v)
{
    const char *a[] = {"readelf", "-lW", elf, NULL};
    if (!command(f, a))
        return;
    char *save;
    unsigned count = 0;
    for (char *line = strtok_r(f->out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        unsigned long long o, b, p, z, m;
        if (sscanf(line, " LOAD %llx %llx %llx %llx %llx", &o, &b, &p, &z, &m) != 5)
            continue;
        if (++count == 1) {
            v->offset = o;
            v->base = b;
            v->found |= 1;
        }
        if (count == 2) {
            v->data = b;
            v->mem = m;
            v->found |= 2;
        }
    }
}
static void read_sections(struct fixture *f, const char *elf, struct elf_layout *v)
{
    const char *a[] = {"readelf", "-SW", elf, NULL};
    if (!command(f, a))
        return;
    char *save;
    for (char *line = strtok_r(f->out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char name[128], type[128], *at = strchr(line, ']');
        unsigned long long address, offset;
        if (!at || sscanf(at + 1, "%127s %127s %llx %llx", name, type, &address, &offset) != 4)
            continue;
        if (!strcmp(name, ".text")) {
            v->text_offset = offset;
            v->found |= 4;
        }
        if (!strcmp(name, ".data")) {
            v->data_address = address;
            v->found |= 8;
        }
    }
}
static void read_symbols(struct fixture *f, const char *elf, struct elf_layout *v)
{
    const char *a[] = {"nm", elf, NULL};
    if (!command(f, a))
        return;
    char *save;
    for (char *line = strtok_r(f->out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        char type, name[128];
        unsigned long long value;
        if (sscanf(line, "%llx %c %127s", &value, &type, name) != 3)
            continue;
        if (!strcmp(name, "stack_top")) {
            v->top = value;
            v->found |= 16;
        }
        if (!strcmp(name, "image_end")) {
            v->end = value;
            v->found |= 32;
        }
        if (!strcmp(name, "HEADERS")) {
            v->headers = value;
            v->found |= 64;
        }
        if (!strcmp(name, "image_marker")) {
            v->marker = value;
            v->found |= 128;
        }
    }
}
static void read_target(struct fixture *f, const char *elf, struct elf_layout *v)
{
    const char *a[] = {"objdump", "-d", elf, NULL};
    if (!command(f, a))
        return;
    char *save;
    for (char *line = strtok_r(f->out, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {
        if (!strstr(line, "lea "))
            continue;
        char *hash = strchr(line, '#');
        if (hash && sscanf(hash + 1, "%llx", &v->target) == 1)
            v->found |= 256;
        return;
    }
}
static void layout(struct fixture *f, const char *elf, const char *symbols)
{
    struct elf_layout v = {0};
    read_segments(f, elf, &v);
    read_sections(f, elf, &v);
    read_target(f, elf, &v);
    read_symbols(f, symbols, &v);
    fx_check(f, (v.found & 5) == 5 && v.base == 0x400000 && v.offset == 0 && v.text_offset == 0xc0,
             "headers at BASE and text offset 0xc0");
    fx_check(f, (v.found & 264) == 264 && v.target == v.data_address,
             "PC-relative lea reaches data_base");
    fx_check(f, (v.found & 18) == 18 && v.data + v.mem == v.top, "data segment reaches stack_top");
    fx_check(f, (v.found & 112) == 112 && v.end == v.top && v.headers == 0xb0,
             "image_end and SIZEOF_HEADERS symbols");
    fx_check(f, (v.found & 128) && v.marker == 123, "image_marker debug symbol equals 123");
}
int build_ldscript(struct fixture *f)
{
    const char *script = bp(f->tree, "vendor/tcc/ld-test/script.ld"),
               *source = bp(f->tree, "vendor/tcc/ld-test/prog.s");
    char flag[8192];
    snprintf(flag, sizeof(flag), "-Wl,-T,%s", script);
    const char *t[] = {tcc(f, 64), "-nostdlib", "-static", flag, "-o", "tcc.elf", source, NULL};
    const char *g[] = {tcc(f, 64), "-nostdlib", "-static", "-g", flag,
                       "-o",       "tccg.elf",  source,    NULL};
    const char *as[] = {"as", "--64", "-o", "prog.o", source, NULL};
    const char *ld[] = {"ld", "-T", script, "-o", "gnu.elf", "prog.o", NULL};
    if (!command(f, t) || !command(f, g) || !command(f, as) || !command(f, ld))
        return 0;
    layout(f, "tcc.elf", "tccg.elf");
    layout(f, "gnu.elf", "gnu.elf");
    return !f->failed;
}

/* A section header of either ELF class. */
struct elf_section {
    unsigned type, link;
    unsigned long long addr, offset, size;
};
/* An ELF file read whole; wide is 1 for ELFCLASS64. */
struct elf_file {
    unsigned char *b;
    size_t n;
    int wide;
};

/** Return the little-endian value of `bytes` bytes at `p`. */
static unsigned long long le_value(const unsigned char *p, int bytes)
{
    unsigned long long v = 0;
    while (bytes--)
        v = v << 8 | p[bytes];
    return v;
}

/** Read the ELF file `path` whole into `e`; return 0 if it cannot be read. */
static int elf_read(const char *path, struct elf_file *e)
{
    FILE *in = fopen(path, "rb");
    long n;
    memset(e, 0, sizeof(*e));
    if (!in)
        return 0;
    if (fseek(in, 0, SEEK_END) == 0 && (n = ftell(in)) > EI_NIDENT && fseek(in, 0, SEEK_SET) == 0 &&
        (e->b = malloc((size_t)n)) && fread(e->b, 1, (size_t)n, in) == (size_t)n)
        e->n = (size_t)n;
    fclose(in);
    if (!e->n || memcmp(e->b, ELFMAG, SELFMAG))
        return 0;
    e->wide = e->b[EI_CLASS] == ELFCLASS64;
    return 1;
}

/** Fill `s` with section `i` of `e`; return 0 past the last section. */
static int elf_section_at(const struct elf_file *e, unsigned i, struct elf_section *s)
{
    if (e->wide) {
        Elf64_Ehdr h;
        Elf64_Shdr sh;
        memcpy(&h, e->b, sizeof(h));
        if (i >= h.e_shnum || h.e_shoff + (unsigned long long)(i + 1) * sizeof(sh) > e->n)
            return 0;
        memcpy(&sh, e->b + h.e_shoff + (size_t)i * sizeof(sh), sizeof(sh));
        *s = (struct elf_section){sh.sh_type, sh.sh_link, sh.sh_addr, sh.sh_offset, sh.sh_size};
    } else {
        Elf32_Ehdr h;
        Elf32_Shdr sh;
        memcpy(&h, e->b, sizeof(h));
        if (i >= h.e_shnum || h.e_shoff + (unsigned long long)(i + 1) * sizeof(sh) > e->n)
            return 0;
        memcpy(&sh, e->b + h.e_shoff + (size_t)i * sizeof(sh), sizeof(sh));
        *s = (struct elf_section){sh.sh_type, sh.sh_link, sh.sh_addr, sh.sh_offset, sh.sh_size};
    }
    return s->offset + s->size <= e->n || s->type == SHT_NOBITS;
}

/** A dynamic symbol's value, size and ELF type. */
struct elf_symbol {
    unsigned long long value, size;
    int type;
};

/**
 * Find `name` in the dynamic symbol table of `e` and fill `y` with it;
 * return 0 if it is not there.
 */
static int elf_dynsym(const struct elf_file *e, const char *name, struct elf_symbol *y)
{
    struct elf_section s, str;
    size_t size = e->wide ? sizeof(Elf64_Sym) : sizeof(Elf32_Sym);
    for (unsigned i = 1; elf_section_at(e, i, &s); i++) {
        if (s.type != SHT_DYNSYM || !elf_section_at(e, s.link, &str))
            continue;
        for (unsigned long long at = s.offset; at + size <= s.offset + s.size; at += size) {
            unsigned name_at;
            unsigned char info;
            if (e->wide) {
                Elf64_Sym w;
                memcpy(&w, e->b + at, sizeof(w));
                name_at = w.st_name, info = w.st_info, y->value = w.st_value, y->size = w.st_size;
            } else {
                Elf32_Sym w;
                memcpy(&w, e->b + at, sizeof(w));
                name_at = w.st_name, info = w.st_info, y->value = w.st_value, y->size = w.st_size;
            }
            if (name_at < str.size &&
                !strncmp((const char *)e->b + str.offset + name_at, name, str.size - name_at)) {
                y->type = ELF64_ST_TYPE(info);
                return 1;
            }
        }
    }
    return 0;
}

/** Read the word of `bytes` bytes at address `addr` of `e` into `v`. */
static int elf_word(const struct elf_file *e, unsigned long long addr, int bytes,
                    unsigned long long *v)
{
    struct elf_section s;
    for (unsigned i = 1; elf_section_at(e, i, &s); i++)
        if (s.type == SHT_PROGBITS && s.addr && addr >= s.addr && addr + bytes <= s.addr + s.size) {
            *v = le_value(e->b + s.offset + (addr - s.addr), bytes);
            return 1;
        }
    return 0;
}

/**
 * Give the address that the IRELATIVE relocation of `e` passes to the loader
 * as the resolver: the addend on x86-64, the word it relocates on i386.
 * Return 0 unless there is exactly one.
 */
static int elf_irelative(const struct elf_file *e, unsigned long long *resolver)
{
    struct elf_section s;
    int found = 0;
    for (unsigned i = 1; elf_section_at(e, i, &s); i++) {
        if (e->wide && s.type == SHT_RELA) {
            for (unsigned long long at = s.offset; at + sizeof(Elf64_Rela) <= s.offset + s.size;
                 at += sizeof(Elf64_Rela)) {
                Elf64_Rela r;
                memcpy(&r, e->b + at, sizeof(r));
                if (ELF64_R_TYPE(r.r_info) == R_X86_64_IRELATIVE)
                    found++, *resolver = (unsigned long long)r.r_addend;
            }
        } else if (!e->wide && s.type == SHT_REL) {
            for (unsigned long long at = s.offset; at + sizeof(Elf32_Rel) <= s.offset + s.size;
                 at += sizeof(Elf32_Rel)) {
                Elf32_Rel r;
                memcpy(&r, e->b + at, sizeof(r));
                if (ELF32_R_TYPE(r.r_info) == R_386_IRELATIVE &&
                    elf_word(e, r.r_offset, 4, resolver))
                    found++;
            }
        }
    }
    return found == 1;
}

/**
 * Assemble vendor/tcc/ifunc-test/pickBITS.s with GNU as into pickBITS.o, the
 * IFUNC pick as GCC's output defines one, with the size of its resolver;
 * return 0 if as fails.
 */
static int ifunc_pick(struct fixture *f, int bits)
{
    char source[64], object[64];
    snprintf(source, sizeof(source), "vendor/tcc/ifunc-test/pick%d.s", bits);
    snprintf(object, sizeof(object), "pick%d.o", bits);
    const char *as[] = {"as",   bits == 32 ? "--32" : "--64", "-o",
                        object, bp(f->tree, source),          NULL};
    return command(f, as);
}

/** Return the `bits`-bit driver of TinyCC's library. */
static const char *dynlink(struct fixture *f, int bits)
{
    return product_path(f, bits == 32 ? "tcc/i386-tcc-dynlink" : "tcc/x86_64-tcc-dynlink");
}

/**
 * Link vendor/tcc/ifunc-test/export.c and pickBITS.o into a dynamic
 * executable with the `bits`-bit TinyCC's library and check what it
 * exports: pick, an IFUNC the executable points to, must be a function of
 * size 0 at the address of the pointer fp, as GNU ld exports it, so that
 * dlsym gives what the executable's own references take, while the
 * IRELATIVE relocation still names its resolver, whose size is not 0.
 */
static void ifunc_export(struct fixture *f, int bits)
{
    char exe[64], object[64], what[160];
    struct elf_file e;
    struct elf_symbol pick, fp, resolver;
    unsigned long long fp_value = 0, irelative = 0;
    if (!ifunc_pick(f, bits))
        return;
    snprintf(exe, sizeof(exe), "export%d.elf", bits);
    snprintf(object, sizeof(object), "pick%d.o", bits);
    const char *link[] = {dynlink(f, bits), exe, bp(f->tree, "vendor/tcc/ifunc-test/export.c"),
                          object, NULL};
    if (!command(f, link))
        return;
    snprintf(what, sizeof(what), "%d-bit executable %s is an ELF", bits, exe);
    if (!fx_check(f, elf_read(bp(f->work, exe), &e), what)) {
        free(e.b);
        return;
    }
    int ok = elf_dynsym(&e, "pick", &pick) && elf_dynsym(&e, "fp", &fp) &&
             elf_dynsym(&e, "pick_resolver", &resolver) &&
             elf_word(&e, fp.value, bits / 8, &fp_value);
    snprintf(what, sizeof(what), "%d-bit executable exports pick, pick_resolver and fp", bits);
    if (fx_check(f, ok, what)) {
        snprintf(what, sizeof(what), "%d-bit executable exports its IFUNC pick as a function",
                 bits);
        fx_check(f, pick.type == STT_FUNC, what);
        snprintf(what, sizeof(what), "%d-bit executable exports pick at the address fp holds",
                 bits);
        fx_check(f, pick.value == fp_value, what);
        snprintf(what, sizeof(what),
                 "%d-bit executable exports pick with size 0, not its resolver's %llu", bits,
                 resolver.size);
        fx_check(f, pick.size == 0 && resolver.size != 0, what);
        snprintf(what, sizeof(what), "%d-bit IRELATIVE relocation names pick's resolver", bits);
        fx_check(f,
                 elf_irelative(&e, &irelative) && irelative == resolver.value &&
                     resolver.value != pick.value,
                 what);
    }
    free(e.b);
}

/**
 * Link vendor/tcc/ifunc-test/ref.c into a shared library with the `bits`-bit
 * TinyCC's library, link vendor/tcc/ifunc-test/run.c and pickBITS.o
 * against the host's loader and C library for that target, and run it:
 * dlsym, the library's reference to pick and the executable's own
 * pointers to it must give one address, which reaches triple. A 32-bit
 * run takes /lib/ld-linux.so.2 and /lib32/libc.so.6, which the x86-64 GNU
 * helper the check runs in does not have, so `build_ifunc` runs only the
 * 64-bit target; the 32-bit target's export is checked in its ELF file
 * (`ifunc_export`). run.c loads ./libref.so, so the
 * library of the target under run is copied to that name first when it was
 * linked under another.
 */
static void ifunc_run(struct fixture *f, int bits)
{
    char exe[64], object[64], path[80], what[160];
    const char *lib = bits == 32 ? "libref32.so" : "libref.so";
    const char *loader = bits == 32 ? "/lib/ld-linux.so.2" : "/lib64/ld-linux-x86-64.so.2";
    const char *libc = bits == 32 ? "/lib32/libc.so.6" : "/lib/libc.so.6";
    const char *shared[] = {dynlink(f, bits), "-shared", lib,
                            bp(f->tree, "vendor/tcc/ifunc-test/ref.c"), NULL};
    if (!command(f, shared))
        return;
    struct elf_file e;
    snprintf(what, sizeof(what), "%d-bit shared library %s is an ELF", bits, lib);
    fx_check(f, elf_read(bp(f->work, lib), &e), what);
    free(e.b);
    snprintf(what, sizeof(what), "the host has the %d-bit loader %s and C library %s", bits, loader,
             libc);
    if (!fx_check(f, access(loader, F_OK) == 0 && access(libc, F_OK) == 0, what))
        return;
    snprintf(object, sizeof(object), "pick%d.o", bits);
    if (!fx_exists(bp(f->work, object)) && !ifunc_pick(f, bits))
        return;
    if (strcmp(lib, "libref.so") &&
        !fx_check(f, fx_copy(bp(f->work, lib), bp(f->work, "libref.so")),
                  "32-bit shared library is libref.so for run32.elf to load"))
        return;
    snprintf(exe, sizeof(exe), "run%d.elf", bits);
    snprintf(path, sizeof(path), "./%s", exe);
    const char *link[] = {dynlink(f, bits),
                          exe,
                          bp(f->tree, "vendor/tcc/ifunc-test/run.c"),
                          object,
                          loader,
                          libc,
                          NULL};
    const char *run[] = {path, NULL};
    if (!command(f, link) || !command(f, run))
        return;
    snprintf(what, sizeof(what),
             "%d-bit dlsym, the library's reference and the executable's pointers to pick are "
             "one address that reaches triple",
             bits);
    fx_check(f, f->out && !strcmp(f->out, "IFUNCRUN 1 1 1\n"), what);
}

int build_ifunc(struct fixture *f)
{
    ifunc_export(f, 32);
    ifunc_export(f, 64);
    ifunc_run(f, 64);
    return !f->failed;
}
