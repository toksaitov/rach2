/*
 * ram_core.h: a persistent RAM zone, a region of memory that keeps its
 * bytes across a reset, laid out as Linux 6.12's fs/pstore/ram_core.c lays
 * one out, with its optional Reed-Solomon parity.
 */
#ifndef _PSTORE_RAM_CORE_H_
#define _PSTORE_RAM_CORE_H_

#include "../lib/kdef.h"
#include "../lib/kint.h"

/* The signature of a zone's header, "DBGC", which ram_core.c:40 XORs with
 * the zone's own signature: 0 for the dmesg and console zones. */
#define PRZ_SIG 0x43474244u

/* The bytes of the header before the data: the signature, the offset of
 * the next byte to write and the bytes held, each 32 bits, as Linux's
 * struct persistent_ram_buffer (ram_core.c:33). */
#define PRZ_HEADER 12u

/* The bytes of data one parity block covers, ram_core.c:199. */
#define PRZ_ECC_BLOCK 128u

/* The most parity bytes a block may have: a code of 255 symbols, less a
 * whole block of data. With more, a block's word is longer than the code,
 * two of its positions share a root and the decoder cannot tell them apart;
 * Linux's encode_rs8 refuses such a length with -ERANGE and decode_rs8
 * stops at a BUG_ON (lib/reed_solomon/encode_rs.c:24, decode_rs.c:43). */
#define PRZ_ECC_MAX (255u - PRZ_ECC_BLOCK)

/*
 * A zone. `mem` is its mapping, `size` bytes: the header, then
 * `buffer_size` bytes of data in which the log wraps, then, with ECC, the
 * parity of each block of the data and of the header. The header's fields
 * are read and written through `mem` alone, so that what is there is what
 * the next boot finds.
 */
struct prz {
    volatile uint8_t *mem;
    uint32_t size;        /* bytes of the zone */
    uint32_t buffer_size; /* bytes of data */
    uint32_t sig;         /* the header's signature, PRZ_SIG XOR the zone's */
    uint32_t ecc_size;    /* parity bytes of a block, 0 for no ECC */
    uint32_t ecc_blocks;  /* blocks of data with parity */
    int corrected_bytes;  /* symbols the parity mended, from the boot's read */
    int bad_blocks;       /* blocks and headers the parity could not mend */
    char *old_log;        /* the data found at boot, oldest byte first, kmalloc'd */
    uint32_t old_size;
};

/**
 * Take the zone of `size` bytes at `mem` for the signature `sig`, as
 * Linux's persistent_ram_new and persistent_ram_post_init (ram_core.c:528)
 * do: with `ecc_size` parity bytes, set aside the parity at the end of the
 * data and mend the header by it; then, when the header holds the signature
 * and a size and start within the data, copy what it holds to `old_log`,
 * mending each block; and otherwise write the signature and empty it. With
 * `zap_old` the zone is emptied in any case after the copy, as a zone of
 * one boot's lifetime is (PRZ_FLAG_ZAP_OLD). Returns 0, -EINVAL for a zone
 * too small for its header and parity, or -ENOMEM when the copy has no
 * memory, which leaves the zone emptied. Allocates; never from a panic.
 */
int prz_init(struct prz *z, volatile void *mem, uint32_t size, uint32_t sig, uint32_t ecc_size,
             int zap_old);

/** Empty the zone: start and size 0, as persistent_ram_zap. */
void prz_zap(struct prz *z);

/**
 * Append the `n` bytes at `s` to the zone's data, wrapping at its end, as
 * persistent_ram_write: of more than the data holds, the last bytes. The
 * parity of each block written and of the header follows. Allocates
 * nothing and takes no lock: from any context, with the caller keeping two
 * writers of one zone apart.
 */
void prz_write(struct prz *z, const void *s, uint32_t n);

/** Free the copy `prz_init` made, as persistent_ram_free_old. */
void prz_free_old(struct prz *z);

/**
 * Write the notice Linux appends to a record read from a zone with ECC,
 * persistent_ram_ecc_string (ram_core.c:256), into `buf` of `cap` bytes
 * with a NUL, cut to fit; return its length, uncut, or 0 without ECC.
 */
uint32_t prz_ecc_string(const struct prz *z, char *buf, uint32_t cap);

/**
 * Mend the `len` bytes at `data`, followed by the `nroots` parity bytes at
 * `par`, by the Reed-Solomon code of Linux's ramoops: symbols of 8 bits,
 * the field polynomial 0x11d, the first root 1 and the generator 1
 * (ram_core.c:224). Returns the symbols corrected, data and parity both, or
 * -EBADMSG when there are more errors than the code mends, leaving the
 * bytes as they were. `len` + `nroots` is at most 255.
 */
int prz_rs_decode(uint8_t *data, uint32_t len, uint8_t *par, uint32_t nroots);

/** Write into `par` the `nroots` parity bytes of the `len` bytes at
 * `data`, as Linux's encode_rs8 with no inversion mask. */
void prz_rs_encode(const uint8_t *data, uint32_t len, uint8_t *par, uint32_t nroots);

#endif
