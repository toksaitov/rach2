/* Read the boot token published by etude-guest on Rach2's boot-local tmpfs. */
#ifndef ETUDE_BOOT_IDENTITY_H
#define ETUDE_BOOT_IDENTITY_H
#include <stddef.h>

/* Rach2 preserves Linux's uname sysname for binary compatibility. */
int etude_boot_is_rach2(const char *system, const char *release, const char *version);
/* Read exactly 64 hexadecimal bytes from a trusted regular file, or fail. */
int etude_boot_file_read(const char *path, char *out, size_t size);
#endif
