/*
 * backend.h: the parts of the VM's backend around device.c: the renderer
 * on libvirglrenderer (virgl.c), the scanout on the VM's KMS (kms.c) and
 * the region's sources and doorbells (main.c), the probe that imports
 * resources into Vulkan as Venus does (vkprobe.c), and the report of a
 * stall (stall.c).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef BACKEND_H
#define BACKEND_H

#include <signal.h>
#include <stdint.h>

#include "vgpudev.h"

/* A resource's pixels as a dma-buf the display can scan out. */
struct vr_export {
    int fd;          /* the dma-buf, the caller's to close */
    uint32_t fourcc; /* DRM's format */
    uint32_t stride, offset, width, height;
    uint64_t modifier;
    int bottom_up; /* its first row is the bottom one, as GL draws */
};

struct vr;
struct kms;

/* The pins of page lists (VGS_TF_PAGES): made, refused and let go of. */
struct vr_pin_counts {
    uint64_t pinned, refused, unpinned;
    uint64_t backed; /* of the pins made, backings of the VM's memory */
};

/* The renderer, after vr_init; ops and cookie go to vd_init. */
struct vr *vr_init(struct vd *dev, int gbm_fd, int venus, int native, int verbose);
extern const struct vd_ops vr_ops;
int vr_poll_fd(struct vr *r);
int vr_async_fence(struct vr *r);
void vr_poll(struct vr *r);
uint32_t vr_capsets(struct vr *r);
int vr_read(struct vr *r, uint32_t id, const struct vd_rect *rect, void *dst, uint32_t stride);
int vr_export(struct vr *r, uint32_t id, struct vr_export *e);
void vr_set_kms(struct vr *r, struct kms *k);
void vr_set_flush_crc(struct vr *r, int enabled);
void vr_set_probe(struct vr *r, int probe);
void vr_set_module(struct vr *r, int fd);
const struct vr_pin_counts *vr_pin_counts(struct vr *r);

/* Set by SIGUSR2: the serving thread holds the next new backing VR_HOLD_S
 * seconds before it makes it, past the driver's VGS_BEAT_LOST_MS, so that
 * Rach2's test can see a long command answered and not taken for a lost
 * device (tests/kvm/bed-shm.sh). */
#define VR_HOLD_S 45
extern volatile sig_atomic_t vr_hold;
/* Set by SIGWINCH: the serving thread refuses the next new backing for want
 * of memory, as the VM's counts refuse one, so that Rach2's test can see
 * the driver back a resource from the region's heap instead
 * (tests/kvm/bed-shm.sh). */
extern volatile sig_atomic_t vr_refuse;

/* The Venus import probe: a dma-buf imported into Vulkan and read. */
int vk_probe(const struct vr_export *e, uint32_t *crc);

/* The display: one connector of a KMS device, its crtc and primary plane. */
struct kms_counts {
    uint64_t flushes, copies, direct, modesets, flips, skipped, writebacks;
};

struct kms *kms_open(int fd, const char *path, struct vr *r, int direct, int writeback);
int kms_edid(struct kms *k, uint8_t *out, uint32_t *size);
int kms_display(struct kms *k, struct vd_mode *m);
int kms_scanout(struct kms *k, uint32_t id, const struct vd_rect *r);
int kms_flush(struct kms *k, uint32_t id, const struct vd_rect *r);
void kms_forget(struct kms *k, uint32_t id);
void kms_reset(struct kms *k);
const struct kms_counts *kms_counts(struct kms *k);
void kms_close(struct kms *k);

uint32_t crc32_bytes(const uint8_t *p, uint64_t n);

/* The stall report (stall.c): the seconds the serving thread may stay in
 * one place, or its oldest fenced command wait, before the report, and
 * between reports while the stall lasts. */
#define STALL_S       30
#define STALL_AGAIN_S 300

void stall_start(const struct vd *d);
void stall_beat(const char *at);
void stall_report(const char *why);

#endif /* BACKEND_H */
