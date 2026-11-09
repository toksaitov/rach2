/*
 * drm_client.c: the console as an in-kernel DRM client, which draws on a
 * device's dumb buffer and gives the screen to a master and takes it back.
 * A stub until 76-11a writes it: the console stays on the loader's
 * framebuffer, and devices come and go unseen.
 */

#include "drm.h"

void drm_client_dev_register(struct drm_device *dev)
{
    (void)dev;
}

void drm_client_dev_unregister(struct drm_device *dev)
{
    (void)dev;
}
