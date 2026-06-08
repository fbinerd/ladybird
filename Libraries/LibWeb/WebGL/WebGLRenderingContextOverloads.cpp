/*
 * Copyright (c) 2024-2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#define GL_GLEXT_PROTOTYPES 1
#include <GLES2/gl2.h>
#include <GLES2/gl2ext.h>
extern "C" {
#include <GLES2/gl2ext_angle.h>
}

#include <AK/Time.h>
#include <LibJS/Runtime/ArrayBuffer.h>
#include <LibJS/Runtime/DataView.h>
#include <LibJS/Runtime/TypedArray.h>
#include <LibWeb/WebGL/OpenGLContext.h>
#include <LibWeb/WebGL/WebGLBuffer.h>
#include <LibWeb/WebGL/WebGLRenderingContextOverloads.h>
#include <LibWeb/WebGL/WebGLTexture.h>
#include <LibWeb/WebGL/WebGLUniformLocation.h>
#include <stdlib.h>
#include <string.h>

namespace Web::WebGL {

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

static bool mundo_webgl_video_cpu_bitmap_fallback_enabled()
{
    auto const* raw_value = getenv("MUNDO_WEBGL_VIDEO_CPU_BITMAP_FALLBACK");
    if (raw_value)
        return raw_value[0] != '\0' && strcmp(raw_value, "0") && strcmp(raw_value, "false") && strcmp(raw_value, "no") && strcmp(raw_value, "off");

    return false;
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

struct MundoWebGLUploadTimingSummary {
    size_t buffer_data_count { 0 };
    i64 buffer_data_us { 0 };
    size_t buffer_sub_data_count { 0 };
    i64 buffer_sub_data_us { 0 };
    size_t tex_image_count { 0 };
    i64 tex_image_us { 0 };
    size_t tex_sub_image_count { 0 };
    i64 tex_sub_image_us { 0 };
    size_t compressed_tex_count { 0 };
    i64 compressed_tex_us { 0 };
    size_t read_pixels_count { 0 };
    i64 read_pixels_us { 0 };
    MonotonicTime started_at { MonotonicTime::now() };
};

static void record_mundo_webgl_timing_summary(char const* operation, i64 duration_us)
{
    if (!mundo_webgl_timing_enabled())
        return;

    static MundoWebGLUploadTimingSummary s_summary;

    if (!strncmp(operation, "bufferData", 10)) {
        ++s_summary.buffer_data_count;
        s_summary.buffer_data_us += duration_us;
    } else if (!strcmp(operation, "bufferSubData")) {
        ++s_summary.buffer_sub_data_count;
        s_summary.buffer_sub_data_us += duration_us;
    } else if (!strncmp(operation, "texImage2D", 10)) {
        ++s_summary.tex_image_count;
        s_summary.tex_image_us += duration_us;
    } else if (!strncmp(operation, "texSubImage2D", 13)) {
        ++s_summary.tex_sub_image_count;
        s_summary.tex_sub_image_us += duration_us;
    } else if (!strncmp(operation, "compressedTex", 13)) {
        ++s_summary.compressed_tex_count;
        s_summary.compressed_tex_us += duration_us;
    } else if (!strcmp(operation, "readPixels")) {
        ++s_summary.read_pixels_count;
        s_summary.read_pixels_us += duration_us;
    }

    auto now = MonotonicTime::now();
    auto elapsed = (now - s_summary.started_at).to_milliseconds();
    if (elapsed < mundo_webgl_timing_summary_interval_ms())
        return;

    auto total_count = s_summary.buffer_data_count + s_summary.buffer_sub_data_count + s_summary.tex_image_count + s_summary.tex_sub_image_count + s_summary.compressed_tex_count + s_summary.read_pixels_count;
    auto total_us = s_summary.buffer_data_us + s_summary.buffer_sub_data_us + s_summary.tex_image_us + s_summary.tex_sub_image_us + s_summary.compressed_tex_us + s_summary.read_pixels_us;
    if (total_count) {
        dbgln("MUNDO_WEBGL_UPLOAD_SUMMARY elapsed={}ms total_count={} total_us={} avg_us={} buffer_data={}/{}us buffer_sub_data={}/{}us tex_image={}/{}us tex_sub_image={}/{}us compressed_tex={}/{}us read_pixels={}/{}us",
            elapsed,
            total_count,
            total_us,
            total_us / static_cast<i64>(total_count),
            s_summary.buffer_data_count,
            s_summary.buffer_data_us,
            s_summary.buffer_sub_data_count,
            s_summary.buffer_sub_data_us,
            s_summary.tex_image_count,
            s_summary.tex_image_us,
            s_summary.tex_sub_image_count,
            s_summary.tex_sub_image_us,
            s_summary.compressed_tex_count,
            s_summary.compressed_tex_us,
            s_summary.read_pixels_count,
            s_summary.read_pixels_us);
    }

    s_summary = {};
    s_summary.started_at = now;
}

static size_t mundo_webgl_next_timing_count()
{
    static size_t s_count { 0 };
    return ++s_count;
}

WebGLRenderingContextOverloads::WebGLRenderingContextOverloads(JS::Realm& realm, NonnullOwnPtr<OpenGLContext> context)
    : WebGLRenderingContextImpl(realm, move(context))
{
}

void WebGLRenderingContextOverloads::buffer_data(WebIDL::UnsignedLong target, WebIDL::LongLong size, WebIDL::UnsignedLong usage)
{
    m_context->make_current();

    auto start = MonotonicTime::now();
    glBufferData(target, size, 0, usage);
    if (size >= 0) {
        if (auto buffer = current_bound_buffer_for_target(target))
            buffer->set_shadow_size(target, static_cast<size_t>(size));
    }
    record_mundo_webgl_timing_summary("bufferData(size)", (MonotonicTime::now() - start).to_microseconds());
    if (auto duration = mundo_webgl_slow_duration(start); duration.has_value())
        dbgln("MUNDO_WEBGL_TIMING count={} op=bufferData(size) duration={}ms threshold={}ms target={} size={} usage={}", mundo_webgl_next_timing_count(), duration.value(), mundo_webgl_timing_threshold_ms(), target, size, usage);
}

void WebGLRenderingContextOverloads::buffer_data(WebIDL::UnsignedLong target, Optional<GC::Root<WebIDL::BufferSource>> data, WebIDL::UnsignedLong usage)
{
    m_context->make_current();

    // https://registry.khronos.org/webgl/specs/latest/1.0/#5.14.5
    // If the passed data is null then an INVALID_VALUE error is generated.
    if (!data.has_value()) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    auto span = MUST(get_offset_span<u8 const>(*data.value(), /* src_offset= */ 0));
    auto start = MonotonicTime::now();
    glBufferData(target, static_cast<GLsizeiptr>(span.size()), span.data(), usage);
    if (auto buffer = current_bound_buffer_for_target(target))
        buffer->set_shadow_data(target, span.size(), span);
    record_mundo_webgl_timing_summary("bufferData(data)", (MonotonicTime::now() - start).to_microseconds());
    if (auto duration = mundo_webgl_slow_duration(start); duration.has_value())
        dbgln("MUNDO_WEBGL_TIMING count={} op=bufferData(data) duration={}ms threshold={}ms target={} bytes={} usage={}", mundo_webgl_next_timing_count(), duration.value(), mundo_webgl_timing_threshold_ms(), target, span.size(), usage);
}

void WebGLRenderingContextOverloads::buffer_sub_data(WebIDL::UnsignedLong target, WebIDL::LongLong offset, GC::Root<WebIDL::BufferSource> data)
{
    m_context->make_current();

    auto span = MUST(get_offset_span<u8 const>(*data, /* src_offset= */ 0));
    auto start = MonotonicTime::now();
    glBufferSubData(target, offset, span.size(), span.data());
    if (offset >= 0) {
        if (auto buffer = current_bound_buffer_for_target(target))
            buffer->update_shadow_data(static_cast<size_t>(offset), span);
    }
    record_mundo_webgl_timing_summary("bufferSubData", (MonotonicTime::now() - start).to_microseconds());
    if (auto duration = mundo_webgl_slow_duration(start); duration.has_value())
        dbgln("MUNDO_WEBGL_TIMING count={} op=bufferSubData duration={}ms threshold={}ms target={} offset={} bytes={}", mundo_webgl_next_timing_count(), duration.value(), mundo_webgl_timing_threshold_ms(), target, offset, span.size());
}

void WebGLRenderingContextOverloads::compressed_tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::UnsignedLong internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, GC::Root<WebIDL::ArrayBufferView> data)
{
    m_context->make_current();

    if (!enabled_compressed_texture_formats().contains_slow(internalformat)) {
        set_error(GL_INVALID_ENUM);
        return;
    }

    auto span = MUST(get_offset_span<u8 const>(*data, /* src_offset= */ 0));
    auto start = MonotonicTime::now();
    glCompressedTexImage2DRobustANGLE(target, level, internalformat, width, height, border, span.size(), span.size(), span.data());
    record_mundo_webgl_timing_summary("compressedTexImage2D", (MonotonicTime::now() - start).to_microseconds());
    if (auto duration = mundo_webgl_slow_duration(start); duration.has_value())
        dbgln("MUNDO_WEBGL_TIMING count={} op=compressedTexImage2D duration={}ms threshold={}ms target={} level={} internalformat={} size={}x{} bytes={}", mundo_webgl_next_timing_count(), duration.value(), mundo_webgl_timing_threshold_ms(), target, level, internalformat, width, height, span.size());
}

void WebGLRenderingContextOverloads::compressed_tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, GC::Root<WebIDL::ArrayBufferView> data)
{
    m_context->make_current();

    if (!enabled_compressed_texture_formats().contains_slow(format)) {
        set_error(GL_INVALID_ENUM);
        return;
    }

    auto span = MUST(get_offset_span<u8 const>(*data, /* src_offset= */ 0));
    auto start = MonotonicTime::now();
    glCompressedTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, width, height, format, span.size(), span.size(), span.data());
    record_mundo_webgl_timing_summary("compressedTexSubImage2D", (MonotonicTime::now() - start).to_microseconds());
    if (auto duration = mundo_webgl_slow_duration(start); duration.has_value())
        dbgln("MUNDO_WEBGL_TIMING count={} op=compressedTexSubImage2D duration={}ms threshold={}ms target={} level={} offset={}x{} size={}x{} format={} bytes={}", mundo_webgl_next_timing_count(), duration.value(), mundo_webgl_timing_threshold_ms(), target, level, xoffset, yoffset, width, height, format, span.size());
}

void WebGLRenderingContextOverloads::read_pixels(WebIDL::Long x, WebIDL::Long y, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> pixels)
{
    m_context->make_current();

    if (!pixels) {
        set_error(GL_INVALID_VALUE);
        return;
    }

    auto span = MUST(get_offset_span<u8>(*pixels, /* src_offset= */ 0));
    auto start = MonotonicTime::now();
    glReadPixelsRobustANGLE(x, y, width, height, format, type, span.size(), nullptr, nullptr, nullptr, span.data());
    record_mundo_webgl_timing_summary("readPixels", (MonotonicTime::now() - start).to_microseconds());
    if (auto duration = mundo_webgl_slow_duration(start); duration.has_value())
        dbgln("MUNDO_WEBGL_TIMING count={} op=readPixels duration={}ms threshold={}ms rect={}x{}+{}+{} format={} type={} bytes={}", mundo_webgl_next_timing_count(), duration.value(), mundo_webgl_timing_threshold_ms(), width, height, x, y, format, type, span.size());
}

void WebGLRenderingContextOverloads::tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::Long width, WebIDL::Long height, WebIDL::Long border, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> pixels)
{
    m_context->make_current();

    if (pixels) {
        auto span = MUST(get_offset_span<u8>(*pixels, /* src_offset= */ 0));
        if (auto texture = current_bound_texture_for_target(target)) {
            texture->clear_hardware_video_backing();
            texture->set_mundo_texture_upload_snapshot(width, height, internalformat, format, type, span);
        }
        auto start = MonotonicTime::now();
        glTexImage2DRobustANGLE(target, level, internalformat, width, height, border, format, type, span.size(), span.data());
        record_mundo_webgl_timing_summary("texImage2D(pixels)", (MonotonicTime::now() - start).to_microseconds());
        if (auto duration = mundo_webgl_slow_duration(start); duration.has_value())
            dbgln("MUNDO_WEBGL_TIMING count={} op=texImage2D(pixels) duration={}ms threshold={}ms target={} level={} size={}x{} format={} type={} bytes={}", mundo_webgl_next_timing_count(), duration.value(), mundo_webgl_timing_threshold_ms(), target, level, width, height, format, type, span.size());
        return;
    }

    Checked<size_t> bytes = 0;
    if (type == GL_UNSIGNED_SHORT_5_6_5 && format != GL_RGB) {
        set_error(GL_INVALID_OPERATION);
        return;
    }

    if ((type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1) && format != GL_RGBA) {
        set_error(GL_INVALID_OPERATION);
        return;
    }

    switch (format) {
    case GL_ALPHA:
    case GL_LUMINANCE:
    case GL_LUMINANCE_ALPHA: {
        if (type != GL_UNSIGNED_BYTE) {
            set_error(GL_INVALID_ENUM);
            return;
        }

        bytes = format == GL_LUMINANCE_ALPHA ? 2 : 1;
        break;
    }
    case GL_RGB:
    case GL_RGBA: {
        switch (type) {
        case GL_UNSIGNED_BYTE:
            bytes = format == GL_RGB ? 3 : 4;
            break;
        case GL_UNSIGNED_SHORT_4_4_4_4:
        case GL_UNSIGNED_SHORT_5_5_5_1:
        case GL_UNSIGNED_SHORT_5_6_5:
            bytes = 2;
            break;
        default:
            set_error(GL_INVALID_ENUM);
            return;
        }

        break;
    }
    default:
        set_error(GL_INVALID_ENUM);
        return;
    }

    bytes *= width;
    bytes *= height;

    if (bytes.has_overflow()) {
        set_error(GL_INVALID_OPERATION);
        return;
    }

    auto byte_buffer = MUST(ByteBuffer::create_zeroed(bytes.value_unchecked()));
    if (auto texture = current_bound_texture_for_target(target)) {
        texture->clear_hardware_video_backing();
        texture->set_mundo_texture_upload_snapshot(width, height, internalformat, format, type, byte_buffer.bytes());
    }
    auto start = MonotonicTime::now();
    glTexImage2DRobustANGLE(target, level, internalformat, width, height, border, format, type, byte_buffer.size(), byte_buffer.data());
    record_mundo_webgl_timing_summary("texImage2D(null)", (MonotonicTime::now() - start).to_microseconds());
    if (auto duration = mundo_webgl_slow_duration(start); duration.has_value())
        dbgln("MUNDO_WEBGL_TIMING count={} op=texImage2D(null) duration={}ms threshold={}ms target={} level={} size={}x{} format={} type={} bytes={}", mundo_webgl_next_timing_count(), duration.value(), mundo_webgl_timing_threshold_ms(), target, level, width, height, format, type, byte_buffer.size());
}

void WebGLRenderingContextOverloads::tex_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long internalformat, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source)
{
    m_context->make_current();

    if (upload_texture_source_with_video_nv12_shader_fast_path(source, target, level, internalformat, 0, 0, 0, format, type, OptionalNone {}, OptionalNone {}, false))
        return;
    if (texture_source_is_video_with_nv12_frame(source)) {
        dbgln("MUNDO_WEBGL_VIDEO_NV12_BITMAP_FALLBACK kind=texImage2D reason=nv12_shader_rejected target={} level={} format={} type={}", target, level, format, type);
        if (!mundo_webgl_video_cpu_bitmap_fallback_enabled()) {
            dbgln("MUNDO_WEBGL_VIDEO_CPU_BITMAP_FALLBACK_BLOCKED kind=texImage2D reason=hardware_only_mode target={} level={} format={} type={}", target, level, format, type);
            return;
        }
    }
    if (upload_texture_source_with_video_bitmap_fast_path(source, target, level, internalformat, 0, 0, 0, format, type, OptionalNone {}, OptionalNone {}, false))
        return;

    auto maybe_converted_texture = read_and_pixel_convert_texture_image_source(source, format, type);
    if (!maybe_converted_texture.has_value())
        return;
    auto converted_texture = maybe_converted_texture.release_value();
    if (upload_texture_source_with_video_pbo(source, target, level, internalformat, 0, 0, 0, format, type, converted_texture, false))
        return;
    if (auto texture = current_bound_texture_for_target(target)) {
        texture->clear_hardware_video_backing();
        texture->set_mundo_texture_upload_snapshot(converted_texture.width, converted_texture.height, internalformat, format, type, converted_texture.buffer.bytes());
    }
    auto start = MonotonicTime::now();
    glTexImage2DRobustANGLE(target, level, internalformat, converted_texture.width, converted_texture.height, 0, format, type, converted_texture.buffer.size(), converted_texture.buffer.data());
    record_mundo_webgl_timing_summary("texImage2D(source)", (MonotonicTime::now() - start).to_microseconds());
    if (auto duration = mundo_webgl_slow_duration(start); duration.has_value())
        dbgln("MUNDO_WEBGL_TIMING count={} op=texImage2D(source) duration={}ms threshold={}ms target={} level={} size={}x{} format={} type={} bytes={}", mundo_webgl_next_timing_count(), duration.value(), mundo_webgl_timing_threshold_ms(), target, level, converted_texture.width, converted_texture.height, format, type, converted_texture.buffer.size());
}

void WebGLRenderingContextOverloads::tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::Long width, WebIDL::Long height, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, GC::Root<WebIDL::ArrayBufferView> pixels)
{
    m_context->make_current();

    auto span = MUST(get_offset_span<u8>(*pixels, /* src_offset= */ 0));
    if (auto texture = current_bound_texture_for_target(target)) {
        texture->clear_hardware_video_backing();
        if (!texture->update_mundo_texture_upload_snapshot_region(xoffset, yoffset, width, height, format, type, span))
            texture->mark_mundo_texture_upload_snapshot_incomplete();
    }
    auto start = MonotonicTime::now();
    glTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, width, height, format, type, span.size(), span.data());
    record_mundo_webgl_timing_summary("texSubImage2D(pixels)", (MonotonicTime::now() - start).to_microseconds());
    if (auto duration = mundo_webgl_slow_duration(start); duration.has_value())
        dbgln("MUNDO_WEBGL_TIMING count={} op=texSubImage2D(pixels) duration={}ms threshold={}ms target={} level={} offset={}x{} size={}x{} format={} type={} bytes={}", mundo_webgl_next_timing_count(), duration.value(), mundo_webgl_timing_threshold_ms(), target, level, xoffset, yoffset, width, height, format, type, span.size());
}

void WebGLRenderingContextOverloads::tex_sub_image2d(WebIDL::UnsignedLong target, WebIDL::Long level, WebIDL::Long xoffset, WebIDL::Long yoffset, WebIDL::UnsignedLong format, WebIDL::UnsignedLong type, TexImageSource source)
{
    m_context->make_current();

    if (upload_texture_source_with_video_nv12_shader_fast_path(source, target, level, 0, xoffset, yoffset, 0, format, type, OptionalNone {}, OptionalNone {}, true))
        return;
    if (texture_source_is_video_with_nv12_frame(source)) {
        dbgln("MUNDO_WEBGL_VIDEO_NV12_BITMAP_FALLBACK kind=texSubImage2D reason=nv12_shader_rejected target={} level={} offset={}x{} format={} type={}", target, level, xoffset, yoffset, format, type);
        if (!mundo_webgl_video_cpu_bitmap_fallback_enabled()) {
            dbgln("MUNDO_WEBGL_VIDEO_CPU_BITMAP_FALLBACK_BLOCKED kind=texSubImage2D reason=hardware_only_mode target={} level={} offset={}x{} format={} type={}", target, level, xoffset, yoffset, format, type);
            return;
        }
    }
    if (upload_texture_source_with_video_bitmap_fast_path(source, target, level, 0, xoffset, yoffset, 0, format, type, OptionalNone {}, OptionalNone {}, true))
        return;

    auto maybe_converted_texture = read_and_pixel_convert_texture_image_source(source, format, type);

    if (!maybe_converted_texture.has_value())
        return;
    auto converted_texture = maybe_converted_texture.release_value();
    if (upload_texture_source_with_video_pbo(source, target, level, 0, xoffset, yoffset, 0, format, type, converted_texture, true))
        return;
    if (auto texture = current_bound_texture_for_target(target)) {
        texture->clear_hardware_video_backing();
        if (!texture->update_mundo_texture_upload_snapshot_region(xoffset, yoffset, converted_texture.width, converted_texture.height, format, type, converted_texture.buffer.bytes()))
            texture->mark_mundo_texture_upload_snapshot_incomplete();
    }
    auto start = MonotonicTime::now();
    glTexSubImage2DRobustANGLE(target, level, xoffset, yoffset, converted_texture.width, converted_texture.height, format, type, converted_texture.buffer.size(), converted_texture.buffer.data());
    record_mundo_webgl_timing_summary("texSubImage2D(source)", (MonotonicTime::now() - start).to_microseconds());
    if (auto duration = mundo_webgl_slow_duration(start); duration.has_value())
        dbgln("MUNDO_WEBGL_TIMING count={} op=texSubImage2D(source) duration={}ms threshold={}ms target={} level={} offset={}x{} size={}x{} format={} type={} bytes={}", mundo_webgl_next_timing_count(), duration.value(), mundo_webgl_timing_threshold_ms(), target, level, xoffset, yoffset, converted_texture.width, converted_texture.height, format, type, converted_texture.buffer.size());
}

void WebGLRenderingContextOverloads::uniform1fv(GC::Root<WebGLUniformLocation> location, Float32List v)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = MUST(span_from_float32_list(v, /* src_offset= */ 0));
    glUniform1fv(location_handle, span.size(), span.data());
}

void WebGLRenderingContextOverloads::uniform2fv(GC::Root<WebGLUniformLocation> location, Float32List v)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = MUST(span_from_float32_list(v, /* src_offset= */ 0));
    if (span.size() % 2 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform2fv(location_handle, span.size() / 2, span.data());
}

void WebGLRenderingContextOverloads::uniform3fv(GC::Root<WebGLUniformLocation> location, Float32List v)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = MUST(span_from_float32_list(v, /* src_offset= */ 0));
    if (span.size() % 3 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform3fv(location_handle, span.size() / 3, span.data());
}

void WebGLRenderingContextOverloads::uniform4fv(GC::Root<WebGLUniformLocation> location, Float32List v)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = MUST(span_from_float32_list(v, /* src_offset= */ 0));
    if (span.size() % 4 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform4fv(location_handle, span.size() / 4, span.data());
}

void WebGLRenderingContextOverloads::uniform1iv(GC::Root<WebGLUniformLocation> location, Int32List v)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = MUST(span_from_int32_list(v, /* src_offset= */ 0));
    glUniform1iv(location_handle, span.size(), span.data());
}

void WebGLRenderingContextOverloads::uniform2iv(GC::Root<WebGLUniformLocation> location, Int32List v)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = MUST(span_from_int32_list(v, /* src_offset= */ 0));
    if (span.size() % 2 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform2iv(location_handle, span.size() / 2, span.data());
}

void WebGLRenderingContextOverloads::uniform3iv(GC::Root<WebGLUniformLocation> location, Int32List v)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = MUST(span_from_int32_list(v, /* src_offset= */ 0));
    if (span.size() % 3 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform3iv(location_handle, span.size() / 3, span.data());
}

void WebGLRenderingContextOverloads::uniform4iv(GC::Root<WebGLUniformLocation> location, Int32List v)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    auto span = MUST(span_from_int32_list(v, /* src_offset= */ 0));
    if (span.size() % 4 != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniform4iv(location_handle, span.size() / 4, span.data());
}

void WebGLRenderingContextOverloads::uniform_matrix2fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List value)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    constexpr auto matrix_size = 2 * 2;
    auto span = MUST(span_from_float32_list(value, /* src_offset= */ 0));
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix2fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

void WebGLRenderingContextOverloads::uniform_matrix3fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List value)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    constexpr auto matrix_size = 3 * 3;
    auto span = MUST(span_from_float32_list(value, /* src_offset= */ 0));
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix3fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

void WebGLRenderingContextOverloads::uniform_matrix4fv(GC::Root<WebGLUniformLocation> location, bool transpose, Float32List value)
{
    m_context->make_current();

    if (!location)
        return;

    GLuint location_handle = SET_ERROR_VALUE_IF_ERROR(location->handle(m_current_program), GL_INVALID_OPERATION);

    constexpr auto matrix_size = 4 * 4;
    auto span = MUST(span_from_float32_list(value, /* src_offset= */ 0));
    if (span.size() % matrix_size != 0) [[unlikely]] {
        set_error(GL_INVALID_VALUE);
        return;
    }
    glUniformMatrix4fv(location_handle, span.size() / matrix_size, transpose, span.data());
}

}
