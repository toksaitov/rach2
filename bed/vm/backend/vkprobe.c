/*
 * vkprobe.c: the backend's check that a resource a Venus context takes can
 * be imported into Vulkan as a dma-buf, as Venus's renderer imports it, and
 * that the memory it imports holds the resource's pixels.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#define _GNU_SOURCE
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <drm_fourcc.h>
#include <vulkan/vulkan.h>

#include "backend.h"

/* The device extensions an import of a dma-buf needs. */
static const char *const exts[] = {
    VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
    VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
};

/** Return 1 when phys has every extension of exts. */
static int has_exts(VkPhysicalDevice phys)
{
    VkExtensionProperties *p;
    uint32_t n = 0, i, j, found = 0;

    if (vkEnumerateDeviceExtensionProperties(phys, 0, &n, 0) != VK_SUCCESS ||
        (p = calloc(n ? n : 1, sizeof(*p))) == 0) {
        return 0;
    }
    if (vkEnumerateDeviceExtensionProperties(phys, 0, &n, p) == VK_SUCCESS) {
        for (j = 0; j < sizeof(exts) / sizeof(exts[0]); j++) {
            for (i = 0; i < n; i++) {
                if (strcmp(p[i].extensionName, exts[j]) == 0) {
                    found++;
                    break;
                }
            }
        }
    }
    free(p);
    return found == sizeof(exts) / sizeof(exts[0]);
}

/** Make a device on the first physical device that imports dma-bufs. */
static VkDevice make_device(VkInstance inst, VkPhysicalDevice *phys)
{
    VkPhysicalDevice list[8];
    uint32_t n = 8, i;
    float prio = 1.0f;
    VkDeviceQueueCreateInfo q = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
    VkDeviceCreateInfo d = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    VkDevice dev;

    if (vkEnumeratePhysicalDevices(inst, &n, list) < 0) {
        return VK_NULL_HANDLE;
    }
    for (i = 0; i < n && !has_exts(list[i]); i++) {
    }
    if (i == n) {
        fprintf(stderr, "backend: venus: no Vulkan device imports dma-bufs\n");
        return VK_NULL_HANDLE;
    }
    *phys = list[i];
    q.queueCount = 1;
    q.pQueuePriorities = &prio;
    d.queueCreateInfoCount = 1;
    d.pQueueCreateInfos = &q;
    d.enabledExtensionCount = sizeof(exts) / sizeof(exts[0]);
    d.ppEnabledExtensionNames = exts;
    return vkCreateDevice(*phys, &d, 0, &dev) == VK_SUCCESS ? dev : VK_NULL_HANDLE;
}

/** CRC-32 of the export's rows, read from map, X bytes set to 0xff. */
static uint32_t rows_crc(const uint8_t *map, const struct vr_export *e)
{
    uint64_t row = (uint64_t)e->width * 4, y, x;
    uint8_t *all = malloc(row * e->height);
    uint32_t crc;

    if (all == 0) {
        return 0;
    }
    for (y = 0; y < e->height; y++) {
        memcpy(all + row * y, map + e->offset + (uint64_t)e->stride * y, row);
        for (x = 3; x < row; x += 4) {
            all[row * y + x] = 0xff;
        }
    }
    crc = crc32_bytes(all, row * e->height);
    free(all);
    return crc;
}

/**
 * Import memory from dma-buf e on dev, map it and put its CRC in *crc.
 * Returns 0, or -1 with a message. e->fd stays the caller's.
 */
static int import(VkDevice dev, VkPhysicalDevice phys, const struct vr_export *e, uint32_t *crc)
{
    PFN_vkGetMemoryFdPropertiesKHR fdprops =
        (PFN_vkGetMemoryFdPropertiesKHR)vkGetDeviceProcAddr(dev, "vkGetMemoryFdPropertiesKHR");
    VkMemoryFdPropertiesKHR props = {.sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR};
    VkImportMemoryFdInfoKHR imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
    VkMemoryAllocateInfo alloc = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    VkPhysicalDeviceMemoryProperties mem;
    VkDeviceMemory dm;
    off_t size = lseek(e->fd, 0, SEEK_END);
    uint32_t i;
    void *map;

    imp.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT;
    if (fdprops == 0 || size <= 0 || (uint64_t)size < e->offset + (uint64_t)e->stride * e->height ||
        fdprops(dev, imp.handleType, e->fd, &props) != VK_SUCCESS) {
        fprintf(stderr, "backend: venus: the dma-buf is not one Vulkan takes\n");
        return -1;
    }
    vkGetPhysicalDeviceMemoryProperties(phys, &mem);
    for (i = 0; i < mem.memoryTypeCount; i++) {
        if ((props.memoryTypeBits >> i & 1) &&
            (mem.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT)) {
            break;
        }
    }
    if (i == mem.memoryTypeCount) {
        fprintf(stderr, "backend: venus: no mappable memory type takes the dma-buf\n");
        return -1;
    }
    /* A successful import owns the descriptor it is given. */
    if ((imp.fd = fcntl(e->fd, F_DUPFD_CLOEXEC, 0)) < 0) {
        return -1;
    }
    alloc.pNext = &imp;
    alloc.allocationSize = (VkDeviceSize)size;
    alloc.memoryTypeIndex = i;
    if (vkAllocateMemory(dev, &alloc, 0, &dm) != VK_SUCCESS) {
        close(imp.fd);
        fprintf(stderr, "backend: venus: Vulkan refused the dma-buf's import\n");
        return -1;
    }
    if (vkMapMemory(dev, dm, 0, VK_WHOLE_SIZE, 0, &map) != VK_SUCCESS) {
        vkFreeMemory(dev, dm, 0);
        fprintf(stderr, "backend: venus: cannot map the imported memory\n");
        return -1;
    }
    *crc = rows_crc(map, e);
    vkUnmapMemory(dev, dm);
    vkFreeMemory(dev, dm, 0);
    return 0;
}

/**
 * Import a resource's dma-buf e into Vulkan's device memory on the first
 * device that imports dma-bufs, map it, and put the CRC-32 of its rows as
 * stored, X bytes set to 0xff, in *crc. Returns 0, or -1 with a message.
 * The instance and device last one call.
 */
int vk_probe(const struct vr_export *e, uint32_t *crc)
{
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    VkInstance inst;
    VkPhysicalDevice phys;
    VkDevice dev;
    int err = -1;

    if (e->fourcc != DRM_FORMAT_XRGB8888 && e->fourcc != DRM_FORMAT_ARGB8888) {
        fprintf(stderr, "backend: venus: the probe reads only 32-bit RGB, not %08x\n",
                (unsigned)e->fourcc);
        return -1;
    }
    app.pApplicationName = "vgpu-backend";
    app.apiVersion = VK_API_VERSION_1_1;
    ici.pApplicationInfo = &app;
    if (vkCreateInstance(&ici, 0, &inst) != VK_SUCCESS) {
        fprintf(stderr, "backend: venus: no Vulkan instance\n");
        return -1;
    }
    if ((dev = make_device(inst, &phys)) != VK_NULL_HANDLE) {
        err = import(dev, phys, e, crc);
        vkDestroyDevice(dev, 0);
    }
    vkDestroyInstance(inst, 0);
    return err;
}
