/*
 * Copyright (c) 2022-2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/ByteBuffer.h>
#include <AK/Function.h>
#include <AK/NonnullRefPtr.h>
#include <AK/RefCounted.h>
#include <AK/RefPtr.h>
#include <AK/Time.h>
#include <AK/Error.h>
#include <LibGfx/Forward.h>
#include <LibGfx/Size.h>
#include <LibMedia/Color/CodingIndependentCodePoints.h>
#include <LibMedia/Export.h>

namespace Media {

struct MEDIA_API NV12VideoFrameData : public RefCounted<NV12VideoFrameData> {
    ~NV12VideoFrameData();

    u8 const* y_plane_data() const { return m_y_plane_data ? m_y_plane_data : y_plane.data(); }
    u8 const* uv_plane_data() const { return m_uv_plane_data ? m_uv_plane_data : uv_plane.data(); }
    size_t y_plane_size() const { return m_y_plane_size ? m_y_plane_size : y_plane.size(); }
    size_t uv_plane_size() const { return m_uv_plane_size ? m_uv_plane_size : uv_plane.size(); }

    void set_external_planes(u8 const* y_plane_data, size_t y_plane_size, u8 const* uv_plane_data, size_t uv_plane_size, Function<void()>&& cleanup)
    {
        m_y_plane_data = y_plane_data;
        m_y_plane_size = y_plane_size;
        m_uv_plane_data = uv_plane_data;
        m_uv_plane_size = uv_plane_size;
        m_external_storage_cleanup = move(cleanup);
    }

    ByteBuffer y_plane;
    ByteBuffer uv_plane;
    int width { 0 };
    int height { 0 };
    int y_stride { 0 };
    int uv_stride { 0 };
    CodingIndependentCodePoints cicp;

private:
    u8 const* m_y_plane_data { nullptr };
    size_t m_y_plane_size { 0 };
    u8 const* m_uv_plane_data { nullptr };
    size_t m_uv_plane_size { 0 };
    Function<void()> m_external_storage_cleanup;
};

class MEDIA_API VideoFrame final {

public:
    using BitmapFactory = Function<ErrorOr<NonnullRefPtr<Gfx::ImmutableBitmap>>()>;
    using NV12DataFactory = Function<ErrorOr<NonnullRefPtr<NV12VideoFrameData>>()>;

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
    VideoFrame(
        AK::Duration timestamp,
        AK::Duration duration,
        Gfx::Size<u32> size,
        u8 bit_depth,
        CodingIndependentCodePoints cicp,
        BitmapFactory&& bitmap_factory,
        NonnullRefPtr<NV12VideoFrameData>);
    VideoFrame(
        AK::Duration timestamp,
        AK::Duration duration,
        Gfx::Size<u32> size,
        u8 bit_depth,
        CodingIndependentCodePoints cicp,
        BitmapFactory&& bitmap_factory,
        NV12DataFactory&& nv12_data_factory);
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
    bool has_lazy_nv12_data() const { return m_nv12_data == nullptr && m_nv12_data_factory; }
    NV12VideoFrameData const* nv12_data() const;

private:
    AK::Duration m_timestamp;
    AK::Duration m_duration;
    Gfx::Size<u32> m_size;
    u8 m_bit_depth;
    CodingIndependentCodePoints m_cicp;
    mutable RefPtr<Gfx::ImmutableBitmap> m_bitmap;
    mutable BitmapFactory m_bitmap_factory;
    mutable RefPtr<NV12VideoFrameData> m_nv12_data;
    mutable NV12DataFactory m_nv12_data_factory;
};

}
