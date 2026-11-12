/*
 * exits.h: counts, on each processor, of the instructions a hypervisor may
 * take as a VM exit: `rdmsr` and `wrmsr` by register, `cpuid` by leaf,
 * port reads and writes by port, and `hlt`. Under KVM `cpuid`, `hlt` and
 * the port accesses always exit, and an MSR access exits unless KVM hands
 * the register to the guest, as it hands the FS and GS bases, or the
 * processor's APIC virtualization takes it. With `rach2.exits` on the
 * command line, debugfs's `exits` prints them beside the interprocessor
 * interrupts each processor sent, by kind, so that a load's exits are named
 * from the guest's side, where KVM's own counters leave every MSR access,
 * `cpuid` and pause unnamed. Without it nothing is counted, and a count
 * costs one test.
 */

#ifndef _X86_EXITS_H_
#define _X86_EXITS_H_

#include "../../lib/kint.h"

/* The kinds of instruction counted; 0 is a free slot. */
#define EXIT_RDMSR 1u
#define EXIT_WRMSR 2u
#define EXIT_CPUID 3u
#define EXIT_IN    4u
#define EXIT_OUT   5u
#define EXIT_HLT   6u
#define EXIT_KINDS 7u

/** Count from now on when the command line has `rach2.exits`, and return
 * whether it does: called once every processor reaches its own record
 * through `arch_cpu_id`, after the processors start. Before, nothing is
 * counted, since a processor coming up writes its MSRs before it has one. */
int exits_start(void);

/** Count one instruction of `kind` on `number`, the register, the leaf or
 * the port, or 0 for `hlt`, on the running processor. Any context, NMIs
 * included. */
void exits_count(unsigned int kind, uint32_t number);

/** debugfs's `exits` (debugfs.h's `show`): a line `KIND NUMBER TOTAL CPU0
 * CPU1 ...` for each kind and number any processor counted, numbers in
 * hexadecimal, then `ipi KIND ...` for each kind of interprocessor
 * interrupt (proc/smp.h's `smp_ipi_count`) and `lost ...`, the counts that
 * found a processor's table full. */
int32_t exits_show(void *data, char *buf, uint32_t cap);

#endif /* _X86_EXITS_H_ */
