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

/*
 * What the vDSO's clock functions do: call the kernel; read the TSC; or
 * read KVM's clock, the TSC converted through KVM's clock page for the boot
 * processor, mapped below the data page, while that page has
 * PVCLOCK_TSC_STABLE_BIT set, checked inside each reading, and call the
 * kernel when it is clear, as Linux's vDSO does in VDSO_CLOCKMODE_PVCLOCK
 * (vread_pvclock, arch/x86/include/asm/vdso/gettimeofday.h:197-232).
 */
#define VCLOCK_NONE    0u
#define VCLOCK_TSC     1u
#define VCLOCK_PVCLOCK 2u

/* The pages of [vvar]: KVM's clock page, then the data page, just below
 * the image. */
#define VDSO_VVAR_PAGES 2u

/*
 * The data page, the upper page of [vvar] in /proc/PID/maps, mapped
 * read-only one page below the vDSO's image, which reads it at these
 * offsets (arch/x86_64/vdsocode.s): the vDSO's copy of the offsets must
 * change with this structure.
 *
 * The kernel changes it between two increments of `seq`, which is odd
 * meanwhile; a reader reads `seq`, the fields and the TSC, then `seq`
 * again, and reads again when it was odd or has moved. The page's count is
 * the TSC in VCLOCK_TSC and KVM's clock in nanoseconds in VCLOCK_PVCLOCK.
 * At the count `cycle_base` the monotonic clock was `mono_sec` seconds and
 * `mono_nsec` nanoseconds, real time `real_sec` and `real_nsec`, and the
 * raw clock `raw_sec` and `raw_nsec`; each count since adds `mult` /
 * 2^`shift` nanoseconds to the first, `real_mult` / 2^`shift`, the same, to
 * the second, and `raw_mult` / 2^`shift` to the third, the count's measured
 * rate, which the NTP state's slew does not change. Each clock's rate is 8
 * bytes before its seconds. The coarse clocks are the first two times as
 * they are, without the counts since.
 *
 * The time zone, which gettimeofday gives, is the kernel's
 * (`vclock_set_tz`), outside the sequence count, as Linux keeps it.
 */
struct vdso_data {
    volatile uint32_t seq; /* odd while the kernel changes the page */
    uint32_t mode;         /* VCLOCK_NONE, VCLOCK_TSC or VCLOCK_PVCLOCK */
    uint64_t cycle_base;   /* the page's count at the times below */
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
    uint32_t pad2;
    uint32_t raw_mult; /* the raw clock's nanoseconds a count, times 2^shift */
    uint32_t pad3;
    int64_t raw_sec;        /* CLOCK_MONOTONIC_RAW at cycle_base */
    uint64_t raw_nsec;      /* below 10^9 */
    int32_t tz_minuteswest; /* struct timezone's, as settimeofday set it */
    int32_t tz_dsttime;
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
#define VDSO_DATA_RAW_MULT   80
#define VDSO_DATA_RAW_SEC    88
#define VDSO_DATA_RAW_NSEC   96
#define VDSO_DATA_TZ_MW      104
#define VDSO_DATA_TZ_DST     108

/* The clocks `vclock_read` and `vclock_read_cycles` read. */
#define VCLOCK_READ_MONO 0
#define VCLOCK_READ_REAL 1
#define VCLOCK_READ_RAW  2

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

/** What the running clock counts: VCLOCK_TSC, the TSC itself,
 * VCLOCK_PVCLOCK, the TSC through KVM's clock pages, or VCLOCK_NONE while
 * it does not run. Read without a lock. */
uint32_t vclock_mode(void);

/** The clock's generation: a count that goes up each time the clock stops
 * or starts running from the TSC, after the page shows the change, so that
 * KVM decides its VMs' master clocks again (kvm/pvclock.c), as Linux's
 * pvclock_gtod_notify does when the host's clocksource leaves the TSC.
 * Read without a lock. */
uint32_t vclock_generation(void);

/**
 * Carry the clock on at the boot processor's tick, with interrupts off: the
 * page's times are moved to the page's count now, and the monotonic clock's
 * rate, the measured one at the rate of the NTP state's tick length
 * (`rtc_mono_rate`), and the raw clock's, the measured one, are each steered
 * toward the same clock of the tick (`rtc_tick_both`) by at most one part
 * in 2000, so that neither drifts from what it follows and no reading is
 * earlier than one before it. Real time is the monotonic clock plus the
 * offset (`vclock_set_wall`). On x86, where KVM's clock pages are kept,
 * the page counts KVM's clock, and the clock stops while the boot
 * processor's page withdraws its stable bit and starts again once the bit
 * is back, each inside `rtc_vclock_change`, so that the tick's clocks go
 * on from the stopped clock's last time and the started clock from the
 * latest reading given.
 */
void vclock_tick(void);

/**
 * Read the end of the watched TSC's span on this processor, when the tick
 * found it due and it is this processor's turn, and pass the turn to the
 * next online processor, so that the watch sets every processor's TSC
 * against the tick's clock, as Linux's clocksource watchdog does. Every
 * processor's tick runs it, and so does the interprocessor interrupt that
 * the tick's kick sends one in an idle wait. With interrupts off; nothing
 * where no TSC is watched.
 */
void vclock_watch_cpu(void);

/**
 * The clock `which` now, VCLOCK_READ_MONO, VCLOCK_READ_REAL or
 * VCLOCK_READ_RAW, from the TSC, or with `coarse` as of the last tick, in
 * `*sec` and `*nsec`, which is below 10^9; the raw clock has no coarse
 * reading. 0 with nothing read when the clock does not run from the TSC,
 * which each pass of the reading checks on the page, so that a reading the
 * clock's stop overtakes reads nothing. In VCLOCK_PVCLOCK, once KVM's page
 * for the boot processor withdraws its stable bit, where the vDSO makes
 * the system call, the clock is carried on from the page's base by KVM's
 * clock read from the running processor's page, never behind a reading
 * before it, as the stop carries it, until the tick stops the clock. Any
 * processor and any context may call it.
 */
int vclock_read(int which, int coarse, int64_t *sec, int64_t *nsec);

/**
 * The clock `which`, as for `vclock_read`, now in nanoseconds into `*ns`,
 * with the TSC it was read at in `*cycles`, in VCLOCK_PVCLOCK the TSC that
 * KVM's page converted, so that the two are one
 * moment, as Linux's kvm_get_time_and_clockread pairs them for KVM's master
 * clock. 1, or 0 with nothing read when the clock does not run from the
 * TSC or KVM's page for the boot processor has withdrawn its stable bit.
 */
int vclock_read_cycles(int which, uint64_t *ns, uint64_t *cycles);

/**
 * Make real time the monotonic clock plus `sec` seconds and `nsec`
 * nanoseconds, from 0 to 10^9 - 1, the offset clock.c gives at the anchor,
 * each setting and each leap second (`rtc_offset`): the page's base moves to
 * the page's count now and real time there is the monotonic clock's plus the
 * offset. The offset is kept for the clock's start when it does not run
 * yet. Interrupts may be on or off; the caller may hold clock.c's lock.
 */
void vclock_set_wall(int64_t sec, int32_t nsec);

/**
 * Give the vDSO's gettimeofday the time zone settimeofday set, struct
 * timezone's two fields, as Linux's update_vsyscall_tz does
 * (kernel/time/vsyscall.c): two stores outside the page's sequence count,
 * so that a reader may see one field old and one new, as on Linux. Kept
 * for the page when it is not built yet. Any context may call it.
 */
void vclock_set_tz(int32_t minuteswest, int32_t dsttime);

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

/** The physical frame of page `page` of the image for the ABI `abi`, of
 * the data page for VDSO_DATA_PAGE, or of the page that holds KVM's clock
 * pages for VDSO_KVM_PAGE. */
#define VDSO_DATA_PAGE 0xFFFFFFFFu
#define VDSO_KVM_PAGE  0xFFFFFFFEu
uintptr_t vdso_frame(int abi, uint32_t page);

/** The bytes the vDSO takes in an address space of the ABI `abi`: the
 * VDSO_VVAR_PAGES pages of [vvar] and the image, or 0 where there is
 * none. */
uintptr_t vdso_span(int abi);

/**
 * Add [vvar] at `at`, KVM's clock page and the data page, and the image
 * for the ABI `abi` after it to `vm`, each a region of its own whose pages
 * come from the kernel's frames when first touched: [vvar] read-only and
 * never writable, and the
 * image read and execute, a private mapping that mprotect may make writable
 * and that a write, or a tracer's, copies. Returns the image's address,
 * AT_SYSINFO_EHDR; 0 where there is no vDSO; -1 when a region cannot be
 * added, with `vm` as it was.
 */
intptr_t vdso_map(struct vm_space *vm, uintptr_t at, int abi);

/** The region's name in /proc/PID/maps, "[vvar]" or "[vdso]", for a file
 * that `vdso_map` maps, of either image, and NULL for any other. */
const char *vdso_region_name(const struct vm_file *f);

/** Whether `f` is the file of [vvar], which a core file
 * leaves as a hole (proc/coredump.c). */
int vdso_is_data(const struct vm_file *f);

#endif /* _VDSO_H_ */
