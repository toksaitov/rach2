/*
 * vdso.c: the i686 has no vDSO, so its clock is the kernel's alone and no
 * image is mapped (../../vdso.h).
 */

#include "../../vdso.h"

void vclock_start(void)
{}

int vclock_running(void)
{
    return 0;
}

uint32_t vclock_mode(void)
{
    return VCLOCK_NONE;
}

uint32_t vclock_generation(void)
{
    return 0;
}

void vclock_tick(void)
{}

void vclock_watch_cpu(void)
{}

int vclock_read(int which, int coarse, int64_t *sec, int64_t *nsec)
{
    (void)which;
    (void)coarse;
    (void)sec;
    (void)nsec;
    return 0;
}

int vclock_read_cycles(int which, uint64_t *ns, uint64_t *cycles)
{
    (void)which;
    (void)ns;
    (void)cycles;
    return 0;
}

void vclock_set_wall(int64_t sec, int32_t nsec)
{
    (void)sec;
    (void)nsec;
}

void vclock_set_tz(int32_t minuteswest, int32_t dsttime)
{
    (void)minuteswest;
    (void)dsttime;
}

uint32_t vclock_resolution_ns(void)
{
    return 0;
}

uint32_t vdso_image_pages(int abi)
{
    (void)abi;
    return 0;
}

uintptr_t vdso_frame(int abi, uint32_t page)
{
    (void)abi;
    (void)page;
    return 0;
}
