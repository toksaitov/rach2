#ifndef _KERNEL_H_
#define _KERNEL_H_

#include "sys/lib/kint.h"
#include "vendor/multiboot.h"

/*
 * What kernel.c exports: the entry the loader jumps to, and the two calls
 * the interrupt path and the system calls make into the process shell.
 */

/** The C entry, from loader.asm, with the Multiboot magic and info. */
void k_main(multiboot_uint32_t magic, multiboot_info_t *info);

/** Ends the running process as if by `signo` and schedules the next;
 * `signo | WCOREFLAG` leaves a core file first where one is allowed. */
void terminate_current_process(int32_t signo);

/** Halts, reboots or powers the machine off; `how` is the reboot(2) command. */
void kernel_shutdown(int how);

#endif
