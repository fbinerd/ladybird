/*
 * Copyright (c) 2020, the SerenityOS developers.
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Optional.h>
#include <LibGfx/Forward.h>
#include <LibWeb/DOM/DocumentLoadEventDelayer.h>
#include <LibWeb/Forward.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/WebIDL/CallbackType.h>
#include <LibWeb/WebIDL/ExceptionOr.h>
#include <LibWeb/WebIDL/Types.h>

namespace Web::HTML {

struct VideoFrame {
    RefPtr<Gfx::Bitmap> frame;
    double position { 0.0 };
};

class HTMLVideoElement final : public HTMLMediaElement {
    WEB_PLATFORM_OBJECT(HTMLVideoElement, HTMLMediaElement);
    GC_DECLARE_ALLOCATOR(HTMLVideoElement);

public:
    static constexpr bool OVERRIDES_FINALIZE = true;

    virtual ~HTMLVideoElement() override;

    Layout::VideoBox* layout_node();
    Layout::VideoBox const* layout_node() const;

    void set_intrinsic_video_dimensions(Optional<Gfx::Size<u32>>);
    u32 video_width() const;
    u32 video_height() const;

    virtual void update_intrinsic_video_dimensions() override;
    virtual void update_natural_dimensions() override;
    Optional<Gfx::Size<u32>> natural_media_size() const;
    Optional<CSSPixelSize> natural_element_size() const;

    RefPtr<Gfx::Bitmap> const& poster_frame() const { return m_poster_frame; }

    // https://html.spec.whatwg.org/multipage/media.html#the-video-element:the-video-element-7
    // NB: We combine the values of...
    //      - The last frame of the video to have been rendered
    //      - The frame of video corresponding to the current playback position
    //     ...into the value of VideoFrame below, as the playback system itself implements
    //     the details of the selection of a video frame to match the specification in this
    //     respect.
    enum class Representation : u8 {
        VideoFrame,
        FirstVideoFrame,
        PosterFrame,
        TransparentBlack,
    };
    Representation current_representation() const;

    // FIXME: This is a hack for images used as CanvasImageSource. Do something more elegant.
    RefPtr<Gfx::ImmutableBitmap> bitmap() const;

    WebIDL::UnsignedLong request_video_frame_callback(GC::Ref<WebIDL::CallbackType>);
    void cancel_video_frame_callback(WebIDL::UnsignedLong handle);
    void notify_about_new_video_frame();

private:
    HTMLVideoElement(DOM::Document&, DOM::QualifiedName);

    virtual void initialize(JS::Realm&) override;
    virtual void finalize() override;
    virtual void visit_edges(Cell::Visitor&) override;

    virtual void attribute_changed(FlyString const& name, Optional<String> const& old_value, Optional<String> const& value, Optional<FlyString> const& namespace_) override;

    // https://html.spec.whatwg.org/multipage/media.html#the-video-element:dimension-attributes
    virtual bool supports_dimension_attributes() const override { return true; }

    virtual GC::Ptr<Layout::Node> create_layout_node(GC::Ref<CSS::ComputedProperties>) override;

    WebIDL::ExceptionOr<void> determine_element_poster_frame(Optional<String> const& poster);

    GC::Ptr<HTML::VideoTrack> m_video_track;
    VideoFrame m_current_frame;
    RefPtr<Gfx::Bitmap> m_poster_frame;

    Optional<Gfx::Size<u32>> m_intrinsic_video_dimensions;
    Optional<CSSPixelSize> m_natural_dimensiosn;

    GC::Ptr<Fetch::Infrastructure::FetchController> m_fetch_controller;
    Optional<DOM::DocumentLoadEventDelayer> m_load_event_delayer;

    struct VideoFrameCallback {
        WebIDL::UnsignedLong handle { 0 };
        GC::Ref<WebIDL::CallbackType> callback;
    };

    Vector<VideoFrameCallback> m_video_frame_callbacks;
    WebIDL::UnsignedLong m_next_video_frame_callback_handle { 1 };
    u32 m_presented_video_frame_count { 0 };
};

}
