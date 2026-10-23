/* Native commands for fixtures whose kernel headers exclude hosted types. */
#ifndef RACH2_HOST_COMMAND_H
#define RACH2_HOST_COMMAND_H

/* A prepared tool or data path, below HOST_TEST_TOOLS when set. */
const char *host_tool_path(const char *name);

/*
 * Run argv directly, with optional complete output files and stdout capture.
 * Return the exit status, 128 plus a signal, or -1 on incomplete execution.
 */
int host_command(const char *const argv[], const char *out_path, const char *err_path, char *out,
                 unsigned long capacity);

#endif
