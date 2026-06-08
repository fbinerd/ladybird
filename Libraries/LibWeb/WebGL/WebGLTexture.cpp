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

#if defined(__linux__)
#    include <unistd.h>
#endif

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLTexture);

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
    static constexpr size_t max_snapshot_bytes = 64 * 1024 * 1024;

    MundoTextureUploadSnapshot snapshot;
    snapshot.width = width;
    snapshot.height = height;
    snapshot.internal_format = internal_format;
    snapshot.format = format;
    snapshot.type = type;
    snapshot.byte_length = pixels.size();
    snapshot.complete = pixels.size() <= max_snapshot_bytes;
    if (snapshot.complete)
        snapshot.pixels = MUST(ByteBuffer::copy(pixels));
    m_mundo_texture_upload_snapshot = move(snapshot);
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
}

void WebGLTexture::mark_mundo_texture_upload_snapshot_incomplete()
{
    if (!m_mundo_texture_upload_snapshot.has_value())
        return;
    m_mundo_texture_upload_snapshot->complete = false;
    m_mundo_texture_upload_snapshot->pixels = {};
}

void WebGLTexture::clear_mundo_texture_upload_snapshot()
{
    m_mundo_texture_upload_snapshot.clear();
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
