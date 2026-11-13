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

/* A cache of a processor, as leaf 4 describes one and Linux's
 * /sys/devices/system/cpu/cpuN/cache/indexM shows it. */
struct cpu_cache {
    uint32_t level;
    uint32_t type;       /* 1 data, 2 instruction, 3 unified */
    uint32_t line;       /* the coherency line, in bytes */
    uint32_t partitions; /* lines to a tag */
    uint32_t ways;
    uint32_t sets;
    uint32_t sharing; /* the logical processors the leaf says share it */
    uint32_t id;      /* the APIC ID above the bits that number those */
    uint64_t shared;  /* the processors started that share it, a bit each */
};

/**
 * Describe processor `cpu`'s cache `index`, from 0, into `*cache`: 1, or 0
 * past the last and on a processor that describes none. The leaves are
 * those Linux reads: leaf 4, or on Hygon and AMD leaf 0x8000001D, or on
 * AMD without TOPOEXT the older leaves 0x80000005 and 0x80000006, which
 * give an L1 data, an L1 instruction, an L2 and, when there is one, an
 * L3.
 */
int cpuinfo_cache(int cpu, int index, struct cpu_cache *cache);

/** The processors started that share processor `cpu`'s last-level cache, a
 * bit each, `cpu`'s own among them: the `shared` of its highest-level data
 * or unified cache (`cpuinfo_cache`), or its package's processors when it
 * describes none. */
uint64_t cpuinfo_llc_shared(int cpu);

#endif /* _CPUINFO_H_ */
