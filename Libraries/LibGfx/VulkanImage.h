/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#ifdef USE_VULKAN_DMABUF_IMAGES

#    include <AK/Assertions.h>
#    include <AK/NonnullRefPtr.h>
#    include <AK/NonnullOwnPtr.h>
#    include <AK/RefCounted.h>
#    include <AK/Span.h>
#    include <LibGfx/VulkanContext.h>
#    include <libdrm/drm_fourcc.h>

namespace Gfx {

struct VulkanBuffer {
    VulkanContext const& context;
    VkBuffer buffer { VK_NULL_HANDLE };
    VkDeviceMemory memory { VK_NULL_HANDLE };
    VkDeviceSize size { 0 };
    VkBufferUsageFlags usage { 0 };

    explicit VulkanBuffer(VulkanContext const& context)
        : context(context)
    {
    }
    ~VulkanBuffer();
};

struct VulkanImage : public RefCounted<VulkanImage> {
    VkImage image { VK_NULL_HANDLE };
    VkDeviceMemory memory { VK_NULL_HANDLE };
    VkImageView cached_video_color_attachment_view { VK_NULL_HANDLE };
    VkFramebuffer cached_video_framebuffer { VK_NULL_HANDLE };
    VkRenderPass cached_video_framebuffer_render_pass { VK_NULL_HANDLE };
    uint32_t cached_video_framebuffer_width { 0 };
    uint32_t cached_video_framebuffer_height { 0 };
    struct {
        VkFormat format;
        VkExtent3D extent;
        VkImageTiling tiling;
        VkImageUsageFlags usage;
        VkSharingMode sharing_mode;
        VkImageLayout layout;
        VkDeviceSize row_pitch; // for tiled images this is some implementation-specific value
        VkDeviceSize allocation_size;
        uint64_t modifier { DRM_FORMAT_MOD_INVALID };
    } info;
    VulkanContext const& context;

    int get_dma_buf_fd() const;
    int get_opaque_fd() const;
    void transition_layout(VkImageLayout old_layout, VkImageLayout new_layout);
    VulkanImage(VulkanContext const& context)
        : context(context)
    {
    }
    ~VulkanImage();
};

struct ImportedVulkanNV12Image {
    VulkanContext const& context;
    VkImage image { VK_NULL_HANDLE };
    VkDeviceMemory memory { VK_NULL_HANDLE };
    VkImageView ycbcr_image_view { VK_NULL_HANDLE };
    VkSampler ycbcr_sampler { VK_NULL_HANDLE };
    VkSamplerYcbcrConversion ycbcr_conversion { VK_NULL_HANDLE };
    VkFormat format { VK_FORMAT_UNDEFINED };
    VkImageLayout layout { VK_IMAGE_LAYOUT_UNDEFINED };
    VkDeviceSize allocation_size { 0 };
    VkDeviceSize required_size { 0 };
    VkExternalMemoryHandleTypeFlagBits handle_type {};
    uint32_t width { 0 };
    uint32_t height { 0 };
    bool direct_sample_ready { false };
    bool owns_ycbcr_resources { true };

    explicit ImportedVulkanNV12Image(VulkanContext const& context)
        : context(context)
    {
    }
    ~ImportedVulkanNV12Image();
};

static inline uint32_t vk_format_to_drm_format(VkFormat format)
{
    switch (format) {
    case VK_FORMAT_B8G8R8A8_UNORM:
        return DRM_FORMAT_ARGB8888;
    // add more as needed
    default:
        VERIFY_NOT_REACHED();
        return DRM_FORMAT_INVALID;
    }
}

ErrorOr<NonnullRefPtr<VulkanImage>> create_shared_vulkan_image(VulkanContext const& context, uint32_t width, uint32_t height, VkFormat format, ReadonlySpan<uint64_t> modifiers);
ErrorOr<NonnullRefPtr<VulkanImage>> create_opaque_fd_vulkan_image(VulkanContext const& context, uint32_t width, uint32_t height, VkFormat format);
ErrorOr<NonnullOwnPtr<VulkanBuffer>> create_host_visible_vulkan_buffer_from_bytes(VulkanContext const& context, ReadonlyBytes bytes, VkBufferUsageFlags usage);
ErrorOr<NonnullOwnPtr<ImportedVulkanNV12Image>> import_vulkan_nv12_external_memory(VulkanContext const& context, int source_fd, VkExternalMemoryHandleTypeFlagBits source_handle_type, VkDeviceSize source_allocation_size, uint32_t width, uint32_t height, VkFormat source_format, VkImageLayout source_layout);
ErrorOr<void> copy_vulkan_nv12_external_memory_planes_to_opaque_images(VulkanContext const& context, int source_fd, VkExternalMemoryHandleTypeFlagBits source_handle_type, VkDeviceSize source_allocation_size, uint32_t width, uint32_t height, VkFormat source_format, VkImageLayout source_layout, VulkanImage& y_destination, VulkanImage& uv_destination);
ErrorOr<void> render_vulkan_nv12_external_memory_to_opaque_rgba_image(VulkanContext const& context, int source_fd, VkExternalMemoryHandleTypeFlagBits source_handle_type, VkDeviceSize source_allocation_size, uint32_t width, uint32_t height, VkFormat source_format, VkImageLayout source_layout, VulkanImage& rgba_destination, bool flip_y = false);

}

#endif
