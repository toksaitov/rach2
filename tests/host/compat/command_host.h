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

/*
 * Fork the process that runs host_command's commands from now on, while the
 * test is small: a fork copies the forking process's page tables, and a test
 * under the sanitizers grows to hundreds of megabytes. Each command runs in
 * the caller's environment and directory at the call. Nothing on Windows;
 * when the server cannot start or is lost, the test runs its commands itself.
 */
void host_command_server(void);

#endif
