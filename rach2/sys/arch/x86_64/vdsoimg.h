/*
 * vdsoimg.h: what the x86-64's vDSO image (vdso.c) and its clock
 * (vclock.c) give each other.
 */
#ifndef _VDSOIMG_H_
#define _VDSOIMG_H_

#include "../../lib/kint.h"

/* Where in the image the code of vdsocode.s goes, which vdsocode.s's VDSO_BACK
 * counts back from: the headers and tables are before it. */
#define VDSO_TEXT_OFFSET 0x800u

/** Build the image in a page of its own, once: 0, or -1 with no frame or
 * with code that does not fit. */
int vdso_build(void);

/** The data page's physical frame, from vclock.c. */
uintptr_t vdso_data_frame(void);

#endif /* _VDSOIMG_H_ */
