/* C control of the host suite's compiler and build-product regressions. */
#include "build_support.h"

static int run_check(struct fixture *f, const char *id)
{
    static const struct {
        const char *name;
        int (*run)(struct fixture *);
    } checks[] = {{"commit_msg", build_commit_msg},
                  {"run_scripts", build_run_scripts},
                  {"gnu_port_resume", build_resume},
                  {"gnu_cmake_jobs", build_jobs},
                  {"gnu_host_checks_key", build_host_checks},
                  {"check_asm", build_asm},
                  {"check_ldscript", build_ldscript},
                  {"check_ifunc", build_ifunc},
                  {"prereq", build_prereq},
                  {"together", build_together},
                  {"host_suite", build_suite}};
    if (!strcmp(id, "case_select"))
        return build_runner(f, id);
    for (size_t i = 0; i < sizeof(checks) / sizeof(checks[0]); i++)
        if (!strcmp(id, checks[i].name))
            return checks[i].run(f);
    fprintf(stderr, "host-build-check: unknown check: %s\n", id);
    return 0;
}

int main(int argc, char **argv)
{
    int mock = build_prereq_mock(argc, argv);
    if (mock >= 0)
        return mock;
    mock = build_mock(argc, argv);
    if (mock >= 0)
        return mock;
    if (argc >= 3 && !strcmp(argv[1], "--unprivileged")) {
        if (geteuid() == 0 && (setgid(65534) || setuid(65534)))
            return 120;
        execvp(argv[2], argv + 2);
        return 121;
    }
    if (argc == 3 && !strcmp(argv[1], "--job"))
        return build_job_process(argv[2]);
    if (argc >= 3 && !strcmp(argv[1], "--maker"))
        return build_maker_process(argc - 2, argv + 2);
    if (argc != 3) {
        fputs("usage: host-build-check ID ROOT\n", stderr);
        return 2;
    }
    struct fixture f;
    if (!fx_init(&f, argv[2], argv[1], argv[0]))
        return 2;
    build_env(&f);
    fx_env("LC_ALL", "C");
    fx_env("HOME", bp(f.work, "home"));
    fx_env("TMPDIR", bp(f.work, "tmp"));
    int ok = fx_mkdir(getenv("HOME")) && fx_mkdir(getenv("TMPDIR"));
    if (ok)
        ok = run_check(&f, argv[1]);
    fx_check(&f, ok, argv[1]);
    ok = !f.failed;
    if (!ok)
        fprintf(stderr, "host-build-check: evidence: %s\n", f.work);
    fx_destroy(&f);
    return ok ? 0 : 1;
}
