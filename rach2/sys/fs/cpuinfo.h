#ifndef _CPUINFO_H_
#define _CPUINFO_H_

#include "../lib/kint.h"

/*
 * cpuinfo: the text of /proc/cpuinfo, in Linux's format for x86, from what
 * `cpuid` reports: the vendor, family, model and stepping, the brand string,
 * the last-level cache, the APIC ID, the feature flags by Linux's names, the
 * cache line and the address sizes. The kernel runs on one processor, so
 * the text describes processor 0 alone. It has no "cpu MHz" or "bogomips"
 * line, since the kernel does not measure the processor's clock.
 */

/** The largest text `cpuinfo_text` makes. */
#define CPUINFO_TEXT_MAX 2048

/** Write the text into `out`, at most `cap` bytes with the NUL; its length. */
uint32_t cpuinfo_text(char *out, uint32_t cap);

#endif /* _CPUINFO_H_ */
