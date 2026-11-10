/*
 * backend.h: the parts of the VM's backend around device.c: the renderer
 * on libvirglrenderer (virgl.c), the scanout on the VM's KMS (kms.c) and
 * the region's sources and doorbells (main.c).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef BACKEND_H
#define BACKEND_H

#include <stdint.h>

#include "vgpudev.h"

/* A resource's pixels as a dma-buf the display can scan out. */
struct vr_export {
    int fd;          /* the dma-buf, the caller's to close */
    uint32_t fourcc; /* DRM's format */
    uint32_t stride, offset, width, height;
    uint64_t modifier;
};

struct vr;
struct kms;

/* The renderer, after vr_init; ops and cookie go to vd_init. */
struct vr *vr_init(struct vd *dev, int gbm_fd, int venus, int verbose);
extern const struct vd_ops vr_ops;
int vr_poll_fd(struct vr *r);
void vr_poll(struct vr *r);
uint32_t vr_capsets(struct vr *r);
int vr_read(struct vr *r, uint32_t id, const struct vd_rect *rect, void *dst, uint32_t stride);
int vr_export(struct vr *r, uint32_t id, struct vr_export *e);
void vr_set_kms(struct vr *r, struct kms *k);

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

#endif /* BACKEND_H */
