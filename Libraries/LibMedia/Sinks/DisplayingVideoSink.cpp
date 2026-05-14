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

namespace Media {

static AK::Duration late_frame_age_threshold()
{
    auto const* raw_value = getenv("MUNDO_VIDEO_LATE_DROP_MS");
    if (!raw_value)
        return AK::Duration::from_milliseconds(250);

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
    if (m_has_new_current_frame) {
        result = DisplayingVideoSinkUpdateResult::NewFrameAvailable;
        m_has_new_current_frame = false;
    }

    while (true) {
        if (!m_next_frame.is_valid()) {
            m_next_frame = m_provider->retrieve_frame();
            if (!m_next_frame.is_valid()) {
                m_empty_provider_frame_count++;
                if (m_empty_provider_frame_count <= 8 || m_empty_provider_frame_count % 120 == 0)
                    dbgln("MUNDO_MEDIA_VIDEO_SINK provider_empty count={} track_id={} blocked={}", m_empty_provider_frame_count, m_track.has_value() ? m_track.value().identifier() : 0, m_provider->is_blocked());
                if (m_provider->is_blocked() && m_on_start_buffering)
                    m_on_start_buffering();
                break;
            }
        }
        if (m_next_frame.timestamp() > current_time)
            break;

        auto frame_age = current_time - m_next_frame.timestamp();
        auto late_frame_threshold = late_frame_age_threshold();
        auto late_frame_drop_limit = max_consecutive_late_frame_drops();
        if (m_current_frame.is_valid() && frame_age > late_frame_threshold) {
            m_dropped_late_frame_count++;
            m_consecutive_late_frame_drop_count++;
            if (m_dropped_late_frame_count <= 8 || m_dropped_late_frame_count % 60 == 0)
                dbgln("MUNDO_MEDIA_VIDEO_SINK drop_late_frame count={} consecutive={} limit={} track_id={} current_time={}ms frame_time={}ms age={}ms threshold={}ms", m_dropped_late_frame_count, m_consecutive_late_frame_drop_count, late_frame_drop_limit, m_track.has_value() ? m_track.value().identifier() : 0, current_time.to_milliseconds(), m_next_frame.timestamp().to_milliseconds(), frame_age.to_milliseconds(), late_frame_threshold.to_milliseconds());
            if (m_consecutive_late_frame_drop_count == late_frame_drop_limit && late_frame_drop_limit > 0) {
                m_late_drop_limit_hit_count++;
                dbgln("MUNDO_MEDIA_VIDEO_SINK late_drop_limit_hit count={} limit={} track_id={} current_time={}ms frame_time={}ms age={}ms action=continue_dropping", m_late_drop_limit_hit_count, late_frame_drop_limit, m_track.has_value() ? m_track.value().identifier() : 0, current_time.to_milliseconds(), m_next_frame.timestamp().to_milliseconds(), frame_age.to_milliseconds());
            }
            m_next_frame.clear();
            continue;
        }

        m_current_frame = move(m_next_frame);
        m_consecutive_late_frame_drop_count = 0;
        m_presented_frame_count++;
        if (m_presented_frame_count <= 8 || m_presented_frame_count % 60 == 0) {
            auto size = m_current_frame.size();
            dbgln("MUNDO_MEDIA_VIDEO_SINK present_frame count={} track_id={} current_time={}ms size={}x{} lazy_bitmap={}", m_presented_frame_count, m_track.has_value() ? m_track.value().identifier() : 0, current_time.to_milliseconds(), size.width(), size.height(), m_current_frame.has_lazy_bitmap());
        }
        result = DisplayingVideoSinkUpdateResult::NewFrameAvailable;
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
    return m_current_frame.image();
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
