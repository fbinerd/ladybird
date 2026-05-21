/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Debug.h>
#include <LibMedia/Demuxer.h>
#include <LibMedia/Providers/MediaTimeProvider.h>
#include <LibMedia/Providers/VideoDataProvider.h>

#include "DisplayingVideoSink.h"

#include <stdlib.h>
#include <strings.h>

namespace Media {

static AK::Duration late_frame_age_threshold()
{
    auto const* raw_value = getenv("MUNDO_VIDEO_LATE_DROP_MS");
    if (!raw_value)
        return AK::Duration::max();

    auto value = atoi(raw_value);
    if (value <= 0)
        return AK::Duration::max();

    return AK::Duration::from_milliseconds(max(value, 16));
}

static AK::Duration hard_late_frame_coalesce_threshold()
{
    auto const* raw_value = getenv("MUNDO_VIDEO_HARD_LATE_COALESCE_MS");
    if (!raw_value)
        return AK::Duration::max();

    auto value = atoi(raw_value);
    if (value <= 0)
        return AK::Duration::max();

    return AK::Duration::from_milliseconds(max(value, 16));
}

static size_t max_consecutive_late_frame_drops()
{
    auto const* raw_value = getenv("MUNDO_VIDEO_MAX_LATE_DROPS");
    if (!raw_value)
        return 24;

    auto value = atoi(raw_value);
    if (value <= 0)
        return 0;

    return static_cast<size_t>(value);
}

static size_t video_sink_drain_log_threshold()
{
    auto const* raw_value = getenv("MUNDO_VIDEO_SINK_DRAIN_LOG_THRESHOLD");
    if (!raw_value)
        return 4;

    auto value = atoi(raw_value);
    if (value <= 0)
        return 0;

    return static_cast<size_t>(value);
}

static bool present_one_frame_per_update()
{
    auto const* raw_value = getenv("MUNDO_VIDEO_SINK_PRESENT_ONE_PER_UPDATE");
    if (!raw_value)
        return true;

    return strcasecmp(raw_value, "0") != 0
        && strcasecmp(raw_value, "false") != 0
        && strcasecmp(raw_value, "no") != 0
        && strcasecmp(raw_value, "off") != 0;
}

static bool coalesce_due_frames_per_update()
{
    auto const* raw_value = getenv("MUNDO_VIDEO_SINK_COALESCE_DUE_FRAMES");
    if (!raw_value)
        return false;

    return strcasecmp(raw_value, "0") != 0
        && strcasecmp(raw_value, "false") != 0
        && strcasecmp(raw_value, "no") != 0
        && strcasecmp(raw_value, "off") != 0;
}

static AK::Duration cadence_gap_log_threshold()
{
    auto const* raw_value = getenv("MUNDO_VIDEO_CADENCE_GAP_LOG_MS");
    if (!raw_value)
        return AK::Duration::from_milliseconds(120);

    auto value = atoi(raw_value);
    if (value <= 0)
        return AK::Duration::max();

    return AK::Duration::from_milliseconds(max(value, 16));
}

ErrorOr<NonnullRefPtr<DisplayingVideoSink>> DisplayingVideoSink::try_create(NonnullRefPtr<MediaTimeProvider> const& time_provider)
{
    return TRY(try_make_ref_counted<DisplayingVideoSink>(time_provider));
}

DisplayingVideoSink::DisplayingVideoSink(NonnullRefPtr<MediaTimeProvider> const& time_provider)
    : m_time_provider(time_provider)
{
}

DisplayingVideoSink::~DisplayingVideoSink() = default;

void DisplayingVideoSink::set_time_provider(NonnullRefPtr<MediaTimeProvider> const& provider)
{
    m_time_provider = provider;
}

void DisplayingVideoSink::verify_track(Track const& track) const
{
    if (m_provider == nullptr)
        return;
    VERIFY(m_track.has_value());
    VERIFY(m_track.value() == track);
}

void DisplayingVideoSink::set_provider(Track const& track, RefPtr<VideoDataProvider> const& provider)
{
    verify_track(track);
    m_track = track;
    m_provider = provider;
    dbgln("MUNDO_MEDIA_VIDEO_SINK set_provider track_id={} provider={}", track.identifier(), provider.ptr());
    if (provider != nullptr)
        provider->start();
}

RefPtr<VideoDataProvider> DisplayingVideoSink::provider(Track const& track) const
{
    verify_track(track);
    return m_provider;
}

DisplayingVideoSinkUpdateResult DisplayingVideoSink::update()
{
    m_update_count++;
    if (m_provider == nullptr)
        return DisplayingVideoSinkUpdateResult::NoChange;
    if (m_pause_updates)
        return DisplayingVideoSinkUpdateResult::NoChange;

    auto current_time = m_time_provider->current_time();
    if (m_update_count <= 8 || m_update_count % 120 == 0)
        dbgln("MUNDO_MEDIA_VIDEO_SINK update count={} track_id={} current_time={}ms has_next={} has_current={} current_lazy={}", m_update_count, m_track.has_value() ? m_track.value().identifier() : 0, current_time.to_milliseconds(), m_next_frame.is_valid(), m_current_frame.is_valid(), m_current_frame.has_lazy_bitmap());
    auto result = DisplayingVideoSinkUpdateResult::NoChange;
    size_t retrieved_this_update = 0;
    size_t presented_this_update = 0;
    size_t dropped_late_this_update = 0;
    size_t coalesced_this_update = 0;
    bool saw_provider_empty_this_update = false;
    TimedImage latest_presentable_frame;
    if (m_has_new_current_frame) {
        result = DisplayingVideoSinkUpdateResult::NewFrameAvailable;
        m_has_new_current_frame = false;
    }

    auto present_frame = [&](TimedImage&& frame) {
        auto presented_frame_time = frame.timestamp();
        m_current_frame = move(frame);
        m_consecutive_late_frame_drop_count = 0;
        m_presented_frame_count++;
        presented_this_update++;
        auto now = MonotonicTime::now();
        if (m_last_present_wall_time.has_value()) {
            auto wall_delta = now - m_last_present_wall_time.value();
            auto media_delta = current_time - m_last_present_media_time.value();
            auto frame_delta = presented_frame_time - m_last_present_frame_time.value();
            auto frame_age = current_time - presented_frame_time;
            auto cadence_threshold = cadence_gap_log_threshold();
            auto media_delta_threshold = AK::Duration::from_milliseconds(cadence_threshold.to_milliseconds() * 2);
            if (wall_delta > cadence_threshold || media_delta > media_delta_threshold) {
                m_cadence_gap_log_count++;
                if (m_cadence_gap_log_count <= 24 || m_cadence_gap_log_count % 60 == 0)
                    dbgln("MUNDO_MEDIA_VIDEO_SINK present_cadence_gap count={} present_count={} track_id={} wall_delta={}ms media_delta={}ms frame_delta={}ms current_time={}ms frame_time={}ms age={}ms threshold={}ms queue_had_next={} lazy_bitmap={}",
                        m_cadence_gap_log_count,
                        m_presented_frame_count,
                        m_track.has_value() ? m_track.value().identifier() : 0,
                        wall_delta.to_milliseconds(),
                        media_delta.to_milliseconds(),
                        frame_delta.to_milliseconds(),
                        current_time.to_milliseconds(),
                        presented_frame_time.to_milliseconds(),
                        frame_age.to_milliseconds(),
                        cadence_threshold.to_milliseconds(),
                        m_next_frame.is_valid(),
                        m_current_frame.has_lazy_bitmap());
            }
        }
        m_last_present_wall_time = now;
        m_last_present_media_time = current_time;
        m_last_present_frame_time = presented_frame_time;
        if (m_presented_frame_count <= 8 || m_presented_frame_count % 60 == 0) {
            auto size = m_current_frame.size();
            dbgln("MUNDO_MEDIA_VIDEO_SINK present_frame count={} track_id={} current_time={}ms size={}x{} lazy_bitmap={}", m_presented_frame_count, m_track.has_value() ? m_track.value().identifier() : 0, current_time.to_milliseconds(), size.width(), size.height(), m_current_frame.has_lazy_bitmap());
        }
        result = DisplayingVideoSinkUpdateResult::NewFrameAvailable;
    };

    while (true) {
        if (!m_next_frame.is_valid()) {
            m_next_frame = m_provider->retrieve_frame();
            if (!m_next_frame.is_valid()) {
                saw_provider_empty_this_update = true;
                m_empty_provider_frame_count++;
                if (m_empty_provider_frame_count <= 8 || m_empty_provider_frame_count % 120 == 0)
                    dbgln("MUNDO_MEDIA_VIDEO_SINK provider_empty count={} track_id={} blocked={}", m_empty_provider_frame_count, m_track.has_value() ? m_track.value().identifier() : 0, m_provider->is_blocked());
                if (m_provider->is_blocked() && m_on_start_buffering)
                    m_on_start_buffering();
                break;
            }
            retrieved_this_update++;
        }
        if (m_next_frame.timestamp() > current_time)
            break;

        auto frame_age = current_time - m_next_frame.timestamp();
        auto late_frame_threshold = late_frame_age_threshold();
        auto late_frame_drop_limit = max_consecutive_late_frame_drops();
        auto hard_late_threshold = hard_late_frame_coalesce_threshold();
        if (m_current_frame.is_valid() && frame_age > hard_late_threshold) {
            TimedImage newest_due_frame;
            size_t hard_coalesced_frames = 0;
            auto oldest_late_frame_time = m_next_frame.timestamp();
            auto oldest_late_frame_age = frame_age;

            while (m_next_frame.is_valid() && m_next_frame.timestamp() <= current_time) {
                if (newest_due_frame.is_valid())
                    hard_coalesced_frames++;
                newest_due_frame = move(m_next_frame);
                m_next_frame = m_provider->retrieve_frame();
                if (!m_next_frame.is_valid()) {
                    saw_provider_empty_this_update = true;
                    m_empty_provider_frame_count++;
                    if (m_empty_provider_frame_count <= 8 || m_empty_provider_frame_count % 120 == 0)
                        dbgln("MUNDO_MEDIA_VIDEO_SINK provider_empty count={} track_id={} blocked={}", m_empty_provider_frame_count, m_track.has_value() ? m_track.value().identifier() : 0, m_provider->is_blocked());
                    if (m_provider->is_blocked() && m_on_start_buffering)
                        m_on_start_buffering();
                    break;
                }
                retrieved_this_update++;
            }

            if (newest_due_frame.is_valid()) {
                m_hard_late_coalesce_count++;
                coalesced_this_update += hard_coalesced_frames;
                if (m_hard_late_coalesce_count <= 24 || m_hard_late_coalesce_count % 60 == 0)
                    dbgln("MUNDO_MEDIA_VIDEO_SINK hard_late_coalesce count={} track_id={} current_time={}ms oldest_frame={}ms oldest_age={}ms presented_frame={}ms presented_age={}ms threshold={}ms coalesced={} provider_empty={} has_next={}",
                        m_hard_late_coalesce_count,
                        m_track.has_value() ? m_track.value().identifier() : 0,
                        current_time.to_milliseconds(),
                        oldest_late_frame_time.to_milliseconds(),
                        oldest_late_frame_age.to_milliseconds(),
                        newest_due_frame.timestamp().to_milliseconds(),
                        (current_time - newest_due_frame.timestamp()).to_milliseconds(),
                        hard_late_threshold.to_milliseconds(),
                        hard_coalesced_frames,
                        saw_provider_empty_this_update,
                        m_next_frame.is_valid());
                present_frame(move(newest_due_frame));
                break;
            }
        }

        if (m_current_frame.is_valid() && frame_age > late_frame_threshold) {
            if (late_frame_drop_limit > 0 && m_consecutive_late_frame_drop_count >= late_frame_drop_limit) {
                m_late_drop_limit_hit_count++;
                if (m_late_drop_limit_hit_count <= 16 || m_late_drop_limit_hit_count % 60 == 0)
                    dbgln("MUNDO_MEDIA_VIDEO_SINK late_drop_limit_hit count={} limit={} track_id={} current_time={}ms frame_time={}ms age={}ms action=coalesce_late_frame", m_late_drop_limit_hit_count, late_frame_drop_limit, m_track.has_value() ? m_track.value().identifier() : 0, current_time.to_milliseconds(), m_next_frame.timestamp().to_milliseconds(), frame_age.to_milliseconds());
            } else {
                m_dropped_late_frame_count++;
                m_consecutive_late_frame_drop_count++;
                if (m_dropped_late_frame_count <= 8 || m_dropped_late_frame_count % 60 == 0)
                    dbgln("MUNDO_MEDIA_VIDEO_SINK drop_late_frame count={} consecutive={} limit={} track_id={} current_time={}ms frame_time={}ms age={}ms threshold={}ms", m_dropped_late_frame_count, m_consecutive_late_frame_drop_count, late_frame_drop_limit, m_track.has_value() ? m_track.value().identifier() : 0, current_time.to_milliseconds(), m_next_frame.timestamp().to_milliseconds(), frame_age.to_milliseconds(), late_frame_threshold.to_milliseconds());
                if (m_consecutive_late_frame_drop_count == late_frame_drop_limit && late_frame_drop_limit > 0) {
                    m_late_drop_limit_hit_count++;
                    dbgln("MUNDO_MEDIA_VIDEO_SINK late_drop_limit_hit count={} limit={} track_id={} current_time={}ms frame_time={}ms age={}ms action=coalesce_next_frame", m_late_drop_limit_hit_count, late_frame_drop_limit, m_track.has_value() ? m_track.value().identifier() : 0, current_time.to_milliseconds(), m_next_frame.timestamp().to_milliseconds(), frame_age.to_milliseconds());
                }
                m_next_frame.clear();
                dropped_late_this_update++;
                continue;
            }
        }

        if (present_one_frame_per_update() && coalesce_due_frames_per_update()) {
            if (latest_presentable_frame.is_valid())
                coalesced_this_update++;
            latest_presentable_frame = move(m_next_frame);
            continue;
        }

        present_frame(move(m_next_frame));
        if (present_one_frame_per_update())
            break;
    }
    if (latest_presentable_frame.is_valid())
        present_frame(move(latest_presentable_frame));
    if (coalesced_this_update > 0) {
        m_coalesced_frame_count += coalesced_this_update;
        if (m_coalesced_frame_count <= 24 || m_coalesced_frame_count % 60 == 0)
            dbgln("MUNDO_MEDIA_VIDEO_SINK coalesce_due_frames total={} this_update={} track_id={} current_time={}ms presented_frame={}ms provider_empty={} has_next={}",
                m_coalesced_frame_count,
                coalesced_this_update,
                m_track.has_value() ? m_track.value().identifier() : 0,
                current_time.to_milliseconds(),
                m_current_frame.is_valid() ? m_current_frame.timestamp().to_milliseconds() : 0,
                saw_provider_empty_this_update,
                m_next_frame.is_valid());
    }
    auto drain_log_threshold = video_sink_drain_log_threshold();
    if (drain_log_threshold > 0 && (retrieved_this_update >= drain_log_threshold || dropped_late_this_update > 0 || (saw_provider_empty_this_update && retrieved_this_update > 0))) {
        m_drain_log_count++;
        if (m_drain_log_count <= 16 || m_drain_log_count % 60 == 0)
            dbgln("MUNDO_MEDIA_VIDEO_SINK update_drain count={} track_id={} current_time={}ms retrieved={} presented={} coalesced={} dropped_late={} provider_empty={} has_next={} next_time={}ms has_current={} current_time_frame={}ms result={}",
                m_drain_log_count,
                m_track.has_value() ? m_track.value().identifier() : 0,
                current_time.to_milliseconds(),
                retrieved_this_update,
                presented_this_update,
                coalesced_this_update,
                dropped_late_this_update,
                saw_provider_empty_this_update,
                m_next_frame.is_valid(),
                m_next_frame.is_valid() ? m_next_frame.timestamp().to_milliseconds() : 0,
                m_current_frame.is_valid(),
                m_current_frame.is_valid() ? m_current_frame.timestamp().to_milliseconds() : 0,
                result == DisplayingVideoSinkUpdateResult::NewFrameAvailable ? "new-frame" : "no-change");
    }
    return result;
}

void DisplayingVideoSink::prepare_current_frame_for_next_update()
{
    auto update_result = update();
    if (update_result == DisplayingVideoSinkUpdateResult::NewFrameAvailable)
        m_has_new_current_frame = true;
}

RefPtr<Gfx::ImmutableBitmap> DisplayingVideoSink::current_frame()
{
    if (!m_current_frame.is_valid())
        return nullptr;
    static size_t s_current_frame_request_count { 0 };
    auto count = ++s_current_frame_request_count;
    if (count <= 16 || count % 120 == 0) {
        auto size = m_current_frame.size();
        dbgln("MUNDO_MEDIA_VIDEO_SINK current_frame_request count={} track_id={} timestamp={}ms size={}x{} lazy_bitmap={}",
            count,
            m_track.has_value() ? m_track.value().identifier() : 0,
            m_current_frame.timestamp().to_milliseconds(),
            size.width(),
            size.height(),
            m_current_frame.has_lazy_bitmap());
    }
    return m_current_frame.image();
}

VideoFrame const* DisplayingVideoSink::current_video_frame() const
{
    if (!m_current_frame.is_valid())
        return nullptr;
    return m_current_frame.video_frame();
}

Optional<Gfx::Size<u32>> DisplayingVideoSink::current_frame_size() const
{
    if (!m_current_frame.is_valid())
        return {};
    return m_current_frame.size();
}

void DisplayingVideoSink::pause_updates()
{
    m_pause_updates = true;
}

void DisplayingVideoSink::resume_updates()
{
    m_next_frame.clear();
    m_current_frame.clear();
    m_pause_updates = false;
    m_has_new_current_frame = true;
    prepare_current_frame_for_next_update();
}

}
