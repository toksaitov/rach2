#ifndef _UIMAGE_H_
#define _UIMAGE_H_

#include "../lib/kint.h"
#include "elf.h"

/* Maximum argv or envp entries the builder accepts. */
#define UIMAGE_MAX_ARGS 64

/*
 * Build the SysV/i386 initial process stack in a single zeroed user stack page
 * and return the entry ESP (16-byte aligned, pointing at argc), or 0 if the
 * arguments do not fit in the page.
 *
 * `page` is the writable mapping of the stack page -- its physical address in
 * the kernel, a plain buffer in host tests; the process sees that page based at
 * `base_vaddr`, `page_size` bytes long. argv and envp are NULL-terminated string
 * arrays; `img` supplies AT_PHDR/PHENT/PHNUM/ENTRY; `random16` is the 16 bytes
 * AT_RANDOM points at.
 *
 * Layout, high to low: the arg/env strings and the 16 random bytes sit near the
 * top; then, starting at the aligned ESP, argc, the argv pointers + NULL, the
 * envp pointers + NULL, and the auxv {type,value} pairs terminated by AT_NULL.
 *
 * Real-OS analogue: the Linux i386 ELF initial stack the C runtime expects.
 *
 * Pure (no kernel state): the same source compiles freestanding and hosted, so
 * the layout is unit-tested natively under the sanitizers (the dual-target seam).
 */
uint32_t build_user_stack(void *page, uint32_t base_vaddr, uint32_t page_size, int argc,
                          char *const argv[], char *const envp[], const elf_image_t *img,
                          const uint8_t random16[16]);

#endif /* _UIMAGE_H_ */
