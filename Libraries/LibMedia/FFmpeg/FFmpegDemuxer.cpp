/*
 * Copyright (c) 2025, Luke Wilde <luke@ladybird.org>
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <AK/Math.h>
#include <AK/MemoryStream.h>
#include <AK/Stream.h>
#include <AK/StringView.h>
#include <AK/Time.h>
#include <LibMedia/CodecID.h>
#include <LibMedia/FFmpeg/FFmpegDemuxer.h>
#include <LibMedia/FFmpeg/FFmpegHelpers.h>
#include <LibMedia/MediaStream.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
}

namespace Media::FFmpeg {

FFmpegDemuxer::FFmpegDemuxer(NonnullRefPtr<MediaStream> const& stream)
    : m_stream(stream)
{
}

FFmpegDemuxer::~FFmpegDemuxer()
{
    for (auto& [track, context] : m_track_contexts) {
        if (context->format_context != nullptr)
            avformat_close_input(&context->format_context);
    }
}

static DecoderErrorOr<bool> stream_looks_like_hls(MediaStreamCursor& cursor)
{
    Array<u8, 512> probe_buffer;

    if (cursor.seek(0, AK::SeekMode::SetPosition).is_error())
        return false;

    auto bytes_read_or_error = cursor.read_into(probe_buffer);
    auto reset_result = cursor.seek(0, AK::SeekMode::SetPosition);
    if (reset_result.is_error())
        return reset_result.release_error();

    if (bytes_read_or_error.is_error())
        return false;

    auto bytes_read = bytes_read_or_error.release_value();
    auto probe = StringView { reinterpret_cast<char const*>(probe_buffer.data()), bytes_read };

    // HLS playlists often arrive through custom IO without a filename or MIME type,
    // so libavformat's normal m3u8 probing can miss them unless we provide the demuxer.
    return probe.starts_with("#EXTM3U"sv) && probe.contains("#EXT-X-"sv);
}

static DecoderErrorOr<void> initialize_format_context(AVFormatContext*& format_context, AVIOContext& io_context, bool force_hls_demuxer)
{
    format_context = avformat_alloc_context();
    if (format_context == nullptr)
        return DecoderError::with_description(DecoderErrorCategory::Memory, "Failed to allocate format context"sv);
    format_context->pb = &io_context;

    AVInputFormat const* input_format = nullptr;
    if (force_hls_demuxer)
        input_format = av_find_input_format("hls");

    dbgln("MUNDO_MEDIA_FFMPEG open_input force_hls={}", force_hls_demuxer);
    auto open_input_result = avformat_open_input(&format_context, nullptr, input_format, nullptr);
    if (open_input_result < 0) {
        dbgln("MUNDO_MEDIA_FFMPEG open_input failed error={} force_hls={}", open_input_result, force_hls_demuxer);
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Failed to open input for format parsing"sv);
    }

    // Read stream info; doing this is required for headerless formats like MPEG
    auto find_stream_info_result = avformat_find_stream_info(format_context, nullptr);
    if (find_stream_info_result < 0) {
        dbgln("MUNDO_MEDIA_FFMPEG find_stream_info failed error={} format={}", find_stream_info_result, format_context->iformat ? format_context->iformat->name : "(null)");
        return DecoderError::with_description(DecoderErrorCategory::Corrupted, "Failed to find stream info"sv);
    }

    dbgln("MUNDO_MEDIA_FFMPEG stream_info format={} streams={} duration={} force_hls={}",
        format_context->iformat ? format_context->iformat->name : "(null)",
        format_context->nb_streams,
        format_context->duration,
        force_hls_demuxer);

    return {};
}

static DecoderErrorOr<Track> create_track_from_stream(AVStream const& stream, StringView format_name, HashTable<TrackType>& seen_types)
{
    auto type = track_type_from_ffmpeg_media_type(stream.codecpar->codec_type);
    auto get_string_metadata = [&](char const* key) {
        auto* name_entry = av_dict_get(stream.metadata, key, nullptr, 0);
        if (name_entry == nullptr)
            return Utf16String();
        return Utf16String::from_utf8(StringView(name_entry->value, strlen(name_entry->value)));
    };

    // https://dev.w3.org/html5/html-sourcing-inband-tracks/
    auto kind = [&] {
        auto is_first_of_type = seen_types.set(type) == HashSetResult::InsertedNewEntry;
        if (format_name.starts_with("mov"sv)) {
            // https://dev.w3.org/html5/html-sourcing-inband-tracks/#mpeg4avta
            // "main": first audio (video) track
            if (is_first_of_type)
                return Track::Kind::Main;
            // "translation": not first audio (video) track
            return Track::Kind::Translation;
        }

        // AD-HOC: For container formats not covered by the spec, default to "main".
        return Track::Kind::Main;
    }();

    auto name = get_string_metadata("title");
    auto language = get_string_metadata("language");
    Track track(type, stream.index, kind, name, language);

    if (type == TrackType::Video) {
        auto color_primaries = static_cast<ColorPrimaries>(stream.codecpar->color_primaries);
        auto transfer_characteristics = static_cast<TransferCharacteristics>(stream.codecpar->color_trc);
        auto matrix_coefficients = static_cast<MatrixCoefficients>(stream.codecpar->color_space);
        auto color_range = [&stream] {
            switch (stream.codecpar->color_range) {
            case AVColorRange::AVCOL_RANGE_MPEG:
                return VideoFullRangeFlag::Studio;
            case AVColorRange::AVCOL_RANGE_JPEG:
                return VideoFullRangeFlag::Full;
            default:
                return VideoFullRangeFlag::Unspecified;
            }
        }();

        track.set_video_data({
            .pixel_width = static_cast<u64>(stream.codecpar->width),
            .pixel_height = static_cast<u64>(stream.codecpar->height),
            .cicp = CodingIndependentCodePoints(color_primaries, transfer_characteristics, matrix_coefficients, color_range),
        });
    } else if (type == TrackType::Audio) {
        auto channel_map = Audio::ChannelMap::invalid();

        auto& channel_layout = stream.codecpar->ch_layout;
        if (channel_layout.nb_channels != 0) {
            auto channel_map_result = av_channel_layout_to_channel_map(channel_layout);
            if (channel_map_result.is_error())
                return DecoderError::with_description(DecoderErrorCategory::Invalid, channel_map_result.error().string_literal());
            channel_map = channel_map_result.release_value();
        }

        auto sample_specification = Audio::SampleSpecification(stream.codecpar->sample_rate, channel_map);

        track.set_audio_data({
            .sample_specification = sample_specification,
        });
    }

    return track;
}

DecoderErrorOr<NonnullRefPtr<FFmpegDemuxer>> FFmpegDemuxer::from_stream(NonnullRefPtr<MediaStream> const& stream)
{
    auto cursor = stream->create_cursor();
    auto force_hls_demuxer = TRY(stream_looks_like_hls(cursor));
    dbgln("MUNDO_MEDIA_FFMPEG from_stream force_hls={}", force_hls_demuxer);
    auto io_context = DECODER_TRY_ALLOC(Media::FFmpeg::FFmpegIOContext::create(cursor));

    AVFormatContext* format_context = nullptr;
    TRY(initialize_format_context(format_context, *io_context->avio_context(), force_hls_demuxer));

    auto demuxer = DECODER_TRY_ALLOC(adopt_nonnull_ref_or_enomem(new (nothrow) FFmpegDemuxer(stream)));
    demuxer->m_total_duration = AK::Duration::from_time_units(format_context->duration, 1, AV_TIME_BASE);

    auto format_name = StringView(format_context->iformat->name, strlen(format_context->iformat->name));
    auto seen_types = HashTable<TrackType>();

    for (u32 i = 0; i < format_context->nb_streams; i++) {
        auto& stream = *format_context->streams[i];

        auto track = TRY(create_track_from_stream(stream, format_name, seen_types));
        auto codec_id = media_codec_id_from_ffmpeg_codec_id(stream.codecpar->codec_id);
        auto codec_initialization_data = DECODER_TRY_ALLOC(ByteBuffer::copy(stream.codecpar->extradata, stream.codecpar->extradata_size));
        dbgln("MUNDO_MEDIA_FFMPEG stream index={} type={} codec={} mapped_codec={} width={} height={} sample_rate={} channels={} extradata={} duration={} time_base={}/{}",
            stream.index,
            static_cast<int>(stream.codecpar->codec_type),
            avcodec_get_name(stream.codecpar->codec_id),
            codec_id,
            stream.codecpar->width,
            stream.codecpar->height,
            stream.codecpar->sample_rate,
            stream.codecpar->ch_layout.nb_channels,
            stream.codecpar->extradata_size,
            stream.duration,
            stream.time_base.num,
            stream.time_base.den);

        AK::Duration duration;
        if (stream.duration >= 0)
            duration = AK::Duration::from_time_units(stream.duration, stream.time_base.num, stream.time_base.den);
        else
            duration = demuxer->m_total_duration;

        DECODER_TRY_ALLOC(demuxer->m_stream_info.try_empend(StreamInfo {
            .track = move(track),
            .codec_id = codec_id,
            .codec_initialization_data = move(codec_initialization_data),
            .duration = duration,
            .time_base_numerator = stream.time_base.num,
            .time_base_denominator = stream.time_base.den,
        }));
    }

    demuxer->m_preferred_track_for_type.fill(-1);
    for (u32 i = 0; i < format_context->nb_streams; i++) {
        auto& stream = *format_context->streams[i];
        auto type = track_type_from_ffmpeg_media_type(stream.codecpar->codec_type);
        auto type_index = to_underlying(type);
        if (type_index >= demuxer->m_preferred_track_for_type.size())
            continue;
        if (demuxer->m_preferred_track_for_type[type_index] >= 0)
            continue;
        if (stream.disposition & AV_DISPOSITION_DEFAULT)
            demuxer->m_preferred_track_for_type[type_index] = static_cast<int>(i);
    }

    avformat_close_input(&format_context);
    return demuxer;
}

DecoderErrorOr<void> FFmpegDemuxer::create_context_for_track(Track const& track)
{
    auto cursor = m_stream->create_cursor();
    auto force_hls_demuxer = TRY(stream_looks_like_hls(cursor));
    dbgln("MUNDO_MEDIA_FFMPEG create_context track_id={} type={} force_hls={}", track.identifier(), to_underlying(track.type()), force_hls_demuxer);
    auto io_context = MUST(Media::FFmpeg::FFmpegIOContext::create(cursor));

    auto track_context = make<TrackContext>(move(cursor), move(io_context), force_hls_demuxer);

    // We've already initialized a format context, so the only way this can fail is OOM.
    MUST(initialize_format_context(track_context->format_context, *track_context->io_context->avio_context(), force_hls_demuxer));

    track_context->packet = av_packet_alloc();
    VERIFY(track_context->packet != nullptr);

    VERIFY(m_track_contexts.set(track, move(track_context)) == HashSetResult::InsertedNewEntry);

    return {};
}

FFmpegDemuxer::StreamInfo const& FFmpegDemuxer::get_track_info(Track const& track) const
{
    return m_stream_info[track.identifier()];
}

FFmpegDemuxer::TrackContext& FFmpegDemuxer::get_track_context(Track const& track)
{
    return *m_track_contexts.get(track).release_value();
}

DecoderErrorOr<void> FFmpegDemuxer::recreate_context_for_track(Track const& track, TrackContext& track_context)
{
    if (track_context.format_context != nullptr)
        avformat_close_input(&track_context.format_context);
    if (track_context.packet != nullptr)
        av_packet_unref(track_context.packet);

    auto cursor = m_stream->create_cursor();
    auto reset_result = cursor->seek(0, AK::SeekMode::SetPosition);
    if (reset_result.is_error())
        return DecoderError::with_description(DecoderErrorCategory::IO, "Failed to reset HLS stream cursor"sv);

    auto io_context = DECODER_TRY_ALLOC(Media::FFmpeg::FFmpegIOContext::create(cursor));

    track_context.cursor = move(cursor);
    track_context.io_context = move(io_context);
    track_context.is_seekable = true;

    TRY(initialize_format_context(track_context.format_context, *track_context.io_context->avio_context(), track_context.force_hls_demuxer));
    track_context.hls_reopen_count++;
    track_context.context_was_recreated = true;
    dbgln("MUNDO_MEDIA_FFMPEG hls_reopen track_id={} count={}", track.identifier(), track_context.hls_reopen_count);
    return {};
}

bool FFmpegDemuxer::consume_context_recreated_flag_for_track(Track const& track)
{
    auto track_context = m_track_contexts.get(track);
    if (!track_context.has_value())
        return false;
    if (!(*track_context)->context_was_recreated)
        return false;
    (*track_context)->context_was_recreated = false;
    return true;
}

static inline AK::Duration time_units_to_duration(i64 time_units, AVRational const& time_base)
{
    VERIFY(time_base.num > 0);
    VERIFY(time_base.den > 0);
    return AK::Duration::from_time_units(time_units, time_base.num, time_base.den);
}

static inline i64 duration_to_time_units(AK::Duration duration, AVRational const& time_base)
{
    VERIFY(time_base.num > 0);
    VERIFY(time_base.den > 0);
    return duration.to_time_units(time_base.num, time_base.den);
}

DecoderErrorOr<AK::Duration> FFmpegDemuxer::total_duration()
{
    return m_total_duration;
}

TimeRanges FFmpegDemuxer::buffered_time_ranges() const
{
    // FIXME: Use the format context's index to determine the buffered ranges from the underlying stream.
    TimeRanges ranges;
    if (!m_total_duration.is_zero())
        ranges.add_range(AK::Duration::zero(), m_total_duration);
    return ranges;
}

DecoderErrorOr<AK::Duration> FFmpegDemuxer::duration_of_track(Track const& track)
{
    auto const& track_info = get_track_info(track);
    return track_info.duration;
}

DecoderErrorOr<Vector<Track>> FFmpegDemuxer::get_tracks_for_type(TrackType type)
{
    Vector<Track> tracks;
    for (auto const& info : m_stream_info) {
        if (info.track.type() == type)
            DECODER_TRY_ALLOC(tracks.try_append(info.track));
    }
    return tracks;
}

DecoderErrorOr<Optional<Track>> FFmpegDemuxer::get_preferred_track_for_type(TrackType type)
{
    auto preferred_index = m_preferred_track_for_type[to_underlying(type)];
    if (preferred_index < 0)
        return OptionalNone();

    return m_stream_info[preferred_index].track;
}

DecoderErrorOr<DemuxerSeekResult> FFmpegDemuxer::seek_to_most_recent_keyframe(Track const& track, AK::Duration timestamp, DemuxerSeekOptions)
{
    auto& track_context = get_track_context(track);
    auto& format_context = *track_context.format_context;

    VERIFY(track.identifier() < format_context.nb_streams);
    auto& stream = *format_context.streams[track.identifier()];
    auto av_timestamp = duration_to_time_units(timestamp, stream.time_base);

    auto seek_succeeded = false;
    if (track_context.is_seekable && av_seek_frame(&format_context, stream.index, av_timestamp, AVSEEK_FLAG_BACKWARD) >= 0)
        seek_succeeded = true;
    if (!seek_succeeded) {
        track_context.is_seekable = false;
        auto av_base_timestamp = duration_to_time_units(timestamp, AV_TIME_BASE_Q);
        if (av_seek_frame(&format_context, -1, av_base_timestamp, AVSEEK_FLAG_BACKWARD) < 0) {
            if (track_context.cursor->is_aborted())
                return DecoderError::format(DecoderErrorCategory::Aborted, "Seek aborted");

            return DecoderError::format(DecoderErrorCategory::Corrupted, "Failed to seek");
        }
    }

    return DemuxerSeekResult::MovedPosition;
}

DecoderErrorOr<CodecID> FFmpegDemuxer::get_codec_id_for_track(Track const& track)
{
    auto const& track_info = get_track_info(track);
    return track_info.codec_id;
}

DecoderErrorOr<ReadonlyBytes> FFmpegDemuxer::get_codec_initialization_data_for_track(Track const& track)
{
    auto const& track_info = get_track_info(track);
    return track_info.codec_initialization_data.bytes();
}

DecoderErrorOr<CodedFrame> FFmpegDemuxer::get_next_sample_for_track(Track const& track)
{
    auto& track_context = get_track_context(track);
    auto& packet = *track_context.packet;

    for (;;) {
        auto& format_context = *track_context.format_context;
        VERIFY(track.identifier() < format_context.nb_streams);
        auto& stream = *format_context.streams[track.identifier()];

        auto read_frame_error = av_read_frame(&format_context, &packet);
        if (read_frame_error < 0) {
            if (track_context.cursor->is_aborted())
                return DecoderError::format(DecoderErrorCategory::Aborted, "Read aborted");

            if (read_frame_error == AVERROR_EOF) {
                if (track_context.force_hls_demuxer && track_context.hls_reopen_count < 8) {
                    dbgln("MUNDO_MEDIA_FFMPEG hls_eof_reopen track_id={} count={}", track.identifier(), track_context.hls_reopen_count + 1);
                    TRY(recreate_context_for_track(track, track_context));
                    continue;
                }
                return DecoderError::format(DecoderErrorCategory::EndOfStream, "End of stream");
            }

            return DecoderError::with_description(DecoderErrorCategory::Corrupted, av_error_code_to_string(read_frame_error));
        }
        if (packet.stream_index != stream.index) {
            av_packet_unref(&packet);
            continue;
        }

        auto auxiliary_data = [&]() -> CodedFrame::AuxiliaryData {
            if (track.type() == TrackType::Video) {
                return CodedVideoFrameData();
            }
            if (track.type() == TrackType::Audio) {
                return CodedAudioFrameData();
            }
            VERIFY_NOT_REACHED();
        }();

        // Copy the packet data so that we have a permanent reference to it whilst the Sample is alive, which allows us
        // to wipe the packet afterwards.
        auto packet_data = DECODER_TRY_ALLOC(ByteBuffer::copy(packet.data, packet.size));

        auto flags = (packet.flags & AV_PKT_FLAG_KEY) != 0 ? FrameFlags::Keyframe : FrameFlags::None;
        auto sample = CodedFrame(
            time_units_to_duration(packet.pts, stream.time_base),
            time_units_to_duration(packet.duration, stream.time_base),
            flags,
            move(packet_data),
            auxiliary_data);

        // Wipe the packet now that the data is safe.
        av_packet_unref(&packet);
        return sample;
    }
}

void FFmpegDemuxer::set_blocking_reads_aborted_for_track(Track const& track)
{
    auto& track_context = get_track_context(track);
    track_context.cursor->abort();
}

void FFmpegDemuxer::reset_blocking_reads_aborted_for_track(Track const& track)
{
    auto& track_context = get_track_context(track);
    track_context.cursor->reset_abort();
}

bool FFmpegDemuxer::is_read_blocked_for_track(Track const& track)
{
    auto& track_context = get_track_context(track);
    return track_context.cursor->is_blocked();
}

FFmpegDemuxer::TrackContext::~TrackContext()
{
    av_packet_free(&packet);
    avformat_free_context(format_context);
}

}
