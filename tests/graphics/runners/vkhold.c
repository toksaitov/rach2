/*
 * vkhold.c: a Vulkan layer that times a program's presents and holds it
 * after chosen ones, so that the frame on the screen is a known one while
 * vkcube.sh reads it back.
 *
 * The layer intercepts vkQueuePresentKHR alone and passes every other call
 * down the chain. Its environment:
 *
 *   VKHOLD_FRAMES  the presents to hold after, in rising order, as "300 345"
 *   VKHOLD_TIMED   the presents to time from the first, 300 by default
 *   VKHOLD_LOG     the file it appends its lines to
 *   VKHOLD_FIFO    the FIFO it reads a line from to go on after a hold
 *
 * After the present that VKHOLD_TIMED names it writes
 *
 *   vkhold: timed: N presents in S s
 *
 * S being the time from the first present's call to the Nth's return, and
 * after each present that VKHOLD_FRAMES names it writes
 *
 *   vkhold: held: frame N
 *
 * and blocks until a line can be read from VKHOLD_FIFO. The Vulkan types it
 * needs are written out here from vulkan_core.h and vk_layer.h, so that it
 * builds with the C library alone, in Rach2's root and in the Debian guest.
 */
#define _POSIX_C_SOURCE 200809L
#include <fcntl.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

typedef int32_t VkResult;
typedef struct VkInstance_T *VkInstance;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;
typedef struct VkDevice_T *VkDevice;
typedef struct VkQueue_T *VkQueue;
typedef void (*PFN_vkVoidFunction)(void);
typedef PFN_vkVoidFunction (*PFN_vkGetInstanceProcAddr)(VkInstance, const char *);
typedef PFN_vkVoidFunction (*PFN_vkGetDeviceProcAddr)(VkDevice, const char *);
typedef VkResult (*PFN_vkCreateInstance)(const void *, const void *, VkInstance *);
typedef VkResult (*PFN_vkCreateDevice)(VkPhysicalDevice, const void *, const void *, VkDevice *);
typedef VkResult (*PFN_vkQueuePresentKHR)(VkQueue, const void *);

#define VK_SUCCESS                                    0
#define VK_ERROR_INITIALIZATION_FAILED                (-3)
#define VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO 47
#define VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO   48
#define VK_LAYER_LINK_INFO                            0

/* The head of every structure of a pNext chain. */
struct vk_chain {
    int32_t sType;
    const struct vk_chain *pNext;
};

/* VkLayerInstanceLink and VkLayerDeviceLink of vk_layer.h. */
struct instance_link {
    struct instance_link *pNext;
    PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
    PFN_vkVoidFunction pfnNextGetPhysicalDeviceProcAddr;
};

struct device_link {
    struct device_link *pNext;
    PFN_vkGetInstanceProcAddr pfnNextGetInstanceProcAddr;
    PFN_vkGetDeviceProcAddr pfnNextGetDeviceProcAddr;
};

/* VkLayerInstanceCreateInfo and VkLayerDeviceCreateInfo with their link
 * member alone, the union's first. */
struct instance_create_info {
    int32_t sType;
    const void *pNext;
    int32_t function;
    struct instance_link *pLayerInfo;
};

struct device_create_info {
    int32_t sType;
    const void *pNext;
    int32_t function;
    struct device_link *pLayerInfo;
};

/* The program's one instance and one device, as vkcube makes them. */
static VkInstance instance;
static PFN_vkGetInstanceProcAddr next_gipa;
static PFN_vkGetDeviceProcAddr next_gdpa;
static PFN_vkQueuePresentKHR next_present;

static unsigned long presents; /* presents returned so far */
static struct timespec first;  /* the first present's call */

PFN_vkVoidFunction vkhold_GetInstanceProcAddr(VkInstance inst, const char *name);
PFN_vkVoidFunction vkhold_GetDeviceProcAddr(VkDevice dev, const char *name);

/** Append one line, formatted as printf's, to VKHOLD_LOG. */
static void say(const char *format, ...)
{
    const char *path = getenv("VKHOLD_LOG");
    va_list ap;
    FILE *f;

    if (path == NULL || (f = fopen(path, "a")) == NULL)
        return;
    va_start(ap, format);
    vfprintf(f, format, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/** Give 1 when VKHOLD_FRAMES names present `n`. */
static int held(unsigned long n)
{
    const char *p = getenv("VKHOLD_FRAMES");
    char *end;

    while (p != NULL && *p != '\0') {
        unsigned long v = strtoul(p, &end, 10);

        if (end == p)
            break;
        if (v == n)
            return 1;
        p = end;
    }
    return 0;
}

/** Block until a line, or the end, is read from VKHOLD_FIFO. */
static void wait_fifo(void)
{
    const char *path = getenv("VKHOLD_FIFO");
    char c;
    int fd;

    if (path == NULL || (fd = open(path, O_RDONLY)) < 0)
        return;
    while (read(fd, &c, 1) == 1 && c != '\n')
        ;
    close(fd);
}

/**
 * Present through the next layer, then time or hold as the environment
 * says. The hold comes after the present returns, so that the frame it
 * presented is the one the compositor shows during the hold.
 */
static VkResult vkhold_QueuePresentKHR(VkQueue queue, const void *info)
{
    const char *timed = getenv("VKHOLD_TIMED");
    unsigned long want = timed != NULL ? strtoul(timed, NULL, 10) : 300;
    struct timespec now;
    VkResult r;

    if (presents == 0)
        clock_gettime(CLOCK_MONOTONIC, &first);
    r = next_present(queue, info);
    presents++;
    if (presents == want) {
        clock_gettime(CLOCK_MONOTONIC, &now);
        say("vkhold: timed: %lu presents in %.3f s", presents,
            (double)(now.tv_sec - first.tv_sec) + (now.tv_nsec - first.tv_nsec) / 1e9);
    }
    if (held(presents)) {
        say("vkhold: held: frame %lu", presents);
        wait_fifo();
    }
    return r;
}

/** Create the instance through the next layer, keeping its proc address. */
static VkResult vkhold_CreateInstance(const void *info, const void *alloc, VkInstance *out)
{
    const struct vk_chain *c = info;
    struct instance_create_info *link = NULL;
    PFN_vkCreateInstance create;
    VkResult r;

    for (c = c->pNext; c != NULL; c = c->pNext) {
        struct instance_create_info *l = (struct instance_create_info *)c;

        if (l->sType == VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO &&
            l->function == VK_LAYER_LINK_INFO) {
            link = l;
            break;
        }
    }
    if (link == NULL)
        return VK_ERROR_INITIALIZATION_FAILED;
    next_gipa = link->pLayerInfo->pfnNextGetInstanceProcAddr;
    link->pLayerInfo = link->pLayerInfo->pNext;
    create = (PFN_vkCreateInstance)next_gipa(NULL, "vkCreateInstance");
    if (create == NULL)
        return VK_ERROR_INITIALIZATION_FAILED;
    r = create(info, alloc, out);
    if (r == VK_SUCCESS)
        instance = *out;
    return r;
}

/** Create the device through the next layer, keeping its present. */
static VkResult vkhold_CreateDevice(VkPhysicalDevice phys, const void *info, const void *alloc,
                                    VkDevice *out)
{
    const struct vk_chain *c = info;
    struct device_create_info *link = NULL;
    PFN_vkGetInstanceProcAddr gipa;
    PFN_vkCreateDevice create;
    VkResult r;

    for (c = c->pNext; c != NULL; c = c->pNext) {
        struct device_create_info *l = (struct device_create_info *)c;

        if (l->sType == VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO &&
            l->function == VK_LAYER_LINK_INFO) {
            link = l;
            break;
        }
    }
    if (link == NULL)
        return VK_ERROR_INITIALIZATION_FAILED;
    gipa = link->pLayerInfo->pfnNextGetInstanceProcAddr;
    next_gdpa = link->pLayerInfo->pfnNextGetDeviceProcAddr;
    link->pLayerInfo = link->pLayerInfo->pNext;
    create = (PFN_vkCreateDevice)gipa(instance, "vkCreateDevice");
    if (create == NULL)
        return VK_ERROR_INITIALIZATION_FAILED;
    r = create(phys, info, alloc, out);
    if (r == VK_SUCCESS)
        next_present = (PFN_vkQueuePresentKHR)next_gdpa(*out, "vkQueuePresentKHR");
    return r;
}

/** The layer's vkGetDeviceProcAddr: its present, else the next layer's. */
PFN_vkVoidFunction vkhold_GetDeviceProcAddr(VkDevice dev, const char *name)
{
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkhold_GetDeviceProcAddr;
    if (strcmp(name, "vkQueuePresentKHR") == 0 && next_present != NULL)
        return (PFN_vkVoidFunction)vkhold_QueuePresentKHR;
    return next_gdpa != NULL ? next_gdpa(dev, name) : NULL;
}

/** The layer's vkGetInstanceProcAddr: its own four, else the next layer's. */
PFN_vkVoidFunction vkhold_GetInstanceProcAddr(VkInstance inst, const char *name)
{
    if (strcmp(name, "vkGetInstanceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkhold_GetInstanceProcAddr;
    if (strcmp(name, "vkCreateInstance") == 0)
        return (PFN_vkVoidFunction)vkhold_CreateInstance;
    if (strcmp(name, "vkCreateDevice") == 0)
        return (PFN_vkVoidFunction)vkhold_CreateDevice;
    if (strcmp(name, "vkGetDeviceProcAddr") == 0)
        return (PFN_vkVoidFunction)vkhold_GetDeviceProcAddr;
    return next_gipa != NULL ? next_gipa(inst, name) : NULL;
}
