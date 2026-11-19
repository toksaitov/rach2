/*
 * drm_blank.h: the console's blank as its DRM client carries it out, the
 * console's display turned off and on again, as Linux's fbdev emulation
 * does for a blank of its console (drm_fb_helper_blank).
 */
#ifndef _DRM_BLANK_H_
#define _DRM_BLANK_H_

/**
 * Ask the console's DRM client to turn off each CRTC it shows the console
 * on, while `blank` is set, and to turn on again those it turned off, when
 * it is clear: Linux's drm_fb_helper_dpms, which sets each CRTC's `active`
 * and keeps its mode and planes, so that an eDP panel loses its power and
 * its backlight. The commits are the client's worker's, `kdrmcon`, since
 * they may sleep; nothing is done on the firmware's framebuffer, with no
 * native device, or while a master holds the device, whose own commits
 * decide. Any context, the tick with interrupts off among them; it does
 * not sleep. (drm_client.c)
 */
void drm_client_blank(int blank);

#endif /* _DRM_BLANK_H_ */
