#ifndef _X86_FPU_H_
#define _X86_FPU_H_

#include "../../fpu.h"

#include "../../lib/kint.h"

/*
 * The x86's floating-point state, behind ../../fpu.h: the x87, SSE, AVX
 * and AVX-512 registers.
 *
 * Three formats, chosen at boot. With XSAVE, which `cpuid` reports, the
 * kernel sets CR4.OSXSAVE and XCR0 to the components it saves: x87 and SSE,
 * AVX where the processor has it, and AVX-512's opmask, upper ZMM and high
 * ZMM components together where it has all three and AVX512F. A program
 * then sees OSXSAVE in `cpuid` and may use the AVX instructions. The area
 * is the standard-format XSAVE image, as large as leaf 0xD of `cpuid` says
 * those components need, and `xsave` and `xrstor` move it; on x86-64 they
 * are `xsave64` and `xrstor64`, which keep the x87 unit's pointers at 64
 * bits. `xsave` writes every component the mask names, so the area is a
 * whole image that a signal frame, ptrace and a core file copy as it is;
 * `xsaveopt` and `xsavec` would leave the components in their initial
 * state unwritten, or move them, and every copy would have to undo that.
 * `rach2.noxsave` on the boot line keeps the next format on a processor
 * with XSAVE.
 *
 * Without XSAVE, where `cpuid` reports FXSR, `fxsave` and `fxrstor` keep
 * the x87 and XMM registers with MXCSR in 512 bytes, and CR4.OSFXSR is set
 * when `cpuid` reports SSE as well, so the SSE instructions run. Otherwise,
 * on i686 alone, and there under `rach2.nofxsr` too, `fnsave` and `frstor`
 * keep the x87 state in the first 108 bytes, CR4.OSFXSR is cleared, and an
 * SSE instruction is an invalid opcode: a register file that is not saved
 * is not offered. CR0 gets MP and NE and loses EM and TS, so the unit is
 * always present and never traps on first use; the lazy switch through TS
 * is not used here. The initial state has control word 0x37F, MXCSR 0x1F80
 * and every other register zero.
 *
 * Every save area is 64-byte aligned, as `xsave` needs, and holds a whole
 * image: what `xsave` wrote, the initial state, or what a tracer set, which
 * `fpu_xstate_set` builds from the initial state. The bytes of the legacy
 * part from 416 to 511, which no save writes, stay zero.
 */

/* The layouts Linux gives x86-64, where the kernel is built for it; a host
 * test gets i686's, as arch.h gives it i686's cpu.h, unless it defines
 * this itself. */
#if defined(__x86_64__) && !defined(RACH2_HOSTTEST)
#define FPU_X86_64 1
#endif

/* The most bytes a save area may take. A processor whose components need
 * more keeps the larger ones out of XCR0. */
#define FPU_AREA_MAX 4096u

/* The `fxsave` image, which opens every save area but the `fnsave` one. */
#define FX_SIZE 512u

/* Linux's register sets and core notes of the state (`elf.h`): the i386
 * `user_i387_struct` or x86-64's `fxsave` image, the i386 `fxsave` image,
 * and the XSAVE image. */
#define NT_PRFPREG    2u
#define NT_PRXFPREG   0x46E62B7Fu
#define NT_X86_XSTATE 0x202u

/** The bytes of `struct _fpstate` in a signal frame without XSAVE. On i686,
 * `struct _fpstate_32`: the x87 environment and registers in the 108-byte
 * `fnsave` layout, the status word, a magic word, and the 512-byte `fxsave`
 * image. On x86-64, `struct _fpstate_64`, the `fxsave64` image alone. The
 * plain i686 frame keeps an unused area of this size. */
#if defined(FPU_X86_64)
#define FPU_LINUX_SIZE 512
#else
#define FPU_LINUX_SIZE 624
#endif

/** The bytes of the i386 `user_i387_struct`, the x87 environment and
 * registers in the `fnsave` layout. */
#define FPU_ENV_SIZE 108u

/**
 * Set up the running processor for the format `fpu_init` chose: CR0, CR4
 * and XCR0. `fpu_init` runs it on the processor the kernel boots on; each
 * other processor runs it as it starts.
 */
void fpu_cpu_setup(void);

/** Nonzero when the state is kept by `fxsave` or `xsave`, and not by
 * `fnsave`. */
int fpu_has_fxsr(void);

/** The bytes of the XSAVE image, NT_X86_XSTATE's size, or 0 without
 * XSAVE. */
uint32_t fpu_xstate_size(void);

/** Write the 512-byte `fxsave` image of `area` into `fx`, with the bytes
 * from 416 zero, as Linux's NT_PRFPREG on x86-64 and NT_PRXFPREG on i686
 * give it. Needs `fpu_has_fxsr`. */
void fpu_fx_get(const void *area, uint8_t *fx);

/**
 * Put the `fxsave` image `fx` into `area`: the x87 and SSE state and MXCSR,
 * which with XSAVE are then marked in use. Returns 0, or -1 for an image
 * whose MXCSR sets a reserved bit, which `fxrstor` would fault on. Needs
 * `fpu_has_fxsr`.
 */
int fpu_fx_set(void *area, const uint8_t *fx);

/**
 * Make `copy`, a copy of a save area of `fpu_xstate_size` bytes, the image
 * Linux's NT_X86_XSTATE gives: the area with bytes 416 to 511 zero, but for
 * XCR0 in the first eight of the 48 at 464 that the processor leaves to
 * software, where GDB reads which components the image has.
 */
void fpu_xstate_view(uint8_t *copy);

/**
 * Build `area` from `in`, an XSAVE image of `fpu_xstate_size` bytes, as
 * PTRACE_SETREGSET takes NT_X86_XSTATE: the initial state, with each
 * component the header's XSTATE_BV marks in use copied from `in`, and MXCSR
 * with its mask always. Returns 0, or -1 for an image Linux refuses:
 * XSTATE_BV marks a component XCR0 lacks, the rest of the header is not
 * zero, or MXCSR sets a reserved bit.
 */
int fpu_xstate_set(void *area, const uint8_t *in);

#if !defined(FPU_X86_64)
/** Write the x87 part of `area` into `env` as the 108-byte i386
 * `user_i387_struct`, converted from the `fxsave` image as Linux's
 * `convert_from_fxsr` converts it, or copied from the `fnsave` one. */
void fpu_env_get(const void *area, uint8_t *env);

/** Put the 108-byte `user_i387_struct` `env` into the x87 part of `area`,
 * as Linux's `convert_to_fxsr` does; with XSAVE the x87 state is then
 * marked in use. */
void fpu_env_set(void *area, const uint8_t *env);
#endif

/*
 * The signal frame's floating-point state, Linux's on each architecture.
 *
 * On x86-64 the sigcontext's `fpstate` points at the 512-byte `fxsave64`
 * image or, with XSAVE, at the XSAVE image, 64-byte aligned. On i686 it
 * points at `struct _fpstate_32`: the 108-byte x87 part, the status word,
 * the magic word 0 (0xFFFF for the `fnsave` format), and from byte 112 the
 * `fxsave` image or, with XSAVE, the XSAVE image, which is then the part
 * 64-byte aligned. With XSAVE the 48 bytes at 464 of the image are Linux's
 * `struct _fpx_sw_bytes`: FP_XSTATE_MAGIC1, the bytes from `fpstate` to
 * the end of FP_XSTATE_MAGIC2, XCR0, and the image's size, past which
 * FP_XSTATE_MAGIC2 follows; and XSTATE_BV marks x87 and SSE in use, so that
 * a handler that changes them in the legacy part changes what sigreturn
 * loads. Without XSAVE those 48 bytes are zero, and no extended state is
 * announced.
 */

/** The flag of a `ucontext_t`'s `uc_flags` that announces an XSAVE image
 * at its `fpstate`. */
#define UC_FP_XSTATE 0x1u

/** The `uc_flags` bits the state of `fpu_frame_put` gives an rt frame:
 * UC_FP_XSTATE with XSAVE, as Linux sets it, and none without. */
uint32_t fpu_frame_uc_flags(void);

/** The address of the floating-point state in a frame below the stack
 * pointer `sp`, aligned as Linux aligns it. */
uintptr_t fpu_frame_place(uintptr_t sp);

/** The most bytes `fpu_frame_place` puts between a stack pointer and the
 * state it places, the state's alignment included. */
uintptr_t fpu_frame_bytes(void);

/** Write the state in `area` into a frame, at the user address `fp` that
 * `fpu_frame_place` gave. Returns 0, or -1 when the stack cannot be
 * written. */
int fpu_frame_put(uintptr_t fp, const void *area);

/**
 * Build `area` from the frame's state at the user address `fp`, as a
 * signal return does, for `fpu_restore` to load.
 *
 * With XSAVE, a frame whose software bytes are not FP_XSTATE_MAGIC1 with
 * sizes that fit, or whose FP_XSTATE_MAGIC2 is not where they say, gives
 * the x87 and SSE state of its `fxsave` part and the initial state of the
 * rest, as Linux's `fx_only` restore does; otherwise the image is taken,
 * with the components the software bytes' XCR0 lacks put in their initial
 * state. On i686 the x87 state is then taken from the 108-byte part, which
 * a handler may have changed. Returns 0, or -1 for a frame Linux refuses:
 * one that cannot be read, a header `xrstor` would fault on, or an MXCSR
 * with a reserved bit set.
 */
int fpu_frame_take(uintptr_t fp, void *area);

/** Load the live state from the frame's state at the user address `fp`,
 * through an area from the heap that `fpu_frame_take` builds. Returns 0, or
 * -1 for a frame it refuses or an area the heap cannot give. */
int fpu_frame_load(uintptr_t fp);

#endif /* _X86_FPU_H_ */
