/*
 * Copyright (c) 2025-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Noncopyable.h>
#include <AK/OwnPtr.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <LibGfx/Forward.h>
#include <LibMedia/VideoFrame.h>

namespace Media {

class TimedImage final {
    AK_MAKE_NONCOPYABLE(TimedImage);
    AK_MAKE_DEFAULT_MOVABLE(TimedImage);

public:
    TimedImage(AK::Duration timestamp, NonnullRefPtr<Gfx::ImmutableBitmap>&& image);
    TimedImage(AK::Duration timestamp, NonnullOwnPtr<VideoFrame>&& frame);
    TimedImage();
    ~TimedImage();

    bool is_valid() const { return m_image != nullptr || m_frame != nullptr; }
    AK::Duration const& timestamp() const;
    NonnullRefPtr<Gfx::ImmutableBitmap> image() const;
    NonnullRefPtr<Gfx::ImmutableBitmap> release_image();
    void clear();

private:
    AK::Duration m_timestamp;
    RefPtr<Gfx::ImmutableBitmap> m_image;
    OwnPtr<VideoFrame> m_frame;
};

}
