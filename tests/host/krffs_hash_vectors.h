#ifndef KRFFS_HASH_VECTORS_H_
#define KRFFS_HASH_VECTORS_H_

/**
 * Common KRFFS djb2 and parent-id vectors, modulo 2^64.
 *
 * A test defines `HASH_VECTOR(parent, name, djb, mixed)` as the check it
 * wants, writes `KRFFS_HASH_VECTORS`, and undefines the macro again. The list
 * expands where it is written, so one translation unit may use it more than
 * once.
 */
#define KRFFS_HASH_VECTORS                                                                        \
    HASH_VECTOR(0x0000000000000000ULL, "", 0x0000000000001505ULL, 0x0016690000233142ULL)          \
    HASH_VECTOR(0x0000000000000000ULL, "a", 0x000000000002b5c4ULL, 0x02b62a000498a635ULL)         \
    HASH_VECTOR(0x0000000000000000ULL, "hello", 0x000000310a9cede7ULL, 0x9cee5a5508a592bcULL)     \
    HASH_VECTOR(0x0000000000000001ULL, "hello", 0x000000310a9cede7ULL, 0x9cf23a5508a94533ULL)     \
    HASH_VECTOR(0xffffffffffffffffULL, "root", 0x000000017c7902e3ULL, 0x7902e586819fe7b9ULL)      \
    HASH_VECTOR(0x123456789abcdef0ULL, "\200\377A", 0x000000000b84803bULL, 0x59273aefd3ac41a8ULL) \
    HASH_VECTOR(0x0000000000000025ULL,                                                            \
                "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"  \
                "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz"  \
                "abcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyzabcdefghijklmnopqrstuvwxyz", \
                0xcde5ee45dd8ebfbeULL, 0x6c727ab779fb97c4ULL)                                     \
    HASH_VECTOR(0x5d86c325aa140266ULL, "", 0x0000000000001505ULL, 0x0000000000000001ULL)

#endif
