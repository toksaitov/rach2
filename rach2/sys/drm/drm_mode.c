/*
 * drm_mode.c: the mode objects of a device's display, Linux's KMS objects:
 * their IDs and properties, blobs, pixel formats and framebuffers, the
 * drivers' calls that make CRTCs, planes, encoders and connectors, the
 * probe of a connector's modes, and the ioctls that report them.
 */

#include "drm.h"

#include "../lib/kerrno.h"
#include "../lib/kstring.h"
#include "../mm/kheap.h"
#include "../mm/usercopy.h"
#include "../proc/process.h"

/* Linux's INT_MAX and UINT_MAX, the bounds of its properties and sizes. */
#define DRM_INT_MAX  0x7fffffffL
#define DRM_INT_MIN  (-DRM_INT_MAX - 1)
#define DRM_UINT_MAX 0xffffffffUL

/* The last stereo layout a mode may name, Linux's DRM_MODE_FLAG_3D_MAX. */
#define DRM_MODE_FLAG_3D_MAX DRM_MODE_FLAG_3D_SIDE_BY_SIDE_HALF

/* ------------------------------------------------------------------------
 * Pixel formats
 */

/* The formats the core knows, with Linux's descriptions (drm_fourcc.c). A
 * framebuffer of any other format is refused, as Linux refuses one it does
 * not know; a format here no plane takes is refused too. */
static const struct drm_format_info formats[] = {
    {DRM_FORMAT_C8, 8, 1, {1, 0, 0}, 1, 1, 0},
    {DRM_FORMAT_XRGB1555, 15, 1, {2, 0, 0}, 1, 1, 0},
    {DRM_FORMAT_ARGB1555, 15, 1, {2, 0, 0}, 1, 1, 1},
    {DRM_FORMAT_RGB565, 16, 1, {2, 0, 0}, 1, 1, 0},
    {DRM_FORMAT_BGR565, 16, 1, {2, 0, 0}, 1, 1, 0},
    {DRM_FORMAT_RGB888, 24, 1, {3, 0, 0}, 1, 1, 0},
    {DRM_FORMAT_BGR888, 24, 1, {3, 0, 0}, 1, 1, 0},
    {DRM_FORMAT_XRGB8888, 24, 1, {4, 0, 0}, 1, 1, 0},
    {DRM_FORMAT_XBGR8888, 24, 1, {4, 0, 0}, 1, 1, 0},
    {DRM_FORMAT_RGBX8888, 24, 1, {4, 0, 0}, 1, 1, 0},
    {DRM_FORMAT_BGRX8888, 24, 1, {4, 0, 0}, 1, 1, 0},
    {DRM_FORMAT_XRGB2101010, 30, 1, {4, 0, 0}, 1, 1, 0},
    {DRM_FORMAT_XBGR2101010, 30, 1, {4, 0, 0}, 1, 1, 0},
    {DRM_FORMAT_ARGB2101010, 30, 1, {4, 0, 0}, 1, 1, 1},
    {DRM_FORMAT_ABGR2101010, 30, 1, {4, 0, 0}, 1, 1, 1},
    {DRM_FORMAT_ARGB8888, 32, 1, {4, 0, 0}, 1, 1, 1},
    {DRM_FORMAT_ABGR8888, 32, 1, {4, 0, 0}, 1, 1, 1},
    {DRM_FORMAT_RGBA8888, 32, 1, {4, 0, 0}, 1, 1, 1},
    {DRM_FORMAT_BGRA8888, 32, 1, {4, 0, 0}, 1, 1, 1},
    {DRM_FORMAT_YUV420, 0, 3, {1, 1, 1}, 2, 2, 0},
    {DRM_FORMAT_YVU420, 0, 3, {1, 1, 1}, 2, 2, 0},
    {DRM_FORMAT_NV12, 0, 2, {1, 2, 0}, 2, 2, 0},
    {DRM_FORMAT_NV21, 0, 2, {1, 2, 0}, 2, 2, 0},
};

#define NFORMATS ((int)(sizeof(formats) / sizeof(formats[0])))

const struct drm_format_info *drm_format_info(uint32_t format)
{
    int i;

    for (i = 0; i < NFORMATS; i++) {
        if (formats[i].format == format) {
            return &formats[i];
        }
    }
    return NULL;
}

uint32_t drm_mode_legacy_fb_format(uint32_t bpp, uint32_t depth)
{
    switch (bpp) {
    case 8:
        return depth == 8 ? DRM_FORMAT_C8 : DRM_FORMAT_INVALID;
    case 16:
        if (depth == 15) {
            return DRM_FORMAT_XRGB1555;
        }
        return depth == 16 ? DRM_FORMAT_RGB565 : DRM_FORMAT_INVALID;
    case 24:
        return depth == 24 ? DRM_FORMAT_RGB888 : DRM_FORMAT_INVALID;
    case 32:
        if (depth == 24) {
            return DRM_FORMAT_XRGB8888;
        }
        if (depth == 30) {
            return DRM_FORMAT_XRGB2101010;
        }
        return depth == 32 ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_INVALID;
    default:
        return DRM_FORMAT_INVALID;
    }
}

/** The width of plane `i` of a framebuffer `width` pixels wide. */
static uint32_t plane_width(const struct drm_format_info *info, uint32_t width, int i)
{
    return i == 0 ? width : (width + info->hsub - 1) / info->hsub;
}

/** The height of plane `i` of a framebuffer `height` lines high. */
static uint32_t plane_height(const struct drm_format_info *info, uint32_t height, int i)
{
    return i == 0 ? height : (height + info->vsub - 1) / info->vsub;
}

/* ------------------------------------------------------------------------
 * Mode objects
 */

/** The fixed or listed object of `dev` with ID `id`, of any type, or NULL;
 * no reference is taken. */
static struct drm_mode_object *object_by_id(struct drm_device *dev, uint32_t id)
{
    struct drm_mode_config *mc = &dev->mode_config;
    struct drm_property *prop;
    struct drm_property_blob *blob;
    struct drm_framebuffer *fb;
    uint32_t i;

    if (id == 0) {
        return NULL;
    }
    for (i = 0; i < mc->num_crtc; i++) {
        if (mc->crtcs[i]->base.id == id) {
            return &mc->crtcs[i]->base;
        }
    }
    for (i = 0; i < mc->num_plane; i++) {
        if (mc->planes[i]->base.id == id) {
            return &mc->planes[i]->base;
        }
    }
    for (i = 0; i < mc->num_encoder; i++) {
        if (mc->encoders[i]->base.id == id) {
            return &mc->encoders[i]->base;
        }
    }
    for (i = 0; i < mc->num_connector; i++) {
        if (mc->connectors[i]->base.id == id) {
            return &mc->connectors[i]->base;
        }
    }
    for (prop = mc->properties; prop; prop = prop->next) {
        if (prop->base.id == id) {
            return &prop->base;
        }
    }
    for (blob = mc->blobs; blob; blob = blob->next) {
        if (blob->base.id == id) {
            return &blob->base;
        }
    }
    for (fb = mc->fbs; fb; fb = fb->next) {
        if (fb->base.id == id) {
            return &fb->base;
        }
    }
    return NULL;
}

/** Give `obj` the lowest ID of `dev` that is free, as Linux's idr does, and
 * the type `type`. */
static void object_add(struct drm_device *dev, struct drm_mode_object *obj, uint32_t type)
{
    uint32_t id = 1;

    while (object_by_id(dev, id)) {
        id++;
    }
    obj->id = id;
    obj->type = type;
}

struct drm_mode_object *drm_mode_object_find(struct drm_device *dev, struct drm_file *file,
                                             uint32_t id, uint32_t type)
{
    struct drm_mode_object *obj = object_by_id(dev, id);

    (void)file;
    if (!obj || (type != DRM_MODE_OBJECT_ANY && obj->type != type)) {
        return NULL;
    }
    if (obj->type == DRM_MODE_OBJECT_FB || obj->type == DRM_MODE_OBJECT_BLOB) {
        obj->refs++;
    }
    return obj;
}

void drm_mode_object_put(struct drm_mode_object *obj)
{
    if (!obj) {
        return;
    }
    if (obj->type == DRM_MODE_OBJECT_FB) {
        drm_framebuffer_put((struct drm_framebuffer *)obj);
    } else if (obj->type == DRM_MODE_OBJECT_BLOB) {
        drm_property_blob_put((struct drm_property_blob *)obj);
    }
}

struct drm_crtc *drm_crtc_find(struct drm_device *dev, struct drm_file *file, uint32_t id)
{
    return (struct drm_crtc *)drm_mode_object_find(dev, file, id, DRM_MODE_OBJECT_CRTC);
}

struct drm_plane *drm_plane_find(struct drm_device *dev, struct drm_file *file, uint32_t id)
{
    return (struct drm_plane *)drm_mode_object_find(dev, file, id, DRM_MODE_OBJECT_PLANE);
}

struct drm_encoder *drm_encoder_find(struct drm_device *dev, struct drm_file *file, uint32_t id)
{
    return (struct drm_encoder *)drm_mode_object_find(dev, file, id, DRM_MODE_OBJECT_ENCODER);
}

struct drm_connector *drm_connector_lookup(struct drm_device *dev, struct drm_file *file,
                                           uint32_t id)
{
    return (struct drm_connector *)drm_mode_object_find(dev, file, id, DRM_MODE_OBJECT_CONNECTOR);
}

struct drm_framebuffer *drm_framebuffer_lookup(struct drm_device *dev, struct drm_file *file,
                                               uint32_t id)
{
    return (struct drm_framebuffer *)drm_mode_object_find(dev, file, id, DRM_MODE_OBJECT_FB);
}

/* ------------------------------------------------------------------------
 * Properties
 */

/** Copy `name` into a property's name, cut to fit. */
static void prop_name(char *dst, const char *name)
{
    uint32_t i;

    for (i = 0; i + 1 < DRM_PROP_NAME_LEN && name[i]; i++) {
        dst[i] = name[i];
    }
    while (i < DRM_PROP_NAME_LEN) {
        dst[i++] = '\0';
    }
}

struct drm_property *drm_property_create(struct drm_device *dev, uint32_t flags, const char *name,
                                         uint32_t num_values)
{
    struct drm_property *prop = (struct drm_property *)kzalloc(sizeof(*prop));
    struct drm_property **link;

    if (!prop) {
        return NULL;
    }
    if (num_values) {
        prop->values = (uint64_t *)kzalloc(num_values * sizeof(uint64_t));
        if (!prop->values) {
            kfree(prop);
            return NULL;
        }
    }
    prop->dev = dev;
    object_add(dev, &prop->base, DRM_MODE_OBJECT_PROPERTY);
    prop->flags = flags;
    prop->num_values = num_values;
    prop_name(prop->name, name);
    for (link = &dev->mode_config.properties; *link; link = &(*link)->next) {
    }
    *link = prop;
    return prop;
}

struct drm_property *drm_property_create_range(struct drm_device *dev, uint32_t flags,
                                               const char *name, uint64_t min, uint64_t max)
{
    struct drm_property *prop = drm_property_create(dev, flags | DRM_MODE_PROP_RANGE, name, 2);

    if (prop) {
        prop->values[0] = min;
        prop->values[1] = max;
    }
    return prop;
}

struct drm_property *drm_property_create_signed_range(struct drm_device *dev, uint32_t flags,
                                                      const char *name, int64_t min, int64_t max)
{
    struct drm_property *prop =
        drm_property_create(dev, flags | DRM_MODE_PROP_SIGNED_RANGE, name, 2);

    if (prop) {
        prop->values[0] = (uint64_t)min;
        prop->values[1] = (uint64_t)max;
    }
    return prop;
}

struct drm_property *drm_property_create_bool(struct drm_device *dev, uint32_t flags,
                                              const char *name)
{
    return drm_property_create_range(dev, flags, name, 0, 1);
}

/** Give `prop` room for one name per value. 0 or -ENOMEM. */
static int32_t prop_names(struct drm_property *prop)
{
    prop->enum_names =
        (const char **)kzalloc((prop->num_values ? prop->num_values : 1) * sizeof(const char *));
    return prop->enum_names ? 0 : -ENOMEM;
}

/** Take `prop` off its device's list and free it. */
static void property_destroy(struct drm_property *prop)
{
    struct drm_property **link;

    for (link = &prop->dev->mode_config.properties; *link; link = &(*link)->next) {
        if (*link == prop) {
            *link = prop->next;
            break;
        }
    }
    kfree(prop->values);
    kfree((void *)prop->enum_names);
    kfree(prop);
}

struct drm_property *drm_property_create_enum(struct drm_device *dev, uint32_t flags,
                                              const char *name,
                                              const struct drm_prop_enum_list *list, uint32_t n)
{
    struct drm_property *prop = drm_property_create(dev, flags | DRM_MODE_PROP_ENUM, name, n);
    uint32_t i;

    if (!prop) {
        return NULL;
    }
    if (prop_names(prop) != 0) {
        property_destroy(prop);
        return NULL;
    }
    for (i = 0; i < n; i++) {
        prop->values[i] = (uint64_t)(int64_t)list[i].type;
        prop->enum_names[i] = list[i].name;
    }
    return prop;
}

struct drm_property *drm_property_create_bitmask(struct drm_device *dev, uint32_t flags,
                                                 const char *name,
                                                 const struct drm_prop_enum_list *list, uint32_t n,
                                                 uint64_t supported)
{
    struct drm_property *prop;
    uint32_t i, count = 0;

    for (i = 0; i < n; i++) {
        if (supported & ((uint64_t)1 << list[i].type)) {
            count++;
        }
    }
    prop = drm_property_create(dev, flags | DRM_MODE_PROP_BITMASK, name, count);
    if (!prop) {
        return NULL;
    }
    if (prop_names(prop) != 0) {
        property_destroy(prop);
        return NULL;
    }
    count = 0;
    for (i = 0; i < n; i++) {
        if (supported & ((uint64_t)1 << list[i].type)) {
            prop->values[count] = (uint64_t)list[i].type;
            prop->enum_names[count++] = list[i].name;
        }
    }
    return prop;
}

struct drm_property *drm_property_create_object(struct drm_device *dev, uint32_t flags,
                                                const char *name, uint32_t type)
{
    struct drm_property *prop = drm_property_create(dev, flags | DRM_MODE_PROP_OBJECT, name, 1);

    if (prop) {
        prop->values[0] = type;
    }
    return prop;
}

void drm_object_attach_property(struct drm_mode_object *obj, struct drm_property *prop,
                                uint64_t value)
{
    if (!prop || obj->count_props == DRM_OBJECT_MAX_PROPERTY) {
        return;
    }
    obj->props[obj->count_props] = prop;
    obj->prop_values[obj->count_props] = value;
    obj->count_props++;
}

int32_t drm_object_property_set_value(struct drm_mode_object *obj, struct drm_property *prop,
                                      uint64_t value)
{
    uint32_t i;

    for (i = 0; i < obj->count_props; i++) {
        if (obj->props[i] == prop) {
            obj->prop_values[i] = value;
            return 0;
        }
    }
    return -EINVAL;
}

int32_t drm_object_property_get_value(struct drm_mode_object *obj, struct drm_property *prop,
                                      uint64_t *value)
{
    uint32_t i;

    if (drm_core_check_feature(prop->dev, DRIVER_ATOMIC) &&
        !(prop->flags & DRM_MODE_PROP_IMMUTABLE)) {
        return drm_atomic_get_property(obj, prop, value);
    }
    for (i = 0; i < obj->count_props; i++) {
        if (obj->props[i] == prop) {
            *value = obj->prop_values[i];
            return 0;
        }
    }
    return -EINVAL;
}

struct drm_property *drm_object_find_property(const struct drm_mode_object *obj, uint32_t id)
{
    uint32_t i;

    for (i = 0; i < obj->count_props; i++) {
        if (obj->props[i]->base.id == id) {
            return obj->props[i];
        }
    }
    return NULL;
}

/** Whether `prop`'s type, its flags less the others, is `type`, as Linux's
 * drm_property_type_is. */
static int prop_type_is(const struct drm_property *prop, uint32_t type)
{
    if (type & DRM_MODE_PROP_EXTENDED_TYPE) {
        return (prop->flags & DRM_MODE_PROP_EXTENDED_TYPE) == type;
    }
    return (prop->flags & type) != 0;
}

int drm_property_change_valid(struct drm_property *prop, uint64_t value)
{
    struct drm_mode_object *ref;
    uint64_t mask = 0;
    uint32_t i;

    if (prop->flags & DRM_MODE_PROP_IMMUTABLE) {
        return 0;
    }
    if (prop_type_is(prop, DRM_MODE_PROP_RANGE)) {
        return value >= prop->values[0] && value <= prop->values[1];
    }
    if (prop_type_is(prop, DRM_MODE_PROP_SIGNED_RANGE)) {
        return (int64_t)value >= (int64_t)prop->values[0] &&
               (int64_t)value <= (int64_t)prop->values[1];
    }
    if (prop_type_is(prop, DRM_MODE_PROP_BITMASK)) {
        for (i = 0; i < prop->num_values; i++) {
            mask |= (uint64_t)1 << prop->values[i];
        }
        return (value & ~mask) == 0;
    }
    if (prop_type_is(prop, DRM_MODE_PROP_BLOB) || prop_type_is(prop, DRM_MODE_PROP_OBJECT)) {
        if (value == 0) {
            return 1;
        }
        ref = drm_mode_object_find(prop->dev, NULL, (uint32_t)value,
                                   prop_type_is(prop, DRM_MODE_PROP_BLOB)
                                       ? DRM_MODE_OBJECT_BLOB
                                       : (uint32_t)prop->values[0]);
        if (!ref || value > DRM_UINT_MAX) {
            drm_mode_object_put(ref);
            return 0;
        }
        drm_mode_object_put(ref);
        return 1;
    }
    for (i = 0; i < prop->num_values; i++) {
        if (prop->values[i] == value) {
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * Blobs
 */

struct drm_property_blob *drm_property_create_blob(struct drm_device *dev, uint32_t length,
                                                   const void *data, int32_t *err)
{
    struct drm_property_blob *blob, **link;

    if (length == 0 || length > DRM_INT_MAX - sizeof(*blob)) {
        *err = -EINVAL;
        return NULL;
    }
    blob = (struct drm_property_blob *)kzalloc(sizeof(*blob));
    if (blob) {
        blob->data = data ? kmalloc(length) : kzalloc(length);
    }
    if (!blob || !blob->data) {
        kfree(blob);
        *err = -ENOMEM;
        return NULL;
    }
    if (data) {
        k_memcpy(blob->data, data, length);
    }
    blob->dev = dev;
    blob->length = length;
    blob->base.refs = 1;
    object_add(dev, &blob->base, DRM_MODE_OBJECT_BLOB);
    for (link = &dev->mode_config.blobs; *link; link = &(*link)->next) {
    }
    *link = blob;
    return blob;
}

struct drm_property_blob *drm_property_lookup_blob(struct drm_device *dev, uint32_t id)
{
    return (struct drm_property_blob *)drm_mode_object_find(dev, NULL, id, DRM_MODE_OBJECT_BLOB);
}

struct drm_property_blob *drm_property_blob_get(struct drm_property_blob *blob)
{
    if (blob) {
        blob->base.refs++;
    }
    return blob;
}

void drm_property_blob_put(struct drm_property_blob *blob)
{
    struct drm_property_blob **link;

    if (!blob || --blob->base.refs > 0) {
        return;
    }
    for (link = &blob->dev->mode_config.blobs; *link; link = &(*link)->next) {
        if (*link == blob) {
            *link = blob->next;
            break;
        }
    }
    kfree(blob->data);
    kfree(blob);
}

int32_t drm_property_replace_blob_from_id(struct drm_device *dev, struct drm_property_blob **slot,
                                          uint64_t id, int32_t expected_size,
                                          int32_t expected_elem_size, int *replaced)
{
    struct drm_property_blob *blob = NULL;

    if (id != 0) {
        blob = id > DRM_UINT_MAX ? NULL : drm_property_lookup_blob(dev, (uint32_t)id);
        if (!blob) {
            return -EINVAL;
        }
        if ((expected_size > 0 && blob->length != (uint32_t)expected_size) ||
            (expected_elem_size > 0 && blob->length % (uint32_t)expected_elem_size != 0)) {
            drm_property_blob_put(blob);
            return -EINVAL;
        }
    }
    *replaced = *slot != blob;
    drm_property_blob_put(*slot);
    *slot = blob;
    return 0;
}

/** CREATEPROPBLOB, Linux's drm_property_create_blob_ioctl: a blob of the
 * caller's bytes, which the caller holds until DESTROYPROPBLOB or close. */
int32_t drm_mode_createblob_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_create_blob *req = (struct drm_mode_create_blob *)data;
    struct drm_property_blob *blob;
    int32_t err = 0;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    blob = drm_property_create_blob(dev, req->length, NULL, &err);
    if (!blob) {
        return err;
    }
    if (copy_from_user(blob->data, (const void __user *)(uintptr_t)req->data, req->length) != 0) {
        drm_property_blob_put(blob);
        return -EFAULT;
    }
    blob->owner = file;
    req->blob_id = blob->base.id;
    return 0;
}

/** DESTROYPROPBLOB: let go of a blob the caller made. -EINVAL for no blob,
 * -EPERM for one the caller did not make, as Linux. */
int32_t drm_mode_destroyblob_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_destroy_blob *req = (struct drm_mode_destroy_blob *)data;
    struct drm_property_blob *blob;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    blob = drm_property_lookup_blob(dev, req->blob_id);
    if (!blob) {
        return -EINVAL;
    }
    if (blob->owner != file) {
        drm_property_blob_put(blob);
        return -EPERM;
    }
    blob->owner = NULL;
    drm_property_blob_put(blob);
    drm_property_blob_put(blob);
    return 0;
}

/** GETPROPBLOB: the blob's length, and its bytes when the caller's length
 * is the blob's. */
int32_t drm_mode_getblob_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_get_blob *req = (struct drm_mode_get_blob *)data;
    struct drm_property_blob *blob;
    int32_t rc = 0;

    (void)file;
    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    blob = drm_property_lookup_blob(dev, req->blob_id);
    if (!blob) {
        return -ENOENT;
    }
    if (req->length == blob->length &&
        copy_to_user((void __user *)(uintptr_t)req->data, blob->data, blob->length) != 0) {
        rc = -EFAULT;
    } else {
        req->length = blob->length;
    }
    drm_property_blob_put(blob);
    return rc;
}

/* ------------------------------------------------------------------------
 * Making a device's display
 */

static const struct drm_prop_enum_list dpms_names[] = {{DRM_MODE_DPMS_ON, "On"},
                                                       {DRM_MODE_DPMS_STANDBY, "Standby"},
                                                       {DRM_MODE_DPMS_SUSPEND, "Suspend"},
                                                       {DRM_MODE_DPMS_OFF, "Off"}};

static const struct drm_prop_enum_list link_status_names[] = {{DRM_MODE_LINK_STATUS_GOOD, "Good"},
                                                              {DRM_MODE_LINK_STATUS_BAD, "Bad"}};

static const struct drm_prop_enum_list plane_type_names[] = {{DRM_PLANE_TYPE_OVERLAY, "Overlay"},
                                                             {DRM_PLANE_TYPE_PRIMARY, "Primary"},
                                                             {DRM_PLANE_TYPE_CURSOR, "Cursor"}};

/* The rotation bitmask's names by bit, Linux's
 * drm_plane_create_rotation_property. */
static const struct drm_prop_enum_list rotation_names[] = {{0, "rotate-0"},   {1, "rotate-90"},
                                                           {2, "rotate-180"}, {3, "rotate-270"},
                                                           {4, "reflect-x"},  {5, "reflect-y"}};

int32_t drm_mode_config_init(struct drm_device *dev)
{
    struct drm_mode_config *mc = &dev->mode_config;
    uint32_t atomic = DRM_MODE_PROP_ATOMIC;

    mc->max_width = mc->max_width ? mc->max_width : 8192;
    mc->max_height = mc->max_height ? mc->max_height : 8192;
    /* Linux's order, which gives EDID the ID 1 as on Linux. */
    mc->edid_property =
        drm_property_create(dev, DRM_MODE_PROP_BLOB | DRM_MODE_PROP_IMMUTABLE, "EDID", 0);
    mc->dpms_property = drm_property_create_enum(dev, 0, "DPMS", dpms_names, 4);
    mc->path_property =
        drm_property_create(dev, DRM_MODE_PROP_BLOB | DRM_MODE_PROP_IMMUTABLE, "PATH", 0);
    mc->tile_property =
        drm_property_create(dev, DRM_MODE_PROP_BLOB | DRM_MODE_PROP_IMMUTABLE, "TILE", 0);
    mc->link_status_property =
        drm_property_create_enum(dev, 0, "link-status", link_status_names, 2);
    mc->non_desktop_property =
        drm_property_create_bool(dev, DRM_MODE_PROP_IMMUTABLE, "non-desktop");
    mc->plane_type_property =
        drm_property_create_enum(dev, DRM_MODE_PROP_IMMUTABLE, "type", plane_type_names, 3);
    mc->prop_src_x = drm_property_create_range(dev, atomic, "SRC_X", 0, DRM_UINT_MAX);
    mc->prop_src_y = drm_property_create_range(dev, atomic, "SRC_Y", 0, DRM_UINT_MAX);
    mc->prop_src_w = drm_property_create_range(dev, atomic, "SRC_W", 0, DRM_UINT_MAX);
    mc->prop_src_h = drm_property_create_range(dev, atomic, "SRC_H", 0, DRM_UINT_MAX);
    mc->prop_crtc_x =
        drm_property_create_signed_range(dev, atomic, "CRTC_X", DRM_INT_MIN, DRM_INT_MAX);
    mc->prop_crtc_y =
        drm_property_create_signed_range(dev, atomic, "CRTC_Y", DRM_INT_MIN, DRM_INT_MAX);
    mc->prop_crtc_w = drm_property_create_range(dev, atomic, "CRTC_W", 0, DRM_INT_MAX);
    mc->prop_crtc_h = drm_property_create_range(dev, atomic, "CRTC_H", 0, DRM_INT_MAX);
    mc->prop_fb_id = drm_property_create_object(dev, atomic, "FB_ID", DRM_MODE_OBJECT_FB);
    mc->prop_in_fence_fd =
        drm_property_create_signed_range(dev, atomic, "IN_FENCE_FD", -1, DRM_INT_MAX);
    mc->prop_out_fence_ptr =
        drm_property_create_range(dev, atomic, "OUT_FENCE_PTR", 0, ~(uint64_t)0);
    mc->prop_crtc_id = drm_property_create_object(dev, atomic, "CRTC_ID", DRM_MODE_OBJECT_CRTC);
    mc->prop_fb_damage_clips =
        drm_property_create(dev, atomic | DRM_MODE_PROP_BLOB, "FB_DAMAGE_CLIPS", 0);
    mc->prop_active = drm_property_create_bool(dev, atomic, "ACTIVE");
    mc->prop_mode_id = drm_property_create(dev, atomic | DRM_MODE_PROP_BLOB, "MODE_ID", 0);
    mc->prop_vrr_enabled = drm_property_create_bool(dev, 0, "VRR_ENABLED");
    mc->degamma_lut_property = drm_property_create(dev, DRM_MODE_PROP_BLOB, "DEGAMMA_LUT", 0);
    mc->degamma_lut_size_property = drm_property_create_range(dev, DRM_MODE_PROP_IMMUTABLE,
                                                              "DEGAMMA_LUT_SIZE", 0, DRM_UINT_MAX);
    mc->ctm_property = drm_property_create(dev, DRM_MODE_PROP_BLOB, "CTM", 0);
    mc->gamma_lut_property = drm_property_create(dev, DRM_MODE_PROP_BLOB, "GAMMA_LUT", 0);
    mc->gamma_lut_size_property =
        drm_property_create_range(dev, DRM_MODE_PROP_IMMUTABLE, "GAMMA_LUT_SIZE", 0, DRM_UINT_MAX);
    mc->modifiers_property =
        drm_property_create(dev, DRM_MODE_PROP_IMMUTABLE | DRM_MODE_PROP_BLOB, "IN_FORMATS", 0);
    mc->size_hints_property =
        drm_property_create(dev, DRM_MODE_PROP_IMMUTABLE | DRM_MODE_PROP_BLOB, "SIZE_HINTS", 0);
    if (!mc->edid_property || !mc->dpms_property || !mc->path_property || !mc->tile_property ||
        !mc->link_status_property || !mc->non_desktop_property || !mc->plane_type_property ||
        !mc->prop_src_x || !mc->prop_src_y || !mc->prop_src_w || !mc->prop_src_h ||
        !mc->prop_crtc_x || !mc->prop_crtc_y || !mc->prop_crtc_w || !mc->prop_crtc_h ||
        !mc->prop_fb_id || !mc->prop_in_fence_fd || !mc->prop_out_fence_ptr || !mc->prop_crtc_id ||
        !mc->prop_fb_damage_clips || !mc->prop_active || !mc->prop_mode_id ||
        !mc->prop_vrr_enabled || !mc->degamma_lut_property || !mc->degamma_lut_size_property ||
        !mc->ctm_property || !mc->gamma_lut_property || !mc->gamma_lut_size_property ||
        !mc->modifiers_property || !mc->size_hints_property) {
        drm_mode_config_cleanup(dev);
        return -ENOMEM;
    }
    return 0;
}

/** Free a connector's mode list `*list`. */
static void modes_free(struct drm_display_mode **list)
{
    struct drm_display_mode *m;

    while ((m = *list) != NULL) {
        *list = m->next;
        kfree(m);
    }
}

void drm_mode_config_cleanup(struct drm_device *dev)
{
    struct drm_mode_config *mc = &dev->mode_config;
    struct drm_framebuffer *fb;
    uint32_t i;

    for (i = 0; i < mc->num_plane; i++) {
        drm_plane_state_destroy(mc->planes[i]->state);
        mc->planes[i]->state = NULL;
    }
    for (i = 0; i < mc->num_crtc; i++) {
        drm_crtc_state_destroy(mc->crtcs[i]->state);
        mc->crtcs[i]->state = NULL;
        kfree(mc->crtcs[i]->gamma_store);
        mc->crtcs[i]->gamma_store = NULL;
    }
    for (i = 0; i < mc->num_connector; i++) {
        struct drm_connector *c = mc->connectors[i];

        kfree(c->state);
        c->state = NULL;
        modes_free(&c->modes);
        modes_free(&c->probed_modes);
        drm_property_blob_put(c->edid);
        drm_property_blob_put(c->edid_override);
        c->edid = c->edid_override = NULL;
    }
    /* A framebuffer left over is one no file owns; its objects go with it. */
    while ((fb = mc->fbs) != NULL) {
        fb->base.refs = 1;
        drm_framebuffer_put(fb);
    }
    while (mc->blobs) {
        mc->blobs->base.refs = 1;
        drm_property_blob_put(mc->blobs);
    }
    while (mc->properties) {
        property_destroy(mc->properties);
    }
}

/** Build the IN_FORMATS blob of `plane`, Linux's create_in_format_blob:
 * the header, the formats, and each modifier with the mask of the first 64
 * formats it takes. 0 or -ENOMEM. */
static int32_t in_formats_blob(struct drm_device *dev, struct drm_plane *plane)
{
    static const uint64_t linear = DRM_FORMAT_MOD_LINEAR;
    const uint64_t *mods = plane->modifiers ? plane->modifiers : &linear;
    uint32_t nmods = plane->modifiers ? plane->modifier_count : 1;
    uint32_t formats_size = plane->format_count * 4;
    uint32_t mods_offset =
        (uint32_t)((sizeof(struct drm_format_modifier_blob) + formats_size + 7) & ~(uint32_t)7);
    uint32_t size = mods_offset + nmods * (uint32_t)sizeof(struct drm_format_modifier);
    struct drm_format_modifier_blob head;
    struct drm_format_modifier mod;
    struct drm_property_blob *blob;
    int32_t err = 0;
    uint32_t i, j;
    char *bytes;

    blob = drm_property_create_blob(dev, size, NULL, &err);
    if (!blob) {
        return err;
    }
    bytes = (char *)blob->data;
    head.version = FORMAT_BLOB_CURRENT;
    head.flags = 0;
    head.count_formats = plane->format_count;
    head.formats_offset = (uint32_t)sizeof(head);
    head.count_modifiers = nmods;
    head.modifiers_offset = mods_offset;
    k_memcpy(bytes, &head, sizeof(head));
    k_memcpy(bytes + sizeof(head), plane->formats, formats_size);
    for (i = 0; i < nmods; i++) {
        k_memset(&mod, 0, sizeof(mod));
        for (j = 0; j < plane->format_count && j < 64; j++) {
            mod.formats |= (uint64_t)1 << j;
        }
        mod.modifier = mods[i];
        k_memcpy(bytes + mods_offset + i * sizeof(mod), &mod, sizeof(mod));
    }
    drm_object_attach_property(&plane->base, dev->mode_config.modifiers_property, blob->base.id);
    return 0;
}

/** A new plane state of `plane` with Linux's reset values: off, unrotated,
 * opaque. */
static struct drm_plane_state *plane_state_new(struct drm_plane *plane)
{
    struct drm_plane_state *ps = (struct drm_plane_state *)kzalloc(sizeof(*ps));

    if (ps) {
        ps->plane = plane;
        ps->rotation = DRM_MODE_ROTATE_0;
        ps->alpha = 0xffff;
        ps->zpos = plane->type == DRM_PLANE_TYPE_PRIMARY ? 0 : plane->index;
    }
    return ps;
}

int32_t drm_universal_plane_init(struct drm_device *dev, struct drm_plane *plane,
                                 uint32_t possible_crtcs, const struct drm_plane_funcs *funcs,
                                 const uint32_t *formats_list, uint32_t format_count,
                                 const uint64_t *modifiers, int type)
{
    struct drm_mode_config *mc = &dev->mode_config;
    uint32_t n = 0;
    int32_t rc;

    if (mc->num_plane == DRM_MAX_PLANES) {
        return -EINVAL;
    }
    while (modifiers && modifiers[n] != DRM_FORMAT_MOD_INVALID) {
        n++;
    }
    plane->dev = dev;
    plane->index = mc->num_plane;
    plane->type = type;
    plane->possible_crtcs = possible_crtcs;
    plane->formats = formats_list;
    plane->format_count = format_count;
    plane->modifiers = modifiers;
    plane->modifier_count = modifiers ? n : 1;
    plane->funcs = funcs;
    plane->state = plane_state_new(plane);
    if (!plane->state) {
        return -ENOMEM;
    }
    object_add(dev, &plane->base, DRM_MODE_OBJECT_PLANE);
    mc->planes[mc->num_plane++] = plane;
    drm_object_attach_property(&plane->base, mc->plane_type_property, (uint64_t)type);
    if (drm_core_check_feature(dev, DRIVER_ATOMIC)) {
        drm_object_attach_property(&plane->base, mc->prop_fb_id, 0);
        drm_object_attach_property(&plane->base, mc->prop_in_fence_fd, (uint64_t)(int64_t)-1);
        drm_object_attach_property(&plane->base, mc->prop_crtc_id, 0);
        drm_object_attach_property(&plane->base, mc->prop_crtc_x, 0);
        drm_object_attach_property(&plane->base, mc->prop_crtc_y, 0);
        drm_object_attach_property(&plane->base, mc->prop_crtc_w, 0);
        drm_object_attach_property(&plane->base, mc->prop_crtc_h, 0);
        drm_object_attach_property(&plane->base, mc->prop_src_x, 0);
        drm_object_attach_property(&plane->base, mc->prop_src_y, 0);
        drm_object_attach_property(&plane->base, mc->prop_src_w, 0);
        drm_object_attach_property(&plane->base, mc->prop_src_h, 0);
    }
    if (drm_core_check_feature(dev, DRIVER_CURSOR_HOTSPOT) && type == DRM_PLANE_TYPE_CURSOR) {
        plane->hotspot_x_property =
            drm_property_create_signed_range(dev, 0, "HOTSPOT_X", DRM_INT_MIN, DRM_INT_MAX);
        plane->hotspot_y_property =
            drm_property_create_signed_range(dev, 0, "HOTSPOT_Y", DRM_INT_MIN, DRM_INT_MAX);
        if (!plane->hotspot_x_property || !plane->hotspot_y_property) {
            return -ENOMEM;
        }
        drm_object_attach_property(&plane->base, plane->hotspot_x_property, 0);
        drm_object_attach_property(&plane->base, plane->hotspot_y_property, 0);
    }
    rc = in_formats_blob(dev, plane);
    return rc;
}

int32_t drm_crtc_init_with_planes(struct drm_device *dev, struct drm_crtc *crtc,
                                  struct drm_plane *primary, struct drm_plane *cursor,
                                  const struct drm_crtc_funcs *funcs)
{
    struct drm_mode_config *mc = &dev->mode_config;
    uint32_t at = 0, n;
    char digits[10];
    int i = 0;

    if (mc->num_crtc == DRM_MAX_CRTCS) {
        return -EINVAL;
    }
    crtc->dev = dev;
    crtc->index = mc->num_crtc;
    crtc->funcs = funcs;
    crtc->primary = primary;
    crtc->cursor = cursor;
    crtc->state = (struct drm_crtc_state *)kzalloc(sizeof(*crtc->state));
    if (!crtc->state) {
        return -ENOMEM;
    }
    crtc->state->crtc = crtc;
    wait_queue_init(&crtc->vblank.wait);
    for (; "crtc-"[at]; at++) {
        crtc->name[at] = "crtc-"[at];
    }
    n = crtc->index;
    do {
        digits[i++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    while (i > 0) {
        crtc->name[at++] = digits[--i];
    }
    crtc->name[at] = '\0';
    if (primary && !primary->possible_crtcs) {
        primary->possible_crtcs = drm_crtc_mask(crtc);
    }
    if (cursor && !cursor->possible_crtcs) {
        cursor->possible_crtcs = drm_crtc_mask(crtc);
    }
    object_add(dev, &crtc->base, DRM_MODE_OBJECT_CRTC);
    mc->crtcs[mc->num_crtc++] = crtc;
    if (drm_core_check_feature(dev, DRIVER_ATOMIC)) {
        drm_object_attach_property(&crtc->base, mc->prop_active, 0);
        drm_object_attach_property(&crtc->base, mc->prop_mode_id, 0);
        drm_object_attach_property(&crtc->base, mc->prop_out_fence_ptr, 0);
        drm_object_attach_property(&crtc->base, mc->prop_vrr_enabled, 0);
    }
    return 0;
}

int32_t drm_mode_crtc_set_gamma_size(struct drm_crtc *crtc, uint32_t size)
{
    uint16_t *store;
    uint32_t i;

    if (size == 0 || size > 0x10000) {
        return -EINVAL;
    }
    store = (uint16_t *)kmalloc(size * 3 * sizeof(uint16_t));
    if (!store) {
        return -ENOMEM;
    }
    for (i = 0; i < size; i++) {
        uint16_t v = size > 1 ? (uint16_t)((i * 0xffffu) / (size - 1)) : 0;

        store[i] = store[size + i] = store[2 * size + i] = v;
    }
    kfree(crtc->gamma_store);
    crtc->gamma_store = store;
    crtc->gamma_size = size;
    return 0;
}

int32_t drm_encoder_init(struct drm_device *dev, struct drm_encoder *encoder, uint32_t type)
{
    struct drm_mode_config *mc = &dev->mode_config;

    if (mc->num_encoder == DRM_MAX_CONNECTORS) {
        return -EINVAL;
    }
    encoder->dev = dev;
    encoder->index = mc->num_encoder;
    encoder->encoder_type = type;
    object_add(dev, &encoder->base, DRM_MODE_OBJECT_ENCODER);
    mc->encoders[mc->num_encoder++] = encoder;
    return 0;
}

/* The connector types' names, Linux's drm_connector_enum_list. */
static const char *const connector_names[] = {
    "Unknown", "VGA",       "DVI-I", "DVI-D", "DVI-A",     "Composite", "SVIDEO",
    "LVDS",    "Component", "DIN",   "DP",    "HDMI-A",    "HDMI-B",    "TV",
    "eDP",     "Virtual",   "DSI",   "DPI",   "Writeback", "SPI",       "USB"};

int32_t drm_connector_init(struct drm_device *dev, struct drm_connector *connector,
                           const struct drm_connector_funcs *funcs, uint32_t type)
{
    struct drm_mode_config *mc = &dev->mode_config;
    const char *tname = type < sizeof(connector_names) / sizeof(connector_names[0])
                            ? connector_names[type]
                            : "Unknown";
    uint32_t id = 1, i, at = 0, n;
    char digits[10];
    int d = 0;

    if (mc->num_connector == DRM_MAX_CONNECTORS) {
        return -EINVAL;
    }
    /* The lowest number of the type that is free, Linux's ida. */
    for (i = 0; i < mc->num_connector; i++) {
        if (mc->connectors[i]->connector_type == type &&
            mc->connectors[i]->connector_type_id == id) {
            id++;
            i = (uint32_t)-1;
        }
    }
    connector->state = (struct drm_connector_state *)kzalloc(sizeof(*connector->state));
    if (!connector->state) {
        return -ENOMEM;
    }
    connector->state->connector = connector;
    connector->state->link_status = DRM_MODE_LINK_STATUS_GOOD;
    connector->dev = dev;
    connector->index = mc->num_connector;
    connector->connector_type = type;
    connector->connector_type_id = id;
    connector->funcs = funcs;
    connector->status = DRM_CONNECTOR_UNKNOWN;
    connector->dpms = DRM_MODE_DPMS_OFF;
    while (*tname && at + 12 < DRM_CONNECTOR_NAME_LEN) {
        connector->name[at++] = *tname++;
    }
    connector->name[at++] = '-';
    n = id;
    do {
        digits[d++] = (char)('0' + n % 10);
        n /= 10;
    } while (n);
    while (d > 0) {
        connector->name[at++] = digits[--d];
    }
    connector->name[at] = '\0';
    object_add(dev, &connector->base, DRM_MODE_OBJECT_CONNECTOR);
    mc->connectors[mc->num_connector++] = connector;
    if (type != DRM_MODE_CONNECTOR_VIRTUAL && type != DRM_MODE_CONNECTOR_WRITEBACK) {
        drm_connector_attach_edid_property(connector);
    }
    drm_object_attach_property(&connector->base, mc->dpms_property, 0);
    drm_object_attach_property(&connector->base, mc->link_status_property, 0);
    drm_object_attach_property(&connector->base, mc->non_desktop_property, 0);
    drm_object_attach_property(&connector->base, mc->tile_property, 0);
    if (drm_core_check_feature(dev, DRIVER_ATOMIC)) {
        drm_object_attach_property(&connector->base, mc->prop_crtc_id, 0);
    }
    return 0;
}

int32_t drm_connector_attach_encoder(struct drm_connector *connector, struct drm_encoder *encoder)
{
    connector->possible_encoders |= 1u << encoder->index;
    return 0;
}

void drm_connector_attach_edid_property(struct drm_connector *connector)
{
    drm_object_attach_property(&connector->base, connector->dev->mode_config.edid_property,
                               connector->edid ? connector->edid->base.id : 0);
}

void drm_plane_enable_fb_damage_clips(struct drm_plane *plane)
{
    drm_object_attach_property(&plane->base, plane->dev->mode_config.prop_fb_damage_clips, 0);
}

int32_t drm_plane_create_rotation_property(struct drm_plane *plane, uint32_t rotation,
                                           uint32_t supported)
{
    struct drm_property *prop =
        drm_property_create_bitmask(plane->dev, 0, "rotation", rotation_names, 6, supported);

    if (!prop) {
        return -ENOMEM;
    }
    drm_object_attach_property(&plane->base, prop, rotation);
    plane->state->rotation = rotation;
    plane->rotation_property = prop;
    return 0;
}

int32_t drm_plane_create_zpos_property(struct drm_plane *plane, uint32_t zpos, uint32_t min,
                                       uint32_t max)
{
    struct drm_property *prop = drm_property_create_range(
        plane->dev, min == max ? DRM_MODE_PROP_IMMUTABLE : 0, "zpos", min, max);

    if (!prop) {
        return -ENOMEM;
    }
    drm_object_attach_property(&plane->base, prop, zpos);
    plane->state->zpos = zpos;
    plane->zpos_property = prop;
    return 0;
}

int32_t drm_plane_create_alpha_property(struct drm_plane *plane)
{
    struct drm_property *prop = drm_property_create_range(plane->dev, 0, "alpha", 0, 0xffff);

    if (!prop) {
        return -ENOMEM;
    }
    drm_object_attach_property(&plane->base, prop, 0xffff);
    plane->state->alpha = 0xffff;
    plane->alpha_property = prop;
    return 0;
}

/* ------------------------------------------------------------------------
 * Framebuffers
 */

void drm_framebuffer_get(struct drm_framebuffer *fb)
{
    if (fb) {
        fb->base.refs++;
    }
}

void drm_framebuffer_put(struct drm_framebuffer *fb)
{
    struct drm_framebuffer **link;
    int i;

    if (!fb || --fb->base.refs > 0) {
        return;
    }
    for (link = &fb->dev->mode_config.fbs; *link; link = &(*link)->next) {
        if (*link == fb) {
            *link = fb->next;
            break;
        }
    }
    for (i = 0; i < 4; i++) {
        drm_gem_object_put(fb->obj[i]);
    }
    kfree(fb);
}

int drm_plane_has_format(const struct drm_plane *plane, uint32_t format, uint64_t modifier)
{
    uint32_t i;

    for (i = 0; i < plane->format_count && plane->formats[i] != format; i++) {
    }
    if (i == plane->format_count) {
        return 0;
    }
    if (!plane->modifiers) {
        return modifier == DRM_FORMAT_MOD_LINEAR;
    }
    for (i = 0; i < plane->modifier_count; i++) {
        if (plane->modifiers[i] == modifier) {
            return 1;
        }
    }
    return 0;
}

/** Whether some plane of `dev` takes `format` with `modifier`. */
static int any_plane_has_format(struct drm_device *dev, uint32_t format, uint64_t modifier)
{
    uint32_t i;

    for (i = 0; i < dev->mode_config.num_plane; i++) {
        if (drm_plane_has_format(dev->mode_config.planes[i], format, modifier)) {
            return 1;
        }
    }
    return 0;
}

int32_t drm_framebuffer_check_src_coords(uint32_t src_x, uint32_t src_y, uint32_t src_w,
                                         uint32_t src_h, const struct drm_framebuffer *fb)
{
    uint32_t fb_width = fb->width << 16, fb_height = fb->height << 16;

    if (src_w > fb_width || src_x > fb_width - src_w || src_h > fb_height ||
        src_y > fb_height - src_h) {
        return -ENOSPC;
    }
    return 0;
}

/** Linux's framebuffer_check: the format, the sizes, each plane's handle,
 * pitch, offset and modifier, and the unused planes left clear. 0,
 * -EINVAL or -ERANGE. */
static int32_t framebuffer_check(const struct drm_mode_fb_cmd2 *r,
                                 const struct drm_format_info **out)
{
    const struct drm_format_info *info = drm_format_info(r->pixel_format);
    int i;

    if (!info || r->width == 0 || r->height == 0) {
        return -EINVAL;
    }
    for (i = 0; i < info->num_planes; i++) {
        uint32_t width = plane_width(info, r->width, i);
        uint32_t height = plane_height(info, r->height, i);
        uint64_t min_pitch = (uint64_t)width * info->cpp[i];

        if (!r->handles[i]) {
            return -EINVAL;
        }
        if (min_pitch > DRM_UINT_MAX) {
            return -ERANGE;
        }
        if ((uint64_t)height * r->pitches[i] + r->offsets[i] > DRM_UINT_MAX) {
            return -ERANGE;
        }
        if (r->pitches[i] < min_pitch) {
            return -EINVAL;
        }
        if (r->modifier[i] && !(r->flags & DRM_MODE_FB_MODIFIERS)) {
            return -EINVAL;
        }
        if ((r->flags & DRM_MODE_FB_MODIFIERS) && r->modifier[i] != r->modifier[0]) {
            return -EINVAL;
        }
    }
    for (i = info->num_planes; i < 4; i++) {
        if (r->modifier[i]) {
            return -EINVAL;
        }
        /* Programs before FB_MODIFIERS left the rest uncleared. */
        if (!(r->flags & DRM_MODE_FB_MODIFIERS)) {
            continue;
        }
        if (r->handles[i] || r->pitches[i] || r->offsets[i]) {
            return -EINVAL;
        }
    }
    *out = info;
    return 0;
}

struct drm_framebuffer *drm_internal_framebuffer_create(struct drm_device *dev,
                                                        const struct drm_mode_fb_cmd2 *r,
                                                        struct drm_file *file, int32_t *err)
{
    const struct drm_mode_config *mc = &dev->mode_config;
    const struct drm_format_info *info = NULL;
    struct drm_framebuffer *fb, **link;
    uint64_t modifier;
    int i;

    if (r->flags & ~(uint32_t)(DRM_MODE_FB_INTERLACED | DRM_MODE_FB_MODIFIERS)) {
        *err = -EINVAL;
        return NULL;
    }
    if (mc->min_width > r->width || r->width > mc->max_width || mc->min_height > r->height ||
        r->height > mc->max_height) {
        *err = -EINVAL;
        return NULL;
    }
    *err = framebuffer_check(r, &info);
    if (*err != 0) {
        return NULL;
    }
    modifier = (r->flags & DRM_MODE_FB_MODIFIERS) ? r->modifier[0] : DRM_FORMAT_MOD_LINEAR;
    if (drm_core_check_feature(dev, DRIVER_ATOMIC) &&
        !any_plane_has_format(dev, r->pixel_format, modifier)) {
        *err = -EINVAL;
        return NULL;
    }
    fb = (struct drm_framebuffer *)kzalloc(sizeof(*fb));
    if (!fb) {
        *err = -ENOMEM;
        return NULL;
    }
    for (i = 0; i < info->num_planes; i++) {
        uint32_t width = plane_width(info, r->width, i);
        uint32_t height = plane_height(info, r->height, i);
        uint64_t min_size =
            (uint64_t)(height - 1) * r->pitches[i] + (uint64_t)width * info->cpp[i] + r->offsets[i];

        fb->obj[i] = drm_gem_object_lookup(file, r->handles[i]);
        if (!fb->obj[i]) {
            *err = -ENOENT;
        } else if (fb->obj[i]->size < min_size) {
            *err = -EINVAL;
        }
        if (*err != 0) {
            while (i >= 0) {
                drm_gem_object_put(fb->obj[i--]);
            }
            kfree(fb);
            return NULL;
        }
        fb->pitches[i] = r->pitches[i];
        fb->offsets[i] = r->offsets[i];
    }
    fb->dev = dev;
    fb->info = info;
    fb->format = r->pixel_format;
    fb->modifier = modifier;
    fb->flags = r->flags;
    fb->width = r->width;
    fb->height = r->height;
    fb->base.refs = 1;
    object_add(dev, &fb->base, DRM_MODE_OBJECT_FB);
    for (link = &dev->mode_config.fbs; *link; link = &(*link)->next) {
    }
    *link = fb;
    return fb;
}

/** ADDFB2, Linux's drm_mode_addfb2: a framebuffer the caller owns. */
int32_t drm_mode_addfb2_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_fb_cmd2 *r = (struct drm_mode_fb_cmd2 *)data;
    struct drm_framebuffer *fb;
    int32_t err = 0;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    fb = drm_internal_framebuffer_create(dev, r, file, &err);
    if (!fb) {
        return err;
    }
    fb->owner = file;
    fb->file_next = file->fbs;
    file->fbs = fb;
    r->fb_id = fb->base.id;
    return 0;
}

/** ADDFB, Linux's drm_mode_addfb: ADDFB2 of the format of `bpp` and
 * `depth`. */
int32_t drm_mode_addfb_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_fb_cmd *o = (struct drm_mode_fb_cmd *)data;
    struct drm_mode_fb_cmd2 r;
    int32_t rc;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    k_memset(&r, 0, sizeof(r));
    r.pixel_format = drm_mode_legacy_fb_format(o->bpp, o->depth);
    if (r.pixel_format == DRM_FORMAT_INVALID) {
        return -EINVAL;
    }
    r.width = o->width;
    r.height = o->height;
    r.pitches[0] = o->pitch;
    r.handles[0] = o->handle;
    rc = drm_mode_addfb2_ioctl(dev, &r, file);
    if (rc == 0) {
        o->fb_id = r.fb_id;
    }
    return rc;
}

/** Turn `crtc` off in `state`: inactive, no mode and no connectors, as
 * Linux's atomic_remove_fb does for a primary plane's CRTC. 0 or the
 * error. */
static int32_t disable_crtc_in_state(struct drm_atomic_state *state, struct drm_crtc *crtc)
{
    struct drm_crtc_state *cs = drm_atomic_get_crtc_state(state, crtc);
    int32_t rc;
    uint32_t j;

    rc = cs ? drm_atomic_add_affected_connectors(state, crtc) : -ENOMEM;
    if (rc == 0) {
        cs->active = 0;
        rc = drm_atomic_set_mode_for_crtc(cs, NULL);
    }
    for (j = 0; rc == 0 && j < crtc->dev->mode_config.num_connector; j++) {
        struct drm_connector_state *conn = state->connectors_new[j];

        if (conn && conn->crtc == crtc) {
            rc = drm_atomic_set_crtc_for_connector(state, conn, NULL);
        }
    }
    return rc;
}

/** One try of `remove_fb_from_planes`: a commit that takes `fb` off its
 * planes, and with `disable_crtcs` turns off the CRTCs whose primary plane
 * shows it. 0 or the error. */
static int32_t remove_fb_once(struct drm_framebuffer *fb, int disable_crtcs)
{
    struct drm_device *dev = fb->dev;
    struct drm_atomic_state *state = drm_atomic_state_alloc(dev);
    int32_t rc = 0;
    int any = 0;
    uint32_t i;

    if (!state) {
        return -ENOMEM;
    }
    for (i = 0; i < dev->mode_config.num_plane && rc == 0; i++) {
        struct drm_plane *plane = dev->mode_config.planes[i];
        struct drm_plane_state *ps;

        if (plane->state->fb != fb) {
            continue;
        }
        ps = drm_atomic_get_plane_state(state, plane);
        if (!ps) {
            rc = -ENOMEM;
            break;
        }
        if (disable_crtcs && ps->crtc && ps->crtc->primary == plane) {
            rc = disable_crtc_in_state(state, ps->crtc);
        }
        if (rc == 0) {
            rc = drm_atomic_set_crtc_for_plane(state, ps, NULL);
            drm_atomic_set_fb_for_plane(ps, NULL);
            any = 1;
        }
    }
    if (rc == 0 && any) {
        rc = drm_atomic_commit(state);
    }
    drm_atomic_state_free(state);
    return rc;
}

/**
 * Take `fb` off every plane that shows it, Linux's atomic_remove_fb: first
 * the planes alone, and when the check refuses that, each CRTC whose
 * primary plane it is as well. The caller holds the commit lock. 0 or the
 * commit's error.
 */
static int32_t remove_fb_from_planes(struct drm_framebuffer *fb)
{
    int32_t rc = remove_fb_once(fb, 0);

    return rc == -EINVAL ? remove_fb_once(fb, 1) : rc;
}

/** Take `fb` off its owner's list; 0, or -ENOENT when `file` does not own
 * it. */
static int32_t fb_disown(struct drm_file *file, struct drm_framebuffer *fb)
{
    struct drm_framebuffer **link;

    for (link = &file->fbs; *link; link = &(*link)->file_next) {
        if (*link == fb) {
            *link = fb->file_next;
            fb->file_next = NULL;
            fb->owner = NULL;
            return 0;
        }
    }
    return -ENOENT;
}

/** Drop the owner's reference to `fb`, taken off its owner's list; a
 * framebuffer some plane still shows is first taken off the display, as
 * Linux's drm_framebuffer_remove. */
static void fb_remove(struct drm_framebuffer *fb)
{
    if (fb->base.refs > 1) {
        drm_modeset_lock_nointr(fb->dev);
        remove_fb_from_planes(fb);
        drm_modeset_unlock(fb->dev);
    }
    drm_framebuffer_put(fb);
}

/** RMFB, Linux's drm_mode_rmfb: remove a framebuffer the caller owns from
 * the display, and its ID once nothing holds it. */
int32_t drm_mode_rmfb_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    uint32_t *fb_id = (uint32_t *)data;
    struct drm_framebuffer *fb;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    fb = drm_framebuffer_lookup(dev, file, *fb_id);
    if (!fb) {
        return -ENOENT;
    }
    if (fb_disown(file, fb) != 0) {
        drm_framebuffer_put(fb);
        return -ENOENT;
    }
    drm_framebuffer_put(fb);
    fb_remove(fb);
    return 0;
}

/** CLOSEFB, Linux 6.12's: give up a framebuffer the caller owns and leave
 * it on the display while a plane shows it. */
int32_t drm_mode_closefb_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_closefb *r = (struct drm_mode_closefb *)data;
    struct drm_framebuffer *fb;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    if (r->pad) {
        return -EINVAL;
    }
    fb = drm_framebuffer_lookup(dev, file, r->fb_id);
    if (!fb) {
        return -ENOENT;
    }
    if (fb_disown(file, fb) != 0) {
        drm_framebuffer_put(fb);
        return -ENOENT;
    }
    drm_framebuffer_put(fb);
    drm_framebuffer_put(fb);
    return 0;
}

/** Whether `file` may have handles to a framebuffer's objects from GETFB
 * and GETFB2: the master or root, as Linux. */
static int may_get_handles(struct drm_file *file)
{
    return drm_is_current_master(file) || (current_proc && proc_privileged(current_proc));
}

/** GETFB, Linux's drm_mode_getfb: a one-plane framebuffer's size, depth,
 * bits per pixel and pitch, and for the master or root a handle to its
 * object. */
int32_t drm_mode_getfb(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_fb_cmd *r = (struct drm_mode_fb_cmd *)data;
    struct drm_framebuffer *fb;
    int32_t rc = 0;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    fb = drm_framebuffer_lookup(dev, file, r->fb_id);
    if (!fb) {
        return -ENOENT;
    }
    if (fb->info->num_planes > 1) {
        drm_framebuffer_put(fb);
        return -EINVAL;
    }
    r->height = fb->height;
    r->width = fb->width;
    r->depth = fb->info->depth;
    r->bpp = fb->info->cpp[0] * 8u;
    r->pitch = fb->pitches[0];
    r->handle = 0;
    if (may_get_handles(file)) {
        rc = drm_gem_handle_create(file, fb->obj[0], &r->handle);
    }
    drm_framebuffer_put(fb);
    return rc;
}

/** Give `file` a handle to each of `fb`'s objects in `handles`, one handle
 * for an object that repeats, as GETFB2 does; on a failure delete those
 * made and zero them. 0 or the error. */
static int32_t fb_plane_handles(struct drm_file *file, struct drm_framebuffer *fb,
                                uint32_t *handles)
{
    int32_t rc = 0;
    uint32_t h;
    int i, j;

    for (i = 0; i < fb->info->num_planes && rc == 0; i++) {
        for (j = 0; j < i && fb->obj[i] != fb->obj[j]; j++) {
        }
        if (j < i) {
            handles[i] = handles[j];
        } else {
            rc = drm_gem_handle_create(file, fb->obj[i], &handles[i]);
        }
    }
    for (i = 0; rc != 0 && i < 4; i++) {
        h = handles[i];
        if (!h) {
            continue;
        }
        drm_gem_handle_delete(file, h);
        for (j = i; j < 4; j++) {
            if (handles[j] == h) {
                handles[j] = 0;
            }
        }
    }
    return rc;
}

/** GETFB2, Linux's drm_mode_getfb2_ioctl: every plane's pitch, offset and
 * modifier, and for the master or root a handle to each object, one handle
 * for an object that repeats. */
int32_t drm_mode_getfb2_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_fb_cmd2 *r = (struct drm_mode_fb_cmd2 *)data;
    struct drm_framebuffer *fb;
    int32_t rc = 0;
    int i;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    fb = drm_framebuffer_lookup(dev, file, r->fb_id);
    if (!fb) {
        return -ENOENT;
    }
    r->height = fb->height;
    r->width = fb->width;
    r->pixel_format = fb->format;
    r->flags = DRM_MODE_FB_MODIFIERS;
    for (i = 0; i < 4; i++) {
        r->handles[i] = 0;
        r->pitches[i] = 0;
        r->offsets[i] = 0;
        r->modifier[i] = 0;
    }
    for (i = 0; i < fb->info->num_planes; i++) {
        r->pitches[i] = fb->pitches[i];
        r->offsets[i] = fb->offsets[i];
        r->modifier[i] = fb->modifier;
    }
    if (may_get_handles(file)) {
        rc = fb_plane_handles(file, fb, r->handles);
    }
    drm_framebuffer_put(fb);
    return rc;
}

void drm_mode_file_release(struct drm_file *file)
{
    struct drm_device *dev = file->dev;
    struct drm_property_blob *blob, *next;
    struct drm_framebuffer *fb;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return;
    }
    while ((fb = file->fbs) != NULL) {
        fb_disown(file, fb);
        fb_remove(fb);
    }
    for (blob = dev->mode_config.blobs; blob; blob = next) {
        next = blob->next;
        if (blob->owner == file) {
            blob->owner = NULL;
            /* A later blob is still on the list unless this put frees the
             * one before it, which it does not. */
            drm_property_blob_put(blob);
        }
    }
}

/* ------------------------------------------------------------------------
 * Modes
 */

uint32_t drm_mode_vrefresh(const struct drm_mode_modeinfo *mode)
{
    uint64_t num, den;

    if (mode->htotal == 0 || mode->vtotal == 0) {
        return 0;
    }
    num = mode->clock;
    den = (uint64_t)mode->htotal * mode->vtotal;
    if (mode->flags & DRM_MODE_FLAG_INTERLACE) {
        num *= 2;
    }
    if (mode->flags & DRM_MODE_FLAG_DBLSCAN) {
        den *= 2;
    }
    if (mode->vscan > 1) {
        den *= mode->vscan;
    }
    return (uint32_t)drm_div64(num * 1000 + den / 2, den);
}

void drm_mode_set_name(struct drm_mode_modeinfo *mode)
{
    uint32_t vals[2], at = 0, v;
    char digits[6];
    int i, d;

    vals[0] = mode->hdisplay;
    vals[1] = mode->vdisplay;
    for (i = 0; i < 2; i++) {
        if (i == 1) {
            mode->name[at++] = 'x';
        }
        v = vals[i];
        d = 0;
        do {
            digits[d++] = (char)('0' + v % 10);
            v /= 10;
        } while (v);
        while (d > 0) {
            mode->name[at++] = digits[--d];
        }
    }
    if (mode->flags & DRM_MODE_FLAG_INTERLACE) {
        mode->name[at++] = 'i';
    }
    while (at < DRM_DISPLAY_MODE_LEN) {
        mode->name[at++] = '\0';
    }
}

int drm_mode_equal(const struct drm_mode_modeinfo *a, const struct drm_mode_modeinfo *b)
{
    return a->clock == b->clock && a->hdisplay == b->hdisplay && a->hsync_start == b->hsync_start &&
           a->hsync_end == b->hsync_end && a->htotal == b->htotal && a->hskew == b->hskew &&
           a->vdisplay == b->vdisplay && a->vsync_start == b->vsync_start &&
           a->vsync_end == b->vsync_end && a->vtotal == b->vtotal && a->vscan == b->vscan &&
           a->flags == b->flags;
}

int32_t drm_mode_validate_basic(const struct drm_mode_modeinfo *mode)
{
    uint32_t flags = mode->flags & ~(uint32_t)DRM_MODE_FLAG_PIC_AR_MASK;

    if (mode->type & ~(uint32_t)DRM_MODE_TYPE_ALL) {
        return -EINVAL;
    }
    if (flags & ~(uint32_t)DRM_MODE_FLAG_ALL) {
        return -EINVAL;
    }
    if ((flags & DRM_MODE_FLAG_3D_MASK) > DRM_MODE_FLAG_3D_MAX) {
        return -EINVAL;
    }
    if (mode->clock == 0) {
        return -EINVAL;
    }
    if (mode->hdisplay == 0 || mode->hsync_start < mode->hdisplay ||
        mode->hsync_end < mode->hsync_start || mode->htotal < mode->hsync_end) {
        return -EINVAL;
    }
    if (mode->vdisplay == 0 || mode->vsync_start < mode->vdisplay ||
        mode->vsync_end < mode->vsync_start || mode->vtotal < mode->vsync_end) {
        return -EINVAL;
    }
    return 0;
}

int32_t drm_mode_convert_umode(struct drm_mode_modeinfo *out, const struct drm_mode_modeinfo *in)
{
    if (in->clock > DRM_INT_MAX || in->vrefresh > DRM_INT_MAX) {
        return -ERANGE;
    }
    *out = *in;
    out->type &= DRM_MODE_TYPE_ALL;
    if ((in->flags & DRM_MODE_FLAG_PIC_AR_MASK) > DRM_MODE_FLAG_PIC_AR_256_135) {
        return -EINVAL;
    }
    return drm_mode_validate_basic(out);
}

uint64_t drm_div64(uint64_t n, uint64_t d)
{
    uint64_t q = 0, r = 0;
    int i;

    if ((n >> 32) == 0 && (d >> 32) == 0) {
        return (uint32_t)n / (uint32_t)d;
    }
    for (i = 63; i >= 0; i--) {
        r = (r << 1) | ((n >> i) & 1u);
        if (r >= d) {
            r -= d;
            q |= (uint64_t)1 << i;
        }
    }
    return q;
}

int32_t drm_mode_probed_add(struct drm_connector *connector, const struct drm_mode_modeinfo *mode)
{
    struct drm_display_mode *m = (struct drm_display_mode *)kmalloc(sizeof(*m)), **link;

    if (!m) {
        return -ENOMEM;
    }
    m->info = *mode;
    m->info.vrefresh = drm_mode_vrefresh(mode);
    m->next = NULL;
    for (link = &connector->probed_modes; *link; link = &(*link)->next) {
    }
    *link = m;
    return 0;
}

/** Whether `mode` is one `connector` can show: sane, inside the limits, of
 * flags the connector allows, and valid to the connector and to some CRTC
 * an encoder of it reaches, as Linux's probe validates it. */
static int mode_ok(struct drm_connector *connector, const struct drm_mode_modeinfo *mode,
                   uint32_t max_width, uint32_t max_height)
{
    struct drm_mode_config *mc = &connector->dev->mode_config;
    uint32_t e, c;
    int any = 0;

    if (drm_mode_validate_basic(mode) != 0) {
        return 0;
    }
    if ((max_width && mode->hdisplay > max_width) || (max_height && mode->vdisplay > max_height)) {
        return 0;
    }
    if (((mode->flags & DRM_MODE_FLAG_INTERLACE) && !connector->interlace_allowed) ||
        ((mode->flags & DRM_MODE_FLAG_DBLSCAN) && !connector->doublescan_allowed) ||
        (mode->flags & DRM_MODE_FLAG_3D_MASK)) {
        return 0;
    }
    if (connector->funcs && connector->funcs->mode_valid &&
        connector->funcs->mode_valid(connector, mode) != 0) {
        return 0;
    }
    for (e = 0; e < mc->num_encoder; e++) {
        if (!(connector->possible_encoders & (1u << e))) {
            continue;
        }
        for (c = 0; c < mc->num_crtc; c++) {
            struct drm_crtc *crtc = mc->crtcs[c];

            if (!(mc->encoders[e]->possible_crtcs & drm_crtc_mask(crtc))) {
                continue;
            }
            any = 1;
            if (!crtc->funcs || !crtc->funcs->mode_valid ||
                crtc->funcs->mode_valid(crtc, mode) == 0) {
                return 1;
            }
        }
    }
    return !any;
}

/** Linux's drm_mode_compare: below zero when `a` goes before `b`. */
static int mode_compare(const struct drm_mode_modeinfo *a, const struct drm_mode_modeinfo *b)
{
    int32_t diff =
        ((b->type & DRM_MODE_TYPE_PREFERRED) != 0) - ((a->type & DRM_MODE_TYPE_PREFERRED) != 0);

    if (diff) {
        return diff;
    }
    diff = (int32_t)(b->hdisplay * b->vdisplay) - (int32_t)(a->hdisplay * a->vdisplay);
    if (diff) {
        return diff;
    }
    diff = (int32_t)drm_mode_vrefresh(b) - (int32_t)drm_mode_vrefresh(a);
    if (diff) {
        return diff;
    }
    return (int32_t)b->clock - (int32_t)a->clock;
}

/** Sort the list `*list` by `mode_compare`, stably, as Linux's list_sort. */
static void modes_sort(struct drm_display_mode **list)
{
    struct drm_display_mode *sorted = NULL, *m, **link;

    while ((m = *list) != NULL) {
        *list = m->next;
        for (link = &sorted; *link && mode_compare(&(*link)->info, &m->info) <= 0;
             link = &(*link)->next) {
        }
        m->next = *link;
        *link = m;
    }
    *list = sorted;
}

int drm_helper_probe_single_connector_modes(struct drm_connector *connector, uint32_t max_width,
                                            uint32_t max_height)
{
    struct drm_display_mode *kept = NULL, **tail = &kept, *m, *k;
    int count = 0;

    if (connector->force) {
        connector->status =
            connector->force == DRM_FORCE_ON || connector->force == DRM_FORCE_ON_DIGITAL
                ? DRM_CONNECTOR_CONNECTED
                : DRM_CONNECTOR_DISCONNECTED;
        if (connector->funcs && connector->funcs->force) {
            connector->funcs->force(connector);
        }
    } else {
        connector->status = connector->funcs && connector->funcs->detect
                                ? connector->funcs->detect(connector, 1)
                                : DRM_CONNECTOR_CONNECTED;
    }
    modes_free(&connector->probed_modes);
    if (connector->status == DRM_CONNECTOR_DISCONNECTED) {
        drm_property_blob_put(connector->edid);
        connector->edid = NULL;
        drm_object_property_set_value(&connector->base, connector->dev->mode_config.edid_property,
                                      0);
        connector->mm_width = connector->mm_height = 0;
        modes_free(&connector->modes);
        return 0;
    }
    if (connector->funcs && connector->funcs->get_modes) {
        count = connector->funcs->get_modes(connector);
    }
    /* The override stands in for a sink whose EDID could not be read. */
    if (count == 0 && connector->status == DRM_CONNECTOR_CONNECTED && connector->edid_override) {
        count = drm_connector_update_edid(connector, NULL, 0);
    }
    if (count == 0) {
        drm_add_modes_noedid(connector, 1024, 768);
        /* DisplayPort's compliance tests want 640x480 without an EDID. */
        if (connector->connector_type == DRM_MODE_CONNECTOR_DisplayPort) {
            for (m = connector->probed_modes; m; m = m->next) {
                if (m->info.hdisplay == 640 && m->info.vdisplay == 480) {
                    m->info.type |= DRM_MODE_TYPE_PREFERRED;
                }
            }
        }
    }
    /* Linux's drm_connector_list_update: one mode of equal ones, with the
     * type bits of all, and the timings of the preferred one. */
    while ((m = connector->probed_modes) != NULL) {
        connector->probed_modes = m->next;
        m->next = NULL;
        for (k = kept; k && !drm_mode_equal(&k->info, &m->info); k = k->next) {
        }
        if (k) {
            if (!(k->info.type & DRM_MODE_TYPE_PREFERRED) &&
                (m->info.type & DRM_MODE_TYPE_PREFERRED)) {
                m->info.type |= k->info.type;
                k->info = m->info;
            } else {
                k->info.type |= m->info.type;
            }
            kfree(m);
        } else if (!mode_ok(connector, &m->info, max_width, max_height)) {
            kfree(m);
        } else {
            *tail = m;
            tail = &m->next;
        }
    }
    modes_sort(&kept);
    modes_free(&connector->modes);
    connector->modes = kept;
    count = 0;
    for (m = kept; m; m = m->next) {
        count++;
    }
    return count;
}

/* ------------------------------------------------------------------------
 * The ioctls that report the display
 */

/** Write `id` as element `i` of the user array `ptr` when `i` is below
 * `room`. 0 or -EFAULT. */
static int32_t put_id(uint64_t ptr, uint32_t i, uint32_t room, uint32_t id)
{
    if (i < room && copy_to_user((void __user *)(uintptr_t)(ptr + 4u * i), &id, 4) != 0) {
        return -EFAULT;
    }
    return 0;
}

/** GETRESOURCES, Linux's drm_mode_getresources: the caller's framebuffers,
 * newest first, and the CRTCs, encoders and connectors, with the size
 * limits. */
int32_t drm_mode_getresources(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_card_res *res = (struct drm_mode_card_res *)data;
    struct drm_mode_config *mc = &dev->mode_config;
    struct drm_framebuffer *fb;
    uint32_t i, count = 0;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    for (fb = file->fbs; fb; fb = fb->file_next) {
        if (put_id(res->fb_id_ptr, count, res->count_fbs, fb->base.id) != 0) {
            return -EFAULT;
        }
        count++;
    }
    res->count_fbs = count;
    res->max_height = mc->max_height;
    res->min_height = mc->min_height;
    res->max_width = mc->max_width;
    res->min_width = mc->min_width;
    for (i = 0; i < mc->num_crtc; i++) {
        if (put_id(res->crtc_id_ptr, i, res->count_crtcs, mc->crtcs[i]->base.id) != 0) {
            return -EFAULT;
        }
    }
    res->count_crtcs = mc->num_crtc;
    for (i = 0; i < mc->num_encoder; i++) {
        if (put_id(res->encoder_id_ptr, i, res->count_encoders, mc->encoders[i]->base.id) != 0) {
            return -EFAULT;
        }
    }
    res->count_encoders = mc->num_encoder;
    count = 0;
    for (i = 0; i < mc->num_connector; i++) {
        struct drm_connector *c = mc->connectors[i];

        /* Only a program that knows writeback connectors sees them. */
        if (!file->writeback_connectors && c->connector_type == DRM_MODE_CONNECTOR_WRITEBACK) {
            continue;
        }
        if (put_id(res->connector_id_ptr, count, res->count_connectors, c->base.id) != 0) {
            return -EFAULT;
        }
        count++;
    }
    res->count_connectors = count;
    return 0;
}

/** Whether `file` sees `plane` in GETPLANERESOURCES: overlays always, the
 * rest with the universal-planes cap, and a virtualized driver's cursor
 * only for an atomic client that takes its hotspot, as Linux. */
static int plane_listed(struct drm_device *dev, struct drm_file *file,
                        const struct drm_plane *plane)
{
    if (plane->type != DRM_PLANE_TYPE_OVERLAY && !file->universal_planes) {
        return 0;
    }
    if (plane->type == DRM_PLANE_TYPE_CURSOR &&
        drm_core_check_feature(dev, DRIVER_CURSOR_HOTSPOT) && file->atomic &&
        !file->supports_virtualized_cursor_plane) {
        return 0;
    }
    return 1;
}

/** GETPLANERESOURCES: the planes the caller sees. */
int32_t drm_mode_getplane_res(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_get_plane_res *res = (struct drm_mode_get_plane_res *)data;
    uint32_t i, count = 0;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    for (i = 0; i < dev->mode_config.num_plane; i++) {
        struct drm_plane *plane = dev->mode_config.planes[i];

        if (!plane_listed(dev, file, plane)) {
            continue;
        }
        if (put_id(res->plane_id_ptr, count, res->count_planes, plane->base.id) != 0) {
            return -EFAULT;
        }
        count++;
    }
    res->count_planes = count;
    return 0;
}

/** GETPLANE: a plane's CRTC, framebuffer, CRTCs and formats. */
int32_t drm_mode_getplane(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_get_plane *req = (struct drm_mode_get_plane *)data;
    struct drm_plane *plane;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    plane = drm_plane_find(dev, file, req->plane_id);
    if (!plane) {
        return -ENOENT;
    }
    req->crtc_id = plane->state->crtc ? plane->state->crtc->base.id : 0;
    req->fb_id = plane->state->fb ? plane->state->fb->base.id : 0;
    req->possible_crtcs = plane->possible_crtcs;
    req->gamma_size = 0;
    if (plane->format_count && req->count_format_types >= plane->format_count &&
        copy_to_user((void __user *)(uintptr_t)req->format_type_ptr, plane->formats,
                     plane->format_count * 4u) != 0) {
        return -EFAULT;
    }
    req->count_format_types = plane->format_count;
    return 0;
}

/** GETCRTC: a CRTC's framebuffer and its place, its mode and its gamma
 * size. */
int32_t drm_mode_getcrtc(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_crtc *req = (struct drm_mode_crtc *)data;
    struct drm_crtc *crtc;
    struct drm_plane_state *ps;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    crtc = drm_crtc_find(dev, file, req->crtc_id);
    if (!crtc) {
        return -ENOENT;
    }
    req->gamma_size = crtc->gamma_size;
    ps = crtc->primary ? crtc->primary->state : NULL;
    req->fb_id = ps && ps->fb ? ps->fb->base.id : 0;
    if (ps) {
        req->x = ps->src_x >> 16;
        req->y = ps->src_y >> 16;
    }
    if (crtc->state->enable) {
        req->mode = crtc->state->mode;
        req->mode.vrefresh = drm_mode_vrefresh(&req->mode);
        req->mode_valid = 1;
    } else {
        k_memset(&req->mode, 0, sizeof(req->mode));
        req->mode_valid = 0;
    }
    if (!file->aspect_ratio) {
        req->mode.flags &= ~(uint32_t)DRM_MODE_FLAG_PIC_AR_MASK;
    }
    return 0;
}

/** GETENCODER: an encoder's type, its CRTC now and the CRTCs it can
 * take. */
int32_t drm_mode_getencoder(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_get_encoder *req = (struct drm_mode_get_encoder *)data;
    struct drm_encoder *encoder;
    struct drm_crtc *crtc = NULL;
    uint32_t i;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    encoder = drm_encoder_find(dev, file, req->encoder_id);
    if (!encoder) {
        return -ENOENT;
    }
    /* Linux's drm_encoder_get_crtc: the CRTC of the connector it drives. */
    for (i = 0; i < dev->mode_config.num_connector; i++) {
        struct drm_connector_state *cs = dev->mode_config.connectors[i]->state;

        if (cs->best_encoder == encoder) {
            crtc = cs->crtc;
            break;
        }
    }
    req->crtc_id = crtc ? crtc->base.id : 0;
    req->encoder_type = encoder->encoder_type;
    req->encoder_id = encoder->base.id;
    req->possible_crtcs = encoder->possible_crtcs;
    req->possible_clones = encoder->possible_clones;
    return 0;
}

/** Fill the user arrays of an object's properties and values, up to
 * `*count`, with those `atomic` lets the caller see, and set `*count` to
 * their number, as Linux's drm_mode_object_get_properties. */
static int32_t object_properties(struct drm_mode_object *obj, int atomic, uint64_t props_ptr,
                                 uint64_t values_ptr, uint32_t *count)
{
    uint32_t i, n = 0;
    uint64_t value;
    int32_t rc;

    for (i = 0; i < obj->count_props; i++) {
        struct drm_property *prop = obj->props[i];

        if ((prop->flags & DRM_MODE_PROP_ATOMIC) && !atomic) {
            continue;
        }
        if (*count > n) {
            rc = drm_object_property_get_value(obj, prop, &value);
            if (rc != 0) {
                return rc;
            }
            if (put_id(props_ptr, n, *count, prop->base.id) != 0 ||
                copy_to_user((void __user *)(uintptr_t)(values_ptr + 8u * n), &value, 8) != 0) {
                return -EFAULT;
            }
        }
        n++;
    }
    *count = n;
    return 0;
}

/** Whether a mode goes to a client: not a stereo one without the stereo
 * cap, nor, without the aspect-ratio cap, one equal but for its aspect to
 * a mode already given, as Linux's drm_mode_expose_to_userspace. `given`
 * is the list's first mode and `mode` a later one. */
static int mode_exposed(const struct drm_display_mode *given, const struct drm_display_mode *mode,
                        const struct drm_file *file)
{
    const struct drm_display_mode *m;
    uint32_t ar = DRM_MODE_FLAG_PIC_AR_MASK;

    if (!file->stereo_allowed && (mode->info.flags & DRM_MODE_FLAG_3D_MASK)) {
        return 0;
    }
    if (!file->aspect_ratio) {
        for (m = given; m != mode; m = m->next) {
            if (mode_exposed(given, m, file) && m->info.clock == mode->info.clock &&
                m->info.hdisplay == mode->info.hdisplay &&
                m->info.hsync_start == mode->info.hsync_start &&
                m->info.hsync_end == mode->info.hsync_end && m->info.htotal == mode->info.htotal &&
                m->info.hskew == mode->info.hskew && m->info.vdisplay == mode->info.vdisplay &&
                m->info.vsync_start == mode->info.vsync_start &&
                m->info.vsync_end == mode->info.vsync_end && m->info.vtotal == mode->info.vtotal &&
                m->info.vscan == mode->info.vscan &&
                (m->info.flags & ~ar) == (mode->info.flags & ~ar)) {
                return 0;
            }
        }
    }
    return 1;
}

/** GETCONNECTOR, Linux's drm_mode_getconnector: the connector's encoders,
 * its modes, probed afresh when the master asks with no room for them, its
 * status, size and encoder, and its properties. */
int32_t drm_mode_getconnector(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_get_connector *req = (struct drm_mode_get_connector *)data;
    struct drm_connector *connector;
    struct drm_display_mode *m;
    struct drm_mode_modeinfo u;
    uint32_t i, n = 0, count;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    connector = drm_connector_lookup(dev, file, req->connector_id);
    if (!connector) {
        return -ENOENT;
    }
    count = 0;
    for (i = 0; i < dev->mode_config.num_encoder; i++) {
        if (connector->possible_encoders & (1u << i)) {
            count++;
        }
    }
    if (req->count_encoders >= count && count) {
        for (i = 0; i < dev->mode_config.num_encoder; i++) {
            if (!(connector->possible_encoders & (1u << i))) {
                continue;
            }
            if (put_id(req->encoders_ptr, n, count, dev->mode_config.encoders[i]->base.id) != 0) {
                return -EFAULT;
            }
            n++;
        }
    }
    req->count_encoders = count;
    req->connector_id = connector->base.id;
    req->connector_type = connector->connector_type;
    req->connector_type_id = connector->connector_type_id;
    if (req->count_modes == 0 && drm_is_current_master(file) && drm_modeset_lock(dev) == 0) {
        drm_helper_probe_single_connector_modes(connector, dev->mode_config.max_width,
                                                dev->mode_config.max_height);
        drm_modeset_unlock(dev);
    }
    req->mm_width = connector->mm_width;
    req->mm_height = connector->mm_height;
    req->subpixel = connector->subpixel;
    req->connection = (uint32_t)connector->status;
    count = 0;
    for (m = connector->modes; m; m = m->next) {
        count += mode_exposed(connector->modes, m, file);
    }
    if (req->count_modes >= count && count) {
        n = 0;
        for (m = connector->modes; m; m = m->next) {
            if (!mode_exposed(connector->modes, m, file)) {
                continue;
            }
            u = m->info;
            u.vrefresh = drm_mode_vrefresh(&u);
            if (!file->aspect_ratio) {
                u.flags &= ~(uint32_t)DRM_MODE_FLAG_PIC_AR_MASK;
            }
            if (copy_to_user((void __user *)(uintptr_t)(req->modes_ptr + n * sizeof(u)), &u,
                             sizeof(u)) != 0) {
                return -EFAULT;
            }
            n++;
        }
    }
    req->count_modes = count;
    req->encoder_id = connector->state->best_encoder ? connector->state->best_encoder->base.id : 0;
    return object_properties(&connector->base, file->atomic, req->props_ptr, req->prop_values_ptr,
                             &req->count_props);
}

/** OBJ_GETPROPERTIES: the properties of any object that has some. */
int32_t drm_mode_obj_get_properties_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_obj_get_properties *req = (struct drm_mode_obj_get_properties *)data;
    struct drm_mode_object *obj;
    int32_t rc;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    obj = drm_mode_object_find(dev, file, req->obj_id, req->obj_type);
    if (!obj) {
        return -ENOENT;
    }
    rc = obj->count_props == 0 && obj->type != DRM_MODE_OBJECT_CRTC &&
                 obj->type != DRM_MODE_OBJECT_PLANE && obj->type != DRM_MODE_OBJECT_CONNECTOR
             ? -EINVAL
             : object_properties(obj, file->atomic, req->props_ptr, req->prop_values_ptr,
                                 &req->count_props);
    drm_mode_object_put(obj);
    return rc;
}

/** GETPROPERTY, Linux's drm_mode_getproperty_ioctl: a property's name,
 * flags, values, and an enum's or bitmask's names. */
int32_t drm_mode_getproperty_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_get_property *req = (struct drm_mode_get_property *)data;
    struct drm_mode_property_enum e;
    struct drm_property *prop;
    uint32_t i, j;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    prop = (struct drm_property *)drm_mode_object_find(dev, file, req->prop_id,
                                                       DRM_MODE_OBJECT_PROPERTY);
    if (!prop) {
        return -ENOENT;
    }
    k_memcpy(req->name, prop->name, DRM_PROP_NAME_LEN);
    req->flags = prop->flags;
    for (i = 0; i < prop->num_values; i++) {
        if (i < req->count_values &&
            copy_to_user((void __user *)(uintptr_t)(req->values_ptr + 8u * i), &prop->values[i],
                         8) != 0) {
            return -EFAULT;
        }
    }
    req->count_values = prop->num_values;
    if (prop_type_is(prop, DRM_MODE_PROP_ENUM) || prop_type_is(prop, DRM_MODE_PROP_BITMASK)) {
        for (i = 0; i < prop->num_values; i++) {
            if (req->count_enum_blobs < i + 1) {
                continue;
            }
            k_memset(&e, 0, sizeof(e));
            e.value = prop->values[i];
            for (j = 0; j + 1 < DRM_PROP_NAME_LEN && prop->enum_names[i][j]; j++) {
                e.name[j] = prop->enum_names[i][j];
            }
            if (copy_to_user((void __user *)(uintptr_t)(req->enum_blob_ptr + i * sizeof(e)), &e,
                             sizeof(e)) != 0) {
                return -EFAULT;
            }
        }
        req->count_enum_blobs = prop->num_values;
    }
    if (prop_type_is(prop, DRM_MODE_PROP_BLOB)) {
        req->count_enum_blobs = 0;
    }
    return 0;
}

/** GETGAMMA: a CRTC's gamma table, whose size the caller must give. */
int32_t drm_mode_gamma_get_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    struct drm_mode_crtc_lut *req = (struct drm_mode_crtc_lut *)data;
    struct drm_crtc *crtc;
    uint32_t size;

    if (!drm_core_check_feature(dev, DRIVER_MODESET)) {
        return -EOPNOTSUPP;
    }
    crtc = drm_crtc_find(dev, file, req->crtc_id);
    if (!crtc) {
        return -ENOENT;
    }
    if (req->gamma_size != crtc->gamma_size) {
        return -EINVAL;
    }
    size = crtc->gamma_size * 2u;
    if (size && (copy_to_user((void __user *)(uintptr_t)req->red, crtc->gamma_store, size) != 0 ||
                 copy_to_user((void __user *)(uintptr_t)req->green,
                              crtc->gamma_store + crtc->gamma_size, size) != 0 ||
                 copy_to_user((void __user *)(uintptr_t)req->blue,
                              crtc->gamma_store + 2 * crtc->gamma_size, size) != 0)) {
        return -EFAULT;
    }
    return 0;
}

/* ------------------------------------------------------------------------
 * Leases, which no driver offers yet
 */

int32_t drm_mode_create_lease_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_mode_list_lessees_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_mode_get_lease_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}

int32_t drm_mode_revoke_lease_ioctl(struct drm_device *dev, void *data, struct drm_file *file)
{
    (void)dev;
    (void)data;
    (void)file;
    return -EOPNOTSUPP;
}
