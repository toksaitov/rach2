/*
 * vdso.h: the virtual dynamic shared object, a library the kernel maps into
 * every new image so that a program reads the clock without a system call,
 * and the data page beside it that the kernel keeps the clock's state in.
 * See arch/x86_64/vdso.c and vclock.c, and proc/vdso.c for the mapping.
 */
#ifndef _VDSO_H_
#define _VDSO_H_

#include "lib/kint.h"

struct rtc_course;
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
 * count since adds `mult` / 2^`shift` nanoseconds to the first and
 * `real_mult` / 2^`shift` to the second, which the NTP state's slew makes
 * faster or slower. Each clock's rate is 8 bytes before its seconds. The
 * coarse clocks are those two times as they are, without the counts since.
 */
struct vdso_data {
    volatile uint32_t seq; /* odd while the kernel changes the page */
    uint32_t mode;         /* VCLOCK_NONE or VCLOCK_TSC */
    uint64_t cycle_base;   /* the TSC at the times below */
    uint32_t mult;         /* nanoseconds a count, times 2^shift */
    uint32_t shift;        /* at most 32 */
    int64_t mono_sec;      /* CLOCK_MONOTONIC at cycle_base */
    uint64_t mono_nsec;    /* below 10^9 */
    uint32_t real_mult;    /* real time's nanoseconds a count, times 2^shift */
    uint32_t pad;
    int64_t real_sec;   /* CLOCK_REALTIME at cycle_base */
    uint64_t real_nsec; /* below 10^9 */
    uint32_t res_ns;    /* the resolution clock_getres gives the two */
    uint32_t getcpu;    /* 1 when `rdtscp` gives the processor's number */
    uint32_t coarse_ns; /* the coarse clocks' resolution, the tick */
};

/* The offsets the vDSO reads the page at. */
#define VDSO_DATA_SEQ        0
#define VDSO_DATA_MODE       4
#define VDSO_DATA_CYCLE_BASE 8
#define VDSO_DATA_MULT       16
#define VDSO_DATA_SHIFT      20
#define VDSO_DATA_MONO_SEC   24
#define VDSO_DATA_MONO_NSEC  32
#define VDSO_DATA_REAL_MULT  40
#define VDSO_DATA_REAL_SEC   48
#define VDSO_DATA_REAL_NSEC  56
#define VDSO_DATA_RES_NS     64
#define VDSO_DATA_GETCPU     68
#define VDSO_DATA_COARSE_NS  72

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
 * page's times are moved to the TSC's count now, the monotonic clock's rate
 * is steered toward the reference that keeps the tick, and real time's
 * toward the course (`vclock_set_course`), each by at most one part in
 * 2000, so that neither drifts from what it follows and no reading is
 * earlier than one before it.
 */
void vclock_tick(void);

/**
 * The clock now, from the TSC, or with `coarse` as of the last tick: real
 * time when `real` is set, the monotonic clock otherwise, in `*sec` and
 * `*nsec`, which is below 10^9. 0 with nothing read when the clock does not
 * run from the TSC. Any processor and any context may call it.
 */
int vclock_read(int real, int coarse, int64_t *sec, int64_t *nsec);

/**
 * The clock now in nanoseconds into `*ns`, real time when `real` is set and
 * the monotonic clock otherwise, with the TSC count it was read at in
 * `*cycles`, so that the two are one moment, as Linux's
 * kvm_get_time_and_clockread pairs them for KVM's master clock. 1, or 0
 * with nothing read when the clock does not run from the TSC.
 */
int vclock_read_cycles(int real, uint64_t *ns, uint64_t *cycles);

/**
 * Make real time follow the course `c` (rtc.h), which clock.c gives at the
 * anchor, at each setting with `step`, and at each change of its rate: the
 * page's base moves to the TSC's count now, real time there is the
 * course's with `step` and the page's own otherwise, so that a change of
 * rate makes no jump, and its rate is the monotonic clock's times the
 * course's. Interrupts may be on or off; the caller may hold clock.c's
 * lock.
 */
void vclock_set_course(const struct rtc_course *c, int step);

/** The resolution of the clock from the TSC, in nanoseconds: a count,
 * rounded up. */
uint32_t vclock_resolution_ns(void);

/** The system calls clock_gettime, clock_gettime64, gettimeofday and time
 * made since boot, modulo 2^32, which /proc/stat gives as `clock_calls`, so
 * that a test sees a read of the clock the vDSO answered (syscall.c). */
uint32_t clock_calls(void);

/*
 * The images and the page, which the mapping below places. The images are
 * one for each ABI the kernel runs programs of: its own programs', and on
 * x86-64 the 32-bit one an i386 program is given (arch/x86_64/vdso32.c),
 * which shares the data page. An architecture without a vDSO has no pages.
 */
#define VDSO_NATIVE 0
#define VDSO_IA32   1

/** The pages of the vDSO's image for the ABI `abi`, 0 where there is
 * none. */
uint32_t vdso_image_pages(int abi);

/** The physical frame of page `page` of the image for the ABI `abi`, or
 * of the data page for VDSO_DATA_PAGE. */
#define VDSO_DATA_PAGE 0xFFFFFFFFu
uintptr_t vdso_frame(int abi, uint32_t page);

/** The bytes the vDSO takes in an address space of the ABI `abi`: the
 * data page and the image, or 0 where there is none. */
uintptr_t vdso_span(int abi);

/**
 * Add the data page at `at` and the image for the ABI `abi` after it to
 * `vm`, each a region of its own whose pages come from the kernel's frames
 * when first touched: the data page read-only and never writable, and the
 * image read and execute, a private mapping that mprotect may make writable
 * and that a write, or a tracer's, copies. Returns the image's address,
 * AT_SYSINFO_EHDR; 0 where there is no vDSO; -1 when a region cannot be
 * added, with `vm` as it was.
 */
intptr_t vdso_map(struct vm_space *vm, uintptr_t at, int abi);

/** The region's name in /proc/PID/maps, "[vvar]" or "[vdso]", for a file
 * that `vdso_map` maps, of either image, and NULL for any other. */
const char *vdso_region_name(const struct vm_file *f);

/** Whether `f` is the file of the data page, [vvar], which a core file
 * leaves as a hole (proc/coredump.c). */
int vdso_is_data(const struct vm_file *f);

#endif /* _VDSO_H_ */
