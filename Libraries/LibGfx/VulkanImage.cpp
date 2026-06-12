/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#ifdef USE_VULKAN_DMABUF_IMAGES

#    include <AK/Array.h>
#    include <AK/Format.h>
#    include <AK/ScopeGuard.h>
#    include <AK/Vector.h>
#    include <LibGfx/VulkanImage.h>
#    include <string.h>
#    include <unistd.h>

namespace Gfx {

static uint32_t find_memory_type_index(VkPhysicalDeviceMemoryProperties const& memory_properties, VkMemoryRequirements const& memory_requirements, VkMemoryPropertyFlags required_flags)
{
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        auto const property_flags = memory_properties.memoryTypes[i].propertyFlags;
        if ((memory_requirements.memoryTypeBits & (1u << i)) && (property_flags & required_flags) == required_flags)
            return i;
    }

    return memory_properties.memoryTypeCount;
}

static uint32_t find_memory_type_index(VkPhysicalDeviceMemoryProperties const& memory_properties, uint32_t memory_type_bits, VkMemoryPropertyFlags preferred_flags)
{
    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        auto const property_flags = memory_properties.memoryTypes[i].propertyFlags;
        if ((memory_type_bits & (1u << i)) && (property_flags & preferred_flags) == preferred_flags)
            return i;
    }

    for (uint32_t i = 0; i < memory_properties.memoryTypeCount; ++i) {
        if (memory_type_bits & (1u << i))
            return i;
    }

    return memory_properties.memoryTypeCount;
}

VulkanImage::~VulkanImage()
{
    if (cached_solid_mesh_framebuffer != VK_NULL_HANDLE)
        vkDestroyFramebuffer(context.logical_device, cached_solid_mesh_framebuffer, nullptr);
    if (cached_solid_mesh_color_attachment_view != VK_NULL_HANDLE)
        vkDestroyImageView(context.logical_device, cached_solid_mesh_color_attachment_view, nullptr);
    if (cached_video_framebuffer != VK_NULL_HANDLE)
        vkDestroyFramebuffer(context.logical_device, cached_video_framebuffer, nullptr);
    if (cached_video_color_attachment_view != VK_NULL_HANDLE)
        vkDestroyImageView(context.logical_device, cached_video_color_attachment_view, nullptr);
    if (image != VK_NULL_HANDLE) {
        vkDestroyImage(context.logical_device, image, nullptr);
    }
    if (memory != VK_NULL_HANDLE) {
        vkFreeMemory(context.logical_device, memory, nullptr);
    }
}

ImportedVulkanNV12Image::~ImportedVulkanNV12Image()
{
    if (owns_ycbcr_resources && ycbcr_sampler != VK_NULL_HANDLE)
        vkDestroySampler(context.logical_device, ycbcr_sampler, nullptr);
    if (ycbcr_image_view != VK_NULL_HANDLE)
        vkDestroyImageView(context.logical_device, ycbcr_image_view, nullptr);
    if (owns_ycbcr_resources && ycbcr_conversion != VK_NULL_HANDLE)
        vkDestroySamplerYcbcrConversion(context.logical_device, ycbcr_conversion, nullptr);
    if (image != VK_NULL_HANDLE)
        vkDestroyImage(context.logical_device, image, nullptr);
    if (memory != VK_NULL_HANDLE)
        vkFreeMemory(context.logical_device, memory, nullptr);
}

VulkanBuffer::~VulkanBuffer()
{
    if (buffer != VK_NULL_HANDLE)
        vkDestroyBuffer(context.logical_device, buffer, nullptr);
    if (memory != VK_NULL_HANDLE)
        vkFreeMemory(context.logical_device, memory, nullptr);
}

void VulkanImage::transition_layout(VkImageLayout old_layout, VkImageLayout new_layout)
{
    vkResetCommandBuffer(context.command_buffer, 0);
    VkCommandBufferBeginInfo begin_info = {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    vkBeginCommandBuffer(context.command_buffer, &begin_info);
    VkImageMemoryBarrier imageMemoryBarrier = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = 0,
        .dstAccessMask = 0,
        .oldLayout = old_layout,
        .newLayout = new_layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(context.command_buffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &imageMemoryBarrier);
    vkEndCommandBuffer(context.command_buffer);
    VkSubmitInfo submit_info = {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &context.command_buffer,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    vkQueueSubmit(context.graphics_queue, 1, &submit_info, nullptr);
    vkQueueWaitIdle(context.graphics_queue);
}

int VulkanImage::get_dma_buf_fd() const
{
    VkMemoryGetFdInfoKHR get_fd_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .pNext = nullptr,
        .memory = memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    int fd = -1;
    VkResult result = context.ext_procs.get_memory_fd(context.logical_device, &get_fd_info, &fd);
    if (result != VK_SUCCESS) {
        dbgln("vkGetMemoryFdKHR returned {}", to_underlying(result));
        return -1;
    }
    return fd;
}

int VulkanImage::get_opaque_fd() const
{
    VkMemoryGetFdInfoKHR get_fd_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR,
        .pNext = nullptr,
        .memory = memory,
        .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    int fd = -1;
    VkResult result = context.ext_procs.get_memory_fd(context.logical_device, &get_fd_info, &fd);
    if (result != VK_SUCCESS) {
        dbgln("vkGetMemoryFdKHR returned {} for OPAQUE_FD", to_underlying(result));
        return -1;
    }
    return fd;
}

ErrorOr<NonnullRefPtr<VulkanImage>> create_shared_vulkan_image(VulkanContext const& context, uint32_t width, uint32_t height, VkFormat format, ReadonlySpan<uint64_t> modifiers)
{
    VkDrmFormatModifierPropertiesListEXT format_mod_props_list = {};
    format_mod_props_list.sType = VK_STRUCTURE_TYPE_DRM_FORMAT_MODIFIER_PROPERTIES_LIST_EXT;
    format_mod_props_list.pNext = nullptr;
    VkFormatProperties2 format_props = {};
    format_props.sType = VK_STRUCTURE_TYPE_FORMAT_PROPERTIES_2;
    format_props.pNext = &format_mod_props_list;
    vkGetPhysicalDeviceFormatProperties2(context.physical_device, format, &format_props);
    Vector<VkDrmFormatModifierPropertiesEXT> format_mod_props;
    format_mod_props.resize(format_mod_props_list.drmFormatModifierCount);
    format_mod_props_list.pDrmFormatModifierProperties = format_mod_props.data();
    vkGetPhysicalDeviceFormatProperties2(context.physical_device, format, &format_props);

    // populate a list of all format modifiers that are both renderable and accepted by the caller
    Vector<uint64_t> format_mods;
    for (VkDrmFormatModifierPropertiesEXT const& props : format_mod_props) {
        if ((props.drmFormatModifierTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) && (props.drmFormatModifierPlaneCount == 1)) {
            if (modifiers.contains_slow(props.drmFormatModifier))
                format_mods.append(props.drmFormatModifier);
        }
    }

    // If the caller requested specific DRM modifiers and none are supported for a renderable image,
    // fail here so higher-level code can fall back to a different backing-store type.
    if (!modifiers.is_empty() && format_mods.is_empty())
        return Error::from_string_literal("no supported DRM format modifiers for shared image");

    NonnullRefPtr<VulkanImage> image = make_ref_counted<VulkanImage>(context);
    VkImageDrmFormatModifierListCreateInfoEXT image_drm_format_modifier_list_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_LIST_CREATE_INFO_EXT,
        .pNext = nullptr,
        .drmFormatModifierCount = static_cast<uint32_t>(format_mods.size()),
        .pDrmFormatModifiers = format_mods.data(),
    };
    VkExternalMemoryImageCreateInfo external_mem_image_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = &image_drm_format_modifier_list_info,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    Array<uint32_t, 1> queue_families = { context.graphics_queue_family };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_mem_image_info,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {
            .width = width,
            .height = height,
            .depth = 1,
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_DRM_FORMAT_MODIFIER_EXT,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = queue_families.size(),
        .pQueueFamilyIndices = queue_families.data(),
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    auto result = vkCreateImage(context.logical_device, &image_info, nullptr, &image->image);
    if (result != VK_SUCCESS) {
        dbgln("vkCreateImage returned {}", to_underlying(result));
        return Error::from_string_literal("image creation failed");
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(context.logical_device, image->image, &mem_reqs);
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(context.physical_device, &mem_props);
    bool const is_linear_image = format_mods.size() == 1 && format_mods[0] == DRM_FORMAT_MOD_LINEAR;
    uint32_t mem_type_idx = mem_props.memoryTypeCount;

    if (is_linear_image) {
        mem_type_idx = find_memory_type_index(mem_props, mem_reqs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT);
        if (mem_type_idx == mem_props.memoryTypeCount) {
            mem_type_idx = find_memory_type_index(mem_props, mem_reqs, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
        }
    } else {
        mem_type_idx = find_memory_type_index(mem_props, mem_reqs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    }

    if (mem_type_idx == mem_props.memoryTypeCount) {
        return Error::from_string_literal("unable to find suitable image memory type");
    }

    // Set up dedicated memory allocation; required for NVIDIA 10 series GPUs.
    // https://docs.vulkan.org/refpages/latest/refpages/source/VkMemoryAllocateInfo.html#VUID-VkMemoryAllocateInfo-pNext-00639
    VkMemoryDedicatedAllocateInfo mem_dedicated_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = nullptr,
        .image = image->image,
        .buffer = VK_NULL_HANDLE,
    };

    VkExportMemoryAllocateInfo export_mem_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = &mem_dedicated_alloc_info,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT,
    };
    VkMemoryAllocateInfo mem_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_mem_alloc_info,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type_idx,
    };
    result = vkAllocateMemory(context.logical_device, &mem_alloc_info, nullptr, &image->memory);
    if (result != VK_SUCCESS) {
        dbgln("vkAllocateMemory returned {}", to_underlying(result));
        return Error::from_string_literal("image memory allocation failed");
    }

    result = vkBindImageMemory(context.logical_device, image->image, image->memory, 0);
    if (result != VK_SUCCESS) {
        dbgln("vkBindImageMemory returned {}", to_underlying(result));
        return Error::from_string_literal("bind image memory failed");
    }

    VkImageSubresource subresource = { VK_IMAGE_ASPECT_MEMORY_PLANE_0_BIT_EXT, 0, 0 };
    VkSubresourceLayout subresource_layout = {};
    vkGetImageSubresourceLayout(context.logical_device, image->image, &subresource, &subresource_layout);

    VkImageDrmFormatModifierPropertiesEXT image_format_mod_props = {};
    image_format_mod_props.sType = VK_STRUCTURE_TYPE_IMAGE_DRM_FORMAT_MODIFIER_PROPERTIES_EXT;
    image_format_mod_props.pNext = nullptr;
    result = context.ext_procs.get_image_drm_format_modifier_properties(context.logical_device, image->image, &image_format_mod_props);
    if (result != VK_SUCCESS) {
        dbgln("vkGetImageDrmFormatModifierPropertiesEXT returned {}", to_underlying(result));
        return Error::from_string_literal("image format modifier retrieval failed");
    }

    // external APIs require general layout
    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    image->transition_layout(VK_IMAGE_LAYOUT_UNDEFINED, layout);

    image->info = {
        .format = image_info.format,
        .extent = image_info.extent,
        .tiling = image_info.tiling,
        .usage = image_info.usage,
        .sharing_mode = image_info.sharingMode,
        .layout = layout,
        .row_pitch = subresource_layout.rowPitch,
        .allocation_size = mem_reqs.size,
        .modifier = image_format_mod_props.drmFormatModifier,
    };
    return image;
}

ErrorOr<NonnullOwnPtr<VulkanBuffer>> create_host_visible_vulkan_buffer_from_bytes(VulkanContext const& context, ReadonlyBytes bytes, VkBufferUsageFlags usage)
{
    if (bytes.is_empty())
        return Error::from_string_literal("cannot create empty Vulkan buffer");

    auto buffer = make<VulkanBuffer>(context);
    buffer->size = bytes.size();
    buffer->usage = usage;

    VkBufferCreateInfo buffer_info {
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .size = buffer->size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
    };
    auto result = vkCreateBuffer(context.logical_device, &buffer_info, nullptr, &buffer->buffer);
    if (result != VK_SUCCESS)
        return Error::from_string_literal("failed to create Vulkan buffer");

    VkMemoryRequirements memory_requirements;
    vkGetBufferMemoryRequirements(context.logical_device, buffer->buffer, &memory_requirements);

    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(context.physical_device, &memory_properties);
    auto memory_type_index = find_memory_type_index(memory_properties, memory_requirements, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (memory_type_index == memory_properties.memoryTypeCount)
        return Error::from_string_literal("failed to find host-visible Vulkan buffer memory");

    VkMemoryAllocateInfo memory_allocate_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = nullptr,
        .allocationSize = memory_requirements.size,
        .memoryTypeIndex = memory_type_index,
    };
    result = vkAllocateMemory(context.logical_device, &memory_allocate_info, nullptr, &buffer->memory);
    if (result != VK_SUCCESS)
        return Error::from_string_literal("failed to allocate Vulkan buffer memory");

    void* mapped_memory = nullptr;
    result = vkMapMemory(context.logical_device, buffer->memory, 0, buffer->size, 0, &mapped_memory);
    if (result != VK_SUCCESS)
        return Error::from_string_literal("failed to map Vulkan buffer memory");

    memcpy(mapped_memory, bytes.data(), bytes.size());
    vkUnmapMemory(context.logical_device, buffer->memory);

    result = vkBindBufferMemory(context.logical_device, buffer->buffer, buffer->memory, 0);
    if (result != VK_SUCCESS)
        return Error::from_string_literal("failed to bind Vulkan buffer memory");

    return buffer;
}

struct SharedYcbcrSampleResources {
    VkDevice device { VK_NULL_HANDLE };
    VkFormat format { VK_FORMAT_UNDEFINED };
    VkSamplerYcbcrConversion conversion { VK_NULL_HANDLE };
    VkSampler sampler { VK_NULL_HANDLE };
};

static ErrorOr<SharedYcbcrSampleResources*> get_or_create_shared_ycbcr_sample_resources(VulkanContext const& context, VkFormat format)
{
    static SharedYcbcrSampleResources s_resources;
    if (s_resources.conversion != VK_NULL_HANDLE || s_resources.sampler != VK_NULL_HANDLE) {
        if (s_resources.device != context.logical_device || s_resources.format != format)
            return Error::from_string_literal("multiple Vulkan YCbCr sample resource configurations are not supported yet");
        return &s_resources;
    }

    VkSamplerYcbcrConversionCreateInfo conversion_info {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_CREATE_INFO,
        .pNext = nullptr,
        .format = format,
        .ycbcrModel = VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
        .ycbcrRange = VK_SAMPLER_YCBCR_RANGE_ITU_FULL,
        .components = {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .xChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,
        .yChromaOffset = VK_CHROMA_LOCATION_MIDPOINT,
        .chromaFilter = VK_FILTER_LINEAR,
        .forceExplicitReconstruction = VK_FALSE,
    };

    auto result = vkCreateSamplerYcbcrConversion(context.logical_device, &conversion_info, nullptr, &s_resources.conversion);
    if (result != VK_SUCCESS)
        return Error::from_string_literal("failed to create shared Vulkan YCbCr sampler conversion");

    VkSamplerYcbcrConversionInfo sampler_conversion_info {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .pNext = nullptr,
        .conversion = s_resources.conversion,
    };
    VkSamplerCreateInfo sampler_info {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .pNext = &sampler_conversion_info,
        .flags = 0,
        .magFilter = VK_FILTER_LINEAR,
        .minFilter = VK_FILTER_LINEAR,
        .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        .mipLodBias = 0.0f,
        .anisotropyEnable = VK_FALSE,
        .maxAnisotropy = 1.0f,
        .compareEnable = VK_FALSE,
        .compareOp = VK_COMPARE_OP_ALWAYS,
        .minLod = 0.0f,
        .maxLod = 0.0f,
        .borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK,
        .unnormalizedCoordinates = VK_FALSE,
    };

    result = vkCreateSampler(context.logical_device, &sampler_info, nullptr, &s_resources.sampler);
    if (result != VK_SUCCESS)
        return Error::from_string_literal("failed to create shared Vulkan YCbCr sampler");

    s_resources.device = context.logical_device;
    s_resources.format = format;
    dbgln("MUNDO_VULKAN_VIDEO_YCBCR_SHARED_RESOURCES status=ok format={} sampler={} conversion={}",
        to_underlying(format),
        reinterpret_cast<uintptr_t>(s_resources.sampler),
        reinterpret_cast<uintptr_t>(s_resources.conversion));
    return &s_resources;
}

static ErrorOr<void> create_ycbcr_direct_sample_resources(ImportedVulkanNV12Image& imported_image, size_t log_count)
{
    auto const& context = imported_image.context;
    if (!context.sampler_ycbcr_conversion_supported || !context.nv12_ycbcr_sampling_supported) {
        if (log_count <= 8 || log_count % 120 == 0) {
            dbgln("MUNDO_VULKAN_VIDEO_YCBCR_DIRECT_SAMPLE_IMAGE count={} status=skipped reason=unsupported sampler_ycbcr_conversion={} nv12_ycbcr_sampling={} size={}x{} format={} handle_type={}",
                log_count,
                context.sampler_ycbcr_conversion_supported,
                context.nv12_ycbcr_sampling_supported,
                imported_image.width,
                imported_image.height,
                to_underlying(imported_image.format),
                to_underlying(imported_image.handle_type));
        }
        return Error::from_string_literal("Vulkan YCbCr direct sampling is unsupported");
    }

    auto* shared_resources = TRY(get_or_create_shared_ycbcr_sample_resources(context, imported_image.format));
    imported_image.ycbcr_conversion = shared_resources->conversion;
    imported_image.ycbcr_sampler = shared_resources->sampler;
    imported_image.owns_ycbcr_resources = false;

    VkSamplerYcbcrConversionInfo sampler_conversion_info {
        .sType = VK_STRUCTURE_TYPE_SAMPLER_YCBCR_CONVERSION_INFO,
        .pNext = nullptr,
        .conversion = imported_image.ycbcr_conversion,
    };
    VkImageViewCreateInfo image_view_info {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = &sampler_conversion_info,
        .flags = 0,
        .image = imported_image.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = imported_image.format,
        .components = {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };

    auto result = vkCreateImageView(context.logical_device, &image_view_info, nullptr, &imported_image.ycbcr_image_view);
    if (result != VK_SUCCESS) {
        if (log_count <= 8 || log_count % 120 == 0) {
            dbgln("MUNDO_VULKAN_VIDEO_YCBCR_DIRECT_SAMPLE_IMAGE count={} status=failed step=create_image_view result={} size={}x{} format={} handle_type={}",
                log_count, to_underlying(result), imported_image.width, imported_image.height, to_underlying(imported_image.format), to_underlying(imported_image.handle_type));
        }
        return Error::from_string_literal("failed to create Vulkan YCbCr image view");
    }

    imported_image.direct_sample_ready = true;
    if (log_count <= 8 || log_count % 120 == 0) {
        dbgln("MUNDO_VULKAN_VIDEO_YCBCR_DIRECT_SAMPLE_IMAGE count={} status=ok size={}x{} format={} handle_type={} image={} image_view={} sampler={} conversion={}",
            log_count,
            imported_image.width,
            imported_image.height,
            to_underlying(imported_image.format),
            to_underlying(imported_image.handle_type),
            reinterpret_cast<uintptr_t>(imported_image.image),
            reinterpret_cast<uintptr_t>(imported_image.ycbcr_image_view),
            reinterpret_cast<uintptr_t>(imported_image.ycbcr_sampler),
            reinterpret_cast<uintptr_t>(imported_image.ycbcr_conversion));
    }
    return {};
}

ErrorOr<NonnullOwnPtr<ImportedVulkanNV12Image>> import_vulkan_nv12_external_memory(VulkanContext const& context, int source_fd, VkExternalMemoryHandleTypeFlagBits source_handle_type, VkDeviceSize source_allocation_size, uint32_t width, uint32_t height, VkFormat source_format, VkImageLayout source_layout)
{
    static size_t s_import_count { 0 };
    auto import_count = ++s_import_count;

    if (source_fd < 0)
        return Error::from_string_literal("invalid source Vulkan external memory fd");
    if (source_format != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
        return Error::from_string_literal("unsupported source Vulkan NV12 format");

    int import_fd = source_fd;
    auto close_import_fd = ArmedScopeGuard([&] {
        if (import_fd >= 0)
            close(import_fd);
    });

    auto imported_image = make<ImportedVulkanNV12Image>(context);
    imported_image->format = source_format;
    imported_image->layout = source_layout;
    imported_image->allocation_size = source_allocation_size;
    imported_image->handle_type = source_handle_type;
    imported_image->width = width;
    imported_image->height = height;

    VkExternalMemoryImageCreateInfo external_memory_image_info {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .handleTypes = source_handle_type,
    };
    VkImageCreateInfo source_image_info {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_memory_image_info,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = source_format,
        .extent = {
            .width = width,
            .height = height,
            .depth = 1,
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = 0,
        .pQueueFamilyIndices = nullptr,
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };

    auto result = vkCreateImage(context.logical_device, &source_image_info, nullptr, &imported_image->image);
    if (result != VK_SUCCESS) {
        dbgln("MUNDO_VULKAN_VIDEO_IMPORT_NV12 status=failed step=create_source_image result={} size={}x{} source_format={} handle_type={}", to_underlying(result), width, height, to_underlying(source_format), to_underlying(source_handle_type));
        return Error::from_string_literal("failed to create imported Vulkan NV12 source image");
    }

    VkMemoryRequirements source_memory_requirements;
    vkGetImageMemoryRequirements(context.logical_device, imported_image->image, &source_memory_requirements);
    imported_image->required_size = source_memory_requirements.size;

    static bool s_skip_opaque_fd_properties_query { false };
    static bool s_skip_dma_buf_fd_properties_query { false };
    auto& skip_fd_properties_query = source_handle_type == VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT
        ? s_skip_dma_buf_fd_properties_query
        : s_skip_opaque_fd_properties_query;

    VkMemoryFdPropertiesKHR fd_properties {
        .sType = VK_STRUCTURE_TYPE_MEMORY_FD_PROPERTIES_KHR,
        .pNext = nullptr,
        .memoryTypeBits = 0,
    };
    VkResult fd_properties_result { VK_SUCCESS };
    if (!skip_fd_properties_query) {
        fd_properties_result = context.ext_procs.get_memory_fd_properties(context.logical_device, source_handle_type, import_fd, &fd_properties);
        if (fd_properties_result != VK_SUCCESS) {
            skip_fd_properties_query = true;
            dbgln("MUNDO_VULKAN_VIDEO_IMPORT_NV12 status=warning step=get_fd_properties result={} action=skip_future_queries size={}x{} source_allocation_size={} required_memory_type_bits={} handle_type={}",
                to_underlying(fd_properties_result),
                width,
                height,
                source_allocation_size,
                source_memory_requirements.memoryTypeBits,
                to_underlying(source_handle_type));
        }
    }

    VkPhysicalDeviceMemoryProperties memory_properties;
    vkGetPhysicalDeviceMemoryProperties(context.physical_device, &memory_properties);
    auto compatible_memory_type_bits = source_memory_requirements.memoryTypeBits;
    if (fd_properties_result == VK_SUCCESS && !skip_fd_properties_query)
        compatible_memory_type_bits &= fd_properties.memoryTypeBits;
    auto memory_type_index = find_memory_type_index(memory_properties, compatible_memory_type_bits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (memory_type_index == memory_properties.memoryTypeCount)
        return Error::from_string_literal("no compatible memory type for imported Vulkan NV12 source image");

    VkMemoryDedicatedAllocateInfo dedicated_allocate_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = nullptr,
        .image = imported_image->image,
        .buffer = VK_NULL_HANDLE,
    };
    VkImportMemoryFdInfoKHR import_memory_fd_info {
        .sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR,
        .pNext = &dedicated_allocate_info,
        .handleType = source_handle_type,
        .fd = import_fd,
    };
    auto allocation_size = source_allocation_size > source_memory_requirements.size ? source_allocation_size : source_memory_requirements.size;
    imported_image->allocation_size = allocation_size;
    VkMemoryAllocateInfo memory_allocate_info {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &import_memory_fd_info,
        .allocationSize = allocation_size,
        .memoryTypeIndex = memory_type_index,
    };

    result = vkAllocateMemory(context.logical_device, &memory_allocate_info, nullptr, &imported_image->memory);
    if (result != VK_SUCCESS) {
        dbgln("MUNDO_VULKAN_VIDEO_IMPORT_NV12 status=failed step=allocate_imported_memory result={} size={}x{} source_allocation_size={} required_size={} allocation_size={} memory_type_bits={} fd_memory_type_bits={} handle_type={} fd_properties_result={}",
            to_underlying(result),
            width,
            height,
            source_allocation_size,
            source_memory_requirements.size,
            allocation_size,
            source_memory_requirements.memoryTypeBits,
            fd_properties.memoryTypeBits,
            to_underlying(source_handle_type),
            to_underlying(fd_properties_result));
        return Error::from_string_literal("failed to allocate imported Vulkan NV12 source memory");
    }
    import_fd = -1; // vkAllocateMemory consumes the imported fd on success.

    result = vkBindImageMemory(context.logical_device, imported_image->image, imported_image->memory, 0);
    if (result != VK_SUCCESS) {
        dbgln("MUNDO_VULKAN_VIDEO_IMPORT_NV12 status=failed step=bind_source_memory result={} size={}x{}", to_underlying(result), width, height);
        return Error::from_string_literal("failed to bind imported Vulkan NV12 source memory");
    }

    auto ycbcr_result = create_ycbcr_direct_sample_resources(*imported_image, import_count);
    if (ycbcr_result.is_error() && (import_count <= 8 || import_count % 120 == 0)) {
        dbgln("MUNDO_VULKAN_VIDEO_IMPORT_NV12 count={} status=ok direct_sample_ready=false direct_sample_reason={} size={}x{} source_format={} source_layout={} source_allocation_size={} required_size={} handle_type={} image={}",
            import_count,
            ycbcr_result.error().string_literal(),
            width,
            height,
            to_underlying(source_format),
            to_underlying(source_layout),
            source_allocation_size,
            source_memory_requirements.size,
            to_underlying(source_handle_type),
            reinterpret_cast<uintptr_t>(imported_image->image));
    } else if (import_count <= 8 || import_count % 120 == 0) {
        dbgln("MUNDO_VULKAN_VIDEO_IMPORT_NV12 count={} status=ok direct_sample_ready={} size={}x{} source_format={} source_layout={} source_allocation_size={} required_size={} handle_type={} image={}",
            import_count,
            imported_image->direct_sample_ready,
            width,
            height,
            to_underlying(source_format),
            to_underlying(source_layout),
            source_allocation_size,
            source_memory_requirements.size,
            to_underlying(source_handle_type),
            reinterpret_cast<uintptr_t>(imported_image->image));
    }

    return imported_image;
}

static constexpr u32 s_video_nv12_to_rgba_vertex_shader_spirv[] {
    0x07230203, 0x00010000, 0x0008000b, 0x00000039, 0x00000000, 0x00020011, 0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0008000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x00000017, 0x00000021, 0x0000002c, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00050005, 0x0000000c, 0x69736f70, 0x6e6f6974, 0x00000073, 0x00030005, 0x00000014, 0x00736f70, 0x00060005, 0x00000017, 0x565f6c67, 0x65747265, 0x646e4978, 0x00007865, 0x00060005, 0x0000001f, 0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006, 0x0000001f, 0x00000000, 0x505f6c67, 0x7469736f, 0x006e6f69, 0x00070006, 0x0000001f, 0x00000001, 0x505f6c67, 0x746e696f, 0x657a6953, 0x00000000, 0x00070006, 0x0000001f, 0x00000002, 0x435f6c67, 0x4470696c, 0x61747369, 0x0065636e, 0x00070006, 0x0000001f, 0x00000003, 0x435f6c67, 0x446c6c75, 0x61747369, 0x0065636e, 0x00030005, 0x00000021, 0x00000000, 0x00040005, 0x0000002c, 0x76755f76, 0x00000000, 0x00040047, 0x00000017, 0x0000000b, 0x0000002a, 0x00030047, 0x0000001f, 0x00000002, 0x00050048, 0x0000001f, 0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x0000001f, 0x00000001, 0x0000000b, 0x00000001, 0x00050048, 0x0000001f, 0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x0000001f, 0x00000003, 0x0000000b, 0x00000004, 0x00040047, 0x0000002c, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000002, 0x00040015, 0x00000008, 0x00000020, 0x00000000, 0x0004002b, 0x00000008, 0x00000009, 0x00000003, 0x0004001c, 0x0000000a, 0x00000007, 0x00000009, 0x00040020, 0x0000000b, 0x00000006, 0x0000000a, 0x0004003b, 0x0000000b, 0x0000000c, 0x00000006, 0x0004002b, 0x00000006, 0x0000000d, 0xbf800000, 0x0005002c, 0x00000007, 0x0000000e, 0x0000000d, 0x0000000d, 0x0004002b, 0x00000006, 0x0000000f, 0x40400000, 0x0005002c, 0x00000007, 0x00000010, 0x0000000f, 0x0000000d, 0x0005002c, 0x00000007, 0x00000011, 0x0000000d, 0x0000000f, 0x0006002c, 0x0000000a, 0x00000012, 0x0000000e, 0x00000010, 0x00000011, 0x00040020, 0x00000013, 0x00000007, 0x00000007, 0x00040015, 0x00000015, 0x00000020, 0x00000001, 0x00040020, 0x00000016, 0x00000001, 0x00000015, 0x0004003b, 0x00000016, 0x00000017, 0x00000001, 0x00040020, 0x00000019, 0x00000006, 0x00000007, 0x00040017, 0x0000001c, 0x00000006, 0x00000004, 0x0004002b, 0x00000008, 0x0000001d, 0x00000001, 0x0004001c, 0x0000001e, 0x00000006, 0x0000001d, 0x0006001e, 0x0000001f, 0x0000001c, 0x00000006, 0x0000001e, 0x0000001e, 0x00040020, 0x00000020, 0x00000003, 0x0000001f, 0x0004003b, 0x00000020, 0x00000021, 0x00000003, 0x0004002b, 0x00000015, 0x00000022, 0x00000000, 0x0004002b, 0x00000006, 0x00000024, 0x00000000, 0x0004002b, 0x00000006, 0x00000025, 0x3f800000, 0x00040020, 0x00000029, 0x00000003, 0x0000001c, 0x00040020, 0x0000002b, 0x00000003, 0x00000007, 0x0004003b, 0x0000002b, 0x0000002c, 0x00000003, 0x0004002b, 0x00000008, 0x0000002d, 0x00000000, 0x00040020, 0x0000002e, 0x00000007, 0x00000006, 0x0004002b, 0x00000006, 0x00000032, 0x3f000000, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003b, 0x00000013, 0x00000014, 0x00000007, 0x0003003e, 0x0000000c, 0x00000012, 0x0004003d, 0x00000015, 0x00000018, 0x00000017, 0x00050041, 0x00000019, 0x0000001a, 0x0000000c, 0x00000018, 0x0004003d, 0x00000007, 0x0000001b, 0x0000001a, 0x0003003e, 0x00000014, 0x0000001b, 0x0004003d, 0x00000007, 0x00000023, 0x00000014, 0x00050051, 0x00000006, 0x00000026, 0x00000023, 0x00000000, 0x00050051, 0x00000006, 0x00000027, 0x00000023, 0x00000001, 0x00070050, 0x0000001c, 0x00000028, 0x00000026, 0x00000027, 0x00000024, 0x00000025, 0x00050041, 0x00000029, 0x0000002a, 0x00000021, 0x00000022, 0x0003003e, 0x0000002a, 0x00000028, 0x00050041, 0x0000002e, 0x0000002f, 0x00000014, 0x0000002d, 0x0004003d, 0x00000006, 0x00000030, 0x0000002f, 0x00050081, 0x00000006, 0x00000031, 0x00000030, 0x00000025, 0x00050085, 0x00000006, 0x00000033, 0x00000031, 0x00000032, 0x00050041, 0x0000002e, 0x00000034, 0x00000014, 0x0000001d, 0x0004003d, 0x00000006, 0x00000035, 0x00000034, 0x00050081, 0x00000006, 0x00000036, 0x00000035, 0x00000025, 0x00050085, 0x00000006, 0x00000037, 0x00000036, 0x00000032, 0x00050050, 0x00000007, 0x00000038, 0x00000033, 0x00000037, 0x0003003e, 0x0000002c, 0x00000038, 0x000100fd, 0x00010038
};

static constexpr u32 s_video_nv12_to_rgba_fragment_shader_spirv[] {
    0x07230203, 0x00010000, 0x0008000b, 0x00000014, 0x00000000, 0x00020011, 0x00000001, 0x0006000b, 0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001, 0x0007000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00000011, 0x00030010, 0x00000004, 0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d, 0x00000000, 0x00050005, 0x00000009, 0x5f74756f, 0x6f6c6f63, 0x00000072, 0x00060005, 0x0000000d, 0x65646976, 0x65745f6f, 0x72757478, 0x00000065, 0x00040005, 0x00000011, 0x76755f76, 0x00000000, 0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00040047, 0x0000000d, 0x00000021, 0x00000000, 0x00040047, 0x0000000d, 0x00000022, 0x00000000, 0x00040047, 0x00000011, 0x0000001e, 0x00000000, 0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007, 0x0004003b, 0x00000008, 0x00000009, 0x00000003, 0x00090019, 0x0000000a, 0x00000006, 0x00000001, 0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x0003001b, 0x0000000b, 0x0000000a, 0x00040020, 0x0000000c, 0x00000000, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000000, 0x00040017, 0x0000000f, 0x00000006, 0x00000002, 0x00040020, 0x00000010, 0x00000001, 0x0000000f, 0x0004003b, 0x00000010, 0x00000011, 0x00000001, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000b, 0x0000000e, 0x0000000d, 0x0004003d, 0x0000000f, 0x00000012, 0x00000011, 0x00050057, 0x00000007, 0x00000013, 0x0000000e, 0x00000012, 0x0003003e, 0x00000009, 0x00000013, 0x000100fd, 0x00010038
};

static ErrorOr<VkShaderModule> create_shader_module(VulkanContext const& context, ReadonlySpan<u32 const> spirv)
{
    VkShaderModuleCreateInfo create_info {
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .codeSize = spirv.size() * sizeof(u32),
        .pCode = spirv.data(),
    };
    VkShaderModule shader_module { VK_NULL_HANDLE };
    auto result = vkCreateShaderModule(context.logical_device, &create_info, nullptr, &shader_module);
    if (result != VK_SUCCESS)
        return Error::from_string_literal("failed to create Vulkan video shader module");
    return shader_module;
}

struct VideoNV12ToRGBAPipelineResources {
    VkDevice device { VK_NULL_HANDLE };
    VkFormat destination_format { VK_FORMAT_UNDEFINED };
    VkSampler immutable_sampler { VK_NULL_HANDLE };
    VkShaderModule vertex_shader { VK_NULL_HANDLE };
    VkShaderModule fragment_shader { VK_NULL_HANDLE };
    VkRenderPass render_pass { VK_NULL_HANDLE };
    VkDescriptorSetLayout descriptor_set_layout { VK_NULL_HANDLE };
    VkPipelineLayout pipeline_layout { VK_NULL_HANDLE };
    VkPipeline pipeline { VK_NULL_HANDLE };
    VkDescriptorPool descriptor_pool { VK_NULL_HANDLE };
    VkDescriptorSet descriptor_set { VK_NULL_HANDLE };
};

static ErrorOr<VideoNV12ToRGBAPipelineResources*> get_or_create_video_nv12_to_rgba_pipeline_resources(VulkanContext const& context, VkFormat destination_format, VkSampler immutable_sampler)
{
    static VideoNV12ToRGBAPipelineResources s_resources;
    if (s_resources.pipeline != VK_NULL_HANDLE) {
        if (s_resources.device != context.logical_device || s_resources.destination_format != destination_format || s_resources.immutable_sampler != immutable_sampler)
            return Error::from_string_literal("multiple Vulkan video RGBA pipeline configurations are not supported yet");
        return &s_resources;
    }

    s_resources.vertex_shader = TRY(create_shader_module(context, s_video_nv12_to_rgba_vertex_shader_spirv));
    s_resources.fragment_shader = TRY(create_shader_module(context, s_video_nv12_to_rgba_fragment_shader_spirv));

    VkAttachmentDescription color_attachment {
        .flags = 0,
        .format = destination_format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
        .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
        .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
        .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkAttachmentReference color_attachment_ref {
        .attachment = 0,
        .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
    };
    VkSubpassDescription subpass {
        .flags = 0,
        .pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS,
        .inputAttachmentCount = 0,
        .pInputAttachments = nullptr,
        .colorAttachmentCount = 1,
        .pColorAttachments = &color_attachment_ref,
        .pResolveAttachments = nullptr,
        .pDepthStencilAttachment = nullptr,
        .preserveAttachmentCount = 0,
        .pPreserveAttachments = nullptr,
    };
    VkRenderPassCreateInfo render_pass_info {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .attachmentCount = 1,
        .pAttachments = &color_attachment,
        .subpassCount = 1,
        .pSubpasses = &subpass,
        .dependencyCount = 0,
        .pDependencies = nullptr,
    };
    auto result = vkCreateRenderPass(context.logical_device, &render_pass_info, nullptr, &s_resources.render_pass);
    if (result != VK_SUCCESS)
        return Error::from_string_literal("failed to create cached Vulkan video render pass");

    VkDescriptorSetLayoutBinding sampler_binding {
        .binding = 0,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
        .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
        .pImmutableSamplers = &immutable_sampler,
    };
    VkDescriptorSetLayoutCreateInfo descriptor_set_layout_info {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .bindingCount = 1,
        .pBindings = &sampler_binding,
    };
    result = vkCreateDescriptorSetLayout(context.logical_device, &descriptor_set_layout_info, nullptr, &s_resources.descriptor_set_layout);
    if (result != VK_SUCCESS)
        return Error::from_string_literal("failed to create cached Vulkan video descriptor set layout");

    VkPipelineLayoutCreateInfo pipeline_layout_info {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = &s_resources.descriptor_set_layout,
        .pushConstantRangeCount = 0,
        .pPushConstantRanges = nullptr,
    };
    result = vkCreatePipelineLayout(context.logical_device, &pipeline_layout_info, nullptr, &s_resources.pipeline_layout);
    if (result != VK_SUCCESS)
        return Error::from_string_literal("failed to create cached Vulkan video pipeline layout");

    VkPipelineShaderStageCreateInfo shader_stages[] {
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = s_resources.vertex_shader,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
        {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = s_resources.fragment_shader,
            .pName = "main",
            .pSpecializationInfo = nullptr,
        },
    };
    VkPipelineVertexInputStateCreateInfo vertex_input_info {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .vertexBindingDescriptionCount = 0,
        .pVertexBindingDescriptions = nullptr,
        .vertexAttributeDescriptionCount = 0,
        .pVertexAttributeDescriptions = nullptr,
    };
    VkPipelineInputAssemblyStateCreateInfo input_assembly {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        .primitiveRestartEnable = VK_FALSE,
    };
    VkViewport viewport {
        .x = 0,
        .y = 0,
        .width = 1.0f,
        .height = 1.0f,
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor {
        .offset = { 0, 0 },
        .extent = { 1, 1 },
    };
    VkPipelineViewportStateCreateInfo viewport_state {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .viewportCount = 1,
        .pViewports = &viewport,
        .scissorCount = 1,
        .pScissors = &scissor,
    };
    VkPipelineRasterizationStateCreateInfo rasterizer {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .depthClampEnable = VK_FALSE,
        .rasterizerDiscardEnable = VK_FALSE,
        .polygonMode = VK_POLYGON_MODE_FILL,
        .cullMode = VK_CULL_MODE_NONE,
        .frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE,
        .depthBiasEnable = VK_FALSE,
        .depthBiasConstantFactor = 0.0f,
        .depthBiasClamp = 0.0f,
        .depthBiasSlopeFactor = 0.0f,
        .lineWidth = 1.0f,
    };
    VkPipelineMultisampleStateCreateInfo multisampling {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
        .sampleShadingEnable = VK_FALSE,
        .minSampleShading = 1.0f,
        .pSampleMask = nullptr,
        .alphaToCoverageEnable = VK_FALSE,
        .alphaToOneEnable = VK_FALSE,
    };
    VkPipelineColorBlendAttachmentState color_blend_attachment {
        .blendEnable = VK_FALSE,
        .srcColorBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstColorBlendFactor = VK_BLEND_FACTOR_ZERO,
        .colorBlendOp = VK_BLEND_OP_ADD,
        .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
        .dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO,
        .alphaBlendOp = VK_BLEND_OP_ADD,
        .colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT,
    };
    VkPipelineColorBlendStateCreateInfo color_blending {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .logicOpEnable = VK_FALSE,
        .logicOp = VK_LOGIC_OP_COPY,
        .attachmentCount = 1,
        .pAttachments = &color_blend_attachment,
        .blendConstants = { 0, 0, 0, 0 },
    };
    VkDynamicState dynamic_states[] {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
    };
    VkPipelineDynamicStateCreateInfo dynamic_state {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .dynamicStateCount = AK::array_size(dynamic_states),
        .pDynamicStates = dynamic_states,
    };
    VkGraphicsPipelineCreateInfo pipeline_info {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stageCount = AK::array_size(shader_stages),
        .pStages = shader_stages,
        .pVertexInputState = &vertex_input_info,
        .pInputAssemblyState = &input_assembly,
        .pTessellationState = nullptr,
        .pViewportState = &viewport_state,
        .pRasterizationState = &rasterizer,
        .pMultisampleState = &multisampling,
        .pDepthStencilState = nullptr,
        .pColorBlendState = &color_blending,
        .pDynamicState = &dynamic_state,
        .layout = s_resources.pipeline_layout,
        .renderPass = s_resources.render_pass,
        .subpass = 0,
        .basePipelineHandle = VK_NULL_HANDLE,
        .basePipelineIndex = -1,
    };
    result = vkCreateGraphicsPipelines(context.logical_device, VK_NULL_HANDLE, 1, &pipeline_info, nullptr, &s_resources.pipeline);
    if (result != VK_SUCCESS) {
        dbgln("MUNDO_VULKAN_VIDEO_NV12_RGBA_RENDER status=failed step=create_cached_pipeline result={} destination_format={}", to_underlying(result), to_underlying(destination_format));
        return Error::from_string_literal("failed to create cached Vulkan video graphics pipeline");
    }

    VkDescriptorPoolSize pool_size {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1,
    };
    VkDescriptorPoolCreateInfo descriptor_pool_info {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .maxSets = 1,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    result = vkCreateDescriptorPool(context.logical_device, &descriptor_pool_info, nullptr, &s_resources.descriptor_pool);
    if (result != VK_SUCCESS)
        return Error::from_string_literal("failed to create cached Vulkan video descriptor pool");

    VkDescriptorSetAllocateInfo descriptor_set_allocate_info {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorPool = s_resources.descriptor_pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &s_resources.descriptor_set_layout,
    };
    result = vkAllocateDescriptorSets(context.logical_device, &descriptor_set_allocate_info, &s_resources.descriptor_set);
    if (result != VK_SUCCESS)
        return Error::from_string_literal("failed to allocate cached Vulkan video descriptor set");

    s_resources.device = context.logical_device;
    s_resources.destination_format = destination_format;
    s_resources.immutable_sampler = immutable_sampler;
    dbgln("MUNDO_VULKAN_VIDEO_NV12_RGBA_PIPELINE_CACHE status=ok destination_format={} sampler={} pipeline={}",
        to_underlying(destination_format),
        reinterpret_cast<uintptr_t>(immutable_sampler),
        reinterpret_cast<uintptr_t>(s_resources.pipeline));
    return &s_resources;
}

struct VideoRGBAFramebufferResources {
    VkImageView image_view { VK_NULL_HANDLE };
    VkFramebuffer framebuffer { VK_NULL_HANDLE };
};

static ErrorOr<VideoRGBAFramebufferResources> get_or_create_video_rgba_framebuffer(VulkanImage& rgba_destination, VkRenderPass render_pass, uint32_t width, uint32_t height)
{
    auto const& context = rgba_destination.context;
    if (rgba_destination.cached_video_framebuffer != VK_NULL_HANDLE
        && rgba_destination.cached_video_color_attachment_view != VK_NULL_HANDLE
        && rgba_destination.cached_video_framebuffer_render_pass == render_pass
        && rgba_destination.cached_video_framebuffer_width == width
        && rgba_destination.cached_video_framebuffer_height == height) {
        return VideoRGBAFramebufferResources {
            .image_view = rgba_destination.cached_video_color_attachment_view,
            .framebuffer = rgba_destination.cached_video_framebuffer,
        };
    }

    if (rgba_destination.cached_video_framebuffer != VK_NULL_HANDLE) {
        vkDestroyFramebuffer(context.logical_device, rgba_destination.cached_video_framebuffer, nullptr);
        rgba_destination.cached_video_framebuffer = VK_NULL_HANDLE;
    }
    if (rgba_destination.cached_video_color_attachment_view != VK_NULL_HANDLE) {
        vkDestroyImageView(context.logical_device, rgba_destination.cached_video_color_attachment_view, nullptr);
        rgba_destination.cached_video_color_attachment_view = VK_NULL_HANDLE;
    }

    VkImageViewCreateInfo destination_image_view_info {
        .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .image = rgba_destination.image,
        .viewType = VK_IMAGE_VIEW_TYPE_2D,
        .format = rgba_destination.info.format,
        .components = {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    auto result = vkCreateImageView(context.logical_device, &destination_image_view_info, nullptr, &rgba_destination.cached_video_color_attachment_view);
    if (result != VK_SUCCESS)
        return Error::from_string_literal("failed to create cached Vulkan video RGBA destination image view");

    VkFramebufferCreateInfo framebuffer_info {
        .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .renderPass = render_pass,
        .attachmentCount = 1,
        .pAttachments = &rgba_destination.cached_video_color_attachment_view,
        .width = width,
        .height = height,
        .layers = 1,
    };
    result = vkCreateFramebuffer(context.logical_device, &framebuffer_info, nullptr, &rgba_destination.cached_video_framebuffer);
    if (result != VK_SUCCESS)
        return Error::from_string_literal("failed to create cached Vulkan video framebuffer");

    rgba_destination.cached_video_framebuffer_render_pass = render_pass;
    rgba_destination.cached_video_framebuffer_width = width;
    rgba_destination.cached_video_framebuffer_height = height;
    dbgln("MUNDO_VULKAN_VIDEO_RGBA_FRAMEBUFFER_CACHE status=ok size={}x{} image={} image_view={} framebuffer={}",
        width,
        height,
        reinterpret_cast<uintptr_t>(rgba_destination.image),
        reinterpret_cast<uintptr_t>(rgba_destination.cached_video_color_attachment_view),
        reinterpret_cast<uintptr_t>(rgba_destination.cached_video_framebuffer));
    return VideoRGBAFramebufferResources {
        .image_view = rgba_destination.cached_video_color_attachment_view,
        .framebuffer = rgba_destination.cached_video_framebuffer,
    };
}

ErrorOr<void> render_vulkan_nv12_external_memory_to_opaque_rgba_image(VulkanContext const& context, int source_fd, VkExternalMemoryHandleTypeFlagBits source_handle_type, VkDeviceSize source_allocation_size, uint32_t width, uint32_t height, VkFormat source_format, VkImageLayout source_layout, VulkanImage& rgba_destination, bool flip_y)
{
    auto imported_source = TRY(import_vulkan_nv12_external_memory(context, source_fd, source_handle_type, source_allocation_size, width, height, source_format, source_layout));
    if (!imported_source->direct_sample_ready)
        return Error::from_string_literal("imported NV12 image is not ready for direct YCbCr sampling");
    if (rgba_destination.info.format != VK_FORMAT_R8G8B8A8_UNORM)
        return Error::from_string_literal("RGBA render destination is not RGBA8");
    if (!(rgba_destination.info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        return Error::from_string_literal("RGBA render destination is not a color attachment");

    auto* pipeline_resources = TRY(get_or_create_video_nv12_to_rgba_pipeline_resources(context, rgba_destination.info.format, imported_source->ycbcr_sampler));
    auto framebuffer_resources = TRY(get_or_create_video_rgba_framebuffer(rgba_destination, pipeline_resources->render_pass, width, height));

    VkDescriptorImageInfo descriptor_image_info {
        .sampler = VK_NULL_HANDLE,
        .imageView = imported_source->ycbcr_image_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet descriptor_write {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = pipeline_resources->descriptor_set,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &descriptor_image_info,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
    };
    vkUpdateDescriptorSets(context.logical_device, 1, &descriptor_write, 0, nullptr);

    vkResetCommandBuffer(context.command_buffer, 0);
    VkCommandBufferBeginInfo begin_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    auto result = vkBeginCommandBuffer(context.command_buffer, &begin_info);
    if (result != VK_SUCCESS)
        return Error::from_string_literal("failed to begin Vulkan video render command buffer");

    VkImageMemoryBarrier pre_render_barriers[] {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = source_layout,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = imported_source->image,
            .subresourceRange = { VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .oldLayout = rgba_destination.info.layout,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = rgba_destination.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
    };
    vkCmdPipelineBarrier(context.command_buffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,
        0, nullptr,
        0, nullptr,
        AK::array_size(pre_render_barriers), pre_render_barriers);

    VkRenderPassBeginInfo render_pass_begin {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext = nullptr,
        .renderPass = pipeline_resources->render_pass,
        .framebuffer = framebuffer_resources.framebuffer,
        .renderArea = { { 0, 0 }, { width, height } },
        .clearValueCount = 0,
        .pClearValues = nullptr,
    };
    vkCmdBeginRenderPass(context.command_buffer, &render_pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport {
        .x = 0,
        .y = flip_y ? static_cast<float>(height) : 0.0f,
        .width = static_cast<float>(width),
        .height = flip_y ? -static_cast<float>(height) : static_cast<float>(height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor {
        .offset = { 0, 0 },
        .extent = { width, height },
    };
    vkCmdSetViewport(context.command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(context.command_buffer, 0, 1, &scissor);
    vkCmdBindPipeline(context.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_resources->pipeline);
    vkCmdBindDescriptorSets(context.command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_resources->pipeline_layout, 0, 1, &pipeline_resources->descriptor_set, 0, nullptr);
    vkCmdDraw(context.command_buffer, 3, 1, 0, 0);
    vkCmdEndRenderPass(context.command_buffer);

    VkImageMemoryBarrier post_render_barriers[] {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout = source_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = imported_source->image,
            .subresourceRange = { VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout = rgba_destination.info.layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = rgba_destination.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
    };
    vkCmdPipelineBarrier(context.command_buffer,
        VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,
        0, nullptr,
        0, nullptr,
        AK::array_size(post_render_barriers), post_render_barriers);

    result = vkEndCommandBuffer(context.command_buffer);
    if (result != VK_SUCCESS)
        return Error::from_string_literal("failed to end Vulkan video render command buffer");

    VkSubmitInfo submit_info {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &context.command_buffer,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    VkFence render_fence { context.command_fence };
    if (render_fence == VK_NULL_HANDLE) {
        VkFenceCreateInfo fence_info {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        };
        result = vkCreateFence(context.logical_device, &fence_info, nullptr, &render_fence);
        if (result != VK_SUCCESS)
            return Error::from_string_literal("failed to create Vulkan video render fence");
    } else {
        result = vkResetFences(context.logical_device, 1, &render_fence);
        if (result != VK_SUCCESS)
            return Error::from_string_literal("failed to reset Vulkan video render fence");
    }
    auto cleanup_render_fence = ScopeGuard([&] {
        if (render_fence != context.command_fence)
            vkDestroyFence(context.logical_device, render_fence, nullptr);
    });

    result = vkQueueSubmit(context.graphics_queue, 1, &submit_info, render_fence);
    if (result != VK_SUCCESS) {
        dbgln("MUNDO_VULKAN_VIDEO_NV12_RGBA_RENDER status=failed step=queue_submit result={} size={}x{}", to_underlying(result), width, height);
        return Error::from_string_literal("failed to submit Vulkan video render");
    }
    result = vkWaitForFences(context.logical_device, 1, &render_fence, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) {
        dbgln("MUNDO_VULKAN_VIDEO_NV12_RGBA_RENDER status=failed step=wait_fence result={} size={}x{}", to_underlying(result), width, height);
        return Error::from_string_literal("failed to wait for Vulkan video render");
    }

    static size_t s_render_success_count { 0 };
    auto success_count = ++s_render_success_count;
    if (success_count <= 8 || success_count % 120 == 0) {
        dbgln("MUNDO_VULKAN_VIDEO_NV12_RGBA_RENDER status=ok count={} size={}x{} source_format={} source_layout={} source_allocation_size={} handle_type={} source_image={} rgba_image={} flip_y={}",
            success_count,
            width,
            height,
            to_underlying(source_format),
            to_underlying(source_layout),
            source_allocation_size,
            to_underlying(source_handle_type),
            reinterpret_cast<uintptr_t>(imported_source->image),
            reinterpret_cast<uintptr_t>(rgba_destination.image),
            flip_y);
    }
    return {};
}

ErrorOr<void> copy_vulkan_nv12_external_memory_planes_to_opaque_images(VulkanContext const& context, int source_fd, VkExternalMemoryHandleTypeFlagBits source_handle_type, VkDeviceSize source_allocation_size, uint32_t width, uint32_t height, VkFormat source_format, VkImageLayout source_layout, VulkanImage& y_destination, VulkanImage& uv_destination)
{
    auto imported_source = TRY(import_vulkan_nv12_external_memory(context, source_fd, source_handle_type, source_allocation_size, width, height, source_format, source_layout));
    if (y_destination.info.format != VK_FORMAT_R8_UNORM || uv_destination.info.format != VK_FORMAT_R8G8_UNORM)
        return Error::from_string_literal("destination Vulkan images are not R8/RG8 NV12 planes");

    auto source_image = imported_source->image;
    auto source_memory_requirements_size = imported_source->required_size;

    vkResetCommandBuffer(context.command_buffer, 0);
    VkCommandBufferBeginInfo begin_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    auto result = vkBeginCommandBuffer(context.command_buffer, &begin_info);
    if (result != VK_SUCCESS)
        return Error::from_string_literal("failed to begin Vulkan NV12 plane copy command buffer");

    VkImageMemoryBarrier pre_copy_barriers[] {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .oldLayout = source_layout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source_image,
            .subresourceRange = { VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = y_destination.info.layout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = y_destination.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .oldLayout = uv_destination.info.layout,
            .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = uv_destination.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
    };
    vkCmdPipelineBarrier(context.command_buffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        0,
        0, nullptr,
        0, nullptr,
        AK::array_size(pre_copy_barriers), pre_copy_barriers);

    VkImageCopy y_copy_region {
        .srcSubresource = { VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1 },
        .srcOffset = { 0, 0, 0 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffset = { 0, 0, 0 },
        .extent = { width, height, 1 },
    };
    vkCmdCopyImage(context.command_buffer, source_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, y_destination.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &y_copy_region);

    VkImageCopy uv_copy_region {
        .srcSubresource = { VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1 },
        .srcOffset = { 0, 0, 0 },
        .dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .dstOffset = { 0, 0, 0 },
        .extent = { (width + 1) / 2, (height + 1) / 2, 1 },
    };
    vkCmdCopyImage(context.command_buffer, source_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, uv_destination.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &uv_copy_region);

    VkImageMemoryBarrier post_copy_barriers[] {
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            .newLayout = source_layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source_image,
            .subresourceRange = { VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = y_destination.info.layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = y_destination.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            .newLayout = uv_destination.info.layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = uv_destination.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
    };
    vkCmdPipelineBarrier(context.command_buffer,
        VK_PIPELINE_STAGE_TRANSFER_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,
        0, nullptr,
        0, nullptr,
        AK::array_size(post_copy_barriers), post_copy_barriers);

    result = vkEndCommandBuffer(context.command_buffer);
    if (result != VK_SUCCESS)
        return Error::from_string_literal("failed to end Vulkan NV12 plane copy command buffer");

    VkSubmitInfo submit_info {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &context.command_buffer,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    VkFence copy_fence { context.command_fence };
    if (copy_fence == VK_NULL_HANDLE) {
        VkFenceCreateInfo fence_info {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
        };
        result = vkCreateFence(context.logical_device, &fence_info, nullptr, &copy_fence);
        if (result != VK_SUCCESS) {
            dbgln("MUNDO_VULKAN_VIDEO_PLANE_COPY status=failed step=create_fence result={} size={}x{}", to_underlying(result), width, height);
            return Error::from_string_literal("failed to create Vulkan NV12 plane copy fence");
        }
    } else {
        result = vkResetFences(context.logical_device, 1, &copy_fence);
        if (result != VK_SUCCESS) {
            dbgln("MUNDO_VULKAN_VIDEO_PLANE_COPY status=failed step=reset_fence result={} size={}x{}", to_underlying(result), width, height);
            return Error::from_string_literal("failed to reset Vulkan NV12 plane copy fence");
        }
    }
    auto cleanup_copy_fence = ScopeGuard([&] {
        if (copy_fence != context.command_fence)
            vkDestroyFence(context.logical_device, copy_fence, nullptr);
    });

    result = vkQueueSubmit(context.graphics_queue, 1, &submit_info, copy_fence);
    if (result != VK_SUCCESS) {
        dbgln("MUNDO_VULKAN_VIDEO_PLANE_COPY status=failed step=queue_submit result={} size={}x{}", to_underlying(result), width, height);
        return Error::from_string_literal("failed to submit Vulkan NV12 plane copy");
    }
    result = vkWaitForFences(context.logical_device, 1, &copy_fence, VK_TRUE, UINT64_MAX);
    if (result != VK_SUCCESS) {
        dbgln("MUNDO_VULKAN_VIDEO_PLANE_COPY status=failed step=wait_fence result={} size={}x{}", to_underlying(result), width, height);
        return Error::from_string_literal("failed to wait for Vulkan NV12 plane copy");
    }

    static size_t s_plane_copy_success_count { 0 };
    auto success_count = ++s_plane_copy_success_count;
    if (success_count <= 8 || success_count % 120 == 0) {
        dbgln("MUNDO_VULKAN_VIDEO_PLANE_COPY status=ok count={} size={}x{} source_format={} source_layout={} source_allocation_size={} required_size={} handle_type={} y_image={} uv_image={}",
            success_count,
            width,
            height,
            to_underlying(source_format),
            to_underlying(source_layout),
            source_allocation_size,
            source_memory_requirements_size,
            to_underlying(source_handle_type),
            reinterpret_cast<uintptr_t>(y_destination.image),
            reinterpret_cast<uintptr_t>(uv_destination.image));
    }
    return {};
}

ErrorOr<NonnullRefPtr<VulkanImage>> create_opaque_fd_vulkan_image(VulkanContext const& context, uint32_t width, uint32_t height, VkFormat format)
{
    NonnullRefPtr<VulkanImage> image = make_ref_counted<VulkanImage>(context);

    VkExternalMemoryImageCreateInfo external_mem_image_info = {
        .sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO,
        .pNext = nullptr,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    Array<uint32_t, 1> queue_families = { context.graphics_queue_family };
    VkImageCreateInfo image_info = {
        .sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO,
        .pNext = &external_mem_image_info,
        .flags = 0,
        .imageType = VK_IMAGE_TYPE_2D,
        .format = format,
        .extent = {
            .width = width,
            .height = height,
            .depth = 1,
        },
        .mipLevels = 1,
        .arrayLayers = 1,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .tiling = VK_IMAGE_TILING_OPTIMAL,
        .usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
        .queueFamilyIndexCount = queue_families.size(),
        .pQueueFamilyIndices = queue_families.data(),
        .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED,
    };
    auto result = vkCreateImage(context.logical_device, &image_info, nullptr, &image->image);
    if (result != VK_SUCCESS) {
        dbgln("vkCreateImage returned {} for OPAQUE_FD", to_underlying(result));
        return Error::from_string_literal("opaque image creation failed");
    }

    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(context.logical_device, image->image, &mem_reqs);
    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(context.physical_device, &mem_props);

    uint32_t mem_type_idx = find_memory_type_index(mem_props, mem_reqs, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (mem_type_idx == mem_props.memoryTypeCount)
        return Error::from_string_literal("unable to find suitable opaque image memory type");

    VkMemoryDedicatedAllocateInfo mem_dedicated_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO,
        .pNext = nullptr,
        .image = image->image,
        .buffer = VK_NULL_HANDLE,
    };

    VkExportMemoryAllocateInfo export_mem_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO,
        .pNext = &mem_dedicated_alloc_info,
        .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
    };
    VkMemoryAllocateInfo mem_alloc_info = {
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .pNext = &export_mem_alloc_info,
        .allocationSize = mem_reqs.size,
        .memoryTypeIndex = mem_type_idx,
    };
    result = vkAllocateMemory(context.logical_device, &mem_alloc_info, nullptr, &image->memory);
    if (result != VK_SUCCESS) {
        dbgln("vkAllocateMemory returned {} for OPAQUE_FD", to_underlying(result));
        return Error::from_string_literal("opaque image memory allocation failed");
    }

    result = vkBindImageMemory(context.logical_device, image->image, image->memory, 0);
    if (result != VK_SUCCESS) {
        dbgln("vkBindImageMemory returned {} for OPAQUE_FD", to_underlying(result));
        return Error::from_string_literal("bind opaque image memory failed");
    }

    VkImageLayout layout = VK_IMAGE_LAYOUT_GENERAL;
    image->transition_layout(VK_IMAGE_LAYOUT_UNDEFINED, layout);

    image->info = {
        .format = image_info.format,
        .extent = image_info.extent,
        .tiling = image_info.tiling,
        .usage = image_info.usage,
        .sharing_mode = image_info.sharingMode,
        .layout = layout,
        .row_pitch = 0,
        .allocation_size = mem_reqs.size,
        .modifier = DRM_FORMAT_MOD_INVALID,
    };
    return image;
}

}

#endif
