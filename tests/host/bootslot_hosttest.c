/**
 * Check kernel address bounds, an x86-64 kernel's a.out kludge, and fallback
 * copy failures over resolved files.
 *
 * host-deps: utils/bootslot.c
 *
 * The sector model starts after FAT discovery. It drops a payload write
 * beyond the 8192-byte header prefix, fails a payload read, and drops or
 * tears the final header write. The FAT fixture test covers discovery.
 */

#include <stdio.h>
#include <string.h>

#include "bootslot.h"

#define SLOT_SECTORS 32
#define SLOT_BYTES   (SLOT_SECTORS * BOOTSLOT_SECTOR)
#define ENV_SECTOR   (2 * SLOT_SECTORS)
#define BAD_SECTOR   20

static struct bootslot bs;
static uint8_t disk[ENV_SECTOR + 2][BOOTSLOT_SECTOR];
static uint8_t kernel[SLOT_BYTES];
static char err[128];
static unsigned writes, drop_write, tear_write;
static int bad_read;
static int fails;

/** Count a failed assertion without stopping the remaining cases. */
static void check(int ok, const char *message)
{
    if (!ok) {
        fprintf(stderr, "bootslot-hosttest: fail: %s\n", message);
        fails++;
    }
}

/** Read one sector, or fail at the selected sector. */
static int read_sector(void *ctx, uint32_t lba, void *out)
{
    (void)ctx;
    if (lba >= ENV_SECTOR + 2 || (int)lba == bad_read) {
        return -1;
    }
    memcpy(out, disk[lba], BOOTSLOT_SECTOR);
    return 0;
}

/** Drop or tear the selected write; other writes copy a whole sector. */
static int write_sector(void *ctx, uint32_t lba, const void *in)
{
    (void)ctx;
    if (lba >= ENV_SECTOR + 2) {
        return -1;
    }
    writes++;
    if (writes == drop_write) {
        return 0;
    }
    if (writes == tear_write) {
        memcpy(disk[lba], in, 16);
        return -1;
    }
    memcpy(disk[lba], in, BOOTSLOT_SECTOR);
    return 0;
}

/** Encode an ELF32 word independently of host byte order. */
static void put32(uint8_t *out, uint32_t value)
{
    unsigned i;

    for (i = 0; i < 4; i++) {
        out[i] = (uint8_t)(value >> (8 * i));
    }
}

/** Build a kernel with one load and a valid Multiboot header. */
static void make_kernel(void)
{
    unsigned i;

    memset(kernel, 0, sizeof(kernel));
    memcpy(kernel, "\177ELF\1\1", 6);
    kernel[16] = 2;
    kernel[18] = 3;
    put32(kernel + 24, 0x100000u);
    put32(kernel + 28, 52);
    kernel[42] = 32;
    kernel[44] = 1;
    put32(kernel + 52, 1);
    put32(kernel + 60, 0x100000u);
    put32(kernel + 64, 0x100000u);
    put32(kernel + 68, sizeof(kernel));
    put32(kernel + 72, sizeof(kernel));
    put32(kernel + 96, 0x1BADB002u);
    put32(kernel + 100, 3);
    put32(kernel + 104, 0u - 0x1BADB005u);
    for (i = 128; i < sizeof(kernel); i++) {
        kernel[i] = (uint8_t)(i * 7);
    }
    /* The banner a real kernel carries: an image without it names no policy
       and the validator refuses it. */
    memcpy(kernel + 200, "rach2: disk policy: read-write", 31);
}

/**
 * Build an x86-64 kernel as the x86-64 link lays one out: an ELF64 header
 * and a Multiboot header at 0x1000 whose a.out kludge copies the file from
 * the header on to 1 MiB, the bytes up to 0x3000 past it, and zeroes the
 * memory to 0x5000 past it.
 */
static void make_kernel64(void)
{
    make_kernel();
    memset(kernel, 0, 128);
    memcpy(kernel, "\177ELF\2\1", 6);
    kernel[16] = 2;
    kernel[18] = 62;
    put32(kernel + 0x1000, 0x1BADB002u);
    put32(kernel + 0x1004, 0x10003u);
    put32(kernel + 0x1008, 0u - (0x1BADB002u + 0x10003u));
    put32(kernel + 0x100C, 0x100000u);
    put32(kernel + 0x1010, 0x100000u);
    put32(kernel + 0x1014, 0x103000u);
    put32(kernel + 0x1018, 0x105000u);
    put32(kernel + 0x101C, 0x100020u);
}

/** Check that an x86-64 kernel's load is judged by its a.out kludge. */
static void check_kludge(void)
{
    uint32_t whole = sizeof(kernel);

    make_kernel64();
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == 0,
          "an x86-64 kernel with the a.out kludge is taken");
    put32(kernel + 0x1004, 3);
    put32(kernel + 0x1008, 0u - 0x1BADB005u);
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == -1 &&
              strstr(err, "not ALIGN|MEMINFO|AOUT_KLUDGE") != NULL,
          "an x86-64 kernel without the a.out kludge is refused");
    make_kernel64();
    kernel[18] = 3;
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == -1 &&
              strstr(err, "not an x86-64 executable") != NULL,
          "an ELF64 i386 executable is refused");
    make_kernel64();
    put32(kernel + 0x100C, 0xFF000u);
    put32(kernel + 0x1010, 0xFF000u);
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == -1 &&
              strstr(err, "below the megabyte") != NULL,
          "an x86-64 load below 1 MiB is refused");
    make_kernel64();
    put32(kernel + 0x1010, 0xFE000u + 0x2000u);
    put32(kernel + 0x100C, 0x100000u + 0x2000u);
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == -1 &&
              strstr(err, "outside its load") != NULL,
          "a load that starts before the file is refused");
    make_kernel64();
    put32(kernel + 0x1014, 0x100000u + whole);
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == -1 &&
              strstr(err, "leaves the file") != NULL,
          "an x86-64 load past the file is refused");
    make_kernel64();
    put32(kernel + 0x1018, 0x102000u);
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == -1 &&
              strstr(err, "ends inside the load") != NULL,
          "zeroed memory ending inside the load is refused");
    make_kernel64();
    put32(kernel + 0x101C, 0x103000u);
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == -1 &&
              strstr(err, "outside the load") != NULL,
          "an entry past the copied bytes is refused");
    make_kernel64();
    put32(kernel + 0x1014, 0);
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == 0,
          "a load to the end of the file is taken");
    put32(kernel + 0x101C, 0x100000u + whole - 0x1000u);
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == -1 &&
              strstr(err, "outside the load") != NULL,
          "an entry past the end of the file is refused");
}

/** Initialize two resolved slots and an environment that permits copying. */
static void setup(void)
{
    const char env[] = "# GRUB Environment Block\nsaved_entry=candidate\nnext_entry=\n";
    unsigned i;

    memset(&bs, 0, sizeof(bs));
    bs.io.read = read_sector;
    bs.io.write = write_sector;
    bs.cluster_sectors = 1;
    bs.kernel.size = bs.fallback.size = SLOT_BYTES;
    bs.kernel.chain_len = bs.fallback.chain_len = SLOT_SECTORS;
    bs.grubenv.size = BOOTSLOT_ENV_BYTES;
    bs.grubenv.chain_len = 2;
    for (i = 0; i < SLOT_SECTORS; i++) {
        bs.kernel.chain[i] = 2 + i;
        bs.fallback.chain[i] = 2 + SLOT_SECTORS + i;
    }
    bs.grubenv.chain[0] = 2 + ENV_SECTOR;
    bs.grubenv.chain[1] = 3 + ENV_SECTOR;
    make_kernel();
    memcpy(disk, kernel, SLOT_BYTES);
    memcpy(disk + SLOT_SECTORS, kernel, SLOT_BYTES);
    memset(disk + ENV_SECTOR, '#', BOOTSLOT_ENV_BYTES);
    memcpy(disk[ENV_SECTOR], env, sizeof(env) - 1);
    memset(disk[SLOT_SECTORS + BAD_SECTOR], 0xEE, BOOTSLOT_SECTOR);
    writes = drop_write = tear_write = 0;
    bad_read = -1;
}

/** Reject an unverified fallback before an install changes any sector. */
static void check_retry(void)
{
    unsigned before = writes;

    check(bootslot_install(&bs, kernel, sizeof(kernel), err, sizeof(err)) == -1 &&
              strstr(err, "fallback file is not a bootable kernel") != NULL,
          "install refuses the unverified fallback");
    check(writes == before, "refused install writes no sectors");
}

/** Check a dropped payload write, a failed read and failed final writes. */
static void check_fallback(void)
{
    static const uint8_t zero[BOOTSLOT_SECTOR];

    setup();
    drop_write = 1 + SLOT_SECTORS - BAD_SECTOR;
    check(bootslot_fallback(&bs, err, sizeof(err)) == -1 &&
              strstr(err, "do not match the candidate's") != NULL,
          "dropped payload write fails readback");
    drop_write = 0;
    check(memcmp(disk[SLOT_SECTORS], zero, sizeof(zero)) == 0,
          "payload mismatch leaves the header invalid");
    check_retry();
    check(bootslot_fallback(&bs, err, sizeof(err)) == 0, "fallback retry succeeds");
    check(memcmp(disk, disk + SLOT_SECTORS, SLOT_BYTES) == 0, "fallback retry copies every byte");
    check(bootslot_install(&bs, kernel, sizeof(kernel), err, sizeof(err)) == 0,
          "install accepts the verified fallback");

    setup();
    bad_read = SLOT_SECTORS + BAD_SECTOR;
    check(bootslot_fallback(&bs, err, sizeof(err)) == -1 &&
              strstr(err, "does not read back") != NULL,
          "failed payload read stops the copy");
    bad_read = -1;
    check_retry();

    setup();
    drop_write = SLOT_SECTORS + 1;
    check(bootslot_fallback(&bs, err, sizeof(err)) == -1 &&
              strstr(err, "do not match the candidate's") != NULL,
          "dropped final header fails readback");
    drop_write = 0;
    check_retry();

    setup();
    tear_write = SLOT_SECTORS + 1;
    check(bootslot_fallback(&bs, err, sizeof(err)) == -1 && strstr(err, "does not write") != NULL,
          "torn final header reports failure");
}

/** Check the largest representable end and memory extents past that end. */
static void check_bounds(void)
{
    make_kernel();
    put32(kernel + 24, 0xFFFFF000u);
    put32(kernel + 60, 0xFFFFF000u);
    put32(kernel + 64, 0xFFFFF000u);
    put32(kernel + 68, 512);
    put32(kernel + 72, 0xFFF);
    check(bootslot_validate_kernel(kernel, 512, sizeof(kernel), err, sizeof(err)) == 0,
          "load ending at 0xffffffff is accepted from a header prefix");
    put32(kernel + 72, 0x1000);
    check(bootslot_validate_kernel(kernel, 512, sizeof(kernel), err, sizeof(err)) == -1 &&
              strstr(err, "leaves the 32-bit address space") != NULL,
          "load end at 4 GiB is refused");
    put32(kernel + 72, 0x2000);
    check(bootslot_validate_kernel(kernel, 512, sizeof(kernel), err, sizeof(err)) == -1 &&
              strstr(err, "leaves the 32-bit address space") != NULL,
          "load crossing 4 GiB is refused");
}

/**
 * Check the disk policy a whole image names.
 *
 * The read-only build refuses every write command, so a machine booted from
 * it cannot mount its disk root and stops with nothing mounted.
 */
static void check_policy(void)
{
    uint32_t whole = sizeof(kernel);

    make_kernel();
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == 0,
          "the read-write build is taken");
    memcpy(kernel + 200, "rach2: disk policy: read-only", 30);
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == -1 &&
              strstr(err, "read-only disk build") != NULL,
          "the read-only build is refused as a machine's kernel");
    make_kernel();
    memset(kernel + 200, 0, 31);
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == -1 &&
              strstr(err, "names no one disk policy") != NULL,
          "an image with no policy banner is refused");
}

/** Run the focused boot-slot checks. */
int main(void)
{
    check_fallback();
    check_bounds();
    check_policy();
    check_kludge();
    printf("bootslot-hosttest: result: %s: %d failures\n", fails ? "fail" : "pass", fails);
    return fails != 0;
}
