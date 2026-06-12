/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/Array.h>
#include <AK/RefPtr.h>
#include <AK/StringView.h>
#include <AK/Vector.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>
#include <LibMedia/VideoFrame.h>
#include <LibWeb/Export.h>

#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <LibGfx/VulkanImage.h>
#endif

namespace Web::WebGL {

class WEB_API OpenGLContext {
public:
    enum class WebGLVersion {
        WebGL1,
        WebGL2,
    };

    struct DrawingBufferOptions {
        bool depth;
        bool stencil;
        bool antialias;
    };

    static OwnPtr<OpenGLContext> create(NonnullRefPtr<Gfx::SkiaBackendContext>, WebGLVersion, DrawingBufferOptions);

    void notify_content_will_change();
    void clear_buffer_to_default_values();
    void allocate_painting_surface_if_needed();

    struct Impl;
    OpenGLContext(NonnullRefPtr<Gfx::SkiaBackendContext>, Impl, WebGLVersion, DrawingBufferOptions);

    ~OpenGLContext();

    void make_current();

    void present(bool preserve_drawing_buffer);
    void note_gl_draw_submitted();
    void note_direct_vulkan_video_draw_submitted();
    bool has_direct_vulkan_video_draw_pending_gl_present() const;

    void set_size(Gfx::IntSize const&);

    RefPtr<Gfx::PaintingSurface> surface();

    u32 default_framebuffer() const;
    u32 default_renderbuffer() const;

    Vector<String> get_supported_opengl_extensions();
    void request_extension(char const* extension_name);

    WebGLVersion webgl_version() const { return m_webgl_version; }

#ifdef USE_VULKAN_DMABUF_IMAGES
    struct ImportedVideoOpaqueFDTexture {
        NonnullRefPtr<Gfx::VulkanImage> image;
        u32 memory_object { 0 };
        u32 texture { 0 };
        u32 width { 0 };
        u32 height { 0 };
        u64 allocation_size { 0 };
        bool owns_texture { true };
    };
    struct ImportedVideoOpaqueFDTexturePair {
        ImportedVideoOpaqueFDTexture* y { nullptr };
        ImportedVideoOpaqueFDTexture* uv { nullptr };
        bool reused_y { false };
        bool reused_uv { false };
    };
    struct SkiaVulkanYcbcrProbeResult {
        bool attempted { false };
        bool supported { false };
        StringView reason { "not_attempted"sv };
        bool backend_texture_valid { false };
        bool backend_format_valid { false };
        bool backend_format_has_ycbcr { false };
        bool promise_format_valid { false };
        bool promise_image_created { false };
    };
    struct GLExternalVideoImportProbeResult {
        bool attempted { false };
        bool y_plane_supported { false };
        bool uv_plane_supported { false };
        StringView y_plane_reason { "not_attempted"sv };
        StringView uv_plane_reason { "not_attempted"sv };
        u32 uv_plane_gl_error { 0 };
    };
    struct RetainedVulkanVideoSourceProbeResult {
        bool attempted { false };
        bool supported { false };
        bool direct_sample_ready { false };
        StringView reason { "not_attempted"sv };
        u64 required_size { 0 };
        u64 allocation_size { 0 };
    };
    struct VulkanVideoReplayBufferProbeResult {
        bool attempted { false };
        bool supported { false };
        StringView reason { "not_attempted"sv };
        u64 total_bytes { 0 };
    };
    struct VulkanVideoMeshPipelineProbeResult {
        bool attempted { false };
        bool supported { false };
        bool executed { false };
        StringView reason { "not_attempted"sv };
    };
    struct VulkanVideoMeshUniformSnapshot {
        bool has_model_view_matrix { false };
        bool has_projection_matrix { false };
        Array<float, 16> model_view_matrix {};
        Array<float, 16> projection_matrix {};
        float opacity { 1.0f };
        float output_intensity { 1.0f };
        float stereo_eye { 0.0f };
        float stereo_eye_left { 1.0f };
    };
    struct VulkanSolidMeshUniformSnapshot {
        bool has_model_view_matrix { false };
        bool has_projection_matrix { false };
        Array<float, 16> model_view_matrix {};
        Array<float, 16> projection_matrix {};
        Array<float, 4> diffuse { 1.0f, 1.0f, 1.0f, 1.0f };
        float opacity { 1.0f };
        float output_intensity { 1.0f };
    };

    void probe_video_opaque_fd_texture_import(u32 width, u32 height, u32 uv_width, u32 uv_height, size_t log_count);
    ErrorOr<ImportedVideoOpaqueFDTexture> create_imported_video_opaque_fd_texture(u32 width, u32 height, u32 vulkan_format, u32 gl_internal_format, char const* label, size_t log_count);
    ErrorOr<ImportedVideoOpaqueFDTexturePair> get_or_create_imported_video_opaque_fd_textures(u32 width, u32 height, u32 uv_width, u32 uv_height, size_t log_count);
    ErrorOr<ImportedVideoOpaqueFDTexture*> get_or_create_imported_video_rgba_texture(u32 width, u32 height, size_t log_count);
    ErrorOr<ImportedVideoOpaqueFDTexture*> get_or_create_imported_video_rgba_target_texture(u32 target_texture, u32 width, u32 height, size_t log_count);
    ErrorOr<ImportedVideoOpaqueFDTexture*> get_or_create_vulkan_rgba_render_target_image(u32 target_texture, u32 width, u32 height, size_t log_count);
    ErrorOr<u64> copy_vulkan_nv12_external_memory_to_imported_video_textures(Media::HardwareVideoFrameExternalMemoryDescriptor const&, ImportedVideoOpaqueFDTexturePair const&, size_t log_count);
    ErrorOr<u64> render_vulkan_nv12_external_memory_to_imported_video_rgba_texture(Media::HardwareVideoFrameExternalMemoryDescriptor const&, ImportedVideoOpaqueFDTexture const&, size_t log_count, bool flip_y = false);
    SkiaVulkanYcbcrProbeResult probe_skia_vulkan_ycbcr_texture_import(Media::HardwareVideoFrameExternalMemoryDescriptor const&, size_t log_count);
    GLExternalVideoImportProbeResult probe_video_external_memory_gl_texture_import(Media::HardwareVideoFrameExternalMemoryDescriptor const&, size_t log_count);
    ErrorOr<NonnullOwnPtr<Gfx::ImportedVulkanNV12Image>> import_retained_vulkan_video_source_for_virtual_draw(int source_opaque_fd, u32 source_handle_type, u64 source_allocation_size, u32 width, u32 height, u32 source_format, u32 source_layout);
    RetainedVulkanVideoSourceProbeResult probe_retained_vulkan_video_source_for_virtual_draw(int source_opaque_fd, u32 source_handle_type, u64 source_allocation_size, u32 width, u32 height, u32 source_format, u32 source_layout, u64 frame_id, size_t log_count);
    VulkanVideoReplayBufferProbeResult probe_vulkan_video_replay_buffers(ReadonlyBytes position_data, ReadonlyBytes uv_data, ReadonlyBytes uv_right_data, ReadonlyBytes index_data, u64 frame_id, size_t log_count);
    VulkanVideoMeshPipelineProbeResult probe_vulkan_video_mesh_pipeline(u64 frame_id, u32 destination_format, VkImage source_image, VkImageView source_image_view, VkSampler immutable_sampler, u32 source_layout, VulkanVideoMeshUniformSnapshot const&, u32 draw_count, u32 draw_type, u64 draw_offset, int viewport_x, int viewport_y, int viewport_width, int viewport_height, size_t log_count);
    VulkanVideoMeshPipelineProbeResult probe_vulkan_solid_mesh_pipeline(u32 destination_format, VulkanSolidMeshUniformSnapshot const&, ReadonlyBytes position_data, ReadonlyBytes index_data, u32 draw_count, u32 draw_type, u64 draw_offset, int viewport_x, int viewport_y, int viewport_width, int viewport_height, size_t log_count, Gfx::VulkanImage* target_image_override = nullptr);
    VulkanVideoMeshPipelineProbeResult probe_vulkan_colored_mesh_pipeline(u32 destination_format, VulkanSolidMeshUniformSnapshot const&, ReadonlyBytes position_data, ReadonlyBytes color_data, ReadonlyBytes index_data, u32 draw_count, u32 draw_type, u64 draw_offset, int viewport_x, int viewport_y, int viewport_width, int viewport_height, size_t log_count, Gfx::VulkanImage* target_image_override = nullptr);
    Optional<u32> vulkan_painting_surface_format() const;
    void delete_imported_video_opaque_fd_texture(ImportedVideoOpaqueFDTexture&);
#endif

private:
    NonnullRefPtr<Gfx::SkiaBackendContext> m_skia_backend_context;
    Gfx::IntSize m_size;
    RefPtr<Gfx::PaintingSurface> m_painting_surface;
#ifdef AK_OS_MACOS
    OwnPtr<Gfx::SharedImageBuffer> m_shared_image_buffer;
#endif
    NonnullOwnPtr<Impl> m_impl;
    Optional<Vector<String>> m_requestable_extensions;
    WebGLVersion m_webgl_version;
    [[maybe_unused]] DrawingBufferOptions m_drawing_buffer_options;

    void free_surface_resources();
#if defined(AK_OS_MACOS)
    void allocate_iosurface_painting_surface();
#elif defined(USE_VULKAN_DMABUF_IMAGES)
    void allocate_vkimage_painting_surface();
#endif
};

}
