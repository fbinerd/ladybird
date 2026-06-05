/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Format.h>
#include <AK/Vector.h>
#include <LibGfx/VulkanContext.h>

namespace Gfx {

#if VULKAN_VALIDATION_LAYERS_DEBUG
static void setup_vulkan_validation_layers_callback(VkInstanceCreateInfo& create_info, VkDebugUtilsMessengerCreateInfoEXT& debug_messenger_create_info);
static void enable_vulkan_validation_layers_callback(VkInstance const& instance, VkDebugUtilsMessengerCreateInfoEXT const& debug_messenger_create_info);
#endif

static ErrorOr<VkInstance> create_instance(uint32_t api_version)
{
    VkInstance instance;

    VkApplicationInfo app_info {};
    app_info.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    app_info.pApplicationName = "Ladybird";
    app_info.applicationVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.pEngineName = nullptr;
    app_info.engineVersion = VK_MAKE_VERSION(1, 0, 0);
    app_info.apiVersion = api_version;

    VkInstanceCreateInfo create_info {};
    create_info.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    create_info.pApplicationInfo = &app_info;

#if VULKAN_VALIDATION_LAYERS_DEBUG
    VkDebugUtilsMessengerCreateInfoEXT debug_messenger_create_info {};
    setup_vulkan_validation_layers_callback(create_info, debug_messenger_create_info);
#endif

    auto result = vkCreateInstance(&create_info, nullptr, &instance);
    if (result != VK_SUCCESS) {
        dbgln("vkCreateInstance returned {}", to_underlying(result));
        return Error::from_string_literal("Application instance creation failed");
    }

#if VULKAN_VALIDATION_LAYERS_DEBUG
    enable_vulkan_validation_layers_callback(instance, debug_messenger_create_info);
#endif

    return instance;
}

static ErrorOr<VkPhysicalDevice> pick_physical_device(VkInstance instance)
{
    uint32_t device_count = 0;
    vkEnumeratePhysicalDevices(instance, &device_count, nullptr);

    if (device_count == 0)
        return Error::from_string_literal("Can't find any physical devices available");

    Vector<VkPhysicalDevice> devices;
    devices.resize(device_count);
    vkEnumeratePhysicalDevices(instance, &device_count, devices.data());

    VkPhysicalDevice picked_device = VK_NULL_HANDLE;
    // Pick discrete GPU or the first device in the list
    for (auto const& device : devices) {
        if (picked_device == VK_NULL_HANDLE)
            picked_device = device;

        VkPhysicalDeviceProperties device_properties;
        vkGetPhysicalDeviceProperties(device, &device_properties);
        if (device_properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            picked_device = device;
    }

    if (picked_device != VK_NULL_HANDLE)
        return picked_device;

    VERIFY_NOT_REACHED();
}

struct VulkanDeviceFeatureProbe {
    bool sampler_ycbcr_conversion_supported { false };
    bool nv12_ycbcr_sampling_supported { false };
};

static VulkanDeviceFeatureProbe probe_vulkan_device_features(VkPhysicalDevice physical_device)
{
    VkPhysicalDeviceSamplerYcbcrConversionFeatures sampler_ycbcr_features {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
        .pNext = nullptr,
        .samplerYcbcrConversion = VK_FALSE,
    };
    VkPhysicalDeviceFeatures2 features {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2,
        .pNext = &sampler_ycbcr_features,
        .features = {},
    };
    vkGetPhysicalDeviceFeatures2(physical_device, &features);

    VkFormatProperties2 nv12_format_properties {
        .sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2,
        .pNext = nullptr,
        .formatProperties = {},
    };
    vkGetPhysicalDeviceFormatProperties2(physical_device, VK_FORMAT_G8_B8R8_2PLANE_420_UNORM, &nv12_format_properties);
    auto optimal_features = nv12_format_properties.formatProperties.optimalTilingFeatures;
    auto nv12_sampled = (optimal_features & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) != 0;
    auto nv12_ycbcr = (optimal_features & (VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_LINEAR_FILTER_BIT
                               | VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_SEPARATE_RECONSTRUCTION_FILTER_BIT
                               | VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_CHROMA_RECONSTRUCTION_EXPLICIT_BIT
                               | VK_FORMAT_FEATURE_SAMPLED_IMAGE_YCBCR_CONVERSION_CHROMA_RECONSTRUCTION_EXPLICIT_FORCEABLE_BIT))
        != 0;

    dbgln("MUNDO_VULKAN_VIDEO_YCBCR_CAPS sampler_ycbcr_conversion={} nv12_sampled={} nv12_ycbcr_features={} optimal_features={} linear_features={}",
        sampler_ycbcr_features.samplerYcbcrConversion,
        nv12_sampled,
        nv12_ycbcr,
        static_cast<unsigned long long>(optimal_features),
        static_cast<unsigned long long>(nv12_format_properties.formatProperties.linearTilingFeatures));

    return VulkanDeviceFeatureProbe {
        .sampler_ycbcr_conversion_supported = sampler_ycbcr_features.samplerYcbcrConversion == VK_TRUE,
        .nv12_ycbcr_sampling_supported = sampler_ycbcr_features.samplerYcbcrConversion == VK_TRUE && nv12_sampled && nv12_ycbcr,
    };
}

static ErrorOr<VkDevice> create_logical_device(VkPhysicalDevice physical_device, uint32_t* graphics_queue_family, VulkanDeviceFeatureProbe const& feature_probe)
{
    VkDevice device;

    uint32_t queue_family_count = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, nullptr);
    Vector<VkQueueFamilyProperties> queue_families;
    queue_families.resize(queue_family_count);
    vkGetPhysicalDeviceQueueFamilyProperties(physical_device, &queue_family_count, queue_families.data());

    bool graphics_queue_family_found = false;
    uint32_t graphics_queue_family_index = 0;
    for (; graphics_queue_family_index < queue_families.size(); ++graphics_queue_family_index) {
        if (queue_families[graphics_queue_family_index].queueFlags & VK_QUEUE_GRAPHICS_BIT) {
            graphics_queue_family_found = true;
            break;
        }
    }

    if (!graphics_queue_family_found) {
        return Error::from_string_literal("Graphics queue family not found");
    }

    *graphics_queue_family = graphics_queue_family_index;

    VkDeviceQueueCreateInfo queue_create_info {};
    queue_create_info.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queue_create_info.queueFamilyIndex = graphics_queue_family_index;
    queue_create_info.queueCount = 1;

    float const queue_priority = 1.0f;
    queue_create_info.pQueuePriorities = &queue_priority;

    VkPhysicalDeviceFeatures deviceFeatures {};
    VkPhysicalDeviceSamplerYcbcrConversionFeatures sampler_ycbcr_features {
        .sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SAMPLER_YCBCR_CONVERSION_FEATURES,
        .pNext = nullptr,
        .samplerYcbcrConversion = feature_probe.sampler_ycbcr_conversion_supported ? VK_TRUE : VK_FALSE,
    };
#ifdef USE_VULKAN_DMABUF_IMAGES
    Array<char const*, 4> device_extensions = {
        VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
        VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
        VK_KHR_IMAGE_FORMAT_LIST_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
    };
#else
    Array<char const*, 0> device_extensions;
#endif
    VkDeviceCreateInfo create_device_info {};
    create_device_info.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    create_device_info.pNext = &sampler_ycbcr_features;
    create_device_info.pQueueCreateInfos = &queue_create_info;
    create_device_info.queueCreateInfoCount = 1;
    create_device_info.pEnabledFeatures = &deviceFeatures;
    create_device_info.enabledExtensionCount = device_extensions.size();
    create_device_info.ppEnabledExtensionNames = device_extensions.data();

    if (vkCreateDevice(physical_device, &create_device_info, nullptr, &device) != VK_SUCCESS) {
        return Error::from_string_literal("Logical device creation failed");
    }

    return device;
}

#ifdef USE_VULKAN_DMABUF_IMAGES
static ErrorOr<VkCommandPool> create_command_pool(VkDevice logical_device, uint32_t queue_family_index)
{
    VkCommandPoolCreateInfo command_pool_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
        .queueFamilyIndex = queue_family_index,
    };
    VkCommandPool command_pool = VK_NULL_HANDLE;
    VkResult result = vkCreateCommandPool(logical_device, &command_pool_info, nullptr, &command_pool);
    if (result != VK_SUCCESS) {
        dbgln("vkCreateCommandPool returned {}", to_underlying(result));
        return Error::from_string_literal("command pool creation failed");
    }
    return command_pool;
}

static ErrorOr<VkCommandBuffer> allocate_command_buffer(VkDevice logical_device, VkCommandPool commandPool)
{
    VkCommandBufferAllocateInfo command_buffer_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
        .pNext = nullptr,
        .commandPool = commandPool,
        .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
        .commandBufferCount = 1,
    };
    VkCommandBuffer command_buffer = VK_NULL_HANDLE;
    VkResult result = vkAllocateCommandBuffers(logical_device, &command_buffer_alloc_info, &command_buffer);
    if (result != VK_SUCCESS) {
        dbgln("vkAllocateCommandBuffers returned {}", to_underlying(result));
        return Error::from_string_literal("command buffer allocation failed");
    }
    return command_buffer;
}

static ErrorOr<VkFence> create_command_fence(VkDevice logical_device)
{
    VkFenceCreateInfo fence_info {
        .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
    };
    VkFence fence = VK_NULL_HANDLE;
    VkResult result = vkCreateFence(logical_device, &fence_info, nullptr, &fence);
    if (result != VK_SUCCESS) {
        dbgln("vkCreateFence returned {}", to_underlying(result));
        return Error::from_string_literal("command fence creation failed");
    }
    return fence;
}
#endif

ErrorOr<VulkanContext> create_vulkan_context()
{
    uint32_t const api_version = VK_API_VERSION_1_1; // v1.1 needed for vkGetPhysicalDeviceFormatProperties2
    auto* instance = TRY(create_instance(api_version));
    auto* physical_device = TRY(pick_physical_device(instance));
    auto feature_probe = probe_vulkan_device_features(physical_device);

    uint32_t graphics_queue_family = 0;
    auto* logical_device = TRY(create_logical_device(physical_device, &graphics_queue_family, feature_probe));
    VkQueue graphics_queue;
    vkGetDeviceQueue(logical_device, graphics_queue_family, 0, &graphics_queue);

#ifdef USE_VULKAN_DMABUF_IMAGES
    VkCommandPool command_pool = TRY(create_command_pool(logical_device, graphics_queue_family));
    VkCommandBuffer command_buffer = TRY(allocate_command_buffer(logical_device, command_pool));
    VkFence command_fence = TRY(create_command_fence(logical_device));

    auto pfn_vk_get_memory_fd_khr = reinterpret_cast<PFN_vkGetMemoryFdKHR>(vkGetDeviceProcAddr(logical_device, "vkGetMemoryFdKHR"));
    if (pfn_vk_get_memory_fd_khr == nullptr) {
        return Error::from_string_literal("vkGetMemoryFdKHR unavailable");
    }
    auto pfn_vk_get_memory_fd_properties_khr = reinterpret_cast<PFN_vkGetMemoryFdPropertiesKHR>(vkGetDeviceProcAddr(logical_device, "vkGetMemoryFdPropertiesKHR"));
    if (pfn_vk_get_memory_fd_properties_khr == nullptr) {
        return Error::from_string_literal("vkGetMemoryFdPropertiesKHR unavailable");
    }
    auto pfn_vk_get_image_drm_format_modifier_properties_khr = reinterpret_cast<PFN_vkGetImageDrmFormatModifierPropertiesEXT>(vkGetDeviceProcAddr(logical_device, "vkGetImageDrmFormatModifierPropertiesEXT"));
    if (pfn_vk_get_image_drm_format_modifier_properties_khr == nullptr) {
        return Error::from_string_literal("vkGetImageDrmFormatModifierPropertiesEXT unavailable");
    }
#endif

    return VulkanContext {
        .api_version = api_version,
        .instance = instance,
        .physical_device = physical_device,
        .logical_device = logical_device,
        .graphics_queue = graphics_queue,
        .graphics_queue_family = graphics_queue_family,
        .sampler_ycbcr_conversion_supported = feature_probe.sampler_ycbcr_conversion_supported,
        .nv12_ycbcr_sampling_supported = feature_probe.nv12_ycbcr_sampling_supported,
#ifdef USE_VULKAN_DMABUF_IMAGES
        .command_pool = command_pool,
        .command_buffer = command_buffer,
        .command_fence = command_fence,
        .ext_procs = {
            .get_memory_fd = pfn_vk_get_memory_fd_khr,
            .get_memory_fd_properties = pfn_vk_get_memory_fd_properties_khr,
            .get_image_drm_format_modifier_properties = pfn_vk_get_image_drm_format_modifier_properties_khr,
        },
#endif
    };
}

#if VULKAN_VALIDATION_LAYERS_DEBUG
static bool check_layer_support(StringView layer_name)
{
    uint32_t layer_count = 0;
    vkEnumerateInstanceLayerProperties(&layer_count, nullptr);

    Vector<VkLayerProperties> layers;
    layers.resize(layer_count);
    vkEnumerateInstanceLayerProperties(&layer_count, layers.data());

    return layers.contains([&layer_name](auto const& layer_properties) {
        return layer_properties.layerName == layer_name;
    });
}

static constexpr Array<char const*, 1> vvl_layers = { "VK_LAYER_KHRONOS_validation" };
static constexpr Array<char const*, 1> vvl_extensions = { VK_EXT_DEBUG_UTILS_EXTENSION_NAME };

static void setup_vulkan_validation_layers_callback(VkInstanceCreateInfo& create_info, VkDebugUtilsMessengerCreateInfoEXT& debug_messenger_create_info)
{
    if (!check_layer_support("VK_LAYER_KHRONOS_validation"sv))
        return;

    debug_messenger_create_info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    debug_messenger_create_info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    debug_messenger_create_info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    debug_messenger_create_info.pfnUserCallback = [](VkDebugUtilsMessageSeverityFlagBitsEXT,
                                                      VkDebugUtilsMessageTypeFlagsEXT,
                                                      VkDebugUtilsMessengerCallbackDataEXT const* pCallbackData,
                                                      void*) {
        dbgln("Vulkan validation layers: {}", pCallbackData->pMessage);
        dump_backtrace(3);
        return VK_FALSE;
    };

    create_info.enabledLayerCount = vvl_layers.size();
    create_info.ppEnabledLayerNames = vvl_layers.data();
    create_info.enabledExtensionCount = vvl_extensions.size();
    create_info.ppEnabledExtensionNames = vvl_extensions.data();
    create_info.pNext = &debug_messenger_create_info;
}

static void enable_vulkan_validation_layers_callback(VkInstance const& instance, VkDebugUtilsMessengerCreateInfoEXT const& debug_messenger_create_info)
{
    // Vulkan validation layers not available if struct not setup properly
    if (debug_messenger_create_info.sType != VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT) {
        dbgln("Vulkan validation layers: not available");
        return;
    }

    auto pfn_create_debug_messenger = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
    VkDebugUtilsMessengerEXT debug_messenger { VK_NULL_HANDLE };
    auto result = pfn_create_debug_messenger(instance, &debug_messenger_create_info, nullptr, &debug_messenger);
    if (result != VK_SUCCESS)
        dbgln("vkCreateDebugUtilsMessengerEXT returned {}", to_underlying(result));
    else
        dbgln("Vulkan validation layers: active");
}
#endif

}
