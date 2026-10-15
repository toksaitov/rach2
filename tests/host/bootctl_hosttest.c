/**
 * bootctl_hosttest.c: utils/bootslot.c, the boot-slot library bootctl
 * drives, over FAT32 images in memory.
 *
 * host-deps: utils/bootslot.c tests/host/compat/fixdir_host.c
 *
 * The fixtures come from tests/host/mkfatfixture.c, a FAT32 writer written
 * apart from bootslot.c, so two implementations of the format meet over
 * every image. The well-formed image (a volume label, a long-name entry, dot
 * entries, an empty file, the four named files) must open and install; each
 * malformed image must be refused with its own diagnostic, matched by
 * substring so a mutation cannot pass by failing a different rule. The
 * write callback injects the faults: it can report success while dropping a
 * chosen sector, which only the byte-for-byte readback can catch; stop the
 * run after any chosen write, which leaves the states of the interruption
 * matrix in dev-log/23-install/plan.md, whose order the rows check on the
 * bytes; and tear a sector into a prefix of the new bytes over the old.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bootslot.h"
#include "fixdir_host.h"

#define IMG_BYTES (8192 * 512)

static int fails;

#define CHECK(cond, what)                          \
    do {                                           \
        if (!(cond)) {                             \
            fails++;                               \
            fprintf(stderr, "  FAIL: %s\n", what); \
        }                                          \
    } while (0)

/* The image under test, in memory, with the injector's controls. A control
   of -1 is off. */
static struct {
    uint8_t data[IMG_BYTES];
    long writes_left; /* stop (report failure) when this reaches 0 */
    long drop_at;     /* report success but write nothing at this count */
    long tear_at;     /* write only the first tear_bytes at this count */
    int tear_bytes;
    long count; /* writes so far, the first counted as 1 */
} img;

/** Read sector `lba` of the image into `buf`. Returns 0, or -1 for a sector
 *  past the image. */
static int io_read(void *ctx, uint32_t lba, void *buf)
{
    (void)ctx;
    if ((long)lba * 512 + 512 > IMG_BYTES) {
        return -1;
    }
    memcpy(buf, img.data + (long)lba * 512, 512);
    return 0;
}

/**
 * Write sector `lba` of the image from `buf`, through the injector. The
 * write numbered `drop_at` is reported done and not made. The one numbered
 * `tear_at` stores its first `tear_bytes` and fails, a cut inside the
 * sector. When `writes_left` counts down to 0 the write fails and stores
 * nothing, a cut after the write before it. Returns 0, or -1 for a cut and
 * for a sector past the image.
 */
static int io_write(void *ctx, uint32_t lba, const void *buf)
{
    (void)ctx;
    if ((long)lba * 512 + 512 > IMG_BYTES) {
        return -1;
    }
    img.count++;
    if (img.count == img.drop_at) {
        return 0;
    }
    if (img.count == img.tear_at) {
        memcpy(img.data + (long)lba * 512, buf, (size_t)img.tear_bytes);
        return -1;
    }
    if (img.writes_left > 0 && --img.writes_left == 0) {
        return -1;
    }
    memcpy(img.data + (long)lba * 512, buf, 512);
    return 0;
}

static const struct bootslot_io IO = {io_read, io_write, NULL};

/** Build the fixture `mkfatfixture` makes with the option `opt`, load it
 *  into `img` and clear the injector. Exits with status 1 when the builder
 *  cannot be built or run or the image does not load, since no row means
 *  anything then. The builder is compiled at the first load, into the
 *  test's own fixture directory (compat/fixdir_host.c), since the suite
 *  compiles and runs this test directly and has no build system to ask. */
static void load(const char *opt)
{
    static int built;
    char cmd[2048];
    FILE *f;

    if (!built) {
        fix_format(cmd, sizeof(cmd), "${CC:-cc} -O2 -o %s tests/host/mkfatfixture.c",
                   fix_path("mkfatfixture"));
        if (system(cmd) != 0) {
            fprintf(stderr, "bootctl_hosttest: cannot build the fixture builder\n");
            exit(1);
        }
        built = 1;
    }
    fix_format(cmd, sizeof(cmd), "%s %s %s", fix_path("mkfatfixture"), fix_path("fat_case.img"),
               opt);
    if (system(cmd) != 0) {
        fprintf(stderr, "bootctl_hosttest: fixture builder failed\n");
        exit(1);
    }
    f = fopen(fix_path("fat_case.img"), "rb");
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

/** Store `v` at `p` in the host's byte order, which the ELF and Multiboot
 *  fields take to be little-endian. */
static void put32(uint8_t *p, uint32_t v)
{
    memcpy(p, &v, 4);
}

/**
 * Write to `out` a valid minimal kernel image of `size` bytes, filled
 * differently from the fixture's, so an install changes the slot. It is an
 * ELF32 i386 executable with one load of the whole file at 1 MiB, entered at
 * 1 MiB; a Multiboot header at offset 96 with the flags ALIGN|MEMINFO; the
 * multiboot2 header the GPT menu loads by at offset 112, with only its end
 * tag; and, when `size` is at least 231, the read-write policy banner at
 * offset 200, without which an image names no policy and is refused.
 */
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
    put32(out + 112, 0xE85250D6u);
    put32(out + 116, 0);
    put32(out + 120, 24);
    put32(out + 124, (uint32_t)(0u - (0xE85250D6u + 24u)));
    put32(out + 128, 0);
    put32(out + 132, 8);
    if (size >= 231) {
        memcpy(out + 200, "rach2: disk policy: read-write", 31);
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

/**
 * Check the disk policy a whole image names.
 *
 * A kernel built read-only issues no write command to a disk, so its mount
 * of a disk root fails and the machine stops with nothing mounted;
 * installing one costs a boot and a trip to the machine. An image that
 * names neither policy, or both, is refused too: the banner is how a built
 * kernel's policy is read, and nothing else in the image says it. A header
 * prefix cannot show a banner past it, so the policy is not judged there;
 * that is the readback a slot's own check makes.
 */
static void check_policy(void)
{
    make_kernel(kernel, 5000);
    memcpy(kernel + 200, "rach2: disk policy: read-only", 30);
    CHECK(bootslot_validate_kernel(kernel, 5000, 5000, err, sizeof(err)) == -1 &&
              strstr(err, "read-only disk build") != NULL,
          "the read-only build is refused as a machine's kernel");
    make_kernel(kernel, 5000);
    memset(kernel + 200, 0, 31);
    CHECK(bootslot_validate_kernel(kernel, 5000, 5000, err, sizeof(err)) == -1 &&
              strstr(err, "names no one disk policy") != NULL,
          "an image with no policy banner is refused");
    make_kernel(kernel, 5000);
    memcpy(kernel + 300, "rach2: disk policy: read-only", 30);
    CHECK(bootslot_validate_kernel(kernel, 5000, 5000, err, sizeof(err)) == -1 &&
              strstr(err, "names no one disk policy") != NULL,
          "an image carrying both banners is refused");
    make_kernel(kernel, 5000);
    CHECK(bootslot_validate_kernel(kernel, 5000, 5000, err, sizeof(err)) == 0,
          "the read-write build is taken");
    make_kernel(kernel, 5000);
    memset(kernel + 200, 0, 31);
    CHECK(bootslot_validate_kernel(kernel, 136, 5000, err, sizeof(err)) == 0,
          "a header prefix is judged without the policy");
}

/** Load the fixture of option `opt` and require `bootslot_open` to refuse
 *  it with a diagnostic containing `needle`. */
static void expect_open_error(const char *opt, const char *needle)
{
    load(opt);
    err[0] = '\0';
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == -1, opt);
    CHECK(strstr(err, needle) != NULL, needle);
}

/** Report the failures, or mark the fixture directory for removal when there
 *  were none. Returns the count of failures, at most 255, since the exit
 *  status keeps only its low eight bits. */
static int finish(void)
{
    if (fails) {
        fprintf(stderr, "bootctl_hosttest: %d failure(s)\n", fails);
    } else {
        fix_passed();
    }
    return fails > 255 ? 255 : fails;
}

/**
 * Run the rows, in this order. Returns the count of failed checks, at most
 * 255.
 *
 * - The well-formed image opens, the scan passing over the label, the
 *   long-name entry, the dot entries and the empty file. An install saves
 *   the fallback, arms the one-boot trial, and writes the kernel at the
 *   slot's start with zero padding after it. A commit is allowed only after
 *   the trial boot consumed the arming line; GRUB's consumption (the value
 *   emptied, the line kept) is made on the bytes, and the commit then
 *   replaces the saved entry.
 * - The copy of the committed candidate into the fallback is refused by
 *   name where the fallback file is shorter than the slot. On the
 *   `--fallback-slot` image it is refused while the trial is armed, and
 *   after the commit writes the fallback and reads it back whole. The copy
 *   zeroes the fallback's first sector first and writes it last, so a copy
 *   cut part way leaves no header and the next install refuses it by name.
 *   It writes the rest from the last sector down, so the write of the third
 *   sector is number 1 + (slot sectors - 2); dropped, with the sector
 *   spoiled beforehand, it is caught by the readback.
 * - An unknown variable, put after the block's last newline, survives the
 *   install's environment writes.
 * - The fragmented candidate installs cluster by cluster: the kernel's
 *   bytes in the first run, the zero padding across the hole into the
 *   second run, and the hole not written.
 * - Each malformed volume is refused with its own diagnostic, and so is a
 *   kernel larger than the slot. Kernel validation refuses a non-ELF, a
 *   broken Multiboot checksum, no program headers, a load past the file, an
 *   entry outside every load, a physical address away from the virtual one,
 *   and Multiboot flags with the a.out kludge; then `check_policy`.
 * - A fallback that is not a bootable kernel stops the install before the
 *   first environment write. A grub.cfg whose size does not match its chain
 *   refuses the volume, since every reachable regular file is held to the
 *   rule, and not only the named ones.
 * - A stale armed trial is dropped by the install's first environment
 *   write: cut mid-slot, the block has the fallback saved and no armed line.
 * - At eight sectors per cluster (`--cluster8`), what the library reports is
 *   in sectors and bytes, so the rows test its multiplication by the cluster
 *   size. 8192 sectors less a data start of 160, divided by eight, is 1004
 *   clusters; a count computed as though a cluster were a sector would be
 *   8032, larger than the volume, and bound nothing. The armed trial is read
 *   back through the library, since `env_has` reads at the default
 *   geometry's offset. A chain one cluster past the last is refused, so the
 *   count is a range bound and not only a number reported.
 * - A cancel removes `next_entry` and does not set it empty. An empty
 *   `next_entry` is the marker GRUB leaves once a trial has booted, which
 *   `bootslot_commit` requires, so a cancel that wrote it would let a
 *   kernel that never ran become the default.
 * - The menu slot: a long menu and then a short one over it. Every sector is
 *   rewritten, so nothing of the long menu is left behind for GRUB, which
 *   reads the whole file. A menu over the slot's size is refused and one of
 *   exactly its size taken. A slot that is not a whole number of sectors
 *   cannot be rewritten whole, so it is refused when the volume is opened,
 *   not at a write that would half-succeed; one whose size does not match
 *   its chain is refused by the rule for every file. A disk with no menu
 *   slot opens, and only staging a menu on it fails.
 * - A key that only ends in `next_entry`, which the writer keeps as an
 *   unknown line, does not block a commit beside the consumed marker.
 * - A dropped environment sector, the install's first write, is caught by
 *   the block's readback. A commit is refused with the trial armed, on a
 *   fresh block, and after an interrupted install; the last two have no
 *   consumed marker, and an interrupted slot has bytes the running kernel
 *   never was.
 * - A dropped slot sector, the 41st slot write after the two environment
 *   writes, is sector 40, away from everything structural validation
 *   reads, so only the readback catches it.
 * - The order of the interruption matrix, on the bytes: a cut at the 30th
 *   slot write, after the two environment writes, and a cut at the arming
 *   write after all 4096, both leave the fallback saved and the trial
 *   unarmed. A torn environment write, 100 bytes of the new block over the
 *   old, fails the install.
 * - `bootslot_env_set`, which `bootctl env` and `bootctl trial` use, writes a
 *   menu id and an empty value that read back; a setter that reported
 *   success and wrote nothing would pass every row before it.
 */
int main(void)
{
    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "wellformed opens");
    CHECK(bs.kernel.size == 4096 * 512, "the candidate slot is 2 MiB");
    CHECK(bs.grubenv.size == 1024, "grubenv is 1024 bytes");

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

    CHECK(bootslot_fallback(&bs, err, sizeof(err)) == -1 &&
              strstr(err, "shorter than the candidate slot") != NULL,
          "a fallback file shorter than the slot refuses the copy by name");

    load("--fallback-slot");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "the fallback-slot image opens");
    CHECK(bs.kernel.size == bs.fallback.size, "and its two slots are one length");
    make_kernel(kernel, 5000);
    CHECK(bootslot_install(&bs, kernel, 5000, err, sizeof(err)) == 0, "a kernel installs there");
    CHECK(bootslot_fallback(&bs, err, sizeof(err)) == -1 && strstr(err, "trial is armed") != NULL,
          "an installed, untried candidate is refused as the fallback's source");
    {
        static uint8_t env[1024];
        long i;

        CHECK(bootslot_read_env(&bs, env, err, sizeof(err)) == 0, "the block reads");
        for (i = 0; i + 21 <= 1024; i++) {
            if (memcmp(env + i, "next_entry=candidate\n", 21) == 0) {
                memmove(env + i + 11, env + i + 20, (size_t)(1024 - i - 20));
                memset(env + 1024 - 9, '#', 9);
                break;
            }
        }
        for (i = 0; i < (long)bs.grubenv.chain_len; i++) {
            long at =
                ((long)bs.data_start + ((long)bs.grubenv.chain[i] - 2) * bs.cluster_sectors) * 512;
            long len = (long)bs.cluster_sectors * 512;

            if (i * len < 1024) {
                memcpy(img.data + at, env + i * len,
                       (size_t)(1024 - i * len < len ? 1024 - i * len : len));
            }
        }
    }
    CHECK(bootslot_commit(&bs, err, sizeof(err)) == 0, "the tried candidate commits");
    img.writes_left = 3;
    CHECK(bootslot_fallback(&bs, err, sizeof(err)) == -1,
          "a fallback copy the device stops part way fails");
    img.writes_left = 0;
    CHECK(bootslot_install(&bs, kernel, 5000, err, sizeof(err)) == -1 &&
              strstr(err, "fallback") != NULL,
          "and the next install refuses the fallback it left, by name");
    CHECK(bootslot_fallback(&bs, err, sizeof(err)) == 0,
          "the committed candidate becomes the fallback");
    {
        int same = 1;
        uint32_t c;

        for (c = 0; c < bs.kernel.chain_len && c < bs.fallback.chain_len; c++) {
            long a =
                ((long)bs.data_start + ((long)bs.kernel.chain[c] - 2) * bs.cluster_sectors) * 512;
            long b =
                ((long)bs.data_start + ((long)bs.fallback.chain[c] - 2) * bs.cluster_sectors) * 512;

            if (memcmp(img.data + a, img.data + b, (size_t)bs.cluster_sectors * 512) != 0) {
                same = 0;
            }
        }
        CHECK(same && bs.kernel.chain_len == bs.fallback.chain_len,
              "and the fallback file's bytes are the candidate slot's, whole");
        CHECK(memcmp(img.data + ((long)bs.data_start +
                                 ((long)bs.fallback.chain[0] - 2) * bs.cluster_sectors) *
                                    512,
                     kernel, 5000) == 0,
              "beginning with the kernel that was installed");
    }
    memset(img.data +
               ((long)bs.data_start + ((long)bs.fallback.chain[2] - 2) * bs.cluster_sectors) * 512,
           0xEE, 512);
    img.count = 0;
    img.drop_at = 1 + (long)(bs.kernel.size / 512) - 2;
    CHECK(bootslot_fallback(&bs, err, sizeof(err)) == -1 &&
              strstr(err, "do not match the candidate's") != NULL,
          "a fallback write the device dropped is caught by the readback");
    img.drop_at = -1;
    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "the well-formed image opens again");

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

    expect_open_error("--fats-differ", "the two FAT copies disagree");
    expect_open_error("--loop", "two chains share a cluster");
    expect_open_error("--out-of-range", "a chain leaves the volume");
    expect_open_error("--short-chain", "size does not match its chain");
    expect_open_error("--cross-link", "two chains share a cluster");
    expect_open_error("--no-signature", "this is not a boot partition");
    expect_open_error("--bpb-sector-size", "sector size is not 512");
    expect_open_error("--bpb-fat-oversized", "FAT region is out of bounds");
    expect_open_error("--bpb-root-out", "root cluster is out of range");
    expect_open_error("--bpb-overflow", "FAT regions do not fit");
    expect_open_error("--no-grubenv", "no /boot/grub/grubenv");

    load("--small-candidate");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "small slot opens");
    make_kernel(kernel, 16384);
    CHECK(bootslot_install(&bs, kernel, 16384, err, sizeof(err)) == -1,
          "an oversized kernel is refused");
    CHECK(strstr(err, "does not fit") != NULL, "the slot-size diagnostic");

    make_kernel(kernel, 5000);
    kernel[0] = 'X';
    CHECK(bootslot_validate_kernel(kernel, 5000, 5000, err, sizeof(err)) == -1 &&
              strstr(err, "not an ELF") != NULL,
          "a non-ELF is refused");
    make_kernel(kernel, 5000);
    kernel[104] ^= 0x40;
    CHECK(bootslot_validate_kernel(kernel, 5000, 5000, err, sizeof(err)) == -1 &&
              strstr(err, "no multiboot header") != NULL,
          "a broken checksum is refused");
    make_kernel(kernel, 5000);
    kernel[44] = 0; /* e_phnum */
    CHECK(bootslot_validate_kernel(kernel, 5000, 5000, err, sizeof(err)) == -1 &&
              strstr(err, "not a loader's") != NULL,
          "a kernel with no program headers is refused");
    make_kernel(kernel, 5000);
    put32(kernel + 68, 6000); /* p_filesz */
    put32(kernel + 72, 6000);
    CHECK(bootslot_validate_kernel(kernel, 5000, 5000, err, sizeof(err)) == -1 &&
              strstr(err, "leaves the file") != NULL,
          "a load past the file is refused");
    make_kernel(kernel, 5000);
    put32(kernel + 24, 0x200000); /* e_entry */
    CHECK(bootslot_validate_kernel(kernel, 5000, 5000, err, sizeof(err)) == -1 &&
              strstr(err, "outside every load") != NULL,
          "an entry outside every load is refused");
    make_kernel(kernel, 5000);
    put32(kernel + 64, 0); /* p_paddr, where a Multiboot loader puts the load */
    CHECK(bootslot_validate_kernel(kernel, 5000, 5000, err, sizeof(err)) == -1 &&
              strstr(err, "physical address differs") != NULL,
          "a physical address away from the virtual is refused");
    make_kernel(kernel, 5000);
    put32(kernel + 100, 0x10003); /* flags: bit 16 is the a.out kludge */
    put32(kernel + 104, (uint32_t)(0u - (0x1BADB002u + 0x10003)));
    CHECK(bootslot_validate_kernel(kernel, 5000, 5000, err, sizeof(err)) == -1 &&
              strstr(err, "flags are not ALIGN") != NULL,
          "multiboot flags away from ALIGN|MEMINFO are refused");

    check_policy();

    load("--bad-fallback");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "bad-fallback opens");
    make_kernel(kernel, 5000);
    CHECK(bootslot_install(&bs, kernel, 5000, err, sizeof(err)) == -1,
          "a broken fallback stops the install");
    CHECK(strstr(err, "fallback file is not a bootable kernel") != NULL, "the fallback diagnostic");
    CHECK(env_has("saved_entry=candidate\n"), "the environment was never touched");

    expect_open_error("--cfg-short", "size does not match its chain");

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

    load("--cluster8");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "a 4 KiB-cluster volume opens");
    CHECK(bs.cluster_sectors == 8, "the volume reports eight sectors per cluster");
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

    expect_open_error("--cluster8-past-end", "a chain leaves the volume");

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

    expect_open_error("--menu-odd", "the menu slot is not a whole number of sectors");
    expect_open_error("--menu-short", "size does not match its chain");

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

    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "reopen for env drop");
    img.drop_at = 1;
    CHECK(bootslot_install(&bs, kernel, 60 * 512, err, sizeof(err)) == -1,
          "a dropped environment sector fails the install");
    CHECK(strstr(err, "did not read back") != NULL, "the env readback diagnostic");

    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "reopen for commit gate");
    make_kernel(kernel, 60 * 512);
    CHECK(bootslot_install(&bs, kernel, 60 * 512, err, sizeof(err)) == 0,
          "install before the commit gate");
    CHECK(bootslot_commit(&bs, err, sizeof(err)) == -1,
          "a commit with the trial still armed is refused");
    CHECK(strstr(err, "has not booted") != NULL, "the commit-gate diagnostic");

    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "reopen for fresh commit");
    CHECK(bootslot_commit(&bs, err, sizeof(err)) == -1, "a commit on a fresh block is refused");
    CHECK(strstr(err, "no consumed trial") != NULL, "the fresh-block diagnostic");
    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "reopen for cut commit");
    img.writes_left = 2 + 30;
    CHECK(bootslot_install(&bs, kernel, 60 * 512, err, sizeof(err)) == -1,
          "the install is cut for the commit case");
    img.writes_left = -1;
    CHECK(bootslot_commit(&bs, err, sizeof(err)) == -1,
          "a commit after an interrupted install is refused");
    CHECK(strstr(err, "no consumed trial") != NULL, "the interrupted-install diagnostic");

    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "reopen for the drop");
    make_kernel(kernel, 60 * 512);
    img.drop_at = 2 + 41;
    CHECK(bootslot_install(&bs, kernel, 60 * 512, err, sizeof(err)) == -1,
          "a dropped load sector fails the install");
    CHECK(strstr(err, "do not match") != NULL, "the readback diagnostic");

    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "reopen for cut 1");
    make_kernel(kernel, 60 * 512);
    img.writes_left = 2 + 30;
    CHECK(bootslot_install(&bs, kernel, 60 * 512, err, sizeof(err)) == -1,
          "a mid-slot cut fails the install");
    CHECK(env_has("saved_entry=fallback\n"),
          "the saved entry moved to fallback before the slot was touched");
    CHECK(!env_has("next_entry=candidate\n"), "the trial is not armed before the slot is whole");

    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "reopen for cut 2");
    img.writes_left = 2 + 4096 + 1;
    CHECK(bootslot_install(&bs, kernel, 60 * 512, err, sizeof(err)) == -1,
          "a cut before the arming write fails the install");
    CHECK(env_has("saved_entry=fallback\n"), "the fallback stays saved");
    CHECK(!env_has("next_entry=candidate\n"), "the trial stays unarmed");

    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "reopen for the tear");
    img.tear_at = 1;
    img.tear_bytes = 100;
    CHECK(bootslot_install(&bs, kernel, 60 * 512, err, sizeof(err)) == -1,
          "a torn env write fails the install");

    load("");
    CHECK(bootslot_open(&bs, &IO, err, sizeof(err)) == 0, "reopen for the setter");
    CHECK(bootslot_env_set(&bs, "saved_entry", "recovery", err, sizeof(err)) == 0,
          "the setter writes a menu id");
    CHECK(env_has("saved_entry=recovery\n"), "and the block holds what it wrote");
    CHECK(bootslot_env_set(&bs, "next_entry", "", err, sizeof(err)) == 0,
          "the setter writes the empty value a consumed trial leaves");
    CHECK(env_has("next_entry=\n"), "and that line is there too");

    return finish();
}
