/*
 * tooling.h: the local Linux tooling machine. It is made once from a pinned
 * Ubuntu cloud image and the recipe in tools/etude/tooling, and runs a job,
 * a shell script with its files, in a private copy of itself under QEMU, with
 * the host's disk images attached, for a host that lacks a command the job
 * needs. Each boot's time, processor time and memory are measured.
 * tools/etude/Readme.md describes the machine and the job volume.
 */
#ifndef ETUDE_TOOLING_H
#define ETUDE_TOOLING_H

#include <stddef.h>
#include <stdint.h>

#include "etude/exec.h"
#include "etude/history.h"

/* The files of the recipe, in the order the machine's key digests them. */
#define ETUDE_TOOLING_RECIPE                                        \
    {"image",     "user-data",         "meta-data", "provision.sh", \
     "runner.sh", "etude-job.service", "tools",     NULL}

/* Where the machine is made and run. Programs are found on the PATH of
 * `envp`: qemu-system-x86_64, qemu-img, curl, and mtools' mformat, mmd and
 * mcopy. */
struct etude_tooling_site {
    const char *recipe; /* the recipe's directory */
    const char *cache;  /* the image, the made machine and the jobs' copies */
    const char *accel;  /* QEMU's accelerator: kvm, hvf, whpx or tcg */
    char *const *envp;  /* the environment of every program run */
};

/* The machine a recipe makes: its key, the digest of the recipe and the
 * image, and its files in the cache. */
struct etude_tooling_machine {
    char key[65];
    char arch[16], url[1024], image_sha256[65];
    char image[4096]; /* CACHE/image-SHA256.img, the downloaded image */
    char disk[4096];  /* CACHE/tooling-KEY.qcow2, the made machine */
    char tools[4096]; /* CACHE/tooling-KEY.tools, provision.sh's report */
};

/* One job. Lists end with NULL. */
struct etude_tooling_job {
    const char *script;         /* the job's shell script on the host */
    const char *const *inputs;  /* files and directories copied to in/ */
    const char *const *disks;   /* raw images attached as etude-d1 on */
    const char *const *outputs; /* names in the workspace brought back */
    const char *outdir;         /* where the outputs go */
    unsigned cpus;              /* 0 gives 2 */
    unsigned memory_mib;        /* 0 gives 2048 */
    unsigned volume_mib;        /* the job volume; 0 gives 64 */
    int network;                /* nonzero gives QEMU's user network */
    uint64_t deadline_us;       /* from QEMU's start to its end; 0 gives 1800 s */
    int keep;                   /* keep the job's directory after a pass */
};

/* How a boot of the machine ended. */
enum etude_tooling_outcome {
    ETUDE_TOOLING_ENDED,  /* the job ran and the runner wrote its volume */
    ETUDE_TOOLING_FAILED, /* the machine, the runner or the host's side failed */
    ETUDE_TOOLING_LATE    /* the deadline passed, and QEMU was stopped */
};

/* The steps of a boot: from QEMU's start to the job's start, the job, and
 * from the job's end to the end of the cleanup. A boot that makes the
 * machine has the first alone, from the download to the end. */
enum { ETUDE_TOOLING_START, ETUDE_TOOLING_JOB, ETUDE_TOOLING_END, ETUDE_TOOLING_STEPS };

/* One step of a boot. */
struct etude_tooling_step {
    int begun, ended;
    int done; /* it ended by its witness: the job's start, the job's end, or
                 the end of the cleanup after a job that ended */
    struct etude_reading from, to;
    struct etude_measurement cpu_us;     /* QEMU's processor time in the step */
    struct etude_measurement peak_bytes; /* QEMU's largest resident size sampled */
};

/* What a boot cost and how it ended. */
struct etude_tooling_cost {
    struct etude_tooling_step steps[ETUDE_TOOLING_STEPS];
    unsigned nsteps;
    struct etude_measure cpu_us;     /* the whole tree's, from the executor */
    struct etude_measure peak_bytes; /* the tree's peak, from the executor */
    uint64_t disk_bytes;             /* the private copy and the volumes, at the end */
    enum etude_cleanup cleanup;
    struct etude_pid qemu;
};

/* The result of etude_tooling_prepare or etude_tooling_run. */
struct etude_tooling_result {
    enum etude_tooling_outcome outcome;
    int status;      /* the job's exit status, once ENDED */
    int making;      /* the boot made the machine */
    int reused;      /* prepare: the machine was made before */
    char why[512];   /* why it did not end, or "" */
    char dir[4096];  /* the job's directory, when kept, or "" */
    char *out, *err; /* the job's standard output and error */
    size_t out_len, err_len;
    struct etude_tooling_cost cost;
};

/**
 * Read the recipe at `site->recipe` and name its machine in `m`.
 *
 * @return 0, or -1 with `why` saying what is wrong with the recipe.
 */
int etude_tooling_machine(const struct etude_tooling_site *site, struct etude_tooling_machine *m,
                          char *why, size_t whylen);

/**
 * Make the machine unless the cache has it: download and check the image,
 * boot a copy of it with the seed volume and the network, wait until
 * provision.sh reports, and publish the copy, read-only, under its key.
 * `deadline_us` bounds the boot, 0 giving two hours.
 *
 * @return 0 when the machine is there, made now or before; -1 otherwise,
 *         with the reason in `res->why`.
 */
int etude_tooling_prepare(const struct etude_tooling_site *site, unsigned cpus,
                          uint64_t deadline_us, struct etude_tooling_result *res);

/**
 * Run `job` in a private copy of the made machine and bring its outputs back.
 *
 * @return 0 when the job ran to its end, whatever its status; -1 when the
 *         machine is not made, it failed, or the deadline passed. `res` has
 *         the outcome, the job's output and the cost either way, and the
 *         caller frees it with etude_tooling_result_free.
 */
int etude_tooling_run(const struct etude_tooling_site *site, const struct etude_tooling_job *job,
                      struct etude_tooling_result *res);

/** Free the output a result keeps. */
void etude_tooling_result_free(struct etude_tooling_result *res);

/* The identities under which a boot's steps are recorded. */
struct etude_tooling_ids {
    const char *plan, *item, *attempt; /* item and attempt may be NULL */
    const char *machine, *boot;        /* the clock's machine and boot */
    const char *store;                 /* where blobs are kept, by digest */
    const char *key;                   /* the tooling machine's key */
};

/**
 * Append a `step` record to `h` for each step of `res`'s boot: tooling_start,
 * execute and cleanup for a job, or one_time_setup for the making of the
 * machine. A step's processor time is QEMU's between two samples, where the
 * platform reads a running process's usage; the scope blob of each has the
 * whole tree's processor time, peak and disk use.
 *
 * @return 0, or -1 when a record or a blob could not be written.
 */
int etude_tooling_record(struct etude_history *h, const struct etude_tooling_ids *ids,
                         const struct etude_tooling_result *res);

#endif
