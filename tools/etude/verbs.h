/*
 * verbs.h: the entry of each command of etude, which etude.c runs for the
 * verb and the form its arguments name. Each takes its arguments as main
 * does, with `argv[0]` the path of etude itself and `argv[1]` the first
 * argument after the verb and form, and returns the command's exit status.
 */
#ifndef ETUDE_VERBS_H
#define ETUDE_VERBS_H

int etude_observe_main(int argc, char **argv);
int etude_collection_prepare_main(int argc, char **argv);
int etude_collection_main(int argc, char **argv);
int etude_suites_main(int argc, char **argv);
int etude_host_main(int argc, char **argv);
int etude_native_main(int argc, char **argv);
int etude_native_collection_main(int argc, char **argv);
int etude_prepare_main(int argc, char **argv);
int etude_check_main(int argc, char **argv);
int etude_record_main(int argc, char **argv);
int etude_lines_main(int argc, char **argv);
int etude_failures_main(int argc, char **argv);
int cmp_main(int argc, char **argv);
int etude_budget_main(int argc, char **argv);
int etude_lock_tree_main(int argc, char **argv);
int etude_lock_ports_main(int argc, char **argv);
int etude_admit_main(int argc, char **argv);
int etude_status_main(int argc, char **argv);
int etude_release_main(int argc, char **argv);
int etude_serve_main(int argc, char **argv);
int etude_load_main(int argc, char **argv);
int etude_tooling_main(int argc, char **argv);
int etude_build_main(int argc, char **argv);

#ifdef _WIN32
int etude_win_bridge_main(int argc, char **argv);
#endif

#endif
