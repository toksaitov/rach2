/**
 * kvmmusl: check the run page of a KVM vCPU, mapped through its descriptor,
 * and its fields across a fork.
 *
 * The program opens /dev/kvm, makes a VM and its vCPU 0, and maps the
 * vCPU's KVM_GET_VCPU_MMAP_SIZE bytes shared. It sets immediate_exit in the
 * mapping, and KVM_RUN fails with EINTR before any entry, which shows the
 * kernel reads the page the program wrote. A forked child finds the field
 * set in its copy of the mapping, gets EIO from KVM_RUN, since the VM
 * belongs to its parent's address space, and sets request_interrupt_window,
 * which the parent then reads in its own mapping, as on Linux, where a
 * fork's child shares the run page. A private mapping gives a copy of the
 * page, whose writes the shared one does not see. The descriptors are
 * close-on-exec, and the mapping keeps working after both are closed.
 *
 * It needs VMX or SVM on and allowed by the firmware, which the Lenovo has,
 * and QEMU's -cpu max under TCG gives as SVM. It prints "KVMMUSL OK" and
 * exits 0, or "KVMMUSL FAIL (detail)" and exits 1 at the first failure,
 * /dev/kvm missing among them.
 */

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>

/* The ioctls of linux/kvm.h this uses; the port has no such header. */
#define KVM_GET_API_VERSION    0xAE00
#define KVM_CREATE_VM          0xAE01
#define KVM_CHECK_EXTENSION    0xAE03
#define KVM_GET_VCPU_MMAP_SIZE 0xAE04
#define KVM_CREATE_VCPU        0xAE41
#define KVM_RUN                0xAE80
#define KVM_CAP_IMMEDIATE_EXIT 136

/* The head of struct kvm_run. */
struct run_head {
    unsigned char request_interrupt_window;
    unsigned char immediate_exit;
    unsigned char padding[6];
    unsigned int exit_reason;
};

/** Print the failure `detail` and return 1. */
static int fail(const char *detail)
{
    printf("KVMMUSL FAIL (%s: errno %d)\n", detail, errno);
    return 1;
}

/** The child's rows on the run page `run` it inherited, whose vCPU is on
 * `vcpu`: the status the child exits with. */
static int child_rows(volatile struct run_head *run, int vcpu)
{
    if (run->immediate_exit != 1) {
        printf("KVMMUSL FAIL (the child's mapping lost immediate_exit)\n");
        return 1;
    }
    if (ioctl(vcpu, KVM_RUN, 0) != -1 || errno != EIO) {
        printf("KVMMUSL FAIL (KVM_RUN from the child is not EIO: errno %d)\n", errno);
        return 1;
    }
    run->request_interrupt_window = 1;
    return 0;
}

/** Fork, run the child's rows, and check what the child wrote reached the
 * parent's mapping `run`: 0, or 1 after a failure. */
static int fork_rows(volatile struct run_head *run, int vcpu)
{
    pid_t pid;
    int status;

    pid = fork();
    if (pid < 0) {
        return fail("fork");
    }
    if (pid == 0) {
        _exit(child_rows(run, vcpu));
    }
    if (waitpid(pid, &status, 0) != pid || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        printf("KVMMUSL FAIL (the child failed)\n");
        return 1;
    }
    if (run->request_interrupt_window != 1) {
        printf("KVMMUSL FAIL (the child's write is not in the parent's mapping)\n");
        return 1;
    }
    if (ioctl(vcpu, KVM_RUN, 0) != -1 || errno != EINTR) {
        return fail("KVM_RUN after the fork");
    }
    return 0;
}

/** A private mapping of the vCPU `vcpu`, `size` bytes, beside the shared
 * `run`: a copy, whose writes stay its own. 0, or 1 after a failure. */
static int private_rows(volatile struct run_head *run, int vcpu, long size)
{
    volatile struct run_head *copy;

    copy = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_PRIVATE, vcpu, 0);
    if (copy == MAP_FAILED) {
        return fail("a private mapping");
    }
    if (copy->immediate_exit != 1 || copy->request_interrupt_window != 1) {
        printf("KVMMUSL FAIL (the private mapping is not a copy of the run page)\n");
        return 1;
    }
    copy->immediate_exit = 0;
    if (run->immediate_exit != 1) {
        printf("KVMMUSL FAIL (a private write reached the run page)\n");
        return 1;
    }
    munmap((void *)copy, (size_t)size);
    return 0;
}

/** Run the rows; 0 when they pass, 1 otherwise. */
int main(void)
{
    volatile struct run_head *run;
    int kvm, vm, vcpu;
    long size;

    kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm < 0) {
        return fail("open /dev/kvm");
    }
    if (ioctl(kvm, KVM_GET_API_VERSION, 0) != 12) {
        return fail("KVM_GET_API_VERSION is not 12");
    }
    vm = ioctl(kvm, KVM_CREATE_VM, 0);
    if (vm < 0) {
        return fail("KVM_CREATE_VM");
    }
    if (fcntl(vm, F_GETFD) != FD_CLOEXEC) {
        return fail("the VM's descriptor is not close-on-exec");
    }
    if (ioctl(vm, KVM_CHECK_EXTENSION, KVM_CAP_IMMEDIATE_EXIT) != 1) {
        return fail("KVM_CAP_IMMEDIATE_EXIT");
    }
    size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
    vcpu = ioctl(vm, KVM_CREATE_VCPU, 0);
    if (size < 4096 || vcpu < 0) {
        return fail("KVM_GET_VCPU_MMAP_SIZE or KVM_CREATE_VCPU");
    }
    run = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, vcpu, 0);
    if (run == MAP_FAILED) {
        return fail("mmap of the vCPU");
    }
    run->immediate_exit = 1;
    if (ioctl(vcpu, KVM_RUN, 0) != -1 || errno != EINTR) {
        return fail("KVM_RUN under immediate_exit is not EINTR");
    }
    if (fork_rows(run, vcpu) != 0 || private_rows(run, vcpu, size) != 0) {
        return 1;
    }
    close(vcpu);
    close(vm);
    close(kvm);
    if (run->immediate_exit != 1 || run->request_interrupt_window != 1) {
        printf("KVMMUSL FAIL (the mapping changed when the descriptors closed)\n");
        return 1;
    }
    munmap((void *)run, (size_t)size);
    printf("KVMMUSL OK\n");
    return 0;
}
