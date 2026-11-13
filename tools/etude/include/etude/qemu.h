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

/* A forward of one of the case's local ports to a port of the guest: QEMU
 * listens on 127.0.0.1 at the local port and joins each connection, or each
 * datagram, to the port at the first address that DHCP gives on the card's
 * user network, 10.0.2.15 unless the card says otherwise. */
struct etude_qemu_forward {
    int udp;             /* a UDP port; a TCP port when 0 */
    unsigned port;       /* which of the profile's `ports`, counted from 0 */
    unsigned guest_port; /* 1 to 65535 */
};

/* A forward of the guest's connections to a TCP port of an address of the
 * card's user network to one of the case's local ports: QEMU runs `relay` for
 * each connection, with the local port as its last argument and the
 * connection as its standard input, output and error. */
struct etude_qemu_guest_forward {
    const char *address;      /* dotted decimal, which etude_qemu_guest_address
                                 takes */
    unsigned guest_port;      /* 1 to 65535 */
    unsigned port;            /* which of the profile's `ports`, counted
                                 from 0 */
    const char *const *relay; /* the program and its first arguments, ended
                                 by NULL */
};

/*
 * A device of a profile: `-device MODEL` with the members given. The network
 * members are a card's alone, for rtl8139, e1000 and virtio-net-pci,
 * and NULL leaves QEMU's
 * default. A card is on a user network of its own, or with `hub` on a hub of
 * its own, where nothing answers it.
 */
struct etude_qemu_device {
    const char *model;             /* QEMU's name of it; NULL ends a list */
    const char *id;                /* its identity, or NULL */
    const char *bus;               /* `ID.N`, or ID of an earlier PCIe root
                                      port; NULL for the default bus */
    const char *port;              /* a USB port, such as `1.2`, or NULL */
    const char *drive;             /* the identity of a disk, or NULL */
    const char *const *properties; /* names and values in turn, ended by
                                      NULL; NULL for none */
    const char *net;               /* IPv4 network/prefix, as 10.0.2.0/24 */
    const char *host;
    const char *dhcpstart;
    const char *dns;
    const char *mac;                           /* six hexadecimal octets */
    const struct etude_qemu_forward *forwards; /* `forward_count` of them */
    size_t forward_count;
    int restricted; /* the guest reaches only the guest forwards */
    /* `guest_forward_count` guest forwards */
    const struct etude_qemu_guest_forward *guest_forwards;
    size_t guest_forward_count;
    int hub; /* a hub port of its own in place of the user network, with no
                network options, restriction or forwards */
};

/**
 * Check device models and network options, including IPv4 subnet membership,
 * unicast station addresses and duplicate explicit or default addresses.
 * Network options, the restriction, forwards, guest forwards and `hub` are
 * valid only for rtl8139, e1000 and virtio-net-pci, and a card with `hub`
 * has no network
 * option but its station address, no restriction and no forwards.
 *
 * @return 0, or EINVAL for an invalid device list; NULL is an empty list.
 */
int etude_qemu_devices_check(const struct etude_qemu_device *devices);

/**
 * Say whether `address` can be the address of a guest forward on the user
 * network of card `d`: a dotted decimal address of the network that is not
 * the network's own, its broadcast address, the host's, its name server's or
 * one of the sixteen that DHCP gives from `dhcpstart`, each as the card gives
 * it or as QEMU's default for the network. A card on a hub, and one whose
 * network options etude_qemu_devices_check refuses, has no such address.
 */
int etude_qemu_guest_address(const struct etude_qemu_device *d, const char *address);

/* The controller a disk of a profile is attached to. */
enum etude_disk_bus {
    ETUDE_BUS_IDE,   /* one of the four places of the PC's IDE controllers */
    ETUDE_BUS_NVME,  /* an NVMe controller of its own */
    ETUDE_BUS_AHCI,  /* one of the six ports of the profile's one ich9-ahci */
    ETUDE_BUS_DEVICE /* the device whose `drive` is the disk's `id` */
};

/* A disk of a profile, its file resolved to a path. */
struct etude_qemu_disk {
    const char *path; /* the file QEMU opens; NULL ends a list */
    enum etude_disk_bus bus;
    int cdrom;          /* a CD drive with the file as its read-only medium, on IDE */
    const char *serial; /* the drive's serial number, or NULL; an NVMe disk has one */
    int discard;        /* the guest's discards, and its writes of zeros, free the
                           file's blocks */
    const char *id;     /* the identity a device's `drive` names, on
                           ETUDE_BUS_DEVICE alone */
    int boot;           /* the firmware boots this disk first; one disk at most,
                           and none on ETUDE_BUS_DEVICE */
    int qcow2;          /* the file is a qcow2 overlay over a raw base, not raw */
    int unsafe;         /* the guest's flushes are not passed to the file, QEMU's
                           cache=unsafe */
};

/* The most disks of a profile, of which four at most are on IDE and six on
 * AHCI. */
#define ETUDE_QEMU_DISKS 8

/* The longest serial number, the size of NVMe's and ATA's serial fields. */
#define ETUDE_QEMU_SERIAL_MAX 20

/* A network card whose frames the machine records, and the file they go to. */
struct etude_qemu_capture {
    unsigned card;    /* the card's network, netN: the cards of `devices`
                         counted in order from 0 */
    const char *path; /* the file QEMU writes the frames to in pcap format;
                         NULL ends a list */
};

/* A case's QEMU profile with its artifacts resolved to paths. */
struct etude_qemu_profile {
    const char *architecture;            /* the kernel: "i686" or "x86_64" */
    const char *kernel;                  /* loaded by direct firmware alone */
    const char *initrd;                  /* NULL for none */
    const struct etude_qemu_disk *disks; /* in the order of the profile, ended by
                                            one whose path is NULL; NULL for none */
    const char *cpu;                     /* the processor profile (processor.h) */
    unsigned processors;                 /* 1 to 64 */
    uint64_t memory_bytes;               /* a whole number of mebibytes */
    const char *memory_profile;          /* NULL or "reserved", or "bigmem-boundary-v1" */
    const char *audio;                   /* NULL, "hda" or "sb16", on the
                                            backend `audio_backend` names */
    enum etude_firmware firmware;
    const struct etude_qemu_device *devices; /* ended by a NULL model */
    const char *const *boot_options;         /* the kernel's command line, ended
                                                by NULL; direct firmware alone */
    const unsigned *ports;                   /* the case's local ports, each
                                                above 1023, `port_count` of them */
    size_t port_count;
    int no_i8042;                              /* the machine has no 8042 */
    int q35;                                   /* the machine is q35, whose AHCI
                                                  at 0:31.2 has the disks on IDE */
    const struct etude_qemu_capture *captures; /* NULL for none; each card
                                                  at most once */
    const char *smbios_vendor;                 /* the BIOS vendor the SMBIOS
                                                  tables name, printable ASCII,
                                                  or NULL for the firmware's own */
    const char *acpi_table;                    /* a file of an ACPI table, such
                                                  as an SSDT, that the firmware
                                                  adds to QEMU's; NULL for none */
    const char *audio_backend;                 /* NULL or "wav", which records the
                                                  card's output to the launcher's
                                                  WAV; "none", which discards it
                                                  and gives silence in; "sdl",
                                                  SDL's disk driver, which gives
                                                  `audio_input` in */
    const char *audio_input;                   /* with "sdl" alone, a file of raw
                                                  PCM, S16LE, two channels at
                                                  48000 Hz; NULL otherwise */
    const char *display;                       /* NULL, or the display profile:
                                                  "none", "std-vga",
                                                  "virtio-vga", "virtio-gpu",
                                                  "virgl" or "venus"; a profile
                                                  with one is a graphics boot,
                                                  which takes the host's
                                                  processor under KVM */
};

/* The longest SMBIOS vendor a profile may name. */
#define ETUDE_SMBIOS_VENDOR_MAX 64

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
 * devices or disks, the default PC with its 8042, and the max-nopdpe1gb
 * processor profile. Its only guest command is `exec /memprobe total`.
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
    const char *ovmf_i686;   /* reserved; OVMF boots both kernels on x86-64 */
    const char *ovmf_x86_64; /* the x86-64 OVMF code image, or NULL */
    int boundary_lazy;       /* the site permits the proved lazy boundary profile */
};

/* The longest command line the launcher makes, in arguments. */
#define ETUDE_QEMU_ARGS_MAX 224

/**
 * Check the disks of a profile, `disks` as the profile gives them, against
 * what the launcher attaches: at most ETUDE_QEMU_DISKS, of which at most four
 * are on IDE, CDs among them, and six on AHCI; a path for each; an NVMe disk
 * with a serial; a serial that is a word of at most ETUDE_QEMU_SERIAL_MAX
 * printable characters without a comma, on NVMe and IDE alone; a CD on IDE,
 * of a raw medium, without a serial or `discard`; an identity of QEMU's, given
 * once, for each disk on ETUDE_BUS_DEVICE alone; and at most one disk that
 * boots first, on another bus.
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
 * is the site's program for the kernel architecture, except that OVMF uses
 * qemu_x86_64 and ovmf_x86_64 for either kernel. `audio` is the launcher-made
 * WAV path, required when the profile selects an audio card on the `wav`
 * backend and unused on the others. Each capture of
 * the profile is a `filter-dump` on its card's network, which QEMU creates or
 * truncates before the machine starts and to which it writes each frame the
 * card sends or receives as it passes.
 *
 * @return 0, or an error number: EINVAL for a profile the launcher cannot
 *         make, among them an audio backend that is not `wav`, `none` or
 *         `sdl`, an input file without `sdl` or `sdl` without one, a forward
 *         of a port that is not one of the
 *         profile's `ports`, a local port forwarded twice by one protocol or
 *         both to the guest and from it, a guest forward that QEMU could not
 *         make, a capture of a card the profile lacks and an ACPI table
 *         whose path keeps a colon, and a display that is not one of
 *         `display`'s or that has another display device, a USB tablet or no
 *         8042 beside it; ENOENT for a
 *         firmware the site lacks, E2BIG when `out` is too small; ENOTSUP for
 *         a lazy boundary profile the site has not enabled, and on Windows
 *         for the `virgl` and `venus` displays.
 */
int etude_qemu_argv(const struct etude_qemu_profile *p, const struct etude_qemu_site *s,
                    const char *console, const char *qmp, const char *audio,
                    const struct etude_qemu_line *out);

/**
 * Make QEMU's environment for profile `p` in `out`: the caller's `envp`
 * without SDL's audio variables, those whose names begin SDL_AUDIO or
 * SDL_DISKAUDIO, and under the `sdl` backend SDL_AUDIODRIVER=disk,
 * SDL_DISKAUDIOFILEIN set to `input` and SDL_DISKAUDIOFILE to the null
 * device, so that SDL's disk driver reads the profile's input file as the
 * card's input and writes its output nowhere. Under the `virgl` display it
 * sets GALLIUM_DRIVER=d3d12 and MESA_D3D12_DEFAULT_ADAPTER_NAME=NVIDIA in
 * place of the caller's, which put QEMU's GL on the testing PC's GPU; under
 * `venus`, GALLIUM_DRIVER=llvmpipe, VK_DRIVER_FILES naming lavapipe and
 * LD_PRELOAD naming libm, for Venus over the host's lavapipe.
 * `input` is the profile's `audio_input` as the QEMU that runs names it.
 *
 * @return 0, or an error number: EINVAL for audio etude_qemu_argv refuses
 *         or an `sdl` profile without `input`, E2BIG when `out` is too small.
 */
int etude_qemu_env(const struct etude_qemu_profile *p, char *const *envp, const char *input,
                   const struct etude_qemu_line *out);

/**
 * Say whether device `model` is one that a profile with a display may not
 * have beside it: another display device, or the USB tablet, since a
 * display's pointer is the PS/2 mouse.
 */
int etude_qemu_display_device(const char *model);

/**
 * Say whether device `model` of a profile is a network card, which the
 * launcher numbers among the cards, from net0, and may record.
 */
int etude_qemu_nic(const char *model);

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
                                       used only when the profile selects audio on
                                       the `wav` backend */
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
    ETUDE_MACHINE_ENDED     /* executor ended; inspect qemu->cleanup separately */
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
    char host_full[64];          /* the reason of the BLOCK_IO_ERROR at which
                                    QEMU stopped the guest for want of space
                                    on the host, or ""; the machine then
                                    ends lost */
    int by_guest;                /* the event's `guest` */
    struct etude_output console; /* what the console wrote; `closed` once QEMU
                                    closed it */
    const char *audio_path;      /* WAV path, or NULL without the `wav` backend;
                                    complete after QEMU ends;
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
 * descriptor and with `host` NULL it returns at once.
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
 * cancellation of QEMU after the grace period, or at once without QMP. A
 * suspended QEMU is resumed first, since a stopped one cannot answer the
 * quit. Its end becomes ETUDE_END_STOPPED unless it had ended before. A
 * machine whose QMP has reported SHUTDOWN keeps that end, and its QEMU, if
 * it has not exited after the grace period, is cancelled.
 */
void etude_machine_cancel(struct etude_machine *m);

/**
 * Suspend the machine's QEMU process by etude_exec_suspend: SIGSTOP for its
 * process group on POSIX systems, native process suspension of its job on
 * Windows. The `suspension` of its result, `qemu`, becomes
 * ETUDE_SUSPEND_STOPPED once every thread of QEMU has stopped. No processor
 * of the guest runs, no timer of QEMU's fires and neither the console nor
 * QMP answers until etude_machine_resume; QMP's `stop`, which pauses the
 * processors while QEMU runs on, is not the same.
 *
 * @return 0, or -1 with errno EAGAIN before QMP has started the machine,
 *         ESRCH once it has ended or is being cancelled, or the errno of
 *         etude_exec_suspend.
 */
int etude_machine_suspend(struct etude_machine *m);

/**
 * Resume a suspended machine by etude_exec_resume: SIGCONT for its process
 * group on POSIX systems, native process resumption on Windows. A machine
 * not suspended is left as it is.
 *
 * @return 0, or -1 with the errno of etude_exec_resume.
 */
int etude_machine_resume(struct etude_machine *m);

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
