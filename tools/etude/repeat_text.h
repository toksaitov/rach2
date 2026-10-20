/*
 * repeat_text.h: what the repetition record and the driver share of
 * writing JSON.
 */
#ifndef ETUDE_REPEAT_TEXT_H
#define ETUDE_REPEAT_TEXT_H

#include "compare.h"

/** Append `text` to `t` as a JSON string, escaped. */
void rep_string(struct cmp_text *t, const char *text);

#endif
