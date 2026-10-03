#ifndef _KSTD_H_
#define _KSTD_H_

#include "kdef.h"
#include "kstring.h"

/** Convert `input` to base-`radix` text in `buffer`; returns the length
 * needed. */
size_t k_itoa(int input, char *buffer, size_t buffer_size, int radix, bool force_sign);

/** As `k_itoa`, for a long. */
size_t k_ltoa(long input, char *buffer, size_t buffer_size, int radix, bool force_sign);

#endif
