/* Read native process argument vectors for fixture ownership checks. */
#ifndef RACH2_HOST_PROCESS_H
#define RACH2_HOST_PROCESS_H

/* Visit accessible live processes. Return -1 on an enumeration failure. */
int host_process_arguments(int (*visit)(long pid, const char *arguments, void *ctx), void *ctx);

#endif
