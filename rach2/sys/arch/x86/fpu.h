#ifndef _X86_FPU_H_
#define _X86_FPU_H_

#include "../../fpu.h"

#include "../../lib/kint.h"

/*
 * The x86's floating-point state, behind ../../fpu.h: the x87 and SSE
 * registers.
 *
 * Two formats. When `cpuid` reports FXSR and `rach2.nofxsr` is not on the
 * boot line, `fxsave` and `fxrstor` keep the x87 and the XMM registers with
 * MXCSR in 512 bytes at a 16-byte alignment, and CR4.OSFXSR is set, when
 * `cpuid` reports SSE as well, so the SSE instructions run. Otherwise
 * `fnsave` and `frstor` keep the x87 state in the first 108 bytes,
 * CR4.OSFXSR is cleared, and an SSE instruction is an invalid opcode: a
 * register file that is not saved is not offered. CR0 gets MP and NE and
 * loses EM and TS, so the unit is always present and never traps on first
 * use; the lazy switch through TS is not used here. The initial state has
 * control word 0x37F and MXCSR 0x1F80.
 *
 * On x86-64 the first format is the only one, since every x86-64 processor
 * has FXSR and SSE and its programs compute in SSE, and the save and the
 * load are `fxsave64` and `fxrstor64`, which keep the x87 unit's
 * instruction and operand pointers at 64 bits.
 */

/** The bytes of the floating-point state in a Linux signal frame. On i686,
 * `struct _fpstate_32`: the x87 environment and registers in the 108-byte
 * `fnsave` layout, the status word, a magic word, and then the 512-byte
 * `fxsave` image. On x86-64, `struct _fpstate_64`: the `fxsave64` image
 * alone. */
#if defined(__x86_64__)
#define FPU_LINUX_SIZE 512
#else
#define FPU_LINUX_SIZE 624
#endif

/**
 * Write the state in `area` as Linux's frame has it into `out`.
 *
 * On x86-64 it is the image, with the 48 bytes at 464 that the processor
 * leaves to software zero, so that no XSAVE area is announced. On i686,
 * in both formats bytes 108 and 109 are the x87 status word. In the FXSR
 * format the first 108 bytes are the x87 part converted from the `fxsave`
 * image, as Linux's `convert_from_fxsr` makes it, the magic word is 0, and
 * the image follows at byte 112. In the x87 format the first 108 bytes are
 * the `fnsave` image, the magic word is 0xFFFF, and bytes 112 to 623 are
 * zero.
 */
void fpu_to_linux(const void *area, uint8_t *out);

/**
 * Build a save area from the state in a signal frame, `in`, as a signal
 * return does. On i686 the SSE state comes from the `fxsave` image and the
 * x87 state from the 108-byte part, which a handler may have changed; on
 * x86-64 both come from the image. Returns 0, or -1 for an image `fxrstor`
 * would fault on.
 */
int fpu_from_linux(const uint8_t *in, void *area);

#endif /* _X86_FPU_H_ */
