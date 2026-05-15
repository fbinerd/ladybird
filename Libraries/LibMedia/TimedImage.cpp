/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibMedia/VideoFrame.h>

#include "TimedImage.h"

namespace Media {

static bool should_log_mundo_timed_image_diagnostic(size_t count)
{
    return count <= 16 || count % 120 == 0;
}

TimedImage::TimedImage(AK::Duration timestamp, NonnullRefPtr<Gfx::ImmutableBitmap>&& image)
    : m_timestamp(timestamp)
    , m_image(move(image))
{
}

TimedImage::TimedImage(AK::Duration timestamp, NonnullOwnPtr<VideoFrame>&& frame)
    : m_timestamp(timestamp)
    , m_frame(move(frame))
{
}

TimedImage::TimedImage() = default;
TimedImage::~TimedImage() = default;

AK::Duration const& TimedImage::timestamp() const
{
    VERIFY(is_valid());
    return m_timestamp;
}

Gfx::Size<u32> TimedImage::size() const
{
    VERIFY(is_valid());
    if (m_frame)
        return m_frame->size();
    return m_image->size().to_type<u32>();
}

bool TimedImage::has_lazy_bitmap() const
{
    return m_frame && m_frame->has_lazy_bitmap();
}

NonnullRefPtr<Gfx::ImmutableBitmap> TimedImage::image() const
{
    VERIFY(is_valid());
    if (m_frame) {
        static size_t s_image_materialize_request_count { 0 };
        auto count = ++s_image_materialize_request_count;
        if (should_log_mundo_timed_image_diagnostic(count)) {
            dbgln("MUNDO_MEDIA_TIMED_IMAGE image_request count={} timestamp={}ms lazy_bitmap={} size={}x{} frame={}",
                count,
                m_timestamp.to_milliseconds(),
                m_frame->has_lazy_bitmap(),
                m_frame->width(),
                m_frame->height(),
                static_cast<void const*>(m_frame.ptr()));
        }
        return m_frame->immutable_bitmap();
    }
    return *m_image;
}

NonnullRefPtr<Gfx::ImmutableBitmap> TimedImage::release_image()
{
    VERIFY(is_valid());
    m_timestamp = AK::Duration::zero();
    if (m_frame) {
        static size_t s_release_materialize_request_count { 0 };
        auto count = ++s_release_materialize_request_count;
        if (should_log_mundo_timed_image_diagnostic(count)) {
            dbgln("MUNDO_MEDIA_TIMED_IMAGE release_image_request count={} lazy_bitmap={} size={}x{} frame={}",
                count,
                m_frame->has_lazy_bitmap(),
                m_frame->width(),
                m_frame->height(),
                static_cast<void const*>(m_frame.ptr()));
        }
        auto image = m_frame->immutable_bitmap();
        m_frame = nullptr;
        return image;
    }
    return m_image.release_nonnull();
}

void TimedImage::clear()
{
    m_timestamp = AK::Duration::zero();
    m_image = nullptr;
    m_frame = nullptr;
}

}
