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

/* A device, optionally with its user network and station address. */
struct etude_qemu_device {
    const char *model; /* NULL ends the device list */
    const char *net;   /* IPv4 network/prefix, or NULL for 10.0.2.0/24 */
    const char *host;  /* NULL leaves QEMU's default for this network */
    const char *dhcpstart;
    const char *dns;
    const char *mac; /* six hexadecimal octets, or NULL for QEMU's default */
};

/**
 * Check device models and network options, including IPv4 subnet membership,
 * unicast station addresses and duplicate explicit or default addresses.
 * Network options are valid only for rtl8139 and e1000.
 *
 * @return 0, or EINVAL for an invalid device list; NULL is an empty list.
 */
int etude_qemu_devices_check(const struct etude_qemu_device *devices);

/* The controller a disk of a profile is attached to. */
enum etude_disk_bus {
    ETUDE_BUS_IDE, /* one of the four places of the PC's IDE controllers */
    ETUDE_BUS_NVME /* an NVMe controller of its own */
};

/* A disk of a profile, its file resolved to a path. */
struct etude_qemu_disk {
    const char *path; /* the file QEMU opens; NULL ends a list */
    enum etude_disk_bus bus;
    int cdrom;          /* a CD drive with the file as its read-only medium, on IDE */
    const char *serial; /* the drive's serial number, or NULL; an NVMe disk has one */
    int discard;        /* the guest's discards, and its writes of zeros, free the
                           file's blocks */
};

/* The most disks of a profile, of which four at most are on IDE. */
#define ETUDE_QEMU_DISKS 8

/* The longest serial number, the size of NVMe's and ATA's serial fields. */
#define ETUDE_QEMU_SERIAL_MAX 20

/* A case's QEMU profile with its artifacts resolved to paths. */
struct etude_qemu_profile {
    const char *architecture;            /* "i686" or "x86_64" */
    const char *kernel;                  /* loaded by direct firmware alone */
    const char *initrd;                  /* NULL for none */
    const struct etude_qemu_disk *disks; /* in the order of the profile, ended by
                                            one whose path is NULL; NULL for none */
    const char *cpu;                     /* the processor profile (processor.h) */
    unsigned processors;                 /* 1 to 64 */
    uint64_t memory_bytes;               /* a whole number of mebibytes */
    const char *memory_profile;          /* NULL or "reserved", or "bigmem-boundary-v1" */
    const char *audio;                   /* NULL, "hda" or "sb16", recorded
                                            through the WAV backend */
    enum etude_firmware firmware;
    const struct etude_qemu_device *devices; /* ended by a NULL model */
    const char *const *boot_options;         /* the kernel's command line, ended
                                                by NULL; direct firmware alone */
};

/* Admission amounts for one QEMU, separate from its configured RAM. */
struct etude_qemu_memory {
    uint64_t configured_bytes;
    uint64_t resident_bytes; /* guest pages expected to be touched */
    uint64_t overhead_bytes; /* QEMU, firmware and the controller */
    uint64_t backing_bytes;  /* disk backing; zero for anonymous RAM */
};

/* The overhead an ordinary machine reserves for QEMU and its controller. */
#define ETUDE_QEMU_OVERHEAD (UINT64_C(128) << 20)

#define ETUDE_BIGMEM_CONFIGURED (UINT64_C(72) << 30)
#define ETUDE_BIGMEM_RESIDENT   (UINT64_C(2) << 30)
#define ETUDE_BIGMEM_OVERHEAD   (UINT64_C(2) << 30)

/**
 * Resolve the memory reservation of profile `p` into `out`. Ordinary guests
 * reserve all configured RAM and 128 MiB of overhead. bigmem-boundary-v1
 * admits only the x86-64 rescue boot at 72 GiB, with one processor, no
 * devices or disks, and the max-nopdpe1gb processor profile. Its only guest
 * command is `exec /memprobe total`.
 *
 * @return 0, or EINVAL for another profile or an unsafe boundary profile,
 *         EOVERFLOW when the reservation cannot be represented.
 */
int etude_qemu_memory(const struct etude_qemu_profile *p, struct etude_qemu_memory *out);

/* What the site profile gives the launcher. */
struct etude_qemu_site {
    enum etude_accel accel;
    const char *qemu_i686;   /* the program for i686, qemu-system-i386 */
    const char *qemu_x86_64; /* and for x86-64 */
    const char *ovmf_i686;   /* the code image of each one's OVMF, or NULL */
    const char *ovmf_x86_64;
    int boundary_lazy; /* the site permits the proved lazy boundary profile */
};

/* The longest command line the launcher makes, in arguments. */
#define ETUDE_QEMU_ARGS_MAX 128

/**
 * Check the disks of a profile, `disks` as the profile gives them, against
 * what the launcher attaches: at most ETUDE_QEMU_DISKS, of which at most four
 * are on IDE, CDs among them; a path for each; an NVMe disk with a serial; a
 * serial that is a word of at most ETUDE_QEMU_SERIAL_MAX printable characters
 * without a comma, on NVMe alone; and a CD on IDE, without `discard`.
 *
 * @return 0, or EINVAL.
 */
int etude_qemu_disks_check(const struct etude_qemu_disk *disks);

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
 * is the site's program for the architecture. `audio` is the launcher-made
 * WAV path, required when the profile selects an audio card.
 *
 * @return 0, or an error number: EINVAL for a profile the launcher cannot
 *         make, ENOENT for a firmware the site lacks, E2BIG when `out` is too
 *         small; ENOTSUP for a lazy boundary profile the site has not enabled.
 */
int etude_qemu_argv(const struct etude_qemu_profile *p, const struct etude_qemu_site *s,
                    const char *console, const char *qmp, const char *audio,
                    const struct etude_qemu_line *out);

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
    char *const *envp;              /* QEMU's whole environment */
    const char *console_path;       /* a new file that gets every console byte,
                                       or NULL */
    const char *audio_path;         /* new WAV file, or NULL for a private temporary file;
                                       used only when the profile selects audio */
    size_t console_max;             /* console bytes kept in memory */
    uint64_t grace_us;              /* from QMP's quit to a kill, and from the
                                       termination signal to SIGKILL */
    uint64_t memory_reserved_bytes; /* admitted memory, including overhead;
                                      required for the boundary profile */
    etude_register_fn reg;          /* as the executor's */
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
    struct etude_output console; /* what the console wrote; `closed` once QEMU
                                    closed it */
    const char *audio_path;      /* WAV path, or NULL; complete after QEMU ends;
                                    owned until machine_free, which removes a
                                    temporary file but leaves a caller-given file */
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
 *         executor's start; ENOMEM for a boundary boot without its whole
 *         reservation, ENOTSUP when macOS core files are enabled, EFBIG for a
 *         kernel over 64 MiB or RAM image over
 *         256 MiB. Nothing of the machine is left.
 */
int etude_qemu_start(struct etude_qemu *q, const struct etude_machine_spec *spec,
                     struct etude_machine **m);

/**
 * Serve every machine until one ends, its console or state changes, an
 * attempt of the launcher's executor writes output, or `timeout_ms` passes;
 * -1 sets no time.
 *
 * @return the machine that ended, each once, or NULL.
 */
struct etude_machine *etude_qemu_wait(struct etude_qemu *q, int timeout_ms);

/**
 * Wait as etude_qemu_wait does, and also for one of the `n` descriptors of
 * `w` to be ready, which sets its `revents`, and, when `host` is not NULL,
 * for a host attempt of the caller's on the launcher's executor to finish,
 * which is put in `*host`. Since the output of a host attempt ends the wait
 * too, a caller that looks for a line in it reads it as it comes. Each call
 * serves the machines at least once; with no machine running and no
 * descriptor it returns at once.
 *
 * @return the machine that ended, each once, or NULL.
 */
struct etude_machine *etude_qemu_watch(struct etude_qemu *q, int timeout_ms, struct etude_watch *w,
                                       size_t n, struct etude_proc **host);

/**
 * Give the launcher's executor, on which the caller may start host attempts
 * of its own beside the machines. etude_qemu_watch serves them and gives
 * each once it has finished; the caller frees it.
 */
struct etude_exec *etude_qemu_exec(struct etude_qemu *q);

/**
 * Write `n` bytes to the machine's console input, after what was written
 * before. A boundary boot accepts `exec /memprobe total\r` once and
 * `poweroff\r`; another command returns -1 with errno EPERM.
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

/* The first id of the caller's QMP commands; the launcher's own are below. */
#define ETUDE_QMP_FIRST_ID 100

/**
 * Send QMP command `execute` of the caller's, with `arguments`, the text of
 * a JSON object on one line, or NULL. Its reply comes later, by
 * etude_machine_reply. A boundary boot takes only `qmp_capabilities`,
 * `quit`, the `query-` commands and `human-monitor-command` with an `info`
 * command line, and only with arguments that parse as one object.
 *
 * @return the command's id, from ETUDE_QMP_FIRST_ID up; or -1 with errno
 *         EINVAL for a name or arguments QMP cannot take, EPERM for a
 *         command a boundary boot does not take, EAGAIN before QMP has
 *         started the machine or once it has asked QEMU to quit, EPIPE when
 *         QMP is gone, or ENOMEM.
 */
long etude_machine_qmp(struct etude_machine *m, const char *execute, const char *arguments);

/**
 * Give the reply to the caller's QMP command `id`: QMP's whole line, with its
 * `return` or its `error`.
 *
 * @return 1 with the line in `*reply`, 0 while it has not come, or -1 when
 *         it never will: QMP or the machine is gone, or `id` was not sent.
 */
int etude_machine_reply(const struct etude_machine *m, long id, const char **reply);

/** Give what was observed of the machine so far. */
const struct etude_machine_result *etude_machine_result(const struct etude_machine *m);

/** Free a machine that etude_qemu_wait has returned. */
void etude_machine_free(struct etude_machine *m);

#endif
