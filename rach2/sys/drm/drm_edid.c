/*
 * drm_edid.c: a sink's EDID, its blob and the modes read from it. A stub
 * until 76-5a writes it, which records no EDID and finds no mode.
 */

#include "drm.h"

int drm_connector_update_edid(struct drm_connector *connector, const uint8_t *edid, uint32_t len)
{
    (void)connector;
    (void)edid;
    (void)len;
    return 0;
}
