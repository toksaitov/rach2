#ifndef _UIMAGE_H_
#define _UIMAGE_H_

#include "../lib/kint.h"
#include "elf.h"

/* Maximum argv or envp entries the builder accepts. */
#define UIMAGE_MAX_ARGS 64

/** Build the SysV/i386 initial process stack; returns the entry ESP, or 0. */
uint32_t build_user_stack(void *page, uint32_t base_vaddr, uint32_t page_size, int argc,
                          char *const argv[], char *const envp[], const elf_image_t *img,
                          const uint8_t random16[16]);

#endif /* _UIMAGE_H_ */
