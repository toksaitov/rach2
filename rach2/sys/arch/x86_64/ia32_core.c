/*
 * ia32_core.c: what the core file of an i386 program on x86-64 has of each
 * thread, its registers in i386's `user_regs_struct` and its floating-point
 * and TLS notes, as Linux's compat core files have them (proc/coredump.c).
 */

#include "../arch.h"
#include "../x86/fpu.h"
#include "percpu.h"
#include "tls.h"
#include "../../lib/kstring.h"
#include "../../proc/coredump.h"
#include "../../proc/process.h"

/* The words of x86-64's `user_regs_struct` that `arch_regs_get` fills,
 * by their index in Linux's layout. */
enum {
    U64_RBP = 4,
    U64_RBX = 5,
    U64_RAX = 10,
    U64_RCX = 11,
    U64_RDX = 12,
    U64_RSI = 13,
    U64_RDI = 14,
    U64_ORIG_RAX = 15,
    U64_RIP = 16,
    U64_CS = 17,
    U64_EFLAGS = 18,
    U64_RSP = 19,
    U64_SS = 20,
    U64_DS = 23,
    U64_ES = 24,
    U64_FS = 25,
    U64_GS = 26
};

/* The order of i386's `user_regs_struct`, IA32_GREGS words. */
static const unsigned char ia32_order[IA32_GREGS] = {
    U64_RBX, U64_RCX, U64_RDX,      U64_RSI, U64_RDI, U64_RBP,    U64_RAX, U64_DS, U64_ES,
    U64_FS,  U64_GS,  U64_ORIG_RAX, U64_RIP, U64_CS,  U64_EFLAGS, U64_RSP, U64_SS};

/* Linux's note of the TLS entries, three `struct user_desc`s. */
#define NT_386_TLS  0x200u
#define TLS_ENTRIES (GDT_TLS_MAX - GDT_TLS_MIN + 1)

void ia32_regs_get(const process_t *t, uint32_t *regs)
{
    unsigned long r[ARCH_GREGS];
    uint32_t i;

    arch_regs_get(t, r);
    for (i = 0; i < IA32_GREGS; i++) {
        regs[i] = (uint32_t)r[ia32_order[i]];
    }
}

uint32_t ia32_core_fp_size(void)
{
    uint32_t xstate = fpu_xstate_size();

    return core_note_bytes(FPU_ENV_SIZE) + core_note_bytes(FX_SIZE) +
           (xstate != 0 ? core_note_bytes(xstate) : 0u) +
           core_note_bytes(TLS_ENTRIES * (uint32_t)sizeof(struct user_desc));
}

/**
 * The notes are those of `arch_core_fp_notes` on i686, NT_PRFPREG, the
 * `user_i387_struct`, NT_PRXFPREG, the `fxsave` image, and with XSAVE
 * NT_X86_XSTATE, then NT_386_TLS, the three TLS entries as get_thread_area
 * gives them, when one is set, as Linux's `regset_tls_active` has it.
 */
void ia32_core_fp_notes(uint8_t **p, const process_t *t)
{
    uint8_t buf[FX_SIZE];
    struct user_desc tls[TLS_ENTRIES];
    uint32_t xstate = fpu_xstate_size();
    uint8_t *desc;

    fpu_env_get(t->fpu, buf);
    core_note_put(p, "CORE", NT_PRFPREG, buf, FPU_ENV_SIZE);
    fpu_fx_get(t->fpu, buf);
    core_note_put(p, "LINUX", NT_PRXFPREG, buf, FX_SIZE);
    if (xstate != 0) {
        desc = *p + core_note_bytes(0);
        core_note_put(p, "LINUX", NT_X86_XSTATE, t->fpu, xstate);
        fpu_xstate_view(desc);
    }
    if (ia32_tls_get(t, tls) != 0) {
        core_note_put(p, "LINUX", NT_386_TLS, tls, sizeof(tls));
    }
}
