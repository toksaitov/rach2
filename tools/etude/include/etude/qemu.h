/*
 * qemu.h: the QEMU launcher. It makes QEMU's command line from a case's QEMU
 * profile and the site's accelerator, runs QEMU as an attempt of the host
 * executor, keeps its serial console, speaks QMP to it, and tells how the
 * machine ended. tools/etude/Readme.md describes the machine it makes.
 */
#ifndef ETUDE_QEMU_H
#define ETUDE_QEMU_H

#include <stddef.h>
#include <stdint.h>

#include "etude/contract.h"
#include "etude/exec.h"

/* The accelerators a site profile may choose; each is asked for alone. */
enum etude_accel {
    ETUDE_ACCEL_TCG,  /* QEMU's emulation of the processor */
    ETUDE_ACCEL_KVM,  /* Linux's virtualization */
    ETUDE_ACCEL_HVF,  /* macOS's Hypervisor.framework */
    ETUDE_ACCEL_WHPX, /* the Windows Hypervisor Platform */
    ETUDE_ACCEL_COUNT
};

/* How the machine starts the kernel, as the profile's `firmware` says. */
enum etude_firmware {
    ETUDE_FIRMWARE_DIRECT,  /* QEMU loads the kernel and the RAM image */
    ETUDE_FIRMWARE_SEABIOS, /* the BIOS boots from the first disk */
    ETUDE_FIRMWARE_OVMF     /* the UEFI firmware boots from the disks */
};

/* A case's QEMU profile with its artifacts resolved to paths. */
struct etude_qemu_profile {
    const char *architecture; /* "i686" or "x86_64" */
    const char *kernel;       /* loaded by direct firmware alone */
    const char *initrd;       /* NULL for none */
    const char *const *disks; /* private copies, ended by NULL */
    const char *cpu;          /* QEMU's processor model */
    unsigned processors;      /* 1 to 64 */
    uint64_t memory_bytes;    /* a whole number of mebibytes */
    enum etude_firmware firmware;
    const char *const *devices;      /* ended by NULL */
    const char *const *boot_options; /* the kernel's command line, ended
                                        by NULL; direct firmware alone */
};

/* What the site profile gives the launcher. */
struct etude_qemu_site {
    enum etude_accel accel;
    const char *qemu_i686;   /* the program for i686, qemu-system-i386 */
    const char *qemu_x86_64; /* and for x86-64 */
    const char *ovmf_i686;   /* the UEFI firmware of each, or NULL */
    const char *ovmf_x86_64;
};

/* The longest command line the launcher makes, in arguments. */
#define ETUDE_QEMU_ARGS_MAX 128

/** Give the name QEMU and QMP use for `accel`, or NULL for none. */
const char *etude_accel_name(enum etude_accel accel);

/**
 * Find the accelerator called `name`.
 *
 * @return 0 with it in `*accel`, or -1 for a name not in the list.
 */
int etude_accel_parse(const char *name, enum etude_accel *accel);

/* Where etude_qemu_argv puts a command line. */
struct etude_qemu_line {
    char **argv; /* room for `max` pointers, the last of them NULL */
    size_t max;
    char *buf; /* room for `size` bytes, which keep the strings */
    size_t size;
};

/**
 * Make QEMU's command line for profile `p` on site `s` in `out`, with the
 * console on the socket at `console` and QMP on the one at `qmp`, both of
 * which QEMU connects to as a client. The machine starts stopped, with `-S`,
 * and the launcher starts it once QMP has confirmed the accelerator. argv[0]
 * is the site's program for the architecture.
 *
 * @return 0, or an error number: EINVAL for a profile the launcher cannot
 *         make, ENOENT for a firmware the site lacks, E2BIG when `out` is too
 *         small.
 */
int etude_qemu_argv(const struct etude_qemu_profile *p, const struct etude_qemu_site *s,
                    const char *console, const char *qmp, const struct etude_qemu_line *out);

/**
 * Say whether QEMU's standard error `err` reports that it could not start
 * accelerator `accel`: a line that QEMU's location of the `-accel` option
 * begins, "PROGRAM: -accel NAME: ".
 */
int etude_qemu_accel_refused(const char *err, enum etude_accel accel);

/* The launcher: the machines it runs at once, each QEMU an attempt of the
 * launcher's own host executor. */
struct etude_qemu;

/* One QEMU machine. */
struct etude_machine;

/* One machine to start. */
struct etude_machine_spec {
    const char *attempt_id;
    const struct etude_qemu_profile *profile;
    const struct etude_qemu_site *site;
    char *const *envp;        /* QEMU's whole environment */
    const char *console_path; /* a new file that gets every console byte,
                                 or NULL */
    size_t console_max;       /* console bytes kept in memory */
    uint64_t grace_us;        /* from QMP's quit to a kill, and from the
                                 termination signal to SIGKILL */
    etude_register_fn reg;    /* as the executor's */
    void *reg_ctx;
};

/* The state of a machine. */
enum etude_machine_state {
    ETUDE_MACHINE_STARTING, /* QEMU runs, and QMP has not started the machine */
    ETUDE_MACHINE_RUNNING,  /* QMP confirmed the accelerator and started it */
    ETUDE_MACHINE_ENDED     /* QEMU has ended and its cleanup is done */
};

/* What the launcher observed of one machine. */
struct etude_machine_result {
    const char *attempt_id;
    enum etude_machine_state state;
    /* Once ended: SHUTDOWN, RESET, PANIC, QEMU_EXITED, ACCEL_FAILED, STOPPED
     * or LOST. */
    enum etude_end end;
    const char *accel;           /* the accelerator QMP reported in use, or NULL */
    const char *method;          /* how it was confirmed: "query-accelerators",
                                    "query-kvm", or NULL */
    char reason[64];             /* the reason of QEMU's SHUTDOWN event, or "" */
    int by_guest;                /* the event's `guest` */
    struct etude_output console; /* what the console wrote */
    int console_file_error;      /* the error that ended the writes to the
                                    console's file, or 0 */
    uint64_t end_console;        /* console bytes read when the end was seen */
    /* QEMU's process: its exit, its standard error, its cleanup and its
     * resources. */
    const struct etude_exec_result *qemu;
    unsigned qmp_bad;    /* QMP lines that were not JSON or too long */
    uint64_t started_us; /* monotonic times: QEMU was started, */
    uint64_t running_us; /* the machine was started, */
    uint64_t ended_us;   /* and the end was seen */
};

/** Make a launcher with no machines; NULL when memory ran out. */
struct etude_qemu *etude_qemu_new(void);

/** Stop every machine still running, wait for each to end, and free all. */
void etude_qemu_free(struct etude_qemu *q);

/**
 * Start QEMU for `spec`, stopped, with the launcher's private sockets for
 * its console and QMP.
 *
 * @return 0 with the machine in `*m`, or -1 with errno set: an error of the
 *         profile as etude_qemu_argv gives it, of the system, or of the
 *         executor's start; nothing of the machine is left.
 */
int etude_qemu_start(struct etude_qemu *q, const struct etude_machine_spec *spec,
                     struct etude_machine **m);

/**
 * Serve every machine until one ends, its console or state changes, or
 * `timeout_ms` passes; -1 sets no time.
 *
 * @return the machine that ended, each once, or NULL.
 */
struct etude_machine *etude_qemu_wait(struct etude_qemu *q, int timeout_ms);

/**
 * Write `n` bytes to the machine's console input, after what was written
 * before.
 *
 * @return 0, or -1 when the console is closed or memory ran out.
 */
int etude_machine_type(struct etude_machine *m, const char *text, size_t n);

/**
 * Stop the machine: QMP's quit when QMP is ready, and the executor's
 * cancellation of QEMU after the grace period, or at once without QMP. Its
 * end becomes ETUDE_END_STOPPED unless it had ended before.
 */
void etude_machine_cancel(struct etude_machine *m);

/** Give what was observed of the machine so far. */
const struct etude_machine_result *etude_machine_result(const struct etude_machine *m);

/** Free a machine that etude_qemu_wait has returned. */
void etude_machine_free(struct etude_machine *m);

#endif
