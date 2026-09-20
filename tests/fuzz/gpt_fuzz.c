/**
 * Fuzz harness for the GPT reader (sys/block/gpt.c): the input is a header
 * sector followed by the entry array. Both checksums are recomputed after
 * the fuzzer's mutation, or every input would die at the header's CRC and
 * the entries would never be read; the checks the fuzzer is for are the
 * ones behind the checksums.
 *
 * fuzz-deps: rach2/sys/block/gpt.c
 */
#include "../../rach2/sys/block/gpt.h"
#include "../fuzz/fuzz.h"

#define DISK_SECTORS 8192u
#define ARRAY_MAX    (GPT_PART_COUNT * 128u * 4u)

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

int LLVMFuzzerTestOneInput(const unsigned char *data, unsigned long size)
{
    uint8_t header[512];
    uint8_t array[ARRAY_MAX];
    struct gpt_part parts[GPT_PART_COUNT];
    uint32_t count, entry_size, bytes;
    int refused = 0;
    unsigned i;

    for (i = 0; i < 512; i++) {
        header[i] = i < size ? data[i] : 0;
    }
    for (i = 0; i < ARRAY_MAX; i++) {
        array[i] = 512 + i < size ? data[512 + i] : 0;
    }
    count = gpt_entry_count(header);
    entry_size = gpt_entry_size(header);
    /* The array's checksum covers what the header says is there; a header
       whose numbers exceed the array on offer is left for the reader to
       refuse, with the checksum of what is there. */
    bytes = (count <= ARRAY_MAX && entry_size <= ARRAY_MAX && count * entry_size <= ARRAY_MAX)
                ? count * entry_size
                : ARRAY_MAX;
    wr32(header + 88, gpt_crc32(array, bytes));
    wr32(header + 16, 0);
    wr32(header + 16, gpt_crc32(header, 92));
    if (gpt_header_ok(header, 1, DISK_SECTORS) != 0) {
        return 0;
    }
    if (count * entry_size > ARRAY_MAX) {
        return 0;
    }
    (void)gpt_parse_entries(array, count, entry_size, gpt_first_usable(header),
                            gpt_last_usable(header), parts, &refused);
    return 0;
}
