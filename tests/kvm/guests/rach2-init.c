/* The i686 KVM guest's first shell: report userspace entry and power off. */
#include "../../probe.h"

void probe_main(unsigned long *sp);

PROBE_SP_ENTRY;

/** Report that the kernel started its user process, then power off. */
void probe_main(unsigned long *sp)
{
    static const char token[] = "KVMRACH2 OK\n";

    (void)sp;
    syscall3(SYS_WRITE, 1, (long)token, sizeof(token) - 1);
    syscall3(SYS_REBOOT, (long)0xFEE1DEADul, (long)0x28121969ul, (long)0x4321FEDCul);
    syscall1(SYS_EXIT, 1);
    __builtin_unreachable();
}
