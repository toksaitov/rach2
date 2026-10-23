/*
 * lines.h: the compatible formatter, which prints a run's history as the
 * lines tests/run printed, each suite's result and the run's.
 */
#ifndef ETUDE_LINES_H
#define ETUDE_LINES_H

/**
 * Run `etude show lines [-s SCHEMAS] MAP STREAM...`, as tools/etude/Readme.md
 * says: read the run in the history streams, judge each suite of the suite
 * map by its cases, and print the lines of tests/run.
 *
 * @return 0 when every suite passed and the history is whole, 1 when a suite
 *         failed or did not finish or the runner failed, and 2 for a wrong
 *         usage or a map that cannot be used.
 */
int etude_lines_main(int argc, char **argv);

#endif
