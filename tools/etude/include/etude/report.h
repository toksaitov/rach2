/*
 * report.h: the report records a case's program writes on its standard
 * output, for programs on the host and on Rach2. It defines strings and
 * limits alone, in C89, so that a probe without a C library can use it.
 */
#ifndef ETUDE_REPORT_H
#define ETUDE_REPORT_H

/*
 * A record is one line: the prefix, a verb, its fields separated by single
 * spaces, and a newline, at most ETUDE_REPORT_LINE_MAX bytes in all. A
 * program writes each record with one write(2), so that on the console no
 * kernel record can fall inside it. The records, in order:
 *
 *   @etude 1 begin CASE                first; the case the program runs
 *   @etude 1 assert ID pass            the assertion ID held
 *   @etude 1 assert ID fail DETAIL     it did not; DETAIL says what was seen
 *   @etude 1 notrun ID REASON          it could not be evaluated
 *   @etude 1 branch GROUP ALTERNATIVE  which alternative of GROUP ran
 *   @etude 1 progress WITNESS UNITS    WITNESS has completed UNITS in all;
 *                                      at a quiet witness's present UNITS,
 *                                      its work begins
 *   @etude 1 child PID                 a process that may outlive the program
 *   @etude 1 machine BOOT END          a machine scenario's alone: the boot
 *                                      BOOT, MACHINE.N, ended as END, such
 *                                      as panic, halted or reset; at most
 *                                      one for each boot
 *   @etude 1 end COUNT                 last; COUNT assert, notrun and branch
 *                                      records came before it
 *
 * Every other line of the output is the program's own text, kept as
 * evidence and never read as a result.
 */
#define ETUDE_REPORT_PREFIX   "@etude 1 "
#define ETUDE_REPORT_LINE_MAX 255

#define ETUDE_REPORT_BEGIN    "begin"
#define ETUDE_REPORT_ASSERT   "assert"
#define ETUDE_REPORT_NOTRUN   "notrun"
#define ETUDE_REPORT_BRANCH   "branch"
#define ETUDE_REPORT_PROGRESS "progress"
#define ETUDE_REPORT_CHILD    "child"
#define ETUDE_REPORT_END      "end"
#define ETUDE_REPORT_MACHINE  "machine"

#define ETUDE_REPORT_PASS "pass"
#define ETUDE_REPORT_FAIL "fail"

#endif
