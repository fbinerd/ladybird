/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Error.h>
#include <AK/NonnullOwnPtr.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefPtr.h>
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

    void probe_video_opaque_fd_texture_import(u32 width, u32 height, u32 uv_width, u32 uv_height, size_t log_count);
    ErrorOr<ImportedVideoOpaqueFDTexture> create_imported_video_opaque_fd_texture(u32 width, u32 height, u32 vulkan_format, u32 gl_internal_format, char const* label, size_t log_count);
    ErrorOr<ImportedVideoOpaqueFDTexturePair> get_or_create_imported_video_opaque_fd_textures(u32 width, u32 height, u32 uv_width, u32 uv_height, size_t log_count);
    ErrorOr<ImportedVideoOpaqueFDTexture*> get_or_create_imported_video_rgba_texture(u32 width, u32 height, size_t log_count);
    ErrorOr<ImportedVideoOpaqueFDTexture*> get_or_create_imported_video_rgba_target_texture(u32 target_texture, u32 width, u32 height, size_t log_count);
    ErrorOr<u64> copy_vulkan_nv12_external_memory_to_imported_video_textures(Media::HardwareVideoFrameExternalMemoryDescriptor const&, ImportedVideoOpaqueFDTexturePair const&, size_t log_count);
    ErrorOr<u64> render_vulkan_nv12_external_memory_to_imported_video_rgba_texture(Media::HardwareVideoFrameExternalMemoryDescriptor const&, ImportedVideoOpaqueFDTexture const&, size_t log_count, bool flip_y = false);
    void probe_skia_vulkan_ycbcr_texture_import(Media::HardwareVideoFrameExternalMemoryDescriptor const&, size_t log_count);
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
