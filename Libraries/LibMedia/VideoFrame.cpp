/*
 * Copyright (c) 2022-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImmutableBitmap.h>
#include <AK/Debug.h>

#include "VideoFrame.h"

namespace Media {

NV12VideoFrameData::~NV12VideoFrameData()
{
    if (m_external_storage_cleanup)
        m_external_storage_cleanup();
}

VideoFrame::VideoFrame(
    AK::Duration timestamp,
    AK::Duration duration,
    Gfx::Size<u32> size,
    u8 bit_depth, CodingIndependentCodePoints cicp,
    NonnullRefPtr<Gfx::ImmutableBitmap> bitmap)
    : m_timestamp(timestamp)
    , m_duration(duration)
    , m_size(size)
    , m_bit_depth(bit_depth)
    , m_cicp(cicp)
    , m_bitmap(move(bitmap))
{
}

VideoFrame::VideoFrame(
    AK::Duration timestamp,
    AK::Duration duration,
    Gfx::Size<u32> size,
    u8 bit_depth, CodingIndependentCodePoints cicp,
    BitmapFactory&& bitmap_factory)
    : m_timestamp(timestamp)
    , m_duration(duration)
    , m_size(size)
    , m_bit_depth(bit_depth)
    , m_cicp(cicp)
    , m_bitmap_factory(move(bitmap_factory))
{
}

VideoFrame::VideoFrame(
    AK::Duration timestamp,
    AK::Duration duration,
    Gfx::Size<u32> size,
    u8 bit_depth, CodingIndependentCodePoints cicp,
    BitmapFactory&& bitmap_factory,
    NonnullRefPtr<NV12VideoFrameData> nv12_data)
    : m_timestamp(timestamp)
    , m_duration(duration)
    , m_size(size)
    , m_bit_depth(bit_depth)
    , m_cicp(cicp)
    , m_bitmap_factory(move(bitmap_factory))
    , m_nv12_data(move(nv12_data))
{
}

VideoFrame::VideoFrame(
    AK::Duration timestamp,
    AK::Duration duration,
    Gfx::Size<u32> size,
    u8 bit_depth, CodingIndependentCodePoints cicp,
    BitmapFactory&& bitmap_factory,
    NV12DataFactory&& nv12_data_factory)
    : m_timestamp(timestamp)
    , m_duration(duration)
    , m_size(size)
    , m_bit_depth(bit_depth)
    , m_cicp(cicp)
    , m_bitmap_factory(move(bitmap_factory))
    , m_nv12_data_factory(move(nv12_data_factory))
{
}

VideoFrame::~VideoFrame() = default;

NonnullRefPtr<Gfx::ImmutableBitmap> VideoFrame::immutable_bitmap() const
{
    if (!m_bitmap) {
        VERIFY(m_bitmap_factory);
        auto bitmap_or_error = m_bitmap_factory();
        if (bitmap_or_error.is_error()) {
            dbgln("MUNDO_MEDIA_VIDEO_FRAME lazy_bitmap_materialize_failed error={}", bitmap_or_error.error());
            VERIFY_NOT_REACHED();
        }
        m_bitmap = bitmap_or_error.release_value();
        m_bitmap_factory = {};
    }
    return *m_bitmap;
}

NV12VideoFrameData const* VideoFrame::nv12_data() const
{
    if (!m_nv12_data && m_nv12_data_factory) {
        auto nv12_data_or_error = m_nv12_data_factory();
        if (nv12_data_or_error.is_error()) {
            dbgln("MUNDO_MEDIA_VIDEO_FRAME lazy_nv12_materialize_failed error={}", nv12_data_or_error.error());
            m_nv12_data_factory = {};
            return nullptr;
        }
        m_nv12_data = nv12_data_or_error.release_value();
        m_nv12_data_factory = {};
    }
    return m_nv12_data.ptr();
}

}
