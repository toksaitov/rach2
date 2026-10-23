/* C control of the host suite's compiler and build-product regressions. */
#include "build_support.h"

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
    if (ok) {
        if (!strcmp(argv[1], "commit_msg"))
            ok = build_commit_msg(&f);
        else if (!strcmp(argv[1], "run_scripts"))
            ok = build_run_scripts(&f);
        else if (!strcmp(argv[1], "gnu_port_resume"))
            ok = build_resume(&f);
        else if (!strcmp(argv[1], "gnu_cmake_jobs"))
            ok = build_jobs(&f);
        else if (!strcmp(argv[1], "doom_libc"))
            ok = build_doom(&f);
        else if (!strcmp(argv[1], "check_asm"))
            ok = build_asm(&f);
        else if (!strcmp(argv[1], "check_ldscript"))
            ok = build_ldscript(&f);
        else if (!strcmp(argv[1], "prereq"))
            ok = build_prereq(&f);
        else if (!strcmp(argv[1], "together"))
            ok = build_together(&f);
        else if (!strcmp(argv[1], "host_suite"))
            ok = build_suite(&f);
        else if (!strcmp(argv[1], "case_select"))
            ok = build_runner(&f, argv[1]);
        else {
            fprintf(stderr, "host-build-check: unknown check: %s\n", argv[1]);
            ok = 0;
        }
    }
    fx_check(&f, ok, argv[1]);
    ok = !f.failed;
    if (!ok)
        fprintf(stderr, "host-build-check: evidence: %s\n", f.work);
    fx_destroy(&f);
    return ok ? 0 : 1;
}
