/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 * Copyright (c) 2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2024, Luke Wilde <luke@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <LibWeb/WebGL/Types.h>
#include <LibWeb/WebGL/WebGLObject.h>

namespace Web::WebGL {

class WebGLTexture final : public WebGLObject {
    WEB_PLATFORM_OBJECT(WebGLTexture, WebGLObject);
    GC_DECLARE_ALLOCATOR(WebGLTexture);

public:
    static GC::Ref<WebGLTexture> create(JS::Realm& realm, GC::Ref<WebGLRenderingContextBase>, GLuint handle);

    virtual ~WebGLTexture();

    struct HardwareVideoBacking {
        u64 frame_id { 0 };
        u32 width { 0 };
        u32 height { 0 };
        char const* backend { "none" };
        char const* upload_mode { "none" };
        char const* copy_stage { "none" };
        bool direct_zero_copy { false };
        bool copied_on_gpu { false };
        char const* direct_sampling_route { "none" };
        char const* direct_sampling_reason { "none" };
        u32 source_vulkan_format { 0 };
        u32 source_vulkan_layout { 0 };
        u64 source_allocation_size { 0 };
        u32 source_handle_type { 0 };
        bool source_single_optimal_multiplanar { false };
    };

    void set_hardware_video_backing(HardwareVideoBacking);
    void clear_hardware_video_backing();
    bool has_hardware_video_backing() const { return m_hardware_video_backing.has_value(); }
    Optional<HardwareVideoBacking> const& hardware_video_backing() const { return m_hardware_video_backing; }

protected:
    explicit WebGLTexture(JS::Realm&, GC::Ref<WebGLRenderingContextBase>, GLuint handle);

    virtual void initialize(JS::Realm&) override;

private:
    Optional<HardwareVideoBacking> m_hardware_video_backing;
};

}
