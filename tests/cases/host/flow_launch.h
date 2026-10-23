/* flow_launch.h: installed launcher fixtures and durable claim checks. */
#ifndef HOST_FLOW_LAUNCH_H
#define HOST_FLOW_LAUNCH_H
#include "flow_check.h"
struct launch_fixture {
    struct fixture *f;
    char tree[4096], policy[32768];
};
int flow_launch(struct fixture *f);
int flow_launch_mock(int argc, char **argv);
int flow_launch_aux(int argc, char **argv);
int launch_saw(struct launch_fixture *u, unsigned count, int same);
int launch_clear(struct launch_fixture *u);
int launch_run(struct launch_fixture *u, const char *const argv[], int status);
int launch_entries(struct launch_fixture *u);
int launch_parallel(struct launch_fixture *u);
int launch_refusals(struct launch_fixture *u);
#endif
