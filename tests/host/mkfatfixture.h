/* The independent FAT32 checker over an immutable complete volume. */
#ifndef MKFATFIXTURE_H
#define MKFATFIXTURE_H

/* Check DATA without writing it, with fresh state on each call. OUT is a
 * separate buffer of CAP bytes, including its terminating NUL. Return 0 for
 * no findings, 1 for findings, or 2 when checking cannot start or output does
 * not fit. LIST also prints each directory entry. */
int mkfatfixture_check(const unsigned char *data, unsigned long bytes, int list, char *out,
                       unsigned long cap);

#endif
