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
#endif
}

#ifdef USE_VULKAN_DMABUF_IMAGES
void WebGLTexture::set_cached_virtual_vulkan_video_source(NonnullOwnPtr<Gfx::ImportedVulkanNV12Image> source)
{
    m_cached_virtual_vulkan_video_source = move(source);
}
#endif

void WebGLTexture::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLTexture);
    Base::initialize(realm);
}

}
