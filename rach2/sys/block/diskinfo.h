#ifndef _DISKINFO_H_
#define _DISKINFO_H_

#include "../lib/kint.h"
#include "mbr.h"

/*
 * diskinfo: what a disk says about itself, read once and published as text.
 *
 * Milestone 24 must decide how to carve a laptop's SSD without writing a
 * sector of it, and that decision needs facts: the drive's identity, how many
 * sectors it has, what its partition table says, and hashes of chosen sectors
 * to compare against the ones Windows reported. Mounting would answer some of
 * that and is forbidden, since the mount stamps a dirty flag and that is a
 * write.
 *
 * So the answer is split in two. The identity comes from an ATA IDENTIFY
 * response, which no sector holds; both drivers already ask for it and both
 * threw the answer away, so it is kept here and published as `/proc/disk`. The
 * sectors come through `/dev/disk0`, a root-only read-only window over the
 * whole disk, and are hashed in userland by the busybox the image already
 * carries, because a SHA-256 in the kernel would duplicate `sha256sum` to no
 * purpose.
 *
 * Nothing here writes. The only commands this path issues are IDENTIFY and a
 * sector read, both of which the read-only build's allowlist permits, so the
 * metal kernel runs it unchanged.
 *
 * The formatting helpers are pure so the host test can reach the cases a real
 * device will not produce on request: a model field with no terminator, an
 * all-ones serial, a sector count above 2^32, a buffer one byte too short.
 */

/* IDENTIFY string fields, in characters, as ATA lays them out. */
#define DISKINFO_MODEL_CHARS    40 /* words 27-46 */
#define DISKINFO_SERIAL_CHARS   20 /* words 10-19 */
#define DISKINFO_FIRMWARE_CHARS 8  /* words 23-26 */

/* Whether a disk was looked for, and what was found. `not-asked` and
   `no-device` are kept apart so a script can tell a kernel that was never
   asked from one that asked and got no answer. */
#define DISKINFO_NOT_ASKED 0
#define DISKINFO_NO_DEVICE 1
#define DISKINFO_PRESENT   2

/* What the first sector turned out to be. */
#define DISKINFO_MBR_UNREAD 0 /* the read failed */
#define DISKINFO_MBR_NONE   1 /* read, but no 0x55AA signature */
#define DISKINFO_MBR_OK     2

/*
 * The last sector `/dev/disk0` can reach.
 *
 * This was 8388607 until milestone 25, because an open file's offset was 32
 * bits and the byte just past that sector is the largest one such an offset can
 * name. The raw disk windows now seek and transfer through a 64-bit offset, so
 * the window covers whatever the drive reports. A recovery image has to be able
 * to rewrite any region of a 250 GB drive, and a window that stopped at 4 GiB
 * could not reach most of one.
 *
 * The bound that remains is this window's own, chosen rather than forced: 2^32
 * sectors, which is 2 TiB, comfortably past any disk this project is aimed at
 * and past what the ATA driver's 28-bit LBA could reach anyway. AHCI programs a
 * 48-bit LBA and could go further; the ceiling is here so the window has one,
 * not because a driver imposes it. A larger disk is reported at its true size
 * beside the window's, so the two disagree visibly rather than one quietly
 * lying. This is a COUNT, not the largest LBA: a count of 0xFFFFFFFF would
 * leave the last addressable sector unreachable.
 */
#define DISKINFO_WINDOW_MAX_SECTORS 0x100000000ull

/** What one disk said about itself, filled once and then only read. */
struct diskinfo {
    int state;          /* DISKINFO_NOT_ASKED / NO_DEVICE / PRESENT */
    int mounted;        /* the root file system is on this disk */
    const char *driver; /* "ahci", "ata", or "none" */
    int port;           /* the AHCI port, or -1 for ATA */
    uint64_t sectors;
    uint32_t sector_size;
    uint64_t window_sectors;
    char model[DISKINFO_MODEL_CHARS + 1];
    char serial[DISKINFO_SERIAL_CHARS + 1];
    char firmware[DISKINFO_FIRMWARE_CHARS + 1];
    int mbr_state; /* DISKINFO_MBR_* */
    uint32_t mbr_id;
    struct mbr_part parts[MBR_PART_COUNT];
};

/**
 * Copy an IDENTIFY string field out of `id`, starting at `first_word`.
 *
 * ATA stores these big-endian within each 16-bit word and pads with spaces
 * without a terminator, so this un-swaps, trims the trailing spaces and
 * terminates. Any byte outside printable ASCII becomes '?', because the result
 * goes into a text file that `grep` and `awk` parse on the machine, and a
 * device that answered with a newline or a NUL would otherwise split or
 * truncate a line. `id` is `volatile` so a caller may pass a DMA buffer.
 */
void diskinfo_ata_string(char *out, int chars, const volatile uint16_t *id, int first_word);

/**
 * Write `v` as decimal into `out`, which holds `cap` bytes including the NUL.
 *
 * Written by hand because the kernel links no libgcc: dividing a `uint64_t` by
 * ten would emit a call to `__udivdi3` that does not exist. The sector count
 * is a 48-bit field, so the full range matters even though today's disks fit
 * in 32 bits.
 *
 * @return the number of characters written, or 0 if they would not fit.
 */
int diskinfo_u64_dec(char *out, int cap, uint64_t v);

/** How many sectors of a `sectors`-sector disk `/dev/disk0` can reach. */
uint64_t diskinfo_window(uint64_t sectors);

/**
 * Render `d` as the text of `/proc/disk` into `buf`, which holds `cap` bytes.
 *
 * One fact per line, a single-token key then values. Truncates rather than
 * overruns, and always terminates.
 *
 * @return the number of characters written, not counting the NUL.
 */
int diskinfo_format(const struct diskinfo *d, char *buf, int cap);

/** Keep what an IDENTIFY reported. Called by both drivers on success. */
void diskinfo_record_identify(const char *driver, int port, const volatile uint16_t *id,
                              uint64_t sectors);

/**
 * Find a disk, read its first sector once, and publish `/dev/disk0`.
 *
 * Adopts an already-published `block0` rather than probing again, so a machine
 * whose root is on the disk does not get its controller reset underneath a
 * mounted file system. `mounted` says whether that is the case, for the
 * report.
 *
 * @return 0 when a disk answered, -1 otherwise.
 */
int diskinfo_probe(int mounted);

/** The record, whatever state it is in. Never NULL. */
const struct diskinfo *diskinfo_get(void);

#endif /* _DISKINFO_H_ */
