/*
 * Copyright (c) 2023-2024, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 * Copyright (c) 2026, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/OwnPtr.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/PaintingSurface.h>
#include <LibGfx/SkiaBackendContext.h>
#include <LibGfx/SkiaUtils.h>
#include <LibGfx/YUVData.h>

#include <core/SkBitmap.h>
#include <core/SkCanvas.h>
#include <core/SkColorSpace.h>
#include <core/SkImage.h>
#include <core/SkSurface.h>
#include <core/SkYUVAPixmaps.h>
#include <gpu/ganesh/GrDirectContext.h>
#include <gpu/ganesh/SkImageGanesh.h>
#include <stdlib.h>
#include <string.h>

namespace Gfx {

StringView export_format_name(ExportFormat format)
{
    switch (format) {
#define ENUMERATE_EXPORT_FORMAT(format) \
    case Gfx::ExportFormat::format:     \
        return #format##sv;
        ENUMERATE_EXPORT_FORMATS(ENUMERATE_EXPORT_FORMAT)
#undef ENUMERATE_EXPORT_FORMAT
    }
    VERIFY_NOT_REACHED();
}

struct ImmutableBitmapImpl {
    RefPtr<SkiaBackendContext> context;
    sk_sp<SkImage> sk_image;
    SkBitmap sk_bitmap;
    RefPtr<Gfx::Bitmap const> bitmap;
    ColorSpace color_space;
};

int ImmutableBitmap::width() const
{
    return m_impl->sk_image->width();
}

int ImmutableBitmap::height() const
{
    return m_impl->sk_image->height();
}

IntRect ImmutableBitmap::rect() const
{
    return { {}, size() };
}

IntSize ImmutableBitmap::size() const
{
    return { width(), height() };
}

AlphaType ImmutableBitmap::alpha_type() const
{
    // We assume premultiplied alpha type for opaque surfaces since that is Skia's preferred alpha type and the
    // effective pixel data is identical between premultiplied and unpremultiplied in that case.
    return m_impl->sk_image->alphaType() == kUnpremul_SkAlphaType ? AlphaType::Unpremultiplied : AlphaType::Premultiplied;
}

SkImage const* ImmutableBitmap::sk_image() const
{
    return m_impl->sk_image.get();
}

static int bytes_per_pixel_for_export_format(ExportFormat format)
{
    switch (format) {
    case ExportFormat::Gray8:
    case ExportFormat::Alpha8:
        return 1;
    case ExportFormat::RGB565:
    case ExportFormat::RGBA5551:
    case ExportFormat::RGBA4444:
        return 2;
    case ExportFormat::RGB888:
        return 3;
    case ExportFormat::RGBA8888:
        return 4;
    default:
        VERIFY_NOT_REACHED();
    }
}

static SkColorType export_format_to_skia_color_type(ExportFormat format)
{
    switch (format) {
    case ExportFormat::Gray8:
        return SkColorType::kGray_8_SkColorType;
    case ExportFormat::Alpha8:
        return SkColorType::kAlpha_8_SkColorType;
    case ExportFormat::RGB565:
        return SkColorType::kRGB_565_SkColorType;
    case ExportFormat::RGBA5551:
        dbgln("FIXME: Support conversion to RGBA5551.");
        return SkColorType::kUnknown_SkColorType;
    case ExportFormat::RGBA4444:
        return SkColorType::kARGB_4444_SkColorType;
    case ExportFormat::RGB888:
        // This one needs to be converted manually because Skia has no valid 24-bit color type.
        VERIFY_NOT_REACHED();
    case ExportFormat::RGBA8888:
        return SkColorType::kRGBA_8888_SkColorType;
    default:
        VERIFY_NOT_REACHED();
    }
}

static u8 premultiply_channel(u8 channel, u8 alpha)
{
    return (channel * alpha + 127) / 255;
}

static u8 unpremultiply_channel(u8 channel, u8 alpha)
{
    if (alpha == 0)
        return 0;
    return clamp((channel * 255 + alpha / 2) / alpha, 0, 255);
}

static Color convert_alpha_for_export(Color pixel, AlphaType source_alpha_type, int flags)
{
    auto alpha = pixel.alpha();
    if (alpha == 0 || alpha == 255)
        return pixel;

    if ((flags & ExportFlags::PremultiplyAlpha) && source_alpha_type == AlphaType::Unpremultiplied) {
        return Color {
            premultiply_channel(pixel.red(), alpha),
            premultiply_channel(pixel.green(), alpha),
            premultiply_channel(pixel.blue(), alpha),
            alpha
        };
    }

    if (!(flags & ExportFlags::PremultiplyAlpha) && source_alpha_type == AlphaType::Premultiplied) {
        return Color {
            unpremultiply_channel(pixel.red(), alpha),
            unpremultiply_channel(pixel.green(), alpha),
            unpremultiply_channel(pixel.blue(), alpha),
            alpha
        };
    }

    return pixel;
}

static bool can_export_raster_bitmap_directly(Bitmap const& bitmap, ExportFormat format, int width, int height)
{
    if (width != bitmap.width() || height != bitmap.height())
        return false;

    return format == ExportFormat::RGB888 || format == ExportFormat::RGBA8888;
}

static Color color_from_raw_bitmap_bytes(u8 const* pixel, BitmapFormat format)
{
    switch (format) {
    case BitmapFormat::BGRx8888:
        return Color { pixel[2], pixel[1], pixel[0], 0xff };
    case BitmapFormat::BGRA8888:
        return Color { pixel[2], pixel[1], pixel[0], pixel[3] };
    case BitmapFormat::RGBx8888:
        return Color { pixel[0], pixel[1], pixel[2], 0xff };
    case BitmapFormat::RGBA8888:
        return Color { pixel[0], pixel[1], pixel[2], pixel[3] };
    case BitmapFormat::Invalid:
        VERIFY_NOT_REACHED();
    }
    VERIFY_NOT_REACHED();
}

static void export_raster_bitmap_directly(Bitmap const& bitmap, ByteBuffer& buffer, size_t buffer_pitch, ExportFormat format, int flags)
{
    auto* raw_buffer = buffer.data();
    if (format == ExportFormat::RGBA8888 && bitmap.format() == BitmapFormat::RGBA8888) {
        auto row_size = static_cast<size_t>(bitmap.width()) * 4;
        for (auto y = 0; y < bitmap.height(); y++) {
            auto target_y = flags & ExportFlags::FlipY ? bitmap.height() - y - 1 : y;
            memcpy(raw_buffer + (static_cast<size_t>(target_y) * buffer_pitch), bitmap.scanline_u8(y), row_size);
        }
        return;
    }

    for (auto y = 0; y < bitmap.height(); y++) {
        auto target_y = flags & ExportFlags::FlipY ? bitmap.height() - y - 1 : y;
        auto const* source_row = bitmap.scanline_u8(y);
        for (auto x = 0; x < bitmap.width(); x++) {
            auto pixel = convert_alpha_for_export(color_from_raw_bitmap_bytes(source_row + x * 4ull, bitmap.format()), bitmap.alpha_type(), flags);
            if (format == ExportFormat::RGB888) {
                auto buffer_offset = (target_y * buffer_pitch) + (x * 3ull);
                raw_buffer[buffer_offset + 0] = pixel.red();
                raw_buffer[buffer_offset + 1] = pixel.green();
                raw_buffer[buffer_offset + 2] = pixel.blue();
            } else {
                auto buffer_offset = (target_y * buffer_pitch) + (x * 4ull);
                raw_buffer[buffer_offset + 0] = pixel.red();
                raw_buffer[buffer_offset + 1] = pixel.green();
                raw_buffer[buffer_offset + 2] = pixel.blue();
                raw_buffer[buffer_offset + 3] = pixel.alpha();
            }
        }
    }
}

static int max_large_video_raster_width()
{
    auto const* raw_value = getenv("MUNDO_VIDEO_MAX_RASTER_WIDTH");
    if (!raw_value) {
        auto const* decoder_backend = getenv("MUNDO_VIDEO_DECODER_BACKEND");
        if (decoder_backend && (!strcmp(decoder_backend, "nvdec") || !strcmp(decoder_backend, "cuda"))) {
            auto const* nvdec_raw_value = getenv("MUNDO_VIDEO_MAX_RASTER_WIDTH_NVDEC");
            if (!nvdec_raw_value)
                return 0;

            auto nvdec_value = atoi(nvdec_raw_value);
            if (nvdec_value <= 0)
                return 0;

            return max(nvdec_value, 320);
        }

        return 1280;
    }

    auto value = atoi(raw_value);
    if (value <= 0)
        return 0;

    return max(value, 320);
}

static ErrorOr<NonnullRefPtr<Bitmap>> downscale_large_video_bitmap_if_needed(NonnullRefPtr<Bitmap> bitmap)
{
    auto max_width = max_large_video_raster_width();
    if (max_width == 0 || bitmap->width() <= max_width)
        return bitmap;

    auto scaled_height = max(1, static_cast<int>((static_cast<i64>(bitmap->height()) * max_width) / bitmap->width()));
    auto scaled_bitmap = TRY(bitmap->scaled(max_width, scaled_height, ScalingMode::Bilinear));

    static size_t s_downscaled_large_video_frame_count { 0 };
    auto count = ++s_downscaled_large_video_frame_count;
    if (count <= 8 || count % 120 == 0) {
        dbgln("MUNDO_IMMUTABLE_BITMAP yuv_large_downscale count={} from={}x{} to={}x{}",
            count, bitmap->width(), bitmap->height(), scaled_bitmap->width(), scaled_bitmap->height());
    }

    return scaled_bitmap;
}

static Optional<IntSize> scaled_large_video_size(IntSize size)
{
    auto max_width = max_large_video_raster_width();
    if (max_width == 0 || size.width() <= max_width)
        return {};

    return IntSize {
        max_width,
        max(1, static_cast<int>((static_cast<i64>(size.height()) * max_width) / size.width()))
    };
}

enum class YUVFrameBackend {
    Auto,
    Software,
    Gpu,
};

static YUVFrameBackend yuv_frame_backend()
{
    auto const* raw_value = getenv("MUNDO_VIDEO_BACKEND");
    if (!raw_value)
        return YUVFrameBackend::Auto;

    if (!strcmp(raw_value, "software") || !strcmp(raw_value, "cpu"))
        return YUVFrameBackend::Software;
    if (!strcmp(raw_value, "gpu") || !strcmp(raw_value, "hardware"))
        return YUVFrameBackend::Gpu;

    return YUVFrameBackend::Auto;
}

static bool is_large_video_frame(IntSize size)
{
    return static_cast<i64>(size.width()) * size.height() >= 1920 * 1080;
}

static size_t max_gpu_yuv_upload_pixels()
{
    auto const* raw_value = getenv("MUNDO_VIDEO_GPU_YUV_MAX_PIXELS");
    if (!raw_value)
        return 1920 * 1080;

    auto value = atoll(raw_value);
    if (value <= 0)
        return 0;

    return static_cast<size_t>(value);
}

static bool exceeds_gpu_yuv_upload_size_limit(IntSize size)
{
    auto max_pixels = max_gpu_yuv_upload_pixels();
    if (max_pixels == 0)
        return false;

    auto pixels = static_cast<size_t>(size.width()) * static_cast<size_t>(size.height());
    return pixels > max_pixels;
}

static ErrorOr<NonnullRefPtr<ImmutableBitmap>> create_from_rasterized_yuv(NonnullOwnPtr<YUVData> yuv_data, ColorSpace color_space)
{
    auto size = yuv_data->size();
    auto bitmap = TRY([&]() -> ErrorOr<NonnullRefPtr<Bitmap>> {
        auto scaled_size = scaled_large_video_size(size);
        if (scaled_size.has_value()) {
            auto scaled_bitmap = yuv_data->to_scaled_bitmap(scaled_size.value());
            if (!scaled_bitmap.is_error()) {
                static size_t s_scaled_yuv_video_frame_count { 0 };
                auto scaled_count = ++s_scaled_yuv_video_frame_count;
                if (scaled_count <= 8 || scaled_count % 120 == 0) {
                    dbgln("MUNDO_IMMUTABLE_BITMAP yuv_scaled_raster count={} from={}x{} to={}x{} backend=software",
                        scaled_count, size.width(), size.height(), scaled_size.value().width(), scaled_size.value().height());
                }
                return scaled_bitmap.release_value();
            }

            dbgln("MUNDO_IMMUTABLE_BITMAP yuv_scaled_raster_fallback size={}x{} error={}", size.width(), size.height(), scaled_bitmap.error().string_literal());
        }

        auto bitmap = TRY(yuv_data->to_bitmap());
        return TRY(downscale_large_video_bitmap_if_needed(move(bitmap)));
    }());

    static size_t s_large_raster_video_frame_count { 0 };
    auto count = ++s_large_raster_video_frame_count;
    if (count <= 8 || count % 120 == 0)
        dbgln("MUNDO_IMMUTABLE_BITMAP yuv_large_raster count={} size={}x{} raster={}x{} backend=software", count, size.width(), size.height(), bitmap->width(), bitmap->height());
    return ImmutableBitmap::create(move(bitmap), move(color_space));
}

ErrorOr<NonnullRefPtr<ImmutableBitmap>> ImmutableBitmap::create_from_gpu_yuv(NonnullOwnPtr<YUVData> yuv_data, ColorSpace color_space, NonnullRefPtr<SkiaBackendContext> context)
{
    auto size = yuv_data->size();
    auto* gr_context = context->sk_context();
    if (!gr_context)
        return Error::from_string_literal("No GPU context available for YUV frame");

    if (yuv_data->bit_depth() > 8)
        yuv_data->expand_samples_to_full_16_bit_range();

    context->lock();
    auto sk_image = SkImages::TextureFromYUVAPixmaps(
        gr_context,
        yuv_data->make_pixmaps(),
        skgpu::Mipmapped::kNo,
        false,
        color_space.color_space<sk_sp<SkColorSpace>>());
    context->unlock();

    if (!sk_image)
        return Error::from_string_literal("Failed to upload YUV data");

    static size_t s_gpu_yuv_video_frame_count { 0 };
    auto count = ++s_gpu_yuv_video_frame_count;
    if (count <= 8 || count % 120 == 0)
        dbgln("MUNDO_IMMUTABLE_BITMAP yuv_gpu_texture count={} size={}x{} backend=gpu", count, size.width(), size.height());

    ImmutableBitmapImpl impl {
        .context = context,
        .sk_image = move(sk_image),
        .sk_bitmap = {},
        .bitmap = nullptr,
        .color_space = {},
    };
    return adopt_ref(*new ImmutableBitmap(make<ImmutableBitmapImpl>(move(impl))));
}

ErrorOr<BitmapExportResult> ImmutableBitmap::export_to_byte_buffer(ExportFormat format, int flags, Optional<int> target_width, Optional<int> target_height) const
{
    if (!m_impl->bitmap && SkiaBackendContext::the() && !ensure_sk_image(*SkiaBackendContext::the()))
        return Error::from_string_literal("Failed to create a Skia image for this ImmutableBitmap");

    int width = target_width.value_or(this->width());
    int height = target_height.value_or(this->height());

    if (format == ExportFormat::RGB888 && (width != this->width() || height != this->height())) {
        dbgln("FIXME: Ignoring target width and height because scaling is not implemented for this export format.");
        width = this->width();
        height = this->height();
    }

    Checked<size_t> buffer_pitch = width;
    int number_of_bytes = bytes_per_pixel_for_export_format(format);
    buffer_pitch *= number_of_bytes;
    if (buffer_pitch.has_overflow())
        return Error::from_string_literal("Gfx::ImmutableBitmap::export_to_byte_buffer size overflow");

    if (Checked<size_t>::multiplication_would_overflow(buffer_pitch.value(), height))
        return Error::from_string_literal("Gfx::ImmutableBitmap::export_to_byte_buffer size overflow");

    auto buffer = MUST(ByteBuffer::create_zeroed(buffer_pitch.value() * height));

    if (width > 0 && height > 0) {
        if (m_impl->bitmap && can_export_raster_bitmap_directly(*m_impl->bitmap, format, width, height)) {
            export_raster_bitmap_directly(*m_impl->bitmap, buffer, buffer_pitch.value(), format, flags);
        } else if (format == ExportFormat::RGB888) {
            // 24 bit RGB is not supported by Skia, so we need to handle this format ourselves.
            auto* raw_buffer = buffer.data();
            for (auto y = 0; y < height; y++) {
                auto target_y = flags & ExportFlags::FlipY ? height - y - 1 : y;
                for (auto x = 0; x < width; x++) {
                    auto pixel = get_pixel(x, y);
                    auto buffer_offset = (target_y * buffer_pitch.value()) + (x * 3ull);
                    raw_buffer[buffer_offset + 0] = pixel.red();
                    raw_buffer[buffer_offset + 1] = pixel.green();
                    raw_buffer[buffer_offset + 2] = pixel.blue();
                }
            }
        } else {
            auto skia_format = export_format_to_skia_color_type(format);
            auto color_space = SkColorSpace::MakeSRGB();

            auto image_info = SkImageInfo::Make(width, height, skia_format, flags & ExportFlags::PremultiplyAlpha ? SkAlphaType::kPremul_SkAlphaType : SkAlphaType::kUnpremul_SkAlphaType, color_space);
            auto surface = SkSurfaces::WrapPixels(image_info, buffer.data(), buffer_pitch.value());
            VERIFY(surface);
            auto* surface_canvas = surface->getCanvas();
            auto dst_rect = Gfx::to_skia_rect(Gfx::Rect { 0, 0, width, height });

            if (flags & ExportFlags::FlipY) {
                surface_canvas->translate(0, dst_rect.height());
                surface_canvas->scale(1, -1);
            }

            surface_canvas->drawImageRect(sk_image(), dst_rect, Gfx::to_skia_sampling_options(Gfx::ScalingMode::NearestNeighbor));
        }
    } else {
        VERIFY(buffer.is_empty());
    }

    return BitmapExportResult {
        .buffer = move(buffer),
        .width = width,
        .height = height,
    };
}

RefPtr<Gfx::Bitmap const> ImmutableBitmap::bitmap() const
{
    if (!m_impl->bitmap && m_impl->sk_image) {
        auto bitmap = MUST(Gfx::Bitmap::create(Gfx::BitmapFormat::BGRA8888, Gfx::AlphaType::Premultiplied, { m_impl->sk_image->width(), m_impl->sk_image->height() }));
        auto image_info = SkImageInfo::Make(bitmap->width(), bitmap->height(), kBGRA_8888_SkColorType, kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
        SkPixmap pixmap(image_info, bitmap->begin(), bitmap->pitch());
        if (m_impl->context)
            m_impl->context->lock();
        m_impl->sk_image->readPixels(pixmap, 0, 0);
        if (m_impl->context)
            m_impl->context->unlock();
        m_impl->bitmap = move(bitmap);
    }
    return m_impl->bitmap;
}

ErrorOr<NonnullRefPtr<ImmutableBitmap>> ImmutableBitmap::create_from_yuv(NonnullOwnPtr<YUVData> yuv_data)
{
    auto color_space = TRY(ColorSpace::from_cicp(yuv_data->cicp()));
    auto size = yuv_data->size();

    auto context = SkiaBackendContext::the();
    auto backend = yuv_frame_backend();

    if (backend == YUVFrameBackend::Software)
        return create_from_rasterized_yuv(move(yuv_data), move(color_space));

    if (backend == YUVFrameBackend::Gpu) {
        if (!context)
            return create_from_rasterized_yuv(move(yuv_data), move(color_space));
        if (yuv_data->prefers_gpu_upload() && exceeds_gpu_yuv_upload_size_limit(size)) {
            static size_t s_size_limited_gpu_yuv_video_frame_count { 0 };
            auto count = ++s_size_limited_gpu_yuv_video_frame_count;
            if (count <= 8 || count % 120 == 0) {
                dbgln("MUNDO_IMMUTABLE_BITMAP yuv_gpu_texture_skipped count={} size={}x{} reason=gpu_yuv_size_limit backend=gpu max_pixels={}",
                    count, size.width(), size.height(), max_gpu_yuv_upload_pixels());
            }
            return create_from_rasterized_yuv(move(yuv_data), move(color_space));
        }
        if (is_large_video_frame(size) && !yuv_data->prefers_gpu_upload()) {
            static size_t s_unpreferred_gpu_yuv_video_frame_count { 0 };
            auto count = ++s_unpreferred_gpu_yuv_video_frame_count;
            if (count <= 8 || count % 120 == 0) {
                dbgln("MUNDO_IMMUTABLE_BITMAP yuv_gpu_texture_skipped count={} size={}x{} reason=large_software_frame backend=gpu",
                    count, size.width(), size.height());
            }
            return create_from_rasterized_yuv(move(yuv_data), move(color_space));
        }
        return create_from_gpu_yuv(move(yuv_data), move(color_space), *context);
    }

    // Large video frames are commonly uploaded back into WebGL by sites using
    // requestVideoFrameCallback. The current GPU-backed SkImage path can force
    // expensive readbacks there, so auto mode keeps large live-video frames on
    // the software/scaled path until a true hardware/zero-copy backend exists.
    if (is_large_video_frame(size) || !context)
        return create_from_rasterized_yuv(move(yuv_data), move(color_space));

    return create_from_gpu_yuv(move(yuv_data), move(color_space), *context);
}

bool ImmutableBitmap::ensure_sk_image(SkiaBackendContext& context) const
{
    if (m_impl->context) {
        VERIFY(m_impl->context.ptr() == &context);
        return true;
    }

    context.lock();
    ScopeGuard unlock_guard = [&context] {
        context.unlock();
    };

    auto* gr_context = context.sk_context();

    VERIFY(m_impl->sk_image);
    if (!gr_context)
        return true; // No GPU, but raster image is still usable
    auto gpu_image = SkImages::TextureFromImage(gr_context, m_impl->sk_image.get(), skgpu::Mipmapped::kNo, skgpu::Budgeted::kYes);
    if (gpu_image) {
        m_impl->context = context;
        m_impl->sk_image = move(gpu_image);
    }
    return true;
}

Color ImmutableBitmap::get_pixel(int x, int y) const
{
    return m_impl->bitmap->get_pixel(x, y);
}

static SkAlphaType to_skia_alpha_type(Gfx::AlphaType alpha_type)
{
    switch (alpha_type) {
    case AlphaType::Premultiplied:
        return kPremul_SkAlphaType;
    case AlphaType::Unpremultiplied:
        return kUnpremul_SkAlphaType;
    default:
        VERIFY_NOT_REACHED();
    }
}

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create(NonnullRefPtr<Bitmap const> const& bitmap, ColorSpace color_space)
{
    SkBitmap sk_bitmap;
    auto info = SkImageInfo::Make(bitmap->width(), bitmap->height(), to_skia_color_type(bitmap->format()), to_skia_alpha_type(bitmap->alpha_type()), color_space.color_space<sk_sp<SkColorSpace>>());
    sk_bitmap.installPixels(info, const_cast<void*>(static_cast<void const*>(bitmap->scanline(0))), bitmap->pitch());
    sk_bitmap.setImmutable();
    auto sk_image = sk_bitmap.asImage();

    ImmutableBitmapImpl impl {
        .context = nullptr,
        .sk_image = move(sk_image),
        .sk_bitmap = move(sk_bitmap),
        .bitmap = bitmap,
        .color_space = move(color_space),
    };
    return adopt_ref(*new ImmutableBitmap(make<ImmutableBitmapImpl>(move(impl))));
}

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create(NonnullRefPtr<Bitmap const> const& bitmap, AlphaType alpha_type, ColorSpace color_space)
{
    // Convert the source bitmap to the right alpha type on a mismatch. We want to do this when converting from a
    // Bitmap to an ImmutableBitmap, since at that point we usually know the right alpha type to use in context.
    auto converted_bitmap = [&] -> NonnullRefPtr<Bitmap const> {
        if (bitmap->alpha_type() == alpha_type)
            return bitmap;
        auto new_bitmap = MUST(bitmap->clone());
        new_bitmap->set_alpha_type_destructive(alpha_type);
        return new_bitmap;
    }();

    return create(converted_bitmap, move(color_space));
}

NonnullRefPtr<ImmutableBitmap> ImmutableBitmap::create_snapshot_from_painting_surface(NonnullRefPtr<PaintingSurface> const& painting_surface)
{
    painting_surface->lock_context();
    auto sk_image = painting_surface->sk_image_snapshot<sk_sp<SkImage>>();
    painting_surface->unlock_context();

    ImmutableBitmapImpl impl {
        .context = painting_surface->skia_backend_context(),
        .sk_image = move(sk_image),
        .sk_bitmap = {},
        .bitmap = nullptr,
        .color_space = {},
    };
    return adopt_ref(*new ImmutableBitmap(make<ImmutableBitmapImpl>(move(impl))));
}

ImmutableBitmap::ImmutableBitmap(NonnullOwnPtr<ImmutableBitmapImpl>&& impl)
    : m_impl(move(impl))
{
}

ImmutableBitmap::~ImmutableBitmap()
{
    lock_context();
    m_impl->sk_image = nullptr;
    unlock_context();
}

void ImmutableBitmap::lock_context()
{
    auto& context = m_impl->context;
    if (context)
        context->lock();
}

void ImmutableBitmap::unlock_context()
{
    auto& context = m_impl->context;
    if (context)
        context->unlock();
}

}
