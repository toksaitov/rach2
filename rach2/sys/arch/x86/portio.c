/*
 * The x86's I/O port instructions, which the PC's devices are reached
 * through, and the harness's exit through QEMU's isa-debug-exit port.
 */

#include "portio.h"
#include "../../kio.h"

void outb(uint16_t port, uint8_t val)
{
    __asm__ __volatile__("outb %1, %0" : : "dN"(port), "a"(val));
}

#ifdef RACH2_HARNESS
/**
 * Report a process exit status to QEMU's isa-debug-exit device (harness only).
 *
 * Writing the status to I/O port 0xF4 makes the test harness read a real exit
 * code instead of scraping the serial log: a hang or triple-fault then surfaces
 * as a wall-clock timeout, not a missing marker. Only ring 0 may write
 * this port, so a ring-3 probe cannot fake it. QEMU exits with the value
 * `((status << 1) | 1)`, so an exit code of 1 means a status of 0.
 */
void harness_exit(uint8_t status)
{
    outb(0xF4, status);
}
#endif

void outw(uint16_t port, uint16_t val)
{
    __asm__ __volatile__("outw %1, %0" : : "dN"(port), "a"(val));
}

void outd(uint16_t port, uint32_t val)
{
    __asm__ __volatile__("outl %1, %0" : : "dN"(port), "a"(val));
}

uint8_t inb(uint16_t port)
{
    uint8_t result;

    __asm__ __volatile__("inb %1, %0" : "=a"(result) : "dN"(port));

    return result;
}

uint16_t inw(uint16_t port)
{
    uint16_t result;

    __asm__ __volatile__("inw %1, %0" : "=a"(result) : "dN"(port));

    return result;
}

uint32_t ind(uint16_t port)
{
    uint32_t result;

    __asm__ __volatile__("inl %1, %0" : "=a"(result) : "dN"(port));

    return result;
}

/* On x86-64 `rep` counts in the whole of RCX, so the count is widened
 * before the instruction is given it. */
void insw(uint16_t port, void *buffer, uint32_t count)
{
#if defined(__x86_64__)
    unsigned long n = count;

    __asm__ __volatile__("cld\n\t"
                         "rep insw"
                         : "+D"(buffer), "+c"(n)
                         : "d"(port)
                         : "memory", "cc");
#else
    __asm__ __volatile__("cld\n\t"
                         "rep insw"
                         : "+D"(buffer), "+c"(count)
                         : "d"(port)
                         : "memory", "cc");
#endif
}

void outsw(uint16_t port, const void *buffer, uint32_t count)
{
#if defined(__x86_64__)
    unsigned long n = count;

    __asm__ __volatile__("cld\n\t"
                         "rep outsw"
                         : "+S"(buffer), "+c"(n)
                         : "d"(port)
                         : "memory", "cc");
#else
    __asm__ __volatile__("cld\n\t"
                         "rep outsw"
                         : "+S"(buffer), "+c"(count)
                         : "d"(port)
                         : "memory", "cc");
#endif
}
