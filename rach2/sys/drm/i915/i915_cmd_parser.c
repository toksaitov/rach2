/*
 * i915_cmd_parser.c: the check of a program's batch before the render or
 * blitter ring runs it, where Linux 6.18's i915_cmd_parser.c will stand.
 */

#include "i915.h"

#include "../../lib/kerrno.h"

/*
 * Linux parses each batch on Haswell's render and blitter rings whose
 * length the call gives against its tables of commands and registers,
 * copying it to a shadow that the ring runs as privileged. Until those
 * tables are here, a batch of a caller with privilege passes unparsed, and
 * then runs privileged as a parsed one does; every other batch is refused
 * with -EACCES, the error Linux's parser gives a command it rejects.
 */
int32_t i915_cmd_parser_check(struct i915_engine *engine, struct drm_i915_gem_object *batch,
                              uint32_t start, uint32_t len, int privileged)
{
    (void)engine;
    (void)batch;
    (void)start;
    (void)len;
    return privileged ? 0 : -EACCES;
}
