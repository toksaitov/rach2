/**
 * kvmslotmusl: check KVM's memory slots against Linux's rules, and what a
 * guest sees of its slot as the caller's memory changes under it.
 *
 * The program opens /dev/kvm and makes a VM. KVM_SET_USER_MEMORY_REGION
 * then refuses an unknown flag, a size, address or caller's address not
 * page-aligned, address space 1, the deletion of no slot, an overlap, and a
 * change of a slot's size, caller's address or READONLY flag, all EINVAL
 * but the overlap's EEXIST, and takes a new slot, a second over the same
 * memory, a move and a change of the dirty-log flag; KVM_GET_DIRTY_LOG of
 * a slot that does not log is ENOENT, as is that of the last slot below
 * KVM_CHECK_EXTENSION(KVM_CAP_NR_MEMSLOTS), which does not exist, and of
 * the slot at that limit EINVAL, as Linux's kvm_get_dirty_log answers.
 *
 * A vCPU then runs a real-mode guest from a slot at guest address 0: at
 * 0x1000 it loads the byte at 0x2000, writes it to port 0x10 and halts.
 * The caller writes the byte, forks, writes it again in the parent after
 * the fork, makes the page read-only, drops it with MADV_DONTNEED, and
 * maps a new page over it, and after each the guest must read what the
 * caller's page holds then: the parent's copy, not the child's. A second
 * guest stores 0x44 at 0x2000 through a slot that logs, and the log names
 * page 2. VmPin stays 0 throughout, since KVM pins no page. KVM_RUN must
 * run the guest; EOPNOTSUPP is a failure, as is any other refusal.
 *
 * It needs VMX or SVM on and allowed by the firmware. It prints
 * "KVMSLOTMUSL OK" and exits 0, or "KVMSLOTMUSL FAIL (detail)" and exits 1
 * at the first failure, /dev/kvm missing among them.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* The ioctls and numbers of linux/kvm.h this uses; the port has no such
 * header. */
#define KVM_CREATE_VM              0xAE01
#define KVM_CHECK_EXTENSION        0xAE03
#define KVM_GET_VCPU_MMAP_SIZE     0xAE04
#define KVM_CREATE_VCPU            0xAE41
#define KVM_GET_DIRTY_LOG          0x4010AE42
#define KVM_SET_USER_MEMORY_REGION 0x4020AE46
#define KVM_SET_TSS_ADDR           0xAE47
#define KVM_RUN                    0xAE80
#define KVM_SET_REGS               0x4090AE82
#define KVM_GET_SREGS              0x8138AE83
#define KVM_SET_SREGS              0x4138AE84
#define KVM_CAP_USER_MEMORY        3
#define KVM_CAP_NR_MEMSLOTS        10
#define KVM_CAP_READONLY_MEM       81
#define KVM_MEM_LOG_DIRTY_PAGES    1u
#define KVM_MEM_READONLY           2u
#define KVM_EXIT_IO                2

#define PAGE 4096ul

struct region {
    unsigned int slot;
    unsigned int flags;
    unsigned long long guest_phys_addr;
    unsigned long long memory_size;
    unsigned long long userspace_addr;
};

struct dirty_log {
    unsigned int slot;
    unsigned int padding;
    unsigned long long bitmap;
};

/* The head of struct kvm_run, through kvm_run.io. */
struct run_head {
    unsigned char request_interrupt_window;
    unsigned char immediate_exit;
    unsigned char padding[6];
    unsigned int exit_reason;
    unsigned char ready, if_flag;
    unsigned short flags;
    unsigned long long cr8, apic_base;
    unsigned char io_direction, io_size;
    unsigned short io_port;
    unsigned int io_count;
    unsigned long long io_data_offset;
};

static int vm;

/** Print the failure `detail` and return 1. */
static int fail(const char *detail)
{
    printf("KVMSLOTMUSL FAIL (%s: errno %d)\n", detail, errno);
    return 1;
}

/** KVM_SET_USER_MEMORY_REGION: 0, or the errno it failed with. */
static int set(unsigned int slot, unsigned int flags, unsigned long long gpa,
               unsigned long long size, const void *hva)
{
    struct region r;

    r.slot = slot;
    r.flags = flags;
    r.guest_phys_addr = gpa;
    r.memory_size = size;
    r.userspace_addr = (unsigned long long)(unsigned long)hva;
    return ioctl(vm, KVM_SET_USER_MEMORY_REGION, &r) == 0 ? 0 : errno;
}

/** KVM_GET_DIRTY_LOG of `slot` into `bits`: 0, or the errno. */
static int get_log(unsigned int slot, unsigned long long *bits)
{
    struct dirty_log d;

    d.slot = slot;
    d.padding = 0;
    d.bitmap = (unsigned long long)(unsigned long)bits;
    return ioctl(vm, KVM_GET_DIRTY_LOG, &d) == 0 ? 0 : errno;
}

/** One row of the region checks: `got` must be `want`. */
static int expect(int got, int want, const char *detail)
{
    if (got != want) {
        printf("KVMSLOTMUSL FAIL (%s: %d, not %d)\n", detail, got, want);
        return 1;
    }
    return 0;
}

/** KVM_SET_USER_MEMORY_REGION's and KVM_GET_DIRTY_LOG's rules, over the
 * 16 pages at `mem`, for a VM of `nslots` slots, KVM_CAP_NR_MEMSLOTS: 0,
 * or 1 after a failure. */
static int region_rows(char *mem, unsigned int nslots)
{
    unsigned long long bits[1];
    unsigned long long size = 16 * PAGE;

    return expect(set(0, 4, 0x100000, size, mem), EINVAL, "an unknown flag") ||
           expect(set(0, 0, 0x100000, size + 1, mem), EINVAL, "a size not aligned") ||
           expect(set(0, 0, 0x100800, size, mem), EINVAL, "an address not aligned") ||
           expect(set(0, 0, 0x100000, size, mem + 8), EINVAL, "a caller's address not aligned") ||
           expect(set(0x10000, 0, 0x100000, size, mem), EINVAL, "address space 1") ||
           expect(set(0, 0, 0, 0, mem), EINVAL, "the deletion of no slot") ||
           expect(set(0, 0, 0x100000, size, mem), 0, "a new slot") ||
           expect(set(1, 0, 0x108000, size, mem), EEXIST, "an overlap") ||
           expect(set(1, 0, 0x110000, size, mem), 0, "a second slot over the same memory") ||
           expect(set(0, 0, 0x100000, size / 2, mem), EINVAL, "a change of size") ||
           expect(set(0, 0, 0x100000, size, mem + PAGE), EINVAL, "a change of address") ||
           expect(set(0, KVM_MEM_READONLY, 0x100000, size, mem), EINVAL, "READONLY") ||
           expect(set(0, 0, 0x100000, size, mem), 0, "no change") ||
           expect(set(0, 0, 0x118000, size, mem), EEXIST, "a move into the other") ||
           expect(set(0, 0, 0x200000, size, mem), 0, "a move") ||
           expect(set(0, KVM_MEM_LOG_DIRTY_PAGES, 0x200000, size, mem), 0, "logging on") ||
           expect(get_log(0, bits), 0, "the log") ||
           expect(bits[0] != 0, 0, "a log with no guest run is clear") ||
           expect(get_log(1, bits), ENOENT, "the log of a slot that does not log") ||
           expect(get_log(nslots - 1, bits), ENOENT,
                  "the log of the last slot, which does not exist") ||
           expect(get_log(nslots, bits), EINVAL, "the log of the slot at KVM_CAP_NR_MEMSLOTS") ||
           expect(set(0, 0, 0, 0, mem), 0, "a deletion") ||
           expect(set(1, 0, 0, 0, mem), 0, "the other deletion");
}

/** The kB of VmPin in /proc/self/status, or -1. */
static long vm_pin(void)
{
    char line[128];
    long kb = -1;
    FILE *f = fopen("/proc/self/status", "r");

    while (f && fgets(line, sizeof(line), f)) {
        if (strncmp(line, "VmPin:", 6) == 0) {
            kb = strtol(line + 6, NULL, 10);
        }
    }
    if (f) {
        fclose(f);
    }
    return kb;
}

/*
 * The guests' code, at 0x1000 in real mode. The reader:
 *   mov al, [0x2000]; out 0x10, al; hlt
 * The writer:
 *   mov byte [0x2000], 0x44; mov al, 0x44; out 0x10, al; hlt
 */
static const unsigned char reader[] = {0xA0, 0x00, 0x20, 0xE6, 0x10, 0xF4};
static const unsigned char writer[] = {0xC6, 0x06, 0x00, 0x20, 0x44, 0xB0, 0x44, 0xE6, 0x10, 0xF4};

/** Run the guest of `vcpu` from 0x1000 to its OUT: the byte it wrote to
 * port 0x10, or -1 after a failure. */
static int run_guest(int vcpu, volatile struct run_head *run)
{
    unsigned long long regs[18];

    memset(regs, 0, sizeof(regs));
    regs[16] = 0x1000;
    regs[17] = 2;
    if (ioctl(vcpu, KVM_SET_REGS, regs) != 0) {
        return fail("KVM_SET_REGS"), -1;
    }
    if (ioctl(vcpu, KVM_RUN, 0) != 0) {
        return fail("KVM_RUN"), -1;
    }
    if (run->exit_reason != KVM_EXIT_IO || run->io_port != 0x10 || run->io_direction != 1) {
        printf("KVMSLOTMUSL FAIL (the guest's exit: reason %u, port 0x%x)\n", run->exit_reason,
               run->io_port);
        return -1;
    }
    return ((volatile unsigned char *)run)[run->io_data_offset];
}

/** Check that the guest reads `want`: 0, or 1 after a failure. */
static int sees(int vcpu, volatile struct run_head *run, int want, const char *after)
{
    int got = run_guest(vcpu, run);

    if (got != want) {
        if (got >= 0) {
            printf("KVMSLOTMUSL FAIL (after %s the guest read 0x%x, not 0x%x)\n", after, got, want);
        }
        return 1;
    }
    return 0;
}

/** Fork; the child finds the old byte 0x11 in its copy and exits; the
 * parent then writes 0x22, a copy of its own: 0, or 1 after a failure. */
static int fork_row(volatile char *data)
{
    int status;
    pid_t pid = fork();

    if (pid < 0) {
        return fail("fork");
    }
    if (pid == 0) {
        _exit(data[0] == 0x11 ? 0 : 1);
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("KVMSLOTMUSL FAIL (the child's copy)\n");
        return 1;
    }
    data[0] = 0x22;
    return 0;
}

/** The guest rows over the 16 pages at `mem`, with the vCPU `vcpu`: 0,
 * or 1 after a failure. */
static int guest_rows(char *mem, int vcpu, volatile struct run_head *run)
{
    unsigned char sregs[312];
    unsigned long long bits[1];
    volatile char *data = mem + 2 * PAGE;
    int r;

    if (ioctl(vcpu, KVM_GET_SREGS, sregs) != 0) {
        return fail("KVM_GET_SREGS");
    }
    memset(sregs, 0, 8);      /* CS's base */
    memset(sregs + 12, 0, 2); /* CS's selector */
    if (ioctl(vcpu, KVM_SET_SREGS, sregs) != 0 || set(0, 0, 0, 16 * PAGE, mem) != 0) {
        return fail("KVM_SET_SREGS or the guest's slot");
    }
    memcpy(mem + PAGE, reader, sizeof(reader));
    data[0] = 0x11;
    r = sees(vcpu, run, 0x11, "the caller's write");
    if (r != 0) {
        return r;
    }
    if (fork_row(data) || sees(vcpu, run, 0x22, "a fork and the parent's write") ||
        mprotect(mem + 2 * PAGE, PAGE, PROT_READ) != 0 ||
        sees(vcpu, run, 0x22, "mprotect to read-only") ||
        mprotect(mem + 2 * PAGE, PAGE, PROT_READ | PROT_WRITE) != 0 ||
        madvise(mem + 2 * PAGE, PAGE, MADV_DONTNEED) != 0 || sees(vcpu, run, 0, "MADV_DONTNEED")) {
        return 1;
    }
    if (mmap(mem + 2 * PAGE, PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
             -1, 0) == MAP_FAILED) {
        return fail("a new page over the old");
    }
    data[0] = 0x33;
    if (sees(vcpu, run, 0x33, "a new page mapped over the old")) {
        return 1;
    }
    memcpy(mem + PAGE, writer, sizeof(writer));
    if (set(0, KVM_MEM_LOG_DIRTY_PAGES, 0, 16 * PAGE, mem) != 0 || get_log(0, bits) != 0 ||
        sees(vcpu, run, 0x44, "the store") || get_log(0, bits) != 0) {
        return fail("the dirty log's guest");
    }
    if (data[0] != 0x44 || !(bits[0] & 4)) {
        printf("KVMSLOTMUSL FAIL (the guest's store: 0x%x in memory, log 0x%llx)\n", data[0],
               bits[0]);
        return 1;
    }
    return set(0, 0, 0, 0, mem) != 0 ? fail("the guest's slot deleted") : 0;
}

/** Run the rows; 0 when they pass, 1 otherwise. */
int main(void)
{
    volatile struct run_head *run;
    int kvm, vcpu, r;
    long size, nslots;
    char *mem;

    kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm < 0) {
        return fail("open /dev/kvm");
    }
    vm = ioctl(kvm, KVM_CREATE_VM, 0);
    if (vm < 0) {
        return fail("KVM_CREATE_VM");
    }
    if (ioctl(vm, KVM_CHECK_EXTENSION, KVM_CAP_USER_MEMORY) != 1 ||
        ioctl(vm, KVM_CHECK_EXTENSION, KVM_CAP_READONLY_MEM) != 1) {
        return fail("KVM_CAP_USER_MEMORY or KVM_CAP_READONLY_MEM");
    }
    nslots = ioctl(vm, KVM_CHECK_EXTENSION, KVM_CAP_NR_MEMSLOTS);
    if (nslots < 3) {
        printf("KVMSLOTMUSL FAIL (KVM_CAP_NR_MEMSLOTS is %ld)\n", nslots);
        return 1;
    }
    mem = mmap(NULL, 16 * PAGE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        return fail("mmap");
    }
    if (region_rows(mem, (unsigned int)nslots) != 0) {
        return 1;
    }
    size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
    if (ioctl(vm, KVM_SET_TSS_ADDR, 0xFFFBD000ul) != 0 ||
        (vcpu = ioctl(vm, KVM_CREATE_VCPU, 0)) < 0 || size < 4096) {
        return fail("KVM_SET_TSS_ADDR or KVM_CREATE_VCPU");
    }
    run = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
    if (run == MAP_FAILED) {
        return fail("mmap of the vCPU");
    }
    r = guest_rows(mem, vcpu, run);
    if (r != 0) {
        return 1;
    }
    if (vm_pin() != 0) {
        printf("KVMSLOTMUSL FAIL (VmPin is %ld kB)\n", vm_pin());
        return 1;
    }
    printf("KVMSLOTMUSL OK\n");
    return 0;
}
