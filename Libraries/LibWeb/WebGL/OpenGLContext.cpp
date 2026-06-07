/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/ScopeGuard.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImageBuffer.h>
#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <LibGfx/VulkanImage.h>
#endif
#include <LibWeb/WebGL/OpenGLContext.h>

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
#    include <unistd.h>
#endif

// Enable WebGL if we're on MacOS and can use Metal or if we can use shareable Vulkan images
#if defined(AK_OS_MACOS) || defined(USE_VULKAN_DMABUF_IMAGES)
#    define ENABLE_WEBGL 1
#endif

namespace Web::WebGL {

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
    struct {
        PFNEGLQUERYDMABUFFORMATSEXTPROC query_dma_buf_formats { nullptr };
        PFNEGLQUERYDMABUFMODIFIERSEXTPROC query_dma_buf_modifiers { nullptr };
    } ext_procs;
#endif
};

OpenGLContext::OpenGLContext(NonnullRefPtr<Gfx::SkiaBackendContext> skia_backend_context, Impl impl, WebGLVersion webgl_version, DrawingBufferOptions drawing_buffer_options)
    : m_skia_backend_context(move(skia_backend_context))
    , m_impl(make<Impl>(impl))
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

    auto log_failure = [&](StringView reason) {
        if (should_log) {
            dbgln("MUNDO_WEBGL_VIDEO_VULKAN_REPLAY_BUFFER_PROBE draw_count={} probe_count={} frame_id={} status=failed reason={} position_bytes={} uv_bytes={} uv_right_bytes={} index_bytes={} total_bytes={} next_step=fix_shadowed_buffers_before_vulkan_replay",
                log_count,
                probe_count,
                frame_id,
                reason,
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

    if (should_log) {
        dbgln("MUNDO_WEBGL_VIDEO_VULKAN_REPLAY_BUFFER_PROBE draw_count={} probe_count={} frame_id={} status=ok position_buffer={} uv_buffer={} uv_right_buffer={} index_buffer={} position_bytes={} uv_bytes={} uv_right_bytes={} index_bytes={} total_bytes={} next_step=persist_replay_buffers_and_build_vulkan_pipeline",
            log_count,
            probe_count,
            frame_id,
            reinterpret_cast<uintptr_t>(position_buffer->buffer),
            reinterpret_cast<uintptr_t>(uv_buffer->buffer),
            uv_right_buffer ? reinterpret_cast<uintptr_t>(uv_right_buffer->buffer) : 0,
            reinterpret_cast<uintptr_t>(index_buffer->buffer),
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

    auto reused = m_impl->cached_video_rgba_target_texture.has_value() && texture_matches(m_impl->cached_video_rgba_target_texture.value(), target_texture, width, height);
    if (!reused) {
        if (m_impl->cached_video_rgba_target_texture.has_value())
            delete_imported_video_opaque_fd_texture(m_impl->cached_video_rgba_target_texture.value());

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
    // FIXME: CPU sync for now, but it would be better to export a fence and have Skia wait for it before reading from the surface
    glFinish();
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
