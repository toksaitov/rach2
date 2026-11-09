/*
 * vdsoimg.h: what the x86-64's vDSO images (vdso.c, vdso32.c) and its clock
 * (vclock.c) give each other.
 */
#ifndef _VDSOIMG_H_
#define _VDSOIMG_H_

#include "../../lib/kint.h"

/* Where in the image the code of vdsocode.s goes, which vdsocode.s's VDSO_BACK
 * counts back from: the headers and tables are before it. */
#define VDSO_TEXT_OFFSET 0x800u

/** Build the image in a page of its own, once, and the 32-bit image after
 * it: 0, or -1 with no frame or with code that does not fit. */
int vdso_build(void);

/** Build the 32-bit image an i386 program is given in a page of its own,
 * once, under the same rules (vdso32.c). */
int vdso32_build(void);

/** The pages of the 32-bit image, 0 before it is built, and its page's
 * physical frame (vdso32.c). */
uint32_t vdso32_image_pages(void);
uintptr_t vdso32_image_frame(void);

/** The System V ABI's hash of a symbol's or a version's name, which an
 * image's hash table and a version's definition hold (vdso.c). */
uint32_t vdso_elf_hash(const char *name);

/** The data page's physical frame, from vclock.c. */
uintptr_t vdso_data_frame(void);

#endif /* _VDSOIMG_H_ */
