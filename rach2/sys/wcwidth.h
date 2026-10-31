/*
 * wcwidth.h: the cells a character takes on a terminal, by the table the
 * console and term share.
 */
#ifndef _WCWIDTH_H_
#define _WCWIDTH_H_

#include "lib/kint.h"

/**
 * The columns Unicode code point `ucs` takes on a terminal: 2 for a wide or
 * full-width character of East Asian Width (CJK, most emoji), 0 for a
 * combining mark, a variation selector or a format character of no width,
 * and NUL; -1 for a C0 or C1 control character; 1 for any other. The table
 * is libtsm 4.8.0's, so that the console and `term` agree. No state; any
 * context.
 */
int k_wcwidth(uint32_t ucs);

#endif
