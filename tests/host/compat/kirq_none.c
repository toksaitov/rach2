/* The interrupt flag for a host test with no interrupts (lib/kirq.h). */
#include "../../../rach2/sys/lib/kirq.h"

/** No interrupt can come, so there is no flag to save. */
uint32_t kirq_save(void)
{
    return 0;
}

/** Nothing was saved. */
void kirq_restore(uint32_t flags)
{
    (void)flags;
}
