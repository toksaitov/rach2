/* family_rows.h: report identities shared by native and guest controllers. */
#ifndef HOST_FAMILY_ROWS_H
#define HOST_FAMILY_ROWS_H
#include <string.h>
static const char *family_rows(const char *id)
{
    if (!strcmp(id, "machine.grow.measure"))
        return "valid-sub-tick all-zero wrong-phase missing-witness duplicate-witness wrong-order";
    if (!strcmp(id, "machine.grow.check"))
        return "plan-small ranges-small plan-samsung ranges-samsung plan-samsung-gpt "
               "ranges-samsung-gpt boot-copy boot-corrupt";
    if (!strcmp(id, "machine.grow.boot"))
        return "unchanged unparseable torn repaired retry-unchanged candidate own-input";
    if (!strcmp(id, "machine.grow.space"))
        return "work absent producer log ranges output geometry measured measure-fail overflow";
    if (!strcmp(id, "machine.grow.memory"))
        return "status refused";
    if (!strcmp(id, "machine.grow.replay"))
        return "records bytes";
    return NULL;
}
#endif
