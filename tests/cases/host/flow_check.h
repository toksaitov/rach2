/* flow_check.h: C controllers for flow and deployment regressions. */
#ifndef HOST_FLOW_CHECK_H
#define HOST_FLOW_CHECK_H
#include "fixture.h"
const char *flow_path(struct fixture *f, const char *name);
const char *flow_source(struct fixture *f, const char *name);
int flow_put(struct fixture *f, const char *name, const char *text);
int flow_copy(struct fixture *f, const char *source, const char *name);
int flow_basic(struct fixture *f, const char *id);
int flow_basic_mock(int argc, char **argv);
int flow_progress(struct fixture *f);
int flow_progress_mock(int argc, char **argv);
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
#endif
