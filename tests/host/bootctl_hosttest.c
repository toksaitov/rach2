/**
 * Host test for the boot-slot library bootctl drives (utils/bootslot.c).
 *
 * host-deps: utils/bootslot.c
 *
 * The fixtures come from tests/host/mkfatfixture.py, an independent FAT32
 * writer, so two implementations of the format meet over every image. The
 * well-formed image (a volume label, a long-name entry, dot entries, an empty
 * file, the four named files) must open and install; each malformed image
 * must be refused with its own diagnostic, matched by substring so a mutation
 * cannot pass by tripping a different rule. The write callback is the fault
 * injector: it can report success while dropping a chosen sector (what makes
 * the byte-for-byte readback falsifiable), stop the run after any chosen
 * write (what produces the interruption matrix's states, whose order
 * properties are asserted on the bytes), and tear a sector into a prefix of
 * the new bytes over the old. Returns the failed-check count (0 == pass).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootslot.h"

#define IMG_BYTES (8192 * 512)
#define FIXDIR    "/tmp/rach2_verify"

static int fails;

#define CHECK(cond, what)                          \
    do {                                           \
        if (!(cond)) {                             \
            fails++;                               \
            fprintf(stderr, "  FAIL: %s\n", what); \
        }                                          \
    } while (0)

/* The image under test, in memory, with the injector's controls. */
static struct {
    uint8_t data[IMG_BYTES];
    long writes_left; /* stop (report failure) when this reaches 0 */
    long drop_at;     /* report success but write nothing at this count */
    long tear_at;     /* write only the first tear_bytes at this count */
    int tear_bytes;
    long count;
} img;

static int io_read(void *ctx, uint32_t lba, void *buf)
{
    (void)ctx;
    if ((long)lba * 512 + 512 > IMG_BYTES) {
        return -1;
    }
    memcpy(buf, img.data + (long)lba * 512, 512);
    return 0;
}

static int io_write(void *ctx, uint32_t lba, const void *buf)
{
    (void)ctx;
    if ((long)lba * 512 + 512 > IMG_BYTES) {
        return -1;
    }
    img.count++;
    if (img.count == img.drop_at) {
        return 0; /* reported written, not written */
    }
    if (img.count == img.tear_at) {
        memcpy(img.data + (long)lba * 512, buf, (size_t)img.tear_bytes);
        return -1; /* the cut lands inside this sector */
    }
    if (img.writes_left > 0 && --img.writes_left == 0) {
        return -1; /* the cut lands after the previous write */
    }
    memcpy(img.data + (long)lba * 512, buf, 512);
    return 0;
}

static const struct bootslot_io IO = {io_read, io_write, NULL};

/** Build the named fixture and load it; aborts the test run on builder
 *  failure, which would make every case meaningless. */
static void load(const char *opt)
{
    char cmd[256];
    FILE *f;

    snprintf(cmd, sizeof(cmd), "python3 tests/host/mkfatfixture.py %s/fat_case.img %s", FIXDIR,
             opt);
    if (system(cmd) != 0) {
        fprintf(stderr, "bootctl_hosttest: fixture builder failed\n");
        exit(1);
    }
    f = fopen(FIXDIR "/fat_case.img", "rb");
    if (!f || fread(img.data, 1, IMG_BYTES, f) != IMG_BYTES) {
        fprintf(stderr, "bootctl_hosttest: fixture did not load\n");
        exit(1);
    }
    fclose(f);
    img.writes_left = -1;
    img.drop_at = -1;
    img.tear_at = -1;
    img.count = 0;
}

/** A valid minimal kernel image of `size` bytes, distinct from the fixture's
 *  (different fill), so an install visibly changes the slot. */
static void put32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

static void make_kernel(uint8_t *out, uint32_t size)
{
    uint32_t magic = 0x1BADB002u, flags = 3, i;

    memset(out, 0, size);
    out[0] = 0x7F;
    out[1] = 'E';
    out[2] = 'L';
    out[3] = 'F';
    out[4] = 1;
    out[5] = 1;
    out[16] = 2;
    out[18] = 3;
    put32(out + 24, 0x100000); /* e_entry */
    put32(out + 28, 52);       /* e_phoff */
    out[42] = 32;              /* e_phentsize */
    out[44] = 1;               /* e_phnum */
    put32(out + 52, 1);        /* PT_LOAD */
    put32(out + 56, 0);        /* p_offset */
    put32(out + 60, 0x100000); /* p_vaddr */
    put32(out + 64, 0x100000); /* p_paddr */
    put32(out + 68, size);     /* p_filesz */
    put32(out + 72, size);     /* p_memsz */
    put32(out + 96, magic);
    put32(out + 100, flags);
    put32(out + 104, (uint32_t)(0u - (magic + flags)));
    for (i = 128; i < size; i++) {
        out[i] = (uint8_t)(i * 7);
    }
}

/** Whether the on-disk environment block contains `line` (grubenv is at a
 *  fixed place in the fixture geometry: cluster 4160, data start 160, so
 *  sector 160 + 4160 - 2). */
static int env_has(const char *line)
{
    const char *env = (const char *)img.data + (long)(160 + 4160 - 2) * 512;
    size_t n = strlen(line);
    size_t i;

    for (i = 0; i + n <= 1024; i++) {
        if (memcmp(env + i, line, n) == 0) {
            return 1;
        }
    }
    return 0;
}

static struct bootslot bs;
static uint8_t kernel[64 * 1024];
static char err[128];

/** Open the loaded image and expect the named diagnostic. */
static void expect_open_error(const char *opt, const char *needle)
{
    load(opt);
    err[0] = '\0';
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == -1, opt);
    CHECK(strstr(err, needle) != NULL, needle);
}

int main(void)
{
    /* The well-formed image opens; the skip rules took the label, the
       long-name entry, the dot entries and the empty file in stride. */
    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "wellformed opens");
    CHECK(bs.kernel.size == 4096 * 512, "the candidate slot is 2 MiB");
    CHECK(bs.grubenv.size == 1024, "grubenv is 1024 bytes");

    /* A full install: order, bytes, and the trial marker. */
    make_kernel(kernel, 5000);
    CHECK(bootslot_install(&bs, kernel, 5000, err, sizeof(err)) == 0,
          "install on the well-formed image");
    CHECK(env_has("saved_entry=fallback\n"), "install points the saved entry at fallback");
    CHECK(env_has("next_entry=candidate\n"), "install arms the one-boot trial");
    CHECK(memcmp(img.data + (long)(160 + 16 - 2) * 512, kernel, 5000) == 0,
          "the slot's first bytes are the new kernel");
    {
        long off = (long)(160 + 16 - 2) * 512 + 5000;
        long end = (long)(160 + 16 - 2) * 512 + 4096L * 512;
        int allzero = 1;

        while (off < end) {
            if (img.data[off] != 0) {
                allzero = 0;
                break;
            }
            off++;
        }
        CHECK(allzero, "the slot's tail is zero padding");
    }

    /* Commit is allowed only after the trial boot consumed the arming line;
       the consumption GRUB performs (the value emptied, the line kept) is
       simulated on the bytes, and then the commit replaces the saved entry. */
    {
        long env_off = (long)(160 + 4160 - 2) * 512;
        long i;

        for (i = 0; i + 21 <= 1024; i++) {
            if (memcmp(img.data + env_off + i, "next_entry=candidate\n", 21) == 0) {
                memmove(img.data + env_off + i + 11, img.data + env_off + i + 20,
                        (size_t)(1024 - i - 20));
                memset(img.data + env_off + 1024 - 9, '#', 9);
                break;
            }
        }
    }
    CHECK(env_has("next_entry=\n"), "the simulated consumption emptied the line");
    CHECK(bootslot_commit(&bs, err, sizeof(err)) == 0, "commit");
    CHECK(env_has("saved_entry=candidate\n"), "commit saves the candidate");
    CHECK(!env_has("saved_entry=fallback\n"), "one saved entry only");

    /* An unknown variable survives both env writers. The insertion point is
       found, not assumed: the byte after the block's last newline. */
    load("");
    {
        long env_off = (long)(160 + 4160 - 2) * 512;
        const char *extra = "exp_probe=kept\n";
        long end = 0, i;

        for (i = 0; i < 1024; i++) {
            if (img.data[env_off + i] == '\n') {
                end = i + 1;
            }
        }
        memmove(img.data + env_off + end + strlen(extra), img.data + env_off + end,
                (size_t)(1024 - end) - strlen(extra));
        memcpy(img.data + env_off + end, extra, strlen(extra));
    }
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "reopen with extra var");
    CHECK(bootslot_install(&bs, kernel, 5000, err, sizeof(err)) == 0, "install with extra var");
    CHECK(env_has("exp_probe=kept\n"), "an unknown variable survives");

    /* The fragmented candidate installs cluster by cluster: the kernel's
       bytes land in the first run, the zero padding continues across the
       hole into the second run, and the hole itself is not written. */
    load("--fragmented");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "fragmented opens");
    make_kernel(kernel, 40000);
    memset(img.data + (long)(160 + 16 + 2048 - 2) * 512, 0xEE, 8 * 512);
    CHECK(bootslot_install(&bs, kernel, 40000, err, sizeof(err)) == 0,
          "install on the fragmented chain");
    CHECK(memcmp(img.data + (long)(160 + 16 - 2) * 512, kernel, 40000) == 0,
          "the first run has the kernel's bytes");
    CHECK(img.data[(long)(160 + 16 - 2) * 512 + 40000] == 0 &&
              img.data[(long)(160 + 16 + 2048 + 8 - 2) * 512] == 0,
          "the padding continues across the hole into the second run");
    CHECK(img.data[(long)(160 + 16 + 2048 - 2) * 512] == 0xEE,
          "the hole between the runs is not written");

    /* Every malformed shape, each to its own diagnostic. */
    expect_open_error("--fats-differ", "the two FAT copies disagree");
    expect_open_error("--loop", "two chains share a cluster");
    expect_open_error("--out-of-range", "a chain leaves the volume");
    expect_open_error("--short-chain", "size does not match its chain");
    expect_open_error("--cross-link", "two chains share a cluster");
    expect_open_error("--bpb-sector-size", "sector size is not 512");
    expect_open_error("--bpb-fat-oversized", "FAT region is out of bounds");
    expect_open_error("--bpb-root-out", "root cluster is out of range");
    expect_open_error("--bpb-overflow", "FAT regions do not fit");
    expect_open_error("--no-grubenv", "no /boot/grub/grubenv");

    /* A kernel larger than the slot. */
    load("--small-candidate");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "small slot opens");
    make_kernel(kernel, 16384);
    CHECK(bootslot_install(&bs, kernel, 16384, err, sizeof(err)) == -1,
          "an oversized kernel is refused");
    CHECK(strstr(err, "does not fit") != NULL, "the slot-size diagnostic");

    /* Validation rejects a non-kernel, a broken checksum, and every
       loader-surface break: no loadable segment, a load past the file, an
       entry outside every load. */
    make_kernel(kernel, 5000);
    kernel[0] = 'X';
    CHECK(bootslot_validate_kernel(kernel, 5000, 5000, err, sizeof(err)) == -1 &&
              strstr(err, "not an ELF") != NULL,
          "a non-ELF is refused");
    make_kernel(kernel, 5000);
    kernel[104] ^= 0x40; /* the multiboot checksum no longer sums to zero */
    CHECK(bootslot_validate_kernel(kernel, 5000, 5000, err, sizeof(err)) == -1 &&
              strstr(err, "no multiboot header") != NULL,
          "a broken checksum is refused");
    make_kernel(kernel, 5000);
    kernel[44] = 0; /* e_phnum */
    CHECK(bootslot_validate_kernel(kernel, 5000, 5000, err, sizeof(err)) == -1 &&
              strstr(err, "not a loader's") != NULL,
          "a kernel with no program headers is refused");
    make_kernel(kernel, 5000);
    put32(kernel + 68, 6000); /* p_filesz beyond the file */
    put32(kernel + 72, 6000);
    CHECK(bootslot_validate_kernel(kernel, 5000, 5000, err, sizeof(err)) == -1 &&
              strstr(err, "leaves the file") != NULL,
          "a load past the file is refused");
    make_kernel(kernel, 5000);
    put32(kernel + 24, 0x200000); /* the entry outside the one load */
    CHECK(bootslot_validate_kernel(kernel, 5000, 5000, err, sizeof(err)) == -1 &&
              strstr(err, "outside every load") != NULL,
          "an entry outside every load is refused");
    make_kernel(kernel, 5000);
    put32(kernel + 64, 0); /* p_paddr away from p_vaddr: the loader loads THERE */
    CHECK(bootslot_validate_kernel(kernel, 5000, 5000, err, sizeof(err)) == -1 &&
              strstr(err, "physical address differs") != NULL,
          "a physical address away from the virtual is refused");
    make_kernel(kernel, 5000);
    put32(kernel + 100, 0x10003); /* the a.out kludge bit changes the load */
    put32(kernel + 104, (uint32_t)(0u - (0x1BADB002u + 0x10003)));
    CHECK(bootslot_validate_kernel(kernel, 5000, 5000, err, sizeof(err)) == -1 &&
              strstr(err, "flags are not ALIGN") != NULL,
          "multiboot flags away from ALIGN|MEMINFO are refused");

    /* A fallback whose loader surface is broken stops the install before the
       first environment write. */
    load("--bad-fallback");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "bad-fallback opens");
    make_kernel(kernel, 5000);
    CHECK(bootslot_install(&bs, kernel, 5000, err, sizeof(err)) == -1,
          "a broken fallback stops the install");
    CHECK(strstr(err, "fallback file is not a bootable kernel") != NULL, "the fallback diagnostic");
    CHECK(env_has("saved_entry=candidate\n"), "the environment was never touched");

    /* A grub.cfg whose size does not match its chain is a refused volume:
       every reachable regular file is held to the rule, not only the four
       named ones. */
    expect_open_error("--cfg-short", "size does not match its chain");

    /* A stale armed trial is dropped by the install's first transition: cut
       mid-slot, the block has the fallback and no armed line. */
    load("");
    {
        long env_off = (long)(160 + 4160 - 2) * 512;
        const char *armed = "next_entry=candidate\n";
        long end = 0, i;

        for (i = 0; i < 1024; i++) {
            if (img.data[env_off + i] == '\n') {
                end = i + 1;
            }
        }
        memmove(img.data + env_off + end + strlen(armed), img.data + env_off + end,
                (size_t)(1024 - end) - strlen(armed));
        memcpy(img.data + env_off + end, armed, strlen(armed));
    }
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "armed image opens");
    make_kernel(kernel, 60 * 512);
    img.writes_left = 2 + 30;
    CHECK(bootslot_install(&bs, kernel, 60 * 512, err, sizeof(err)) == -1,
          "the armed retry is cut mid-slot");
    CHECK(env_has("saved_entry=fallback\n"), "the retry saved the fallback");
    CHECK(!env_has("next_entry=candidate\n"),
          "the stale armed trial was dropped before the slot was touched");

    /* Eight sectors per cluster. Every fixture before milestone 25 was
       one-to-one, so the library's cluster arithmetic had never been driven at
       any other geometry, and the sealed 64 MiB boot partition cannot use the
       old one: at one sector per cluster it is past both BOOTSLOT_FAT_MAX_SECTORS
       and BOOTSLOT_MAX_CLUSTERS. Everything the library reports here is in
       sectors and bytes, so the multiplication is what is under test. */
    load("--cluster8");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "a 4 KiB-cluster volume opens");
    CHECK(bs.cluster_sectors == 8, "the volume reports eight sectors per cluster");
    /* 8192 total sectors less a data start of 160, divided by eight, is 1004.
       A count computed as though a cluster were a sector gives 8032, which is
       larger than the volume and so bounds nothing. */
    CHECK(bs.cluster_count == 1004,
          "the cluster count divides by the cluster size instead of counting sectors");
    CHECK(bs.kernel.size == 2 * 1024 * 1024, "the candidate is still 2 MiB");
    CHECK(bs.kernel.chain_len == 512,
          "2 MiB at 4 KiB per cluster is 512 clusters, not the 4096 it would be at 512 bytes");
    CHECK(bs.grubenv.size == 1024, "grubenv is still 1024 bytes");
    CHECK(bs.grubenv.chain_len == 1, "1024 bytes fits one 4 KiB cluster");
    make_kernel(kernel, 60 * 512);
    CHECK(bootslot_install(&bs, kernel, 60 * 512, err, sizeof(err)) == 0,
          "an install works at eight sectors per cluster");
    {
        /* env_has reads the block at the default geometry's offset; here the
           library is asked where it actually is. */
        static uint8_t envbuf[BOOTSLOT_ENV_BYTES];
        int armed = 0;
        size_t i;

        CHECK(bootslot_read_env(&bs, envbuf, err, sizeof(err)) == 0,
              "the environment block reads back at the new geometry");
        for (i = 0; i + 21 <= BOOTSLOT_ENV_BYTES; i++) {
            if (memcmp(envbuf + i, "next_entry=candidate\n", 21) == 0) {
                armed = 1;
            }
        }
        CHECK(armed, "the trial is armed at the new geometry");
    }
    CHECK(bootslot_commit(&bs, err, sizeof(err)) == -1,
          "and an unbooted candidate is still refused there");

    /* The count has to be the range bound, not a number that is merely
       reported: a chain reaching one cluster past the last legal one must be
       refused at this geometry too. */
    expect_open_error("--cluster8-past-end", "a chain leaves the volume");

    /* Cancelling a trial removes the key; it does not set it empty. An empty
       `next_entry` is the marker GRUB leaves after a trial has actually booted
       and `bootslot_commit` requires it, so a cancel that wrote it would let a
       kernel that never ran be made the default. */
    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "the cancel image opens");
    make_kernel(kernel, 60 * 512);
    CHECK(bootslot_install(&bs, kernel, 60 * 512, err, sizeof(err)) == 0, "a kernel installs");
    CHECK(env_has("next_entry=candidate\n"), "and the trial is armed");
    CHECK(bootslot_env_drop(&bs, "next_entry", err, sizeof(err)) == 0, "the trial cancels");
    CHECK(!env_has("next_entry=candidate\n"), "the armed line is gone");
    CHECK(!env_has("\nnext_entry=\n"), "and no consumed marker was written in its place");
    CHECK(bootslot_commit(&bs, err, sizeof(err)) == -1,
          "so a commit after a cancel is refused, because nothing booted");
    CHECK(strstr(err, "no consumed trial") != NULL, "and says why");

    /* The rewritable menu slot: written whole, read back, and refused when the
       text is larger than the slot. Every sector is rewritten whether the new
       text reaches it or not, so a shorter menu leaves nothing of a longer one
       behind, which is the property a torn boot would otherwise expose. */
    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "the menu image opens");
    CHECK(bs.menu.size == 8192, "the menu slot is 8192 bytes");
    {
        static uint8_t menu[8192];
        static uint8_t seen[8192];
        uint32_t i;
        int ok = 1;

        for (i = 0; i < sizeof(menu); i++) {
            menu[i] = (uint8_t)('a' + (i % 26));
        }
        /* A long menu first, so the slot has something other than its fill,
           and then a short one over it. A writer that touched only the sectors
           the new text reaches would leave the long menu's tail behind, and
           GRUB reads the whole file. */
        CHECK(bootslot_write_menu(&bs, menu, 8192, err, sizeof(err)) == 0, "a full menu writes");
        CHECK(bootslot_write_menu(&bs, menu, 400, err, sizeof(err)) == 0,
              "and a short one writes over it");
        for (i = 0; i < sizeof(seen); i++) {
            static uint8_t sector[512];

            if ((i % 512) == 0) {
                CHECK(IO.read(IO.ctx,
                              (bs.data_start +
                               (bs.menu.chain[i / (bs.cluster_sectors * 512)] - 2) *
                                   bs.cluster_sectors +
                               ((i / 512) % bs.cluster_sectors)),
                              sector) == 0,
                      "the slot reads back");
            }
            seen[i] = sector[i % 512];
        }
        for (i = 0; i < 400; i++) {
            if (seen[i] != menu[i]) {
                ok = 0;
            }
        }
        CHECK(ok, "the short menu's bytes are on the disk");
        ok = 1;
        for (i = 400; i < sizeof(seen); i++) {
            if (seen[i] != (uint8_t)'#') {
                ok = 0;
            }
        }
        CHECK(ok, "and the long menu's tail is gone, not left behind");

        CHECK(bootslot_write_menu(&bs, menu, 8193, err, sizeof(err)) == -1,
              "a menu one byte over the slot is refused");
        CHECK(strstr(err, "does not fit the slot") != NULL, "and says so");
        CHECK(bootslot_write_menu(&bs, menu, 8192, err, sizeof(err)) == 0,
              "a menu exactly the slot's size is accepted");
    }

    /* A slot whose size is not a whole number of sectors cannot be rewritten
       whole, so it is refused where the volume is judged, not later,
       where a write would half-succeed. */
    expect_open_error("--menu-odd", "the menu slot is not a whole number of sectors");
    /* And a slot whose directory size does not match its chain is the ordinary
       inconsistency, caught by the same rule as any other file's. */
    expect_open_error("--menu-short", "size does not match its chain");

    /* A disk built before the menu slot existed still opens, and every command
       that does not need a menu still works on it. Only staging one fails, and
       it says why. */
    load("--no-menu");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "a disk with no menu slot still opens");
    CHECK(bs.have_menu == 0, "and reports that it has none");
    CHECK(bs.kernel.size == 2 * 1024 * 1024, "its candidate slot is still readable");
    {
        static uint8_t one[512];

        CHECK(bootslot_write_menu(&bs, one, 1, err, sizeof(err)) == -1,
              "staging a menu on it is refused");
        CHECK(strstr(err, "no menu slot") != NULL, "and says why");
    }

    /* A key that merely ends in next_entry is not the trial state. The writer
       preserves lines it does not recognise, so a block can hold one; commit
       must read the consumed marker beside it and not refuse on the impostor. */
    load("");
    {
        long env_off = (long)(160 + 4160 - 2) * 512;
        const char *junk = "x_next_entry=candidate\n";
        const char *done = "next_entry=\n";
        long end = 0, i;
        size_t add_len = strlen(junk) + strlen(done);

        for (i = 0; i < 1024; i++) {
            if (img.data[env_off + i] == '\n') {
                end = i + 1;
            }
        }
        memmove(img.data + env_off + end + add_len, img.data + env_off + end,
                (size_t)(1024 - end) - add_len);
        memcpy(img.data + env_off + end, junk, strlen(junk));
        memcpy(img.data + env_off + end + strlen(junk), done, strlen(done));
    }
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "the impostor image opens");
    CHECK(bootslot_commit(&bs, err, sizeof(err)) == 0,
          "a line ending in next_entry is not an armed trial");
    CHECK(env_has("saved_entry=candidate\n"), "the impostor did not block the commit");

    /* A dropped environment sector is caught by the block's own readback. */
    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "reopen for env drop");
    img.drop_at = 1; /* the first environment sector */
    CHECK(bootslot_install(&bs, kernel, 60 * 512, err, sizeof(err)) == -1,
          "a dropped environment sector fails the install");
    CHECK(strstr(err, "did not read back") != NULL, "the env readback diagnostic");

    /* A commit under an armed trial names the unbooted candidate. */
    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "reopen for commit gate");
    make_kernel(kernel, 60 * 512);
    CHECK(bootslot_install(&bs, kernel, 60 * 512, err, sizeof(err)) == 0,
          "install before the commit gate");
    CHECK(bootslot_commit(&bs, err, sizeof(err)) == -1,
          "a commit with the trial still armed is refused");
    CHECK(strstr(err, "has not booted") != NULL, "the commit-gate diagnostic");

    /* A commit with no trial state at all is refused too: a fresh block has
       no consumed marker, and neither does an interrupted install, whose slot
       has partial bytes the running kernel never was. */
    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "reopen for fresh commit");
    CHECK(bootslot_commit(&bs, err, sizeof(err)) == -1, "a commit on a fresh block is refused");
    CHECK(strstr(err, "no consumed trial") != NULL, "the fresh-block diagnostic");
    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "reopen for cut commit");
    img.writes_left = 2 + 30; /* the install is cut mid-slot */
    CHECK(bootslot_install(&bs, kernel, 60 * 512, err, sizeof(err)) == -1,
          "the install is cut for the commit case");
    img.writes_left = -1;
    CHECK(bootslot_commit(&bs, err, sizeof(err)) == -1,
          "a commit after an interrupted install is refused");
    CHECK(strstr(err, "no consumed trial") != NULL, "the interrupted-install diagnostic");

    /* The dropped sector: the write callback reports success and writes
       nothing, and only the byte-for-byte readback can catch it. The header
       sectors land early, so sector 40 of the slot is far from anything
       structural validation reads. */
    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "reopen for the drop");
    make_kernel(kernel, 60 * 512);
    img.drop_at = 2 /* env */ + 41; /* the 41st slot write */
    CHECK(bootslot_install(&bs, kernel, 60 * 512, err, sizeof(err)) == -1,
          "a dropped load sector fails the install");
    CHECK(strstr(err, "do not match") != NULL, "the readback diagnostic");

    /* The interruption matrix's order properties, on the bytes:
       a cut mid-slot finds saved_entry=fallback already durable and no
       next_entry; a cut after the last slot write, before the arming env
       write, finds the same. */
    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "reopen for cut 1");
    make_kernel(kernel, 60 * 512);
    img.writes_left = 2 + 30; /* the cut lands mid-slot */
    CHECK(bootslot_install(&bs, kernel, 60 * 512, err, sizeof(err)) == -1,
          "a mid-slot cut fails the install");
    CHECK(env_has("saved_entry=fallback\n"),
          "the saved entry moved to fallback before the slot was touched");
    CHECK(!env_has("next_entry=candidate\n"), "the trial is not armed before the slot is whole");

    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "reopen for cut 2");
    img.writes_left = 2 + 4096 + 1; /* every slot write lands; the cut takes
                                       the first arming env write */
    CHECK(bootslot_install(&bs, kernel, 60 * 512, err, sizeof(err)) == -1,
          "a cut before the arming write fails the install");
    CHECK(env_has("saved_entry=fallback\n"), "the fallback stays saved");
    CHECK(!env_has("next_entry=candidate\n"), "the trial stays unarmed");

    /* A torn env sector: the first 100 bytes of the new block over the old.
       The library reports the write failed; the bytes are the mix the phase 4
       matrix boots. */
    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "reopen for the tear");
    img.tear_at = 1;
    img.tear_bytes = 100;
    CHECK(bootslot_install(&bs, kernel, 60 * 512, err, sizeof(err)) == -1,
          "a torn env write fails the install");

    /* The fixed-address pair. A machine whose network has no DHCP server runs
       from the two menu entries that carry an address, and an install or a
       commit must keep it there: writing the other pair's ids would move it, on
       the next boot, to entries that ask for a lease nobody answers. */
    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "reopen for the static pair");
    bootslot_use_static_entries(&bs, 1);
    make_kernel(kernel, 5000);
    CHECK(bootslot_install(&bs, kernel, 5000, err, sizeof(err)) == 0,
          "an install runs in the fixed-address pair");
    CHECK(env_has("saved_entry=fallback-static\n"),
          "the saved default is the fixed-address fallback");
    CHECK(env_has("next_entry=candidate-static\n"), "the trial arms the fixed-address candidate");
    CHECK(!env_has("saved_entry=fallback\n"), "and not the other pair's fallback");
    CHECK(bootslot_commit(&bs, err, sizeof(err)) == -1,
          "a commit with that trial still armed is refused, as the other pair's is");
    CHECK(bootslot_env_set(&bs, "next_entry", "", err, sizeof(err)) == 0,
          "the trial boot consumes the key");
    CHECK(bootslot_commit(&bs, err, sizeof(err)) == 0,
          "and the commit then runs in the fixed-address pair");
    CHECK(env_has("saved_entry=candidate-static\n"),
          "the commit saves the fixed-address candidate");
    CHECK(!env_has("saved_entry=candidate\n"), "and not the other pair's candidate");

    /* The same machine told it is running from the ordinary pair writes the
       ordinary ids, which is what says the flag is what decides. */
    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "reopen for the ordinary pair");
    bootslot_use_static_entries(&bs, 0);
    CHECK(bootslot_install(&bs, kernel, 5000, err, sizeof(err)) == 0, "an install runs");
    CHECK(env_has("next_entry=candidate\n"), "the ordinary candidate is armed");
    CHECK(!env_has("next_entry=candidate-static\n"), "and not the fixed-address one");

    if (fails) {
        fprintf(stderr, "bootctl_hosttest: %d failure(s)\n", fails);
    }
    return fails;
}
