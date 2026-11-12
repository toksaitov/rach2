/* The interrupt flag's test for a host test that runs as a thread would in a
 * system call, with interrupts on (lib/kirq.h). */
#include "../../../rach2/sys/lib/kirq.h"

/** Interrupts are on, so a caller may sleep. */
int kirq_enabled(void)
{
    return 1;
}
