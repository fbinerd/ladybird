/*
 * Copyright (c) 2022-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Function.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <AK/Error.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>
#include <LibMedia/Export.h>

namespace Media {

class MEDIA_API VideoFrame final {

public:
    using BitmapFactory = Function<ErrorOr<NonnullRefPtr<Gfx::ImmutableBitmap>>()>;

    VideoFrame(
        AK::Duration timestamp,
        AK::Duration duration,
        Gfx::Size<u32> size,
        u8 bit_depth,
        CodingIndependentCodePoints cicp,
        NonnullRefPtr<Gfx::ImmutableBitmap> bitmap);
    VideoFrame(
        AK::Duration timestamp,
        AK::Duration duration,
        Gfx::Size<u32> size,
        u8 bit_depth,
        CodingIndependentCodePoints cicp,
        BitmapFactory&& bitmap_factory);
    ~VideoFrame();

    AK::Duration timestamp() const { return m_timestamp; }
    AK::Duration duration() const { return m_duration; }

    Gfx::Size<u32> size() const { return m_size; }
    u32 width() const { return size().width(); }
    u32 height() const { return size().height(); }

    u8 bit_depth() const { return m_bit_depth; }
    CodingIndependentCodePoints& cicp() { return m_cicp; }

    NonnullRefPtr<Gfx::ImmutableBitmap> immutable_bitmap() const;
    bool has_lazy_bitmap() const { return m_bitmap == nullptr && m_bitmap_factory; }

private:
    AK::Duration m_timestamp;
    AK::Duration m_duration;
    Gfx::Size<u32> m_size;
    u8 m_bit_depth;
    CodingIndependentCodePoints m_cicp;
    mutable RefPtr<Gfx::ImmutableBitmap> m_bitmap;
    mutable BitmapFactory m_bitmap_factory;
};

}
