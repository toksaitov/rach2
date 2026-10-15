/*
 * modlayout: the judgement on where the bootloader put the boot modules.
 *
 * Multiboot gives a module's address and length and says nothing of where
 * it lies, so the kernel checks the layout before it reserves memory over
 * it. The rules
 * are the four ways a placement breaks this kernel: a module past the
 * memory the kernel can address (its end would wrap), a module
 * overlapping the kernel image (the bump allocator would carve over its own
 * code), two modules overlapping each other (one image would be torn), and an
 * empty or backwards range (a bootloader bug that would make every later
 * length calculation wrong). Where a module lies is otherwise the loader's
 * choice: the boot maps it into the kernel's layout wherever it is.
 *
 * The check calls nothing of the kernel, so tests/host/modlayout_hosttest.c
 * can give it every refusal, the ones no bootloader makes among them.
 */
#ifndef _MODLAYOUT_H_
#define _MODLAYOUT_H_

#include "../lib/kint.h"

/** One module's half-open physical range, as Multiboot reports it. */
struct modlayout_range {
    uintptr_t start;
    uintptr_t end;
};

/* What a check found. `modlayout_reason` turns each into its diagnostic. */
#define MODLAYOUT_OK          0
#define MODLAYOUT_EMPTY       1
#define MODLAYOUT_PAST_LIMIT  2
#define MODLAYOUT_HITS_KERNEL 3
#define MODLAYOUT_OVERLAP     4

/**
 * Judge `count` module ranges against the kernel image and the memory ceiling.
 *
 * `kernel_end` is the first byte past the kernel image, `limit` the first byte
 * past addressable memory. On a refusal `*bad` receives the index of the
 * offending module (the second of the pair for an overlap), so the caller can
 * name it, and 0 otherwise; `bad` may be NULL. Ranges are half-open and
 * compared end to end, with no length added, so a range near the top of the
 * address space cannot wrap past a check. Each module is checked for the
 * first three rules in turn, and the pairs for overlap after.
 *
 * @return `MODLAYOUT_OK`, or the first rule broken.
 */
int modlayout_check(const struct modlayout_range *mods, uint32_t count, uint32_t kernel_end,
                    uint32_t limit, uint32_t *bad);

/** The one-line diagnostic for a `modlayout_check` result. */
const char *modlayout_reason(int code);

/**
 * Write the full refusal into `buf`: the rule, the module's index, and its
 * range, as `reason: module N spans 0xSTART - 0xEND`.
 *
 * `k_panic` takes one string and draws its picture over the screen, and the
 * Lenovo has no serial port, so the detail goes into that string rather than
 * into a line printed before it. The formatting calls nothing of the kernel
 * either, so the host test can check the exact text. The result is
 * truncated to fit and ends in a NUL; a `buflen` of 0 writes nothing.
 */
void modlayout_describe(int code, uint32_t index, uintptr_t start, uintptr_t end, char *buf,
                        uint32_t buflen);

#endif /* _MODLAYOUT_H_ */
