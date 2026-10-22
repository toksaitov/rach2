/**
 * Check etude's set driver (etude_legacy_set in tools/etude/posix/legacy_set.c)
 * on the suites of a fixture tree: case manifests, a Makefile that builds
 * their programs and artifacts, and a stand-in for QEMU first on the path.
 * This program holds the tree's run lock and is the driver's etude-legacy:
 * run as `prepare` or `check`, it does what etude-legacy does. Each set runs
 * in a child of its own, with a private ledger, and with `locks` a private
 * directory of the old runner's locks. The fixture's host cases run one
 * shell script of the tree's, which its arguments make pass, fail, end its
 * report early, outrun its bound, meet another case, note its start and end,
 * or wait, and the built casecheck judges them. Its
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
 * host-deps: tools/etude/posix/bridge.c tools/etude/portlock.c
 * host-deps: tools/etude/posix/status.c
 * host-deps: tools/etude/compare.c tools/etude/baseline.c
 * host-deps: tools/etude/compare_report.c tools/etude/compare_run.c
 * host-deps: tools/etude/history.c tools/etude/history_json.c
 * host-deps: tools/etude/history_collect.c utils/sha256.c
 * host-cflags: -I tools/etude/include -I tools/etude -I tools/etude/posix
 * host-cflags: -Wall -Wextra -Werror
 * host-first: it runs for minutes
 * host-arch: its guest cases boot the kernel and test image of ARCH
 *
 * A set of two suites of one case each that pass and one that fails exits 1,
 * with a history the schema accepts of one plan, whose items are the cases
 * alone, one step, an attempt for each case and a run record, the failed
 * case's workspace kept and the others' removed, no claim left in the
 * ledger, each command's output kept, and one line of each case's start. A
 * width of two lets the cases of two suites meet, with `locks` as without,
 * and a width of one keeps them apart. A serial member runs first, with
 * nothing beside it. A failure under `stop_on_fail`, and one of a member that
 * `stops`, cancel the case that runs and the one not started, and say so. A
 * stop signal exits 143 with the running case cancelled and its output
 * printed, the history whole and the ledger empty; while the case runs, the
 * status record has a line of its attempt, and the record is removed at the
 * end. A failed preparation exits 3 with every item cancelled and no attempt.
 * A case that asks for more memory than the machine has runs. A set of no
 * suite, a bad name, a suite named twice, a suite with no manifest, a bad
 * accelerator and a relative ledger are refused, with nothing run. The
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
 * before the set stops. A manifest that cannot be read, of another suite's
 * family, with a case of a
 * domain other than host, guest and machine, that reads its input, needs
 * loop devices or a fixture other than scratch, or has no case on the
 * architecture is refused, and so is a host case that needs ports, a case
 * suite without casecheck, a guest case of the init role, booted by OVMF,
 * ending other than by an exit, or with a disk of no disk fixture, and a
 * machine case with arguments.
 *
 * A guest case boots its own machine of its profile, its program stored to
 * etude-guest from the image and run there: rngprobe passes on the processor
 * `max`, with its branch `trusted` decided by the profile's features, with the
 * program's output, the console kept for casecheck and the plan's domain
 * `qemu`; rngprobe passes on `qemu64` with its branch `fail-closed`; a boot
 * past its stage's bound is stopped and incomplete;
 * each machine is given its profile's processor, which a wrapper of QEMU
 * records; and the run's store of artifacts is removed at its end. The set
 * runs with `-n fixture`: the case whose profile boots the node
 * `rach2/kernel`, which the fixture makes a file that does not boot, boots
 * `rach2-fixture/kernel` in its place and passes, and the cases that boot
 * another node boot it, each kernel told by its digest in the preparation's
 * record and in QEMU's command line. A missing QEMU or image fails the test.
 *
 * A suite of machine cases runs a scenario of the tree's, a shell script that
 * keeps a console in its workspace as the case's identity says, with one
 * argument, its invocation. The invocation gives each artifact's digest in the
 * run's store under the evidence, which the run removes at its end, the case's
 * tool and the stand-in for QEMU found on the path, the accelerator, the
 * x86-64 firmware that `RACH2_OVMF_CODE` names, each step's bound and the
 * cleanup's, and the workspace as its scratch. A clean
 * console passes, and so does one with the critical record that the case's
 * machine declares; a console with a panic fails at the panic's line, and keeps
 * the workspace with its console; a scenario whose report stops short and that
 * exits 1, as one whose machine's transport was lost does, fails; and a
 * scenario that keeps no console is incomplete. Each attempt is placed qemu.
 * A case's invocation gives the local ports it declares, from 2400 past the
 * caller's port block on, none given twice, and a set of machine cases with
 * ports and `locks` is refused.
 * Their invocations, of a run that no site policy admitted, do not permit
 * the lazy boundary memory profile.
 *
 * The kernel, the disk and the scenario that the five machine cases share
 * are each copied to the store once, which the test counts by putting a
 * copy of its own in the driver's place, `legacy_set_put`, with the
 * store's settling time made zero so that a digest is kept as soon as it is
 * read, and every case is given the digest of each and the tree's digests
 * file. A disk changed after its first copy is copied again for the next
 * case: the first case is given its first digest, and the next and those
 * after it the changed disk's. When the disk's first copy fails, its case
 * ends incomplete with the reason and no invocation, the next case copies
 * the disk again, and the cases after it are given that copy's digest.
 *
 * A suite of one machine case, on x86_64, whose machine is of the lazy 72 GiB
 * boundary profile with the case's 4 GiB reservation: given a site policy
 * with the line `boundary_lazy bigmem-boundary-v1`, its invocation permits
 * the profile, its scenario passes, and the ledger's claim holds 4 GiB of
 * memory and not 72 GiB. Given a policy whose line names no profile, no
 * policy, or a ledger whose memory limit is less than 4 GiB, it ends
 * incomplete before its scenario starts, with the reason and no invocation;
 * a policy that cannot be read is refused.
 *
 * A host case that declares `tooling_memory_bytes`, the memory it takes on
 * a host where it runs its work in etude's tooling machine, claims its
 * `memory_bytes` on Linux and its tooling memory on any other host, as the
 * ledger's claim, which the case prints, shows. Its manifest read, the case
 * claims its tooling memory on such a host and its `memory_bytes` on Linux,
 * and a case without tooling memory its `memory_bytes` on both; tooling
 * memory of no bytes is refused. A machine case's manifest read, the case
 * claims its tooling memory and its `memory_bytes` as a host case does, and
 * a guest case's tooling memory is refused.
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
#include "compat/pathmax_host.h"
#include "sha256.h"
#include "etude/artifact.h"
#include "etude/history.h"
#include "etude/ledger.h"
#include "etude/legacy.h"
#include "legacy_cases.h"

/* The set driver's copy of a file to its store, which a host test build of
 * legacy_set.c lets the test replace. */
extern int (*legacy_set_put)(struct etude_store *s, const char *path, const char *expect,
                             char digest[ETUDE_DIGEST_HEX + 1], uint64_t *size);

/* A host test build of store.c's settling time, after which a file's digest
 * may be kept, and its count of the copies it made. */
extern long long etude_store_settle_ns;
extern unsigned long etude_store_copied;

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

/* The bound of a wait for a case to start, and for the status record to
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

/* The fixture's Makefile: the components, which fail when FIXTURE_BREAK is
 * set, the machine cases' disk, and the cases' programs, each its script
 * under cases/ copied to build/cases or build/x86_64/cases. */
static const char makefile[] =
    "ARCH ?= i686\n"
    "ifeq ($(ARCH),x86_64)\n"
    "ARCH_BUILD := build/x86_64\n"
    "else\n"
    "ARCH_BUILD := build\n"
    "endif\n"
    "IMAGES := $(ARCH_BUILD)/images\n"
    ".PHONY: components\n"
    "components: $(ARCH_BUILD)/rach2/kernel\n"
    "\t@if [ -n \"$$FIXTURE_BREAK\" ]; then echo broken components; exit 4; fi\n"
    "$(ARCH_BUILD)/rach2/kernel:\n"
    "\t@mkdir -p $(@D) && echo kernel > $@\n"
    "$(IMAGES)/disk.img:\n"
    "\t@mkdir -p $(@D) && echo disk > $@\n"
    "build/cases/%: cases/%.sh\n"
    "\t@mkdir -p $(@D) && cp $< $@ && chmod 755 $@\n"
    "build/x86_64/cases/%: cases/%.sh\n"
    "\t@mkdir -p $(@D) && cp $< $@ && chmod 755 $@\n";

/* The program of the fixture's host cases. Its first argument says what it
 * does before its report ends: `pass`; `fail`, a failed assertion; `cut`, a
 * report with no end; `tick`, progress for 4.5 s; `slow`, progress for
 * 11.7 s; `plain`, a line of output; `long`, eight seconds of nothing;
 * `meet SELF OTHER`, a wait of three seconds at most for the other case to
 * be present, and a note in the tree of whether it met it; `mark NAME
 * SECONDS`, notes in the tree of its start and its end; or `hold`, a word
 * through the tree's FIFO that it has started and a wait of fifteen seconds
 * at most. It prints how it was run and where, and leaves a file in
 * $TMPDIR. */
static const char probe[] =
    "#!/bin/sh\n"
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
    "plain) echo 'plain ran'; echo \"@etude 1 assert ran pass\" ;;\n"
    "long) echo 'long running'; sleep 8; echo \"@etude 1 assert ran pass\" ;;\n"
    "meet)\n"
    "    m=$ETUDE_TREE/meet\n"
    "    touch \"$m/$2.here\"\n"
    "    i=0\n"
    "    until [ -e \"$m/$3.here\" ] || [ $i -ge 30 ]; do\n"
    "        sleep 0.1\n"
    "        i=$((i + 1))\n"
    "    done\n"
    "    if [ -e \"$m/$3.here\" ]; then w=met; else w=alone; fi\n"
    "    sleep 0.3\n"
    "    rm -f \"$m/$2.here\"\n"
    "    echo \"$2 $w\" >> \"$m/log\"\n"
    "    echo \"@etude 1 assert ran pass\" ;;\n"
    "mark)\n"
    "    echo \"start $2\" >> \"$ETUDE_TREE/marks\"\n"
    "    sleep \"$3\"\n"
    "    echo \"end $2\" >> \"$ETUDE_TREE/marks\"\n"
    "    echo \"@etude 1 assert ran pass\" ;;\n"
    "claim) sed -n 's/^resource memory_bytes /ledger: &/p' \"$FIXTURE_LEDGER/claims\"\n"
    "    echo \"@etude 1 assert ran pass\" ;;\n"
    "hold)\n"
    "    echo holding\n"
    "    echo started > \"$ETUDE_TREE/started.fifo\"\n"
    "    i=0\n"
    "    until [ -e \"$ETUDE_TREE/release\" ] || [ $i -ge 150 ]; do\n"
    "        sleep 0.1\n"
    "        i=$((i + 1))\n"
    "    done\n"
    "    echo \"@etude 1 assert ran pass\" ;;\n"
    "*) echo \"@etude 1 assert ran pass\" ;;\n"
    "esac\n"
    "echo \"@etude 1 end 1\"\n";

/* The scenario of the fixture's machine cases. It takes the scratch from
 * its invocation, keeps the console `evidence/vm.1.console` there and
 * reports, as its case's identity says: a clean console, one with a panic,
 * one with the record of init's death that the case's machine declares, a
 * report that stops short with the exit status 1, as a lost transport gives,
 * or no console at all. The boundary case also prints the memory of the
 * claims in the ledger that FIXTURE_LEDGER names. */
static const char scenario[] =
    "#!/bin/sh\n"
    "echo \"scenario: $0 $*\"\n"
    "scratch=$(sed -n 's/.*\"scratch\":\"\\([^\"]*\\)\".*/\\1/p' \"$1\")\n"
    "mkdir -p \"$scratch/evidence\"\n"
    "c=$scratch/evidence/vm.1.console\n"
    "echo 'Rach2 kernel booting' > \"$c\"\n"
    "echo \"@etude 1 begin $ETUDE_CASE\"\n"
    "case $ETUDE_CASE in\n"
    "*.panic) echo '<2>[    0.500000] panic: page fault' >> \"$c\" ;;\n"
    "*.allowed) echo '<2>[    1.250000] init: exited with status 1' >> \"$c\" ;;\n"
    "*.lost) echo '@etude 1 progress stage 1'; exit 1 ;;\n"
    "*.noconsole) rm -r \"$scratch/evidence\" ;;\n"
    "*.accounting) sed -n 's/^resource memory_bytes /ledger: &/p' \"$FIXTURE_LEDGER/claims\" ;;\n"
    "esac\n"
    "echo '@etude 1 assert booted pass'\n"
    "echo '@etude 1 progress stage 1'\n"
    "echo '@etude 1 end 1'\n";

/* The form of a machine case of the fixture, with its identity, its
 * machine's `critical` member, or nothing, and its local ports. */
static const char machine_form[] =
    "{\"case_id\": \"%s\", \"behavior\": \"The fixture's scenario keeps a console as its case "
    "says\",\n"
    " \"units\": [\"tests/host/etude_legacyset_hosttest.c\"], \"domain\": \"machine\",\n"
    " \"architectures\": [\"i686\"],\n"
    " \"workload\": {\"arguments\": [], \"inputs\": {}, \"repetitions\": 1, \"seed\": null},\n"
    " \"requirements\": {\"host_api\": \"posix\", \"tools\": [\"sh\"], \"features\": {}},\n"
    " \"execution\": {\"program\": \"scenario\", \"stdin\": null, \"role\": \"ordinary\",\n"
    "  \"privileges\": \"an ordinary user\", \"isolation\": [{\"fixture\": \"scratch\"},\n"
    "  {\"fixture\": \"disk\", \"names\": [\"disk\"]}], \"children\": \"none\",\n"
    "  \"cleanup\": \"nothing outside its scratch directory\"},\n"
    " \"steps\": [{\"step_id\": \"boot\", \"stage\": \"execute\", \"machine\": \"vm\",\n"
    "  \"does\": \"boot the machine and keep its console\"}],\n"
    " \"machines\": [{\"machine_id\": \"vm\", \"qemu\": {\"kernel\": \"kernel\",\n"
    "  \"initrd\": null,"
    "  \"disks\": [\"disk\"], \"cpu\": \"max\", \"processors\": 1, \"memory_bytes\": 134217728,\n"
    "  \"firmware\": \"direct\", \"devices\": [], \"boot_options\": []}, \"console\": \"serial\",\n"
    "  \"qmp\": true, \"helper\": false%s}],\n"
    " \"assertions\": [{\"assertion_id\": \"booted\", \"checks\": \"the machine booted\",\n"
    "  \"source\": \"scenario\"}],\n"
    " \"branches\": [], \"termination\": {\"end\": \"exited\", \"value\": 0},\n"
    " \"progress\": [{\"witness_id\": \"stage\", \"kind\": \"step\", \"means\": \"a step has "
    "ended\",\n"
    "  \"limit\": 1}],\n"
    " \"bounds\": {\"case\": {\"floor_us\": 100000, \"initial_us\": 30000000, \"ceiling_us\": "
    "30000000},\n"
    "  \"gap\": {\"floor_us\": 100000, \"initial_us\": 20000000, \"ceiling_us\": 20000000},\n"
    "  \"stages\": {\"boot\": {\"floor_us\": 100000, \"initial_us\": 20000000, \"ceiling_us\": "
    "20000000},\n"
    "  \"cleanup\": {\"floor_us\": 100000, \"initial_us\": 7000000, \"ceiling_us\": "
    "7000000}}},\n"
    " \"resources\": {\"cpu_millis\": 100, \"cpu_ceiling_millis\": 1000, \"memory_bytes\": "
    "268435456,\n"
    "  \"disk_bytes\": 0, \"locks\": [], \"ports\": %u, \"loop_devices\": 0},\n"
    " \"prepare\": [{\"name\": \"kernel\", \"node\": \"rach2/kernel\", \"per_architecture\": "
    "true},\n"
    "  {\"name\": \"disk\", \"node\": \"images/disk.img\", \"per_architecture\": true},\n"
    "  {\"name\": \"scenario\", \"node\": \"cases/scenario\", \"per_architecture\": false}],\n"
    " \"timing\": [], \"native\": {\"plan\": \"none\", \"class\": \"boot\",\n"
    "  \"reason\": \"the fixture's machine is a stand-in\"}}";

/* The machine cases of the family `machine`, each with its machine's
 * `critical` member and its local ports. */
static const struct {
    const char *id, *critical;
    unsigned ports;
} machine_cases[] = {{"fixture.machine.clean", "", 2},
                     {"fixture.machine.panic", "", 0},
                     {"fixture.machine.allowed", ", \"critical\": \"init: exited\"", 1},
                     {"fixture.machine.lost", "", 0},
                     {"fixture.machine.noconsole", "", 0}};

/* The disk that the machine cases share, its text as the fixture's Makefile
 * makes it, and the text a row gives it after its first copy. */
#define DISK "build/images/disk.img"
static const char disk_text[] = "disk\n";
static const char disk_changed[] = "disk changed after its first copy\n";

/* The family `boundary`: one machine case of a machine of the lazy 72 GiB
 * boundary profile on x86-64, with the boundary's 4 GiB reservation, which
 * the fixture's scenario runs. */
static const char boundary_manifest[] =
    "{\"schema_version\": 1, \"family_id\": \"fixture.boundary\",\n"
    " \"summary\": \"A boundary machine case of the set driver's fixture\",\n \"cases\": [\n"
    "{\"case_id\": \"fixture.boundary.accounting\", \"behavior\": \"The fixture's scenario "
    "keeps a console in place of the boundary boot\",\n"
    " \"units\": [\"tests/host/etude_legacyset_hosttest.c\"], \"domain\": \"machine\",\n"
    " \"architectures\": [\"x86_64\"],\n"
    " \"workload\": {\"arguments\": [], \"inputs\": {}, \"repetitions\": 1, \"seed\": null},\n"
    " \"requirements\": {\"host_api\": \"posix\", \"tools\": [\"sh\"], \"features\": {}},\n"
    " \"execution\": {\"program\": \"scenario\", \"stdin\": null, \"role\": \"ordinary\",\n"
    "  \"privileges\": \"an ordinary user\", \"isolation\": [{\"fixture\": \"scratch\"}],\n"
    "  \"children\": \"none\", \"cleanup\": \"nothing outside its scratch directory\"},\n"
    " \"steps\": [{\"step_id\": \"boot\", \"stage\": \"execute\", \"machine\": \"vm\",\n"
    "  \"does\": \"boot the machine and keep its console\"}],\n"
    " \"machines\": [{\"machine_id\": \"vm\", \"qemu\": {\"kernel\": \"kernel\",\n"
    "  \"initrd\": \"initrd\", \"disks\": [], \"cpu\": \"max-nopdpe1gb\", \"processors\": 1,\n"
    "  \"memory_bytes\": 77309411328, \"memory_profile\": \"bigmem-boundary-v1\",\n"
    "  \"firmware\": \"direct\", \"devices\": [], \"boot_options\": [\"rach2.rescue\"]},\n"
    "  \"console\": \"serial\", \"qmp\": true, \"helper\": false}],\n"
    " \"assertions\": [{\"assertion_id\": \"booted\", \"checks\": \"the machine booted\",\n"
    "  \"source\": \"scenario\"}],\n"
    " \"branches\": [], \"termination\": {\"end\": \"exited\", \"value\": 0},\n"
    " \"progress\": [{\"witness_id\": \"stage\", \"kind\": \"step\", \"means\": \"a step has "
    "ended\",\n"
    "  \"limit\": 1}],\n"
    " \"bounds\": {\"case\": {\"floor_us\": 100000, \"initial_us\": 30000000, \"ceiling_us\": "
    "30000000},\n"
    "  \"gap\": {\"floor_us\": 100000, \"initial_us\": 20000000, \"ceiling_us\": 20000000},\n"
    "  \"stages\": {\"boot\": {\"floor_us\": 100000, \"initial_us\": 20000000, \"ceiling_us\": "
    "20000000},\n"
    "  \"cleanup\": {\"floor_us\": 100000, \"initial_us\": 7000000, \"ceiling_us\": "
    "7000000}}},\n"
    " \"resources\": {\"cpu_millis\": 100, \"cpu_ceiling_millis\": 1000, \"memory_bytes\": "
    "4294967296,\n"
    "  \"disk_bytes\": 0, \"guest_memory\": {\"configured_bytes\": 77309411328,\n"
    "  \"resident_bytes\": 2147483648, \"backing\": \"lazy\"}, \"locks\": [], \"ports\": 0,\n"
    "  \"loop_devices\": 0},\n"
    " \"prepare\": [{\"name\": \"kernel\", \"node\": \"rach2/kernel\", \"per_architecture\": "
    "true},\n"
    "  {\"name\": \"initrd\", \"node\": \"images/disk.img\", \"per_architecture\": true},\n"
    "  {\"name\": \"scenario\", \"node\": \"cases/scenario\", \"per_architecture\": false}],\n"
    " \"timing\": [], \"native\": {\"plan\": \"none\", \"class\": \"boot\",\n"
    "  \"reason\": \"the fixture's machine is a stand-in\"}}]}\n";

/* The site policies a set may be given: one that permits the lazy boundary
 * profile, and one whose permission names no profile the driver knows. */
static const char site_permits[] = "etude-site 1\nendpoint linux\n"
                                   "boundary_lazy bigmem-boundary-v1\nwait 3\n";
static const char site_other[] = "etude-site 1\nendpoint linux\nboundary_lazy yes\nwait 3\n";

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
    "%u000},\n"
    "  \"gap\": {\"floor_us\": 100000, \"initial_us\": %u000, \"ceiling_us\": %u000},\n"
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

/* The families of one host case each: the family `fixture.SUITE` of the
 * suite SUITE, whose case `fixture.SUITE.one`, in the manifest ONE(SUITE),
 * gives the program the arguments `args`, the items of a JSON array. */
#define ONE(suite) "cases/one/" suite ".json"
static const struct {
    const char *suite, *args;
} ones[] = {{"plain", "\"plain\""},
            {"fail", "\"fail\""},
            {"long", "\"long\""},
            {"hold", "\"hold\""},
            {"meet-a", "\"meet\", \"meet-a\", \"meet-b\""},
            {"meet-b", "\"meet\", \"meet-b\", \"meet-a\""},
            {"serial", "\"mark\", \"serial\", \"1\""},
            {"side-x", "\"mark\", \"side-x\", \"0.2\""},
            {"side-y", "\"mark\", \"side-y\", \"0.2\""}};

/* The resources of the case of the family `tooling`, which claims 16 MiB on
 * Linux and 3200 MiB on a host where it runs its work in the tooling
 * machine, and of its manifest `none.json`, whose tooling memory is none. */
#define TOOLING_FROM "\"memory_bytes\": 16777216,"
#define TOOLING_TO   "\"memory_bytes\": 16777216, \"tooling_memory_bytes\": 3355443200,"
#define TOOLING_NONE "\"memory_bytes\": 16777216, \"tooling_memory_bytes\": 0,"

/* The resources of a machine case of the family `machine` and of the guest
 * case of `badguest`, and the same with 3456 MiB of tooling memory, the
 * manifests `machine.json` and `guest.json` of cases/tooling. */
#define TOOLING_CASE    "\"memory_bytes\": 268435456,"
#define TOOLING_CASE_TO "\"memory_bytes\": 268435456, \"tooling_memory_bytes\": 3623878656,"

/* The case of `bad` that runs on x86_64 alone. */
static const struct fcase other_cases[] = {
    {"fixture.bad.one", "[\"x86_64\"]", "\"pass\"", 1, "null", 1, 30000, 30000, 0}};

/* The manifests of `bad` that the driver refuses: each the case of `bad`
 * with the text `from` made `to`, or the case of `other` for `noarch`. */
static const struct {
    const char *name, *from, *to;
} refusals[] = {{"domain", "\"domain\": \"host\"", "\"domain\": \"native\""},
                {"stdin", "\"stdin\": null", "\"stdin\": \"input\""},
                {"ports", "\"ports\": 0", "\"ports\": 1"},
                {"loops", "\"loop_devices\": 0", "\"loop_devices\": 1"},
                {"fixture", "{\"fixture\": \"scratch\"}", "{\"fixture\": \"network\"}"},
                {"machineargs", "\"domain\": \"host\"", "\"domain\": \"machine\""},
                {"noarch", NULL, NULL}};

/* The form of a guest case of a fixture manifest: the tree's rngprobe on
 * the test image, under the processor profile `cpu`, with its boot stage
 * bounded in milliseconds and its kernel built as a node, which casecheck
 * accepts. It reserves a port and a loop device for its native run, which
 * the driver allows a guest case whose profile forwards no port. */
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
    "120000000},\n"
    "  \"gap\": {\"floor_us\": 100000, \"initial_us\": 60000000, \"ceiling_us\": 60000000},\n"
    "  \"stages\": {\"boot\": {\"floor_us\": 100000, \"initial_us\": %u000, \"ceiling_us\": "
    "%u000}}},\n"
    " \"resources\": {\"cpu_millis\": 1000, \"cpu_ceiling_millis\": 1000, \"memory_bytes\": "
    "268435456,\n"
    "  \"disk_bytes\": 0, \"locks\": [], \"ports\": 1, \"loop_devices\": 1},\n"
    " \"prepare\": [{\"name\": \"rngprobe\", \"node\": \"guest/rngprobe\", \"per_architecture\": "
    "true},\n"
    "  {\"name\": \"kernel\", \"node\": \"%s\", \"per_architecture\": true},\n"
    "  {\"name\": \"etude-guest\", \"node\": \"guest/etude-guest\", \"per_architecture\": true},\n"
    "  {\"name\": \"test-image\", \"node\": \"guest/test.krffs\", \"per_architecture\": true}],\n"
    " \"timing\": [], \"native\": {\"plan\": \"native\", \"fixtures\": [{\"fixture\": "
    "\"workspace\"},\n"
    "  {\"fixture\": \"ports\", \"count\": 1}, {\"fixture\": \"volume\", \"type\": \"krffs\"}]}}";

/* A guest case of a fixture manifest, which guest_text writes. */
struct gcase {
    const char *id;
    const char *cpu;  /* its processor profile */
    unsigned boot_ms; /* the bound of its boot */
    const char *node; /* the node of its kernel */
};

/* The family `guest`: rngprobe with and without a random source, a boot
 * bounded far below what the boot takes, and rngprobe on the kernel of the
 * node `rach2/kernel`, which `-n` replaces. */
static const struct gcase guest_cases[] = {
    {"fixture.guest.rng", "max", 60000, "guest/kernel"},
    {"fixture.guest.closed", "qemu64", 60000, "guest/kernel"},
    {"fixture.guest.boot", "qemu64", 200, "guest/kernel"},
    {"fixture.guest.named", "qemu64", 60000, "rach2/kernel"}};

/* The family `badguest`, whose one case the refused manifests change. */
static const struct gcase badguest_cases[] = {
    {"fixture.badguest.one", "max", 60000, "guest/kernel"}};

/* The manifests of `badguest` that the driver refuses, each its case with
 * the text `from` made `to`. */
static const struct {
    const char *name, *from, *to;
} guest_refusals[] = {
    {"init", "\"role\": \"ordinary\"", "\"role\": \"init\""},
    {"ovmf", "\"firmware\": \"direct\"", "\"firmware\": \"ovmf\""},
    {"end", "\"end\": \"exited\", \"value\": 0", "\"end\": \"shutdown\", \"value\": null"},
    {"disk", "\"disks\": []", "\"disks\": [\"etude-guest\"]"},
    {"forwards", "\"devices\": [\"rtl8139\"]",
     "\"devices\": [{\"model\": \"rtl8139\", \"forwards\": [{\"protocol\": \"tcp\", "
     "\"port\": 0, \"guest_port\": 22}]}]"}};

/* The stand-in for QEMU, which the machine cases' invocations name. */
static const char standin[] = "#!/bin/sh\n"
                              "echo FIXTURE-BOOT-OK\n"
                              "sleep 0.2\n";

/* The records of a history, each a line of the stream. */
struct history {
    char **line;
    size_t n, cap;
};

/* The fault a row puts into the driver's copies to its store: none, the
 * first copy of the disk failing, or the disk changed after its first copy. */
enum put_fault { PUT_NONE, PUT_FAIL, PUT_CHANGE };

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
    const char *kernel; /* the NAME of `-n NAME`, or NULL */
    const char *site;   /* the site's policy, or NULL for none */
    unsigned limit_mib; /* the ledger's memory limit, or 0 for MEMORY_MIB */
    int nojudge;        /* the driver is given no casecheck */
    int guest;          /* it runs guest cases, on the real QEMU */
    enum put_fault put; /* the fault of the driver's copies */
    pid_t pid;
    int status;  /* the driver's, or -1 when it did not end by itself */
    long claims; /* the claims the ledger kept after it, or -1 */
    int leaks;   /* LeakSanitizer found a leak of the driver */
    char results[PATH_LEN], evidence[PATH_LEN], status_file[PATH_LEN];
    char out_file[PATH_LEN], side[PATH_LEN];
    char copies[PATH_LEN]; /* the path of each copy to the store, a line each */
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

/** Give the outcome of the item of the first repetition of case `id` on
 * i686 in the run record in `out`, or "" when there is none. */
static const char *item_of(const struct run *r, const char *id, char *out, size_t size)
{
    char key[160];
    size_t i;

    out[0] = '\0';
    fix_format(key, sizeof(key), ":%s:i686:r0\",\"attempt_records\":[", id);
    for (i = 0; i < r->h.n; i++)
        if (strstr(r->h.line[i], "\"kind\":\"run\"") != NULL)
            outcome_after(r->h.line[i], key, "\"outcome\":\"", out, size);
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

/** Say whether the run record's item of case `id` has outcome `want`. */
static int item_is(const struct run *r, const char *id, const char *want)
{
    char got[32];

    return strcmp(item_of(r, id, got, sizeof(got)), want) == 0;
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
 * Do what etude-legacy does with `argv`, its arguments from `prepare` or
 * `check` on: the options the set driver gives it, parsed as
 * tools/etude/posix/etude_legacy.c parses them.
 *
 * @return its status, or 2 for a wrong usage.
 */
static int as_legacy(int argc, char **argv)
{
    static char *goals[64];
    struct etude_legacy_opts p;
    int opt;

    setvbuf(stdout, NULL, _IOLBF, 0);
    catch_stops();
    if (strcmp(argv[0], "check") == 0)
        return argc == 2 ? etude_legacy_check(argv[1], 0, stdout) : 2;
    memset(&p, 0, sizeof(p));
    p.tree = ".";
    p.seconds = ETUDE_LEGACY_SECONDS;
    p.out = stdout;
    p.stop = &stop_signal;
    p.goals = goals;
    while ((opt = getopt(argc, argv, "t:a:n:w:o:s:b:")) != -1) {
        if (opt == 'b' && p.ngoals < sizeof(goals) / sizeof(goals[0]))
            goals[p.ngoals++] = optarg;
        else if (opt == 't')
            p.tree = optarg;
        else if (opt == 'a')
            p.arch = optarg;
        else if (opt == 'n')
            p.kernel = optarg;
        else if (opt == 'w')
            p.work = optarg;
        else if (opt == 'o')
            p.record = optarg;
        else if (opt == 's' && (p.seconds = seconds_of(optarg)) != 0)
            continue;
        else
            return 2;
    }
    if (strcmp(argv[0], "prepare") == 0 && optind == argc && p.ngoals > 0)
        return etude_legacy_prepare(&p);
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
                     c[i].case_ms, c[i].gap_ms, c[i].gap_ms, c[i].per_arch ? "true" : "false");
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
        k = snprintf(buf + len, size - len, guest_form, c[i].id, c[i].cpu, c[i].boot_ms,
                     c[i].boot_ms, c[i].node);
        k = k < 0 ? k : (int)len + k;
        if (k >= 0 && (size_t)k < size && i + 1 < n)
            k += snprintf(buf + k, size - (size_t)k, ",\n");
    }
    if (k >= 0 && (size_t)k < size)
        k += snprintf(buf + k, size - (size_t)k, "]}\n");
    return k >= 0 && (size_t)k < size ? 0 : -1;
}

/** Write in `buf` the manifest of the family `machine`; 0, or -1 when it
 * does not fit. */
static int machine_text(char *buf, size_t size)
{
    size_t i, n = sizeof(machine_cases) / sizeof(machine_cases[0]);
    int k;

    k = snprintf(buf, size,
                 "{\"schema_version\": 1, \"family_id\": \"fixture.machine\",\n"
                 " \"summary\": \"Machine cases of the set driver's fixture\",\n \"cases\": [");
    for (i = 0; k >= 0 && (size_t)k < size && i < n; i++) {
        k += snprintf(buf + k, size - (size_t)k, machine_form, machine_cases[i].id,
                      machine_cases[i].critical, machine_cases[i].ports);
        if (k >= 0 && (size_t)k < size)
            k += snprintf(buf + k, size - (size_t)k, i + 1 < n ? ",\n" : "]}\n");
    }
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
 * casecheck's verdict reads them, the families of one case in cases/one,
 * the family `tooling` of one case with tooling memory, a machine and a
 * guest family with tooling memory beside it, and the refused ones of `bad`
 * and `badguest`; 0, or -1. */
static int make_cases(void)
{
    static char text[32768], changed[32768];
    char path[PATH_LEN], name[64], family[64], id[64];
    struct fcase one = {NULL, "[\"i686\"]", NULL, 1, "null", 1, 30000, 30000, 0};
    size_t i;
    int bad = 0;

    bad |= write_text(in(path, sizeof(path), "cases/probe.sh"), probe, 0755);
    bad |= write_text(in(path, sizeof(path), "cases/scenario.sh"), scenario, 0755);
    bad |= machine_text(text, sizeof(text)) != 0 ||
           write_text(in(path, sizeof(path), "cases/machine/machine.json"), text, 0644) != 0;
    bad |=
        write_text(in(path, sizeof(path), "cases/boundary/boundary.json"), boundary_manifest, 0644);
    bad |= manifest_text(text, sizeof(text), "fixture.good", good_cases, 4) != 0 ||
           write_text(in(path, sizeof(path), "cases/good/good.json"), text, 0644) != 0;
    bad |= manifest_text(text, sizeof(text), "fixture.mixed", mixed_cases, 4) != 0 ||
           write_text(in(path, sizeof(path), "cases/mixed/mixed.json"), text, 0644) != 0;
    for (i = 0; i < sizeof(ones) / sizeof(ones[0]); i++) {
        fix_format(family, sizeof(family), "fixture.%s", ones[i].suite);
        fix_format(id, sizeof(id), "fixture.%s.one", ones[i].suite);
        fix_format(name, sizeof(name), "cases/one/%s.json", ones[i].suite);
        one.id = id;
        one.args = ones[i].args;
        bad |= manifest_text(text, sizeof(text), family, &one, 1) != 0 ||
               write_text(in(path, sizeof(path), name), text, 0644) != 0;
    }
    one.id = "fixture.tooling.one";
    one.args = "\"claim\"";
    bad |= manifest_text(text, sizeof(text), "fixture.tooling", &one, 1) != 0 ||
           replaced(changed, sizeof(changed), text, TOOLING_FROM, TOOLING_TO) != 0 ||
           write_text(in(path, sizeof(path), "cases/tooling/tooling.json"), changed, 0644) != 0 ||
           replaced(text, sizeof(text), changed, TOOLING_TO, TOOLING_NONE) != 0 ||
           write_text(in(path, sizeof(path), "cases/tooling/none.json"), text, 0644) != 0;
    bad |= machine_text(text, sizeof(text)) != 0 ||
           replaced(changed, sizeof(changed), text, TOOLING_CASE, TOOLING_CASE_TO) != 0 ||
           write_text(in(path, sizeof(path), "cases/tooling/machine.json"), changed, 0644) != 0;
    bad |= guest_text(text, sizeof(text), "fixture.badguest", badguest_cases, 1) != 0 ||
           replaced(changed, sizeof(changed), text, TOOLING_CASE, TOOLING_CASE_TO) != 0 ||
           write_text(in(path, sizeof(path), "cases/tooling/guest.json"), changed, 0644) != 0;
    bad |= manifest_text(text, sizeof(text), "fixture.bad", bad_cases, 1);
    for (i = 0; i < sizeof(refusals) / sizeof(refusals[0]); i++) {
        if (refusals[i].from != NULL)
            bad |= replaced(changed, sizeof(changed), text, refusals[i].from, refusals[i].to);
        else
            bad |= manifest_text(changed, sizeof(changed), "fixture.bad", other_cases, 1);
        fix_format(name, sizeof(name), "cases/bad/%s.json", refusals[i].name);
        bad |= write_text(in(path, sizeof(path), name), changed, 0644);
    }
    bad |= guest_text(text, sizeof(text), "fixture.guest", guest_cases,
                      sizeof(guest_cases) / sizeof(guest_cases[0])) != 0 ||
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
 * fixture's preparation makes, and the rule of `-n fixture`'s kernel,
 * `rach2-fixture/kernel`: a copy of the tree's kernel with a line after
 * its end, which no loader reads, so that its digest is its own; 0, or -1
 * when they do not fit.
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
    k = snprintf(out + used, size - used,
                 ".PHONY: fixture-kernel\nfixture-kernel:\n\t@mkdir -p %s/rach2-fixture && "
                 "cp %s/rach2/kernel %s/rach2-fixture/kernel && echo fixture >> "
                 "%s/rach2-fixture/kernel\n",
                 dir, guest_build, dir, dir);
    return k < 0 || (size_t)k >= size - used ? -1 : 0;
}

/** Make the fixture tree, with its run lock held by this process and the
 * stand-in first on the path. */
static void make_tree(void)
{
    static const char *const dirs[] = {"",
                                       "build",
                                       "build/.run.lock",
                                       "meet",
                                       "qemu",
                                       "cases",
                                       "cases/one",
                                       "cases/good",
                                       "cases/mixed",
                                       "cases/bad",
                                       "cases/guest",
                                       "cases/badguest",
                                       "cases/machine",
                                       "cases/boundary",
                                       "cases/tooling"};
    static char source[1 << 20], newpath[8192], rules[8192];
    const char *oldpath = getenv("PATH");
    char path[PATH_LEN], owner[64], dir[PATH_MAX] = "";
    size_t i;
    int bad = 0;

    /* The driver reports the tree by its real path: /private/tmp on macOS. */
    bad |= realpath(fix_dir(), dir) == NULL;
    fix_format(tree, sizeof(tree), "%s/tree", dir);
    fix_format(ledger, sizeof(ledger), "%s/ledger", fix_dir());
    fix_format(locks, sizeof(locks), "%s/locks", fix_dir());
    fix_format(path, sizeof(path), "%s/tmp", fix_dir());
    bad |= mkdir(path, 0755) != 0 || setenv("TMPDIR", path, 1) != 0;
    bad |= mkdir(locks, 0755) != 0;
    for (i = 0; i < sizeof(dirs) / sizeof(dirs[0]); i++)
        bad |= mkdir(in(path, sizeof(path), dirs[i]), 0755) != 0;
    bad |= mkfifo(in(path, sizeof(path), "started.fifo"), 0644) != 0;
    snprintf(owner, sizeof(owner), "pid %ld\n", (long)getpid());
    bad |= guest_rules(rules, sizeof(rules)) != 0;
    fix_format(source, sizeof(source), "%s%s", makefile, rules);
    bad |= write_text(in(path, sizeof(path), "Makefile"), source, 0644);
    bad |= write_text(in(path, sizeof(path), "qemu/qemu-system-i386"), standin, 0755);
    bad |= write_text(in(path, sizeof(path), "qemu/qemu-system-x86_64"), standin, 0755);
    bad |= write_text(in(path, sizeof(path), "site.permits"), site_permits, 0644);
    bad |= write_text(in(path, sizeof(path), "site.other"), site_other, 0644);
    bad |= write_text(in(path, sizeof(path), "build/.run.lock/owner"), owner, 0644);
    bad |= make_cases();
    fix_format(real_path, sizeof(real_path), "%s", oldpath != NULL ? oldpath : "/usr/bin:/bin");
    fix_format(newpath, sizeof(newpath), "%s/qemu:%s", tree, real_path);
    bad |= setenv("PATH", newpath, 1) != 0;
    if (bad) {
        printf("etude-legacyset-test: fail: the fixture tree %s was not made\n", tree);
        exit(1);
    }
}

/** Remove the files of the cases that meet and that mark their times. */
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
    o->kernel = r->kernel;
    o->program = self;
    o->casecheck = r->nojudge ? NULL : casecheck;
    o->members = r->members;
    o->nmembers = r->n;
    o->ledger = r->ledger != NULL ? r->ledger : ledger;
    o->account = "fixture";
    o->cap[ETUDE_LEGACY_CPU] = 8000;
    o->cap[ETUDE_LEGACY_MEMORY] = (uint64_t)(r->limit_mib != 0 ? r->limit_mib : MEMORY_MIB) << 20;
    o->cap[ETUDE_LEGACY_DISK] = UINT64_C(64) << 30;
    o->locks = r->locks ? locks : NULL;
    o->wait = 30;
    o->seconds = 60;
    o->width = r->width;
    o->cpus = 1;
    o->accel = r->accel != NULL ? r->accel : "tcg";
    o->stop_on_fail = r->stop_on_fail;
    o->results = r->results;
    o->schemas = schemas;
    o->evidence = r->evidence;
    o->status = r->status_file;
    o->args = r->name;
    o->site = r->site;
}

/* The run whose set this process drives. */
static const struct run *driving;

/** Say whether `path` names the file `name` of the tree: whether it ends in
 * a slash and `name`. */
static int names_file(const char *path, const char *name)
{
    size_t n = strlen(path), k = strlen(name);

    return n > k && path[n - k - 1] == '/' && strcmp(path + n - k, name) == 0;
}

/** Add `path` to the list of copies of the run the driver runs. */
static void count_copy(const char *path)
{
    FILE *f = fopen(driving->copies, "a");

    if (f != NULL) {
        fprintf(f, "%s\n", path);
        fclose(f);
    }
}

/**
 * Copy the file at `path` to the store `s` in the set driver's place, as
 * `legacy_set_put`: for the row's fault, copy the disk the first time from
 * a name that does not exist, so that the copy fails, or change the disk
 * after its first copy, and add `path` to the run's list of copies when the
 * store copied it or failed to.
 */
static int counted_put(struct etude_store *s, const char *path, const char *expect,
                       char digest[ETUDE_DIGEST_HEX + 1], uint64_t *size)
{
    static int disks;
    char absent[PATH_MAX + 8];
    unsigned long copied = etude_store_copied;
    int first = names_file(path, DISK) && disks++ == 0, rc;

    if (first && driving->put == PUT_FAIL) {
        fix_format(absent, sizeof(absent), "%s.absent", path);
        count_copy(path);
        return etude_store_put(s, absent, expect, digest, size);
    }
    rc = etude_store_put(s, path, expect, digest, size);
    if (rc != 0 || etude_store_copied != copied)
        count_copy(path);
    if (first && driving->put == PUT_CHANGE)
        write_text(path, disk_changed, 0644);
    return rc;
}

/**
 * Run the set of `r` as a set's driver, in this child of the test, with its
 * copies to its store counted: write what it printed to `r->out_file`, then
 * the claims the ledger keeps while this process, their owner, still runs
 * and whether LeakSanitizer finds a leak to `r->side`, and exit with the
 * driver's status.
 */
static void driver(const struct run *r)
{
    struct etude_legacy_set_opts o;
    struct etude_ledger *l;
    long claims = -1;
    int status, leaks;
    FILE *f;

    driving = r;
    legacy_set_put = counted_put;
    etude_store_settle_ns = 0;
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
    fix_format(r->copies, sizeof(r->copies), "%s/%s.copies", dir, r->name);
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

/** Say whether the plan of run `r` has the text `text`. */
static int plan_has(const struct run *r, const char *text)
{
    size_t i;

    for (i = 0; i < r->h.n; i++)
        if (strstr(r->h.line[i], "\"kind\":\"plan\"") != NULL)
            return strstr(r->h.line[i], text) != NULL;
    return 0;
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

/** A set of two suites of one case each that pass and one that fails. */
static void test_mixed(void)
{
    static const struct etude_legacy_member m[] = {{"plain", 0, 0, ONE("plain")},
                                                   {"side-x", 0, 0, ONE("side-x")},
                                                   {"fail", 0, 0, ONE("fail")}};
    static const char *const names[] = {"plain", "side-x", "fail"};
    static struct run r;
    char line[PATH_LEN * 2];
    size_t i;
    int before = fails;

    clear_marks();
    r.name = "mixed";
    r.members = m;
    r.n = 3;
    set_run(&r);
    CHECK(r.status == 1, "mixed: failed");
    CHECK(r.read == 0, "mixed: the history is whole");
    CHECK(count_kind(&r.h, "plan") == 1, "mixed: one plan");
    CHECK(plan_has(&r, "fixture.plain.one") && !plan_has(&r, "legacy."),
          "mixed: the plan's items are the cases alone");
    CHECK(count_kind(&r.h, "step") == 1, "mixed: one step");
    CHECK(count_kind(&r.h, "attempt") == 3, "mixed: an attempt of each case");
    CHECK(count_kind(&r.h, "run") == 1, "mixed: one run record");
    CHECK(case_is(&r, "fixture.plain.one", "i686", 0, "pass", NULL), "mixed: plain passed");
    CHECK(case_is(&r, "fixture.side-x.one", "i686", 0, "pass", NULL), "mixed: side-x passed");
    CHECK(case_is(&r, "fixture.fail.one", "i686", 0, "failure", NULL), "mixed: fail failed");
    CHECK(item_is(&r, "fixture.fail.one", "failure"), "mixed: the run record's item of fail");
    CHECK(kept(&r, "fixture.fail.one.r0"), "mixed: the failed case's workspace kept");
    CHECK(!kept(&r, "fixture.plain.one.r0") && !kept(&r, "fixture.side-x.one.r0"),
          "mixed: a passed case's workspace removed");
    CHECK(r.claims == 0, "mixed: the ledger has no claim");
    CHECK(!r.leaks, "mixed: the driver leaks nothing");
    CHECK(output_has(&r, "prepare", NULL), "mixed: the preparation's output kept");
    CHECK(output_has(&r, "fixture.plain.one.r0", "plain ran"), "mixed: the output of plain kept");
    fix_format(line, sizeof(line), "program: %s/build/cases/probe fail", tree);
    CHECK(output_has(&r, "fixture.fail.one.r0", line), "mixed: the output of fail kept");
    CHECK(has_line(r.out, "plain ran") && has_line(r.out, line),
          "mixed: the cases' output printed");
    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        fix_format(line, sizeof(line), "run: %s: fixture.%s.one: starting", names[i], names[i]);
        CHECK(count_line(r.out, line) == 1, "mixed: one line of each case's start");
    }
    CHECK(has_line(r.out, "etude-legacy: result: ran: 3 suites, 2 passed"), "mixed: the result");
    show(&r, before);
}

/** Read the meeting cases' notes into `log`, and say whether both have the
 * note that begins with `a` and `b`. */
static int both(char *log, size_t size, const char *a, const char *b)
{
    char path[PATH_LEN];

    read_text(in(path, sizeof(path), "meet/log"), log, size);
    return has_start(log, a) && has_start(log, b);
}

/** A width of two lets the cases of two suites meet, with `locks` and
 * without, and a width of one keeps them apart. */
static void test_width(void)
{
    static const struct etude_legacy_member m[] = {{"meet-a", 0, 0, ONE("meet-a")},
                                                   {"meet-b", 0, 0, ONE("meet-b")}};
    static const char *const names[] = {"meet", "meetlocks", "width"};
    static struct run r[3];
    char log[1024];
    size_t i;
    int before;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        before = fails;
        clear_marks();
        r[i].name = names[i];
        r[i].members = m;
        r[i].n = 2;
        r[i].width = i < 2 ? 2 : 1;
        r[i].locks = i == 1;
        set_run(&r[i]);
        CHECK(r[i].status == 0, "width: passed");
        if (i < 2)
            CHECK(both(log, sizeof(log), "meet-a met", "meet-b met"),
                  "width: two at once with width 2");
        else
            CHECK(both(log, sizeof(log), "meet-a alone", "meet-b alone"),
                  "width: one at a time with width 1");
        CHECK(r[i].claims == 0, "width: the ledger has no claim");
        if (fails != before)
            printf("etude-legacyset-test: %s: the notes:\n%s", names[i], log);
        show(&r[i], before);
    }
}

/** A serial member, though named second, runs first with nothing beside
 * it. */
static void test_serial(void)
{
    static const struct etude_legacy_member m[] = {{"side-x", 0, 0, ONE("side-x")},
                                                   {"serial", 1, 0, ONE("serial")},
                                                   {"side-y", 0, 0, ONE("side-y")}};
    static struct run r;
    char path[PATH_LEN], marks[1024];
    int before = fails, serial, side;

    clear_marks();
    r.name = "serial";
    r.members = m;
    r.n = 3;
    set_run(&r);
    read_text(in(path, sizeof(path), "marks"), marks, sizeof(marks));
    serial = line_no(r.out, "run: serial: fixture.serial.one: starting");
    side = line_no(r.out, "run: side-x: fixture.side-x.one: starting");
    CHECK(r.status == 0, "serial: passed");
    CHECK(line_no(marks, "start serial") == 1, "serial: it runs first");
    CHECK(line_no(marks, "end serial") == 2, "serial: nothing beside it");
    CHECK(serial > 0 && serial < side, "serial: it starts first");
    if (fails != before)
        printf("etude-legacyset-test: serial: the marks:\n%s", marks);
    show(&r, before);
}

/** A failure under `stop_on_fail` cancels the case that runs and the one
 * not started. */
static void test_first(void)
{
    static const struct etude_legacy_member m[] = {
        {"fail", 0, 0, ONE("fail")}, {"long", 0, 0, ONE("long")}, {"plain", 0, 0, ONE("plain")}};
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
    CHECK(case_is(&r, "fixture.fail.one", "i686", 0, "failure", NULL), "first: fail failed");
    CHECK(case_is(&r, "fixture.long.one", "i686", 0, "cancelled", NULL),
          "first: the running case cancelled");
    CHECK(case_attempt(&r, "fixture.plain.one", "i686", 0) == NULL,
          "first: no attempt of the case not started");
    CHECK(item_is(&r, "fixture.plain.one", "cancelled"), "first: the case not started cancelled");
    CHECK(has_line(r.out, "run: stopped on the first failure; not started: fixture.plain.one"),
          "first: the cases not started named");
    CHECK(has_line(r.out, "run: stopped on the first failure; stopped part way: fixture.long.one"),
          "first: the cases stopped part way named");
    CHECK(
        has_line(r.out, "run: long: fixture.long.one: stopped before its end; its output so far:"),
        "first: the stopped case's output");
    CHECK(r.claims == 0, "first: the ledger has no claim");
    show(&r, before);
}

/** A failure of a member that `stops` cancels the rest. */
static void test_stops(void)
{
    static const struct etude_legacy_member m[] = {
        {"fail", 0, 1, ONE("fail")}, {"long", 0, 0, ONE("long")}, {"plain", 0, 0, ONE("plain")}};
    static struct run r;
    int before = fails;

    r.name = "stops";
    r.members = m;
    r.n = 3;
    r.width = 2;
    set_run(&r);
    CHECK(r.status == 1, "stops: failed");
    CHECK(case_is(&r, "fixture.long.one", "i686", 0, "cancelled", NULL),
          "stops: the running case cancelled");
    CHECK(item_is(&r, "fixture.plain.one", "cancelled"), "stops: the case not started cancelled");
    CHECK(has_line(r.out, "run: stopped: fail failed; not started: fixture.plain.one"),
          "stops: the cases not started named");
    CHECK(has_line(r.out, "run: stopped: fail failed; stopped part way: fixture.long.one"),
          "stops: the cases stopped part way named");
    show(&r, before);
}

/** A stop signal while a case runs: the status record has a line of its
 * attempt first, and the set then ends with the case cancelled. */
static void test_signal(void)
{
    static const struct etude_legacy_member m[] = {{"hold", 0, 0, ONE("hold")},
                                                   {"plain", 0, 0, ONE("plain")}};
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
          "signal: the case started");
    for (i = 0; i < STATUS_BOUND / 50 && !named; i++) {
        read_text(r.status_file, status, sizeof(status));
        named = (at = line_start(status, "attempt ")) != NULL &&
                strstr(at, ":fixture.hold.one:i686:r0:a0 fixture.hold.one i686 ") != NULL;
        if (!named)
            /* interval: the record is read again every 50 ms */
            usleep(50000);
    }
    CHECK(named, "status: the running case's attempt");
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
    CHECK(case_is(&r, "fixture.hold.one", "i686", 0, "cancelled", NULL),
          "signal: the running case cancelled");
    CHECK(item_is(&r, "fixture.hold.one", "cancelled"), "signal: the run record's item cancelled");
    at = line_start(r.out,
                    "run: hold: fixture.hold.one: stopped before its end; its output so far:");
    CHECK(at != NULL && has_line(at, "holding"), "signal: its output so far printed");
    CHECK(has_line(r.out, "etude-legacy: result: stopped by signal 15"), "signal: the result");
    CHECK(r.claims == 0, "signal: the ledger has no claim");
    if (fails != before)
        printf("etude-legacyset-test: signal: the status record:\n%s", status);
    show(&r, before);
}

/** A preparation that fails cancels every item, and no case starts. */
static void test_prepare(void)
{
    static const struct etude_legacy_member m[] = {{"plain", 0, 0, ONE("plain")},
                                                   {"fail", 0, 0, ONE("fail")}};
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
    CHECK(item_is(&r, "fixture.plain.one", "cancelled") &&
              item_is(&r, "fixture.fail.one", "cancelled"),
          "prepare: every item cancelled");
    CHECK(has_start(r.out, "etude-legacy: result: preparation failed: "), "prepare: the result");
    CHECK(r.claims == 0, "prepare: the ledger has no claim");
    show(&r, before);
}

/** A case that asks for more memory than the machine has, 16 MiB of 8,
 * runs, its claim held to the limit. */
static void test_large(void)
{
    static const struct etude_legacy_member m[] = {{"plain", 0, 0, ONE("plain")}};
    static struct run r;
    int before = fails;

    r.name = "large";
    r.members = m;
    r.n = 1;
    r.locks = 1;
    r.limit_mib = 8;
    set_run(&r);
    CHECK(r.status == 0, "large: passed");
    CHECK(case_is(&r, "fixture.plain.one", "i686", 0, "pass", NULL), "large: it ran");
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
    static const struct etude_legacy_member m[] = {{"good", 0, 0, "cases/good/good.json"}};
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
    static const struct etude_legacy_member m[] = {{"good", 0, 0, "cases/good/good.json"}};
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
    static const struct etude_legacy_member m[] = {{"mixed", 0, 0, "cases/mixed/mixed.json"}};
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
    static const struct etude_legacy_member m[] = {{"mixed", 1, 1, "cases/mixed/mixed.json"},
                                                   {"plain", 0, 0, ONE("plain")}};
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
    CHECK(item_is(&r, "fixture.plain.one", "cancelled"), "casesstops: the next suite cancelled");
    CHECK(has_line(r.out, "run: stopped: mixed failed; not started: fixture.plain.one"),
          "casesstops: the stop names the suite");
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
         "fixture.bad.one: a case of the domain native; the set driver runs host, guest and "
         "machine cases alone"},
        {"bad", "cases/bad/machineargs.json",
         "fixture.bad.one: a machine case has arguments, but its scenario takes its invocation "
         "alone"},
        {"bad", "cases/bad/stdin.json", "fixture.bad.one: the case reads standard input"},
        {"bad", "cases/bad/ports.json",
         "fixture.bad.one: the case needs local ports, which the set driver gives a machine case "
         "alone"},
        {"bad", "cases/bad/loops.json", "fixture.bad.one: the case needs loop devices"},
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
        {"badguest", "cases/badguest/forwards.json",
         "fixture.badguest.one: the case needs loop devices or host locks, which the set driver "
         "does not give"},
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

/** A machine case's ports are of the caller's port block, so a set with
 * `locks` that has one is refused. */
static void test_machine_locks(void)
{
    static const struct etude_legacy_member m[] = {{"machine", 0, 0, "cases/machine/machine.json"}};
    static struct run r;

    r.members = m;
    r.n = 1;
    r.locks = 1;
    refused(&r, "machinelocks", "machine: no ports with locks",
            "a machine case's local ports are of the caller's port block");
}

/** Say whether the invocation of the attempt of case member `tail`,
 * `CASE.rR`, of run `r` has the text `text`. */
static int invocation_has(const struct run *r, const char *tail, const char *text)
{
    static char doc[OUT_LEN];
    char path[PATH_LEN * 2];

    fix_format(path, sizeof(path), "%s/output/%s.invocation", r->results, tail);
    return read_text(path, doc, sizeof(doc)) > 0 && strstr(doc, text) != NULL;
}

/** Write in `out` the member `"NAME":"DIGEST"` of an invocation's artifacts
 * for a file whose content is `text`. */
static void digest_text(char *out, size_t size, const char *name, const char *text)
{
    unsigned char sum[32];
    char hex[65];
    struct sha256 h;

    sha256_init(&h);
    sha256_update(&h, (const unsigned char *)text, strlen(text));
    sha256_final(&h, sum);
    sha256_hex(sum, hex);
    fix_format(out, size, "\"%s\":\"%s\"", name, hex);
}

/** Write in `out` the member `"NAME":"DIGEST"` of an invocation's artifacts
 * for the file `file` of the tree. */
static void digest_member(char *out, size_t size, const char *name, const char *file)
{
    static char data[4096];
    char path[PATH_LEN];

    read_text(in(path, sizeof(path), file), data, sizeof(data));
    digest_text(out, size, name, data);
}

/** Say whether the invocation of each machine case of run `r`, from the
 * `first` of `machine_cases` on, has the text `text`. */
static int invocations_have(const struct run *r, size_t first, const char *text)
{
    char tail[64];
    size_t i;

    for (i = first; i < sizeof(machine_cases) / sizeof(machine_cases[0]); i++) {
        fix_format(tail, sizeof(tail), "%s.r0", machine_cases[i].id);
        if (!invocation_has(r, tail, text))
            return 0;
    }
    return 1;
}

/** Count the copies of the file `name` of the tree that the driver of run
 * `r` made to its store. */
static int copies_of(const struct run *r, const char *name)
{
    static char text[OUT_LEN];
    char *line, *next;
    int count = 0;

    read_text(r->copies, text, sizeof(text));
    for (line = text; *line != '\0'; line = next) {
        if ((next = strchr(line, '\n')) != NULL)
            *next++ = '\0';
        else
            next = line + strlen(line);
        count += names_file(line, name);
    }
    return count;
}

/** A suite of machine cases: their scenario's invocations, the artifacts
 * they share copied to the store once, and the judgement of each one's
 * report, end and console. */
static void test_machine(void)
{
    static const struct etude_legacy_member m[] = {{"machine", 0, 0, "cases/machine/machine.json"}};
    static struct run r;
    char text[PATH_LEN * 2], console[PATH_LEN * 2];
    int before = fails, given;

    r.name = "machine";
    r.members = m;
    r.n = 1;
    setenv("RACH2_OVMF_CODE", "/firmware/OVMF_CODE_4M.fd", 1);
    set_run(&r);
    unsetenv("RACH2_OVMF_CODE");
    CHECK(r.status == 1, "machine: a case did not pass");
    CHECK(r.read == 0, "machine: the history is whole");
    CHECK(count_kind(&r.h, "attempt") == 5, "machine: an attempt of each case");
    CHECK(case_is(&r, "fixture.machine.clean", "i686", 0, "pass", NULL),
          "machine: a clean console passes");
    CHECK(case_is(&r, "fixture.machine.allowed", "i686", 0, "pass", NULL),
          "machine: the critical record its machine declares passes");
    CHECK(case_is(&r, "fixture.machine.panic", "i686", 0, "failure",
                  "etude.kernel failed: the console has <2>[    0.500000] panic: page fault"),
          "machine: a panic on the console fails etude.kernel");
    CHECK(case_is(&r, "fixture.machine.lost", "i686", 0, "failure", NULL),
          "machine: a report cut short with status 1 fails");
    CHECK(case_is(&r, "fixture.machine.noconsole", "i686", 0, "incomplete",
                  "etude.kernel is missing: casecheck attempt was given no console"),
          "machine: no console is incomplete");
    fix_format(text, sizeof(text),
               "scenario: %s/build/cases/scenario %s/output/"
               "fixture.machine.clean.r0.invocation",
               tree, r.results);
    CHECK(output_has(&r, "fixture.machine.clean.r0", text),
          "machine: the scenario takes its invocation alone");
    digest_member(text, sizeof(text), "kernel", "build/rach2/kernel");
    CHECK(invocation_has(&r, "fixture.machine.clean.r0", text), "invocation: the kernel's digest");
    digest_member(text, sizeof(text), "disk", DISK);
    CHECK(invocation_has(&r, "fixture.machine.clean.r0", text), "invocation: the disk's digest");
    digest_member(text, sizeof(text), "scenario", "cases/scenario.sh");
    CHECK(invocation_has(&r, "fixture.machine.clean.r0", text),
          "invocation: the scenario's digest");
    CHECK(copies_of(&r, "build/rach2/kernel") == 1 && copies_of(&r, DISK) == 1 &&
              copies_of(&r, "build/cases/scenario") == 1,
          "publish: the artifacts the five cases share are copied once each");
    given = invocations_have(&r, 0, text);
    digest_member(text, sizeof(text), "kernel", "build/rach2/kernel");
    given &= invocations_have(&r, 0, text);
    digest_member(text, sizeof(text), "disk", DISK);
    given &= invocations_have(&r, 0, text);
    CHECK(given, "publish: each case is given each artifact's digest");
    fix_format(text, sizeof(text), "\"store\":\"%s/%s.store\"", r.evidence, r.run_id);
    CHECK(invocation_has(&r, "fixture.machine.clean.r0", text), "invocation: the store");
    fix_format(text, sizeof(text), "\"digests\":\"%s/build/tests/digests\"", tree);
    CHECK(invocation_has(&r, "fixture.machine.clean.r0", text),
          "invocation: the tree's digests file");
    fix_format(text, sizeof(text), "%s/%s.store", r.evidence, r.run_id);
    CHECK(!exists(text), "machine: the store of the artifacts is removed at the end");
    fix_format(text, sizeof(text), "\"qemu_i686\":\"%s/qemu/qemu-system-i386\"", tree);
    CHECK(invocation_has(&r, "fixture.machine.clean.r0", text),
          "invocation: the emulator found on the path");
    CHECK(invocation_has(&r, "fixture.machine.clean.r0", "\"tools\":{\"sh\":\"/") &&
              invocation_has(&r, "fixture.machine.clean.r0", "\"accel\":\"tcg\"") &&
              invocation_has(&r, "fixture.machine.clean.r0", "\"steps\":{\"boot\":20000000}") &&
              invocation_has(&r, "fixture.machine.clean.r0", "\"cleanup_us\":7000000"),
          "invocation: the tools, the accelerator and the bounds");
    CHECK(invocation_has(&r, "fixture.machine.clean.r0",
                         "\"ovmf_i686\":null,\"ovmf_x86_64\":\"/firmware/OVMF_CODE_4M.fd\""),
          "invocation: the firmware RACH2_OVMF_CODE names");
    CHECK(invocation_has(&r, "fixture.machine.clean.r0", "\"ports\":[9400,9401]") &&
              invocation_has(&r, "fixture.machine.allowed.r0", "\"ports\":[9402]") &&
              invocation_has(&r, "fixture.machine.panic.r0", "\"ports\":[]"),
          "invocation: the ports of the caller's block, each case's its own");
    CHECK(invocation_has(&r, "fixture.machine.clean.r0", "\"boundary_lazy\":false}"),
          "invocation: a run of no site does not permit the lazy boundary profile");
    fix_format(text, sizeof(text), "\"scratch\":\"%s/%s.fixture.machine.clean.r0\"", r.evidence,
               r.run_id);
    CHECK(invocation_has(&r, "fixture.machine.clean.r0", text), "invocation: the scratch");
    CHECK(!kept(&r, "fixture.machine.clean.r0"), "machine: the workspace removed after a pass");
    fix_format(console, sizeof(console), "%s/%s.fixture.machine.panic.r0/evidence/vm.1.console",
               r.evidence, r.run_id);
    CHECK(exists(console), "machine: the console kept after a failure");
    CHECK(results_has(&r, "fixture.machine.clean.r0", "\"placement\":\"qemu\"") &&
              results_has(&r, "fixture.machine.clean.r0", "\"observed_by\":\"host_process\""),
          "machine: placed qemu, its end observed by its host process");
    CHECK(plan_has(&r, "\"domain\":\"qemu\"") && !plan_has(&r, "\"domain\":\"host\""),
          "machine: the plan's items are of the domain qemu");
    CHECK(r.claims == 0, "machine: the ledger has no claim");
    CHECK(!r.leaks, "machine: the driver leaks nothing");
    show(&r, before);
}

/**
 * The machine cases' disk copied to the store again. Changed after its first
 * copy, it is copied for the next case, which is given its new digest, as
 * are the cases after it, and the first case its old one. When its first
 * copy fails, that case ends incomplete with the reason, before its scenario
 * starts, and the next case copies the disk again and gives its copy to the
 * cases after it.
 */
static void test_publish(void)
{
    static const struct etude_legacy_member m[] = {{"machine", 0, 0, "cases/machine/machine.json"}};
    static struct run changed, failed;
    char text[PATH_LEN * 2], disk[PATH_LEN];
    int before = fails;

    changed.name = "publish-changed";
    changed.members = m;
    changed.n = 1;
    changed.put = PUT_CHANGE;
    set_run(&changed);
    CHECK(write_text(in(disk, sizeof(disk), DISK), disk_text, 0644) == 0,
          "publish-changed: the disk restored");
    CHECK(changed.status == 1, "publish-changed: a case did not pass");
    CHECK(changed.read == 0, "publish-changed: the history is whole");
    CHECK(copies_of(&changed, "build/rach2/kernel") == 1 && copies_of(&changed, DISK) == 2 &&
              copies_of(&changed, "build/cases/scenario") == 1,
          "publish-changed: the disk changed after its first copy is copied again, and "
          "nothing else");
    digest_text(text, sizeof(text), "disk", disk_text);
    CHECK(invocation_has(&changed, "fixture.machine.clean.r0", text),
          "publish-changed: the first case is given the disk's first digest");
    digest_text(text, sizeof(text), "disk", disk_changed);
    CHECK(invocations_have(&changed, 1, text),
          "publish-changed: the cases after it are given the changed disk's digest");
    CHECK(changed.claims == 0, "publish-changed: the ledger has no claim");
    CHECK(!changed.leaks, "publish-changed: the driver leaks nothing");
    show(&changed, before);

    before = fails;
    failed.name = "publish-failed";
    failed.members = m;
    failed.n = 1;
    failed.put = PUT_FAIL;
    set_run(&failed);
    CHECK(failed.status == 1, "publish-failed: a case did not pass");
    CHECK(failed.read == 0, "publish-failed: the history is whole");
    CHECK(case_is(&failed, "fixture.machine.clean", "i686", 0, "incomplete",
                  "its artifact disk, " DISK ", could not be published: No such file or "
                  "directory"),
          "publish-failed: the case whose copy failed is incomplete with the reason");
    CHECK(!invocation_has(&failed, "fixture.machine.clean.r0", "{"),
          "publish-failed: that case has no invocation");
    CHECK(copies_of(&failed, "build/rach2/kernel") == 1 && copies_of(&failed, DISK) == 2 &&
              copies_of(&failed, "build/cases/scenario") == 1,
          "publish-failed: the next case copies the disk again, and the others reuse its copy");
    digest_text(text, sizeof(text), "disk", disk_text);
    CHECK(invocations_have(&failed, 1, text),
          "publish-failed: the cases after it are given the disk's digest");
    CHECK(case_is(&failed, "fixture.machine.allowed", "i686", 0, "pass", NULL),
          "publish-failed: a case after it passes");
    CHECK(failed.claims == 0, "publish-failed: the ledger has no claim");
    CHECK(!failed.leaks, "publish-failed: the driver leaks nothing");
    show(&failed, before);
}

/** Say whether the scenario of the boundary case of run `r` started. */
static int boundary_started(const struct run *r)
{
    static char text[OUT_LEN];
    char path[PATH_LEN * 2];

    fix_format(path, sizeof(path), "%s/output/fixture.boundary.accounting.r0.log", r->results);
    return read_text(path, text, sizeof(text)) > 0 && has_start(text, "scenario: ");
}

/**
 * Run the boundary case of run `r` on x86_64, and check that it ended
 * before its scenario started with the reason `why`, its invocation not
 * written.
 */
static void boundary_refused(struct run *r, const char *row, const char *why)
{
    static const struct etude_legacy_member m[] = {
        {"boundary", 0, 0, "cases/boundary/boundary.json"}};
    int before = fails;

    r->name = row;
    r->members = m;
    r->n = 1;
    r->arch = "x86_64";
    set_run(r);
    CHECK(r->status == 1, row);
    CHECK(case_is(r, "fixture.boundary.accounting", "x86_64", 0, "incomplete", why), row);
    CHECK(!boundary_started(r), row);
    CHECK(!invocation_has(r, "fixture.boundary.accounting.r0", "{"), row);
    CHECK(r->claims == 0, row);
    show(r, before);
}

/**
 * The boundary case: at a site whose policy permits the lazy boundary
 * profile, its invocation says so, its scenario runs and passes, and its
 * claim holds the case's 4 GiB, not the machine's 72 GiB. Without that
 * permission, from a policy without the line or with no policy, and with a
 * ledger whose memory limit would cut the claim short, it ends incomplete
 * before its scenario starts. A policy that cannot be read is refused.
 */
static void test_boundary(void)
{
    static const struct etude_legacy_member m[] = {
        {"boundary", 0, 0, "cases/boundary/boundary.json"}};
    static const char deny[] =
        "the site's policy does not permit the lazy memory profile bigmem-boundary-v1";
    static struct run r, other, none, small, missing;
    static char site[PATH_LEN], var[PATH_LEN + 32], policy[PATH_LEN];
    char text[PATH_LEN * 2];
    int before = fails;

    fix_format(var, sizeof(var), "FIXTURE_LEDGER=%s", ledger);
    r.name = "boundary";
    r.members = m;
    r.n = 1;
    r.arch = "x86_64";
    r.site = in(site, sizeof(site), "site.permits");
    r.var = var;
    set_run(&r);
    CHECK(r.status == 0, "boundary: a permitted site's case passed");
    CHECK(case_is(&r, "fixture.boundary.accounting", "x86_64", 0, "pass", NULL),
          "boundary: the case passed");
    CHECK(boundary_started(&r), "boundary: the scenario ran");
    CHECK(invocation_has(&r, "fixture.boundary.accounting.r0", "\"boundary_lazy\":true}"),
          "boundary: the invocation permits the lazy boundary profile");
    fix_format(text, sizeof(text), "%s/qemu/qemu-system-x86_64", tree);
    CHECK(invocation_has(&r, "fixture.boundary.accounting.r0", text),
          "boundary: the emulator of x86-64 found on the path");
    CHECK(output_has(&r, "fixture.boundary.accounting.r0",
                     "ledger: resource memory_bytes 4294967296"),
          "boundary: the claim holds the case's 4 GiB");
    CHECK(!output_has(&r, "fixture.boundary.accounting.r0",
                      "ledger: resource memory_bytes 77309411328"),
          "boundary: the claim does not hold the machine's 72 GiB");
    CHECK(r.claims == 0, "boundary: the ledger has no claim");
    CHECK(!r.leaks, "boundary: the driver leaks nothing");
    show(&r, before);

    other.site = in(policy, sizeof(policy), "site.other");
    boundary_refused(&other, "boundary-unproved", deny);
    boundary_refused(&none, "boundary-nosite", deny);
    small.site = site;
    small.limit_mib = 4095;
    boundary_refused(&small, "boundary-short",
                     "its 4294967296 bytes of memory are more than the run's limit of "
                     "4293918720, and a machine of bigmem-boundary-v1 runs only on its whole "
                     "reservation");
    missing.members = m;
    missing.n = 1;
    missing.arch = "x86_64";
    missing.site = in(text, sizeof(text), "site.absent");
    refused(&missing, "boundary-nopolicy", "boundary-nopolicy: a policy that cannot be read",
            "the site's policy ");
}

/** Read the manifest `name` of the fixture tree for the suite `suite` on
 * i686; the memory its first case claims on a host whose cases run in the
 * tooling machine when `tooling` is set, and on Linux otherwise, or 0 with
 * the reason in `why`. */
static uint64_t tooling_claim(const char *suite, const char *name, int tooling, char *why,
                              size_t size)
{
    struct legacy_manifest m;
    uint64_t bytes = 0;

    why[0] = '\0';
    if (legacy_manifest_read(&m, tree, name, suite, "i686", NULL) == 0)
        bytes = legacy_case_memory(&m.cases[0], tooling);
    else
        fix_format(why, size, "%s", m.why);
    legacy_manifest_free(&m);
    return bytes;
}

/** A host case with tooling memory claims its `memory_bytes` on Linux and
 * its tooling memory on another host, where its work runs in the tooling
 * machine; a case without tooling memory claims its `memory_bytes` on both,
 * and tooling memory of no bytes is refused. A machine case with tooling
 * memory claims it as a host case does, and a guest case's is refused. */
static void test_tooling(void)
{
    static const struct etude_legacy_member m[] = {{"tooling", 0, 0, "cases/tooling/tooling.json"}};
    static struct run r;
    static char var[PATH_LEN + 32];
    char why[400];
    int before = fails;

    fix_format(var, sizeof(var), "FIXTURE_LEDGER=%s", ledger);
    r.name = "tooling";
    r.members = m;
    r.n = 1;
    r.var = var;
    set_run(&r);
    CHECK(r.status == 0, "tooling: the case passed");
#ifdef __linux__
    CHECK(output_has(&r, "fixture.tooling.one.r0", "ledger: resource memory_bytes 16777216"),
          "tooling: the claim on Linux is the case's memory");
#else
    CHECK(output_has(&r, "fixture.tooling.one.r0", "ledger: resource memory_bytes 3355443200"),
          "tooling: the claim on this host is the case's tooling memory");
#endif
    CHECK(r.claims == 0, "tooling: the ledger has no claim");
    CHECK(!r.leaks, "tooling: the driver leaks nothing");
    show(&r, before);

    CHECK(tooling_claim("tooling", "cases/tooling/tooling.json", 1, why, sizeof(why)) ==
              UINT64_C(3355443200),
          "tooling: on a host of the tooling machine, the case claims its tooling memory");
    CHECK(tooling_claim("tooling", "cases/tooling/tooling.json", 0, why, sizeof(why)) ==
              UINT64_C(16777216),
          "tooling: on Linux, the case claims its memory");
    CHECK(tooling_claim("plain", ONE("plain"), 1, why, sizeof(why)) == UINT64_C(16777216),
          "tooling: a case without tooling memory claims its memory");
    CHECK(tooling_claim("tooling", "cases/tooling/none.json", 1, why, sizeof(why)) == 0 &&
              strstr(why, "fixture.tooling.one: the case's tooling memory is not a host or "
                          "machine case's count of bytes") != NULL,
          "tooling: tooling memory of no bytes is refused");
    CHECK(tooling_claim("machine", "cases/tooling/machine.json", 1, why, sizeof(why)) ==
              UINT64_C(3623878656),
          "tooling: on a host of the tooling machine, a machine case claims its tooling memory");
    CHECK(tooling_claim("machine", "cases/tooling/machine.json", 0, why, sizeof(why)) ==
              UINT64_C(268435456),
          "tooling: on Linux, a machine case claims its memory");
    CHECK(tooling_claim("badguest", "cases/tooling/guest.json", 1, why, sizeof(why)) == 0 &&
              strstr(why, "fixture.badguest.one: the case's tooling memory is not a host or "
                          "machine case's count of bytes") != NULL,
          "tooling: a guest case's tooling memory is refused");
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
    static const char *const ids[] = {"rng", "boot", "named"};
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
 * Give in `hex` the digest of `goal` in the preparation's record of run `r`,
 * from its line `file DIGEST SIZE INODE MTIME PATH`; 0, or -1 when it has
 * none.
 */
static int record_digest(const struct run *r, const char *goal, char hex[65])
{
    static char text[OUT_LEN];
    char path[PATH_LEN], want[PATH_LEN];
    const char *at, *line;

    fix_format(path, sizeof(path), "%s/prepare.record", r->results);
    fix_format(want, sizeof(want), " %s\n", goal);
    if (read_text(path, text, sizeof(text)) <= 0)
        return -1;
    for (at = strstr(text, want); at != NULL; at = strstr(at + 1, want)) {
        for (line = at; line > text && line[-1] != '\n'; line--)
            continue;
        if (sscanf(line, "file %64[0-9a-f] ", hex) == 1 && strlen(hex) == 64)
            return 0;
    }
    return -1;
}

/** Count the lines of `text` that have `word`. */
static int lines_with(const char *text, const char *word)
{
    const char *at = strstr(text, word);
    int n = 0;

    while (at != NULL) {
        n++;
        at = strchr(at, '\n');
        at = at != NULL ? strstr(at, word) : NULL;
    }
    return n;
}

/**
 * A suite run by guest cases on the real QEMU with `-n fixture`: rngprobe
 * on the processor `max` and `qemu64`, whose features decide its branch, a
 * boot that runs past its stage's bound, and rngprobe on the kernel that
 * replaces the node `rach2/kernel`.
 */
static void test_guest(void)
{
    static const struct etude_legacy_member m[] = {{"guest", 0, 0, "cases/guest/guest.json"}};
    static struct run r;
    static char text[OUT_LEN];
    const char *dir = strcmp(guest_arch, "x86_64") == 0 ? "build/x86_64" : "build";
    char path[PATH_LEN * 2], goal[PATH_LEN], other[PATH_LEN], hex[65] = "", hex2[65] = "";
    int before = fails, named, own, all;

    r.name = "guest";
    r.members = m;
    r.n = 1;
    r.arch = guest_arch;
    r.kernel = "fixture";
    r.guest = 1;
    set_run(&r);
    CHECK(r.status == 1, "guest: the boot past its bound fails the run");
    CHECK(r.read == 0, "guest: the history is whole");
    CHECK(count_kind(&r.h, "attempt") == 4, "guest: an attempt of each case");
    CHECK(case_is(&r, "fixture.guest.rng", guest_arch, 0, "pass", NULL),
          "guest: rngprobe passed on max");
    CHECK(results_has(&r, "fixture.guest.rng.r0",
                      "{\"group_id\":\"source\",\"alternative_id\":\"trusted\",\"decided_by\":"
                      "\"feature\"}"),
          "guest: max's features decide the branch trusted");
    CHECK(case_is(&r, "fixture.guest.closed", guest_arch, 0, "pass", NULL),
          "guest: rngprobe passed on qemu64");
    CHECK(results_has(&r, "fixture.guest.closed.r0",
                      "{\"group_id\":\"source\",\"alternative_id\":\"fail-closed\",\"decided_by\":"
                      "\"feature\"}"),
          "guest: qemu64's features decide the branch fail-closed");
    CHECK(output_has(&r, "fixture.guest.closed.r0", "RNGPROBE OK (fail-closed)"),
          "guest: qemu64 completed the fail-closed checks");
    fix_format(path, sizeof(path), "%s/qemu.argv", guest_evidence);
    CHECK(read_text(path, text, sizeof(text)) > 0 && strstr(text, "-cpu max,") != NULL &&
              strstr(text, "-cpu qemu64,") != NULL,
          "guest: each machine has its profile's processor");
    CHECK(case_is(&r, "fixture.guest.named", guest_arch, 0, "pass", NULL),
          "kernel: the case of rach2/kernel passed on rach2-fixture/kernel");
    fix_format(goal, sizeof(goal), "%s/rach2-fixture/kernel", dir);
    fix_format(other, sizeof(other), "%s/guest/kernel", dir);
    named = record_digest(&r, goal, hex) == 0 ? lines_with(text, hex) : -1;
    own =
        record_digest(&r, other, hex2) == 0 && strcmp(hex, hex2) != 0 ? lines_with(text, hex2) : -1;
    all = lines_with(text, "-kernel ");
    CHECK(named == 1, "kernel: one machine booted rach2-fixture/kernel under -n");
    /* The boot past its 200 ms bound may be stopped before its QEMU starts. */
    CHECK(own >= 2 && named + own == all, "kernel: the cases of another node booted it under -n");
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
    static const struct etude_legacy_member bad[] = {{"Plain", 0, 0, ONE("plain")}};
    static const struct etude_legacy_member twice[] = {{"plain", 0, 0, ONE("plain")},
                                                       {"plain", 0, 0, ONE("plain")}};
    static const struct etude_legacy_member one[] = {{"plain", 0, 0, ONE("plain")}};
    static const struct etude_legacy_member bare[] = {{"plain", 0, 0, NULL}};
    static struct run r[6];

    r[0].members = one;
    refused(&r[0], "none", "refused: no suite", NULL);
    r[1].members = bad;
    r[1].n = 1;
    refused(&r[1], "badname", "refused: a bad name", NULL);
    r[2].members = twice;
    r[2].n = 2;
    refused(&r[2], "twice", "refused: a suite named twice", NULL);
    r[3].members = bare;
    r[3].n = 1;
    refused(&r[3], "nocases", "refused: a suite with no manifest",
            "a suite is run by the cases of a manifest, and one names none");
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
    const char *arch = getenv("ARCH"), *path = getenv("PATH");
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
        if (len == 0)
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

    if (argc >= 2 && (strcmp(argv[1], "prepare") == 0 || strcmp(argv[1], "check") == 0))
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
    unsetenv("RACH2_OVMF_CODE");
    unsetenv("FIXTURE_BREAK");
    make_tree();
    test_mixed();
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
    test_machine();
    test_publish();
    test_boundary();
    test_tooling();
    test_cases_refused();
    test_machine_locks();
    test_guest();
    if (fails != 0)
        return 1;
    fix_passed();
    printf("etude_legacyset_hosttest: pass\n");
    return 0;
}
