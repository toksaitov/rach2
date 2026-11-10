/* A small KVM guest that exercises SVM entry, memory faults and intercepts. */

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/reboot.h>
#include <sys/wait.h>
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
    slot.memory_size = 2u * PAGE;
    slot.userspace_addr = (uint64_t)(uintptr_t)g->mem;
    if (ioctl(g->vm, KVM_SET_USER_MEMORY_REGION, &slot) != 0 ||
        ioctl(g->cpu, KVM_GET_SREGS, &s) != 0)
        return fail("slot or special registers");
    slot.slot = 1;
    slot.guest_phys_addr = 2u * PAGE;
    slot.userspace_addr += 2u * PAGE;
    if (ioctl(g->vm, KVM_SET_USER_MEMORY_REGION, &slot) != 0)
        return fail("data slot");
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

/** Fork while the parent owns a populated slot, then break its copy. */
static int fork_slot(struct guest *g)
{
    pid_t child;
    int status;
    static const unsigned char read[] = {0xA0, 0x00, 0x20, 0xE6, 0x10, 0xF4};

    child = fork();
    if (child < 0)
        return fail("fork");
    if (child == 0)
        _exit(g->mem[2u * PAGE] == 0x52 ? 0 : 1);
    g->mem[2u * PAGE] = 0x63;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status))
        return fail("child's copy");
    return run_code(g, read, sizeof(read), 0x63);
}

/** Check slot translations after fork, protection, discard and unmap. */
static int slot_rows(struct guest *g)
{
    static const unsigned char read[] = {0xA0, 0x00, 0x20, 0xE6, 0x10, 0xF4};
    void *page = g->mem + 2u * PAGE;

    if (fork_slot(g) != 0 || mprotect(page, PAGE, PROT_READ) != 0 ||
        run_code(g, read, sizeof(read), 0x63) != 0 ||
        mprotect(page, PAGE, PROT_READ | PROT_WRITE) != 0 ||
        madvise(page, PAGE, MADV_DONTNEED) != 0 || run_code(g, read, sizeof(read), 0) != 0)
        return fail("slot protection or discard");
    if (munmap(page, PAGE) != 0 ||
        mmap(page, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) ==
            MAP_FAILED)
        return fail("slot unmap and replacement");
    g->mem[2u * PAGE] = 0x74;
    if (run_code(g, read, sizeof(read), 0x74) != 0)
        return -1;
    puts("KVMSLOTS OK");
    return 0;
}

struct active_guest {
    struct guest *g;
    int done;
    int error;
};

/** A signal that interrupts a guest being stopped after a caller's error. */
static void wake_guest(int signo)
{
    (void)signo;
}

/** Run a guest that counts iterations until its data page contains 0x85. */
static void *active_run(void *arg)
{
    struct active_guest *a = arg;
    struct guest *g = a->g;
    struct kvm_regs regs;
    static const unsigned char spin[] = {0x66, 0xFF, 0x06, 0x00, 0x30, 0xA0, 0x00, 0x20,
                                         0x3C, 0x85, 0x75, 0xF4, 0xE6, 0x10, 0xF4};
    int r;

    memcpy(g->mem + PAGE, spin, sizeof(spin));
    memset(&regs, 0, sizeof(regs));
    regs.rip = PAGE;
    regs.rsp = 4u * PAGE - 16u;
    regs.rflags = 2;
    if (ioctl(g->cpu, KVM_SET_REGS, &regs) != 0) {
        a->error = errno;
    } else {
        do {
            r = ioctl(g->cpu, KVM_RUN, 0);
        } while (r < 0 && errno == EFAULT && !g->run->immediate_exit);
        if (r != 0 || g->run->exit_reason != KVM_EXIT_IO || g->run->u.io.port != 0x10 ||
            ((unsigned char *)g->run)[g->run->u.io.data_offset] != 0x85)
            a->error = errno ? errno : EIO;
    }
    __atomic_store_n(&a->done, 1, __ATOMIC_RELEASE);
    return NULL;
}

/** Change a populated slot while the guest runs, then stop it on all paths. */
static int active_slot(struct guest *g)
{
    struct active_guest a;
    struct sigaction action;
    pthread_t thread;
    volatile uint32_t *counter = (volatile uint32_t *)(g->mem + 3u * PAGE);
    void *page = g->mem + 2u * PAGE;
    pid_t child;
    int result = 0, status;

    memset(&action, 0, sizeof(action));
    action.sa_handler = wake_guest;
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL) != 0)
        return fail("signal handler");
    memset(&a, 0, sizeof(a));
    a.g = g;
    *counter = 0;
    if (pthread_create(&thread, NULL, active_run, &a) != 0)
        return fail("guest thread");
    while (*counter == 0 && !__atomic_load_n(&a.done, __ATOMIC_ACQUIRE))
        sched_yield();
    if (__atomic_load_n(&a.done, __ATOMIC_ACQUIRE) || mprotect(page, PAGE, PROT_READ) != 0) {
        result = fail("active slot protection");
    } else {
        child = fork();
        if (child == 0)
            _exit(g->mem[2u * PAGE] == 0x74 ? 0 : 1);
        if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
            WEXITSTATUS(status)) {
            result = fail("active slot fork");
        } else if (munmap(page, PAGE) != 0 ||
                   mmap(page, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                        -1, 0) == MAP_FAILED) {
            result = fail("active slot replacement");
        } else {
            g->mem[2u * PAGE] = 0x85;
        }
    }
    if (result != 0) {
        g->run->immediate_exit = 1;
        pthread_kill(thread, SIGUSR1);
    }
    pthread_join(thread, NULL);
    g->run->immediate_exit = 0;
    if (a.error != 0) {
        errno = a.error;
        return fail("active guest");
    }
    if (result == 0)
        puts("KVMACTIVE OK");
    return result;
}

/** Replace the data slot at 0x2000 with size bytes of a caller's mapping. */
static int data_slot(struct guest *g, void *mapping, size_t size)
{
    struct kvm_userspace_memory_region slot;

    memset(&slot, 0, sizeof(slot));
    slot.slot = 1;
    if (ioctl(g->vm, KVM_SET_USER_MEMORY_REGION, &slot) != 0)
        return fail("delete data slot");
    slot.guest_phys_addr = 2u * PAGE;
    slot.memory_size = size;
    slot.userspace_addr = (uint64_t)(uintptr_t)mapping;
    return ioctl(g->vm, KVM_SET_USER_MEMORY_REGION, &slot) == 0 ? 0 : fail("replace data slot");
}

/** Read a page of VGA memory through a /dev/mem slot, without changing it. */
static int device_slot(struct guest *g)
{
    static const unsigned char read[] = {0xA0, 0x00, 0x20, 0xE6, 0x10, 0xF4};
    unsigned char *mapping;
    int fd = open("/dev/mem", O_RDWR | O_CLOEXEC), result;

    if (fd < 0)
        return fail("open /dev/mem");
    mapping = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0xA0000);
    if (mapping == MAP_FAILED) {
        close(fd);
        return fail("map VGA memory");
    }
    result = data_slot(g, mapping, PAGE);
    if (result == 0)
        result = run_code(g, read, sizeof(read), mapping[0]);
    if (data_slot(g, g->mem + 2u * PAGE, 2u * PAGE) != 0)
        result = -1;
    munmap(mapping, PAGE);
    close(fd);
    if (result == 0)
        puts("KVMDEVICE OK");
    return result;
}

/* Linux's MODE_CREATE_DUMB, MODE_MAP_DUMB and MODE_DESTROY_DUMB records. */
struct dumb_create {
    uint32_t height, width, bpp, flags, handle, pitch;
    uint64_t size;
};
struct dumb_map {
    uint32_t handle, pad;
    uint64_t offset;
};

/** Use flat 32-bit protected mode, so an operand can reach the object's end. */
static int flat_mode(struct guest *g)
{
    struct kvm_sregs s;

    if (ioctl(g->cpu, KVM_GET_SREGS, &s) != 0)
        return fail("read segments");
    s.cs.base = 0;
    s.cs.selector = 8;
    s.cs.limit = 0xFFFFFFFFu;
    s.cs.db = s.cs.g = 1;
    s.ds = s.es = s.ss = s.cs;
    s.ds.type = s.es.type = s.ss.type = 3;
    s.ds.selector = s.es.selector = s.ss.selector = 16;
    s.cr0 = 0x11;
    return ioctl(g->cpu, KVM_SET_SREGS, &s) == 0 ? 0 : fail("flat segments");
}

/** Read the end of a 4 MiB GEM object as a slot, including its first fault. */
static int object_slot(struct guest *g)
{
    struct dumb_create create;
    struct dumb_map map;
    unsigned char read[] = {0xA0, 0, 0, 0, 0, 0xE6, 0x10, 0xF4};
    unsigned char *mapping = MAP_FAILED;
    uint32_t address;
    int fd = open("/dev/dri/card0", O_RDWR | O_CLOEXEC), result = -1;

    if (fd < 0)
        return fail("open DRM card");
    memset(&create, 0, sizeof(create));
    create.width = create.height = 1024;
    create.bpp = 32;
    if (ioctl(fd, 0xC02064B2ul, &create) != 0) {
        close(fd);
        return fail("create 4 MiB object");
    }
    memset(&map, 0, sizeof(map));
    map.handle = create.handle;
    if (ioctl(fd, 0xC01064B3ul, &map) != 0)
        fail("object offset");
    else
        mapping = mmap(NULL, (size_t)create.size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                       (off_t)map.offset);
    if (mapping != MAP_FAILED && create.size >= 4u * 1024u * 1024u) {
        address = 2u * PAGE + (uint32_t)create.size - PAGE;
        read[1] = (unsigned char)address;
        read[2] = (unsigned char)(address >> 8);
        read[3] = (unsigned char)(address >> 16);
        read[4] = (unsigned char)(address >> 24);
        mapping[create.size - PAGE] = 0x96;
        if (flat_mode(g) == 0 && data_slot(g, mapping, (size_t)create.size) == 0)
            result = run_code(g, read, sizeof(read), 0x96);
    } else {
        fail("map 4 MiB object");
    }
    if (data_slot(g, g->mem + 2u * PAGE, 2u * PAGE) != 0)
        result = -1;
    if (mapping != MAP_FAILED)
        munmap(mapping, (size_t)create.size);
    if (ioctl(fd, 0xC00464B4ul, &create.handle) != 0)
        result = fail("destroy object");
    close(fd);
    if (result == 0)
        puts("KVMOBJECT OK");
    return result;
}

#define RESOURCE_COUNTS 6u

struct resources {
    unsigned long value[RESOURCE_COUNTS];
};

/** Read the live KVM object counts and the system's open file descriptions. */
static int resources_read(struct resources *r)
{
    static const char *const names[] = {"kvm_vms", "kvm_vcpus", "kvm_slots", "kvm_tdp_pages",
                                        "kvm_run_mappings"};
    FILE *f;
    char line[256], name[64];
    unsigned long value;
    unsigned int i, seen = 0;

    memset(r, 0, sizeof(*r));
    f = fopen("/proc/stat", "r");
    if (f == NULL)
        return fail("resource counts");
    while (fgets(line, sizeof(line), f) != NULL) {
        if (sscanf(line, "%63s %lu", name, &value) != 2)
            continue;
        for (i = 0; i < 5u; i++) {
            if (strcmp(name, names[i]) == 0) {
                r->value[i] = value;
                seen |= 1u << i;
            }
        }
    }
    fclose(f);
    f = fopen("/proc/sys/fs/file-nr", "r");
    if (f == NULL)
        return fail("file descriptions");
    i = fscanf(f, "%lu", &r->value[5]) == 1;
    fclose(f);
    return seen == 31u && i ? 0 : fail("incomplete resource counts");
}

/** Require every live count to have returned to its value before the VM. */
static int resources_equal(const struct resources *before, const struct resources *after)
{
    unsigned int i;

    for (i = 0; i < RESOURCE_COUNTS; i++) {
        if (before->value[i] != after->value[i]) {
            fprintf(stderr, "svm-entry: fail: resource %u: %lu before, %lu after\n", i,
                    before->value[i], after->value[i]);
            return -1;
        }
    }
    return 0;
}

/** Run until killed, writing a shared counter once per guest iteration. */
static void leak_child(volatile uint32_t *shared)
{
    struct guest g;
    struct kvm_regs regs;
    static const unsigned char spin[] = {0x66, 0xFF, 0x06, 0x00, 0x20, 0xEB, 0xF9};
    int result = open_guest(&g);

    if (result == 0)
        result = data_slot(&g, (void *)shared, PAGE);
    if (result == 0) {
        memcpy(g.mem + PAGE, spin, sizeof(spin));
        memset(&regs, 0, sizeof(regs));
        regs.rip = PAGE;
        regs.rflags = 2;
        if (ioctl(g.cpu, KVM_SET_REGS, &regs) == 0)
            ioctl(g.cpu, KVM_RUN, 0);
    }
    shared[1] = 1;
    close_guest(&g);
    _exit(1);
}

/** Kill one process while its guest is running, and reap it on every path. */
static int kill_guest(volatile uint32_t *shared)
{
    struct resources before, during, after;
    pid_t child;
    int status, result = 0;

    if (resources_read(&before) != 0)
        return -1;
    shared[0] = shared[1] = 0;
    child = fork();
    if (child < 0)
        return fail("leak fork");
    if (child == 0)
        leak_child(shared);
    while (shared[0] == 0 && shared[1] == 0)
        sched_yield();
    if (shared[1] != 0 || resources_read(&during) != 0) {
        result = fail("start guest to kill");
    } else if (during.value[0] != before.value[0] + 1u || during.value[1] != before.value[1] + 1u ||
               during.value[2] != before.value[2] + 2u || during.value[3] <= before.value[3] ||
               during.value[4] != before.value[4] + 1u) {
        result = fail("live guest counts");
    }
    if (kill(child, SIGKILL) != 0 || waitpid(child, &status, 0) != child)
        return fail("kill and reap guest");
    if (!WIFSIGNALED(status) || WTERMSIG(status) != SIGKILL)
        result = fail("guest kill status");
    if (resources_read(&after) != 0 || resources_equal(&before, &after) != 0)
        return -1;
    return result;
}

/** Count the resources of three killed guests back to their initial values. */
static int leak_rows(void)
{
    volatile uint32_t *shared;
    int i, result = 0;

    shared = mmap(NULL, PAGE, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    if (shared == MAP_FAILED)
        return fail("shared loop counter");
    for (i = 0; i < 3 && result == 0; i++)
        result = kill_guest(shared);
    munmap((void *)shared, PAGE);
    if (result == 0)
        puts("KVMLEAK OK");
    return result;
}

/** Run the small guest; as PID 1, power off after reporting its result. */
int main(int argc, char **argv)
{
    struct guest g;
    struct resources before, after;
    int result;

    if (resources_read(&before) != 0)
        return 1;
    result = open_guest(&g);
    if (result == 0)
        result = rows(&g);
    if (result == 0 && (getpid() == 1 || (argc == 2 && strcmp(argv[1], "slots") == 0))) {
        result = slot_rows(&g);
        if (result == 0)
            result = active_slot(&g);
        if (result == 0)
            result = device_slot(&g);
        if (result == 0)
            result = object_slot(&g);
    }
    close_guest(&g);
    if (resources_read(&after) != 0 || resources_equal(&before, &after) != 0)
        result = -1;
    if (result == 0 && (getpid() == 1 || (argc == 2 && strcmp(argv[1], "leak") == 0)))
        result = leak_rows();
    fflush(NULL);
    if (getpid() == 1) {
        reboot(RB_POWER_OFF);
        for (;;)
            pause();
    }
    return result == 0 ? 0 : 1;
}
