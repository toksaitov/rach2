/*
 * mbinfo_hosttest.c: the loader's information into the boot record, from
 * multiboot2 tag lists and Multiboot structures built here.
 *
 * host-deps: rach2/sys/arch/x86/mbinfo.c
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 *
 * Each row writes a tag list the way the Multiboot2 Specification lays it
 * out, a total size, a reserved word, and tags of a type, a size and their
 * data, each starting on an 8-byte boundary, and checks the record the
 * parser filled: the memory map, from multiboot2's own tag and from the EFI
 * map that takes its place under UEFI; the framebuffer of each type; the
 * two RSDP tags; the modules with their command lines; the command line;
 * and lists cut short, which must stop the parser where they stop. The
 * Multiboot structure's pointers are physical addresses, answered by a
 * model of physical memory: one buffer at a fixed address.
 */

#include "../../rach2/sys/arch/x86/mbinfo.h"
#include "../../rach2/sys/lib/kdef.h"

extern int printf(const char *fmt, ...);
extern int memcmp(const void *a, const void *b, unsigned long n);
extern int strcmp(const char *a, const char *b);
extern unsigned long strlen(const char *s);

static int fails;

#define CHECK(cond, what)                              \
    do {                                               \
        if (!(cond)) {                                 \
            printf("mbinfo-test: fail: %s\n", (what)); \
            fails++;                                   \
        }                                              \
    } while (0)

static struct boot_info boot;
static struct boot_store store;

/* ---- a tag list ---- */

static uint8_t mbi[16384] __attribute__((aligned(8)));
static uint32_t at;
static uint32_t tag_start;

static void put8(uint8_t v)
{
    mbi[at++] = v;
}

static void put16(uint32_t v)
{
    put8((uint8_t)v);
    put8((uint8_t)(v >> 8));
}

static void put32(uint32_t v)
{
    put16(v & 0xFFFF);
    put16(v >> 16);
}

static void put64(uint64_t v)
{
    put32((uint32_t)v);
    put32((uint32_t)(v >> 32));
}

static void puts_z(const char *s)
{
    while (*s) {
        put8((uint8_t)*s++);
    }
    put8(0);
}

static void set32(uint32_t off, uint32_t v)
{
    mbi[off] = (uint8_t)v;
    mbi[off + 1] = (uint8_t)(v >> 8);
    mbi[off + 2] = (uint8_t)(v >> 16);
    mbi[off + 3] = (uint8_t)(v >> 24);
}

static void list_begin(void)
{
    unsigned i;

    for (i = 0; i < sizeof(mbi); i++) {
        mbi[i] = 0xEE;
    }
    at = 0;
    put32(0);
    put32(0);
}

static void tag_begin(uint32_t type)
{
    tag_start = at;
    put32(type);
    put32(0);
}

static void tag_end(void)
{
    set32(tag_start + 4, at - tag_start);
    while (at % 8) {
        put8(0);
    }
}

/** The end tag and the total size; returns the total. */
static uint32_t list_end(void)
{
    tag_begin(0);
    tag_end();
    set32(0, at);
    return at;
}

static void tag_string(uint32_t type, const char *s)
{
    tag_begin(type);
    puts_z(s);
    tag_end();
}

static void tag_module(uint32_t start, uint32_t end, const char *cmdline)
{
    tag_begin(3);
    put32(start);
    put32(end);
    puts_z(cmdline);
    tag_end();
}

/* ---- the record ---- */

static int map_entry(uint32_t i, uint64_t *base, uint64_t *len, uint32_t *type)
{
    const uint8_t *e = (const uint8_t *)boot.mmap + i * BOOT_MMAP_ENTRY;
    uint32_t size;

    if (boot.mmap == NULL || (i + 1) * BOOT_MMAP_ENTRY > boot.mmap_len) {
        return 0;
    }
    size = e[0] | e[1] << 8 | e[2] << 16 | (uint32_t)e[3] << 24;
    *base = 0;
    *len = 0;
    for (int b = 7; b >= 0; b--) {
        *base = *base << 8 | e[4 + b];
        *len = *len << 8 | e[12 + b];
    }
    *type = e[20] | e[21] << 8 | e[22] << 16 | (uint32_t)e[23] << 24;
    return size == 20;
}

static int map_is(uint32_t i, uint64_t base, uint64_t len, uint32_t type)
{
    uint64_t b, l;
    uint32_t t;

    return map_entry(i, &b, &l, &t) && b == base && l == len && t == type;
}

/* ---- the rows ---- */

static void row_strings_and_meminfo(void)
{
    list_begin();
    tag_string(1, "rach2.root=ram rach2.loglevel=7");
    tag_string(2, "GRUB 2.12-1ubuntu7.3");
    tag_begin(4);
    put32(639);
    put32(522112);
    tag_end();
    list_end();
    CHECK(mb2_read(mbi, &boot, &store) == 0, "a whole list reads");
    CHECK(boot.cmdline && strcmp(boot.cmdline, "rach2.root=ram rach2.loglevel=7") == 0,
          "the command line");
    CHECK(boot.cmdline == store.cmdline, "the command line is the kernel's copy");
    CHECK(boot.loader && strcmp(boot.loader, "GRUB 2.12-1ubuntu7.3") == 0, "the loader's name");
    CHECK(boot.mem_upper_kb == 522112, "the memory above 1 MiB");
    CHECK(boot.mmap == NULL && boot.mmap_len == 0, "no map tag, no map");
    CHECK(boot.fb.type == BOOT_FB_NONE && !boot.uefi && boot.acpi_rsdp == 0 &&
              store.rsdp_len == 0 && boot.modules == 0,
          "nothing the list did not give");

    /* A command line longer than the record keeps is cut, and terminated. */
    {
        char longline[BOOT_CMDLINE_MAX + 40];
        unsigned i;

        for (i = 0; i < sizeof(longline) - 1; i++) {
            longline[i] = (char)('a' + i % 26);
        }
        longline[sizeof(longline) - 1] = '\0';
        list_begin();
        tag_string(1, longline);
        list_end();
        CHECK(mb2_read(mbi, &boot, &store) == 0 && strlen(boot.cmdline) == BOOT_CMDLINE_MAX - 1 &&
                  memcmp(boot.cmdline, longline, BOOT_CMDLINE_MAX - 1) == 0,
              "a long command line is cut to the record's buffer");
    }
}

static void row_mmap(void)
{
    list_begin();
    tag_begin(6);
    put32(24);
    put32(0);
    put64(0);
    put64(0x9FC00);
    put32(1);
    put32(0);
    put64(0x9FC00);
    put64(0x400);
    put32(2);
    put32(0);
    put64(0x100000);
    put64(0x1FEE0000);
    put32(1);
    put32(0);
    put64(0x1FFE0000);
    put64(0x20000);
    put32(2);
    put32(0);
    put64(0xFEFFC000);
    put64(0x4000);
    put32(2);
    put32(0);
    put64(0x100000000ull);
    put64(0x40000000);
    put32(1);
    put32(0);
    tag_end();
    list_end();
    CHECK(mb2_read(mbi, &boot, &store) == 0, "a map reads");
    CHECK(boot.mmap_len == 6 * BOOT_MMAP_ENTRY, "six ranges, none of them adjacent of a kind");
    CHECK(map_is(0, 0, 0x9FC00, 1), "the first range");
    CHECK(map_is(2, 0x100000, 0x1FEE0000, 1), "the memory above 1 MiB");
    CHECK(map_is(5, 0x100000000ull, 0x40000000, 1), "the memory above 4 GiB");
    CHECK(!boot.uefi && !store.mmap_efi, "multiboot2's own map is not the EFI one");

    /* Entries larger than the specification's 24 bytes are stepped over by
       the size the tag gives. */
    list_begin();
    tag_begin(6);
    put32(32);
    put32(0);
    put64(0x100000);
    put64(0x100000);
    put32(1);
    put32(0);
    put64(0xDEADBEEFDEADBEEFull);
    put64(0x200000);
    put64(0x100000);
    put32(2);
    put32(0);
    put64(0);
    tag_end();
    list_end();
    CHECK(mb2_read(mbi, &boot, &store) == 0 && boot.mmap_len == 2 * BOOT_MMAP_ENTRY &&
              map_is(0, 0x100000, 0x100000, 1) && map_is(1, 0x200000, 0x100000, 2),
          "entries of 32 bytes are read by their size");

    /* An entry size under 24 bytes is not a map. */
    list_begin();
    tag_begin(6);
    put32(16);
    put32(0);
    put64(0x100000);
    put64(0x100000);
    tag_end();
    list_end();
    CHECK(mb2_read(mbi, &boot, &store) == 0 && boot.mmap == NULL,
          "entries of 16 bytes are refused");
}

/* One EFI memory descriptor of 48 bytes, the size OVMF gives. */
static void efi_desc(uint32_t type, uint64_t phys, uint64_t pages)
{
    put32(type);
    put32(0);
    put64(phys);
    put64(0);
    put64(pages);
    put64(0xF);
    put64(0);
}

static void row_efi_mmap(void)
{
    list_begin();
    /* multiboot2's own map first, as GRUB writes it, and the EFI map after. */
    tag_begin(6);
    put32(24);
    put32(0);
    put64(0);
    put64(0x80000000ull);
    put32(1);
    put32(0);
    tag_end();
    tag_begin(12);
    put64(0x7FB9E018);
    tag_end();
    tag_begin(17);
    put32(48);
    put32(1);
    efi_desc(3, 0, 1);                   /* boot services code */
    efi_desc(7, 0x1000, 0x9F);           /* conventional, adjacent: one range */
    efi_desc(0, 0xA0000, 0x60);          /* reserved: the legacy hole */
    efi_desc(2, 0x100000, 0x700);        /* loader data, the kernel */
    efi_desc(1, 0x800000, 0x100);        /* loader code */
    efi_desc(4, 0x900000, 0x1000);       /* boot services data */
    efi_desc(7, 0x1900000, 0x5000);      /* conventional */
    efi_desc(9, 0x6900000, 0x10);        /* ACPI reclaimable */
    efi_desc(10, 0x6910000, 0x20);       /* ACPI NVS */
    efi_desc(5, 0x6930000, 0x40);        /* runtime services code */
    efi_desc(6, 0x6970000, 0x40);        /* runtime services data: one reserved range */
    efi_desc(8, 0x69B0000, 0x1);         /* unusable */
    efi_desc(11, 0xFEC00000, 0x1);       /* the I/O APIC, memory-mapped */
    efi_desc(14, 0x100000000ull, 0x100); /* persistent */
    efi_desc(7, 0x100100000ull, 0x40000);
    tag_end();
    list_end();
    CHECK(mb2_read(mbi, &boot, &store) == 0, "a UEFI list reads");
    CHECK(boot.uefi && store.mmap_efi, "the EFI tags say UEFI");
    CHECK(map_is(0, 0, 0xA0000, 1), "boot services code and conventional memory join");
    CHECK(map_is(1, 0xA0000, 0x60000, 2), "the hole is reserved");
    CHECK(map_is(2, 0x100000, 0x6800000, 1),
          "loader data and code and boot services data are usable, joined");
    CHECK(map_is(3, 0x6900000, 0x10000, 3), "ACPI's tables stay out, as reclaimable");
    CHECK(map_is(4, 0x6910000, 0x20000, 4), "ACPI's NVS stays out");
    CHECK(map_is(5, 0x6930000, 0x80000, 2), "runtime services, code and data, one range");
    CHECK(map_is(6, 0x69B0000, 0x1000, 5), "unusable memory is bad memory");
    CHECK(map_is(7, 0xFEC00000, 0x1000, 2), "memory-mapped I/O is reserved");
    CHECK(map_is(8, 0x100000000ull, 0x100000, 2), "persistent memory is not the allocator's");
    CHECK(map_is(9, 0x100100000ull, 0x40000000ull, 1), "the memory above 4 GiB");
    CHECK(boot.mmap_len == 10 * BOOT_MMAP_ENTRY, "ten ranges and nothing else");

    /* The EFI map first and multiboot2's after it: the EFI one stays. */
    list_begin();
    tag_begin(17);
    put32(48);
    put32(1);
    efi_desc(7, 0x100000, 0x100);
    tag_end();
    tag_begin(6);
    put32(24);
    put32(0);
    put64(0);
    put64(0x80000000ull);
    put32(1);
    put32(0);
    tag_end();
    list_end();
    CHECK(mb2_read(mbi, &boot, &store) == 0 && boot.mmap_len == BOOT_MMAP_ENTRY &&
              map_is(0, 0x100000, 0x100000, 1),
          "a later multiboot2 map does not replace the EFI one");

    /* A descriptor size under EFI's 40 bytes refuses the map. */
    list_begin();
    tag_begin(17);
    put32(32);
    put32(1);
    efi_desc(7, 0x100000, 0x100);
    tag_end();
    list_end();
    CHECK(mb2_read(mbi, &boot, &store) == 0 && boot.mmap == NULL,
          "descriptors of 32 bytes are refused");

    /* A map of more ranges than the record keeps is cut and counted. */
    {
        uint32_t i;

        list_begin();
        tag_begin(17);
        put32(48);
        put32(1);
        for (i = 0; i < BOOT_MMAP_MAX + 5; i++) {
            efi_desc(i % 2 ? 7 : 0, 0x100000 + i * 0x1000ull, 1);
        }
        tag_end();
        list_end();
        CHECK(mb2_read(mbi, &boot, &store) == 0 &&
                  boot.mmap_len == BOOT_MMAP_MAX * BOOT_MMAP_ENTRY && store.mmap_cut == 5,
              "a map past the record's entries is cut and counted");
    }
}

static void row_framebuffers(void)
{
    uint32_t i;

    /* Direct color, as OVMF's GOP gives it: 32 bits, blue lowest. */
    list_begin();
    tag_begin(8);
    put64(0x80000000ull);
    put32(5120);
    put32(1280);
    put32(800);
    put8(32);
    put8(1);
    put16(0);
    put8(16);
    put8(8);
    put8(8);
    put8(8);
    put8(0);
    put8(8);
    tag_end();
    list_end();
    CHECK(mb2_read(mbi, &boot, &store) == 0, "a framebuffer list reads");
    CHECK(boot.fb.type == BOOT_FB_RGB && boot.fb.addr == 0x80000000ull && boot.fb.pitch == 5120 &&
              boot.fb.width == 1280 && boot.fb.height == 800 && boot.fb.bpp == 32,
          "a direct-color framebuffer's geometry");
    CHECK(boot.fb.red_shift == 16 && boot.fb.red_bits == 8 && boot.fb.green_shift == 8 &&
              boot.fb.green_bits == 8 && boot.fb.blue_shift == 0 && boot.fb.blue_bits == 8,
          "its color fields");

    /* 16 bits, five, six and five. */
    list_begin();
    tag_begin(8);
    put64(0xFD000000ull);
    put32(2048);
    put32(1024);
    put32(768);
    put8(16);
    put8(1);
    put16(0);
    put8(11);
    put8(5);
    put8(5);
    put8(6);
    put8(0);
    put8(5);
    tag_end();
    list_end();
    CHECK(mb2_read(mbi, &boot, &store) == 0 && boot.fb.type == BOOT_FB_RGB && boot.fb.bpp == 16 &&
              boot.fb.red_shift == 11 && boot.fb.green_bits == 6 && boot.fb.blue_bits == 5,
          "a 16-bit framebuffer's fields");

    /* A direct-color tag too short for its color fields is none. */
    list_begin();
    tag_begin(8);
    put64(0x80000000ull);
    put32(5120);
    put32(1280);
    put32(800);
    put8(32);
    put8(1);
    put16(0);
    put8(16);
    put8(8);
    tag_end();
    list_end();
    CHECK(mb2_read(mbi, &boot, &store) == 0 && boot.fb.type == BOOT_FB_NONE,
          "a direct-color tag without its fields is no framebuffer");

    /* Indexed, with a palette of 16 colors. */
    list_begin();
    tag_begin(8);
    put64(0xFD000000ull);
    put32(1024);
    put32(1024);
    put32(768);
    put8(8);
    put8(0);
    put16(0);
    put16(16);
    for (i = 0; i < 16; i++) {
        put8((uint8_t)(i * 16));
        put8((uint8_t)(255 - i));
        put8((uint8_t)i);
    }
    tag_end();
    list_end();
    CHECK(mb2_read(mbi, &boot, &store) == 0 && boot.fb.type == BOOT_FB_INDEXED &&
              boot.fb.bpp == 8 && boot.fb.colors == 16,
          "an indexed framebuffer and its palette's size");
    CHECK(boot.fb.palette[15][0] == 240 && boot.fb.palette[15][1] == 240 &&
              boot.fb.palette[15][2] == 15 && boot.fb.palette[16][0] == 0,
          "the palette's entries, and none past them");

    /* A palette that claims more colors than its tag holds is cut to them. */
    list_begin();
    tag_begin(8);
    put64(0xFD000000ull);
    put32(1024);
    put32(1024);
    put32(768);
    put8(8);
    put8(0);
    put16(0);
    put16(200);
    for (i = 0; i < 4; i++) {
        put8(1);
        put8(2);
        put8(3);
    }
    tag_end();
    list_end();
    CHECK(mb2_read(mbi, &boot, &store) == 0 && boot.fb.colors == 4,
          "a palette is cut to the colors its tag holds");

    /* EGA text, as GRUB describes the VGA's text mode. */
    list_begin();
    tag_begin(8);
    put64(0xB8000);
    put32(160);
    put32(80);
    put32(25);
    put8(16);
    put8(2);
    put16(0);
    tag_end();
    list_end();
    CHECK(mb2_read(mbi, &boot, &store) == 0 && boot.fb.type == BOOT_FB_TEXT &&
              boot.fb.addr == 0xB8000 && boot.fb.width == 80 && boot.fb.height == 25,
          "the text mode");

    /* A type the specification does not have. */
    list_begin();
    tag_begin(8);
    put64(0xB8000);
    put32(160);
    put32(80);
    put32(25);
    put8(16);
    put8(7);
    put16(0);
    tag_end();
    list_end();
    CHECK(mb2_read(mbi, &boot, &store) == 0 && boot.fb.type == BOOT_FB_NONE,
          "an unknown type is no framebuffer");
}

static void rsdp(int revision, uint8_t *out)
{
    unsigned i;
    unsigned len = revision >= 2 ? 36 : 20;

    for (i = 0; i < len; i++) {
        out[i] = (uint8_t)(revision * 16 + i);
    }
    for (i = 0; i < 8; i++) {
        out[i] = (uint8_t)"RSD PTR "[i];
    }
}

static void row_rsdp(void)
{
    uint8_t v1[36], v2[36];
    unsigned i;

    rsdp(0, v1);
    rsdp(2, v2);

    list_begin();
    tag_begin(14);
    for (i = 0; i < 20; i++) {
        put8(v1[i]);
    }
    tag_end();
    tag_begin(15);
    for (i = 0; i < 36; i++) {
        put8(v2[i]);
    }
    tag_end();
    list_end();
    CHECK(mb2_read(mbi, &boot, &store) == 0 && store.rsdp_len == 36 &&
              memcmp(store.rsdp, v2, 36) == 0,
          "the second revision's RSDP is taken over the first");

    list_begin();
    tag_begin(15);
    for (i = 0; i < 36; i++) {
        put8(v2[i]);
    }
    tag_end();
    tag_begin(14);
    for (i = 0; i < 20; i++) {
        put8(v1[i]);
    }
    tag_end();
    list_end();
    CHECK(mb2_read(mbi, &boot, &store) == 0 && store.rsdp_len == 36 &&
              memcmp(store.rsdp, v2, 36) == 0,
          "a first revision after the second does not replace it");

    list_begin();
    tag_begin(14);
    for (i = 0; i < 20; i++) {
        put8(v1[i]);
    }
    tag_end();
    list_end();
    CHECK(mb2_read(mbi, &boot, &store) == 0 && store.rsdp_len == 20 &&
              memcmp(store.rsdp, v1, 20) == 0,
          "the first revision alone");

    list_begin();
    tag_begin(15);
    for (i = 0; i < 24; i++) {
        put8(v2[i]);
    }
    tag_end();
    list_end();
    CHECK(mb2_read(mbi, &boot, &store) == 0 && store.rsdp_len == 0,
          "a second-revision tag too short for its RSDP is left out");
}

static void row_modules(void)
{
    char name[16];
    char longname[200];
    uint32_t i;

    list_begin();
    tag_module(0x2000000, 0x2400000, "");
    tag_module(0x2400000, 0x2401234, "second");
    for (i = 0; i < sizeof(longname) - 1; i++) {
        longname[i] = 'x';
    }
    longname[sizeof(longname) - 1] = '\0';
    tag_module(0x2402000, 0x2403000, longname);
    for (i = 3; i < BOOT_MODULE_MAX + 2; i++) {
        name[0] = 'm';
        name[1] = (char)('0' + i);
        name[2] = '\0';
        tag_module(0x3000000 + i * 0x1000, 0x3000800 + i * 0x1000, name);
    }
    list_end();
    CHECK(mb2_read(mbi, &boot, &store) == 0, "a list of modules reads");
    CHECK(boot.modules == BOOT_MODULE_MAX && boot.modules_passed == BOOT_MODULE_MAX + 2,
          "eight kept of ten, and ten counted");
    CHECK(boot.module[0].start == 0x2000000 && boot.module[0].end == 0x2400000 &&
              boot.module[0].cmdline && boot.module[0].cmdline[0] == '\0',
          "the first module, with an empty command line");
    CHECK(boot.module[1].end == 0x2401234 && strcmp(boot.module[1].cmdline, "second") == 0,
          "the second module's end and command line");
    CHECK(strlen(boot.module[2].cmdline) == BOOT_MODULE_CMDLINE_MAX - 1,
          "a long module command line is cut to its buffer");
    CHECK(strcmp(boot.module[7].cmdline, "m7") == 0 &&
              boot.module[7].cmdline == store.module_cmdline[7],
          "the last kept module, its command line the kernel's copy");

    /* An end before the start is taken as an empty module. */
    list_begin();
    tag_module(0x2000000, 0x1000000, "back");
    list_end();
    CHECK(mb2_read(mbi, &boot, &store) == 0 && boot.modules == 1 &&
              boot.module[0].end == boot.module[0].start,
          "a module that ends before it starts is empty");
}

static void row_cut_short(void)
{
    uint32_t total, cmdline_at;

    /* The total size ends inside the second tag: the first is read. */
    list_begin();
    tag_string(1, "first");
    cmdline_at = at;
    tag_string(2, "a loader whose tag runs past the list");
    total = list_end();
    set32(0, cmdline_at + 12);
    CHECK(mb2_read(mbi, &boot, &store) == -1, "a tag past the total size stops the list");
    CHECK(boot.cmdline && strcmp(boot.cmdline, "first") == 0 && boot.loader == NULL,
          "the tags before it are read, and it is not");

    /* No end tag before the total size. */
    set32(0, total - 8);
    CHECK(mb2_read(mbi, &boot, &store) == -1, "a list without its end tag");
    CHECK(boot.loader && strcmp(boot.loader, "a loader whose tag runs past the list") == 0,
          "every whole tag before the missing end is read");

    /* A tag whose size is under its own header. */
    list_begin();
    tag_string(1, "first");
    tag_begin(2);
    tag_end();
    set32(tag_start + 4, 4);
    list_end();
    CHECK(mb2_read(mbi, &boot, &store) == -1, "a tag of four bytes stops the list");

    /* A total size with no room for the end tag, and one past the largest
       list. */
    list_begin();
    list_end();
    CHECK(mb2_read(mbi, &boot, &store) == 0, "an empty list reads");
    set32(0, 8);
    CHECK(mb2_read(mbi, &boot, &store) == -1, "a total of 8 bytes has no room for an end tag");
    set32(0, MB2_INFO_MAX + 8);
    CHECK(mb2_read(mbi, &boot, &store) == -1, "a total past the largest list is refused");

    /* An end tag of four bytes is not an end. */
    list_begin();
    tag_begin(0);
    set32(tag_start + 4, 4);
    put32(0);
    set32(0, at);
    CHECK(mb2_read(mbi, &boot, &store) == -1, "an end tag under 8 bytes");
}

/* ---- Multiboot ---- */

/* Physical memory: one buffer at PHYS_BASE. */
#define PHYS_BASE 0x10000u
static uint8_t phys[8192];

static const void *reach(uint64_t pa, uint32_t len)
{
    if (pa < PHYS_BASE || pa + len > PHYS_BASE + sizeof(phys)) {
        return NULL;
    }
    return phys + (pa - PHYS_BASE);
}

static void pset32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static void pset64(uint8_t *p, uint64_t v)
{
    pset32(p, (uint32_t)v);
    pset32(p + 4, (uint32_t)(v >> 32));
}

static void row_multiboot(void)
{
    static uint8_t raw[116] __attribute__((aligned(8)));
    multiboot_info_t *info = (multiboot_info_t *)raw;
    uint8_t *m;
    unsigned i;

    for (i = 0; i < sizeof(phys); i++) {
        phys[i] = 0;
    }
    for (i = 0; i < sizeof(raw); i++) {
        raw[i] = 0;
    }
    /* The command line at 0x10000, the loader's name at 0x10100, two modules
       at 0x10200 with their command lines at 0x10300, the map at 0x10400 and
       a palette at 0x10800. */
    {
        const char *c = "rach2.root=ram";
        const char *l = "qemu";

        for (i = 0; c[i]; i++) {
            phys[i] = (uint8_t)c[i];
        }
        for (i = 0; l[i]; i++) {
            phys[0x100 + i] = (uint8_t)l[i];
        }
        phys[0x300] = 'r';
        phys[0x301] = 'd';
    }
    m = phys + 0x200;
    pset32(m, 0x400000);
    pset32(m + 4, 0x500000);
    pset32(m + 8, PHYS_BASE + 0x300);
    pset32(m + 16, 0x500000);
    pset32(m + 20, 0x500100);
    pset32(m + 24, 0);
    m = phys + 0x400;
    pset32(m, 20);
    pset64(m + 4, 0);
    pset64(m + 12, 0x9FC00);
    pset32(m + 20, 1);
    /* An entry whose size word says 24, four bytes past the 20. */
    pset32(m + 24, 24);
    pset64(m + 28, 0x100000);
    pset64(m + 36, 0x7EE0000);
    pset32(m + 44, 1);
    pset32(m + 52, 20);
    pset64(m + 56, 0xFFFC0000);
    pset64(m + 64, 0x40000);
    pset32(m + 72, 2);

    info->flags = MULTIBOOT_INFO_MEMORY | MULTIBOOT_INFO_CMDLINE | MULTIBOOT_INFO_MODS |
                  MULTIBOOT_INFO_MEM_MAP | MULTIBOOT_INFO_BOOT_LOADER_NAME | 0x1000u;
    info->mem_upper = 129920;
    info->cmdline = PHYS_BASE;
    info->boot_loader_name = PHYS_BASE + 0x100;
    info->mods_count = 2;
    info->mods_addr = PHYS_BASE + 0x200;
    info->mmap_addr = PHYS_BASE + 0x400;
    info->mmap_length = 76;
    /* The framebuffer, from offset 88: direct color, 32 bits. */
    pset64(raw + 88, 0xFD000000ull);
    pset32(raw + 96, 4096);
    pset32(raw + 100, 1024);
    pset32(raw + 104, 768);
    raw[108] = 32;
    raw[109] = 1;
    raw[110] = 16;
    raw[111] = 8;
    raw[112] = 8;
    raw[113] = 8;
    raw[114] = 0;
    raw[115] = 8;

    CHECK(mb1_read(info, reach, &boot, &store) == 0, "a Multiboot structure reads");
    CHECK(boot.cmdline && strcmp(boot.cmdline, "rach2.root=ram") == 0 &&
              boot.cmdline == store.cmdline,
          "Multiboot's command line, copied");
    CHECK(boot.loader && strcmp(boot.loader, "qemu") == 0, "Multiboot's loader name");
    CHECK(boot.mem_upper_kb == 129920, "Multiboot's memory above 1 MiB");
    CHECK(boot.modules == 2 && boot.modules_passed == 2 && boot.module[0].start == 0x400000 &&
              boot.module[0].end == 0x500000 && strcmp(boot.module[0].cmdline, "rd") == 0 &&
              boot.module[1].start == 0x500000 && boot.module[1].cmdline == NULL,
          "Multiboot's modules and their command lines");
    CHECK(boot.mmap_len == 3 * BOOT_MMAP_ENTRY && map_is(0, 0, 0x9FC00, 1) &&
              map_is(1, 0x100000, 0x7EE0000, 1) && map_is(2, 0xFFFC0000, 0x40000, 2),
          "Multiboot's map, each entry stepped by its size word");
    CHECK(boot.fb.type == BOOT_FB_RGB && boot.fb.addr == 0xFD000000ull && boot.fb.width == 1024 &&
              boot.fb.red_shift == 16 && boot.fb.blue_bits == 8,
          "Multiboot's framebuffer");

    /* Indexed, with the palette by its physical address. */
    raw[109] = 0;
    pset32(raw + 110, PHYS_BASE + 0x800);
    raw[114] = 2;
    raw[115] = 0;
    phys[0x800] = 1;
    phys[0x805] = 6;
    CHECK(mb1_read(info, reach, &boot, &store) == 0 && boot.fb.type == BOOT_FB_INDEXED &&
              boot.fb.colors == 2 && boot.fb.palette[0][0] == 1 && boot.fb.palette[1][2] == 6,
          "Multiboot's indexed framebuffer and its palette");

    /* A map whose last entry runs past its length is read up to it. */
    info->mmap_length = 70;
    CHECK(mb1_read(info, reach, &boot, &store) == -1 && boot.mmap_len == 2 * BOOT_MMAP_ENTRY,
          "a Multiboot map cut short is read up to the cut");

    /* A command line the model cannot reach. */
    info->mmap_length = 76;
    info->cmdline = 0x9000;
    CHECK(mb1_read(info, reach, &boot, &store) == -1 && boot.cmdline == NULL &&
              boot.mmap_len == 3 * BOOT_MMAP_ENTRY,
          "a command line out of reach is left out, and the rest read");

    /* Without the flags, nothing. */
    info->flags = 0;
    CHECK(mb1_read(info, reach, &boot, &store) == 0 && boot.cmdline == NULL && boot.mmap == NULL &&
              boot.modules == 0 && boot.fb.type == BOOT_FB_NONE,
          "a structure without flags gives an empty record");
}

int main(void)
{
    row_strings_and_meminfo();
    row_mmap();
    row_efi_mmap();
    row_framebuffers();
    row_rsdp();
    row_modules();
    row_cut_short();
    row_multiboot();
    if (fails) {
        printf("mbinfo-test: %d failure(s)\n", fails);
        return 1;
    }
    printf("mbinfo-test: OK\n");
    return 0;
}
