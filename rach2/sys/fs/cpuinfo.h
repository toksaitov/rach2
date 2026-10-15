#ifndef _CPUINFO_H_
#define _CPUINFO_H_

/*
 * cpuinfo: the text of /proc/cpuinfo, in Linux's format for x86, from what
 * `cpuid` reports: the vendor, family, model and stepping, the brand string,
 * the last-level cache, the APIC ID, the feature flags by Linux's names, the
 * cache line and the address sizes, a block for each processor. The
 * processors are taken to be alike, so `cpuid` on the running one answers
 * for each but its APIC ID, and each is a core of one package with no
 * sibling thread. It has no "cpu MHz" or "bogomips" line, since the kernel
 * does not measure the processor's clock.
 */

#include "../lib/kint.h"

/** The largest text `cpuinfo_text` makes. */
#define CPUINFO_TEXT_MAX 2048

/** Write processor `cpu`'s block into `out`, at most `cap` bytes with the
 * NUL; its length. */
uint32_t cpuinfo_text(char *out, uint32_t cap, int cpu);

#endif /* _CPUINFO_H_ */
