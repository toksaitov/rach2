/* A small KVM guest that exercises SVM entry, memory faults and intercepts. */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <unistd.h>

/* The hosted integer types supply the kernel ABI header's fixed widths. */
#define _KINT_H_
#include "../../rach2/sys/kvm/kvm_abi.h"

#define PAGE 4096u

struct guest {
    int kvm, vm, cpu;
    struct kvm_run *run;
    unsigned char *mem;
    size_t run_size;
};

/** Report an error and return -1. */
static int fail(const char *what)
{
    fprintf(stderr, "svm-entry: fail: %s: errno %d\n", what, errno);
    return -1;
}

/** Release every mapping and descriptor the guest owns, including on error. */
static void close_guest(struct guest *g)
{
    if (g->run != MAP_FAILED)
        munmap(g->run, g->run_size);
    if (g->mem != MAP_FAILED)
        munmap(g->mem, 4u * PAGE);
    if (g->cpu >= 0)
        close(g->cpu);
    if (g->vm >= 0)
        close(g->vm);
    if (g->kvm >= 0)
        close(g->kvm);
}

/** Make a real-mode guest with four pages at guest address zero. */
static int open_guest(struct guest *g)
{
    struct kvm_userspace_memory_region slot;
    struct kvm_sregs s;
    int size;

    memset(g, 0, sizeof(*g));
    g->kvm = g->vm = g->cpu = -1;
    g->run = MAP_FAILED;
    g->mem = MAP_FAILED;
    g->kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (g->kvm < 0 || (g->vm = ioctl(g->kvm, KVM_CREATE_VM, 0)) < 0 ||
        (g->cpu = ioctl(g->vm, KVM_CREATE_VCPU, 0)) < 0)
        return fail("create");
    size = ioctl(g->kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (size < (int)PAGE)
        return fail("run size");
    g->run_size = (size_t)size;
    g->run = mmap(NULL, g->run_size, PROT_READ | PROT_WRITE, MAP_SHARED, g->cpu, 0);
    g->mem = mmap(NULL, 4u * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (g->run == MAP_FAILED || g->mem == MAP_FAILED)
        return fail("mmap");
    memset(&slot, 0, sizeof(slot));
    slot.memory_size = 4u * PAGE;
    slot.userspace_addr = (uint64_t)(uintptr_t)g->mem;
    if (ioctl(g->vm, KVM_SET_USER_MEMORY_REGION, &slot) != 0 ||
        ioctl(g->cpu, KVM_GET_SREGS, &s) != 0)
        return fail("slot or special registers");
    s.cs.base = s.cs.selector = 0;
    s.ds.base = s.ds.selector = 0;
    s.ss.base = s.ss.selector = 0;
    if (ioctl(g->cpu, KVM_SET_SREGS, &s) != 0)
        return fail("real mode");
    return 0;
}

/** Run code at 0x1000 to its OUT of want to port 0x10, then to HLT. */
static int run_code(struct guest *g, const unsigned char *code, size_t n, unsigned char want)
{
    struct kvm_regs regs;

    memcpy(g->mem + PAGE, code, n);
    memset(&regs, 0, sizeof(regs));
    regs.rip = PAGE;
    regs.rsp = 4u * PAGE - 16u;
    regs.rflags = 2;
    if (ioctl(g->cpu, KVM_SET_REGS, &regs) != 0 || ioctl(g->cpu, KVM_RUN, 0) != 0)
        return fail("entry");
    if (g->run->exit_reason != KVM_EXIT_IO || g->run->u.io.port != 0x10 ||
        g->run->u.io.direction != KVM_EXIT_IO_OUT || g->run->u.io.size != 1 ||
        g->run->u.io.count != 1 || ((unsigned char *)g->run)[g->run->u.io.data_offset] != want) {
        fprintf(stderr, "svm-entry: fail: exit %u, port %u\n", g->run->exit_reason,
                g->run->u.io.port);
        return -1;
    }
    if (ioctl(g->cpu, KVM_RUN, 0) != 0 || g->run->exit_reason != KVM_EXIT_HLT)
        return fail("halt");
    return 0;
}

/** Check the first entry, CPUID, CR0, RDMSR and a slot's changed frame. */
static int rows(struct guest *g)
{
    static const unsigned char read[] = {0xA0, 0x00, 0x20, 0xE6, 0x10, 0xF4};
    static const unsigned char cpuid[] = {0x31, 0xC0, 0x0F, 0xA2, 0xB0, 0x42, 0xE6, 0x10, 0xF4};
    static const unsigned char cr0[] = {0x0F, 0x20, 0xC0, 0xE6, 0x10, 0xF4};
    static const unsigned char msr[] = {0x66, 0xB9, 0x80, 0x00, 0x00, 0xC0,
                                        0x0F, 0x32, 0xE6, 0x10, 0xF4};

    g->mem[2u * PAGE] = 0x31;
    if (run_code(g, read, sizeof(read), 0x31) != 0 ||
        run_code(g, cpuid, sizeof(cpuid), 0x42) != 0 || run_code(g, cr0, sizeof(cr0), 0x10) != 0 ||
        run_code(g, msr, sizeof(msr), 0) != 0)
        return -1;
    if (mmap(g->mem + 2u * PAGE, PAGE, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == MAP_FAILED)
        return fail("replace a slot's frame");
    g->mem[2u * PAGE] = 0x52;
    if (run_code(g, read, sizeof(read), 0x52) != 0)
        return -1;
    puts("SVMENTRY OK");
    return 0;
}

/** Run the small guest; as PID 1, power off after reporting its result. */
int main(void)
{
    struct guest g;
    int result;

    result = open_guest(&g);
    if (result == 0)
        result = rows(&g);
    close_guest(&g);
    fflush(NULL);
    if (getpid() == 1) {
        reboot(RB_POWER_OFF);
        for (;;)
            pause();
    }
    return result == 0 ? 0 : 1;
}
