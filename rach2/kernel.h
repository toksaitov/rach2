#ifndef _KERNEL_H_
#define _KERNEL_H_

#include "sys/boot.h"

#include "sys/lib/kint.h"

/*
 * What kernel.c exports: the shared boot the architecture's entry calls
 * (sys/boot.h), and the two calls the interrupt path and the system calls
 * make into the process shell.
 */

/** Ends the running process as if by `signo` and schedules the next;
 * `signo | WCOREFLAG` leaves a core file first where one is allowed. */
void terminate_current_process(int32_t signo);

/** Halts, reboots or powers the machine off; `how` is the reboot(2) command. */
void kernel_shutdown(int how);

#endif
