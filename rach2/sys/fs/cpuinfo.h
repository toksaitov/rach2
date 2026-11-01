#ifndef _CPUINFO_H_
#define _CPUINFO_H_

/*
 * cpuinfo: the text of /proc/cpuinfo, in Linux's format for x86, from what
 * `cpuid` reports: the vendor, family, model and stepping, the brand string,
 * the last-level cache, the APIC ID, the feature flags by Linux's names, the
 * cache line and the address sizes, a block for each processor. The
 * processors are taken to be alike, so `cpuid` on the running one answers
 * for each but its APIC ID. It has no "cpu MHz" or "bogomips" line, since
 * the kernel does not measure the processor's clock.
 *
 * The topology, which /proc/cpuinfo and /sys/devices/system/cpu/cpuN/topology
 * show, cuts a processor's APIC ID into its package, its core in the package
 * and its thread in the core, at the widths `cpuid` gives: leaf 0xB's levels,
 * and without them leaf 1's count of logical processors divided into cores
 * by leaf 4 on Intel, or by leaf 0x80000008 and 0x8000001E on AMD.
 */

#include "../lib/kint.h"

/** The largest text `cpuinfo_text` makes. */
#define CPUINFO_TEXT_MAX 2048

/** Write processor `cpu`'s block into `out`, at most `cap` bytes with the
 * NUL; its length. */
uint32_t cpuinfo_text(char *out, uint32_t cap, int cpu);

/* Where a processor sits: each number counts from 0 within the next level
 * up, as Linux's physical_package_id, core_id and the thread's place. */
struct cpu_place {
    uint32_t package;
    uint32_t core;   /* within the package */
    uint32_t thread; /* within the core */
};

/** Processor `cpu`'s place, from its APIC ID. */
void cpuinfo_place(int cpu, struct cpu_place *p);

/* The levels `cpuinfo_siblings` compares. */
#define CPUINFO_CORE    0 /* the same core of the same package */
#define CPUINFO_PACKAGE 1 /* the same package */

/** The processors started that share `cpu`'s core or package, as `level`
 * says, a bit for each, `cpu`'s own among them when it is started. */
uint64_t cpuinfo_siblings(int cpu, int level);

#endif /* _CPUINFO_H_ */
