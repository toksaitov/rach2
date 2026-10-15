/**
 * bootslot_hosttest.c: the kernel checks and the fallback copy of
 * utils/bootslot.c: load address bounds, an x86-64 kernel's a.out kludge,
 * the multiboot2 header, the disk policy, a chain's cluster count, and copy
 * failures over resolved files.
 *
 * The sector model starts after FAT discovery, which bootctl_hosttest.c
 * tests. It drops a payload write beyond the 8192-byte header prefix, fails
 * a payload read, and drops or tears the final header write. The source is
 * included, so that the count a chain must have is checked at a size no
 * fixture reaches: a chain past 4 GiB.
 */

#include <stdio.h>
#include <string.h>

#include "bootslot.c"

#define SLOT_SECTORS 32
#define SLOT_BYTES   (SLOT_SECTORS * BOOTSLOT_SECTOR)
#define ENV_SECTOR   (2 * SLOT_SECTORS)
#define BAD_SECTOR   20

static struct bootslot bs;
static uint8_t disk[ENV_SECTOR + 2][BOOTSLOT_SECTOR];
static uint8_t kernel[SLOT_BYTES];
static char err[128];
/* The faults of the model: the numbers, counted from 1, of the write to drop
   and the write to tear, 0 for none, and the sector that fails to read, -1
   for none. */
static unsigned writes, drop_write, tear_write;
static int bad_read;
static int fails;

/** Count a failed check and print `message`, without stopping the
 *  remaining cases. */
static void check(int ok, const char *message)
{
    if (!ok) {
        fprintf(stderr, "bootslot-hosttest: fail: %s\n", message);
        fails++;
    }
}

/** Read sector `lba` of the model into `out`. Returns 0, or -1 for the
 *  sector `bad_read` names and for one past the model. */
static int read_sector(void *ctx, uint32_t lba, void *out)
{
    (void)ctx;
    if (lba >= ENV_SECTOR + 2 || (int)lba == bad_read) {
        return -1;
    }
    memcpy(out, disk[lba], BOOTSLOT_SECTOR);
    return 0;
}

/** Write sector `lba` of the model from `in`, counting the write. Write
 *  number `drop_write` is dropped and reported done; write number
 *  `tear_write` stores 16 bytes and fails; others store the whole sector.
 *  Returns 0, or -1 for a torn write and for a sector past the model. */
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

/** Store `value` little-endian in the four bytes at `out`, whatever the
 *  host's byte order. */
static void put32(uint8_t *out, uint32_t value)
{
    unsigned i;

    for (i = 0; i < 4; i++) {
        out[i] = (uint8_t)(value >> (8 * i));
    }
}

/**
 * Build in `kernel` an i386 kernel the checks take: an ELF32 executable
 * with one load of the whole file at 1 MiB, entered at 1 MiB; a Multiboot
 * header at offset 96 with the flags ALIGN|MEMINFO; the multiboot2 header
 * the GPT menu loads by at offset 112, with only its end tag; a pattern in
 * the rest; and the read-write policy banner at offset 200. An image
 * without a banner names no policy, and the check refuses it.
 */
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
    put32(kernel + 112, 0xE85250D6u);
    put32(kernel + 116, 0);
    put32(kernel + 120, 24);
    put32(kernel + 124, 0u - (0xE85250D6u + 24u));
    put32(kernel + 128, 0);
    put32(kernel + 132, 8);
    memcpy(kernel + 200, "rach2: disk policy: read-write", 31);
}

/**
 * Build an x86-64 kernel as the x86-64 link lays one out: an ELF64 header
 * and a Multiboot header at 0x1000 whose a.out kludge copies the file from
 * the header on to 1 MiB, the bytes up to 0x3000 past it, and zeroes the
 * memory to 0x5000 past it; then the multiboot2 header at 0x1020, whose
 * address and entry tags give the same load.
 */
static void make_kernel64(void)
{
    make_kernel();
    memset(kernel, 0, 136);
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
    put32(kernel + 0x1020, 0xE85250D6u);
    put32(kernel + 0x1024, 0);
    put32(kernel + 0x1028, 0x40u);
    put32(kernel + 0x102C, 0u - (0xE85250D6u + 0x40u));
    put32(kernel + 0x1030, 2);
    put32(kernel + 0x1034, 24);
    put32(kernel + 0x1038, 0x100020u);
    put32(kernel + 0x103C, 0x100000u);
    put32(kernel + 0x1040, 0x103000u);
    put32(kernel + 0x1044, 0x105000u);
    put32(kernel + 0x1048, 3);
    put32(kernel + 0x104C, 12);
    put32(kernel + 0x1050, 0x100020u);
    put32(kernel + 0x1054, 0);
    put32(kernel + 0x1058, 0);
    put32(kernel + 0x105C, 8);
}

/** Check that an x86-64 kernel's load is judged by its a.out kludge: the
 *  kernel of `make_kernel64` is taken, and each of the faults the messages
 *  name is refused with its own diagnostic. */
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
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == -1 &&
              strstr(err, "which the slot pads") != NULL,
          "a load to the end of the file, which the slot pads, is refused");
    make_kernel64();
    memset(kernel + 0x1000, 0, 32);
    put32(kernel + 0x1FF0, 0x1BADB002u);
    put32(kernel + 0x1FF4, 0x10003u);
    put32(kernel + 0x1FF8, 0u - (0x1BADB002u + 0x10003u));
    put32(kernel + 0x1FFC, 0x100FF0u);
    put32(kernel + 0x2000, 0x100000u);
    put32(kernel + 0x2004, 0x103000u);
    put32(kernel + 0x2008, 0x105000u);
    put32(kernel + 0x200C, 0x100020u);
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == -1 &&
              strstr(err, "first 8192 bytes") != NULL,
          "a Multiboot header that ends past the 8192 bytes the slot checks read is refused");
}

/** Check that the multiboot2 header the GPT menu loads by is required and
 * judged: its tags, and an x86-64 kernel's load from them. */
static void check_multiboot2(void)
{
    uint32_t whole = sizeof(kernel);

    make_kernel();
    put32(kernel + 112, 0);
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == -1 &&
              strstr(err, "no multiboot2 header") != NULL,
          "an i386 kernel without a multiboot2 header is refused");
    make_kernel();
    put32(kernel + 120, 25);
    put32(kernel + 124, 0u - (0xE85250D6u + 25u));
    put32(kernel + 128, 4);
    put32(kernel + 132, 9);
    put32(kernel + 144, 0);
    put32(kernel + 148, 8);
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == -1 &&
              strstr(err, "no end tag") != NULL,
          "a multiboot2 tag padded past the header's end is refused");
    make_kernel64();
    put32(kernel + 0x1020, 0);
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == -1 &&
              strstr(err, "no multiboot2 header") != NULL,
          "an x86-64 kernel without a multiboot2 header is refused");
    make_kernel64();
    put32(kernel + 0x1030, 0x10005u);
    put32(kernel + 0x1048, 0x10004u);
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == -1 &&
              strstr(err, "lacks the address tag") != NULL,
          "an x86-64 multiboot2 header without its address and entry tags is refused");
    make_kernel64();
    put32(kernel + 0x1048, 0x10004u);
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == -1 &&
              strstr(err, "one of its address and entry tags") != NULL,
          "a multiboot2 address tag without an entry tag is refused");
    make_kernel64();
    put32(kernel + 0x1030, 6);
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == -1 &&
              strstr(err, "changes how the loader reads") != NULL,
          "a multiboot2 tag the loader must honour and the check does not know is refused");
    make_kernel64();
    put32(kernel + 0x105C, 0x10u);
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == -1 &&
              strstr(err, "leaves its header") != NULL,
          "a multiboot2 tag past its header is refused");
    make_kernel64();
    put32(kernel + 0x1040, 0x100000u + whole);
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == -1 &&
              strstr(err, "leaves the file") != NULL,
          "a multiboot2 load past the file is refused");
    make_kernel64();
    put32(kernel + 0x1040, 0);
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == -1 &&
              strstr(err, "which the slot pads") != NULL,
          "a multiboot2 load to the end of the file is refused");
    make_kernel64();
    put32(kernel + 0x1050, 0x103000u);
    check(bootslot_validate_kernel(kernel, whole, whole, err, sizeof(err)) == -1 &&
              strstr(err, "outside the load") != NULL,
          "a multiboot2 entry past the copied bytes is refused");
}

/** Check a chain's cluster count against its file's size at clusters of
 * 64 KiB, where the chain's bytes pass 32 bits. */
static void check_chain(void)
{
    memset(&bs, 0, sizeof(bs));
    bs.cluster_sectors = 128;
    check(chain_fits(&bs, 1, 65536) && chain_fits(&bs, 2, 65537) && chain_fits(&bs, 0, 0),
          "a chain of as many clusters as its file fills fits");
    check(!chain_fits(&bs, 1, 0) && !chain_fits(&bs, 2, 65536),
          "a chain longer than its file fills does not fit");
    check(!chain_fits(&bs, 65537, 65536),
          "a chain of 4 GiB and 64 KiB does not fit a file of 64 KiB");
}

/** Make `bs` a volume with three resolved files over the model: the kernel
 *  slot in sectors 0 to 31 and the fallback in 32 to 63, both holding the
 *  kernel of `make_kernel`, and an environment block in sectors 64 and 65
 *  that permits copying. Sector `BAD_SECTOR` of the fallback is 0xEE, so
 *  the fallback differs from the kernel until a copy writes that sector.
 *  Clears the faults. */
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

/** Check that an install refuses an unverified fallback before it writes
 *  any sector. */
static void check_retry(void)
{
    unsigned before = writes;

    check(bootslot_install(&bs, kernel, sizeof(kernel), err, sizeof(err)) == -1 &&
              strstr(err, "fallback file is not a bootable kernel") != NULL,
          "install refuses the unverified fallback");
    check(writes == before, "refused install writes no sectors");
}

/**
 * Check `bootslot_fallback`, the copy of the kernel slot into the fallback,
 * against a dropped payload write, a failed payload read, and a dropped or
 * torn final header write. Each fails the copy, and after each but the torn
 * write the fallback's header stays invalid, so an install refuses the
 * fallback (`check_retry`). After the dropped payload write a retry copies
 * every byte and the install is taken.
 */
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

/** Check an i386 load's memory end against the 32-bit address space: a load
 *  ending at 0xffffffff is taken from a 512-byte header prefix, and one
 *  ending at 4 GiB or crossing it is refused. */
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
 * The read-only build issues no write command to a disk, so a machine booted
 * from it cannot mount its disk root and stops with nothing mounted. The
 * read-write build is taken; the read-only build and an image with no
 * banner are refused.
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

/** Run the groups of rows above. Returns 0 when every check passed and 1
 *  otherwise. */
int main(void)
{
    check_fallback();
    check_bounds();
    check_policy();
    check_kludge();
    check_multiboot2();
    check_chain();
    printf("bootslot-hosttest: result: %s: %d failures\n", fails ? "fail" : "pass", fails);
    return fails != 0;
}
