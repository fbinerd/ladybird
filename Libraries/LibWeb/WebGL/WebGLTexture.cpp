/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLTexture.h>
#include <LibWeb/WebGL/WebGLTexture.h>
#include <GLES3/gl3.h>
#include <string.h>

#if defined(__linux__)
#    include <unistd.h>
#endif

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLTexture);

static constexpr size_t max_mundo_texture_upload_snapshot_bytes = 64 * 1024 * 1024;

static bool should_log_mundo_texture_snapshot_event()
{
    static size_t s_count = 0;
    ++s_count;
    return s_count <= 80 || s_count % 240 == 0;
}

static Optional<size_t> mundo_texture_bytes_per_pixel(u32 format, u32 type)
{
    if (type == GL_UNSIGNED_BYTE) {
        switch (format) {
        case GL_ALPHA:
        case GL_LUMINANCE:
        case GL_RED:
            return 1;
        case GL_LUMINANCE_ALPHA:
        case GL_RG:
            return 2;
        case GL_RGB:
            return 3;
        case GL_RGBA:
            return 4;
        default:
            return {};
        }
    }

    if (type == GL_UNSIGNED_SHORT_5_6_5 || type == GL_UNSIGNED_SHORT_4_4_4_4 || type == GL_UNSIGNED_SHORT_5_5_5_1)
        return 2;

    return {};
}

GC::Ref<WebGLTexture> WebGLTexture::create(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
{
    return realm.create<WebGLTexture>(realm, context, handle);
}

WebGLTexture::WebGLTexture(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
    : WebGLObject(realm, context, handle)
{
}

WebGLTexture::~WebGLTexture()
{
    clear_hardware_video_backing();
}

void WebGLTexture::set_hardware_video_backing(HardwareVideoBacking backing)
{
    clear_hardware_video_backing();
    m_hardware_video_backing = backing;
}

void WebGLTexture::clear_hardware_video_backing()
{
#if defined(__linux__)
    if (m_hardware_video_backing.has_value() && m_hardware_video_backing->source_opaque_fd >= 0)
        close(m_hardware_video_backing->source_opaque_fd);
#endif
    m_hardware_video_backing.clear();
#ifdef USE_VULKAN_DMABUF_IMAGES
    m_cached_virtual_vulkan_video_source = nullptr;
    m_cached_virtual_vulkan_video_source_frame_id = 0;
#endif
}

void WebGLTexture::set_mundo_texture_upload_snapshot(u32 width, u32 height, u32 internal_format, u32 format, u32 type, ReadonlyBytes pixels)
{
    MundoTextureUploadSnapshot snapshot;
    snapshot.width = width;
    snapshot.height = height;
    snapshot.internal_format = internal_format;
    snapshot.format = format;
    snapshot.type = type;
    snapshot.byte_length = pixels.size();
    snapshot.complete = pixels.size() <= max_mundo_texture_upload_snapshot_bytes;
    if (snapshot.complete)
        snapshot.pixels = MUST(ByteBuffer::copy(pixels));
    m_mundo_texture_upload_snapshot = move(snapshot);
    if (!m_mundo_texture_upload_snapshot->complete && should_log_mundo_texture_snapshot_event())
        dbgln("MUNDO_WEBGL_TEXTURE_SNAPSHOT_STATE texture={} state=incomplete reason=initial_upload_too_large size={}x{} internal_format={} format={} type={} bytes={} max_bytes={}", handle(m_context.ptr()).value_or(0), width, height, internal_format, format, type, pixels.size(), max_mundo_texture_upload_snapshot_bytes);
}

void WebGLTexture::set_mundo_texture_upload_snapshot_incomplete(u32 width, u32 height, u32 internal_format, u32 format, u32 type, size_t byte_length)
{
    MundoTextureUploadSnapshot snapshot;
    snapshot.width = width;
    snapshot.height = height;
    snapshot.internal_format = internal_format;
    snapshot.format = format;
    snapshot.type = type;
    snapshot.byte_length = byte_length;
    snapshot.complete = false;
    m_mundo_texture_upload_snapshot = move(snapshot);
    if (should_log_mundo_texture_snapshot_event())
        dbgln("MUNDO_WEBGL_TEXTURE_SNAPSHOT_STATE texture={} state=incomplete reason=explicit_incomplete_storage size={}x{} internal_format={} format={} type={} bytes={}", handle(m_context.ptr()).value_or(0), width, height, internal_format, format, type, byte_length);
}

bool WebGLTexture::update_mundo_texture_upload_snapshot_region(i32 xoffset, i32 yoffset, u32 width, u32 height, u32 format, u32 type, ReadonlyBytes pixels)
{
    if (!m_mundo_texture_upload_snapshot.has_value()) {
        if (xoffset == 0 && yoffset == 0) {
            MundoTextureUploadSnapshot snapshot;
            snapshot.width = width;
            snapshot.height = height;
            snapshot.internal_format = format;
            snapshot.format = format;
            snapshot.type = type;
            snapshot.byte_length = 0;
            snapshot.complete = false;
            m_mundo_texture_upload_snapshot = move(snapshot);
            if (should_log_mundo_texture_snapshot_event())
                dbgln("MUNDO_WEBGL_TEXTURE_SNAPSHOT_STATE texture={} state=incomplete reason=subimage_created_base_from_zero_offset size={}x{} format={} type={} bytes={}", handle(m_context.ptr()).value_or(0), width, height, format, type, pixels.size());
        } else {
            if (should_log_mundo_texture_snapshot_event())
                dbgln("MUNDO_WEBGL_TEXTURE_SNAPSHOT_STATE texture={} state=incomplete reason=subimage_without_base offset={}x{} size={}x{} format={} type={} bytes={}", handle(m_context.ptr()).value_or(0), xoffset, yoffset, width, height, format, type, pixels.size());
            return false;
        }
    }

    if (!m_mundo_texture_upload_snapshot.has_value()) {
        if (should_log_mundo_texture_snapshot_event())
            dbgln("MUNDO_WEBGL_TEXTURE_SNAPSHOT_STATE texture={} state=incomplete reason=subimage_without_base offset={}x{} size={}x{} format={} type={} bytes={}", handle(m_context.ptr()).value_or(0), xoffset, yoffset, width, height, format, type, pixels.size());
        return false;
    }

    auto& snapshot = *m_mundo_texture_upload_snapshot;
    if (xoffset < 0 || yoffset < 0) {
        if (should_log_mundo_texture_snapshot_event())
            dbgln("MUNDO_WEBGL_TEXTURE_SNAPSHOT_STATE texture={} state=incomplete reason=subimage_negative_offset offset={}x{} size={}x{} base={}x{} format={} type={} bytes={}", handle(m_context.ptr()).value_or(0), xoffset, yoffset, width, height, snapshot.width, snapshot.height, format, type, pixels.size());
        mark_mundo_texture_upload_snapshot_incomplete();
        return false;
    }

    if (format != snapshot.format || type != snapshot.type) {
        if (!snapshot.complete && snapshot.byte_length == 0 && snapshot.type == 0) {
            if (should_log_mundo_texture_snapshot_event())
                dbgln("MUNDO_WEBGL_TEXTURE_SNAPSHOT_STATE texture={} state=incomplete reason=subimage_adopted_storage_format offset={}x{} size={}x{} base={}x{} internal_format={} old_format={} old_type={} format={} type={} bytes={}", handle(m_context.ptr()).value_or(0), xoffset, yoffset, width, height, snapshot.width, snapshot.height, snapshot.internal_format, snapshot.format, snapshot.type, format, type, pixels.size());
            snapshot.format = format;
            snapshot.type = type;
        } else {
            if (should_log_mundo_texture_snapshot_event())
                dbgln("MUNDO_WEBGL_TEXTURE_SNAPSHOT_STATE texture={} state=incomplete reason=subimage_format_mismatch offset={}x{} size={}x{} base={}x{} base_format={} base_type={} format={} type={} bytes={}", handle(m_context.ptr()).value_or(0), xoffset, yoffset, width, height, snapshot.width, snapshot.height, snapshot.format, snapshot.type, format, type, pixels.size());
            mark_mundo_texture_upload_snapshot_incomplete();
            return false;
        }
    }

    auto bytes_per_pixel = mundo_texture_bytes_per_pixel(format, type);
    if (!bytes_per_pixel.has_value()) {
        if (should_log_mundo_texture_snapshot_event())
            dbgln("MUNDO_WEBGL_TEXTURE_SNAPSHOT_STATE texture={} state=incomplete reason=subimage_unsupported_format offset={}x{} size={}x{} base={}x{} format={} type={} bytes={}", handle(m_context.ptr()).value_or(0), xoffset, yoffset, width, height, snapshot.width, snapshot.height, format, type, pixels.size());
        mark_mundo_texture_upload_snapshot_incomplete();
        return false;
    }

    auto destination_x = static_cast<u32>(xoffset);
    auto destination_y = static_cast<u32>(yoffset);
    if (destination_x > snapshot.width || destination_y > snapshot.height || width > snapshot.width - destination_x || height > snapshot.height - destination_y) {
        if (should_log_mundo_texture_snapshot_event())
            dbgln("MUNDO_WEBGL_TEXTURE_SNAPSHOT_STATE texture={} state=incomplete reason=subimage_out_of_bounds offset={}x{} size={}x{} base={}x{} format={} type={} bytes={}", handle(m_context.ptr()).value_or(0), xoffset, yoffset, width, height, snapshot.width, snapshot.height, format, type, pixels.size());
        mark_mundo_texture_upload_snapshot_incomplete();
        return false;
    }

    Checked<size_t> checked_row_bytes = width;
    checked_row_bytes *= bytes_per_pixel.value();
    Checked<size_t> checked_source_bytes = checked_row_bytes;
    checked_source_bytes *= height;
    Checked<size_t> checked_destination_row_bytes = snapshot.width;
    checked_destination_row_bytes *= bytes_per_pixel.value();
    Checked<size_t> checked_destination_bytes = checked_destination_row_bytes;
    checked_destination_bytes *= snapshot.height;
    if (checked_row_bytes.has_overflow() || checked_source_bytes.has_overflow() || checked_destination_row_bytes.has_overflow() || checked_destination_bytes.has_overflow()) {
        if (should_log_mundo_texture_snapshot_event())
            dbgln("MUNDO_WEBGL_TEXTURE_SNAPSHOT_STATE texture={} state=incomplete reason=subimage_size_overflow offset={}x{} size={}x{} base={}x{} format={} type={} bytes={}", handle(m_context.ptr()).value_or(0), xoffset, yoffset, width, height, snapshot.width, snapshot.height, format, type, pixels.size());
        mark_mundo_texture_upload_snapshot_incomplete();
        return false;
    }

    auto row_bytes = checked_row_bytes.value();
    auto source_bytes = checked_source_bytes.value();
    auto destination_row_bytes = checked_destination_row_bytes.value();
    auto destination_bytes = checked_destination_bytes.value();
    if (pixels.size() < source_bytes || destination_bytes > max_mundo_texture_upload_snapshot_bytes) {
        if (should_log_mundo_texture_snapshot_event())
            dbgln("MUNDO_WEBGL_TEXTURE_SNAPSHOT_STATE texture={} state=incomplete reason={} offset={}x{} size={}x{} base={}x{} format={} type={} bytes={} required_bytes={} destination_bytes={} max_bytes={}", handle(m_context.ptr()).value_or(0), pixels.size() < source_bytes ? "subimage_too_few_bytes" : "subimage_destination_too_large", xoffset, yoffset, width, height, snapshot.width, snapshot.height, format, type, pixels.size(), source_bytes, destination_bytes, max_mundo_texture_upload_snapshot_bytes);
        mark_mundo_texture_upload_snapshot_incomplete();
        return false;
    }

    if (!snapshot.complete || snapshot.pixels.size() != destination_bytes)
        snapshot.pixels = MUST(ByteBuffer::create_zeroed(destination_bytes));

    for (u32 row = 0; row < height; ++row) {
        auto source_offset = static_cast<size_t>(row) * row_bytes;
        auto destination_offset = (static_cast<size_t>(destination_y + row) * destination_row_bytes) + (static_cast<size_t>(destination_x) * bytes_per_pixel.value());
        memcpy(snapshot.pixels.data() + destination_offset, pixels.data() + source_offset, row_bytes);
    }

    snapshot.byte_length = destination_bytes;
    snapshot.complete = true;
    if (should_log_mundo_texture_snapshot_event())
        dbgln("MUNDO_WEBGL_TEXTURE_SNAPSHOT_STATE texture={} state=complete reason=subimage_region_captured offset={}x{} size={}x{} base={}x{} format={} type={} bytes={}", handle(m_context.ptr()).value_or(0), xoffset, yoffset, width, height, snapshot.width, snapshot.height, format, type, snapshot.byte_length);
    return true;
}

void WebGLTexture::mark_mundo_texture_upload_snapshot_incomplete()
{
    if (!m_mundo_texture_upload_snapshot.has_value())
        return;
    m_mundo_texture_upload_snapshot->complete = false;
    m_mundo_texture_upload_snapshot->pixels = {};
    if (should_log_mundo_texture_snapshot_event())
        dbgln("MUNDO_WEBGL_TEXTURE_SNAPSHOT_STATE texture={} state=incomplete reason=marked_incomplete size={}x{} internal_format={} format={} type={} bytes={}", handle(m_context.ptr()).value_or(0), m_mundo_texture_upload_snapshot->width, m_mundo_texture_upload_snapshot->height, m_mundo_texture_upload_snapshot->internal_format, m_mundo_texture_upload_snapshot->format, m_mundo_texture_upload_snapshot->type, m_mundo_texture_upload_snapshot->byte_length);
}

void WebGLTexture::clear_mundo_texture_upload_snapshot()
{
    m_mundo_texture_upload_snapshot.clear();
}

void WebGLTexture::mark_mundo_render_target_written(u32 viewport_width, u32 viewport_height, u32 program)
{
    if (!m_mundo_render_target_write_state.has_value())
        m_mundo_render_target_write_state = MundoRenderTargetWriteState {};

    auto& state = *m_mundo_render_target_write_state;
    ++state.write_count;
    state.last_viewport_width = viewport_width;
    state.last_viewport_height = viewport_height;
    state.last_program = program;

    if (state.write_count <= 24 || state.write_count % 120 == 0)
        dbgln("MUNDO_WEBGL_TEXTURE_RENDER_TARGET_WRITE texture={} count={} viewport={}x{} program={} reason=framebuffer_color_attachment_draw next_step=virtualize_render_target_texture_for_pure_vulkan_present",
            handle(m_context.ptr()).value_or(0),
            state.write_count,
            viewport_width,
            viewport_height,
            program);
}

#ifdef USE_VULKAN_DMABUF_IMAGES
void WebGLTexture::set_cached_virtual_vulkan_video_source(u64 frame_id, NonnullOwnPtr<Gfx::ImportedVulkanNV12Image> source)
{
    m_cached_virtual_vulkan_video_source = move(source);
    m_cached_virtual_vulkan_video_source_frame_id = frame_id;
}
#endif

void WebGLTexture::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLTexture);
    Base::initialize(realm);
}

}
