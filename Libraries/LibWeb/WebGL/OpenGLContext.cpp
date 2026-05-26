/*
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/HashMap.h>
#include <AK/OwnPtr.h>
#include <AK/String.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SharedImageBuffer.h>
#ifdef USE_VULKAN_DMABUF_IMAGES
#    include <LibGfx/VulkanImage.h>
#endif
#include <LibWeb/WebGL/OpenGLContext.h>

#include <EGL/egl.h>
#include <EGL/eglext.h>
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
