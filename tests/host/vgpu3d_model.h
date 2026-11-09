/* vgpu3d_model.h: a virgl device over the 2D test's queues and backing. A
 * test that defines VGPU_BLOB_TEST gives `execute_blob` the commands this
 * model does not know. */
#ifndef VGPU3D_MODEL_H
#define VGPU3D_MODEL_H

#ifdef VGPU_BLOB_TEST
static uint32_t execute_blob(const uint8_t *req, uint32_t n, uint8_t *resp, uint32_t *ok);
#endif

static struct {
    uint32_t contexts[64];
    uint32_t context_init[64]; /* by slot of `contexts` */
    char name[64];             /* the last CTX_CREATE's debug name */
    uint32_t capsets[8];       /* the ids offered; none means 1 and 2 */
    uint32_t ncapsets;
    uint32_t submit_flags, submit_ring; /* the last SUBMIT_3D's header */
    uint32_t creates, destroys, attaches, detaches, submits;
    uint32_t cap_queries;
    struct virtio_gpu_resource_create_3d resource;
    struct virtio_gpu_transfer_3d transfer;
    uint8_t submitted[8192];
    uint32_t submitted_size;
} three;

/** Whether the model has a context with this id. */
static int context_present(uint32_t id)
{
    uint32_t i;

    for (i = 0; i < 64; i++) {
        if (id && three.contexts[i] == id) {
            return 1;
        }
    }
    return 0;
}

/** Copy host resource bytes into backing pages, the reverse of backing_read. */
static void backing_write(struct model_res *r, uint64_t off, const uint8_t *src, uint32_t len)
{
    uint32_t i, here;
    uint64_t base = 0;

    for (i = 0; i < r->nents && len; i++) {
        if (off >= base + r->ents[i].length) {
            base += r->ents[i].length;
            continue;
        }
        here = r->ents[i].length - (uint32_t)(off - base);
        if (here > len) {
            here = len;
        }
        k_memcpy((void *)(uintptr_t)(r->ents[i].addr + off - base), src, here);
        src += here;
        off += here;
        len -= here;
        base += r->ents[i].length;
    }
    CHECK(!len, "the readback fits the backing entries");
}

/** The id of capability set `i` the model offers, or 0 past them. */
static uint32_t capset_id(uint32_t i)
{
    if (!three.ncapsets) {
        return i < 2 ? i + 1 : 0;
    }
    return i < three.ncapsets ? three.capsets[i] : 0;
}

/** Whether the model offers capability set `id`. */
static int capset_offered(uint32_t id)
{
    uint32_t i;

    for (i = 0; capset_id(i); i++) {
        if (capset_id(i) == id) {
            return 1;
        }
    }
    return 0;
}

/** Interpret the 3D wire commands against contexts and byte resources.
 * Capability bytes are an independent repeating sequence spanning two pages. */
static uint32_t execute_3d(const uint8_t *req, uint32_t n, uint8_t *resp, uint32_t *ok)
{
    const struct virtio_gpu_ctrl_hdr *hdr = (const void *)req;
    uint32_t i, id;
    struct model_res *r;
    const struct virtio_gpu_transfer_3d *t;
    const struct virtio_gpu_resource_unref *a;
    const struct virtio_gpu_resource_create_3d *c;
    const struct virtio_gpu_submit_3d *s;
    struct virtio_gpu_resource_create_2d two;
    struct virtio_gpu_capset_info *info;
    const struct virtio_gpu_get_capset *caps;

    switch (hdr->type) {
    case VIRTIO_GPU_CMD_GET_CAPSET_INFO:
        i = ((const struct virtio_gpu_get_capset_info *)(const void *)req)->index;
        CHECK(n == 32 && capset_id(i), "capset info has the wire size and index");
        info = (void *)resp;
        info->id = capset_id(i);
        info->version = 2;
        info->size = info->id == 1 ? 512 : 5000;
        *ok = VIRTIO_GPU_RESP_OK_CAPSET_INFO;
        three.cap_queries++;
        return 40;
    case VIRTIO_GPU_CMD_GET_CAPSET:
        caps = (const void *)req;
        CHECK(n == 32 && capset_offered(caps->id) && caps->version <= 2,
              "GET_CAPSET carries id and version, not index");
        for (i = 0; i < (caps->id == 1 ? 512u : 5000u); i++) {
            resp[24 + i] = (uint8_t)(i * 7 + caps->id);
        }
        *ok = VIRTIO_GPU_RESP_OK_CAPSET;
        return 24 + (caps->id == 1 ? 512 : 5000);
    case VIRTIO_GPU_CMD_CTX_CREATE:
        CHECK(n == 96 && hdr->ctx_id && !context_present(hdr->ctx_id),
              "CTX_CREATE has a fresh context id and its full wire payload");
        for (i = 0; i < 64 && three.contexts[i]; i++) {
        }
        CHECK(i < 64, "the model has context room");
        if (i < 64) {
            three.contexts[i] = hdr->ctx_id;
            three.context_init[i] =
                ((const struct virtio_gpu_ctx_create *)(const void *)req)->context_init;
            k_memset(three.name, 0, sizeof(three.name));
            k_memcpy(three.name,
                     ((const struct virtio_gpu_ctx_create *)(const void *)req)->debug_name,
                     ((const struct virtio_gpu_ctx_create *)(const void *)req)->nlen & 63);
        }
        three.creates++;
        break;
    case VIRTIO_GPU_CMD_CTX_DESTROY:
        CHECK(n == 24 && context_present(hdr->ctx_id), "CTX_DESTROY names a live context");
        for (i = 0; i < 64; i++) {
            if (three.contexts[i] == hdr->ctx_id) {
                three.contexts[i] = 0;
            }
        }
        three.destroys++;
        break;
    case VIRTIO_GPU_CMD_RESOURCE_CREATE_3D:
        CHECK(n == 72, "RESOURCE_CREATE_3D has Linux's wire size");
        c = (const void *)req;
        three.resource = *c;
        k_memset(&two, 0, sizeof(two));
        two.resource_id = c->resource_id;
        two.format = c->format;
        two.width = c->width;
        two.height = c->height;
        *ok = exec_create(&two);
        break;
    case VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE:
    case VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE:
        a = (const void *)req;
        CHECK(n == 32 && context_present(hdr->ctx_id) && res_find(a->resource_id),
              "context attachment names a live context and resource");
        if (hdr->type == VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE) {
            three.attaches++;
        } else {
            three.detaches++;
        }
        break;
    case VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D:
    case VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D:
        t = (const void *)req;
        three.transfer = *t;
        r = res_find(t->resource_id);
        CHECK(n == 72 && context_present(hdr->ctx_id) && r && r->ents,
              "3D transfer names backed resource and context with the wire layout");
        if (r && r->ents) {
            if (hdr->type == VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D) {
                backing_read(r, t->offset, r->pixels + t->x, t->w);
            } else {
                backing_write(r, t->offset, r->pixels + t->x, t->w);
            }
        }
        break;
    case VIRTIO_GPU_CMD_SUBMIT_3D:
        s = (const void *)req;
        CHECK(context_present(hdr->ctx_id) && n == 32 + s->size,
              "SUBMIT_3D appends exactly its size of command bytes");
        three.submitted_size = s->size;
        three.submit_flags = hdr->flags;
        three.submit_ring = hdr->ring_idx;
        if (s->size <= sizeof(three.submitted)) {
            k_memcpy(three.submitted, req + 32, s->size);
        }
        three.submits++;
        break;
    default:
#ifdef VGPU_BLOB_TEST
        return execute_blob(req, n, resp, ok);
#else
        *ok = VIRTIO_GPU_RESP_ERR_UNSPEC;
        break;
#endif
    }
    id = hdr->ctx_id;
    (void)id;
    return 24;
}
#endif
