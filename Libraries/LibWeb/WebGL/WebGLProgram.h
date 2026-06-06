/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024-2025, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <AK/String.h>
#include <LibWeb/WebGL/Types.h>
#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLProgram final : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLProgram, WebGLObject);
    GC_DECLARE_ALLOCATOR(WebGLProgram);

public:
    static GC::Ref<WebGLProgram> create(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase>, GLuint handle);

    virtual ~WebGLProgram();

    GC::Ptr<WebGLShader> attached_vertex_shader() const { return m_attached_vertex_shader; }
    void set_attached_vertex_shader(GC::Ptr<WebGLShader> shader) { m_attached_vertex_shader = shader; }

    GC::Ptr<WebGLShader> attached_fragment_shader() const { return m_attached_fragment_shader; }
    void set_attached_fragment_shader(GC::Ptr<WebGLShader> shader) { m_attached_fragment_shader = shader; }

    struct VideoSamplerPlan {
        String uniform_name;
        GLenum uniform_type { 0 };
        bool direct_texture_call { false };
        bool fragment_mentions_sampler_2d { false };
        bool fragment_mentions_external_sampler { false };
    };

    void set_video_sampler_plan(VideoSamplerPlan);
    void clear_video_sampler_plan() { m_video_sampler_plan.clear(); }
    Optional<VideoSamplerPlan> const& video_sampler_plan() const { return m_video_sampler_plan; }

protected:
    explicit WebGLProgram(JS::Realm&, GC::Ref<WebGLRenderingContextBase>, GLuint handle);

    virtual void initialize(JS::Realm&) override;
    virtual void visit_edges(JS::Cell::Visitor&) override;

private:
    GC::Ptr<WebGLShader> m_attached_vertex_shader;
    GC::Ptr<WebGLShader> m_attached_fragment_shader;
    Optional<VideoSamplerPlan> m_video_sampler_plan;
};

}
