/*
 * nocopy.c: a Vulkan buffer of the test bed's VM mapped into a Rach2
 * program through Venus, without copies (tests/kvm/bed-shm.sh).
 *
 *   nocopy rounds N BYTES   N rounds through two mapped buffers
 *   nocopy serial N BYTES   N buffers made, mapped, touched and freed in turn
 *   nocopy held N BYTES     N buffers held at once, then freed together
 *   nocopy bandwidth BYTES  the CPU's writes and reads through a mapping
 *   nocopy hold BYTES       a mapping touched until it is taken back
 *
 * The memory is Venus's host-visible and coherent memory, which Rach2 maps
 * from the VM's own pages: each allocation is a blob whose page list the
 * VM's backend answers and Rach2's KVM resolves (vgpu-shm(4)). A round
 * writes a pattern of its own into the first buffer through the mapping,
 * has the VM's GPU copy it to the second, then fill the first with a value
 * of its own, each submission waited for on a fence, and reads both back
 * through their mappings: the CPU's writes seen by the GPU, the GPU's seen
 * by the CPU, in fence order. `hold` maps a buffer, writes `NOCOPY HELD`
 * and touches the mapping without end; the VM's death takes the mapping
 * back, and the touch that follows raises SIGBUS, as Linux's revoked
 * mappings do, which it reports as `NOCOPY SIGBUS` before it exits.
 *
 * Each mode ends with `NOCOPY OK` or `NOCOPY FAIL: reason`; the figures are
 * `NOCOPY NAME: ...` lines. The Vulkan types it needs are written out here
 * from vulkan_core.h, so that it builds with the C library alone on
 * Rach2's GNU root, whose loader it opens by name.
 */
#define _POSIX_C_SOURCE 200809L
#include <dlfcn.h>
#include <setjmp.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <time.h>
#include <unistd.h>

/* getrusage's calling thread alone, Linux's, which POSIX does not name. */
#ifndef RUSAGE_THREAD
#define RUSAGE_THREAD 1
#endif

typedef int32_t VkResult;
typedef uint32_t VkFlags;
typedef struct VkInstance_T *VkInstance;
typedef struct VkPhysicalDevice_T *VkPhysicalDevice;
typedef struct VkDevice_T *VkDevice;
typedef struct VkQueue_T *VkQueue;
typedef struct VkCommandBuffer_T *VkCommandBuffer;
typedef uint64_t VkHandle; /* every non-dispatchable handle */
typedef void (*PFN_vkVoidFunction)(void);

#define VK_SUCCESS 0

#define VK_STRUCTURE_TYPE_APPLICATION_INFO             0
#define VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO         1
#define VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO     2
#define VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO           3
#define VK_STRUCTURE_TYPE_SUBMIT_INFO                  4
#define VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO         5
#define VK_STRUCTURE_TYPE_FENCE_CREATE_INFO            8
#define VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO           12
#define VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO     39
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO 40
#define VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO    42
#define VK_STRUCTURE_TYPE_MEMORY_BARRIER               46

#define VK_API_VERSION_1_1                   ((1u << 22) | (1u << 12))
#define VK_QUEUE_GRAPHICS_BIT                1u
#define VK_QUEUE_COMPUTE_BIT                 2u
#define VK_QUEUE_TRANSFER_BIT                4u
#define VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT  2u
#define VK_MEMORY_PROPERTY_HOST_COHERENT_BIT 4u
#define VK_BUFFER_USAGE_TRANSFER_SRC_BIT     1u
#define VK_BUFFER_USAGE_TRANSFER_DST_BIT     2u
#define VK_PIPELINE_STAGE_TRANSFER_BIT       0x1000u
#define VK_PIPELINE_STAGE_HOST_BIT           0x4000u
#define VK_ACCESS_TRANSFER_READ_BIT          0x800u
#define VK_ACCESS_TRANSFER_WRITE_BIT         0x1000u
#define VK_ACCESS_HOST_READ_BIT              0x2000u
#define VK_ACCESS_HOST_WRITE_BIT             0x4000u
#define VK_COMMAND_BUFFER_LEVEL_PRIMARY      0
#define VK_COMMAND_POOL_RESET_BUFFER_BIT     2u
#define VK_COMMAND_BUFFER_ONE_TIME_BIT       1u

struct vk_app {
    int32_t sType;
    const void *pNext;
    const char *pApplicationName;
    uint32_t applicationVersion;
    const char *pEngineName;
    uint32_t engineVersion;
    uint32_t apiVersion;
};

struct vk_instance_info {
    int32_t sType;
    const void *pNext;
    VkFlags flags;
    const struct vk_app *pApplicationInfo;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
};

struct vk_queue_info {
    int32_t sType;
    const void *pNext;
    VkFlags flags;
    uint32_t queueFamilyIndex;
    uint32_t queueCount;
    const float *pQueuePriorities;
};

struct vk_device_info {
    int32_t sType;
    const void *pNext;
    VkFlags flags;
    uint32_t queueCreateInfoCount;
    const struct vk_queue_info *pQueueCreateInfos;
    uint32_t enabledLayerCount;
    const char *const *ppEnabledLayerNames;
    uint32_t enabledExtensionCount;
    const char *const *ppEnabledExtensionNames;
    const void *pEnabledFeatures;
};

struct vk_family {
    uint32_t queueFlags, queueCount, timestampValidBits, granularity[3];
};

struct vk_memory_type {
    uint32_t propertyFlags, heapIndex;
};

struct vk_memory_properties {
    uint32_t memoryTypeCount;
    struct vk_memory_type memoryTypes[32];
    uint32_t memoryHeapCount;
    struct {
        uint64_t size;
        uint32_t flags;
    } memoryHeaps[16];
};

struct vk_buffer_info {
    int32_t sType;
    const void *pNext;
    VkFlags flags;
    uint64_t size;
    VkFlags usage;
    uint32_t sharingMode;
    uint32_t queueFamilyIndexCount;
    const uint32_t *pQueueFamilyIndices;
};

struct vk_requirements {
    uint64_t size, alignment;
    uint32_t memoryTypeBits;
};

struct vk_allocate_info {
    int32_t sType;
    const void *pNext;
    uint64_t allocationSize;
    uint32_t memoryTypeIndex;
};

struct vk_pool_info {
    int32_t sType;
    const void *pNext;
    VkFlags flags;
    uint32_t queueFamilyIndex;
};

struct vk_command_info {
    int32_t sType;
    const void *pNext;
    VkHandle commandPool;
    uint32_t level;
    uint32_t commandBufferCount;
};

struct vk_begin_info {
    int32_t sType;
    const void *pNext;
    VkFlags flags;
    const void *pInheritanceInfo;
};

struct vk_fence_info {
    int32_t sType;
    const void *pNext;
    VkFlags flags;
};

struct vk_submit_info {
    int32_t sType;
    const void *pNext;
    uint32_t waitSemaphoreCount;
    const VkHandle *pWaitSemaphores;
    const VkFlags *pWaitDstStageMask;
    uint32_t commandBufferCount;
    const VkCommandBuffer *pCommandBuffers;
    uint32_t signalSemaphoreCount;
    const VkHandle *pSignalSemaphores;
};

struct vk_barrier {
    int32_t sType;
    const void *pNext;
    VkFlags srcAccessMask, dstAccessMask;
};

struct vk_copy {
    uint64_t srcOffset, dstOffset, size;
};

/* The entry points, loaded by name. */
static struct {
    PFN_vkVoidFunction (*GetInstanceProcAddr)(VkInstance, const char *);
    VkResult (*CreateInstance)(const struct vk_instance_info *, const void *, VkInstance *);
    VkResult (*EnumeratePhysicalDevices)(VkInstance, uint32_t *, VkPhysicalDevice *);
    void (*GetPhysicalDeviceProperties)(VkPhysicalDevice, void *);
    void (*GetPhysicalDeviceMemoryProperties)(VkPhysicalDevice, struct vk_memory_properties *);
    void (*GetPhysicalDeviceQueueFamilyProperties)(VkPhysicalDevice, uint32_t *,
                                                   struct vk_family *);
    VkResult (*CreateDevice)(VkPhysicalDevice, const struct vk_device_info *, const void *,
                             VkDevice *);
    void (*GetDeviceQueue)(VkDevice, uint32_t, uint32_t, VkQueue *);
    VkResult (*CreateBuffer)(VkDevice, const struct vk_buffer_info *, const void *, VkHandle *);
    void (*DestroyBuffer)(VkDevice, VkHandle, const void *);
    void (*GetBufferMemoryRequirements)(VkDevice, VkHandle, struct vk_requirements *);
    VkResult (*AllocateMemory)(VkDevice, const struct vk_allocate_info *, const void *, VkHandle *);
    void (*FreeMemory)(VkDevice, VkHandle, const void *);
    VkResult (*BindBufferMemory)(VkDevice, VkHandle, VkHandle, uint64_t);
    VkResult (*MapMemory)(VkDevice, VkHandle, uint64_t, uint64_t, VkFlags, void **);
    void (*UnmapMemory)(VkDevice, VkHandle);
    VkResult (*CreateCommandPool)(VkDevice, const struct vk_pool_info *, const void *, VkHandle *);
    VkResult (*AllocateCommandBuffers)(VkDevice, const struct vk_command_info *, VkCommandBuffer *);
    VkResult (*BeginCommandBuffer)(VkCommandBuffer, const struct vk_begin_info *);
    VkResult (*EndCommandBuffer)(VkCommandBuffer);
    void (*CmdCopyBuffer)(VkCommandBuffer, VkHandle, VkHandle, uint32_t, const struct vk_copy *);
    void (*CmdFillBuffer)(VkCommandBuffer, VkHandle, uint64_t, uint64_t, uint32_t);
    void (*CmdPipelineBarrier)(VkCommandBuffer, VkFlags, VkFlags, VkFlags, uint32_t,
                               const struct vk_barrier *, uint32_t, const void *, uint32_t,
                               const void *);
    VkResult (*CreateFence)(VkDevice, const struct vk_fence_info *, const void *, VkHandle *);
    VkResult (*ResetFences)(VkDevice, uint32_t, const VkHandle *);
    VkResult (*WaitForFences)(VkDevice, uint32_t, const VkHandle *, uint32_t, uint64_t);
    VkResult (*QueueSubmit)(VkQueue, uint32_t, const struct vk_submit_info *, VkHandle);
} vk;

/* The device, its queue and what one submission needs. */
static struct {
    VkInstance instance;
    VkPhysicalDevice gpu;
    VkDevice dev;
    VkQueue queue;
    uint32_t family, type;
    VkHandle pool, fence;
    VkCommandBuffer cmd;
    char name[256];
} g;

/* A buffer and the memory it is bound to, mapped at `map`. */
struct buf {
    VkHandle buffer, memory;
    uint32_t *map;
    uint64_t bytes;
};

static sigjmp_buf touched;
static volatile sig_atomic_t holding;

/** Print `NOCOPY FAIL: why` and exit with status 1. */
static void die(const char *why, long code)
{
    printf("NOCOPY FAIL: %s (%ld)\n", why, code);
    fflush(stdout);
    exit(1);
}

/** Microseconds of the monotonic clock. */
static double now_us(void)
{
    struct timespec t;

    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

/** A count of /proc/stat, or -1. */
static long stat_count(const char *name)
{
    char line[256];
    size_t n = strlen(name);
    long v = -1;
    FILE *f = fopen("/proc/stat", "r");

    while (f && fgets(line, sizeof(line), f)) {
        if (strncmp(line, name, n) == 0 && line[n] == ' ') {
            v = strtol(line + n + 1, 0, 10);
            break;
        }
    }
    if (f) {
        fclose(f);
    }
    return v;
}

/** Load one entry point of the instance or the loader, or die. */
static PFN_vkVoidFunction need(const char *name)
{
    PFN_vkVoidFunction f = vk.GetInstanceProcAddr(g.instance, name);

    if (!f) {
        die(name, 0);
    }
    return f;
}

/* A function pointer is copied into a field of another type, which C99
 * allows only through memcpy. */
#define LOAD(field, name)                         \
    do {                                          \
        PFN_vkVoidFunction f_ = need(name);       \
        memcpy(&vk.field, &f_, sizeof(vk.field)); \
    } while (0)

/** Open the loader, make an instance of Vulkan 1.1 and load its calls. */
static void open_instance(void)
{
    static const struct vk_app app = {
        VK_STRUCTURE_TYPE_APPLICATION_INFO, 0, "nocopy", 1, 0, 0, VK_API_VERSION_1_1};
    struct vk_instance_info info;
    void *lib = dlopen("libvulkan.so.1", RTLD_NOW), *sym;
    VkResult r;

    if (!lib) {
        die("no libvulkan.so.1", 0);
    }
    sym = dlsym(lib, "vkGetInstanceProcAddr");
    memcpy(&vk.GetInstanceProcAddr, &sym, sizeof(sym));
    if (!vk.GetInstanceProcAddr) {
        die("no vkGetInstanceProcAddr", 0);
    }
    LOAD(CreateInstance, "vkCreateInstance");
    memset(&info, 0, sizeof(info));
    info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    info.pApplicationInfo = &app;
    if ((r = vk.CreateInstance(&info, 0, &g.instance)) != VK_SUCCESS) {
        die("vkCreateInstance", r);
    }
    LOAD(EnumeratePhysicalDevices, "vkEnumeratePhysicalDevices");
    LOAD(GetPhysicalDeviceProperties, "vkGetPhysicalDeviceProperties");
    LOAD(GetPhysicalDeviceMemoryProperties, "vkGetPhysicalDeviceMemoryProperties");
    LOAD(GetPhysicalDeviceQueueFamilyProperties, "vkGetPhysicalDeviceQueueFamilyProperties");
    LOAD(CreateDevice, "vkCreateDevice");
    LOAD(GetDeviceQueue, "vkGetDeviceQueue");
    LOAD(CreateBuffer, "vkCreateBuffer");
    LOAD(DestroyBuffer, "vkDestroyBuffer");
    LOAD(GetBufferMemoryRequirements, "vkGetBufferMemoryRequirements");
    LOAD(AllocateMemory, "vkAllocateMemory");
    LOAD(FreeMemory, "vkFreeMemory");
    LOAD(BindBufferMemory, "vkBindBufferMemory");
    LOAD(MapMemory, "vkMapMemory");
    LOAD(UnmapMemory, "vkUnmapMemory");
    LOAD(CreateCommandPool, "vkCreateCommandPool");
    LOAD(AllocateCommandBuffers, "vkAllocateCommandBuffers");
    LOAD(BeginCommandBuffer, "vkBeginCommandBuffer");
    LOAD(EndCommandBuffer, "vkEndCommandBuffer");
    LOAD(CmdCopyBuffer, "vkCmdCopyBuffer");
    LOAD(CmdFillBuffer, "vkCmdFillBuffer");
    LOAD(CmdPipelineBarrier, "vkCmdPipelineBarrier");
    LOAD(CreateFence, "vkCreateFence");
    LOAD(ResetFences, "vkResetFences");
    LOAD(WaitForFences, "vkWaitForFences");
    LOAD(QueueSubmit, "vkQueueSubmit");
}

/** Take the first device, a queue that copies, the first host-visible
 * and coherent memory type, a command buffer and a fence. */
static void open_device(void)
{
    static const float priority = 1.0f;
    struct vk_memory_properties mem;
    struct vk_family fam[16];
    struct vk_queue_info q;
    struct vk_device_info d;
    struct vk_pool_info pool = {VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, 0,
                                VK_COMMAND_POOL_RESET_BUFFER_BIT, 0};
    struct vk_command_info ci = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, 0, 0,
                                 VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1};
    struct vk_fence_info fi = {VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, 0, 0};
    uint64_t props[1024]; /* VkPhysicalDeviceProperties, 824 bytes */
    uint32_t n = 1, i,
             want = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
    VkResult r;

    if ((r = vk.EnumeratePhysicalDevices(g.instance, &n, &g.gpu)) < 0 || n == 0) {
        die("no Vulkan device", r);
    }
    vk.GetPhysicalDeviceProperties(g.gpu, props);
    memcpy(g.name, (const char *)props + 20, sizeof(g.name) - 1);
    n = 16;
    vk.GetPhysicalDeviceQueueFamilyProperties(g.gpu, &n, fam);
    for (g.family = 0; g.family < n; g.family++) {
        if (fam[g.family].queueFlags &
            (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)) {
            break;
        }
    }
    if (g.family == n) {
        die("no queue that copies", 0);
    }
    vk.GetPhysicalDeviceMemoryProperties(g.gpu, &mem);
    for (i = 0; i < mem.memoryTypeCount && (mem.memoryTypes[i].propertyFlags & want) != want; i++) {
    }
    if (i == mem.memoryTypeCount) {
        die("no host-visible and coherent memory", 0);
    }
    g.type = i;
    memset(&q, 0, sizeof(q));
    q.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    q.queueFamilyIndex = g.family;
    q.queueCount = 1;
    q.pQueuePriorities = &priority;
    memset(&d, 0, sizeof(d));
    d.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    d.queueCreateInfoCount = 1;
    d.pQueueCreateInfos = &q;
    if ((r = vk.CreateDevice(g.gpu, &d, 0, &g.dev)) != VK_SUCCESS) {
        die("vkCreateDevice", r);
    }
    vk.GetDeviceQueue(g.dev, g.family, 0, &g.queue);
    pool.queueFamilyIndex = g.family;
    if ((r = vk.CreateCommandPool(g.dev, &pool, 0, &g.pool)) != VK_SUCCESS) {
        die("vkCreateCommandPool", r);
    }
    ci.commandPool = g.pool;
    if ((r = vk.AllocateCommandBuffers(g.dev, &ci, &g.cmd)) != VK_SUCCESS) {
        die("vkAllocateCommandBuffers", r);
    }
    if ((r = vk.CreateFence(g.dev, &fi, 0, &g.fence)) != VK_SUCCESS) {
        die("vkCreateFence", r);
    }
    printf("NOCOPY DEVICE: %s, memory type %u\n", g.name, (unsigned)g.type);
}

/** Make a buffer of `bytes` in the host-visible memory type and map it. */
static void buf_make(struct buf *b, uint64_t bytes)
{
    struct vk_buffer_info bi;
    struct vk_requirements req;
    struct vk_allocate_info ai = {VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, 0, 0, 0};
    VkResult r;

    memset(&bi, 0, sizeof(bi));
    bi.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if ((r = vk.CreateBuffer(g.dev, &bi, 0, &b->buffer)) != VK_SUCCESS) {
        die("vkCreateBuffer", r);
    }
    vk.GetBufferMemoryRequirements(g.dev, b->buffer, &req);
    if (!(req.memoryTypeBits & (1u << g.type))) {
        die("the buffer cannot use the host-visible memory type", (long)req.memoryTypeBits);
    }
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = g.type;
    if ((r = vk.AllocateMemory(g.dev, &ai, 0, &b->memory)) != VK_SUCCESS) {
        die("vkAllocateMemory", r);
    }
    if ((r = vk.BindBufferMemory(g.dev, b->buffer, b->memory, 0)) != VK_SUCCESS) {
        die("vkBindBufferMemory", r);
    }
    if ((r = vk.MapMemory(g.dev, b->memory, 0, req.size, 0, (void **)&b->map)) != VK_SUCCESS) {
        die("vkMapMemory", r);
    }
    b->bytes = bytes;
}

/** Unmap and free a buffer. */
static void buf_free(struct buf *b)
{
    vk.UnmapMemory(g.dev, b->memory);
    vk.DestroyBuffer(g.dev, b->buffer, 0);
    vk.FreeMemory(g.dev, b->memory, 0);
    memset(b, 0, sizeof(*b));
}

/** Record `dst` filled with `fill`, or `src` copied to `dst` when `src` is
 * set, then the transfer made visible to the host; submit it, and wait
 * on its fence. */
static void gpu_step(struct buf *src, struct buf *dst, uint32_t fill)
{
    struct vk_begin_info bi = {VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, 0,
                               VK_COMMAND_BUFFER_ONE_TIME_BIT, 0};
    struct vk_barrier in = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_HOST_WRITE_BIT,
                            VK_ACCESS_TRANSFER_READ_BIT | VK_ACCESS_TRANSFER_WRITE_BIT};
    struct vk_barrier out = {VK_STRUCTURE_TYPE_MEMORY_BARRIER, 0, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_ACCESS_HOST_READ_BIT};
    struct vk_submit_info si;
    struct vk_copy c = {0, 0, 0};
    VkResult r;

    if ((r = vk.BeginCommandBuffer(g.cmd, &bi)) != VK_SUCCESS) {
        die("vkBeginCommandBuffer", r);
    }
    vk.CmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1,
                          &in, 0, 0, 0, 0);
    if (src) {
        c.size = dst->bytes;
        vk.CmdCopyBuffer(g.cmd, src->buffer, dst->buffer, 1, &c);
    } else {
        vk.CmdFillBuffer(g.cmd, dst->buffer, 0, dst->bytes, fill);
    }
    vk.CmdPipelineBarrier(g.cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1,
                          &out, 0, 0, 0, 0);
    if ((r = vk.EndCommandBuffer(g.cmd)) != VK_SUCCESS) {
        die("vkEndCommandBuffer", r);
    }
    memset(&si, 0, sizeof(si));
    si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
    si.commandBufferCount = 1;
    si.pCommandBuffers = &g.cmd;
    if ((r = vk.ResetFences(g.dev, 1, &g.fence)) != VK_SUCCESS ||
        (r = vk.QueueSubmit(g.queue, 1, &si, g.fence)) != VK_SUCCESS) {
        die("vkQueueSubmit", r);
    }
    if ((r = vk.WaitForFences(g.dev, 1, &g.fence, 1, UINT64_MAX)) != VK_SUCCESS) {
        die("vkWaitForFences", r);
    }
}

/** The words of round `r` at word `i`, which no other round writes. */
static uint32_t pattern(uint32_t r, uint32_t i)
{
    return (r << 24) ^ (i * 2654435761u) ^ 0x5a000000u;
}

/** `rounds` rounds of a pattern copied by the GPU from one mapped buffer
 * to the other and a value filled into the first, each read back. */
static int rounds(uint32_t n, uint64_t bytes)
{
    struct buf a, b;
    uint32_t r, i, words = (uint32_t)(bytes / 4), bad = 0, filled = 0;
    long maps, pages;
    double t0;

    buf_make(&a, bytes);
    buf_make(&b, bytes);
    maps = stat_count("kvm_gpa_maps");
    pages = stat_count("kvm_gpa_map_pages");
    printf("NOCOPY MAPPED: kvm_gpa_maps %ld, kvm_gpa_map_pages %ld\n", maps, pages);
    t0 = now_us();
    for (r = 0; r < n; r++) {
        for (i = 0; i < words; i++) {
            a.map[i] = pattern(r, i);
        }
        gpu_step(&a, &b, 0);
        for (i = 0; i < words; i++) {
            bad += b.map[i] != pattern(r, i);
        }
        gpu_step(0, &a, 0xc0000000u | r);
        for (i = 0; i < words; i++) {
            filled += a.map[i] != (0xc0000000u | r);
        }
    }
    printf("NOCOPY ROUNDS: %u rounds of %llu bytes in %.0f us, %u words copied wrong, "
           "%u filled wrong\n",
           (unsigned)n, (unsigned long long)bytes, now_us() - t0, (unsigned)bad, (unsigned)filled);
    buf_free(&a);
    buf_free(&b);
    if (maps < 1 || pages < (long)(2 * bytes / 4096)) {
        die("the buffers are not mapped from the VM's pages", maps);
    }
    return bad == 0 && filled == 0;
}

/** `n` buffers made, mapped, touched and freed one after another: the
 * mean, first and largest time to make, map and touch, and to free, and
 * the most page faults one buffer's touch took, which a mapping that maps
 * every page at its first fault keeps at one. */
static void serial(uint32_t n, uint64_t bytes)
{
    double make = 0, first = 0, most = 0, freed = 0, t0, t1, t2;
    struct rusage r0, r1;
    long faults = 0;
    struct buf b;
    uint32_t k;
    uint64_t i;

    for (k = 0; k < n; k++) {
        t0 = now_us();
        buf_make(&b, bytes);
        getrusage(RUSAGE_THREAD, &r0);
        for (i = 0; i < bytes / 4; i += 1024) {
            b.map[i] = (uint32_t)i;
        }
        getrusage(RUSAGE_THREAD, &r1);
        t1 = now_us();
        if (r1.ru_minflt + r1.ru_majflt - r0.ru_minflt - r0.ru_majflt > faults) {
            faults = r1.ru_minflt + r1.ru_majflt - r0.ru_minflt - r0.ru_majflt;
        }
        buf_free(&b);
        t2 = now_us();
        first = k == 0 ? t1 - t0 : first;
        most = t1 - t0 > most ? t1 - t0 : most;
        make += t1 - t0;
        freed += t2 - t1;
    }
    printf("NOCOPY SERIAL: %u of %llu bytes: make, map and touch mean %.0f us, first %.0f, "
           "largest %.0f; unmap and free mean %.0f us; touch faults most %ld\n",
           (unsigned)n, (unsigned long long)bytes, make / n, first, most, freed / n, faults);
}

/** `n` buffers held at once, each made, mapped and touched; then freed
 * together; then one more made. */
static void held(uint32_t n, uint64_t bytes)
{
    struct buf *b = calloc(n, sizeof(*b));
    double make = 0, first = 0, most = 0, t0, t1;
    uint32_t k;

    if (!b) {
        die("no memory", 0);
    }
    for (k = 0; k < n; k++) {
        t0 = now_us();
        buf_make(&b[k], bytes);
        b[k].map[0] = k;
        t1 = now_us();
        first = k == 0 ? t1 - t0 : first;
        most = t1 - t0 > most ? t1 - t0 : most;
        make += t1 - t0;
    }
    printf("NOCOPY HELD-MAPS: kvm_gpa_maps %ld, kvm_gpa_map_pages %ld\n",
           stat_count("kvm_gpa_maps"), stat_count("kvm_gpa_map_pages"));
    t0 = now_us();
    for (k = 0; k < n; k++) {
        buf_free(&b[k]);
    }
    t1 = now_us();
    printf("NOCOPY HELD: %u of %llu bytes: make, map and touch mean %.0f us, first %.0f, "
           "largest %.0f; all unmapped and freed in %.0f us\n",
           (unsigned)n, (unsigned long long)bytes, make / n, first, most, t1 - t0);
    t0 = now_us();
    buf_make(&b[0], bytes);
    b[0].map[0] = 1;
    t1 = now_us();
    buf_free(&b[0]);
    printf("NOCOPY NEXT: one more of %llu bytes made, mapped and touched in %.0f us\n",
           (unsigned long long)bytes, t1 - t0);
    free(b);
}

/** The CPU's write and read bandwidth through a mapping of `bytes`, and
 * through the program's own memory, in MB/s; and a GPU copy of it. */
static void bandwidth(uint64_t bytes)
{
    struct buf a, b;
    uint32_t *own = malloc(bytes), sum = 0;
    double t0, w, rd, ow, ord, cp;
    uint64_t i, words = bytes / 4;

    if (!own) {
        die("no memory", 0);
    }
    buf_make(&a, bytes);
    buf_make(&b, bytes);
    memset(a.map, 1, bytes);
    memset(own, 1, bytes);
    t0 = now_us();
    memset(a.map, 2, bytes);
    w = now_us() - t0;
    t0 = now_us();
    for (i = 0; i < words; i++) {
        sum += a.map[i];
    }
    rd = now_us() - t0;
    t0 = now_us();
    memset(own, 2, bytes);
    ow = now_us() - t0;
    t0 = now_us();
    for (i = 0; i < words; i++) {
        sum += own[i];
    }
    ord = now_us() - t0;
    t0 = now_us();
    gpu_step(&a, &b, 0);
    cp = now_us() - t0;
    printf("NOCOPY BANDWIDTH: %llu bytes: mapped write %.0f MB/s, read %.0f MB/s; own write "
           "%.0f MB/s, read %.0f MB/s; GPU copy %.0f us (%u)\n",
           (unsigned long long)bytes, bytes / w, bytes / rd, bytes / ow, bytes / ord, cp,
           (unsigned)(sum & 1));
    buf_free(&a);
    buf_free(&b);
    free(own);
}

/** SIGBUS while holding: the mapping was taken back. */
static void on_bus(int sig)
{
    (void)sig;
    if (holding) {
        siglongjmp(touched, 1);
    }
    _exit(3);
}

/** Map a buffer and touch it until the mapping is taken back. */
static int hold(uint64_t bytes)
{
    static struct buf b;
    struct sigaction sa;
    volatile uint64_t touches = 0;
    uint64_t i, words = bytes / 4;

    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_bus;
    sigaction(SIGBUS, &sa, 0);
    buf_make(&b, bytes);
    for (i = 0; i < words; i++) {
        b.map[i] = pattern(7, (uint32_t)i);
    }
    printf("NOCOPY HELD: %llu bytes, kvm_gpa_maps %ld, kvm_gpa_map_pages %ld\n",
           (unsigned long long)bytes, stat_count("kvm_gpa_maps"), stat_count("kvm_gpa_map_pages"));
    fflush(stdout);
    if (sigsetjmp(touched, 1)) {
        holding = 0;
        printf("NOCOPY SIGBUS: after %llu touches, kvm_gpa_maps %ld, kvm_gpa_map_pages %ld\n"
               "NOCOPY OK\n",
               (unsigned long long)touches, stat_count("kvm_gpa_maps"),
               stat_count("kvm_gpa_map_pages"));
        fflush(stdout);
        _exit(0); /* the device is lost: no teardown of Vulkan's */
    }
    holding = 1;
    for (;;) {
        for (i = 0; i < words; i += 1024) {
            if (b.map[i] != pattern(7, (uint32_t)i)) {
                holding = 0;
                printf("NOCOPY FAIL: word %llu changed under the mapping\n", (unsigned long long)i);
                return 0;
            }
        }
        touches++;
    }
}

/**
 * Run one mode, as the head of this file gives them.
 */
int main(int argc, char **argv)
{
    uint64_t bytes = argc > 2 ? strtoull(argv[argc - 1], 0, 0) : 0;
    uint32_t n = argc > 3 ? (uint32_t)strtoul(argv[2], 0, 0) : 0;
    int ok = 1;

    setvbuf(stdout, 0, _IOLBF, 0);
    if (argc < 3 || bytes == 0 || bytes % 4096) {
        fprintf(stderr, "usage: nocopy rounds|serial|held N BYTES | bandwidth|hold BYTES\n");
        return 2;
    }
    open_instance();
    open_device();
    if (strcmp(argv[1], "rounds") == 0 && n) {
        ok = rounds(n, bytes);
    } else if (strcmp(argv[1], "serial") == 0 && n) {
        serial(n, bytes);
    } else if (strcmp(argv[1], "held") == 0 && n) {
        held(n, bytes);
    } else if (strcmp(argv[1], "bandwidth") == 0) {
        bandwidth(bytes);
    } else if (strcmp(argv[1], "hold") == 0) {
        ok = hold(bytes);
    } else {
        fprintf(stderr, "nocopy: unknown mode: %s\n", argv[1]);
        return 2;
    }
    printf(ok ? "NOCOPY OK\n" : "NOCOPY FAIL: see above\n");
    return ok ? 0 : 1;
}
