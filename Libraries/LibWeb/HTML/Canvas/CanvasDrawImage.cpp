/*
 * Copyright (c) 2022, Sam Atkins <atkinssj@serenityos.org>
 * Copyright (c) 2025, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibGfx/Bitmap.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibMedia/VideoFrame.h>
#include <LibWeb/HTML/Canvas/CanvasDrawImage.h>
#include <LibWeb/HTML/ImageBitmap.h>

namespace Web::HTML {

static u8 clamp_to_u8(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return static_cast<u8>(value);
}

static void convert_nv12_pixel_to_rgba(Media::CodingIndependentCodePoints const& cicp, u8 y, u8 u, u8 v, u8* dst)
{
    auto y_value = static_cast<int>(y);
    auto u_offset = static_cast<int>(u) - 128;
    auto v_offset = static_cast<int>(v) - 128;

    int r;
    int g;
    int b;

    if (cicp.video_full_range_flag() == Media::VideoFullRangeFlag::Full) {
        switch (cicp.matrix_coefficients()) {
        case Media::MatrixCoefficients::BT601:
        case Media::MatrixCoefficients::BT470BG:
            r = y_value + ((359 * v_offset) >> 8);
            g = y_value - ((88 * u_offset + 183 * v_offset) >> 8);
            b = y_value + ((454 * u_offset) >> 8);
            break;
        case Media::MatrixCoefficients::BT2020NonConstantLuminance:
        case Media::MatrixCoefficients::BT2020ConstantLuminance:
            r = y_value + ((377 * v_offset) >> 8);
            g = y_value - ((42 * u_offset + 146 * v_offset) >> 8);
            b = y_value + ((482 * u_offset) >> 8);
            break;
        case Media::MatrixCoefficients::BT709:
        case Media::MatrixCoefficients::Unspecified:
        default:
            r = y_value + ((403 * v_offset) >> 8);
            g = y_value - ((48 * u_offset + 120 * v_offset) >> 8);
            b = y_value + ((475 * u_offset) >> 8);
            break;
        }
    } else {
        auto c = max(0, y_value - 16);
        switch (cicp.matrix_coefficients()) {
        case Media::MatrixCoefficients::BT601:
        case Media::MatrixCoefficients::BT470BG:
            r = (298 * c + 409 * v_offset + 128) >> 8;
            g = (298 * c - 100 * u_offset - 208 * v_offset + 128) >> 8;
            b = (298 * c + 516 * u_offset + 128) >> 8;
            break;
        case Media::MatrixCoefficients::BT2020NonConstantLuminance:
        case Media::MatrixCoefficients::BT2020ConstantLuminance:
            r = (298 * c + 430 * v_offset + 128) >> 8;
            g = (298 * c - 48 * u_offset - 167 * v_offset + 128) >> 8;
            b = (298 * c + 548 * u_offset + 128) >> 8;
            break;
        case Media::MatrixCoefficients::BT709:
        case Media::MatrixCoefficients::Unspecified:
        default:
            r = (298 * c + 459 * v_offset + 128) >> 8;
            g = (298 * c - 55 * u_offset - 136 * v_offset + 128) >> 8;
            b = (298 * c + 541 * u_offset + 128) >> 8;
            break;
        }
    }

    dst[0] = clamp_to_u8(r);
    dst[1] = clamp_to_u8(g);
    dst[2] = clamp_to_u8(b);
    dst[3] = 255;
}

Gfx::IntSize canvas_image_source_dimensions(CanvasImageSource const& image)
{
    return image.visit(
        [](GC::Root<HTMLImageElement> const& source) -> Gfx::IntSize {
            if (auto immutable_bitmap = source->immutable_bitmap())
                return immutable_bitmap->size();

            // FIXME: This is very janky and not correct.
            return { source->width(), source->height() };
        },
        [](GC::Root<SVG::SVGImageElement> const& source) -> Gfx::IntSize {
            if (auto immutable_bitmap = source->current_image_bitmap())
                return immutable_bitmap->size();

            // FIXME: This is very janky and not correct.
            return { source->width()->anim_val()->value(), source->height()->anim_val()->value() };
        },
        [](GC::Root<HTMLCanvasElement> const& source) -> Gfx::IntSize {
            if (auto painting_surface = source->surface())
                return painting_surface->size();
            return { source->width(), source->height() };
        },
        [](GC::Root<ImageBitmap> const& source) -> Gfx::IntSize {
            if (auto* bitmap = source->bitmap())
                return bitmap->size();
            return { source->width(), source->height() };
        },
        [](GC::Root<OffscreenCanvas> const& source) -> Gfx::IntSize {
            if (auto bitmap = source->bitmap())
                return bitmap->size();
            return {};
        },
        [](GC::Root<HTMLVideoElement> const& source) -> Gfx::IntSize {
            if (auto const* frame = source->current_media_frame())
                return frame->size().to_type<int>();
            return { source->video_width(), source->video_height() };
        });
}

RefPtr<Gfx::ImmutableBitmap> canvas_image_source_bitmap(CanvasImageSource const& image)
{
    return image.visit(
        [](OneOf<GC::Root<HTMLImageElement>, GC::Root<SVG::SVGImageElement>> auto const& element) {
            return element->default_image_bitmap();
        },
        [](GC::Root<HTMLCanvasElement> const& canvas) -> RefPtr<Gfx::ImmutableBitmap> {
            canvas->present();
            auto surface = canvas->surface();
            if (!surface)
                return Gfx::ImmutableBitmap::create(*canvas->get_bitmap_from_surface());
            return Gfx::ImmutableBitmap::create_snapshot_from_painting_surface(*surface);
        },
        [](OneOf<GC::Root<ImageBitmap>, GC::Root<OffscreenCanvas>> auto const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            auto bitmap = source->bitmap();
            if (!bitmap)
                return {};
            return Gfx::ImmutableBitmap::create(*bitmap);
        },
        [](GC::Root<HTMLVideoElement> const& source) -> RefPtr<Gfx::ImmutableBitmap> {
            return source->bitmap();
        });
}

Media::VideoFrame const* canvas_image_source_video_frame(CanvasImageSource const& image)
{
    return image.visit(
        [](GC::Root<HTMLVideoElement> const& source) -> Media::VideoFrame const* {
            return source->current_media_frame();
        },
        [](auto const&) -> Media::VideoFrame const* {
            return nullptr;
        });
}

ErrorOr<NonnullRefPtr<Gfx::Bitmap>> create_scaled_bitmap_from_video_frame(Media::VideoFrame const& frame, Gfx::IntRect const& source_rect, Gfx::IntSize const& target_size)
{
    auto const* nv12_data = frame.nv12_data();
    if (!nv12_data)
        return Error::from_string_literal("Video frame does not have NV12 data");
    if (source_rect.is_empty() || target_size.is_empty())
        return Error::from_string_literal("Video frame draw target is empty");

    auto bitmap = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::RGBA8888, Gfx::AlphaType::Premultiplied, target_size));
    auto const source_width = static_cast<u32>(source_rect.width());
    auto const source_height = static_cast<u32>(source_rect.height());
    auto const target_width = static_cast<u32>(target_size.width());
    auto const target_height = static_cast<u32>(target_size.height());

    for (u32 row = 0; row < target_height; ++row) {
        auto source_y = source_rect.y() + static_cast<int>((static_cast<u64>(row) * source_height) / target_height);
        source_y = clamp(source_y, 0, nv12_data->height - 1);
        auto* dst_row = bitmap->scanline_u8(row);
        auto const* y_row = nv12_data->y_plane_data() + static_cast<size_t>(source_y) * static_cast<size_t>(nv12_data->y_stride);
        auto const* uv_row = nv12_data->uv_plane_data() + static_cast<size_t>(source_y / 2) * static_cast<size_t>(nv12_data->uv_stride);

        for (u32 col = 0; col < target_width; ++col) {
            auto source_x = source_rect.x() + static_cast<int>((static_cast<u64>(col) * source_width) / target_width);
            source_x = clamp(source_x, 0, nv12_data->width - 1);
            auto uv_x = (source_x / 2) * 2;
            convert_nv12_pixel_to_rgba(nv12_data->cicp, y_row[source_x], uv_row[uv_x], uv_row[uv_x + 1], dst_row + col * 4);
        }
    }

    return bitmap;
}

WebIDL::ExceptionOr<void> CanvasDrawImage::draw_image(CanvasImageSource const& image, float destination_x, float destination_y)
{
    // If not specified, the dw and dh arguments must default to the values of sw and sh, interpreted such that one CSS pixel in the image is treated as one unit in the output bitmap's coordinate space.
    // If the sx, sy, sw, and sh arguments are omitted, then they must default to 0, 0, the image's intrinsic width in image pixels, and the image's intrinsic height in image pixels, respectively.
    // If the image has no intrinsic dimensions, then the concrete object size must be used instead, as determined using the CSS "Concrete Object Size Resolution" algorithm, with the specified size having
    // neither a definite width nor height, nor any additional constraints, the object's intrinsic properties being those of the image argument, and the default object size being the size of the output bitmap.
    auto size = canvas_image_source_dimensions(image);
    return draw_image_internal(image, 0, 0, size.width(), size.height(), destination_x, destination_y, size.width(), size.height());
}

WebIDL::ExceptionOr<void> CanvasDrawImage::draw_image(CanvasImageSource const& image, float destination_x, float destination_y, float destination_width, float destination_height)
{
    // If the sx, sy, sw, and sh arguments are omitted, then they must default to 0, 0, the image's intrinsic width in image pixels, and the image's intrinsic height in image pixels, respectively.
    // If the image has no intrinsic dimensions, then the concrete object size must be used instead, as determined using the CSS "Concrete Object Size Resolution" algorithm, with the specified size having
    // neither a definite width nor height, nor any additional constraints, the object's intrinsic properties being those of the image argument, and the default object size being the size of the output bitmap.
    auto size = canvas_image_source_dimensions(image);
    return draw_image_internal(image, 0, 0, size.width(), size.height(), destination_x, destination_y, destination_width, destination_height);
}

WebIDL::ExceptionOr<void> CanvasDrawImage::draw_image(CanvasImageSource const& image, float source_x, float source_y, float source_width, float source_height, float destination_x, float destination_y, float destination_width, float destination_height)
{
    return draw_image_internal(image, source_x, source_y, source_width, source_height, destination_x, destination_y, destination_width, destination_height);
}

}
