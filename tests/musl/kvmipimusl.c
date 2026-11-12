/**
 * kvmipimusl: wake a halted vCPU many thousands of times, by IPIs and by
 * INIT and SIPI, and stop at the first wake that never comes.
 *
 * The program makes a VM with two vCPUs in real mode and the local APICs in
 * the kernel, the I/O APIC and PIC in the caller as QEMU's
 * kernel-irqchip=split has them, or all in the kernel with the argument
 * `kernel`. Both vCPUs put their APICs in x2APIC mode. vCPU 1 then halts
 * with interrupts on, and each IPI vCPU 0 sends it runs a handler that
 * counts the IPI and ends it; vCPU 0 waits for the count before it sends
 * the next, as kvm-unit-tests' on_cpu waits. Then vCPU 0 sends INIT and
 * SIPI, each SIPI starting vCPU 1 at a page that counts it and halts with
 * interrupts off, as a firmware's processor does after its start, and
 * waits for the count each time. The main thread reads the two counts in
 * the guest's memory; when neither moves for the stall's seconds, a wake
 * was lost, and the program prints the round, each vCPU thread's state and
 * wchan, the VM's interrupt counts from its fdinfo, and each vCPU's APIC,
 * run state and events, then whether a kick moved a count. A vCPU that
 * ends early has its exit printed, an internal error's data with it.
 *
 * Arguments: [split|kernel] [IPIS [SIPIS [STALL [pin]]]], by default split,
 * 20000, 5000, 10 and the threads free to move; `pin` keeps vCPU n's thread
 * on processor n + 1. It needs VMX or SVM, as kvmmusl does. It prints
 * "KVMIPIMUSL OK" and exits 0, or "KVMIPIMUSL FAIL (detail)" and exits 1.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>

/* The ioctls of linux/kvm.h this uses; the port has no such header. */
#define KVM_CREATE_VM              0xAE01
#define KVM_GET_VCPU_MMAP_SIZE     0xAE04
#define KVM_GET_SUPPORTED_CPUID    0xC008AE05
#define KVM_CREATE_VCPU            0xAE41
#define KVM_SET_USER_MEMORY_REGION 0x4020AE46
#define KVM_SET_TSS_ADDR           0xAE47
#define KVM_CREATE_IRQCHIP         0xAE60
#define KVM_RUN                    0xAE80
#define KVM_GET_REGS               0x8090AE81
#define KVM_SET_REGS               0x4090AE82
#define KVM_GET_SREGS              0x8138AE83
#define KVM_SET_SREGS              0x4138AE84
#define KVM_SET_CPUID2             0x4008AE90
#define KVM_ENABLE_CAP             0x4068AEA3
#define KVM_GET_LAPIC              0x8400AE8E
#define KVM_GET_MP_STATE           0x8004AE98
#define KVM_GET_VCPU_EVENTS        0x8040AE9F
#define KVM_CAP_SPLIT_IRQCHIP      121
#define KVM_EXIT_IO                2

/* The guest's memory, and where its code and counts lie in it. */
#define MEM_SIZE   (1024ul * 1024ul)
#define BSP_CODE   0x1000u
#define AP_CODE    0x2000u
#define AP_HANDLER 0x2100u
#define SIPI_CODE  0x3000u /* SIPI vector 3 */
#define IPI_VECTOR 0x40u
#define N_IPIS     0x500u /* IPIs vCPU 1 has handled */
#define N_SIPIS    0x504u /* SIPIs vCPU 1 has started at */
#define AP_READY   0x508u /* vCPU 1 has its APIC on */
#define WANT_IPIS  0x510u /* rounds of IPIs */
#define WANT_SIPIS 0x514u /* rounds of INIT and SIPI */

/* The two writes of the x2APIC on: EXTD and EN in IA32_APIC_BASE, then the
 * APIC enabled in SPIV with the spurious vector 0xFF. */
#define X2APIC_ON                                                \
    0x66, 0xB9, 0x1B, 0x00, 0x00, 0x00,     /* mov ecx, 0x1B */  \
        0x0F, 0x32,                         /* rdmsr */          \
        0x66, 0x0D, 0x00, 0x0C, 0x00, 0x00, /* or eax, 0xC00 */  \
        0x0F, 0x30,                         /* wrmsr */          \
        0x66, 0xB9, 0x0F, 0x08, 0x00, 0x00, /* mov ecx, 0x80F */ \
        0x66, 0xB8, 0xFF, 0x01, 0x00, 0x00, /* mov eax, 0x1FF */ \
        0x66, 0x31, 0xD2,                   /* xor edx, edx */   \
        0x0F, 0x30                          /* wrmsr */

/* vCPU 0: wait for vCPU 1's APIC, send the IPIs one at a time, each after
 * the last was counted, then INIT and SIPI likewise, and end with an OUT
 * to port 0x80. */
static const unsigned char bsp_code[] = {
    X2APIC_ON, 0x66, 0x83, 0x3E, 0x08, 0x05, 0x00, /* 33: cmp dword [AP_READY], 0 */
    0x74,      0xF8,                               /* 39: je 33 */
    0x66,      0x8B, 0x36, 0x10, 0x05,             /* 41: mov esi, [WANT_IPIS] */
    0x66,      0x8B, 0x1E, 0x00, 0x05,             /* 46: mov ebx, [N_IPIS] */
    0x66,      0xB9, 0x30, 0x08, 0x00, 0x00,       /* 51: mov ecx, 0x830 (ICR) */
    0x66,      0xB8, 0x40, 0x40, 0x00, 0x00,       /* 57: mov eax, 0x4040: fixed, assert */
    0x66,      0xBA, 0x01, 0x00, 0x00, 0x00,       /* 63: mov edx, 1: APIC id 1 */
    0x0F,      0x30,                               /* 69: wrmsr */
    0x66,      0x39, 0x1E, 0x00, 0x05,             /* 71: cmp [N_IPIS], ebx */
    0x74,      0xF9,                               /* 76: je 71 */
    0x66,      0x4E,                               /* 78: dec esi */
    0x75,      0xDC,                               /* 80: jnz 46 */
    0x66,      0x8B, 0x36, 0x14, 0x05,             /* 82: mov esi, [WANT_SIPIS] */
    0x66,      0x8B, 0x1E, 0x04, 0x05,             /* 87: mov ebx, [N_SIPIS] */
    0x66,      0xB9, 0x30, 0x08, 0x00, 0x00,       /* 92: mov ecx, 0x830 */
    0x66,      0xB8, 0x00, 0x45, 0x00, 0x00,       /* 98: mov eax, 0x4500: INIT, assert */
    0x66,      0xBA, 0x01, 0x00, 0x00, 0x00,       /* 104: mov edx, 1 */
    0x0F,      0x30,                               /* 110: wrmsr */
    0x66,      0xB8, 0x03, 0x46, 0x00, 0x00,       /* 112: mov eax, 0x4603: SIPI, vector 3 */
    0x0F,      0x30,                               /* 118: wrmsr */
    0x66,      0x39, 0x1E, 0x04, 0x05,             /* 120: cmp [N_SIPIS], ebx */
    0x74,      0xF9,                               /* 125: je 120 */
    0x66,      0x4E,                               /* 127: dec esi */
    0x75,      0xD4,                               /* 129: jnz 87 */
    0xE6,      0x80,                               /* 131: out 0x80, al */
    0xF4,                                          /* 133: hlt */
    0xEB,      0xFD,                               /* 134: jmp 133 */
};

/* vCPU 1 at its first start: its APIC on, then halted with interrupts on. */
static const unsigned char ap_code[] = {
    X2APIC_ON, 0x66, 0xC7, 0x06, 0x08,
    0x05,      0x01, 0x00, 0x00, 0x00, /* 33: mov dword [AP_READY], 1 */
    0xFB,                              /* 42: sti */
    0xF4,                              /* 43: hlt */
    0xEB,      0xFD,                   /* 44: jmp 43 */
};

/* vCPU 1's handler of IPI_VECTOR: count the IPI and end it with an EOI. */
static const unsigned char ap_handler[] = {
    0x66, 0x50, 0x66, 0x51, 0x66, 0x52, /* push eax, ecx, edx */
    0xF0, 0x66, 0xFF, 0x06, 0x00, 0x05, /* lock inc dword [N_IPIS] */
    0x66, 0xB9, 0x0B, 0x08, 0x00, 0x00, /* mov ecx, 0x80B (EOI) */
    0x66, 0x31, 0xC0,                   /* xor eax, eax */
    0x66, 0x31, 0xD2,                   /* xor edx, edx */
    0x0F, 0x30,                         /* wrmsr */
    0x66, 0x5A, 0x66, 0x59, 0x66, 0x58, /* pop edx, ecx, eax */
    0xCF,                               /* iret */
};

/* vCPU 1 after each SIPI: count it and halt with interrupts off. */
static const unsigned char sipi_code[] = {
    0xF0, 0x66, 0xFF, 0x06, 0x04, 0x05, /* 0: lock inc dword [N_SIPIS] */
    0xFA,                               /* 6: cli */
    0xF4,                               /* 7: hlt */
    0xEB, 0xFD,                         /* 8: jmp 7 */
};

struct region {
    unsigned int slot;
    unsigned int flags;
    unsigned long long guest_phys_addr;
    unsigned long long memory_size;
    unsigned long long userspace_addr;
};

struct enable_cap {
    unsigned int cap;
    unsigned int flags;
    unsigned long long args[4];
    unsigned char pad[64];
};

struct cpuid_entry {
    unsigned int function, index, flags, eax, ebx, ecx, edx, padding[3];
};

#define CPUID_MAX 128

struct cpuid {
    unsigned int nent, padding;
    struct cpuid_entry e[CPUID_MAX];
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

/* A vCPU and its thread's end. */
struct vcpu {
    int fd;
    volatile struct run_head *run;
    pthread_t thread;
    volatile int tid;
    volatile int ended;     /* 1 at the OUT to port 0x80, 2 at another end */
    volatile int reason;    /* the exit reason or -errno of another end */
    volatile int want_dump; /* the main thread asks for a dump */
    int id;
};

static volatile unsigned int *counts; /* the guest's memory, as words */
static int pin;                       /* each vCPU's thread on processor id + 1 */

/** Print the failure `detail` and return 1. */
static int fail(const char *detail)
{
    printf("KVMIPIMUSL FAIL (%s: errno %d)\n", detail, errno);
    return 1;
}

/** Print what KVM holds of the vCPU `c`, from its own thread between two
 * KVM_RUNs: its APIC's IRR, ISR, TPR, PPR and SPIV, its mp_state, the head
 * of its events, and RIP and RFLAGS. */
static void dump(struct vcpu *c)
{
    static unsigned char apic[1024];
    unsigned char ev[64];
    unsigned long long regs[18];
    unsigned int mp = 99, i;

    memset(apic, 0, sizeof(apic));
    memset(ev, 0, sizeof(ev));
    memset(regs, 0, sizeof(regs));
    (void)ioctl(c->fd, KVM_GET_LAPIC, apic);
    (void)ioctl(c->fd, KVM_GET_MP_STATE, &mp);
    (void)ioctl(c->fd, KVM_GET_VCPU_EVENTS, ev);
    (void)ioctl(c->fd, KVM_GET_REGS, regs);
    printf("kvmipimusl: dump: vcpu %d: mp_state %u rip 0x%llx rflags 0x%llx rbx 0x%llx rsi 0x%llx "
           "tpr 0x%x ppr 0x%x spiv 0x%x\n",
           c->id, mp, regs[16], regs[17], regs[1], regs[4], *(unsigned int *)(apic + 0x80),
           *(unsigned int *)(apic + 0xA0), *(unsigned int *)(apic + 0xF0));
    printf("kvmipimusl: dump: irr");
    for (i = 0; i < 8; i++) {
        printf(" %08x", *(unsigned int *)(apic + 0x200 + i * 0x10));
    }
    printf(" isr");
    for (i = 0; i < 8; i++) {
        printf(" %08x", *(unsigned int *)(apic + 0x100 + i * 0x10));
    }
    printf("\nkvmipimusl: dump: events");
    for (i = 0; i < 20; i++) {
        printf(" %02x", ev[i]);
    }
    printf("\n");
}

/** A signal that only ends KVM_RUN. */
static void on_usr1(int sig)
{
    (void)sig;
}

/** Run the vCPU `arg` until its OUT to port 0x80 or another exit. */
static void *vcpu_main(void *arg)
{
    struct vcpu *c = (struct vcpu *)arg;

    c->tid = (int)syscall(SYS_gettid);
    if (pin) {
        cpu_set_t set;

        CPU_ZERO(&set);
        CPU_SET(c->id + 1, &set);
        if (sched_setaffinity(0, sizeof(set), &set) != 0) {
            printf("kvmipimusl: sched_setaffinity: errno %d\n", errno);
        }
    }
    for (;;) {
        if (ioctl(c->fd, KVM_RUN, 0) != 0) {
            if (errno == EINTR) {
                if (c->want_dump) {
                    dump(c);
                    c->want_dump = 0;
                }
                continue;
            }
            c->reason = -errno;
            c->ended = 2;
            return NULL;
        }
        if (c->run->exit_reason == KVM_EXIT_IO && c->run->io_port == 0x80) {
            c->ended = 1;
            return NULL;
        }
        c->reason = (int)c->run->exit_reason;
        c->ended = 2;
        return NULL;
    }
}

/** Give vCPU `c`, whose APIC id is `id`, the supported CPUID with its id in
 * leaf 1, and start it in real mode at `ip`, CS 0: 0, or 1 after a
 * failure. */
static int setup(int kvm, struct vcpu *c, unsigned int id, unsigned int ip)
{
    static struct cpuid cpuid;
    unsigned char sregs[312];
    unsigned long long regs[18];
    unsigned int i;

    memset(&cpuid, 0, sizeof(cpuid));
    cpuid.nent = CPUID_MAX;
    if (ioctl(kvm, KVM_GET_SUPPORTED_CPUID, &cpuid) != 0) {
        return fail("KVM_GET_SUPPORTED_CPUID");
    }
    for (i = 0; i < cpuid.nent; i++) {
        if (cpuid.e[i].function == 1) {
            cpuid.e[i].ebx = (cpuid.e[i].ebx & 0x00FFFFFFu) | (id << 24);
            cpuid.e[i].ecx |= 1u << 21; /* x2APIC */
        }
    }
    if (ioctl(c->fd, KVM_SET_CPUID2, &cpuid) != 0) {
        return fail("KVM_SET_CPUID2");
    }
    if (ioctl(c->fd, KVM_GET_SREGS, sregs) != 0) {
        return fail("KVM_GET_SREGS");
    }
    memset(sregs, 0, 8);      /* CS's base */
    memset(sregs + 12, 0, 2); /* CS's selector */
    if (ioctl(c->fd, KVM_SET_SREGS, sregs) != 0 || ioctl(c->fd, KVM_GET_REGS, regs) != 0) {
        return fail("KVM_SET_SREGS or KVM_GET_REGS");
    }
    regs[6] = 0x8000u - id * 0x1000u; /* rsp */
    regs[16] = ip;                    /* rip */
    regs[17] = 2;                     /* rflags */
    return ioctl(c->fd, KVM_SET_REGS, regs) != 0 ? fail("KVM_SET_REGS") : 0;
}

/** Print the state and wchan of thread `tid`, which runs vCPU `n`. */
static void show_thread(int n, int tid)
{
    char path[64], buf[256];
    FILE *f;
    size_t len;

    snprintf(path, sizeof(path), "/proc/self/task/%d/wchan", tid);
    f = fopen(path, "r");
    len = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    buf[len] = '\0';
    if (f) {
        fclose(f);
    }
    printf("kvmipimusl: vcpu %d: tid %d: wchan %s\n", n, tid, len ? buf : "?");
    snprintf(path, sizeof(path), "/proc/self/task/%d/stat", tid);
    f = fopen(path, "r");
    len = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    buf[len] = '\0';
    if (f) {
        fclose(f);
    }
    printf("kvmipimusl: vcpu %d: stat %s\n", n, len ? buf : "?");
}

/** Print the VM descriptor `vm`'s fdinfo, its interrupts' counts. */
static void show_fdinfo(int vm)
{
    char path[64], buf[4096];
    FILE *f;
    size_t len;

    snprintf(path, sizeof(path), "/proc/self/fdinfo/%d", vm);
    f = fopen(path, "r");
    len = f ? fread(buf, 1, sizeof(buf) - 1, f) : 0;
    buf[len] = '\0';
    if (f) {
        fclose(f);
    }
    printf("kvmipimusl: fdinfo:\n%s\n", buf);
}

/** Make the VM `*vm` of `kvm`, its controllers split or in the kernel by
 * `split`, its memory with the guest's code and the rounds `ipis` and
 * `sipis`, and its two vCPUs `c`: 0, or 1 after a failure. */
static int make_vm(int kvm, int split, unsigned int ipis, unsigned int sipis, int *vm,
                   struct vcpu *c)
{
    struct region r;
    struct enable_cap cap;
    unsigned short *ivt;
    unsigned char *mem;
    unsigned int i;
    long size;

    *vm = ioctl(kvm, KVM_CREATE_VM, 0);
    if (*vm < 0) {
        return fail("KVM_CREATE_VM");
    }
    (void)ioctl(*vm, KVM_SET_TSS_ADDR, 0xFFFBD000ul);
    memset(&cap, 0, sizeof(cap));
    cap.cap = KVM_CAP_SPLIT_IRQCHIP;
    cap.args[0] = 24;
    if (split ? ioctl(*vm, KVM_ENABLE_CAP, &cap) != 0 : ioctl(*vm, KVM_CREATE_IRQCHIP, 0) != 0) {
        return fail(split ? "KVM_CAP_SPLIT_IRQCHIP" : "KVM_CREATE_IRQCHIP");
    }
    mem = mmap(NULL, MEM_SIZE, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (mem == MAP_FAILED) {
        return fail("mmap of the guest's memory");
    }
    memcpy(mem + BSP_CODE, bsp_code, sizeof(bsp_code));
    memcpy(mem + AP_CODE, ap_code, sizeof(ap_code));
    memcpy(mem + AP_HANDLER, ap_handler, sizeof(ap_handler));
    memcpy(mem + SIPI_CODE, sipi_code, sizeof(sipi_code));
    ivt = (unsigned short *)mem;
    ivt[IPI_VECTOR * 2u] = AP_HANDLER;
    ivt[IPI_VECTOR * 2u + 1u] = 0;
    counts = (volatile unsigned int *)mem;
    counts[WANT_IPIS / 4u] = ipis;
    counts[WANT_SIPIS / 4u] = sipis;
    memset(&r, 0, sizeof(r));
    r.memory_size = MEM_SIZE;
    r.userspace_addr = (unsigned long long)(unsigned long)mem;
    if (ioctl(*vm, KVM_SET_USER_MEMORY_REGION, &r) != 0) {
        return fail("KVM_SET_USER_MEMORY_REGION");
    }
    size = ioctl(kvm, KVM_GET_VCPU_MMAP_SIZE, 0);
    for (i = 0; i < 2; i++) {
        c[i].id = (int)i;
        c[i].fd = ioctl(*vm, KVM_CREATE_VCPU, (unsigned long)i);
        if (c[i].fd < 0 || size <= 0) {
            return fail("KVM_CREATE_VCPU");
        }
        c[i].run = mmap(NULL, (size_t)size, PROT_READ | PROT_WRITE, MAP_SHARED, c[i].fd, 0);
        if (c[i].run == MAP_FAILED) {
            return fail("the run page");
        }
        if (setup(kvm, &c[i], i, i == 0 ? BSP_CODE : AP_CODE) != 0) {
            return 1;
        }
    }
    return 0;
}

/** Whether a count has moved since `last_i` and `last_s`. */
static int moved(unsigned int last_i, unsigned int last_s)
{
    return counts[N_IPIS / 4u] != last_i || counts[N_SIPIS / 4u] != last_s;
}

/** Report a stall at the counts `last_i` and `last_s`: the threads of `c`,
 * the fdinfo of `vm`, each vCPU's state from its own thread after a kick,
 * and whether a count moved after the kicks, waiting up to `naps` naps for
 * each. */
static void report_stall(struct vcpu *c, int vm, unsigned int last_i, unsigned int last_s,
                         unsigned int naps)
{
    struct timespec nap = {0, 50000000L};
    unsigned int i, n;

    show_thread(0, c[0].tid);
    show_thread(1, c[1].tid);
    show_fdinfo(vm);
    /* Kick each vCPU out of its wait: its thread prints its state and
     * enters again; a count that then moves means the wait's condition
     * held and only its wake was lost. */
    for (n = 0; n < 2; n++) {
        c[n].want_dump = 1;
        pthread_kill(c[n].thread, SIGUSR1);
        for (i = 0; i < naps && c[n].want_dump; i++) {
            nanosleep(&nap, NULL); /* interval: the guest's counts are looked at every 50 ms */
        }
    }
    for (i = 0; i < naps && !moved(last_i, last_s); i++) {
        nanosleep(&nap, NULL); /* interval: the guest's counts are looked at every 50 ms */
    }
    printf("kvmipimusl: after the kick: %s (ipis %u, sipis %u)\n",
           moved(last_i, last_s) ? "moved" : "still stuck", counts[N_IPIS / 4u],
           counts[N_SIPIS / 4u]);
}

/** Report the vCPUs `c` that ended before vCPU 0's OUT, with the exit of
 * each and, for KVM_EXIT_INTERNAL_ERROR, its suberror, ndata and data. */
static void report_end(const struct vcpu *c)
{
    unsigned int i;

    printf("KVMIPIMUSL FAIL (a vCPU ended: vcpu 0 %d reason %d, vcpu 1 %d reason %d: ipis %u, "
           "sipis %u)\n",
           c[0].ended, c[0].reason, c[1].ended, c[1].reason, counts[N_IPIS / 4u],
           counts[N_SIPIS / 4u]);
    for (i = 0; i < 2; i++) {
        const volatile unsigned int *u = (const volatile unsigned int *)c[i].run;
        const volatile unsigned long long *d = (const volatile unsigned long long *)c[i].run;

        printf("kvmipimusl: vcpu %u: exit %u: suberror %u ndata %u: data 0x%llx 0x%llx 0x%llx "
               "0x%llx 0x%llx 0x%llx\n",
               i, u[2], u[8], u[9], d[5], d[6], d[7], d[8], d[9], d[10]);
    }
}

int main(int argc, char **argv)
{
    int split = argc < 2 || strcmp(argv[1], "kernel") != 0;
    unsigned int ipis = argc > 2 ? (unsigned int)strtoul(argv[2], NULL, 0) : 20000u;
    unsigned int sipis = argc > 3 ? (unsigned int)strtoul(argv[3], NULL, 0) : 5000u;
    unsigned int stall = argc > 4 ? (unsigned int)strtoul(argv[4], NULL, 0) : 10u;
    struct timespec nap = {0, 50000000L}, t0, t1;
    struct vcpu c[2];
    unsigned int last_i = 0, last_s = 0, still = 0, i;
    int kvm, vm;

    pin = argc > 5 && strcmp(argv[5], "pin") == 0;
    if (ipis == 0 || sipis == 0) {
        printf("KVMIPIMUSL FAIL (IPIS and SIPIS must be above 0)\n");
        return 1;
    }
    kvm = open("/dev/kvm", O_RDWR | O_CLOEXEC);
    if (kvm < 0) {
        return fail("/dev/kvm");
    }
    memset(c, 0, sizeof(c));
    if (make_vm(kvm, split, ipis, sipis, &vm, c) != 0) {
        return 1;
    }
    signal(SIGUSR1, on_usr1);
    clock_gettime(CLOCK_MONOTONIC, &t0);
    for (i = 0; i < 2; i++) {
        if (pthread_create(&c[i].thread, NULL, vcpu_main, &c[i]) != 0) {
            return fail("pthread_create");
        }
    }
    while (!c[0].ended && !c[1].ended) {
        nanosleep(&nap, NULL); /* interval: the guest's counts are looked at every 50 ms */
        if (moved(last_i, last_s)) {
            last_i = counts[N_IPIS / 4u];
            last_s = counts[N_SIPIS / 4u];
            still = 0;
            continue;
        }
        if (++still < stall * 20u) {
            continue;
        }
        printf("KVMIPIMUSL FAIL (a lost wake: no count moved for %u s: ipis %u of %u, sipis %u "
               "of %u, ready %u, %s irqchip)\n",
               stall, last_i, ipis, last_s, sipis, counts[AP_READY / 4u],
               split ? "split" : "kernel");
        report_stall(c, vm, last_i, last_s, stall * 20u);
        return 1;
    }
    clock_gettime(CLOCK_MONOTONIC, &t1);
    if (c[0].ended != 1 || c[1].ended) {
        report_end(c);
        return 1;
    }
    printf("KVMIPIMUSL OK (%s irqchip: %u ipis, %u sipis in %ld ms)\n", split ? "split" : "kernel",
           counts[N_IPIS / 4u], counts[N_SIPIS / 4u],
           (long)(t1.tv_sec - t0.tv_sec) * 1000L + (t1.tv_nsec - t0.tv_nsec) / 1000000L);
    return 0;
}
