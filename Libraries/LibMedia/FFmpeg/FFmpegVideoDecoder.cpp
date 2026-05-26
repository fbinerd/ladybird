/*
 * Copyright (c) 2024, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibGfx/Bitmap.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/YUVData.h>
#include <LibMedia/VideoFrame.h>
#include <LibThreading/ConditionVariable.h>
#include <LibThreading/Mutex.h>
#include <LibThreading/ThreadPool.h>
#include <AK/Atomic.h>
#include <AK/ByteBuffer.h>
#include <AK/Time.h>

#include "FFmpegHelpers.h"
#include "FFmpegVideoDecoder.h"

#include <stdlib.h>
#include <string.h>
#if defined(__linux__)
#    include <dlfcn.h>
#endif

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
#include <libavutil/log.h>
}

#if defined(__linux__)
#    include <ffnvcodec/dynlink_cuda.h>
typedef CUresult CUDAAPI tcuGraphicsGLRegisterBuffer(CUgraphicsResource* pCudaResource, GLuint buffer, unsigned int Flags);
#endif

namespace Media::FFmpeg {

enum class VideoDecoderBackend {
    Auto,
    Software,
    Vaapi,
    Nvdec,
};

static VideoDecoderBackend requested_video_decoder_backend()
{
    auto const* raw_value = getenv("MUNDO_VIDEO_DECODER_BACKEND");
    if (!raw_value)
        return VideoDecoderBackend::Auto;

    if (!strcmp(raw_value, "software") || !strcmp(raw_value, "cpu"))
        return VideoDecoderBackend::Software;
    if (!strcmp(raw_value, "vaapi"))
        return VideoDecoderBackend::Vaapi;
    if (!strcmp(raw_value, "nvdec") || !strcmp(raw_value, "cuda"))
        return VideoDecoderBackend::Nvdec;

    return VideoDecoderBackend::Auto;
}

static StringView video_decoder_backend_name(VideoDecoderBackend backend)
{
    switch (backend) {
    case VideoDecoderBackend::Auto:
        return "auto"sv;
    case VideoDecoderBackend::Software:
        return "software"sv;
    case VideoDecoderBackend::Vaapi:
        return "vaapi"sv;
    case VideoDecoderBackend::Nvdec:
        return "nvdec"sv;
    }
    VERIFY_NOT_REACHED();
}

static AVHWDeviceType hw_device_type_for_backend(VideoDecoderBackend backend)
{
    switch (backend) {
    case VideoDecoderBackend::Vaapi:
        return AV_HWDEVICE_TYPE_VAAPI;
    case VideoDecoderBackend::Nvdec:
        return AV_HWDEVICE_TYPE_CUDA;
    case VideoDecoderBackend::Auto:
    case VideoDecoderBackend::Software:
        return AV_HWDEVICE_TYPE_NONE;
    }
    VERIFY_NOT_REACHED();
}

static AVPixelFormat hw_pixel_format_for_backend(VideoDecoderBackend backend)
{
    switch (backend) {
    case VideoDecoderBackend::Vaapi:
        return AV_PIX_FMT_VAAPI;
    case VideoDecoderBackend::Nvdec:
        return AV_PIX_FMT_CUDA;
    case VideoDecoderBackend::Auto:
    case VideoDecoderBackend::Software:
        return AV_PIX_FMT_NONE;
    }
    VERIFY_NOT_REACHED();
}

static bool codec_has_hw_config(AVCodec const* codec, VideoDecoderBackend backend)
{
    auto device_type = hw_device_type_for_backend(backend);
    auto pixel_format = hw_pixel_format_for_backend(backend);
    if (device_type == AV_HWDEVICE_TYPE_NONE || pixel_format == AV_PIX_FMT_NONE)
        return false;

    for (int index = 0;; ++index) {
        auto const* config = avcodec_get_hw_config(codec, index);
        if (!config)
            return false;

        if (config->device_type == device_type && config->pix_fmt == pixel_format && (config->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX))
            return true;
    }
}

static bool can_create_hw_device(VideoDecoderBackend backend)
{
    auto device_type = hw_device_type_for_backend(backend);
    if (device_type == AV_HWDEVICE_TYPE_NONE)
        return false;

    AVBufferRef* device_context = nullptr;
    auto result = av_hwdevice_ctx_create(&device_context, device_type, nullptr, nullptr, 0);
    if (device_context)
        av_buffer_unref(&device_context);
    return result >= 0;
}

static char const* pixel_format_name(AVPixelFormat format)
{
    auto const* name = av_get_pix_fmt_name(format);
    return name ? name : "unknown";
}

static char const* codec_profile_name(AVCodecContext const* codec_context)
{
    auto const* name = avcodec_profile_name(codec_context->codec_id, codec_context->profile);
    return name ? name : "unknown";
}

static bool is_known_unsupported_nvdec_h264_resolution(AVCodecContext const* codec_context)
{
    if (codec_context->codec_id != AV_CODEC_ID_H264)
        return false;

    // NVIDIA's public NVDEC capability tables list H.264 support up to 4096x4096
    // and level 5.1. Some sites serve 4320-wide H.264 level 5.2 streams.
    return codec_context->width > 4096 || codec_context->height > 4096 || codec_context->level > 51;
}

static bool should_keep_small_frame_on_software(AVCodecContext const* codec_context)
{
    auto const* raw_min_pixels = getenv("MUNDO_VIDEO_NVDEC_MIN_PIXELS");
    if (raw_min_pixels && (!strcmp(raw_min_pixels, "0") || !strcmp(raw_min_pixels, "false") || !strcmp(raw_min_pixels, "no") || !strcmp(raw_min_pixels, "off")))
        return false;

    auto min_pixels = raw_min_pixels ? strtoull(raw_min_pixels, nullptr, 10) : 640ull * 360ull;
    if (min_pixels == 0)
        return false;

    if (codec_context->width <= 0 || codec_context->height <= 0)
        return false;

    return static_cast<unsigned long long>(codec_context->width) * static_cast<unsigned long long>(codec_context->height) < min_pixels;
}

static void log_hwaccel_probe(AVCodec const* codec, CodecID codec_id, VideoDecoderBackend backend)
{
    if (backend == VideoDecoderBackend::Auto || backend == VideoDecoderBackend::Software)
        return;

    auto ffmpeg_has_config = codec_has_hw_config(codec, backend);
    auto device_available = ffmpeg_has_config && can_create_hw_device(backend);
    dbgln("MUNDO_MEDIA_FFMPEG hwaccel_probe backend={} codec={} ffmpeg_config={} device_available={}",
        video_decoder_backend_name(backend), codec_id, ffmpeg_has_config, device_available);
}

static void log_video_decoder_backend_probe(AVCodec const* codec, CodecID codec_id)
{
    auto requested_backend = requested_video_decoder_backend();
    auto active_backend = VideoDecoderBackend::Software;
    if (requested_backend == VideoDecoderBackend::Nvdec && codec_has_hw_config(codec, VideoDecoderBackend::Nvdec) && can_create_hw_device(VideoDecoderBackend::Nvdec))
        active_backend = VideoDecoderBackend::Nvdec;
    else if (requested_backend == VideoDecoderBackend::Auto && codec_has_hw_config(codec, VideoDecoderBackend::Nvdec) && can_create_hw_device(VideoDecoderBackend::Nvdec))
        active_backend = VideoDecoderBackend::Nvdec;

    dbgln("MUNDO_MEDIA_FFMPEG video_decoder_backend requested={} codec={} active={}",
        video_decoder_backend_name(requested_backend), codec_id, video_decoder_backend_name(active_backend));

    if (requested_backend == VideoDecoderBackend::Software)
        return;

    if (requested_backend == VideoDecoderBackend::Auto) {
        log_hwaccel_probe(codec, codec_id, VideoDecoderBackend::Nvdec);
        log_hwaccel_probe(codec, codec_id, VideoDecoderBackend::Vaapi);
        return;
    }

    log_hwaccel_probe(codec, codec_id, requested_backend);
}

static AVPixelFormat negotiate_output_format(AVCodecContext* codec_context, AVPixelFormat const* formats)
{
    if (codec_context->hw_device_ctx) {
        for (auto const* format = formats; *format >= 0; ++format) {
            dbgln("MUNDO_MEDIA_FFMPEG hwaccel_format_offer codec={} profile={} level={} size={}x{} offered={} sw_pix_fmt={}",
                avcodec_get_name(codec_context->codec_id),
                codec_profile_name(codec_context),
                codec_context->level,
                codec_context->width,
                codec_context->height,
                pixel_format_name(*format),
                pixel_format_name(codec_context->sw_pix_fmt));
        }
        auto skipped_hardware_format = false;
        if (is_known_unsupported_nvdec_h264_resolution(codec_context)) {
            skipped_hardware_format = true;
            dbgln("MUNDO_MEDIA_FFMPEG hwaccel_format selected=software reason=nvdec_h264_resolution_or_level_limit codec={} profile={} level={} size={}x{} sw_pix_fmt={}",
                avcodec_get_name(codec_context->codec_id),
                codec_profile_name(codec_context),
                codec_context->level,
                codec_context->width,
                codec_context->height,
                pixel_format_name(codec_context->sw_pix_fmt));
        } else if (should_keep_small_frame_on_software(codec_context)) {
            skipped_hardware_format = true;
            dbgln("MUNDO_MEDIA_FFMPEG hwaccel_format selected=software reason=small_frame codec={} profile={} level={} size={}x{} sw_pix_fmt={}",
                avcodec_get_name(codec_context->codec_id),
                codec_profile_name(codec_context),
                codec_context->level,
                codec_context->width,
                codec_context->height,
                pixel_format_name(codec_context->sw_pix_fmt));
        } else {
            for (auto const* format = formats; *format >= 0; ++format) {
                if (*format == AV_PIX_FMT_CUDA) {
                    dbgln("MUNDO_MEDIA_FFMPEG hwaccel_format selected=cuda codec={} profile={} level={} size={}x{} sw_pix_fmt={}",
                        avcodec_get_name(codec_context->codec_id),
                        codec_profile_name(codec_context),
                        codec_context->level,
                        codec_context->width,
                        codec_context->height,
                        pixel_format_name(codec_context->sw_pix_fmt));
                    return *format;
                }
            }
        }
        if (!skipped_hardware_format) {
            dbgln("MUNDO_MEDIA_FFMPEG hwaccel_format selected=software reason=cuda_not_offered codec={} profile={} level={} size={}x{} sw_pix_fmt={}",
                avcodec_get_name(codec_context->codec_id),
                codec_profile_name(codec_context),
                codec_context->level,
                codec_context->width,
                codec_context->height,
                pixel_format_name(codec_context->sw_pix_fmt));
        }
    }

    while (*formats >= 0) {
        switch (*formats) {
        case AV_PIX_FMT_NV12:
        case AV_PIX_FMT_P010LE:
        case AV_PIX_FMT_P016LE:
        case AV_PIX_FMT_YUV420P:
        case AV_PIX_FMT_YUV420P10:
        case AV_PIX_FMT_YUV420P12:
        case AV_PIX_FMT_YUV422P:
        case AV_PIX_FMT_YUV422P10:
        case AV_PIX_FMT_YUV422P12:
        case AV_PIX_FMT_YUV444P:
        case AV_PIX_FMT_YUV444P10:
        case AV_PIX_FMT_YUV444P12:
        case AV_PIX_FMT_YUVJ420P:
        case AV_PIX_FMT_YUVJ422P:
        case AV_PIX_FMT_YUVJ444P:
            return *formats;
        default:
            break;
        }
        formats++;
    }
    return AV_PIX_FMT_NONE;
}

static bool should_try_nvdec(AVCodec const* codec)
{
    auto requested_backend = requested_video_decoder_backend();
    if (requested_backend != VideoDecoderBackend::Auto && requested_backend != VideoDecoderBackend::Nvdec)
        return false;

    return codec_has_hw_config(codec, VideoDecoderBackend::Nvdec) && can_create_hw_device(VideoDecoderBackend::Nvdec);
}

static void quiet_ffmpeg_logs_for_nvdec()
{
    static bool s_did_quiet_logs { false };
    if (s_did_quiet_logs)
        return;

    // The CUDA decoder can emit malformed teardown diagnostics after the
    // WebContent process is already shutting down. Keep our own structured
    // MUNDO_MEDIA_FFMPEG logs, but avoid routing those FFmpeg internals through
    // av_log_default_callback.
    av_log_set_level(AV_LOG_FATAL);
    s_did_quiet_logs = true;
}

static bool is_hardware_frame(AVFrame const* frame)
{
    return frame->format == AV_PIX_FMT_CUDA;
}

struct HardwareTransferTiming {
    bool transferred_from_hardware { false };
    i64 transfer_microseconds { 0 };
};

static HardwareVideoFrameDescriptor hardware_descriptor_for_cuda_frame(AVCodecContext const* codec_context, AVFrame const* frame, u8 bit_depth)
{
    static u64 s_hardware_video_frame_id { 0 };

    HardwareVideoFrameDescriptor descriptor;
    descriptor.backend = HardwareVideoFrameBackend::Cuda;
    descriptor.frame_id = ++s_hardware_video_frame_id;
    descriptor.size = Gfx::Size<u32> { frame->width, frame->height };
    descriptor.hardware_format = frame->format;
    descriptor.software_format = codec_context->sw_pix_fmt;
    descriptor.bit_depth = bit_depth;
    descriptor.zero_copy_capable = true;
    descriptor.requires_cpu_transfer = true;
    return descriptor;
}

static void log_software_frame_while_hwaccel_requested(AVCodecContext const* codec_context, AVFrame const* frame)
{
    if (!codec_context->hw_device_ctx)
        return;

    static size_t s_hw_fallback_frame_count { 0 };
    auto count = ++s_hw_fallback_frame_count;
    if (count <= 8 || count % 120 == 0) {
        dbgln("MUNDO_MEDIA_FFMPEG hwaccel_fallback_frame count={} codec={} profile={} level={} frame_format={} sw_pix_fmt={} size={}x{}",
            count,
            avcodec_get_name(codec_context->codec_id),
            codec_profile_name(codec_context),
            codec_context->level,
            pixel_format_name(static_cast<AVPixelFormat>(frame->format)),
            pixel_format_name(codec_context->sw_pix_fmt),
            frame->width,
            frame->height);
    }
}

static DecoderErrorOr<AVFrame*> software_frame_for_decoded_frame(AVFrame* frame, AVFrame* transfer_frame, HardwareTransferTiming& timing)
{
    if (!is_hardware_frame(frame))
        return frame;

    av_frame_unref(transfer_frame);
    auto transfer_start = MonotonicTime::now();
    auto result = av_hwframe_transfer_data(transfer_frame, frame, 0);
    timing.transfer_microseconds = (MonotonicTime::now() - transfer_start).to_microseconds();
    if (result < 0)
        return DecoderError::format(DecoderErrorCategory::Unknown, "Failed to transfer FFmpeg hardware frame to CPU with code {:x}", result);

    result = av_frame_copy_props(transfer_frame, frame);
    if (result < 0)
        return DecoderError::format(DecoderErrorCategory::Unknown, "Failed to copy FFmpeg hardware frame properties with code {:x}", result);

    timing.transferred_from_hardware = true;

    static size_t s_hw_transfer_frame_count { 0 };
    auto count = ++s_hw_transfer_frame_count;
    if (count <= 8 || count % 120 == 0) {
        dbgln("MUNDO_MEDIA_FFMPEG hwaccel_transfer backend=nvdec count={} hw_format={} sw_format={} size={}x{} transfer_us={}",
            count,
            av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)),
            av_get_pix_fmt_name(static_cast<AVPixelFormat>(transfer_frame->format)),
            transfer_frame->width,
            transfer_frame->height,
            timing.transfer_microseconds);
    }

    return transfer_frame;
}

static size_t bit_depth_for_pixel_format(AVPixelFormat format)
{
    switch (format) {
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUVJ420P:
    case AV_PIX_FMT_YUVJ422P:
    case AV_PIX_FMT_YUVJ444P:
        return 8;
    case AV_PIX_FMT_P010LE:
    case AV_PIX_FMT_YUV420P10:
    case AV_PIX_FMT_YUV422P10:
    case AV_PIX_FMT_YUV444P10:
        return 10;
    case AV_PIX_FMT_P016LE:
    case AV_PIX_FMT_YUV420P12:
    case AV_PIX_FMT_YUV422P12:
    case AV_PIX_FMT_YUV444P12:
        return 12;
    default:
        VERIFY_NOT_REACHED();
    }
}

static Subsampling subsampling_for_pixel_format(AVPixelFormat format)
{
    switch (format) {
    case AV_PIX_FMT_NV12:
    case AV_PIX_FMT_P010LE:
    case AV_PIX_FMT_P016LE:
    case AV_PIX_FMT_YUV420P:
    case AV_PIX_FMT_YUV420P10:
    case AV_PIX_FMT_YUV420P12:
    case AV_PIX_FMT_YUVJ420P:
        return { true, true };
    case AV_PIX_FMT_YUV422P:
    case AV_PIX_FMT_YUV422P10:
    case AV_PIX_FMT_YUV422P12:
    case AV_PIX_FMT_YUVJ422P:
        return { true, false };
    case AV_PIX_FMT_YUV444P:
    case AV_PIX_FMT_YUV444P10:
    case AV_PIX_FMT_YUV444P12:
    case AV_PIX_FMT_YUVJ444P:
        return { false, false };
    default:
        VERIFY_NOT_REACHED();
    }
}

static bool is_semiplanar_pixel_format(AVPixelFormat format)
{
    return format == AV_PIX_FMT_NV12 || format == AV_PIX_FMT_P010LE || format == AV_PIX_FMT_P016LE;
}

static int max_nvdec_video_raster_width()
{
    auto const* raw_value = getenv("MUNDO_VIDEO_MAX_RASTER_WIDTH");
    if (!raw_value) {
        raw_value = getenv("MUNDO_VIDEO_MAX_RASTER_WIDTH_NVDEC");
        if (!raw_value)
            return 0;
    }

    auto value = atoi(raw_value);
    if (value <= 0)
        return 0;

    return max(value, 320);
}

static bool direct_nv12_rgba_bitmap_enabled()
{
    auto const* raw_value = getenv("MUNDO_VIDEO_DIRECT_NV12_RGBA");
    if (!raw_value)
        return true;

    return strcmp(raw_value, "0") && strcmp(raw_value, "no") && strcmp(raw_value, "false");
}

static bool lazy_nv12_rgba_bitmap_enabled()
{
    auto const* raw_value = getenv("MUNDO_VIDEO_LAZY_NV12_RGBA");
    if (!raw_value)
        return true;

    return strcmp(raw_value, "0") && strcmp(raw_value, "no") && strcmp(raw_value, "false");
}

static bool parallel_nv12_frame_copy_enabled()
{
    auto const* raw_value = getenv("MUNDO_VIDEO_PARALLEL_NV12_COPY");
    if (!raw_value)
        return false;

    return strcmp(raw_value, "0") && strcmp(raw_value, "no") && strcmp(raw_value, "false");
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

static bool should_use_gpu_yuv_for_nv12_frame(int width, int height)
{
    auto const* raw_backend = getenv("MUNDO_VIDEO_BACKEND");
    if (!raw_backend || (strcmp(raw_backend, "gpu") && strcmp(raw_backend, "hardware")))
        return false;

    auto max_pixels = max_gpu_yuv_upload_pixels();
    if (max_pixels == 0)
        return true;

    auto pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    return pixels <= max_pixels;
}

static Gfx::IntSize target_size_for_nvdec_dimensions(int width, int height)
{
    auto max_width = max_nvdec_video_raster_width();
    if (max_width == 0 || width <= max_width)
        return { width, height };

    return {
        max_width,
        max(1, static_cast<int>((static_cast<i64>(height) * max_width) / width))
    };
}

static Gfx::IntSize target_size_for_nvdec_frame(AVFrame const* frame)
{
    return target_size_for_nvdec_dimensions(frame->width, frame->height);
}

static u8 clamp_to_u8(int value)
{
    if (value < 0)
        return 0;
    if (value > 255)
        return 255;
    return static_cast<u8>(value);
}

struct YUVToRGBCoefficients {
    bool full_range { false };
    int y_multiplier { 298 };
    int y_offset { 16 };
    int r_v { 459 };
    int g_u { 55 };
    int g_v { 136 };
    int b_u { 541 };
};

static YUVToRGBCoefficients coefficients_for_cicp(CodingIndependentCodePoints const& cicp)
{
    YUVToRGBCoefficients coefficients;
    coefficients.full_range = cicp.video_full_range_flag() == VideoFullRangeFlag::Full;

    if (coefficients.full_range) {
        coefficients.y_multiplier = 256;
        coefficients.y_offset = 0;
        switch (cicp.matrix_coefficients()) {
        case MatrixCoefficients::BT601:
        case MatrixCoefficients::BT470BG:
            coefficients.r_v = 359;
            coefficients.g_u = 88;
            coefficients.g_v = 183;
            coefficients.b_u = 454;
            break;
        case MatrixCoefficients::BT2020NonConstantLuminance:
        case MatrixCoefficients::BT2020ConstantLuminance:
            coefficients.r_v = 377;
            coefficients.g_u = 42;
            coefficients.g_v = 146;
            coefficients.b_u = 482;
            break;
        case MatrixCoefficients::BT709:
        case MatrixCoefficients::Unspecified:
        default:
            coefficients.r_v = 403;
            coefficients.g_u = 48;
            coefficients.g_v = 120;
            coefficients.b_u = 475;
            break;
        }
    } else {
        switch (cicp.matrix_coefficients()) {
        case MatrixCoefficients::BT601:
        case MatrixCoefficients::BT470BG:
            coefficients.r_v = 409;
            coefficients.g_u = 100;
            coefficients.g_v = 208;
            coefficients.b_u = 516;
            break;
        case MatrixCoefficients::BT2020NonConstantLuminance:
        case MatrixCoefficients::BT2020ConstantLuminance:
            coefficients.r_v = 430;
            coefficients.g_u = 48;
            coefficients.g_v = 167;
            coefficients.b_u = 548;
            break;
        case MatrixCoefficients::BT709:
        case MatrixCoefficients::Unspecified:
        default:
            coefficients.r_v = 459;
            coefficients.g_u = 55;
            coefficients.g_v = 136;
            coefficients.b_u = 541;
            break;
        }
    }

    return coefficients;
}

struct YUVToRGBLookupTables {
    int y[256] {};
    int r_from_v[256] {};
    int g_from_u[256] {};
    int g_from_v[256] {};
    int b_from_u[256] {};
};

static YUVToRGBLookupTables make_yuv_to_rgb_lookup_tables(YUVToRGBCoefficients const& coefficients)
{
    YUVToRGBLookupTables tables;
    for (auto value = 0; value < 256; ++value) {
        auto y_value = max(0, value - coefficients.y_offset);
        auto chroma_offset = value - 128;
        tables.y[value] = coefficients.y_multiplier * y_value;
        tables.r_from_v[value] = coefficients.r_v * chroma_offset;
        tables.g_from_u[value] = coefficients.g_u * chroma_offset;
        tables.g_from_v[value] = coefficients.g_v * chroma_offset;
        tables.b_from_u[value] = coefficients.b_u * chroma_offset;
    }
    return tables;
}

static void write_yuv_to_rgba(YUVToRGBLookupTables const& tables, u8 y, int r_uv, int g_uv, int b_uv, u8* dst)
{
    auto y_value = tables.y[y];
    auto r = (y_value + r_uv + 128) >> 8;
    auto g = (y_value - g_uv + 128) >> 8;
    auto b = (y_value + b_uv + 128) >> 8;

    dst[0] = clamp_to_u8(r);
    dst[1] = clamp_to_u8(g);
    dst[2] = clamp_to_u8(b);
    dst[3] = 255;
}

static void convert_yuv_to_rgba(YUVToRGBLookupTables const& tables, u8 y, u8 u, u8 v, u8* dst)
{
    auto r_uv = tables.r_from_v[v];
    auto g_uv = tables.g_from_u[u] + tables.g_from_v[v];
    auto b_uv = tables.b_from_u[u];
    write_yuv_to_rgba(tables, y, r_uv, g_uv, b_uv, dst);
}

static void convert_nv12_row_to_rgba(YUVToRGBLookupTables const& tables, u8 const* y_row, u8 const* uv_row, u8* dst_row, u32 width)
{
    u32 col = 0;
    for (; col + 1 < width; col += 2) {
        auto u = uv_row[col];
        auto v = uv_row[col + 1];
        auto r_uv = tables.r_from_v[v];
        auto g_uv = tables.g_from_u[u] + tables.g_from_v[v];
        auto b_uv = tables.b_from_u[u];
        write_yuv_to_rgba(tables, y_row[col], r_uv, g_uv, b_uv, dst_row + col * 4);
        write_yuv_to_rgba(tables, y_row[col + 1], r_uv, g_uv, b_uv, dst_row + (col + 1) * 4);
    }

    if (col < width) {
        auto uv_x = (col / 2) * 2;
        convert_yuv_to_rgba(tables, y_row[col], uv_row[uv_x], uv_row[uv_x + 1], dst_row + col * 4);
    }
}

static bool parallel_direct_nv12_rgba_enabled()
{
    auto const* raw_value = getenv("MUNDO_VIDEO_PARALLEL_NV12_RGBA");
    if (!raw_value)
        return true;

    return strcmp(raw_value, "0") && strcmp(raw_value, "no") && strcmp(raw_value, "false");
}

static size_t direct_nv12_parallel_job_count(u32 width, u32 height)
{
    if (!parallel_direct_nv12_rgba_enabled())
        return 1;

    auto pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    if (pixels < 1920 * 1080)
        return 1;

    // LibThreading::ThreadPool currently has four workers. Keeping this bounded
    // avoids flooding the shared pool while still moving the large VR conversion
    // path off a single decoder thread.
    auto available_jobs = min(Core::System::hardware_concurrency(), static_cast<size_t>(4));
    return max(static_cast<size_t>(1), min(available_jobs, static_cast<size_t>(height)));
}

template<typename Callback>
static void parallel_for_nv12_rows(u32 height, size_t job_count, Callback callback)
{
    if (job_count <= 1) {
        callback(0, height);
        return;
    }

    struct WorkState {
        Threading::Mutex mutex;
        Threading::ConditionVariable condition { mutex };
        size_t remaining_jobs { 0 };
    };

    WorkState state;
    {
        Threading::MutexLocker locker(state.mutex);
        state.remaining_jobs = job_count;
    }

    auto rows_per_job = (height + job_count - 1) / job_count;
    rows_per_job = (rows_per_job + 1) & ~static_cast<size_t>(1);
    for (size_t job_index = 0; job_index < job_count; ++job_index) {
        auto start_row = min(static_cast<u32>(job_index * rows_per_job), height);
        auto end_row = min(static_cast<u32>((job_index + 1) * rows_per_job), height);
        if (start_row == end_row) {
            Threading::MutexLocker locker(state.mutex);
            state.remaining_jobs--;
            continue;
        }

        Threading::ThreadPool::the().submit([&state, &callback, start_row, end_row] {
            callback(start_row, end_row);

            Threading::MutexLocker locker(state.mutex);
            state.remaining_jobs--;
            state.condition.broadcast();
        });
    }

    Threading::MutexLocker locker(state.mutex);
    state.condition.wait_while([&state] {
        return state.remaining_jobs > 0;
    });
}

static ErrorOr<NonnullRefPtr<Gfx::ImmutableBitmap>> create_bitmap_directly_from_nv12_frame(AVFrame const* frame, CodingIndependentCodePoints const& cicp)
{
    VERIFY(frame->format == AV_PIX_FMT_NV12);
    if (frame->linesize[0] < 0 || frame->linesize[1] < 0)
        return Error::from_string_literal("Reversed NV12 scanlines are not supported");
    if (!frame->data[0] || !frame->data[1])
        return Error::from_string_literal("NV12 frame had missing planes");
    if (cicp.matrix_coefficients() == MatrixCoefficients::Identity)
        return Error::from_string_literal("NV12 direct conversion does not support identity matrix");

    auto target_size = target_size_for_nvdec_frame(frame);
    auto bitmap = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::RGBA8888, Gfx::AlphaType::Premultiplied, target_size));

    auto source_width = static_cast<u32>(frame->width);
    auto source_height = static_cast<u32>(frame->height);
    auto target_width = static_cast<u32>(target_size.width());
    auto target_height = static_cast<u32>(target_size.height());
    auto coefficients = coefficients_for_cicp(cicp);
    auto lookup_tables = make_yuv_to_rgb_lookup_tables(coefficients);
    auto job_count = direct_nv12_parallel_job_count(target_width, target_height);

    if (target_width == source_width && target_height == source_height) {
        parallel_for_nv12_rows(source_height, job_count, [&](u32 start_row, u32 end_row) {
            for (u32 row = start_row; row < end_row; row += 2) {
                auto const* y_row = frame->data[0] + row * frame->linesize[0];
                auto const* uv_row = frame->data[1] + (row / 2) * frame->linesize[1];
                auto* dst_row = bitmap->scanline_u8(row);
                convert_nv12_row_to_rgba(lookup_tables, y_row, uv_row, dst_row, source_width);

                if (row + 1 < source_height && row + 1 < end_row) {
                    auto const* next_y_row = frame->data[0] + (row + 1) * frame->linesize[0];
                    auto* next_dst_row = bitmap->scanline_u8(row + 1);
                    convert_nv12_row_to_rgba(lookup_tables, next_y_row, uv_row, next_dst_row, source_width);
                }
            }
        });
    } else {
        parallel_for_nv12_rows(target_height, job_count, [&](u32 start_row, u32 end_row) {
            for (u32 row = start_row; row < end_row; ++row) {
                auto source_y = min((static_cast<u64>(row) * source_height) / target_height, static_cast<u64>(source_height - 1));
                auto const* y_row = frame->data[0] + source_y * frame->linesize[0];
                auto const* uv_row = frame->data[1] + (source_y / 2) * frame->linesize[1];
                auto* dst_row = bitmap->scanline_u8(row);

                for (u32 col = 0; col < target_width; ++col) {
                    auto source_x = min((static_cast<u64>(col) * source_width) / target_width, static_cast<u64>(source_width - 1));
                    auto uv_x = (source_x / 2) * 2;
                    convert_yuv_to_rgba(lookup_tables, y_row[source_x], uv_row[uv_x], uv_row[uv_x + 1], dst_row + col * 4);
                }
            }
        });
    }

    static size_t s_direct_nv12_frame_count { 0 };
    auto count = ++s_direct_nv12_frame_count;
    if (count <= 8 || count % 120 == 0) {
        dbgln("MUNDO_MEDIA_FFMPEG direct_nv12_bitmap count={} from={}x{} to={}x{} jobs={}",
            count,
            frame->width,
            frame->height,
            target_size.width(),
            target_size.height(),
            job_count);
    }

    return Gfx::ImmutableBitmap::create(move(bitmap));
}

static ErrorOr<NonnullRefPtr<NV12VideoFrameData>> copy_nv12_frame_data(AVFrame const* frame, CodingIndependentCodePoints const& cicp)
{
    auto copy_start = MonotonicTime::now();
    VERIFY(frame->format == AV_PIX_FMT_NV12);
    if (frame->linesize[0] < 0 || frame->linesize[1] < 0)
        return Error::from_string_literal("Reversed NV12 scanlines are not supported");
    if (!frame->data[0] || !frame->data[1])
        return Error::from_string_literal("NV12 frame had missing planes");

    auto width = frame->width;
    auto height = frame->height;
    auto source_y_stride = frame->linesize[0];
    auto source_uv_stride = frame->linesize[1];
    auto y_stride = width;
    auto uv_stride = ((width + 1) / 2) * 2;
    auto uv_rows = (height + 1) / 2;

    auto create_retained_frame_data = [&]() -> ErrorOr<NonnullRefPtr<NV12VideoFrameData>> {
        auto* retained_frame = av_frame_alloc();
        if (!retained_frame)
            return Error::from_errno(ENOMEM);

        auto result = av_frame_ref(retained_frame, frame);
        if (result < 0) {
            av_frame_free(&retained_frame);
            return Error::from_errno(ENOMEM);
        }

        auto data = make_ref_counted<NV12VideoFrameData>();
        data->width = width;
        data->height = height;
        data->y_stride = source_y_stride;
        data->uv_stride = source_uv_stride;
        data->cicp = cicp;
        data->set_external_planes(
            retained_frame->data[0],
            static_cast<size_t>(source_y_stride) * static_cast<size_t>(height),
            retained_frame->data[1],
            static_cast<size_t>(source_uv_stride) * static_cast<size_t>(uv_rows),
            [retained_frame]() mutable {
                av_frame_free(&retained_frame);
            });

        auto total_microseconds = (MonotonicTime::now() - copy_start).to_microseconds();
        static size_t s_nv12_frame_retain_count { 0 };
        auto count = ++s_nv12_frame_retain_count;
        if (count <= 8 || count % 120 == 0) {
            dbgln("MUNDO_MEDIA_FFMPEG nv12_frame_retain count={} total_us={} size={}x{} y_stride={} uv_stride={} compact={}/{}",
                count,
                total_microseconds,
                width,
                height,
                source_y_stride,
                source_uv_stride,
                source_y_stride == y_stride,
                source_uv_stride == uv_stride);
        }

        return data;
    };

    if (auto retained_data = create_retained_frame_data(); !retained_data.is_error())
        return retained_data.release_value();

    auto allocation_start = MonotonicTime::now();
    auto y_plane = TRY(ByteBuffer::create_uninitialized(static_cast<size_t>(y_stride) * static_cast<size_t>(height)));
    auto uv_plane = TRY(ByteBuffer::create_uninitialized(static_cast<size_t>(uv_stride) * static_cast<size_t>(uv_rows)));
    auto allocation_microseconds = (MonotonicTime::now() - allocation_start).to_microseconds();

    auto pixels = static_cast<size_t>(width) * static_cast<size_t>(height);
    auto job_count = parallel_nv12_frame_copy_enabled() && pixels >= 1920 * 1080
        ? direct_nv12_parallel_job_count(width, height)
        : 1;
    auto copy_body_start = MonotonicTime::now();
    if (source_y_stride == y_stride)
        __builtin_memcpy(y_plane.data(), frame->data[0], static_cast<size_t>(y_stride) * static_cast<size_t>(height));
    else {
        parallel_for_nv12_rows(height, job_count, [&](u32 start_row, u32 end_row) {
            for (auto row = start_row; row < end_row; ++row)
                __builtin_memcpy(y_plane.data() + static_cast<size_t>(row) * static_cast<size_t>(y_stride), frame->data[0] + static_cast<size_t>(row) * static_cast<size_t>(source_y_stride), y_stride);
        });
    }

    if (source_uv_stride == uv_stride)
        __builtin_memcpy(uv_plane.data(), frame->data[1], static_cast<size_t>(uv_stride) * static_cast<size_t>(uv_rows));
    else {
        parallel_for_nv12_rows(height, job_count, [&](u32 start_row, u32 end_row) {
            auto start_uv_row = start_row / 2;
            auto end_uv_row = min(static_cast<u32>(uv_rows), (end_row + 1) / 2);
            for (auto row = start_uv_row; row < end_uv_row; ++row)
                __builtin_memcpy(uv_plane.data() + static_cast<size_t>(row) * static_cast<size_t>(uv_stride), frame->data[1] + static_cast<size_t>(row) * static_cast<size_t>(source_uv_stride), uv_stride);
        });
    }
    auto copy_body_microseconds = (MonotonicTime::now() - copy_body_start).to_microseconds();
    auto total_microseconds = (MonotonicTime::now() - copy_start).to_microseconds();

    static size_t s_nv12_frame_copy_count { 0 };
    auto count = ++s_nv12_frame_copy_count;
    if (count <= 8 || count % 120 == 0) {
        dbgln("MUNDO_MEDIA_FFMPEG nv12_frame_copy count={} total_us={} alloc_us={} copy_body_us={} jobs={} size={}x{} y_stride={}/{} uv_stride={}/{} compact={}/{}",
            count,
            total_microseconds,
            allocation_microseconds,
            copy_body_microseconds,
            job_count,
            width,
            height,
            y_stride,
            source_y_stride,
            uv_stride,
            source_uv_stride,
            source_y_stride == y_stride,
            source_uv_stride == uv_stride);
    }

    auto data = make_ref_counted<NV12VideoFrameData>();
    data->y_plane = move(y_plane);
    data->uv_plane = move(uv_plane);
    data->width = width;
    data->height = height;
    data->y_stride = y_stride;
    data->uv_stride = uv_stride;
    data->cicp = cicp;
    return data;
}

static ErrorOr<NonnullRefPtr<Gfx::ImmutableBitmap>> create_bitmap_directly_from_nv12_frame_data(NV12VideoFrameData const& frame)
{
    if (frame.cicp.matrix_coefficients() == MatrixCoefficients::Identity)
        return Error::from_string_literal("NV12 direct conversion does not support identity matrix");

    auto target_size = target_size_for_nvdec_dimensions(frame.width, frame.height);
    auto bitmap = TRY(Gfx::Bitmap::create(Gfx::BitmapFormat::RGBA8888, Gfx::AlphaType::Premultiplied, target_size));

    auto source_width = static_cast<u32>(frame.width);
    auto source_height = static_cast<u32>(frame.height);
    auto target_width = static_cast<u32>(target_size.width());
    auto target_height = static_cast<u32>(target_size.height());
    auto coefficients = coefficients_for_cicp(frame.cicp);
    auto lookup_tables = make_yuv_to_rgb_lookup_tables(coefficients);
    auto job_count = direct_nv12_parallel_job_count(target_width, target_height);

    if (target_width == source_width && target_height == source_height) {
        parallel_for_nv12_rows(source_height, job_count, [&](u32 start_row, u32 end_row) {
            for (u32 row = start_row; row < end_row; row += 2) {
                auto const* y_row = frame.y_plane_data() + row * frame.y_stride;
                auto const* uv_row = frame.uv_plane_data() + (row / 2) * frame.uv_stride;
                auto* dst_row = bitmap->scanline_u8(row);
                convert_nv12_row_to_rgba(lookup_tables, y_row, uv_row, dst_row, source_width);

                if (row + 1 < source_height && row + 1 < end_row) {
                    auto const* next_y_row = frame.y_plane_data() + (row + 1) * frame.y_stride;
                    auto* next_dst_row = bitmap->scanline_u8(row + 1);
                    convert_nv12_row_to_rgba(lookup_tables, next_y_row, uv_row, next_dst_row, source_width);
                }
            }
        });
    } else {
        parallel_for_nv12_rows(target_height, job_count, [&](u32 start_row, u32 end_row) {
            for (u32 row = start_row; row < end_row; ++row) {
                auto source_y = min((static_cast<u64>(row) * source_height) / target_height, static_cast<u64>(source_height - 1));
                auto const* y_row = frame.y_plane_data() + source_y * frame.y_stride;
                auto const* uv_row = frame.uv_plane_data() + (source_y / 2) * frame.uv_stride;
                auto* dst_row = bitmap->scanline_u8(row);

                for (u32 col = 0; col < target_width; ++col) {
                    auto source_x = min((static_cast<u64>(col) * source_width) / target_width, static_cast<u64>(source_width - 1));
                    auto uv_x = (source_x / 2) * 2;
                    convert_yuv_to_rgba(lookup_tables, y_row[source_x], uv_row[uv_x], uv_row[uv_x + 1], dst_row + col * 4);
                }
            }
        });
    }

    return Gfx::ImmutableBitmap::create(move(bitmap));
}

static bool lazy_hardware_frame_transfer_enabled()
{
    auto const* raw_value = getenv("MUNDO_VIDEO_LAZY_HW_TRANSFER");
    if (!raw_value)
        return true;
    return strcmp(raw_value, "0") && strcmp(raw_value, "false") && strcmp(raw_value, "no") && strcmp(raw_value, "off");
}

#if defined(__linux__)
static Atomic<bool>& cuda_gl_buffer_interop_disabled()
{
    static Atomic<bool> disabled { false };
    return disabled;
}

static void log_cuda_external_memory_symbols_once()
{
    static Atomic<bool> did_log { false };
    if (did_log.exchange(true))
        return;

    auto* library = dlopen("libcuda.so.1", RTLD_LAZY);
    if (!library) {
        dbgln("MUNDO_MEDIA_FFMPEG cuda_external_memory_symbols source=nvdec_frame library=false import_memory=false destroy_memory=false mapped_buffer=false mapped_mipmap=false mip_level=false mip_destroy=false import_semaphore=false destroy_semaphore=false signal_semaphore=false wait_semaphore=false");
        return;
    }

    dbgln("MUNDO_MEDIA_FFMPEG cuda_external_memory_symbols source=nvdec_frame library=true import_memory={} destroy_memory={} mapped_buffer={} mapped_mipmap={} mip_level={} mip_destroy={} import_semaphore={} destroy_semaphore={} signal_semaphore={} wait_semaphore={}",
        dlsym(library, "cuImportExternalMemory") != nullptr,
        dlsym(library, "cuDestroyExternalMemory") != nullptr,
        dlsym(library, "cuExternalMemoryGetMappedBuffer") != nullptr,
        dlsym(library, "cuExternalMemoryGetMappedMipmappedArray") != nullptr,
        dlsym(library, "cuMipmappedArrayGetLevel") != nullptr,
        dlsym(library, "cuMipmappedArrayDestroy") != nullptr,
        dlsym(library, "cuImportExternalSemaphore") != nullptr,
        dlsym(library, "cuDestroyExternalSemaphore") != nullptr,
        dlsym(library, "cuSignalExternalSemaphoresAsync") != nullptr,
        dlsym(library, "cuWaitExternalSemaphoresAsync") != nullptr);
}
#endif

class RetainedHardwareFrameSource final : public HardwareVideoFrameHandle {
public:
    static ErrorOr<NonnullRefPtr<RetainedHardwareFrameSource>> create(AVFrame const* frame, CodingIndependentCodePoints const& cicp, HardwareVideoFrameDescriptor descriptor)
    {
        auto* retained_frame = av_frame_alloc();
        if (!retained_frame)
            return Error::from_errno(ENOMEM);

        auto result = av_frame_ref(retained_frame, frame);
        if (result < 0) {
            av_frame_free(&retained_frame);
            return Error::from_errno(ENOMEM);
        }

        return make_ref_counted<RetainedHardwareFrameSource>(retained_frame, cicp, descriptor);
    }

    RetainedHardwareFrameSource(AVFrame* frame, CodingIndependentCodePoints cicp, HardwareVideoFrameDescriptor descriptor)
        : HardwareVideoFrameHandle(descriptor)
        , m_frame(frame)
        , m_cicp(cicp)
    {
#if defined(__linux__)
        if (descriptor.backend == HardwareVideoFrameBackend::Cuda)
            log_cuda_external_memory_symbols_once();
#endif
    }

    ~RetainedHardwareFrameSource()
    {
        if (!m_was_transferred) {
            static size_t s_dropped_deferred_hw_frame_count { 0 };
            auto count = ++s_dropped_deferred_hw_frame_count;
            if (count <= 8 || count % 120 == 0) {
                dbgln("MUNDO_MEDIA_FFMPEG deferred_hw_frame_released_without_transfer count={} hw_format={} size={}x{}",
                    count,
                    m_frame ? pixel_format_name(static_cast<AVPixelFormat>(m_frame->format)) : "unknown",
                    m_frame ? m_frame->width : 0,
                    m_frame ? m_frame->height : 0);
            }
        }
        static size_t s_retained_hw_frame_destroy_count { 0 };
        auto destroy_count = ++s_retained_hw_frame_destroy_count;
        if (destroy_count <= 8 || destroy_count % 120 == 0) {
            auto const& hardware_descriptor = descriptor();
            dbgln("MUNDO_MEDIA_FFMPEG retained_hw_frame_destroy count={} frame_id={} backend={} transferred={} hw_format={} size={}x{}",
                destroy_count,
                hardware_descriptor.frame_id,
                hardware_video_frame_backend_name(hardware_descriptor.backend),
                m_was_transferred,
                m_frame ? pixel_format_name(static_cast<AVPixelFormat>(m_frame->format)) : "unknown",
                m_frame ? m_frame->width : 0,
                m_frame ? m_frame->height : 0);
        }
        av_frame_free(&m_frame);
    }

    ErrorOr<NonnullRefPtr<NV12VideoFrameData>> nv12_data()
    {
        if (m_nv12_data)
            return *m_nv12_data;

        auto* transfer_frame = av_frame_alloc();
        if (!transfer_frame)
            return Error::from_errno(ENOMEM);

        auto cleanup_transfer_frame = ScopeGuard([&] {
            av_frame_free(&transfer_frame);
        });

        auto transfer_start = MonotonicTime::now();
        auto result = av_hwframe_transfer_data(transfer_frame, m_frame, 0);
        auto transfer_microseconds = (MonotonicTime::now() - transfer_start).to_microseconds();
        if (result < 0)
            return Error::from_string_literal("Failed to transfer retained FFmpeg hardware frame to CPU");

        result = av_frame_copy_props(transfer_frame, m_frame);
        if (result < 0)
            return Error::from_string_literal("Failed to copy retained FFmpeg hardware frame properties");

        if (transfer_frame->format != AV_PIX_FMT_NV12)
            return Error::from_string_literal("Retained hardware frame did not transfer as NV12");

        auto retain_start = MonotonicTime::now();
        auto nv12_data = TRY(copy_nv12_frame_data(transfer_frame, m_cicp));
        auto retain_microseconds = (MonotonicTime::now() - retain_start).to_microseconds();
        m_was_transferred = true;

        static size_t s_lazy_hw_transfer_count { 0 };
        auto count = ++s_lazy_hw_transfer_count;
        if (count <= 8 || count % 120 == 0) {
            dbgln("MUNDO_MEDIA_FFMPEG lazy_hw_transfer count={} frame_id={} hw_format={} sw_format={} size={}x{} transfer_us={} retain_us={}",
                count,
                descriptor().frame_id,
                pixel_format_name(static_cast<AVPixelFormat>(m_frame->format)),
                pixel_format_name(static_cast<AVPixelFormat>(transfer_frame->format)),
                transfer_frame->width,
                transfer_frame->height,
                transfer_microseconds,
                retain_microseconds);
        }

        m_nv12_data = nv12_data;
        return nv12_data;
    }

    virtual ErrorOr<HardwareVideoFrameGLTextureUploadResult> upload_to_gl_textures(HardwareVideoFrameGLTextureUploadRequest const& request) const override
    {
#if defined(__linux__)
        if (m_frame->format != AV_PIX_FMT_CUDA)
            return Error::from_string_literal("Retained frame is not a CUDA frame");
        if (!m_frame->data[0] || !m_frame->data[1])
            return Error::from_string_literal("CUDA frame does not expose both NV12 planes");
        if (m_frame->linesize[0] <= 0 || m_frame->linesize[1] <= 0)
            return Error::from_string_literal("CUDA frame has invalid plane pitch");
        if (!request.y_texture || !request.uv_texture || !request.texture_target)
            return Error::from_string_literal("Invalid GL texture upload request");

        auto* functions = TRY(cuda_gl_functions());
        auto* cuda_context = cuda_context_from_frame();
        if (!cuda_context)
            return Error::from_string_literal("CUDA frame has no associated CUDA context");

        CUcontext previous_context { nullptr };
        auto push_result = functions->cuCtxPushCurrent(cuda_context);
        if (push_result != CUDA_SUCCESS)
            return Error::from_string_literal("Failed to push CUDA context for GL texture upload");
        auto pop_context = ScopeGuard([&] {
            functions->cuCtxPopCurrent(&previous_context);
        });

        auto upload_start = MonotonicTime::now();
        if (request.y_upload_buffer && request.uv_upload_buffer) {
            if (cuda_gl_buffer_interop_disabled().load())
                return Error::from_string_literal("CUDA GL upload buffer interop disabled after mismatched mapping");
            TRY(copy_cuda_plane_to_gl_buffer(*functions, "y", descriptor().frame_id, reinterpret_cast<CUdeviceptr>(m_frame->data[0]), static_cast<size_t>(m_frame->linesize[0]), request.y_upload_buffer, request.width, request.height, request.y_upload_buffer_size));
            TRY(copy_cuda_plane_to_gl_buffer(*functions, "uv", descriptor().frame_id, reinterpret_cast<CUdeviceptr>(m_frame->data[1]), static_cast<size_t>(m_frame->linesize[1]), request.uv_upload_buffer, request.uv_width * 2, request.uv_height, request.uv_upload_buffer_size));
        } else {
            TRY(copy_cuda_plane_to_gl_texture(*functions, "y", descriptor().frame_id, reinterpret_cast<CUdeviceptr>(m_frame->data[0]), static_cast<size_t>(m_frame->linesize[0]), request.y_texture, request.texture_target, request.width, request.height));
            TRY(copy_cuda_plane_to_gl_texture(*functions, "uv", descriptor().frame_id, reinterpret_cast<CUdeviceptr>(m_frame->data[1]), static_cast<size_t>(m_frame->linesize[1]), request.uv_texture, request.texture_target, request.uv_width * 2, request.uv_height));
        }
        auto upload_microseconds = (MonotonicTime::now() - upload_start).to_microseconds();

        m_was_gpu_uploaded = true;
        static size_t s_cuda_gl_texture_upload_count { 0 };
        auto count = ++s_cuda_gl_texture_upload_count;
        if (count <= 8 || count % 120 == 0) {
            dbgln("MUNDO_MEDIA_FFMPEG cuda_gl_texture_upload count={} frame_id={} size={}x{} uv={}x{} y_pitch={} uv_pitch={} upload_us={}",
                count,
                descriptor().frame_id,
                request.width,
                request.height,
                request.uv_width,
                request.uv_height,
                m_frame->linesize[0],
                m_frame->linesize[1],
                upload_microseconds);
        }

        return HardwareVideoFrameGLTextureUploadResult { .upload_microseconds = static_cast<u64>(upload_microseconds) };
#else
        (void)request;
        return Error::from_string_literal("CUDA GL texture upload is only implemented on Linux");
#endif
    }

private:
#if defined(__linux__)
    struct CudaGLFunctions {
        void* library { nullptr };
        tcuCtxPushCurrent_v2* cuCtxPushCurrent { nullptr };
        tcuCtxPopCurrent_v2* cuCtxPopCurrent { nullptr };
        tcuGraphicsGLRegisterImage* cuGraphicsGLRegisterImage { nullptr };
        tcuGraphicsUnregisterResource* cuGraphicsUnregisterResource { nullptr };
        tcuGraphicsMapResources* cuGraphicsMapResources { nullptr };
        tcuGraphicsUnmapResources* cuGraphicsUnmapResources { nullptr };
        tcuGraphicsSubResourceGetMappedArray* cuGraphicsSubResourceGetMappedArray { nullptr };
        tcuGraphicsGLRegisterBuffer* cuGraphicsGLRegisterBuffer { nullptr };
        tcuGraphicsResourceGetMappedPointer* cuGraphicsResourceGetMappedPointer { nullptr };
        tcuMemcpy2D_v2* cuMemcpy2D { nullptr };
        tcuImportExternalMemory* cuImportExternalMemory { nullptr };
        tcuDestroyExternalMemory* cuDestroyExternalMemory { nullptr };
        tcuExternalMemoryGetMappedBuffer* cuExternalMemoryGetMappedBuffer { nullptr };
        tcuExternalMemoryGetMappedMipmappedArray* cuExternalMemoryGetMappedMipmappedArray { nullptr };
        tcuMipmappedArrayGetLevel* cuMipmappedArrayGetLevel { nullptr };
        tcuMipmappedArrayDestroy* cuMipmappedArrayDestroy { nullptr };
        tcuImportExternalSemaphore* cuImportExternalSemaphore { nullptr };
        tcuDestroyExternalSemaphore* cuDestroyExternalSemaphore { nullptr };
        tcuSignalExternalSemaphoresAsync* cuSignalExternalSemaphoresAsync { nullptr };
        tcuWaitExternalSemaphoresAsync* cuWaitExternalSemaphoresAsync { nullptr };
    };

    struct MinimalAVCUDADeviceContext {
        CUcontext cuda_ctx { nullptr };
        CUstream stream { nullptr };
        void* internal { nullptr };
    };

    static ErrorOr<CudaGLFunctions*> cuda_gl_functions()
    {
        static CudaGLFunctions s_functions;
        static bool s_load_attempted { false };
        if (!s_load_attempted) {
            s_load_attempted = true;
            s_functions.library = dlopen("libcuda.so.1", RTLD_LAZY);
            if (s_functions.library) {
                s_functions.cuCtxPushCurrent = reinterpret_cast<tcuCtxPushCurrent_v2*>(dlsym(s_functions.library, "cuCtxPushCurrent_v2"));
                s_functions.cuCtxPopCurrent = reinterpret_cast<tcuCtxPopCurrent_v2*>(dlsym(s_functions.library, "cuCtxPopCurrent_v2"));
                s_functions.cuGraphicsGLRegisterImage = reinterpret_cast<tcuGraphicsGLRegisterImage*>(dlsym(s_functions.library, "cuGraphicsGLRegisterImage"));
                s_functions.cuGraphicsUnregisterResource = reinterpret_cast<tcuGraphicsUnregisterResource*>(dlsym(s_functions.library, "cuGraphicsUnregisterResource"));
                s_functions.cuGraphicsMapResources = reinterpret_cast<tcuGraphicsMapResources*>(dlsym(s_functions.library, "cuGraphicsMapResources"));
                s_functions.cuGraphicsUnmapResources = reinterpret_cast<tcuGraphicsUnmapResources*>(dlsym(s_functions.library, "cuGraphicsUnmapResources"));
                s_functions.cuGraphicsSubResourceGetMappedArray = reinterpret_cast<tcuGraphicsSubResourceGetMappedArray*>(dlsym(s_functions.library, "cuGraphicsSubResourceGetMappedArray"));
                s_functions.cuGraphicsGLRegisterBuffer = reinterpret_cast<tcuGraphicsGLRegisterBuffer*>(dlsym(s_functions.library, "cuGraphicsGLRegisterBuffer"));
                s_functions.cuGraphicsResourceGetMappedPointer = reinterpret_cast<tcuGraphicsResourceGetMappedPointer*>(dlsym(s_functions.library, "cuGraphicsResourceGetMappedPointer_v2"));
                s_functions.cuMemcpy2D = reinterpret_cast<tcuMemcpy2D_v2*>(dlsym(s_functions.library, "cuMemcpy2D_v2"));
                s_functions.cuImportExternalMemory = reinterpret_cast<tcuImportExternalMemory*>(dlsym(s_functions.library, "cuImportExternalMemory"));
                s_functions.cuDestroyExternalMemory = reinterpret_cast<tcuDestroyExternalMemory*>(dlsym(s_functions.library, "cuDestroyExternalMemory"));
                s_functions.cuExternalMemoryGetMappedBuffer = reinterpret_cast<tcuExternalMemoryGetMappedBuffer*>(dlsym(s_functions.library, "cuExternalMemoryGetMappedBuffer"));
                s_functions.cuExternalMemoryGetMappedMipmappedArray = reinterpret_cast<tcuExternalMemoryGetMappedMipmappedArray*>(dlsym(s_functions.library, "cuExternalMemoryGetMappedMipmappedArray"));
                s_functions.cuMipmappedArrayGetLevel = reinterpret_cast<tcuMipmappedArrayGetLevel*>(dlsym(s_functions.library, "cuMipmappedArrayGetLevel"));
                s_functions.cuMipmappedArrayDestroy = reinterpret_cast<tcuMipmappedArrayDestroy*>(dlsym(s_functions.library, "cuMipmappedArrayDestroy"));
                s_functions.cuImportExternalSemaphore = reinterpret_cast<tcuImportExternalSemaphore*>(dlsym(s_functions.library, "cuImportExternalSemaphore"));
                s_functions.cuDestroyExternalSemaphore = reinterpret_cast<tcuDestroyExternalSemaphore*>(dlsym(s_functions.library, "cuDestroyExternalSemaphore"));
                s_functions.cuSignalExternalSemaphoresAsync = reinterpret_cast<tcuSignalExternalSemaphoresAsync*>(dlsym(s_functions.library, "cuSignalExternalSemaphoresAsync"));
                s_functions.cuWaitExternalSemaphoresAsync = reinterpret_cast<tcuWaitExternalSemaphoresAsync*>(dlsym(s_functions.library, "cuWaitExternalSemaphoresAsync"));
                dbgln("MUNDO_MEDIA_FFMPEG cuda_external_memory_symbols import_memory={} destroy_memory={} mapped_buffer={} mapped_mipmap={} mip_level={} mip_destroy={} import_semaphore={} destroy_semaphore={} signal_semaphore={} wait_semaphore={}",
                    s_functions.cuImportExternalMemory != nullptr,
                    s_functions.cuDestroyExternalMemory != nullptr,
                    s_functions.cuExternalMemoryGetMappedBuffer != nullptr,
                    s_functions.cuExternalMemoryGetMappedMipmappedArray != nullptr,
                    s_functions.cuMipmappedArrayGetLevel != nullptr,
                    s_functions.cuMipmappedArrayDestroy != nullptr,
                    s_functions.cuImportExternalSemaphore != nullptr,
                    s_functions.cuDestroyExternalSemaphore != nullptr,
                    s_functions.cuSignalExternalSemaphoresAsync != nullptr,
                    s_functions.cuWaitExternalSemaphoresAsync != nullptr);
            }
        }

        if (!s_functions.library)
            return Error::from_string_literal("Failed to load libcuda.so.1");
        if (!s_functions.cuCtxPushCurrent || !s_functions.cuCtxPopCurrent || !s_functions.cuGraphicsGLRegisterImage || !s_functions.cuGraphicsGLRegisterBuffer || !s_functions.cuGraphicsUnregisterResource || !s_functions.cuGraphicsMapResources || !s_functions.cuGraphicsUnmapResources || !s_functions.cuGraphicsSubResourceGetMappedArray || !s_functions.cuGraphicsResourceGetMappedPointer || !s_functions.cuMemcpy2D)
            return Error::from_string_literal("Missing CUDA GL interop symbols");
        return &s_functions;
    }

    CUcontext cuda_context_from_frame() const
    {
        if (!m_frame->hw_frames_ctx || !m_frame->hw_frames_ctx->data)
            return nullptr;
        auto* frames_context = reinterpret_cast<AVHWFramesContext*>(m_frame->hw_frames_ctx->data);
        if (!frames_context->device_ctx)
            return nullptr;
        auto* device_context = frames_context->device_ctx;
        if (!device_context->hwctx)
            return nullptr;
        return reinterpret_cast<MinimalAVCUDADeviceContext*>(device_context->hwctx)->cuda_ctx;
    }

    static ErrorOr<void> copy_cuda_plane_to_gl_texture(CudaGLFunctions& functions, char const* plane_name, u64 frame_id, CUdeviceptr source, size_t source_pitch, u32 texture, u32 texture_target, u32 width_in_bytes, u32 height)
    {
        CUgraphicsResource resource { nullptr };
        auto register_result = functions.cuGraphicsGLRegisterImage(&resource, texture, texture_target, CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD);
        if (register_result != CUDA_SUCCESS) {
            dbgln("MUNDO_MEDIA_FFMPEG cuda_gl_texture_upload_error frame_id={} plane={} step=register result={} texture={} target={} width_bytes={} height={} pitch={}",
                frame_id, plane_name, static_cast<int>(register_result), texture, texture_target, width_in_bytes, height, source_pitch);
            return Error::from_string_literal("Failed to register GL texture with CUDA");
        }
        auto unregister_resource = ScopeGuard([&] {
            functions.cuGraphicsUnregisterResource(resource);
        });

        auto map_result = functions.cuGraphicsMapResources(1, &resource, nullptr);
        if (map_result != CUDA_SUCCESS) {
            dbgln("MUNDO_MEDIA_FFMPEG cuda_gl_texture_upload_error frame_id={} plane={} step=map result={} texture={} target={} width_bytes={} height={} pitch={}",
                frame_id, plane_name, static_cast<int>(map_result), texture, texture_target, width_in_bytes, height, source_pitch);
            return Error::from_string_literal("Failed to map CUDA graphics resource");
        }
        auto unmap_resource = ScopeGuard([&] {
            functions.cuGraphicsUnmapResources(1, &resource, nullptr);
        });

        CUarray array { nullptr };
        auto array_result = functions.cuGraphicsSubResourceGetMappedArray(&array, resource, 0, 0);
        if (array_result != CUDA_SUCCESS || !array) {
            dbgln("MUNDO_MEDIA_FFMPEG cuda_gl_texture_upload_error frame_id={} plane={} step=array result={} array={} texture={} target={} width_bytes={} height={} pitch={}",
                frame_id, plane_name, static_cast<int>(array_result), static_cast<void*>(array), texture, texture_target, width_in_bytes, height, source_pitch);
            return Error::from_string_literal("Failed to get mapped CUDA array from GL texture");
        }

        CUDA_MEMCPY2D copy {};
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice = source;
        copy.srcPitch = source_pitch;
        copy.dstMemoryType = CU_MEMORYTYPE_ARRAY;
        copy.dstArray = array;
        copy.WidthInBytes = width_in_bytes;
        copy.Height = height;
        auto copy_result = functions.cuMemcpy2D(&copy);
        if (copy_result != CUDA_SUCCESS) {
            dbgln("MUNDO_MEDIA_FFMPEG cuda_gl_texture_upload_error frame_id={} plane={} step=copy result={} texture={} target={} width_bytes={} height={} pitch={} source={}",
                frame_id, plane_name, static_cast<int>(copy_result), texture, texture_target, width_in_bytes, height, source_pitch, source);
            return Error::from_string_literal("Failed to copy CUDA plane into GL texture");
        }
        return {};
    }

    static ErrorOr<void> copy_cuda_plane_to_gl_buffer(CudaGLFunctions& functions, char const* plane_name, u64 frame_id, CUdeviceptr source, size_t source_pitch, u32 buffer, u32 width_in_bytes, u32 height, size_t buffer_size)
    {
        auto required_size = static_cast<size_t>(width_in_bytes) * static_cast<size_t>(height);
        if (buffer_size < required_size)
            return Error::from_string_literal("GL upload buffer is smaller than CUDA plane copy");

        CUgraphicsResource resource { nullptr };
        auto register_result = functions.cuGraphicsGLRegisterBuffer(&resource, buffer, CU_GRAPHICS_REGISTER_FLAGS_WRITE_DISCARD);
        if (register_result != CUDA_SUCCESS) {
            dbgln("MUNDO_MEDIA_FFMPEG cuda_gl_buffer_upload_error frame_id={} plane={} step=register result={} buffer={} width_bytes={} height={} pitch={} buffer_size={}",
                frame_id, plane_name, static_cast<int>(register_result), buffer, width_in_bytes, height, source_pitch, buffer_size);
            return Error::from_string_literal("Failed to register GL upload buffer with CUDA");
        }
        auto unregister_resource = ScopeGuard([&] {
            functions.cuGraphicsUnregisterResource(resource);
        });

        auto map_result = functions.cuGraphicsMapResources(1, &resource, nullptr);
        if (map_result != CUDA_SUCCESS) {
            dbgln("MUNDO_MEDIA_FFMPEG cuda_gl_buffer_upload_error frame_id={} plane={} step=map result={} buffer={} width_bytes={} height={} pitch={} buffer_size={}",
                frame_id, plane_name, static_cast<int>(map_result), buffer, width_in_bytes, height, source_pitch, buffer_size);
            return Error::from_string_literal("Failed to map CUDA graphics buffer");
        }
        auto unmap_resource = ScopeGuard([&] {
            functions.cuGraphicsUnmapResources(1, &resource, nullptr);
        });

        CUdeviceptr mapped_pointer {};
        size_t mapped_size { 0 };
        auto pointer_result = functions.cuGraphicsResourceGetMappedPointer(&mapped_pointer, &mapped_size, resource);
        if (pointer_result != CUDA_SUCCESS || mapped_size < required_size) {
            dbgln("MUNDO_MEDIA_FFMPEG cuda_gl_buffer_upload_error frame_id={} plane={} step=pointer result={} buffer={} width_bytes={} height={} pitch={} mapped_size={} required_size={} gl_reported_buffer_size={}",
                frame_id, plane_name, static_cast<int>(pointer_result), buffer, width_in_bytes, height, source_pitch, mapped_size, required_size, buffer_size);
            if (pointer_result == CUDA_SUCCESS && buffer_size >= required_size && mapped_size < required_size) {
                auto was_disabled = cuda_gl_buffer_interop_disabled().exchange(true);
                if (!was_disabled) {
                    dbgln("MUNDO_MEDIA_FFMPEG cuda_gl_buffer_interop_disabled reason=mapped_size_mismatch plane={} buffer={} mapped_size={} required_size={} gl_reported_buffer_size={}",
                        plane_name, buffer, mapped_size, required_size, buffer_size);
                }
            }
            return Error::from_string_literal("Failed to get mapped CUDA pointer from GL upload buffer");
        }

        CUDA_MEMCPY2D copy {};
        copy.srcMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.srcDevice = source;
        copy.srcPitch = source_pitch;
        copy.dstMemoryType = CU_MEMORYTYPE_DEVICE;
        copy.dstDevice = mapped_pointer;
        copy.dstPitch = width_in_bytes;
        copy.WidthInBytes = width_in_bytes;
        copy.Height = height;
        auto copy_result = functions.cuMemcpy2D(&copy);
        if (copy_result != CUDA_SUCCESS) {
            dbgln("MUNDO_MEDIA_FFMPEG cuda_gl_buffer_upload_error frame_id={} plane={} step=copy result={} buffer={} width_bytes={} height={} pitch={} mapped_size={} source={} destination={}",
                frame_id, plane_name, static_cast<int>(copy_result), buffer, width_in_bytes, height, source_pitch, mapped_size, source, mapped_pointer);
            return Error::from_string_literal("Failed to copy CUDA plane into GL upload buffer");
        }
        return {};
    }
#endif

    AVFrame* m_frame { nullptr };
    CodingIndependentCodePoints m_cicp;
    RefPtr<NV12VideoFrameData> m_nv12_data;
    bool m_was_transferred { false };
    mutable bool m_was_gpu_uploaded { false };
};

static DecoderErrorOr<void> copy_planar_frame_to_yuv_data(AVFrame const* frame, Gfx::YUVData& yuv_data, Gfx::Size<u32> size, Subsampling subsampling, size_t component_size)
{
    auto y_plane_size = size.to_type<size_t>();
    auto uv_plane_size = subsampling.subsampled_size(size).to_type<size_t>();

    Bytes buffers[] = { yuv_data.y_data(), yuv_data.u_data(), yuv_data.v_data() };
    Gfx::Size<size_t> plane_sizes[] = { y_plane_size, uv_plane_size, uv_plane_size };

    for (u32 plane = 0; plane < 3; plane++) {
        VERIFY(frame->linesize[plane] != 0);
        if (frame->linesize[plane] < 0)
            return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "Reversed scanlines are not supported"sv);

        auto plane_size = plane_sizes[plane];
        auto const* source = frame->data[plane];
        VERIFY(source != nullptr);
        auto destination = buffers[plane];

        auto output_line_size = plane_size.width() * component_size;
        VERIFY(output_line_size <= static_cast<size_t>(frame->linesize[plane]));

        auto* dest_ptr = destination.data();
        for (size_t row = 0; row < plane_size.height(); row++) {
            memcpy(dest_ptr, source, output_line_size);
            source += frame->linesize[plane];
            dest_ptr += output_line_size;
        }
    }

    return {};
}

static DecoderErrorOr<void> copy_semiplanar_frame_to_yuv_data(AVFrame const* frame, Gfx::YUVData& yuv_data, Gfx::Size<u32> size, Subsampling subsampling, size_t component_size)
{
    VERIFY(is_semiplanar_pixel_format(static_cast<AVPixelFormat>(frame->format)));
    VERIFY(subsampling.x() && subsampling.y());

    if (frame->linesize[0] < 0 || frame->linesize[1] < 0)
        return DecoderError::with_description(DecoderErrorCategory::NotImplemented, "Reversed scanlines are not supported"sv);

    auto y_plane_size = size.to_type<size_t>();
    auto uv_plane_size = subsampling.subsampled_size(size).to_type<size_t>();

    auto y_destination = yuv_data.y_data();
    auto* y_dest_ptr = y_destination.data();
    auto const* y_source = frame->data[0];
    VERIFY(y_source != nullptr);
    auto y_output_line_size = y_plane_size.width() * component_size;
    VERIFY(y_output_line_size <= static_cast<size_t>(frame->linesize[0]));
    for (size_t row = 0; row < y_plane_size.height(); row++) {
        memcpy(y_dest_ptr, y_source, y_output_line_size);
        y_source += frame->linesize[0];
        y_dest_ptr += y_output_line_size;
    }

    auto u_destination = yuv_data.u_data();
    auto v_destination = yuv_data.v_data();
    auto* u_dest_ptr = u_destination.data();
    auto* v_dest_ptr = v_destination.data();
    auto const* uv_source = frame->data[1];
    VERIFY(uv_source != nullptr);
    auto uv_source_line_size = uv_plane_size.width() * 2 * component_size;
    VERIFY(uv_source_line_size <= static_cast<size_t>(frame->linesize[1]));

    for (size_t row = 0; row < uv_plane_size.height(); row++) {
        if (component_size == 1) {
            for (size_t col = 0; col < uv_plane_size.width(); col++) {
                u_dest_ptr[col] = uv_source[col * 2];
                v_dest_ptr[col] = uv_source[col * 2 + 1];
            }
        } else {
            auto const* uv_source_u16 = reinterpret_cast<u16 const*>(uv_source);
            auto* u_dest_u16 = reinterpret_cast<u16*>(u_dest_ptr);
            auto* v_dest_u16 = reinterpret_cast<u16*>(v_dest_ptr);
            for (size_t col = 0; col < uv_plane_size.width(); col++) {
                u_dest_u16[col] = uv_source_u16[col * 2];
                v_dest_u16[col] = uv_source_u16[col * 2 + 1];
            }
        }

        uv_source += frame->linesize[1];
        u_dest_ptr += uv_plane_size.width() * component_size;
        v_dest_ptr += uv_plane_size.width() * component_size;
    }

    return {};
}

DecoderErrorOr<NonnullOwnPtr<FFmpegVideoDecoder>> FFmpegVideoDecoder::try_create(CodecID codec_id, ReadonlyBytes codec_initialization_data)
{
    AVCodecContext* codec_context = nullptr;
    AVPacket* packet = nullptr;
    AVFrame* frame = nullptr;
    AVFrame* transfer_frame = nullptr;
    AVBufferRef* hw_device_context = nullptr;
    ArmedScopeGuard memory_guard {
        [&] {
            avcodec_free_context(&codec_context);
            av_packet_free(&packet);
            av_frame_free(&frame);
            av_frame_free(&transfer_frame);
            if (hw_device_context)
                av_buffer_unref(&hw_device_context);
        }
    };

    auto ff_codec_id = ffmpeg_codec_id_from_media_codec_id(codec_id);
    auto const* codec = avcodec_find_decoder(ff_codec_id);
    if (!codec)
        return DecoderError::format(DecoderErrorCategory::NotImplemented, "Could not find FFmpeg decoder for codec {}", codec_id);

    log_video_decoder_backend_probe(codec, codec_id);

    codec_context = avcodec_alloc_context3(codec);
    if (!codec_context)
        return DecoderError::format(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg codec context for codec {}", codec_id);

    codec_context->get_format = negotiate_output_format;
    codec_context->time_base = { 1, 1'000'000 };
    auto use_nvdec = should_try_nvdec(codec);
    codec_context->thread_count = use_nvdec ? 1 : static_cast<int>(min(Core::System::hardware_concurrency(), 16));
    dbgln("MUNDO_MEDIA_FFMPEG video_decoder_threads codec={} threads={} reason={}",
        codec_id,
        codec_context->thread_count,
        use_nvdec ? "nvdec"sv : "software"sv);

    if (use_nvdec) {
        quiet_ffmpeg_logs_for_nvdec();
        auto result = av_hwdevice_ctx_create(&hw_device_context, AV_HWDEVICE_TYPE_CUDA, nullptr, nullptr, 0);
        if (result >= 0) {
            codec_context->hw_device_ctx = av_buffer_ref(hw_device_context);
            if (codec_context->hw_device_ctx)
                dbgln("MUNDO_MEDIA_FFMPEG hwaccel_enable backend=nvdec codec={} status=enabled transfer=cpu", codec_id);
            else
                dbgln("MUNDO_MEDIA_FFMPEG hwaccel_enable backend=nvdec codec={} status=failed reason=av_buffer_ref", codec_id);
        } else {
            dbgln("MUNDO_MEDIA_FFMPEG hwaccel_enable backend=nvdec codec={} status=failed error={} fallback=software", codec_id, av_error_code_to_string(result));
        }
    }

    if (!codec_initialization_data.is_empty()) {
        if (codec_initialization_data.size() > NumericLimits<int>::max())
            return DecoderError::corrupted("Codec initialization data is too large"sv);

        codec_context->extradata = static_cast<u8*>(av_malloc(codec_initialization_data.size() + AV_INPUT_BUFFER_PADDING_SIZE));
        if (!codec_context->extradata)
            return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate codec initialization data buffer for FFmpeg codec"sv);

        memcpy(codec_context->extradata, codec_initialization_data.data(), codec_initialization_data.size());
        codec_context->extradata_size = static_cast<int>(codec_initialization_data.size());
    }

    if (avcodec_open2(codec_context, codec, nullptr) < 0)
        return DecoderError::format(DecoderErrorCategory::Unknown, "Unknown error occurred when opening FFmpeg codec {}", codec_id);

    packet = av_packet_alloc();
    if (!packet)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg packet"sv);

    frame = av_frame_alloc();
    if (!frame)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg frame"sv);

    transfer_frame = av_frame_alloc();
    if (!transfer_frame)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate FFmpeg hardware transfer frame"sv);

    memory_guard.disarm();
    if (hw_device_context)
        av_buffer_unref(&hw_device_context);
    return DECODER_TRY_ALLOC(try_make<FFmpegVideoDecoder>(codec_context, packet, frame, transfer_frame));
}

FFmpegVideoDecoder::FFmpegVideoDecoder(AVCodecContext* codec_context, AVPacket* packet, AVFrame* frame, AVFrame* transfer_frame)
    : m_codec_context(codec_context)
    , m_packet(packet)
    , m_frame(frame)
    , m_transfer_frame(transfer_frame)
{
}

FFmpegVideoDecoder::~FFmpegVideoDecoder()
{
    av_packet_free(&m_packet);
    av_frame_free(&m_frame);
    av_frame_free(&m_transfer_frame);
    avcodec_free_context(&m_codec_context);
}

DecoderErrorOr<void> FFmpegVideoDecoder::receive_coded_data(AK::Duration timestamp, AK::Duration duration, ReadonlyBytes coded_data)
{
    VERIFY(coded_data.size() < NumericLimits<int>::max());

    m_packet->data = const_cast<u8*>(coded_data.data());
    m_packet->size = static_cast<int>(coded_data.size());
    m_packet->pts = timestamp.to_microseconds();
    m_packet->dts = m_packet->pts;
    m_packet->duration = duration.to_microseconds();

    auto result = avcodec_send_packet(m_codec_context, m_packet);
    switch (result) {
    case 0:
        return {};
    case AVERROR(EAGAIN):
        return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "FFmpeg decoder cannot decode any more data until frames have been retrieved"sv);
    case AVERROR_EOF:
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "FFmpeg decoder has been flushed"sv);
    case AVERROR(EINVAL):
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "FFmpeg codec has not been opened"sv);
    case AVERROR(ENOMEM):
        return DecoderError::with_description(DecoderErrorCategory::Memory, "FFmpeg codec ran out of internal memory"sv);
    default:
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "FFmpeg codec reports that the data is corrupted"sv);
    }
}

void FFmpegVideoDecoder::signal_end_of_stream()
{
    m_packet->data = nullptr;
    m_packet->size = 0;
    m_packet->pts = 0;
    m_packet->dts = 0;

    auto result = avcodec_send_packet(m_codec_context, m_packet);
    VERIFY(result == 0 || result == AVERROR_EOF);
}

DecoderErrorOr<NonnullOwnPtr<VideoFrame>> FFmpegVideoDecoder::get_decoded_frame(CodingIndependentCodePoints const& container_cicp)
{
    auto result = avcodec_receive_frame(m_codec_context, m_frame);

    switch (result) {
    case 0: {
        if (is_hardware_frame(m_frame)
            && lazy_hardware_frame_transfer_enabled()
            && direct_nv12_rgba_bitmap_enabled()
            && lazy_nv12_rgba_bitmap_enabled()
            && !should_use_gpu_yuv_for_nv12_frame(m_frame->width, m_frame->height)) {
            auto color_primaries = static_cast<ColorPrimaries>(m_frame->color_primaries);
            auto transfer_characteristics = static_cast<TransferCharacteristics>(m_frame->color_trc);
            auto matrix_coefficients = static_cast<MatrixCoefficients>(m_frame->colorspace);
            auto color_range = [&] {
                switch (m_frame->color_range) {
                case AVColorRange::AVCOL_RANGE_MPEG:
                    return VideoFullRangeFlag::Studio;
                case AVColorRange::AVCOL_RANGE_JPEG:
                    return VideoFullRangeFlag::Full;
                default:
                    return VideoFullRangeFlag::Unspecified;
                }
            }();
            auto cicp = CodingIndependentCodePoints { color_primaries, transfer_characteristics, matrix_coefficients, color_range };
            cicp.adopt_specified_values(container_cicp);

            auto software_pixel_format = static_cast<AVPixelFormat>(m_codec_context->sw_pix_fmt);
            auto bit_depth = bit_depth_for_pixel_format(software_pixel_format);
            auto size = Gfx::Size<u32> { m_frame->width, m_frame->height };
            auto timestamp = AK::Duration::from_microseconds(m_frame->pts);
            auto duration = AK::Duration::from_microseconds(m_frame->duration);
            auto hardware_descriptor = hardware_descriptor_for_cuda_frame(m_codec_context, m_frame, bit_depth);

            auto source = DECODER_TRY_ALLOC(RetainedHardwareFrameSource::create(m_frame, cicp, hardware_descriptor));
            auto source_for_bitmap = source;
            auto bitmap_factory = [source_for_bitmap]() mutable -> ErrorOr<NonnullRefPtr<Gfx::ImmutableBitmap>> {
                auto frame_data = TRY(source_for_bitmap->nv12_data());
                return create_bitmap_directly_from_nv12_frame_data(*frame_data);
            };
            auto source_for_nv12 = source;
            auto nv12_data_factory = [source_for_nv12]() mutable -> ErrorOr<NonnullRefPtr<NV12VideoFrameData>> {
                return source_for_nv12->nv12_data();
            };

            static size_t s_deferred_hw_frame_count { 0 };
            auto count = ++s_deferred_hw_frame_count;
            if (count <= 8 || count % 120 == 0) {
                dbgln("MUNDO_MEDIA_FFMPEG defer_hw_transfer count={} frame_id={} hw_format={} sw_pix_fmt={} size={}x{} timestamp={}ms zero_copy_capable={} requires_cpu_transfer={}",
                    count,
                    hardware_descriptor.frame_id,
                    pixel_format_name(static_cast<AVPixelFormat>(m_frame->format)),
                    pixel_format_name(software_pixel_format),
                    m_frame->width,
                    m_frame->height,
                    timestamp.to_milliseconds(),
                    hardware_descriptor.zero_copy_capable,
                    hardware_descriptor.requires_cpu_transfer);
            }

            auto video_frame = DECODER_TRY_ALLOC(try_make<VideoFrame>(timestamp, duration, size, bit_depth, cicp, move(bitmap_factory), move(nv12_data_factory)));
            video_frame->set_hardware_handle(source);
            return video_frame;
        }

        if (!is_hardware_frame(m_frame))
            log_software_frame_while_hwaccel_requested(m_codec_context, m_frame);
        HardwareTransferTiming transfer_timing;
        auto* frame = DECODER_TRY(DecoderErrorCategory::Unknown, software_frame_for_decoded_frame(m_frame, m_transfer_frame, transfer_timing));

        auto color_primaries = static_cast<ColorPrimaries>(frame->color_primaries);
        auto transfer_characteristics = static_cast<TransferCharacteristics>(frame->color_trc);
        auto matrix_coefficients = static_cast<MatrixCoefficients>(frame->colorspace);
        auto color_range = [&] {
            switch (frame->color_range) {
            case AVColorRange::AVCOL_RANGE_MPEG:
                return VideoFullRangeFlag::Studio;
            case AVColorRange::AVCOL_RANGE_JPEG:
                return VideoFullRangeFlag::Full;
            default:
                return VideoFullRangeFlag::Unspecified;
            }
        }();
        auto cicp = CodingIndependentCodePoints { color_primaries, transfer_characteristics, matrix_coefficients, color_range };
        cicp.adopt_specified_values(container_cicp);

        auto pixel_format = static_cast<AVPixelFormat>(frame->format);
        auto bit_depth = bit_depth_for_pixel_format(pixel_format);
        auto subsampling = subsampling_for_pixel_format(pixel_format);

        auto size = Gfx::Size<u32> { frame->width, frame->height };
        auto gfx_size = Gfx::IntSize { frame->width, frame->height };

        auto timestamp = AK::Duration::from_microseconds(frame->pts);
        auto duration = AK::Duration::from_microseconds(frame->duration);

        auto pipeline_start = MonotonicTime::now();
        i64 copy_microseconds = 0;
        auto bitmap_start = MonotonicTime::now();
        RefPtr<Gfx::ImmutableBitmap> bitmap;
        Optional<HardwareVideoFrameDescriptor> hardware_descriptor;
        if (transfer_timing.transferred_from_hardware)
            hardware_descriptor = hardware_descriptor_for_cuda_frame(m_codec_context, m_frame, bit_depth);
        auto used_direct_nv12_bitmap = false;
        auto used_lazy_nv12_bitmap = false;
        auto should_use_gpu_yuv_for_nv12 = transfer_timing.transferred_from_hardware && pixel_format == AV_PIX_FMT_NV12 && should_use_gpu_yuv_for_nv12_frame(frame->width, frame->height);
        if (transfer_timing.transferred_from_hardware && pixel_format == AV_PIX_FMT_NV12) {
            if (!should_use_gpu_yuv_for_nv12 && direct_nv12_rgba_bitmap_enabled()) {
                if (lazy_nv12_rgba_bitmap_enabled()) {
                    auto copy_start = MonotonicTime::now();
                    auto owned_frame_data = copy_nv12_frame_data(frame, cicp);
                    copy_microseconds = (MonotonicTime::now() - copy_start).to_microseconds();
                    if (!owned_frame_data.is_error()) {
                        auto frame_data = owned_frame_data.release_value();
                        auto source_width = frame_data->width;
                        auto source_height = frame_data->height;
                        auto target_size = target_size_for_nvdec_dimensions(source_width, source_height);
                        auto bitmap_factory = [frame_data]() mutable -> ErrorOr<NonnullRefPtr<Gfx::ImmutableBitmap>> {
                            auto materialize_start = MonotonicTime::now();
                            auto bitmap = TRY(create_bitmap_directly_from_nv12_frame_data(*frame_data));
                            auto materialize_microseconds = (MonotonicTime::now() - materialize_start).to_microseconds();

                            static size_t s_lazy_materialize_count { 0 };
                            auto materialize_count = ++s_lazy_materialize_count;
                            if (materialize_count <= 8 || materialize_count % 120 == 0) {
                                auto materialized_size = bitmap->size();
                                dbgln("MUNDO_MEDIA_FFMPEG lazy_nv12_materialize count={} materialize_us={} source_size={}x{} bitmap_size={}x{}",
                                    materialize_count,
                                    materialize_microseconds,
                                    frame_data->width,
                                    frame_data->height,
                                    materialized_size.width(),
                                    materialized_size.height());
                            }

                            return bitmap;
                        };

                        auto bitmap_microseconds = 0;
                        auto pipeline_microseconds = (MonotonicTime::now() - pipeline_start).to_microseconds();

                        static size_t s_video_frame_pipeline_count { 0 };
                        auto count = ++s_video_frame_pipeline_count;
                        if (count <= 8 || count % 120 == 0) {
                            dbgln("MUNDO_MEDIA_FFMPEG decoded_frame_pipeline count={} frame_id={} hw_transfer={} direct_nv12={} lazy_nv12={} transfer_us={} copy_us={} bitmap_us={} total_us={} frame_format={} bitmap_size={}x{} source_size={}x{} zero_copy_capable={} requires_cpu_transfer={}",
                                count,
                                hardware_descriptor.has_value() ? hardware_descriptor->frame_id : 0,
                                transfer_timing.transferred_from_hardware,
                                false,
                                true,
                                transfer_timing.transfer_microseconds,
                                copy_microseconds,
                                bitmap_microseconds,
                                pipeline_microseconds,
                                pixel_format_name(pixel_format),
                                target_size.width(),
                                target_size.height(),
                                source_width,
                                source_height,
                                hardware_descriptor.has_value() ? hardware_descriptor->zero_copy_capable : false,
                                hardware_descriptor.has_value() ? hardware_descriptor->requires_cpu_transfer : false);
                        }

                        used_lazy_nv12_bitmap = true;
                        auto video_frame = DECODER_TRY_ALLOC(try_make<VideoFrame>(timestamp, duration, size, bit_depth, cicp, move(bitmap_factory), frame_data));
                        if (hardware_descriptor.has_value())
                            video_frame->set_hardware_descriptor(*hardware_descriptor);
                        return video_frame;
                    }

                    dbgln("MUNDO_MEDIA_FFMPEG lazy_nv12_bitmap_fallback size={}x{} error={}", frame->width, frame->height, owned_frame_data.error().string_literal());
                } else {
                    auto direct_bitmap = create_bitmap_directly_from_nv12_frame(frame, cicp);
                    if (!direct_bitmap.is_error()) {
                        used_direct_nv12_bitmap = true;
                        bitmap = direct_bitmap.release_value();
                    } else {
                        dbgln("MUNDO_MEDIA_FFMPEG direct_nv12_bitmap_fallback size={}x{} error={}", frame->width, frame->height, direct_bitmap.error().string_literal());
                    }
                }
            } else {
                static size_t s_skipped_direct_nv12_frame_count { 0 };
                auto skipped_count = ++s_skipped_direct_nv12_frame_count;
                if (skipped_count <= 8 || skipped_count % 120 == 0) {
                    auto reason = should_use_gpu_yuv_for_nv12 ? "gpu_yuv_backend"sv : "direct_nv12_disabled"sv;
                    dbgln("MUNDO_MEDIA_FFMPEG direct_nv12_bitmap_skipped count={} reason={} size={}x{} gpu_yuv_max_pixels={}",
                        skipped_count, reason, frame->width, frame->height, max_gpu_yuv_upload_pixels());
                }
            }
        }

        if (!bitmap) {
            auto yuv_data = DECODER_TRY_ALLOC(Gfx::YUVData::create(gfx_size, bit_depth, subsampling, cicp));
            if (should_use_gpu_yuv_for_nv12)
                yuv_data->set_prefers_gpu_upload(true);

            auto component_size = bit_depth <= 8 ? 1 : 2;
            auto copy_start = MonotonicTime::now();
            if (is_semiplanar_pixel_format(pixel_format))
                DECODER_TRY(DecoderErrorCategory::Unknown, copy_semiplanar_frame_to_yuv_data(frame, *yuv_data, size, subsampling, component_size));
            else
                DECODER_TRY(DecoderErrorCategory::Unknown, copy_planar_frame_to_yuv_data(frame, *yuv_data, size, subsampling, component_size));
            copy_microseconds = (MonotonicTime::now() - copy_start).to_microseconds();

            bitmap = DECODER_TRY_ALLOC(Gfx::ImmutableBitmap::create_from_yuv(move(yuv_data)));
        }
        auto bitmap_microseconds = (MonotonicTime::now() - bitmap_start).to_microseconds();
        auto pipeline_microseconds = (MonotonicTime::now() - pipeline_start).to_microseconds();

        static size_t s_video_frame_pipeline_count { 0 };
        auto count = ++s_video_frame_pipeline_count;
        if (count <= 8 || count % 120 == 0) {
            dbgln("MUNDO_MEDIA_FFMPEG decoded_frame_pipeline count={} frame_id={} hw_transfer={} direct_nv12={} lazy_nv12={} transfer_us={} copy_us={} bitmap_us={} total_us={} frame_format={} bitmap_size={}x{} source_size={}x{} zero_copy_capable={} requires_cpu_transfer={}",
                count,
                hardware_descriptor.has_value() ? hardware_descriptor->frame_id : 0,
                transfer_timing.transferred_from_hardware,
                used_direct_nv12_bitmap,
                used_lazy_nv12_bitmap,
                transfer_timing.transfer_microseconds,
                copy_microseconds,
                bitmap_microseconds,
                pipeline_microseconds,
                pixel_format_name(pixel_format),
                bitmap->width(),
                bitmap->height(),
                frame->width,
                frame->height,
                hardware_descriptor.has_value() ? hardware_descriptor->zero_copy_capable : false,
                hardware_descriptor.has_value() ? hardware_descriptor->requires_cpu_transfer : false);
        }

        auto video_frame = DECODER_TRY_ALLOC(try_make<VideoFrame>(timestamp, duration, size, bit_depth, cicp, bitmap.release_nonnull()));
        if (hardware_descriptor.has_value())
            video_frame->set_hardware_descriptor(*hardware_descriptor);
        return video_frame;
    }
    case AVERROR(EAGAIN):
        return DecoderError::with_description(DecoderErrorCategory::NeedsMoreInput, "FFmpeg decoder has no frames available, send more input"sv);
    case AVERROR_EOF:
        return DecoderError::with_description(DecoderErrorCategory::EndOfStream, "FFmpeg decoder has been flushed"sv);
    case AVERROR(EINVAL):
        return DecoderError::with_description(DecoderErrorCategory::Invalid, "FFmpeg codec has not been opened"sv);
    default:
        return DecoderError::format(DecoderErrorCategory::Unknown, "FFmpeg codec encountered an unexpected error retrieving frames with code {:x}", result);
    }
}

void FFmpegVideoDecoder::flush()
{
    avcodec_flush_buffers(m_codec_context);
}

}
