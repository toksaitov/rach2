/*
 * contract.h: the names and limits of etude's execution contract, shared by
 * the controller, the executors, etude-guest and the machine scenarios.
 * tools/etude/Readme.md defines what each name means.
 */
#ifndef ETUDE_CONTRACT_H
#define ETUDE_CONTRACT_H

/* The version of the case manifest, the invocation and the results. */
#define ETUDE_CONTRACT_VERSION 1

/* The longest case, assertion, witness, step or fixture identity. */
#define ETUDE_ID_MAX 128

/* Where a case's program runs, as its manifest declares. */
enum etude_domain {
    ETUDE_DOMAIN_HOST,   /* on the controller's host, or its tooling machine */
    ETUDE_DOMAIN_GUEST,  /* on Rach2 under etude-guest, in QEMU or natively */
    ETUDE_DOMAIN_MACHINE /* a scenario on the host that drives Rach2 in QEMU */
};

/* Where one item of a plan runs: the history's `domain`. */
enum etude_placement {
    ETUDE_PLACE_HOST,  /* a host case */
    ETUDE_PLACE_QEMU,  /* a guest case in a fresh boot, or a machine case */
    ETUDE_PLACE_NATIVE /* a guest case on a running Rach2, over SSH */
};

/* What the case needs to be on Rach2 (dev-log/62-etude/native.md). */
enum etude_role {
    ETUDE_ROLE_ORDINARY, /* any process; a child of etude-guest */
    ETUDE_ROLE_INIT,     /* the kernel's designated init */
    ETUDE_ROLE_BOOT      /* a boot of its own: a command line or a fresh kernel */
};

/* Who reports an assertion's result. */
enum etude_source {
    ETUDE_SOURCE_PROGRAM,  /* the case's program, in report records */
    ETUDE_SOURCE_SCENARIO, /* a machine scenario, through libetude-case */
    ETUDE_SOURCE_EXECUTOR  /* the executor: termination, kernel records */
};

/* The outcome of one assertion in the results. */
enum etude_check {
    ETUDE_CHECK_PASS,
    ETUDE_CHECK_FAIL,
    ETUDE_CHECK_NOT_RUN, /* reported as not evaluated; never a pass */
    ETUDE_CHECK_MISSING  /* declared and never reported */
};

/* How a case's execution ended. Each is a distinct fact. */
enum etude_end {
    ETUDE_END_EXITED,       /* an ordinary process exited; the status is kept */
    ETUDE_END_SIGNALED,     /* an ordinary process was killed by a signal */
    ETUDE_END_INIT_EXITED,  /* the designated init exited, by the kernel's record */
    ETUDE_END_INIT_KILLED,  /* the designated init was killed, by the kernel's record */
    ETUDE_END_PANIC,        /* the kernel panicked */
    ETUDE_END_RESET,        /* the machine reset */
    ETUDE_END_SHUTDOWN,     /* the machine powered off */
    ETUDE_END_HALTED,       /* the machine runs with every processor halted */
    ETUDE_END_QEMU_EXITED,  /* QEMU ended without a shutdown or reset event */
    ETUDE_END_ACCEL_FAILED, /* QEMU could not start the requested accelerator */
    ETUDE_END_STOPPED,      /* a watchdog or a cancellation ended the case */
    ETUDE_END_LOST          /* the transport was lost; the end was not observed */
};

/* The stage of a step, as the history's `stage` gives it. */
enum etude_stage {
    ETUDE_STAGE_QUEUE,
    ETUDE_STAGE_BUILD,
    ETUDE_STAGE_PREPARE,
    ETUDE_STAGE_TRANSFER,
    ETUDE_STAGE_BOOT,
    ETUDE_STAGE_EXECUTE,
    ETUDE_STAGE_CAPTURE,
    ETUDE_STAGE_CLEANUP,
    ETUDE_STAGE_TOOLING_START,
    ETUDE_STAGE_ONE_TIME_SETUP
};

/* A progress witness's kind. */
enum etude_witness {
    ETUDE_WITNESS_STEP,    /* a completed step; units count completions */
    ETUDE_WITNESS_COUNTER, /* bounded work; units advance toward a declared limit */
    ETUDE_WITNESS_QUIET    /* an opaque child's step, with a completion bound of its own */
};

/* The step identities every executor uses for its own stages. */
#define ETUDE_STEP_PREPARE  "etude.prepare"
#define ETUDE_STEP_TRANSFER "etude.transfer"
#define ETUDE_STEP_BOOT     "etude.boot"
#define ETUDE_STEP_EXECUTE  "etude.execute"
#define ETUDE_STEP_CAPTURE  "etude.capture"
#define ETUDE_STEP_CLEANUP  "etude.cleanup"

/* The assertions an executor adds to a case; tools/etude/Readme.md says
 * when. */
#define ETUDE_ASSERT_TERMINATION "etude.termination"
#define ETUDE_ASSERT_KERNEL      "etude.kernel"
#define ETUDE_ASSERT_REPORT      "etude.report"

#endif
