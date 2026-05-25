/*
 * Copyright (c) 2026, Fabiano Nascimento <fabiano@fbinerd.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#pragma once

#include <AK/Time.h>
#include <LibMedia/Export.h>

namespace Media {

struct MEDIA_API VideoFrameSchedulerConfig {
    AK::Duration late_frame_age_threshold { AK::Duration::max() };
    AK::Duration hard_late_frame_coalesce_threshold { AK::Duration::max() };
    AK::Duration gradual_catch_up_age_threshold { AK::Duration::from_milliseconds(500) };
    AK::Duration gradual_catch_up_target_age { AK::Duration::from_milliseconds(600) };
    AK::Duration cadence_gap_log_threshold { AK::Duration::from_milliseconds(120) };
    size_t max_consecutive_late_frame_drops { 24 };
    size_t drain_log_threshold { 4 };
    size_t gradual_catch_up_max_frames { 3 };
    size_t gradual_catch_up_burst_max_frames { 4 };
    size_t gradual_catch_up_fullness_percent { 85 };
    bool present_one_frame_per_update { true };
    bool coalesce_due_frames_per_update { false };
    bool gradual_catch_up_enabled { true };
};

class MEDIA_API VideoFrameScheduler {
public:
    static VideoFrameScheduler from_runtime();

    explicit VideoFrameScheduler(VideoFrameSchedulerConfig);

    VideoFrameSchedulerConfig const& config() const { return m_config; }

    bool should_hard_coalesce_late_frame(bool has_current_frame, AK::Duration frame_age) const;
    bool should_drop_late_frame(bool has_current_frame, AK::Duration frame_age, size_t consecutive_late_drops) const;
    bool should_report_late_drop_limit(size_t consecutive_late_drops) const;
    bool should_try_gradual_catch_up(bool has_current_frame, AK::Duration frame_age, size_t queue_size, size_t queue_max_size) const;
    size_t gradual_catch_up_frame_budget(AK::Duration frame_age, AK::Duration estimated_frame_duration) const;

private:
    VideoFrameSchedulerConfig m_config;
};

}
