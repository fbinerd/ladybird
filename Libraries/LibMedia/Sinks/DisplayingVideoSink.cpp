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

namespace Media {

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
        dbgln("MUNDO_MEDIA_VIDEO_SINK update count={} track_id={} current_time={}ms has_next={} has_current={}", m_update_count, m_track.has_value() ? m_track.value().identifier() : 0, current_time.to_milliseconds(), m_next_frame.is_valid(), m_current_frame.ptr());
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
        m_current_frame = m_next_frame.release_image();
        m_presented_frame_count++;
        if (m_presented_frame_count <= 8 || m_presented_frame_count % 60 == 0)
            dbgln("MUNDO_MEDIA_VIDEO_SINK present_frame count={} track_id={} current_time={}ms frame={} size={}x{}", m_presented_frame_count, m_track.has_value() ? m_track.value().identifier() : 0, current_time.to_milliseconds(), static_cast<void const*>(m_current_frame.ptr()), m_current_frame->size().width(), m_current_frame->size().height());
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
    return m_current_frame;
}

void DisplayingVideoSink::pause_updates()
{
    m_pause_updates = true;
}

void DisplayingVideoSink::resume_updates()
{
    m_next_frame.clear();
    m_current_frame = nullptr;
    m_pause_updates = false;
    m_has_new_current_frame = true;
    prepare_current_frame_for_next_update();
}

}
