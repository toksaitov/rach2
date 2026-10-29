/* flow_check.h: C controllers for flow and deployment regressions. */
#ifndef HOST_FLOW_CHECK_H
#define HOST_FLOW_CHECK_H
#include "fixture.h"
/* The owner's home on the testing host, where the owner's copy is `rach2`.
 * Pass it to printf apart from that name: the step store refuses a product
 * that has the path of the copy that built it. */
#define FLOW_OWNER_HOME "/home/testing"
const char *flow_path(struct fixture *f, const char *name);
const char *flow_source(struct fixture *f, const char *name);
int flow_put(struct fixture *f, const char *name, const char *text);
int flow_copy(struct fixture *f, const char *source, const char *name);
int flow_basic(struct fixture *f, const char *id);
int flow_basic_mock(int argc, char **argv);
int flow_space(struct fixture *f);
int flow_space_mock(int argc, char **argv);
int flow_agents_guest(struct fixture *f);
int flow_credential_arguments(struct fixture *f);
int flow_check_workload(int argc, char **argv);
int flow_worker(struct fixture *f);
int flow_worker_mock(int argc, char **argv);
int flow_leftovers(struct fixture *f);
int flow_tidy(struct fixture *f);
int flow_version(struct fixture *f);
int flow_version_mock(int argc, char **argv);
int flow_version_mock_source(const char *path);
int flow_snapshot(const char *const paths[], char hex[65]);
int flow_deploy(struct fixture *f);
int flow_deploy_mock(int argc, char **argv);
int flow_testing(struct fixture *f);
int flow_testing_mock(int argc, char **argv);
int flow_status(struct fixture *f);
int flow_status_lock(int argc, char **argv);
int flow_measure(struct fixture *f);
int flow_measure_mock(int argc, char **argv);
int flow_ci(struct fixture *f);
int flow_ci_retention_setup(struct fixture *f);
int flow_ci_retained_out(struct fixture *f, char *out, size_t size);
int flow_ci_preparation(struct fixture *f);
int flow_ci_reader_record(struct fixture *f, const char *program);
int flow_ci_mock(int argc, char **argv);
int flow_board(struct fixture *f);
int flow_board_mock(int argc, char **argv);
int flow_ci_run(struct fixture *f);
int flow_ci_run_mock(int argc, char **argv);
int flow_release(struct fixture *f);
int flow_release_mock(int argc, char **argv);
int flow_tooling(struct fixture *f);
int flow_tooling_mock(int argc, char **argv);
int flow_launch(struct fixture *f);
int flow_launch_mock(int argc, char **argv);
int flow_launch_aux(int argc, char **argv);
#endif
