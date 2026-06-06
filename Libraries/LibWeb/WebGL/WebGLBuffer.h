/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Undefine <undefine@undefine.pl>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <LibWeb/WebGL/Types.h>
#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLBuffer final : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLBuffer, WebGLObject);
    GC_DECLARE_ALLOCATOR(WebGLBuffer);

public:
    static GC::Ref<WebGLBuffer> create(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase> context, GLuint handle);

    virtual ~WebGLBuffer();

    bool is_compatible_with(GLenum target);
    void set_shadow_data(GLenum target, size_t byte_length, ReadonlyBytes data);
    void set_shadow_size(GLenum target, size_t byte_length);
    void update_shadow_data(size_t offset, ReadonlyBytes data);
    size_t shadow_byte_length() const { return m_shadow_byte_length; }
    bool has_complete_shadow_data() const { return m_has_complete_shadow_data; }
    ReadonlyBytes shadow_data() const { return m_shadow_data.bytes(); }
    Optional<GLenum> shadow_target() const { return m_target; }

protected:
    explicit WebGLBuffer(JS::Realm&, GC::Ref<WebGLRenderingContextBase>, GLuint handle);

    virtual void initialize(JS::Realm&) override;

private:
    Optional<GLenum> m_target;
    ByteBuffer m_shadow_data;
    size_t m_shadow_byte_length { 0 };
    bool m_has_complete_shadow_data { false };
};

}
