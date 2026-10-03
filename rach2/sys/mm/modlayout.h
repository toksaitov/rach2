#ifndef _MODLAYOUT_H_
#define _MODLAYOUT_H_

#include "../lib/kint.h"

/*
 * modlayout: the judgement on where the bootloader put the boot modules.
 *
 * GRUB promises a module's address and length and nothing about placement, so
 * the kernel checks the layout before it reserves memory over it. The rules
 * are the four ways a placement breaks this kernel: a module past the
 * memory the kernel can address (its end would wrap), a module
 * overlapping the kernel image (the bump allocator would carve over its own
 * code), two modules overlapping each other (one image would be torn), and an
 * empty or backwards range (a bootloader bug that would make every later
 * length calculation wrong). Where a module lies is otherwise the loader's
 * choice: the boot maps it into the kernel's layout wherever it is.
 *
 * The function is pure so tests/host/modlayout_hosttest.c can craft every
 * refusal, including the ones no bootloader will produce on demand.
 */

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
 * name it. Ranges are half-open and compared by subtraction, so a range near
 * the 32-bit edge cannot wrap past a check.
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
 * The kernel's panic screen takes a string and the target laptop has no serial
 * port, so the detail has to travel INTO the panic, not be printed
 * before it: `k_panic` clears the display. The formatting is self-contained
 * for the same reason the check is, so the host test can assert the exact
 * text. The result is always NUL-terminated and truncated to fit.
 */
void modlayout_describe(int code, uint32_t index, uintptr_t start, uintptr_t end, char *buf,
                        uint32_t buflen);

#endif /* _MODLAYOUT_H_ */
