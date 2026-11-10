/* One host group over native checks and a declared GNU products helper. */
#define main host_driver_main
#include "host.c"
#undef main
#include "etude/scenario.h"

static void case_assert(void *ctx, const char *id, int held, const char *why)
{
    etude_assert(ctx, id, held, held ? "the prepared check passed" : why);
}

static void case_progress(void *ctx, size_t n)
{
    etude_progress(ctx, "checks", n);
}

/* Run ARGV in the guest with standard input from the raw disk INPUT, or
 * none, and say whether it exited 0 with its cleanup confirmed. */
static int guest_run(struct etude_case_machine *m, const char *const argv[], const char *input,
                     const char *label)
{
    struct etude_guest_result result;
    enum etude_status st = input ? etude_guest_command_file(m, argv, NULL, input, label, &result)
                                 : etude_guest_command(m, argv, NULL, NULL, label, &result);
    return st == ETUDE_OK && result.complete && result.termination.end == ETUDE_END_EXITED &&
           result.termination.value == 0 && result.cleanup == ETUDE_CLEANUP_CONFIRMED;
}

/* Begin the step `gnu` by unpacking into /host-products the host checks,
 * gnu-host-checks.tar on the helper's raw disk1, and its products,
 * gnu-host-products.tar on disk2, which the image does not carry since they
 * change with every kernel and case source (mk/gnu-image.mk). */
static int guest_stage(struct etude_case *c, struct etude_case_machine *m)
{
    const char *argv[] = {"/bin/tar", "-x", "-o", "-f", "-", "-C", "/host-products", NULL};
    return etude_step(c, "gnu") == ETUDE_OK &&
           guest_run(m, argv, "/dev/disk1", "host-checks-unpack") &&
           guest_run(m, argv, "/dev/disk2", "host-products-unpack");
}

/* The unpacked tree and driver are immutable inputs of the helper's disk. */
static int guest_check(struct etude_case_machine *m, const char *group, const char *id,
                       const char *arch)
{
    const char *argv[] = {"/host-products/host-checks/driver", group, id, arch, NULL};
    return guest_run(m, argv, NULL, id);
}

/* Export failed scratch while the helper still owns its private disk. */
static int guest_evidence(struct etude_case_machine *m, const char *group, const char *arch)
{
    char name[100];
    const char *args[] = {"/bin/tar", "-Scf", "/host-work/host-fixtures.tar", "-C", "/host-work",
                          name,       NULL};
    const char *compress[] = {"/usr/bin/xz", "-9", "-T1", "/host-work/host-fixtures.tar", NULL};
    int n = snprintf(name, sizeof name, "host-checks.%s.%s/.checks", group, arch);
    return n > 0 && n < (int)sizeof name && guest_run(m, args, NULL, "host-fixture-archive") &&
           guest_run(m, compress, NULL, "host-fixture-compress") &&
           etude_guest_get(m, "/host-work/host-fixtures.tar.xz", "host-fixtures.tar.xz") ==
               ETUDE_OK;
}

static void (*prior_int)(int), (*prior_term)(int);
#ifndef _WIN32
static void (*prior_hup)(int);
#endif

/* Let both the native executor and the case library observe cancellation. */
static void native_stop(int sig)
{
    void (*prior)(int) = sig == SIGINT ? prior_int : prior_term;
#ifndef _WIN32
    if (sig == SIGHUP)
        prior = prior_hup;
#endif
    stopped = sig;
    if (prior != NULL && prior != SIG_IGN && prior != SIG_DFL)
        prior(sig);
}

static int native_checks(struct etude_case *c, struct host *h)
{
    int status;
    if (etude_step(c, "native") != ETUDE_OK || (h->executor = etude_exec_new()) == NULL)
        return -1;
    prior_int = signal(SIGINT, native_stop);
    prior_term = signal(SIGTERM, native_stop);
#ifndef _WIN32
    prior_hup = signal(SIGHUP, native_stop);
#endif
    status = execute(h, 4);
    signal(SIGINT, prior_int);
    signal(SIGTERM, prior_term);
#ifndef _WIN32
    signal(SIGHUP, prior_hup);
#endif
    return status;
}

static int select_native(struct host *h)
{
    int need_guest = 0;
    for (size_t i = 0; i < h->nchecks; i++) {
        if (!h->checks[i].selected)
            continue;
        const char *backend = json_text(h->checks[i].entry, "backend");
        if (backend && !strcmp(backend, "gnu")) {
            h->checks[i].selected = 0;
            need_guest = 1;
        } else if (!backend || strcmp(backend, "native")) {
            assertion(h, h->checks[i].id, 0, "unknown prepared execution backend");
            return -1;
        }
    }
    return need_guest;
}

static int scenario_inventory(struct etude_case *c, struct host *h, const char **selected_group,
                              const char **selected_arch)
{
    const char *id = etude_case_id(c), *tree = getenv("ETUDE_TREE"), *arch = getenv("ARCH");
    const char *group = strrchr(id, '.');
    int listed;

    memset(h, 0, sizeof *h);
    h->report_ctx = c;
    h->report_assert = case_assert;
    h->report_progress = case_progress;
    if (!group || !realpath(tree ? tree : ".", h->tree))
        return -1;
    group++;
    if (!arch || !*arch)
        arch = "i686";
    snprintf(h->case_id, sizeof h->case_id, "%s", id);
    json_program = "host checks";
    if (etude_step(c, "listed") != ETUDE_OK || load_inventory(h, group, arch) != 0) {
        etude_assert(c, "listed", 0, "the prepared host inventory is missing or stale");
        return -1;
    }
    listed = check_inventory(h, group) == 0 && h->failures == 0;
    if (!listed)
        return -1;
    *selected_group = group;
    *selected_arch = arch;
    return 0;
}

int etude_scenario(struct etude_case *c)
{
    struct host h;
    struct etude_case_machine *products = NULL;
    struct etude_termination end;
    const char *group, *arch, *tmp = getenv("TMPDIR");
    int ready = 0, failed = 1, need_guest = 0, complete = 0;
    size_t native_count;

    if (scenario_inventory(c, &h, &group, &arch) != 0)
        goto done;
#ifdef _WIN32
    if (!tmp)
        tmp = etude_win_temp();
#else
    if (!tmp)
        tmp = "/tmp";
#endif
    snprintf(h.work, sizeof h.work, "%s/rach2-host.XXXXXX", tmp);
    if (!mkdtemp(h.work)) {
        h.work[0] = '\0';
        goto done;
    }
    need_guest = select_native(&h);
    if (need_guest < 0)
        goto done;
    if (native_checks(c, &h) < 0 || stopped)
        goto done;
    if (h.failures)
        failed = 1;
    native_count = h.reported;
    if (need_guest && etude_step(c, "products") == ETUDE_OK &&
        etude_boot(c, "products", NULL, &products, &end) == ETUDE_OK)
        ready = guest_stage(c, products);
    for (size_t i = 0; i < h.nchecks; i++) {
        struct check *check = &h.checks[i];
        if (check->selected || !declared(&h, check->id))
            continue;
        const char *backend = json_text(check->entry, "backend");
        if (!backend || strcmp(backend, "gnu"))
            continue;
        int held = ready && guest_check(products, group, check->id, arch);
        etude_assert(c, check->id, held,
                     held ? "the GNU fixture passed every row"
                          : "the GNU fixture failed or did not complete");
        h.failures += !held;
        etude_progress(c, "checks", ++native_count);
    }
    if (ready && h.failures && !guest_evidence(products, group, arch))
        goto done;
    if (ready && !h.failures && !guest_check(products, group, "--cleanup", arch))
        goto done;
    complete = 1;
    failed = h.failures != 0;
done:
    release_host(&h, failed);
    return complete ? 0 : 1;
}
