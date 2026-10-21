/*
 * console_report.h: the machine scenarios' reading of a probe's report
 * records from the console of the machine that ran it.
 */
#ifndef CONSOLE_REPORT_H
#define CONSOLE_REPORT_H

#include <stdint.h>

#include "etude/scenario.h"

/**
 * Make the report records that a program writes on the console of machine
 * `m`, boot `boot` of it counted from 1, the case's own.
 *
 * The program was started at the kernel's debug shell after console offset
 * `from`, typically by `exec /PROGRAM` typed with etude_type. The function
 * waits for the program's `end` record and then reads the console's evidence
 * file, `MACHINE.BOOT.console`, from the program's `begin` record on: each
 * `assert`, `notrun`, `branch` and `progress` record becomes the scenario's,
 * and a `child` record is passed over, since the machine's end ends every
 * process of it. Any other line is the program's or the kernel's own output.
 *
 * The begin may give any case identity: a program started at the debug
 * shell has no ETUDE_CASE and gives its own default. `*at`, when `at` is not
 * NULL, is the console offset just past the end record.
 *
 * @return ETUDE_OK when the report was read whole; ETUDE_FAILED, with the
 *         reason on stderr, when it had no begin before its end, a record
 *         did not parse, or the end's count disagrees with the records; or
 *         the status of a wait that did not complete.
 */
enum etude_status console_report(struct etude_case *c, struct etude_case_machine *m,
                                 const char *machine, unsigned boot, uint64_t from, uint64_t *at);

#endif
