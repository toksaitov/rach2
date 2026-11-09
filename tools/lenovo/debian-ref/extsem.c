/* extsem.c: for each Vulkan device, the external semaphore and memory
 * features that Dawn's Vulkan backend asks of a device before Chromium may
 * hand it to a page (Dawn's SupportsExternalImages: opaque-fd semaphores
 * exportable and importable, VK_KHR_external_memory_fd and
 * VK_EXT_image_drm_format_modifier). Build: cc -o extsem extsem.c -lvulkan
 */
#include <stdio.h>
#include <string.h>
#include <vulkan/vulkan.h>

/**
 * Print whether a device has the named extension.
 *
 * @param pd the physical device
 * @param name the extension's name
 * @return 1 if it is listed, 0 if not
 */
static int has_ext(VkPhysicalDevice pd, const char *name)
{
    VkExtensionProperties e[512];
    uint32_t n = 512, i;

    vkEnumerateDeviceExtensionProperties(pd, NULL, &n, e);
    for (i = 0; i < n; i++)
        if (strcmp(e[i].extensionName, name) == 0)
            return 1;
    return 0;
}

/**
 * Print one semaphore handle type's external features.
 *
 * @param pd the physical device
 * @param t the handle type
 * @param tn its name, for the output
 */
static void sem(VkPhysicalDevice pd, VkExternalSemaphoreHandleTypeFlagBits t, const char *tn)
{
    VkPhysicalDeviceExternalSemaphoreInfo si;
    VkExternalSemaphoreProperties sp;

    memset(&si, 0, sizeof(si));
    memset(&sp, 0, sizeof(sp));
    si.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO;
    si.handleType = t;
    sp.sType = VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES;
    vkGetPhysicalDeviceExternalSemaphoreProperties(pd, &si, &sp);
    printf("  semaphore %s: features 0x%x (exportable %d, importable %d), "
           "export from 0x%x, compatible 0x%x\n",
           tn, sp.externalSemaphoreFeatures,
           !!(sp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT),
           !!(sp.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT),
           sp.exportFromImportedHandleTypes, sp.compatibleHandleTypes);
}

int main(void)
{
    VkApplicationInfo ai;
    VkInstanceCreateInfo ci;
    VkInstance in;
    VkPhysicalDevice pd[8];
    VkPhysicalDeviceProperties p;
    uint32_t n = 8, i;

    memset(&ai, 0, sizeof(ai));
    ai.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    ai.apiVersion = VK_API_VERSION_1_1;
    memset(&ci, 0, sizeof(ci));
    ci.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    ci.pApplicationInfo = &ai;
    if (vkCreateInstance(&ci, NULL, &in) != VK_SUCCESS) {
        fprintf(stderr, "extsem: vkCreateInstance failed\n");
        return 1;
    }
    vkEnumeratePhysicalDevices(in, &n, pd);
    for (i = 0; i < n; i++) {
        vkGetPhysicalDeviceProperties(pd[i], &p);
        printf("%s (0x%04x:0x%04x, type %d)\n", p.deviceName, p.vendorID, p.deviceID, p.deviceType);
        printf("  VK_KHR_external_semaphore_fd %d, "
               "VK_KHR_external_memory_fd %d, "
               "VK_EXT_image_drm_format_modifier %d, "
               "VK_EXT_external_memory_dma_buf %d\n",
               has_ext(pd[i], "VK_KHR_external_semaphore_fd"),
               has_ext(pd[i], "VK_KHR_external_memory_fd"),
               has_ext(pd[i], "VK_EXT_image_drm_format_modifier"),
               has_ext(pd[i], "VK_EXT_external_memory_dma_buf"));
        sem(pd[i], VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT, "OPAQUE_FD");
        sem(pd[i], VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_SYNC_FD_BIT, "SYNC_FD");
    }
    vkDestroyInstance(in, NULL);
    return 0;
}
