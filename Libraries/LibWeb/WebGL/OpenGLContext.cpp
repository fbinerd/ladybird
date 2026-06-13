/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashFunctions.h>
#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <AK/Vector.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImageBuffer.h>
#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <LibGfx/VulkanImage.h>
#endif
#include <LibWeb/WebGL/OpenGLContext.h>
#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <LibWeb/WebGL/MundoColoredMeshShaders.inc>
#    include <LibWeb/WebGL/MundoSolidMeshShaders.inc>
#    include <LibWeb/WebGL/MundoTexturedAlphaMeshShaders.inc>
#    include <LibWeb/WebGL/MundoTexturedMeshShaders.inc>
#endif

#include <EGL/egl.h>
#include <EGL/eglext.h>
#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <core/SkColorSpace.h>
#    include <core/SkImage.h>
#    include <core/SkYUVAInfo.h>
#    include <gpu/ganesh/GrBackendSurface.h>
#    include <gpu/ganesh/GrContextThreadSafeProxy.h>
#    include <gpu/ganesh/GrDirectContext.h>
#    include <gpu/ganesh/GrYUVABackendTextures.h>
#    include <gpu/ganesh/SkImageGanesh.h>
#    include <gpu/ganesh/vk/GrVkBackendSurface.h>
#    include <gpu/ganesh/vk/GrVkTypes.h>
#    include <private/chromium/GrPromiseImageTexture.h>
#    include <private/chromium/SkImageChromium.h>
#endif
#define EGL_EGLEXT_PROTOTYPES 1
extern "C" {
#include <EGL/eglext_angle.h>
}
#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
extern "C" {
#include <GLES2/gl2ext_angle.h>
}
#include <GLES3/gl3.h>

#ifndef GL_R8_EXT
#    define GL_R8_EXT 0x8229
#endif
#ifndef GL_RG8_EXT
#    define GL_RG8_EXT 0x822B
#endif

#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <ffnvcodec/dynlink_cuda.h>
#    include <dlfcn.h>
#    include <stdlib.h>
#    include <unistd.h>
#endif

// Enable WebGL if we're on MacOS and can use Metal or if we can use shareable Vulkan images
#if defined(AK_OS_MACOS) || defined(USE_VULKAN_DMABUF_IMAGES)
#    define ENABLE_WEBGL 1
#endif

namespace Web::WebGL {

#ifdef USE_VULKAN_DMABUF_IMAGES
static bool mundo_webgl_video_direct_vulkan_mesh_enabled()
{
    auto const* value = getenv("MUNDO_WEBGL_VIDEO_DIRECT_VULKAN_MESH");
    if (!value)
        return true;

    auto view = StringView { value, strlen(value) };
    if (view == "0"sv || view == "false"sv || view == "off"sv || view == "no"sv)
        return false;

    return view == "1"sv || view == "auto"sv;
}

static constexpr u32 s_mundo_video_nv12_mesh_vertex_shader_spirv[] {
    0x07230203, 0x00010000, 0x0008000b, 0x0000004b, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x000a000f, 0x00000000, 0x00000004, 0x6e69616d, 0x00000000, 0x0000000c, 0x00000030, 0x00000036,
    0x00000044, 0x00000046, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d,
    0x00000000, 0x00050005, 0x00000009, 0x69736f70, 0x6e6f6974, 0x00000000, 0x00050005, 0x0000000c,
    0x6f705f61, 0x69746973, 0x00006e6f, 0x00070005, 0x00000014, 0x65646956, 0x7375506f, 0x6e6f4368,
    0x6e617473, 0x00007374, 0x00080006, 0x00000014, 0x00000000, 0x65646f6d, 0x69765f6c, 0x6d5f7765,
    0x69727461, 0x00000078, 0x00080006, 0x00000014, 0x00000001, 0x6a6f7270, 0x69746365, 0x6d5f6e6f,
    0x69727461, 0x00000078, 0x00070006, 0x00000014, 0x00000002, 0x5f657375, 0x7274616d, 0x73656369,
    0x00000000, 0x00050006, 0x00000014, 0x00000003, 0x6361706f, 0x00797469, 0x00080006, 0x00000014,
    0x00000004, 0x7074756f, 0x695f7475, 0x6e65746e, 0x79746973, 0x00000000, 0x00060006, 0x00000014,
    0x00000005, 0x72657473, 0x655f6f65, 0x00006579, 0x00070006, 0x00000014, 0x00000006, 0x72657473,
    0x655f6f65, 0x6c5f6579, 0x00746665, 0x00030005, 0x00000016, 0x00006370, 0x00060005, 0x0000002e,
    0x505f6c67, 0x65567265, 0x78657472, 0x00000000, 0x00060006, 0x0000002e, 0x00000000, 0x505f6c67,
    0x7469736f, 0x006e6f69, 0x00070006, 0x0000002e, 0x00000001, 0x505f6c67, 0x746e696f, 0x657a6953,
    0x00000000, 0x00070006, 0x0000002e, 0x00000002, 0x435f6c67, 0x4470696c, 0x61747369, 0x0065636e,
    0x00070006, 0x0000002e, 0x00000003, 0x435f6c67, 0x446c6c75, 0x61747369, 0x0065636e, 0x00030005,
    0x00000030, 0x00000000, 0x00040005, 0x00000036, 0x76755f76, 0x00000000, 0x00050005, 0x00000044,
    0x76755f61, 0x6769725f, 0x00007468, 0x00040005, 0x00000046, 0x76755f61, 0x00000000, 0x00040047,
    0x0000000c, 0x0000001e, 0x00000000, 0x00030047, 0x00000014, 0x00000002, 0x00040048, 0x00000014,
    0x00000000, 0x00000005, 0x00050048, 0x00000014, 0x00000000, 0x00000007, 0x00000010, 0x00050048,
    0x00000014, 0x00000000, 0x00000023, 0x00000000, 0x00040048, 0x00000014, 0x00000001, 0x00000005,
    0x00050048, 0x00000014, 0x00000001, 0x00000007, 0x00000010, 0x00050048, 0x00000014, 0x00000001,
    0x00000023, 0x00000040, 0x00050048, 0x00000014, 0x00000002, 0x00000023, 0x00000080, 0x00050048,
    0x00000014, 0x00000003, 0x00000023, 0x00000084, 0x00050048, 0x00000014, 0x00000004, 0x00000023,
    0x00000088, 0x00050048, 0x00000014, 0x00000005, 0x00000023, 0x0000008c, 0x00050048, 0x00000014,
    0x00000006, 0x00000023, 0x00000090, 0x00030047, 0x0000002e, 0x00000002, 0x00050048, 0x0000002e,
    0x00000000, 0x0000000b, 0x00000000, 0x00050048, 0x0000002e, 0x00000001, 0x0000000b, 0x00000001,
    0x00050048, 0x0000002e, 0x00000002, 0x0000000b, 0x00000003, 0x00050048, 0x0000002e, 0x00000003,
    0x0000000b, 0x00000004, 0x00040047, 0x00000036, 0x0000001e, 0x00000000, 0x00040047, 0x00000044,
    0x0000001e, 0x00000002, 0x00040047, 0x00000046, 0x0000001e, 0x00000001, 0x00020013, 0x00000002,
    0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020, 0x00040017, 0x00000007,
    0x00000006, 0x00000004, 0x00040020, 0x00000008, 0x00000007, 0x00000007, 0x00040017, 0x0000000a,
    0x00000006, 0x00000003, 0x00040020, 0x0000000b, 0x00000001, 0x0000000a, 0x0004003b, 0x0000000b,
    0x0000000c, 0x00000001, 0x0004002b, 0x00000006, 0x0000000e, 0x3f800000, 0x00040018, 0x00000013,
    0x00000007, 0x00000004, 0x0009001e, 0x00000014, 0x00000013, 0x00000013, 0x00000006, 0x00000006,
    0x00000006, 0x00000006, 0x00000006, 0x00040020, 0x00000015, 0x00000009, 0x00000014, 0x0004003b,
    0x00000015, 0x00000016, 0x00000009, 0x00040015, 0x00000017, 0x00000020, 0x00000001, 0x0004002b,
    0x00000017, 0x00000018, 0x00000002, 0x00040020, 0x00000019, 0x00000009, 0x00000006, 0x0004002b,
    0x00000006, 0x0000001c, 0x3f000000, 0x00020014, 0x0000001d, 0x0004002b, 0x00000017, 0x00000021,
    0x00000001, 0x00040020, 0x00000022, 0x00000009, 0x00000013, 0x0004002b, 0x00000017, 0x00000025,
    0x00000000, 0x00040015, 0x0000002b, 0x00000020, 0x00000000, 0x0004002b, 0x0000002b, 0x0000002c,
    0x00000001, 0x0004001c, 0x0000002d, 0x00000006, 0x0000002c, 0x0006001e, 0x0000002e, 0x00000007,
    0x00000006, 0x0000002d, 0x0000002d, 0x00040020, 0x0000002f, 0x00000003, 0x0000002e, 0x0004003b,
    0x0000002f, 0x00000030, 0x00000003, 0x00040020, 0x00000032, 0x00000003, 0x00000007, 0x00040017,
    0x00000034, 0x00000006, 0x00000002, 0x00040020, 0x00000035, 0x00000003, 0x00000034, 0x0004003b,
    0x00000035, 0x00000036, 0x00000003, 0x0004002b, 0x00000017, 0x00000037, 0x00000005, 0x0004002b,
    0x00000017, 0x0000003e, 0x00000006, 0x00040020, 0x00000043, 0x00000001, 0x00000034, 0x0004003b,
    0x00000043, 0x00000044, 0x00000001, 0x0004003b, 0x00000043, 0x00000046, 0x00000001, 0x00040017,
    0x00000048, 0x0000001d, 0x00000002, 0x00050036, 0x00000002, 0x00000004, 0x00000000, 0x00000003,
    0x000200f8, 0x00000005, 0x0004003b, 0x00000008, 0x00000009, 0x00000007, 0x0004003d, 0x0000000a,
    0x0000000d, 0x0000000c, 0x00050051, 0x00000006, 0x0000000f, 0x0000000d, 0x00000000, 0x00050051,
    0x00000006, 0x00000010, 0x0000000d, 0x00000001, 0x00050051, 0x00000006, 0x00000011, 0x0000000d,
    0x00000002, 0x00070050, 0x00000007, 0x00000012, 0x0000000f, 0x00000010, 0x00000011, 0x0000000e,
    0x0003003e, 0x00000009, 0x00000012, 0x00050041, 0x00000019, 0x0000001a, 0x00000016, 0x00000018,
    0x0004003d, 0x00000006, 0x0000001b, 0x0000001a, 0x000500ba, 0x0000001d, 0x0000001e, 0x0000001b,
    0x0000001c, 0x000300f7, 0x00000020, 0x00000000, 0x000400fa, 0x0000001e, 0x0000001f, 0x00000020,
    0x000200f8, 0x0000001f, 0x00050041, 0x00000022, 0x00000023, 0x00000016, 0x00000021, 0x0004003d,
    0x00000013, 0x00000024, 0x00000023, 0x00050041, 0x00000022, 0x00000026, 0x00000016, 0x00000025,
    0x0004003d, 0x00000013, 0x00000027, 0x00000026, 0x00050092, 0x00000013, 0x00000028, 0x00000024,
    0x00000027, 0x0004003d, 0x00000007, 0x00000029, 0x00000009, 0x00050091, 0x00000007, 0x0000002a,
    0x00000028, 0x00000029, 0x0003003e, 0x00000009, 0x0000002a, 0x000200f9, 0x00000020, 0x000200f8,
    0x00000020, 0x0004003d, 0x00000007, 0x00000031, 0x00000009, 0x00050041, 0x00000032, 0x00000033,
    0x00000030, 0x00000025, 0x0003003e, 0x00000033, 0x00000031, 0x00050041, 0x00000019, 0x00000038,
    0x00000016, 0x00000037, 0x0004003d, 0x00000006, 0x00000039, 0x00000038, 0x000500ba, 0x0000001d,
    0x0000003a, 0x00000039, 0x0000001c, 0x000400a8, 0x0000001d, 0x0000003b, 0x0000003a, 0x000300f7,
    0x0000003d, 0x00000000, 0x000400fa, 0x0000003b, 0x0000003c, 0x0000003d, 0x000200f8, 0x0000003c,
    0x00050041, 0x00000019, 0x0000003f, 0x00000016, 0x0000003e, 0x0004003d, 0x00000006, 0x00000040,
    0x0000003f, 0x000500b8, 0x0000001d, 0x00000041, 0x00000040, 0x0000001c, 0x000200f9, 0x0000003d,
    0x000200f8, 0x0000003d, 0x000700f5, 0x0000001d, 0x00000042, 0x0000003a, 0x00000020, 0x00000041,
    0x0000003c, 0x0004003d, 0x00000034, 0x00000045, 0x00000044, 0x0004003d, 0x00000034, 0x00000047,
    0x00000046, 0x00050050, 0x00000048, 0x00000049, 0x00000042, 0x00000042, 0x000600a9, 0x00000034,
    0x0000004a, 0x00000049, 0x00000045, 0x00000047, 0x0003003e, 0x00000036, 0x0000004a, 0x000100fd,
    0x00010038
};

static constexpr u32 s_mundo_video_nv12_mesh_fragment_shader_spirv[] {
    0x07230203, 0x00010000, 0x0008000b, 0x00000014, 0x00000000, 0x00020011, 0x00000001, 0x0006000b,
    0x00000001, 0x4c534c47, 0x6474732e, 0x3035342e, 0x00000000, 0x0003000e, 0x00000000, 0x00000001,
    0x0007000f, 0x00000004, 0x00000004, 0x6e69616d, 0x00000000, 0x00000009, 0x00000011, 0x00030010,
    0x00000004, 0x00000007, 0x00030003, 0x00000002, 0x000001c2, 0x00040005, 0x00000004, 0x6e69616d,
    0x00000000, 0x00050005, 0x00000009, 0x5f74756f, 0x6f6c6f63, 0x00000072, 0x00060005, 0x0000000d,
    0x65646976, 0x65745f6f, 0x72757478, 0x00000065, 0x00040005, 0x00000011, 0x76755f76, 0x00000000,
    0x00040047, 0x00000009, 0x0000001e, 0x00000000, 0x00040047, 0x0000000d, 0x00000021, 0x00000000,
    0x00040047, 0x0000000d, 0x00000022, 0x00000000, 0x00040047, 0x00000011, 0x0000001e, 0x00000000,
    0x00020013, 0x00000002, 0x00030021, 0x00000003, 0x00000002, 0x00030016, 0x00000006, 0x00000020,
    0x00040017, 0x00000007, 0x00000006, 0x00000004, 0x00040020, 0x00000008, 0x00000003, 0x00000007,
    0x0004003b, 0x00000008, 0x00000009, 0x00000003, 0x00090019, 0x0000000a, 0x00000006, 0x00000001,
    0x00000000, 0x00000000, 0x00000000, 0x00000001, 0x00000000, 0x0003001b, 0x0000000b, 0x0000000a,
    0x00040020, 0x0000000c, 0x00000000, 0x0000000b, 0x0004003b, 0x0000000c, 0x0000000d, 0x00000000,
    0x00040017, 0x0000000f, 0x00000006, 0x00000002, 0x00040020, 0x00000010, 0x00000001, 0x0000000f,
    0x0004003b, 0x00000010, 0x00000011, 0x00000001, 0x00050036, 0x00000002, 0x00000004, 0x00000000,
    0x00000003, 0x000200f8, 0x00000005, 0x0004003d, 0x0000000b, 0x0000000e, 0x0000000d, 0x0004003d,
    0x0000000f, 0x00000012, 0x00000011, 0x00050057, 0x00000007, 0x00000013, 0x0000000e, 0x00000012,
    0x0003003e, 0x00000009, 0x00000013, 0x000100fd, 0x00010038
};
#endif

struct OpenGLContext::Impl {
    EGLDisplay display { EGL_NO_DISPLAY };
    EGLConfig config { EGL_NO_CONFIG_KHR };
    EGLContext context { EGL_NO_CONTEXT };
    EGLSurface surface { EGL_NO_SURFACE };

    GLuint framebuffer { 0 };
    GLuint color_buffer { 0 };
    GLuint depth_buffer { 0 };
    EGLint texture_target { 0 };

#ifdef USE_VULKAN_DMABUF_IMAGES
    EGLImage egl_image { EGL_NO_IMAGE };
    Optional<ImportedVideoOpaqueFDTexture> cached_video_y_texture;
    Optional<ImportedVideoOpaqueFDTexture> cached_video_uv_texture;
    Optional<ImportedVideoOpaqueFDTexture> cached_video_rgba_texture;
    Optional<ImportedVideoOpaqueFDTexture> cached_video_rgba_target_texture;
    Vector<ImportedVideoOpaqueFDTexture> cached_vulkan_rgba_render_target_images;
    struct CachedVulkanRGBAStaticTextureImage {
        u32 texture { 0 };
        u32 width { 0 };
        u32 height { 0 };
        unsigned signature { 0 };
        ImportedVideoOpaqueFDTexture imported_texture;
    };
    Vector<CachedVulkanRGBAStaticTextureImage> cached_vulkan_rgba_static_texture_images;
    struct FailedVideoRGBATargetTextureImport {
        u32 target_texture { 0 };
        u32 width { 0 };
        u32 height { 0 };
        StringView reason {};
        u32 gl_error { 0 };
    };
    Vector<FailedVideoRGBATargetTextureImport> failed_video_rgba_target_texture_imports;
    u64 gl_draw_serial { 0 };
    u64 direct_vulkan_video_draw_serial { 0 };
    struct CachedVulkanVideoReplayBuffers {
        unsigned signature { 0 };
        size_t position_bytes { 0 };
        size_t uv_bytes { 0 };
        size_t uv_right_bytes { 0 };
        size_t index_bytes { 0 };
        NonnullOwnPtr<Gfx::VulkanBuffer> position_buffer;
        NonnullOwnPtr<Gfx::VulkanBuffer> uv_buffer;
        OwnPtr<Gfx::VulkanBuffer> uv_right_buffer;
        NonnullOwnPtr<Gfx::VulkanBuffer> index_buffer;

        CachedVulkanVideoReplayBuffers(unsigned signature, size_t position_bytes, size_t uv_bytes, size_t uv_right_bytes, size_t index_bytes, NonnullOwnPtr<Gfx::VulkanBuffer> position_buffer, NonnullOwnPtr<Gfx::VulkanBuffer> uv_buffer, OwnPtr<Gfx::VulkanBuffer> uv_right_buffer, NonnullOwnPtr<Gfx::VulkanBuffer> index_buffer)
            : signature(signature)
            , position_bytes(position_bytes)
            , uv_bytes(uv_bytes)
            , uv_right_bytes(uv_right_bytes)
            , index_bytes(index_bytes)
            , position_buffer(move(position_buffer))
            , uv_buffer(move(uv_buffer))
            , uv_right_buffer(move(uv_right_buffer))
            , index_buffer(move(index_buffer))
        {
        }
    };
    OwnPtr<CachedVulkanVideoReplayBuffers> cached_vulkan_video_replay_buffers;
    struct CachedVulkanSolidMeshReplayBuffers {
        unsigned signature { 0 };
        size_t position_bytes { 0 };
        size_t index_bytes { 0 };
        NonnullOwnPtr<Gfx::VulkanBuffer> position_buffer;
        NonnullOwnPtr<Gfx::VulkanBuffer> index_buffer;

        CachedVulkanSolidMeshReplayBuffers(unsigned signature, size_t position_bytes, size_t index_bytes, NonnullOwnPtr<Gfx::VulkanBuffer> position_buffer, NonnullOwnPtr<Gfx::VulkanBuffer> index_buffer)
            : signature(signature)
            , position_bytes(position_bytes)
            , index_bytes(index_bytes)
            , position_buffer(move(position_buffer))
            , index_buffer(move(index_buffer))
        {
        }
    };
    OwnPtr<CachedVulkanSolidMeshReplayBuffers> cached_vulkan_solid_mesh_replay_buffers;
    struct CachedVulkanColoredMeshReplayBuffers {
        unsigned signature { 0 };
        size_t position_bytes { 0 };
        size_t color_bytes { 0 };
        size_t index_bytes { 0 };
        NonnullOwnPtr<Gfx::VulkanBuffer> position_buffer;
        NonnullOwnPtr<Gfx::VulkanBuffer> color_buffer;
        NonnullOwnPtr<Gfx::VulkanBuffer> index_buffer;

        CachedVulkanColoredMeshReplayBuffers(unsigned signature, size_t position_bytes, size_t color_bytes, size_t index_bytes, NonnullOwnPtr<Gfx::VulkanBuffer> position_buffer, NonnullOwnPtr<Gfx::VulkanBuffer> color_buffer, NonnullOwnPtr<Gfx::VulkanBuffer> index_buffer)
            : signature(signature)
            , position_bytes(position_bytes)
            , color_bytes(color_bytes)
            , index_bytes(index_bytes)
            , position_buffer(move(position_buffer))
            , color_buffer(move(color_buffer))
            , index_buffer(move(index_buffer))
        {
        }
    };
    OwnPtr<CachedVulkanColoredMeshReplayBuffers> cached_vulkan_colored_mesh_replay_buffers;
    struct CachedVulkanTexturedMeshReplayBuffers {
        unsigned signature { 0 };
        size_t position_bytes { 0 };
        size_t uv_bytes { 0 };
        size_t index_bytes { 0 };
        NonnullOwnPtr<Gfx::VulkanBuffer> position_buffer;
        NonnullOwnPtr<Gfx::VulkanBuffer> uv_buffer;
        NonnullOwnPtr<Gfx::VulkanBuffer> index_buffer;

        CachedVulkanTexturedMeshReplayBuffers(unsigned signature, size_t position_bytes, size_t uv_bytes, size_t index_bytes, NonnullOwnPtr<Gfx::VulkanBuffer> position_buffer, NonnullOwnPtr<Gfx::VulkanBuffer> uv_buffer, NonnullOwnPtr<Gfx::VulkanBuffer> index_buffer)
            : signature(signature)
            , position_bytes(position_bytes)
            , uv_bytes(uv_bytes)
            , index_bytes(index_bytes)
            , position_buffer(move(position_buffer))
            , uv_buffer(move(uv_buffer))
            , index_buffer(move(index_buffer))
        {
        }
    };
    OwnPtr<CachedVulkanTexturedMeshReplayBuffers> cached_vulkan_textured_mesh_replay_buffers;
    struct {
        PFNEGLQUERYDMABUFFORMATSEXTPROC query_dma_buf_formats { nullptr };
        PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_dma_buf_modifiers { nullptr };
    } ext_procs;
#endif
};

OpenGLContext::OpenGLContext(NonnullRefPtr<Gfx::SkiaBackendContext> skia_backend_context, Impl impl, WebGLVersion webgl_version, DrawingBufferOptions drawing_buffer_options)
    : m_skia_backend_context(move(skia_backend_context))
    , m_impl(make<Impl>(move(impl)))
    , m_webgl_version(webgl_version)
    , m_drawing_buffer_options(drawing_buffer_options)
{
}

OpenGLContext::~OpenGLContext()
{
#ifdef ENABLE_WEBGL
    free_surface_resources();
    eglMakeCurrent(m_impl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(m_impl->display, m_impl->context);
#endif
}

void OpenGLContext::free_surface_resources()
{
#ifdef ENABLE_WEBGL
    eglMakeCurrent(m_impl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_impl->context);

    if (m_impl->framebuffer) {
        glDeleteFramebuffers(1, &m_impl->framebuffer);
        m_impl->framebuffer = 0;
    }

    if (m_impl->color_buffer) {
        glDeleteTextures(1, &m_impl->color_buffer);
        m_impl->color_buffer = 0;
    }

    if (m_impl->depth_buffer) {
        glDeleteRenderbuffers(1, &m_impl->depth_buffer);
        m_impl->depth_buffer = 0;
    }

#    ifdef USE_VULKAN_DMABUF_IMAGES
    if (m_impl->cached_video_y_texture.has_value())
        delete_imported_video_opaque_fd_texture(m_impl->cached_video_y_texture.value());
    m_impl->cached_video_y_texture.clear();
    if (m_impl->cached_video_uv_texture.has_value())
        delete_imported_video_opaque_fd_texture(m_impl->cached_video_uv_texture.value());
    m_impl->cached_video_uv_texture.clear();
    if (m_impl->cached_video_rgba_texture.has_value())
        delete_imported_video_opaque_fd_texture(m_impl->cached_video_rgba_texture.value());
    m_impl->cached_video_rgba_texture.clear();
    if (m_impl->cached_video_rgba_target_texture.has_value())
        delete_imported_video_opaque_fd_texture(m_impl->cached_video_rgba_target_texture.value());
    m_impl->cached_video_rgba_target_texture.clear();
    m_impl->cached_vulkan_rgba_render_target_images.clear();
    m_impl->cached_vulkan_video_replay_buffers = nullptr;

    if (m_impl->egl_image != EGL_NO_IMAGE) {
        eglDestroyImage(m_impl->display, m_impl->egl_image);
        m_impl->egl_image = EGL_NO_IMAGE;
    }
#    endif

    if (m_impl->surface != EGL_NO_SURFACE) {
#    ifdef AK_OS_MACOS
        eglReleaseTexImage(m_impl->display, m_impl->surface, EGL_BACK_BUFFER);
#    endif
        eglDestroySurface(m_impl->display, m_impl->surface);
        m_impl->surface = EGL_NO_SURFACE;
    }
#endif
}

#ifdef ENABLE_WEBGL
static EGLConfig get_egl_config(EGLDisplay display)
{
    EGLint const config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8,
        EGL_GREEN_SIZE, 8,
        EGL_BLUE_SIZE, 8,
        EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24,
        EGL_STENCIL_SIZE, 8,
        EGL_NONE
    };

    EGLint number_of_configs;
    eglChooseConfig(display, config_attribs, NULL, 0, &number_of_configs);

    Vector<EGLConfig> configs;
    configs.resize(number_of_configs);
    eglChooseConfig(display, config_attribs, configs.data(), number_of_configs, &number_of_configs);
    return number_of_configs > 0 ? configs[0] : EGL_NO_CONFIG_KHR;
}
#endif

OwnPtr<OpenGLContext> OpenGLContext::create(NonnullRefPtr<Gfx::SkiaBackendContext> skia_backend_context, WebGLVersion webgl_version, [[maybe_unused]] DrawingBufferOptions drawing_buffer_options)
{
#ifdef ENABLE_WEBGL
    EGLAttrib display_attributes[] = {
        EGL_PLATFORM_ANGLE_TYPE_ANGLE,
#    if defined(AK_OS_MACOS)
        EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE,
#    elif defined(USE_VULKAN_DMABUF_IMAGES)
        EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE,
        EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE,
        EGL_PLATFORM_SURFACELESS_MESA,
#    endif
        EGL_NONE,
    };

    auto display = eglGetPlatformDisplay(EGL_PLATFORM_ANGLE_ANGLE, reinterpret_cast<void*>(EGL_DEFAULT_DISPLAY), display_attributes);
    if (display == EGL_NO_DISPLAY) {
        dbgln("Failed to get EGL display");
        return {};
    }

    EGLint major, minor;
    if (!eglInitialize(display, &major, &minor)) {
        dbgln("Failed to initialize EGL");
        return {};
    }

    auto* config = get_egl_config(display);
    if (config == EGL_NO_CONFIG_KHR) {
        dbgln("Failed to find EGLConfig");
        return {};
    }

    EGLint texture_target;
#    if defined(AK_OS_MACOS)
    eglGetConfigAttrib(display, config, EGL_BIND_TO_TEXTURE_TARGET_ANGLE, &texture_target);
    VERIFY(texture_target == EGL_TEXTURE_RECTANGLE_ANGLE || texture_target == EGL_TEXTURE_2D);
#    elif defined(USE_VULKAN_DMABUF_IMAGES)
    texture_target = EGL_TEXTURE_2D;
#    endif

    EGLint context_attributes[] = {
        EGL_CONTEXT_CLIENT_VERSION,
        webgl_version == WebGLVersion::WebGL1 ? 2 : 3,
        EGL_CONTEXT_WEBGL_COMPATIBILITY_ANGLE,
        EGL_TRUE,
        EGL_ROBUST_RESOURCE_INITIALIZATION_ANGLE,
        EGL_TRUE,
        EGL_CONTEXT_OPENGL_BACKWARDS_COMPATIBLE_ANGLE,
        EGL_FALSE,
#    ifdef USE_VULKAN_DMABUF_IMAGES
        // we need GL_OES_EGL_image
        EGL_EXTENSIONS_ENABLED_ANGLE,
        EGL_TRUE,
#    endif
        EGL_NONE,
        EGL_NONE,
    };
    auto context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attributes);
    if (context == EGL_NO_CONTEXT) {
        dbgln("Failed to create EGL context");
        return {};
    }

#    ifdef USE_VULKAN_DMABUF_IMAGES
    auto pfn_egl_query_dma_buf_formats_ext = reinterpret_cast<PFNEGLQUERYDMABUFFORMATSEXTPROC>(eglGetProcAddress("eglQueryDmaBufFormatsEXT"));
    if (!pfn_egl_query_dma_buf_formats_ext) {
        dbgln("eglQueryDmaBufFormatsEXT unavailable");
        return {};
    }

    auto pfn_egl_query_dma_buf_modifiers_ext = reinterpret_cast<PFNEGLQUERYDMABUFMODIFIERSEXTPROC>(eglGetProcAddress("eglQueryDmaBufModifiersEXT"));
    if (!pfn_egl_query_dma_buf_modifiers_ext) {
        dbgln("eglQueryDmaBufModifiersEXT unavailable");
        return {};
    }
#    endif

    return make<OpenGLContext>(skia_backend_context, Impl {
                                                         .display = display,
                                                         .config = config,
                                                         .context = context,
                                                         .texture_target = texture_target,
#    ifdef USE_VULKAN_DMABUF_IMAGES
                                                         .cached_video_y_texture = {},
                                                         .cached_video_uv_texture = {},
                                                         .cached_video_rgba_texture = {},
                                                         .cached_video_rgba_target_texture = {},
                                                         .cached_vulkan_rgba_render_target_images = {},
                                                         .cached_vulkan_rgba_static_texture_images = {},
                                                         .failed_video_rgba_target_texture_imports = {},
                                                         .cached_vulkan_video_replay_buffers = {},
                                                         .cached_vulkan_solid_mesh_replay_buffers = {},
                                                         .cached_vulkan_colored_mesh_replay_buffers = {},
                                                         .cached_vulkan_textured_mesh_replay_buffers = {},
                                                         .ext_procs = {
                                                             .query_dma_buf_formats = pfn_egl_query_dma_buf_formats_ext,
                                                             .query_dma_buf_modifiers = pfn_egl_query_dma_buf_modifiers_ext,
                                                         },
#    endif
                                                     },
        webgl_version, drawing_buffer_options);
#else
    (void)skia_backend_context;
    (void)webgl_version;
    return {};
#endif
}

void OpenGLContext::notify_content_will_change()
{
#ifdef ENABLE_WEBGL
    if (!m_painting_surface)
        return;
    m_painting_surface->notify_content_will_change();
#endif
}

void OpenGLContext::clear_buffer_to_default_values()
{
#ifdef ENABLE_WEBGL
    GLint original_framebuffer;
    GLint original_renderbuffer;
    GLenum framebuffer_target = GL_FRAMEBUFFER;
    GLenum framebuffer_binding = GL_FRAMEBUFFER_BINDING;
    if (m_webgl_version == WebGLVersion::WebGL2) {
        framebuffer_target = GL_DRAW_FRAMEBUFFER;
        framebuffer_binding = GL_DRAW_FRAMEBUFFER_BINDING;
    }
    glGetIntegerv(framebuffer_binding, &original_framebuffer);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &original_renderbuffer);

    glBindFramebuffer(framebuffer_target, default_framebuffer());
    glBindRenderbuffer(GL_RENDERBUFFER, default_renderbuffer());

    Array<GLfloat, 4> current_clear_color;
    glGetFloatv(GL_COLOR_CLEAR_VALUE, current_clear_color.data());

    GLfloat current_clear_depth;
    glGetFloatv(GL_DEPTH_CLEAR_VALUE, &current_clear_depth);

    GLint current_clear_stencil;
    glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &current_clear_stencil);

    // The implicit clear value for the color buffer is (0, 0, 0, 0)
    glClearColor(0, 0, 0, 0);

    // The implicit clear value for the depth buffer is 1.0.
    glClearDepthf(1.0f);

    // The implicit clear value for the stencil buffer is 0.
    glClearStencil(0);

    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);

    // Restore the clear values.
    glClearColor(current_clear_color[0], current_clear_color[1], current_clear_color[2], current_clear_color[3]);
    glClearDepthf(current_clear_depth);
    glClearStencil(current_clear_stencil);

    glBindFramebuffer(framebuffer_target, original_framebuffer);
    glBindRenderbuffer(GL_RENDERBUFFER, original_renderbuffer);
#endif
}

#ifdef AK_OS_MACOS
void OpenGLContext::allocate_iosurface_painting_surface()
{
    m_shared_image_buffer = make<Gfx::SharedImageBuffer>(Gfx::SharedImageBuffer::create(m_size));
    m_painting_surface = Gfx::PaintingSurface::create_from_shared_image_buffer(*m_shared_image_buffer, m_skia_backend_context, Gfx::PaintingSurface::Origin::BottomLeft);

    EGLint const surface_attributes[] = {
        EGL_WIDTH,
        m_size.width(),
        EGL_HEIGHT,
        m_size.height(),
        EGL_IOSURFACE_PLANE_ANGLE,
        0,
        EGL_TEXTURE_TARGET,
        m_impl->texture_target,
        EGL_TEXTURE_INTERNAL_FORMAT_ANGLE,
        GL_BGRA_EXT,
        EGL_TEXTURE_FORMAT,
        EGL_TEXTURE_RGBA,
        EGL_TEXTURE_TYPE_ANGLE,
        GL_UNSIGNED_BYTE,
        EGL_NONE,
        EGL_NONE,
    };
    m_impl->surface = eglCreatePbufferFromClientBuffer(m_impl->display, EGL_IOSURFACE_ANGLE, m_shared_image_buffer->iosurface_handle().core_foundation_pointer(), m_impl->config, surface_attributes);

    eglMakeCurrent(m_impl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_impl->context);

    glGenTextures(1, &m_impl->color_buffer);
    glBindTexture(m_impl->texture_target == EGL_TEXTURE_RECTANGLE_ANGLE ? GL_TEXTURE_RECTANGLE_ANGLE : GL_TEXTURE_2D, m_impl->color_buffer);
    auto result = eglBindTexImage(m_impl->display, m_impl->surface, EGL_BACK_BUFFER);
    VERIFY(result == EGL_TRUE);

    glViewport(0, 0, m_size.width(), m_size.height());
}
#endif

#ifdef USE_VULKAN_DMABUF_IMAGES
static constexpr unsigned cuda_external_memory_dedicated_flag = 1u;

static bool gl_extension_list_contains(char const* extensions, StringView needle)
{
    if (!extensions)
        return false;

    StringView extensions_view(extensions, strlen(extensions));
    for (auto extension : extensions_view.split_view(' ')) {
        if (extension == needle)
            return true;
    }
    return false;
}

static void probe_cuda_import_for_vulkan_fd(char const* probe_kind, int fd, Gfx::VulkanImage const& vulkan_image, size_t log_count)
{
    if (fd < 0)
        return;

    auto* library = dlopen("libcuda.so.1", RTLD_LAZY);
    if (!library) {
        if (log_count <= 8 || log_count % 120 == 0)
            dbgln("MUNDO_WEBGL_VULKAN_CUDA_IMPORT_PROBE count={} probe={} status=failed reason=missing_libcuda", log_count, probe_kind);
        return;
    }

    auto* cu_init = reinterpret_cast<tcuInit*>(dlsym(library, "cuInit"));
    auto* cu_get_error_name = reinterpret_cast<tcuGetErrorName*>(dlsym(library, "cuGetErrorName"));
    auto* cu_device_get = reinterpret_cast<tcuDeviceGet*>(dlsym(library, "cuDeviceGet"));
    auto* cu_ctx_get_current = reinterpret_cast<tcuCtxGetCurrent*>(dlsym(library, "cuCtxGetCurrent"));
    auto* cu_ctx_push_current = reinterpret_cast<tcuCtxPushCurrent_v2*>(dlsym(library, "cuCtxPushCurrent_v2"));
    auto* cu_ctx_pop_current = reinterpret_cast<tcuCtxPopCurrent_v2*>(dlsym(library, "cuCtxPopCurrent_v2"));
    auto* cu_device_primary_ctx_retain = reinterpret_cast<tcuDevicePrimaryCtxRetain*>(dlsym(library, "cuDevicePrimaryCtxRetain"));
    auto* cu_device_primary_ctx_release = reinterpret_cast<tcuDevicePrimaryCtxRelease*>(dlsym(library, "cuDevicePrimaryCtxRelease"));
    auto* cu_import_external_memory = reinterpret_cast<tcuImportExternalMemory*>(dlsym(library, "cuImportExternalMemory"));
    auto* cu_destroy_external_memory = reinterpret_cast<tcuDestroyExternalMemory*>(dlsym(library, "cuDestroyExternalMemory"));
    auto* cu_external_memory_get_mapped_mipmapped_array = reinterpret_cast<tcuExternalMemoryGetMappedMipmappedArray*>(dlsym(library, "cuExternalMemoryGetMappedMipmappedArray"));
    auto* cu_mipmapped_array_destroy = reinterpret_cast<tcuMipmappedArrayDestroy*>(dlsym(library, "cuMipmappedArrayDestroy"));
    if (!cu_init || !cu_device_get || !cu_ctx_get_current || !cu_ctx_push_current || !cu_ctx_pop_current || !cu_device_primary_ctx_retain || !cu_device_primary_ctx_release || !cu_import_external_memory || !cu_destroy_external_memory || !cu_external_memory_get_mapped_mipmapped_array || !cu_mipmapped_array_destroy) {
        if (log_count <= 8 || log_count % 120 == 0) {
            dbgln("MUNDO_WEBGL_VULKAN_CUDA_IMPORT_PROBE count={} probe={} status=failed reason=missing_symbols init={} get_error_name={} device_get={} ctx_get_current={} ctx_push={} ctx_pop={} primary_retain={} primary_release={} import_memory={} destroy_memory={} mapped_mipmap={} mip_destroy={}",
                log_count,
                probe_kind,
                cu_init != nullptr,
                cu_get_error_name != nullptr,
                cu_device_get != nullptr,
                cu_ctx_get_current != nullptr,
                cu_ctx_push_current != nullptr,
                cu_ctx_pop_current != nullptr,
                cu_device_primary_ctx_retain != nullptr,
                cu_device_primary_ctx_release != nullptr,
                cu_import_external_memory != nullptr,
                cu_destroy_external_memory != nullptr,
                cu_external_memory_get_mapped_mipmapped_array != nullptr,
                cu_mipmapped_array_destroy != nullptr);
        }
        return;
    }

    auto cuda_error_name = [&](CUresult result) -> char const* {
        if (!cu_get_error_name)
            return "unavailable";
        char const* name = nullptr;
        if (cu_get_error_name(result, &name) == CUDA_SUCCESS && name)
            return name;
        return "unknown";
    };

    auto init_result = cu_init(0);
    if (init_result != CUDA_SUCCESS) {
        if (log_count <= 8 || log_count % 120 == 0) {
            dbgln("MUNDO_WEBGL_VULKAN_CUDA_IMPORT_PROBE count={} probe={} status=failed step=init result={} error={} size={}x{} allocation_size={} row_pitch={} modifier={}",
                log_count,
                probe_kind,
                static_cast<unsigned>(init_result),
                cuda_error_name(init_result),
                vulkan_image.info.extent.width,
                vulkan_image.info.extent.height,
                vulkan_image.info.allocation_size,
                vulkan_image.info.row_pitch,
                vulkan_image.info.modifier);
        }
        return;
    }

    CUcontext previous_context { nullptr };
    auto get_current_context_result = cu_ctx_get_current(&previous_context);
    if (get_current_context_result != CUDA_SUCCESS) {
        if (log_count <= 8 || log_count % 120 == 0) {
            dbgln("MUNDO_WEBGL_VULKAN_CUDA_IMPORT_PROBE count={} probe={} status=failed step=get_current_context result={} error={}",
                log_count,
                probe_kind,
                static_cast<unsigned>(get_current_context_result),
                cuda_error_name(get_current_context_result));
        }
        return;
    }

    CUdevice device { 0 };
    auto device_result = cu_device_get(&device, 0);
    if (device_result != CUDA_SUCCESS) {
        if (log_count <= 8 || log_count % 120 == 0) {
            dbgln("MUNDO_WEBGL_VULKAN_CUDA_IMPORT_PROBE count={} probe={} status=failed step=device_get result={} error={}",
                log_count,
                probe_kind,
                static_cast<unsigned>(device_result),
                cuda_error_name(device_result));
        }
        return;
    }

    CUcontext primary_context { nullptr };
    auto retain_result = cu_device_primary_ctx_retain(&primary_context, device);
    if (retain_result != CUDA_SUCCESS || !primary_context) {
        if (log_count <= 8 || log_count % 120 == 0) {
            dbgln("MUNDO_WEBGL_VULKAN_CUDA_IMPORT_PROBE count={} probe={} status=failed step=primary_ctx_retain result={} error={} context={}",
                log_count,
                probe_kind,
                static_cast<unsigned>(retain_result),
                cuda_error_name(retain_result),
                primary_context);
        }
        return;
    }

    auto push_result = cu_ctx_push_current(primary_context);
    if (push_result != CUDA_SUCCESS) {
        cu_device_primary_ctx_release(device);
        if (log_count <= 8 || log_count % 120 == 0) {
            dbgln("MUNDO_WEBGL_VULKAN_CUDA_IMPORT_PROBE count={} probe={} status=failed step=ctx_push result={} error={}",
                log_count,
                probe_kind,
                static_cast<unsigned>(push_result),
                cuda_error_name(push_result));
        }
        return;
    }

    auto release_context = [&] {
        CUcontext popped_context { nullptr };
        auto pop_result = cu_ctx_pop_current(&popped_context);
        auto release_result = cu_device_primary_ctx_release(device);
        if ((pop_result != CUDA_SUCCESS || release_result != CUDA_SUCCESS) && (log_count <= 8 || log_count % 120 == 0)) {
            dbgln("MUNDO_WEBGL_VULKAN_CUDA_IMPORT_PROBE count={} probe={} status=cleanup_warning pop_result={} pop_error={} release_result={} release_error={} previous_context={} popped_context={}",
                log_count,
                probe_kind,
                static_cast<unsigned>(pop_result),
                cuda_error_name(pop_result),
                static_cast<unsigned>(release_result),
                cuda_error_name(release_result),
                previous_context,
                popped_context);
        }
    };

    auto import_fd = dup(fd);
    if (import_fd < 0) {
        release_context();
        if (log_count <= 8 || log_count % 120 == 0)
            dbgln("MUNDO_WEBGL_VULKAN_CUDA_IMPORT_PROBE count={} probe={} status=failed reason=dup_fd", log_count, probe_kind);
        return;
    }

    CUDA_EXTERNAL_MEMORY_HANDLE_DESC memory_handle_desc {};
    memory_handle_desc.type = CU_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD;
    memory_handle_desc.handle.fd = import_fd;
    memory_handle_desc.size = static_cast<unsigned long long>(vulkan_image.info.allocation_size);
    memory_handle_desc.flags = cuda_external_memory_dedicated_flag;

    CUexternalMemory external_memory { nullptr };
    auto import_result = cu_import_external_memory(&external_memory, &memory_handle_desc);
    if (import_result != CUDA_SUCCESS) {
        close(import_fd);
        release_context();
        if (log_count <= 8 || log_count % 120 == 0) {
            dbgln("MUNDO_WEBGL_VULKAN_CUDA_IMPORT_PROBE count={} probe={} status=failed step=import init_result={} import_result={} import_error={} previous_context={} primary_context={} dedicated={} size={}x{} allocation_size={} row_pitch={} modifier={}",
                log_count,
                probe_kind,
                static_cast<unsigned>(init_result),
                static_cast<unsigned>(import_result),
                cuda_error_name(import_result),
                previous_context,
                primary_context,
                memory_handle_desc.flags == cuda_external_memory_dedicated_flag,
                vulkan_image.info.extent.width,
                vulkan_image.info.extent.height,
                vulkan_image.info.allocation_size,
                vulkan_image.info.row_pitch,
                vulkan_image.info.modifier);
        }
        return;
    }

    CUDA_EXTERNAL_MEMORY_MIPMAPPED_ARRAY_DESC mipmapped_array_desc {};
    mipmapped_array_desc.offset = 0;
    mipmapped_array_desc.arrayDesc.Width = vulkan_image.info.extent.width;
    mipmapped_array_desc.arrayDesc.Height = vulkan_image.info.extent.height;
    mipmapped_array_desc.arrayDesc.Depth = 0;
    mipmapped_array_desc.arrayDesc.Format = CU_AD_FORMAT_UNSIGNED_INT8;
    mipmapped_array_desc.arrayDesc.NumChannels = 4;
    mipmapped_array_desc.arrayDesc.Flags = 0;
    mipmapped_array_desc.numLevels = 1;

    CUmipmappedArray mipmapped_array { nullptr };
    auto map_result = cu_external_memory_get_mapped_mipmapped_array(&mipmapped_array, external_memory, &mipmapped_array_desc);
    if (map_result == CUDA_SUCCESS && mipmapped_array)
        cu_mipmapped_array_destroy(mipmapped_array);
    cu_destroy_external_memory(external_memory);
    release_context();

    if (log_count <= 8 || log_count % 120 == 0) {
        dbgln("MUNDO_WEBGL_VULKAN_CUDA_IMPORT_PROBE count={} probe={} status={} init_result={} import_result={} map_result={} map_error={} previous_context={} primary_context={} dedicated={} size={}x{} allocation_size={} row_pitch={} modifier={}",
            log_count,
            probe_kind,
            map_result == CUDA_SUCCESS ? "ok" : "failed",
            static_cast<unsigned>(init_result),
            static_cast<unsigned>(import_result),
            static_cast<unsigned>(map_result),
            cuda_error_name(map_result),
            previous_context,
            primary_context,
            memory_handle_desc.flags == cuda_external_memory_dedicated_flag,
            vulkan_image.info.extent.width,
            vulkan_image.info.extent.height,
            vulkan_image.info.allocation_size,
            vulkan_image.info.row_pitch,
            vulkan_image.info.modifier);
    }
}

static void probe_cuda_import_for_vulkan_dmabuf(int dma_buf_fd, Gfx::VulkanImage const& vulkan_image, size_t log_count)
{
    probe_cuda_import_for_vulkan_fd("dmabuf", dma_buf_fd, vulkan_image, log_count);
}

static void probe_cuda_import_for_vulkan_opaque_fd(Gfx::VulkanContext const& context, VkFormat format, size_t log_count)
{
    auto opaque_image_or_error = Gfx::create_opaque_fd_vulkan_image(context, 64, 64, format);
    if (opaque_image_or_error.is_error()) {
        if (log_count <= 8 || log_count % 120 == 0) {
            dbgln("MUNDO_WEBGL_VULKAN_CUDA_IMPORT_PROBE count={} probe=opaque_fd status=failed reason=create_image error={}",
                log_count,
                opaque_image_or_error.error());
        }
        return;
    }

    auto opaque_image = opaque_image_or_error.release_value();
    auto opaque_fd = opaque_image->get_opaque_fd();
    if (log_count <= 8 || log_count % 120 == 0) {
        dbgln("MUNDO_WEBGL_VULKAN_OPAQUE_FD_SURFACE count={} size={}x{} allocation_size={} fd_valid={}",
            log_count,
            opaque_image->info.extent.width,
            opaque_image->info.extent.height,
            opaque_image->info.allocation_size,
            opaque_fd >= 0);
    }
    probe_cuda_import_for_vulkan_fd("opaque_fd", opaque_fd, *opaque_image, log_count);
    if (opaque_fd >= 0)
        close(opaque_fd);
}

static void probe_gl_memory_object_fd_for_vulkan_opaque_fd(Gfx::VulkanContext const& context, char const* label, u32 width, u32 height, VkFormat vk_format, GLenum gl_internal_format, size_t log_count, bool force_log = false)
{
    auto const* extensions = reinterpret_cast<char const*>(glGetString(GL_EXTENSIONS));
    auto has_memory_object = gl_extension_list_contains(extensions, "GL_EXT_memory_object"sv);
    auto has_memory_object_fd = gl_extension_list_contains(extensions, "GL_EXT_memory_object_fd"sv);

    auto* gl_create_memory_objects_ext = reinterpret_cast<PFNGLCREATEMEMORYOBJECTSEXTPROC>(eglGetProcAddress("glCreateMemoryObjectsEXT"));
    auto* gl_delete_memory_objects_ext = reinterpret_cast<PFNGLDELETEMEMORYOBJECTSEXTPROC>(eglGetProcAddress("glDeleteMemoryObjectsEXT"));
    auto* gl_memory_object_parameteriv_ext = reinterpret_cast<PFNGLMEMORYOBJECTPARAMETERIVEXTPROC>(eglGetProcAddress("glMemoryObjectParameterivEXT"));
    auto* gl_import_memory_fd_ext = reinterpret_cast<PFNGLIMPORTMEMORYFDEXTPROC>(eglGetProcAddress("glImportMemoryFdEXT"));
    auto* gl_tex_storage_mem_2d_ext = reinterpret_cast<PFNGLTEXSTORAGEMEM2DEXTPROC>(eglGetProcAddress("glTexStorageMem2DEXT"));

    auto const should_log = force_log || log_count <= 8 || log_count % 120 == 0;

    if (should_log) {
        dbgln("MUNDO_WEBGL_GL_MEMORY_OBJECT_FD_CAPS count={} memory_object={} memory_object_fd={} create_fn={} delete_fn={} parameter_fn={} import_fd_fn={} tex_storage_mem_2d_fn={} extensions_null={}",
            log_count,
            has_memory_object,
            has_memory_object_fd,
            gl_create_memory_objects_ext != nullptr,
            gl_delete_memory_objects_ext != nullptr,
            gl_memory_object_parameteriv_ext != nullptr,
            gl_import_memory_fd_ext != nullptr,
            gl_tex_storage_mem_2d_ext != nullptr,
            extensions == nullptr);
    }

    if (!has_memory_object || !has_memory_object_fd || !gl_create_memory_objects_ext || !gl_delete_memory_objects_ext || !gl_memory_object_parameteriv_ext || !gl_import_memory_fd_ext || !gl_tex_storage_mem_2d_ext)
        return;

    auto opaque_image_or_error = Gfx::create_opaque_fd_vulkan_image(context, width, height, vk_format);
    if (opaque_image_or_error.is_error()) {
        if (should_log) {
            dbgln("MUNDO_WEBGL_GL_MEMORY_OBJECT_FD_PROBE count={} label={} status=failed reason=create_image vk_format={} gl_internal_format={} size={}x{} error={}",
                log_count,
                label,
                to_underlying(vk_format),
                gl_internal_format,
                width,
                height,
                opaque_image_or_error.error());
        }
        return;
    }

    auto opaque_image = opaque_image_or_error.release_value();
    auto opaque_fd = opaque_image->get_opaque_fd();
    if (opaque_fd < 0) {
        if (should_log)
            dbgln("MUNDO_WEBGL_GL_MEMORY_OBJECT_FD_PROBE count={} label={} status=failed reason=get_opaque_fd vk_format={} gl_internal_format={} size={}x{}", log_count, label, to_underlying(vk_format), gl_internal_format, width, height);
        return;
    }

    while (glGetError() != GL_NO_ERROR) {
    }

    GLuint memory_object { 0 };
    gl_create_memory_objects_ext(1, &memory_object);
    auto create_error = glGetError();

    if (create_error == GL_NO_ERROR && memory_object) {
        GLint dedicated = GL_TRUE;
        gl_memory_object_parameteriv_ext(memory_object, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    }
    auto parameter_error = glGetError();

    if (create_error == GL_NO_ERROR && parameter_error == GL_NO_ERROR && memory_object) {
        gl_import_memory_fd_ext(memory_object, opaque_image->info.allocation_size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, opaque_fd);
        opaque_fd = -1; // glImportMemoryFdEXT takes ownership of the fd, even on failure.
    }
    auto import_error = glGetError();

    GLuint texture { 0 };
    if (create_error == GL_NO_ERROR && parameter_error == GL_NO_ERROR && import_error == GL_NO_ERROR && memory_object) {
        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl_tex_storage_mem_2d_ext(GL_TEXTURE_2D, 1, gl_internal_format, opaque_image->info.extent.width, opaque_image->info.extent.height, memory_object, 0);
    }
    auto storage_error = glGetError();

    if (texture)
        glDeleteTextures(1, &texture);
    if (memory_object)
        gl_delete_memory_objects_ext(1, &memory_object);
    if (opaque_fd >= 0)
        close(opaque_fd);

    if (should_log) {
        dbgln("MUNDO_WEBGL_GL_MEMORY_OBJECT_FD_PROBE count={} label={} status={} create_error={} parameter_error={} import_error={} storage_error={} memory_object={} texture={} size={}x{} allocation_size={} vk_format={} gl_internal_format={}",
            log_count,
            label,
            create_error == GL_NO_ERROR && parameter_error == GL_NO_ERROR && import_error == GL_NO_ERROR && storage_error == GL_NO_ERROR ? "ok" : "failed",
            create_error,
            parameter_error,
            import_error,
            storage_error,
            memory_object,
            texture,
            opaque_image->info.extent.width,
            opaque_image->info.extent.height,
            opaque_image->info.allocation_size,
            to_underlying(vk_format),
            gl_internal_format);
    }
}

void OpenGLContext::probe_video_opaque_fd_texture_import(u32 width, u32 height, u32 uv_width, u32 uv_height, size_t log_count)
{
    auto& vulkan_context = m_skia_backend_context->vulkan_context();
    probe_gl_memory_object_fd_for_vulkan_opaque_fd(vulkan_context, "video_y_r8", width, height, VK_FORMAT_R8_UNORM, GL_R8_EXT, log_count, true);
    probe_gl_memory_object_fd_for_vulkan_opaque_fd(vulkan_context, "video_uv_rg8", uv_width, uv_height, VK_FORMAT_R8G8_UNORM, GL_RG8_EXT, log_count, true);
}

OpenGLContext::SkiaVulkanYcbcrProbeResult OpenGLContext::probe_skia_vulkan_ycbcr_texture_import(Media::HardwareVideoFrameExternalMemoryDescriptor const& external_memory, size_t log_count)
{
    static size_t s_probe_count { 0 };
    auto probe_count = ++s_probe_count;
    if (probe_count != 1 && probe_count % 120 != 0)
        return {};

    auto log_failure = [&](StringView reason) {
        dbgln("MUNDO_WEBGL_VIDEO_SKIA_YCBCR_IMPORT_PROBE attempt={} count={} frame_id={} status=failed reason={} backend={} size={}x{} planes={} single_image={} single_memory={}",
            log_count,
            probe_count,
            external_memory.frame_id,
            reason,
            Media::hardware_video_frame_backend_name(external_memory.backend),
            external_memory.size.width(),
            external_memory.size.height(),
            external_memory.plane_count,
            external_memory.single_image,
            external_memory.single_memory);
        return SkiaVulkanYcbcrProbeResult {
            .attempted = true,
            .supported = false,
            .reason = reason,
        };
    };

    if (external_memory.backend != Media::HardwareVideoFrameBackend::Vulkan) {
        return log_failure("not_vulkan"sv);
    }
    if (!external_memory.single_image || !external_memory.single_memory || external_memory.plane_count != 1) {
        return log_failure("not_single_nv12_image"sv);
    }

    auto const& plane = external_memory.planes[0];
    if (plane.fd < 0 || !plane.allocation_size) {
        return log_failure("missing_opaque_fd"sv);
    }

    auto source_fd = dup(plane.fd);
    if (source_fd < 0) {
        return log_failure("fd_dup_failed"sv);
    }

    auto& vulkan_context = m_skia_backend_context->vulkan_context();
    auto imported_source_or_error = Gfx::import_vulkan_nv12_external_memory(
        vulkan_context,
        source_fd,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
        plane.allocation_size,
        external_memory.size.width(),
        external_memory.size.height(),
        static_cast<VkFormat>(plane.vulkan_format),
        static_cast<VkImageLayout>(plane.vulkan_image_layout));
    if (imported_source_or_error.is_error()) {
        return log_failure(imported_source_or_error.error().string_literal());
    }
    auto imported_source = imported_source_or_error.release_value();
    if (!imported_source->direct_sample_ready) {
        return log_failure("vulkan_ycbcr_direct_sample_not_ready"sv);
    }

    VkFormatProperties format_properties {};
    vkGetPhysicalDeviceFormatProperties(vulkan_context.physical_device, imported_source->format, &format_properties);

    skgpu::VulkanYcbcrConversionInfo ycbcr_info {
        imported_source->format,
        VK_SAMPLER_YCBCR_MODEL_CONVERSION_YCBCR_709,
        VK_SAMPLER_YCBCR_RANGE_ITU_NARROW,
        VK_CHROMA_LOCATION_MIDPOINT,
        VK_CHROMA_LOCATION_MIDPOINT,
        VK_FILTER_LINEAR,
        VK_FALSE,
        VkComponentMapping {
            .r = VK_COMPONENT_SWIZZLE_IDENTITY,
            .g = VK_COMPONENT_SWIZZLE_IDENTITY,
            .b = VK_COMPONENT_SWIZZLE_IDENTITY,
            .a = VK_COMPONENT_SWIZZLE_IDENTITY,
        },
        format_properties.optimalTilingFeatures,
    };

    GrVkImageInfo image_info {};
    image_info.fImage = imported_source->image;
    image_info.fAlloc.fMemory = imported_source->memory;
    image_info.fAlloc.fOffset = 0;
    image_info.fAlloc.fSize = imported_source->allocation_size;
    image_info.fAlloc.fFlags = 0;
    image_info.fImageTiling = VK_IMAGE_TILING_OPTIMAL;
    image_info.fImageLayout = imported_source->layout;
    image_info.fFormat = imported_source->format;
    image_info.fImageUsageFlags = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR;
    image_info.fSampleCount = 1;
    image_info.fLevelCount = 1;
    image_info.fCurrentQueueFamily = VK_QUEUE_FAMILY_IGNORED;
    image_info.fProtected = skgpu::Protected::kNo;
    image_info.fYcbcrConversionInfo = ycbcr_info;
    image_info.fSharingMode = VK_SHARING_MODE_EXCLUSIVE;

    auto backend_texture = GrBackendTextures::MakeVk(external_memory.size.width(), external_memory.size.height(), image_info, "mundo-video-nv12-ycbcr-probe");
    if (!backend_texture.isValid()) {
        return log_failure("skia_backend_texture_invalid"sv);
    }

    m_skia_backend_context->lock();
    auto image = SkImages::BorrowTextureFrom(
        m_skia_backend_context->sk_context(),
        backend_texture,
        kTopLeft_GrSurfaceOrigin,
        kRGBA_8888_SkColorType,
        kOpaque_SkAlphaType,
        SkColorSpace::MakeSRGB());
    m_skia_backend_context->unlock();

    auto backend_format = backend_texture.getBackendFormat();
    auto const* backend_format_ycbcr_info = GrBackendFormats::GetVkYcbcrConversionInfo(backend_format);

    if (!image) {
        auto ycbcr_backend_format = GrBackendFormats::MakeVk(ycbcr_info, false);
        struct SkiaVulkanYcbcrPromiseContext {
            GrBackendTexture const* backend_texture { nullptr };
        };
        SkiaVulkanYcbcrPromiseContext promise_context {
            .backend_texture = &backend_texture,
        };
        auto promise_image = SkImages::PromiseTextureFrom(
            m_skia_backend_context->sk_context()->threadSafeProxy(),
            ycbcr_backend_format,
            SkISize::Make(external_memory.size.width(), external_memory.size.height()),
            skgpu::Mipmapped::kNo,
            kTopLeft_GrSurfaceOrigin,
            kRGBA_8888_SkColorType,
            kOpaque_SkAlphaType,
            SkColorSpace::MakeSRGB(),
            [](SkImages::PromiseImageTextureContext context) -> sk_sp<GrPromiseImageTexture> {
                auto* promise_context = static_cast<SkiaVulkanYcbcrPromiseContext*>(context);
                if (!promise_context || !promise_context->backend_texture)
                    return nullptr;
                return GrPromiseImageTexture::Make(*promise_context->backend_texture);
            },
            [](SkImages::PromiseImageTextureContext) {
            },
            &promise_context);

        GrBackendFormat yuva_backend_formats[SkYUVAInfo::kMaxPlanes] {};
        yuva_backend_formats[0] = ycbcr_backend_format;
        SkYUVAInfo yuva_info {
            SkISize::Make(external_memory.size.width(), external_memory.size.height()),
            SkYUVAInfo::PlaneConfig::kYUVA,
            SkYUVAInfo::Subsampling::k444,
            kRec709_Limited_SkYUVColorSpace,
        };
        GrYUVABackendTextureInfo yuva_backend_texture_info {
            yuva_info,
            yuva_backend_formats,
            skgpu::Mipmapped::kNo,
            kTopLeft_GrSurfaceOrigin,
        };
        SkiaVulkanYcbcrPromiseContext yuva_promise_context {
            .backend_texture = &backend_texture,
        };
        SkImages::PromiseImageTextureContext yuva_texture_contexts[SkYUVAInfo::kMaxPlanes] {};
        yuva_texture_contexts[0] = &yuva_promise_context;
        auto yuva_promise_image = SkImages::PromiseTextureFromYUVA(
            m_skia_backend_context->sk_context()->threadSafeProxy(),
            yuva_backend_texture_info,
            SkColorSpace::MakeSRGB(),
            [](SkImages::PromiseImageTextureContext context) -> sk_sp<GrPromiseImageTexture> {
                auto* promise_context = static_cast<SkiaVulkanYcbcrPromiseContext*>(context);
                if (!promise_context || !promise_context->backend_texture)
                    return nullptr;
                return GrPromiseImageTexture::Make(*promise_context->backend_texture);
            },
            [](SkImages::PromiseImageTextureContext) {
            },
            yuva_texture_contexts);

        dbgln("MUNDO_WEBGL_VIDEO_SKIA_YCBCR_IMPORT_PROBE attempt={} count={} frame_id={} status=failed reason=skia_borrow_texture_failed backend={} size={}x{} planes={} single_image={} single_memory={} backend_texture_valid={} backend_format_valid={} backend_format_has_ycbcr={} promise_format_valid={} promise_image_created={} yuva_info_valid={} yuva_backend_info_valid={} yuva_promise_image_created={}",
            log_count,
            probe_count,
            external_memory.frame_id,
            Media::hardware_video_frame_backend_name(external_memory.backend),
            external_memory.size.width(),
            external_memory.size.height(),
            external_memory.plane_count,
            external_memory.single_image,
            external_memory.single_memory,
            backend_texture.isValid(),
            backend_format.isValid(),
            backend_format_ycbcr_info != nullptr,
            ycbcr_backend_format.isValid(),
            promise_image != nullptr,
            yuva_info.isValid(),
            yuva_backend_texture_info.isValid(),
            yuva_promise_image != nullptr);
        return SkiaVulkanYcbcrProbeResult {
            .attempted = true,
            .supported = false,
            .reason = "skia_borrow_texture_failed"sv,
            .backend_texture_valid = backend_texture.isValid(),
            .backend_format_valid = backend_format.isValid(),
            .backend_format_has_ycbcr = backend_format_ycbcr_info != nullptr,
            .promise_format_valid = ycbcr_backend_format.isValid(),
            .promise_image_created = promise_image != nullptr,
        };
    }

    dbgln("MUNDO_WEBGL_VIDEO_SKIA_YCBCR_IMPORT_PROBE attempt={} count={} frame_id={} status=ok size={}x{} format={} image={} required_size={} allocation_size={} optimal_features={} backend_texture_valid={} backend_format_valid={} backend_format_has_ycbcr={}",
        log_count,
        probe_count,
        external_memory.frame_id,
        external_memory.size.width(),
        external_memory.size.height(),
        to_underlying(imported_source->format),
        reinterpret_cast<uintptr_t>(imported_source->image),
        imported_source->required_size,
        imported_source->allocation_size,
        format_properties.optimalTilingFeatures,
        backend_texture.isValid(),
        backend_format.isValid(),
        backend_format_ycbcr_info != nullptr);

    return SkiaVulkanYcbcrProbeResult {
        .attempted = true,
        .supported = true,
        .reason = "ok"sv,
        .backend_texture_valid = backend_texture.isValid(),
        .backend_format_valid = backend_format.isValid(),
        .backend_format_has_ycbcr = backend_format_ycbcr_info != nullptr,
    };
}

OpenGLContext::GLExternalVideoImportProbeResult OpenGLContext::probe_video_external_memory_gl_texture_import(Media::HardwareVideoFrameExternalMemoryDescriptor const& external_memory, size_t log_count)
{
    static size_t s_probe_count { 0 };
    static GLExternalVideoImportProbeResult s_last_result;
    auto probe_count = ++s_probe_count;
    if (probe_count != 1 && probe_count % 120 != 0)
        return s_last_result;

    auto* gl_create_memory_objects_ext = reinterpret_cast<PFNGLCREATEMEMORYOBJECTSEXTPROC>(eglGetProcAddress("glCreateMemoryObjectsEXT"));
    auto* gl_delete_memory_objects_ext = reinterpret_cast<PFNGLDELETEMEMORYOBJECTSEXTPROC>(eglGetProcAddress("glDeleteMemoryObjectsEXT"));
    auto* gl_memory_object_parameteriv_ext = reinterpret_cast<PFNGLMEMORYOBJECTPARAMETERIVEXTPROC>(eglGetProcAddress("glMemoryObjectParameterivEXT"));
    auto* gl_import_memory_fd_ext = reinterpret_cast<PFNGLIMPORTMEMORYFDEXTPROC>(eglGetProcAddress("glImportMemoryFdEXT"));
    auto* gl_tex_storage_mem_2d_ext = reinterpret_cast<PFNGLTEXSTORAGEMEM2DEXTPROC>(eglGetProcAddress("glTexStorageMem2DEXT"));

    GLExternalVideoImportProbeResult result {
        .attempted = true,
    };
    auto finish = [&] {
        s_last_result = result;
        return result;
    };

    auto probe_plane = [&](char const* label, Media::HardwareVideoFrameExternalMemoryPlane const& plane, GLenum gl_internal_format) -> ErrorOr<void> {
        auto log_failure = [&](StringView reason, u32 gl_error = 0) {
            dbgln("MUNDO_WEBGL_VIDEO_EXTERNAL_MEMORY_REAL_IMPORT_PROBE attempt={} count={} frame_id={} backend={} label={} status=failed reason={} gl_error={} fd={} allocation_size={} offset={} size={}x{} vk_format={} gl_internal_format={} layout={} has_modifier={} modifier={}",
                log_count,
                probe_count,
                external_memory.frame_id,
                Media::hardware_video_frame_backend_name(external_memory.backend),
                label,
                reason,
                gl_error,
                plane.fd,
                plane.allocation_size,
                plane.offset,
                plane.width,
                plane.height,
                plane.vulkan_format,
                gl_internal_format,
                plane.vulkan_image_layout,
                plane.has_vulkan_drm_format_modifier,
                plane.vulkan_drm_format_modifier);
        };

        if (!gl_create_memory_objects_ext || !gl_delete_memory_objects_ext || !gl_memory_object_parameteriv_ext || !gl_import_memory_fd_ext || !gl_tex_storage_mem_2d_ext) {
            log_failure("missing_gl_memory_object_fd_extension"sv);
            return Error::from_string_literal("missing_gl_memory_object_fd_extension");
        }
        if (plane.fd < 0 || !plane.allocation_size || !plane.width || !plane.height) {
            log_failure("empty_plane"sv);
            return Error::from_string_literal("empty_plane");
        }
        if (plane.offset < 0) {
            log_failure("negative_plane_offset"sv);
            return Error::from_string_literal("negative_plane_offset");
        }

        auto import_fd = dup(plane.fd);
        if (import_fd < 0) {
            log_failure("fd_dup_failed"sv);
            return Error::from_string_literal("fd_dup_failed");
        }

        GLuint memory_object { 0 };
        GLuint texture { 0 };
        auto cleanup = ArmedScopeGuard([&] {
            if (texture)
                glDeleteTextures(1, &texture);
            if (memory_object)
                gl_delete_memory_objects_ext(1, &memory_object);
            if (import_fd >= 0)
                close(import_fd);
        });

        while (glGetError() != GL_NO_ERROR) {
        }

        gl_create_memory_objects_ext(1, &memory_object);
        auto gl_error = glGetError();
        if (gl_error != GL_NO_ERROR || !memory_object) {
            log_failure("create_memory_object_failed"sv, gl_error);
            return Error::from_string_literal("create_memory_object_failed");
        }

        GLint dedicated = GL_TRUE;
        gl_memory_object_parameteriv_ext(memory_object, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
        gl_error = glGetError();
        if (gl_error != GL_NO_ERROR) {
            log_failure("set_dedicated_failed"sv, gl_error);
            return Error::from_string_literal("set_dedicated_failed");
        }

        gl_import_memory_fd_ext(memory_object, plane.allocation_size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, import_fd);
        import_fd = -1;
        gl_error = glGetError();
        if (gl_error != GL_NO_ERROR) {
            log_failure("import_fd_failed"sv, gl_error);
            return Error::from_string_literal("import_fd_failed");
        }

        glGenTextures(1, &texture);
        gl_error = glGetError();
        if (gl_error != GL_NO_ERROR || !texture) {
            log_failure("create_texture_failed"sv, gl_error);
            return Error::from_string_literal("create_texture_failed");
        }

        glBindTexture(GL_TEXTURE_2D, texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl_tex_storage_mem_2d_ext(GL_TEXTURE_2D, 1, gl_internal_format, plane.width, plane.height, memory_object, plane.offset);
        gl_error = glGetError();
        if (gl_error != GL_NO_ERROR) {
            log_failure("texture_storage_failed"sv, gl_error);
            if (!strcmp(label, "inferred_uv_rg8") || !strcmp(label, "plane1_rg8"))
                result.uv_plane_gl_error = gl_error;
            return Error::from_string_literal("texture_storage_failed");
        }

        dbgln("MUNDO_WEBGL_VIDEO_EXTERNAL_MEMORY_REAL_IMPORT_PROBE attempt={} count={} frame_id={} backend={} label={} status=ok texture={} memory_object={} fd={} allocation_size={} offset={} size={}x{} vk_format={} gl_internal_format={} layout={} has_modifier={} modifier={}",
            log_count,
            probe_count,
            external_memory.frame_id,
            Media::hardware_video_frame_backend_name(external_memory.backend),
            label,
            texture,
            memory_object,
            plane.fd,
            plane.allocation_size,
            plane.offset,
            plane.width,
            plane.height,
            plane.vulkan_format,
            gl_internal_format,
            plane.vulkan_image_layout,
            plane.has_vulkan_drm_format_modifier,
            plane.vulkan_drm_format_modifier);
        return {};
    };

    if (external_memory.backend != Media::HardwareVideoFrameBackend::Vulkan) {
        dbgln("MUNDO_WEBGL_VIDEO_EXTERNAL_MEMORY_REAL_IMPORT_PROBE attempt={} count={} frame_id={} backend={} label=all status=failed reason=not_vulkan",
            log_count,
            probe_count,
            external_memory.frame_id,
            Media::hardware_video_frame_backend_name(external_memory.backend));
        result.y_plane_reason = "not_vulkan"sv;
        result.uv_plane_reason = "not_vulkan"sv;
        return finish();
    }
    if (external_memory.plane_count < 1) {
        dbgln("MUNDO_WEBGL_VIDEO_EXTERNAL_MEMORY_REAL_IMPORT_PROBE attempt={} count={} frame_id={} backend={} label=all status=failed reason=no_planes",
            log_count,
            probe_count,
            external_memory.frame_id,
            Media::hardware_video_frame_backend_name(external_memory.backend));
        result.y_plane_reason = "no_planes"sv;
        result.uv_plane_reason = "no_planes"sv;
        return finish();
    }

    auto y_result = probe_plane("plane0_r8", external_memory.planes[0], GL_R8_EXT);
    result.y_plane_supported = !y_result.is_error();
    result.y_plane_reason = y_result.is_error() ? y_result.error().string_literal() : "ok"sv;
    if (external_memory.plane_count > 1) {
        auto uv_result = probe_plane("plane1_rg8", external_memory.planes[1], GL_RG8_EXT);
        result.uv_plane_supported = !uv_result.is_error();
        result.uv_plane_reason = uv_result.is_error() ? uv_result.error().string_literal() : "ok"sv;
        return finish();
    }

    if (!external_memory.single_memory || external_memory.planes[0].fd < 0 || !external_memory.planes[0].width || !external_memory.planes[0].height) {
        result.uv_plane_reason = "cannot_infer_uv_plane"sv;
        return finish();
    }

    constexpr u32 vulkan_image_tiling_linear = 1;
    if (!external_memory.planes[0].has_vulkan_drm_format_modifier && external_memory.vulkan_tiling != vulkan_image_tiling_linear) {
        result.uv_plane_reason = "single_optimal_multiplanar_requires_vulkan_ycbcr_sampler"sv;
        dbgln("MUNDO_WEBGL_VIDEO_EXTERNAL_MEMORY_REAL_IMPORT_PROBE attempt={} count={} frame_id={} backend={} label=inferred_uv_rg8 status=skipped reason=single_optimal_multiplanar_requires_vulkan_ycbcr_sampler tiling={} has_modifier={} allocation_size={} size={}x{}",
            log_count,
            probe_count,
            external_memory.frame_id,
            Media::hardware_video_frame_backend_name(external_memory.backend),
            external_memory.vulkan_tiling,
            external_memory.planes[0].has_vulkan_drm_format_modifier,
            external_memory.planes[0].allocation_size,
            external_memory.planes[0].width,
            external_memory.planes[0].height);
        return finish();
    }

    auto inferred_uv_plane = external_memory.planes[0];
    auto y_plane_byte_count = static_cast<u64>(external_memory.planes[0].width) * static_cast<u64>(external_memory.planes[0].height);
    inferred_uv_plane.offset = static_cast<i64>(static_cast<u64>(external_memory.planes[0].offset) + y_plane_byte_count);
    inferred_uv_plane.width = (external_memory.planes[0].width + 1) / 2;
    inferred_uv_plane.height = (external_memory.planes[0].height + 1) / 2;
    if (inferred_uv_plane.offset >= 0 && static_cast<u64>(inferred_uv_plane.offset) < inferred_uv_plane.allocation_size) {
        auto uv_result = probe_plane("inferred_uv_rg8", inferred_uv_plane, GL_RG8_EXT);
        result.uv_plane_supported = !uv_result.is_error();
        result.uv_plane_reason = uv_result.is_error() ? uv_result.error().string_literal() : "ok"sv;
    } else {
        result.uv_plane_reason = "invalid_inferred_offset"sv;
        dbgln("MUNDO_WEBGL_VIDEO_EXTERNAL_MEMORY_REAL_IMPORT_PROBE attempt={} count={} frame_id={} backend={} label=inferred_uv_rg8 status=skipped reason=invalid_inferred_offset allocation_size={} offset={} size={}x{}",
            log_count,
            probe_count,
            external_memory.frame_id,
            Media::hardware_video_frame_backend_name(external_memory.backend),
            inferred_uv_plane.allocation_size,
            inferred_uv_plane.offset,
            inferred_uv_plane.width,
            inferred_uv_plane.height);
    }

    return finish();
}

ErrorOr<NonnullOwnPtr<Gfx::ImportedVulkanNV12Image>> OpenGLContext::import_retained_vulkan_video_source_for_virtual_draw(int source_opaque_fd, u32 source_handle_type, u64 source_allocation_size, u32 width, u32 height, u32 source_format, u32 source_layout)
{
    if (source_opaque_fd < 0)
        return Error::from_string_literal("missing_retained_source_opaque_fd");
    if (source_handle_type != to_underlying(VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT))
        return Error::from_string_literal("unsupported_source_handle_type");
    if (!source_allocation_size)
        return Error::from_string_literal("missing_source_allocation_size");

    auto import_fd = dup(source_opaque_fd);
    if (import_fd < 0)
        return Error::from_string_literal("fd_dup_failed");

    return Gfx::import_vulkan_nv12_external_memory(
        m_skia_backend_context->vulkan_context(),
        import_fd,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT,
        source_allocation_size,
        width,
        height,
        static_cast<VkFormat>(source_format),
        static_cast<VkImageLayout>(source_layout));
}

OpenGLContext::RetainedVulkanVideoSourceProbeResult OpenGLContext::probe_retained_vulkan_video_source_for_virtual_draw(int source_opaque_fd, u32 source_handle_type, u64 source_allocation_size, u32 width, u32 height, u32 source_format, u32 source_layout, u64 frame_id, size_t log_count)
{
    static size_t s_probe_count { 0 };
    auto probe_count = ++s_probe_count;
    auto should_log = probe_count <= 8 || probe_count % 120 == 0;

    auto log_failure = [&](StringView reason) {
        if (should_log) {
            dbgln("MUNDO_WEBGL_VIDEO_VIRTUAL_SOURCE_IMPORT_PROBE draw_count={} probe_count={} frame_id={} status=failed reason={} source_fd_retained={} handle_type={} size={}x{} source_format={} source_layout={} allocation_size={} next_step=fix_retained_source_before_virtual_draw",
                log_count,
                probe_count,
                frame_id,
                reason,
                source_opaque_fd >= 0,
                source_handle_type,
                width,
                height,
                source_format,
                source_layout,
                source_allocation_size);
        }
        return RetainedVulkanVideoSourceProbeResult {
            .attempted = true,
            .supported = false,
            .direct_sample_ready = false,
            .reason = reason,
        };
    };

    auto imported_source_or_error = import_retained_vulkan_video_source_for_virtual_draw(source_opaque_fd, source_handle_type, source_allocation_size, width, height, source_format, source_layout);
    if (imported_source_or_error.is_error())
        return log_failure(imported_source_or_error.error().string_literal());

    auto imported_source = imported_source_or_error.release_value();
    if (should_log) {
        dbgln("MUNDO_WEBGL_VIDEO_VIRTUAL_SOURCE_IMPORT_PROBE draw_count={} probe_count={} frame_id={} status=ok direct_sample_ready={} source_fd_retained=true handle_type={} size={}x{} source_format={} source_layout={} allocation_size={} required_size={} image={} image_view={} sampler={} conversion={} next_step={}",
            log_count,
            probe_count,
            frame_id,
            imported_source->direct_sample_ready,
            source_handle_type,
            width,
            height,
            source_format,
            source_layout,
            source_allocation_size,
            imported_source->required_size,
            reinterpret_cast<uintptr_t>(imported_source->image),
            reinterpret_cast<uintptr_t>(imported_source->ycbcr_image_view),
            reinterpret_cast<uintptr_t>(imported_source->ycbcr_sampler),
            reinterpret_cast<uintptr_t>(imported_source->ycbcr_conversion),
            imported_source->direct_sample_ready ? "cache_imported_source_for_virtual_draw" : "fix_ycbcr_direct_sample_resources");
    }

    return RetainedVulkanVideoSourceProbeResult {
        .attempted = true,
        .supported = imported_source->direct_sample_ready,
        .direct_sample_ready = imported_source->direct_sample_ready,
        .reason = imported_source->direct_sample_ready ? "ok"sv : "direct_sample_not_ready"sv,
        .required_size = imported_source->required_size,
        .allocation_size = imported_source->allocation_size,
    };
}

OpenGLContext::VulkanVideoReplayBufferProbeResult OpenGLContext::probe_vulkan_video_replay_buffers(ReadonlyBytes position_data, ReadonlyBytes uv_data, ReadonlyBytes uv_right_data, ReadonlyBytes index_data, u64 frame_id, size_t log_count)
{
    static size_t s_probe_count { 0 };
    auto probe_count = ++s_probe_count;
    auto should_log = probe_count <= 8 || probe_count % 120 == 0;
    auto total_bytes = position_data.size() + uv_data.size() + uv_right_data.size() + index_data.size();
    auto signature = pair_int_hash(
        pair_int_hash(Traits<ReadonlyBytes>::hash(position_data), Traits<ReadonlyBytes>::hash(uv_data)),
        pair_int_hash(Traits<ReadonlyBytes>::hash(uv_right_data), Traits<ReadonlyBytes>::hash(index_data)));
    signature = pair_int_hash(signature, pair_int_hash(u32_hash(position_data.size()), u32_hash(uv_data.size())));
    signature = pair_int_hash(signature, pair_int_hash(u32_hash(uv_right_data.size()), u32_hash(index_data.size())));

    auto log_failure = [&](StringView reason) {
        if (should_log) {
            dbgln("MUNDO_WEBGL_VIDEO_VULKAN_REPLAY_BUFFER_PROBE draw_count={} probe_count={} frame_id={} status=failed cache_status=miss reason={} signature={} position_bytes={} uv_bytes={} uv_right_bytes={} index_bytes={} total_bytes={} next_step=fix_shadowed_buffers_before_vulkan_replay",
                log_count,
                probe_count,
                frame_id,
                reason,
                signature,
                position_data.size(),
                uv_data.size(),
                uv_right_data.size(),
                index_data.size(),
                total_bytes);
        }
        return VulkanVideoReplayBufferProbeResult {
            .attempted = true,
            .supported = false,
            .reason = reason,
            .total_bytes = total_bytes,
        };
    };

    if (position_data.is_empty())
        return log_failure("missing_position_data"sv);
    if (uv_data.is_empty())
        return log_failure("missing_uv_data"sv);
    if (index_data.is_empty())
        return log_failure("missing_index_data"sv);

    auto cache_matches = [&] {
        if (!m_impl->cached_vulkan_video_replay_buffers)
            return false;
        auto const& cached = *m_impl->cached_vulkan_video_replay_buffers;
        return cached.signature == signature
            && cached.position_bytes == position_data.size()
            && cached.uv_bytes == uv_data.size()
            && cached.uv_right_bytes == uv_right_data.size()
            && cached.index_bytes == index_data.size();
    };

    if (cache_matches()) {
        if (should_log) {
            auto const& cached = *m_impl->cached_vulkan_video_replay_buffers;
            dbgln("MUNDO_WEBGL_VIDEO_VULKAN_REPLAY_BUFFER_PROBE draw_count={} probe_count={} frame_id={} status=ok cache_status=hit signature={} position_buffer={} uv_buffer={} uv_right_buffer={} index_buffer={} position_bytes={} uv_bytes={} uv_right_bytes={} index_bytes={} total_bytes={} next_step=build_or_reuse_vulkan_video_mesh_pipeline",
                log_count,
                probe_count,
                frame_id,
                signature,
                reinterpret_cast<uintptr_t>(cached.position_buffer->buffer),
                reinterpret_cast<uintptr_t>(cached.uv_buffer->buffer),
                cached.uv_right_buffer ? reinterpret_cast<uintptr_t>(cached.uv_right_buffer->buffer) : 0,
                reinterpret_cast<uintptr_t>(cached.index_buffer->buffer),
                position_data.size(),
                uv_data.size(),
                uv_right_data.size(),
                index_data.size(),
                total_bytes);
        }
        return VulkanVideoReplayBufferProbeResult {
            .attempted = true,
            .supported = true,
            .reason = "ok"sv,
            .total_bytes = total_bytes,
        };
    }

    auto const& vulkan_context = m_skia_backend_context->vulkan_context();
    auto position_buffer_or_error = Gfx::create_host_visible_vulkan_buffer_from_bytes(vulkan_context, position_data, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (position_buffer_or_error.is_error())
        return log_failure(position_buffer_or_error.error().string_literal());
    auto position_buffer = position_buffer_or_error.release_value();

    auto uv_buffer_or_error = Gfx::create_host_visible_vulkan_buffer_from_bytes(vulkan_context, uv_data, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    if (uv_buffer_or_error.is_error())
        return log_failure(uv_buffer_or_error.error().string_literal());
    auto uv_buffer = uv_buffer_or_error.release_value();

    OwnPtr<Gfx::VulkanBuffer> uv_right_buffer;
    if (!uv_right_data.is_empty()) {
        auto uv_right_buffer_or_error = Gfx::create_host_visible_vulkan_buffer_from_bytes(vulkan_context, uv_right_data, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        if (uv_right_buffer_or_error.is_error())
            return log_failure(uv_right_buffer_or_error.error().string_literal());
        uv_right_buffer = uv_right_buffer_or_error.release_value();
    }

    auto index_buffer_or_error = Gfx::create_host_visible_vulkan_buffer_from_bytes(vulkan_context, index_data, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    if (index_buffer_or_error.is_error())
        return log_failure(index_buffer_or_error.error().string_literal());
    auto index_buffer = index_buffer_or_error.release_value();

    m_impl->cached_vulkan_video_replay_buffers = make<Impl::CachedVulkanVideoReplayBuffers>(
        signature,
        position_data.size(),
        uv_data.size(),
        uv_right_data.size(),
        index_data.size(),
        move(position_buffer),
        move(uv_buffer),
        move(uv_right_buffer),
        move(index_buffer));

    if (should_log) {
        auto const& cached = *m_impl->cached_vulkan_video_replay_buffers;
        dbgln("MUNDO_WEBGL_VIDEO_VULKAN_REPLAY_BUFFER_PROBE draw_count={} probe_count={} frame_id={} status=ok cache_status=filled signature={} position_buffer={} uv_buffer={} uv_right_buffer={} index_buffer={} position_bytes={} uv_bytes={} uv_right_bytes={} index_bytes={} total_bytes={} next_step=build_or_reuse_vulkan_video_mesh_pipeline",
            log_count,
            probe_count,
            frame_id,
            signature,
            reinterpret_cast<uintptr_t>(cached.position_buffer->buffer),
            reinterpret_cast<uintptr_t>(cached.uv_buffer->buffer),
            cached.uv_right_buffer ? reinterpret_cast<uintptr_t>(cached.uv_right_buffer->buffer) : 0,
            reinterpret_cast<uintptr_t>(cached.index_buffer->buffer),
            position_data.size(),
            uv_data.size(),
            uv_right_data.size(),
            index_data.size(),
            total_bytes);
    }

    return VulkanVideoReplayBufferProbeResult {
        .attempted = true,
        .supported = true,
        .reason = "ok"sv,
        .total_bytes = total_bytes,
    };
}

static ErrorOr<VkShaderModule> create_mundo_vulkan_video_shader_module(Gfx::VulkanContext const& context, ReadonlySpan<u32 const> spirv)
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
        return Error::from_string_literal("failed to create Vulkan video mesh shader module");
    return shader_module;
}

OpenGLContext::VulkanVideoMeshPipelineProbeResult OpenGLContext::probe_vulkan_video_mesh_pipeline(u64 frame_id, u32 destination_format, VkImage source_image, VkImageView source_image_view, VkSampler immutable_sampler, u32 source_layout, VulkanVideoMeshUniformSnapshot const& uniform_snapshot, u32 draw_count, u32 draw_type, u64 draw_offset, int viewport_x, int viewport_y, int viewport_width, int viewport_height, size_t log_count)
{
    struct MeshPushConstants {
        Array<float, 16> model_view_matrix {};
        Array<float, 16> projection_matrix {};
        float use_matrices { 0.0f };
        float opacity { 1.0f };
        float output_intensity { 1.0f };
        float stereo_eye { 0.0f };
        float stereo_eye_left { 1.0f };
    };
    constexpr size_t mesh_ring_slot_count = 3;
    struct MeshPipelineResources {
        VkDevice device { VK_NULL_HANDLE };
        VkFormat destination_format { VK_FORMAT_UNDEFINED };
        VkSampler immutable_sampler { VK_NULL_HANDLE };
        VkImageView last_source_image_view { VK_NULL_HANDLE };
        VkShaderModule vertex_shader { VK_NULL_HANDLE };
        VkShaderModule fragment_shader { VK_NULL_HANDLE };
        VkRenderPass render_pass { VK_NULL_HANDLE };
        VkDescriptorSetLayout descriptor_set_layout { VK_NULL_HANDLE };
        VkPipelineLayout pipeline_layout { VK_NULL_HANDLE };
        VkPipeline pipeline { VK_NULL_HANDLE };
        VkDescriptorPool descriptor_pool { VK_NULL_HANDLE };
        VkDescriptorSet descriptor_set { VK_NULL_HANDLE };
        Array<VkDescriptorSet, mesh_ring_slot_count> ring_descriptor_sets {};
        VkCommandPool ring_command_pool { VK_NULL_HANDLE };
        Array<VkCommandBuffer, mesh_ring_slot_count> ring_command_buffers {};
        Array<VkFence, mesh_ring_slot_count> ring_fences {};
        size_t ring_cursor { 0 };
    };
    static MeshPipelineResources s_resources;
    static size_t s_probe_count { 0 };
    auto probe_count = ++s_probe_count;
    auto should_log = probe_count <= 8 || probe_count % 120 == 0;

    auto log_failure = [&](StringView reason, VkResult result = VK_SUCCESS) {
        if (should_log) {
            dbgln("MUNDO_WEBGL_VIDEO_VULKAN_MESH_PIPELINE_PROBE draw_count={} probe_count={} frame_id={} status=failed reason={} vk_result={} destination_format={} source_image_view={} sampler={} next_step=fix_vulkan_mesh_pipeline_before_draw",
                log_count,
                probe_count,
                frame_id,
                reason,
                to_underlying(result),
                destination_format,
                reinterpret_cast<uintptr_t>(source_image_view),
                reinterpret_cast<uintptr_t>(immutable_sampler));
        }
        return VulkanVideoMeshPipelineProbeResult {
            .attempted = true,
            .supported = false,
            .reason = reason,
        };
    };

    if (immutable_sampler == VK_NULL_HANDLE)
        return log_failure("missing_ycbcr_sampler"sv);
    if (source_image_view == VK_NULL_HANDLE)
        return log_failure("missing_ycbcr_image_view"sv);
    if (viewport_width <= 0 || viewport_height <= 0)
        return log_failure("invalid_viewport"sv);

    auto const& context = m_skia_backend_context->vulkan_context();
    auto format = static_cast<VkFormat>(destination_format);
    auto pipeline_cache_status = "filled"sv;
    VkResult result { VK_SUCCESS };
    if (s_resources.pipeline != VK_NULL_HANDLE) {
        auto matches = s_resources.device == context.logical_device
            && s_resources.destination_format == format
            && s_resources.immutable_sampler == immutable_sampler;
        if (!matches)
            return log_failure("multiple_mesh_pipeline_configurations_not_supported_yet"sv);
        pipeline_cache_status = "hit"sv;
    }

    // Keep the direct video path conservative by default: source images come
    // from live decoder frames, and the async ring path can outlive them unless
    // every submitted frame is explicitly retained per slot.
    auto queue_sync_mode = "fence"sv;
    if (auto const* queue_sync_mode_value = getenv("MUNDO_WEBGL_VIDEO_VULKAN_MESH_QUEUE_SYNC_MODE")) {
        auto value = StringView { queue_sync_mode_value, strlen(queue_sync_mode_value) };
        if (value == "fence"sv)
            queue_sync_mode = "fence"sv;
        else if (value == "none"sv)
            queue_sync_mode = "none"sv;
        else if (value == "ring"sv)
            queue_sync_mode = "ring"sv;
        else if (value == "idle"sv)
            queue_sync_mode = "idle"sv;
    }

    if (s_resources.pipeline == VK_NULL_HANDLE) {
        auto vertex_shader_or_error = create_mundo_vulkan_video_shader_module(context, s_mundo_video_nv12_mesh_vertex_shader_spirv);
        if (vertex_shader_or_error.is_error())
            return log_failure(vertex_shader_or_error.error().string_literal());
        s_resources.vertex_shader = vertex_shader_or_error.release_value();

    auto fragment_shader_or_error = create_mundo_vulkan_video_shader_module(context, s_mundo_video_nv12_mesh_fragment_shader_spirv);
    if (fragment_shader_or_error.is_error())
        return log_failure(fragment_shader_or_error.error().string_literal());
    s_resources.fragment_shader = fragment_shader_or_error.release_value();

    VkAttachmentDescription color_attachment {
        .flags = 0,
        .format = format,
        .samples = VK_SAMPLE_COUNT_1_BIT,
        .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
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
        return log_failure("create_render_pass_failed"sv, result);

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
        return log_failure("create_descriptor_set_layout_failed"sv, result);

    VkPushConstantRange push_constant_range {
        .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
        .offset = 0,
        .size = sizeof(MeshPushConstants),
    };
    VkPipelineLayoutCreateInfo pipeline_layout_info {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .setLayoutCount = 1,
        .pSetLayouts = &s_resources.descriptor_set_layout,
        .pushConstantRangeCount = 1,
        .pPushConstantRanges = &push_constant_range,
    };
    result = vkCreatePipelineLayout(context.logical_device, &pipeline_layout_info, nullptr, &s_resources.pipeline_layout);
    if (result != VK_SUCCESS)
        return log_failure("create_pipeline_layout_failed"sv, result);

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
    VkVertexInputBindingDescription vertex_bindings[] {
        { .binding = 0, .stride = sizeof(float) * 3, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
        { .binding = 1, .stride = sizeof(float) * 2, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
        { .binding = 2, .stride = sizeof(float) * 2, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
    };
    VkVertexInputAttributeDescription vertex_attributes[] {
        { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0 },
        { .location = 1, .binding = 1, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 },
        { .location = 2, .binding = 2, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 },
    };
    VkPipelineVertexInputStateCreateInfo vertex_input_info {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .vertexBindingDescriptionCount = 3,
        .pVertexBindingDescriptions = vertex_bindings,
        .vertexAttributeDescriptionCount = 3,
        .pVertexAttributeDescriptions = vertex_attributes,
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
    VkDynamicState dynamic_states[] { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
    VkPipelineDynamicStateCreateInfo dynamic_state {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .dynamicStateCount = 2,
        .pDynamicStates = dynamic_states,
    };
    VkGraphicsPipelineCreateInfo pipeline_info {
        .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .stageCount = 2,
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
    if (result != VK_SUCCESS)
        return log_failure("create_graphics_pipeline_failed"sv, result);

    VkDescriptorPoolSize pool_size {
        .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .descriptorCount = 1 + mesh_ring_slot_count,
    };
    VkDescriptorPoolCreateInfo descriptor_pool_info {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
        .pNext = nullptr,
        .flags = 0,
        .maxSets = 1 + mesh_ring_slot_count,
        .poolSizeCount = 1,
        .pPoolSizes = &pool_size,
    };
    result = vkCreateDescriptorPool(context.logical_device, &descriptor_pool_info, nullptr, &s_resources.descriptor_pool);
    if (result != VK_SUCCESS)
        return log_failure("create_descriptor_pool_failed"sv, result);

    VkDescriptorSetLayout descriptor_set_layouts[1 + mesh_ring_slot_count];
    for (size_t i = 0; i < 1 + mesh_ring_slot_count; ++i)
        descriptor_set_layouts[i] = s_resources.descriptor_set_layout;
    VkDescriptorSet descriptor_sets[1 + mesh_ring_slot_count];
    VkDescriptorSetAllocateInfo descriptor_set_allocate_info {
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .pNext = nullptr,
        .descriptorPool = s_resources.descriptor_pool,
        .descriptorSetCount = 1 + mesh_ring_slot_count,
        .pSetLayouts = descriptor_set_layouts,
    };
    result = vkAllocateDescriptorSets(context.logical_device, &descriptor_set_allocate_info, descriptor_sets);
    if (result != VK_SUCCESS) {
        return log_failure("allocate_descriptor_set_failed"sv, result);
    }
    s_resources.descriptor_set = descriptor_sets[0];
    for (size_t i = 0; i < mesh_ring_slot_count; ++i)
        s_resources.ring_descriptor_sets[i] = descriptor_sets[i + 1];

    s_resources.device = context.logical_device;
    s_resources.destination_format = format;
    s_resources.immutable_sampler = immutable_sampler;
    }

    i64 queue_submit_us = 0;
    i64 queue_wait_us = 0;
    auto descriptor_set_for_draw = s_resources.descriptor_set;
    VkCommandBuffer command_buffer_for_draw = context.command_buffer;
    VkFence submit_fence = VK_NULL_HANDLE;
    size_t queue_ring_slot = NumericLimits<size_t>::max();
    if (queue_sync_mode == "ring"sv) {
        if (s_resources.ring_command_pool == VK_NULL_HANDLE) {
            VkCommandPoolCreateInfo command_pool_info {
                .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
                .pNext = nullptr,
                .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
                .queueFamilyIndex = context.graphics_queue_family,
            };
            result = vkCreateCommandPool(context.logical_device, &command_pool_info, nullptr, &s_resources.ring_command_pool);
            if (result != VK_SUCCESS)
                return log_failure("create_mesh_ring_command_pool_failed"sv, result);

            VkCommandBufferAllocateInfo command_buffer_alloc_info {
                .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
                .pNext = nullptr,
                .commandPool = s_resources.ring_command_pool,
                .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
                .commandBufferCount = mesh_ring_slot_count,
            };
            result = vkAllocateCommandBuffers(context.logical_device, &command_buffer_alloc_info, s_resources.ring_command_buffers.data());
            if (result != VK_SUCCESS)
                return log_failure("allocate_mesh_ring_command_buffers_failed"sv, result);

            VkFenceCreateInfo fence_info {
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .pNext = nullptr,
                .flags = VK_FENCE_CREATE_SIGNALED_BIT,
            };
            for (size_t i = 0; i < mesh_ring_slot_count; ++i) {
                result = vkCreateFence(context.logical_device, &fence_info, nullptr, &s_resources.ring_fences[i]);
                if (result != VK_SUCCESS)
                    return log_failure("create_mesh_ring_fence_failed"sv, result);
            }
        }

        queue_ring_slot = s_resources.ring_cursor++ % mesh_ring_slot_count;
        auto wait_started_at = MonotonicTime::now();
        result = vkWaitForFences(context.logical_device, 1, &s_resources.ring_fences[queue_ring_slot], VK_TRUE, UINT64_MAX);
        queue_wait_us = (MonotonicTime::now() - wait_started_at).to_microseconds();
        if (result != VK_SUCCESS)
            return log_failure("wait_mesh_ring_fence_failed"sv, result);
        result = vkResetFences(context.logical_device, 1, &s_resources.ring_fences[queue_ring_slot]);
        if (result != VK_SUCCESS)
            return log_failure("reset_mesh_ring_fence_failed"sv, result);
        command_buffer_for_draw = s_resources.ring_command_buffers[queue_ring_slot];
        descriptor_set_for_draw = s_resources.ring_descriptor_sets[queue_ring_slot];
        submit_fence = s_resources.ring_fences[queue_ring_slot];
    }

    VkDescriptorImageInfo descriptor_image_info {
        .sampler = VK_NULL_HANDLE,
        .imageView = source_image_view,
        .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
    };
    VkWriteDescriptorSet descriptor_write {
        .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
        .pNext = nullptr,
        .dstSet = descriptor_set_for_draw,
        .dstBinding = 0,
        .dstArrayElement = 0,
        .descriptorCount = 1,
        .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
        .pImageInfo = &descriptor_image_info,
        .pBufferInfo = nullptr,
        .pTexelBufferView = nullptr,
    };
    vkUpdateDescriptorSets(context.logical_device, 1, &descriptor_write, 0, nullptr);
    s_resources.last_source_image_view = source_image_view;

    auto target_image = m_painting_surface ? m_painting_surface->vulkan_image() : nullptr;
    if (!target_image)
        return log_failure("missing_vulkan_painting_surface_target"sv);
    if (target_image->info.format != format)
        return log_failure("vulkan_painting_surface_format_mismatch"sv);
    if (!(target_image->info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        return log_failure("vulkan_painting_surface_not_color_attachment"sv);

    if (target_image->cached_video_framebuffer != VK_NULL_HANDLE
        && target_image->cached_video_framebuffer_render_pass != s_resources.render_pass) {
        vkDestroyFramebuffer(context.logical_device, target_image->cached_video_framebuffer, nullptr);
        target_image->cached_video_framebuffer = VK_NULL_HANDLE;
    }
    if (target_image->cached_video_color_attachment_view == VK_NULL_HANDLE) {
        VkImageViewCreateInfo target_image_view_info {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = target_image->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = target_image->info.format,
            .components = {
                .r = VK_COMPONENT_SWIZZLE_IDENTITY,
                .g = VK_COMPONENT_SWIZZLE_IDENTITY,
                .b = VK_COMPONENT_SWIZZLE_IDENTITY,
                .a = VK_COMPONENT_SWIZZLE_IDENTITY,
            },
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        result = vkCreateImageView(context.logical_device, &target_image_view_info, nullptr, &target_image->cached_video_color_attachment_view);
        if (result != VK_SUCCESS)
            return log_failure("create_target_image_view_failed"sv, result);
    }
    if (target_image->cached_video_framebuffer == VK_NULL_HANDLE
        || target_image->cached_video_framebuffer_width != target_image->info.extent.width
        || target_image->cached_video_framebuffer_height != target_image->info.extent.height) {
        if (target_image->cached_video_framebuffer != VK_NULL_HANDLE)
            vkDestroyFramebuffer(context.logical_device, target_image->cached_video_framebuffer, nullptr);
        VkFramebufferCreateInfo framebuffer_info {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .renderPass = s_resources.render_pass,
            .attachmentCount = 1,
            .pAttachments = &target_image->cached_video_color_attachment_view,
            .width = target_image->info.extent.width,
            .height = target_image->info.extent.height,
            .layers = 1,
        };
        result = vkCreateFramebuffer(context.logical_device, &framebuffer_info, nullptr, &target_image->cached_video_framebuffer);
        if (result != VK_SUCCESS)
            return log_failure("create_target_framebuffer_failed"sv, result);
        target_image->cached_video_framebuffer_render_pass = s_resources.render_pass;
        target_image->cached_video_framebuffer_width = target_image->info.extent.width;
        target_image->cached_video_framebuffer_height = target_image->info.extent.height;
    }

    auto direct_vulkan_mesh_mode = mundo_webgl_video_direct_vulkan_mesh_enabled();
    auto mesh_draw_enabled = [direct_vulkan_mesh_mode] {
        if (direct_vulkan_mesh_mode)
            return true;
        auto const* value = getenv("MUNDO_WEBGL_VIDEO_VULKAN_MESH_DRAW");
        return value && StringView { value, strlen(value) } == "1"sv;
    }();
    auto draw_status = "disabled"sv;
    auto draw_reason = "set_MUNDO_WEBGL_VIDEO_VULKAN_MESH_DRAW_1_to_execute"sv;
    bool mesh_draw_executed = false;
    auto flip_mesh_viewport_y = [direct_vulkan_mesh_mode] {
        if (direct_vulkan_mesh_mode)
            return true;

        auto const* value = getenv("MUNDO_WEBGL_VIDEO_VULKAN_MESH_FLIP_Y");
        if (value && StringView { value, strlen(value) } == "1"sv)
            return true;

        auto const* force_replace = getenv("MUNDO_WEBGL_VIDEO_VULKAN_MESH_REPLACE_GL_FORCE_TARGET_MISMATCH");
        if (force_replace && StringView { force_replace, strlen(force_replace) } == "1"sv)
            return true;

        auto const* replace_gl = getenv("MUNDO_WEBGL_VIDEO_VULKAN_MESH_REPLACE_GL");
        auto const* skip_rgba_upload = getenv("MUNDO_WEBGL_VIDEO_VULKAN_MESH_SKIP_RGBA_UPLOAD_FOR_REPLACE");
        return replace_gl && StringView { replace_gl, strlen(replace_gl) } == "1"sv
            && skip_rgba_upload && StringView { skip_rgba_upload, strlen(skip_rgba_upload) } == "1"sv;
    }();
    if (mesh_draw_enabled) {
        auto index_type = VK_INDEX_TYPE_UINT16;
        if (draw_type == GL_UNSIGNED_SHORT)
            index_type = VK_INDEX_TYPE_UINT16;
        else if (draw_type == GL_UNSIGNED_INT)
            index_type = VK_INDEX_TYPE_UINT32;
        else {
            return log_failure("unsupported_index_type_for_vulkan_mesh_draw"sv);
        }

        if (!m_impl->cached_vulkan_video_replay_buffers)
            return log_failure("missing_cached_replay_buffers_for_draw"sv);
        auto const& replay_buffers = *m_impl->cached_vulkan_video_replay_buffers;

        vkResetCommandBuffer(command_buffer_for_draw, 0);
        VkCommandBufferBeginInfo begin_info {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
            .pInheritanceInfo = nullptr,
        };
        result = vkBeginCommandBuffer(command_buffer_for_draw, &begin_info);
        if (result != VK_SUCCESS)
            return log_failure("begin_mesh_draw_command_buffer_failed"sv, result);

        VkImageMemoryBarrier pre_render_barriers[] {
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT | VK_ACCESS_MEMORY_READ_BIT,
                .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
                .oldLayout = static_cast<VkImageLayout>(source_layout),
                .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source_image,
                .subresourceRange = { VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 1, 0, 1 },
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
                .oldLayout = target_image->info.layout,
                .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = target_image->image,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            },
        };
        vkCmdPipelineBarrier(command_buffer_for_draw,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            0,
            0, nullptr,
            0, nullptr,
            2, pre_render_barriers);

        VkRenderPassBeginInfo render_pass_begin {
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .pNext = nullptr,
            .renderPass = s_resources.render_pass,
            .framebuffer = target_image->cached_video_framebuffer,
            .renderArea = {
                .offset = { 0, 0 },
                .extent = { target_image->info.extent.width, target_image->info.extent.height },
            },
            .clearValueCount = 0,
            .pClearValues = nullptr,
        };
        vkCmdBeginRenderPass(command_buffer_for_draw, &render_pass_begin, VK_SUBPASS_CONTENTS_INLINE);
        VkViewport viewport {
            .x = static_cast<float>(viewport_x),
            .y = flip_mesh_viewport_y ? static_cast<float>(viewport_y + viewport_height) : static_cast<float>(viewport_y),
            .width = static_cast<float>(viewport_width),
            .height = flip_mesh_viewport_y ? -static_cast<float>(viewport_height) : static_cast<float>(viewport_height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };
        VkRect2D scissor {
            .offset = { viewport_x, viewport_y },
            .extent = { static_cast<u32>(viewport_width), static_cast<u32>(viewport_height) },
        };
        vkCmdSetViewport(command_buffer_for_draw, 0, 1, &viewport);
        vkCmdSetScissor(command_buffer_for_draw, 0, 1, &scissor);
        vkCmdBindPipeline(command_buffer_for_draw, VK_PIPELINE_BIND_POINT_GRAPHICS, s_resources.pipeline);
        vkCmdBindDescriptorSets(command_buffer_for_draw, VK_PIPELINE_BIND_POINT_GRAPHICS, s_resources.pipeline_layout, 0, 1, &descriptor_set_for_draw, 0, nullptr);
        MeshPushConstants push_constants {
            .model_view_matrix = uniform_snapshot.model_view_matrix,
            .projection_matrix = uniform_snapshot.projection_matrix,
            .use_matrices = uniform_snapshot.has_model_view_matrix && uniform_snapshot.has_projection_matrix ? 1.0f : 0.0f,
            .opacity = uniform_snapshot.opacity,
            .output_intensity = uniform_snapshot.output_intensity,
            .stereo_eye = uniform_snapshot.stereo_eye,
            .stereo_eye_left = uniform_snapshot.stereo_eye_left,
        };
        vkCmdPushConstants(command_buffer_for_draw, s_resources.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push_constants), &push_constants);
        if (!replay_buffers.uv_right_buffer)
            return log_failure("missing_uv_right_buffer_for_stereo_mesh_draw"sv);
        VkBuffer vertex_buffers[] { replay_buffers.position_buffer->buffer, replay_buffers.uv_buffer->buffer, replay_buffers.uv_right_buffer->buffer };
        VkDeviceSize vertex_offsets[] { 0, 0, 0 };
        vkCmdBindVertexBuffers(command_buffer_for_draw, 0, 3, vertex_buffers, vertex_offsets);
        vkCmdBindIndexBuffer(command_buffer_for_draw, replay_buffers.index_buffer->buffer, draw_offset, index_type);
        vkCmdDrawIndexed(command_buffer_for_draw, draw_count, 1, 0, 0, 0);
        vkCmdEndRenderPass(command_buffer_for_draw);

        VkImageMemoryBarrier post_render_barriers[] {
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                .newLayout = static_cast<VkImageLayout>(source_layout),
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = source_image,
                .subresourceRange = { VK_IMAGE_ASPECT_PLANE_0_BIT | VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 1, 0, 1 },
            },
            {
                .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
                .pNext = nullptr,
                .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
                .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
                .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .newLayout = target_image->info.layout,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = target_image->image,
                .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
            },
        };
        vkCmdPipelineBarrier(command_buffer_for_draw,
            VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            0,
            0, nullptr,
            0, nullptr,
            2, post_render_barriers);

        result = vkEndCommandBuffer(command_buffer_for_draw);
        if (result != VK_SUCCESS)
            return log_failure("end_mesh_draw_command_buffer_failed"sv, result);

        VkSubmitInfo submit_info {
            .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
            .pNext = nullptr,
            .waitSemaphoreCount = 0,
            .pWaitSemaphores = nullptr,
            .pWaitDstStageMask = nullptr,
            .commandBufferCount = 1,
            .pCommandBuffers = &command_buffer_for_draw,
            .signalSemaphoreCount = 0,
            .pSignalSemaphores = nullptr,
        };
        if (queue_sync_mode == "fence"sv) {
            VkFenceCreateInfo fence_info {
                .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
                .pNext = nullptr,
                .flags = 0,
            };
            result = vkCreateFence(context.logical_device, &fence_info, nullptr, &submit_fence);
            if (result != VK_SUCCESS)
                return log_failure("create_mesh_draw_fence_failed"sv, result);
        }

        auto submit_started_at = MonotonicTime::now();
        result = vkQueueSubmit(context.graphics_queue, 1, &submit_info, submit_fence);
        queue_submit_us = (MonotonicTime::now() - submit_started_at).to_microseconds();
        if (result != VK_SUCCESS) {
            if (submit_fence != VK_NULL_HANDLE)
                vkDestroyFence(context.logical_device, submit_fence, nullptr);
            return log_failure("submit_mesh_draw_command_buffer_failed"sv, result);
        }
        if (queue_sync_mode == "fence"sv) {
            auto wait_started_at = MonotonicTime::now();
            result = vkWaitForFences(context.logical_device, 1, &submit_fence, VK_TRUE, UINT64_MAX);
            queue_wait_us = (MonotonicTime::now() - wait_started_at).to_microseconds();
            vkDestroyFence(context.logical_device, submit_fence, nullptr);
            if (result != VK_SUCCESS)
                return log_failure("wait_mesh_draw_fence_failed"sv, result);
        } else if (queue_sync_mode == "idle"sv) {
            auto wait_started_at = MonotonicTime::now();
            result = vkQueueWaitIdle(context.graphics_queue);
            queue_wait_us = (MonotonicTime::now() - wait_started_at).to_microseconds();
            if (result != VK_SUCCESS)
                return log_failure("wait_mesh_draw_queue_idle_failed"sv, result);
        }
        draw_status = "executed"sv;
        draw_reason = queue_sync_mode == "none"sv || queue_sync_mode == "ring"sv ? "vkCmdDrawIndexed_submitted_without_immediate_cpu_wait"sv : "vkCmdDrawIndexed_submitted"sv;
        mesh_draw_executed = true;
    }

    if (should_log) {
        dbgln("MUNDO_WEBGL_VIDEO_VULKAN_MESH_PIPELINE_PROBE draw_count={} probe_count={} frame_id={} status=ok cache_status={} descriptor_status=updated target_status=ok draw_status={} draw_reason={} queue_sync_mode={} queue_ring_slot={} queue_submit_us={} queue_wait_us={} destination_format={} source_image_view={} sampler={} pipeline={} descriptor_set={} target_image={} target_image_view={} target_framebuffer={} target_size={}x{} draw_index_count={} draw_index_type={} draw_index_offset={} viewport={}x{}+{}+{} viewport_flip_y={} vertex_bindings=3 vertex_attributes=3 matrix_push_constants={} stereo_eye_left={} next_step={}",
            log_count,
            probe_count,
            frame_id,
            pipeline_cache_status,
            draw_status,
            draw_reason,
            mesh_draw_enabled ? queue_sync_mode : "not_submitted"sv,
            queue_ring_slot == NumericLimits<size_t>::max() ? -1 : static_cast<int>(queue_ring_slot),
            queue_submit_us,
            queue_wait_us,
            destination_format,
            reinterpret_cast<uintptr_t>(source_image_view),
            reinterpret_cast<uintptr_t>(immutable_sampler),
            reinterpret_cast<uintptr_t>(s_resources.pipeline),
            reinterpret_cast<uintptr_t>(descriptor_set_for_draw),
            reinterpret_cast<uintptr_t>(target_image->image),
            reinterpret_cast<uintptr_t>(target_image->cached_video_color_attachment_view),
            reinterpret_cast<uintptr_t>(target_image->cached_video_framebuffer),
            target_image->info.extent.width,
            target_image->info.extent.height,
            draw_count,
            draw_type,
            draw_offset,
            viewport_width,
            viewport_height,
            viewport_x,
            viewport_y,
            flip_mesh_viewport_y,
            uniform_snapshot.has_model_view_matrix && uniform_snapshot.has_projection_matrix,
            uniform_snapshot.stereo_eye_left,
            mesh_draw_enabled ? "verify_direct_vulkan_mesh_visual_output" : "enable_MUNDO_WEBGL_VIDEO_VULKAN_MESH_DRAW_for_real_draw");
    }
    return VulkanVideoMeshPipelineProbeResult {
        .attempted = true,
        .supported = true,
        .executed = mesh_draw_executed,
        .reason = "ok"sv,
    };
}

OpenGLContext::VulkanVideoMeshPipelineProbeResult OpenGLContext::probe_vulkan_solid_mesh_pipeline(u32 destination_format, VulkanSolidMeshUniformSnapshot const& uniform_snapshot, ReadonlyBytes position_data, ReadonlyBytes index_data, u32 draw_count, u32 draw_type, u64 draw_offset, int viewport_x, int viewport_y, int viewport_width, int viewport_height, size_t log_count, Gfx::VulkanImage* target_image_override)
{
    struct SolidPushConstants {
        Array<float, 16> model_view_matrix {};
        Array<float, 16> projection_matrix {};
        Array<float, 4> diffuse { 1.0f, 1.0f, 1.0f, 1.0f };
        float use_matrices { 0.0f };
        float opacity { 1.0f };
        float output_intensity { 1.0f };
        float _pad0 { 0.0f };
    };
    constexpr size_t solid_ring_slot_count = 3;
    struct SolidPipelineResources {
        VkDevice device { VK_NULL_HANDLE };
        VkFormat destination_format { VK_FORMAT_UNDEFINED };
        VkShaderModule vertex_shader { VK_NULL_HANDLE };
        VkShaderModule fragment_shader { VK_NULL_HANDLE };
        VkRenderPass render_pass { VK_NULL_HANDLE };
        VkPipelineLayout pipeline_layout { VK_NULL_HANDLE };
        VkPipeline pipeline { VK_NULL_HANDLE };
        VkCommandPool ring_command_pool { VK_NULL_HANDLE };
        Array<VkCommandBuffer, solid_ring_slot_count> ring_command_buffers {};
        Array<VkFence, solid_ring_slot_count> ring_fences {};
        size_t ring_cursor { 0 };
    };
    static SolidPipelineResources s_resources;
    static size_t s_probe_count { 0 };
    auto probe_count = ++s_probe_count;
    auto should_log = probe_count <= 12 || probe_count % 120 == 0;
    auto const& context = m_skia_backend_context->vulkan_context();
    auto format = static_cast<VkFormat>(destination_format);
    VkResult result { VK_SUCCESS };

    auto log_failure = [&](StringView reason, VkResult result = VK_SUCCESS) {
        if (should_log) {
            dbgln("MUNDO_WEBGL_SOLID_MESH_PIPELINE_PROBE count={} probe_count={} status=failed reason={} vk_result={} destination_format={} draw_count={} draw_type={} draw_offset={} viewport={}x{}+{}+{} next_step=fix_solid_vulkan_replay_before_replacing_post_video_gl_draw",
                log_count,
                probe_count,
                reason,
                to_underlying(result),
                destination_format,
                draw_count,
                draw_type,
                draw_offset,
                viewport_width,
                viewport_height,
                viewport_x,
                viewport_y);
        }
        return VulkanVideoMeshPipelineProbeResult {
            .attempted = true,
            .supported = false,
            .reason = reason,
        };
    };

    if (viewport_width <= 0 || viewport_height <= 0)
        return log_failure("invalid_viewport"sv);
    if (position_data.is_empty())
        return log_failure("missing_position_data"sv);
    if (index_data.is_empty())
        return log_failure("missing_index_data"sv);

    auto signature = pair_int_hash(Traits<ReadonlyBytes>::hash(position_data), Traits<ReadonlyBytes>::hash(index_data));
    signature = pair_int_hash(signature, pair_int_hash(u32_hash(position_data.size()), u32_hash(index_data.size())));
    auto cache_matches = [&] {
        if (!m_impl->cached_vulkan_solid_mesh_replay_buffers)
            return false;
        auto const& cached = *m_impl->cached_vulkan_solid_mesh_replay_buffers;
        return cached.signature == signature
            && cached.position_bytes == position_data.size()
            && cached.index_bytes == index_data.size();
    };
    auto buffer_cache_status = "hit"sv;
    if (!cache_matches()) {
        auto position_buffer_or_error = Gfx::create_host_visible_vulkan_buffer_from_bytes(context, position_data, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        if (position_buffer_or_error.is_error())
            return log_failure(position_buffer_or_error.error().string_literal());
        auto index_buffer_or_error = Gfx::create_host_visible_vulkan_buffer_from_bytes(context, index_data, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        if (index_buffer_or_error.is_error())
            return log_failure(index_buffer_or_error.error().string_literal());
        m_impl->cached_vulkan_solid_mesh_replay_buffers = make<Impl::CachedVulkanSolidMeshReplayBuffers>(
            signature,
            position_data.size(),
            index_data.size(),
            position_buffer_or_error.release_value(),
            index_buffer_or_error.release_value());
        buffer_cache_status = "filled"sv;
    }
    auto const& replay_buffers = *m_impl->cached_vulkan_solid_mesh_replay_buffers;

    auto pipeline_cache_status = "hit"sv;
    if (s_resources.pipeline != VK_NULL_HANDLE) {
        auto matches = s_resources.device == context.logical_device
            && s_resources.destination_format == format;
        if (!matches)
            return log_failure("multiple_solid_pipeline_configurations_not_supported_yet"sv);
    } else {
        pipeline_cache_status = "filled"sv;
        auto vertex_shader_or_error = create_mundo_vulkan_video_shader_module(context, s_mundo_solid_mesh_vertex_shader_spirv);
        if (vertex_shader_or_error.is_error())
            return log_failure(vertex_shader_or_error.error().string_literal());
        s_resources.vertex_shader = vertex_shader_or_error.release_value();

        auto fragment_shader_or_error = create_mundo_vulkan_video_shader_module(context, s_mundo_solid_mesh_fragment_shader_spirv);
        if (fragment_shader_or_error.is_error())
            return log_failure(fragment_shader_or_error.error().string_literal());
        s_resources.fragment_shader = fragment_shader_or_error.release_value();

        VkAttachmentDescription color_attachment {
            .flags = 0,
            .format = format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
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
        result = vkCreateRenderPass(context.logical_device, &render_pass_info, nullptr, &s_resources.render_pass);
        if (result != VK_SUCCESS)
            return log_failure("create_solid_render_pass_failed"sv, result);

        VkPushConstantRange push_constant_range {
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0,
            .size = sizeof(SolidPushConstants),
        };
        VkPipelineLayoutCreateInfo pipeline_layout_info {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .setLayoutCount = 0,
            .pSetLayouts = nullptr,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_constant_range,
        };
        result = vkCreatePipelineLayout(context.logical_device, &pipeline_layout_info, nullptr, &s_resources.pipeline_layout);
        if (result != VK_SUCCESS)
            return log_failure("create_solid_pipeline_layout_failed"sv, result);

        VkPipelineShaderStageCreateInfo shader_stages[] {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = nullptr, .flags = 0, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_resources.vertex_shader, .pName = "main", .pSpecializationInfo = nullptr },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = nullptr, .flags = 0, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_resources.fragment_shader, .pName = "main", .pSpecializationInfo = nullptr },
        };
        VkVertexInputBindingDescription vertex_binding { .binding = 0, .stride = sizeof(float) * 3, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX };
        VkVertexInputAttributeDescription vertex_attribute { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0 };
        VkPipelineVertexInputStateCreateInfo vertex_input_info {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &vertex_binding,
            .vertexAttributeDescriptionCount = 1,
            .pVertexAttributeDescriptions = &vertex_attribute,
        };
        VkPipelineInputAssemblyStateCreateInfo input_assembly {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .primitiveRestartEnable = VK_FALSE,
        };
        VkViewport viewport { .x = 0, .y = 0, .width = 1.0f, .height = 1.0f, .minDepth = 0.0f, .maxDepth = 1.0f };
        VkRect2D scissor { .offset = { 0, 0 }, .extent = { 1, 1 } };
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
            .blendEnable = VK_TRUE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
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
        VkDynamicState dynamic_states[] { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic_state {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .dynamicStateCount = 2,
            .pDynamicStates = dynamic_states,
        };
        VkGraphicsPipelineCreateInfo pipeline_info {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stageCount = 2,
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
        if (result != VK_SUCCESS)
            return log_failure("create_solid_graphics_pipeline_failed"sv, result);

        s_resources.device = context.logical_device;
        s_resources.destination_format = format;
    }

    if (s_resources.ring_command_pool == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo command_pool_info {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = context.graphics_queue_family,
        };
        result = vkCreateCommandPool(context.logical_device, &command_pool_info, nullptr, &s_resources.ring_command_pool);
        if (result != VK_SUCCESS)
            return log_failure("create_solid_ring_command_pool_failed"sv, result);

        VkCommandBufferAllocateInfo command_buffer_alloc_info {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext = nullptr,
            .commandPool = s_resources.ring_command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = solid_ring_slot_count,
        };
        result = vkAllocateCommandBuffers(context.logical_device, &command_buffer_alloc_info, s_resources.ring_command_buffers.data());
        if (result != VK_SUCCESS)
            return log_failure("allocate_solid_ring_command_buffers_failed"sv, result);

        VkFenceCreateInfo fence_info {
            .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_FENCE_CREATE_SIGNALED_BIT,
        };
        for (size_t i = 0; i < solid_ring_slot_count; ++i) {
            result = vkCreateFence(context.logical_device, &fence_info, nullptr, &s_resources.ring_fences[i]);
            if (result != VK_SUCCESS)
                return log_failure("create_solid_ring_fence_failed"sv, result);
        }
    }

    RefPtr<Gfx::VulkanImage> painting_surface_target_image;
    auto* target_image = target_image_override;
    if (!target_image && m_painting_surface) {
        painting_surface_target_image = m_painting_surface->vulkan_image();
        target_image = painting_surface_target_image.ptr();
    }
    if (!target_image)
        return log_failure(target_image_override ? "missing_vulkan_solid_target_override"sv : "missing_vulkan_painting_surface_target"sv);
    if (target_image->info.format != format)
        return log_failure(target_image_override ? "vulkan_solid_mesh_target_format_mismatch"sv : "vulkan_painting_surface_format_mismatch"sv);
    if (!(target_image->info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        return log_failure(target_image_override ? "vulkan_solid_mesh_target_not_color_attachment"sv : "vulkan_painting_surface_not_color_attachment"sv);

    if (target_image->cached_solid_mesh_color_attachment_view == VK_NULL_HANDLE) {
        VkImageViewCreateInfo target_image_view_info {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = target_image->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = target_image->info.format,
            .components = { .r = VK_COMPONENT_SWIZZLE_IDENTITY, .g = VK_COMPONENT_SWIZZLE_IDENTITY, .b = VK_COMPONENT_SWIZZLE_IDENTITY, .a = VK_COMPONENT_SWIZZLE_IDENTITY },
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        result = vkCreateImageView(context.logical_device, &target_image_view_info, nullptr, &target_image->cached_solid_mesh_color_attachment_view);
        if (result != VK_SUCCESS)
            return log_failure("create_solid_target_image_view_failed"sv, result);
    }
    if (target_image->cached_solid_mesh_framebuffer == VK_NULL_HANDLE
        || target_image->cached_solid_mesh_framebuffer_render_pass != s_resources.render_pass
        || target_image->cached_solid_mesh_framebuffer_width != target_image->info.extent.width
        || target_image->cached_solid_mesh_framebuffer_height != target_image->info.extent.height) {
        if (target_image->cached_solid_mesh_framebuffer != VK_NULL_HANDLE)
            vkDestroyFramebuffer(context.logical_device, target_image->cached_solid_mesh_framebuffer, nullptr);
        VkFramebufferCreateInfo framebuffer_info {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .renderPass = s_resources.render_pass,
            .attachmentCount = 1,
            .pAttachments = &target_image->cached_solid_mesh_color_attachment_view,
            .width = target_image->info.extent.width,
            .height = target_image->info.extent.height,
            .layers = 1,
        };
        result = vkCreateFramebuffer(context.logical_device, &framebuffer_info, nullptr, &target_image->cached_solid_mesh_framebuffer);
        if (result != VK_SUCCESS)
            return log_failure("create_solid_target_framebuffer_failed"sv, result);
        target_image->cached_solid_mesh_framebuffer_render_pass = s_resources.render_pass;
        target_image->cached_solid_mesh_framebuffer_width = target_image->info.extent.width;
        target_image->cached_solid_mesh_framebuffer_height = target_image->info.extent.height;
    }

    auto index_type = VK_INDEX_TYPE_UINT16;
    if (draw_type == GL_UNSIGNED_SHORT)
        index_type = VK_INDEX_TYPE_UINT16;
    else if (draw_type == GL_UNSIGNED_INT)
        index_type = VK_INDEX_TYPE_UINT32;
    else
        return log_failure("unsupported_index_type_for_solid_mesh_draw"sv);

    auto queue_ring_slot = s_resources.ring_cursor++ % solid_ring_slot_count;
    auto wait_started_at = MonotonicTime::now();
    result = vkWaitForFences(context.logical_device, 1, &s_resources.ring_fences[queue_ring_slot], VK_TRUE, UINT64_MAX);
    auto queue_wait_us = (MonotonicTime::now() - wait_started_at).to_microseconds();
    if (result != VK_SUCCESS)
        return log_failure("wait_solid_ring_fence_failed"sv, result);
    result = vkResetFences(context.logical_device, 1, &s_resources.ring_fences[queue_ring_slot]);
    if (result != VK_SUCCESS)
        return log_failure("reset_solid_ring_fence_failed"sv, result);
    auto command_buffer = s_resources.ring_command_buffers[queue_ring_slot];
    vkResetCommandBuffer(command_buffer, 0);
    VkCommandBufferBeginInfo begin_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    result = vkBeginCommandBuffer(command_buffer, &begin_info);
    if (result != VK_SUCCESS)
        return log_failure("begin_solid_draw_command_buffer_failed"sv, result);

    VkImageMemoryBarrier pre_render_barrier {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
        .oldLayout = target_image->info.layout,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = target_image->image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(command_buffer,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &pre_render_barrier);

    VkRenderPassBeginInfo render_pass_begin {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext = nullptr,
        .renderPass = s_resources.render_pass,
        .framebuffer = target_image->cached_solid_mesh_framebuffer,
        .renderArea = { .offset = { 0, 0 }, .extent = { target_image->info.extent.width, target_image->info.extent.height } },
        .clearValueCount = 0,
        .pClearValues = nullptr,
    };
    vkCmdBeginRenderPass(command_buffer, &render_pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport {
        .x = static_cast<float>(viewport_x),
        .y = static_cast<float>(viewport_y + viewport_height),
        .width = static_cast<float>(viewport_width),
        .height = -static_cast<float>(viewport_height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor {
        .offset = { viewport_x, viewport_y },
        .extent = { static_cast<u32>(viewport_width), static_cast<u32>(viewport_height) },
    };
    vkCmdSetViewport(command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, s_resources.pipeline);
    SolidPushConstants push_constants {
        .model_view_matrix = uniform_snapshot.model_view_matrix,
        .projection_matrix = uniform_snapshot.projection_matrix,
        .diffuse = uniform_snapshot.diffuse,
        .use_matrices = uniform_snapshot.has_model_view_matrix && uniform_snapshot.has_projection_matrix ? 1.0f : 0.0f,
        .opacity = uniform_snapshot.opacity,
        .output_intensity = uniform_snapshot.output_intensity,
        ._pad0 = 0.0f,
    };
    vkCmdPushConstants(command_buffer, s_resources.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push_constants), &push_constants);
    VkDeviceSize vertex_offset = 0;
    vkCmdBindVertexBuffers(command_buffer, 0, 1, &replay_buffers.position_buffer->buffer, &vertex_offset);
    vkCmdBindIndexBuffer(command_buffer, replay_buffers.index_buffer->buffer, draw_offset, index_type);
    vkCmdDrawIndexed(command_buffer, draw_count, 1, 0, 0, 0);
    vkCmdEndRenderPass(command_buffer);

    VkImageMemoryBarrier post_render_barrier {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = target_image->info.layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = target_image->image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(command_buffer,
        VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
        VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
        0,
        0, nullptr,
        0, nullptr,
        1, &post_render_barrier);
    result = vkEndCommandBuffer(command_buffer);
    if (result != VK_SUCCESS)
        return log_failure("end_solid_draw_command_buffer_failed"sv, result);

    VkSubmitInfo submit_info {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    auto submit_started_at = MonotonicTime::now();
    result = vkQueueSubmit(context.graphics_queue, 1, &submit_info, s_resources.ring_fences[queue_ring_slot]);
    auto queue_submit_us = (MonotonicTime::now() - submit_started_at).to_microseconds();
    if (result != VK_SUCCESS)
        return log_failure("submit_solid_draw_command_buffer_failed"sv, result);

    if (should_log) {
        dbgln("MUNDO_WEBGL_SOLID_MESH_PIPELINE_PROBE count={} probe_count={} status=ok pipeline_cache_status={} buffer_cache_status={} draw_status=executed queue_ring_slot={} queue_submit_us={} queue_wait_us={} destination_format={} target_image={} target_image_view={} target_framebuffer={} target_size={}x{} target_override={} draw_index_count={} draw_index_type={} draw_index_offset={} viewport={}x{}+{}+{} matrix_push_constants={} diffuse=({}, {}, {}, {}) opacity={} output_intensity={} next_step=replace_matching_post_video_gl_draw_with_solid_vulkan_mesh",
            log_count,
            probe_count,
            pipeline_cache_status,
            buffer_cache_status,
            queue_ring_slot,
            queue_submit_us,
            queue_wait_us,
            destination_format,
            reinterpret_cast<uintptr_t>(target_image->image),
            reinterpret_cast<uintptr_t>(target_image->cached_solid_mesh_color_attachment_view),
            reinterpret_cast<uintptr_t>(target_image->cached_solid_mesh_framebuffer),
            target_image->info.extent.width,
            target_image->info.extent.height,
            target_image_override != nullptr,
            draw_count,
            draw_type,
            draw_offset,
            viewport_width,
            viewport_height,
            viewport_x,
            viewport_y,
            uniform_snapshot.has_model_view_matrix && uniform_snapshot.has_projection_matrix,
            uniform_snapshot.diffuse[0],
            uniform_snapshot.diffuse[1],
            uniform_snapshot.diffuse[2],
            uniform_snapshot.diffuse[3],
            uniform_snapshot.opacity,
            uniform_snapshot.output_intensity);
    }
    return VulkanVideoMeshPipelineProbeResult {
        .attempted = true,
        .supported = true,
        .executed = true,
        .reason = "ok"sv,
    };
}

OpenGLContext::VulkanVideoMeshPipelineProbeResult OpenGLContext::probe_vulkan_colored_mesh_pipeline(u32 destination_format, VulkanSolidMeshUniformSnapshot const& uniform_snapshot, ReadonlyBytes position_data, ReadonlyBytes color_data, ReadonlyBytes index_data, u32 draw_count, u32 draw_type, u64 draw_offset, int viewport_x, int viewport_y, int viewport_width, int viewport_height, size_t log_count, Gfx::VulkanImage* target_image_override)
{
    struct ColoredPushConstants {
        Array<float, 16> model_view_matrix {};
        Array<float, 16> projection_matrix {};
        Array<float, 4> diffuse { 1.0f, 1.0f, 1.0f, 1.0f };
        float use_matrices { 0.0f };
        float opacity { 1.0f };
        float output_intensity { 1.0f };
        float _pad0 { 0.0f };
    };
    constexpr size_t colored_ring_slot_count = 3;
    struct ColoredPipelineResources {
        VkDevice device { VK_NULL_HANDLE };
        VkFormat destination_format { VK_FORMAT_UNDEFINED };
        VkShaderModule vertex_shader { VK_NULL_HANDLE };
        VkShaderModule fragment_shader { VK_NULL_HANDLE };
        VkRenderPass render_pass { VK_NULL_HANDLE };
        VkPipelineLayout pipeline_layout { VK_NULL_HANDLE };
        VkPipeline pipeline { VK_NULL_HANDLE };
        VkCommandPool ring_command_pool { VK_NULL_HANDLE };
        Array<VkCommandBuffer, colored_ring_slot_count> ring_command_buffers {};
        Array<VkFence, colored_ring_slot_count> ring_fences {};
        size_t ring_cursor { 0 };
    };
    static ColoredPipelineResources s_resources;
    static size_t s_probe_count { 0 };
    auto probe_count = ++s_probe_count;
    auto should_log = probe_count <= 12 || probe_count % 120 == 0;
    auto const& context = m_skia_backend_context->vulkan_context();
    auto format = static_cast<VkFormat>(destination_format);
    VkResult result { VK_SUCCESS };

    auto log_failure = [&](StringView reason, VkResult result = VK_SUCCESS) {
        if (should_log) {
            dbgln("MUNDO_WEBGL_COLORED_MESH_PIPELINE_PROBE count={} probe_count={} status=failed reason={} vk_result={} destination_format={} draw_count={} draw_type={} draw_offset={} viewport={}x{}+{}+{} next_step=fix_colored_vulkan_replay_before_replacing_post_video_gl_draw",
                log_count,
                probe_count,
                reason,
                to_underlying(result),
                destination_format,
                draw_count,
                draw_type,
                draw_offset,
                viewport_width,
                viewport_height,
                viewport_x,
                viewport_y);
        }
        return VulkanVideoMeshPipelineProbeResult {
            .attempted = true,
            .supported = false,
            .reason = reason,
        };
    };

    if (viewport_width <= 0 || viewport_height <= 0)
        return log_failure("invalid_viewport"sv);
    if (position_data.is_empty())
        return log_failure("missing_position_data"sv);
    if (color_data.is_empty())
        return log_failure("missing_color_data"sv);
    if (index_data.is_empty())
        return log_failure("missing_index_data"sv);

    auto signature = pair_int_hash(Traits<ReadonlyBytes>::hash(position_data), Traits<ReadonlyBytes>::hash(color_data));
    signature = pair_int_hash(signature, Traits<ReadonlyBytes>::hash(index_data));
    signature = pair_int_hash(signature, pair_int_hash(u32_hash(position_data.size()), pair_int_hash(u32_hash(color_data.size()), u32_hash(index_data.size()))));
    auto cache_matches = [&] {
        if (!m_impl->cached_vulkan_colored_mesh_replay_buffers)
            return false;
        auto const& cached = *m_impl->cached_vulkan_colored_mesh_replay_buffers;
        return cached.signature == signature
            && cached.position_bytes == position_data.size()
            && cached.color_bytes == color_data.size()
            && cached.index_bytes == index_data.size();
    };
    auto buffer_cache_status = "hit"sv;
    if (!cache_matches()) {
        auto position_buffer_or_error = Gfx::create_host_visible_vulkan_buffer_from_bytes(context, position_data, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        if (position_buffer_or_error.is_error())
            return log_failure(position_buffer_or_error.error().string_literal());
        auto color_buffer_or_error = Gfx::create_host_visible_vulkan_buffer_from_bytes(context, color_data, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        if (color_buffer_or_error.is_error())
            return log_failure(color_buffer_or_error.error().string_literal());
        auto index_buffer_or_error = Gfx::create_host_visible_vulkan_buffer_from_bytes(context, index_data, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        if (index_buffer_or_error.is_error())
            return log_failure(index_buffer_or_error.error().string_literal());
        m_impl->cached_vulkan_colored_mesh_replay_buffers = make<Impl::CachedVulkanColoredMeshReplayBuffers>(
            signature,
            position_data.size(),
            color_data.size(),
            index_data.size(),
            position_buffer_or_error.release_value(),
            color_buffer_or_error.release_value(),
            index_buffer_or_error.release_value());
        buffer_cache_status = "filled"sv;
    }
    auto const& replay_buffers = *m_impl->cached_vulkan_colored_mesh_replay_buffers;

    auto pipeline_cache_status = "hit"sv;
    if (s_resources.pipeline != VK_NULL_HANDLE) {
        auto matches = s_resources.device == context.logical_device
            && s_resources.destination_format == format;
        if (!matches)
            return log_failure("multiple_colored_pipeline_configurations_not_supported_yet"sv);
    } else {
        pipeline_cache_status = "filled"sv;
        auto vertex_shader_or_error = create_mundo_vulkan_video_shader_module(context, s_mundo_colored_mesh_vertex_shader_spirv);
        if (vertex_shader_or_error.is_error())
            return log_failure(vertex_shader_or_error.error().string_literal());
        s_resources.vertex_shader = vertex_shader_or_error.release_value();

        auto fragment_shader_or_error = create_mundo_vulkan_video_shader_module(context, s_mundo_colored_mesh_fragment_shader_spirv);
        if (fragment_shader_or_error.is_error())
            return log_failure(fragment_shader_or_error.error().string_literal());
        s_resources.fragment_shader = fragment_shader_or_error.release_value();

        VkAttachmentDescription color_attachment {
            .flags = 0,
            .format = format,
            .samples = VK_SAMPLE_COUNT_1_BIT,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
            .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
            .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
            .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
            .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        };
        VkAttachmentReference color_attachment_ref { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
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
        result = vkCreateRenderPass(context.logical_device, &render_pass_info, nullptr, &s_resources.render_pass);
        if (result != VK_SUCCESS)
            return log_failure("create_colored_render_pass_failed"sv, result);

        VkPushConstantRange push_constant_range {
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0,
            .size = sizeof(ColoredPushConstants),
        };
        VkPipelineLayoutCreateInfo pipeline_layout_info {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .setLayoutCount = 0,
            .pSetLayouts = nullptr,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_constant_range,
        };
        result = vkCreatePipelineLayout(context.logical_device, &pipeline_layout_info, nullptr, &s_resources.pipeline_layout);
        if (result != VK_SUCCESS)
            return log_failure("create_colored_pipeline_layout_failed"sv, result);

        VkPipelineShaderStageCreateInfo shader_stages[] {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = nullptr, .flags = 0, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_resources.vertex_shader, .pName = "main", .pSpecializationInfo = nullptr },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = nullptr, .flags = 0, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_resources.fragment_shader, .pName = "main", .pSpecializationInfo = nullptr },
        };
        Array<VkVertexInputBindingDescription, 2> vertex_bindings {
            VkVertexInputBindingDescription { .binding = 0, .stride = sizeof(float) * 3, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
            VkVertexInputBindingDescription { .binding = 1, .stride = sizeof(float) * 3, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
        };
        Array<VkVertexInputAttributeDescription, 2> vertex_attributes {
            VkVertexInputAttributeDescription { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0 },
            VkVertexInputAttributeDescription { .location = 1, .binding = 1, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0 },
        };
        VkPipelineVertexInputStateCreateInfo vertex_input_info {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .vertexBindingDescriptionCount = vertex_bindings.size(),
            .pVertexBindingDescriptions = vertex_bindings.data(),
            .vertexAttributeDescriptionCount = vertex_attributes.size(),
            .pVertexAttributeDescriptions = vertex_attributes.data(),
        };
        VkPipelineInputAssemblyStateCreateInfo input_assembly {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .primitiveRestartEnable = VK_FALSE,
        };
        VkViewport viewport { .x = 0, .y = 0, .width = 1.0f, .height = 1.0f, .minDepth = 0.0f, .maxDepth = 1.0f };
        VkRect2D scissor { .offset = { 0, 0 }, .extent = { 1, 1 } };
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
            .blendEnable = VK_TRUE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
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
        VkDynamicState dynamic_states[] { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic_state {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .dynamicStateCount = 2,
            .pDynamicStates = dynamic_states,
        };
        VkGraphicsPipelineCreateInfo pipeline_info {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stageCount = 2,
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
        if (result != VK_SUCCESS)
            return log_failure("create_colored_graphics_pipeline_failed"sv, result);

        s_resources.device = context.logical_device;
        s_resources.destination_format = format;
    }

    if (s_resources.ring_command_pool == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo command_pool_info {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = context.graphics_queue_family,
        };
        result = vkCreateCommandPool(context.logical_device, &command_pool_info, nullptr, &s_resources.ring_command_pool);
        if (result != VK_SUCCESS)
            return log_failure("create_colored_ring_command_pool_failed"sv, result);

        VkCommandBufferAllocateInfo command_buffer_alloc_info {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext = nullptr,
            .commandPool = s_resources.ring_command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = colored_ring_slot_count,
        };
        result = vkAllocateCommandBuffers(context.logical_device, &command_buffer_alloc_info, s_resources.ring_command_buffers.data());
        if (result != VK_SUCCESS)
            return log_failure("allocate_colored_ring_command_buffers_failed"sv, result);

        VkFenceCreateInfo fence_info { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        for (size_t i = 0; i < colored_ring_slot_count; ++i) {
            result = vkCreateFence(context.logical_device, &fence_info, nullptr, &s_resources.ring_fences[i]);
            if (result != VK_SUCCESS)
                return log_failure("create_colored_ring_fence_failed"sv, result);
        }
    }

    RefPtr<Gfx::VulkanImage> target_image;
    if (target_image_override)
        target_image = *target_image_override;
    else if (m_painting_surface)
        target_image = m_painting_surface->vulkan_image();
    if (!target_image)
        return log_failure("missing_vulkan_colored_mesh_target"sv);
    if (target_image->info.format != format)
        return log_failure("vulkan_colored_mesh_target_format_mismatch"sv);
    if (!(target_image->info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        return log_failure("vulkan_colored_mesh_target_not_color_attachment"sv);

    if (target_image->cached_solid_mesh_color_attachment_view == VK_NULL_HANDLE) {
        VkImageViewCreateInfo target_image_view_info {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = target_image->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = target_image->info.format,
            .components = { .r = VK_COMPONENT_SWIZZLE_IDENTITY, .g = VK_COMPONENT_SWIZZLE_IDENTITY, .b = VK_COMPONENT_SWIZZLE_IDENTITY, .a = VK_COMPONENT_SWIZZLE_IDENTITY },
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        result = vkCreateImageView(context.logical_device, &target_image_view_info, nullptr, &target_image->cached_solid_mesh_color_attachment_view);
        if (result != VK_SUCCESS)
            return log_failure("create_colored_target_image_view_failed"sv, result);
    }
    if (target_image->cached_solid_mesh_framebuffer == VK_NULL_HANDLE
        || target_image->cached_solid_mesh_framebuffer_render_pass != s_resources.render_pass
        || target_image->cached_solid_mesh_framebuffer_width != target_image->info.extent.width
        || target_image->cached_solid_mesh_framebuffer_height != target_image->info.extent.height) {
        if (target_image->cached_solid_mesh_framebuffer != VK_NULL_HANDLE)
            vkDestroyFramebuffer(context.logical_device, target_image->cached_solid_mesh_framebuffer, nullptr);
        VkFramebufferCreateInfo framebuffer_info {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .renderPass = s_resources.render_pass,
            .attachmentCount = 1,
            .pAttachments = &target_image->cached_solid_mesh_color_attachment_view,
            .width = target_image->info.extent.width,
            .height = target_image->info.extent.height,
            .layers = 1,
        };
        result = vkCreateFramebuffer(context.logical_device, &framebuffer_info, nullptr, &target_image->cached_solid_mesh_framebuffer);
        if (result != VK_SUCCESS)
            return log_failure("create_colored_target_framebuffer_failed"sv, result);
        target_image->cached_solid_mesh_framebuffer_render_pass = s_resources.render_pass;
        target_image->cached_solid_mesh_framebuffer_width = target_image->info.extent.width;
        target_image->cached_solid_mesh_framebuffer_height = target_image->info.extent.height;
    }

    auto index_type = VK_INDEX_TYPE_UINT16;
    if (draw_type == GL_UNSIGNED_SHORT)
        index_type = VK_INDEX_TYPE_UINT16;
    else if (draw_type == GL_UNSIGNED_INT)
        index_type = VK_INDEX_TYPE_UINT32;
    else
        return log_failure("unsupported_index_type_for_colored_mesh_draw"sv);

    auto queue_ring_slot = s_resources.ring_cursor++ % colored_ring_slot_count;
    auto wait_started_at = MonotonicTime::now();
    result = vkWaitForFences(context.logical_device, 1, &s_resources.ring_fences[queue_ring_slot], VK_TRUE, UINT64_MAX);
    auto queue_wait_us = (MonotonicTime::now() - wait_started_at).to_microseconds();
    if (result != VK_SUCCESS)
        return log_failure("wait_colored_ring_fence_failed"sv, result);
    result = vkResetFences(context.logical_device, 1, &s_resources.ring_fences[queue_ring_slot]);
    if (result != VK_SUCCESS)
        return log_failure("reset_colored_ring_fence_failed"sv, result);
    auto command_buffer = s_resources.ring_command_buffers[queue_ring_slot];
    vkResetCommandBuffer(command_buffer, 0);
    VkCommandBufferBeginInfo begin_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    result = vkBeginCommandBuffer(command_buffer, &begin_info);
    if (result != VK_SUCCESS)
        return log_failure("begin_colored_draw_command_buffer_failed"sv, result);

    VkImageMemoryBarrier pre_render_barrier {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
        .oldLayout = target_image->info.layout,
        .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = target_image->image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &pre_render_barrier);

    VkRenderPassBeginInfo render_pass_begin {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext = nullptr,
        .renderPass = s_resources.render_pass,
        .framebuffer = target_image->cached_solid_mesh_framebuffer,
        .renderArea = { .offset = { 0, 0 }, .extent = { target_image->info.extent.width, target_image->info.extent.height } },
        .clearValueCount = 0,
        .pClearValues = nullptr,
    };
    vkCmdBeginRenderPass(command_buffer, &render_pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport {
        .x = static_cast<float>(viewport_x),
        .y = static_cast<float>(viewport_y + viewport_height),
        .width = static_cast<float>(viewport_width),
        .height = -static_cast<float>(viewport_height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor {
        .offset = { viewport_x, viewport_y },
        .extent = { static_cast<u32>(viewport_width), static_cast<u32>(viewport_height) },
    };
    vkCmdSetViewport(command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, s_resources.pipeline);
    ColoredPushConstants push_constants {
        .model_view_matrix = uniform_snapshot.model_view_matrix,
        .projection_matrix = uniform_snapshot.projection_matrix,
        .diffuse = uniform_snapshot.diffuse,
        .use_matrices = uniform_snapshot.has_model_view_matrix && uniform_snapshot.has_projection_matrix ? 1.0f : 0.0f,
        .opacity = uniform_snapshot.opacity,
        .output_intensity = uniform_snapshot.output_intensity,
        ._pad0 = 0.0f,
    };
    vkCmdPushConstants(command_buffer, s_resources.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push_constants), &push_constants);
    VkBuffer vertex_buffers[] { replay_buffers.position_buffer->buffer, replay_buffers.color_buffer->buffer };
    VkDeviceSize vertex_offsets[] { 0, 0 };
    vkCmdBindVertexBuffers(command_buffer, 0, 2, vertex_buffers, vertex_offsets);
    vkCmdBindIndexBuffer(command_buffer, replay_buffers.index_buffer->buffer, draw_offset, index_type);
    vkCmdDrawIndexed(command_buffer, draw_count, 1, 0, 0, 0);
    vkCmdEndRenderPass(command_buffer);

    VkImageMemoryBarrier post_render_barrier {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
        .newLayout = target_image->info.layout,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = target_image->image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &post_render_barrier);
    result = vkEndCommandBuffer(command_buffer);
    if (result != VK_SUCCESS)
        return log_failure("end_colored_draw_command_buffer_failed"sv, result);

    VkSubmitInfo submit_info {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    auto submit_started_at = MonotonicTime::now();
    result = vkQueueSubmit(context.graphics_queue, 1, &submit_info, s_resources.ring_fences[queue_ring_slot]);
    auto queue_submit_us = (MonotonicTime::now() - submit_started_at).to_microseconds();
    if (result != VK_SUCCESS)
        return log_failure("submit_colored_draw_command_buffer_failed"sv, result);

    if (should_log) {
        dbgln("MUNDO_WEBGL_COLORED_MESH_PIPELINE_PROBE count={} probe_count={} status=ok pipeline_cache_status={} buffer_cache_status={} draw_status=executed queue_ring_slot={} queue_submit_us={} queue_wait_us={} destination_format={} target_image={} target_image_view={} target_framebuffer={} target_size={}x{} target_override={} draw_index_count={} draw_index_type={} draw_index_offset={} viewport={}x{}+{}+{} matrix_push_constants={} diffuse=({}, {}, {}, {}) opacity={} output_intensity={} vertex_bindings=2 vertex_attributes=2 next_step=replace_colored_render_target_producer_with_vulkan_mesh",
            log_count,
            probe_count,
            pipeline_cache_status,
            buffer_cache_status,
            queue_ring_slot,
            queue_submit_us,
            queue_wait_us,
            destination_format,
            reinterpret_cast<uintptr_t>(target_image->image),
            reinterpret_cast<uintptr_t>(target_image->cached_solid_mesh_color_attachment_view),
            reinterpret_cast<uintptr_t>(target_image->cached_solid_mesh_framebuffer),
            target_image->info.extent.width,
            target_image->info.extent.height,
            target_image_override != nullptr,
            draw_count,
            draw_type,
            draw_offset,
            viewport_width,
            viewport_height,
            viewport_x,
            viewport_y,
            uniform_snapshot.has_model_view_matrix && uniform_snapshot.has_projection_matrix,
            uniform_snapshot.diffuse[0],
            uniform_snapshot.diffuse[1],
            uniform_snapshot.diffuse[2],
            uniform_snapshot.diffuse[3],
            uniform_snapshot.opacity,
            uniform_snapshot.output_intensity);
    }
    return VulkanVideoMeshPipelineProbeResult {
        .attempted = true,
        .supported = true,
        .executed = true,
        .reason = "ok"sv,
    };
}

OpenGLContext::VulkanVideoMeshPipelineProbeResult OpenGLContext::probe_vulkan_textured_mesh_pipeline(u32 destination_format, Gfx::VulkanImage& source_image, VulkanSolidMeshUniformSnapshot const& uniform_snapshot, ReadonlyBytes position_data, ReadonlyBytes uv_data, ReadonlyBytes index_data, u32 draw_count, u32 draw_type, u64 draw_offset, int viewport_x, int viewport_y, int viewport_width, int viewport_height, size_t log_count, Gfx::VulkanImage* alpha_image)
{
    struct TexturedPushConstants {
        Array<float, 16> model_view_matrix {};
        Array<float, 16> projection_matrix {};
        Array<float, 4> diffuse { 1.0f, 1.0f, 1.0f, 1.0f };
        float use_matrices { 0.0f };
        float opacity { 1.0f };
        float output_intensity { 1.0f };
        float flip_y { 0.0f };
    };
    constexpr size_t textured_ring_slot_count = 3;
    struct TexturedPipelineResources {
        VkDevice device { VK_NULL_HANDLE };
        VkFormat destination_format { VK_FORMAT_UNDEFINED };
        VkShaderModule vertex_shader { VK_NULL_HANDLE };
        VkShaderModule fragment_shader { VK_NULL_HANDLE };
        VkRenderPass render_pass { VK_NULL_HANDLE };
        VkDescriptorSetLayout descriptor_set_layout { VK_NULL_HANDLE };
        VkPipelineLayout pipeline_layout { VK_NULL_HANDLE };
        VkPipeline pipeline { VK_NULL_HANDLE };
        VkDescriptorPool descriptor_pool { VK_NULL_HANDLE };
        VkDescriptorSet descriptor_set { VK_NULL_HANDLE };
        VkSampler sampler { VK_NULL_HANDLE };
        VkImageView last_source_image_view { VK_NULL_HANDLE };
        VkImageView last_alpha_image_view { VK_NULL_HANDLE };
        VkImage target_framebuffer_image { VK_NULL_HANDLE };
        VkFramebuffer target_framebuffer { VK_NULL_HANDLE };
        VkRenderPass target_framebuffer_render_pass { VK_NULL_HANDLE };
        u32 target_framebuffer_width { 0 };
        u32 target_framebuffer_height { 0 };
        bool uses_alpha { false };
        VkCommandPool ring_command_pool { VK_NULL_HANDLE };
        Array<VkCommandBuffer, textured_ring_slot_count> ring_command_buffers {};
        Array<VkFence, textured_ring_slot_count> ring_fences {};
        size_t ring_cursor { 0 };
    };
    static TexturedPipelineResources s_single_sampler_resources;
    static TexturedPipelineResources s_alpha_sampler_resources;
    static VkDevice s_shared_textured_render_pass_device { VK_NULL_HANDLE };
    static VkFormat s_shared_textured_render_pass_format { VK_FORMAT_UNDEFINED };
    static VkRenderPass s_shared_textured_render_pass { VK_NULL_HANDLE };
    auto uses_alpha = alpha_image != nullptr;
    auto& s_resources = uses_alpha ? s_alpha_sampler_resources : s_single_sampler_resources;
    static size_t s_probe_count { 0 };
    auto probe_count = ++s_probe_count;
    auto should_log = probe_count <= 12 || probe_count % 120 == 0;
    auto const& context = m_skia_backend_context->vulkan_context();
    auto format = static_cast<VkFormat>(destination_format);
    VkResult result { VK_SUCCESS };

    auto log_failure = [&](StringView reason, VkResult result = VK_SUCCESS) {
        if (should_log) {
            dbgln("MUNDO_WEBGL_TEXTURED_MESH_PIPELINE_PROBE count={} probe_count={} status=failed reason={} vk_result={} destination_format={} source_image={} source_size={}x{} source_usage={} draw_count={} draw_type={} draw_offset={} viewport={}x{}+{}+{} next_step=fix_textured_render_target_consumer_before_enabling_full_gpu_chain",
                log_count,
                probe_count,
                reason,
                to_underlying(result),
                destination_format,
                reinterpret_cast<uintptr_t>(source_image.image),
                source_image.info.extent.width,
                source_image.info.extent.height,
                source_image.info.usage,
                draw_count,
                draw_type,
                draw_offset,
                viewport_width,
                viewport_height,
                viewport_x,
                viewport_y);
        }
        return VulkanVideoMeshPipelineProbeResult {
            .attempted = true,
            .supported = false,
            .reason = reason,
        };
    };

    if (viewport_width <= 0 || viewport_height <= 0)
        return log_failure("invalid_viewport"sv);
    if (position_data.is_empty())
        return log_failure("missing_position_data"sv);
    if (uv_data.is_empty())
        return log_failure("missing_uv_data"sv);
    if (index_data.is_empty())
        return log_failure("missing_index_data"sv);
    if (!(source_image.info.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
        return log_failure("source_image_not_sampled_usage"sv);
    if (alpha_image && !(alpha_image->info.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
        return log_failure("alpha_image_not_sampled_usage"sv);

    RefPtr<Gfx::VulkanImage> target_image;
    if (m_painting_surface)
        target_image = m_painting_surface->vulkan_image();
    if (!target_image)
        return log_failure("missing_vulkan_textured_target"sv);
    if (target_image.ptr() == &source_image)
        return log_failure("source_and_target_are_same_image"sv);
    if (target_image->info.format != format)
        return log_failure("vulkan_textured_mesh_target_format_mismatch"sv);
    if (!(target_image->info.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
        return log_failure("vulkan_textured_mesh_target_not_color_attachment"sv);

    auto signature = pair_int_hash(Traits<ReadonlyBytes>::hash(position_data), Traits<ReadonlyBytes>::hash(uv_data));
    signature = pair_int_hash(signature, Traits<ReadonlyBytes>::hash(index_data));
    signature = pair_int_hash(signature, pair_int_hash(u32_hash(position_data.size()), pair_int_hash(u32_hash(uv_data.size()), u32_hash(index_data.size()))));
    auto cache_matches = [&] {
        if (!m_impl->cached_vulkan_textured_mesh_replay_buffers)
            return false;
        auto const& cached = *m_impl->cached_vulkan_textured_mesh_replay_buffers;
        return cached.signature == signature
            && cached.position_bytes == position_data.size()
            && cached.uv_bytes == uv_data.size()
            && cached.index_bytes == index_data.size();
    };
    auto buffer_cache_status = "hit"sv;
    if (!cache_matches()) {
        auto position_buffer_or_error = Gfx::create_host_visible_vulkan_buffer_from_bytes(context, position_data, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        if (position_buffer_or_error.is_error())
            return log_failure(position_buffer_or_error.error().string_literal());
        auto uv_buffer_or_error = Gfx::create_host_visible_vulkan_buffer_from_bytes(context, uv_data, VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
        if (uv_buffer_or_error.is_error())
            return log_failure(uv_buffer_or_error.error().string_literal());
        auto index_buffer_or_error = Gfx::create_host_visible_vulkan_buffer_from_bytes(context, index_data, VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
        if (index_buffer_or_error.is_error())
            return log_failure(index_buffer_or_error.error().string_literal());
        m_impl->cached_vulkan_textured_mesh_replay_buffers = make<Impl::CachedVulkanTexturedMeshReplayBuffers>(
            signature,
            position_data.size(),
            uv_data.size(),
            index_data.size(),
            position_buffer_or_error.release_value(),
            uv_buffer_or_error.release_value(),
            index_buffer_or_error.release_value());
        buffer_cache_status = "filled"sv;
    }
    auto const& replay_buffers = *m_impl->cached_vulkan_textured_mesh_replay_buffers;

    auto pipeline_cache_status = "hit"sv;
    if (s_resources.pipeline != VK_NULL_HANDLE) {
        auto matches = s_resources.device == context.logical_device
            && s_resources.destination_format == format;
        if (!matches)
            return log_failure("multiple_textured_pipeline_configurations_not_supported_yet"sv);
    } else {
        pipeline_cache_status = "filled"sv;
        auto vertex_shader_or_error = create_mundo_vulkan_video_shader_module(context, s_mundo_textured_mesh_vertex_shader_spirv);
        if (vertex_shader_or_error.is_error())
            return log_failure(vertex_shader_or_error.error().string_literal());
        s_resources.vertex_shader = vertex_shader_or_error.release_value();

        auto fragment_shader_or_error = uses_alpha
            ? create_mundo_vulkan_video_shader_module(context, s_mundo_textured_alpha_mesh_fragment_shader_spirv)
            : create_mundo_vulkan_video_shader_module(context, s_mundo_textured_mesh_fragment_shader_spirv);
        if (fragment_shader_or_error.is_error())
            return log_failure(fragment_shader_or_error.error().string_literal());
        s_resources.fragment_shader = fragment_shader_or_error.release_value();

        Array<VkDescriptorSetLayoutBinding, 2> sampler_bindings {
            VkDescriptorSetLayoutBinding {
                .binding = 0,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                .pImmutableSamplers = nullptr,
            },
            VkDescriptorSetLayoutBinding {
                .binding = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .descriptorCount = 1,
                .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                .pImmutableSamplers = nullptr,
            },
        };
        VkDescriptorSetLayoutCreateInfo descriptor_set_layout_info {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .bindingCount = uses_alpha ? 2u : 1u,
            .pBindings = sampler_bindings.data(),
        };
        result = vkCreateDescriptorSetLayout(context.logical_device, &descriptor_set_layout_info, nullptr, &s_resources.descriptor_set_layout);
        if (result != VK_SUCCESS)
            return log_failure("create_textured_descriptor_set_layout_failed"sv, result);

        if (s_shared_textured_render_pass != VK_NULL_HANDLE) {
            if (s_shared_textured_render_pass_device != context.logical_device || s_shared_textured_render_pass_format != format)
                return log_failure("multiple_textured_render_pass_configurations_not_supported_yet"sv);
            s_resources.render_pass = s_shared_textured_render_pass;
        } else {
            VkAttachmentDescription color_attachment {
                .flags = 0,
                .format = format,
                .samples = VK_SAMPLE_COUNT_1_BIT,
                .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD,
                .storeOp = VK_ATTACHMENT_STORE_OP_STORE,
                .stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE,
                .stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE,
                .initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                .finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            };
            VkAttachmentReference color_attachment_ref { .attachment = 0, .layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL };
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
            result = vkCreateRenderPass(context.logical_device, &render_pass_info, nullptr, &s_shared_textured_render_pass);
            if (result != VK_SUCCESS)
                return log_failure("create_textured_render_pass_failed"sv, result);
            s_shared_textured_render_pass_device = context.logical_device;
            s_shared_textured_render_pass_format = format;
            s_resources.render_pass = s_shared_textured_render_pass;
        }

        VkPushConstantRange push_constant_range {
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            .offset = 0,
            .size = sizeof(TexturedPushConstants),
        };
        VkPipelineLayoutCreateInfo pipeline_layout_info {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .setLayoutCount = 1,
            .pSetLayouts = &s_resources.descriptor_set_layout,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_constant_range,
        };
        result = vkCreatePipelineLayout(context.logical_device, &pipeline_layout_info, nullptr, &s_resources.pipeline_layout);
        if (result != VK_SUCCESS)
            return log_failure("create_textured_pipeline_layout_failed"sv, result);

        VkPipelineShaderStageCreateInfo shader_stages[] {
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = nullptr, .flags = 0, .stage = VK_SHADER_STAGE_VERTEX_BIT, .module = s_resources.vertex_shader, .pName = "main", .pSpecializationInfo = nullptr },
            { .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .pNext = nullptr, .flags = 0, .stage = VK_SHADER_STAGE_FRAGMENT_BIT, .module = s_resources.fragment_shader, .pName = "main", .pSpecializationInfo = nullptr },
        };
        Array<VkVertexInputBindingDescription, 2> vertex_bindings {
            VkVertexInputBindingDescription { .binding = 0, .stride = sizeof(float) * 3, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
            VkVertexInputBindingDescription { .binding = 1, .stride = sizeof(float) * 2, .inputRate = VK_VERTEX_INPUT_RATE_VERTEX },
        };
        Array<VkVertexInputAttributeDescription, 2> vertex_attributes {
            VkVertexInputAttributeDescription { .location = 0, .binding = 0, .format = VK_FORMAT_R32G32B32_SFLOAT, .offset = 0 },
            VkVertexInputAttributeDescription { .location = 1, .binding = 1, .format = VK_FORMAT_R32G32_SFLOAT, .offset = 0 },
        };
        VkPipelineVertexInputStateCreateInfo vertex_input_info {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .vertexBindingDescriptionCount = vertex_bindings.size(),
            .pVertexBindingDescriptions = vertex_bindings.data(),
            .vertexAttributeDescriptionCount = vertex_attributes.size(),
            .pVertexAttributeDescriptions = vertex_attributes.data(),
        };
        VkPipelineInputAssemblyStateCreateInfo input_assembly {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
            .primitiveRestartEnable = VK_FALSE,
        };
        VkViewport viewport { .x = 0, .y = 0, .width = 1.0f, .height = 1.0f, .minDepth = 0.0f, .maxDepth = 1.0f };
        VkRect2D scissor { .offset = { 0, 0 }, .extent = { 1, 1 } };
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
            .blendEnable = VK_TRUE,
            .srcColorBlendFactor = VK_BLEND_FACTOR_SRC_ALPHA,
            .dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
            .colorBlendOp = VK_BLEND_OP_ADD,
            .srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE,
            .dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA,
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
        VkDynamicState dynamic_states[] { VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR };
        VkPipelineDynamicStateCreateInfo dynamic_state {
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .dynamicStateCount = 2,
            .pDynamicStates = dynamic_states,
        };
        VkGraphicsPipelineCreateInfo pipeline_info {
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .stageCount = 2,
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
        if (result != VK_SUCCESS)
            return log_failure("create_textured_graphics_pipeline_failed"sv, result);

        VkDescriptorPoolSize pool_size { .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, .descriptorCount = uses_alpha ? 2u : 1u };
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
            return log_failure("create_textured_descriptor_pool_failed"sv, result);

        VkDescriptorSetAllocateInfo descriptor_set_info {
            .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
            .pNext = nullptr,
            .descriptorPool = s_resources.descriptor_pool,
            .descriptorSetCount = 1,
            .pSetLayouts = &s_resources.descriptor_set_layout,
        };
        result = vkAllocateDescriptorSets(context.logical_device, &descriptor_set_info, &s_resources.descriptor_set);
        if (result != VK_SUCCESS)
            return log_failure("allocate_textured_descriptor_set_failed"sv, result);

        VkSamplerCreateInfo sampler_info {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .pNext = nullptr,
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
            .borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK,
            .unnormalizedCoordinates = VK_FALSE,
        };
        result = vkCreateSampler(context.logical_device, &sampler_info, nullptr, &s_resources.sampler);
        if (result != VK_SUCCESS)
            return log_failure("create_textured_sampler_failed"sv, result);

        s_resources.device = context.logical_device;
        s_resources.destination_format = format;
        s_resources.uses_alpha = uses_alpha;
    }

    if (source_image.cached_solid_mesh_color_attachment_view == VK_NULL_HANDLE) {
        VkImageViewCreateInfo source_image_view_info {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = source_image.image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = source_image.info.format,
            .components = { .r = VK_COMPONENT_SWIZZLE_IDENTITY, .g = VK_COMPONENT_SWIZZLE_IDENTITY, .b = VK_COMPONENT_SWIZZLE_IDENTITY, .a = VK_COMPONENT_SWIZZLE_IDENTITY },
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        result = vkCreateImageView(context.logical_device, &source_image_view_info, nullptr, &source_image.cached_solid_mesh_color_attachment_view);
        if (result != VK_SUCCESS)
            return log_failure("create_textured_source_image_view_failed"sv, result);
    }
    if (target_image->cached_solid_mesh_color_attachment_view == VK_NULL_HANDLE) {
        VkImageViewCreateInfo target_image_view_info {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = target_image->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = target_image->info.format,
            .components = { .r = VK_COMPONENT_SWIZZLE_IDENTITY, .g = VK_COMPONENT_SWIZZLE_IDENTITY, .b = VK_COMPONENT_SWIZZLE_IDENTITY, .a = VK_COMPONENT_SWIZZLE_IDENTITY },
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        result = vkCreateImageView(context.logical_device, &target_image_view_info, nullptr, &target_image->cached_solid_mesh_color_attachment_view);
        if (result != VK_SUCCESS)
            return log_failure("create_textured_target_image_view_failed"sv, result);
    }
    if (alpha_image && alpha_image->cached_solid_mesh_color_attachment_view == VK_NULL_HANDLE) {
        VkImageViewCreateInfo alpha_image_view_info {
            .sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .image = alpha_image->image,
            .viewType = VK_IMAGE_VIEW_TYPE_2D,
            .format = alpha_image->info.format,
            .components = { .r = VK_COMPONENT_SWIZZLE_IDENTITY, .g = VK_COMPONENT_SWIZZLE_IDENTITY, .b = VK_COMPONENT_SWIZZLE_IDENTITY, .a = VK_COMPONENT_SWIZZLE_IDENTITY },
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        };
        result = vkCreateImageView(context.logical_device, &alpha_image_view_info, nullptr, &alpha_image->cached_solid_mesh_color_attachment_view);
        if (result != VK_SUCCESS)
            return log_failure("create_textured_alpha_image_view_failed"sv, result);
    }
    if (s_resources.target_framebuffer == VK_NULL_HANDLE
        || s_resources.target_framebuffer_image != target_image->image
        || s_resources.target_framebuffer_render_pass != s_resources.render_pass
        || s_resources.target_framebuffer_width != target_image->info.extent.width
        || s_resources.target_framebuffer_height != target_image->info.extent.height) {
        if (s_resources.target_framebuffer != VK_NULL_HANDLE) {
            for (auto fence : s_resources.ring_fences) {
                if (fence != VK_NULL_HANDLE)
                    vkWaitForFences(context.logical_device, 1, &fence, VK_TRUE, UINT64_MAX);
            }
            vkDestroyFramebuffer(context.logical_device, s_resources.target_framebuffer, nullptr);
            s_resources.target_framebuffer = VK_NULL_HANDLE;
        }
        VkFramebufferCreateInfo framebuffer_info {
            .sType = VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO,
            .pNext = nullptr,
            .flags = 0,
            .renderPass = s_resources.render_pass,
            .attachmentCount = 1,
            .pAttachments = &target_image->cached_solid_mesh_color_attachment_view,
            .width = target_image->info.extent.width,
            .height = target_image->info.extent.height,
            .layers = 1,
        };
        result = vkCreateFramebuffer(context.logical_device, &framebuffer_info, nullptr, &s_resources.target_framebuffer);
        if (result != VK_SUCCESS)
            return log_failure("create_textured_target_framebuffer_failed"sv, result);
        s_resources.target_framebuffer_image = target_image->image;
        s_resources.target_framebuffer_render_pass = s_resources.render_pass;
        s_resources.target_framebuffer_width = target_image->info.extent.width;
        s_resources.target_framebuffer_height = target_image->info.extent.height;
    }

    auto alpha_image_view = alpha_image ? alpha_image->cached_solid_mesh_color_attachment_view : VK_NULL_HANDLE;
    if (s_resources.last_source_image_view != source_image.cached_solid_mesh_color_attachment_view
        || s_resources.last_alpha_image_view != alpha_image_view) {
        Array<VkDescriptorImageInfo, 2> image_infos {
            VkDescriptorImageInfo {
                .sampler = s_resources.sampler,
                .imageView = source_image.cached_solid_mesh_color_attachment_view,
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            },
            VkDescriptorImageInfo {
                .sampler = s_resources.sampler,
                .imageView = alpha_image_view,
                .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            },
        };
        Array<VkWriteDescriptorSet, 2> descriptor_writes {
            VkWriteDescriptorSet {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = s_resources.descriptor_set,
                .dstBinding = 0,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &image_infos[0],
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
            VkWriteDescriptorSet {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .pNext = nullptr,
                .dstSet = s_resources.descriptor_set,
                .dstBinding = 1,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &image_infos[1],
                .pBufferInfo = nullptr,
                .pTexelBufferView = nullptr,
            },
        };
        vkUpdateDescriptorSets(context.logical_device, uses_alpha ? 2u : 1u, descriptor_writes.data(), 0, nullptr);
        s_resources.last_source_image_view = source_image.cached_solid_mesh_color_attachment_view;
        s_resources.last_alpha_image_view = alpha_image_view;
    }

    if (s_resources.ring_command_pool == VK_NULL_HANDLE) {
        VkCommandPoolCreateInfo command_pool_info {
            .sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO,
            .pNext = nullptr,
            .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT,
            .queueFamilyIndex = context.graphics_queue_family,
        };
        result = vkCreateCommandPool(context.logical_device, &command_pool_info, nullptr, &s_resources.ring_command_pool);
        if (result != VK_SUCCESS)
            return log_failure("create_textured_ring_command_pool_failed"sv, result);

        VkCommandBufferAllocateInfo command_buffer_alloc_info {
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO,
            .pNext = nullptr,
            .commandPool = s_resources.ring_command_pool,
            .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY,
            .commandBufferCount = textured_ring_slot_count,
        };
        result = vkAllocateCommandBuffers(context.logical_device, &command_buffer_alloc_info, s_resources.ring_command_buffers.data());
        if (result != VK_SUCCESS)
            return log_failure("allocate_textured_ring_command_buffers_failed"sv, result);

        VkFenceCreateInfo fence_info { .sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .pNext = nullptr, .flags = VK_FENCE_CREATE_SIGNALED_BIT };
        for (size_t i = 0; i < textured_ring_slot_count; ++i) {
            result = vkCreateFence(context.logical_device, &fence_info, nullptr, &s_resources.ring_fences[i]);
            if (result != VK_SUCCESS)
                return log_failure("create_textured_ring_fence_failed"sv, result);
        }
    }

    auto index_type = VK_INDEX_TYPE_UINT16;
    if (draw_type == GL_UNSIGNED_SHORT)
        index_type = VK_INDEX_TYPE_UINT16;
    else if (draw_type == GL_UNSIGNED_INT)
        index_type = VK_INDEX_TYPE_UINT32;
    else
        return log_failure("unsupported_index_type_for_textured_mesh_draw"sv);

    auto queue_ring_slot = s_resources.ring_cursor++ % textured_ring_slot_count;
    auto wait_started_at = MonotonicTime::now();
    result = vkWaitForFences(context.logical_device, 1, &s_resources.ring_fences[queue_ring_slot], VK_TRUE, UINT64_MAX);
    auto queue_wait_us = (MonotonicTime::now() - wait_started_at).to_microseconds();
    if (result != VK_SUCCESS)
        return log_failure("wait_textured_ring_fence_failed"sv, result);
    result = vkResetFences(context.logical_device, 1, &s_resources.ring_fences[queue_ring_slot]);
    if (result != VK_SUCCESS)
        return log_failure("reset_textured_ring_fence_failed"sv, result);
    auto command_buffer = s_resources.ring_command_buffers[queue_ring_slot];
    vkResetCommandBuffer(command_buffer, 0);
    VkCommandBufferBeginInfo begin_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    result = vkBeginCommandBuffer(command_buffer, &begin_info);
    if (result != VK_SUCCESS)
        return log_failure("begin_textured_draw_command_buffer_failed"sv, result);

    Array<VkImageMemoryBarrier, 3> pre_barriers {
        VkImageMemoryBarrier {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = source_image.info.layout,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source_image.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        VkImageMemoryBarrier {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .oldLayout = alpha_image ? alpha_image->info.layout : VK_IMAGE_LAYOUT_UNDEFINED,
            .newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = alpha_image ? alpha_image->image : VK_NULL_HANDLE,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        VkImageMemoryBarrier {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT,
            .oldLayout = target_image->info.layout,
            .newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = target_image->image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
    };
    if (uses_alpha)
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, pre_barriers.size(), pre_barriers.data());
    else {
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, pre_barriers.data());
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, 0, 0, nullptr, 0, nullptr, 1, &pre_barriers[2]);
    }

    VkRenderPassBeginInfo render_pass_begin {
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .pNext = nullptr,
        .renderPass = s_resources.render_pass,
        .framebuffer = s_resources.target_framebuffer,
        .renderArea = { .offset = { 0, 0 }, .extent = { target_image->info.extent.width, target_image->info.extent.height } },
        .clearValueCount = 0,
        .pClearValues = nullptr,
    };
    vkCmdBeginRenderPass(command_buffer, &render_pass_begin, VK_SUBPASS_CONTENTS_INLINE);
    VkViewport viewport {
        .x = static_cast<float>(viewport_x),
        .y = static_cast<float>(viewport_y + viewport_height),
        .width = static_cast<float>(viewport_width),
        .height = -static_cast<float>(viewport_height),
        .minDepth = 0.0f,
        .maxDepth = 1.0f,
    };
    VkRect2D scissor {
        .offset = { viewport_x, viewport_y },
        .extent = { static_cast<u32>(viewport_width), static_cast<u32>(viewport_height) },
    };
    vkCmdSetViewport(command_buffer, 0, 1, &viewport);
    vkCmdSetScissor(command_buffer, 0, 1, &scissor);
    vkCmdBindPipeline(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, s_resources.pipeline);
    vkCmdBindDescriptorSets(command_buffer, VK_PIPELINE_BIND_POINT_GRAPHICS, s_resources.pipeline_layout, 0, 1, &s_resources.descriptor_set, 0, nullptr);
    TexturedPushConstants push_constants {
        .model_view_matrix = uniform_snapshot.model_view_matrix,
        .projection_matrix = uniform_snapshot.projection_matrix,
        .diffuse = uniform_snapshot.diffuse,
        .use_matrices = uniform_snapshot.has_model_view_matrix && uniform_snapshot.has_projection_matrix ? 1.0f : 0.0f,
        .opacity = uniform_snapshot.opacity,
        .output_intensity = uniform_snapshot.output_intensity,
        .flip_y = 0.0f,
    };
    vkCmdPushConstants(command_buffer, s_resources.pipeline_layout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push_constants), &push_constants);
    VkBuffer vertex_buffers[] { replay_buffers.position_buffer->buffer, replay_buffers.uv_buffer->buffer };
    VkDeviceSize vertex_offsets[] { 0, 0 };
    vkCmdBindVertexBuffers(command_buffer, 0, 2, vertex_buffers, vertex_offsets);
    vkCmdBindIndexBuffer(command_buffer, replay_buffers.index_buffer->buffer, draw_offset, index_type);
    vkCmdDrawIndexed(command_buffer, draw_count, 1, 0, 0, 0);
    vkCmdEndRenderPass(command_buffer);

    Array<VkImageMemoryBarrier, 3> post_barriers {
        VkImageMemoryBarrier {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout = source_image.info.layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = source_image.image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        VkImageMemoryBarrier {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_SHADER_READ_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
            .newLayout = alpha_image ? alpha_image->info.layout : VK_IMAGE_LAYOUT_UNDEFINED,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = alpha_image ? alpha_image->image : VK_NULL_HANDLE,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
        VkImageMemoryBarrier {
            .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
            .pNext = nullptr,
            .srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT,
            .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
            .oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
            .newLayout = target_image->info.layout,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = target_image->image,
            .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
        },
    };
    if (uses_alpha)
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, post_barriers.size(), post_barriers.data());
    else {
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, post_barriers.data());
        vkCmdPipelineBarrier(command_buffer, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &post_barriers[2]);
    }
    result = vkEndCommandBuffer(command_buffer);
    if (result != VK_SUCCESS)
        return log_failure("end_textured_draw_command_buffer_failed"sv, result);

    VkSubmitInfo submit_info {
        .sType = VK_STRUCTURE_TYPE_SUBMIT_INFO,
        .pNext = nullptr,
        .waitSemaphoreCount = 0,
        .pWaitSemaphores = nullptr,
        .pWaitDstStageMask = nullptr,
        .commandBufferCount = 1,
        .pCommandBuffers = &command_buffer,
        .signalSemaphoreCount = 0,
        .pSignalSemaphores = nullptr,
    };
    auto submit_started_at = MonotonicTime::now();
    result = vkQueueSubmit(context.graphics_queue, 1, &submit_info, s_resources.ring_fences[queue_ring_slot]);
    auto queue_submit_us = (MonotonicTime::now() - submit_started_at).to_microseconds();
    if (result != VK_SUCCESS)
        return log_failure("submit_textured_draw_command_buffer_failed"sv, result);

    if (should_log) {
        dbgln("MUNDO_WEBGL_TEXTURED_MESH_PIPELINE_PROBE count={} probe_count={} status=ok pipeline_cache_status={} buffer_cache_status={} draw_status=executed queue_ring_slot={} queue_submit_us={} queue_wait_us={} destination_format={} source_image={} source_image_view={} source_size={}x{} source_layout={} target_image={} target_image_view={} target_framebuffer={} target_size={}x{} draw_index_count={} draw_index_type={} draw_index_offset={} viewport={}x{}+{}+{} matrix_push_constants={} diffuse=({}, {}, {}, {}) opacity={} output_intensity={} vertex_bindings=2 vertex_attributes=2 next_step=wire_render_target_sampler_consumer_to_this_pipeline",
            log_count,
            probe_count,
            pipeline_cache_status,
            buffer_cache_status,
            queue_ring_slot,
            queue_submit_us,
            queue_wait_us,
            destination_format,
            reinterpret_cast<uintptr_t>(source_image.image),
            reinterpret_cast<uintptr_t>(source_image.cached_solid_mesh_color_attachment_view),
            source_image.info.extent.width,
            source_image.info.extent.height,
            to_underlying(source_image.info.layout),
            reinterpret_cast<uintptr_t>(target_image->image),
            reinterpret_cast<uintptr_t>(target_image->cached_solid_mesh_color_attachment_view),
            reinterpret_cast<uintptr_t>(s_resources.target_framebuffer),
            target_image->info.extent.width,
            target_image->info.extent.height,
            draw_count,
            draw_type,
            draw_offset,
            viewport_width,
            viewport_height,
            viewport_x,
            viewport_y,
            uniform_snapshot.has_model_view_matrix && uniform_snapshot.has_projection_matrix,
            uniform_snapshot.diffuse[0],
            uniform_snapshot.diffuse[1],
            uniform_snapshot.diffuse[2],
            uniform_snapshot.diffuse[3],
            uniform_snapshot.opacity,
            uniform_snapshot.output_intensity);
    }
    return VulkanVideoMeshPipelineProbeResult {
        .attempted = true,
        .supported = true,
        .executed = true,
        .reason = "ok"sv,
    };
}

Optional<u32> OpenGLContext::vulkan_painting_surface_format() const
{
    if (!m_painting_surface)
        return {};
    auto vulkan_image = m_painting_surface->vulkan_image();
    if (!vulkan_image)
        return {};
    return to_underlying(vulkan_image->info.format);
}

ErrorOr<OpenGLContext::ImportedVideoOpaqueFDTexture> OpenGLContext::create_imported_video_opaque_fd_texture(u32 width, u32 height, u32 vulkan_format, u32 gl_internal_format, char const* label, size_t log_count)
{
    auto log_failure = [&](StringView reason, u32 gl_error = 0) {
        dbgln("MUNDO_WEBGL_VIDEO_OPAQUE_FD_TEXTURE_IMPORT count={} label={} status=failed reason={} gl_error={} size={}x{} vk_format={} gl_internal_format={}",
            log_count,
            label,
            reason,
            gl_error,
            width,
            height,
            vulkan_format,
            gl_internal_format);
    };

    auto* gl_create_memory_objects_ext = reinterpret_cast<PFNGLCREATEMEMORYOBJECTSEXTPROC>(eglGetProcAddress("glCreateMemoryObjectsEXT"));
    auto* gl_delete_memory_objects_ext = reinterpret_cast<PFNGLDELETEMEMORYOBJECTSEXTPROC>(eglGetProcAddress("glDeleteMemoryObjectsEXT"));
    auto* gl_memory_object_parameteriv_ext = reinterpret_cast<PFNGLMEMORYOBJECTPARAMETERIVEXTPROC>(eglGetProcAddress("glMemoryObjectParameterivEXT"));
    auto* gl_import_memory_fd_ext = reinterpret_cast<PFNGLIMPORTMEMORYFDEXTPROC>(eglGetProcAddress("glImportMemoryFdEXT"));
    auto* gl_tex_storage_mem_2d_ext = reinterpret_cast<PFNGLTEXSTORAGEMEM2DEXTPROC>(eglGetProcAddress("glTexStorageMem2DEXT"));

    if (!gl_create_memory_objects_ext || !gl_delete_memory_objects_ext || !gl_memory_object_parameteriv_ext || !gl_import_memory_fd_ext || !gl_tex_storage_mem_2d_ext) {
        log_failure("missing_gl_memory_object_fd_extension"sv);
        return Error::from_string_literal("GL memory object fd extension functions are unavailable");
    }

    auto image_or_error = Gfx::create_opaque_fd_vulkan_image(m_skia_backend_context->vulkan_context(), width, height, static_cast<VkFormat>(vulkan_format));
    if (image_or_error.is_error()) {
        log_failure(image_or_error.error().string_literal());
        return image_or_error.release_error();
    }
    auto image = image_or_error.release_value();
    auto fd = image->get_opaque_fd();
    if (fd < 0) {
        log_failure("vulkan_opaque_fd_export_failed"sv);
        return Error::from_string_literal("Failed to export Vulkan opaque fd for GL texture import");
    }

    GLuint memory_object { 0 };
    GLuint texture { 0 };
    auto cleanup = ArmedScopeGuard([&] {
        if (texture)
            glDeleteTextures(1, &texture);
        if (memory_object)
            gl_delete_memory_objects_ext(1, &memory_object);
        if (fd >= 0)
            close(fd);
    });

    while (glGetError() != GL_NO_ERROR) {
    }

    gl_create_memory_objects_ext(1, &memory_object);
    auto create_error = glGetError();
    if (create_error != GL_NO_ERROR || !memory_object) {
        log_failure("gl_memory_object_create_failed"sv, create_error);
        return Error::from_string_literal("Failed to create GL memory object for video opaque fd");
    }

    GLint dedicated = GL_TRUE;
    gl_memory_object_parameteriv_ext(memory_object, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
    auto parameter_error = glGetError();
    if (parameter_error != GL_NO_ERROR) {
        log_failure("gl_memory_object_dedicated_flag_failed"sv, parameter_error);
        return Error::from_string_literal("Failed to set GL memory object dedicated flag");
    }

    gl_import_memory_fd_ext(memory_object, image->info.allocation_size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
    fd = -1; // glImportMemoryFdEXT takes ownership.
    auto import_error = glGetError();
    if (import_error != GL_NO_ERROR) {
        log_failure("gl_memory_object_fd_import_failed"sv, import_error);
        return Error::from_string_literal("Failed to import Vulkan opaque fd into GL memory object");
    }

    glGenTextures(1, &texture);
    if (!texture) {
        log_failure("gl_texture_create_failed"sv, glGetError());
        return Error::from_string_literal("Failed to create imported video GL texture");
    }

    glBindTexture(GL_TEXTURE_2D, texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    gl_tex_storage_mem_2d_ext(GL_TEXTURE_2D, 1, gl_internal_format, width, height, memory_object, 0);
    auto storage_error = glGetError();
    if (storage_error != GL_NO_ERROR) {
        log_failure("gl_texture_storage_mem_failed"sv, storage_error);
        return Error::from_string_literal("Failed to bind GL texture storage to imported video memory object");
    }

    if (log_count <= 8 || log_count % 120 == 0) {
        dbgln("MUNDO_WEBGL_VIDEO_OPAQUE_FD_TEXTURE_IMPORT count={} label={} status=ok texture={} memory_object={} size={}x{} allocation_size={} vk_format={} gl_internal_format={}",
            log_count,
            label,
            texture,
            memory_object,
            width,
            height,
            image->info.allocation_size,
            vulkan_format,
            gl_internal_format);
    }

    cleanup.disarm();
    return ImportedVideoOpaqueFDTexture {
        .image = image,
        .memory_object = memory_object,
        .texture = texture,
        .width = width,
        .height = height,
        .allocation_size = image->info.allocation_size,
    };
}

ErrorOr<OpenGLContext::ImportedVideoOpaqueFDTexturePair> OpenGLContext::get_or_create_imported_video_opaque_fd_textures(u32 width, u32 height, u32 uv_width, u32 uv_height, size_t log_count)
{
    auto texture_matches = [](ImportedVideoOpaqueFDTexture const& texture, u32 expected_width, u32 expected_height) {
        return texture.width == expected_width && texture.height == expected_height && texture.texture && texture.memory_object && texture.allocation_size;
    };

    auto reused_y = m_impl->cached_video_y_texture.has_value() && texture_matches(m_impl->cached_video_y_texture.value(), width, height);
    if (!reused_y) {
        if (m_impl->cached_video_y_texture.has_value())
            delete_imported_video_opaque_fd_texture(m_impl->cached_video_y_texture.value());
        m_impl->cached_video_y_texture = TRY(create_imported_video_opaque_fd_texture(width, height, VK_FORMAT_R8_UNORM, GL_R8_EXT, "video_y_r8_cached", log_count));
    }

    auto reused_uv = m_impl->cached_video_uv_texture.has_value() && texture_matches(m_impl->cached_video_uv_texture.value(), uv_width, uv_height);
    if (!reused_uv) {
        if (m_impl->cached_video_uv_texture.has_value())
            delete_imported_video_opaque_fd_texture(m_impl->cached_video_uv_texture.value());
        m_impl->cached_video_uv_texture = TRY(create_imported_video_opaque_fd_texture(uv_width, uv_height, VK_FORMAT_R8G8_UNORM, GL_RG8_EXT, "video_uv_rg8_cached", log_count));
    }

    if (log_count <= 8 || log_count % 120 == 0) {
        dbgln("MUNDO_WEBGL_VIDEO_EXTERNAL_MEMORY_CACHE count={} y_texture={} uv_texture={} y_size={}x{} uv_size={}x{} reused_y={} reused_uv={}",
            log_count,
            m_impl->cached_video_y_texture->texture,
            m_impl->cached_video_uv_texture->texture,
            width,
            height,
            uv_width,
            uv_height,
            reused_y,
            reused_uv);
    }

    return ImportedVideoOpaqueFDTexturePair {
        .y = &m_impl->cached_video_y_texture.value(),
        .uv = &m_impl->cached_video_uv_texture.value(),
        .reused_y = reused_y,
        .reused_uv = reused_uv,
    };
}

ErrorOr<OpenGLContext::ImportedVideoOpaqueFDTexture*> OpenGLContext::get_or_create_imported_video_rgba_texture(u32 width, u32 height, size_t log_count)
{
    auto texture_matches = [](ImportedVideoOpaqueFDTexture const& texture, u32 expected_width, u32 expected_height) {
        return texture.width == expected_width && texture.height == expected_height && texture.texture && texture.memory_object && texture.allocation_size;
    };

    auto reused = m_impl->cached_video_rgba_texture.has_value() && texture_matches(m_impl->cached_video_rgba_texture.value(), width, height);
    if (!reused) {
        if (m_impl->cached_video_rgba_texture.has_value())
            delete_imported_video_opaque_fd_texture(m_impl->cached_video_rgba_texture.value());
        m_impl->cached_video_rgba_texture = TRY(create_imported_video_opaque_fd_texture(width, height, VK_FORMAT_R8G8B8A8_UNORM, GL_RGBA8, "video_rgba8_cached", log_count));
    }

    if (log_count <= 8 || log_count % 120 == 0) {
        dbgln("MUNDO_WEBGL_VIDEO_RGBA_TARGET_IMPORT count={} status=ok texture={} size={}x{} allocation_size={} reused={} vk_format={} gl_internal_format={}",
            log_count,
            m_impl->cached_video_rgba_texture->texture,
            width,
            height,
            m_impl->cached_video_rgba_texture->allocation_size,
            reused,
            to_underlying(VK_FORMAT_R8G8B8A8_UNORM),
            GL_RGBA8);
    }

    return &m_impl->cached_video_rgba_texture.value();
}

ErrorOr<OpenGLContext::ImportedVideoOpaqueFDTexture*> OpenGLContext::get_or_create_imported_video_rgba_target_texture(u32 target_texture, u32 width, u32 height, size_t log_count)
{
    auto texture_matches = [&](ImportedVideoOpaqueFDTexture const& texture, u32 expected_texture, u32 expected_width, u32 expected_height) {
        return texture.texture == expected_texture && texture.width == expected_width && texture.height == expected_height && texture.memory_object && texture.allocation_size;
    };

    for (auto const& failed_import : m_impl->failed_video_rgba_target_texture_imports) {
        if (failed_import.target_texture == target_texture && failed_import.width == width && failed_import.height == height) {
            if (log_count <= 24 || log_count % 120 == 0) {
                dbgln("MUNDO_WEBGL_VIDEO_TARGET_TEXTURE_STORAGE_IMPORT count={} status=failed reason=previous_import_failed original_reason={} original_gl_error={} target_texture={} size={}x{} vk_format={} gl_internal_format={} next_step=use_separate_vulkan_render_target_consumer_instead_of_rebinding_existing_webgl_texture_storage",
                    log_count,
                    failed_import.reason,
                    failed_import.gl_error,
                    target_texture,
                    width,
                    height,
                    to_underlying(VK_FORMAT_R8G8B8A8_UNORM),
                    GL_RGBA8);
            }
            return Error::from_string_literal("Previous WebGL target texture storage import failed");
        }
    }

    auto reused = m_impl->cached_video_rgba_target_texture.has_value() && texture_matches(m_impl->cached_video_rgba_target_texture.value(), target_texture, width, height);
    if (!reused) {
        if (m_impl->cached_video_rgba_target_texture.has_value())
            delete_imported_video_opaque_fd_texture(m_impl->cached_video_rgba_target_texture.value());

        auto remember_failure = [&](StringView reason, u32 gl_error) {
            for (auto const& failed_import : m_impl->failed_video_rgba_target_texture_imports) {
                if (failed_import.target_texture == target_texture && failed_import.width == width && failed_import.height == height)
                    return;
            }
            m_impl->failed_video_rgba_target_texture_imports.append(Impl::FailedVideoRGBATargetTextureImport {
                .target_texture = target_texture,
                .width = width,
                .height = height,
                .reason = reason,
                .gl_error = gl_error,
            });
        };

        auto log_failure = [&](StringView reason, u32 gl_error = 0) {
            dbgln("MUNDO_WEBGL_VIDEO_TARGET_TEXTURE_STORAGE_IMPORT count={} status=failed reason={} gl_error={} target_texture={} size={}x{} vk_format={} gl_internal_format={}",
                log_count,
                reason,
                gl_error,
                target_texture,
                width,
                height,
                to_underlying(VK_FORMAT_R8G8B8A8_UNORM),
                GL_RGBA8);
        };

        auto* gl_create_memory_objects_ext = reinterpret_cast<PFNGLCREATEMEMORYOBJECTSEXTPROC>(eglGetProcAddress("glCreateMemoryObjectsEXT"));
        auto* gl_delete_memory_objects_ext = reinterpret_cast<PFNGLDELETEMEMORYOBJECTSEXTPROC>(eglGetProcAddress("glDeleteMemoryObjectsEXT"));
        auto* gl_memory_object_parameteriv_ext = reinterpret_cast<PFNGLMEMORYOBJECTPARAMETERIVEXTPROC>(eglGetProcAddress("glMemoryObjectParameterivEXT"));
        auto* gl_import_memory_fd_ext = reinterpret_cast<PFNGLIMPORTMEMORYFDEXTPROC>(eglGetProcAddress("glImportMemoryFdEXT"));
        auto* gl_tex_storage_mem_2d_ext = reinterpret_cast<PFNGLTEXSTORAGEMEM2DEXTPROC>(eglGetProcAddress("glTexStorageMem2DEXT"));
        if (!gl_create_memory_objects_ext || !gl_delete_memory_objects_ext || !gl_memory_object_parameteriv_ext || !gl_import_memory_fd_ext || !gl_tex_storage_mem_2d_ext) {
            log_failure("missing_gl_memory_object_fd_extension"sv);
            return Error::from_string_literal("GL memory object fd extension functions are unavailable");
        }

        auto image_or_error = Gfx::create_opaque_fd_vulkan_image(m_skia_backend_context->vulkan_context(), width, height, VK_FORMAT_R8G8B8A8_UNORM);
        if (image_or_error.is_error()) {
            log_failure(image_or_error.error().string_literal());
            return image_or_error.release_error();
        }
        auto image = image_or_error.release_value();
        auto fd = image->get_opaque_fd();
        if (fd < 0) {
            log_failure("vulkan_opaque_fd_export_failed"sv);
            return Error::from_string_literal("Failed to export Vulkan opaque fd for WebGL target texture import");
        }

        GLuint memory_object { 0 };
        auto cleanup = ArmedScopeGuard([&] {
            if (memory_object)
                gl_delete_memory_objects_ext(1, &memory_object);
            if (fd >= 0)
                close(fd);
        });

        while (glGetError() != GL_NO_ERROR) {
        }

        gl_create_memory_objects_ext(1, &memory_object);
        auto create_error = glGetError();
        if (create_error != GL_NO_ERROR || !memory_object) {
            log_failure("gl_memory_object_create_failed"sv, create_error);
            return Error::from_string_literal("Failed to create GL memory object for WebGL target texture");
        }

        GLint dedicated = GL_TRUE;
        gl_memory_object_parameteriv_ext(memory_object, GL_DEDICATED_MEMORY_OBJECT_EXT, &dedicated);
        auto parameter_error = glGetError();
        if (parameter_error != GL_NO_ERROR) {
            log_failure("gl_memory_object_dedicated_flag_failed"sv, parameter_error);
            return Error::from_string_literal("Failed to set GL memory object dedicated flag for WebGL target texture");
        }

        gl_import_memory_fd_ext(memory_object, image->info.allocation_size, GL_HANDLE_TYPE_OPAQUE_FD_EXT, fd);
        fd = -1; // glImportMemoryFdEXT takes ownership.
        auto import_error = glGetError();
        if (import_error != GL_NO_ERROR) {
            log_failure("gl_memory_object_fd_import_failed"sv, import_error);
            return Error::from_string_literal("Failed to import Vulkan opaque fd into WebGL target memory object");
        }

        glBindTexture(GL_TEXTURE_2D, target_texture);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        gl_tex_storage_mem_2d_ext(GL_TEXTURE_2D, 1, GL_RGBA8, width, height, memory_object, 0);
        auto storage_error = glGetError();
        if (storage_error != GL_NO_ERROR) {
            log_failure("gl_target_texture_storage_mem_failed"sv, storage_error);
            remember_failure("gl_target_texture_storage_mem_failed"sv, storage_error);
            return Error::from_string_literal("Failed to bind WebGL target texture storage to imported video memory object");
        }

        cleanup.disarm();
        m_impl->cached_video_rgba_target_texture = ImportedVideoOpaqueFDTexture {
            .image = image,
            .memory_object = memory_object,
            .texture = target_texture,
            .width = width,
            .height = height,
            .allocation_size = image->info.allocation_size,
            .owns_texture = false,
        };
    }

    if (log_count <= 8 || log_count % 120 == 0) {
        dbgln("MUNDO_WEBGL_VIDEO_TARGET_TEXTURE_STORAGE_IMPORT count={} status=ok target_texture={} size={}x{} allocation_size={} reused={} vk_format={} gl_internal_format={}",
            log_count,
            target_texture,
            width,
            height,
            m_impl->cached_video_rgba_target_texture->allocation_size,
            reused,
            to_underlying(VK_FORMAT_R8G8B8A8_UNORM),
            GL_RGBA8);
    }

    return &m_impl->cached_video_rgba_target_texture.value();
}

ErrorOr<OpenGLContext::ImportedVideoOpaqueFDTexture*> OpenGLContext::get_or_create_vulkan_rgba_render_target_image(u32 target_texture, u32 width, u32 height, size_t log_count)
{
    for (auto& cached_image : m_impl->cached_vulkan_rgba_render_target_images) {
        if (cached_image.texture == target_texture && cached_image.width == width && cached_image.height == height) {
            if (log_count <= 24 || log_count % 120 == 0) {
                dbgln("MUNDO_WEBGL_VULKAN_RENDER_TARGET_IMAGE_CACHE count={} status=ok reused=true target_texture={} size={}x{} allocation_size={} vk_format={} usage={} layout={} next_step=sample_this_image_from_vulkan_consumer_without_gl_storage_rebind",
                    log_count,
                    target_texture,
                    width,
                    height,
                    cached_image.allocation_size,
                    to_underlying(cached_image.image->info.format),
                    cached_image.image->info.usage,
                    to_underlying(cached_image.image->info.layout));
            }
            return &cached_image;
        }
    }

    m_impl->cached_vulkan_rgba_render_target_images.remove_first_matching([&](auto const& cached_image) {
        return cached_image.texture == target_texture;
    });

    auto image_or_error = Gfx::create_opaque_fd_vulkan_image(m_skia_backend_context->vulkan_context(), width, height, VK_FORMAT_R8G8B8A8_UNORM);
    if (image_or_error.is_error()) {
        dbgln("MUNDO_WEBGL_VULKAN_RENDER_TARGET_IMAGE_CACHE count={} status=failed reason={} target_texture={} size={}x{} vk_format={} next_step=fix_vulkan_rgba_offscreen_render_target_allocation",
            log_count,
            image_or_error.error().string_literal(),
            target_texture,
            width,
            height,
            to_underlying(VK_FORMAT_R8G8B8A8_UNORM));
        return image_or_error.release_error();
    }

    auto image = image_or_error.release_value();
    m_impl->cached_vulkan_rgba_render_target_images.append(ImportedVideoOpaqueFDTexture {
        .image = image,
        .memory_object = 0,
        .texture = target_texture,
        .width = width,
        .height = height,
        .allocation_size = image->info.allocation_size,
        .owns_texture = false,
    });

    auto& cached_image = m_impl->cached_vulkan_rgba_render_target_images.last();
    dbgln("MUNDO_WEBGL_VULKAN_RENDER_TARGET_IMAGE_CACHE count={} status=ok reused=false target_texture={} size={}x{} allocation_size={} vk_format={} usage={} layout={} next_step=draw_render_target_producer_into_safe_vulkan_offscreen_image",
        log_count,
        target_texture,
        width,
        height,
        cached_image.allocation_size,
        to_underlying(cached_image.image->info.format),
        cached_image.image->info.usage,
        to_underlying(cached_image.image->info.layout));

    return &cached_image;
}

ErrorOr<OpenGLContext::ImportedVideoOpaqueFDTexture*> OpenGLContext::get_or_create_vulkan_rgba_static_texture_image(u32 source_texture, u32 width, u32 height, unsigned signature, ReadonlyBytes rgba_pixels, size_t log_count)
{
    auto expected_size = static_cast<size_t>(width) * static_cast<size_t>(height) * 4;
    if (!width || !height || rgba_pixels.size() < expected_size)
        return Error::from_string_literal("invalid static RGBA texture snapshot");

    for (auto& cached_image : m_impl->cached_vulkan_rgba_static_texture_images) {
        if (cached_image.texture == source_texture && cached_image.width == width && cached_image.height == height && cached_image.signature == signature) {
            if (log_count <= 24 || log_count % 120 == 0) {
                dbgln("MUNDO_WEBGL_VULKAN_STATIC_TEXTURE_IMAGE_CACHE count={} status=ok reused=true source_texture={} size={}x{} signature={} allocation_size={} vk_format={} usage={} layout={} next_step=sample_static_texture_from_multisampler_vulkan_replay",
                    log_count,
                    source_texture,
                    width,
                    height,
                    signature,
                    cached_image.imported_texture.allocation_size,
                    to_underlying(cached_image.imported_texture.image->info.format),
                    cached_image.imported_texture.image->info.usage,
                    to_underlying(cached_image.imported_texture.image->info.layout));
            }
            return &cached_image.imported_texture;
        }
    }

    m_impl->cached_vulkan_rgba_static_texture_images.remove_first_matching([&](auto const& cached_image) {
        return cached_image.texture == source_texture;
    });

    auto image_or_error = Gfx::create_opaque_fd_vulkan_image(m_skia_backend_context->vulkan_context(), width, height, VK_FORMAT_R8G8B8A8_UNORM);
    if (image_or_error.is_error()) {
        dbgln("MUNDO_WEBGL_VULKAN_STATIC_TEXTURE_IMAGE_CACHE count={} status=failed reason={} source_texture={} size={}x{} signature={} next_step=fix_static_texture_vulkan_allocation",
            log_count,
            image_or_error.error().string_literal(),
            source_texture,
            width,
            height,
            signature);
        return image_or_error.release_error();
    }
    auto image = image_or_error.release_value();
    auto const& context = m_skia_backend_context->vulkan_context();

    auto staging_buffer_or_error = Gfx::create_host_visible_vulkan_buffer_from_bytes(context, rgba_pixels.trim(expected_size), VK_BUFFER_USAGE_TRANSFER_SRC_BIT);
    if (staging_buffer_or_error.is_error()) {
        dbgln("MUNDO_WEBGL_VULKAN_STATIC_TEXTURE_IMAGE_CACHE count={} status=failed reason={} source_texture={} size={}x{} signature={} next_step=fix_static_texture_staging_upload",
            log_count,
            staging_buffer_or_error.error().string_literal(),
            source_texture,
            width,
            height,
            signature);
        return staging_buffer_or_error.release_error();
    }
    auto staging_buffer = staging_buffer_or_error.release_value();

    vkQueueWaitIdle(context.graphics_queue);
    vkResetCommandBuffer(context.command_buffer, 0);
    VkCommandBufferBeginInfo begin_info {
        .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
        .pNext = nullptr,
        .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        .pInheritanceInfo = nullptr,
    };
    auto result = vkBeginCommandBuffer(context.command_buffer, &begin_info);
    if (result != VK_SUCCESS)
        return Error::from_string_literal("failed to begin static texture upload command buffer");

    VkImageMemoryBarrier pre_copy_barrier {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .oldLayout = image->info.layout,
        .newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image->image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(context.command_buffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &pre_copy_barrier);

    VkBufferImageCopy copy_region {
        .bufferOffset = 0,
        .bufferRowLength = 0,
        .bufferImageHeight = 0,
        .imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 },
        .imageOffset = { 0, 0, 0 },
        .imageExtent = { width, height, 1 },
    };
    vkCmdCopyBufferToImage(context.command_buffer, staging_buffer->buffer, image->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy_region);

    VkImageMemoryBarrier post_copy_barrier {
        .sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER,
        .pNext = nullptr,
        .srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT,
        .dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT,
        .oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
        .image = image->image,
        .subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 },
    };
    vkCmdPipelineBarrier(context.command_buffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &post_copy_barrier);

    result = vkEndCommandBuffer(context.command_buffer);
    if (result != VK_SUCCESS)
        return Error::from_string_literal("failed to end static texture upload command buffer");

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
    result = vkQueueSubmit(context.graphics_queue, 1, &submit_info, nullptr);
    if (result != VK_SUCCESS)
        return Error::from_string_literal("failed to submit static texture upload command buffer");
    result = vkQueueWaitIdle(context.graphics_queue);
    if (result != VK_SUCCESS)
        return Error::from_string_literal("failed to wait static texture upload queue");
    image->info.layout = VK_IMAGE_LAYOUT_GENERAL;

    m_impl->cached_vulkan_rgba_static_texture_images.append(Impl::CachedVulkanRGBAStaticTextureImage {
        .texture = source_texture,
        .width = width,
        .height = height,
        .signature = signature,
        .imported_texture = ImportedVideoOpaqueFDTexture {
            .image = image,
            .memory_object = 0,
            .texture = source_texture,
            .width = width,
            .height = height,
            .allocation_size = image->info.allocation_size,
            .owns_texture = false,
        },
    });

    auto& cached_image = m_impl->cached_vulkan_rgba_static_texture_images.last();
    dbgln("MUNDO_WEBGL_VULKAN_STATIC_TEXTURE_IMAGE_CACHE count={} status=ok reused=false source_texture={} size={}x{} signature={} byte_length={} allocation_size={} vk_format={} usage={} layout={} next_step=sample_static_texture_from_multisampler_vulkan_replay",
        log_count,
        source_texture,
        width,
        height,
        signature,
        expected_size,
        cached_image.imported_texture.allocation_size,
        to_underlying(cached_image.imported_texture.image->info.format),
        cached_image.imported_texture.image->info.usage,
        to_underlying(cached_image.imported_texture.image->info.layout));

    return &cached_image.imported_texture;
}

ErrorOr<u64> OpenGLContext::copy_vulkan_nv12_external_memory_to_imported_video_textures(Media::HardwareVideoFrameExternalMemoryDescriptor const& external_memory, ImportedVideoOpaqueFDTexturePair const& texture_pair, size_t log_count)
{
    if (external_memory.backend != Media::HardwareVideoFrameBackend::Vulkan)
        return Error::from_string_literal("external memory source is not Vulkan");
    if (!external_memory.single_image || !external_memory.single_memory || external_memory.plane_count != 1)
        return Error::from_string_literal("Vulkan plane copy requires a single-image single-memory NV12 source");
    if (!texture_pair.y || !texture_pair.uv)
        return Error::from_string_literal("missing imported video texture pair");

    auto const& plane = external_memory.planes[0];
    if (plane.fd < 0 || !plane.allocation_size)
        return Error::from_string_literal("Vulkan plane copy source has no opaque fd");

    auto try_copy = [&](char const* label, int fd, VkExternalMemoryHandleTypeFlagBits handle_type) -> ErrorOr<u64> {
        if (fd < 0)
            return Error::from_string_literal("missing Vulkan plane copy fd");

        auto source_fd = dup(fd);
        if (source_fd < 0)
            return Error::from_string_literal("failed to duplicate Vulkan plane copy source fd");

        auto started_at = MonotonicTime::now();
        auto copy_result = Gfx::copy_vulkan_nv12_external_memory_planes_to_opaque_images(
            m_skia_backend_context->vulkan_context(),
            source_fd,
            handle_type,
            plane.allocation_size,
            external_memory.size.width(),
            external_memory.size.height(),
            static_cast<VkFormat>(plane.vulkan_format),
            static_cast<VkImageLayout>(plane.vulkan_image_layout),
            texture_pair.y->image,
            texture_pair.uv->image);
        auto copy_microseconds = (MonotonicTime::now() - started_at).to_microseconds();

        if (copy_result.is_error()) {
            dbgln("MUNDO_WEBGL_VIDEO_VULKAN_PLANE_COPY count={} frame_id={} status=failed label={} reason={} copy_us={} size={}x{} source_format={} source_layout={} source_allocation_size={} handle_type={} y_texture={} uv_texture={}",
                log_count,
                external_memory.frame_id,
                label,
                copy_result.error().string_literal(),
                copy_microseconds,
                external_memory.size.width(),
                external_memory.size.height(),
                plane.vulkan_format,
                plane.vulkan_image_layout,
                plane.allocation_size,
                to_underlying(handle_type),
                texture_pair.y->texture,
                texture_pair.uv->texture);
            return copy_result.release_error();
        }

        if (log_count <= 8 || log_count % 120 == 0 || copy_microseconds > 5000) {
            dbgln("MUNDO_WEBGL_VIDEO_VULKAN_PLANE_COPY count={} frame_id={} status=ok label={} copy_us={} size={}x{} source_format={} source_layout={} source_allocation_size={} handle_type={} y_texture={} uv_texture={}",
                log_count,
                external_memory.frame_id,
                label,
                copy_microseconds,
                external_memory.size.width(),
                external_memory.size.height(),
                plane.vulkan_format,
                plane.vulkan_image_layout,
                plane.allocation_size,
                to_underlying(handle_type),
                texture_pair.y->texture,
                texture_pair.uv->texture);
        }
        return copy_microseconds;
    };

    auto opaque_result = try_copy("opaque_fd", plane.fd, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);
    if (!opaque_result.is_error())
        return opaque_result.release_value();

    auto dma_buf_result = try_copy("dma_buf", plane.dma_buf_fd, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
    if (!dma_buf_result.is_error())
        return dma_buf_result.release_value();

    return dma_buf_result.release_error();
}

ErrorOr<u64> OpenGLContext::render_vulkan_nv12_external_memory_to_imported_video_rgba_texture(Media::HardwareVideoFrameExternalMemoryDescriptor const& external_memory, ImportedVideoOpaqueFDTexture const& rgba_texture, size_t log_count, bool flip_y)
{
    if (external_memory.backend != Media::HardwareVideoFrameBackend::Vulkan)
        return Error::from_string_literal("external memory source is not Vulkan");
    if (!external_memory.single_image || !external_memory.single_memory || external_memory.plane_count != 1)
        return Error::from_string_literal("Vulkan RGBA render requires a single-image single-memory NV12 source");
    if (!rgba_texture.texture || !rgba_texture.memory_object)
        return Error::from_string_literal("missing imported video RGBA texture");

    auto const& plane = external_memory.planes[0];
    if (plane.fd < 0 || !plane.allocation_size)
        return Error::from_string_literal("Vulkan RGBA render source has no opaque fd");

    auto try_render = [&](char const* label, int fd, VkExternalMemoryHandleTypeFlagBits handle_type) -> ErrorOr<u64> {
        if (fd < 0)
            return Error::from_string_literal("missing Vulkan RGBA render fd");

        auto source_fd = dup(fd);
        if (source_fd < 0)
            return Error::from_string_literal("failed to duplicate Vulkan RGBA render source fd");

        auto started_at = MonotonicTime::now();
        auto render_result = Gfx::render_vulkan_nv12_external_memory_to_opaque_rgba_image(
            m_skia_backend_context->vulkan_context(),
            source_fd,
            handle_type,
            plane.allocation_size,
            external_memory.size.width(),
            external_memory.size.height(),
            static_cast<VkFormat>(plane.vulkan_format),
            static_cast<VkImageLayout>(plane.vulkan_image_layout),
            rgba_texture.image,
            flip_y);
        auto render_microseconds = (MonotonicTime::now() - started_at).to_microseconds();

        if (render_result.is_error()) {
            dbgln("MUNDO_WEBGL_VIDEO_VULKAN_RGBA_RENDER count={} frame_id={} status=failed label={} reason={} render_us={} size={}x{} source_format={} source_layout={} source_allocation_size={} handle_type={} rgba_texture={} flip_y={}",
                log_count,
                external_memory.frame_id,
                label,
                render_result.error().string_literal(),
                render_microseconds,
                external_memory.size.width(),
                external_memory.size.height(),
                plane.vulkan_format,
                plane.vulkan_image_layout,
                plane.allocation_size,
                to_underlying(handle_type),
                rgba_texture.texture,
                flip_y);
            return render_result.release_error();
        }

        if (log_count <= 8 || log_count % 120 == 0 || render_microseconds > 5000) {
            dbgln("MUNDO_WEBGL_VIDEO_VULKAN_RGBA_RENDER count={} frame_id={} status=ok label={} render_us={} size={}x{} source_format={} source_layout={} source_allocation_size={} handle_type={} rgba_texture={} flip_y={}",
                log_count,
                external_memory.frame_id,
                label,
                render_microseconds,
                external_memory.size.width(),
                external_memory.size.height(),
                plane.vulkan_format,
                plane.vulkan_image_layout,
                plane.allocation_size,
                to_underlying(handle_type),
                rgba_texture.texture,
                flip_y);
        }
        return render_microseconds;
    };

    auto opaque_result = try_render("opaque_fd", plane.fd, VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT);
    if (!opaque_result.is_error())
        return opaque_result.release_value();

    auto dma_buf_result = try_render("dma_buf", plane.dma_buf_fd, VK_EXTERNAL_MEMORY_HANDLE_TYPE_DMA_BUF_BIT_EXT);
    if (!dma_buf_result.is_error())
        return dma_buf_result.release_value();

    return dma_buf_result.release_error();
}

void OpenGLContext::delete_imported_video_opaque_fd_texture(ImportedVideoOpaqueFDTexture& texture)
{
    auto* gl_delete_memory_objects_ext = reinterpret_cast<PFNGLDELETEMEMORYOBJECTSEXTPROC>(eglGetProcAddress("glDeleteMemoryObjectsEXT"));
    if (texture.owns_texture && texture.texture) {
        auto gl_texture = static_cast<GLuint>(texture.texture);
        glDeleteTextures(1, &gl_texture);
        texture.texture = 0;
    }
    if (texture.memory_object && gl_delete_memory_objects_ext) {
        auto memory_object = static_cast<GLuint>(texture.memory_object);
        gl_delete_memory_objects_ext(1, &memory_object);
        texture.memory_object = 0;
    }
}

void OpenGLContext::allocate_vkimage_painting_surface()
{
    VkFormat vulkan_format = VK_FORMAT_B8G8R8A8_UNORM;
    uint32_t drm_format = Gfx::vk_format_to_drm_format(vulkan_format);

    // Ensure that our format is supported by the implementation.
    // FIXME: try other formats if not?
    EGLint num_formats = 0;
    m_impl->ext_procs.query_dma_buf_formats(m_impl->display, 0, nullptr, &num_formats);
    Vector<EGLint> egl_formats;
    egl_formats.resize(num_formats);
    m_impl->ext_procs.query_dma_buf_formats(m_impl->display, num_formats, egl_formats.data(), &num_formats);
    VERIFY(egl_formats.find(drm_format) != egl_formats.end());

    EGLint num_modifiers = 0;
    m_impl->ext_procs.query_dma_buf_modifiers(m_impl->display, drm_format, 0, nullptr, nullptr, &num_modifiers);
    Vector<uint64_t> egl_modifiers;
    egl_modifiers.resize(num_modifiers);
    Vector<EGLBoolean> external_only;
    external_only.resize(num_modifiers);
    m_impl->ext_procs.query_dma_buf_modifiers(m_impl->display, drm_format, num_modifiers, egl_modifiers.data(), external_only.data(), &num_modifiers);
    Vector<uint64_t> renderable_modifiers;
    for (int i = 0; i < num_modifiers; ++i) {
        if (!external_only[i]) {
            renderable_modifiers.append(egl_modifiers[i]);
        }
    }
    static size_t s_dmabuf_caps_log_count { 0 };
    auto dmabuf_caps_log_count = ++s_dmabuf_caps_log_count;
    if (dmabuf_caps_log_count <= 8 || dmabuf_caps_log_count % 120 == 0) {
        dbgln("MUNDO_WEBGL_VULKAN_DMABUF_CAPS count={} size={}x{} drm_format={} egl_formats={} egl_modifiers={} renderable_modifiers={}",
            dmabuf_caps_log_count,
            m_size.width(),
            m_size.height(),
            drm_format,
            num_formats,
            num_modifiers,
            renderable_modifiers.size());
    }

    auto vulkan_image = MUST(Gfx::create_shared_vulkan_image(m_skia_backend_context->vulkan_context(), m_size.width(), m_size.height(), vulkan_format, renderable_modifiers));
    m_painting_surface = Gfx::PaintingSurface::create_from_vkimage(m_skia_backend_context, vulkan_image, Gfx::PaintingSurface::Origin::BottomLeft);
    auto dma_buf_fd = vulkan_image->get_dma_buf_fd();
    if (dmabuf_caps_log_count <= 8 || dmabuf_caps_log_count % 120 == 0) {
        dbgln("MUNDO_WEBGL_VULKAN_DMABUF_SURFACE count={} size={}x{} row_pitch={} modifier={} fd_valid={}",
            dmabuf_caps_log_count,
            m_size.width(),
            m_size.height(),
            vulkan_image->info.row_pitch,
            vulkan_image->info.modifier,
            dma_buf_fd >= 0);
    }
    probe_cuda_import_for_vulkan_dmabuf(dma_buf_fd, *vulkan_image, dmabuf_caps_log_count);
    probe_cuda_import_for_vulkan_opaque_fd(m_skia_backend_context->vulkan_context(), vulkan_format, dmabuf_caps_log_count);

    EGLAttrib attribs[] = {
        EGL_WIDTH,
        m_size.width(),
        EGL_HEIGHT,
        m_size.height(),
        EGL_LINUX_DRM_FOURCC_EXT,
        drm_format,
        EGL_DMA_BUF_PLANE0_FD_EXT,
        dma_buf_fd, // EGL takes ownership of the fd
        EGL_DMA_BUF_PLANE0_OFFSET_EXT,
        0,
        EGL_DMA_BUF_PLANE0_PITCH_EXT,
        static_cast<uint32_t>(vulkan_image->info.row_pitch),
        EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT,
        static_cast<uint32_t>(vulkan_image->info.modifier & 0xffffffff),
        EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT,
        static_cast<uint32_t>(vulkan_image->info.modifier >> 32),
        EGL_NONE,
    };
    m_impl->egl_image = eglCreateImage(m_impl->display, EGL_NO_CONTEXT, EGL_LINUX_DMA_BUF_EXT, nullptr, attribs);
    VERIFY(m_impl->egl_image != EGL_NO_IMAGE);

    m_impl->surface = EGL_NO_SURFACE;
    eglMakeCurrent(m_impl->display, m_impl->surface, m_impl->surface, m_impl->context);
    probe_gl_memory_object_fd_for_vulkan_opaque_fd(m_skia_backend_context->vulkan_context(), "paint_bgra8", 64, 64, vulkan_format, GL_RGBA8, dmabuf_caps_log_count);

    glGenTextures(1, &m_impl->color_buffer);
    glBindTexture(GL_TEXTURE_2D, m_impl->color_buffer);
    glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, m_impl->egl_image);

    glViewport(0, 0, m_size.width(), m_size.height());
}
#endif

void OpenGLContext::allocate_painting_surface_if_needed()
{
#ifdef ENABLE_WEBGL
    if (m_painting_surface)
        return;

    free_surface_resources();

    VERIFY(!m_size.is_empty());

#    if defined(AK_OS_MACOS)
    allocate_iosurface_painting_surface();
#    elif defined(USE_VULKAN_DMABUF_IMAGES)
    allocate_vkimage_painting_surface();
#    endif
    VERIFY(m_painting_surface);
    VERIFY(eglGetCurrentContext() == m_impl->context);

    glGenFramebuffers(1, &m_impl->framebuffer);
    glBindFramebuffer(GL_FRAMEBUFFER, m_impl->framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, m_impl->texture_target == EGL_TEXTURE_RECTANGLE_ANGLE ? GL_TEXTURE_RECTANGLE_ANGLE : GL_TEXTURE_2D, m_impl->color_buffer, 0);

    if (m_drawing_buffer_options.depth || m_drawing_buffer_options.stencil) {
        glGenRenderbuffers(1, &m_impl->depth_buffer);
        glBindRenderbuffer(GL_RENDERBUFFER, m_impl->depth_buffer);

        if (m_drawing_buffer_options.depth && m_drawing_buffer_options.stencil) {
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, m_size.width(), m_size.height());
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_impl->depth_buffer);
        } else if (m_drawing_buffer_options.depth) {
            glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH_COMPONENT24, m_size.width(), m_size.height());
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_RENDERBUFFER, m_impl->depth_buffer);
        } else {
            VERIFY(m_drawing_buffer_options.stencil);
            glRenderbufferStorage(GL_RENDERBUFFER, GL_STENCIL_INDEX8, m_size.width(), m_size.height());
            glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_impl->depth_buffer);
        }
    }

    VERIFY(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
#endif
}

void OpenGLContext::set_size(Gfx::IntSize const& size)
{
    if (m_size != size) {
#ifdef ENABLE_WEBGL
        free_surface_resources();
#endif
        m_painting_surface = nullptr;
    }
    m_size = size;
}

void OpenGLContext::make_current()
{
#ifdef ENABLE_WEBGL
    allocate_painting_surface_if_needed();
    eglMakeCurrent(m_impl->display, EGL_NO_SURFACE, EGL_NO_SURFACE, m_impl->context);
#endif
}

void OpenGLContext::note_gl_draw_submitted()
{
#ifdef USE_VULKAN_DMABUF_IMAGES
    ++m_impl->gl_draw_serial;
#endif
}

void OpenGLContext::note_direct_vulkan_video_draw_submitted()
{
#ifdef USE_VULKAN_DMABUF_IMAGES
    m_impl->direct_vulkan_video_draw_serial = ++m_impl->gl_draw_serial;
#endif
}

bool OpenGLContext::has_direct_vulkan_video_draw_pending_gl_present() const
{
#ifdef USE_VULKAN_DMABUF_IMAGES
    return m_impl->direct_vulkan_video_draw_serial > 0
        && m_impl->gl_draw_serial == m_impl->direct_vulkan_video_draw_serial;
#else
    return false;
#endif
}

void OpenGLContext::present(bool preserve_drawing_buffer)
{
#ifdef ENABLE_WEBGL
    make_current();

    // "Before the drawing buffer is presented for compositing the implementation shall ensure that all rendering operations have been flushed to the drawing buffer."
    // With Metal, glFlush flushes the command buffer, but without waiting for it to be scheduled or completed.
    // eglWaitUntilWorkScheduledANGLE flushes the command buffer, and waits until it has been scheduled, hence the name.
    // eglWaitUntilWorkScheduledANGLE only has an effect on CGL and Metal backends, so we only use it on macOS.
#    if defined(AK_OS_MACOS)
    eglWaitUntilWorkScheduledANGLE(m_impl->display);
#    elif defined(USE_VULKAN_DMABUF_IMAGES)
    // FIXME: CPU sync for now, but it would be better to export a fence and have Skia wait for it before reading from the surface.
    // MUNDO_WEBGL_PRESENT_SYNC_MODE lets the direct-video path validate lighter present synchronization without changing the default for non-direct video paths.
    auto direct_vulkan_video_was_last_draw = m_impl->direct_vulkan_video_draw_serial > 0
        && m_impl->gl_draw_serial == m_impl->direct_vulkan_video_draw_serial;
    auto direct_vulkan_mesh_present = m_impl->direct_vulkan_video_draw_serial > 0;
    auto present_sync_mode = direct_vulkan_mesh_present ? "auto"sv : "finish"sv;
    if (auto const* present_sync_mode_value = getenv("MUNDO_WEBGL_PRESENT_SYNC_MODE")) {
        auto value = StringView { present_sync_mode_value, strlen(present_sync_mode_value) };
        if (value == "auto"sv)
            present_sync_mode = "auto"sv;
        else if (value == "flush"sv)
            present_sync_mode = "flush"sv;
        else if (value == "none"sv)
            present_sync_mode = "none"sv;
        else if (value == "finish"sv)
            present_sync_mode = "finish"sv;
    }
    auto requested_present_sync_mode = present_sync_mode;
    if (present_sync_mode == "auto"sv)
        present_sync_mode = direct_vulkan_video_was_last_draw ? "none"sv : "flush"sv;

    auto present_sync_started_at = MonotonicTime::now();
    if (present_sync_mode == "flush"sv)
        glFlush();
    else if (present_sync_mode == "finish"sv)
        glFinish();
    auto present_sync_us = (MonotonicTime::now() - present_sync_started_at).to_microseconds();

    static size_t s_present_sync_log_count = 0;
    ++s_present_sync_log_count;
    if (s_present_sync_log_count <= 8 || s_present_sync_log_count % 120 == 0)
        dbgln("MUNDO_WEBGL_PRESENT_SYNC count={} requested_mode={} mode={} sync_us={} direct_vulkan_mesh_present={} direct_vulkan_video_was_last_draw={} gl_draw_serial={} direct_vulkan_video_draw_serial={} preserve_drawing_buffer={} next_step={}",
            s_present_sync_log_count,
            requested_present_sync_mode,
            present_sync_mode,
            present_sync_us,
            direct_vulkan_mesh_present,
            direct_vulkan_video_was_last_draw,
            m_impl->gl_draw_serial,
            m_impl->direct_vulkan_video_draw_serial,
            preserve_drawing_buffer,
            present_sync_mode == "finish"sv ? "test_flush_or_export_fence_for_gpu_only_present"sv : present_sync_mode == "none"sv ? "verify_pure_vulkan_video_present_without_gl_sync"sv
                                                                                                      : "verify_webgl_present_visual_integrity"sv);
#    endif

    // "By default, after compositing the contents of the drawing buffer shall be cleared to their default values, as shown in the table above.
    // This default behavior can be changed by setting the preserveDrawingBuffer attribute of the WebGLContextAttributes object.
    // If this flag is true, the contents of the drawing buffer shall be preserved until the author either clears or overwrites them."
    if (!preserve_drawing_buffer) {
        // FIXME: we're assuming the clear operation won't actually be submitted to the GPU
        clear_buffer_to_default_values();
    }
#else
    (void)preserve_drawing_buffer;
#endif
}

RefPtr<Gfx::PaintingSurface> OpenGLContext::surface()
{
    return m_painting_surface;
}

u32 OpenGLContext::default_renderbuffer() const
{
    return m_impl->depth_buffer;
}

u32 OpenGLContext::default_framebuffer() const
{
    return m_impl->framebuffer;
}

Vector<String> OpenGLContext::get_supported_opengl_extensions()
{
#ifdef ENABLE_WEBGL
    if (m_requestable_extensions.has_value())
        return m_requestable_extensions.value();

    make_current();

    Vector<String> extensions;

    auto const* extensions_string = reinterpret_cast<char const*>(glGetString(GL_EXTENSIONS));
    StringView extensions_view(extensions_string, strlen(extensions_string));
    for (auto extension : extensions_view.split_view(' ')) {
        extensions.append(MUST(String::from_utf8(extension)));
    }

    auto const* requestable_extensions_string = reinterpret_cast<char const*>(glGetString(GL_REQUESTABLE_EXTENSIONS_ANGLE));
    StringView requestable_extensions_view(requestable_extensions_string, strlen(requestable_extensions_string));
    for (auto extension : requestable_extensions_view.split_view(' ')) {
        extensions.append(MUST(String::from_utf8(extension)));
    }

    // We must cache this, because once extensions have been requested, they're no longer requestable extensions and would
    // not appear in this list. However, we must always report every supported extension, regardless of what has already
    // been requested.
    m_requestable_extensions = extensions;
    return extensions;
#else
    (void)m_webgl_version;
    return {};
#endif
}

void OpenGLContext::request_extension(char const* extension_name)
{
#ifdef ENABLE_WEBGL
    make_current();
    glRequestExtensionANGLE(extension_name);
#else
    (void)extension_name;
#endif
}

}
