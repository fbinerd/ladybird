/*
 * Copyright (c) 2024, Gregory Bertilson <zaggy1024@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibCore/System.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/YUVData.h>
#include <LibMedia/VideoFrame.h>

#include "FFmpegHelpers.h"
#include "FFmpegVideoDecoder.h"

#include <stdlib.h>
#include <string.h>

extern "C" {
#include <libavutil/hwcontext.h>
#include <libavutil/pixdesc.h>
}

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
    dbgln("MUNDO_MEDIA_FFMPEG video_decoder_backend requested={} codec={} active=software",
        video_decoder_backend_name(requested_backend), codec_id);

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
    if (requested_backend != VideoDecoderBackend::Nvdec)
        return false;

    return codec_has_hw_config(codec, VideoDecoderBackend::Nvdec);
}

static bool is_hardware_frame(AVFrame const* frame)
{
    return frame->format == AV_PIX_FMT_CUDA;
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

static DecoderErrorOr<AVFrame*> software_frame_for_decoded_frame(AVFrame* frame, AVFrame* transfer_frame)
{
    if (!is_hardware_frame(frame))
        return frame;

    av_frame_unref(transfer_frame);
    auto result = av_hwframe_transfer_data(transfer_frame, frame, 0);
    if (result < 0)
        return DecoderError::format(DecoderErrorCategory::Unknown, "Failed to transfer FFmpeg hardware frame to CPU with code {:x}", result);

    result = av_frame_copy_props(transfer_frame, frame);
    if (result < 0)
        return DecoderError::format(DecoderErrorCategory::Unknown, "Failed to copy FFmpeg hardware frame properties with code {:x}", result);

    static size_t s_hw_transfer_frame_count { 0 };
    auto count = ++s_hw_transfer_frame_count;
    if (count <= 8 || count % 120 == 0) {
        dbgln("MUNDO_MEDIA_FFMPEG hwaccel_transfer backend=nvdec count={} hw_format={} sw_format={} size={}x{}",
            count,
            av_get_pix_fmt_name(static_cast<AVPixelFormat>(frame->format)),
            av_get_pix_fmt_name(static_cast<AVPixelFormat>(transfer_frame->format)),
            transfer_frame->width,
            transfer_frame->height);
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
    codec_context->thread_count = static_cast<int>(min(Core::System::hardware_concurrency(), 16));
    dbgln("MUNDO_MEDIA_FFMPEG video_decoder_threads codec={} threads={}", codec_id, codec_context->thread_count);

    if (should_try_nvdec(codec)) {
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
        if (!is_hardware_frame(m_frame))
            log_software_frame_while_hwaccel_requested(m_codec_context, m_frame);
        auto* frame = DECODER_TRY(DecoderErrorCategory::Unknown, software_frame_for_decoded_frame(m_frame, m_transfer_frame));

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

        auto yuv_data = DECODER_TRY_ALLOC(Gfx::YUVData::create(gfx_size, bit_depth, subsampling, cicp));

        auto component_size = bit_depth <= 8 ? 1 : 2;
        if (is_semiplanar_pixel_format(pixel_format))
            DECODER_TRY(DecoderErrorCategory::Unknown, copy_semiplanar_frame_to_yuv_data(frame, *yuv_data, size, subsampling, component_size));
        else
            DECODER_TRY(DecoderErrorCategory::Unknown, copy_planar_frame_to_yuv_data(frame, *yuv_data, size, subsampling, component_size));

        auto bitmap = DECODER_TRY_ALLOC(Gfx::ImmutableBitmap::create_from_yuv(move(yuv_data)));

        return DECODER_TRY_ALLOC(try_make<VideoFrame>(timestamp, duration, size, bit_depth, cicp, move(bitmap)));
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
