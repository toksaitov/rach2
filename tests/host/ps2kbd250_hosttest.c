/**
 * Run ps2kbd_hosttest's rows at the shipped 250 Hz tick: the LED command
 * waits 13 ticks between commands and 25 for an answer, Linux's
 * msecs_to_jiffies(50) and msecs_to_jiffies(100) at HZ=250.
 *
 * host-deps: rach2/sys/lib/kstring.c rach2/sys/lib/kctypes.c
 */
#include "ps2kbd_hosttest.c"
