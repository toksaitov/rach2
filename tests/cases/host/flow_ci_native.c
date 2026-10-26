/* Run the declared native routine contracts in the prepared GNU fixture. */
#include "flow_check.h"

/* The shared mock dispatcher keeps this symbol for older script operands. */
int flow_ci_native_mock(int argc, char **argv)
{
    (void)argc;
    (void)argv;
    return 2;
}

int flow_ci_native(struct fixture *f)
{
    const char *argv[] = {
        "/host-products/host-checks/tree/build/tests/host-suite/etude/etude_platform_hosttest",
        "--routine-entry-only", NULL};
    fx_run(f, argv, NULL, NULL, NULL);
    return fx_check(f, f->status == 0,
                    "native routine preserves all projection, push and sequence contracts");
}
