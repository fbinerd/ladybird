/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
extern "C" {
#include <GLES2/gl2ext_angle.h>
}

#include <AK/NonnullOwnPtr.h>
#include <AK/Time.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/SkiaUtils.h>
#include <LibMedia/VideoFrame.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibWeb/HTML/HTMLCanvasElement.h>
#include <LibWeb/HTML/HTMLImageElement.h>
#include <LibWeb/HTML/HTMLVideoElement.h>
#include <LibWeb/HTML/ImageBitmap.h>
#include <LibWeb/HTML/ImageData.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/UniversalGlobalScope.h>
#include <LibWeb/Platform/EventLoopPlugin.h>
#include <LibWeb/WebGL/Extensions/ANGLEInstancedArrays.h>
#include <LibWeb/WebGL/Extensions/EXTBlendMinMax.h>
#include <LibWeb/WebGL/Extensions/EXTColorBufferFloat.h>
#include <LibWeb/WebGL/Extensions/EXTRenderSnorm.h>
#include <LibWeb/WebGL/Extensions/EXTTextureFilterAnisotropic.h>
#include <LibWeb/WebGL/Extensions/EXTTextureNorm16.h>
#include <LibWeb/WebGL/Extensions/OESElementIndexUint.h>
#include <LibWeb/WebGL/Extensions/OESStandardDerivatives.h>
#include <LibWeb/WebGL/Extensions/OESVertexArrayObject.h>
#include <LibWeb/WebGL/Extensions/WebGLCompressedTextureS3tc.h>
#include <LibWeb/WebGL/Extensions/WebGLCompressedTextureS3tcSrgb.h>
#include <LibWeb/WebGL/Extensions/WebGLDebugRendererInfo.h>
#include <LibWeb/WebGL/Extensions/WebGLDrawBuffers.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLRenderingContextBase.h>

#include <core/SkCanvas.h>
#include <core/SkColorSpace.h>
#include <core/SkColorType.h>
#include <core/SkImage.h>
#include <core/SkPixmap.h>
#include <core/SkSurface.h>
#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#    include <dlfcn.h>
#endif

#ifndef GL_PIXEL_UNPACK_BUFFER
#    define GL_PIXEL_UNPACK_BUFFER GL_PIXEL_UNPACK_BUFFER_NV
#endif
#ifndef GL_PIXEL_UNPACK_BUFFER_BINDING
#    define GL_PIXEL_UNPACK_BUFFER_BINDING GL_PIXEL_UNPACK_BUFFER_BINDING_NV
#endif
#ifndef GL_RED_EXT
#    define GL_RED_EXT 0x1903
#endif
#ifndef GL_RG_EXT
#    define GL_RG_EXT 0x8227
#endif
#ifndef GL_R8_EXT
#    define GL_R8_EXT 0x8229
#endif
#ifndef GL_RG8_EXT
#    define GL_RG8_EXT 0x822B
#endif
#ifndef GL_BUFFER_SIZE
#    define GL_BUFFER_SIZE 0x8764
#endif

namespace Web::WebGL {

static bool should_log_mundo_webgl_texture_diagnostic(size_t count)
{
    return count <= 12 || count % 120 == 0;
}

static bool mundo_webgl_video_pbo_upload_enabled()
{
    auto const* raw_value = getenv("MUNDO_WEBGL_VIDEO_PBO_UPLOAD");
    if (!raw_value)
        return true;

    return raw_value[0] != '\0' && strcmp(raw_value, "0") && strcmp(raw_value, "false") && strcmp(raw_value, "no") && strcmp(raw_value, "off");
}

static bool mundo_webgl_video_pbo_orphan_each_upload_enabled()
{
    auto const* raw_value = getenv("MUNDO_WEBGL_VIDEO_PBO_ORPHAN_EACH_UPLOAD");
    if (!raw_value)
        return false;

    return raw_value[0] != '\0' && strcmp(raw_value, "0") && strcmp(raw_value, "false") && strcmp(raw_value, "no") && strcmp(raw_value, "off");
}

static bool mundo_webgl_video_direct_bitmap_upload_enabled()
{
    auto const* raw_value = getenv("MUNDO_WEBGL_VIDEO_DIRECT_BITMAP_UPLOAD");
    if (!raw_value)
        return true;

    return raw_value[0] != '\0' && strcmp(raw_value, "0") && strcmp(raw_value, "false") && strcmp(raw_value, "no") && strcmp(raw_value, "off");
}

static bool mundo_webgl_video_nv12_shader_upload_enabled()
{
    auto const* raw_value = getenv("MUNDO_WEBGL_VIDEO_NV12_SHADER_UPLOAD");
    if (!raw_value)
        return true;

    return raw_value[0] != '\0' && strcmp(raw_value, "0") && strcmp(raw_value, "false") && strcmp(raw_value, "no") && strcmp(raw_value, "off");
}

static bool mundo_webgl_video_nv12_shader_black_probe_enabled()
{
    auto const* raw_value = getenv("MUNDO_WEBGL_VIDEO_NV12_SHADER_BLACK_PROBE");
    if (!raw_value)
        return false;

    return raw_value[0] != '\0' && strcmp(raw_value, "0") && strcmp(raw_value, "false") && strcmp(raw_value, "no") && strcmp(raw_value, "off");
}

enum class MundoWebGLVideoCudaUploadMode {
    Texture,
    PBO,
};

static MundoWebGLVideoCudaUploadMode mundo_webgl_video_cuda_upload_mode()
{
    auto const* raw_value = getenv("MUNDO_WEBGL_VIDEO_CUDA_UPLOAD_MODE");
    if (!raw_value || raw_value[0] == '\0')
        return MundoWebGLVideoCudaUploadMode::Texture;
    if (!strcmp(raw_value, "pbo") || !strcmp(raw_value, "buffer"))
        return MundoWebGLVideoCudaUploadMode::PBO;
    return MundoWebGLVideoCudaUploadMode::Texture;
}

static bool mundo_webgl_cuda_gl_interop_probe_enabled()
{
    auto const* raw_value = getenv("MUNDO_WEBGL_CUDA_GL_INTEROP_PROBE");
    if (!raw_value)
        return true;

    return raw_value[0] != '\0' && strcmp(raw_value, "0") && strcmp(raw_value, "false") && strcmp(raw_value, "no") && strcmp(raw_value, "off");
}

static void probe_mundo_cuda_gl_interop(size_t attempt_count)
{
#if defined(__linux__)
    if (!mundo_webgl_cuda_gl_interop_probe_enabled())
        return;

    static size_t s_probe_count { 0 };
    auto probe_count = ++s_probe_count;
    if (probe_count != 1 && probe_count % 720 != 0)
        return;

    using CUresult = int;
    using CUdevice = int;
    using CuInit = CUresult (*)(unsigned int);
    using CuDriverGetVersion = CUresult (*)(int*);
    using CuDeviceGetCount = CUresult (*)(int*);
    using CuGLGetDevices = CUresult (*)(unsigned int*, CUdevice*, unsigned int, unsigned int);

    static void* s_cuda_library { nullptr };
    static bool s_cuda_library_load_attempted { false };
    if (!s_cuda_library_load_attempted) {
        s_cuda_library_load_attempted = true;
        s_cuda_library = dlopen("libcuda.so.1", RTLD_LAZY);
    }
    if (!s_cuda_library) {
        dbgln("MUNDO_WEBGL_CUDA_GL_INTEROP_PROBE attempt={} probe={} status=unavailable reason=libcuda_load_failed", attempt_count, probe_count);
        return;
    }

    auto* cu_init = reinterpret_cast<CuInit>(dlsym(s_cuda_library, "cuInit"));
    auto* cu_driver_get_version = reinterpret_cast<CuDriverGetVersion>(dlsym(s_cuda_library, "cuDriverGetVersion"));
    auto* cu_device_get_count = reinterpret_cast<CuDeviceGetCount>(dlsym(s_cuda_library, "cuDeviceGetCount"));
    auto* cu_gl_get_devices = reinterpret_cast<CuGLGetDevices>(dlsym(s_cuda_library, "cuGLGetDevices_v2"));
    if (!cu_gl_get_devices)
        cu_gl_get_devices = reinterpret_cast<CuGLGetDevices>(dlsym(s_cuda_library, "cuGLGetDevices"));

    if (!cu_init || !cu_driver_get_version || !cu_device_get_count || !cu_gl_get_devices) {
        dbgln("MUNDO_WEBGL_CUDA_GL_INTEROP_PROBE attempt={} probe={} status=unavailable reason=missing_symbol cuInit={} cuDriverGetVersion={} cuDeviceGetCount={} cuGLGetDevices={}",
            attempt_count,
            probe_count,
            cu_init != nullptr,
            cu_driver_get_version != nullptr,
            cu_device_get_count != nullptr,
            cu_gl_get_devices != nullptr);
        return;
    }

    auto init_result = cu_init(0);
    int driver_version = 0;
    auto driver_result = cu_driver_get_version(&driver_version);
    int device_count = 0;
    auto device_count_result = cu_device_get_count(&device_count);

    CUdevice all_devices[8] {};
    unsigned int gl_all_count = 0;
    auto gl_all_result = cu_gl_get_devices(&gl_all_count, all_devices, 8, 1);

    CUdevice current_devices[8] {};
    unsigned int gl_current_count = 0;
    auto gl_current_result = cu_gl_get_devices(&gl_current_count, current_devices, 8, 2);

    dbgln("MUNDO_WEBGL_CUDA_GL_INTEROP_PROBE attempt={} probe={} status=probed init_result={} driver_result={} driver_version={} device_count_result={} device_count={} gl_all_result={} gl_all_count={} gl_all_device0={} gl_current_result={} gl_current_count={} gl_current_device0={}",
        attempt_count,
        probe_count,
        init_result,
        driver_result,
        driver_version,
        device_count_result,
        device_count,
        gl_all_result,
        gl_all_count,
        gl_all_count > 0 ? all_devices[0] : -1,
        gl_current_result,
        gl_current_count,
        gl_current_count > 0 ? current_devices[0] : -1);
#else
    (void)attempt_count;
#endif
}

static constexpr Optional<Gfx::ExportFormat> determine_export_format(WebIDL::UnsignedLong format, WebIDL::UnsignedLong type)
{
    switch (format) {
    case GL_RGB:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return Gfx::ExportFormat::RGB888;
        case GL_UNSIGNED_SHORT_5_6_5:
            return Gfx::ExportFormat::RGB565;
        default:
            break;
        }
        break;
    case GL_RGBA:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return Gfx::ExportFormat::RGBA8888;
        case GL_UNSIGNED_SHORT_4_4_4_4:
            // FIXME: This is not exactly the same as RGBA.
            return Gfx::ExportFormat::RGBA4444;
        case GL_UNSIGNED_SHORT_5_5_5_1:
            return Gfx::ExportFormat::RGBA5551;
            break;
        default:
            break;
        }
        break;
    case GL_ALPHA:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return Gfx::ExportFormat::Alpha8;
        default:
            break;
        }
        break;
    case GL_LUMINANCE:
        switch (type) {
        case GL_UNSIGNED_BYTE:
            return Gfx::ExportFormat::Gray8;
        default:
            break;
        }
        break;
    default:
        break;
    }

    dbgln("WebGL: Unsupported format and type combination. format: 0x{:04x}, type: 0x{:04x}", format, type);
    return {};
}

WebGLRenderingContextBase::WebGLRenderingContextBase(JS::Realm& realm)
    : Bindings::PlatformObject(realm)
{
}

struct Extension {
    Vector<StringView> required_angle_extensions;
    JS::ThrowCompletionOr<GC::Ref<JS::Object>> (*factory)(JS::Realm&, GC::Ref<WebGLRenderingContextBase>);
    Optional<OpenGLContext::WebGLVersion> only_for_webgl_version { OptionalNone {} };
};

static HashMap<String, Extension, AK::ASCIICaseInsensitiveStringTraits> s_available_webgl_extensions {
    // Khronos ratified WebGL Extensions
    { "ANGLE_instanced_arrays"_string, { { "GL_ANGLE_instanced_arrays"sv }, ANGLEInstancedArrays::create, OpenGLContext::WebGLVersion::WebGL1 } },
    { "EXT_blend_minmax"_string, { { "GL_EXT_blend_minmax"sv }, EXTBlendMinMax::create, OpenGLContext::WebGLVersion::WebGL1 } },
    { "EXT_frag_depth"_string, { { "GL_EXT_frag_depth"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL1 } },
    { "EXT_shader_texture_lod"_string, { { "GL_EXT_shader_texture_lod"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL1 } },
    { "EXT_texture_filter_anisotropic"_string, { { "GL_EXT_texture_filter_anisotropic"sv }, EXTTextureFilterAnisotropic::create } },
    { "OES_element_index_uint"_string, { { "GL_OES_element_index_uint"sv }, OESElementIndexUint::create, OpenGLContext::WebGLVersion::WebGL1 } },
    { "OES_standard_derivatives"_string, { { "GL_OES_standard_derivatives"sv }, OESStandardDerivatives::create, OpenGLContext::WebGLVersion::WebGL1 } },
    { "OES_texture_float"_string, { { "GL_OES_texture_float"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL1 } },
    { "OES_texture_float_linear"_string, { { "GL_OES_texture_float_linear"sv }, nullptr } },
    { "OES_texture_half_float"_string, { { "GL_OES_texture_half_float"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL1 } },
    { "OES_texture_half_float_linear"_string, { { "GL_OES_texture_half_float_linear"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL1 } },
    { "OES_vertex_array_object"_string, { { "GL_OES_vertex_array_object"sv }, OESVertexArrayObject::create, OpenGLContext::WebGLVersion::WebGL1 } },
    { "WEBGL_compressed_texture_s3tc"_string, { { "GL_EXT_texture_compression_dxt1"sv, "GL_ANGLE_texture_compression_dxt3"sv, "GL_ANGLE_texture_compression_dxt5"sv }, WebGLCompressedTextureS3tc::create } },
    { "WEBGL_debug_renderer_info"_string, { {}, WebGLDebugRendererInfo::create } },
    { "WEBGL_debug_shaders"_string, { {}, nullptr } },
    { "WEBGL_depth_texture"_string, { { "GL_ANGLE_depth_texture"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL1 } },
    { "WEBGL_draw_buffers"_string, { { "GL_EXT_draw_buffers"sv }, WebGLDrawBuffers::create, OpenGLContext::WebGLVersion::WebGL1 } },
    { "WEBGL_lose_context"_string, { {}, nullptr } },

    // Community approved WebGL Extensions
    { "EXT_clip_control"_string, { { "GL_EXT_clip_control"sv }, nullptr } },
    { "EXT_color_buffer_float"_string, { { "GL_EXT_color_buffer_float"sv }, EXTColorBufferFloat::create, OpenGLContext::WebGLVersion::WebGL2 } },
    { "EXT_color_buffer_half_float"_string, { { "GL_EXT_color_buffer_half_float"sv }, nullptr } },
    { "EXT_conservative_depth"_string, { { "GL_EXT_conservative_depth"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL2 } },
    { "EXT_depth_clamp"_string, { { "GL_EXT_depth_clamp"sv }, nullptr } },
    { "EXT_disjoint_timer_query"_string, { { "GL_EXT_disjoint_timer_query"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL1 } },
    { "EXT_disjoint_timer_query_webgl2"_string, { { "GL_EXT_disjoint_timer_query"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL2 } },
    { "EXT_float_blend"_string, { { "GL_EXT_float_blend"sv }, nullptr } },
    { "EXT_polygon_offset_clamp"_string, { { "GL_EXT_polygon_offset_clamp"sv }, nullptr } },
    { "EXT_render_snorm"_string, { { "GL_EXT_render_snorm"sv }, EXTRenderSnorm::create, OpenGLContext::WebGLVersion::WebGL2 } },
    { "EXT_sRGB"_string, { { "GL_EXT_sRGB"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL1 } },
    { "EXT_texture_compression_bptc"_string, { { "GL_EXT_texture_compression_bptc"sv }, nullptr } },
    { "EXT_texture_compression_rgtc"_string, { { "GL_EXT_texture_compression_rgtc"sv }, nullptr } },
    { "EXT_texture_mirror_clamp_to_edge"_string, { { "GL_EXT_texture_mirror_clamp_to_edge"sv }, nullptr } },
    { "EXT_texture_norm16"_string, { { "GL_EXT_texture_norm16"sv }, EXTTextureNorm16::create, OpenGLContext::WebGLVersion::WebGL2 } },
    { "KHR_parallel_shader_compile"_string, { { "GL_KHR_parallel_shader_compile"sv }, nullptr } },
    { "NV_shader_noperspective_interpolation"_string, { { "GL_NV_shader_noperspective_interpolation"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL2 } },
    { "OES_draw_buffers_indexed"_string, { { "GL_OES_draw_buffers_indexed"sv }, nullptr } },
    { "OES_fbo_render_mipmap"_string, { { "GL_OES_fbo_render_mipmap"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL1 } },
    { "OES_sample_variables"_string, { { "GL_OES_sample_variables"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL2 } },
    { "OES_shader_multisample_interpolation"_string, { { "GL_OES_shader_multisample_interpolation"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL2 } },
    { "OVR_multiview2"_string, { { "GL_OVR_multiview2"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL2 } },
    { "WEBGL_blend_func_extended"_string, { { "GL_EXT_blend_func_extended"sv }, nullptr } },
    { "WEBGL_clip_cull_distance"_string, { { "GL_EXT_clip_cull_distance"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL2 } },
    { "WEBGL_color_buffer_float"_string, { { "EXT_color_buffer_half_float"sv, "OES_texture_float"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL1 } },
    { "WEBGL_compressed_texture_astc"_string, { { "KHR_texture_compression_astc_hdr"sv, "KHR_texture_compression_astc_ldr"sv }, nullptr } },
    { "WEBGL_compressed_texture_etc"_string, { { "GL_ANGLE_compressed_texture_etc"sv }, nullptr } },
    { "WEBGL_compressed_texture_etc1"_string, { { "GL_OES_compressed_ETC1_RGB8_texture"sv }, nullptr } },
    { "WEBGL_compressed_texture_pvrtc"_string, { { "GL_IMG_texture_compression_pvrtc"sv }, nullptr } },
    { "WEBGL_compressed_texture_s3tc_srgb"_string, { { "GL_EXT_texture_compression_s3tc_srgb"sv }, WebGLCompressedTextureS3tcSrgb::create } },
    { "WEBGL_multi_draw"_string, { { "GL_ANGLE_multi_draw"sv }, nullptr } },
    { "WEBGL_polygon_mode"_string, { { "GL_ANGLE_polygon_mode"sv }, nullptr } },
    { "WEBGL_provoking_vertex"_string, { { "GL_ANGLE_provoking_vertex"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL2 } },
    { "WEBGL_render_shared_exponent"_string, { { "GL_QCOM_render_shared_exponent"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL2 } },
    { "WEBGL_stencil_texturing"_string, { { "GL_ANGLE_stencil_texturing"sv }, nullptr, OpenGLContext::WebGLVersion::WebGL2 } },
};

Optional<Vector<String>> WebGLRenderingContextBase::get_supported_extensions()
{
    auto opengl_extensions = context().get_supported_opengl_extensions();
    Vector<String> webgl_extensions;

    for (auto const& [available_extension_name, available_extension_info] : s_available_webgl_extensions) {
        bool supported = !available_extension_info.only_for_webgl_version.has_value()
            || context().webgl_version() == available_extension_info.only_for_webgl_version;

        if (!available_extension_info.factory && !HTML::UniversalGlobalScopeMixin::expose_experimental_interfaces()) {
            supported = false;
        }

        if (supported) {
            for (auto const& required_extension : available_extension_info.required_angle_extensions) {
                if (!opengl_extensions.contains_slow(required_extension)) {
                    supported = false;
                    break;
                }
            }
        }

        if (supported)
            webgl_extensions.append(available_extension_name);
    }

    return webgl_extensions;
}

JS::Object* WebGLRenderingContextBase::get_extension(String const& name)
{
    // Returns an object if, and only if, name is an ASCII case-insensitive match [HTML] for one of the names returned
    // from getSupportedExtensions; otherwise, returns null. The object returned from getExtension contains any constants
    // or functions provided by the extension. A returned object may have no constants or functions if the extension does
    // not define any, but a unique object must still be returned. That object is used to indicate that the extension has
    // been enabled.
    auto supported_extensions = get_supported_extensions();
    auto supported_extension_iterator = supported_extensions->find_if([&name](String const& supported_extension) {
        return supported_extension.equals_ignoring_ascii_case(name);
    });
    if (supported_extension_iterator == supported_extensions->end())
        return nullptr;

    auto maybe_extension = m_enabled_extensions.get(name);
    if (maybe_extension.has_value())
        return maybe_extension.release_value();

    // If we pass the check above this will always return a value
    auto const& extension_info = s_available_webgl_extensions.get(name).release_value();

    if (!extension_info.factory)
        return nullptr;

    for (auto const& required_extension : extension_info.required_angle_extensions) {
        context().request_extension(null_terminated_string(required_extension).data());
    }

    auto extension = MUST(extension_info.factory(realm(), *this));
    m_enabled_extensions.set(name, extension);
    return extension;
}

void WebGLRenderingContextBase::enable_compressed_texture_format(WebIDL::UnsignedLong format)
{
    m_enabled_compressed_texture_formats.append(format);
}

void WebGLRenderingContextBase::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_enabled_extensions);
}

bool WebGLRenderingContextBase::extension_enabled(StringView extension) const
{
    return m_enabled_extensions.contains(MUST(String::from_utf8(extension)));
}

ReadonlySpan<WebIDL::UnsignedLong> WebGLRenderingContextBase::enabled_compressed_texture_formats() const
{
    return m_enabled_compressed_texture_formats;
}

Optional<Gfx::BitmapExportResult> WebGLRenderingContextBase::read_and_pixel_convert_texture_image_source(TexImageSource const& source, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, Optional<int> destination_width, Optional<int> destination_height)
{
    // FIXME: If this function is called with an ImageData whose data attribute has been neutered,
    //        an INVALID_VALUE error is generated.
    // FIXME: If this function is called with an ImageBitmap that has been neutered, an INVALID_VALUE
    //        error is generated.
    // FIXME: If this function is called with an HTMLImageElement or HTMLVideoElement whose origin
    //        differs from the origin of the containing Document, or with an HTMLCanvasElement,
    //        ImageBitmap or OffscreenCanvas whose bitmap's origin-clean flag is set to false,
    //        a SECURITY_ERR exception must be thrown. See Origin Restrictions.
    // FIXME: If source is null then an INVALID_VALUE error is generated.
    static size_t s_texture_source_read_count { 0 };
    auto texture_source_read_count = ++s_texture_source_read_count;
    auto should_log_texture_source = should_log_mundo_webgl_texture_diagnostic(texture_source_read_count);
    StringView texture_source_name = "Unknown"sv;
    auto log_webgl_source = [&](StringView source_name, Gfx::Bitmap const* bitmap) {
        if (!should_log_texture_source)
            return;
        if (!bitmap) {
            dbgln("MUNDO_WEBGL_TEX_SOURCE attempt={} type={} bitmap=null format={} type_enum={} dest={}x{} flip_y={} premultiply={}",
                texture_source_read_count,
                source_name,
                format,
                type,
                destination_width.value_or(-1),
                destination_height.value_or(-1),
                m_unpack_flip_y,
                m_unpack_premultiply_alpha);
            return;
        }
        dbgln("MUNDO_WEBGL_TEX_SOURCE attempt={} type={} bitmap={} size={}x{} pitch={} data_size={} format={} alpha={} format_enum={} type_enum={} dest={}x{} flip_y={} premultiply={}",
            texture_source_read_count,
            source_name,
            static_cast<void const*>(bitmap),
            bitmap->width(),
            bitmap->height(),
            bitmap->pitch(),
            bitmap->data_size(),
            Gfx::bitmap_format_name(bitmap->format()),
            bitmap->alpha_type() == Gfx::AlphaType::Premultiplied ? "premultiplied"sv : "unpremultiplied"sv,
            format,
            type,
            destination_width.value_or(-1),
            destination_height.value_or(-1),
            m_unpack_flip_y,
            m_unpack_premultiply_alpha);
    };

    auto bitmap = source.visit(
        [&](GC::Root<HTML::HTMLImageElement> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            texture_source_name = "HTMLImageElement"sv;
            return source->immutable_bitmap();
        },
        [&](GC::Root<HTML::HTMLCanvasElement> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            texture_source_name = "HTMLCanvasElement"sv;
            auto surface = source->surface();
            if (!surface)
                return Gfx::ImmutableBitmap::create(*source->get_bitmap_from_surface());
            return Gfx::ImmutableBitmap::create_snapshot_from_painting_surface(*surface);
        },
        [&](GC::Root<HTML::OffscreenCanvas> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            texture_source_name = "OffscreenCanvas"sv;
            auto bitmap = source->bitmap();
            if (!bitmap)
                return {};
            log_webgl_source("OffscreenCanvas"sv, bitmap.ptr());
            return Gfx::ImmutableBitmap::create(*bitmap);
        },
        [&](GC::Root<HTML::HTMLVideoElement> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            texture_source_name = "HTMLVideoElement"sv;
            auto bitmap = source->bitmap();
            if (should_log_texture_source) {
                dbgln("MUNDO_WEBGL_TEX_SOURCE attempt={} type=HTMLVideoElement current_src={} ready_state={} bitmap={} size={}x{}",
                    texture_source_read_count,
                    source->current_src(),
                    to_underlying(source->ready_state()),
                    static_cast<void const*>(bitmap.ptr()),
                    bitmap ? bitmap->size().width() : 0,
                    bitmap ? bitmap->size().height() : 0);
            }
            return bitmap;
        },
        [&](GC::Root<HTML::ImageBitmap> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            texture_source_name = "ImageBitmap"sv;
            auto* bitmap = source->bitmap();
            if (!bitmap)
                return {};
            log_webgl_source("ImageBitmap"sv, bitmap);
            if (bitmap->width() <= 0 || bitmap->height() <= 0) {
                dbgln("MUNDO_WEBGL_TEX_SOURCE attempt={} type=ImageBitmap upload rejected empty width={} height={}", texture_source_read_count, source->width(), source->height());
                return {};
            }
            auto buffer = ByteBuffer::copy(bitmap->scanline_u8(0), bitmap->data_size());
            if (buffer.is_error()) {
                dbgln("MUNDO_WEBGL_TEX_SOURCE attempt={} type=ImageBitmap upload rejected copy failed width={} height={} error={}", texture_source_read_count, source->width(), source->height(), buffer.error());
                return {};
            }
            auto snapshot_buffer = buffer.release_value();
            auto snapshot_storage = try_make<ByteBuffer>(move(snapshot_buffer));
            if (snapshot_storage.is_error()) {
                dbgln("MUNDO_WEBGL_TEX_SOURCE attempt={} type=ImageBitmap upload rejected snapshot allocation failed width={} height={} error={}", texture_source_read_count, source->width(), source->height(), snapshot_storage.error());
                return {};
            }
            auto storage = snapshot_storage.release_value();
            auto* snapshot_pixels = storage->data();
            auto snapshot = Gfx::Bitmap::create_wrapper(bitmap->format(), bitmap->alpha_type(), bitmap->size(), bitmap->pitch(), snapshot_pixels, [storage = move(storage)] { });
            if (snapshot.is_error()) {
                dbgln("MUNDO_WEBGL_TEX_SOURCE attempt={} type=ImageBitmap upload rejected wrapper failed width={} height={} error={}", texture_source_read_count, source->width(), source->height(), snapshot.error());
                return {};
            }
            if (should_log_texture_source) {
                dbgln("MUNDO_WEBGL_TEX_SOURCE attempt={} type=ImageBitmap upload snapshot bitmap={} size={}x{} pitch={}",
                    texture_source_read_count,
                    static_cast<void const*>(snapshot.value().ptr()),
                    snapshot.value()->width(),
                    snapshot.value()->height(),
                    snapshot.value()->pitch());
            }
            return Gfx::ImmutableBitmap::create(snapshot.release_value());
        },
        [&](GC::Root<HTML::ImageData> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            texture_source_name = "ImageData"sv;
            log_webgl_source("ImageData"sv, &source->bitmap());
            return Gfx::ImmutableBitmap::create(source->bitmap());
        });
    if (!bitmap)
        return OptionalNone {};

    auto export_format = determine_export_format(format, type);
    if (!export_format.has_value())
        return OptionalNone {};

    // FIXME: Respect unpackColorSpace
    auto export_flags = 0;
    if (m_unpack_flip_y && !source.has<GC::Root<HTML::ImageBitmap>>())
        // The first pixel transferred from the source to the WebGL implementation corresponds to the upper left corner of
        // the source. This behavior is modified by the UNPACK_FLIP_Y_WEBGL pixel storage parameter, except for ImageBitmap
        // arguments, as described in the abovementioned section.
        export_flags |= Gfx::ExportFlags::FlipY;
    if (m_unpack_premultiply_alpha)
        export_flags |= Gfx::ExportFlags::PremultiplyAlpha;

    auto export_start = MonotonicTime::now();
    auto result = bitmap->export_to_byte_buffer(export_format.value(), export_flags, destination_width, destination_height);
    auto export_microseconds = (MonotonicTime::now() - export_start).to_microseconds();
    if (result.is_error()) {
        dbgln("Could not export bitmap: {}", result.release_error());
        return OptionalNone {};
    }

    auto value = result.release_value();
    if (should_log_texture_source) {
        dbgln("MUNDO_WEBGL_TEX_EXPORT attempt={} type={} export_us={} source_size={}x{} output_size={}x{} output_bytes={}",
            texture_source_read_count,
            texture_source_name,
            export_microseconds,
            bitmap->width(),
            bitmap->height(),
            value.width,
            value.height,
            value.buffer.size());
    }

    return value;
}

bool WebGLRenderingContextBase::texture_source_is_video_with_nv12_frame(TexImageSource const& source) const
{
    if (!source.has<GC::Root<HTML::HTMLVideoElement>>())
        return false;
    auto const& video = source.get<GC::Root<HTML::HTMLVideoElement>>();
    auto const* media_frame = video->current_media_frame();
    if (!media_frame)
        return false;
    if (media_frame->nv12_data())
        return true;
    auto const& hardware_descriptor = media_frame->hardware_descriptor();
    return media_frame->hardware_handle() && hardware_descriptor.has_value() && hardware_descriptor->zero_copy_capable;
}

bool WebGLRenderingContextBase::upload_texture_source_with_video_pbo(TexImageSource const& source, WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, Gfx::BitmapExportResult const& converted_texture, bool is_sub_image)
{
    if (!mundo_webgl_video_pbo_upload_enabled())
        return false;
    if (!source.has<GC::Root<HTML::HTMLVideoElement>>())
        return false;
    if (format != GL_RGBA || type != GL_UNSIGNED_BYTE)
        return false;
    if (converted_texture.buffer.is_empty() || converted_texture.width <= 0 || converted_texture.height <= 0)
        return false;

    auto upload_start = MonotonicTime::now();

    auto pbo_index = m_mundo_video_upload_pbo_index++ % 3;
    auto& pbo = m_mundo_video_upload_pbos[pbo_index];
    auto& pbo_size = m_mundo_video_upload_pbo_sizes[pbo_index];

    if (!pbo)
        glGenBuffers(1, &pbo);
    if (!pbo)
        return false;

    GLint previous_unpack_buffer = 0;
    glGetIntegervRobustANGLE(GL_PIXEL_UNPACK_BUFFER_BINDING, 1, nullptr, &previous_unpack_buffer);

    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
    if (pbo_size < converted_texture.buffer.size()) {
        glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(converted_texture.buffer.size()), nullptr, GL_STREAM_DRAW);
        pbo_size = converted_texture.buffer.size();
    }

    glBufferSubData(GL_PIXEL_UNPACK_BUFFER, 0, static_cast<GLsizeiptr>(converted_texture.buffer.size()), converted_texture.buffer.data());
    if (is_sub_image) {
        glTexSubImage2D(target, level, xoffset, yoffset, converted_texture.width, converted_texture.height, format, type, nullptr);
    } else {
        glTexImage2D(target, level, internalformat, converted_texture.width, converted_texture.height, border, format, type, nullptr);
    }
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, previous_unpack_buffer);

    auto upload_microseconds = (MonotonicTime::now() - upload_start).to_microseconds();
    static size_t s_video_pbo_upload_count { 0 };
    auto upload_count = ++s_video_pbo_upload_count;
    if (should_log_mundo_webgl_texture_diagnostic(upload_count)) {
        dbgln("MUNDO_WEBGL_VIDEO_PBO_UPLOAD attempt={} kind={} upload_us={} size={}x{} bytes={} pbo_slot={} pbo_size={}",
            upload_count,
            is_sub_image ? "texSubImage2D"sv : "texImage2D"sv,
            upload_microseconds,
            converted_texture.width,
            converted_texture.height,
            converted_texture.buffer.size(),
            pbo_index,
            pbo_size);
    }
    return true;
}

static GLuint compile_mundo_video_nv12_shader(GLenum type, StringView source)
{
    auto shader = glCreateShader(type);
    if (!shader)
        return 0;

    Vector<GLchar> shader_source;
    shader_source.ensure_capacity(source.length() + 1);
    for (auto c : source.bytes())
        shader_source.append(c);
    shader_source.append('\0');
    auto const* shader_source_ptr = shader_source.data();
    glShaderSource(shader, 1, &shader_source_ptr, nullptr);
    glCompileShader(shader);

    GLint compile_status = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compile_status);
    if (compile_status != GL_TRUE) {
        glDeleteShader(shader);
        return 0;
    }

    return shader;
}

static bool link_mundo_video_nv12_program(GLuint program)
{
    glLinkProgram(program);

    GLint link_status = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &link_status);
    return link_status == GL_TRUE;
}

template<typename PlaneTextureState, typename PlaneUploadPBOs>
static bool upload_mundo_video_nv12_plane_texture(GLuint texture, GLenum unit, GLenum internal_format, GLenum format, int width, int height, void const* data, size_t byte_count, PlaneTextureState& state, PlaneUploadPBOs& pbos, bool& used_pbo)
{
    glActiveTexture(unit);
    glBindTexture(GL_TEXTURE_2D, texture);
    auto reused_storage = state.width == width && state.height == height && state.internal_format == internal_format && state.format == format;
    auto const use_pbo = mundo_webgl_video_pbo_upload_enabled() && byte_count > 0;
    GLint previous_unpack_buffer = 0;
    if (use_pbo) {
        auto pbo_index = pbos.index++ % 3;
        auto& pbo = pbos.buffers[pbo_index];
        auto& pbo_size = pbos.sizes[pbo_index];
        if (!pbo)
            glGenBuffers(1, &pbo);
        if (pbo) {
            glGetIntegervRobustANGLE(GL_PIXEL_UNPACK_BUFFER_BINDING, 1, nullptr, &previous_unpack_buffer);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
            if (mundo_webgl_video_pbo_orphan_each_upload_enabled() || pbo_size < byte_count) {
                glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(byte_count), nullptr, GL_STREAM_DRAW);
                pbo_size = byte_count;
            }
            glBufferSubData(GL_PIXEL_UNPACK_BUFFER, 0, static_cast<GLsizeiptr>(byte_count), data);
            data = nullptr;
            used_pbo = true;
        }
    }
    if (!reused_storage) {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, GL_UNSIGNED_BYTE, data);
        state.width = width;
        state.height = height;
        state.internal_format = internal_format;
        state.format = format;
    } else if (!data && byte_count == 0) {
        if (used_pbo)
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, previous_unpack_buffer);
        return reused_storage;
    } else {
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height, format, GL_UNSIGNED_BYTE, data);
    }
    if (used_pbo)
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, previous_unpack_buffer);
    return reused_storage;
}

struct MundoVideoVertexAttribState {
    GLint enabled { 0 };
    GLint size { 4 };
    GLint type { GL_FLOAT };
    GLint normalized { GL_FALSE };
    GLint stride { 0 };
    GLint buffer_binding { 0 };
    void* pointer { nullptr };
};

static MundoVideoVertexAttribState save_mundo_video_vertex_attrib_state(GLuint index)
{
    MundoVideoVertexAttribState state;
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, &state.enabled);
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, &state.size);
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, &state.type);
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, &state.normalized);
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, &state.stride);
    glGetVertexAttribiv(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, &state.buffer_binding);
    glGetVertexAttribPointerv(index, GL_VERTEX_ATTRIB_ARRAY_POINTER, &state.pointer);
    return state;
}

static void restore_mundo_video_vertex_attrib_state(GLuint index, MundoVideoVertexAttribState const& state)
{
    glBindBuffer(GL_ARRAY_BUFFER, state.buffer_binding);
    glVertexAttribPointer(index, state.size, state.type, state.normalized, state.stride, state.pointer);
    if (state.enabled)
        glEnableVertexAttribArray(index);
    else
        glDisableVertexAttribArray(index);
}

bool WebGLRenderingContextBase::upload_texture_source_with_video_nv12_shader_fast_path(TexImageSource const& source, WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, Optional<int> destination_width, Optional<int> destination_height, bool is_sub_image)
{
    static size_t s_video_nv12_shader_attempt_count { 0 };
    auto attempt_count = ++s_video_nv12_shader_attempt_count;
    auto reject = [&](StringView reason) {
        auto source_is_video = source.has<GC::Root<HTML::HTMLVideoElement>>();
        auto has_nv12_frame = false;
        auto nv12_width = -1;
        auto nv12_height = -1;
        auto nv12_y_stride = -1;
        auto nv12_uv_stride = -1;
        size_t nv12_y_bytes = 0;
        size_t nv12_uv_bytes = 0;
        auto const* hardware_backend = "none";
        auto hardware_frame_id = 0ull;
        auto zero_copy_capable = false;
        auto requires_cpu_transfer = false;
        auto has_hardware_handle = false;
        if (source_is_video) {
            auto const& video = source.get<GC::Root<HTML::HTMLVideoElement>>();
            if (auto const* media_frame = video->current_media_frame()) {
                has_hardware_handle = media_frame->hardware_handle() != nullptr;
                if (auto const& hardware_descriptor = media_frame->hardware_descriptor(); hardware_descriptor.has_value()) {
                    hardware_backend = Media::hardware_video_frame_backend_name(hardware_descriptor->backend);
                    hardware_frame_id = hardware_descriptor->frame_id;
                    zero_copy_capable = hardware_descriptor->zero_copy_capable;
                    requires_cpu_transfer = hardware_descriptor->requires_cpu_transfer;
                }
                if (auto const* nv12_data = media_frame->cached_nv12_data()) {
                    has_nv12_frame = true;
                    nv12_width = nv12_data->width;
                    nv12_height = nv12_data->height;
                    nv12_y_stride = nv12_data->y_stride;
                    nv12_uv_stride = nv12_data->uv_stride;
                    nv12_y_bytes = nv12_data->y_plane_size();
                    nv12_uv_bytes = nv12_data->uv_plane_size();
                }
            }
        }
        if (has_nv12_frame || should_log_mundo_webgl_texture_diagnostic(attempt_count)) {
            dbgln("MUNDO_WEBGL_VIDEO_NV12_SHADER_UPLOAD_REJECT attempt={} reason={} source_is_video={} has_nv12={} frame_id={} hardware_backend={} zero_copy_capable={} requires_cpu_transfer={} has_hardware_handle={} nv12_size={}x{} nv12_stride={}x{} nv12_bytes={}+{} target={} level={} format={} type={} dest={}x{} flip_y={} premultiply={} sub_image={}",
                attempt_count,
                reason,
                source_is_video,
                has_nv12_frame,
                hardware_frame_id,
                hardware_backend,
                zero_copy_capable,
                requires_cpu_transfer,
                has_hardware_handle,
                nv12_width,
                nv12_height,
                nv12_y_stride,
                nv12_uv_stride,
                nv12_y_bytes,
                nv12_uv_bytes,
                target,
                level,
                format,
                type,
                destination_width.value_or(-1),
                destination_height.value_or(-1),
                m_unpack_flip_y,
                m_unpack_premultiply_alpha,
                is_sub_image);
        }
        return false;
    };

    if (!mundo_webgl_video_nv12_shader_upload_enabled())
        return reject("disabled"sv);
    if (!source.has<GC::Root<HTML::HTMLVideoElement>>())
        return reject("not_video"sv);
    if (target != GL_TEXTURE_2D || level != 0 || border != 0)
        return reject("unsupported_target_or_sub_image"sv);
    if (format != GL_RGBA || type != GL_UNSIGNED_BYTE)
        return reject("unsupported_format_or_type"sv);
    if (m_unpack_premultiply_alpha)
        return reject("unpack_transform"sv);

    auto const& video = source.get<GC::Root<HTML::HTMLVideoElement>>();
    auto const* media_frame = video->current_media_frame();
    if (!media_frame)
        return reject("missing_nv12_frame"sv);
    auto const* hardware_backend = "none";
    auto hardware_frame_id = 0ull;
    auto zero_copy_capable = false;
    auto requires_cpu_transfer = false;
    auto has_hardware_handle = media_frame->hardware_handle() != nullptr;
    if (auto const& hardware_descriptor = media_frame->hardware_descriptor(); hardware_descriptor.has_value()) {
        hardware_backend = Media::hardware_video_frame_backend_name(hardware_descriptor->backend);
        hardware_frame_id = hardware_descriptor->frame_id;
        zero_copy_capable = hardware_descriptor->zero_copy_capable;
        requires_cpu_transfer = hardware_descriptor->requires_cpu_transfer;
    }
    auto video_width = static_cast<int>(media_frame->width());
    auto video_height = static_cast<int>(media_frame->height());
    if (video_width <= 0 || video_height <= 0)
        return reject("empty_nv12_frame"sv);
    if (destination_width.has_value() && destination_width.value() != video_width)
        return reject("destination_width_mismatch"sv);
    if (destination_height.has_value() && destination_height.value() != video_height)
        return reject("destination_height_mismatch"sv);
    auto visible_uv_width = (video_width + 1) / 2;
    auto uv_texture_height = (video_height + 1) / 2;
    auto const* nv12_data = media_frame->cached_nv12_data();
    static int s_video_opaque_fd_plane_probe_width { 0 };
    static int s_video_opaque_fd_plane_probe_height { 0 };
    if (has_hardware_handle && zero_copy_capable && video_width > 0 && video_height > 0 && (s_video_opaque_fd_plane_probe_width != video_width || s_video_opaque_fd_plane_probe_height != video_height)) {
        s_video_opaque_fd_plane_probe_width = video_width;
        s_video_opaque_fd_plane_probe_height = video_height;
        dbgln("MUNDO_WEBGL_VIDEO_OPAQUE_FD_PLANE_PROBE_REQUEST attempt={} frame_id={} backend={} size={}x{} uv_size={}x{} has_hardware_handle={} zero_copy_capable={} requires_cpu_transfer={}",
            attempt_count,
            hardware_frame_id,
            hardware_backend,
            video_width,
            video_height,
            visible_uv_width,
            uv_texture_height,
            has_hardware_handle,
            zero_copy_capable,
            requires_cpu_transfer);
        context().probe_video_opaque_fd_texture_import(video_width, video_height, visible_uv_width, uv_texture_height, attempt_count);
    }
    static bool s_cuda_gl_buffer_upload_disabled { false };
    static bool s_cuda_gl_direct_texture_upload_disabled { false };
    auto cuda_upload_mode = mundo_webgl_video_cuda_upload_mode();
    auto can_attempt_hardware_gl_upload = has_hardware_handle
        && zero_copy_capable
        && !is_sub_image
        && ((cuda_upload_mode == MundoWebGLVideoCudaUploadMode::Texture && !s_cuda_gl_direct_texture_upload_disabled)
            || (cuda_upload_mode == MundoWebGLVideoCudaUploadMode::PBO && !s_cuda_gl_buffer_upload_disabled));
    if (s_cuda_gl_buffer_upload_disabled && cuda_upload_mode == MundoWebGLVideoCudaUploadMode::PBO && has_hardware_handle && zero_copy_capable && should_log_mundo_webgl_texture_diagnostic(attempt_count)) {
        dbgln("MUNDO_WEBGL_VIDEO_ZERO_COPY_STATUS attempt={} frame_id={} backend={} status=blocked reason=cuda_gl_buffer_interop_disabled has_hardware_handle={} gl_api=gles_angle_or_egl",
            attempt_count,
            hardware_frame_id,
            hardware_backend,
            has_hardware_handle);
    }
    if (s_cuda_gl_direct_texture_upload_disabled && cuda_upload_mode == MundoWebGLVideoCudaUploadMode::Texture && has_hardware_handle && zero_copy_capable && should_log_mundo_webgl_texture_diagnostic(attempt_count)) {
        dbgln("MUNDO_WEBGL_VIDEO_ZERO_COPY_STATUS attempt={} frame_id={} backend={} status=blocked reason=cuda_gl_direct_texture_upload_disabled has_hardware_handle={} gl_api=gles_angle_or_egl",
            attempt_count,
            hardware_frame_id,
            hardware_backend,
            has_hardware_handle);
    }

    auto validate_nv12_data = [&]() -> bool {
        if (!nv12_data)
            return false;
        if (nv12_data->width <= 0 || nv12_data->height <= 0)
            return false;
        visible_uv_width = (nv12_data->width + 1) / 2;
        uv_texture_height = (nv12_data->height + 1) / 2;
        if (nv12_data->y_stride < nv12_data->width || nv12_data->uv_stride < visible_uv_width * 2 || nv12_data->uv_stride % 2 != 0)
            return false;
        video_width = nv12_data->width;
        video_height = nv12_data->height;
        return true;
    };
    if (!can_attempt_hardware_gl_upload) {
        nv12_data = media_frame->nv12_data();
        if (!validate_nv12_data()) {
            if (zero_copy_capable && should_log_mundo_webgl_texture_diagnostic(attempt_count)) {
                dbgln("MUNDO_WEBGL_VIDEO_ZERO_COPY_STATUS attempt={} frame_id={} backend={} status=blocked reason=hardware_frame_without_cpu_nv12_or_interop_handle has_hardware_handle={} gl_api=gles_angle_or_egl",
                    attempt_count,
                    hardware_frame_id,
                    hardware_backend,
                    has_hardware_handle);
            }
            return reject("missing_nv12_frame"sv);
        }
    }

    if (!m_mundo_video_nv12_program) {
        auto vertex_shader = compile_mundo_video_nv12_shader(GL_VERTEX_SHADER, R"~~~(
            attribute vec2 a_position;
            attribute vec2 a_tex_coord;
            varying vec2 v_tex_coord;
            void main()
            {
                gl_Position = vec4(a_position, 0.0, 1.0);
                v_tex_coord = a_tex_coord;
            }
        )~~~"sv);
        auto fragment_shader = compile_mundo_video_nv12_shader(GL_FRAGMENT_SHADER, R"~~~(
            precision mediump float;
            varying vec2 v_tex_coord;
            uniform sampler2D u_y_plane;
            uniform sampler2D u_uv_plane;
            uniform vec2 u_y_coord_scale;
            uniform vec2 u_uv_coord_scale;
            uniform float u_uv_second_channel_is_alpha;
            uniform float u_y_offset;
            uniform float u_y_scale;
            uniform vec3 u_r_coefficients;
            uniform vec3 u_g_coefficients;
            uniform vec3 u_b_coefficients;
            void main()
            {
                float y = (texture2D(u_y_plane, v_tex_coord * u_y_coord_scale).r - u_y_offset) * u_y_scale;
                vec4 uv_sample = texture2D(u_uv_plane, v_tex_coord * u_uv_coord_scale);
                vec2 uv = vec2(uv_sample.r, mix(uv_sample.g, uv_sample.a, u_uv_second_channel_is_alpha)) - vec2(0.5, 0.5);
                vec3 yuv = vec3(y, uv.x, uv.y);
                gl_FragColor = vec4(dot(yuv, u_r_coefficients), dot(yuv, u_g_coefficients), dot(yuv, u_b_coefficients), 1.0);
            }
        )~~~"sv);

        if (!vertex_shader || !fragment_shader)
            return reject("shader_compile_failed"sv);

        m_mundo_video_nv12_program = glCreateProgram();
        if (!m_mundo_video_nv12_program)
            return reject("program_create_failed"sv);
        glAttachShader(m_mundo_video_nv12_program, vertex_shader);
        glAttachShader(m_mundo_video_nv12_program, fragment_shader);
        glBindAttribLocation(m_mundo_video_nv12_program, 0, "a_position");
        glBindAttribLocation(m_mundo_video_nv12_program, 1, "a_tex_coord");
        auto linked = link_mundo_video_nv12_program(m_mundo_video_nv12_program);
        glDeleteShader(vertex_shader);
        glDeleteShader(fragment_shader);
        if (!linked) {
            glDeleteProgram(m_mundo_video_nv12_program);
            m_mundo_video_nv12_program = 0;
            return reject("program_link_failed"sv);
        }
        m_mundo_video_nv12_uniform_locations.y_plane = glGetUniformLocation(m_mundo_video_nv12_program, "u_y_plane");
        m_mundo_video_nv12_uniform_locations.uv_plane = glGetUniformLocation(m_mundo_video_nv12_program, "u_uv_plane");
        m_mundo_video_nv12_uniform_locations.y_coord_scale = glGetUniformLocation(m_mundo_video_nv12_program, "u_y_coord_scale");
        m_mundo_video_nv12_uniform_locations.uv_coord_scale = glGetUniformLocation(m_mundo_video_nv12_program, "u_uv_coord_scale");
        m_mundo_video_nv12_uniform_locations.uv_second_channel_is_alpha = glGetUniformLocation(m_mundo_video_nv12_program, "u_uv_second_channel_is_alpha");
        m_mundo_video_nv12_uniform_locations.y_offset = glGetUniformLocation(m_mundo_video_nv12_program, "u_y_offset");
        m_mundo_video_nv12_uniform_locations.y_scale = glGetUniformLocation(m_mundo_video_nv12_program, "u_y_scale");
        m_mundo_video_nv12_uniform_locations.r_coefficients = glGetUniformLocation(m_mundo_video_nv12_program, "u_r_coefficients");
        m_mundo_video_nv12_uniform_locations.g_coefficients = glGetUniformLocation(m_mundo_video_nv12_program, "u_g_coefficients");
        m_mundo_video_nv12_uniform_locations.b_coefficients = glGetUniformLocation(m_mundo_video_nv12_program, "u_b_coefficients");
    }

    if (!m_mundo_video_nv12_y_texture)
        glGenTextures(1, &m_mundo_video_nv12_y_texture);
    if (!m_mundo_video_nv12_uv_texture)
        glGenTextures(1, &m_mundo_video_nv12_uv_texture);
    if (!m_mundo_video_nv12_framebuffer)
        glGenFramebuffers(1, &m_mundo_video_nv12_framebuffer);
    if (!m_mundo_video_nv12_vertex_buffer)
        glGenBuffers(1, &m_mundo_video_nv12_vertex_buffer);
    if (!m_mundo_video_nv12_y_texture || !m_mundo_video_nv12_uv_texture || !m_mundo_video_nv12_framebuffer || !m_mundo_video_nv12_vertex_buffer)
        return reject("resource_create_failed"sv);

    auto preserved_pending_gl_errors = 0;
    for (;;) {
        auto pending_error = glGetError();
        if (pending_error == GL_NO_ERROR)
            break;
        if (m_error == GL_NO_ERROR)
            m_error = pending_error;
        ++preserved_pending_gl_errors;
    }

    GLint previous_program = 0;
    GLint previous_active_texture = 0;
    GLint previous_texture_2d = 0;
    GLint previous_texture0_2d = 0;
    GLint previous_texture1_2d = 0;
    GLint previous_framebuffer = 0;
    GLint previous_array_buffer = 0;
    GLint previous_unpack_alignment = 4;
    GLint previous_viewport[4] {};
    GLint previous_color_writemask[4] {};
    GLboolean previous_scissor_test = GL_FALSE;
    GLboolean previous_blend = GL_FALSE;
    GLboolean previous_depth_test = GL_FALSE;
    GLboolean previous_stencil_test = GL_FALSE;
    GLboolean previous_cull_face = GL_FALSE;
    GLboolean previous_dither = GL_FALSE;
    glGetIntegervRobustANGLE(GL_CURRENT_PROGRAM, 1, nullptr, &previous_program);
    glGetIntegervRobustANGLE(GL_ACTIVE_TEXTURE, 1, nullptr, &previous_active_texture);
    glGetIntegervRobustANGLE(GL_TEXTURE_BINDING_2D, 1, nullptr, &previous_texture_2d);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegervRobustANGLE(GL_TEXTURE_BINDING_2D, 1, nullptr, &previous_texture0_2d);
    glActiveTexture(GL_TEXTURE1);
    glGetIntegervRobustANGLE(GL_TEXTURE_BINDING_2D, 1, nullptr, &previous_texture1_2d);
    glActiveTexture(previous_active_texture);
    glGetIntegervRobustANGLE(GL_FRAMEBUFFER_BINDING, 1, nullptr, &previous_framebuffer);
    glGetIntegervRobustANGLE(GL_ARRAY_BUFFER_BINDING, 1, nullptr, &previous_array_buffer);
    glGetIntegervRobustANGLE(GL_UNPACK_ALIGNMENT, 1, nullptr, &previous_unpack_alignment);
    glGetIntegervRobustANGLE(GL_VIEWPORT, 4, nullptr, previous_viewport);
    glGetIntegervRobustANGLE(GL_COLOR_WRITEMASK, 4, nullptr, previous_color_writemask);
    previous_scissor_test = glIsEnabled(GL_SCISSOR_TEST);
    previous_blend = glIsEnabled(GL_BLEND);
    previous_depth_test = glIsEnabled(GL_DEPTH_TEST);
    previous_stencil_test = glIsEnabled(GL_STENCIL_TEST);
    previous_cull_face = glIsEnabled(GL_CULL_FACE);
    previous_dither = glIsEnabled(GL_DITHER);
    auto previous_attrib0 = save_mundo_video_vertex_attrib_state(0);
    auto previous_attrib1 = save_mundo_video_vertex_attrib_state(1);

    auto restore_state = [&] {
        glBindFramebuffer(GL_FRAMEBUFFER, previous_framebuffer);
        glUseProgram(previous_program);
        restore_mundo_video_vertex_attrib_state(0, previous_attrib0);
        restore_mundo_video_vertex_attrib_state(1, previous_attrib1);
        glBindBuffer(GL_ARRAY_BUFFER, previous_array_buffer);
        glPixelStorei(GL_UNPACK_ALIGNMENT, previous_unpack_alignment);
        glViewport(previous_viewport[0], previous_viewport[1], previous_viewport[2], previous_viewport[3]);
        glColorMask(previous_color_writemask[0], previous_color_writemask[1], previous_color_writemask[2], previous_color_writemask[3]);
        if (previous_scissor_test)
            glEnable(GL_SCISSOR_TEST);
        else
            glDisable(GL_SCISSOR_TEST);
        if (previous_blend)
            glEnable(GL_BLEND);
        else
            glDisable(GL_BLEND);
        if (previous_depth_test)
            glEnable(GL_DEPTH_TEST);
        else
            glDisable(GL_DEPTH_TEST);
        if (previous_stencil_test)
            glEnable(GL_STENCIL_TEST);
        else
            glDisable(GL_STENCIL_TEST);
        if (previous_cull_face)
            glEnable(GL_CULL_FACE);
        else
            glDisable(GL_CULL_FACE);
        if (previous_dither)
            glEnable(GL_DITHER);
        else
            glDisable(GL_DITHER);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, previous_texture0_2d);
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, previous_texture1_2d);
        glActiveTexture(previous_active_texture);
        glBindTexture(GL_TEXTURE_2D, previous_texture_2d);
    };

    auto upload_start = MonotonicTime::now();

    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    auto target_texture_key = (static_cast<u64>(static_cast<unsigned>(previous_texture_2d)) << 32) | static_cast<u32>(level);
    bool reused_target_storage = false;
    bool allocated_target_storage = false;
    auto allocate_target_storage = [&] {
        glTexImage2D(target, level, internalformat, video_width, video_height, border, format, type, nullptr);
        allocated_target_storage = true;
        if (previous_texture_2d > 0) {
            m_mundo_video_nv12_target_texture_states.set(target_texture_key, MundoVideoNV12TargetTextureState {
                                                                           .width = video_width,
                                                                           .height = video_height,
                                                                           .internalformat = internalformat,
                                                                           .format = format,
                                                                           .type = type,
                                                                       });
        }
    };
    if (!is_sub_image) {
        auto previous_state = previous_texture_2d > 0 ? m_mundo_video_nv12_target_texture_states.find(target_texture_key) : m_mundo_video_nv12_target_texture_states.end();
        if (previous_state != m_mundo_video_nv12_target_texture_states.end()
            && previous_state->value.width == video_width
            && previous_state->value.height == video_height
            && previous_state->value.internalformat == internalformat
            && previous_state->value.format == format
            && previous_state->value.type == type) {
            reused_target_storage = true;
        } else {
            allocate_target_storage();
        }
    }
    auto used_y_plane_pbo = false;
    auto used_uv_plane_pbo = false;
    auto uv_texture_width = nv12_data ? nv12_data->uv_stride / 2 : visible_uv_width;
    bool reused_y_plane_storage = false;
    bool reused_uv_plane_storage = false;
    bool used_hardware_gl_upload = false;
    u64 hardware_gl_upload_microseconds = 0;
    StringView hardware_gl_upload_failure_reason = "not_attempted"sv;

    if (can_attempt_hardware_gl_upload) {
        reused_y_plane_storage = upload_mundo_video_nv12_plane_texture(m_mundo_video_nv12_y_texture, GL_TEXTURE0, GL_R8_EXT, GL_RED_EXT, video_width, video_height, nullptr, 0, m_mundo_video_nv12_y_texture_state, m_mundo_video_nv12_y_upload_pbos, used_y_plane_pbo);
        reused_uv_plane_storage = upload_mundo_video_nv12_plane_texture(m_mundo_video_nv12_uv_texture, GL_TEXTURE1, GL_RG8_EXT, GL_RG_EXT, visible_uv_width, uv_texture_height, nullptr, 0, m_mundo_video_nv12_uv_texture_state, m_mundo_video_nv12_uv_upload_pbos, used_uv_plane_pbo);
        if (cuda_upload_mode == MundoWebGLVideoCudaUploadMode::Texture) {
            auto upload_result = media_frame->hardware_handle()->upload_to_gl_textures(Media::HardwareVideoFrameGLTextureUploadRequest {
                .texture_target = GL_TEXTURE_2D,
                .y_texture = m_mundo_video_nv12_y_texture,
                .uv_texture = m_mundo_video_nv12_uv_texture,
                .width = static_cast<u32>(video_width),
                .height = static_cast<u32>(video_height),
                .uv_width = static_cast<u32>(visible_uv_width),
                .uv_height = static_cast<u32>(uv_texture_height),
            });
            if (!upload_result.is_error()) {
                used_hardware_gl_upload = true;
                hardware_gl_upload_microseconds = upload_result.value().upload_microseconds;
            } else {
                hardware_gl_upload_failure_reason = upload_result.error().string_literal();
                s_cuda_gl_direct_texture_upload_disabled = true;
                if (should_log_mundo_webgl_texture_diagnostic(attempt_count)) {
                    dbgln("MUNDO_WEBGL_VIDEO_ZERO_COPY_STATUS attempt={} frame_id={} backend={} status=gpu_texture_upload_failed reason={} has_hardware_handle={} gl_api=gles_angle_or_egl upload_mode=texture",
                        attempt_count,
                        hardware_frame_id,
                        hardware_backend,
                        hardware_gl_upload_failure_reason,
                        has_hardware_handle);
                }
            }
        }
        struct CudaUploadBuffer {
            GLuint buffer { 0 };
            size_t size { 0 };
            GLenum error { GL_NO_ERROR };
        };
        auto prepare_cuda_upload_buffer = [](MundoVideoNV12PlaneUploadPBOs& pbos, size_t byte_count) -> CudaUploadBuffer {
            auto pbo_index = pbos.index++ % 3;
            auto& pbo = pbos.buffers[pbo_index];
            auto& pbo_size = pbos.sizes[pbo_index];
            if (!pbo)
                glGenBuffers(1, &pbo);
            if (!pbo)
                return {};
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
            glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(byte_count), nullptr, GL_STREAM_DRAW);
            auto error = glGetError();
            GLint allocated_size = 0;
            if (error == GL_NO_ERROR)
                glGetBufferParameteriv(GL_PIXEL_UNPACK_BUFFER, GL_BUFFER_SIZE, &allocated_size);
            if (allocated_size > 0)
                pbo_size = static_cast<size_t>(allocated_size);
            else
                pbo_size = 0;
            glFinish();
            return CudaUploadBuffer { .buffer = pbo, .size = pbo_size, .error = error };
        };

        if (!used_hardware_gl_upload && cuda_upload_mode == MundoWebGLVideoCudaUploadMode::PBO) {
            GLint previous_unpack_buffer = 0;
            glGetIntegervRobustANGLE(GL_PIXEL_UNPACK_BUFFER_BINDING, 1, nullptr, &previous_unpack_buffer);
            auto y_upload_buffer_size = static_cast<size_t>(video_width) * static_cast<size_t>(video_height);
            auto uv_upload_buffer_size = static_cast<size_t>(visible_uv_width) * 2u * static_cast<size_t>(uv_texture_height);
            auto y_upload_buffer = prepare_cuda_upload_buffer(m_mundo_video_nv12_y_cuda_upload_pbos, y_upload_buffer_size);
            auto uv_upload_buffer = prepare_cuda_upload_buffer(m_mundo_video_nv12_uv_cuda_upload_pbos, uv_upload_buffer_size);
            glBindBuffer(GL_PIXEL_UNPACK_BUFFER, previous_unpack_buffer);
            auto plane_texture_error = y_upload_buffer.error != GL_NO_ERROR ? y_upload_buffer.error : uv_upload_buffer.error;
            if (!y_upload_buffer.buffer || !uv_upload_buffer.buffer) {
                hardware_gl_upload_failure_reason = "gpu_upload_buffer_create_failed"sv;
                if (should_log_mundo_webgl_texture_diagnostic(attempt_count)) {
                    dbgln("MUNDO_WEBGL_VIDEO_ZERO_COPY_STATUS attempt={} frame_id={} backend={} status=gpu_texture_upload_failed reason=gpu_upload_buffer_create_failed y_buffer={} uv_buffer={} has_hardware_handle={}",
                        attempt_count,
                        hardware_frame_id,
                        hardware_backend,
                        y_upload_buffer.buffer,
                        uv_upload_buffer.buffer,
                        has_hardware_handle);
                }
            } else if (y_upload_buffer.size < y_upload_buffer_size || uv_upload_buffer.size < uv_upload_buffer_size) {
                hardware_gl_upload_failure_reason = "gpu_upload_buffer_gl_size_mismatch"sv;
                if (should_log_mundo_webgl_texture_diagnostic(attempt_count)) {
                    dbgln("MUNDO_WEBGL_VIDEO_ZERO_COPY_STATUS attempt={} frame_id={} backend={} status=gpu_texture_upload_failed reason=gpu_upload_buffer_gl_size_mismatch y_buffer={} y_size={}/{} uv_buffer={} uv_size={}/{} has_hardware_handle={}",
                        attempt_count,
                        hardware_frame_id,
                        hardware_backend,
                        y_upload_buffer.buffer,
                        y_upload_buffer.size,
                        y_upload_buffer_size,
                        uv_upload_buffer.buffer,
                        uv_upload_buffer.size,
                        uv_upload_buffer_size,
                        has_hardware_handle);
                }
            } else if (plane_texture_error != GL_NO_ERROR) {
                hardware_gl_upload_failure_reason = "gpu_plane_texture_gl_error"sv;
                if (should_log_mundo_webgl_texture_diagnostic(attempt_count)) {
                    dbgln("MUNDO_WEBGL_VIDEO_ZERO_COPY_STATUS attempt={} frame_id={} backend={} status=gpu_texture_upload_failed reason=gpu_plane_texture_gl_error gl_error={} has_hardware_handle={} y_format={}/{} uv_format={}/{}",
                        attempt_count,
                        hardware_frame_id,
                        hardware_backend,
                        plane_texture_error,
                        has_hardware_handle,
                        GL_R8_EXT,
                        GL_RED_EXT,
                        GL_RG8_EXT,
                        GL_RG_EXT);
                }
            } else {
                auto upload_result = media_frame->hardware_handle()->upload_to_gl_textures(Media::HardwareVideoFrameGLTextureUploadRequest {
                    .texture_target = GL_TEXTURE_2D,
                    .y_texture = m_mundo_video_nv12_y_texture,
                    .uv_texture = m_mundo_video_nv12_uv_texture,
                    .width = static_cast<u32>(video_width),
                    .height = static_cast<u32>(video_height),
                    .uv_width = static_cast<u32>(visible_uv_width),
                    .uv_height = static_cast<u32>(uv_texture_height),
                    .y_upload_buffer = y_upload_buffer.buffer,
                    .uv_upload_buffer = uv_upload_buffer.buffer,
                    .y_upload_buffer_size = y_upload_buffer.size,
                    .uv_upload_buffer_size = uv_upload_buffer.size,
                });
                if (!upload_result.is_error()) {
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, m_mundo_video_nv12_y_texture);
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, y_upload_buffer.buffer);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, video_width, video_height, GL_RED_EXT, GL_UNSIGNED_BYTE, nullptr);
                    glActiveTexture(GL_TEXTURE1);
                    glBindTexture(GL_TEXTURE_2D, m_mundo_video_nv12_uv_texture);
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, uv_upload_buffer.buffer);
                    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, visible_uv_width, uv_texture_height, GL_RG_EXT, GL_UNSIGNED_BYTE, nullptr);
                    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, previous_unpack_buffer);
                    auto pbo_texture_error = glGetError();
                    if (pbo_texture_error != GL_NO_ERROR) {
                        hardware_gl_upload_failure_reason = "gpu_upload_buffer_tex_sub_image_failed"sv;
                        if (should_log_mundo_webgl_texture_diagnostic(attempt_count)) {
                            dbgln("MUNDO_WEBGL_VIDEO_ZERO_COPY_STATUS attempt={} frame_id={} backend={} status=gpu_texture_upload_failed reason=gpu_upload_buffer_tex_sub_image_failed gl_error={} has_hardware_handle={} y_buffer={} uv_buffer={}",
                                attempt_count,
                                hardware_frame_id,
                                hardware_backend,
                                pbo_texture_error,
                                has_hardware_handle,
                                y_upload_buffer.buffer,
                                uv_upload_buffer.buffer);
                        }
                    } else {
                        used_hardware_gl_upload = true;
                        hardware_gl_upload_microseconds = upload_result.value().upload_microseconds;
                    }
                }
                if (!upload_result.is_error() && used_hardware_gl_upload) {
                    used_hardware_gl_upload = true;
                    hardware_gl_upload_microseconds = upload_result.value().upload_microseconds;
                } else {
                    if (upload_result.is_error()) {
                        hardware_gl_upload_failure_reason = upload_result.error().string_literal();
                        if (hardware_gl_upload_failure_reason == "CUDA GL upload buffer interop disabled after mismatched mapping"sv)
                            s_cuda_gl_buffer_upload_disabled = true;
                    }
                    if (should_log_mundo_webgl_texture_diagnostic(attempt_count)) {
                        dbgln("MUNDO_WEBGL_VIDEO_ZERO_COPY_STATUS attempt={} frame_id={} backend={} status=gpu_texture_upload_failed reason={} has_hardware_handle={} gl_api=gles_angle_or_egl upload_mode=pbo",
                            attempt_count,
                            hardware_frame_id,
                            hardware_backend,
                            hardware_gl_upload_failure_reason,
                            has_hardware_handle);
                    }
                }
            }
        }
        if (!used_hardware_gl_upload) {
            nv12_data = media_frame->nv12_data();
            if (!validate_nv12_data()) {
                restore_state();
                return reject("missing_nv12_frame"sv);
            }
            uv_texture_width = nv12_data->uv_stride / 2;
        }
    }

    if (!used_hardware_gl_upload) {
        if (!nv12_data) {
            nv12_data = media_frame->nv12_data();
            if (!validate_nv12_data()) {
                restore_state();
                return reject("missing_nv12_frame"sv);
            }
            uv_texture_width = nv12_data->uv_stride / 2;
        }
        reused_y_plane_storage = upload_mundo_video_nv12_plane_texture(m_mundo_video_nv12_y_texture, GL_TEXTURE0, GL_LUMINANCE, GL_LUMINANCE, nv12_data->y_stride, nv12_data->height, nv12_data->y_plane_data(), nv12_data->y_plane_size(), m_mundo_video_nv12_y_texture_state, m_mundo_video_nv12_y_upload_pbos, used_y_plane_pbo);
        reused_uv_plane_storage = upload_mundo_video_nv12_plane_texture(m_mundo_video_nv12_uv_texture, GL_TEXTURE1, GL_LUMINANCE_ALPHA, GL_LUMINANCE_ALPHA, uv_texture_width, uv_texture_height, nv12_data->uv_plane_data(), nv12_data->uv_plane_size(), m_mundo_video_nv12_uv_texture_state, m_mundo_video_nv12_uv_upload_pbos, used_uv_plane_pbo);
    }

    if (zero_copy_capable && should_log_mundo_webgl_texture_diagnostic(attempt_count)) {
        probe_mundo_cuda_gl_interop(attempt_count);
        dbgln("MUNDO_WEBGL_VIDEO_ZERO_COPY_STATUS attempt={} frame_id={} backend={} status={} reason={} has_hardware_handle={} gl_api=gles_angle_or_egl y_bytes={} uv_bytes={} y_upload={} uv_upload={} gpu_upload_us={}",
            attempt_count,
            hardware_frame_id,
            hardware_backend,
            used_hardware_gl_upload ? "gpu_texture_upload"sv : "cpu_upload_required"sv,
            used_hardware_gl_upload ? (cuda_upload_mode == MundoWebGLVideoCudaUploadMode::Texture ? "cuda_gl_direct_texture_copy"sv : "cuda_gl_pbo_texture_copy"sv) : hardware_gl_upload_failure_reason,
            has_hardware_handle,
            nv12_data ? nv12_data->y_plane_size() : 0,
            nv12_data ? nv12_data->uv_plane_size() : 0,
            used_hardware_gl_upload ? "gpu"sv : used_y_plane_pbo ? "pbo"sv
                                                                  : "client"sv,
            used_hardware_gl_upload ? "gpu"sv : used_uv_plane_pbo ? "pbo"sv
                                                                   : "client"sv,
            hardware_gl_upload_microseconds);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, m_mundo_video_nv12_framebuffer);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, previous_texture_2d, level);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        if (!is_sub_image && reused_target_storage) {
            allocate_target_storage();
            reused_target_storage = false;
            glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, previous_texture_2d, level);
        }
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            restore_state();
            return reject("framebuffer_incomplete"sv);
        }
    }

    GLfloat vertices[16];
    vertices[0] = -1.0f;
    vertices[1] = -1.0f;
    vertices[4] = 1.0f;
    vertices[5] = -1.0f;
    vertices[8] = -1.0f;
    vertices[9] = 1.0f;
    vertices[12] = 1.0f;
    vertices[13] = 1.0f;
    if (m_unpack_flip_y) {
        vertices[2] = 0.0f;
        vertices[3] = 1.0f;
        vertices[6] = 1.0f;
        vertices[7] = 1.0f;
        vertices[10] = 0.0f;
        vertices[11] = 0.0f;
        vertices[14] = 1.0f;
        vertices[15] = 0.0f;
    } else {
        vertices[2] = 0.0f;
        vertices[3] = 0.0f;
        vertices[6] = 1.0f;
        vertices[7] = 0.0f;
        vertices[10] = 0.0f;
        vertices[11] = 1.0f;
        vertices[14] = 1.0f;
        vertices[15] = 1.0f;
    }

    glViewport(is_sub_image ? xoffset : 0, is_sub_image ? yoffset : 0, video_width, video_height);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_BLEND);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_DITHER);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glUseProgram(m_mundo_video_nv12_program);
    glBindBuffer(GL_ARRAY_BUFFER, m_mundo_video_nv12_vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER, sizeof(vertices), vertices, GL_STREAM_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), reinterpret_cast<void*>(0));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(GLfloat), reinterpret_cast<void*>(2 * sizeof(GLfloat)));
    glUniform1i(m_mundo_video_nv12_uniform_locations.y_plane, 0);
    glUniform1i(m_mundo_video_nv12_uniform_locations.uv_plane, 1);
    glUniform2f(m_mundo_video_nv12_uniform_locations.y_coord_scale, used_hardware_gl_upload ? 1.0f : static_cast<float>(video_width) / static_cast<float>(nv12_data->y_stride), 1.0f);
    glUniform2f(m_mundo_video_nv12_uniform_locations.uv_coord_scale, static_cast<float>(visible_uv_width) / static_cast<float>(uv_texture_width), 1.0f);
    glUniform1f(m_mundo_video_nv12_uniform_locations.uv_second_channel_is_alpha, used_hardware_gl_upload ? 0.0f : 1.0f);

    auto const& cicp = used_hardware_gl_upload ? media_frame->cicp() : nv12_data->cicp;
    auto const is_full_range = cicp.video_full_range_flag() == Media::VideoFullRangeFlag::Full;
    glUniform1f(m_mundo_video_nv12_uniform_locations.y_offset, is_full_range ? 0.0f : 16.0f / 255.0f);
    glUniform1f(m_mundo_video_nv12_uniform_locations.y_scale, is_full_range ? 1.0f : 255.0f / 219.0f);
    glUniform3f(m_mundo_video_nv12_uniform_locations.r_coefficients, 1.0f, 0.0f, 1.5748f);
    glUniform3f(m_mundo_video_nv12_uniform_locations.g_coefficients, 1.0f, -0.1873f, -0.4681f);
    glUniform3f(m_mundo_video_nv12_uniform_locations.b_coefficients, 1.0f, 1.8556f, 0.0f);
    glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    auto draw_error = glGetError();

    u8 sampled_rgba[4] {};
    auto sampled_y = 0;
    auto sampled_u = 0;
    auto sampled_v = 0;
    auto sampled_x = video_width / 2;
    auto sampled_y_row = video_height / 2;
    auto should_probe_black_frame = !used_hardware_gl_upload && mundo_webgl_video_nv12_shader_black_probe_enabled() && should_log_mundo_webgl_texture_diagnostic(attempt_count);
    if (should_probe_black_frame && draw_error == GL_NO_ERROR) {
        glReadPixels(sampled_x, sampled_y_row, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, sampled_rgba);
        draw_error = glGetError();
        sampled_y = nv12_data->y_plane_data()[static_cast<size_t>(sampled_y_row) * static_cast<size_t>(nv12_data->y_stride) + static_cast<size_t>(sampled_x)];
        auto uv_x = (sampled_x / 2) * 2;
        auto uv_y = sampled_y_row / 2;
        sampled_u = nv12_data->uv_plane_data()[static_cast<size_t>(uv_y) * static_cast<size_t>(nv12_data->uv_stride) + static_cast<size_t>(uv_x)];
        sampled_v = nv12_data->uv_plane_data()[static_cast<size_t>(uv_y) * static_cast<size_t>(nv12_data->uv_stride) + static_cast<size_t>(uv_x) + 1];
    }

    restore_state();

    auto upload_microseconds = (MonotonicTime::now() - upload_start).to_microseconds();
    if (should_log_mundo_webgl_texture_diagnostic(attempt_count)) {
        dbgln("MUNDO_WEBGL_VIDEO_NV12_SHADER_UPLOAD attempt={} kind={} frame_id={} hardware_backend={} zero_copy_capable={} requires_cpu_transfer={} has_hardware_handle={} upload_us={} size={}x{} offset={}x{} target_storage={} plane_storage={}/{} plane_upload={}/{} y_bytes={} uv_bytes={} flip_y={} full_range={} preserved_gl_errors={} gl_error={} sample_yuv={},{},{} sample_rgba={},{},{},{}",
            attempt_count,
            is_sub_image ? "texSubImage2D"sv : "texImage2D"sv,
            hardware_frame_id,
            hardware_backend,
            zero_copy_capable,
            requires_cpu_transfer,
            has_hardware_handle,
            upload_microseconds,
            video_width,
            video_height,
            is_sub_image ? xoffset : 0,
            is_sub_image ? yoffset : 0,
            reused_target_storage ? "reused"sv : allocated_target_storage ? "allocated"sv
                                                                           : "subimage"sv,
            reused_y_plane_storage ? "reused"sv : "allocated"sv,
            reused_uv_plane_storage ? "reused"sv : "allocated"sv,
            used_hardware_gl_upload ? "gpu"sv : used_y_plane_pbo ? "pbo"sv
                                                                  : "client"sv,
            used_hardware_gl_upload ? "gpu"sv : used_uv_plane_pbo ? "pbo"sv
                                                                   : "client"sv,
            nv12_data ? nv12_data->y_plane_size() : 0,
            nv12_data ? nv12_data->uv_plane_size() : 0,
            m_unpack_flip_y,
            is_full_range,
            preserved_pending_gl_errors,
            draw_error,
            sampled_y,
            sampled_u,
            sampled_v,
            sampled_rgba[0],
            sampled_rgba[1],
            sampled_rgba[2],
            sampled_rgba[3]);
    }
    if (draw_error == GL_NO_ERROR && should_probe_black_frame) {
        auto input_luma_is_visible = sampled_y > 24;
        auto output_is_black = sampled_rgba[0] < 4 && sampled_rgba[1] < 4 && sampled_rgba[2] < 4;
        if (input_luma_is_visible && output_is_black)
            return reject("black_probe"sv);
    }
    if (draw_error != GL_NO_ERROR) {
        dbgln("MUNDO_WEBGL_VIDEO_NV12_SHADER_UPLOAD_DRAW_ERROR attempt={} kind={} gl_error={} size={}x{} offset={}x{}",
            attempt_count,
            is_sub_image ? "texSubImage2D"sv : "texImage2D"sv,
            draw_error,
            video_width,
            video_height,
            is_sub_image ? xoffset : 0,
            is_sub_image ? yoffset : 0);
        return reject("draw_error"sv);
    }
    return draw_error == GL_NO_ERROR;
}

bool WebGLRenderingContextBase::upload_texture_source_with_video_bitmap_fast_path(TexImageSource const& source, WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, Optional<int> destination_width, Optional<int> destination_height, bool is_sub_image)
{
    static size_t s_video_direct_bitmap_attempt_count { 0 };
    auto attempt_count = ++s_video_direct_bitmap_attempt_count;
    RefPtr<Gfx::ImmutableBitmap> bitmap;
    RefPtr<Gfx::Bitmap const> raster_bitmap;
    auto reject = [&](StringView reason) {
        if (should_log_mundo_webgl_texture_diagnostic(attempt_count)) {
            dbgln("MUNDO_WEBGL_VIDEO_DIRECT_BITMAP_UPLOAD_REJECT attempt={} reason={} source_is_video={} format={} type={} dest={}x{} flip_y={} premultiply={} bitmap={} bitmap_size={}x{} raster={} raster_size={}x{} raster_format={} alpha_type={} pitch={} data_size={}",
                attempt_count,
                reason,
                source.has<GC::Root<HTML::HTMLVideoElement>>(),
                format,
                type,
                destination_width.value_or(-1),
                destination_height.value_or(-1),
                m_unpack_flip_y,
                m_unpack_premultiply_alpha,
                static_cast<void const*>(bitmap.ptr()),
                bitmap ? bitmap->width() : 0,
                bitmap ? bitmap->height() : 0,
                static_cast<void const*>(raster_bitmap.ptr()),
                raster_bitmap ? raster_bitmap->width() : 0,
                raster_bitmap ? raster_bitmap->height() : 0,
                raster_bitmap ? static_cast<int>(raster_bitmap->format()) : -1,
                raster_bitmap ? static_cast<int>(raster_bitmap->alpha_type()) : -1,
                raster_bitmap ? raster_bitmap->pitch() : 0,
                raster_bitmap ? raster_bitmap->data_size() : 0);
        }
        return false;
    };

    if (!mundo_webgl_video_direct_bitmap_upload_enabled())
        return reject("disabled"sv);
    if (!source.has<GC::Root<HTML::HTMLVideoElement>>())
        return reject("not_video"sv);
    if (format != GL_RGBA || type != GL_UNSIGNED_BYTE)
        return reject("unsupported_format_or_type"sv);
    if (m_unpack_premultiply_alpha)
        return reject("unpack_transform"sv);

    auto const& video = source.get<GC::Root<HTML::HTMLVideoElement>>();
    if (auto const* media_frame = video->current_media_frame(); media_frame && media_frame->nv12_data()) {
        auto const* nv12_data = media_frame->nv12_data();
        if (should_log_mundo_webgl_texture_diagnostic(attempt_count)) {
            dbgln("MUNDO_WEBGL_VIDEO_NV12_SOURCE attempt={} source_size={}x{} y_stride={} uv_stride={} y_bytes={} uv_bytes={} flip_y={} note=materializes_before_upload",
                attempt_count,
                nv12_data->width,
                nv12_data->height,
                nv12_data->y_stride,
                nv12_data->uv_stride,
                nv12_data->y_plane_size(),
                nv12_data->uv_plane_size(),
                m_unpack_flip_y);
        }
    }

    bitmap = video->bitmap();
    if (!bitmap)
        return reject("missing_bitmap"sv);

    raster_bitmap = bitmap->bitmap();
    if (!raster_bitmap)
        return reject("missing_raster_bitmap"sv);
    if (raster_bitmap->format() != Gfx::BitmapFormat::RGBA8888)
        return reject("unsupported_bitmap_format"sv);
    if (raster_bitmap->alpha_type() != Gfx::AlphaType::Premultiplied)
        return reject("unsupported_alpha_type"sv);
    if (raster_bitmap->width() <= 0 || raster_bitmap->height() <= 0)
        return reject("empty_bitmap"sv);
    if (destination_width.has_value() && destination_width.value() != raster_bitmap->width())
        return reject("destination_width_mismatch"sv);
    if (destination_height.has_value() && destination_height.value() != raster_bitmap->height())
        return reject("destination_height_mismatch"sv);

    auto row_bytes = static_cast<size_t>(raster_bitmap->width()) * 4;
    if (raster_bitmap->pitch() != row_bytes)
        return reject("non_contiguous_rows"sv);

    auto data_size = raster_bitmap->data_size();

    auto upload_start = MonotonicTime::now();
    auto copy_microseconds = 0;
    if (m_unpack_flip_y) {
        auto pbo_index = m_mundo_video_upload_pbo_index++ % 3;
        auto& pbo = m_mundo_video_upload_pbos[pbo_index];
        auto& pbo_size = m_mundo_video_upload_pbo_sizes[pbo_index];
        auto& flipped_buffer = m_mundo_video_upload_staging_buffers[pbo_index];

        auto copy_start = MonotonicTime::now();
        MUST(flipped_buffer.try_resize(data_size));
        for (int y = 0; y < raster_bitmap->height(); ++y)
            memcpy(flipped_buffer.data() + static_cast<size_t>(y) * row_bytes, raster_bitmap->scanline_u8(raster_bitmap->height() - 1 - y), row_bytes);
        copy_microseconds = (MonotonicTime::now() - copy_start).to_microseconds();

        if (!pbo)
            glGenBuffers(1, &pbo);
        if (!pbo)
            return reject("missing_pbo"sv);

        GLint previous_unpack_buffer = 0;
        glGetIntegervRobustANGLE(GL_PIXEL_UNPACK_BUFFER_BINDING, 1, nullptr, &previous_unpack_buffer);

        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, pbo);
        if (pbo_size < flipped_buffer.size()) {
            glBufferData(GL_PIXEL_UNPACK_BUFFER, static_cast<GLsizeiptr>(flipped_buffer.size()), nullptr, GL_STREAM_DRAW);
            pbo_size = flipped_buffer.size();
        }
        glBufferSubData(GL_PIXEL_UNPACK_BUFFER, 0, static_cast<GLsizeiptr>(flipped_buffer.size()), flipped_buffer.data());
        if (is_sub_image) {
            glTexSubImage2D(target, level, xoffset, yoffset, raster_bitmap->width(), raster_bitmap->height(), format, type, nullptr);
        } else {
            glTexImage2D(target, level, internalformat, raster_bitmap->width(), raster_bitmap->height(), border, format, type, nullptr);
        }
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, previous_unpack_buffer);
    } else {
        if (is_sub_image) {
            glTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, raster_bitmap->width(), raster_bitmap->height(), format, type, data_size, raster_bitmap->scanline_u8(0));
        } else {
            glTexImage2DRobustANGLE(target, level, internalformat, raster_bitmap->width(), raster_bitmap->height(), border, format, type, data_size, raster_bitmap->scanline_u8(0));
        }
    }
    auto upload_microseconds = (MonotonicTime::now() - upload_start).to_microseconds();

    if (should_log_mundo_webgl_texture_diagnostic(attempt_count)) {
        dbgln("MUNDO_WEBGL_VIDEO_DIRECT_BITMAP_UPLOAD attempt={} kind={} upload_us={} copy_us={} size={}x{} bytes={} flip_y={} staging_buffers={} bitmap={} raster={}",
            attempt_count,
            is_sub_image ? "texSubImage2D"sv : "texImage2D"sv,
            upload_microseconds,
            copy_microseconds,
            raster_bitmap->width(),
            raster_bitmap->height(),
            data_size,
            m_unpack_flip_y,
            m_unpack_flip_y ? "reused"sv : "none"sv,
            static_cast<void const*>(bitmap.ptr()),
            static_cast<void const*>(raster_bitmap.ptr()));
    }

    return true;
}

// TODO: The glGetError spec allows for queueing errors which is something we should probably do, for now
//       this just keeps track of one error which is also fine by the spec
GLenum WebGLRenderingContextBase::get_error_value()
{
    if (m_error == GL_NO_ERROR)
        return glGetError();

    auto error = m_error;
    m_error = GL_NO_ERROR;
    return error;
}

void WebGLRenderingContextBase::set_error(GLenum error)
{
    if (m_error != GL_NO_ERROR)
        return;

    auto context_error = glGetError();
    if (context_error != GL_NO_ERROR)
        m_error = context_error;
    else
        m_error = error;
}

bool WebGLRenderingContextBase::is_context_lost() const
{
    dbgln_if(WEBGL_CONTEXT_DEBUG, "WebGLRenderingContext::is_context_lost()");
    return m_context_lost;
}

// https://immersive-web.github.io/webxr/#dom-webglrenderingcontextbase-makexrcompatible
GC::Ref<WebIDL::Promise> WebGLRenderingContextBase::make_xr_compatible()
{
    // 1. If the requesting document’s origin is not allowed to use the "xr-spatial-tracking" permissions policy,
    //    resolve promise and return it.
    // FIXME: Implement this.

    // 2. Let promise be a new Promise created in the Realm of this WebGLRenderingContextBase.
    auto& realm = this->realm();
    auto promise = WebIDL::create_promise(realm);

    // 3. Let context be this.
    auto context = this;

    // 4. Run the following steps in parallel:
    Platform::EventLoopPlugin::the().deferred_invoke(GC::create_function(realm.heap(), [&realm, context, promise]() {
        // 1. Let device be the result of ensuring an immersive XR device is selected.
        // FIXME: Implement https://immersive-web.github.io/webxr/#ensure-an-immersive-xr-device-is-selected

        // 2. Set context’s XR compatible boolean as follows:

        // -> If context’s WebGL context lost flag is set:
        if (context->is_context_lost()) {
            // Queue a task to set context’s XR compatible boolean to false and reject promise with an InvalidStateError.
            HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(realm.heap(), [&realm, promise, context]() {
                context->set_xr_compatible(false);
                HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                WebIDL::reject_promise(realm, promise, WebIDL::InvalidStateError::create(realm, "The WebGL context has been lost."_utf16));
            }));
        }
        // -> If device is null:
        else if (false) {
            // Queue a task to set context’s XR compatible boolean to false and reject promise with an InvalidStateError.
            HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(realm.heap(), [&realm, promise, context]() {
                context->set_xr_compatible(false);
                HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                WebIDL::reject_promise(realm, promise, WebIDL::InvalidStateError::create(realm, "Could not select an immersive XR device."_utf16));
            }));
        }
        // -> If context’s XR compatible boolean is true:
        else if (context->xr_compatible()) {
            // Queue a task to resolve promise.
            HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(realm.heap(), [&realm, promise]() {
                HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                WebIDL::resolve_promise(realm, promise);
            }));
        }
        // -> If context was created on a compatible graphics adapter for device:
        // FIXME: For now we just pretend that this happened, so that we can resolve the promise and proceed running basic WPT tests for this.
        else if (true) {
            // Queue a task to set context’s XR compatible boolean to true and resolve promise.
            HTML::queue_a_task(HTML::Task::Source::Unspecified, nullptr, nullptr, GC::create_function(realm.heap(), [&realm, promise, context]() {
                context->set_xr_compatible(true);
                HTML::TemporaryExecutionContext execution_context { realm, HTML::TemporaryExecutionContext::CallbacksEnabled::Yes };
                WebIDL::resolve_promise(realm, promise);
            }));
        }
        // -> Otherwise:
        else {
            // Queue a task on the WebGL task source to perform the following steps:
            HTML::queue_a_task(HTML::Task::Source::WebGL, nullptr, nullptr, GC::create_function(realm.heap(), []() {
                // 1. Force context to be lost.

                // 2. Handle the context loss as described by the WebGL specification:
                // FIXME: Implement https://registry.khronos.org/webgl/specs/latest/1.0/#CONTEXT_LOST
            }));
        }
    }));

    // 5. Return promise.
    return promise;
}

}
