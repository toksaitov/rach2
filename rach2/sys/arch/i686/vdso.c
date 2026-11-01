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

void vclock_tick(void)
{}

int vclock_read(int real, int coarse, int64_t *sec, int64_t *nsec)
{
    (void)real;
    (void)coarse;
    (void)sec;
    (void)nsec;
    return 0;
}

void vclock_set_offset(int64_t sec, int32_t nsec)
{
    (void)sec;
    (void)nsec;
}

uint32_t vclock_resolution_ns(void)
{
    return 0;
}

uint32_t vdso_image_pages(void)
{
    return 0;
}

uintptr_t vdso_frame(uint32_t page)
{
    (void)page;
    return 0;
}
