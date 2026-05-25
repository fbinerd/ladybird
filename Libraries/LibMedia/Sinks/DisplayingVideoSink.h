/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/NonnullRefPtr.h>
#include <AK/Optional.h>
#include <AK/Time.h>
#include <LibGfx/ImmutableBitmap.h>
#include <LibGfx/Size.h>
#include <LibMedia/Export.h>
#include <LibMedia/Forward.h>
#include <LibMedia/Sinks/VideoSink.h>
#include <LibMedia/Sinks/VideoFrameScheduler.h>
#include <LibMedia/TimedImage.h>
#include <LibMedia/Track.h>

namespace Media {

enum class DisplayingVideoSinkUpdateResult : u8 {
    NewFrameAvailable,
    NoChange,
};

class MEDIA_API DisplayingVideoSink final : public VideoSink {
public:
    static ErrorOr<NonnullRefPtr<DisplayingVideoSink>> try_create(NonnullRefPtr<MediaTimeProvider> const&);

    DisplayingVideoSink(NonnullRefPtr<MediaTimeProvider> const&);
    virtual ~DisplayingVideoSink() override;

    void set_time_provider(NonnullRefPtr<MediaTimeProvider> const&);

    virtual void set_provider(Track const&, RefPtr<VideoDataProvider> const&) override;
    RefPtr<VideoDataProvider> provider(Track const&) const override;

    // Updates the frame returned by current_frame() based on the time provider's current timestamp.
    //
    // Note that push_frame may block until update() is called, so do not call them from the same thread.
    DisplayingVideoSinkUpdateResult update();
    void prepare_current_frame_for_next_update();
    RefPtr<Gfx::ImmutableBitmap> current_frame();
    VideoFrame const* current_video_frame() const;
    Optional<Gfx::Size<u32>> current_frame_size() const;

    void pause_updates();
    void resume_updates();

    Function<void()> m_on_start_buffering;

private:
    static constexpr size_t DEFAULT_QUEUE_SIZE = 8;

    void verify_track(Track const&) const;
    void log_runtime_video_snapshot(char const* note, AK::Duration current_time) const;
    void adjust_smoothness_after_present(VideoFrameSchedulerConfig const&, AK::Duration wall_delta, AK::Duration frame_age);
    size_t smoothness_adjusted_catch_up_budget(VideoFrameSchedulerConfig const&, size_t base_budget, AK::Duration frame_age) const;

    NonnullRefPtr<MediaTimeProvider> m_time_provider;
    RefPtr<VideoDataProvider> m_provider;
    Optional<Track> m_track;

    TimedImage m_next_frame;
    TimedImage m_current_frame;
    size_t m_update_count { 0 };
    size_t m_empty_provider_frame_count { 0 };
    size_t m_presented_frame_count { 0 };
    size_t m_dropped_late_frame_count { 0 };
    size_t m_consecutive_late_frame_drop_count { 0 };
    size_t m_late_drop_limit_hit_count { 0 };
    size_t m_drain_log_count { 0 };
    size_t m_cadence_gap_log_count { 0 };
    size_t m_coalesced_frame_count { 0 };
    size_t m_hard_late_coalesce_count { 0 };
    size_t m_gradual_catch_up_count { 0 };
    size_t m_smoothness_penalty { 0 };
    size_t m_smoothness_stable_present_count { 0 };
    size_t m_smoothness_adaptation_log_count { 0 };
    AK::Duration m_last_present_wall_delta { AK::Duration::zero() };
    AK::Duration m_last_present_media_delta { AK::Duration::zero() };
    AK::Duration m_last_present_frame_delta { AK::Duration::zero() };
    AK::Duration m_last_present_frame_age { AK::Duration::zero() };
    Optional<MonotonicTime> m_last_present_wall_time;
    Optional<AK::Duration> m_last_present_media_time;
    Optional<AK::Duration> m_last_present_frame_time;
    bool m_pause_updates { false };
    bool m_has_new_current_frame { false };
};

}
