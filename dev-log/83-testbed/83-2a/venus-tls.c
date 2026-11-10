/*
 * venus-tls.c: show that a thread which made a device on Mesa's Venus driver
 * faults when it ends after the driver was unloaded (../upstream.md).
 *
 *   cc -O1 -o venus-tls venus-tls.c -ldl -lpthread
 *   VK_DRIVER_FILES=/usr/share/vulkan/icd.d/venus_icd.x86_64.json ./venus-tls [device]
 *
 * A second thread loads the Vulkan loader, creates an instance, names the
 * first device, with an argument creates and destroys a device on it, and
 * destroys the instance, printing how many mappings of the driver the
 * process has before and after. The structures are Vulkan's, written out so
 * that the program needs no headers.
 */
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* VkInstanceCreateInfo, VkDeviceQueueCreateInfo and VkDeviceCreateInfo. */
struct instance_info {
    uint32_t type; /* VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, 1 */
    const void *next;
    uint32_t flags;
    const void *application;
    uint32_t layer_count;
    const char *const *layers;
    uint32_t extension_count;
    const char *const *extensions;
};

struct queue_info {
    uint32_t type; /* VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, 2 */
    const void *next;
    uint32_t flags;
    uint32_t family;
    uint32_t count;
    const float *priorities;
};

struct device_info {
    uint32_t type; /* VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, 3 */
    const void *next;
    uint32_t flags;
    uint32_t queue_count;
    const struct queue_info *queues;
    uint32_t layer_count;
    const char *const *layers;
    uint32_t extension_count;
    const char *const *extensions;
    const void *features;
};

static void *loader;    /* libvulkan.so.1 */
static int with_device; /* create a device too */

/** Print how many mappings of the Venus driver the process has. */
static void mappings(const char *when)
{
    char line[512];
    FILE *f = fopen("/proc/self/maps", "r");
    int n = 0;

    while (f != NULL && fgets(line, sizeof(line), f) != NULL)
        n += strstr(line, "libvulkan_virtio") != NULL;
    if (f != NULL)
        fclose(f);
    printf("%s: %d mappings of libvulkan_virtio\n", when, n);
    fflush(stdout);
}

/** The second thread: an instance, perhaps a device, and their end. */
static void *run(void *arg)
{
    int (*create)(const struct instance_info *, const void *, void **) =
        dlsym(loader, "vkCreateInstance");
    int (*enumerate)(void *, uint32_t *, void **) = dlsym(loader, "vkEnumeratePhysicalDevices");
    void (*properties)(void *, void *) = dlsym(loader, "vkGetPhysicalDeviceProperties");
    void (*destroy)(void *, const void *) = dlsym(loader, "vkDestroyInstance");
    struct instance_info info = {1, NULL, 0, NULL, 0, NULL, 0, NULL};
    void *instance = NULL, *devices[8];
    uint32_t n = 8;
    static char text[4096]; /* VkPhysicalDeviceProperties, its name at 20 */
    int rc = create(&info, NULL, &instance);

    (void)arg;
    printf("create %d\n", rc);
    if (rc != 0)
        return NULL;
    rc = enumerate(instance, &n, devices);
    printf("enumerate %d, %u devices\n", rc, n);
    if (rc == 0 && n > 0) {
        properties(devices[0], text);
        printf("device %s\n", text + 20);
    }
    if (rc == 0 && n > 0 && with_device) {
        int (*make)(void *, const struct device_info *, const void *, void **) =
            dlsym(loader, "vkCreateDevice");
        void (*unmake)(void *, const void *) = dlsym(loader, "vkDestroyDevice");
        static const float priority = 1.0f;
        struct queue_info queue = {2, NULL, 0, 0, 1, &priority};
        struct device_info dinfo = {3, NULL, 0, 1, &queue, 0, NULL, 0, NULL, NULL};
        void *device = NULL;

        rc = make(devices[0], &dinfo, NULL, &device);
        printf("device create %d\n", rc);
        if (rc == 0)
            unmake(device, NULL);
    }
    mappings("before the destroy");
    destroy(instance, NULL);
    mappings("after the destroy");
    return NULL;
}

int main(int argc, char **argv)
{
    pthread_t thread;

    (void)argv;
    with_device = argc > 1;
    loader = dlopen("libvulkan.so.1", RTLD_NOW);
    if (loader == NULL) {
        fprintf(stderr, "venus-tls: %s\n", dlerror());
        return 2;
    }
    pthread_create(&thread, NULL, run, NULL);
    pthread_join(thread, NULL);
    printf("the thread ended\n");
    return 0;
}
