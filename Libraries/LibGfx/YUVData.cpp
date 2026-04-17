/*
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibGfx/YUVData.h>

#include <core/SkColorSpace.h>
#include <core/SkImage.h>
#include <core/SkYUVAInfo.h>
#include <core/SkYUVAPixmaps.h>
#include <cstddef>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/SkImageGanesh.h>

namespace Gfx {

namespace Details {

struct YUVDataImpl {
    IntSize size;
    u8 bit_depth;
    Media::Subsampling subsampling;
    Media::CodingIndependentCodePoints cicp;

    FixedArray<u8> y_buffer;
    FixedArray<u8> u_buffer;
    FixedArray<u8> v_buffer;
};

}

ErrorOr<NonnullOwnPtr<YUVData>> YUVData::create(IntSize size, u8 bit_depth, Media::Subsampling subsampling, Media::CodingIndependentCodePoints cicp)
{
    VERIFY(bit_depth <= 16);
    auto component_size = bit_depth <= 8 ? 1 : 2;

    auto y_buffer_size = static_cast<size_t>(size.width()) * size.height() * component_size;

    auto uv_size = subsampling.subsampled_size(size);
    auto uv_buffer_size = static_cast<size_t>(uv_size.width()) * uv_size.height() * component_size;

    auto y_buffer = TRY(FixedArray<u8>::create(y_buffer_size));
    auto u_buffer = TRY(FixedArray<u8>::create(uv_buffer_size));
    auto v_buffer = TRY(FixedArray<u8>::create(uv_buffer_size));

    auto impl = TRY(try_make<Details::YUVDataImpl>(Details::YUVDataImpl {
        .size = size,
        .bit_depth = bit_depth,
        .subsampling = subsampling,
        .cicp = cicp,
        .y_buffer = move(y_buffer),
        .u_buffer = move(u_buffer),
        .v_buffer = move(v_buffer),
    }));

    return adopt_nonnull_own_or_enomem(new (nothrow) YUVData(move(impl)));
}

YUVData::YUVData(NonnullOwnPtr<Details::YUVDataImpl> impl)
    : m_impl(move(impl))
{
}

YUVData::~YUVData() = default;

IntSize YUVData::size() const
{
    return m_impl->size;
}

u8 YUVData::bit_depth() const
{
    return m_impl->bit_depth;
}

Media::Subsampling YUVData::subsampling() const
{
    return m_impl->subsampling;
}

Media::CodingIndependentCodePoints const& YUVData::cicp() const
{
    return m_impl->cicp;
}

Bytes YUVData::y_data()
{
    return m_impl->y_buffer.span();
}

Bytes YUVData::u_data()
{
    return m_impl->u_buffer.span();
}

Bytes YUVData::v_data()
{
    return m_impl->v_buffer.span();
}

void YUVData::expand_samples_to_full_16_bit_range()
{
    auto const shift = 16 - m_impl->bit_depth;
    auto const inverse_shift = m_impl->bit_depth - shift;

    for (auto buffer : { m_impl->y_buffer.span(), m_impl->u_buffer.span(), m_impl->v_buffer.span() }) {
        auto* samples = reinterpret_cast<u16*>(buffer.data());
        auto sample_count = buffer.size() / sizeof(u16);
        for (size_t i = 0; i < sample_count; i++)
            samples[i] = static_cast<u16>((samples[i] << shift) | (samples[i] >> inverse_shift));
    }

    m_impl->bit_depth = 16;
}

static SkYUVColorSpace skia_yuv_color_space(Media::CodingIndependentCodePoints cicp)
{
    bool full_range = cicp.video_full_range_flag() == Media::VideoFullRangeFlag::Full;

    switch (cicp.matrix_coefficients()) {
    case Media::MatrixCoefficients::BT709:
        return full_range ? kRec709_Full_SkYUVColorSpace : kRec709_Limited_SkYUVColorSpace;
    case Media::MatrixCoefficients::FCC:
        return full_range ? kFCC_Full_SkYUVColorSpace : kFCC_Limited_SkYUVColorSpace;
    case Media::MatrixCoefficients::BT470BG:
    case Media::MatrixCoefficients::BT601:
        return full_range ? kJPEG_Full_SkYUVColorSpace : kRec601_Limited_SkYUVColorSpace;
    case Media::MatrixCoefficients::SMPTE240:
        return full_range ? kSMPTE240_Full_SkYUVColorSpace : kSMPTE240_Limited_SkYUVColorSpace;
    case Media::MatrixCoefficients::YCgCo:
        return full_range ? kYCgCo_16bit_Full_SkYUVColorSpace : kYCgCo_16bit_Limited_SkYUVColorSpace;
    case Media::MatrixCoefficients::BT2020NonConstantLuminance:
    case Media::MatrixCoefficients::BT2020ConstantLuminance:
        return full_range ? kBT2020_16bit_Full_SkYUVColorSpace : kBT2020_16bit_Limited_SkYUVColorSpace;
    case Media::MatrixCoefficients::SMPTE2085:
        return full_range ? kYDZDX_Full_SkYUVColorSpace : kYDZDX_Limited_SkYUVColorSpace;
    case Media::MatrixCoefficients::Identity:
        return kIdentity_SkYUVColorSpace;
    default:
        // Default to BT.709 for unsupported matrix coefficients
        return full_range ? kRec709_Full_SkYUVColorSpace : kRec709_Limited_SkYUVColorSpace;
    }
}

static SkYUVAInfo::Subsampling skia_subsampling(Media::Subsampling subsampling)
{
    if (!subsampling.x() && !subsampling.y())
        return SkYUVAInfo::Subsampling::k444;
    if (subsampling.x() && !subsampling.y())
        return SkYUVAInfo::Subsampling::k422;
    if (!subsampling.x() && subsampling.y())
        return SkYUVAInfo::Subsampling::k440;
    return SkYUVAInfo::Subsampling::k420;
}

SkYUVAPixmaps YUVData::make_pixmaps() const
{
    auto skia_size = SkISize::Make(m_impl->size.width(), m_impl->size.height());

    auto yuva_info = SkYUVAInfo(
        skia_size,
        SkYUVAInfo::PlaneConfig::kY_U_V,
        skia_subsampling(m_impl->subsampling),
        skia_yuv_color_space(m_impl->cicp));

    SkColorType color_type;
    SkYUVAPixmapInfo::DataType data_type;
    size_t component_size;
    if (m_impl->bit_depth <= 8) {
        color_type = kAlpha_8_SkColorType;
        data_type = SkYUVAPixmapInfo::DataType::kUnorm8;
        component_size = 1;
    } else {
        color_type = kA16_unorm_SkColorType;
        data_type = SkYUVAPixmapInfo::DataType::kUnorm16;
        component_size = 2;
    }

    auto y_row_bytes = static_cast<size_t>(m_impl->size.width()) * component_size;

    auto uv_size = m_impl->subsampling.subsampled_size(m_impl->size);
    auto uv_row_bytes = static_cast<size_t>(uv_size.width()) * component_size;

    SkYUVAPixmapInfo pixmap_info(yuva_info, data_type, nullptr);

    // Create pixmaps from our buffers
    SkPixmap y_pixmap(
        SkImageInfo::Make(skia_size, color_type, kOpaque_SkAlphaType),
        m_impl->y_buffer.data(),
        y_row_bytes);
    SkPixmap u_pixmap(
        SkImageInfo::Make(uv_size.width(), uv_size.height(), color_type, kOpaque_SkAlphaType),
        m_impl->u_buffer.data(),
        uv_row_bytes);
    SkPixmap v_pixmap(
        SkImageInfo::Make(uv_size.width(), uv_size.height(), color_type, kOpaque_SkAlphaType),
        m_impl->v_buffer.data(),
        uv_row_bytes);

    SkPixmap plane_pixmaps[SkYUVAInfo::kMaxPlanes] = { y_pixmap, u_pixmap, v_pixmap, {} };

    return SkYUVAPixmaps::FromExternalPixmaps(yuva_info, plane_pixmaps);
}

}
