#include "uimage.h"
#include "auxv.h"
#include "../lib/kstring.h"

/**
 * Lay out a fresh user stack image in `page` per the System V i386 ABI.
 *
 * The strings (argv then envp) and the 16 AT_RANDOM bytes are copied near the
 * top of the page; the info block (argc, the argv and envp pointer arrays each
 * NUL-terminated, then the auxiliary vector) is written just below them with
 * ESP aligned down to a 16-byte boundary. `page` maps at `base_vaddr`, so a
 * stored pointer is the virtual address `base_vaddr + (mem offset)` while the
 * copy itself targets `mem + (vaddr - base_vaddr)`. Each piece is checked
 * against `cur - base_vaddr`, the room still free below `cur`, which cannot
 * underflow.
 *
 * @param page mapped backing store for the topmost stack page.
 * @param base_vaddr virtual address at which `page` is mapped in the new image.
 * @param random16 16 bytes published to the program through AT_RANDOM.
 * @param secure published as AT_SECURE: nonzero when the new image's
 *        effective id differs from its real id.
 * @return the initial ESP, or 0 if the arguments are rejected or do not fit.
 */
uint32_t build_user_stack(void *page, uint32_t base_vaddr, uint32_t page_size, int argc,
                          char *const argv[], char *const envp[], const elf_image_t *img,
                          const uint8_t random16[16], int secure)
{
    uint8_t *mem = (uint8_t *)page;
    uint32_t top = base_vaddr + page_size;
    uint32_t cur = top;
    uint32_t argv_v[UIMAGE_MAX_ARGS];
    uint32_t envp_v[UIMAGE_MAX_ARGS];
    Elf32_auxv_t aux[10];
    uint32_t random_v;
    uint32_t info_words;
    uint32_t esp;
    uint32_t *w;
    int envc = 0;
    int naux = 0;
    int i;

    if (!page || !img || !random16) {
        return 0;
    }
    if (argc < 0 || argc > UIMAGE_MAX_ARGS) {
        return 0;
    }

    if (envp) {
        while (envc < UIMAGE_MAX_ARGS && envp[envc]) {
            envc++;
        }
        if (envp[envc]) {
            return 0; /* over UIMAGE_MAX_ARGS env entries: reject, do not truncate */
        }
    }

    for (i = 0; i < argc; i++) {
        uint32_t len = (uint32_t)k_strlen(argv[i]) + 1;

        if (len > cur - base_vaddr) {
            return 0;
        }
        cur -= len;
        k_memcpy(mem + (cur - base_vaddr), argv[i], len);
        argv_v[i] = cur;
    }
    for (i = 0; i < envc; i++) {
        uint32_t len = (uint32_t)k_strlen(envp[i]) + 1;

        if (len > cur - base_vaddr) {
            return 0;
        }
        cur -= len;
        k_memcpy(mem + (cur - base_vaddr), envp[i], len);
        envp_v[i] = cur;
    }

    /* The 16 AT_RANDOM bytes. */
    if (16u > cur - base_vaddr) {
        return 0;
    }
    cur -= 16;
    k_memcpy(mem + (cur - base_vaddr), random16, 16);
    random_v = cur;

    aux[naux].a_type = AT_PHDR;
    aux[naux].a_val = img->phdr_vaddr;
    naux++;
    aux[naux].a_type = AT_PHENT;
    aux[naux].a_val = img->phent;
    naux++;
    aux[naux].a_type = AT_PHNUM;
    aux[naux].a_val = img->phnum;
    naux++;
    aux[naux].a_type = AT_ENTRY;
    aux[naux].a_val = img->entry;
    naux++;
    aux[naux].a_type = AT_PAGESZ;
    aux[naux].a_val = page_size;
    naux++;
    aux[naux].a_type = AT_CLKTCK;
    aux[naux].a_val = 100;
    naux++;
    aux[naux].a_type = AT_SECURE;
    aux[naux].a_val = secure ? 1u : 0u;
    naux++;
    aux[naux].a_type = AT_RANDOM;
    aux[naux].a_val = random_v;
    naux++;
    aux[naux].a_type = AT_NULL;
    aux[naux].a_val = 0;
    naux++;

    /* Info-block size in words: argc + argv[argc] + NULL + envp[envc] + NULL
     * + 2 words per auxv entry. */
    info_words = 1 + (uint32_t)argc + 1 + (uint32_t)envc + 1 + 2 * (uint32_t)naux;

    /* The info block sits below the strings; ESP is then aligned down to 16. */
    if (info_words * 4 > cur - base_vaddr) {
        return 0;
    }
    esp = (cur - info_words * 4) & ~0xFu;
    if (esp < base_vaddr) {
        return 0;
    }

    w = (uint32_t *)page + (esp - base_vaddr) / sizeof(uint32_t);
    *w++ = (uint32_t)argc;
    for (i = 0; i < argc; i++) {
        *w++ = argv_v[i];
    }
    *w++ = 0;
    for (i = 0; i < envc; i++) {
        *w++ = envp_v[i];
    }
    *w++ = 0;
    for (i = 0; i < naux; i++) {
        *w++ = aux[i].a_type;
        *w++ = aux[i].a_val;
    }

    return esp;
}
