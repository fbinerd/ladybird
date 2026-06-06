/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <GLES2/gl2.h>
#include <LibJS/Runtime/Realm.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/WebGLBuffer.h>
#include <LibWeb/WebGL/WebGLBuffer.h>
#include <string.h>

namespace Web::WebGL {

GC_DEFINE_ALLOCATOR(WebGLBuffer);

static constexpr size_t max_mundo_webgl_buffer_shadow_bytes = 64 * 1024 * 1024;

GC::Ref<WebGLBuffer> WebGLBuffer::create(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
{
    return realm.create<WebGLBuffer>(realm, context, handle);
}

WebGLBuffer::WebGLBuffer(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context, GLuint handle)
    : WebGLObject(realm, context, handle)
{
}

WebGLBuffer::~WebGLBuffer() = default;

void WebGLBuffer::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(WebGLBuffer);
    Base::initialize(realm);
}

bool WebGLBuffer::is_compatible_with(GLenum target)
{
    // https://registry.khronos.org/webgl/specs/latest/2.0/#5.1
    if (!m_target.has_value()) {
        m_target = target;
        return true;
    }

    if (target == GL_ELEMENT_ARRAY_BUFFER)
        return m_target.value() == GL_ELEMENT_ARRAY_BUFFER;

    return m_target.value() != GL_ELEMENT_ARRAY_BUFFER;
}

void WebGLBuffer::set_shadow_data(GLenum target, size_t byte_length, ReadonlyBytes data)
{
    m_target = target;
    m_shadow_byte_length = byte_length;
    m_has_complete_shadow_data = data.size() == byte_length && byte_length <= max_mundo_webgl_buffer_shadow_bytes;
    if (!m_has_complete_shadow_data) {
        m_shadow_data = {};
        return;
    }
    m_shadow_data = MUST(ByteBuffer::copy(data));
}

void WebGLBuffer::set_shadow_size(GLenum target, size_t byte_length)
{
    m_target = target;
    m_shadow_byte_length = byte_length;
    m_has_complete_shadow_data = false;
    m_shadow_data = {};
}

void WebGLBuffer::update_shadow_data(size_t offset, ReadonlyBytes data)
{
    if (!m_has_complete_shadow_data)
        return;
    if (offset > m_shadow_data.size() || data.size() > m_shadow_data.size() - offset) {
        m_has_complete_shadow_data = false;
        m_shadow_data = {};
        return;
    }
    memcpy(m_shadow_data.data() + offset, data.data(), data.size());
}

}
