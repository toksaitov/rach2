/**
 * Check etude's set driver (etude_legacy_set in tools/etude/posix/legacy_set.c)
 * on the suites of a fixture tree: a registry, a Makefile whose recipes run a
 * script of the tree's, and a stand-in for QEMU first on the path. This
 * program holds the tree's run lock and is the driver's etude-legacy: run as
 * `prepare`, `run` or `check`, it does what etude-legacy does. Each set runs
 * in a child of its own, with a private ledger, and with `locks` a private
 * directory of the old runner's locks. The fixture's case manifests run one
 * shell script of the tree's, which its arguments make pass, fail, end its
 * report early or outrun its bound, and the built casecheck judges them. Its
 * guest cases run the tree's rngprobe on the real QEMU, booting the tree's
 * kernel and test image of the architecture ARCH names, i686 when it is
 * unset, which the fixture's preparation links into the fixture tree; their
 * evidence goes under the tree's build/tests.
 *
 * host-deps: tools/etude/posix/legacy.c tools/etude/posix/legacy_set.c
 * host-deps: tools/etude/posix/legacy_cases.c tools/etude/digest.c
 * host-deps: tools/etude/posix/legacy_guest.c tools/etude/serial.c
 * host-deps: tools/etude/guest/wire.c tools/etude/posix/store.c
 * host-deps: tools/etude/json.c
 * host-deps: tools/etude/posix/exec.c tools/etude/posix/keeper.c
 * host-deps: tools/etude/posix/process.c tools/etude/capture.c
 * host-deps: tools/etude/watchdog.c tools/etude/supervise.c
 * host-deps: tools/etude/qemu_profile.c tools/etude/posix/qemu.c
 * host-deps: tools/etude/processor.c tools/etude/ledger.c tools/etude/sched.c
 * host-deps: tools/etude/posix/bridge.c tools/etude/posix/status.c
 * host-deps: tools/etude/compare.c tools/etude/baseline.c
 * host-deps: tools/etude/compare_report.c tools/etude/compare_run.c
 * host-deps: tools/etude/history.c tools/etude/history_json.c
 * host-deps: tools/etude/history_collect.c utils/sha256.c
 * host-cflags: -I tools/etude/include -I tools/etude -I tools/etude/posix
 * host-cflags: -Wall -Wextra -Werror
 * host-seconds: 480
 *
 * A set of two suites that pass and one that fails exits 1, with a history
 * the schema accepts of one plan, one step, an attempt for each suite and a
 * run record, the failed suite's work kept and the others' removed, no claim
 * left in the ledger, each command's output kept, and the driver's own line
 * of each suite's start in place of etude-legacy's lines of its start and
 * result. With `locks`, two suites that run at once have port blocks of their
 * own; without, both have the caller's. A width of one keeps two suites
 * apart, and one of two lets them meet. A serial member runs first, with
 * nothing beside it. A failure under `stop_on_fail`, and one of a member that
 * `stops`, cancel the suite that runs and the one not started, and say so. A
 * stop signal exits 143 with the running suite cancelled and its output
 * printed, the history whole and the ledger empty; while the suite runs, the
 * status record has a line of its attempt, and the record is removed at the
 * end. A failed preparation exits 3 with every item cancelled and no attempt.
 * A suite that asks for more memory than the machine has runs. A set of no
 * suite, a bad name, a suite named twice, no port block without `locks`, a
 * bad accelerator and a relative ledger are refused, with nothing run. The
 * driver leaks no memory.
 *
 * A suite run by its cases gives an item of each repetition of each case that
 * runs on the architecture, or on none, with its program built by the
 * preparation as a goal: the tree's `build/NODE`, or `build/x86_64/NODE` for an
 * artifact of each architecture on x86_64. The program runs in its workspace,
 * which is removed after a pass and kept otherwise, with the driver's
 * environment, less the driver's own ARCH and ETUDE_SEED, and the tree, the
 * case's identity, seed, architecture and, with scratch, the workspace as
 * TMPDIR; a case whose progress keeps its gap bound passes. Its attempt has
 * casecheck's outcome and the reason of the assertion that decided it, an
 * attempt that runs past its case bound is incomplete, and `casecheck
 * verdict` passes a run of passing cases and fails the suite of failing ones.
 * A failed case of a suite that `stops` leaves the suite's other cases to run
 * before the set stops. A case suite runs beside a suite of the registry. A
 * manifest that cannot be read, of another suite's family, with a case of a
 * domain other than host and guest, that reads its input, needs ports, loop
 * devices or a fixture other than scratch, or has no case on the
 * architecture is refused, and so is a case suite without casecheck, and a
 * guest case of the init role, booted by OVMF, ending other than by an exit,
 * or with a disk of no disk fixture.
 *
 * A guest case boots its own machine of its profile, its program stored to
 * etude-guest from the image and run there: rngprobe passes on the processor
 * `max`, with its branch `trusted` decided by the profile's features, with the
 * program's output, the console kept for casecheck and the plan's domain
 * `qemu`; a boot on `qemu64` past its stage's bound is stopped and incomplete;
 * each machine is given its profile's processor, which a wrapper of QEMU
 * records; and the run's store of artifacts is removed at its end. A missing
 * QEMU or image fails the test.
 */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "compat/fixdir_host.c"
#include "etude/history.h"
#include "etude/ledger.h"
#include "etude/legacy.h"

static int fails;

/** Count a failure when `cond` is false, and print its file, line and row. */
#define CHECK(cond, row)                                                                     \
    do {                                                                                     \
        if (!(cond)) {                                                                       \
            printf("etude-legacyset-test: fail: %s:%d: %s: %s\n", __FILE__, __LINE__, (row), \
                   #cond);                                                                   \
            fails++;                                                                         \
        }                                                                                    \
    } while (0)

/* The room for a path: a fixture's are under a $TMPDIR of at most 100
 * characters (fixdir_host.h). */
#define PATH_LEN 1024

/* The room for what a set printed, and for a status record. */
#define OUT_LEN 131072

/* The bound of a wait for a recipe to start, and for the status record to
 * name an attempt, in milliseconds. */
#define START_BOUND  60000
#define STATUS_BOUND 30000

/* The seconds a set may take before it is ended as hung, and a set of guest
 * cases, whose machines boot under TCG on the testing host. */
#define SET_BOUND   120
#define GUEST_BOUND 300

/* The memory of the fixture machine's ledger, in MiB. */
#define MEMORY_MIB 8192u

/* LeakSanitizer's check, when the test is built with it; the reference is
 * weak, so a test built without it links. */
extern int __lsan_do_recoverable_leak_check(void) __attribute__((weak));

/* This program, the fixture tree, etude's schemas, the built casecheck, and
 * the fixtures' ledger and lock directory. */
static char self[PATH_MAX];
static char casecheck[PATH_MAX];
static char tree[PATH_LEN];
static char schemas[PATH_MAX];
static char ledger[PATH_LEN];
static char locks[PATH_LEN];

/* The guest cases' architecture, the tree's build of it, the directory of
 * their evidence under the tree's build/tests, and the path without the
 * stand-in for QEMU. */
static const char *guest_arch;
static char guest_build[PATH_MAX];
static char guest_evidence[PATH_MAX];
static char guest_bin[PATH_MAX]; /* the wrapper of QEMU, first on the path */
static char real_path[8192];

/* The suites of the fixture tree. */
static const char registry[] = "# The suites of the fixture tree.\n"
                               "boot|qemu|core\n"
                               "plain|-|core\n"
                               "fail|-|core\n"
                               "meet-a|-|core\n"
                               "meet-b|-|core\n"
                               "serial|-|core\n"
                               "side-x|-|core\n"
                               "side-y|-|core\n"
                               "long|-|core\n"
                               "hold|-|core\n";

/* The fixture's Makefile. Each suite's target needs the one image, and its
 * recipe runs the tree's script, which says what each does; the components
 * fail when FIXTURE_BREAK is set. A case's program is its script under
 * cases/, copied to build/cases or build/x86_64/cases. */
static const char makefile[] =
    "ARCH ?= i686\n"
    "ifeq ($(ARCH),x86_64)\n"
    "ARCH_BUILD := build/x86_64\n"
    "else\n"
    "ARCH_BUILD := build\n"
    "endif\n"
    "IMAGES := $(ARCH_BUILD)/images\n"
    ".PHONY: components prepare-all prepare-tools run-boot run-plain run-fail run-meet-a \\\n"
    "        run-meet-b run-serial run-side-x run-side-y run-long run-hold\n"
    "components: $(ARCH_BUILD)/rach2/kernel\n"
    "\t@if [ -n \"$$FIXTURE_BREAK\" ]; then echo broken components; exit 4; fi\n"
    "$(ARCH_BUILD)/rach2/kernel:\n"
    "\t@mkdir -p $(@D) && echo kernel > $@\n"
    "prepare-all: components\n"
    "prepare-tools:\n"
    "\t@echo sh\n"
    "$(IMAGES)/disk.img:\n"
    "\t@mkdir -p $(@D) && echo disk > $@\n"
    "run-boot: $(IMAGES)/disk.img\n"
    "\t@qemu-system-i386 -m 64 pass\n"
    "run-plain: $(IMAGES)/disk.img\n"
    "\t@echo plain ran\n"
    "run-fail: $(IMAGES)/disk.img\n"
    "\t@echo failing; exit 3\n"
    "run-meet-a: $(IMAGES)/disk.img\n"
    "\t@sh fixture.sh meet meet-a meet-b\n"
    "run-meet-b: $(IMAGES)/disk.img\n"
    "\t@sh fixture.sh meet meet-b meet-a\n"
    "run-serial: $(IMAGES)/disk.img\n"
    "\t@sh fixture.sh mark serial 1\n"
    "run-side-x: $(IMAGES)/disk.img\n"
    "\t@sh fixture.sh mark side-x 0.2\n"
    "run-side-y: $(IMAGES)/disk.img\n"
    "\t@sh fixture.sh mark side-y 0.2\n"
    "run-long: $(IMAGES)/disk.img\n"
    "\t@echo long running; sleep 8\n"
    "run-hold: $(IMAGES)/disk.img\n"
    "\t@sh fixture.sh hold\n"
    "build/cases/%: cases/%.sh\n"
    "\t@mkdir -p $(@D) && cp $< $@ && chmod 755 $@\n"
    "build/x86_64/cases/%: cases/%.sh\n"
    "\t@mkdir -p $(@D) && cp $< $@ && chmod 755 $@\n";

/* The program of the fixture's cases. Its first argument says what it
 * reports: `pass`; `fail`, a failed assertion; `cut`, a report with no end;
 * `tick`, progress for 4.5 s; or `slow`, progress for 11.7 s. It prints how
 * it was run and where, and leaves a file in $TMPDIR. */
static const char probe[] = "#!/bin/sh\n"
                            "echo \"program: $0 $*\"\n"
                            "echo \"cwd: $(pwd -P)\"\n"
                            "echo \"tree: ${ETUDE_TREE-unset}\"\n"
                            "echo \"case: $ETUDE_CASE arch: ${ARCH-unset} seed: ${ETUDE_SEED-unset}"
                            " inherited: ${FIXTURE_INHERITED-unset}\"\n"
                            "echo \"tmpdir: $TMPDIR\"\n"
                            "echo made > \"$TMPDIR/made\"\n"
                            "echo \"@etude 1 begin $ETUDE_CASE\"\n"
                            "echo \"@etude 1 progress ticks 1\"\n"
                            "case $1 in\n"
                            "fail) echo \"@etude 1 assert ran fail the count was 2\" ;;\n"
                            "cut) echo \"@etude 1 assert ran pass\"; exit 0 ;;\n"
                            "tick|slow)\n"
                            "    if [ \"$1\" = tick ]; then n=16; else n=40; fi\n"
                            "    i=2\n"
                            "    while [ $i -le $n ]; do\n"
                            "        sleep 0.3\n"
                            "        echo \"@etude 1 progress ticks $i\"\n"
                            "        i=$((i + 1))\n"
                            "    done\n"
                            "    echo \"@etude 1 assert ran pass\" ;;\n"
                            "*) echo \"@etude 1 assert ran pass\" ;;\n"
                            "esac\n"
                            "echo \"@etude 1 end 1\"\n";

/* One case of a fixture manifest, which manifest_text writes. */
struct fcase {
    const char *id;
    const char *archs; /* its architectures, as JSON */
    const char *args;  /* its arguments, as the items of a JSON array */
    int reps;
    const char *seed; /* its seed, as JSON */
    int scratch;      /* it has the fixture scratch */
    unsigned case_ms, gap_ms;
    int per_arch; /* its program is built for each architecture */
};

/* The form of a case of a fixture manifest, which casecheck accepts. */
static const char case_form[] =
    "{\"case_id\": \"%s\", \"behavior\": \"The fixture's program reports as its arguments say\",\n"
    " \"units\": [\"tests/host/etude_legacyset_hosttest.c\"], \"domain\": \"host\",\n"
    " \"architectures\": %s,\n"
    " \"workload\": {\"arguments\": [%s], \"inputs\": {}, \"repetitions\": %d, \"seed\": %s},\n"
    " \"requirements\": {\"host_api\": \"posix\", \"tools\": [\"sh\"], \"features\": {}},\n"
    " \"execution\": {\"program\": \"probe\", \"stdin\": null, \"role\": \"ordinary\",\n"
    "  \"privileges\": \"an ordinary user\", \"isolation\": [%s], \"children\": \"none\",\n"
    "  \"cleanup\": \"nothing outside its scratch directory\"},\n"
    " \"assertions\": [{\"assertion_id\": \"ran\", \"checks\": \"the program ran to its end\",\n"
    "  \"source\": \"program\"}],\n"
    " \"branches\": [], \"termination\": {\"end\": \"exited\", \"value\": 0},\n"
    " \"progress\": [{\"witness_id\": \"ticks\", \"kind\": \"counter\", \"means\": \"a tick "
    "passed\",\n"
    "  \"limit\": 1000}],\n"
    " \"bounds\": {\"case\": {\"floor_us\": 100000, \"initial_us\": %u000, \"ceiling_us\": "
    "600000000},\n"
    "  \"gap\": {\"floor_us\": 100000, \"initial_us\": %u000, \"ceiling_us\": 600000000},\n"
    "  \"stages\": {}},\n"
    " \"resources\": {\"cpu_millis\": 100, \"cpu_ceiling_millis\": 1000, \"memory_bytes\": "
    "16777216,\n"
    "  \"disk_bytes\": 0, \"locks\": [], \"ports\": 0, \"loop_devices\": 0},\n"
    " \"prepare\": [{\"name\": \"probe\", \"node\": \"cases/probe\", \"per_architecture\": %s}],\n"
    " \"timing\": [], \"native\": {\"plan\": \"host\"}}";

/* The family `good`: cases that pass, one of two repetitions with a seed,
 * one kept within its gap bound by its progress, one of no architecture
 * and one built for each architecture that runs on x86_64 alone. */
static const struct fcase good_cases[] = {
    {"fixture.good.pass", "[\"i686\", \"x86_64\"]", "\"pass\", \"two words\"", 2, "7", 1, 30000,
     30000, 0},
    {"fixture.good.tick", "[\"i686\", \"x86_64\"]", "\"tick\"", 1, "null", 1, 30000, 2000, 0},
    {"fixture.good.none", "\"none\"", "\"pass\"", 1, "null", 0, 30000, 30000, 0},
    {"fixture.good.arch", "[\"x86_64\"]", "\"pass\"", 1, "null", 1, 30000, 30000, 1}};

/* The family `mixed`: a case that passes, one that fails, one whose report
 * has no end and one that runs past its case bound of 3 s. */
static const struct fcase mixed_cases[] = {
    {"fixture.mixed.pass", "[\"i686\"]", "\"pass\"", 1, "null", 1, 30000, 30000, 0},
    {"fixture.mixed.fail", "[\"i686\"]", "\"fail\"", 1, "null", 1, 30000, 30000, 0},
    {"fixture.mixed.cut", "[\"i686\"]", "\"cut\"", 1, "null", 1, 30000, 30000, 0},
    {"fixture.mixed.slow", "[\"i686\"]", "\"slow\"", 1, "null", 1, 3000, 2000, 0}};

/* The family `bad`, whose one case the refused manifests change. */
static const struct fcase bad_cases[] = {
    {"fixture.bad.one", "[\"i686\"]", "\"pass\"", 1, "null", 1, 30000, 30000, 0}};

/* The case of `bad` that runs on x86_64 alone. */
static const struct fcase other_cases[] = {
    {"fixture.bad.one", "[\"x86_64\"]", "\"pass\"", 1, "null", 1, 30000, 30000, 0}};

/* The manifests of `bad` that the driver refuses: each the case of `bad`
 * with the text `from` made `to`, or the case of `other` for `noarch`. */
static const struct {
    const char *name, *from, *to;
} refusals[] = {{"domain", "\"domain\": \"host\"", "\"domain\": \"machine\""},
                {"stdin", "\"stdin\": null", "\"stdin\": \"input\""},
                {"ports", "\"ports\": 0", "\"ports\": 1"},
                {"loops", "\"loop_devices\": 0", "\"loop_devices\": 1"},
                {"fixture", "{\"fixture\": \"scratch\"}", "{\"fixture\": \"network\"}"},
                {"noarch", NULL, NULL}};

/* The form of a guest case of a fixture manifest: the tree's rngprobe on
 * the test image, under the processor profile `cpu`, with its boot stage
 * bounded in milliseconds, which casecheck accepts. */
static const char guest_form[] =
    "{\"case_id\": \"%s\", \"behavior\": \"The random devices give bytes from a trusted source "
    "or refuse to read without one\",\n"
    " \"units\": [\"rngprobe\"], \"domain\": \"guest\", \"architectures\": [\"i686\", "
    "\"x86_64\"],\n"
    " \"workload\": {\"arguments\": [], \"inputs\": {}, \"repetitions\": 1, \"seed\": null},\n"
    " \"requirements\": {\"host_api\": \"c\", \"tools\": [], \"features\": {\"rdrand\": "
    "\"either\", \"rdseed\": \"either\"},\n"
    "  \"qemu\": {\"kernel\": \"kernel\", \"initrd\": \"test-image\", \"disks\": [], \"cpu\": "
    "\"%s\", \"processors\": 1,\n"
    "   \"memory_bytes\": 134217728, \"firmware\": \"direct\", \"devices\": [\"rtl8139\"], "
    "\"boot_options\": []}},\n"
    " \"execution\": {\"program\": \"rngprobe\", \"stdin\": null, \"role\": \"ordinary\", "
    "\"privileges\": \"any\",\n"
    "  \"isolation\": [], \"children\": \"none\", \"cleanup\": \"nothing to remove\"},\n"
    " \"assertions\": [\n"
    "  {\"assertion_id\": \"cpuid-leaf7\", \"checks\": \"kcpu_id gives leaf 7 as cpuid does\", "
    "\"source\": \"program\"},\n"
    "  {\"assertion_id\": \"trusted-urandom\", \"checks\": \"/dev/urandom reads and varies\", "
    "\"source\": \"program\"},\n"
    "  {\"assertion_id\": \"trusted-monobit\", \"checks\": \"its bytes pass a monobit count\", "
    "\"source\": \"program\"},\n"
    "  {\"assertion_id\": \"closed-urandom\", \"checks\": \"/dev/urandom answers EIO\", "
    "\"source\": \"program\"},\n"
    "  {\"assertion_id\": \"closed-random\", \"checks\": \"/dev/random answers EIO\", "
    "\"source\": \"program\"},\n"
    "  {\"assertion_id\": \"closed-insecure\", \"checks\": \"GRND_INSECURE returns bytes\", "
    "\"source\": \"program\"}],\n"
    " \"branches\": [{\"group_id\": \"source\", \"decided_by\": {\"features\": [\"rdrand\", "
    "\"rdseed\"]},\n"
    "  \"alternatives\": [\n"
    "   {\"alternative_id\": \"trusted\", \"when\": \"present\", \"assertions\": "
    "[\"trusted-urandom\", \"trusted-monobit\"]},\n"
    "   {\"alternative_id\": \"fail-closed\", \"when\": \"absent\", \"assertions\": "
    "[\"closed-urandom\", \"closed-random\", \"closed-insecure\"]}]}],\n"
    " \"termination\": {\"end\": \"exited\", \"value\": 0}, \"progress\": [],\n"
    " \"bounds\": {\"case\": {\"floor_us\": 100000, \"initial_us\": 120000000, \"ceiling_us\": "
    "600000000},\n"
    "  \"gap\": {\"floor_us\": 100000, \"initial_us\": 60000000, \"ceiling_us\": 300000000},\n"
    "  \"stages\": {\"boot\": {\"floor_us\": 100000, \"initial_us\": %u000, \"ceiling_us\": "
    "300000000}}},\n"
    " \"resources\": {\"cpu_millis\": 1000, \"cpu_ceiling_millis\": 1000, \"memory_bytes\": "
    "268435456,\n"
    "  \"disk_bytes\": 0, \"locks\": [], \"ports\": 0, \"loop_devices\": 0},\n"
    " \"prepare\": [{\"name\": \"rngprobe\", \"node\": \"guest/rngprobe\", \"per_architecture\": "
    "true},\n"
    "  {\"name\": \"kernel\", \"node\": \"guest/kernel\", \"per_architecture\": true},\n"
    "  {\"name\": \"etude-guest\", \"node\": \"guest/etude-guest\", \"per_architecture\": true},\n"
    "  {\"name\": \"test-image\", \"node\": \"guest/test.krffs\", \"per_architecture\": true}],\n"
    " \"timing\": [], \"native\": {\"plan\": \"native\", \"fixtures\": [{\"fixture\": "
    "\"workspace\"}]}}";

/* A guest case of a fixture manifest, which guest_text writes. */
struct gcase {
    const char *id;
    const char *cpu;  /* its processor profile */
    unsigned boot_ms; /* the bound of its boot */
};

/* The family `guest`: rngprobe on a processor with a random source, and a
 * boot on one without, bounded far below what the boot takes. etude-guest
 * reads /dev/urandom for its boot's token, which a machine without a random
 * source refuses, so no program runs on `qemu64`. */
static const struct gcase guest_cases[] = {{"fixture.guest.rng", "max", 60000},
                                           {"fixture.guest.boot", "qemu64", 200}};

/* The family `badguest`, whose one case the refused manifests change. */
static const struct gcase badguest_cases[] = {{"fixture.badguest.one", "max", 60000}};

/* The manifests of `badguest` that the driver refuses, each its case with
 * the text `from` made `to`. */
static const struct {
    const char *name, *from, *to;
} guest_refusals[] = {
    {"init", "\"role\": \"ordinary\"", "\"role\": \"init\""},
    {"ovmf", "\"firmware\": \"direct\"", "\"firmware\": \"ovmf\""},
    {"end", "\"end\": \"exited\", \"value\": 0", "\"end\": \"shutdown\", \"value\": null"},
    {"disk", "\"disks\": []", "\"disks\": [\"etude-guest\"]"}};

/* The script of the recipes. `meet SELF OTHER` waits two seconds at most
 * for the other suite to be present, and notes whether it met it and its
 * port block; `mark NAME SECONDS` notes its start and its end; `hold` says it
 * has started through a FIFO and waits fifteen seconds at most. */
static const char script[] =
    "mode=$1\n"
    "shift\n"
    "case $mode in\n"
    "meet)\n"
    "    touch \"meet/$1.here\"\n"
    "    i=0\n"
    "    until [ -e \"meet/$2.here\" ] || [ $i -ge 20 ]; do sleep 0.1; i=$((i + 1)); done\n"
    "    if [ -e \"meet/$2.here\" ]; then w=met; else w=alone; fi\n"
    "    sleep 0.3\n"
    "    rm -f \"meet/$1.here\"\n"
    "    echo \"$1 $w $RACH2_PORT_OFFSET\" >> meet/log ;;\n"
    "mark)\n"
    "    echo \"start $1\" >> marks\n"
    "    sleep \"$2\"\n"
    "    echo \"end $1\" >> marks ;;\n"
    "hold)\n"
    "    echo holding\n"
    "    echo started > started.fifo\n"
    "    i=0\n"
    "    until [ -e release ] || [ $i -ge 150 ]; do sleep 0.1; i=$((i + 1)); done ;;\n"
    "esac\n";

/* The stand-in for QEMU, which the shim makes a machine of. */
static const char standin[] = "#!/bin/sh\n"
                              "echo FIXTURE-BOOT-OK\n"
                              "sleep 0.2\n";

/* The records of a history, each a line of the stream. */
struct history {
    char **line;
    size_t n, cap;
};

/* One run of the set driver, as a row asks for it, and what it gave. */
struct run {
    const char *name; /* the row's, after which its files are named */
    const struct etude_legacy_member *members;
    size_t n;
    unsigned width;
    int locks, stop_on_fail;
    const char *accel;  /* the accelerator, or NULL for tcg */
    const char *ledger; /* the ledger, or NULL for the fixtures' */
    const char *unset;  /* a variable the driver runs without, or NULL */
    const char *var;    /* a variable the driver runs with, or NULL */
    const char *arch;   /* the architecture, or NULL for i686 */
    int nojudge;        /* the driver is given no casecheck */
    int guest;          /* it runs guest cases, on the real QEMU */
    pid_t pid;
    int status;  /* the driver's, or -1 when it did not end by itself */
    long claims; /* the claims the ledger kept after it, or -1 */
    int leaks;   /* LeakSanitizer found a leak of the driver */
    char results[PATH_LEN], evidence[PATH_LEN], status_file[PATH_LEN];
    char out_file[PATH_LEN], side[PATH_LEN];
    char run_id[256];
    char out[OUT_LEN]; /* what it printed */
    int read;          /* etude_history_read's result, or -2 for no stream */
    struct history h;
};

/* ---- Files and text. ---- */

/** Give the path of `name` in the fixture tree in `buf`. */
static const char *in(char *buf, size_t size, const char *name)
{
    fix_format(buf, size, "%s/%s", tree, name);
    return buf;
}

/** Make file `path` hold `text` with mode `mode`; 0 or -1. */
static int write_text(const char *path, const char *text, mode_t mode)
{
    FILE *f = fopen(path, "w");
    int r;

    if (f == NULL)
        return -1;
    r = fputs(text, f) < 0 ? -1 : 0;
    if (fclose(f) != 0)
        r = -1;
    return r == 0 ? chmod(path, mode) : -1;
}

/** Read up to `max - 1` bytes of `path` into `buf`, ended by NUL; the
 * length, or -1 with `buf` empty. */
static long read_text(const char *path, char *buf, size_t max)
{
    FILE *f = fopen(path, "r");
    size_t n;

    buf[0] = '\0';
    if (f == NULL)
        return -1;
    n = fread(buf, 1, max - 1, f);
    fclose(f);
    buf[n] = '\0';
    return (long)n;
}

/** Say whether `path` exists. */
static int exists(const char *path)
{
    struct stat st;

    return lstat(path, &st) == 0;
}

/** Give the first line of `text` that begins with `start`, or NULL. */
static const char *line_start(const char *text, const char *start)
{
    const char *at;

    for (at = text; (at = strstr(at, start)) != NULL; at++) {
        if (at == text || at[-1] == '\n')
            return at;
    }
    return NULL;
}

/** Say whether `text` has a line that begins with `start`. */
static int has_start(const char *text, const char *start)
{
    return line_start(text, start) != NULL;
}

/** Count the lines of `text` that are `line` whole. */
static int count_line(const char *text, const char *line)
{
    size_t n = strlen(line);
    const char *at;
    int count = 0;

    for (at = text; (at = strstr(at, line)) != NULL; at++) {
        if ((at == text || at[-1] == '\n') && (at[n] == '\n' || at[n] == '\0'))
            count++;
    }
    return count;
}

/** Say whether `text` has the whole line `line`. */
static int has_line(const char *text, const char *line)
{
    return count_line(text, line) > 0;
}

/** Give the line number of `line` in `text`, from 1, or 0 when it has none. */
static int line_no(const char *text, const char *line)
{
    size_t n = strlen(line);
    const char *at = text;
    int no = 1;

    while (*at != '\0') {
        if (strncmp(at, line, n) == 0 && (at[n] == '\n' || at[n] == '\0'))
            return no;
        at = strchr(at, '\n');
        if (at == NULL)
            break;
        at++;
        no++;
    }
    return 0;
}

/* ---- Histories. ---- */

/** Keep a copy of line `line` of a stream in the history `ctx`. */
static int keep_line(const char *line, void *ctx)
{
    struct history *h = ctx;
    char **grown;

    if (h->n == h->cap) {
        h->cap = h->cap == 0 ? 16 : h->cap * 2;
        if ((grown = realloc(h->line, h->cap * sizeof(*grown))) == NULL)
            return -1;
        h->line = grown;
    }
    if ((h->line[h->n] = strdup(line)) == NULL)
        return -1;
    h->n++;
    return 0;
}

/** Free the lines of history `h`. */
static void history_free(struct history *h)
{
    size_t i;

    for (i = 0; i < h->n; i++)
        free(h->line[i]);
    free(h->line);
    memset(h, 0, sizeof(*h));
}

/** Read the one stream under RESULTS/history of run `r`, with the run's
 * identity from its name. */
static void history_read(struct run *r)
{
    char dir[PATH_LEN], path[PATH_LEN * 2], schema[PATH_MAX + 32];
    struct dirent *e;
    size_t n;
    uint64_t bad = 0;
    DIR *d;

    r->read = -2;
    r->run_id[0] = '\0';
    fix_format(dir, sizeof(dir), "%s/history", r->results);
    if ((d = opendir(dir)) == NULL)
        return;
    while ((e = readdir(d)) != NULL) {
        n = strlen(e->d_name);
        if (n > 6 && strcmp(e->d_name + n - 6, ".jsonl") == 0 && n - 6 < sizeof(r->run_id)) {
            snprintf(r->run_id, sizeof(r->run_id), "%.*s", (int)(n - 6), e->d_name);
            break;
        }
    }
    closedir(d);
    if (r->run_id[0] == '\0')
        return;
    fix_format(path, sizeof(path), "%s/%s.jsonl", dir, r->run_id);
    fix_format(schema, sizeof(schema), "%s/history.schema.json", schemas);
    r->read = etude_history_read(path, schema, keep_line, &r->h, &bad);
}

/** Count the records of kind `kind` in history `h`. */
static int count_kind(const struct history *h, const char *kind)
{
    char want[64];
    size_t i;
    int n = 0;

    fix_format(want, sizeof(want), "\"kind\":\"%s\"", kind);
    for (i = 0; i < h->n; i++)
        n += strstr(h->line[i], want) != NULL;
    return n;
}

/** Copy into `out` the outcome that follows `key` after `from` in `line`, or
 * "" when there is none. */
static void outcome_after(const char *line, const char *from, const char *key, char *out,
                          size_t size)
{
    const char *at = strstr(line, from);

    out[0] = '\0';
    if (at == NULL || (at = strstr(at, key)) == NULL)
        return;
    at += strlen(key);
    snprintf(out, size, "%.*s", (int)strcspn(at, "\""), at);
}

/** Give the outcome of the attempt record of suite `suite` in `out`, or ""
 * when the history has no attempt of it. */
static const char *attempt_of(const struct run *r, const char *suite, char *out, size_t size)
{
    char id[128];
    size_t i;

    out[0] = '\0';
    fix_format(id, sizeof(id), ":legacy.%s:i686:r0:a0\"", suite);
    for (i = 0; i < r->h.n; i++)
        if (strstr(r->h.line[i], "\"kind\":\"attempt\"") != NULL && strstr(r->h.line[i], id))
            outcome_after(r->h.line[i], "\"kind\":\"attempt\"", "\"result\":{\"outcome\":\"", out,
                          size);
    return out;
}

/** Give the outcome of the item of suite `suite` in the run record in
 * `out`, or "" when there is none. */
static const char *item_of(const struct run *r, const char *suite, char *out, size_t size)
{
    char id[128];
    size_t i;

    out[0] = '\0';
    fix_format(id, sizeof(id), ":legacy.%s:i686:r0\",\"attempt_records\":[", suite);
    for (i = 0; i < r->h.n; i++)
        if (strstr(r->h.line[i], "\"kind\":\"run\"") != NULL)
            outcome_after(r->h.line[i], id, "\"outcome\":\"", out, size);
    return out;
}

/** Give the outcome of the preparation's step record in `out`, or "". */
static const char *step_of(const struct run *r, char *out, size_t size)
{
    size_t i;

    out[0] = '\0';
    for (i = 0; i < r->h.n; i++)
        if (strstr(r->h.line[i], "\"kind\":\"step\"") != NULL)
            outcome_after(r->h.line[i], "\"kind\":\"step\"", "\"result\":{\"outcome\":\"", out,
                          size);
    return out;
}

/** Say whether the attempt of `suite` has outcome `want`. */
static int attempt_is(const struct run *r, const char *suite, const char *want)
{
    char got[32];

    return strcmp(attempt_of(r, suite, got, sizeof(got)), want) == 0;
}

/** Say whether the run record's item of `suite` has outcome `want`. */
static int item_is(const struct run *r, const char *suite, const char *want)
{
    char got[32];

    return strcmp(item_of(r, suite, got, sizeof(got)), want) == 0;
}

/* ---- This program as etude-legacy. ---- */

/* The stop signal this process received, as etude-legacy or as a set's
 * driver. */
static volatile sig_atomic_t stop_signal;

/** Note a stop signal. */
static void note_stop(int sig)
{
    stop_signal = sig;
}

/** Catch the stop signals without restarting the executor's waits, as
 * etude-legacy does. */
static void catch_stops(void)
{
    static const int sigs[] = {SIGTERM, SIGINT, SIGHUP};
    struct sigaction sa;
    size_t i;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = note_stop;
    sigemptyset(&sa.sa_mask);
    for (i = 0; i < sizeof(sigs) / sizeof(sigs[0]); i++)
        sigaction(sigs[i], &sa, NULL);
}

/** Read a whole number of seconds from 1 to a week; 0 when `s` is not one. */
static unsigned seconds_of(const char *s)
{
    char *end;
    unsigned long v;

    if (*s < '0' || *s > '9')
        return 0;
    v = strtoul(s, &end, 10);
    return *end == '\0' && v >= 1 && v <= 604800ul ? (unsigned)v : 0;
}

/**
 * Do what etude-legacy does with `argv`, its arguments from `prepare`, `run`
 * or `check` on: the options the set driver gives it, parsed as
 * tools/etude/posix/etude_legacy.c parses them.
 *
 * @return its status, or 2 for a wrong usage.
 */
static int as_legacy(int argc, char **argv)
{
    static char *goals[64];
    struct etude_legacy_opts p;
    struct etude_legacy_run_opts r;
    int opt;

    setvbuf(stdout, NULL, _IOLBF, 0);
    catch_stops();
    if (strcmp(argv[0], "check") == 0)
        return argc == 2 ? etude_legacy_check(argv[1], 0, stdout) : 2;
    memset(&p, 0, sizeof(p));
    memset(&r, 0, sizeof(r));
    p.tree = ".";
    p.seconds = r.seconds = ETUDE_LEGACY_SECONDS;
    r.idle = ETUDE_LEGACY_IDLE;
    p.out = r.out = stdout;
    p.stop = r.stop = &stop_signal;
    p.goals = goals;
    while ((opt = getopt(argc, argv, "t:a:n:w:o:s:i:b:")) != -1) {
        if (opt == 'b' && p.ngoals < sizeof(goals) / sizeof(goals[0]))
            goals[p.ngoals++] = optarg;
        else if (opt == 't')
            p.tree = optarg;
        else if (opt == 'a')
            p.arch = optarg;
        else if (opt == 'n')
            p.kernel = optarg;
        else if (opt == 'w')
            p.work = r.work = optarg;
        else if (opt == 'o')
            p.record = optarg;
        else if (opt == 's' && (p.seconds = r.seconds = seconds_of(optarg)) != 0)
            continue;
        else if (opt == 'i' && (r.idle = seconds_of(optarg)) != 0)
            continue;
        else
            return 2;
    }
    if (strcmp(argv[0], "prepare") == 0 && (optind < argc || p.ngoals > 0)) {
        p.suites = argv + optind;
        p.nsuites = (size_t)(argc - optind);
        return etude_legacy_prepare(&p);
    }
    if (strcmp(argv[0], "run") == 0 && optind == argc - 2) {
        r.record = argv[optind];
        r.suite = argv[optind + 1];
        return etude_legacy_run(&r);
    }
    return 2;
}

/* ---- The fixture tree. ---- */

/** Write in `buf` the manifest of family `family` with the `n` cases `c`;
 * 0, or -1 when it does not fit. */
static int manifest_text(char *buf, size_t size, const char *family, const struct fcase *c,
                         size_t n)
{
    size_t i, len;
    int k;

    k = snprintf(buf, size,
                 "{\"schema_version\": 1, \"family_id\": \"%s\",\n"
                 " \"summary\": \"Cases of the set driver's fixture\",\n \"cases\": [",
                 family);
    for (i = 0; k >= 0 && (size_t)k < size && i < n; i++) {
        len = (size_t)k;
        k = snprintf(buf + len, size - len, case_form, c[i].id, c[i].archs, c[i].args, c[i].reps,
                     c[i].seed, c[i].scratch ? "{\"fixture\": \"scratch\"}" : "", c[i].case_ms,
                     c[i].gap_ms, c[i].per_arch ? "true" : "false");
        k = k < 0 ? k : (int)len + k;
        if (k >= 0 && (size_t)k < size && i + 1 < n)
            k += snprintf(buf + k, size - (size_t)k, ",\n");
    }
    if (k >= 0 && (size_t)k < size)
        k += snprintf(buf + k, size - (size_t)k, "]}\n");
    return k >= 0 && (size_t)k < size ? 0 : -1;
}

/** Write in `buf` the manifest of family `family` with the `n` guest cases
 * `c`; 0, or -1 when it does not fit. */
static int guest_text(char *buf, size_t size, const char *family, const struct gcase *c, size_t n)
{
    size_t i, len;
    int k;

    k = snprintf(buf, size,
                 "{\"schema_version\": 1, \"family_id\": \"%s\",\n"
                 " \"summary\": \"Guest cases of the set driver's fixture\",\n \"cases\": [",
                 family);
    for (i = 0; k >= 0 && (size_t)k < size && i < n; i++) {
        len = (size_t)k;
        k = snprintf(buf + len, size - len, guest_form, c[i].id, c[i].cpu, c[i].boot_ms);
        k = k < 0 ? k : (int)len + k;
        if (k >= 0 && (size_t)k < size && i + 1 < n)
            k += snprintf(buf + k, size - (size_t)k, ",\n");
    }
    if (k >= 0 && (size_t)k < size)
        k += snprintf(buf + k, size - (size_t)k, "]}\n");
    return k >= 0 && (size_t)k < size ? 0 : -1;
}

/** Write in `out` the text `text` with its first `from` made `to`; 0, or -1
 * when it has no `from` or the result does not fit. */
static int replaced(char *out, size_t size, const char *text, const char *from, const char *to)
{
    const char *at = strstr(text, from);
    int k;

    if (at == NULL)
        return -1;
    k = snprintf(out, size, "%.*s%s%s", (int)(at - text), text, to, at + strlen(from));
    return k >= 0 && (size_t)k < size ? 0 : -1;
}

/** Write the program of the fixture's cases and their manifests under
 * cases/: `good`, `mixed` and `guest` in directories of their own, as
 * casecheck's verdict reads them, and the refused ones of `bad` and
 * `badguest`; 0, or -1. */
static int make_cases(void)
{
    static char text[16384], changed[16384];
    char path[PATH_LEN], name[64];
    size_t i;
    int bad = 0;

    bad |= write_text(in(path, sizeof(path), "cases/probe.sh"), probe, 0755);
    bad |= manifest_text(text, sizeof(text), "fixture.good", good_cases, 4) != 0 ||
           write_text(in(path, sizeof(path), "cases/good/good.json"), text, 0644) != 0;
    bad |= manifest_text(text, sizeof(text), "fixture.mixed", mixed_cases, 4) != 0 ||
           write_text(in(path, sizeof(path), "cases/mixed/mixed.json"), text, 0644) != 0;
    bad |= manifest_text(text, sizeof(text), "fixture.bad", bad_cases, 1);
    for (i = 0; i < sizeof(refusals) / sizeof(refusals[0]); i++) {
        if (refusals[i].from != NULL)
            bad |= replaced(changed, sizeof(changed), text, refusals[i].from, refusals[i].to);
        else
            bad |= manifest_text(changed, sizeof(changed), "fixture.bad", other_cases, 1);
        fix_format(name, sizeof(name), "cases/bad/%s.json", refusals[i].name);
        bad |= write_text(in(path, sizeof(path), name), changed, 0644);
    }
    bad |= guest_text(text, sizeof(text), "fixture.guest", guest_cases, 2) != 0 ||
           write_text(in(path, sizeof(path), "cases/guest/guest.json"), text, 0644) != 0;
    bad |= guest_text(text, sizeof(text), "fixture.badguest", badguest_cases, 1);
    for (i = 0; i < sizeof(guest_refusals) / sizeof(guest_refusals[0]); i++) {
        bad |=
            replaced(changed, sizeof(changed), text, guest_refusals[i].from, guest_refusals[i].to);
        fix_format(name, sizeof(name), "cases/badguest/%s.json", guest_refusals[i].name);
        bad |= write_text(in(path, sizeof(path), name), changed, 0644);
    }
    return bad ? -1 : 0;
}

/**
 * Give in `out` the rules that link the guest cases' artifacts, the nodes
 * `guest/NAME`, to the tree's build of their architecture, which the
 * fixture's preparation makes; 0, or -1 when they do not fit.
 */
static int guest_rules(char *out, size_t size)
{
    static const char *const links[][2] = {{"kernel", "rach2/kernel"},
                                           {"test.krffs", "images/test.krffs"},
                                           {"etude-guest", "tools/etude/guest/etude-guest"},
                                           {"rngprobe", "tests/rngprobe"}};
    const char *dir = strcmp(guest_arch, "x86_64") == 0 ? "build/x86_64" : "build";
    size_t i, used = 0;
    int k;

    for (i = 0; i < sizeof(links) / sizeof(links[0]); i++) {
        k = snprintf(out + used, size - used,
                     "%s/guest/%s:\n\t@mkdir -p $(@D) && ln -sf %s/%s $@\n", dir, links[i][0],
                     guest_build, links[i][1]);
        if (k < 0 || (size_t)k >= size - used)
            return -1;
        used += (size_t)k;
    }
    return 0;
}

/** Make the fixture tree, with its run lock held by this process, runutil's
 * source, the recipes' script and the stand-in first on the path. */
static void make_tree(void)
{
    static const char *const dirs[] = {"",
                                       "tests",
                                       "tests/system",
                                       "tests/system/runner",
                                       "build",
                                       "build/.run.lock",
                                       "meet",
                                       "qemu",
                                       "cases",
                                       "cases/good",
                                       "cases/mixed",
                                       "cases/bad",
                                       "cases/guest",
                                       "cases/badguest"};
    static char source[1 << 20], newpath[8192], rules[8192];
    const char *oldpath = getenv("PATH");
    char path[PATH_LEN], owner[64];
    size_t i;
    int bad = 0;

    fix_format(tree, sizeof(tree), "%s/tree", fix_dir());
    fix_format(ledger, sizeof(ledger), "%s/ledger", fix_dir());
    fix_format(locks, sizeof(locks), "%s/locks", fix_dir());
    fix_format(path, sizeof(path), "%s/tmp", fix_dir());
    bad |= mkdir(path, 0755) != 0 || setenv("TMPDIR", path, 1) != 0;
    bad |= mkdir(locks, 0755) != 0;
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        bad |= mkdir(in(path, sizeof(path), dirs[i]), 0755) != 0;
    bad |= mkfifo(in(path, sizeof(path), "started.fifo"), 0644) != 0;
    snprintf(owner, sizeof(owner), "pid %ld\n", (long)getpid());
    bad |= write_text(in(path, sizeof(path), "tests/registry"), registry, 0644);
    bad |= guest_rules(rules, sizeof(rules)) != 0;
    fix_format(source, sizeof(source), "%s%s", makefile, rules);
    bad |= write_text(in(path, sizeof(path), "Makefile"), source, 0644);
    bad |= write_text(in(path, sizeof(path), "fixture.sh"), script, 0644);
    bad |= write_text(in(path, sizeof(path), "qemu/qemu-system-i386"), standin, 0755);
    bad |= write_text(in(path, sizeof(path), "build/.run.lock/owner"), owner, 0644);
    bad |= make_cases();
    bad |= read_text("tests/system/runner/runutil.c", source, sizeof(source)) <= 0;
    bad |= write_text(in(path, sizeof(path), "tests/system/runner/runutil.c"), source, 0644);
    fix_format(real_path, sizeof(real_path), "%s", oldpath != NULL ? oldpath : "/usr/bin:/bin");
    fix_format(newpath, sizeof(newpath), "%s/qemu:%s", tree, real_path);
    bad |= setenv("PATH", newpath, 1) != 0;
    if (bad) {
        printf("etude-legacyset-test: fail: the fixture tree %s was not made\n", tree);
        exit(1);
    }
}

/** Remove the files of the recipes that meet and that mark their times. */
static void clear_marks(void)
{
    static const char *const names[] = {"marks", "meet/log", "meet/meet-a.here",
                                        "meet/meet-b.here"};
    char path[PATH_LEN];
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        unlink(in(path, sizeof(path), names[i]));
}

/* ---- Sets. ---- */

/** Count claim `c` into the count `ctx`. */
static int count_claim(const struct etude_ledger_claim *c, void *ctx)
{
    (void)c;
    (*(long *)ctx)++;
    return 0;
}

/** Fill `o` for run `r`, with bounds no row reaches. */
static void opts_for(struct etude_legacy_set_opts *o, const struct run *r)
{
    memset(o, 0, sizeof(*o));
    o->tree = tree;
    o->arch = r->arch != NULL ? r->arch : "i686";
    o->program = self;
    o->casecheck = r->nojudge ? NULL : casecheck;
    o->members = r->members;
    o->nmembers = r->n;
    o->ledger = r->ledger != NULL ? r->ledger : ledger;
    o->account = "fixture";
    o->cap[ETUDE_LEGACY_CPU] = 8000;
    o->cap[ETUDE_LEGACY_MEMORY] = (uint64_t)MEMORY_MIB << 20;
    o->cap[ETUDE_LEGACY_DISK] = UINT64_C(64) << 30;
    o->locks = r->locks ? locks : NULL;
    o->wait = 30;
    o->seconds = 60;
    o->idle = 30;
    o->width = r->width;
    o->cpus = 1;
    o->accel = r->accel != NULL ? r->accel : "tcg";
    o->stop_on_fail = r->stop_on_fail;
    o->results = r->results;
    o->schemas = schemas;
    o->evidence = r->evidence;
    o->status = r->status_file;
    o->args = r->name;
}

/**
 * Run the set of `r` as a set's driver, in this child of the test: write
 * what it printed to `r->out_file`, then the claims the ledger keeps while
 * this process, their owner, still runs and whether LeakSanitizer finds a
 * leak to `r->side`, and exit with the driver's status.
 */
static void driver(const struct run *r)
{
    struct etude_legacy_set_opts o;
    struct etude_ledger *l;
    long claims = -1;
    int status, leaks;
    FILE *f;

    catch_stops();
    if (r->unset != NULL)
        unsetenv(r->unset);
    if (r->var != NULL)
        putenv((char *)r->var);
    if (r->guest) {
        static char path[sizeof(guest_bin) + sizeof(real_path) + 1];

        fix_format(path, sizeof(path), "%s:%s", guest_bin, real_path);
        if (setenv("PATH", path, 1) != 0)
            _exit(120);
    }
    opts_for(&o, r);
    o.stop = &stop_signal;
    if ((o.out = fopen(r->out_file, "w")) == NULL)
        _exit(120);
    status = etude_legacy_set(&o);
    fclose(o.out);
    if ((l = etude_ledger_open(ledger)) != NULL) {
        claims = 0;
        if (etude_ledger_list(l, count_claim, &claims) != 0)
            claims = -1;
        etude_ledger_close(l);
    }
    leaks = __lsan_do_recoverable_leak_check != NULL && __lsan_do_recoverable_leak_check() != 0;
    if ((f = fopen(r->side, "w")) != NULL) {
        fprintf(f, "%ld %d\n", claims, leaks);
        fclose(f);
    }
    _exit(status & 0xff);
}

/** Start the set of `r` in a child, with its files named for the row, and
 * the evidence of guest cases under the tree's build/tests. */
static void set_start(struct run *r)
{
    const char *dir = fix_dir();

    fix_format(r->results, sizeof(r->results), "%s/%s.results", dir, r->name);
    fix_format(r->evidence, sizeof(r->evidence), "%s/%s.evidence", r->guest ? guest_evidence : dir,
               r->name);
    fix_format(r->status_file, sizeof(r->status_file), "%s/%s.status", dir, r->name);
    fix_format(r->out_file, sizeof(r->out_file), "%s/%s.out", dir, r->name);
    fix_format(r->side, sizeof(r->side), "%s/%s.side", dir, r->name);
    r->status = -1;
    r->claims = -1;
    fflush(stdout);
    r->pid = fork();
    if (r->pid == 0)
        driver(r);
    if (r->pid < 0)
        printf("etude-legacyset-test: fail: %s: no child: %s\n", r->name, strerror(errno));
}

/** Wait for the set of `r` to end within its bound, SET_BOUND or
 * GUEST_BOUND seconds, or end it, and take what it gave. */
static void set_finish(struct run *r)
{
    int bound = r->guest ? GUEST_BOUND : SET_BOUND;
    char side[64];
    int status, i;
    pid_t got = 0;

    for (i = 0; r->pid > 0 && i < bound * 20; i++) {
        if ((got = waitpid(r->pid, &status, WNOHANG)) != 0)
            break;
        /* interval: a set is looked at again every 50 ms */
        usleep(50000);
    }
    if (r->pid > 0 && got == 0) {
        printf("etude-legacyset-test: fail: %s: the set did not end in %d s\n", r->name, bound);
        fails++;
        kill(r->pid, SIGKILL);
        got = waitpid(r->pid, &status, 0);
    } else if (got == r->pid && WIFEXITED(status)) {
        r->status = WEXITSTATUS(status);
    }
    read_text(r->out_file, r->out, sizeof(r->out));
    if (read_text(r->side, side, sizeof(side)) > 0 &&
        sscanf(side, "%ld %d", &r->claims, &r->leaks) != 2)
        r->claims = -1;
    history_read(r);
}

/** Run the set of `r` and take what it gave. */
static void set_run(struct run *r)
{
    set_start(r);
    set_finish(r);
}

/** Print what a set printed when a check of its row failed, and free its
 * history. */
static void show(struct run *r, int before)
{
    if (fails != before)
        printf("etude-legacyset-test: %s: its output:\n%s", r->name, r->out);
    history_free(&r->h);
}

/** Say whether the work of suite `suite` of run `r` was kept as evidence. */
static int kept(const struct run *r, const char *suite)
{
    char path[PATH_LEN * 2];

    fix_format(path, sizeof(path), "%s/%s.%s", r->evidence, r->run_id, suite);
    return exists(path);
}

/** Say whether the output of command `name` of run `r` was kept, with a line
 * `line` in it when `line` is not NULL. */
static int output_has(const struct run *r, const char *name, const char *line)
{
    static char text[OUT_LEN];
    char path[PATH_LEN * 2];

    fix_format(path, sizeof(path), "%s/output/%s.log", r->results, name);
    return read_text(path, text, sizeof(text)) >= 0 && (line == NULL || has_line(text, line));
}

/** Say whether casecheck's results of the attempt of case member `tail`,
 * `CASE.rR`, of run `r` have the text `text`. */
static int results_has(const struct run *r, const char *tail, const char *text)
{
    static char doc[OUT_LEN];
    char path[PATH_LEN * 2];

    fix_format(path, sizeof(path), "%s/output/%s.results", r->results, tail);
    return read_text(path, doc, sizeof(doc)) > 0 && strstr(doc, text) != NULL;
}

/** Give the attempt record of run `r` of repetition `rep` of case `id` on
 * `arch`, or NULL. */
static const char *case_attempt(const struct run *r, const char *id, const char *arch, int rep)
{
    char tail[256];
    size_t i;

    fix_format(tail, sizeof(tail), ":%s:%s:r%d:a0\"", id, arch, rep);
    for (i = 0; i < r->h.n; i++)
        if (strstr(r->h.line[i], "\"kind\":\"attempt\"") != NULL && strstr(r->h.line[i], tail))
            return r->h.line[i];
    return NULL;
}

/** Say whether the attempt of repetition `rep` of case `id` on `arch` has
 * outcome `want`, and a reason that begins with `reason` when that is not
 * NULL. */
static int case_is(const struct run *r, const char *id, const char *arch, int rep, const char *want,
                   const char *reason)
{
    const char *line = case_attempt(r, id, arch, rep), *at;
    char got[32];

    if (line == NULL)
        return 0;
    outcome_after(line, "\"kind\":\"attempt\"", "\"result\":{\"outcome\":\"", got, sizeof(got));
    if (strcmp(got, want) != 0)
        return 0;
    at = strstr(strstr(line, "\"result\":{\"outcome\":\""), "\"reason\":\"");
    return reason == NULL || (at != NULL && strncmp(at + 10, reason, strlen(reason)) == 0);
}

/** Say whether the preparation's record of run `r` lists the artifact
 * `goal`. */
static int recorded(const struct run *r, const char *goal)
{
    static char text[OUT_LEN];
    char path[PATH_LEN], want[PATH_LEN];

    fix_format(path, sizeof(path), "%s/prepare.record", r->results);
    fix_format(want, sizeof(want), " %s\n", goal);
    return read_text(path, text, sizeof(text)) > 0 && strstr(text, want) != NULL;
}

/** Run casecheck's verdict on the history of run `r` for suite `suite`,
 * with the manifests of the tree's directory `cases`, and give what it
 * printed in `out`; its status, or -1. */
static int verdict(const struct run *r, const char *cases, const char *suite, char *out,
                   size_t size)
{
    char store[PATH_LEN + 8], dir[PATH_LEN], history[PATH_LEN + 16], file[PATH_LEN];
    int status, fd;
    pid_t pid;

    fix_format(store, sizeof(store), "%s/store", r->results);
    fix_format(history, sizeof(history), "%s/history", r->results);
    fix_format(file, sizeof(file), "%s/%s.verdict", fix_dir(), r->name);
    in(dir, sizeof(dir), cases);
    fflush(stdout);
    if ((pid = fork()) == 0) {
        fd = open(file, O_WRONLY | O_CREAT | O_TRUNC, 0644);
        if (fd < 0 || dup2(fd, 1) < 0 || dup2(fd, 2) < 0)
            _exit(127);
        execl(casecheck, "casecheck", "-s", schemas, "verdict", "-b", store, "-m", dir,
              r->arch != NULL ? r->arch : "i686", "fixture", suite, history, (char *)NULL);
        _exit(127);
    }
    if (pid < 0 || waitpid(pid, &status, 0) != pid)
        return -1;
    read_text(file, out, size);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

/* ---- Rows. ---- */

/** A set of suites that pass and one that fails. */
static void test_mixed(void)
{
    static const struct etude_legacy_member m[] = {
        {"boot", 64, 0, 0, NULL}, {"plain", 64, 0, 0, NULL}, {"fail", 64, 0, 0, NULL}};
    static struct run r;
    static const char *const names[] = {"boot", "plain", "fail"};
    char line[128];
    size_t i;
    int before = fails;

    r.name = "mixed";
    r.members = m;
    r.n = 3;
    set_run(&r);
    CHECK(r.status == 1, "mixed: failed");
    CHECK(r.read == 0, "mixed: the history is whole");
    CHECK(count_kind(&r.h, "plan") == 1, "mixed: one plan");
    CHECK(count_kind(&r.h, "step") == 1, "mixed: one step");
    CHECK(count_kind(&r.h, "attempt") == 3, "mixed: an attempt of each suite");
    CHECK(count_kind(&r.h, "run") == 1, "mixed: one run record");
    CHECK(attempt_is(&r, "boot", "pass"), "mixed: boot passed");
    CHECK(attempt_is(&r, "plain", "pass"), "mixed: plain passed");
    CHECK(attempt_is(&r, "fail", "failure"), "mixed: fail failed");
    CHECK(item_is(&r, "fail", "failure"), "mixed: the run record's item of fail");
    CHECK(kept(&r, "fail"), "mixed: the failed suite's work kept");
    CHECK(!kept(&r, "plain") && !kept(&r, "boot"), "mixed: a passed suite's work removed");
    CHECK(r.claims == 0, "mixed: the ledger has no claim");
    CHECK(!r.leaks, "mixed: the driver leaks nothing");
    CHECK(output_has(&r, "prepare", NULL), "mixed: the preparation's output kept");
    CHECK(output_has(&r, "boot", "FIXTURE-BOOT-OK"), "mixed: the output of boot kept");
    CHECK(output_has(&r, "fail", "failing"), "mixed: the output of fail kept");
    CHECK(has_line(r.out, "failing") && has_line(r.out, "plain ran"),
          "mixed: the suites' output printed");
    for (i = 0; i < 3; i++) {
        fix_format(line, sizeof(line), "run: %s: starting (idle limit 30 s)", names[i]);
        CHECK(count_line(r.out, line) == 1, "mixed: one line of each suite's start");
        fix_format(line, sizeof(line), "run: %s: PASS in ", names[i]);
        CHECK(!has_start(r.out, line), "mixed: no line of etude-legacy's result");
        fix_format(line, sizeof(line), "run: %s: FAIL in ", names[i]);
        CHECK(!has_start(r.out, line), "mixed: no line of etude-legacy's result");
    }
    CHECK(has_line(r.out, "etude-legacy: result: ran: 3 suites, 2 passed"), "mixed: the result");
    show(&r, before);
}

/** Read the meeting suites' notes into `log`, and say whether both have the
 * note `what`. */
static int both(char *log, size_t size, const char *a, const char *b)
{
    char path[PATH_LEN];

    read_text(in(path, sizeof(path), "meet/log"), log, size);
    return has_start(log, a) && has_start(log, b);
}

/** Give the port offset of the meeting suite whose note begins with
 * `start` in `log`, or -1 when it has none. */
static long offset_of(const char *log, const char *start)
{
    const char *at = line_start(log, start);
    long offset;

    return at != NULL && sscanf(at, "%*s %*s %ld", &offset) == 1 ? offset : -1;
}

/** Two suites that run at once each have a port block of their own with
 * `locks`, and width 2 lets them meet. */
static void test_ports(void)
{
    static const struct etude_legacy_member m[] = {{"meet-a", 64, 0, 0, NULL},
                                                   {"meet-b", 64, 0, 0, NULL}};
    static struct run r;
    char log[1024];
    int before = fails;

    clear_marks();
    r.name = "ports";
    r.members = m;
    r.n = 2;
    r.width = 2;
    r.locks = 1;
    set_run(&r);
    CHECK(r.status == 0, "ports: passed");
    CHECK(both(log, sizeof(log), "meet-a met ", "meet-b met "), "width: two at once with width 2");
    CHECK(offset_of(log, "meet-a ") >= 0 && offset_of(log, "meet-a ") != offset_of(log, "meet-b "),
          "ports: blocks of their own with locks");
    CHECK(strstr(log, " 7000\n") == NULL, "ports: not the caller's block with locks");
    CHECK(r.claims == 0, "ports: the ledger has no claim");
    if (fails != before)
        printf("etude-legacyset-test: ports: the notes:\n%s", log);
    show(&r, before);
}

/** Without `locks` both suites have the caller's port block, and width 1
 * keeps them apart. */
static void test_width(void)
{
    static const struct etude_legacy_member m[] = {{"meet-a", 64, 0, 0, NULL},
                                                   {"meet-b", 64, 0, 0, NULL}};
    static struct run r;
    char log[1024];
    int before = fails;

    clear_marks();
    r.name = "width";
    r.members = m;
    r.n = 2;
    r.width = 1;
    set_run(&r);
    CHECK(r.status == 0, "width: passed");
    CHECK(both(log, sizeof(log), "meet-a alone ", "meet-b alone "),
          "width: one at a time with width 1");
    CHECK(has_line(log, "meet-a alone 7000") || has_line(log, "meet-a met 7000"),
          "ports: the caller's block without locks");
    CHECK(has_line(log, "meet-b alone 7000") || has_line(log, "meet-b met 7000"),
          "ports: the caller's block without locks");
    if (fails != before)
        printf("etude-legacyset-test: width: the notes:\n%s", log);
    show(&r, before);
}

/** A serial member, though named second, runs first with nothing beside
 * it. */
static void test_serial(void)
{
    static const struct etude_legacy_member m[] = {
        {"side-x", 64, 0, 0, NULL}, {"serial", 64, 1, 0, NULL}, {"side-y", 64, 0, 0, NULL}};
    static struct run r;
    char path[PATH_LEN], marks[1024];
    int before = fails;

    clear_marks();
    r.name = "serial";
    r.members = m;
    r.n = 3;
    set_run(&r);
    read_text(in(path, sizeof(path), "marks"), marks, sizeof(marks));
    CHECK(r.status == 0, "serial: passed");
    CHECK(line_no(marks, "start serial") == 1, "serial: it runs first");
    CHECK(line_no(marks, "end serial") == 2, "serial: nothing beside it");
    CHECK(line_no(r.out, "run: serial: starting (idle limit 30 s)") <
                  line_no(r.out, "run: side-x: starting (idle limit 30 s)") &&
              line_no(r.out, "run: serial: starting (idle limit 30 s)") > 0,
          "serial: it starts first");
    if (fails != before)
        printf("etude-legacyset-test: serial: the marks:\n%s", marks);
    show(&r, before);
}

/** A failure under `stop_on_fail` cancels the suite that runs and the one
 * not started. */
static void test_first(void)
{
    static const struct etude_legacy_member m[] = {
        {"fail", 64, 0, 0, NULL}, {"long", 64, 0, 0, NULL}, {"plain", 64, 0, 0, NULL}};
    static struct run r;
    int before = fails;

    r.name = "first";
    r.members = m;
    r.n = 3;
    r.width = 2;
    r.stop_on_fail = 1;
    set_run(&r);
    CHECK(r.status == 1, "first: failed");
    CHECK(r.read == 0, "first: the history is whole");
    CHECK(attempt_is(&r, "fail", "failure"), "first: fail failed");
    CHECK(attempt_is(&r, "long", "cancelled"), "first: the running suite cancelled");
    CHECK(attempt_is(&r, "plain", ""), "first: no attempt of the suite not started");
    CHECK(item_is(&r, "plain", "cancelled"), "first: the suite not started cancelled");
    CHECK(has_line(r.out, "run: stopped on the first failure; not started: plain"),
          "first: the suites not started named");
    CHECK(has_line(r.out, "run: stopped on the first failure; stopped part way: long"),
          "first: the suites stopped part way named");
    CHECK(has_line(r.out, "run: long: stopped before its end; its output so far:"),
          "first: the stopped suite's output");
    CHECK(r.claims == 0, "first: the ledger has no claim");
    show(&r, before);
}

/** A failure of a member that `stops` cancels the rest. */
static void test_stops(void)
{
    static const struct etude_legacy_member m[] = {
        {"fail", 64, 0, 1, NULL}, {"long", 64, 0, 0, NULL}, {"plain", 64, 0, 0, NULL}};
    static struct run r;
    int before = fails;

    r.name = "stops";
    r.members = m;
    r.n = 3;
    r.width = 2;
    set_run(&r);
    CHECK(r.status == 1, "stops: failed");
    CHECK(attempt_is(&r, "long", "cancelled"), "stops: the running suite cancelled");
    CHECK(item_is(&r, "plain", "cancelled"), "stops: the suite not started cancelled");
    CHECK(has_line(r.out, "run: stopped: fail failed; not started: plain"),
          "stops: the suites not started named");
    CHECK(has_line(r.out, "run: stopped: fail failed; stopped part way: long"),
          "stops: the suites stopped part way named");
    show(&r, before);
}

/** A stop signal while a suite runs: the status record has a line of its
 * attempt first, and the set then ends with the suite cancelled. */
static void test_signal(void)
{
    static const struct etude_legacy_member m[] = {{"hold", 64, 0, 0, NULL},
                                                   {"plain", 64, 0, 0, NULL}};
    static struct run r;
    static char status[OUT_LEN];
    char path[PATH_LEN], got[16], want[512];
    const char *at;
    struct pollfd pfd;
    int fd, i, named = 0, before = fails;

    fd = open(in(path, sizeof(path), "started.fifo"), O_RDWR | O_CLOEXEC);
    r.name = "signal";
    r.members = m;
    r.n = 2;
    set_start(&r);
    pfd.fd = fd;
    pfd.events = POLLIN;
    CHECK(fd >= 0 && r.pid > 0 && poll(&pfd, 1, START_BOUND) == 1 && read(fd, got, sizeof(got)) > 0,
          "signal: the suite started");
    for (i = 0; i < STATUS_BOUND / 50 && !named; i++) {
        read_text(r.status_file, status, sizeof(status));
        named = (at = line_start(status, "attempt ")) != NULL &&
                strstr(at, ":legacy.hold:i686:r0:a0 legacy.hold i686 ") != NULL;
        if (!named)
            /* interval: the record is read again every 50 ms */
            usleep(50000);
    }
    CHECK(named, "status: the running suite's attempt");
    CHECK(strncmp(status, "etude-status 1\n", 15) == 0, "status: the record's form");
    fix_format(want, sizeof(want), "ledger %s", ledger);
    CHECK(has_line(status, want), "status: the run's ledger");
    if (r.pid > 0)
        kill(r.pid, SIGTERM);
    set_finish(&r);
    if (fd >= 0)
        close(fd);
    fix_format(want, sizeof(want), "run %s", r.run_id);
    CHECK(has_line(status, want), "status: the run's identity");
    CHECK(!exists(r.status_file), "status: removed at the end");
    CHECK(r.status == 128 + SIGTERM, "signal: stopped");
    CHECK(r.read == 0 && count_kind(&r.h, "run") == 1, "signal: the history whole");
    CHECK(attempt_is(&r, "hold", "cancelled"), "signal: the running suite cancelled");
    CHECK(item_is(&r, "hold", "cancelled"), "signal: the run record's item cancelled");
    at = line_start(r.out, "run: hold: stopped before its end; its output so far:");
    CHECK(at != NULL && has_line(at, "holding"), "signal: its output so far printed");
    CHECK(has_line(r.out, "etude-legacy: result: stopped by signal 15"), "signal: the result");
    CHECK(r.claims == 0, "signal: the ledger has no claim");
    if (fails != before)
        printf("etude-legacyset-test: signal: the status record:\n%s", status);
    show(&r, before);
}

/** A preparation that fails cancels every item, and no suite starts. */
static void test_prepare(void)
{
    static const struct etude_legacy_member m[] = {{"plain", 64, 0, 0, NULL},
                                                   {"fail", 64, 0, 0, NULL}};
    static char var[] = "FIXTURE_BREAK=1";
    static struct run r;
    char step[32];
    int before = fails;

    r.name = "prepare";
    r.members = m;
    r.n = 2;
    r.var = var;
    set_run(&r);
    CHECK(r.status == 3, "prepare: failed");
    CHECK(r.read == 0, "prepare: the history is whole");
    CHECK(strcmp(step_of(&r, step, sizeof(step)), "failure") == 0, "prepare: the step failed");
    CHECK(count_kind(&r.h, "attempt") == 0, "prepare: no attempt");
    CHECK(item_is(&r, "plain", "cancelled") && item_is(&r, "fail", "cancelled"),
          "prepare: every item cancelled");
    CHECK(has_start(r.out, "etude-legacy: result: preparation failed: "), "prepare: the result");
    CHECK(r.claims == 0, "prepare: the ledger has no claim");
    show(&r, before);
}

/** A suite that asks for more memory than the machine has runs, its claim
 * held to the limit. */
static void test_large(void)
{
    static const struct etude_legacy_member m[] = {{"plain", UINT64_C(1) << 20, 0, 0, NULL}};
    static struct run r;
    int before = fails;

    r.name = "large";
    r.members = m;
    r.n = 1;
    r.locks = 1;
    set_run(&r);
    CHECK(r.status == 0, "large: passed");
    CHECK(attempt_is(&r, "plain", "pass"), "large: it ran");
    CHECK(r.claims == 0, "large: the ledger has no claim");
    show(&r, before);
}

/** Check the passing cases of `good` on i686 that run `r`, and the
 * environment, workspace and scratch directory of their program. */
static void good_ran(struct run *r)
{
    char line[PATH_LEN * 2], evidence[PATH_MAX], root[PATH_MAX];

    if (realpath(r->evidence, evidence) == NULL || realpath(tree, root) == NULL)
        evidence[0] = root[0] = '\0';
    fix_format(line, sizeof(line), "cwd: %s/%s.fixture.good.none.r0", evidence, r->run_id);
    CHECK(output_has(r, "fixture.good.none.r0", line), "workspace: a host case runs in it");
    fix_format(line, sizeof(line), "tree: %s", root);
    CHECK(output_has(r, "fixture.good.none.r0", line), "workspace: ETUDE_TREE is the tree");
    CHECK(!kept(r, "fixture.good.none.r0"), "workspace: removed after a pass without scratch");

    CHECK(case_is(r, "fixture.good.pass", "i686", 0, "pass", NULL) &&
              case_is(r, "fixture.good.pass", "i686", 1, "pass", NULL),
          "cases: both repetitions passed");
    CHECK(case_is(r, "fixture.good.tick", "i686", 0, "pass", NULL),
          "cases: progress keeps a case within its gap bound");
    CHECK(case_is(r, "fixture.good.none", "none", 0, "pass", NULL),
          "cases: the case of no architecture passed");
    CHECK(case_attempt(r, "fixture.good.arch", "i686", 0) == NULL,
          "cases: no attempt of a case of another architecture");
    fix_format(line, sizeof(line), "program: %s/build/cases/probe pass two words", tree);
    CHECK(output_has(r, "fixture.good.pass.r1", line), "cases: the program and its arguments");
    CHECK(output_has(r, "fixture.good.pass.r1",
                     "case: fixture.good.pass arch: i686 seed: 7 inherited: yes"),
          "environment: the case, its architecture and seed, and the driver's own");
    CHECK(output_has(r, "fixture.good.none.r0",
                     "case: fixture.good.none arch: unset seed: unset inherited: yes"),
          "environment: no architecture and no seed for a case of none");
    fix_format(line, sizeof(line), "tmpdir: %s/%s.fixture.good.pass.r0", r->evidence, r->run_id);
    CHECK(output_has(r, "fixture.good.pass.r0", line), "scratch: TMPDIR is the case's scratch");
    fix_format(line, sizeof(line), "tmpdir: %s/tmp", fix_dir());
    CHECK(output_has(r, "fixture.good.none.r0", line), "scratch: the driver's TMPDIR without it");
    CHECK(!kept(r, "fixture.good.pass.r0") && !kept(r, "fixture.good.tick.r0"),
          "scratch: removed after a pass");
    CHECK(results_has(r, "fixture.good.pass.r0", "\"report\":{\"state\":\"complete\""),
          "cases: casecheck's results kept");
    CHECK(has_line(r->out, "run: good: fixture.good.pass: starting") &&
              has_line(r->out, "run: good: fixture.good.pass r1: starting"),
          "cases: a line of each start");
    CHECK(recorded(r, "build/cases/probe"), "prepare: the program a goal");
}

/** A suite run by the cases of its manifest, which pass. */
static void test_cases(void)
{
    static const struct etude_legacy_member m[] = {{"good", 0, 0, 0, "cases/good/good.json"}};
    static char var[] = "FIXTURE_INHERITED=yes";
    static struct run r;
    static char text[OUT_LEN];
    int before = fails;

    r.name = "cases";
    r.members = m;
    r.n = 1;
    r.var = var;
    set_run(&r);
    CHECK(r.status == 0, "cases: passed");
    CHECK(r.read == 0, "cases: the history is whole");
    CHECK(count_kind(&r.h, "attempt") == 4, "cases: an attempt of each repetition of each case");
    good_ran(&r);
    CHECK(!recorded(&r, "build/x86_64/cases/probe"), "prepare: no program of another architecture");
    CHECK(has_line(r.out, "etude-legacy: result: ran: 1 suites, 1 passed"), "cases: the result");
    CHECK(r.claims == 0, "cases: the ledger has no claim");
    CHECK(!r.leaks, "cases: the driver leaks nothing");
    CHECK(verdict(&r, "cases/good", "good", text, sizeof(text)) == 0 &&
              has_line(text, "casecheck: result: passed"),
          "verdict: the run of passing cases passes");
    if (fails != before)
        printf("etude-legacyset-test: cases: the verdict:\n%s", text);
    show(&r, before);
}

/** On x86_64, a case built for each architecture runs its program of
 * build/x86_64, and the others their program of build. */
static void test_cases64(void)
{
    static const struct etude_legacy_member m[] = {{"good", 0, 0, 0, "cases/good/good.json"}};
    static struct run r;
    static char text[OUT_LEN];
    char line[PATH_LEN * 2];
    int before = fails;

    r.name = "cases64";
    r.members = m;
    r.n = 1;
    r.arch = "x86_64";
    set_run(&r);
    CHECK(r.status == 0, "cases64: passed");
    CHECK(count_kind(&r.h, "attempt") == 5, "cases64: an attempt of each repetition of each case");
    CHECK(case_is(&r, "fixture.good.arch", "x86_64", 0, "pass", NULL) &&
              case_is(&r, "fixture.good.pass", "x86_64", 1, "pass", NULL) &&
              case_is(&r, "fixture.good.none", "none", 0, "pass", NULL),
          "cases64: the cases of x86_64 and of none passed");
    fix_format(line, sizeof(line), "program: %s/build/x86_64/cases/probe pass", tree);
    CHECK(output_has(&r, "fixture.good.arch.r0", line),
          "cases64: the program of each architecture");
    fix_format(line, sizeof(line), "program: %s/build/cases/probe pass two words", tree);
    CHECK(output_has(&r, "fixture.good.pass.r0", line),
          "cases64: the program of every architecture");
    CHECK(output_has(&r, "fixture.good.arch.r0",
                     "case: fixture.good.arch arch: x86_64 seed: unset inherited: unset"),
          "environment: the architecture x86_64");
    CHECK(recorded(&r, "build/x86_64/cases/probe") && recorded(&r, "build/cases/probe"),
          "prepare: the goals of both kinds of program");
    CHECK(verdict(&r, "cases/good", "good", text, sizeof(text)) == 0,
          "verdict: the run on x86_64 passes");
    if (fails != before)
        printf("etude-legacyset-test: cases64: the verdict:\n%s", text);
    show(&r, before);
}

/** A suite of cases that pass, fail, end their report early and run past
 * their case bound. */
static void test_cases_mixed(void)
{
    static const struct etude_legacy_member m[] = {{"mixed", 0, 0, 0, "cases/mixed/mixed.json"}};
    static const char *const kept_ones[] = {"fixture.mixed.fail.r0", "fixture.mixed.cut.r0",
                                            "fixture.mixed.slow.r0"};
    static struct run r;
    static char text[OUT_LEN];
    char path[PATH_LEN * 2], line[PATH_LEN * 2];
    size_t i;
    int before = fails;

    r.name = "casesmixed";
    r.members = m;
    r.n = 1;
    set_run(&r);
    CHECK(r.status == 1, "casesmixed: failed");
    CHECK(r.read == 0, "casesmixed: the history is whole");
    CHECK(case_is(&r, "fixture.mixed.pass", "i686", 0, "pass", NULL), "casesmixed: pass passed");
    CHECK(case_is(&r, "fixture.mixed.fail", "i686", 0, "failure", "ran failed: the count was 2"),
          "casesmixed: a failed assertion fails, with its detail");
    CHECK(case_is(&r, "fixture.mixed.cut", "i686", 0, "incomplete", "etude.report is missing"),
          "casesmixed: a report with no end is incomplete");
    CHECK(results_has(&r, "fixture.mixed.cut.r0", "\"report\":{\"state\":\"truncated\""),
          "casesmixed: casecheck's results of a report with no end");
    CHECK(case_is(&r, "fixture.mixed.slow", "i686", 0, "incomplete",
                  "it ran past its case bound of 3000 ms: "),
          "casesmixed: a case past its case bound is incomplete");
    CHECK(results_has(&r, "fixture.mixed.slow.r0", "\"termination\":{\"end\":\"stopped\""),
          "casesmixed: a case past its bound was stopped");
    CHECK(!kept(&r, "fixture.mixed.pass.r0"), "scratch: removed after a pass");
    for (i = 0; i < sizeof(kept_ones) / sizeof(kept_ones[0]); i++) {
        fix_format(path, sizeof(path), "%s/%s.%s/made", r.evidence, r.run_id, kept_ones[i]);
        CHECK(exists(path), "scratch: kept when the case did not pass");
    }
    fix_format(line, sizeof(line), "run: mixed: fixture.mixed.fail: evidence: %s/%s.%s", r.evidence,
               r.run_id, "fixture.mixed.fail.r0");
    CHECK(has_line(r.out, line), "scratch: a kept scratch directory named");
    CHECK(has_line(r.out, "etude-legacy: result: ran: 1 suites, 0 passed"),
          "casesmixed: the result");
    CHECK(r.claims == 0, "casesmixed: the ledger has no claim");
    CHECK(!r.leaks, "casesmixed: the driver leaks nothing");
    CHECK(verdict(&r, "cases/mixed", "mixed", text, sizeof(text)) == 1 &&
              has_line(text, "casecheck: result: failed: mixed"),
          "verdict: the suite of failing cases fails");
    if (fails != before)
        printf("etude-legacyset-test: casesmixed: the verdict:\n%s", text);
    show(&r, before);
}

/** A failed case of a suite that stops the set leaves the other cases of
 * its suite to run, and the set then stops before the next suite. */
static void test_cases_stops(void)
{
    static const struct etude_legacy_member m[] = {{"mixed", 0, 1, 1, "cases/mixed/mixed.json"},
                                                   {"plain", 64, 0, 0, NULL}};
    static struct run r;
    int before = fails;

    r.name = "casesstops";
    r.members = m;
    r.n = 2;
    set_run(&r);
    CHECK(r.status == 1, "casesstops: failed");
    CHECK(case_is(&r, "fixture.mixed.fail", "i686", 0, "failure", "ran failed: the count was 2"),
          "casesstops: the case that failed");
    CHECK(case_is(&r, "fixture.mixed.cut", "i686", 0, "incomplete", "etude.report is missing") &&
              case_is(&r, "fixture.mixed.slow", "i686", 0, "incomplete", NULL),
          "casesstops: the cases after the one that failed ran");
    CHECK(item_is(&r, "plain", "cancelled"), "casesstops: the next suite cancelled");
    CHECK(has_line(r.out, "run: stopped: mixed failed; not started: plain"),
          "casesstops: the stop names the suite");
    show(&r, before);
}

/** A suite run by its cases beside a suite of the registry. */
static void test_cases_beside(void)
{
    static const struct etude_legacy_member m[] = {{"plain", 64, 0, 0, NULL},
                                                   {"good", 0, 0, 0, "cases/good/good.json"}};
    static struct run r;
    int before = fails;

    r.name = "beside";
    r.members = m;
    r.n = 2;
    set_run(&r);
    CHECK(r.status == 0, "beside: passed");
    CHECK(count_kind(&r.h, "attempt") == 5, "beside: an attempt of the suite and of each case");
    CHECK(attempt_is(&r, "plain", "pass") && item_is(&r, "plain", "pass"),
          "beside: the suite of the registry passed");
    CHECK(case_is(&r, "fixture.good.pass", "i686", 1, "pass", NULL), "beside: the cases passed");
    CHECK(recorded(&r, "build/cases/probe"), "prepare: the program a goal beside a suite");
    CHECK(has_line(r.out, "run: plain: starting (idle limit 30 s)"), "beside: the suite started");
    CHECK(has_line(r.out, "etude-legacy: result: ran: 2 suites, 2 passed"), "beside: the result");
    show(&r, before);
}

/** Require of the set `r`, named `name`, a refusal with nothing run, for a
 * reason with `why` in it when that is not NULL. */
static void refused(struct run *r, const char *name, const char *row, const char *why)
{
    int before = fails;

    r->name = name;
    set_run(r);
    CHECK(r->status == 2, row);
    CHECK(has_start(r->out, "etude-legacy: result: refused: "), row);
    CHECK(why == NULL || strstr(r->out, why) != NULL, row);
    CHECK(!exists(r->results), row);
    show(r, before);
}

/** The case suites refused before anything runs. */
static void test_cases_refused(void)
{
    static const struct {
        const char *suite, *cases, *why;
    } rows[] = {
        {"good", "cases/good/absent.json", "cases/good/absent.json: "},
        {"other", "cases/good/good.json", "its family fixture.good is not of the suite other"},
        {"bad", "cases/bad/domain.json",
         "fixture.bad.one: a case of the domain machine; the set driver runs host and guest cases "
         "alone"},
        {"bad", "cases/bad/stdin.json", "fixture.bad.one: the case reads standard input"},
        {"bad", "cases/bad/ports.json", "fixture.bad.one: the case needs local ports"},
        {"bad", "cases/bad/loops.json", "fixture.bad.one: the case needs local ports"},
        {"bad", "cases/bad/fixture.json", "fixture.bad.one: the case needs the fixture network"},
        {"bad", "cases/bad/noarch.json", "no case of it runs on i686"},
        {"badguest", "cases/badguest/init.json",
         "fixture.badguest.one: the case's role is init; the set driver runs ordinary and boot "
         "guest cases alone"},
        {"badguest", "cases/badguest/ovmf.json",
         "fixture.badguest.one: the case boots by ovmf, which the set driver does not find"},
        {"badguest", "cases/badguest/end.json",
         "fixture.badguest.one: the case ends as shutdown; the set driver runs a guest case that "
         "exits"},
        {"badguest", "cases/badguest/disk.json",
         "fixture.badguest.one: the disk etude-guest is in no disk fixture"},
        {"good", "cases/good/good.json", "a suite run by its cases needs casecheck's path"}};
    static struct etude_legacy_member m[sizeof(rows) / sizeof(rows[0])];
    static struct run r[sizeof(rows) / sizeof(rows[0])];
    static char names[sizeof(rows) / sizeof(rows[0])][32];
    size_t i, n = sizeof(rows) / sizeof(rows[0]);

    for (i = 0; i < n; i++) {
        m[i].suite = rows[i].suite;
        m[i].cases = rows[i].cases;
        r[i].members = &m[i];
        r[i].n = 1;
        r[i].nojudge = i == n - 1;
        fix_format(names[i], sizeof(names[i]), "caserefused%lu", (unsigned long)i);
        refused(&r[i], names[i], rows[i].why, rows[i].why);
    }
}

/** Say whether the plan of run `r` has the text `text`. */
static int plan_has(const struct run *r, const char *text)
{
    size_t i;

    for (i = 0; i < r->h.n; i++)
        if (strstr(r->h.line[i], "\"kind\":\"plan\"") != NULL)
            return strstr(r->h.line[i], text) != NULL;
    return 0;
}

/** Give the store of guest artifacts that run `r` left under its evidence,
 * `RUN.store`, in `out`. */
static const char *guest_store(const struct run *r, char *out, size_t size)
{
    fix_format(out, size, "%s/%s.store", r->evidence, r->run_id);
    return out;
}

/** Print the attempt records of guest run `r`, and the end of the console of
 * each case whose workspace was kept, for a row that failed. */
static void show_guest(const struct run *r)
{
    static const char *const ids[] = {"rng", "boot"};
    static char text[OUT_LEN];
    char path[PATH_LEN * 2];
    size_t i;
    long n;

    for (i = 0; i < r->h.n; i++)
        if (strstr(r->h.line[i], "\"kind\":\"attempt\"") != NULL)
            printf("etude-legacyset-test: guest: attempt: %s\n", r->h.line[i]);
    for (i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        fix_format(path, sizeof(path), "%s/%s.fixture.guest.%s.r0/console", r->evidence, r->run_id,
                   ids[i]);
        if ((n = read_text(path, text, sizeof(text))) > 0)
            printf("etude-legacyset-test: guest: the end of %s:\n%s\n", path,
                   text + (n > 2048 ? n - 2048 : 0));
    }
}

/**
 * A suite run by guest cases on the real QEMU: rngprobe on the processor
 * `max`, whose features decide its branch, and a boot on `qemu64` that runs
 * past its stage's bound.
 */
static void test_guest(void)
{
    static const struct etude_legacy_member m[] = {{"guest", 0, 0, 0, "cases/guest/guest.json"}};
    static struct run r;
    static char text[OUT_LEN];
    char path[PATH_LEN * 2];
    int before = fails;

    r.name = "guest";
    r.members = m;
    r.n = 1;
    r.arch = guest_arch;
    r.guest = 1;
    set_run(&r);
    CHECK(r.status == 1, "guest: the boot past its bound fails the run");
    CHECK(r.read == 0, "guest: the history is whole");
    CHECK(count_kind(&r.h, "attempt") == 2, "guest: an attempt of each case");
    CHECK(case_is(&r, "fixture.guest.rng", guest_arch, 0, "pass", NULL),
          "guest: rngprobe passed on max");
    CHECK(results_has(&r, "fixture.guest.rng.r0",
                      "{\"group_id\":\"source\",\"alternative_id\":\"trusted\",\"decided_by\":"
                      "\"feature\"}"),
          "guest: max's features decide the branch trusted");
    fix_format(path, sizeof(path), "%s/qemu.argv", guest_evidence);
    CHECK(read_text(path, text, sizeof(text)) > 0 && strstr(text, "-cpu max,") != NULL &&
              strstr(text, "-cpu qemu64,") != NULL,
          "guest: each machine has its profile's processor");
    CHECK(results_has(&r, "fixture.guest.rng.r0", "\"placement\":\"qemu\"") &&
              results_has(&r, "fixture.guest.rng.r0",
                          "\"termination\":{\"end\":\"exited\",\"value\":0,\"observed_by\":"
                          "\"etude_guest\""),
          "guest: placed qemu, its end from etude-guest");
    CHECK(results_has(&r, "fixture.guest.rng.r0",
                      "{\"assertion_id\":\"etude.kernel\",\"source\":\"executor\",\"outcome\":"
                      "\"pass\""),
          "guest: the console judged for the kernel's faults");
    CHECK(output_has(&r, "fixture.guest.rng.r0", "RNGPROBE START"), "guest: the program's output");
    CHECK(case_is(&r, "fixture.guest.boot", guest_arch, 0, "incomplete",
                  "it ran past its stage bound of 200 ms: "),
          "guest: a boot past its stage's bound is incomplete");
    CHECK(results_has(&r, "fixture.guest.boot.r0", "\"termination\":{\"end\":\"stopped\""),
          "guest: a boot past its bound was stopped");
    fix_format(path, sizeof(path), "%s/%s.fixture.guest.boot.r0/console", r.evidence, r.run_id);
    CHECK(exists(path), "guest: the console kept when the case did not pass");
    CHECK(!kept(&r, "fixture.guest.rng.r0"), "guest: the workspace removed after a pass");
    CHECK(!exists(guest_store(&r, path, sizeof(path))), "guest: the store removed at the end");
    CHECK(plan_has(&r, "\"domain\":\"qemu\"") && !plan_has(&r, "\"domain\":\"host\""),
          "guest: the plan's items are of the domain qemu");
    CHECK(recorded(&r, strcmp(guest_arch, "x86_64") == 0 ? "build/x86_64/guest/test.krffs"
                                                         : "build/guest/test.krffs"),
          "prepare: the test image a goal");
    CHECK(r.claims == 0, "guest: the ledger has no claim");
    CHECK(!r.leaks, "guest: the driver leaks nothing");
    if (fails != before)
        show_guest(&r);
    show(&r, before);
}

/** The requests refused before anything runs. */
static void test_refused(void)
{
    static const struct etude_legacy_member bad[] = {{"Plain", 64, 0, 0, NULL}};
    static const struct etude_legacy_member twice[] = {{"plain", 64, 0, 0, NULL},
                                                       {"plain", 64, 0, 0, NULL}};
    static const struct etude_legacy_member one[] = {{"plain", 64, 0, 0, NULL}};
    static struct run r[6];

    r[0].members = one;
    refused(&r[0], "none", "refused: no suite", NULL);
    r[1].members = bad;
    r[1].n = 1;
    refused(&r[1], "badname", "refused: a bad name", NULL);
    r[2].members = twice;
    r[2].n = 2;
    refused(&r[2], "twice", "refused: a suite named twice", NULL);
    r[3].members = one;
    r[3].n = 1;
    r[3].unset = "RACH2_PORT_OFFSET";
    refused(&r[3], "nooffset", "refused: no port block without locks", NULL);
    r[4].members = one;
    r[4].n = 1;
    r[4].accel = "fast";
    refused(&r[4], "accel", "refused: a bad accelerator", NULL);
    r[5].members = one;
    r[5].n = 1;
    r[5].ledger = "ledger";
    refused(&r[5], "relative", "refused: a relative ledger", NULL);
}

/** Remove the guest cases' evidence under the tree's build/tests. */
static void remove_guest_evidence(void)
{
    char cmd[PATH_MAX + 16];

    if (guest_evidence[0] == '\0')
        return;
    snprintf(cmd, sizeof(cmd), "rm -rf '%s'", guest_evidence);
    if (system(cmd) != 0)
        printf("etude-legacyset-test: %s could not be removed\n", guest_evidence);
}

/** End the test at a signal, through its exit's cleanup. */
static void on_signal(int sig)
{
    (void)sig;
    exit(1);
}

/**
 * Make the wrapper of the QEMU `qemu`, `guest_evidence/bin`, which appends
 * its arguments to `guest_evidence/qemu.argv` and runs `qemu` in its place;
 * 0, or -1.
 */
static int guest_wrapper(const char *qemu)
{
    char path[PATH_MAX + 32], text[PATH_MAX * 2 + 128];
    const char *name = strrchr(qemu, '/');

    fix_format(guest_bin, sizeof(guest_bin), "%s/bin", guest_evidence);
    fix_format(path, sizeof(path), "%s/%s", guest_bin, name != NULL ? name + 1 : qemu);
    fix_format(text, sizeof(text),
               "#!/bin/sh\nprintf '%%s\\n' \"$*\" >> '%s/qemu.argv'\nexec '%s' \"$@\"\n",
               guest_evidence, qemu);
    return mkdir(guest_bin, 0755) != 0 || write_text(path, text, 0755) != 0 ? -1 : 0;
}

/**
 * Find what the guest cases need, before the fixture's path hides QEMU: the
 * architecture ARCH names, i686 when it is unset, QEMU on the path, the
 * tree's build of the kernel, the test image, etude-guest and rngprobe for
 * it, and a directory of their evidence under the tree's build/tests, which
 * the test's exit removes; 0, or -1 after a line of what is missing.
 */
static int guest_site(void)
{
    static const char *const needed[] = {"rach2/kernel", "images/test.krffs",
                                         "tools/etude/guest/etude-guest", "tests/rngprobe"};
    const char *arch = getenv("ARCH"), *path = getenv("PATH"), *shims = getenv("RACH2_QEMU_SHIMS");
    const char *qemu, *end;
    char file[PATH_MAX], real_qemu[PATH_MAX];
    size_t i, len;
    int found = 0;

    guest_arch = arch != NULL && strcmp(arch, "x86_64") == 0 ? "x86_64" : "i686";
    qemu = strcmp(guest_arch, "x86_64") == 0 ? "qemu-system-x86_64" : "qemu-system-i386";
    for (; !found && path != NULL && *path != '\0'; path = *end == ':' ? end + 1 : end) {
        end = strchr(path, ':');
        if (end == NULL)
            end = path + strlen(path);
        len = (size_t)(end - path);
        if (len == 0 || (shims != NULL && len == strlen(shims) && strncmp(path, shims, len) == 0))
            continue;
        snprintf(file, sizeof(file), "%.*s/%s", (int)len, path, qemu);
        found = access(file, X_OK) == 0;
    }
    if (!found) {
        printf("etude-legacyset-test: fail: cannot run the guest cases: no %s on PATH\n", qemu);
        return -1;
    }
    fix_format(real_qemu, sizeof(real_qemu), "%s", file);
    if (realpath(strcmp(guest_arch, "x86_64") == 0 ? "build/x86_64" : "build", guest_build) == NULL)
        guest_build[0] = '\0';
    for (i = 0; i < sizeof(needed) / sizeof(needed[0]); i++) {
        snprintf(file, sizeof(file), "%s/%s", guest_build, needed[i]);
        if (guest_build[0] == '\0' || access(file, R_OK) != 0) {
            printf("etude-legacyset-test: fail: cannot run the guest cases: %s is not built\n",
                   file);
            return -1;
        }
    }
    snprintf(file, sizeof(file), "build/tests/etude-legacyset.XXXXXX");
    if ((mkdir("build/tests", 0755) != 0 && errno != EEXIST) || mkdtemp(file) == NULL ||
        realpath(file, guest_evidence) == NULL) {
        printf("etude-legacyset-test: fail: build/tests/etude-legacyset.XXXXXX cannot be made\n");
        return -1;
    }
    atexit(remove_guest_evidence);
    if (guest_wrapper(real_qemu) != 0) {
        printf("etude-legacyset-test: fail: the wrapper of %s cannot be made\n", real_qemu);
        return -1;
    }
    signal(SIGTERM, on_signal);
    signal(SIGINT, on_signal);
    return 0;
}

int main(int argc, char **argv)
{
    char held[32];

    if (argc >= 2 && (strcmp(argv[1], "prepare") == 0 || strcmp(argv[1], "run") == 0 ||
                      strcmp(argv[1], "check") == 0))
        return as_legacy(argc - 1, argv + 1);
    signal(SIGPIPE, SIG_IGN);
    if (realpath(argv[0], self) == NULL || realpath("tools/etude/schemas", schemas) == NULL) {
        printf("etude-legacyset-test: fail: this program or etude's schemas cannot be found\n");
        return 1;
    }
    if (realpath("build/tools/etude/casecheck", casecheck) == NULL) {
        printf("etude-legacyset-test: fail: build/tools/etude/casecheck is not built\n");
        return 1;
    }
    if (guest_site() != 0)
        return 1;
    snprintf(held, sizeof(held), "%ld", (long)getpid());
    setenv("RACH2_TREE_HELD", held, 1);
    setenv("RACH2_PORT_OFFSET", "7000", 1);
    setenv("RACH2_QEMU_ACCEL", "tcg", 1);
    setenv("RACH2_CPUS", "1", 1);
    setenv("ARCH", "i686", 1);
    setenv("ETUDE_SEED", "99", 1);
    unsetenv("RACH2_DEADLINES");
    unsetenv("FIXTURE_BREAK");
    make_tree();
    test_mixed();
    test_ports();
    test_width();
    test_serial();
    test_first();
    test_stops();
    test_signal();
    test_prepare();
    test_large();
    test_refused();
    test_cases();
    test_cases64();
    test_cases_mixed();
    test_cases_stops();
    test_cases_beside();
    test_cases_refused();
    test_guest();
    if (fails != 0)
        return 1;
    fix_passed();
    printf("etude_legacyset_hosttest: pass\n");
    return 0;
}
