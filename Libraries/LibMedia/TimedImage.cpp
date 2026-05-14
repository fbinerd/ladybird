/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImmutableBitmap.h>
#include <LibMedia/VideoFrame.h>

#include "TimedImage.h"

namespace Media {

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

NonnullRefPtr<Gfx::ImmutableBitmap> TimedImage::image() const
{
    VERIFY(is_valid());
    if (m_frame)
        return m_frame->immutable_bitmap();
    return *m_image;
}

NonnullRefPtr<Gfx::ImmutableBitmap> TimedImage::release_image()
{
    VERIFY(is_valid());
    m_timestamp = AK::Duration::zero();
    if (m_frame) {
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
