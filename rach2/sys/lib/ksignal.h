#ifndef _KSIGNAL_H_
#define _KSIGNAL_H_

/*
 * Linux i386 signal numbers.
 *
 * Phase 5 (copy-on-write fork) uses a few of these only for the wait-status
 * encoding: a process killed by a fault stores its signal number so the parent's
 * waitpid/wait4 reports WIFSIGNALED/WTERMSIG correctly. The actual signal
 * delivery machinery (handlers, masks, sigreturn) arrives in Phase 8; the numbers
 * are stable, so they live here from the start.
 */
#define SIGINT   2  /* terminal interrupt (CTRL+C) */
#define SIGKILL  9  /* uncatchable kill */
#define SIGSEGV 11  /* invalid memory reference */

#endif /* _KSIGNAL_H_ */
