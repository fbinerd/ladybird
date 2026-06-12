/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/NonnullOwnPtr.h>
#include <LibWeb/WebGL/Types.h>
#include <LibWeb/WebGL/WebGLObject.h>

#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <LibGfx/VulkanImage.h>
#endif

namespace Web::WebGL {

class WebGLTexture final : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLTexture, WebGLObject);
    GC_DECLARE_ALLOCATOR(WebGLTexture);

public:
    static GC::Ref<WebGLTexture> create(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase>, GLuint handle);

    virtual ~WebGLTexture();

    struct HardwareVideoBacking {
        u64 frame_id { 0 };
        u32 width { 0 };
        u32 height { 0 };
        char const* backend { "none" };
        char const* upload_mode { "none" };
        char const* copy_stage { "none" };
        bool direct_zero_copy { false };
        bool copied_on_gpu { false };
        char const* direct_sampling_route { "none" };
        char const* direct_sampling_reason { "none" };
        u32 source_vulkan_format { 0 };
        u32 source_vulkan_layout { 0 };
        u64 source_allocation_size { 0 };
        u32 source_handle_type { 0 };
        int source_opaque_fd { -1 };
        bool source_single_optimal_multiplanar { false };
    };

    void set_hardware_video_backing(HardwareVideoBacking);
    void clear_hardware_video_backing();
    bool has_hardware_video_backing() const { return m_hardware_video_backing.has_value(); }
    Optional<HardwareVideoBacking> const& hardware_video_backing() const { return m_hardware_video_backing; }

    struct MundoTextureUploadSnapshot {
        u32 width { 0 };
        u32 height { 0 };
        u32 internal_format { 0 };
        u32 format { 0 };
        u32 type { 0 };
        size_t byte_length { 0 };
        bool complete { false };
        ByteBuffer pixels;
    };

    void set_mundo_texture_upload_snapshot(u32 width, u32 height, u32 internal_format, u32 format, u32 type, ReadonlyBytes pixels);
    void set_mundo_texture_upload_snapshot_incomplete(u32 width, u32 height, u32 internal_format, u32 format, u32 type, size_t byte_length);
    bool update_mundo_texture_upload_snapshot_region(i32 xoffset, i32 yoffset, u32 width, u32 height, u32 format, u32 type, ReadonlyBytes pixels);
    void mark_mundo_texture_upload_snapshot_incomplete();
    void clear_mundo_texture_upload_snapshot();
    Optional<MundoTextureUploadSnapshot> const& mundo_texture_upload_snapshot() const { return m_mundo_texture_upload_snapshot; }

    struct MundoRenderTargetWriteState {
        size_t write_count { 0 };
        u32 last_viewport_width { 0 };
        u32 last_viewport_height { 0 };
        u32 last_program { 0 };
    };

    void mark_mundo_render_target_written(u32 viewport_width, u32 viewport_height, u32 program);
    Optional<MundoRenderTargetWriteState> const& mundo_render_target_write_state() const { return m_mundo_render_target_write_state; }
#ifdef USE_VULKAN_DMABUF_IMAGES
    void set_cached_virtual_vulkan_video_source(u64 frame_id, NonnullOwnPtr<Gfx::ImportedVulkanNV12Image>);
    Gfx::ImportedVulkanNV12Image const* cached_virtual_vulkan_video_source() const { return m_cached_virtual_vulkan_video_source.ptr(); }
    u64 cached_virtual_vulkan_video_source_frame_id() const { return m_cached_virtual_vulkan_video_source_frame_id; }
#endif

protected:
    explicit WebGLTexture(JS::Realm&, GC::Ref<WebGLRenderingContextBase>, GLuint handle);

    virtual void initialize(JS::Realm&) override;

private:
    Optional<HardwareVideoBacking> m_hardware_video_backing;
    Optional<MundoTextureUploadSnapshot> m_mundo_texture_upload_snapshot;
    Optional<MundoRenderTargetWriteState> m_mundo_render_target_write_state;
#ifdef USE_VULKAN_DMABUF_IMAGES
    OwnPtr<Gfx::ImportedVulkanNV12Image> m_cached_virtual_vulkan_video_source;
    u64 m_cached_virtual_vulkan_video_source_frame_id { 0 };
#endif
};

}
