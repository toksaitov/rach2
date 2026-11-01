/*
 * vdso.h: the virtual dynamic shared object, a library the kernel maps into
 * every new image so that a program reads the clock without a system call,
 * and the data page beside it that the kernel keeps the clock's state in.
 * See arch/x86_64/vdso.c and vclock.c, and proc/vdso.c for the mapping.
 */
#ifndef _VDSO_H_
#define _VDSO_H_

#include "lib/kint.h"

struct vm_space;
struct vm_file;

/* What the vDSO's clock functions do: call the kernel, or read the TSC. */
#define VCLOCK_NONE 0u
#define VCLOCK_TSC  1u

/*
 * The data page, [vvar] in /proc/PID/maps, mapped read-only one page below
 * the vDSO's image, which reads it at these offsets (arch/x86_64/vdsocode.s):
 * the vDSO's copy of the offsets must change with this structure.
 *
 * The kernel changes it between two increments of `seq`, which is odd
 * meanwhile; a reader reads `seq`, the fields and the TSC, then `seq`
 * again, and reads again when it was odd or has moved. At `cycle_base`
 * counts of the TSC the monotonic clock was `mono_sec` seconds and
 * `mono_nsec` nanoseconds and real time `real_sec` and `real_nsec`; each
 * count since adds `mult` / 2^`shift` nanoseconds. The coarse clocks are
 * those two times as they are, without the counts since.
 */
struct vdso_data {
    volatile uint32_t seq; /* odd while the kernel changes the page */
    uint32_t mode;         /* VCLOCK_NONE or VCLOCK_TSC */
    uint64_t cycle_base;   /* the TSC at the times below */
    uint32_t mult;         /* nanoseconds a count, times 2^shift */
    uint32_t shift;        /* at most 32 */
    int64_t mono_sec;      /* CLOCK_MONOTONIC at cycle_base */
    uint64_t mono_nsec;    /* below 10^9 */
    int64_t real_sec;      /* CLOCK_REALTIME at cycle_base */
    uint64_t real_nsec;    /* below 10^9 */
    uint32_t res_ns;       /* the resolution clock_getres gives the two */
    uint32_t getcpu;       /* 1 when `rdtscp` gives the processor's number */
    uint32_t coarse_ns;    /* the coarse clocks' resolution, the tick */
};

/* The offsets the vDSO reads the page at. */
#define VDSO_DATA_SEQ        0
#define VDSO_DATA_MODE       4
#define VDSO_DATA_CYCLE_BASE 8
#define VDSO_DATA_MULT       16
#define VDSO_DATA_SHIFT      20
#define VDSO_DATA_MONO_SEC   24
#define VDSO_DATA_MONO_NSEC  32
#define VDSO_DATA_REAL_SEC   40
#define VDSO_DATA_REAL_NSEC  48
#define VDSO_DATA_RES_NS     56
#define VDSO_DATA_GETCPU     60
#define VDSO_DATA_COARSE_NS  64

/*
 * The clock behind the page, which the kernel's own clock calls read as
 * well while it runs from the TSC, so that a reading the vDSO gives and one
 * a system call gives agree (clock.c). Each architecture defines these; one
 * without a vDSO answers that it is not running.
 */

/**
 * Decide, once every processor is started and before the first program,
 * whether the clock runs from the TSC: where `timer_calibrate` measured it,
 * every processor's counter agreed with the boot processor's at its start
 * and the vDSO's image was built; and log the verdict as `vdso: clock:`.
 * Builds the image and the data page.
 */
void vclock_start(void);

/** Whether the clock runs from the TSC. */
int vclock_running(void);

/**
 * Carry the clock on at the boot processor's tick, with interrupts off: the
 * page's times are moved to the TSC's count now, and the rate is steered
 * toward the reference that keeps the tick, by at most one part in 2000,
 * so that the two cannot drift apart and no reading is earlier than one
 * before it.
 */
void vclock_tick(void);

/**
 * The clock now, from the TSC, or with `coarse` as of the last tick: real
 * time when `real` is set, the monotonic clock otherwise, in `*sec` and
 * `*nsec`, which is below 10^9. 0 with nothing read when the clock does not
 * run from the TSC. Any processor and any context may call it.
 */
int vclock_read(int real, int coarse, int64_t *sec, int64_t *nsec);

/** Make real time the monotonic clock plus `sec` seconds and `nsec`
 * nanoseconds, from 0 to 10^9 - 1, as clock.c's anchor and step give it. */
void vclock_set_offset(int64_t sec, int32_t nsec);

/** The resolution of the clock from the TSC, in nanoseconds: a count,
 * rounded up. */
uint32_t vclock_resolution_ns(void);

/** The system calls clock_gettime, clock_gettime64, gettimeofday and time
 * made since boot, modulo 2^32, which /proc/stat gives as `clock_calls`, so
 * that a test sees a read of the clock the vDSO answered (syscall.c). */
uint32_t clock_calls(void);

/*
 * The image and the page, which the mapping below places. An architecture
 * without a vDSO has no pages.
 */

/** The pages of the vDSO's image, 0 where there is none. */
uint32_t vdso_image_pages(void);

/** The physical frame of the image's page `page`, or of the data page for
 * VDSO_DATA_PAGE. */
#define VDSO_DATA_PAGE 0xFFFFFFFFu
uintptr_t vdso_frame(uint32_t page);

/** The bytes the vDSO takes in an address space: the data page and the
 * image, or 0 where there is none. */
uintptr_t vdso_span(void);

/**
 * Add the data page at `at` and the image after it to `vm`, each a region
 * of its own whose pages come from the kernel's frames when first touched,
 * the data page read-only and the image read and execute, neither ever
 * writable. Returns the image's address, AT_SYSINFO_EHDR; 0 where there is
 * no vDSO; -1 when a region cannot be added, with `vm` as it was.
 */
intptr_t vdso_map(struct vm_space *vm, uintptr_t at);

/** The region's name in /proc/PID/maps, "[vvar]" or "[vdso]", for a file
 * that `vdso_map` maps, and NULL for any other. */
const char *vdso_region_name(const struct vm_file *f);

#endif /* _VDSO_H_ */
