/*
 * failures.h: the attempts of a run that did not pass, read from the run's
 * history, as tools/ci/report.sh gives them in ci's issues.
 */
#ifndef ETUDE_FAILURES_H
#define ETUDE_FAILURES_H

/**
 * Run `etude show failures [-s SCHEMAS] [-b STORE] HISTORY...`.
 *
 * Read the history streams HISTORY, which must hold one plan and at most
 * one run record of it, and print, for each item of the plan that did not
 * pass on every attempt, its case and each of its attempts, with the
 * assertions that did not pass from the results documents that STORE keeps.
 * A failed attempt stays listed when a later attempt of its item passed, and
 * when the run record calls the item passed.
 *
 * @return 0 when the run record says the run passed and every attempt of
 *         every item passed; 1 when one did not, or the run has no run
 *         record; 2 for a wrong usage, a stream that cannot be read, is
 *         invalid or ends in an incomplete record, a record whose identity
 *         has two contents, a history without exactly one plan, or a record
 *         that names a plan, item or attempt the history lacks.
 */
int etude_failures_main(int argc, char **argv);

#endif
