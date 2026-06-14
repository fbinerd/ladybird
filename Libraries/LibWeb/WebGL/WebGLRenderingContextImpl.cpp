/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define GL_GLEXT_PROTOTYPES 1

#include <GLES3/gl3.h>
extern "C" {
#include <GLES2/gl2ext.h>
#include <GLES2/gl2ext_angle.h>
}

#include <AK/Time.h>
#include <LibJS/Runtime/Array.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLActiveInfo.h>
#include <LibWeb/WebGL/WebGLBuffer.h>
#include <LibWeb/WebGL/WebGLFramebuffer.h>
#include <LibWeb/WebGL/WebGLProgram.h>
#include <LibWeb/WebGL/WebGLQuery.h>
#include <LibWeb/WebGL/WebGLRenderbuffer.h>
#include <LibWeb/WebGL/WebGLRenderingContextImpl.h>
#include <LibWeb/WebGL/WebGLSampler.h>
#include <LibWeb/WebGL/WebGLShader.h>
#include <LibWeb/WebGL/WebGLShaderPrecisionFormat.h>
#include <LibWeb/WebGL/WebGLSync.h>
#include <LibWeb/WebGL/WebGLTexture.h>
#include <LibWeb/WebGL/WebGLTransformFeedback.h>
#include <LibWeb/WebGL/WebGLUniformLocation.h>
#include <LibWeb/WebGL/WebGLVertexArrayObject.h>
#include <LibWeb/WebIDL/Buffers.h>
#include <stdlib.h>
#include <string.h>

namespace Web::WebGL {

// https://registry.khronos.org/webgl/extensions/WEBGL_debug_renderer_info/
static constexpr GLenum UNMASKED_VENDOR_WEBGL = 0x9245;
static constexpr GLenum UNMASKED_RENDERER_WEBGL = 0x9246;

static bool mundo_webgl_timing_enabled()
{
    auto const* raw_value = getenv("MUNDO_WEBGL_TIMING_LOG");
    if (!raw_value)
        return true;

    return raw_value[0] != '\0' && strcmp(raw_value, "0") && strcmp(raw_value, "false") && strcmp(raw_value, "no") && strcmp(raw_value, "off");
}

static bool mundo_webgl_timing_detail_enabled()
{
    auto const* raw_value = getenv("MUNDO_WEBGL_TIMING_DETAIL_LOG");
    if (!raw_value)
        return false;

    return raw_value[0] != '\0' && strcmp(raw_value, "0") && strcmp(raw_value, "false") && strcmp(raw_value, "no") && strcmp(raw_value, "off");
}

static i64 mundo_webgl_timing_threshold_ms()
{
    auto const* raw_value = getenv("MUNDO_WEBGL_TIMING_LOG_MS");
    if (!raw_value || raw_value[0] == '\0')
        return 40;

    auto value = strtoll(raw_value, nullptr, 10);
    return value > 0 ? value : 40;
}

static i64 mundo_webgl_timing_summary_interval_ms()
{
    auto const* raw_value = getenv("MUNDO_WEBGL_TIMING_SUMMARY_INTERVAL_MS");
    if (!raw_value || raw_value[0] == '\0')
        return 1000;

    auto value = strtoll(raw_value, nullptr, 10);
    return value > 0 ? value : 1000;
}

static int mundo_webgl_draw_elements_budget_per_second()
{
    auto const* raw_value = getenv("MUNDO_WEBGL_DRAW_ELEMENTS_BUDGET_PER_SECOND");
    if (!raw_value)
        return 0;

    auto value = atoi(raw_value);
    if (value <= 0)
        return 0;

    return value;
}

static Optional<i64> mundo_webgl_slow_duration(MonotonicTime start)
{
    if (!mundo_webgl_timing_enabled() || !mundo_webgl_timing_detail_enabled())
        return {};

    auto duration = (MonotonicTime::now() - start).to_milliseconds();
    auto threshold = mundo_webgl_timing_threshold_ms();
    if (duration < threshold)
        return {};

    return duration;
}

static bool should_skip_mundo_webgl_draw_elements_for_budget(WebIDL::Long count)
{
    auto budget_per_second = mundo_webgl_draw_elements_budget_per_second();
    if (budget_per_second <= 0)
        return false;

    static MonotonicTime s_last_refill { MonotonicTime::now() };
    static double s_available_budget { static_cast<double>(budget_per_second) };
    static size_t s_skipped_draws { 0 };
    static size_t s_log_count { 0 };

    auto now = MonotonicTime::now();
    auto elapsed_ms = (now - s_last_refill).to_milliseconds();
    if (elapsed_ms > 0) {
        s_available_budget = min(static_cast<double>(budget_per_second), s_available_budget + (static_cast<double>(elapsed_ms) * static_cast<double>(budget_per_second) / 1000.0));
        s_last_refill = now;
    }

    if (s_available_budget >= 1.0) {
        s_available_budget -= 1.0;
        return false;
    }

    ++s_skipped_draws;
    ++s_log_count;
    if (s_log_count <= 32 || s_log_count % 240 == 0)
        dbgln("MUNDO_WEBGL_DRAW_BUDGET_SKIP log_count={} skipped={} budget_per_second={} available={} count={}",
            s_log_count,
            s_skipped_draws,
            budget_per_second,
            static_cast<int>(s_available_budget),
            count);

    return true;
}

static bool mundo_webgl_env_flag_enabled(char const* name)
{
    auto const* value = getenv(name);
    return value && StringView { value, strlen(value) } == "1"sv;
}

static bool mundo_webgl_env_opt_in_enabled(char const* name)
{
    auto const* value = getenv(name);
    if (!value)
        return false;

    auto view = StringView { value, strlen(value) };
    if (view == "0"sv || view == "false"sv || view == "off"sv || view == "no"sv)
        return false;

    return view == "1"sv || view == "true"sv || view == "on"sv || view == "yes"sv;
}

static bool mundo_webgl_env_enabled_by_default(char const* name)
{
    auto const* value = getenv(name);
    if (!value)
        return true;

    auto view = StringView { value, strlen(value) };
    if (view == "0"sv || view == "false"sv || view == "off"sv || view == "no"sv)
        return false;

    return true;
}

static size_t mundo_webgl_env_size_value(char const* name, size_t default_value)
{
    auto const* value = getenv(name);
    if (!value || value[0] == '\0')
        return default_value;

    auto parsed_value = strtoull(value, nullptr, 10);
    return parsed_value > 0 ? static_cast<size_t>(parsed_value) : default_value;
}

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

static bool mundo_webgl_skip_post_direct_vulkan_non_sampler_gl_enabled()
{
    auto const* value = getenv("MUNDO_WEBGL_VIDEO_DIRECT_VULKAN_SKIP_POST_NON_SAMPLER_GL");
    if (!value)
        return false;

    auto view = StringView { value, strlen(value) };
    return view == "1"sv || view == "true"sv || view == "on"sv;
}

struct MundoWebGLTimingSummary {
    size_t draw_arrays_count { 0 };
    i64 draw_arrays_us { 0 };
    size_t draw_elements_count { 0 };
    i64 draw_elements_us { 0 };
    size_t finish_count { 0 };
    i64 finish_us { 0 };
    size_t flush_count { 0 };
    i64 flush_us { 0 };
    MonotonicTime started_at { MonotonicTime::now() };
};

static void record_mundo_webgl_timing_summary(char const* operation, i64 duration_us)
{
    if (!mundo_webgl_timing_enabled())
        return;

    static MundoWebGLTimingSummary s_summary;

    if (!strcmp(operation, "drawArrays")) {
        ++s_summary.draw_arrays_count;
        s_summary.draw_arrays_us += duration_us;
    } else if (!strcmp(operation, "drawElements")) {
        ++s_summary.draw_elements_count;
        s_summary.draw_elements_us += duration_us;
    } else if (!strcmp(operation, "finish")) {
        ++s_summary.finish_count;
        s_summary.finish_us += duration_us;
    } else if (!strcmp(operation, "flush")) {
        ++s_summary.flush_count;
        s_summary.flush_us += duration_us;
    }

    auto now = MonotonicTime::now();
    auto elapsed = (now - s_summary.started_at).to_milliseconds();
    if (elapsed < mundo_webgl_timing_summary_interval_ms())
        return;

    auto total_count = s_summary.draw_arrays_count + s_summary.draw_elements_count + s_summary.finish_count + s_summary.flush_count;
    auto total_us = s_summary.draw_arrays_us + s_summary.draw_elements_us + s_summary.finish_us + s_summary.flush_us;
    if (total_count) {
        dbgln("MUNDO_WEBGL_FRAME_SUMMARY elapsed={}ms total_count={} total_us={} avg_us={} draw_arrays={}/{}us draw_elements={}/{}us finish={}/{}us flush={}/{}us",
            elapsed,
            total_count,
            total_us,
            total_us / static_cast<i64>(total_count),
            s_summary.draw_arrays_count,
            s_summary.draw_arrays_us,
            s_summary.draw_elements_count,
            s_summary.draw_elements_us,
            s_summary.finish_count,
            s_summary.finish_us,
            s_summary.flush_count,
            s_summary.flush_us);
    }

    s_summary = {};
    s_summary.started_at = now;
}

static size_t mundo_webgl_next_timing_count()
{
    static size_t s_count { 0 };
    return ++s_count;
}

static bool mundo_webgl_is_sampler_uniform_type(GLenum type)
{
    switch (type) {
    case GL_SAMPLER_2D:
    case GL_SAMPLER_CUBE:
    case GL_SAMPLER_2D_SHADOW:
    case GL_SAMPLER_2D_ARRAY:
    case GL_SAMPLER_2D_ARRAY_SHADOW:
    case GL_INT_SAMPLER_2D:
    case GL_INT_SAMPLER_3D:
    case GL_INT_SAMPLER_CUBE:
    case GL_INT_SAMPLER_2D_ARRAY:
    case GL_UNSIGNED_INT_SAMPLER_2D:
    case GL_UNSIGNED_INT_SAMPLER_3D:
    case GL_UNSIGNED_INT_SAMPLER_CUBE:
    case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
#ifdef GL_SAMPLER_EXTERNAL_OES
    case GL_SAMPLER_EXTERNAL_OES:
#endif
        return true;
    default:
        return false;
    }
}

static Vector<GLchar> mundo_webgl_null_terminated_string(StringView string)
{
    Vector<GLchar> result;
    result.ensure_capacity(string.length() + 1);
    for (auto c : string.bytes())
        result.append(c);
    result.append('\0');
    return result;
}

struct MundoWebGLVideoVirtualizationReadiness {
    bool has_plan { false };
    bool route_supported { false };
    bool direct_texture_call { false };
    bool sampler_matches_video_texture { false };
    GLint sampler_location { -1 };
    GLint sampler_unit { -1 };
    GLint sampler_bound_texture { 0 };
    StringView reason { "no_video_sampler_plan"sv };
};

#ifdef USE_VULKAN_DMABUF_IMAGES
struct MundoWebGLVirtualVideoDrawGeometry {
    GLenum mode { 0 };
    GLint first { 0 };
    GLsizei count { 0 };
    GLenum type { 0 };
    GLintptr offset { 0 };
};
#endif

static MundoWebGLVideoVirtualizationReadiness mundo_webgl_video_virtualization_readiness(GLuint program_handle, GLuint video_texture_handle, WebGLTexture::HardwareVideoBacking const& backing, Optional<WebGLProgram::VideoSamplerPlan> const& video_sampler_plan)
{
    MundoWebGLVideoVirtualizationReadiness readiness;
    if (!program_handle || !video_texture_handle) {
        readiness.reason = "missing_program_or_texture"sv;
        return readiness;
    }
    if (!video_sampler_plan.has_value())
        return readiness;

    auto const& plan = video_sampler_plan.value();
    readiness.has_plan = true;
    readiness.direct_texture_call = plan.direct_texture_call;
    readiness.route_supported = !strcmp(backing.direct_sampling_route, "vulkan_direct_sampling_virtualization");
    if (!readiness.route_supported) {
        readiness.reason = "route_not_vulkan_direct_sampling_virtualization"sv;
        return readiness;
    }
    if (!plan.direct_texture_call) {
        readiness.reason = "sampler_not_direct_texture_call"sv;
        return readiness;
    }

    auto uniform_name = mundo_webgl_null_terminated_string(plan.uniform_name.bytes_as_string_view());
    readiness.sampler_location = glGetUniformLocation(program_handle, uniform_name.data());
    if (readiness.sampler_location < 0) {
        readiness.reason = "sampler_uniform_location_missing"sv;
        return readiness;
    }

    glGetUniformiv(program_handle, readiness.sampler_location, &readiness.sampler_unit);
    if (readiness.sampler_unit < 0) {
        readiness.reason = "sampler_unit_unset"sv;
        return readiness;
    }

    GLint previous_active_texture = 0;
    glGetIntegervRobustANGLE(GL_ACTIVE_TEXTURE, 1, nullptr, &previous_active_texture);
    glActiveTexture(GL_TEXTURE0 + readiness.sampler_unit);
    glGetIntegervRobustANGLE(GL_TEXTURE_BINDING_2D, 1, nullptr, &readiness.sampler_bound_texture);
    glActiveTexture(previous_active_texture);

    readiness.sampler_matches_video_texture = readiness.sampler_bound_texture == static_cast<GLint>(video_texture_handle);
    readiness.reason = readiness.sampler_matches_video_texture ? "ready"sv : "sampler_bound_texture_mismatch"sv;
    return readiness;
}

static void log_mundo_webgl_video_virtualization_draw_state(char const* op, size_t draw_log_count, WebGLTexture::HardwareVideoBacking const& backing, GLuint program_handle, GLuint texture_handle, GLenum mode, GLint first, GLsizei count, GLenum type, GLintptr offset)
{
    GLint framebuffer = 0;
    GLint vertex_array = 0;
    GLint array_buffer = 0;
    GLint element_array_buffer = 0;
    GLint active_texture = 0;
    GLint viewport[4] {};
    GLint scissor_box[4] {};

    glGetIntegervRobustANGLE(GL_FRAMEBUFFER_BINDING, 1, nullptr, &framebuffer);
    glGetIntegervRobustANGLE(GL_VERTEX_ARRAY_BINDING, 1, nullptr, &vertex_array);
    glGetIntegervRobustANGLE(GL_ARRAY_BUFFER_BINDING, 1, nullptr, &array_buffer);
    glGetIntegervRobustANGLE(GL_ELEMENT_ARRAY_BUFFER_BINDING, 1, nullptr, &element_array_buffer);
    glGetIntegervRobustANGLE(GL_ACTIVE_TEXTURE, 1, nullptr, &active_texture);
    glGetIntegervRobustANGLE(GL_VIEWPORT, 4, nullptr, viewport);
    glGetIntegervRobustANGLE(GL_SCISSOR_BOX, 4, nullptr, scissor_box);

    dbgln("MUNDO_WEBGL_VIDEO_VIRTUALIZATION_DRAW_STATE count={} op={} frame_id={} program={} texture={} mode={} first={} draw_count={} type={} offset={} framebuffer={} vertex_array={} array_buffer={} element_array_buffer={} active_texture={} viewport={}x{}+{}+{} scissor={}x{}+{}+{} blend={} depth_test={} cull_face={} scissor_test={} route={} next_step=replay_or_replace_this_draw_with_vulkan_video_sampler",
        draw_log_count,
        op,
        backing.frame_id,
        program_handle,
        texture_handle,
        mode,
        first,
        count,
        type,
        offset,
        framebuffer,
        vertex_array,
        array_buffer,
        element_array_buffer,
        active_texture,
        viewport[2],
        viewport[3],
        viewport[0],
        viewport[1],
        scissor_box[2],
        scissor_box[3],
        scissor_box[0],
        scissor_box[1],
        glIsEnabled(GL_BLEND) == GL_TRUE,
        glIsEnabled(GL_DEPTH_TEST) == GL_TRUE,
        glIsEnabled(GL_CULL_FACE) == GL_TRUE,
        glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE,
        backing.direct_sampling_route);

    dbgln("MUNDO_WEBGL_VIDEO_GPU_PIPELINE_STATUS count={} op={} frame_id={} program={} texture={} gpu_only={} direct_zero_copy={} copied_on_gpu={} upload_mode={} copy_stage={} route={} blocker={} next_step={}",
        draw_log_count,
        op,
        backing.frame_id,
        program_handle,
        texture_handle,
        backing.direct_zero_copy || backing.copied_on_gpu,
        backing.direct_zero_copy,
        backing.copied_on_gpu,
        backing.upload_mode,
        backing.copy_stage,
        backing.direct_sampling_route,
        backing.direct_zero_copy ? "none" : "webgl_draw_still_samples_intermediate_rgba_texture",
        backing.direct_zero_copy ? "verify_direct_decoder_surface_sampling" : "replace_this_draw_or_texture_binding_with_direct_nv12_vulkan_sampler");

    dbgln("MUNDO_WEBGL_VIDEO_VIRTUAL_SOURCE_READY count={} op={} frame_id={} program={} texture={} source_opaque_fd_retained={} source_handle_type={} source_format={} source_layout={} source_allocation_size={} source_single_optimal_multiplanar={} route={} ready={} blocker={} next_step={}",
        draw_log_count,
        op,
        backing.frame_id,
        program_handle,
        texture_handle,
        backing.source_opaque_fd >= 0,
        backing.source_handle_type,
        backing.source_vulkan_format,
        backing.source_vulkan_layout,
        backing.source_allocation_size,
        backing.source_single_optimal_multiplanar,
        backing.direct_sampling_route,
        backing.source_opaque_fd >= 0 && backing.source_single_optimal_multiplanar && !strcmp(backing.direct_sampling_route, "vulkan_direct_sampling_virtualization"),
        backing.source_opaque_fd >= 0 ? "none" : "missing_retained_source_opaque_fd",
        backing.source_opaque_fd >= 0 ? "import_retained_source_fd_for_vulkan_virtual_draw" : "retain_decoder_surface_fd_before_virtual_draw");
}

#ifdef USE_VULKAN_DMABUF_IMAGES
struct MundoWebGLVirtualVideoSourceCacheState {
    bool ready { false };
    StringView reason { "not_attempted"sv };
};

static MundoWebGLVirtualVideoSourceCacheState ensure_mundo_webgl_video_virtual_source_cached(OpenGLContext& context, WebGLTexture& texture, WebGLTexture::HardwareVideoBacking const& backing, char const* op, size_t draw_log_count, GLuint program_handle, GLuint texture_handle, bool should_log)
{
    auto const* cached_source = texture.cached_virtual_vulkan_video_source();
    if (cached_source && texture.cached_virtual_vulkan_video_source_frame_id() == backing.frame_id && cached_source->direct_sample_ready && cached_source->width == backing.width && cached_source->height == backing.height) {
        if (should_log) {
            dbgln("MUNDO_WEBGL_VIDEO_VIRTUAL_SOURCE_CACHE count={} op={} frame_id={} cached_frame_id={} program={} texture={} status=hit image={} image_view={} sampler={} next_step=use_cached_source_for_virtual_draw",
                draw_log_count,
                op,
                backing.frame_id,
                texture.cached_virtual_vulkan_video_source_frame_id(),
                program_handle,
                texture_handle,
                reinterpret_cast<uintptr_t>(cached_source->image),
                reinterpret_cast<uintptr_t>(cached_source->ycbcr_image_view),
                reinterpret_cast<uintptr_t>(cached_source->ycbcr_sampler));
        }
        return { true, "cache_hit"sv };
    }

    auto imported_source_or_error = context.import_retained_vulkan_video_source_for_virtual_draw(backing.source_opaque_fd, backing.source_handle_type, backing.source_allocation_size, backing.width, backing.height, backing.source_vulkan_format, backing.source_vulkan_layout);
    if (imported_source_or_error.is_error()) {
        if (should_log) {
            dbgln("MUNDO_WEBGL_VIDEO_VIRTUAL_SOURCE_CACHE count={} op={} frame_id={} program={} texture={} status=failed reason={} next_step=fix_source_import_before_virtual_draw",
                draw_log_count,
                op,
                backing.frame_id,
                program_handle,
                texture_handle,
                imported_source_or_error.error().string_literal());
        }
        return { false, imported_source_or_error.error().string_literal() };
    }

    auto imported_source = imported_source_or_error.release_value();
    auto direct_sample_ready = imported_source->direct_sample_ready;
    auto image = reinterpret_cast<uintptr_t>(imported_source->image);
    auto image_view = reinterpret_cast<uintptr_t>(imported_source->ycbcr_image_view);
    auto sampler = reinterpret_cast<uintptr_t>(imported_source->ycbcr_sampler);
    texture.set_cached_virtual_vulkan_video_source(backing.frame_id, move(imported_source));
    if (should_log) {
        dbgln("MUNDO_WEBGL_VIDEO_VIRTUAL_SOURCE_CACHE count={} op={} frame_id={} cached_frame_id={} program={} texture={} status=filled direct_sample_ready={} image={} image_view={} sampler={} next_step={}",
            draw_log_count,
            op,
            backing.frame_id,
            texture.cached_virtual_vulkan_video_source_frame_id(),
            program_handle,
            texture_handle,
            direct_sample_ready,
            image,
            image_view,
            sampler,
            direct_sample_ready ? "bind_cached_source_to_virtual_draw" : "fix_direct_sample_resources_before_virtual_draw");
    }
    if (!direct_sample_ready)
        return { false, "direct_sample_resources_not_ready"sv };
    return { true, "cache_filled"sv };
}

static void log_mundo_webgl_video_virtual_draw_ready(char const* op, size_t draw_log_count, WebGLTexture& texture, WebGLTexture::HardwareVideoBacking const& backing, GLuint program_handle, GLuint texture_handle, MundoWebGLVideoVirtualizationReadiness const& readiness, MundoWebGLVirtualVideoSourceCacheState const& cache_state)
{
    auto const* cached_source = texture.cached_virtual_vulkan_video_source();
    auto cached_frame_id = texture.cached_virtual_vulkan_video_source_frame_id();
    auto cache_matches_frame = cached_source && cached_frame_id == backing.frame_id;
    auto ready = readiness.route_supported && readiness.sampler_matches_video_texture && cache_state.ready && cache_matches_frame;
    auto reason = ready ? "ready"sv
        : !readiness.route_supported ? "unsupported_route"sv
        : !readiness.sampler_matches_video_texture ? readiness.reason
        : !cache_state.ready ? cache_state.reason
        : "cached_source_frame_mismatch"sv;

    dbgln("MUNDO_WEBGL_VIDEO_VIRTUAL_DRAW_READY count={} op={} frame_id={} cached_frame_id={} program={} texture={} ready={} reason={} route={} sampler_unit={} sampler_bound_texture={} cached_source={} direct_sample_ready={} next_step={}",
        draw_log_count,
        op,
        backing.frame_id,
        cached_frame_id,
        program_handle,
        texture_handle,
        ready,
        reason,
        backing.direct_sampling_route,
        readiness.sampler_unit,
        readiness.sampler_bound_texture,
        cached_source != nullptr,
        cached_source ? cached_source->direct_sample_ready : false,
        ready ? "replace_rgba_sampler_with_cached_nv12_vulkan_source" : "keep_rgba_intermediate_until_virtual_draw_is_ready");
}

static bool log_mundo_webgl_video_vulkan_direct_draw_plan(OpenGLContext& opengl_context, WebGLRenderingContextImpl const& webgl_context, char const* op, size_t draw_log_count, bool should_log, WebGLTexture& texture, WebGLTexture::HardwareVideoBacking const& backing, GLuint program_handle, GLuint texture_handle, MundoWebGLVideoVirtualizationReadiness const& readiness, MundoWebGLVirtualVideoSourceCacheState const& cache_state, MundoWebGLVirtualVideoDrawGeometry geometry)
{
    auto const* cached_source = texture.cached_virtual_vulkan_video_source();
    auto cached_frame_id = texture.cached_virtual_vulkan_video_source_frame_id();
    auto cache_matches_frame = cached_source && cached_frame_id == backing.frame_id;
    auto ready = readiness.route_supported
        && readiness.direct_texture_call
        && readiness.sampler_matches_video_texture
        && cache_state.ready
        && cache_matches_frame
        && cached_source
        && cached_source->direct_sample_ready;
    auto reason = ready ? "ready_for_vulkan_draw_replay"sv
        : !readiness.route_supported ? "unsupported_route"sv
        : !readiness.direct_texture_call ? "sampler_not_direct_texture_call"sv
        : !readiness.sampler_matches_video_texture ? readiness.reason
        : !cache_state.ready ? cache_state.reason
        : !cache_matches_frame ? "cached_source_frame_mismatch"sv
        : "direct_sample_resources_not_ready"sv;

    GLint framebuffer = 0;
    GLint vertex_array = 0;
    GLint element_array_buffer = 0;
    GLint active_attrib_count = 0;
    GLint active_uniform_count = 0;
    GLint attached_shader_count = 0;
    GLint viewport[4] {};
    glGetIntegervRobustANGLE(GL_FRAMEBUFFER_BINDING, 1, nullptr, &framebuffer);
    glGetIntegervRobustANGLE(GL_VERTEX_ARRAY_BINDING, 1, nullptr, &vertex_array);
    glGetIntegervRobustANGLE(GL_ELEMENT_ARRAY_BUFFER_BINDING, 1, nullptr, &element_array_buffer);
    glGetIntegervRobustANGLE(GL_VIEWPORT, 4, nullptr, viewport);
    glGetProgramivRobustANGLE(program_handle, GL_ACTIVE_ATTRIBUTES, 1, nullptr, &active_attrib_count);
    glGetProgramivRobustANGLE(program_handle, GL_ACTIVE_UNIFORMS, 1, nullptr, &active_uniform_count);
    glGetProgramivRobustANGLE(program_handle, GL_ATTACHED_SHADERS, 1, nullptr, &attached_shader_count);

    if (should_log)
        dbgln("MUNDO_WEBGL_VIDEO_VULKAN_DIRECT_DRAW_PLAN count={} op={} frame_id={} cached_frame_id={} program={} texture={} ready={} reason={} route={} sampler_unit={} sampler_location={} sampler_bound_texture={} source_image={} source_image_view={} source_sampler={} source_format={} source_layout={} source_allocation_size={} source_single_optimal_multiplanar={} draw_mode={} draw_first={} draw_count={} draw_type={} draw_offset={} framebuffer={} vertex_array={} element_array_buffer={} active_attribs={} active_uniforms={} attached_shaders={} viewport={}x{}+{}+{} blocker={} next_step={}",
        draw_log_count,
        op,
        backing.frame_id,
        cached_frame_id,
        program_handle,
        texture_handle,
        ready,
        reason,
        backing.direct_sampling_route,
        readiness.sampler_unit,
        readiness.sampler_location,
        readiness.sampler_bound_texture,
        cached_source ? reinterpret_cast<uintptr_t>(cached_source->image) : 0,
        cached_source ? reinterpret_cast<uintptr_t>(cached_source->ycbcr_image_view) : 0,
        cached_source ? reinterpret_cast<uintptr_t>(cached_source->ycbcr_sampler) : 0,
        backing.source_vulkan_format,
        backing.source_vulkan_layout,
        backing.source_allocation_size,
        backing.source_single_optimal_multiplanar,
        geometry.mode,
        geometry.first,
        geometry.count,
        geometry.type,
        geometry.offset,
        framebuffer,
        vertex_array,
        element_array_buffer,
        active_attrib_count,
        active_uniform_count,
        attached_shader_count,
        viewport[2],
        viewport[3],
        viewport[0],
        viewport[1],
        ready ? "webgl_backend_cannot_consume_vulkan_ycbcr_sampler_directly_yet"sv : reason,
        ready ? "implement_vulkan_geometry_or_backend_interop_for_this_draw" : "complete_direct_draw_prerequisites");

    auto uniforms_to_log = active_uniform_count < 12 ? active_uniform_count : 12;
    bool has_matrix_uniform = false;
    size_t sampler_uniform_count = 0;
    size_t non_sampler_uniform_count = 0;
    OpenGLContext::VulkanVideoMeshUniformSnapshot uniform_snapshot;
    for (size_t i = 0; i < 16; ++i) {
        uniform_snapshot.model_view_matrix[i] = (i % 5) == 0 ? 1.0f : 0.0f;
        uniform_snapshot.projection_matrix[i] = (i % 5) == 0 ? 1.0f : 0.0f;
    }
    for (GLint index = 0; index < uniforms_to_log; ++index) {
        GLint size = 0;
        GLenum type = 0;
        GLsizei length = 0;
        GLchar name[256];
        glGetActiveUniform(program_handle, static_cast<GLuint>(index), sizeof(name), &length, &size, &type, name);
        if (!length)
            continue;
        auto uniform_name = StringView { name, static_cast<size_t>(length) };
        auto is_sampler = mundo_webgl_is_sampler_uniform_type(type);
        auto is_matrix = type == GL_FLOAT_MAT2 || type == GL_FLOAT_MAT3 || type == GL_FLOAT_MAT4;
        sampler_uniform_count += is_sampler ? 1 : 0;
        non_sampler_uniform_count += is_sampler ? 0 : 1;
        has_matrix_uniform |= is_matrix;
        auto location = glGetUniformLocation(program_handle, name);
        if (location >= 0) {
            if (type == GL_FLOAT_MAT4 && uniform_name == "modelViewMatrix"sv) {
                glGetUniformfv(program_handle, location, uniform_snapshot.model_view_matrix.data());
                uniform_snapshot.has_model_view_matrix = true;
            } else if (type == GL_FLOAT_MAT4 && uniform_name == "projectionMatrix"sv) {
                glGetUniformfv(program_handle, location, uniform_snapshot.projection_matrix.data());
                uniform_snapshot.has_projection_matrix = true;
            } else if (type == GL_FLOAT && uniform_name == "opacity"sv) {
                glGetUniformfv(program_handle, location, &uniform_snapshot.opacity);
            } else if (type == GL_FLOAT && uniform_name == "uOutputIntensity"sv) {
                glGetUniformfv(program_handle, location, &uniform_snapshot.output_intensity);
            } else if (type == GL_FLOAT && uniform_name == "uStereoEye"sv) {
                glGetUniformfv(program_handle, location, &uniform_snapshot.stereo_eye);
            } else if (type == GL_BOOL && uniform_name == "uStereoEyeLeft"sv) {
                GLint stereo_eye_left = 1;
                glGetUniformiv(program_handle, location, &stereo_eye_left);
                uniform_snapshot.stereo_eye_left = stereo_eye_left ? 1.0f : 0.0f;
            }
        }
        if (should_log)
            dbgln("MUNDO_WEBGL_VIDEO_VULKAN_DIRECT_DRAW_UNIFORM count={} frame_id={} program={} index={} name={} type={} size={} sampler={} matrix={} next_step={}",
            draw_log_count,
            backing.frame_id,
            program_handle,
            index,
            uniform_name,
            type,
            size,
            is_sampler,
            is_matrix,
            is_sampler ? "map_video_sampler_to_vulkan_ycbcr_descriptor" : "preserve_uniform_for_shader_replay");
    }
    if (should_log)
        dbgln("MUNDO_WEBGL_VIDEO_VULKAN_DIRECT_DRAW_UNIFORM_SNAPSHOT count={} frame_id={} program={} has_model_view_matrix={} has_projection_matrix={} opacity={} output_intensity={} stereo_eye={} stereo_eye_left={} model_view_m00={} model_view_m13={} projection_m00={} projection_m11={} next_step=feed_uniform_snapshot_to_vulkan_mesh_pipeline",
        draw_log_count,
        backing.frame_id,
        program_handle,
        uniform_snapshot.has_model_view_matrix,
        uniform_snapshot.has_projection_matrix,
        uniform_snapshot.opacity,
        uniform_snapshot.output_intensity,
        uniform_snapshot.stereo_eye,
        uniform_snapshot.stereo_eye_left,
        uniform_snapshot.model_view_matrix[0],
        uniform_snapshot.model_view_matrix[13],
        uniform_snapshot.projection_matrix[0],
        uniform_snapshot.projection_matrix[5]);
    auto matrix_replay_ready = uniform_snapshot.has_model_view_matrix && uniform_snapshot.has_projection_matrix;
    if (should_log)
        dbgln("MUNDO_WEBGL_VIDEO_VULKAN_DIRECT_DRAW_UNIFORM_SUMMARY count={} frame_id={} program={} scanned_uniforms={} active_uniforms={} sampler_uniforms_scanned={} non_sampler_uniforms_scanned={} has_matrix_uniform={} simple_fixed_shader_safe={} matrix_replay_ready={} next_step={}",
        draw_log_count,
        backing.frame_id,
        program_handle,
        uniforms_to_log,
        active_uniform_count,
        sampler_uniform_count,
        non_sampler_uniform_count,
        has_matrix_uniform,
        !has_matrix_uniform && non_sampler_uniform_count == 0,
        matrix_replay_ready,
        matrix_replay_ready ? "execute_vulkan_mesh_with_captured_matrix_uniforms" : has_matrix_uniform || non_sampler_uniform_count > 0 ? "preserve_or_translate_original_webgl_shader_state" : "fixed_video_mesh_shader_can_be_validated");

    auto attribs_to_log = active_attrib_count < 8 ? active_attrib_count : 8;
    bool all_enabled_attrib_buffers_shadowed = true;
    size_t enabled_attrib_count = 0;
    bool has_position_attrib = false;
    bool has_uv_attrib = false;
    bool has_uv_right_attrib = false;
    ReadonlyBytes position_data;
    ReadonlyBytes uv_data;
    ReadonlyBytes uv_right_data;
    for (GLint index = 0; index < attribs_to_log; ++index) {
        GLint size = 0;
        GLenum type = 0;
        GLsizei length = 0;
        GLchar name[256];
        glGetActiveAttrib(program_handle, static_cast<GLuint>(index), sizeof(name), &length, &size, &type, name);
        if (!length)
            continue;

        auto attribute_name = StringView { name, static_cast<size_t>(length) };
        if (attribute_name == "position"sv)
            has_position_attrib = true;
        else if (attribute_name == "uv"sv)
            has_uv_attrib = true;
        else if (attribute_name == "uv_right"sv)
            has_uv_right_attrib = true;

        auto location = glGetAttribLocation(program_handle, name);
        GLint enabled = 0;
        GLint array_size = 0;
        GLint array_type = 0;
        GLint normalized = 0;
        GLint stride = 0;
        GLint buffer = 0;
        void* pointer = nullptr;
        bool buffer_shadow_complete = false;
        size_t buffer_shadow_bytes = 0;
        if (location >= 0) {
            glGetVertexAttribivRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_ENABLED, 1, nullptr, &enabled);
            glGetVertexAttribivRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_SIZE, 1, nullptr, &array_size);
            glGetVertexAttribivRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_TYPE, 1, nullptr, &array_type);
            glGetVertexAttribivRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, 1, nullptr, &normalized);
            glGetVertexAttribivRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_STRIDE, 1, nullptr, &stride);
            glGetVertexAttribivRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, 1, nullptr, &buffer);
            glGetVertexAttribPointervRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_POINTER, 1, nullptr, &pointer);
            if (auto webgl_buffer = webgl_context.mundo_buffer_for_handle(static_cast<GLuint>(buffer))) {
                buffer_shadow_complete = webgl_buffer->has_complete_shadow_data();
                buffer_shadow_bytes = webgl_buffer->shadow_byte_length();
                if (buffer_shadow_complete) {
                    if (attribute_name == "position"sv)
                        position_data = webgl_buffer->shadow_data();
                    else if (attribute_name == "uv"sv)
                        uv_data = webgl_buffer->shadow_data();
                    else if (attribute_name == "uv_right"sv)
                        uv_right_data = webgl_buffer->shadow_data();
                }
            }
            if (enabled) {
                ++enabled_attrib_count;
                all_enabled_attrib_buffers_shadowed &= buffer_shadow_complete;
            }
        }
        if (should_log)
            dbgln("MUNDO_WEBGL_VIDEO_VULKAN_DIRECT_DRAW_ATTRIB count={} frame_id={} program={} active_index={} name={} location={} declared_size={} declared_type={} enabled={} array_size={} array_type={} normalized={} stride={} pointer_offset={} buffer={} buffer_shadow_complete={} buffer_shadow_bytes={} next_step={}",
            draw_log_count,
            backing.frame_id,
            program_handle,
            index,
            attribute_name,
            location,
            size,
            type,
            enabled,
            array_size,
            array_type,
            normalized,
            stride,
            reinterpret_cast<uintptr_t>(pointer),
            buffer,
            buffer_shadow_complete,
            buffer_shadow_bytes,
            buffer_shadow_complete ? "map_shadowed_webgl_vertex_input_to_vulkan_pipeline" : "capture_or_share_webgl_vertex_buffer_before_vulkan_replay");
    }

    if (geometry.type) {
        bool element_shadow_complete = false;
        size_t element_shadow_bytes = 0;
        ReadonlyBytes element_data;
        auto element_buffer_handle = static_cast<GLuint>(element_array_buffer);
        if (auto element_buffer = webgl_context.mundo_buffer_for_handle(element_buffer_handle)) {
            element_shadow_complete = element_buffer->has_complete_shadow_data();
            element_shadow_bytes = element_buffer->shadow_byte_length();
            if (element_shadow_complete)
                element_data = element_buffer->shadow_data();
        }
        if (should_log)
            dbgln("MUNDO_WEBGL_VIDEO_VULKAN_DIRECT_DRAW_INDEX_BUFFER count={} frame_id={} program={} element_buffer={} shadow_complete={} shadow_bytes={} draw_type={} draw_offset={} draw_count={} next_step={}",
            draw_log_count,
            backing.frame_id,
            program_handle,
            element_buffer_handle,
            element_shadow_complete,
            element_shadow_bytes,
            geometry.type,
            geometry.offset,
            geometry.count,
            element_shadow_complete ? "map_shadowed_webgl_index_buffer_to_vulkan_pipeline" : "capture_or_share_webgl_index_buffer_before_vulkan_replay");

        if (should_log)
            dbgln("MUNDO_WEBGL_VIDEO_VULKAN_DIRECT_REPLAY_INPUTS_READY count={} frame_id={} program={} ready={} reason={} enabled_attribs={} attrib_buffers_shadowed={} element_buffer={} element_shadow_complete={} source_direct_sample_ready={} next_step={}",
            draw_log_count,
            backing.frame_id,
            program_handle,
            all_enabled_attrib_buffers_shadowed && element_shadow_complete && cached_source && cached_source->direct_sample_ready,
            !all_enabled_attrib_buffers_shadowed ? "missing_vertex_buffer_shadow"sv
                : !element_shadow_complete ? "missing_index_buffer_shadow"sv
                : !(cached_source && cached_source->direct_sample_ready) ? "source_not_direct_sample_ready"sv
                : "ready_for_vulkan_replay_pipeline"sv,
            enabled_attrib_count,
            all_enabled_attrib_buffers_shadowed,
            element_buffer_handle,
            element_shadow_complete,
            cached_source ? cached_source->direct_sample_ready : false,
            all_enabled_attrib_buffers_shadowed && element_shadow_complete && cached_source && cached_source->direct_sample_ready ? "create_vulkan_pipeline_from_webgl_draw_inputs" : "complete_replay_inputs_before_vulkan_pipeline");

        auto simple_video_mesh_replay_possible = has_position_attrib && has_uv_attrib && all_enabled_attrib_buffers_shadowed && element_shadow_complete && cached_source && cached_source->direct_sample_ready;
        Optional<OpenGLContext::VulkanVideoReplayBufferProbeResult> replay_buffer_probe;
        Optional<OpenGLContext::VulkanVideoMeshPipelineProbeResult> mesh_pipeline_probe;
        if (simple_video_mesh_replay_possible)
            replay_buffer_probe = opengl_context.probe_vulkan_video_replay_buffers(position_data, uv_data, uv_right_data, element_data, backing.frame_id, draw_log_count);
        auto destination_format = opengl_context.vulkan_painting_surface_format();
        if (simple_video_mesh_replay_possible && replay_buffer_probe.has_value() && replay_buffer_probe->supported && cached_source && destination_format.has_value())
            mesh_pipeline_probe = opengl_context.probe_vulkan_video_mesh_pipeline(backing.frame_id, destination_format.value(), cached_source->image, cached_source->ycbcr_image_view, cached_source->ycbcr_sampler, backing.source_vulkan_layout, uniform_snapshot, geometry.count, geometry.type, geometry.offset, viewport[0], viewport[1], viewport[2], viewport[3], draw_log_count);
        if (should_log)
            dbgln("MUNDO_WEBGL_VIDEO_VULKAN_DIRECT_REPLAY_STRATEGY count={} frame_id={} program={} simple_video_mesh_replay_possible={} matrix_replay_ready={} has_position={} has_uv={} has_uv_right={} active_attribs={} enabled_attribs={} attrib_buffers_shadowed={} element_shadow_complete={} source_direct_sample_ready={} replay_buffers_ready={} mesh_pipeline_ready={} destination_format={} selected={} reason={} next_step={}",
            draw_log_count,
            backing.frame_id,
            program_handle,
            simple_video_mesh_replay_possible,
            matrix_replay_ready,
            has_position_attrib,
            has_uv_attrib,
            has_uv_right_attrib,
            active_attrib_count,
            enabled_attrib_count,
            all_enabled_attrib_buffers_shadowed,
            element_shadow_complete,
            cached_source ? cached_source->direct_sample_ready : false,
            replay_buffer_probe.has_value() && replay_buffer_probe->supported,
            mesh_pipeline_probe.has_value() && mesh_pipeline_probe->supported,
            destination_format.value_or(0),
            simple_video_mesh_replay_possible && matrix_replay_ready ? "matrix_aware_vulkan_video_mesh_shader" : simple_video_mesh_replay_possible ? "fixed_vulkan_video_mesh_shader" : "full_webgl_shader_replay_or_interop",
            !simple_video_mesh_replay_possible ? "missing_simple_mesh_prerequisite"sv
                : !(replay_buffer_probe.has_value() && replay_buffer_probe->supported) ? "vulkan_replay_buffer_probe_failed"sv
                : !destination_format.has_value() ? "missing_vulkan_painting_surface_format"sv
                : mesh_pipeline_probe.has_value() && mesh_pipeline_probe->supported ? "position_uv_mesh_with_vulkan_buffers_and_pipeline"sv
                : "vulkan_mesh_pipeline_probe_failed"sv,
            simple_video_mesh_replay_possible && replay_buffer_probe.has_value() && replay_buffer_probe->supported && mesh_pipeline_probe.has_value() && mesh_pipeline_probe->supported ? "bind_replay_buffers_uniforms_and_execute_vulkan_mesh_draw" : "capture_more_shader_or_buffer_state");
        auto video_vulkan_mesh_executed = mesh_pipeline_probe.has_value() && mesh_pipeline_probe->executed;
        if (should_log) {
            dbgln("MUNDO_WEBGL_VIDEO_GPU_PIPELINE_EXECUTION_STATUS count={} frame_id={} program={} texture={} candidate_gpu_only={} candidate_direct_zero_copy={} mesh_probe_attempted={} mesh_probe_supported={} mesh_probe_executed={} reason={} final_gpu_only={} final_direct_zero_copy={} next_step={}",
                draw_log_count,
                backing.frame_id,
                program_handle,
                texture_handle,
                backing.direct_zero_copy || backing.copied_on_gpu,
                backing.direct_zero_copy,
                mesh_pipeline_probe.has_value() ? mesh_pipeline_probe->attempted : false,
                mesh_pipeline_probe.has_value() ? mesh_pipeline_probe->supported : false,
                video_vulkan_mesh_executed,
                mesh_pipeline_probe.has_value() ? mesh_pipeline_probe->reason : "mesh_pipeline_not_attempted"sv,
                video_vulkan_mesh_executed,
                video_vulkan_mesh_executed && backing.direct_zero_copy,
                video_vulkan_mesh_executed ? "continue_virtualizing_post_video_consumers"sv : "fix_video_vulkan_mesh_probe_before_claiming_gpu_only_playback"sv);
        }
        if (video_vulkan_mesh_executed)
            return true;
    }

    if (attached_shader_count > 0) {
        GLuint shaders[8] {};
        GLsizei shader_count = 0;
        glGetAttachedShaders(program_handle, min(attached_shader_count, 8), &shader_count, shaders);
        for (GLsizei index = 0; index < shader_count; ++index) {
            GLint shader_type = 0;
            GLint source_length = 0;
            GLint compile_status = 0;
            glGetShaderiv(shaders[index], GL_SHADER_TYPE, &shader_type);
            glGetShaderiv(shaders[index], GL_SHADER_SOURCE_LENGTH, &source_length);
            glGetShaderiv(shaders[index], GL_COMPILE_STATUS, &compile_status);
            if (should_log)
                dbgln("MUNDO_WEBGL_VIDEO_VULKAN_DIRECT_DRAW_SHADER count={} frame_id={} program={} shader={} shader_type={} source_length={} compile_status={} next_step=translate_or_reuse_shader_for_vulkan_video_sampler",
                draw_log_count,
                backing.frame_id,
                program_handle,
                shaders[index],
                shader_type,
                source_length,
                compile_status);
        }
    }
    return false;
}
#endif

static void log_mundo_webgl_video_sampler_uniforms(GLuint program_handle, GLuint video_texture_handle, Optional<WebGLTexture::HardwareVideoBacking> const& video_backing, size_t draw_log_count)
{
    if (!program_handle)
        return;

    GLint active_uniform_count = 0;
    glGetProgramivRobustANGLE(program_handle, GL_ACTIVE_UNIFORMS, 1, nullptr, &active_uniform_count);
    auto uniforms_to_scan = active_uniform_count < 32 ? active_uniform_count : 32;
    bool found_sampler_uniform = false;
    for (GLint index = 0; index < uniforms_to_scan; ++index) {
        GLint size = 0;
        GLenum type = 0;
        GLsizei length = 0;
        GLchar name[256];
        glGetActiveUniform(program_handle, static_cast<GLuint>(index), sizeof(name), &length, &size, &type, name);
        if (!length || !mundo_webgl_is_sampler_uniform_type(type))
            continue;

        found_sampler_uniform = true;
        auto location = glGetUniformLocation(program_handle, name);
        GLint unit = -1;
        if (location >= 0)
            glGetUniformiv(program_handle, location, &unit);
        auto uniform_name = String::from_utf8_without_validation(ReadonlyBytes { name, static_cast<size_t>(length) });
        GLint previous_active_texture = 0;
        GLint bound_texture_for_unit = 0;
        if (unit >= 0) {
            glGetIntegervRobustANGLE(GL_ACTIVE_TEXTURE, 1, nullptr, &previous_active_texture);
            glActiveTexture(GL_TEXTURE0 + unit);
            glGetIntegervRobustANGLE(GL_TEXTURE_BINDING_2D, 1, nullptr, &bound_texture_for_unit);
            glActiveTexture(previous_active_texture);
        }
        auto sampler_matches_video_texture = video_texture_handle != 0 && bound_texture_for_unit == static_cast<GLint>(video_texture_handle);
        dbgln("MUNDO_WEBGL_VIDEO_SAMPLER_UNIFORM draw_count={} program={} uniform={} type={} size={} location={} unit={} bound_texture={} video_texture={} sampler_matches_video_texture={} active_uniforms={} next_step=bind_nv12_planes_for_sampler_unit",
            draw_log_count,
            program_handle,
            uniform_name,
            type,
            size,
            location,
            unit,
            bound_texture_for_unit,
            video_texture_handle,
            sampler_matches_video_texture,
            active_uniform_count);
        if (sampler_matches_video_texture && video_backing.has_value()) {
            auto const& backing = video_backing.value();
            dbgln("MUNDO_WEBGL_VIDEO_ZERO_COPY_CANDIDATE_READY draw_count={} program={} uniform={} unit={} texture={} frame_id={} size={}x{} backend={} upload_mode={} copy_stage={} gpu_only={} direct_zero_copy={} direct_sampling_route={} direct_sampling_reason={} source_format={} source_layout={} source_allocation_size={} source_handle_type={} source_single_optimal_multiplanar={} blocker={} next_step={}",
                draw_log_count,
                program_handle,
                uniform_name,
                unit,
                video_texture_handle,
                backing.frame_id,
                backing.width,
                backing.height,
                backing.backend,
                backing.upload_mode,
                backing.copy_stage,
                backing.direct_zero_copy || backing.copied_on_gpu,
                backing.direct_zero_copy,
                backing.direct_sampling_route,
                backing.direct_sampling_reason,
                backing.source_vulkan_format,
                backing.source_vulkan_layout,
                backing.source_allocation_size,
                backing.source_handle_type,
                backing.source_single_optimal_multiplanar,
                backing.direct_zero_copy ? "none" : "webgl_sampler_consumes_rgba_texture_not_decoder_nv12_surface",
                backing.direct_zero_copy ? "verify_no_cpu_or_gpu_copy" : "replace_rgba_texture_backing_with_importable_decoder_surface_or_vulkan_sampler_path");
        }
    }

    if (found_sampler_uniform)
        return;

    dbgln("MUNDO_WEBGL_VIDEO_SAMPLER_UNIFORM_MISSING draw_count={} program={} active_uniforms={} scanned_uniforms={} reason=no_active_sampler_uniform_for_video_draw next_step=inspect_uniform_types",
        draw_log_count,
        program_handle,
        active_uniform_count,
        uniforms_to_scan);

    auto uniforms_to_log = uniforms_to_scan < 8 ? uniforms_to_scan : 8;
    for (GLint index = 0; index < uniforms_to_log; ++index) {
        GLint size = 0;
        GLenum type = 0;
        GLsizei length = 0;
        GLchar name[256];
        glGetActiveUniform(program_handle, static_cast<GLuint>(index), sizeof(name), &length, &size, &type, name);
        if (!length)
            continue;
        auto uniform_name = String::from_utf8_without_validation(ReadonlyBytes { name, static_cast<size_t>(length) });
        dbgln("MUNDO_WEBGL_VIDEO_UNIFORM_SAMPLE draw_count={} program={} index={} uniform={} type={} size={}",
            draw_log_count,
            program_handle,
            index,
            uniform_name,
            type,
            size);
    }
}

WebGLRenderingContextImpl::WebGLRenderingContextImpl(JS::Realm& realm, NonnullOwnPtr<OpenGLContext> context)
    : WebGLRenderingContextBase(realm)
    , m_context(move(context))
{
}

void WebGLRenderingContextImpl::active_texture(WebIDL::UnsignedLong texture)
{
    m_context->make_current();
    glActiveTexture(texture);
    if (texture >= GL_TEXTURE0)
        m_mundo_active_texture_unit_index = texture - GL_TEXTURE0;
}

void WebGLRenderingContextImpl::attach_shader(GC::Root<WebGLProgram> program, GC::Root<WebGLShader> shader)
{
    m_context->make_current();

    GLuint program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        program_handle = handle_or_error.release_value();
    }

    GLuint shader_handle = 0;
    if (shader) {
        auto handle_or_error = shader->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        shader_handle = handle_or_error.release_value();
    }

    if (program->attached_vertex_shader() == shader || program->attached_fragment_shader() == shader) {
        dbgln("WebGL: Shader is already attached to program");
        set_error(GL_INVALID_OPERATION);
        return;
    }

    if (shader->type() == GL_VERTEX_SHADER && program->attached_vertex_shader()) {
        dbgln("WebGL: Not attaching vertex shader to program as it already has a vertex shader attached");
        set_error(GL_INVALID_OPERATION);
        return;
    }

    if (shader->type() == GL_FRAGMENT_SHADER && program->attached_fragment_shader()) {
        dbgln("WebGL: Not attaching fragment shader to program as it already has a fragment shader attached");
        set_error(GL_INVALID_OPERATION);
        return;
    }

    glAttachShader(program_handle, shader_handle);

    switch (shader->type()) {
    case GL_VERTEX_SHADER:
        program->set_attached_vertex_shader(shader.ptr());
        break;
    case GL_FRAGMENT_SHADER:
        program->set_attached_fragment_shader(shader.ptr());
        break;
    default:
        VERIFY_NOT_REACHED();
    }
}

void WebGLRenderingContextImpl::bind_attrib_location(GC::Root<WebGLProgram> program, WebIDL::UnsignedLong index, String name)
{
    m_context->make_current();

    auto program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        program_handle = handle_or_error.release_value();
    }

    auto name_null_terminated = null_terminated_string(name);
    glBindAttribLocation(program_handle, index, name_null_terminated.data());
}

void WebGLRenderingContextImpl::bind_buffer(WebIDL::UnsignedLong target, GC::Root<WebGLBuffer> buffer)
{
    m_context->make_current();

    GLuint buffer_handle = 0;
    if (buffer) {
        auto handle_or_error = buffer->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        buffer_handle = handle_or_error.release_value();

        if (!buffer->is_compatible_with(target)) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
    }

    if (m_context->webgl_version() == OpenGLContext::WebGLVersion::WebGL2) {
        switch (target) {
        case GL_ARRAY_BUFFER:
            m_array_buffer_binding = buffer;
            break;
        case GL_COPY_READ_BUFFER:
            m_copy_read_buffer_binding = buffer;
            break;
        case GL_COPY_WRITE_BUFFER:
            m_copy_write_buffer_binding = buffer;
            break;
        case GL_ELEMENT_ARRAY_BUFFER:
            m_element_array_buffer_binding = buffer;
            break;
        case GL_PIXEL_PACK_BUFFER:
            m_pixel_pack_buffer_binding = buffer;
            break;
        case GL_PIXEL_UNPACK_BUFFER:
            m_pixel_unpack_buffer_binding = buffer;
            break;
        case GL_TRANSFORM_FEEDBACK_BUFFER:
            m_transform_feedback_buffer_binding = buffer;
            break;
        case GL_UNIFORM_BUFFER:
            m_uniform_buffer_binding = buffer;
            break;
        default:
            dbgln("Unknown WebGL buffer object binding target for storing current binding: 0x{:04x}", target);
            set_error(GL_INVALID_ENUM);
            return;
        }
    } else {
        switch (target) {
        case GL_ELEMENT_ARRAY_BUFFER:
            m_element_array_buffer_binding = buffer;
            break;
        case GL_ARRAY_BUFFER:
            m_array_buffer_binding = buffer;
            break;
        default:
            dbgln("Unknown WebGL buffer object binding target for storing current binding: 0x{:04x}", target);
            set_error(GL_INVALID_ENUM);
            return;
        }
    }

    glBindBuffer(target, buffer_handle);
}

GC::Ptr<WebGLBuffer> WebGLRenderingContextImpl::current_bound_buffer_for_target(WebIDL::UnsignedLong target) const
{
    switch (target) {
    case GL_ARRAY_BUFFER:
        return m_array_buffer_binding;
    case GL_ELEMENT_ARRAY_BUFFER:
        return m_element_array_buffer_binding;
    case GL_COPY_READ_BUFFER:
        return m_copy_read_buffer_binding;
    case GL_COPY_WRITE_BUFFER:
        return m_copy_write_buffer_binding;
    case GL_PIXEL_PACK_BUFFER:
        return m_pixel_pack_buffer_binding;
    case GL_PIXEL_UNPACK_BUFFER:
        return m_pixel_unpack_buffer_binding;
    case GL_TRANSFORM_FEEDBACK_BUFFER:
        return m_transform_feedback_buffer_binding;
    case GL_UNIFORM_BUFFER:
        return m_uniform_buffer_binding;
    default:
        return nullptr;
    }
}

GC::Ptr<WebGLBuffer> WebGLRenderingContextImpl::mundo_buffer_for_handle(GLuint handle) const
{
    if (handle >= m_mundo_buffer_by_handle.size())
        return nullptr;
    return m_mundo_buffer_by_handle[handle];
}

void WebGLRenderingContextImpl::bind_framebuffer(WebIDL::UnsignedLong target, GC::Root<WebGLFramebuffer> framebuffer)
{
    m_context->make_current();

    GLuint framebuffer_handle = 0;
    if (framebuffer) {
        auto handle_or_error = framebuffer->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        framebuffer_handle = handle_or_error.release_value();
    }

    glBindFramebuffer(target, framebuffer ? framebuffer_handle : m_context->default_framebuffer());
    m_framebuffer_binding = framebuffer;
}

bool WebGLRenderingContextImpl::note_mundo_framebuffer_draw(char const* operation, WebIDL::UnsignedLong mode, WebIDL::Long count, WebIDL::UnsignedLong type, WebIDL::LongLong offset, bool allow_vulkan_skip_gl_draw)
{
    if (!m_framebuffer_binding)
        return false;

    auto texture = m_framebuffer_binding->mundo_color_attachment_texture();
    if (!texture)
        return false;

    GLuint program_handle = 0;
    if (m_current_program) {
        auto program_handle_or_error = m_current_program->handle(this);
        if (!program_handle_or_error.is_error())
            program_handle = program_handle_or_error.release_value();
    }

    GLint viewport[4] {};
    glGetIntegervRobustANGLE(GL_VIEWPORT, 4, nullptr, viewport);
    auto replay_viewport_valid = viewport[2] > 0 && viewport[3] > 0;
    if (!replay_viewport_valid) {
        static size_t s_skipped_empty_render_target_draw_count { 0 };
        auto skipped_count = ++s_skipped_empty_render_target_draw_count;
        if (skipped_count <= 24 || skipped_count % 120 == 0) {
            auto texture_handle = texture->handle(this).value_or(0);
            auto framebuffer_handle = m_framebuffer_binding->handle(this).value_or(0);
            dbgln("MUNDO_WEBGL_SKIP_EMPTY_RENDER_TARGET_DRAW count={} op={} framebuffer={} color_texture={} program={} viewport={}x{}+{}+{} reason=empty_viewport_has_no_color_effect next_step=keep_previous_render_target_state_for_vulkan_consumers",
                skipped_count,
                operation,
                framebuffer_handle,
                texture_handle,
                program_handle,
                viewport[2],
                viewport[3],
                viewport[0],
                viewport[1]);
        }
        return false;
    }
    texture->mark_mundo_render_target_written(
        static_cast<u32>(viewport[2]),
        static_cast<u32>(viewport[3]),
        program_handle);

    auto render_target_state = texture->mundo_render_target_write_state();
    auto render_target_vulkan_stale = render_target_state.has_value()
        && render_target_state->vulkan_write_count > 0
        && render_target_state->vulkan_write_count < render_target_state->write_count;
    auto should_log_render_target_draw = render_target_state.has_value()
        && (render_target_state->write_count <= 24 || render_target_state->write_count % 120 == 0 || (render_target_vulkan_stale && render_target_state->write_count <= render_target_state->vulkan_write_count + 8));
    if (render_target_state.has_value() && should_log_render_target_draw) {
        auto texture_handle = texture->handle(this).value_or(0);
        auto framebuffer_handle = m_framebuffer_binding->handle(this).value_or(0);
        GLuint sampled_texture_handle = 0;
        bool sampled_texture_has_video_backing = false;
        bool sampled_texture_render_target_written = false;
        size_t sampled_texture_render_target_write_count = 0;
        bool sampled_texture_snapshot = false;
        bool sampled_texture_snapshot_complete = false;
        u32 sampled_texture_snapshot_width = 0;
        u32 sampled_texture_snapshot_height = 0;
        if (m_texture_binding_2d) {
            sampled_texture_handle = m_texture_binding_2d->handle(this).value_or(0);
            sampled_texture_has_video_backing = m_texture_binding_2d->has_hardware_video_backing();
            auto const& sampled_render_target_state = m_texture_binding_2d->mundo_render_target_write_state();
            sampled_texture_render_target_written = sampled_render_target_state.has_value();
            sampled_texture_render_target_write_count = sampled_render_target_state.has_value() ? sampled_render_target_state->write_count : 0;
            auto const& sampled_snapshot = m_texture_binding_2d->mundo_texture_upload_snapshot();
            sampled_texture_snapshot = sampled_snapshot.has_value();
            sampled_texture_snapshot_complete = sampled_snapshot.has_value() ? sampled_snapshot->complete : false;
            sampled_texture_snapshot_width = sampled_snapshot.has_value() ? sampled_snapshot->width : 0;
            sampled_texture_snapshot_height = sampled_snapshot.has_value() ? sampled_snapshot->height : 0;
        }
        dbgln("MUNDO_WEBGL_FRAMEBUFFER_RENDER_TARGET_DRAW op={} framebuffer={} color_texture={} write_count={} vulkan_backed={} vulkan_write_count={} vulkan_stale={} program={} viewport={}x{}+{}+{} sampled_texture={} sampled_video_backing={} sampled_render_target_written={} sampled_render_target_write_count={} sampled_snapshot={} sampled_snapshot_complete={} sampled_snapshot_size={}x{} reason=draw_wrote_to_tracked_color_attachment next_step={}",
            operation,
            framebuffer_handle,
            texture_handle,
            render_target_state->write_count,
            render_target_state->current_contents_vulkan_backed,
            render_target_state->vulkan_write_count,
            render_target_vulkan_stale,
            program_handle,
            viewport[2],
            viewport[3],
            viewport[0],
            viewport[1],
            sampled_texture_handle,
            sampled_texture_has_video_backing,
            sampled_texture_render_target_written,
            sampled_texture_render_target_write_count,
            sampled_texture_snapshot,
            sampled_texture_snapshot_complete,
            sampled_texture_snapshot_width,
            sampled_texture_snapshot_height,
            sampled_texture_has_video_backing ? "replay_render_target_producer_from_video_source"sv : sampled_texture_render_target_written ? "replay_render_target_chain"sv : "replay_or_import_generic_texture_source"sv);

        GLint active_uniform_count = 0;
        GLint active_attrib_count = 0;
        bool all_enabled_attrib_buffers_shadowed = true;
        size_t enabled_attrib_count = 0;
        if (program_handle) {
            glGetProgramivRobustANGLE(program_handle, GL_ACTIVE_UNIFORMS, 1, nullptr, &active_uniform_count);
            glGetProgramivRobustANGLE(program_handle, GL_ACTIVE_ATTRIBUTES, 1, nullptr, &active_attrib_count);
        }

        auto uniforms_to_log = active_uniform_count < 16 ? active_uniform_count : 16;
        size_t sampler_uniform_count = 0;
        size_t sampler_sources_resolved = 0;
        size_t sampler_sources_snapshot_complete = 0;
        size_t sampler_sources_render_target = 0;
        size_t sampler_sources_video = 0;
        GLuint render_target_sampler_texture_handle = 0;
        u32 render_target_sampler_width = 0;
        u32 render_target_sampler_height = 0;
#ifdef USE_VULKAN_DMABUF_IMAGES
        GC::Ptr<WebGLTexture> snapshot_sampler_texture;
        GLuint snapshot_sampler_texture_handle = 0;
        OpenGLContext::VulkanSolidMeshUniformSnapshot uniform_snapshot;
        for (size_t i = 0; i < 16; ++i) {
            uniform_snapshot.model_view_matrix[i] = (i % 5) == 0 ? 1.0f : 0.0f;
            uniform_snapshot.projection_matrix[i] = (i % 5) == 0 ? 1.0f : 0.0f;
        }
#endif
        for (GLint uniform_index = 0; uniform_index < uniforms_to_log; ++uniform_index) {
            GLint size = 0;
            GLenum type = 0;
            GLsizei length = 0;
            GLchar name[256];
            glGetActiveUniform(program_handle, static_cast<GLuint>(uniform_index), sizeof(name), &length, &size, &type, name);
            if (!length)
                continue;

            auto uniform_name = StringView { name, static_cast<size_t>(length) };
            auto is_sampler = type == GL_SAMPLER_2D || type == GL_SAMPLER_CUBE || type == GL_SAMPLER_3D || type == GL_SAMPLER_2D_ARRAY || type == GL_SAMPLER_2D_SHADOW || type == GL_SAMPLER_CUBE_SHADOW || type == GL_INT_SAMPLER_2D || type == GL_UNSIGNED_INT_SAMPLER_2D;
            auto location = glGetUniformLocation(program_handle, name);
#ifdef USE_VULKAN_DMABUF_IMAGES
            if (location >= 0) {
                if (type == GL_FLOAT_MAT4 && uniform_name == "modelViewMatrix"sv) {
                    glGetUniformfv(program_handle, location, uniform_snapshot.model_view_matrix.data());
                    uniform_snapshot.has_model_view_matrix = true;
                } else if (type == GL_FLOAT_MAT4 && uniform_name == "projectionMatrix"sv) {
                    glGetUniformfv(program_handle, location, uniform_snapshot.projection_matrix.data());
                    uniform_snapshot.has_projection_matrix = true;
                } else if (type == GL_FLOAT && (uniform_name == "opacity"sv || uniform_name == "uOpacity"sv)) {
                    glGetUniformfv(program_handle, location, &uniform_snapshot.opacity);
                } else if (type == GL_FLOAT && (uniform_name == "uOutputIntensity"sv || uniform_name == "outputIntensity"sv)) {
                    glGetUniformfv(program_handle, location, &uniform_snapshot.output_intensity);
                } else if ((type == GL_FLOAT_VEC3 || type == GL_FLOAT_VEC4) && (uniform_name == "diffuse"sv || uniform_name == "uTintColor"sv)) {
                    Array<float, 4> diffuse { 1.0f, 1.0f, 1.0f, 1.0f };
                    glGetUniformfv(program_handle, location, diffuse.data());
                    uniform_snapshot.diffuse = diffuse;
                } else if (type == GL_FLOAT_VEC3 && uniform_name == "uPosition"sv) {
                    Array<float, 3> position {};
                    glGetUniformfv(program_handle, location, position.data());
                    uniform_snapshot.ui_position_use_tint[0] = position[0];
                    uniform_snapshot.ui_position_use_tint[1] = position[1];
                    uniform_snapshot.ui_position_use_tint[2] = position[2];
                    uniform_snapshot.ui_transform[3] = 1.0f;
                } else if (type == GL_FLOAT_VEC2 && uniform_name == "uScale"sv) {
                    Array<float, 2> scale { 1.0f, 1.0f };
                    glGetUniformfv(program_handle, location, scale.data());
                    uniform_snapshot.ui_scale_clip[0] = scale[0];
                    uniform_snapshot.ui_scale_clip[1] = scale[1];
                    uniform_snapshot.ui_transform[3] = 1.0f;
                } else if (type == GL_FLOAT_VEC2 && uniform_name == "uClipRect"sv) {
                    Array<float, 2> clip {};
                    glGetUniformfv(program_handle, location, clip.data());
                    uniform_snapshot.ui_scale_clip[2] = clip[0];
                    uniform_snapshot.ui_scale_clip[3] = clip[1];
                } else if (type == GL_FLOAT && uniform_name == "uTransformRotateZ"sv) {
                    glGetUniformfv(program_handle, location, &uniform_snapshot.ui_transform[2]);
                    uniform_snapshot.ui_transform[3] = 1.0f;
                } else if (type == GL_FLOAT_VEC2 && uniform_name == "uTransformScale"sv) {
                    Array<float, 2> transform_scale { 1.0f, 1.0f };
                    glGetUniformfv(program_handle, location, transform_scale.data());
                    uniform_snapshot.ui_transform[0] = transform_scale[0];
                    uniform_snapshot.ui_transform[1] = transform_scale[1];
                    uniform_snapshot.ui_transform[3] = 1.0f;
                } else if (type == GL_FLOAT_VEC2 && uniform_name == "uLayoutCenter"sv) {
                    Array<float, 2> layout_center {};
                    glGetUniformfv(program_handle, location, layout_center.data());
                    uniform_snapshot.ui_layout_center[0] = layout_center[0];
                    uniform_snapshot.ui_layout_center[1] = layout_center[1];
                    uniform_snapshot.ui_transform[3] = 1.0f;
                } else if (type == GL_FLOAT && uniform_name == "uUseTint"sv) {
                    glGetUniformfv(program_handle, location, &uniform_snapshot.ui_position_use_tint[3]);
                }
            }
#endif
            GLint sampler_unit = -1;
            GLuint sampler_texture_handle = 0;
            bool sampler_texture_has_video_backing = false;
            bool sampler_texture_render_target_written = false;
            bool sampler_texture_snapshot_complete = false;
            u32 sampler_texture_snapshot_width = 0;
            u32 sampler_texture_snapshot_height = 0;
            if (is_sampler) {
                ++sampler_uniform_count;
                if (location >= 0) {
                    glGetUniformiv(program_handle, location, &sampler_unit);
                    if (sampler_unit >= 0 && static_cast<size_t>(sampler_unit) < m_mundo_texture_binding_2d_by_unit.size()) {
                        if (auto sampler_texture = m_mundo_texture_binding_2d_by_unit[static_cast<size_t>(sampler_unit)]) {
                            sampler_texture_handle = sampler_texture->handle(this).value_or(0);
                            sampler_texture_has_video_backing = sampler_texture->has_hardware_video_backing();
                            auto const& sampler_render_target_state = sampler_texture->mundo_render_target_write_state();
                            sampler_texture_render_target_written = sampler_render_target_state.has_value();
                            auto const& sampler_snapshot = sampler_texture->mundo_texture_upload_snapshot();
                            sampler_texture_snapshot_complete = sampler_snapshot.has_value() && sampler_snapshot->complete;
                            sampler_texture_snapshot_width = sampler_snapshot.has_value() ? sampler_snapshot->width : 0;
                            sampler_texture_snapshot_height = sampler_snapshot.has_value() ? sampler_snapshot->height : 0;
                            if (sampler_texture_has_video_backing) {
                                ++sampler_sources_resolved;
                                ++sampler_sources_video;
                            } else if (sampler_texture_render_target_written) {
                                ++sampler_sources_resolved;
                                ++sampler_sources_render_target;
                                if (!render_target_sampler_texture_handle && sampler_render_target_state.has_value()) {
                                    render_target_sampler_texture_handle = sampler_texture_handle;
                                    render_target_sampler_width = sampler_render_target_state->last_viewport_width;
                                    render_target_sampler_height = sampler_render_target_state->last_viewport_height;
                                }
                            } else if (sampler_texture_snapshot_complete) {
                                ++sampler_sources_resolved;
                                ++sampler_sources_snapshot_complete;
                                if (!snapshot_sampler_texture) {
                                    snapshot_sampler_texture = sampler_texture;
                                    snapshot_sampler_texture_handle = sampler_texture_handle;
                                }
                            }
                        }
                    }
                }
            }

            dbgln("MUNDO_WEBGL_RENDER_TARGET_REPLAY_UNIFORM color_texture={} write_count={} program={} index={} name={} type={} size={} sampler={} sampler_unit={} sampler_texture={} sampler_video_backing={} sampler_render_target_written={} sampler_snapshot_complete={} sampler_snapshot_size={}x{} next_step={}",
                texture_handle,
                render_target_state->write_count,
                program_handle,
                uniform_index,
                uniform_name,
                type,
                size,
                is_sampler,
                sampler_unit,
                sampler_texture_handle,
                sampler_texture_has_video_backing,
                sampler_texture_render_target_written,
                sampler_texture_snapshot_complete,
                sampler_texture_snapshot_width,
                sampler_texture_snapshot_height,
                !is_sampler ? "preserve_uniform_for_generic_vulkan_replay"sv
                    : sampler_texture_has_video_backing ? "bind_vulkan_ycbcr_video_source"sv
                    : sampler_texture_render_target_written ? "resolve_prior_render_target_chain"sv
                    : sampler_texture_snapshot_complete ? "upload_or_import_static_texture_snapshot"sv
                    : "capture_missing_sampler_texture_source"sv);
        }

        auto attribs_to_log = active_attrib_count < 8 ? active_attrib_count : 8;
#ifdef USE_VULKAN_DMABUF_IMAGES
        bool has_position_attrib = false;
        bool has_color_attrib = false;
        bool has_uv_attrib = false;
        bool position_layout_supported = false;
        bool color_layout_supported = false;
        bool uv_layout_supported = false;
        ReadonlyBytes position_data;
        ReadonlyBytes color_data;
        ReadonlyBytes uv_data;
#endif
        for (GLint attrib_index = 0; attrib_index < attribs_to_log; ++attrib_index) {
            GLint size = 0;
            GLenum type = 0;
            GLsizei length = 0;
            GLchar name[256];
            glGetActiveAttrib(program_handle, static_cast<GLuint>(attrib_index), sizeof(name), &length, &size, &type, name);
            if (!length)
                continue;

            auto attribute_name = StringView { name, static_cast<size_t>(length) };
#ifdef USE_VULKAN_DMABUF_IMAGES
            if (attribute_name == "position"sv)
                has_position_attrib = true;
            else if (attribute_name == "color"sv)
                has_color_attrib = true;
            else if (attribute_name == "uv"sv)
                has_uv_attrib = true;
#endif
            auto location = glGetAttribLocation(program_handle, name);
            GLint enabled = 0;
            GLint array_size = 0;
            GLint array_type = 0;
            GLint normalized = 0;
            GLint stride = 0;
            GLint buffer = 0;
            void* pointer = nullptr;
            bool buffer_shadow_complete = false;
            size_t buffer_shadow_bytes = 0;
            if (location >= 0) {
                glGetVertexAttribivRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_ENABLED, 1, nullptr, &enabled);
                glGetVertexAttribivRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_SIZE, 1, nullptr, &array_size);
                glGetVertexAttribivRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_TYPE, 1, nullptr, &array_type);
                glGetVertexAttribivRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, 1, nullptr, &normalized);
                glGetVertexAttribivRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_STRIDE, 1, nullptr, &stride);
                glGetVertexAttribivRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, 1, nullptr, &buffer);
                glGetVertexAttribPointervRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_POINTER, 1, nullptr, &pointer);
                if (auto webgl_buffer = mundo_buffer_for_handle(static_cast<GLuint>(buffer))) {
                    buffer_shadow_complete = webgl_buffer->has_complete_shadow_data();
                    buffer_shadow_bytes = webgl_buffer->shadow_byte_length();
#ifdef USE_VULKAN_DMABUF_IMAGES
                    if (buffer_shadow_complete) {
                        auto layout_supported = enabled && array_size == 3 && array_type == GL_FLOAT && stride == static_cast<GLint>(sizeof(float) * 3) && reinterpret_cast<uintptr_t>(pointer) == 0;
                        if (attribute_name == "position"sv) {
                            position_layout_supported = layout_supported;
                            if (layout_supported)
                                position_data = webgl_buffer->shadow_data();
                        } else if (attribute_name == "color"sv) {
                            color_layout_supported = layout_supported;
                            if (layout_supported)
                                color_data = webgl_buffer->shadow_data();
                        } else if (attribute_name == "uv"sv) {
                            uv_layout_supported = enabled && array_size == 2 && array_type == GL_FLOAT && stride == static_cast<GLint>(sizeof(float) * 2) && reinterpret_cast<uintptr_t>(pointer) == 0;
                            if (uv_layout_supported)
                                uv_data = webgl_buffer->shadow_data();
                        }
                    }
#endif
                }
                if (enabled) {
                    ++enabled_attrib_count;
                    all_enabled_attrib_buffers_shadowed &= buffer_shadow_complete;
                }
            }

            dbgln("MUNDO_WEBGL_RENDER_TARGET_REPLAY_ATTRIB color_texture={} write_count={} program={} index={} name={} location={} declared_size={} declared_type={} enabled={} array_size={} array_type={} normalized={} stride={} pointer_offset={} buffer={} buffer_shadow_complete={} buffer_shadow_bytes={} next_step={}",
                texture_handle,
                render_target_state->write_count,
                program_handle,
                attrib_index,
                attribute_name,
                location,
                size,
                type,
                enabled,
                array_size,
                array_type,
                normalized,
                stride,
                reinterpret_cast<uintptr_t>(pointer),
                buffer,
                buffer_shadow_complete,
                buffer_shadow_bytes,
                buffer_shadow_complete ? "map_shadowed_vertex_input_to_generic_vulkan_replay" : "capture_or_share_vertex_buffer_before_generic_replay");
        }

        auto element_buffer_handle = m_element_array_buffer_binding ? m_element_array_buffer_binding->handle(this).value_or(0) : 0;
        auto element_shadow_complete = m_element_array_buffer_binding ? m_element_array_buffer_binding->has_complete_shadow_data() : false;
        auto element_shadow_bytes = m_element_array_buffer_binding ? m_element_array_buffer_binding->shadow_byte_length() : 0;
        ReadonlyBytes element_data;
        if (m_element_array_buffer_binding && element_shadow_complete)
            element_data = m_element_array_buffer_binding->shadow_data();
        auto requires_element_buffer = !strcmp(operation, "drawElements");
        auto sampler_sources_ready = sampler_uniform_count == sampler_sources_resolved;
        auto generic_replay_inputs_ready = all_enabled_attrib_buffers_shadowed && sampler_sources_ready && (!requires_element_buffer || element_shadow_complete);
        dbgln("MUNDO_WEBGL_RENDER_TARGET_REPLAY_INPUTS_READY op={} color_texture={} write_count={} program={} ready={} reason={} active_uniforms={} logged_uniforms={} sampler_uniforms={} sampler_sources_resolved={} sampler_sources_snapshot_complete={} sampler_sources_render_target={} sampler_sources_video={} active_attribs={} enabled_attribs={} attrib_buffers_shadowed={} requires_element_buffer={} element_buffer={} element_shadow_complete={} element_shadow_bytes={} next_step={}",
            operation,
            texture_handle,
            render_target_state->write_count,
            program_handle,
            generic_replay_inputs_ready,
            !all_enabled_attrib_buffers_shadowed ? "missing_vertex_buffer_shadow"sv
                : !sampler_sources_ready ? "missing_sampler_texture_source"sv
                : requires_element_buffer && !element_shadow_complete ? "missing_index_buffer_shadow"sv
                : sampler_sources_render_target > 0 ? "resolve_render_target_dependency_chain"sv
                : sampler_sources_video > 0 ? "generic_replay_can_bind_video_source"sv
                : "ready_for_static_texture_generic_vulkan_replay"sv,
            active_uniform_count,
            uniforms_to_log,
            sampler_uniform_count,
            sampler_sources_resolved,
            sampler_sources_snapshot_complete,
            sampler_sources_render_target,
            sampler_sources_video,
            active_attrib_count,
            enabled_attrib_count,
            all_enabled_attrib_buffers_shadowed,
            requires_element_buffer,
            element_buffer_handle,
            element_shadow_complete,
            element_shadow_bytes,
            generic_replay_inputs_ready ? "build_generic_vulkan_render_target_pipeline" : "complete_missing_replay_prerequisites");
#ifdef USE_VULKAN_DMABUF_IMAGES
        static size_t s_colored_render_target_attempt_count { 0 };
        auto colored_attempt_count = ++s_colored_render_target_attempt_count;
        auto colored_replay_to_texture_enabled = mundo_webgl_env_enabled_by_default("MUNDO_WEBGL_RENDER_TARGET_VULKAN_COLORED_REPLAY_TO_TEXTURE");
        auto colored_replay_enabled = colored_replay_to_texture_enabled && mundo_webgl_env_enabled_by_default("MUNDO_WEBGL_RENDER_TARGET_VULKAN_COLORED_REPLAY");
        auto complex_colored_replay_enabled = mundo_webgl_env_enabled_by_default("MUNDO_WEBGL_RENDER_TARGET_VULKAN_COMPLEX_COLORED_REPLAY");
        auto colored_uniforms_supported = active_uniform_count <= 5 || complex_colored_replay_enabled;
        auto colored_replay_possible = !strcmp(operation, "drawElements")
            && replay_viewport_valid
            && mode == GL_TRIANGLES
            && active_attrib_count == 2
            && sampler_uniform_count == 0
            && colored_uniforms_supported
            && has_position_attrib
            && has_color_attrib
            && position_layout_supported
            && color_layout_supported
            && element_shadow_complete
            && (type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT);
        auto destination_format = m_context->vulkan_painting_surface_format();
        auto should_log_colored_replay = colored_attempt_count <= 24 || colored_attempt_count % 120 == 0 || render_target_vulkan_stale;
        if (should_log_colored_replay) {
            dbgln("MUNDO_WEBGL_RENDER_TARGET_COLORED_REPLAY_ATTEMPT count={} color_texture={} write_count={} vulkan_backed={} vulkan_write_count={} vulkan_stale={} program={} enabled={} to_texture_enabled={} complex_enabled={} possible={} operation={} mode={} draw_count={} type={} offset={} active_attribs={} active_uniforms={} sampler_uniforms={} colored_uniforms_supported={} has_position={} has_color={} position_layout_supported={} color_layout_supported={} position_bytes={} color_bytes={} element_ready={} element_bytes={} destination_format={} reason={} next_step={}",
                colored_attempt_count,
                texture_handle,
                render_target_state->write_count,
                render_target_state->current_contents_vulkan_backed,
                render_target_state->vulkan_write_count,
                render_target_vulkan_stale,
                program_handle,
                colored_replay_enabled,
                colored_replay_to_texture_enabled,
                complex_colored_replay_enabled,
                colored_replay_possible,
                operation,
                mode,
                count,
                type,
                offset,
                active_attrib_count,
                active_uniform_count,
                sampler_uniform_count,
                colored_uniforms_supported,
                has_position_attrib,
                has_color_attrib,
                position_layout_supported,
                color_layout_supported,
                position_data.size(),
                color_data.size(),
                element_shadow_complete,
                element_data.size(),
                destination_format.value_or(0),
                strcmp(operation, "drawElements") ? "not_draw_elements"sv
                    : !replay_viewport_valid ? "invalid_viewport"sv
                    : mode != GL_TRIANGLES ? "unsupported_primitive_mode"sv
                    : active_attrib_count != 2 ? "not_two_attrib_colored_mesh"sv
                    : sampler_uniform_count != 0 ? "sampler_based_draw_not_colored_mesh"sv
                    : !colored_uniforms_supported ? "unsupported_complex_colored_shader_uniform_shape"sv
                    : !has_position_attrib ? "missing_position_attrib"sv
                    : !has_color_attrib ? "missing_color_attrib"sv
                    : !position_layout_supported ? "unsupported_position_layout"sv
                    : !color_layout_supported ? "unsupported_color_layout"sv
                    : !element_shadow_complete ? "missing_index_shadow"sv
                    : !(type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT) ? "unsupported_index_type"sv
                    : !destination_format.has_value() ? "missing_vulkan_target_format"sv
                    : !colored_replay_enabled ? "colored_replay_explicitly_disabled"sv
                    : "ready"sv,
                colored_replay_possible && colored_replay_enabled ? (colored_replay_to_texture_enabled ? "execute_colored_vulkan_mesh_to_render_target_texture"sv : "execute_colored_vulkan_mesh_probe_to_painting_surface"sv) : "keep_collecting_colored_render_target_inputs"sv);
        }
        if (colored_replay_enabled && colored_replay_possible && destination_format.has_value()) {
            OpenGLContext::ImportedVideoOpaqueFDTexture* imported_render_target_texture = nullptr;
            if (colored_replay_to_texture_enabled) {
                auto imported_texture_or_error = m_context->get_or_create_vulkan_rgba_render_target_image(texture_handle, static_cast<u32>(viewport[2]), static_cast<u32>(viewport[3]), colored_attempt_count);
                if (imported_texture_or_error.is_error()) {
                    if (should_log_colored_replay) {
                        dbgln("MUNDO_WEBGL_RENDER_TARGET_COLORED_REPLAY_TARGET_IMPORT count={} color_texture={} write_count={} vulkan_write_count={} program={} status=failed reason={} route=vulkan_offscreen_image next_step=fix_vulkan_offscreen_image_before_colored_replay",
                            colored_attempt_count,
                            texture_handle,
                            render_target_state->write_count,
                            render_target_state->vulkan_write_count,
                            program_handle,
                            imported_texture_or_error.error().string_literal());
                    }
                } else {
                    imported_render_target_texture = imported_texture_or_error.release_value();
                    if (should_log_colored_replay) {
                        dbgln("MUNDO_WEBGL_RENDER_TARGET_COLORED_REPLAY_TARGET_IMPORT count={} color_texture={} write_count={} vulkan_write_count={} program={} status=ok route=vulkan_offscreen_image imported_texture={} size={}x{} allocation_size={} next_step=draw_colored_mesh_into_vulkan_render_target_image",
                            colored_attempt_count,
                            texture_handle,
                            render_target_state->write_count,
                            render_target_state->vulkan_write_count,
                            program_handle,
                            imported_render_target_texture->texture,
                            imported_render_target_texture->width,
                            imported_render_target_texture->height,
                            imported_render_target_texture->allocation_size);
                    }
                }
            }
            if (colored_replay_to_texture_enabled && !imported_render_target_texture)
                return false;
            auto* target_image_override = imported_render_target_texture ? imported_render_target_texture->image.ptr() : nullptr;
            auto target_format = target_image_override ? to_underlying(target_image_override->info.format) : destination_format.value();
            auto colored_pipeline_probe = m_context->probe_vulkan_colored_mesh_pipeline(target_format, uniform_snapshot, position_data, color_data, element_data, count, type, static_cast<GLintptr>(offset), viewport[0], viewport[1], viewport[2], viewport[3], colored_attempt_count, target_image_override);
            if (should_log_colored_replay) {
                dbgln("MUNDO_WEBGL_RENDER_TARGET_COLORED_REPLAY_PROBE_RESULT count={} color_texture={} write_count={} vulkan_write_count={} program={} to_texture={} attempted={} supported={} executed={} reason={} next_step={}",
                    colored_attempt_count,
                    texture_handle,
                    render_target_state->write_count,
                    render_target_state->vulkan_write_count,
                    program_handle,
                    target_image_override != nullptr,
                    colored_pipeline_probe.attempted,
                    colored_pipeline_probe.supported,
                    colored_pipeline_probe.executed,
                    colored_pipeline_probe.reason,
                    colored_pipeline_probe.executed ? "move_colored_mesh_output_from_painting_surface_probe_to_offscreen_render_target_image" : "fix_colored_mesh_probe_before_offscreen_target");
            }
            if (target_image_override && colored_pipeline_probe.executed) {
                texture->mark_mundo_render_target_vulkan_backed();
                if (allow_vulkan_skip_gl_draw)
                    return true;
            }
        }

        static size_t s_static_textured_render_target_attempt_count { 0 };
        auto static_textured_attempt_count = ++s_static_textured_render_target_attempt_count;
        auto static_textured_replay_enabled = mundo_webgl_env_enabled_by_default("MUNDO_WEBGL_RENDER_TARGET_VULKAN_STATIC_TEXTURED_REPLAY");
        auto static_textured_replay_possible = !strcmp(operation, "drawElements")
            && replay_viewport_valid
            && mode == GL_TRIANGLES
            && active_attrib_count == 2
            && sampler_uniform_count == 1
            && sampler_sources_snapshot_complete == 1
            && snapshot_sampler_texture
            && snapshot_sampler_texture_handle
            && has_position_attrib
            && has_uv_attrib
            && position_layout_supported
            && uv_layout_supported
            && element_shadow_complete
            && (type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT);
        auto should_log_static_textured_replay = static_textured_attempt_count <= 24 || static_textured_attempt_count % 120 == 0 || render_target_vulkan_stale;
        if (should_log_static_textured_replay) {
            dbgln("MUNDO_WEBGL_RENDER_TARGET_STATIC_TEXTURED_REPLAY_ATTEMPT count={} color_texture={} write_count={} vulkan_backed={} vulkan_write_count={} vulkan_stale={} program={} enabled={} possible={} operation={} mode={} draw_count={} type={} offset={} active_attribs={} active_uniforms={} sampler_uniforms={} sampler_sources_snapshot_complete={} snapshot_texture={} has_position={} has_uv={} position_layout_supported={} uv_layout_supported={} position_bytes={} uv_bytes={} element_ready={} element_bytes={} destination_format={} reason={} next_step={}",
                static_textured_attempt_count,
                texture_handle,
                render_target_state->write_count,
                render_target_state->current_contents_vulkan_backed,
                render_target_state->vulkan_write_count,
                render_target_vulkan_stale,
                program_handle,
                static_textured_replay_enabled,
                static_textured_replay_possible,
                operation,
                mode,
                count,
                type,
                offset,
                active_attrib_count,
                active_uniform_count,
                sampler_uniform_count,
                sampler_sources_snapshot_complete,
                snapshot_sampler_texture_handle,
                has_position_attrib,
                has_uv_attrib,
                position_layout_supported,
                uv_layout_supported,
                position_data.size(),
                uv_data.size(),
                element_shadow_complete,
                element_data.size(),
                destination_format.value_or(0),
                strcmp(operation, "drawElements") ? "not_draw_elements"sv
                    : !replay_viewport_valid ? "invalid_viewport"sv
                    : mode != GL_TRIANGLES ? "unsupported_primitive_mode"sv
                    : active_attrib_count != 2 ? "not_position_uv_mesh"sv
                    : sampler_uniform_count != 1 ? "not_single_sampler_producer"sv
                    : sampler_sources_snapshot_complete != 1 ? "sampler_not_single_static_snapshot_source"sv
                    : !snapshot_sampler_texture ? "missing_snapshot_sampler_texture"sv
                    : !snapshot_sampler_texture_handle ? "missing_snapshot_sampler_texture_handle"sv
                    : !has_position_attrib ? "missing_position_attrib"sv
                    : !has_uv_attrib ? "missing_uv_attrib"sv
                    : !position_layout_supported ? "unsupported_position_layout"sv
                    : !uv_layout_supported ? "unsupported_uv_layout"sv
                    : !element_shadow_complete ? "missing_index_shadow"sv
                    : !(type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT) ? "unsupported_index_type"sv
                    : !destination_format.has_value() ? "missing_vulkan_target_format"sv
                    : !static_textured_replay_enabled ? "static_textured_replay_explicitly_disabled"sv
                    : "ready"sv,
                static_textured_replay_possible && static_textured_replay_enabled ? "execute_static_textured_vulkan_mesh_to_render_target_texture"sv : "keep_collecting_static_textured_render_target_inputs"sv);
        }
        if (static_textured_replay_enabled && static_textured_replay_possible && destination_format.has_value()) {
            auto const& snapshot = snapshot_sampler_texture->mundo_texture_upload_snapshot();
            if (!snapshot.has_value() || !snapshot->complete) {
                if (should_log_static_textured_replay) {
                    dbgln("MUNDO_WEBGL_RENDER_TARGET_STATIC_TEXTURED_REPLAY_SOURCE_IMPORT count={} color_texture={} write_count={} program={} snapshot_texture={} status=failed reason=static_sampler_snapshot_missing route=static_texture_snapshot next_step=capture_complete_static_sampler_snapshot_before_replay",
                        static_textured_attempt_count,
                        texture_handle,
                        render_target_state->write_count,
                        program_handle,
                        snapshot_sampler_texture_handle);
                }
            } else {
                Optional<ByteBuffer> converted_rgba_pixels;
                ReadonlyBytes rgba_pixels;
                auto const expected_rgba_size = static_cast<size_t>(snapshot->width) * static_cast<size_t>(snapshot->height) * 4;
                if (snapshot->format == GL_RGBA && snapshot->type == GL_UNSIGNED_BYTE && snapshot->pixels.size() >= expected_rgba_size) {
                    rgba_pixels = snapshot->pixels.bytes().slice(0, expected_rgba_size);
                } else if ((snapshot->format == GL_ALPHA || snapshot->format == GL_LUMINANCE) && snapshot->type == GL_UNSIGNED_BYTE && snapshot->pixels.size() >= static_cast<size_t>(snapshot->width) * static_cast<size_t>(snapshot->height)) {
                    converted_rgba_pixels = ByteBuffer::create_uninitialized(expected_rgba_size).release_value_but_fixme_should_propagate_errors();
                    auto source_pixels = snapshot->pixels.bytes();
                    auto destination_pixels = converted_rgba_pixels->bytes();
                    for (size_t i = 0; i < static_cast<size_t>(snapshot->width) * static_cast<size_t>(snapshot->height); ++i) {
                        auto value = source_pixels[i];
                        destination_pixels[i * 4 + 0] = value;
                        destination_pixels[i * 4 + 1] = value;
                        destination_pixels[i * 4 + 2] = value;
                        destination_pixels[i * 4 + 3] = value;
                    }
                    rgba_pixels = converted_rgba_pixels->bytes();
                } else if (should_log_static_textured_replay) {
                    dbgln("MUNDO_WEBGL_RENDER_TARGET_STATIC_TEXTURED_REPLAY_SOURCE_IMPORT count={} color_texture={} write_count={} program={} snapshot_texture={} status=failed reason=unsupported_static_sampler_snapshot_format snapshot_size={}x{} internal_format={} format={} type={} byte_length={} route=static_texture_snapshot next_step=add_snapshot_format_conversion_for_static_textured_render_target_replay",
                        static_textured_attempt_count,
                        texture_handle,
                        render_target_state->write_count,
                        program_handle,
                        snapshot_sampler_texture_handle,
                        snapshot->width,
                        snapshot->height,
                        snapshot->internal_format,
                        snapshot->format,
                        snapshot->type,
                        snapshot->byte_length);
                }

                if (!rgba_pixels.is_empty()) {
                    auto signature = pair_int_hash(Traits<ReadonlyBytes>::hash(rgba_pixels), pair_int_hash(u32_hash(snapshot->width), pair_int_hash(u32_hash(snapshot->height), u32_hash(rgba_pixels.size()))));
                    auto source_image_or_error = m_context->get_or_create_vulkan_rgba_static_texture_image(snapshot_sampler_texture_handle, snapshot->width, snapshot->height, signature, rgba_pixels, static_textured_attempt_count);
                    if (source_image_or_error.is_error()) {
                        if (should_log_static_textured_replay) {
                            dbgln("MUNDO_WEBGL_RENDER_TARGET_STATIC_TEXTURED_REPLAY_SOURCE_IMPORT count={} color_texture={} write_count={} program={} snapshot_texture={} status=failed reason={} route=static_texture_snapshot next_step=fix_static_texture_vulkan_upload_before_replay",
                                static_textured_attempt_count,
                                texture_handle,
                                render_target_state->write_count,
                                program_handle,
                                snapshot_sampler_texture_handle,
                                source_image_or_error.error().string_literal());
                        }
                    } else {
                        auto target_image_or_error = m_context->get_or_create_vulkan_rgba_render_target_image(texture_handle, static_cast<u32>(viewport[2]), static_cast<u32>(viewport[3]), static_textured_attempt_count);
                        if (target_image_or_error.is_error()) {
                            if (should_log_static_textured_replay) {
                                dbgln("MUNDO_WEBGL_RENDER_TARGET_STATIC_TEXTURED_REPLAY_TARGET_IMPORT count={} color_texture={} write_count={} program={} status=failed reason={} route=vulkan_offscreen_image next_step=fix_vulkan_offscreen_target_before_static_textured_replay",
                                    static_textured_attempt_count,
                                    texture_handle,
                                    render_target_state->write_count,
                                    program_handle,
                                    target_image_or_error.error().string_literal());
                            }
                        } else {
                            auto* source_image_texture = source_image_or_error.release_value();
                            auto* target_image_texture = target_image_or_error.release_value();
                            auto* target_image_override = target_image_texture->image.ptr();
                            auto target_format = to_underlying(target_image_override->info.format);
                            auto textured_pipeline_probe = m_context->probe_vulkan_textured_mesh_pipeline(target_format, *source_image_texture->image, uniform_snapshot, position_data, uv_data, element_data, count, type, static_cast<GLintptr>(offset), viewport[0], viewport[1], viewport[2], viewport[3], static_textured_attempt_count, nullptr, target_image_override);
                            if (should_log_static_textured_replay) {
                                dbgln("MUNDO_WEBGL_RENDER_TARGET_STATIC_TEXTURED_REPLAY_PROBE_RESULT count={} color_texture={} write_count={} vulkan_write_count={} program={} snapshot_texture={} source_size={}x{} target_size={}x{} attempted={} supported={} executed={} reason={} next_step={}",
                                    static_textured_attempt_count,
                                    texture_handle,
                                    render_target_state->write_count,
                                    render_target_state->vulkan_write_count,
                                    program_handle,
                                    snapshot_sampler_texture_handle,
                                    snapshot->width,
                                    snapshot->height,
                                    target_image_texture->width,
                                    target_image_texture->height,
                                    textured_pipeline_probe.attempted,
                                    textured_pipeline_probe.supported,
                                    textured_pipeline_probe.executed,
                                    textured_pipeline_probe.reason,
                                    textured_pipeline_probe.executed ? "verify_static_textured_render_target_consumers_can_stay_vulkan_backed" : "fix_static_textured_mesh_probe_before_replacing_gl_draw");
                            }
                            if (textured_pipeline_probe.executed) {
                                texture->mark_mundo_render_target_vulkan_backed();
                                if (allow_vulkan_skip_gl_draw)
                                    return true;
                            }
                        }
                    }
                }
            }
        }

        static size_t s_solid_render_target_attempt_count { 0 };
        auto solid_attempt_count = ++s_solid_render_target_attempt_count;
        auto solid_replay_to_texture_enabled = mundo_webgl_env_enabled_by_default("MUNDO_WEBGL_RENDER_TARGET_VULKAN_SOLID_REPLAY_TO_TEXTURE");
        auto solid_replay_enabled = solid_replay_to_texture_enabled && mundo_webgl_env_enabled_by_default("MUNDO_WEBGL_RENDER_TARGET_VULKAN_SOLID_REPLAY");
        auto solid_uniforms_supported = active_uniform_count <= 5;
        auto solid_replay_max_default_area = mundo_webgl_env_size_value("MUNDO_WEBGL_RENDER_TARGET_VULKAN_SOLID_REPLAY_MAX_DEFAULT_PIXELS", 1024 * 1024);
        auto solid_replay_area = replay_viewport_valid ? static_cast<size_t>(viewport[2]) * static_cast<size_t>(viewport[3]) : 0;
        auto solid_replay_area_supported = solid_replay_area <= solid_replay_max_default_area
            || mundo_webgl_env_flag_enabled("MUNDO_WEBGL_RENDER_TARGET_VULKAN_SOLID_REPLAY_ALLOW_LARGE_TARGETS");
        auto solid_replay_possible = !strcmp(operation, "drawElements")
            && replay_viewport_valid
            && mode == GL_TRIANGLES
            && active_attrib_count == 1
            && sampler_uniform_count == 0
            && solid_uniforms_supported
            && solid_replay_area_supported
            && has_position_attrib
            && position_layout_supported
            && element_shadow_complete
            && (type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT);
        if (solid_attempt_count <= 24 || solid_attempt_count % 120 == 0) {
            dbgln("MUNDO_WEBGL_RENDER_TARGET_SOLID_REPLAY_ATTEMPT count={} color_texture={} write_count={} program={} enabled={} to_texture_enabled={} possible={} operation={} mode={} draw_count={} type={} offset={} active_attribs={} active_uniforms={} sampler_uniforms={} solid_uniforms_supported={} solid_area={} solid_max_default_area={} solid_area_supported={} has_position={} position_layout_supported={} position_bytes={} element_ready={} element_bytes={} destination_format={} reason={} next_step={}",
                solid_attempt_count,
                texture_handle,
                render_target_state->write_count,
                program_handle,
                solid_replay_enabled,
                solid_replay_to_texture_enabled,
                solid_replay_possible,
                operation,
                mode,
                count,
                type,
                offset,
                active_attrib_count,
                active_uniform_count,
                sampler_uniform_count,
                solid_uniforms_supported,
                solid_replay_area,
                solid_replay_max_default_area,
                solid_replay_area_supported,
                has_position_attrib,
                position_layout_supported,
                position_data.size(),
                element_shadow_complete,
                element_data.size(),
                destination_format.value_or(0),
                strcmp(operation, "drawElements") ? "not_draw_elements"sv
                    : !replay_viewport_valid ? "invalid_viewport"sv
                    : mode != GL_TRIANGLES ? "unsupported_primitive_mode"sv
                    : active_attrib_count != 1 ? "not_single_position_mesh"sv
                    : sampler_uniform_count != 0 ? "sampler_based_draw_not_solid_mesh"sv
                    : !solid_uniforms_supported ? "unsupported_solid_shader_uniform_shape"sv
                    : !solid_replay_area_supported ? "solid_target_too_large_for_default_replay"sv
                    : !has_position_attrib ? "missing_position_attrib"sv
                    : !position_layout_supported ? "unsupported_position_layout"sv
                    : !element_shadow_complete ? "missing_index_shadow"sv
                    : !(type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT) ? "unsupported_index_type"sv
                    : !destination_format.has_value() ? "missing_vulkan_target_format"sv
                    : !solid_replay_enabled ? "solid_replay_explicitly_disabled"sv
                    : "ready"sv,
                solid_replay_possible && solid_replay_enabled ? "execute_solid_vulkan_mesh_to_render_target_texture"sv : "keep_collecting_solid_render_target_inputs"sv);
        }
        if (solid_replay_enabled && solid_replay_possible && destination_format.has_value()) {
            auto imported_texture_or_error = m_context->get_or_create_vulkan_rgba_render_target_image(texture_handle, static_cast<u32>(viewport[2]), static_cast<u32>(viewport[3]), solid_attempt_count);
            if (imported_texture_or_error.is_error()) {
                if (solid_attempt_count <= 24 || solid_attempt_count % 120 == 0) {
                    dbgln("MUNDO_WEBGL_RENDER_TARGET_SOLID_REPLAY_TARGET_IMPORT count={} color_texture={} write_count={} program={} status=failed reason={} route=vulkan_offscreen_image next_step=fix_vulkan_offscreen_image_before_solid_replay",
                        solid_attempt_count,
                        texture_handle,
                        render_target_state->write_count,
                        program_handle,
                        imported_texture_or_error.error().string_literal());
                }
            } else {
                auto* imported_render_target_texture = imported_texture_or_error.release_value();
                if (solid_attempt_count <= 24 || solid_attempt_count % 120 == 0) {
                    dbgln("MUNDO_WEBGL_RENDER_TARGET_SOLID_REPLAY_TARGET_IMPORT count={} color_texture={} write_count={} program={} status=ok route=vulkan_offscreen_image imported_texture={} size={}x{} allocation_size={} next_step=draw_solid_mesh_into_vulkan_render_target_image",
                        solid_attempt_count,
                        texture_handle,
                        render_target_state->write_count,
                        program_handle,
                        imported_render_target_texture->texture,
                        imported_render_target_texture->width,
                        imported_render_target_texture->height,
                        imported_render_target_texture->allocation_size);
                }
                auto* target_image_override = imported_render_target_texture->image.ptr();
                auto target_format = to_underlying(target_image_override->info.format);
                auto solid_pipeline_probe = m_context->probe_vulkan_solid_mesh_pipeline(target_format, uniform_snapshot, position_data, element_data, count, type, static_cast<GLintptr>(offset), viewport[0], viewport[1], viewport[2], viewport[3], solid_attempt_count, false, 0, 0, target_image_override);
                if (solid_attempt_count <= 24 || solid_attempt_count % 120 == 0) {
                    dbgln("MUNDO_WEBGL_RENDER_TARGET_SOLID_REPLAY_PROBE_RESULT count={} color_texture={} write_count={} program={} to_texture=true attempted={} supported={} executed={} reason={} next_step={}",
                        solid_attempt_count,
                        texture_handle,
                        render_target_state->write_count,
                        program_handle,
                        solid_pipeline_probe.attempted,
                        solid_pipeline_probe.supported,
                        solid_pipeline_probe.executed,
                        solid_pipeline_probe.reason,
                        solid_pipeline_probe.executed ? "verify_solid_render_target_consumers_can_sample_imported_texture" : "fix_solid_mesh_probe_before_offscreen_target");
                }
                if (solid_pipeline_probe.executed) {
                    texture->mark_mundo_render_target_vulkan_backed();
                    if (allow_vulkan_skip_gl_draw)
                        return true;
                }
            }
        }

        static size_t s_textured_render_target_attempt_count { 0 };
        auto textured_attempt_count = ++s_textured_render_target_attempt_count;
        auto textured_consumer_enabled = mundo_webgl_env_opt_in_enabled("MUNDO_WEBGL_RENDER_TARGET_VULKAN_TEXTURED_CONSUMER");
        auto textured_consumer_possible = !strcmp(operation, "drawElements")
            && replay_viewport_valid
            && mode == GL_TRIANGLES
            && active_attrib_count == 2
            && sampler_uniform_count == 1
            && sampler_sources_render_target == 1
            && render_target_sampler_texture_handle
            && has_position_attrib
            && has_uv_attrib
            && position_layout_supported
            && uv_layout_supported
            && element_shadow_complete
            && (type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT);
        if (textured_attempt_count <= 24 || textured_attempt_count % 120 == 0) {
            dbgln("MUNDO_WEBGL_RENDER_TARGET_TEXTURED_CONSUMER_ATTEMPT count={} color_texture={} write_count={} program={} enabled={} possible={} operation={} mode={} draw_count={} type={} offset={} active_attribs={} sampler_uniforms={} sampler_sources_render_target={} sampled_render_target_texture={} sampled_render_target_size={}x{} has_position={} has_uv={} position_layout_supported={} uv_layout_supported={} position_bytes={} uv_bytes={} element_ready={} element_bytes={} destination_format={} reason={} next_step={}",
                textured_attempt_count,
                texture_handle,
                render_target_state->write_count,
                program_handle,
                textured_consumer_enabled,
                textured_consumer_possible,
                operation,
                mode,
                count,
                type,
                offset,
                active_attrib_count,
                sampler_uniform_count,
                sampler_sources_render_target,
                render_target_sampler_texture_handle,
                render_target_sampler_width,
                render_target_sampler_height,
                has_position_attrib,
                has_uv_attrib,
                position_layout_supported,
                uv_layout_supported,
                position_data.size(),
                uv_data.size(),
                element_shadow_complete,
                element_data.size(),
                destination_format.value_or(0),
                strcmp(operation, "drawElements") ? "not_draw_elements"sv
                    : !replay_viewport_valid ? "invalid_viewport"sv
                    : mode != GL_TRIANGLES ? "unsupported_primitive_mode"sv
                    : active_attrib_count != 2 ? "not_position_uv_mesh"sv
                    : sampler_uniform_count != 1 ? "not_single_sampler_consumer"sv
                    : sampler_sources_render_target != 1 ? "sampler_not_single_render_target_source"sv
                    : !render_target_sampler_texture_handle ? "missing_render_target_sampler_texture"sv
                    : !has_position_attrib ? "missing_position_attrib"sv
                    : !has_uv_attrib ? "missing_uv_attrib"sv
                    : !position_layout_supported ? "unsupported_position_layout"sv
                    : !uv_layout_supported ? "unsupported_uv_layout"sv
                    : !element_shadow_complete ? "missing_index_shadow"sv
                    : !(type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT) ? "unsupported_index_type"sv
                    : !destination_format.has_value() ? "missing_vulkan_target_format"sv
                    : !textured_consumer_enabled ? "textured_consumer_explicitly_disabled"sv
                    : "ready"sv,
                textured_consumer_possible && textured_consumer_enabled ? "execute_textured_vulkan_mesh_from_offscreen_render_target"sv : "keep_collecting_textured_consumer_inputs"sv);
        }
        if (textured_consumer_enabled && textured_consumer_possible && destination_format.has_value()) {
            auto source_image_or_error = m_context->get_or_create_vulkan_rgba_render_target_image(render_target_sampler_texture_handle, render_target_sampler_width ? render_target_sampler_width : 1, render_target_sampler_height ? render_target_sampler_height : 1, textured_attempt_count);
            if (source_image_or_error.is_error()) {
                if (textured_attempt_count <= 24 || textured_attempt_count % 120 == 0) {
                    dbgln("MUNDO_WEBGL_RENDER_TARGET_TEXTURED_CONSUMER_SOURCE_IMPORT count={} color_texture={} sampled_render_target_texture={} status=failed reason={} route=vulkan_offscreen_image next_step=fix_vulkan_offscreen_source_before_textured_consumer",
                        textured_attempt_count,
                        texture_handle,
                        render_target_sampler_texture_handle,
                        source_image_or_error.error().string_literal());
                }
            } else {
                auto* source_image_texture = source_image_or_error.release_value();
                auto textured_pipeline_probe = m_context->probe_vulkan_textured_mesh_pipeline(destination_format.value(), *source_image_texture->image, uniform_snapshot, position_data, uv_data, element_data, count, type, static_cast<GLintptr>(offset), viewport[0], viewport[1], viewport[2], viewport[3], textured_attempt_count);
                if (textured_attempt_count <= 24 || textured_attempt_count % 120 == 0) {
                    dbgln("MUNDO_WEBGL_RENDER_TARGET_TEXTURED_CONSUMER_PROBE_RESULT count={} color_texture={} write_count={} program={} sampled_render_target_texture={} attempted={} supported={} executed={} reason={} next_step={}",
                        textured_attempt_count,
                        texture_handle,
                        render_target_state->write_count,
                        program_handle,
                        render_target_sampler_texture_handle,
                        textured_pipeline_probe.attempted,
                        textured_pipeline_probe.supported,
                        textured_pipeline_probe.executed,
                        textured_pipeline_probe.reason,
                        textured_pipeline_probe.executed ? "validate_textured_consumer_visuals_then_make_render_target_chain_gpu_only" : "fix_textured_consumer_pipeline_before_replacing_gl_draw");
                }
            }
        }
#endif
    }
    return false;
}

void WebGLRenderingContextImpl::bind_renderbuffer(WebIDL::UnsignedLong target, GC::Root<WebGLRenderbuffer> renderbuffer)
{
    m_context->make_current();

    GLuint renderbuffer_handle = 0;
    if (renderbuffer) {
        auto handle_or_error = renderbuffer->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        renderbuffer_handle = handle_or_error.release_value();
    }

    glBindRenderbuffer(target, renderbuffer ? renderbuffer_handle : m_context->default_renderbuffer());
    m_renderbuffer_binding = renderbuffer;
}

void WebGLRenderingContextImpl::bind_texture(WebIDL::UnsignedLong target, GC::Root<WebGLTexture> texture)
{
    m_context->make_current();

    GLuint texture_handle = 0;
    if (texture) {
        auto handle_or_error = texture->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        texture_handle = handle_or_error.release_value();
    }

    switch (target) {
    case GL_TEXTURE_2D:
        m_texture_binding_2d = texture;
        if (m_mundo_active_texture_unit_index < 128) {
            if (m_mundo_texture_binding_2d_by_unit.size() <= m_mundo_active_texture_unit_index)
                m_mundo_texture_binding_2d_by_unit.resize(m_mundo_active_texture_unit_index + 1);
            m_mundo_texture_binding_2d_by_unit[m_mundo_active_texture_unit_index] = texture;
        }
        break;
    case GL_TEXTURE_CUBE_MAP:
        m_texture_binding_cube_map = texture;
        break;

    case GL_TEXTURE_2D_ARRAY:
        if (m_context->webgl_version() == OpenGLContext::WebGLVersion::WebGL2) {
            m_texture_binding_2d_array = texture;
            break;
        }

        set_error(GL_INVALID_ENUM);
        return;
    case GL_TEXTURE_3D:
        if (m_context->webgl_version() == OpenGLContext::WebGLVersion::WebGL2) {
            m_texture_binding_3d = texture;
            break;
        }

        set_error(GL_INVALID_ENUM);
        return;

    default:
        dbgln("Unknown WebGL texture target for storing current binding: 0x{:04x}", target);
        set_error(GL_INVALID_ENUM);
        return;
    }
    glBindTexture(target, texture_handle);
}

Optional<GLuint> WebGLRenderingContextImpl::current_bound_texture_handle_for_target(WebIDL::UnsignedLong target) const
{
    auto texture = current_bound_texture_for_target(target);
    if (!texture)
        return {};
    auto handle_or_error = texture->handle(this);
    if (handle_or_error.is_error())
        return {};
    return handle_or_error.release_value();
}

GC::Ptr<WebGLTexture> WebGLRenderingContextImpl::current_bound_texture_for_target(WebIDL::UnsignedLong target) const
{
    switch (target) {
    case GL_TEXTURE_2D:
        return m_texture_binding_2d;
    case GL_TEXTURE_CUBE_MAP:
        return m_texture_binding_cube_map;
    case GL_TEXTURE_2D_ARRAY:
        return m_texture_binding_2d_array;
    case GL_TEXTURE_3D:
        return m_texture_binding_3d;
    default:
        return nullptr;
    }
}

void WebGLRenderingContextImpl::blend_color(float red, float green, float blue, float alpha)
{
    m_context->make_current();
    glBlendColor(red, green, blue, alpha);
}

void WebGLRenderingContextImpl::blend_equation(WebIDL::UnsignedLong mode)
{
    m_context->make_current();
    glBlendEquation(mode);
}

void WebGLRenderingContextImpl::blend_equation_separate(WebIDL::UnsignedLong mode_rgb, WebIDL::UnsignedLong mode_alpha)
{
    m_context->make_current();
    glBlendEquationSeparate(mode_rgb, mode_alpha);
}

void WebGLRenderingContextImpl::blend_func(WebIDL::UnsignedLong sfactor, WebIDL::UnsignedLong dfactor)
{
    m_context->make_current();
    glBlendFunc(sfactor, dfactor);
}

void WebGLRenderingContextImpl::blend_func_separate(WebIDL::UnsignedLong src_rgb, WebIDL::UnsignedLong dst_rgb, WebIDL::UnsignedLong src_alpha, WebIDL::UnsignedLong dst_alpha)
{
    m_context->make_current();
    glBlendFuncSeparate(src_rgb, dst_rgb, src_alpha, dst_alpha);
}

WebIDL::UnsignedLong WebGLRenderingContextImpl::check_framebuffer_status(WebIDL::UnsignedLong target)
{
    m_context->make_current();
    return glCheckFramebufferStatus(target);
}

void WebGLRenderingContextImpl::clear(WebIDL::UnsignedLong mask)
{
    m_context->make_current();
    m_context->notify_content_will_change();
    needs_to_present();
    glClear(mask);
}

void WebGLRenderingContextImpl::clear_color(float red, float green, float blue, float alpha)
{
    m_context->make_current();
    glClearColor(red, green, blue, alpha);
}

void WebGLRenderingContextImpl::clear_depth(float depth)
{
    m_context->make_current();
    glClearDepthf(depth);
}

void WebGLRenderingContextImpl::clear_stencil(WebIDL::Long s)
{
    m_context->make_current();
    glClearStencil(s);
}

void WebGLRenderingContextImpl::color_mask(bool red, bool green, bool blue, bool alpha)
{
    m_context->make_current();
    glColorMask(red, green, blue, alpha);
}

void WebGLRenderingContextImpl::compile_shader(GC::Root<WebGLShader> shader)
{
    m_context->make_current();

    auto shader_handle = 0;
    if (shader) {
        auto handle_or_error = shader->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        shader_handle = handle_or_error.release_value();
    }
    glCompileShader(shader_handle);
}

void WebGLRenderingContextImpl::copy_tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::UnsignedLong internalformat, WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border)
{
    m_context->make_current();
    if (auto texture = current_bound_texture_for_target(target)) {
        texture->clear_hardware_video_backing();
        texture->set_mundo_texture_upload_snapshot_incomplete(width, height, internalformat, internalformat, 0, 0);
    }
    glCopyTexImage2D(target, level, internalformat, x, y, width, height, border);
}

void WebGLRenderingContextImpl::copy_tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height)
{
    m_context->make_current();
    if (auto texture = current_bound_texture_for_target(target)) {
        texture->clear_hardware_video_backing();
        texture->mark_mundo_texture_upload_snapshot_incomplete();
    }
    glCopyTexSubImage2D(target, level, xoffset, yoffset, x, y, width, height);
}

GC::Root<WebGLBuffer> WebGLRenderingContextImpl::create_buffer()
{
    m_context->make_current();

    GLuint handle = 0;
    glGenBuffers(1, &handle);
    auto buffer = WebGLBuffer::create(realm(), *this, handle);
    if (handle < 65536) {
        if (m_mundo_buffer_by_handle.size() <= handle)
            m_mundo_buffer_by_handle.resize(handle + 1);
        m_mundo_buffer_by_handle[handle] = buffer;
    }
    return buffer;
}

GC::Root<WebGLFramebuffer> WebGLRenderingContextImpl::create_framebuffer()
{
    m_context->make_current();

    GLuint handle = 0;
    glGenFramebuffers(1, &handle);
    return WebGLFramebuffer::create(realm(), *this, handle);
}

GC::Root<WebGLProgram> WebGLRenderingContextImpl::create_program()
{
    m_context->make_current();
    return WebGLProgram::create(realm(), *this, glCreateProgram());
}

GC::Root<WebGLRenderbuffer> WebGLRenderingContextImpl::create_renderbuffer()
{
    m_context->make_current();

    GLuint handle = 0;
    glGenRenderbuffers(1, &handle);
    return WebGLRenderbuffer::create(realm(), *this, handle);
}

GC::Root<WebGLShader> WebGLRenderingContextImpl::create_shader(WebIDL::UnsignedLong type)
{
    m_context->make_current();

    if (type != GL_VERTEX_SHADER && type != GL_FRAGMENT_SHADER) {
        dbgln("Unknown WebGL shader type: 0x{:04x}", type);
        set_error(GL_INVALID_ENUM);
        return nullptr;
    }

    GLuint handle = glCreateShader(type);
    return WebGLShader::create(realm(), *this, handle, type);
}

GC::Root<WebGLTexture> WebGLRenderingContextImpl::create_texture()
{
    m_context->make_current();

    GLuint handle = 0;
    glGenTextures(1, &handle);
    return WebGLTexture::create(realm(), *this, handle);
}

void WebGLRenderingContextImpl::cull_face(WebIDL::UnsignedLong mode)
{
    m_context->make_current();
    glCullFace(mode);
}

void WebGLRenderingContextImpl::delete_buffer(GC::Root<WebGLBuffer> buffer)
{
    m_context->make_current();

    GLuint buffer_handle = 0;
    if (buffer) {
        auto handle_or_error = buffer->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        buffer_handle = handle_or_error.release_value();
    }

    glDeleteBuffers(1, &buffer_handle);
    if (buffer_handle < m_mundo_buffer_by_handle.size())
        m_mundo_buffer_by_handle[buffer_handle] = nullptr;
}

void WebGLRenderingContextImpl::delete_framebuffer(GC::Root<WebGLFramebuffer> framebuffer)
{
    m_context->make_current();

    GLuint framebuffer_handle = 0;
    if (framebuffer) {
        auto handle_or_error = framebuffer->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        framebuffer_handle = handle_or_error.release_value();
    }

    glDeleteFramebuffers(1, &framebuffer_handle);
}

void WebGLRenderingContextImpl::delete_program(GC::Root<WebGLProgram> program)
{
    m_context->make_current();

    auto program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        program_handle = handle_or_error.release_value();
    }
    glDeleteProgram(program_handle);
    if (m_current_program == program)
        m_current_program = nullptr;
}

void WebGLRenderingContextImpl::delete_renderbuffer(GC::Root<WebGLRenderbuffer> renderbuffer)
{
    m_context->make_current();

    GLuint renderbuffer_handle = 0;
    if (renderbuffer) {
        auto handle_or_error = renderbuffer->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        renderbuffer_handle = handle_or_error.release_value();
    }

    glDeleteRenderbuffers(1, &renderbuffer_handle);
}

void WebGLRenderingContextImpl::delete_shader(GC::Root<WebGLShader> shader)
{
    m_context->make_current();

    auto shader_handle = 0;
    if (shader) {
        auto handle_or_error = shader->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        shader_handle = handle_or_error.release_value();
    }
    glDeleteShader(shader_handle);
}

void WebGLRenderingContextImpl::delete_texture(GC::Root<WebGLTexture> texture)
{
    m_context->make_current();

    GLuint texture_handle = 0;
    if (texture) {
        auto handle_or_error = texture->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        texture_handle = handle_or_error.release_value();
    }

    glDeleteTextures(1, &texture_handle);

    if (m_texture_binding_2d == texture)
        m_texture_binding_2d = nullptr;
    for (auto& bound_texture : m_mundo_texture_binding_2d_by_unit) {
        if (bound_texture == texture)
            bound_texture = nullptr;
    }
    if (m_texture_binding_cube_map == texture)
        m_texture_binding_cube_map = nullptr;
    if (m_texture_binding_2d_array == texture)
        m_texture_binding_2d_array = nullptr;
    if (m_texture_binding_3d == texture)
        m_texture_binding_3d = nullptr;
}

void WebGLRenderingContextImpl::depth_func(WebIDL::UnsignedLong func)
{
    m_context->make_current();
    glDepthFunc(func);
}

void WebGLRenderingContextImpl::depth_mask(bool flag)
{
    m_context->make_current();
    glDepthMask(flag);
}

void WebGLRenderingContextImpl::depth_range(float z_near, float z_far)
{
    m_context->make_current();
    glDepthRangef(z_near, z_far);
}

void WebGLRenderingContextImpl::detach_shader(GC::Root<WebGLProgram> program, GC::Root<WebGLShader> shader)
{
    m_context->make_current();

    auto program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        program_handle = handle_or_error.release_value();
    }

    auto shader_handle = 0;
    if (shader) {
        auto handle_or_error = shader->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        shader_handle = handle_or_error.release_value();
    }

    glDetachShader(program_handle, shader_handle);

    switch (shader->type()) {
    case GL_VERTEX_SHADER:
        program->set_attached_vertex_shader(nullptr);
        break;
    case GL_FRAGMENT_SHADER:
        program->set_attached_fragment_shader(nullptr);
        break;
    }
}

void WebGLRenderingContextImpl::disable(WebIDL::UnsignedLong cap)
{
    m_context->make_current();
    glDisable(cap);
}

void WebGLRenderingContextImpl::disable_vertex_attrib_array(WebIDL::UnsignedLong index)
{
    m_context->make_current();
    glDisableVertexAttribArray(index);
}

void WebGLRenderingContextImpl::draw_arrays(WebIDL::UnsignedLong mode, WebIDL::Long first, WebIDL::Long count)
{
    m_context->make_current();
    m_context->notify_content_will_change();
    needs_to_present();
    auto gl_after_direct_vulkan_video = m_context->has_direct_vulkan_video_draw_pending_gl_present();
    auto start = MonotonicTime::now();
    glDrawArrays(mode, first, count);
    m_context->note_gl_draw_submitted();
    note_mundo_framebuffer_draw("drawArrays", mode, count);
    if (gl_after_direct_vulkan_video) {
        static size_t s_gl_after_direct_vulkan_video_draw_arrays_count { 0 };
        auto log_count = ++s_gl_after_direct_vulkan_video_draw_arrays_count;
        if (log_count <= 24 || log_count % 120 == 0) {
            GLuint program_handle = 0;
            if (m_current_program) {
                auto program_handle_or_error = m_current_program->handle(this);
                if (!program_handle_or_error.is_error())
                    program_handle = program_handle_or_error.release_value();
            }
            GLuint texture_handle = 0;
            bool bound_texture_has_video_backing = false;
            if (m_texture_binding_2d) {
                auto texture_handle_or_error = m_texture_binding_2d->handle(this);
                if (!texture_handle_or_error.is_error())
                    texture_handle = texture_handle_or_error.release_value();
                bound_texture_has_video_backing = m_texture_binding_2d->has_hardware_video_backing();
            }
            GLint framebuffer = 0;
            GLint vertex_array = 0;
            GLint active_attrib_count = 0;
            GLint active_uniform_count = 0;
            GLint viewport[4] {};
            glGetIntegervRobustANGLE(GL_FRAMEBUFFER_BINDING, 1, nullptr, &framebuffer);
            glGetIntegervRobustANGLE(GL_VERTEX_ARRAY_BINDING, 1, nullptr, &vertex_array);
            glGetIntegervRobustANGLE(GL_VIEWPORT, 4, nullptr, viewport);
            if (program_handle) {
                glGetProgramivRobustANGLE(program_handle, GL_ACTIVE_ATTRIBUTES, 1, nullptr, &active_attrib_count);
                glGetProgramivRobustANGLE(program_handle, GL_ACTIVE_UNIFORMS, 1, nullptr, &active_uniform_count);
            }
            dbgln("MUNDO_WEBGL_GL_AFTER_DIRECT_VULKAN_VIDEO_DRAW count={} op=drawArrays mode={} first={} draw_count={} program={} texture={} bound_texture_has_video_backing={} framebuffer={} vertex_array={} active_attribs={} active_uniforms={} viewport={}x{}+{}+{} reason=gl_draw_after_direct_vulkan_video_before_present next_step=classify_or_virtualize_this_gl_draw_for_pure_vulkan_present",
                log_count,
                mode,
                first,
                count,
                program_handle,
                texture_handle,
                bound_texture_has_video_backing,
                framebuffer,
                vertex_array,
                active_attrib_count,
                active_uniform_count,
                viewport[2],
                viewport[3],
                viewport[0],
                viewport[1]);
        }
    }
    record_mundo_webgl_timing_summary("drawArrays", (MonotonicTime::now() - start).to_microseconds());
    if (auto duration = mundo_webgl_slow_duration(start); duration.has_value())
        dbgln("MUNDO_WEBGL_TIMING count={} op=drawArrays duration={}ms threshold={}ms mode={} first={} count={}", mundo_webgl_next_timing_count(), duration.value(), mundo_webgl_timing_threshold_ms(), mode, first, count);
    if (m_texture_binding_2d && m_texture_binding_2d->has_hardware_video_backing()) {
        static size_t s_hardware_video_draw_arrays_count { 0 };
        auto log_count = ++s_hardware_video_draw_arrays_count;
        if (log_count <= 8 || log_count % 120 == 0) {
            auto const& backing = m_texture_binding_2d->hardware_video_backing().value();
            GLint active_texture = 0;
            glGetIntegervRobustANGLE(GL_ACTIVE_TEXTURE, 1, nullptr, &active_texture);
            GLuint program_handle = 0;
            if (m_current_program) {
                auto handle_or_error = m_current_program->handle(this);
                if (!handle_or_error.is_error())
                    program_handle = handle_or_error.release_value();
            }
            GLuint texture_handle = 0;
            auto texture_handle_or_error = m_texture_binding_2d->handle(this);
            if (!texture_handle_or_error.is_error())
                texture_handle = texture_handle_or_error.release_value();
            auto video_sampler_uniform = "none"sv;
            auto video_sampler_direct_texture_call = false;
            if (m_current_program && m_current_program->video_sampler_plan().has_value()) {
                auto const& plan = m_current_program->video_sampler_plan().value();
                video_sampler_uniform = plan.uniform_name.bytes_as_string_view();
                video_sampler_direct_texture_call = plan.direct_texture_call;
            }
            auto readiness = mundo_webgl_video_virtualization_readiness(program_handle, texture_handle, backing, m_current_program ? m_current_program->video_sampler_plan() : Optional<WebGLProgram::VideoSamplerPlan> {});
            GLuint tracked_sampler_texture_handle = 0;
            bool tracked_sampler_texture_has_video_backing = false;
            if (readiness.sampler_unit >= 0 && static_cast<size_t>(readiness.sampler_unit) < m_mundo_texture_binding_2d_by_unit.size()) {
                if (auto tracked_texture = m_mundo_texture_binding_2d_by_unit[readiness.sampler_unit]) {
                    auto tracked_handle_or_error = tracked_texture->handle(this);
                    if (!tracked_handle_or_error.is_error())
                        tracked_sampler_texture_handle = tracked_handle_or_error.release_value();
                    tracked_sampler_texture_has_video_backing = tracked_texture->has_hardware_video_backing();
                }
            }
            auto uses_video_sampler = readiness.route_supported && readiness.direct_texture_call && readiness.sampler_matches_video_texture;
            dbgln("MUNDO_WEBGL_VIDEO_TEXTURE_BACKING_DRAW count={} op=drawArrays frame_id={} texture_target=2d texture={} active_texture={} program={} size={}x{} backend={} upload_mode={} copy_stage={} direct_zero_copy={} copied_on_gpu={} video_sampler_uniform={} video_sampler_direct_texture_call={} next_step=shader_or_texture_virtualization",
                log_count,
                backing.frame_id,
                texture_handle,
                active_texture,
                program_handle,
                backing.width,
                backing.height,
                backing.backend,
                backing.upload_mode,
                backing.copy_stage,
                backing.direct_zero_copy,
                backing.copied_on_gpu,
                video_sampler_uniform,
                video_sampler_direct_texture_call);
            dbgln("MUNDO_WEBGL_VIDEO_DRAW_CLASSIFICATION count={} op=drawArrays frame_id={} texture={} program={} bound_video_texture=true uses_video_sampler={} reason={} route={} next_step={}",
                log_count,
                backing.frame_id,
                texture_handle,
                program_handle,
                uses_video_sampler,
                readiness.reason,
                backing.direct_sampling_route,
                uses_video_sampler ? "virtualize_this_draw" : "ignore_as_non_sampler_or_wait_for_sampler_draw");
            dbgln("MUNDO_WEBGL_VIDEO_VIRTUALIZATION_READY count={} op=drawArrays frame_id={} texture={} program={} route={} has_plan={} route_supported={} direct_texture_call={} sampler_location={} sampler_unit={} sampler_bound_texture={} tracked_sampler_texture={} tracked_sampler_texture_has_video_backing={} sampler_matches_video_texture={} ready={} reason={} next_step={}",
                log_count,
                backing.frame_id,
                texture_handle,
                program_handle,
                backing.direct_sampling_route,
                readiness.has_plan,
                readiness.route_supported,
                readiness.direct_texture_call,
                readiness.sampler_location,
                readiness.sampler_unit,
                readiness.sampler_bound_texture,
                tracked_sampler_texture_handle,
                tracked_sampler_texture_has_video_backing,
                readiness.sampler_matches_video_texture,
                uses_video_sampler,
                readiness.reason,
                uses_video_sampler ? "implement_vulkan_sampler_draw_path" : "wait_for_matching_video_sampler_draw");
            if (uses_video_sampler)
                log_mundo_webgl_video_virtualization_draw_state("drawArrays", log_count, backing, program_handle, texture_handle, mode, first, count, 0, 0);
#ifdef USE_VULKAN_DMABUF_IMAGES
            if (uses_video_sampler) {
                m_context->probe_retained_vulkan_video_source_for_virtual_draw(backing.source_opaque_fd, backing.source_handle_type, backing.source_allocation_size, backing.width, backing.height, backing.source_vulkan_format, backing.source_vulkan_layout, backing.frame_id, log_count);
                auto cache_state = ensure_mundo_webgl_video_virtual_source_cached(*m_context, *m_texture_binding_2d, backing, "drawArrays", log_count, program_handle, texture_handle, true);
                log_mundo_webgl_video_virtual_draw_ready("drawArrays", log_count, *m_texture_binding_2d, backing, program_handle, texture_handle, readiness, cache_state);
                log_mundo_webgl_video_vulkan_direct_draw_plan(*m_context, *this, "drawArrays", log_count, true, *m_texture_binding_2d, backing, program_handle, texture_handle, readiness, cache_state, { mode, first, count, 0, 0 });
            }
#endif
            log_mundo_webgl_video_sampler_uniforms(program_handle, texture_handle, m_texture_binding_2d->hardware_video_backing(), log_count);
        }
    }
}

void WebGLRenderingContextImpl::draw_elements(WebIDL::UnsignedLong mode, WebIDL::Long count, WebIDL::UnsignedLong type, WebIDL::LongLong offset)
{
    m_context->make_current();
    m_context->notify_content_will_change();

    if (should_skip_mundo_webgl_draw_elements_for_budget(count))
        return;

    auto start = MonotonicTime::now();
    bool vulkan_video_draw_executed = false;
    bool vulkan_video_draw_used_sampler = false;
    bool vulkan_video_draw_direct_zero_copy = false;
    GLint video_draw_framebuffer = 0;
    GLenum video_draw_framebuffer_status = 0;
    GLint video_draw_color_attachment_type = 0;
    GLint video_draw_color_attachment_name = 0;
    if (m_texture_binding_2d && m_texture_binding_2d->has_hardware_video_backing()) {
        static size_t s_hardware_video_draw_elements_count { 0 };
        auto log_count = ++s_hardware_video_draw_elements_count;
        auto const& backing = m_texture_binding_2d->hardware_video_backing().value();
        GLuint program_handle = 0;
        if (m_current_program) {
            auto handle_or_error = m_current_program->handle(this);
            if (!handle_or_error.is_error())
                program_handle = handle_or_error.release_value();
        }
        GLuint texture_handle = 0;
        auto texture_handle_or_error = m_texture_binding_2d->handle(this);
        if (!texture_handle_or_error.is_error())
            texture_handle = texture_handle_or_error.release_value();
        auto readiness = mundo_webgl_video_virtualization_readiness(program_handle, texture_handle, backing, m_current_program ? m_current_program->video_sampler_plan() : Optional<WebGLProgram::VideoSamplerPlan> {});
        auto uses_video_sampler = readiness.route_supported && readiness.direct_texture_call && readiness.sampler_matches_video_texture;
        vulkan_video_draw_used_sampler = uses_video_sampler;
        vulkan_video_draw_direct_zero_copy = backing.direct_zero_copy && !strcmp(backing.upload_mode, "vulkan_mesh_direct_nv12");
        auto should_log_video_draw = log_count <= 8 || log_count % 120 == 0;
        glGetIntegervRobustANGLE(GL_FRAMEBUFFER_BINDING, 1, nullptr, &video_draw_framebuffer);
        if (video_draw_framebuffer) {
            video_draw_framebuffer_status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
            glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &video_draw_color_attachment_type);
            glGetFramebufferAttachmentParameteriv(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &video_draw_color_attachment_name);
        }
#ifdef USE_VULKAN_DMABUF_IMAGES
        Optional<MundoWebGLVirtualVideoSourceCacheState> virtual_source_cache_state;
        if (uses_video_sampler)
            virtual_source_cache_state = ensure_mundo_webgl_video_virtual_source_cached(*m_context, *m_texture_binding_2d, backing, "drawElements", log_count, program_handle, texture_handle, should_log_video_draw);
        if (uses_video_sampler && virtual_source_cache_state.has_value()) {
            auto direct_vulkan_mesh_mode = mundo_webgl_video_direct_vulkan_mesh_enabled();
            auto auto_sync_direct_zero_copy_replace = backing.direct_zero_copy
                && !strcmp(backing.upload_mode, "vulkan_mesh_direct_nv12")
                && (direct_vulkan_mesh_mode || mundo_webgl_env_flag_enabled("MUNDO_WEBGL_VIDEO_VULKAN_MESH_REPLACE_GL"))
                && (direct_vulkan_mesh_mode || mundo_webgl_env_flag_enabled("MUNDO_WEBGL_VIDEO_VULKAN_MESH_SKIP_RGBA_UPLOAD_FOR_REPLACE"))
                && !mundo_webgl_env_flag_enabled("MUNDO_WEBGL_VIDEO_VULKAN_MESH_DISABLE_AUTO_SYNC");
            auto explicit_sync = mundo_webgl_env_flag_enabled("MUNDO_WEBGL_VIDEO_VULKAN_MESH_SYNC_GL_BEFORE_DRAW");
            if (explicit_sync || auto_sync_direct_zero_copy_replace) {
                auto sync_mode = auto_sync_direct_zero_copy_replace ? "flush"sv : "finish"sv;
                if (auto const* sync_mode_value = getenv("MUNDO_WEBGL_VIDEO_VULKAN_MESH_SYNC_MODE")) {
                    auto value = StringView { sync_mode_value, strlen(sync_mode_value) };
                    if (value == "flush"sv)
                        sync_mode = "flush"sv;
                    else if (value == "none"sv)
                        sync_mode = "none"sv;
                    else if (value == "finish"sv)
                        sync_mode = "finish"sv;
                }

                auto sync_started_at = MonotonicTime::now();
                if (sync_mode == "flush"sv)
                    glFlush();
                else if (sync_mode == "finish"sv)
                    glFinish();
                auto sync_us = (MonotonicTime::now() - sync_started_at).to_microseconds();

                if (should_log_video_draw)
                    dbgln("MUNDO_WEBGL_VIDEO_VULKAN_MESH_SYNC_GL_BEFORE_DRAW count={} frame_id={} reason={} sync_mode={} sync_us={}", log_count, backing.frame_id, explicit_sync ? "explicit_diagnostic_sync" : "direct_zero_copy_replace_auto_sync", sync_mode, sync_us);
            }
            vulkan_video_draw_executed = log_mundo_webgl_video_vulkan_direct_draw_plan(*m_context, *this, "drawElements", log_count, should_log_video_draw, *m_texture_binding_2d, backing, program_handle, texture_handle, readiness, virtual_source_cache_state.value(), { mode, 0, count, type, static_cast<GLintptr>(offset) });
        }
#endif
        if (should_log_video_draw) {
            GLint active_texture = 0;
            glGetIntegervRobustANGLE(GL_ACTIVE_TEXTURE, 1, nullptr, &active_texture);
            auto video_sampler_uniform = "none"sv;
            auto video_sampler_direct_texture_call = false;
            if (m_current_program && m_current_program->video_sampler_plan().has_value()) {
                auto const& plan = m_current_program->video_sampler_plan().value();
                video_sampler_uniform = plan.uniform_name.bytes_as_string_view();
                video_sampler_direct_texture_call = plan.direct_texture_call;
            }
            GLuint tracked_sampler_texture_handle = 0;
            bool tracked_sampler_texture_has_video_backing = false;
            if (readiness.sampler_unit >= 0 && static_cast<size_t>(readiness.sampler_unit) < m_mundo_texture_binding_2d_by_unit.size()) {
                if (auto tracked_texture = m_mundo_texture_binding_2d_by_unit[readiness.sampler_unit]) {
                    auto tracked_handle_or_error = tracked_texture->handle(this);
                    if (!tracked_handle_or_error.is_error())
                        tracked_sampler_texture_handle = tracked_handle_or_error.release_value();
                    tracked_sampler_texture_has_video_backing = tracked_texture->has_hardware_video_backing();
                }
            }
            dbgln("MUNDO_WEBGL_VIDEO_TEXTURE_BACKING_DRAW count={} op=drawElements frame_id={} texture_target=2d texture={} active_texture={} program={} size={}x{} backend={} upload_mode={} copy_stage={} direct_zero_copy={} copied_on_gpu={} video_sampler_uniform={} video_sampler_direct_texture_call={} next_step=shader_or_texture_virtualization",
                log_count,
                backing.frame_id,
                texture_handle,
                active_texture,
                program_handle,
                backing.width,
                backing.height,
                backing.backend,
                backing.upload_mode,
                backing.copy_stage,
                backing.direct_zero_copy,
                backing.copied_on_gpu,
                video_sampler_uniform,
                video_sampler_direct_texture_call);
            dbgln("MUNDO_WEBGL_VIDEO_DRAW_CLASSIFICATION count={} op=drawElements frame_id={} texture={} program={} bound_video_texture=true uses_video_sampler={} reason={} route={} next_step={}",
                log_count,
                backing.frame_id,
                texture_handle,
                program_handle,
                uses_video_sampler,
                readiness.reason,
                backing.direct_sampling_route,
                uses_video_sampler ? "virtualize_this_draw" : "ignore_as_non_sampler_or_wait_for_sampler_draw");
            dbgln("MUNDO_WEBGL_VIDEO_VIRTUALIZATION_READY count={} op=drawElements frame_id={} texture={} program={} route={} has_plan={} route_supported={} direct_texture_call={} sampler_location={} sampler_unit={} sampler_bound_texture={} tracked_sampler_texture={} tracked_sampler_texture_has_video_backing={} sampler_matches_video_texture={} ready={} reason={} next_step={}",
                log_count,
                backing.frame_id,
                texture_handle,
                program_handle,
                backing.direct_sampling_route,
                readiness.has_plan,
                readiness.route_supported,
                readiness.direct_texture_call,
                readiness.sampler_location,
                readiness.sampler_unit,
                readiness.sampler_bound_texture,
                tracked_sampler_texture_handle,
                tracked_sampler_texture_has_video_backing,
                readiness.sampler_matches_video_texture,
                uses_video_sampler,
                readiness.reason,
                uses_video_sampler ? "implement_vulkan_sampler_draw_path" : "wait_for_matching_video_sampler_draw");
            dbgln("MUNDO_WEBGL_VIDEO_DRAW_TARGET_STATE count={} op=drawElements frame_id={} framebuffer={} default_framebuffer={} framebuffer_status={} color_attachment_type={} color_attachment_name={} vulkan_mesh_target=painting_surface next_step={}",
                log_count,
                backing.frame_id,
                video_draw_framebuffer,
                m_context->default_framebuffer(),
                video_draw_framebuffer_status,
                video_draw_color_attachment_type,
                video_draw_color_attachment_name,
                video_draw_framebuffer == static_cast<GLint>(m_context->default_framebuffer())
                    ? "map_webgl_default_color_buffer_to_vulkan_mesh_target_before_replacing_gl"
                    : "map_webgl_framebuffer_attachment_to_vulkan_mesh_target_before_replacing_gl");
            if (uses_video_sampler)
                log_mundo_webgl_video_virtualization_draw_state("drawElements", log_count, backing, program_handle, texture_handle, mode, 0, count, type, static_cast<GLintptr>(offset));
#ifdef USE_VULKAN_DMABUF_IMAGES
            if (uses_video_sampler) {
                m_context->probe_retained_vulkan_video_source_for_virtual_draw(backing.source_opaque_fd, backing.source_handle_type, backing.source_allocation_size, backing.width, backing.height, backing.source_vulkan_format, backing.source_vulkan_layout, backing.frame_id, log_count);
                if (virtual_source_cache_state.has_value()) {
                    log_mundo_webgl_video_virtual_draw_ready("drawElements", log_count, *m_texture_binding_2d, backing, program_handle, texture_handle, readiness, virtual_source_cache_state.value());
                }
            }
#endif
            log_mundo_webgl_video_sampler_uniforms(program_handle, texture_handle, m_texture_binding_2d->hardware_video_backing(), log_count);
        }
    }
    auto direct_vulkan_mesh_mode = mundo_webgl_video_direct_vulkan_mesh_enabled();
    auto replace_gl_draw = direct_vulkan_mesh_mode || mundo_webgl_env_flag_enabled("MUNDO_WEBGL_VIDEO_VULKAN_MESH_REPLACE_GL");
    auto force_replace_gl_draw = mundo_webgl_env_flag_enabled("MUNDO_WEBGL_VIDEO_VULKAN_MESH_REPLACE_GL_FORCE_TARGET_MISMATCH");
    auto direct_zero_copy_replace_gl_draw = vulkan_video_draw_direct_zero_copy && vulkan_video_draw_used_sampler;
    auto can_replace_gl_draw = vulkan_video_draw_executed && replace_gl_draw && (force_replace_gl_draw || direct_zero_copy_replace_gl_draw);
    if (!(can_replace_gl_draw)) {
        if (vulkan_video_draw_executed && replace_gl_draw) {
            static size_t s_blocked_replace_draw_count { 0 };
            auto blocked_replace_draw_count = ++s_blocked_replace_draw_count;
            if (blocked_replace_draw_count <= 16 || blocked_replace_draw_count % 120 == 0)
                dbgln("MUNDO_WEBGL_VIDEO_VULKAN_MESH_REPLACE_GL_BLOCKED count={} framebuffer={} default_framebuffer={} framebuffer_status={} color_attachment_type={} color_attachment_name={} direct_zero_copy={} uses_video_sampler={} reason=vulkan_mesh_target_not_wired_to_webgl_present_target next_step=make_vulkan_mesh_render_into_webgl_draw_target_before_skipping_gl",
                    blocked_replace_draw_count,
                    video_draw_framebuffer,
                    m_context->default_framebuffer(),
                    video_draw_framebuffer_status,
                    video_draw_color_attachment_type,
                    video_draw_color_attachment_name,
                    vulkan_video_draw_direct_zero_copy,
                    vulkan_video_draw_used_sampler);
        }
        auto gl_after_direct_vulkan_video = m_context->has_direct_vulkan_video_draw_pending_gl_present();
        auto skip_post_direct_vulkan_non_sampler_gl = direct_vulkan_mesh_mode
            && gl_after_direct_vulkan_video
            && mundo_webgl_skip_post_direct_vulkan_non_sampler_gl_enabled()
            && vulkan_video_draw_direct_zero_copy
            && !vulkan_video_draw_used_sampler
            && m_texture_binding_2d
            && m_texture_binding_2d->has_hardware_video_backing();
        if (skip_post_direct_vulkan_non_sampler_gl) {
            static size_t s_skipped_post_direct_vulkan_non_sampler_gl_count { 0 };
            auto skip_count = ++s_skipped_post_direct_vulkan_non_sampler_gl_count;
            if (skip_count <= 24 || skip_count % 120 == 0) {
                GLuint skipped_program_handle = 0;
                if (m_current_program) {
                    auto handle_or_error = m_current_program->handle(this);
                    if (!handle_or_error.is_error())
                        skipped_program_handle = handle_or_error.value();
                }
                GLuint skipped_texture_handle = 0;
                if (m_texture_binding_2d) {
                    auto handle_or_error = m_texture_binding_2d->handle(this);
                    if (!handle_or_error.is_error())
                        skipped_texture_handle = handle_or_error.value();
                }
                GLint framebuffer = 0;
                GLint vertex_array = 0;
                GLint element_array_buffer = 0;
                GLint active_attrib_count = 0;
                GLint active_uniform_count = 0;
                GLint viewport[4] {};
                glGetIntegervRobustANGLE(GL_FRAMEBUFFER_BINDING, 1, nullptr, &framebuffer);
                glGetIntegervRobustANGLE(GL_VERTEX_ARRAY_BINDING, 1, nullptr, &vertex_array);
                glGetIntegervRobustANGLE(GL_ELEMENT_ARRAY_BUFFER_BINDING, 1, nullptr, &element_array_buffer);
                glGetIntegervRobustANGLE(GL_VIEWPORT, 4, nullptr, viewport);
                if (skipped_program_handle) {
                    glGetProgramivRobustANGLE(skipped_program_handle, GL_ACTIVE_ATTRIBUTES, 1, nullptr, &active_attrib_count);
                    glGetProgramivRobustANGLE(skipped_program_handle, GL_ACTIVE_UNIFORMS, 1, nullptr, &active_uniform_count);
                }
                dbgln("MUNDO_WEBGL_SKIP_POST_DIRECT_VULKAN_NON_SAMPLER_GL count={} op=drawElements mode={} draw_count={} type={} offset={} program={} texture={} framebuffer={} vertex_array={} element_array_buffer={} active_attribs={} active_uniforms={} viewport={}x{}+{}+{} reason=post_direct_vulkan_draw_does_not_use_video_sampler next_step=verify_present_auto_can_skip_gl_sync_without_losing_required_scene_geometry",
                    skip_count,
                    mode,
                    count,
                    type,
                    offset,
                    skipped_program_handle,
                    skipped_texture_handle,
                    framebuffer,
                    vertex_array,
                    element_array_buffer,
                    active_attrib_count,
                    active_uniform_count,
                    viewport[2],
                    viewport[3],
                    viewport[0],
                    viewport[1]);
            }
            return;
        }
#ifdef USE_VULKAN_DMABUF_IMAGES
        auto try_post_direct_vulkan_solid_mesh_replay = [&]() -> bool {
            if (!direct_vulkan_mesh_mode || !gl_after_direct_vulkan_video || vulkan_video_draw_used_sampler)
                return false;

            GLuint program_handle = 0;
            if (m_current_program) {
                auto handle_or_error = m_current_program->handle(this);
                if (handle_or_error.is_error())
                    return false;
                program_handle = handle_or_error.value();
            }
            if (!program_handle)
                return false;

            GLint active_attrib_count = 0;
            GLint active_uniform_count = 0;
            GLint element_array_buffer = 0;
            GLint viewport[4] {};
            glGetProgramivRobustANGLE(program_handle, GL_ACTIVE_ATTRIBUTES, 1, nullptr, &active_attrib_count);
            glGetProgramivRobustANGLE(program_handle, GL_ACTIVE_UNIFORMS, 1, nullptr, &active_uniform_count);
            glGetIntegervRobustANGLE(GL_ELEMENT_ARRAY_BUFFER_BINDING, 1, nullptr, &element_array_buffer);
            glGetIntegervRobustANGLE(GL_VIEWPORT, 4, nullptr, viewport);

            if (active_attrib_count != 1)
                return false;

            OpenGLContext::VulkanSolidMeshUniformSnapshot uniform_snapshot;
            for (size_t i = 0; i < 16; ++i) {
                uniform_snapshot.model_view_matrix[i] = (i % 5) == 0 ? 1.0f : 0.0f;
                uniform_snapshot.projection_matrix[i] = (i % 5) == 0 ? 1.0f : 0.0f;
            }
            auto uniforms_to_scan = active_uniform_count < 16 ? active_uniform_count : 16;
            for (GLint index = 0; index < uniforms_to_scan; ++index) {
                GLint uniform_size = 0;
                GLenum uniform_type = 0;
                GLsizei uniform_length = 0;
                GLchar uniform_name[256];
                glGetActiveUniform(program_handle, static_cast<GLuint>(index), sizeof(uniform_name), &uniform_length, &uniform_size, &uniform_type, uniform_name);
                if (!uniform_length)
                    continue;
                auto uniform_name_view = StringView { uniform_name, static_cast<size_t>(uniform_length) };
                auto location = glGetUniformLocation(program_handle, uniform_name);
                if (location < 0)
                    continue;
                if (uniform_type == GL_FLOAT_MAT4 && uniform_name_view == "modelViewMatrix"sv) {
                    glGetUniformfv(program_handle, location, uniform_snapshot.model_view_matrix.data());
                    uniform_snapshot.has_model_view_matrix = true;
                } else if (uniform_type == GL_FLOAT_MAT4 && uniform_name_view == "projectionMatrix"sv) {
                    glGetUniformfv(program_handle, location, uniform_snapshot.projection_matrix.data());
                    uniform_snapshot.has_projection_matrix = true;
                } else if (uniform_type == GL_FLOAT && uniform_name_view == "opacity"sv) {
                    glGetUniformfv(program_handle, location, &uniform_snapshot.opacity);
                } else if (uniform_type == GL_FLOAT && uniform_name_view == "uOutputIntensity"sv) {
                    glGetUniformfv(program_handle, location, &uniform_snapshot.output_intensity);
                } else if ((uniform_type == GL_FLOAT_VEC3 || uniform_type == GL_FLOAT_VEC4) && uniform_name_view == "diffuse"sv) {
                    Array<float, 4> diffuse { 1.0f, 1.0f, 1.0f, 1.0f };
                    glGetUniformfv(program_handle, location, diffuse.data());
                    uniform_snapshot.diffuse = diffuse;
                }
            }

            ReadonlyBytes position_data;
            bool position_ready = false;
            bool position_layout_supported = false;
            for (GLint index = 0; index < active_attrib_count; ++index) {
                GLint attrib_size = 0;
                GLenum attrib_type = 0;
                GLsizei attrib_length = 0;
                GLchar attrib_name[256];
                glGetActiveAttrib(program_handle, static_cast<GLuint>(index), sizeof(attrib_name), &attrib_length, &attrib_size, &attrib_type, attrib_name);
                if (!attrib_length)
                    continue;
                auto attrib_name_view = StringView { attrib_name, static_cast<size_t>(attrib_length) };
                if (attrib_name_view != "position"sv)
                    continue;
                auto location = glGetAttribLocation(program_handle, attrib_name);
                if (location < 0)
                    continue;
                GLint enabled = 0;
                GLint array_size = 0;
                GLint array_type = 0;
                GLint stride = 0;
                GLint buffer = 0;
                void* pointer = nullptr;
                glGetVertexAttribivRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_ENABLED, 1, nullptr, &enabled);
                glGetVertexAttribivRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_SIZE, 1, nullptr, &array_size);
                glGetVertexAttribivRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_TYPE, 1, nullptr, &array_type);
                glGetVertexAttribivRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_STRIDE, 1, nullptr, &stride);
                glGetVertexAttribivRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, 1, nullptr, &buffer);
                glGetVertexAttribPointervRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_POINTER, 1, nullptr, &pointer);
                position_layout_supported = enabled && array_size == 3 && array_type == GL_FLOAT && stride == static_cast<GLint>(sizeof(float) * 3) && reinterpret_cast<uintptr_t>(pointer) == 0;
                if (!position_layout_supported)
                    break;
                if (auto webgl_buffer = mundo_buffer_for_handle(static_cast<GLuint>(buffer)); webgl_buffer && webgl_buffer->has_complete_shadow_data()) {
                    position_data = webgl_buffer->shadow_data();
                    position_ready = true;
                }
            }

            ReadonlyBytes element_data;
            bool element_ready = false;
            if (auto element_buffer = mundo_buffer_for_handle(static_cast<GLuint>(element_array_buffer)); element_buffer && element_buffer->has_complete_shadow_data()) {
                element_data = element_buffer->shadow_data();
                element_ready = true;
            }

            auto destination_format = m_context->vulkan_painting_surface_format();
            static size_t s_post_direct_solid_attempt_count { 0 };
            auto attempt_count = ++s_post_direct_solid_attempt_count;
            auto solid_replay_enabled = mundo_webgl_env_enabled_by_default("MUNDO_WEBGL_POST_DIRECT_VULKAN_SOLID_REPLAY");
            auto replay_viewport_valid = viewport[2] > 0 && viewport[3] > 0;
            GLuint bound_texture_handle = 0;
            bool bound_texture_has_video_backing = false;
            bool bound_texture_render_target_written = false;
            size_t bound_texture_render_target_write_count = 0;
            u32 bound_texture_render_target_width = 0;
            u32 bound_texture_render_target_height = 0;
            if (m_texture_binding_2d) {
                bound_texture_handle = m_texture_binding_2d->handle(this).value_or(0);
                bound_texture_has_video_backing = m_texture_binding_2d->has_hardware_video_backing();
                auto const& bound_render_target_state = m_texture_binding_2d->mundo_render_target_write_state();
                bound_texture_render_target_written = bound_render_target_state.has_value();
                if (bound_render_target_state.has_value()) {
                    bound_texture_render_target_write_count = bound_render_target_state->write_count;
                    bound_texture_render_target_width = bound_render_target_state->last_viewport_width;
                    bound_texture_render_target_height = bound_render_target_state->last_viewport_height;
                }
            }
            GLboolean color_write_mask[4] {};
            GLboolean depth_write_mask = GL_FALSE;
            GLint depth_func = 0;
            GLint cull_face_mode = 0;
            GLint front_face = 0;
            GLint blend_src_rgb = 0;
            GLint blend_dst_rgb = 0;
            GLint blend_src_alpha = 0;
            GLint blend_dst_alpha = 0;
            GLint blend_equation_rgb = 0;
            GLint blend_equation_alpha = 0;
            auto depth_test_enabled = glIsEnabled(GL_DEPTH_TEST) == GL_TRUE;
            auto stencil_test_enabled = glIsEnabled(GL_STENCIL_TEST) == GL_TRUE;
            auto cull_face_enabled = glIsEnabled(GL_CULL_FACE) == GL_TRUE;
            auto scissor_test_enabled = glIsEnabled(GL_SCISSOR_TEST) == GL_TRUE;
            glGetBooleanvRobustANGLE(GL_COLOR_WRITEMASK, 4, nullptr, color_write_mask);
            glGetBooleanvRobustANGLE(GL_DEPTH_WRITEMASK, 1, nullptr, &depth_write_mask);
            glGetIntegervRobustANGLE(GL_DEPTH_FUNC, 1, nullptr, &depth_func);
            glGetIntegervRobustANGLE(GL_CULL_FACE_MODE, 1, nullptr, &cull_face_mode);
            glGetIntegervRobustANGLE(GL_FRONT_FACE, 1, nullptr, &front_face);
            glGetIntegervRobustANGLE(GL_BLEND_SRC_RGB, 1, nullptr, &blend_src_rgb);
            glGetIntegervRobustANGLE(GL_BLEND_DST_RGB, 1, nullptr, &blend_dst_rgb);
            glGetIntegervRobustANGLE(GL_BLEND_SRC_ALPHA, 1, nullptr, &blend_src_alpha);
            glGetIntegervRobustANGLE(GL_BLEND_DST_ALPHA, 1, nullptr, &blend_dst_alpha);
            glGetIntegervRobustANGLE(GL_BLEND_EQUATION_RGB, 1, nullptr, &blend_equation_rgb);
            glGetIntegervRobustANGLE(GL_BLEND_EQUATION_ALPHA, 1, nullptr, &blend_equation_alpha);
            auto color_mask_supported = color_write_mask[0] == GL_TRUE
                && color_write_mask[1] == GL_TRUE
                && color_write_mask[2] == GL_TRUE
                && color_write_mask[3] == GL_TRUE;
            auto blend_state_supported = glIsEnabled(GL_BLEND) == GL_TRUE
                && blend_src_rgb == GL_SRC_ALPHA
                && blend_dst_rgb == GL_ONE_MINUS_SRC_ALPHA
                && blend_equation_rgb == GL_FUNC_ADD;
            auto solid_uniforms_supported = active_uniform_count <= 5;
            auto depth_state_supported = !depth_test_enabled || depth_write_mask == GL_FALSE;
            auto cull_state_supported = !cull_face_enabled || cull_face_mode != GL_FRONT_AND_BACK;
            auto solid_gl_state_supported = color_mask_supported
                && blend_state_supported
                && depth_state_supported
                && !stencil_test_enabled
                && cull_state_supported
                && !scissor_test_enabled;
            auto can_replay = position_layout_supported && position_ready && element_ready && destination_format.has_value()
                && (type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT)
                && mode == GL_TRIANGLES
                && replay_viewport_valid
                && solid_uniforms_supported
                && solid_gl_state_supported
                && solid_replay_enabled;
            if (attempt_count <= 24 || attempt_count % 120 == 0) {
                dbgln("MUNDO_WEBGL_POST_DIRECT_VULKAN_SOLID_REPLAY_ATTEMPT count={} program={} draw_count={} type={} offset={} active_attribs={} active_uniforms={} solid_uniforms_supported={} bound_texture={} bound_texture_video={} bound_texture_render_target={} bound_texture_render_target_write_count={} bound_texture_render_target_size={}x{} position_layout_supported={} position_ready={} position_bytes={} element_ready={} element_bytes={} destination_format={} enabled={} ready={} reason={} next_step={}",
                    attempt_count,
                    program_handle,
                    count,
                    type,
                    offset,
                    active_attrib_count,
                    active_uniform_count,
                    solid_uniforms_supported,
                    bound_texture_handle,
                    bound_texture_has_video_backing,
                    bound_texture_render_target_written,
                    bound_texture_render_target_write_count,
                    bound_texture_render_target_width,
                    bound_texture_render_target_height,
                    position_layout_supported,
                    position_ready,
                    position_data.size(),
                    element_ready,
                    element_data.size(),
                    destination_format.value_or(0),
                    solid_replay_enabled,
                    can_replay,
                    !position_layout_supported ? "unsupported_position_layout"sv
                        : !position_ready ? "missing_position_shadow"sv
                        : !element_ready ? "missing_index_shadow"sv
                        : !destination_format.has_value() ? "missing_vulkan_target_format"sv
                        : !(type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT) ? "unsupported_index_type"sv
                        : mode != GL_TRIANGLES ? "unsupported_primitive_mode"sv
                        : !replay_viewport_valid ? "invalid_viewport"sv
                        : !solid_uniforms_supported ? "unsupported_solid_shader_uniform_shape"sv
                        : !color_mask_supported ? "unsupported_color_mask_state"sv
                        : !blend_state_supported ? "unsupported_blend_state"sv
                        : !depth_state_supported ? "unsupported_depth_state"sv
                        : stencil_test_enabled ? "unsupported_stencil_state"sv
                        : !cull_state_supported ? "unsupported_cull_state"sv
                        : scissor_test_enabled ? "unsupported_scissor_state"sv
                        : !solid_replay_enabled ? "solid_replay_disabled_by_environment"sv
                        : "ready"sv,
                    can_replay ? "execute_solid_vulkan_mesh_and_skip_matching_gl_draw" : "keep_gl_draw_until_replay_ready");
                dbgln("MUNDO_WEBGL_POST_DIRECT_VULKAN_SOLID_GL_STATE count={} program={} blend={} blend_src_rgb={} blend_dst_rgb={} blend_src_alpha={} blend_dst_alpha={} blend_equation_rgb={} blend_equation_alpha={} depth_test={} depth_func={} depth_write={} stencil_test={} cull_face={} cull_face_mode={} front_face={} scissor_test={} color_mask=({},{},{},{}) diffuse=({}, {}, {}, {}) opacity={} output_intensity={} reason=solid_replay_needs_matching_gl_state next_step=map_blend_depth_cull_color_mask_before_enabling_solid_vulkan_replay",
                    attempt_count,
                    program_handle,
                    glIsEnabled(GL_BLEND) == GL_TRUE,
                    blend_src_rgb,
                    blend_dst_rgb,
                    blend_src_alpha,
                    blend_dst_alpha,
                    blend_equation_rgb,
                    blend_equation_alpha,
                    depth_test_enabled,
                    depth_func,
                    depth_write_mask == GL_TRUE,
                    stencil_test_enabled,
                    cull_face_enabled,
                    cull_face_mode,
                    front_face,
                    scissor_test_enabled,
                    color_write_mask[0] == GL_TRUE,
                    color_write_mask[1] == GL_TRUE,
                    color_write_mask[2] == GL_TRUE,
                    color_write_mask[3] == GL_TRUE,
                    uniform_snapshot.diffuse[0],
                    uniform_snapshot.diffuse[1],
                    uniform_snapshot.diffuse[2],
                    uniform_snapshot.diffuse[3],
                    uniform_snapshot.opacity,
                    uniform_snapshot.output_intensity);
            }
            auto has_color_effect = color_write_mask[0] == GL_TRUE
                || color_write_mask[1] == GL_TRUE
                || color_write_mask[2] == GL_TRUE
                || color_write_mask[3] == GL_TRUE;
            auto has_depth_effect = depth_test_enabled && depth_write_mask == GL_TRUE;
            auto has_stencil_effect = stencil_test_enabled;
            if (!has_color_effect && !has_depth_effect && !has_stencil_effect) {
                static size_t s_skipped_no_effect_solid_count { 0 };
                auto skipped_no_effect_count = ++s_skipped_no_effect_solid_count;
                if (skipped_no_effect_count <= 24 || skipped_no_effect_count % 120 == 0) {
                    dbgln("MUNDO_WEBGL_SKIP_POST_DIRECT_VULKAN_NO_EFFECT_SOLID count={} attempt_count={} program={} draw_count={} type={} offset={} reason=no_color_depth_or_stencil_effect next_step=verify_present_auto_can_avoid_gl_sync_after_skipping_noop_draw",
                        skipped_no_effect_count,
                        attempt_count,
                        program_handle,
                        count,
                        type,
                        offset);
                }
                return true;
            }
            auto skip_black_render_target_solid = mundo_webgl_env_enabled_by_default("MUNDO_WEBGL_POST_DIRECT_VULKAN_SKIP_BLACK_RENDER_TARGET_SOLID")
                && bound_texture_render_target_written
                && uniform_snapshot.diffuse[0] == 0.0f
                && uniform_snapshot.diffuse[1] == 0.0f
                && uniform_snapshot.diffuse[2] == 0.0f
                && uniform_snapshot.diffuse[3] == 1.0f
                && uniform_snapshot.opacity == 1.0f
                && uniform_snapshot.output_intensity == 1.0f;
            if (skip_black_render_target_solid) {
                static size_t s_skipped_black_render_target_solid_count { 0 };
                auto skipped_black_solid_count = ++s_skipped_black_render_target_solid_count;
                if (skipped_black_solid_count <= 24 || skipped_black_solid_count % 120 == 0) {
                    dbgln("MUNDO_WEBGL_SKIP_POST_DIRECT_BLACK_RENDER_TARGET_SOLID count={} attempt_count={} program={} bound_texture={} render_target_write_count={} render_target_size={}x{} draw_count={} type={} offset={} reason=black_render_target_solid_would_occlude_post_video_panels next_step=verify_panels_visible_then_replace_with_correct_gl_equivalent_composition",
                        skipped_black_solid_count,
                        attempt_count,
                        program_handle,
                        bound_texture_handle,
                        bound_texture_render_target_write_count,
                        bound_texture_render_target_width,
                        bound_texture_render_target_height,
                        count,
                        type,
                        offset);
                }
                return true;
            }
            if (!can_replay)
                return false;

            auto solid_pipeline_probe = m_context->probe_vulkan_solid_mesh_pipeline(destination_format.value(), uniform_snapshot, position_data, element_data, count, type, static_cast<GLintptr>(offset), viewport[0], viewport[1], viewport[2], viewport[3], attempt_count, cull_face_enabled, static_cast<u32>(cull_face_mode), static_cast<u32>(front_face));
            if (!solid_pipeline_probe.executed)
                return false;

            m_context->note_direct_vulkan_video_draw_submitted();
            static size_t s_post_direct_solid_replaced_count { 0 };
            auto replaced_count = ++s_post_direct_solid_replaced_count;
            if (replaced_count <= 24 || replaced_count % 120 == 0) {
                dbgln("MUNDO_WEBGL_POST_DIRECT_VULKAN_SOLID_REPLAY_REPLACE_GL count={} program={} draw_count={} type={} offset={} reason=solid_vulkan_mesh_draw_executed next_step=verify_remaining_post_video_gl_draws",
                    replaced_count,
                    program_handle,
                    count,
                    type,
                    offset);
            }
            return true;
        };
        if (try_post_direct_vulkan_solid_mesh_replay()) {
            needs_to_present();
            return;
        }

        auto try_post_direct_vulkan_textured_render_target_replay = [&]() -> bool {
            if (!direct_vulkan_mesh_mode || !gl_after_direct_vulkan_video || vulkan_video_draw_used_sampler)
                return false;

            GLuint program_handle = 0;
            if (m_current_program) {
                auto handle_or_error = m_current_program->handle(this);
                if (handle_or_error.is_error())
                    return false;
                program_handle = handle_or_error.value();
            }
            if (!program_handle)
                return false;

            GLint active_uniform_count = 0;
            glGetProgramivRobustANGLE(program_handle, GL_ACTIVE_UNIFORMS, 1, nullptr, &active_uniform_count);

            GC::Ptr<WebGLTexture> source_texture = nullptr;
            if (m_texture_binding_2d && !m_texture_binding_2d->has_hardware_video_backing() && m_texture_binding_2d->mundo_render_target_write_state().has_value())
                source_texture = m_texture_binding_2d;

            if (!source_texture) {
                auto uniforms_to_scan_for_source = active_uniform_count < 16 ? active_uniform_count : 16;
                for (GLint index = 0; index < uniforms_to_scan_for_source; ++index) {
                    GLint uniform_size = 0;
                    GLenum uniform_type = 0;
                    GLsizei uniform_length = 0;
                    GLchar uniform_name[256];
                    glGetActiveUniform(program_handle, static_cast<GLuint>(index), sizeof(uniform_name), &uniform_length, &uniform_size, &uniform_type, uniform_name);
                    if (!uniform_length || !mundo_webgl_is_sampler_uniform_type(uniform_type))
                        continue;
                    auto location = glGetUniformLocation(program_handle, uniform_name);
                    if (location < 0)
                        continue;
                    GLint sampler_unit = -1;
                    glGetUniformiv(program_handle, location, &sampler_unit);
                    if (sampler_unit < 0 || static_cast<size_t>(sampler_unit) >= m_mundo_texture_binding_2d_by_unit.size())
                        continue;
                    auto sampler_texture = m_mundo_texture_binding_2d_by_unit[static_cast<size_t>(sampler_unit)];
                    if (!sampler_texture || sampler_texture->has_hardware_video_backing())
                        continue;
                    if (sampler_texture->mundo_render_target_write_state().has_value()) {
                        source_texture = sampler_texture;
                        break;
                    }
                }
            }
            if (!source_texture)
                return false;

            auto const& render_target_state = source_texture->mundo_render_target_write_state();
            if (!render_target_state.has_value())
                return false;

            auto source_handle_or_error = source_texture->handle(this);
            if (source_handle_or_error.is_error())
                return false;
            auto source_texture_handle = source_handle_or_error.value();

            GLint active_attrib_count = 0;
            GLint element_array_buffer = 0;
            GLint viewport[4] {};
            glGetProgramivRobustANGLE(program_handle, GL_ACTIVE_ATTRIBUTES, 1, nullptr, &active_attrib_count);
            glGetIntegervRobustANGLE(GL_ELEMENT_ARRAY_BUFFER_BINDING, 1, nullptr, &element_array_buffer);
            glGetIntegervRobustANGLE(GL_VIEWPORT, 4, nullptr, viewport);

            static size_t s_post_direct_textured_render_target_attempt_count { 0 };
            auto attempt_count = ++s_post_direct_textured_render_target_attempt_count;
            OpenGLContext::VulkanSolidMeshUniformSnapshot uniform_snapshot;
            for (size_t i = 0; i < 16; ++i) {
                uniform_snapshot.model_view_matrix[i] = (i % 5) == 0 ? 1.0f : 0.0f;
                uniform_snapshot.projection_matrix[i] = (i % 5) == 0 ? 1.0f : 0.0f;
            }
            size_t sampler_uniform_count = 0;
            size_t sampler_render_target_count = 0;
            size_t sampler_video_count = 0;
            size_t sampler_snapshot_complete_count = 0;
            size_t sampler_unresolved_count = 0;
            GC::Ptr<WebGLTexture> static_sampler_texture;
            GLuint static_sampler_texture_handle = 0;
            auto uniforms_to_scan = active_uniform_count < 16 ? active_uniform_count : 16;
            for (GLint index = 0; index < uniforms_to_scan; ++index) {
                GLint uniform_size = 0;
                GLenum uniform_type = 0;
                GLsizei uniform_length = 0;
                GLchar uniform_name[256];
                glGetActiveUniform(program_handle, static_cast<GLuint>(index), sizeof(uniform_name), &uniform_length, &uniform_size, &uniform_type, uniform_name);
                if (!uniform_length)
                    continue;
                auto uniform_name_view = StringView { uniform_name, static_cast<size_t>(uniform_length) };
                auto is_sampler = mundo_webgl_is_sampler_uniform_type(uniform_type);
                if (is_sampler)
                    ++sampler_uniform_count;
                auto location = glGetUniformLocation(program_handle, uniform_name);
                GLint sampler_unit = -1;
                GLuint sampler_texture_handle = 0;
                bool sampler_texture_has_video_backing = false;
                bool sampler_texture_render_target_written = false;
                bool sampler_texture_snapshot_complete = false;
                u32 sampler_texture_render_target_width = 0;
                u32 sampler_texture_render_target_height = 0;
                size_t sampler_texture_render_target_write_count = 0;
                if (location >= 0) {
                    if (uniform_type == GL_FLOAT_MAT4 && uniform_name_view == "modelViewMatrix"sv) {
                        glGetUniformfv(program_handle, location, uniform_snapshot.model_view_matrix.data());
                        uniform_snapshot.has_model_view_matrix = true;
                    } else if (uniform_type == GL_FLOAT_MAT4 && uniform_name_view == "projectionMatrix"sv) {
                        glGetUniformfv(program_handle, location, uniform_snapshot.projection_matrix.data());
                        uniform_snapshot.has_projection_matrix = true;
                    } else if (uniform_type == GL_FLOAT && (uniform_name_view == "opacity"sv || uniform_name_view == "uOpacity"sv)) {
                        glGetUniformfv(program_handle, location, &uniform_snapshot.opacity);
                    } else if (uniform_type == GL_FLOAT && (uniform_name_view == "uOutputIntensity"sv || uniform_name_view == "outputIntensity"sv)) {
                        glGetUniformfv(program_handle, location, &uniform_snapshot.output_intensity);
                    } else if ((uniform_type == GL_FLOAT_VEC3 || uniform_type == GL_FLOAT_VEC4) && (uniform_name_view == "diffuse"sv || uniform_name_view == "uTintColor"sv)) {
                        Array<float, 4> diffuse { 1.0f, 1.0f, 1.0f, 1.0f };
                        glGetUniformfv(program_handle, location, diffuse.data());
                        uniform_snapshot.diffuse = diffuse;
                    } else if (is_sampler) {
                        glGetUniformiv(program_handle, location, &sampler_unit);
                        if (sampler_unit >= 0 && static_cast<size_t>(sampler_unit) < m_mundo_texture_binding_2d_by_unit.size()) {
                            if (auto sampler_texture = m_mundo_texture_binding_2d_by_unit[static_cast<size_t>(sampler_unit)]) {
                                sampler_texture_handle = sampler_texture->handle(this).value_or(0);
                                sampler_texture_has_video_backing = sampler_texture->has_hardware_video_backing();
                                auto const& sampler_render_target_state = sampler_texture->mundo_render_target_write_state();
                                sampler_texture_render_target_written = sampler_render_target_state.has_value();
                                if (sampler_render_target_state.has_value()) {
                                    sampler_texture_render_target_width = sampler_render_target_state->last_viewport_width;
                                    sampler_texture_render_target_height = sampler_render_target_state->last_viewport_height;
                                    sampler_texture_render_target_write_count = sampler_render_target_state->write_count;
                                }
                                auto const& sampler_snapshot = sampler_texture->mundo_texture_upload_snapshot();
                                sampler_texture_snapshot_complete = sampler_snapshot.has_value() && sampler_snapshot->complete;
                            }
                        }
                    }
                }
                if (is_sampler) {
                    if (sampler_texture_has_video_backing)
                        ++sampler_video_count;
                    else if (sampler_texture_render_target_written)
                        ++sampler_render_target_count;
                    else if (sampler_texture_snapshot_complete) {
                        ++sampler_snapshot_complete_count;
                        if (!static_sampler_texture) {
                            static_sampler_texture = m_mundo_texture_binding_2d_by_unit[static_cast<size_t>(sampler_unit)];
                            static_sampler_texture_handle = sampler_texture_handle;
                        }
                    }
                    else
                        ++sampler_unresolved_count;
                    if (attempt_count <= 24 || attempt_count % 120 == 0) {
                        dbgln("MUNDO_WEBGL_POST_DIRECT_VULKAN_TEXTURED_RT_REPLAY_SAMPLER count={} program={} index={} name={} type={} size={} location={} sampler_unit={} sampler_texture={} sampler_video_backing={} sampler_render_target_written={} sampler_render_target_size={}x{} sampler_render_target_write_count={} sampler_snapshot_complete={} current_source_texture={} current_source_write_count={} next_step={}",
                            attempt_count,
                            program_handle,
                            index,
                            uniform_name_view,
                            uniform_type,
                            uniform_size,
                            location,
                            sampler_unit,
                            sampler_texture_handle,
                            sampler_texture_has_video_backing,
                            sampler_texture_render_target_written,
                            sampler_texture_render_target_width,
                            sampler_texture_render_target_height,
                            sampler_texture_render_target_write_count,
                            sampler_texture_snapshot_complete,
                            source_texture_handle,
                            render_target_state->write_count,
                            sampler_texture_handle == source_texture_handle ? "source_sampler_for_vulkan_replay"sv
                                : sampler_texture_render_target_written ? "secondary_render_target_sampler_needs_vulkan_multisampler_shader"sv
                                : sampler_texture_snapshot_complete ? "secondary_static_sampler_needs_vulkan_multisampler_shader"sv
                                : sampler_texture_has_video_backing ? "secondary_video_sampler_needs_ycbcr_or_prior_replay"sv
                                : "capture_missing_sampler_binding"sv);
                    }
                }
            }

            ReadonlyBytes position_data;
            ReadonlyBytes uv_data;
            bool position_ready = false;
            bool uv_ready = false;
            bool position_layout_supported = false;
            bool uv_layout_supported = false;
            bool has_position_attrib = false;
            bool has_uv_attrib = false;
            for (GLint index = 0; index < active_attrib_count; ++index) {
                GLint attrib_size = 0;
                GLenum attrib_type = 0;
                GLsizei attrib_length = 0;
                GLchar attrib_name[256];
                glGetActiveAttrib(program_handle, static_cast<GLuint>(index), sizeof(attrib_name), &attrib_length, &attrib_size, &attrib_type, attrib_name);
                if (!attrib_length)
                    continue;
                auto attrib_name_view = StringView { attrib_name, static_cast<size_t>(attrib_length) };
                auto location = glGetAttribLocation(program_handle, attrib_name);
                if (location < 0)
                    continue;
                GLint enabled = 0;
                GLint array_size = 0;
                GLint array_type = 0;
                GLint stride = 0;
                GLint buffer = 0;
                void* pointer = nullptr;
                glGetVertexAttribivRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_ENABLED, 1, nullptr, &enabled);
                glGetVertexAttribivRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_SIZE, 1, nullptr, &array_size);
                glGetVertexAttribivRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_TYPE, 1, nullptr, &array_type);
                glGetVertexAttribivRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_STRIDE, 1, nullptr, &stride);
                glGetVertexAttribivRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, 1, nullptr, &buffer);
                glGetVertexAttribPointervRobustANGLE(static_cast<GLuint>(location), GL_VERTEX_ATTRIB_ARRAY_POINTER, 1, nullptr, &pointer);
                auto webgl_buffer = mundo_buffer_for_handle(static_cast<GLuint>(buffer));
                if (attrib_name_view == "position"sv) {
                    has_position_attrib = true;
                    position_layout_supported = enabled && array_size == 3 && array_type == GL_FLOAT && stride == static_cast<GLint>(sizeof(float) * 3) && reinterpret_cast<uintptr_t>(pointer) == 0;
                    if (position_layout_supported && webgl_buffer && webgl_buffer->has_complete_shadow_data()) {
                        position_data = webgl_buffer->shadow_data();
                        position_ready = true;
                    }
                } else if (attrib_name_view == "uv"sv) {
                    has_uv_attrib = true;
                    uv_layout_supported = enabled && array_size == 2 && array_type == GL_FLOAT && stride == static_cast<GLint>(sizeof(float) * 2) && reinterpret_cast<uintptr_t>(pointer) == 0;
                    if (uv_layout_supported && webgl_buffer && webgl_buffer->has_complete_shadow_data()) {
                        uv_data = webgl_buffer->shadow_data();
                        uv_ready = true;
                    }
                }
            }

            ReadonlyBytes element_data;
            bool element_ready = false;
            if (auto element_buffer = mundo_buffer_for_handle(static_cast<GLuint>(element_array_buffer)); element_buffer && element_buffer->has_complete_shadow_data()) {
                element_data = element_buffer->shadow_data();
                element_ready = true;
            }

            auto destination_format = m_context->vulkan_painting_surface_format();
            auto textured_replay_enabled = mundo_webgl_env_enabled_by_default("MUNDO_WEBGL_POST_DIRECT_VULKAN_TEXTURED_RENDER_TARGET_REPLAY");
            auto source_render_target_vulkan_backed = render_target_state->current_contents_vulkan_backed;
            auto resolved_static_extra_samplers_supported = mundo_webgl_env_enabled_by_default("MUNDO_WEBGL_POST_DIRECT_VULKAN_TEXTURED_RENDER_TARGET_AUTO_EXTRA_STATIC_SAMPLERS")
                && sampler_render_target_count == 1
                && sampler_video_count == 0
                && sampler_unresolved_count == 0
                && sampler_snapshot_complete_count > 0
                && sampler_render_target_count + sampler_snapshot_complete_count == sampler_uniform_count;
            auto alpha_map_replay_enabled = mundo_webgl_env_enabled_by_default("MUNDO_WEBGL_POST_DIRECT_VULKAN_TEXTURED_RENDER_TARGET_ALPHA_MAP");
            auto allow_extra_samplers = mundo_webgl_env_opt_in_enabled("MUNDO_WEBGL_POST_DIRECT_VULKAN_TEXTURED_RENDER_TARGET_ALLOW_EXTRA_SAMPLERS")
                || resolved_static_extra_samplers_supported;
            auto sampler_count_supported = sampler_uniform_count == 1
                || (allow_extra_samplers
                    && sampler_render_target_count == 1
                    && sampler_video_count == 0
                    && sampler_unresolved_count == 0
                    && sampler_render_target_count + sampler_snapshot_complete_count == sampler_uniform_count);
            auto replay_viewport_valid = viewport[2] > 0 && viewport[3] > 0;
            auto can_replay = textured_replay_enabled
                && source_render_target_vulkan_backed
                && active_attrib_count == 2
                && sampler_count_supported
                && has_position_attrib
                && has_uv_attrib
                && position_layout_supported
                && uv_layout_supported
                && position_ready
                && uv_ready
                && element_ready
                && destination_format.has_value()
                && (type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT)
                && replay_viewport_valid
                && mode == GL_TRIANGLES;
            if (attempt_count <= 24 || attempt_count % 120 == 0) {
                dbgln("MUNDO_WEBGL_POST_DIRECT_VULKAN_TEXTURED_RT_REPLAY_ATTEMPT count={} program={} source_texture={} source_write_count={} source_vulkan_backed={} source_vulkan_write_count={} source_viewport={}x{} source_program={} draw_count={} type={} offset={} active_attribs={} sampler_uniforms={} sampler_render_targets={} sampler_videos={} sampler_snapshots={} sampler_unresolved={} allow_extra_samplers={} alpha_map_replay_enabled={} sampler_count_supported={} has_position={} has_uv={} position_layout_supported={} uv_layout_supported={} position_ready={} uv_ready={} position_bytes={} uv_bytes={} element_ready={} element_bytes={} destination_format={} enabled={} ready={} reason={} next_step={}",
                    attempt_count,
                    program_handle,
                    source_texture_handle,
                    render_target_state->write_count,
                    source_render_target_vulkan_backed,
                    render_target_state->vulkan_write_count,
                    render_target_state->last_viewport_width,
                    render_target_state->last_viewport_height,
                    render_target_state->last_program,
                    count,
                    type,
                    offset,
                    active_attrib_count,
                    sampler_uniform_count,
                    sampler_render_target_count,
                    sampler_video_count,
                    sampler_snapshot_complete_count,
                    sampler_unresolved_count,
                    allow_extra_samplers,
                    alpha_map_replay_enabled,
                    sampler_count_supported,
                    has_position_attrib,
                    has_uv_attrib,
                    position_layout_supported,
                    uv_layout_supported,
                    position_ready,
                    uv_ready,
                    position_data.size(),
                    uv_data.size(),
                    element_ready,
                    element_data.size(),
                    destination_format.value_or(0),
                    textured_replay_enabled,
                    can_replay,
                    !textured_replay_enabled ? "textured_render_target_replay_explicitly_disabled"sv
                        : !source_render_target_vulkan_backed ? "source_render_target_not_vulkan_backed"sv
                        : active_attrib_count != 2 ? "not_position_uv_mesh"sv
                        : !sampler_count_supported ? "unsupported_sampler_consumer"sv
                        : !has_position_attrib ? "missing_position_attrib"sv
                        : !has_uv_attrib ? "missing_uv_attrib"sv
                        : !position_layout_supported ? "unsupported_position_layout"sv
                        : !uv_layout_supported ? "unsupported_uv_layout"sv
                        : !position_ready ? "missing_position_shadow"sv
                        : !uv_ready ? "missing_uv_shadow"sv
                        : !element_ready ? "missing_index_shadow"sv
                        : !destination_format.has_value() ? "missing_vulkan_target_format"sv
                        : !(type == GL_UNSIGNED_SHORT || type == GL_UNSIGNED_INT) ? "unsupported_index_type"sv
                        : !replay_viewport_valid ? "invalid_viewport"sv
                        : mode != GL_TRIANGLES ? "unsupported_primitive_mode"sv
                        : "ready"sv,
                    can_replay ? "execute_textured_vulkan_mesh_and_skip_matching_gl_draw" : "keep_gl_draw_until_replay_ready");
            }
            if (!can_replay)
                return false;

            auto source_image_or_error = m_context->get_or_create_vulkan_rgba_render_target_image(source_texture_handle, render_target_state->last_viewport_width ? render_target_state->last_viewport_width : 1, render_target_state->last_viewport_height ? render_target_state->last_viewport_height : 1, attempt_count);
            if (source_image_or_error.is_error())
                return false;
            auto* source_image_texture = source_image_or_error.release_value();
            Gfx::VulkanImage* alpha_image = nullptr;
            if (static_sampler_texture && !alpha_map_replay_enabled) {
                if (attempt_count <= 24 || attempt_count % 120 == 0) {
                    dbgln("MUNDO_WEBGL_POST_DIRECT_VULKAN_TEXTURED_RT_REPLAY_ALPHA_UPLOAD count={} program={} status=skipped source_texture={} alpha_texture={} reason=alpha_map_replay_disabled_by_default next_step=verify_panel_visibility_without_alpha_mask_then_reenable_with_correct_channel_semantics",
                        attempt_count,
                        program_handle,
                        source_texture_handle,
                        static_sampler_texture_handle);
                }
            }
            if (static_sampler_texture && alpha_map_replay_enabled) {
                auto const& snapshot = static_sampler_texture->mundo_texture_upload_snapshot();
                if (!snapshot.has_value() || !snapshot->complete) {
                    if (attempt_count <= 24 || attempt_count % 120 == 0) {
                        dbgln("MUNDO_WEBGL_POST_DIRECT_VULKAN_TEXTURED_RT_REPLAY_ALPHA_UPLOAD count={} program={} status=failed reason=static_sampler_snapshot_missing source_texture={} alpha_texture={} next_step=capture_complete_static_sampler_before_multisampler_replay",
                            attempt_count,
                            program_handle,
                            source_texture_handle,
                            static_sampler_texture_handle);
                    }
                    return false;
                }

                Optional<ByteBuffer> converted_rgba_pixels;
                ReadonlyBytes rgba_pixels;
                auto const expected_rgba_size = static_cast<size_t>(snapshot->width) * static_cast<size_t>(snapshot->height) * 4;
                if (snapshot->format == GL_RGBA && snapshot->type == GL_UNSIGNED_BYTE && snapshot->pixels.size() >= expected_rgba_size) {
                    rgba_pixels = snapshot->pixels.bytes().slice(0, expected_rgba_size);
                } else if ((snapshot->format == GL_ALPHA || snapshot->format == GL_LUMINANCE) && snapshot->type == GL_UNSIGNED_BYTE && snapshot->pixels.size() >= static_cast<size_t>(snapshot->width) * static_cast<size_t>(snapshot->height)) {
                    converted_rgba_pixels = ByteBuffer::create_uninitialized(expected_rgba_size).release_value_but_fixme_should_propagate_errors();
                    auto source_pixels = snapshot->pixels.bytes();
                    auto destination_pixels = converted_rgba_pixels->bytes();
                    for (size_t i = 0; i < static_cast<size_t>(snapshot->width) * static_cast<size_t>(snapshot->height); ++i) {
                        auto value = source_pixels[i];
                        destination_pixels[i * 4 + 0] = value;
                        destination_pixels[i * 4 + 1] = value;
                        destination_pixels[i * 4 + 2] = value;
                        destination_pixels[i * 4 + 3] = value;
                    }
                    rgba_pixels = converted_rgba_pixels->bytes();
                } else {
                    if (attempt_count <= 24 || attempt_count % 120 == 0) {
                        dbgln("MUNDO_WEBGL_POST_DIRECT_VULKAN_TEXTURED_RT_REPLAY_ALPHA_UPLOAD count={} program={} status=failed reason=unsupported_static_sampler_snapshot_format source_texture={} alpha_texture={} snapshot_size={}x{} internal_format={} format={} type={} byte_length={} next_step=add_snapshot_format_conversion_for_multisampler_vulkan_replay",
                            attempt_count,
                            program_handle,
                            source_texture_handle,
                            static_sampler_texture_handle,
                            snapshot->width,
                            snapshot->height,
                            snapshot->internal_format,
                            snapshot->format,
                            snapshot->type,
                            snapshot->byte_length);
                    }
                    return false;
                }

                auto signature = pair_int_hash(Traits<ReadonlyBytes>::hash(rgba_pixels), pair_int_hash(u32_hash(snapshot->width), pair_int_hash(u32_hash(snapshot->height), u32_hash(rgba_pixels.size()))));
                auto alpha_image_or_error = m_context->get_or_create_vulkan_rgba_static_texture_image(static_sampler_texture_handle, snapshot->width, snapshot->height, signature, rgba_pixels, attempt_count);
                if (alpha_image_or_error.is_error())
                    return false;
                alpha_image = alpha_image_or_error.release_value()->image.ptr();
            }
            auto textured_pipeline_probe = m_context->probe_vulkan_textured_mesh_pipeline(destination_format.value(), *source_image_texture->image, uniform_snapshot, position_data, uv_data, element_data, count, type, static_cast<GLintptr>(offset), viewport[0], viewport[1], viewport[2], viewport[3], attempt_count, alpha_image);
            if (attempt_count <= 24 || attempt_count % 120 == 0) {
                dbgln("MUNDO_WEBGL_POST_DIRECT_VULKAN_TEXTURED_RT_REPLAY_PROBE_RESULT count={} program={} source_texture={} attempted={} supported={} executed={} reason={} next_step={}",
                    attempt_count,
                    program_handle,
                    source_texture_handle,
                    textured_pipeline_probe.attempted,
                    textured_pipeline_probe.supported,
                    textured_pipeline_probe.executed,
                    textured_pipeline_probe.reason,
                    textured_pipeline_probe.executed ? "skip_matching_gl_draw_and_verify_full_gpu_chain" : "fix_post_direct_textured_render_target_replay");
            }
            if (!textured_pipeline_probe.executed)
                return false;

            m_context->note_direct_vulkan_video_draw_submitted();
            return true;
        };
        if (try_post_direct_vulkan_textured_render_target_replay()) {
            needs_to_present();
            return;
        }
#endif
        if (gl_after_direct_vulkan_video && note_mundo_framebuffer_draw("drawElements", mode, count, type, offset, true)) {
            static size_t s_post_direct_render_target_replay_skip_count { 0 };
            auto skip_count = ++s_post_direct_render_target_replay_skip_count;
            if (skip_count <= 24 || skip_count % 120 == 0) {
                GLuint program_handle = 0;
                if (m_current_program) {
                    auto handle_or_error = m_current_program->handle(this);
                    if (!handle_or_error.is_error())
                        program_handle = handle_or_error.value();
                }
                dbgln("MUNDO_WEBGL_POST_DIRECT_VULKAN_RENDER_TARGET_REPLAY_REPLACE_GL count={} program={} draw_count={} type={} offset={} reason=render_target_vulkan_replay_executed next_step=verify_remaining_post_video_gl_draws",
                    skip_count,
                    program_handle,
                    count,
                    type,
                    offset);
            }
            m_context->note_direct_vulkan_video_draw_submitted();
            needs_to_present();
            return;
        }
        if (gl_after_direct_vulkan_video && m_texture_binding_2d && !m_texture_binding_2d->has_hardware_video_backing()) {
            auto const& texture_snapshot = m_texture_binding_2d->mundo_texture_upload_snapshot();
            auto const& render_target_write_state = m_texture_binding_2d->mundo_render_target_write_state();
            auto texture_has_complete_source = (texture_snapshot.has_value() && texture_snapshot->complete) || (render_target_write_state.has_value() && render_target_write_state->current_contents_vulkan_backed);
            if (!texture_has_complete_source) {
                static size_t s_skipped_post_direct_incomplete_texture_draw_count { 0 };
                auto skip_count = ++s_skipped_post_direct_incomplete_texture_draw_count;
                if (skip_count <= 24 || skip_count % 120 == 0) {
                    GLuint program_handle = 0;
                    GLuint texture_handle = 0;
                    if (m_current_program) {
                        auto handle_or_error = m_current_program->handle(this);
                        if (!handle_or_error.is_error())
                            program_handle = handle_or_error.value();
                    }
                    auto texture_handle_or_error = m_texture_binding_2d->handle(this);
                    if (!texture_handle_or_error.is_error())
                        texture_handle = texture_handle_or_error.value();
                    dbgln("MUNDO_WEBGL_SKIP_POST_DIRECT_INCOMPLETE_TEXTURE_DRAW count={} program={} texture={} draw_count={} type={} offset={} snapshot={} complete={} snapshot_size={}x{} render_target_written={} render_target_vulkan_backed={} reason=post_direct_texture_has_no_complete_source next_step=avoid_gl_present_from_empty_overlay_draw",
                        skip_count,
                        program_handle,
                        texture_handle,
                        count,
                        type,
                        offset,
                        texture_snapshot.has_value(),
                        texture_snapshot.has_value() ? texture_snapshot->complete : false,
                        texture_snapshot.has_value() ? texture_snapshot->width : 0,
                        texture_snapshot.has_value() ? texture_snapshot->height : 0,
                        render_target_write_state.has_value(),
                        render_target_write_state.has_value() ? render_target_write_state->current_contents_vulkan_backed : false);
                }
                m_context->note_direct_vulkan_video_draw_submitted();
                needs_to_present();
                return;
            }
        }
        glDrawElements(mode, count, type, reinterpret_cast<void*>(offset));
        m_context->note_gl_draw_submitted();
        if (!gl_after_direct_vulkan_video)
            note_mundo_framebuffer_draw("drawElements", mode, count, type, offset);
        if (gl_after_direct_vulkan_video) {
            static size_t s_gl_after_direct_vulkan_video_draw_elements_count { 0 };
            auto after_direct_log_count = ++s_gl_after_direct_vulkan_video_draw_elements_count;
            if (after_direct_log_count <= 24 || after_direct_log_count % 120 == 0) {
                GLint active_texture = 0;
                glGetIntegervRobustANGLE(GL_ACTIVE_TEXTURE, 1, nullptr, &active_texture);
                GLuint after_direct_program_handle = 0;
                if (m_current_program) {
                    auto handle_or_error = m_current_program->handle(this);
                    if (!handle_or_error.is_error())
                        after_direct_program_handle = handle_or_error.value();
                }
                GLuint after_direct_texture_handle = 0;
                if (m_texture_binding_2d) {
                    auto handle_or_error = m_texture_binding_2d->handle(this);
                    if (!handle_or_error.is_error())
                        after_direct_texture_handle = handle_or_error.value();
                }
                GLint framebuffer = 0;
                GLint vertex_array = 0;
                GLint element_array_buffer = 0;
                GLint active_attrib_count = 0;
                GLint active_uniform_count = 0;
                GLint viewport[4] {};
                glGetIntegervRobustANGLE(GL_FRAMEBUFFER_BINDING, 1, nullptr, &framebuffer);
                glGetIntegervRobustANGLE(GL_VERTEX_ARRAY_BINDING, 1, nullptr, &vertex_array);
                glGetIntegervRobustANGLE(GL_ELEMENT_ARRAY_BUFFER_BINDING, 1, nullptr, &element_array_buffer);
                glGetIntegervRobustANGLE(GL_VIEWPORT, 4, nullptr, viewport);
                if (after_direct_program_handle) {
                    glGetProgramivRobustANGLE(after_direct_program_handle, GL_ACTIVE_ATTRIBUTES, 1, nullptr, &active_attrib_count);
                    glGetProgramivRobustANGLE(after_direct_program_handle, GL_ACTIVE_UNIFORMS, 1, nullptr, &active_uniform_count);
                }
                auto video_sampler_uniform = "none"sv;
                auto video_sampler_direct_texture_call = false;
                if (m_current_program && m_current_program->video_sampler_plan().has_value()) {
                    auto const& plan = m_current_program->video_sampler_plan().value();
                    video_sampler_uniform = plan.uniform_name.bytes_as_string_view();
                    video_sampler_direct_texture_call = plan.direct_texture_call;
                }
                dbgln("MUNDO_WEBGL_GL_AFTER_DIRECT_VULKAN_VIDEO_DRAW count={} op=drawElements mode={} draw_count={} type={} offset={} program={} texture={} active_texture={} bound_texture_has_video_backing={} framebuffer={} vertex_array={} element_array_buffer={} active_attribs={} active_uniforms={} viewport={}x{}+{}+{} vulkan_video_draw_executed={} vulkan_video_draw_direct_zero_copy={} vulkan_video_draw_used_sampler={} video_sampler_uniform={} video_sampler_direct_texture_call={} reason=gl_draw_after_direct_vulkan_video_before_present next_step=classify_or_virtualize_this_gl_draw_for_pure_vulkan_present",
                    after_direct_log_count,
                    mode,
                    count,
                    type,
                    offset,
                    after_direct_program_handle,
                    after_direct_texture_handle,
                    active_texture,
                    m_texture_binding_2d && m_texture_binding_2d->has_hardware_video_backing(),
                    framebuffer,
                    vertex_array,
                    element_array_buffer,
                    active_attrib_count,
                    active_uniform_count,
                    viewport[2],
                    viewport[3],
                    viewport[0],
                    viewport[1],
                    vulkan_video_draw_executed,
                    vulkan_video_draw_direct_zero_copy,
                    vulkan_video_draw_used_sampler,
                    video_sampler_uniform,
                    video_sampler_direct_texture_call);
                if (m_texture_binding_2d && !m_texture_binding_2d->has_hardware_video_backing()) {
                    auto const& texture_snapshot = m_texture_binding_2d->mundo_texture_upload_snapshot();
                    auto const& render_target_write_state = m_texture_binding_2d->mundo_render_target_write_state();
                    dbgln("MUNDO_WEBGL_GL_AFTER_DIRECT_VULKAN_TEXTURE_STATE count={} texture={} snapshot={} complete={} size={}x{} internal_format={} format={} type={} bytes={} render_target_written={} render_target_write_count={} render_target_last_viewport={}x{} render_target_last_program={} reason=post_direct_gl_draw_uses_generic_texture next_step={}",
                        after_direct_log_count,
                        after_direct_texture_handle,
                        texture_snapshot.has_value(),
                        texture_snapshot.has_value() ? texture_snapshot->complete : false,
                        texture_snapshot.has_value() ? texture_snapshot->width : 0,
                        texture_snapshot.has_value() ? texture_snapshot->height : 0,
                        texture_snapshot.has_value() ? texture_snapshot->internal_format : 0,
                        texture_snapshot.has_value() ? texture_snapshot->format : 0,
                        texture_snapshot.has_value() ? texture_snapshot->type : 0,
                        texture_snapshot.has_value() ? texture_snapshot->byte_length : 0,
                        render_target_write_state.has_value(),
                        render_target_write_state.has_value() ? render_target_write_state->write_count : 0,
                        render_target_write_state.has_value() ? render_target_write_state->last_viewport_width : 0,
                        render_target_write_state.has_value() ? render_target_write_state->last_viewport_height : 0,
                        render_target_write_state.has_value() ? render_target_write_state->last_program : 0,
                        render_target_write_state.has_value() ? "virtualize_render_target_producer_or_import_gpu_texture"sv : (texture_snapshot.has_value() && texture_snapshot->complete ? "build_vulkan_textured_replay_for_this_draw"sv : "capture_missing_texture_upload_or_support_subimage_updates"sv));
                }
                if (after_direct_log_count <= 8 && after_direct_program_handle) {
                    auto uniforms_to_log = active_uniform_count < 8 ? active_uniform_count : 8;
                    for (GLint index = 0; index < uniforms_to_log; ++index) {
                        GLint uniform_size = 0;
                        GLenum uniform_type = 0;
                        GLsizei uniform_length = 0;
                        GLchar uniform_name[256];
                        glGetActiveUniform(after_direct_program_handle, static_cast<GLuint>(index), sizeof(uniform_name), &uniform_length, &uniform_size, &uniform_type, uniform_name);
                        if (!uniform_length)
                            continue;
                        dbgln("MUNDO_WEBGL_GL_AFTER_DIRECT_VULKAN_UNIFORM count={} program={} index={} name={} type={} size={} sampler={} next_step=map_required_uniform_for_vulkan_replay",
                            after_direct_log_count,
                            after_direct_program_handle,
                            index,
                            StringView { uniform_name, static_cast<size_t>(uniform_length) },
                            uniform_type,
                            uniform_size,
                            mundo_webgl_is_sampler_uniform_type(uniform_type));
                    }
                    auto attribs_to_log = active_attrib_count < 8 ? active_attrib_count : 8;
                    for (GLint index = 0; index < attribs_to_log; ++index) {
                        GLint attrib_size = 0;
                        GLenum attrib_type = 0;
                        GLsizei attrib_length = 0;
                        GLchar attrib_name[256];
                        glGetActiveAttrib(after_direct_program_handle, static_cast<GLuint>(index), sizeof(attrib_name), &attrib_length, &attrib_size, &attrib_type, attrib_name);
                        if (!attrib_length)
                            continue;
                        auto attrib_location = glGetAttribLocation(after_direct_program_handle, attrib_name);
                        GLint enabled = 0;
                        GLint array_size = 0;
                        GLint array_type = 0;
                        GLint stride = 0;
                        if (attrib_location >= 0) {
                            glGetVertexAttribivRobustANGLE(static_cast<GLuint>(attrib_location), GL_VERTEX_ATTRIB_ARRAY_ENABLED, 1, nullptr, &enabled);
                            glGetVertexAttribivRobustANGLE(static_cast<GLuint>(attrib_location), GL_VERTEX_ATTRIB_ARRAY_SIZE, 1, nullptr, &array_size);
                            glGetVertexAttribivRobustANGLE(static_cast<GLuint>(attrib_location), GL_VERTEX_ATTRIB_ARRAY_TYPE, 1, nullptr, &array_type);
                            glGetVertexAttribivRobustANGLE(static_cast<GLuint>(attrib_location), GL_VERTEX_ATTRIB_ARRAY_STRIDE, 1, nullptr, &stride);
                        }
                        dbgln("MUNDO_WEBGL_GL_AFTER_DIRECT_VULKAN_ATTRIB count={} program={} index={} name={} location={} declared_size={} declared_type={} enabled={} array_size={} array_type={} stride={} next_step=map_required_vertex_input_for_vulkan_replay",
                            after_direct_log_count,
                            after_direct_program_handle,
                            index,
                            StringView { attrib_name, static_cast<size_t>(attrib_length) },
                            attrib_location,
                            attrib_size,
                            attrib_type,
                            enabled,
                            array_size,
                            array_type,
                            stride);
                    }
                }
            }
        }
    } else {
        m_context->note_direct_vulkan_video_draw_submitted();
        static size_t s_replaced_draw_count { 0 };
        auto replaced_draw_count = ++s_replaced_draw_count;
        if (replaced_draw_count <= 16 || replaced_draw_count % 120 == 0)
            dbgln("MUNDO_WEBGL_VIDEO_VULKAN_MESH_REPLACE_GL_DRAW count={} mode={} draw_count={} type={} offset={} reason={} next_step=verify_rgba_sampler_path_is_not_required_for_this_video_draw",
                replaced_draw_count,
                mode,
                count,
                type,
                offset,
                direct_zero_copy_replace_gl_draw ? "direct_zero_copy_vulkan_mesh_draw_executed" : "forced_vulkan_mesh_draw_executed");
    }
    record_mundo_webgl_timing_summary("drawElements", (MonotonicTime::now() - start).to_microseconds());
    if (auto duration = mundo_webgl_slow_duration(start); duration.has_value())
        dbgln("MUNDO_WEBGL_TIMING count={} op=drawElements duration={}ms threshold={}ms mode={} count={} type={} offset={} vulkan_video_draw_executed={} replace_gl_draw={} can_replace_gl_draw={}", mundo_webgl_next_timing_count(), duration.value(), mundo_webgl_timing_threshold_ms(), mode, count, type, offset, vulkan_video_draw_executed, replace_gl_draw, can_replace_gl_draw);
    needs_to_present();
}

void WebGLRenderingContextImpl::enable(WebIDL::UnsignedLong cap)
{
    m_context->make_current();
    glEnable(cap);
}

void WebGLRenderingContextImpl::enable_vertex_attrib_array(WebIDL::UnsignedLong index)
{
    m_context->make_current();
    glEnableVertexAttribArray(index);
}

void WebGLRenderingContextImpl::finish()
{
    m_context->make_current();
    auto start = MonotonicTime::now();
    glFinish();
    record_mundo_webgl_timing_summary("finish", (MonotonicTime::now() - start).to_microseconds());
    if (auto duration = mundo_webgl_slow_duration(start); duration.has_value())
        dbgln("MUNDO_WEBGL_TIMING count={} op=finish duration={}ms threshold={}ms", mundo_webgl_next_timing_count(), duration.value(), mundo_webgl_timing_threshold_ms());
}

void WebGLRenderingContextImpl::flush()
{
    m_context->make_current();
    auto start = MonotonicTime::now();
    glFlush();
    record_mundo_webgl_timing_summary("flush", (MonotonicTime::now() - start).to_microseconds());
    if (auto duration = mundo_webgl_slow_duration(start); duration.has_value())
        dbgln("MUNDO_WEBGL_TIMING count={} op=flush duration={}ms threshold={}ms", mundo_webgl_next_timing_count(), duration.value(), mundo_webgl_timing_threshold_ms());
}

void WebGLRenderingContextImpl::framebuffer_renderbuffer(WebIDL::UnsignedLong target, WebIDL::UnsignedLong attachment, WebIDL::UnsignedLong renderbuffertarget, GC::Root<WebGLRenderbuffer> renderbuffer)
{
    m_context->make_current();

    auto renderbuffer_handle = 0;
    if (renderbuffer) {
        auto handle_or_error = renderbuffer->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        renderbuffer_handle = handle_or_error.release_value();
    }
    glFramebufferRenderbuffer(target, attachment, renderbuffertarget, renderbuffer_handle);
}

void WebGLRenderingContextImpl::framebuffer_texture2d(WebIDL::UnsignedLong target, WebIDL::UnsignedLong attachment, WebIDL::UnsignedLong textarget, GC::Root<WebGLTexture> texture, WebIDL::Long level)
{
    m_context->make_current();

    auto texture_handle = 0;
    if (texture) {
        auto handle_or_error = texture->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        texture_handle = handle_or_error.release_value();
    }
    glFramebufferTexture2D(target, attachment, textarget, texture_handle, level);
    if (attachment == GL_COLOR_ATTACHMENT0 && m_framebuffer_binding) {
        m_framebuffer_binding->set_mundo_color_attachment_texture(texture.ptr());
        auto framebuffer_handle = m_framebuffer_binding->handle(this).value_or(0);
        static size_t s_mundo_framebuffer_texture2d_count { 0 };
        auto log_count = ++s_mundo_framebuffer_texture2d_count;
        if (log_count <= 80 || log_count % 240 == 0)
            dbgln("MUNDO_WEBGL_FRAMEBUFFER_TEXTURE2D count={} framebuffer={} target={} attachment={} textarget={} texture={} level={} reason=track_color_attachment_texture_for_vulkan_replay",
                log_count,
                framebuffer_handle,
                target,
                attachment,
                textarget,
                texture_handle,
                level);
    }
}

void WebGLRenderingContextImpl::front_face(WebIDL::UnsignedLong mode)
{
    m_context->make_current();
    glFrontFace(mode);
}

void WebGLRenderingContextImpl::generate_mipmap(WebIDL::UnsignedLong target)
{
    m_context->make_current();
    glGenerateMipmap(target);
}

GC::Root<WebGLActiveInfo> WebGLRenderingContextImpl::get_active_attrib(GC::Root<WebGLProgram> program, WebIDL::UnsignedLong index)
{
    m_context->make_current();

    GLuint program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return {};
        }
        program_handle = handle_or_error.release_value();
    }

    GLint size = 0;
    GLenum type = 0;
    GLsizei buf_size = 256;
    GLsizei length = 0;
    GLchar name[256];
    glGetActiveAttrib(program_handle, index, buf_size, &length, &size, &type, name);
    auto readonly_bytes = ReadonlyBytes { name, static_cast<size_t>(length) };
    return WebGLActiveInfo::create(realm(), String::from_utf8_without_validation(readonly_bytes), type, size);
}

GC::Root<WebGLActiveInfo> WebGLRenderingContextImpl::get_active_uniform(GC::Root<WebGLProgram> program, WebIDL::UnsignedLong index)
{
    m_context->make_current();

    GLuint program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return {};
        }
        program_handle = handle_or_error.release_value();
    }

    GLint size = 0;
    GLenum type = 0;
    GLsizei buf_size = 256;
    GLsizei length = 0;
    GLchar name[256];
    glGetActiveUniform(program_handle, index, buf_size, &length, &size, &type, name);
    auto readonly_bytes = ReadonlyBytes { name, static_cast<size_t>(length) };
    return WebGLActiveInfo::create(realm(), String::from_utf8_without_validation(readonly_bytes), type, size);
}

Optional<Vector<GC::Root<WebGLShader>>> WebGLRenderingContextImpl::get_attached_shaders(GC::Root<WebGLProgram> program)
{
    m_context->make_current();

    GLuint program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return OptionalNone {};
        }
        program_handle = handle_or_error.release_value();
    }

    (void)program_handle;

    Vector<GC::Root<WebGLShader>> result;

    if (program->attached_vertex_shader())
        result.append(GC::make_root(*program->attached_vertex_shader()));

    if (program->attached_fragment_shader())
        result.append(GC::make_root(*program->attached_fragment_shader()));

    return result;
}

WebIDL::Long WebGLRenderingContextImpl::get_attrib_location(GC::Root<WebGLProgram> program, String name)
{
    m_context->make_current();

    auto program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return -1;
        }
        program_handle = handle_or_error.release_value();
    }

    auto name_null_terminated = null_terminated_string(name);
    return glGetAttribLocation(program_handle, name_null_terminated.data());
}

JS::Value WebGLRenderingContextImpl::get_buffer_parameter(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname)
{
    m_context->make_current();
    switch (pname) {
    case GL_BUFFER_SIZE: {
        GLint result { 0 };
        glGetBufferParameterivRobustANGLE(target, GL_BUFFER_SIZE, 1, nullptr, &result);
        return JS::Value(result);
    }

    case GL_BUFFER_USAGE: {
        GLint result { 0 };
        glGetBufferParameterivRobustANGLE(target, GL_BUFFER_USAGE, 1, nullptr, &result);
        return JS::Value(result);
    }

    default:
        dbgln("Unknown WebGL buffer parameter name: {:x}", pname);
        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
}

WebIDL::ExceptionOr<JS::Value> WebGLRenderingContextImpl::get_parameter(WebIDL::UnsignedLong pname)
{
    m_context->make_current();
    switch (pname) {
    case GL_ACTIVE_TEXTURE: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_ACTIVE_TEXTURE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_ALIASED_LINE_WIDTH_RANGE: {
        Array<GLfloat, 2> result;
        result.fill(0);
        constexpr size_t buffer_size = 2 * sizeof(GLfloat);
        glGetFloatvRobustANGLE(GL_ALIASED_LINE_WIDTH_RANGE, 2, nullptr, result.data());
        auto byte_buffer = MUST(ByteBuffer::copy(result.data(), buffer_size));
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(byte_buffer));
        return JS::Float32Array::create(realm(), 2, array_buffer);
    }
    case GL_ALIASED_POINT_SIZE_RANGE: {
        Array<GLfloat, 2> result;
        result.fill(0);
        constexpr size_t buffer_size = 2 * sizeof(GLfloat);
        glGetFloatvRobustANGLE(GL_ALIASED_POINT_SIZE_RANGE, 2, nullptr, result.data());
        auto byte_buffer = MUST(ByteBuffer::copy(result.data(), buffer_size));
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(byte_buffer));
        return JS::Float32Array::create(realm(), 2, array_buffer);
    }
    case GL_ALPHA_BITS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_ALPHA_BITS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_ARRAY_BUFFER_BINDING: {
        if (!m_array_buffer_binding)
            return JS::js_null();
        return JS::Value(m_array_buffer_binding);
    }
    case GL_BLEND: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_BLEND, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_BLEND_COLOR: {
        Array<GLfloat, 4> result;
        result.fill(0);
        constexpr size_t buffer_size = 4 * sizeof(GLfloat);
        glGetFloatvRobustANGLE(GL_BLEND_COLOR, 4, nullptr, result.data());
        auto byte_buffer = MUST(ByteBuffer::copy(result.data(), buffer_size));
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(byte_buffer));
        return JS::Float32Array::create(realm(), 4, array_buffer);
    }
    case GL_BLEND_DST_ALPHA: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_BLEND_DST_ALPHA, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_BLEND_DST_RGB: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_BLEND_DST_RGB, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_BLEND_EQUATION_ALPHA: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_BLEND_EQUATION_ALPHA, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_BLEND_EQUATION_RGB: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_BLEND_EQUATION_RGB, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_BLEND_SRC_ALPHA: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_BLEND_SRC_ALPHA, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_BLEND_SRC_RGB: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_BLEND_SRC_RGB, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_BLUE_BITS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_BLUE_BITS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_COLOR_CLEAR_VALUE: {
        Array<GLfloat, 4> result;
        result.fill(0);
        constexpr size_t buffer_size = 4 * sizeof(GLfloat);
        glGetFloatvRobustANGLE(GL_COLOR_CLEAR_VALUE, 4, nullptr, result.data());
        auto byte_buffer = MUST(ByteBuffer::copy(result.data(), buffer_size));
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(byte_buffer));
        return JS::Float32Array::create(realm(), 4, array_buffer);
    }
    case GL_COLOR_WRITEMASK: {
        Array<GLboolean, 4> result;
        result.fill(0);
        glGetBooleanvRobustANGLE(GL_COLOR_WRITEMASK, 4, nullptr, result.data());

        auto sequence = TRY(JS::Array::create(realm(), 4));
        for (int i = 0; i < 4; i++) {
            TRY(sequence->create_data_property(JS::PropertyKey(i), JS::Value(static_cast<WebIDL::Boolean>(result[i]))));
        }

        return JS::Value(sequence);
    }
    case GL_CULL_FACE: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_CULL_FACE, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_CULL_FACE_MODE: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_CULL_FACE_MODE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_CURRENT_PROGRAM: {
        if (!m_current_program)
            return JS::js_null();
        return JS::Value(m_current_program);
    }
    case GL_DEPTH_BITS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_DEPTH_BITS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_DEPTH_CLEAR_VALUE: {
        GLfloat result { 0.0f };
        glGetFloatvRobustANGLE(GL_DEPTH_CLEAR_VALUE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_DEPTH_FUNC: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_DEPTH_FUNC, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_DEPTH_RANGE: {
        Array<GLfloat, 2> result;
        result.fill(0);
        constexpr size_t buffer_size = 2 * sizeof(GLfloat);
        glGetFloatvRobustANGLE(GL_DEPTH_RANGE, 2, nullptr, result.data());
        auto byte_buffer = MUST(ByteBuffer::copy(result.data(), buffer_size));
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(byte_buffer));
        return JS::Float32Array::create(realm(), 2, array_buffer);
    }
    case GL_DEPTH_TEST: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_DEPTH_TEST, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_DEPTH_WRITEMASK: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_DEPTH_WRITEMASK, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_DITHER: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_DITHER, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_ELEMENT_ARRAY_BUFFER_BINDING: {
        if (!m_element_array_buffer_binding)
            return JS::js_null();
        return JS::Value(m_element_array_buffer_binding);
    }
    case GL_FRAMEBUFFER_BINDING: {
        if (!m_framebuffer_binding)
            return JS::js_null();
        return JS::Value(m_framebuffer_binding);
    }
    case GL_FRONT_FACE: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_FRONT_FACE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_GENERATE_MIPMAP_HINT: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_GENERATE_MIPMAP_HINT, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_GREEN_BITS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_GREEN_BITS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_IMPLEMENTATION_COLOR_READ_FORMAT: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_IMPLEMENTATION_COLOR_READ_FORMAT, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_IMPLEMENTATION_COLOR_READ_TYPE: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_IMPLEMENTATION_COLOR_READ_TYPE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_LINE_WIDTH: {
        GLfloat result { 0.0f };
        glGetFloatvRobustANGLE(GL_LINE_WIDTH, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_MAX_CUBE_MAP_TEXTURE_SIZE: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_MAX_CUBE_MAP_TEXTURE_SIZE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_MAX_FRAGMENT_UNIFORM_VECTORS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_MAX_FRAGMENT_UNIFORM_VECTORS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_MAX_RENDERBUFFER_SIZE: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_MAX_RENDERBUFFER_SIZE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_MAX_TEXTURE_IMAGE_UNITS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_MAX_TEXTURE_IMAGE_UNITS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_MAX_TEXTURE_SIZE: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_MAX_TEXTURE_SIZE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_MAX_VARYING_VECTORS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_MAX_VARYING_VECTORS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_MAX_VERTEX_ATTRIBS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_MAX_VERTEX_ATTRIBS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_MAX_VERTEX_UNIFORM_VECTORS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_MAX_VERTEX_UNIFORM_VECTORS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_MAX_VIEWPORT_DIMS: {
        Array<GLint, 2> result;
        result.fill(0);
        constexpr size_t buffer_size = 2 * sizeof(GLint);
        glGetIntegervRobustANGLE(GL_MAX_VIEWPORT_DIMS, 2, nullptr, result.data());
        auto byte_buffer = MUST(ByteBuffer::copy(result.data(), buffer_size));
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(byte_buffer));
        return JS::Int32Array::create(realm(), 2, array_buffer);
    }
    case GL_PACK_ALIGNMENT: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_PACK_ALIGNMENT, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_POLYGON_OFFSET_FACTOR: {
        GLfloat result { 0.0f };
        glGetFloatvRobustANGLE(GL_POLYGON_OFFSET_FACTOR, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_POLYGON_OFFSET_FILL: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_POLYGON_OFFSET_FILL, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_POLYGON_OFFSET_UNITS: {
        GLfloat result { 0.0f };
        glGetFloatvRobustANGLE(GL_POLYGON_OFFSET_UNITS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_RED_BITS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_RED_BITS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_RENDERBUFFER_BINDING: {
        if (!m_renderbuffer_binding)
            return JS::js_null();
        return JS::Value(m_renderbuffer_binding);
    }
    case GL_RENDERER: {
        auto result = reinterpret_cast<char const*>(glGetString(GL_RENDERER));
        return JS::PrimitiveString::create(realm().vm(), ByteString { result });
    }
    case GL_SAMPLE_ALPHA_TO_COVERAGE: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_SAMPLE_ALPHA_TO_COVERAGE, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_SAMPLE_BUFFERS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_SAMPLE_BUFFERS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_SAMPLE_COVERAGE: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_SAMPLE_COVERAGE, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_SAMPLE_COVERAGE_INVERT: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_SAMPLE_COVERAGE_INVERT, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_SAMPLE_COVERAGE_VALUE: {
        GLfloat result { 0.0f };
        glGetFloatvRobustANGLE(GL_SAMPLE_COVERAGE_VALUE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_SAMPLES: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_SAMPLES, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_SCISSOR_BOX: {
        Array<GLint, 4> result;
        result.fill(0);
        constexpr size_t buffer_size = 4 * sizeof(GLint);
        glGetIntegervRobustANGLE(GL_SCISSOR_BOX, 4, nullptr, result.data());
        auto byte_buffer = MUST(ByteBuffer::copy(result.data(), buffer_size));
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(byte_buffer));
        return JS::Int32Array::create(realm(), 4, array_buffer);
    }
    case GL_SCISSOR_TEST: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_SCISSOR_TEST, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_SHADING_LANGUAGE_VERSION: {
        auto result = reinterpret_cast<char const*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
        return JS::PrimitiveString::create(realm().vm(), ByteString { result });
    }
    case GL_STENCIL_BACK_FAIL: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_BACK_FAIL, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_BACK_FUNC: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_BACK_FUNC, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_BACK_PASS_DEPTH_FAIL: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_BACK_PASS_DEPTH_FAIL, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_BACK_PASS_DEPTH_PASS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_BACK_PASS_DEPTH_PASS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_BACK_REF: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_BACK_REF, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_BACK_VALUE_MASK: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_BACK_VALUE_MASK, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_BACK_WRITEMASK: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_BACK_WRITEMASK, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_BITS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_BITS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_CLEAR_VALUE: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_CLEAR_VALUE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_FAIL: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_FAIL, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_FUNC: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_FUNC, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_PASS_DEPTH_FAIL: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_PASS_DEPTH_FAIL, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_PASS_DEPTH_PASS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_PASS_DEPTH_PASS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_REF: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_REF, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_TEST: {
        GLboolean result { GL_FALSE };
        glGetBooleanvRobustANGLE(GL_STENCIL_TEST, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_STENCIL_VALUE_MASK: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_VALUE_MASK, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_STENCIL_WRITEMASK: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_STENCIL_WRITEMASK, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_SUBPIXEL_BITS: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_SUBPIXEL_BITS, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_TEXTURE_BINDING_2D: {
        if (!m_texture_binding_2d)
            return JS::js_null();
        return JS::Value(m_texture_binding_2d);
    }
    case GL_TEXTURE_BINDING_CUBE_MAP: {
        if (!m_texture_binding_cube_map)
            return JS::js_null();
        return JS::Value(m_texture_binding_cube_map);
    }
    case GL_UNPACK_ALIGNMENT: {
        GLint result { 0 };
        glGetIntegervRobustANGLE(GL_UNPACK_ALIGNMENT, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_VENDOR: {
        auto result = reinterpret_cast<char const*>(glGetString(GL_VENDOR));
        return JS::PrimitiveString::create(realm().vm(), ByteString { result });
    }
    case GL_VERSION: {
        auto result = reinterpret_cast<char const*>(glGetString(GL_VERSION));
        return JS::PrimitiveString::create(realm().vm(), ByteString { result });
    }
    case GL_VIEWPORT: {
        Array<GLint, 4> result;
        result.fill(0);
        constexpr size_t buffer_size = 4 * sizeof(GLint);
        glGetIntegervRobustANGLE(GL_VIEWPORT, 4, nullptr, result.data());
        auto byte_buffer = MUST(ByteBuffer::copy(result.data(), buffer_size));
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(byte_buffer));
        return JS::Int32Array::create(realm(), 4, array_buffer);
    }

    case UNMASKED_VENDOR_WEBGL: {
        if (!extension_enabled("WEBGL_debug_renderer_info"sv)) {
            set_error(GL_INVALID_ENUM);
            return JS::js_null();
        }
        auto result = reinterpret_cast<char const*>(glGetString(GL_VENDOR));
        return JS::PrimitiveString::create(realm().vm(), ByteString { result });
    }
    case UNMASKED_RENDERER_WEBGL: {
        if (!extension_enabled("WEBGL_debug_renderer_info"sv)) {
            set_error(GL_INVALID_ENUM);
            return JS::js_null();
        }
        auto result = reinterpret_cast<char const*>(glGetString(GL_RENDERER));
        return JS::PrimitiveString::create(realm().vm(), ByteString { result });
    }

    case GL_FRAGMENT_SHADER_DERIVATIVE_HINT: { // NOTE: This has the same value as GL_FRAGMENT_SHADER_DERIVATIVE_HINT_OES
        if (extension_enabled("OES_standard_derivatives"sv) || m_context->webgl_version() == OpenGLContext::WebGLVersion::WebGL2) {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_FRAGMENT_SHADER_DERIVATIVE_HINT, 1, nullptr, &result);
            return JS::Value(result);
        }

        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
    case GL_MAX_COLOR_ATTACHMENTS: { // NOTE: This has the same value as MAX_COLOR_ATTACHMENTS_WEBGL
        if (extension_enabled("WEBGL_draw_buffers"sv) || m_context->webgl_version() == OpenGLContext::WebGLVersion::WebGL2) {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_COLOR_ATTACHMENTS, 1, nullptr, &result);
            return JS::Value(result);
        }

        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
    case GL_MAX_DRAW_BUFFERS: {
        if (m_context->webgl_version() == OpenGLContext::WebGLVersion::WebGL2) { // FIXME: Allow this code path for MAX_DRAW_BUFFERS_WEBGL
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_DRAW_BUFFERS, 1, nullptr, &result);
            return JS::Value(result);
        }

        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
    case GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT: {
        if (extension_enabled("EXT_texture_filter_anisotropic"sv)) {
            GLfloat result { 0.0f };
            glGetFloatvRobustANGLE(GL_MAX_TEXTURE_MAX_ANISOTROPY_EXT, 1, nullptr, &result);
            return JS::Value(result);
        }

        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }

    case COMPRESSED_TEXTURE_FORMATS: {
        auto formats = enabled_compressed_texture_formats();
        auto byte_buffer = MUST(ByteBuffer::copy(formats.data(), formats.reinterpret<u8 const>().size()));
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(byte_buffer));
        return JS::Uint32Array::create(realm(), formats.size(), array_buffer);
    }
    case UNPACK_FLIP_Y_WEBGL:
        return JS::Value(m_unpack_flip_y);
    case UNPACK_PREMULTIPLY_ALPHA_WEBGL:
        return JS::Value(m_unpack_premultiply_alpha);
    case UNPACK_COLORSPACE_CONVERSION_WEBGL:
        return JS::Value(m_unpack_colorspace_conversion);
    }

    if (m_context->webgl_version() == OpenGLContext::WebGLVersion::WebGL2) {
        switch (pname) {
        case GL_COPY_READ_BUFFER_BINDING: {
            if (!m_copy_read_buffer_binding)
                return JS::js_null();
            return JS::Value(m_copy_read_buffer_binding);
        }
        case GL_COPY_WRITE_BUFFER_BINDING: {
            if (!m_copy_write_buffer_binding)
                return JS::js_null();
            return JS::Value(m_copy_write_buffer_binding);
        }
        case GL_MAX_SAMPLES: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_SAMPLES, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_3D_TEXTURE_SIZE: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_3D_TEXTURE_SIZE, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_ARRAY_TEXTURE_LAYERS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_ARRAY_TEXTURE_LAYERS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_VERTEX_UNIFORM_COMPONENTS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_VERTEX_UNIFORM_COMPONENTS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_UNIFORM_BLOCK_SIZE: {
            GLint64 result { 0 };
            glGetInteger64vRobustANGLE(GL_MAX_UNIFORM_BLOCK_SIZE, 1, nullptr, &result);
            return JS::Value(static_cast<double>(result));
        }
        case GL_MAX_UNIFORM_BUFFER_BINDINGS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_UNIFORM_BUFFER_BINDINGS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_VERTEX_UNIFORM_BLOCKS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_VERTEX_UNIFORM_BLOCKS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_FRAGMENT_INPUT_COMPONENTS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_FRAGMENT_INPUT_COMPONENTS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_FRAGMENT_UNIFORM_COMPONENTS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_FRAGMENT_UNIFORM_COMPONENTS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_COMBINED_UNIFORM_BLOCKS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_COMBINED_UNIFORM_BLOCKS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS: {
            GLint64 result { 0 };
            glGetInteger64vRobustANGLE(GL_MAX_COMBINED_VERTEX_UNIFORM_COMPONENTS, 1, nullptr, &result);
            return JS::Value(static_cast<double>(result));
        }
        case GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS: {
            GLint64 result { 0 };
            glGetInteger64vRobustANGLE(GL_MAX_COMBINED_FRAGMENT_UNIFORM_COMPONENTS, 1, nullptr, &result);
            return JS::Value(static_cast<double>(result));
        }
        case GL_MAX_ELEMENT_INDEX: {
            GLint64 result { 0 };
            glGetInteger64vRobustANGLE(GL_MAX_ELEMENT_INDEX, 1, nullptr, &result);
            return JS::Value(static_cast<double>(result));
        }
        case GL_MAX_FRAGMENT_UNIFORM_BLOCKS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_FRAGMENT_UNIFORM_BLOCKS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_VARYING_COMPONENTS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_VARYING_COMPONENTS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_ELEMENTS_INDICES: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_ELEMENTS_INDICES, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_ELEMENTS_VERTICES: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_ELEMENTS_VERTICES, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_TEXTURE_LOD_BIAS: {
            GLfloat result { 0.0f };
            glGetFloatvRobustANGLE(GL_MAX_TEXTURE_LOD_BIAS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_TRANSFORM_FEEDBACK_INTERLEAVED_COMPONENTS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_ATTRIBS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_TRANSFORM_FEEDBACK_SEPARATE_COMPONENTS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MIN_PROGRAM_TEXEL_OFFSET: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MIN_PROGRAM_TEXEL_OFFSET, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_PROGRAM_TEXEL_OFFSET: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_PROGRAM_TEXEL_OFFSET, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_VERTEX_OUTPUT_COMPONENTS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_MAX_VERTEX_OUTPUT_COMPONENTS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_MAX_SERVER_WAIT_TIMEOUT: {
            GLint64 result { 0 };
            glGetInteger64vRobustANGLE(GL_MAX_SERVER_WAIT_TIMEOUT, 1, nullptr, &result);
            return JS::Value(static_cast<double>(result));
        }
        case GL_PACK_ROW_LENGTH: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_PACK_ROW_LENGTH, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_PACK_SKIP_ROWS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_PACK_SKIP_ROWS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_PACK_SKIP_PIXELS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_PACK_SKIP_PIXELS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_PIXEL_PACK_BUFFER_BINDING: {
            if (!m_pixel_pack_buffer_binding)
                return JS::js_null();
            return JS::Value(m_pixel_pack_buffer_binding);
        }
        case GL_PIXEL_UNPACK_BUFFER_BINDING: {
            if (!m_pixel_unpack_buffer_binding)
                return JS::js_null();
            return JS::Value(m_pixel_unpack_buffer_binding);
        }
        case GL_TEXTURE_BINDING_2D_ARRAY: {
            if (!m_texture_binding_2d_array)
                return JS::js_null();
            return JS::Value(m_texture_binding_2d_array);
        }
        case GL_TRANSFORM_FEEDBACK_ACTIVE: {
            GLboolean result { GL_FALSE };
            glGetBooleanvRobustANGLE(GL_TRANSFORM_FEEDBACK_ACTIVE, 1, nullptr, &result);
            return JS::Value(result == GL_TRUE);
        }
        case GL_TRANSFORM_FEEDBACK_BINDING: {
            if (!m_transform_feedback_binding)
                return JS::js_null();
            return JS::Value(m_transform_feedback_binding);
        }
        case GL_TRANSFORM_FEEDBACK_BUFFER_BINDING: {
            if (!m_transform_feedback_buffer_binding)
                return JS::js_null();
            return JS::Value(m_transform_feedback_buffer_binding);
        }
        case GL_TRANSFORM_FEEDBACK_PAUSED: {
            GLboolean result { GL_FALSE };
            glGetBooleanvRobustANGLE(GL_TRANSFORM_FEEDBACK_PAUSED, 1, nullptr, &result);
            return JS::Value(result == GL_TRUE);
        }
        case GL_RASTERIZER_DISCARD: {
            GLboolean result { GL_FALSE };
            glGetBooleanvRobustANGLE(GL_RASTERIZER_DISCARD, 1, nullptr, &result);
            return JS::Value(result == GL_TRUE);
        }
        case GL_SAMPLER_BINDING: {
            GLint handle { 0 };
            glGetIntegervRobustANGLE(GL_SAMPLER_BINDING, 1, nullptr, &handle);
            return WebGLSampler::create(realm(), *this, handle);
        }
        case GL_UNIFORM_BUFFER_BINDING: {
            if (!m_uniform_buffer_binding)
                return JS::js_null();
            return JS::Value(m_uniform_buffer_binding);
        }
        case GL_UNPACK_IMAGE_HEIGHT: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_UNPACK_IMAGE_HEIGHT, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_UNPACK_ROW_LENGTH: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_UNPACK_ROW_LENGTH, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_UNPACK_SKIP_IMAGES: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_UNPACK_SKIP_IMAGES, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_UNPACK_SKIP_PIXELS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_UNPACK_SKIP_PIXELS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_UNPACK_SKIP_ROWS: {
            GLint result { 0 };
            glGetIntegervRobustANGLE(GL_UNPACK_SKIP_ROWS, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_VERTEX_ARRAY_BINDING: { // FIXME: Allow this for VERTEX_ARRAY_BINDING_OES
            if (!m_current_vertex_array)
                return JS::js_null();
            return JS::Value(m_current_vertex_array);
        }
        case MAX_CLIENT_WAIT_TIMEOUT_WEBGL:
            // FIXME: Make this an actual limit
            return JS::js_infinity();
        }
    }

    dbgln("Unknown WebGL parameter name: {:x}", pname);
    set_error(GL_INVALID_ENUM);
    return JS::js_null();
}

WebIDL::UnsignedLong WebGLRenderingContextImpl::get_error()
{
    m_context->make_current();
    return get_error_value();
}

JS::Value WebGLRenderingContextImpl::get_program_parameter(GC::Root<WebGLProgram> program, WebIDL::UnsignedLong pname)
{
    m_context->make_current();

    GLuint program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return JS::js_null();
        }
        program_handle = handle_or_error.release_value();
    }

    GLint result = 0;
    glGetProgramivRobustANGLE(program_handle, pname, 1, nullptr, &result);
    switch (pname) {
    case GL_ATTACHED_SHADERS:
    case GL_ACTIVE_ATTRIBUTES:
    case GL_ACTIVE_UNIFORMS:
        return JS::Value(result);

    case GL_TRANSFORM_FEEDBACK_BUFFER_MODE:
    case GL_TRANSFORM_FEEDBACK_VARYINGS:
    case GL_ACTIVE_UNIFORM_BLOCKS:
        if (m_context->webgl_version() == OpenGLContext::WebGLVersion::WebGL2)
            return JS::Value(result);

        set_error(GL_INVALID_ENUM);
        return JS::js_null();

    case GL_DELETE_STATUS:
    case GL_LINK_STATUS:
    case GL_VALIDATE_STATUS:
        return JS::Value(result == GL_TRUE);
    default:
        dbgln("Unknown WebGL program parameter name: 0x{:04x}", pname);
        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
}

Optional<String> WebGLRenderingContextImpl::get_program_info_log(GC::Root<WebGLProgram> program)
{
    m_context->make_current();

    GLuint program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return {};
        }
        program_handle = handle_or_error.release_value();
    }

    GLint info_log_length = 0;
    glGetProgramiv(program_handle, GL_INFO_LOG_LENGTH, &info_log_length);
    Vector<GLchar> info_log;
    info_log.resize(info_log_length);
    if (!info_log_length)
        return String {};
    glGetProgramInfoLog(program_handle, info_log_length, nullptr, info_log.data());
    return String::from_utf8_without_validation(ReadonlyBytes { info_log.data(), static_cast<size_t>(info_log_length - 1) });
}

JS::Value WebGLRenderingContextImpl::get_renderbuffer_parameter(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname)
{
    m_context->make_current();

    switch (pname) {
    case GL_RENDERBUFFER_WIDTH:
    case GL_RENDERBUFFER_HEIGHT:
    case GL_RENDERBUFFER_INTERNAL_FORMAT:
    case GL_RENDERBUFFER_RED_SIZE:
    case GL_RENDERBUFFER_GREEN_SIZE:
    case GL_RENDERBUFFER_BLUE_SIZE:
    case GL_RENDERBUFFER_ALPHA_SIZE:
    case GL_RENDERBUFFER_DEPTH_SIZE:
    case GL_RENDERBUFFER_SAMPLES:
    case GL_RENDERBUFFER_STENCIL_SIZE: {
        GLint result = 0;
        glGetRenderbufferParameterivRobustANGLE(target, pname, 1, nullptr, &result);
        return JS::Value(result);
    }
    default:
        // If pname is not in the table above, generates an INVALID_ENUM error.
        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
}

JS::Value WebGLRenderingContextImpl::get_shader_parameter(GC::Root<WebGLShader> shader, WebIDL::UnsignedLong pname)
{
    m_context->make_current();

    GLuint shader_handle = 0;
    if (shader) {
        auto handle_or_error = shader->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return JS::js_null();
        }
        shader_handle = handle_or_error.release_value();
    }

    GLint result = 0;
    glGetShaderivRobustANGLE(shader_handle, pname, 1, nullptr, &result);
    switch (pname) {
    case GL_SHADER_TYPE:
        return JS::Value(result);
    case GL_DELETE_STATUS:
    case GL_COMPILE_STATUS:
        return JS::Value(result == GL_TRUE);
    default:
        dbgln("Unknown WebGL shader parameter name: 0x{:04x}", pname);
        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
}

GC::Root<WebGLShaderPrecisionFormat> WebGLRenderingContextImpl::get_shader_precision_format(WebIDL::UnsignedLong shadertype, WebIDL::UnsignedLong precisiontype)
{
    m_context->make_current();

    GLint range[2];
    GLint precision;
    glGetShaderPrecisionFormat(shadertype, precisiontype, range, &precision);
    return WebGLShaderPrecisionFormat::create(realm(), range[0], range[1], precision);
}

Optional<String> WebGLRenderingContextImpl::get_shader_info_log(GC::Root<WebGLShader> shader)
{
    m_context->make_current();

    GLuint shader_handle = 0;
    if (shader) {
        auto handle_or_error = shader->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return {};
        }
        shader_handle = handle_or_error.release_value();
    }

    GLint info_log_length = 0;
    glGetShaderiv(shader_handle, GL_INFO_LOG_LENGTH, &info_log_length);
    Vector<GLchar> info_log;
    info_log.resize(info_log_length);
    if (!info_log_length)
        return String {};
    glGetShaderInfoLog(shader_handle, info_log_length, nullptr, info_log.data());
    return String::from_utf8_without_validation(ReadonlyBytes { info_log.data(), static_cast<size_t>(info_log_length - 1) });
}

Optional<String> WebGLRenderingContextImpl::get_shader_source(GC::Root<WebGLShader> shader)
{
    m_context->make_current();

    GLuint shader_handle = 0;
    if (shader) {
        auto handle_or_error = shader->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return {};
        }
        shader_handle = handle_or_error.release_value();
    }

    GLint shader_source_length = 0;
    glGetShaderiv(shader_handle, GL_SHADER_SOURCE_LENGTH, &shader_source_length);
    if (!shader_source_length)
        return String {};

    auto shader_source = MUST(ByteBuffer::create_uninitialized(shader_source_length));
    glGetShaderSource(shader_handle, shader_source_length, nullptr, reinterpret_cast<GLchar*>(shader_source.data()));
    return String::from_utf8_without_validation(ReadonlyBytes { shader_source.data(), static_cast<size_t>(shader_source_length - 1) });
}

JS::Value WebGLRenderingContextImpl::get_tex_parameter(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname)
{
    m_context->make_current();

    switch (pname) {
    case GL_TEXTURE_MAG_FILTER:
    case GL_TEXTURE_MIN_FILTER:
    case GL_TEXTURE_WRAP_S:
    case GL_TEXTURE_WRAP_T: {
        GLint result { 0 };
        glGetTexParameterivRobustANGLE(target, pname, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_TEXTURE_MAX_ANISOTROPY_EXT: {
        if (extension_enabled("EXT_texture_filter_anisotropic"sv)) {
            GLint result { 0 };
            glGetTexParameterivRobustANGLE(target, GL_TEXTURE_MAX_ANISOTROPY_EXT, 1, nullptr, &result);
            return JS::Value(result);
        }

        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
    }

    if (m_context->webgl_version() == OpenGLContext::WebGLVersion::WebGL2) {
        switch (pname) {
        case GL_TEXTURE_BASE_LEVEL:
        case GL_TEXTURE_COMPARE_FUNC:
        case GL_TEXTURE_COMPARE_MODE:
        case GL_TEXTURE_IMMUTABLE_LEVELS:
        case GL_TEXTURE_MAX_LEVEL:
        case GL_TEXTURE_WRAP_R: {
            GLint result { 0 };
            glGetTexParameterivRobustANGLE(target, pname, 1, nullptr, &result);
            return JS::Value(result);
        }
        case GL_TEXTURE_IMMUTABLE_FORMAT: {
            GLint result { 0 };
            glGetTexParameterivRobustANGLE(target, GL_TEXTURE_IMMUTABLE_FORMAT, 1, nullptr, &result);
            return JS::Value(result == GL_TRUE);
        }
        case GL_TEXTURE_MAX_LOD:
        case GL_TEXTURE_MIN_LOD: {
            GLfloat result { 0.0f };
            glGetTexParameterfvRobustANGLE(target, GL_TEXTURE_IMMUTABLE_FORMAT, 1, nullptr, &result);
            return JS::Value(result == GL_TRUE);
        }
        }
    }

    set_error(GL_INVALID_ENUM);
    return JS::js_null();
}

JS::Value WebGLRenderingContextImpl::get_uniform(GC::Root<WebGLProgram>, GC::Root<WebGLUniformLocation>)
{
    dbgln("FIXME: Implement get_uniform");
    return JS::Value(0);
}

GC::Root<WebGLUniformLocation> WebGLRenderingContextImpl::get_uniform_location(GC::Root<WebGLProgram> program, String name)
{
    m_context->make_current();

    GLuint program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return {};
        }
        program_handle = handle_or_error.release_value();
    }

    auto name_null_terminated = null_terminated_string(name);

    // "This function returns -1 if name does not correspond to an active uniform variable in program or if name starts
    //  with the reserved prefix "gl_"."
    // WebGL Spec: The return value is null if name does not correspond to an active uniform variable in the passed program.
    auto location = glGetUniformLocation(program_handle, name_null_terminated.data());
    if (location == -1)
        return nullptr;

    return WebGLUniformLocation::create(realm(), location, program.ptr());
}

JS::Value WebGLRenderingContextImpl::get_vertex_attrib(WebIDL::UnsignedLong index, WebIDL::UnsignedLong pname)
{
    switch (pname) {
    case GL_CURRENT_VERTEX_ATTRIB: {
        Array<GLfloat, 4> result;
        result.fill(0);
        glGetVertexAttribfvRobustANGLE(index, GL_CURRENT_VERTEX_ATTRIB, result.size(), nullptr, result.data());

        auto byte_buffer = MUST(ByteBuffer::copy(result.span().reinterpret<u8>()));
        auto array_buffer = JS::ArrayBuffer::create(realm(), move(byte_buffer));
        return JS::Float32Array::create(realm(), result.size(), array_buffer);
    }
    case GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING: {
        GLint handle { 0 };
        glGetVertexAttribivRobustANGLE(index, GL_VERTEX_ATTRIB_ARRAY_BUFFER_BINDING, 1, nullptr, &handle);
        return WebGLBuffer::create(realm(), *this, handle);
    }
    case GL_VERTEX_ATTRIB_ARRAY_DIVISOR: { // NOTE: This has the same value as GL_VERTEX_ATTRIB_ARRAY_DIVISOR_ANGLE
        if (extension_enabled("ANGLE_instanced_arrays"sv) || m_context->webgl_version() == OpenGLContext::WebGLVersion::WebGL2) {
            GLint result { 0 };
            glGetVertexAttribivRobustANGLE(index, GL_VERTEX_ATTRIB_ARRAY_DIVISOR, 1, nullptr, &result);
            return JS::Value(result);
        }

        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
    case GL_VERTEX_ATTRIB_ARRAY_ENABLED: {
        GLint result { 0 };
        glGetVertexAttribivRobustANGLE(index, GL_VERTEX_ATTRIB_ARRAY_ENABLED, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_VERTEX_ATTRIB_ARRAY_INTEGER: {
        if (m_context->webgl_version() == OpenGLContext::WebGLVersion::WebGL2) {
            GLint result { 0 };
            glGetVertexAttribivRobustANGLE(index, GL_VERTEX_ATTRIB_ARRAY_INTEGER, 1, nullptr, &result);
            return JS::Value(result == GL_TRUE);
        }

        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
    case GL_VERTEX_ATTRIB_ARRAY_NORMALIZED: {
        GLint result { 0 };
        glGetVertexAttribivRobustANGLE(index, GL_VERTEX_ATTRIB_ARRAY_NORMALIZED, 1, nullptr, &result);
        return JS::Value(result == GL_TRUE);
    }
    case GL_VERTEX_ATTRIB_ARRAY_SIZE: {
        GLint result { 0 };
        glGetVertexAttribivRobustANGLE(index, GL_VERTEX_ATTRIB_ARRAY_SIZE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_VERTEX_ATTRIB_ARRAY_STRIDE: {
        GLint result { 0 };
        glGetVertexAttribivRobustANGLE(index, GL_VERTEX_ATTRIB_ARRAY_STRIDE, 1, nullptr, &result);
        return JS::Value(result);
    }
    case GL_VERTEX_ATTRIB_ARRAY_TYPE: {
        GLint result { 0 };
        glGetVertexAttribivRobustANGLE(index, GL_VERTEX_ATTRIB_ARRAY_TYPE, 1, nullptr, &result);
        return JS::Value(result);
    }
    default:
        dbgln("Unknown WebGL vertex attrib name: 0x{:04x}", pname);
        set_error(GL_INVALID_ENUM);
        return JS::js_null();
    }
}

WebIDL::LongLong WebGLRenderingContextImpl::get_vertex_attrib_offset(WebIDL::UnsignedLong index, WebIDL::UnsignedLong pname)
{
    if (pname != GL_VERTEX_ATTRIB_ARRAY_POINTER) {
        set_error(GL_INVALID_ENUM);
        return 0;
    }

    GLintptr result { 0 };
    glGetVertexAttribPointervRobustANGLE(index, GL_VERTEX_ATTRIB_ARRAY_POINTER, 1, nullptr, reinterpret_cast<void**>(&result));
    return result;
}

void WebGLRenderingContextImpl::hint(WebIDL::UnsignedLong target, WebIDL::UnsignedLong mode)
{
    m_context->make_current();
    glHint(target, mode);
}

bool WebGLRenderingContextImpl::is_buffer(GC::Root<WebGLBuffer> buffer)
{
    m_context->make_current();

    auto buffer_handle = 0;
    if (buffer) {
        auto handle_or_error = buffer->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return false;
        }
        buffer_handle = handle_or_error.release_value();
    }
    return glIsBuffer(buffer_handle);
}

bool WebGLRenderingContextImpl::is_enabled(WebIDL::UnsignedLong cap)
{
    m_context->make_current();
    return glIsEnabled(cap);
}

bool WebGLRenderingContextImpl::is_framebuffer(GC::Root<WebGLFramebuffer> framebuffer)
{
    m_context->make_current();

    auto framebuffer_handle = 0;
    if (framebuffer) {
        auto handle_or_error = framebuffer->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return false;
        }
        framebuffer_handle = handle_or_error.release_value();
    }
    return glIsFramebuffer(framebuffer_handle);
}

bool WebGLRenderingContextImpl::is_program(GC::Root<WebGLProgram> program)
{
    m_context->make_current();

    auto program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return false;
        }
        program_handle = handle_or_error.release_value();
    }
    return glIsProgram(program_handle);
}

bool WebGLRenderingContextImpl::is_renderbuffer(GC::Root<WebGLRenderbuffer> renderbuffer)
{
    m_context->make_current();

    auto renderbuffer_handle = 0;
    if (renderbuffer) {
        auto handle_or_error = renderbuffer->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return false;
        }
        renderbuffer_handle = handle_or_error.release_value();
    }
    return glIsRenderbuffer(renderbuffer_handle);
}

bool WebGLRenderingContextImpl::is_shader(GC::Root<WebGLShader> shader)
{
    m_context->make_current();

    auto shader_handle = 0;
    if (shader) {
        auto handle_or_error = shader->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return false;
        }
        shader_handle = handle_or_error.release_value();
    }
    return glIsShader(shader_handle);
}

bool WebGLRenderingContextImpl::is_texture(GC::Root<WebGLTexture> texture)
{
    m_context->make_current();

    auto texture_handle = 0;
    if (texture) {
        auto handle_or_error = texture->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return false;
        }
        texture_handle = handle_or_error.release_value();
    }
    return glIsTexture(texture_handle);
}

void WebGLRenderingContextImpl::line_width(float width)
{
    m_context->make_current();
    glLineWidth(width);
}

void WebGLRenderingContextImpl::link_program(GC::Root<WebGLProgram> program)
{
    m_context->make_current();

    auto program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        program_handle = handle_or_error.release_value();
    }
    glLinkProgram(program_handle);

    if (!program)
        return;
    program->clear_video_sampler_plan();

    auto fragment_shader = program->attached_fragment_shader();
    if (!fragment_shader)
        return;

    auto fragment_source = get_shader_source(fragment_shader);
    if (!fragment_source.has_value())
        return;

    auto fragment_source_view = fragment_source->bytes_as_string_view();
    auto has_sampler_2d = fragment_source_view.contains("sampler2D"sv);
    auto has_texture_2d = fragment_source_view.contains("texture2D"sv) || fragment_source_view.contains("texture("sv);
    auto has_external_sampler = fragment_source_view.contains("samplerExternalOES"sv);
    if (has_sampler_2d || has_external_sampler) {
        dbgln("MUNDO_WEBGL_VIDEO_PROGRAM_LINK program={} fragment_shader_video_candidate=true sampler2D={} samplerExternalOES={} texture_call={} source_bytes={} next_step=match_with_TEXTURE_BACKING_DRAW_program",
            program_handle,
            has_sampler_2d,
            has_external_sampler,
            has_texture_2d,
            fragment_source->bytes().size());
    }

    GLint active_uniform_count = 0;
    glGetProgramivRobustANGLE(program_handle, GL_ACTIVE_UNIFORMS, 1, nullptr, &active_uniform_count);
    auto uniforms_to_scan = active_uniform_count < 32 ? active_uniform_count : 32;
    Optional<WebGLProgram::VideoSamplerPlan> first_video_sampler_plan;
    for (GLint index = 0; index < uniforms_to_scan; ++index) {
        GLint size = 0;
        GLenum type = 0;
        GLsizei length = 0;
        GLchar name[256];
        glGetActiveUniform(program_handle, static_cast<GLuint>(index), sizeof(name), &length, &size, &type, name);
        if (!length || !mundo_webgl_is_sampler_uniform_type(type))
            continue;

        auto uniform_name = String::from_utf8_without_validation(ReadonlyBytes { name, static_cast<size_t>(length) });
        auto texture2d_pattern = MUST(String::formatted("texture2D({}", uniform_name));
        auto texture_pattern = MUST(String::formatted("texture({}", uniform_name));
        auto direct_texture_call = fragment_source_view.contains(texture2d_pattern.bytes_as_string_view()) || fragment_source_view.contains(texture_pattern.bytes_as_string_view());
        if (!first_video_sampler_plan.has_value() && direct_texture_call) {
            first_video_sampler_plan = WebGLProgram::VideoSamplerPlan {
                .uniform_name = uniform_name,
                .uniform_type = type,
                .direct_texture_call = direct_texture_call,
                .fragment_mentions_sampler_2d = has_sampler_2d,
                .fragment_mentions_external_sampler = has_external_sampler,
            };
        }
        dbgln("MUNDO_WEBGL_VIDEO_PROGRAM_SAMPLER_SOURCE program={} uniform={} type={} size={} direct_texture_call={} texture2D_pattern={} texture_pattern={} active_uniforms={} next_step=shader_rewrite_feasibility",
            program_handle,
            uniform_name,
            type,
            size,
            direct_texture_call,
            texture2d_pattern,
            texture_pattern,
            active_uniform_count);
    }
    if (first_video_sampler_plan.has_value()) {
        auto const& plan = first_video_sampler_plan.value();
        dbgln("MUNDO_WEBGL_VIDEO_PROGRAM_SAMPLER_PLAN program={} uniform={} type={} direct_texture_call={} sampler2D={} samplerExternalOES={} route_candidate=vulkan_direct_sampling_virtualization next_step=bind_or_virtualize_video_sampler",
            program_handle,
            plan.uniform_name,
            plan.uniform_type,
            plan.direct_texture_call,
            plan.fragment_mentions_sampler_2d,
            plan.fragment_mentions_external_sampler);
        program->set_video_sampler_plan(first_video_sampler_plan.release_value());
    }
}

void WebGLRenderingContextImpl::pixel_storei(WebIDL::UnsignedLong pname, WebIDL::Long param)
{
    m_context->make_current();

    switch (pname) {
    case UNPACK_FLIP_Y_WEBGL:
        m_unpack_flip_y = param != GL_FALSE;
        return;
    case UNPACK_PREMULTIPLY_ALPHA_WEBGL:
        m_unpack_premultiply_alpha = param != GL_FALSE;
        return;
    case UNPACK_COLORSPACE_CONVERSION_WEBGL:
        m_unpack_colorspace_conversion = param;
        return;
    }

    glPixelStorei(pname, param);
}

void WebGLRenderingContextImpl::polygon_offset(float factor, float units)
{
    m_context->make_current();
    glPolygonOffset(factor, units);
}

void WebGLRenderingContextImpl::renderbuffer_storage(WebIDL::UnsignedLong target, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height)
{
    m_context->make_current();

    if (internalformat == GL_DEPTH_STENCIL)
        internalformat = GL_DEPTH24_STENCIL8;

    glRenderbufferStorage(target, internalformat, width, height);
}

void WebGLRenderingContextImpl::sample_coverage(float value, bool invert)
{
    m_context->make_current();
    glSampleCoverage(value, invert);
}

void WebGLRenderingContextImpl::scissor(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height)
{
    m_context->make_current();
    glScissor(x, y, width, height);
}

void WebGLRenderingContextImpl::shader_source(GC::Root<WebGLShader> shader, String source)
{
    m_context->make_current();

    GLuint shader_handle = 0;
    if (shader) {
        auto handle_or_error = shader->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        shader_handle = handle_or_error.release_value();
    }

    Vector<GLchar*> strings;
    auto string = null_terminated_string(source);
    strings.append(string.data());
    Vector<GLint> length;
    length.append(source.bytes().size());
    glShaderSource(shader_handle, 1, strings.data(), length.data());
}

void WebGLRenderingContextImpl::stencil_func(WebIDL::UnsignedLong func, WebIDL::Long ref, WebIDL::UnsignedLong mask)
{
    m_context->make_current();
    glStencilFunc(func, ref, mask);
}

void WebGLRenderingContextImpl::stencil_func_separate(WebIDL::UnsignedLong face, WebIDL::UnsignedLong func, WebIDL::Long ref, WebIDL::UnsignedLong mask)
{
    m_context->make_current();
    glStencilFuncSeparate(face, func, ref, mask);
}

void WebGLRenderingContextImpl::stencil_mask(WebIDL::UnsignedLong mask)
{
    m_context->make_current();
    glStencilMask(mask);
}

void WebGLRenderingContextImpl::stencil_mask_separate(WebIDL::UnsignedLong face, WebIDL::UnsignedLong mask)
{
    m_context->make_current();
    glStencilMaskSeparate(face, mask);
}

void WebGLRenderingContextImpl::stencil_op(WebIDL::UnsignedLong fail, WebIDL::UnsignedLong zfail, WebIDL::UnsignedLong zpass)
{
    m_context->make_current();
    glStencilOp(fail, zfail, zpass);
}

void WebGLRenderingContextImpl::stencil_op_separate(WebIDL::UnsignedLong face, WebIDL::UnsignedLong fail, WebIDL::UnsignedLong zfail, WebIDL::UnsignedLong zpass)
{
    m_context->make_current();
    glStencilOpSeparate(face, fail, zfail, zpass);
}

void WebGLRenderingContextImpl::tex_parameterf(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname, float param)
{
    m_context->make_current();
    glTexParameterf(target, pname, param);
}

void WebGLRenderingContextImpl::tex_parameteri(WebIDL::UnsignedLong target, WebIDL::UnsignedLong pname, WebIDL::Long param)
{
    m_context->make_current();
    glTexParameteri(target, pname, param);
}

void WebGLRenderingContextImpl::uniform1f(GC::Root<WebGLUniformLocation> location, float x)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    glUniform1f(location_handle, x);
}

void WebGLRenderingContextImpl::uniform2f(GC::Root<WebGLUniformLocation> location, float x, float y)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    glUniform2f(location_handle, x, y);
}

void WebGLRenderingContextImpl::uniform3f(GC::Root<WebGLUniformLocation> location, float x, float y, float z)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    glUniform3f(location_handle, x, y, z);
}

void WebGLRenderingContextImpl::uniform4f(GC::Root<WebGLUniformLocation> location, float x, float y, float z, float w)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    glUniform4f(location_handle, x, y, z, w);
}

void WebGLRenderingContextImpl::uniform1i(GC::Root<WebGLUniformLocation> location, WebIDL::Long x)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    glUniform1i(location_handle, x);
}

void WebGLRenderingContextImpl::uniform2i(GC::Root<WebGLUniformLocation> location, WebIDL::Long x, WebIDL::Long y)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    glUniform2i(location_handle, x, y);
}

void WebGLRenderingContextImpl::uniform3i(GC::Root<WebGLUniformLocation> location, WebIDL::Long x, WebIDL::Long y, WebIDL::Long z)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    glUniform3i(location_handle, x, y, z);
}

void WebGLRenderingContextImpl::uniform4i(GC::Root<WebGLUniformLocation> location, WebIDL::Long x, WebIDL::Long y, WebIDL::Long z, WebIDL::Long w)
{
    m_context->make_current();

    GLuint location_handle = 0;
    if (location)
        location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    glUniform4i(location_handle, x, y, z, w);
}

void WebGLRenderingContextImpl::use_program(GC::Root<WebGLProgram> program)
{
    m_context->make_current();

    GLuint program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        program_handle = handle_or_error.release_value();
    }

    glUseProgram(program_handle);
    m_current_program = program;
}

void WebGLRenderingContextImpl::validate_program(GC::Root<WebGLProgram> program)
{
    m_context->make_current();

    auto program_handle = 0;
    if (program) {
        auto handle_or_error = program->handle(this);
        if (handle_or_error.is_error()) {
            set_error(GL_INVALID_OPERATION);
            return;
        }
        program_handle = handle_or_error.release_value();
    }
    glValidateProgram(program_handle);
}

void WebGLRenderingContextImpl::vertex_attrib1f(WebIDL::UnsignedLong index, float x)
{
    m_context->make_current();
    glVertexAttrib1f(index, x);
}

void WebGLRenderingContextImpl::vertex_attrib2f(WebIDL::UnsignedLong index, float x, float y)
{
    m_context->make_current();
    glVertexAttrib2f(index, x, y);
}

void WebGLRenderingContextImpl::vertex_attrib3f(WebIDL::UnsignedLong index, float x, float y, float z)
{
    m_context->make_current();
    glVertexAttrib3f(index, x, y, z);
}

void WebGLRenderingContextImpl::vertex_attrib4f(WebIDL::UnsignedLong index, float x, float y, float z, float w)
{
    m_context->make_current();
    glVertexAttrib4f(index, x, y, z, w);
}

void WebGLRenderingContextImpl::vertex_attrib1fv(WebIDL::UnsignedLong index, Float32List values)
{
    m_context->make_current();

    auto span = MUST(span_from_float32_list(values, /* src_offset= */ 0));
    if (span.size() < 1) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glVertexAttrib1fv(index, span.data());
}

void WebGLRenderingContextImpl::vertex_attrib2fv(WebIDL::UnsignedLong index, Float32List values)
{
    m_context->make_current();

    auto span = MUST(span_from_float32_list(values, /* src_offset= */ 0));
    if (span.size() < 2) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glVertexAttrib2fv(index, span.data());
}

void WebGLRenderingContextImpl::vertex_attrib3fv(WebIDL::UnsignedLong index, Float32List values)
{
    m_context->make_current();

    auto span = MUST(span_from_float32_list(values, /* src_offset= */ 0));
    if (span.size() < 3) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glVertexAttrib3fv(index, span.data());
}

void WebGLRenderingContextImpl::vertex_attrib4fv(WebIDL::UnsignedLong index, Float32List values)
{
    m_context->make_current();

    auto span = MUST(span_from_float32_list(values, /* src_offset= */ 0));
    if (span.size() < 4) {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glVertexAttrib4fv(index, span.data());
}

void WebGLRenderingContextImpl::vertex_attrib_pointer(WebIDL::UnsignedLong index, WebIDL::Long size, WebIDL::UnsignedLong type, bool normalized, WebIDL::Long stride, WebIDL::LongLong offset)
{
    m_context->make_current();

    // If no WebGLBuffer is bound to the ARRAY_BUFFER target and offset is non-zero, an INVALID_OPERATION error will be generated.
    if (!m_array_buffer_binding && offset != 0) {
        set_error(GL_INVALID_OPERATION);
        return;
    }

    glVertexAttribPointer(index, size, type, normalized, stride, reinterpret_cast<void*>(offset));
}

void WebGLRenderingContextImpl::viewport(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height)
{
    m_context->make_current();
    glViewport(x, y, width, height);
}

void WebGLRenderingContextImpl::visit_edges(JS::Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);

    visitor.visit(m_array_buffer_binding);
    visitor.visit(m_element_array_buffer_binding);
    visitor.visit(m_current_program);
    visitor.visit(m_framebuffer_binding);
    visitor.visit(m_renderbuffer_binding);
    visitor.visit(m_texture_binding_2d);
    visitor.visit(m_texture_binding_cube_map);

    visitor.visit(m_uniform_buffer_binding);
    visitor.visit(m_copy_read_buffer_binding);
    visitor.visit(m_copy_write_buffer_binding);
    visitor.visit(m_transform_feedback_buffer_binding);
    visitor.visit(m_texture_binding_2d_array);
    visitor.visit(m_texture_binding_3d);
    visitor.visit(m_transform_feedback_binding);
    visitor.visit(m_pixel_pack_buffer_binding);
    visitor.visit(m_pixel_unpack_buffer_binding);
    visitor.visit(m_current_vertex_array);
    visitor.visit(m_any_samples_passed);
    visitor.visit(m_any_samples_passed_conservative);
    visitor.visit(m_transform_feedback_primitives_written);
    for (auto& texture_binding : m_mundo_texture_binding_2d_by_unit)
        visitor.visit(texture_binding);
    for (auto& buffer : m_mundo_buffer_by_handle)
        visitor.visit(buffer);
}

}
