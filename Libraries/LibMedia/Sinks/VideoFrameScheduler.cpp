/*
 * Copyright (c) 2026, Fabiano Nascimento <fabiano@fbinerd.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <LibMedia/RuntimeConfiguration.h>
#include <LibMedia/Sinks/VideoFrameScheduler.h>

namespace Media {

VideoFrameScheduler VideoFrameScheduler::from_runtime()
{
    VideoFrameSchedulerConfig config;
    config.late_frame_age_threshold = RuntimeConfiguration::duration_ms("MUNDO_VIDEO_LATE_DROP_MS", AK::Duration::max(), 16);
    config.hard_late_frame_coalesce_threshold = RuntimeConfiguration::duration_ms("MUNDO_VIDEO_HARD_LATE_COALESCE_MS", AK::Duration::max(), 16);
    config.gradual_catch_up_age_threshold = RuntimeConfiguration::duration_ms("MUNDO_VIDEO_GRADUAL_CATCH_UP_AGE_MS", AK::Duration::from_milliseconds(500), 16);
    config.cadence_gap_log_threshold = RuntimeConfiguration::duration_ms("MUNDO_VIDEO_CADENCE_GAP_LOG_MS", AK::Duration::from_milliseconds(120), 16);
    config.max_consecutive_late_frame_drops = static_cast<size_t>(RuntimeConfiguration::integer("MUNDO_VIDEO_MAX_LATE_DROPS", 24, 0, 1000000));
    config.drain_log_threshold = static_cast<size_t>(RuntimeConfiguration::integer("MUNDO_VIDEO_SINK_DRAIN_LOG_THRESHOLD", 4, 0, 1000000));
    config.gradual_catch_up_max_frames = static_cast<size_t>(RuntimeConfiguration::integer("MUNDO_VIDEO_GRADUAL_CATCH_UP_MAX_FRAMES", 3, 1, 12));
    config.gradual_catch_up_fullness_percent = static_cast<size_t>(RuntimeConfiguration::integer("MUNDO_VIDEO_GRADUAL_CATCH_UP_FULLNESS_PERCENT", 85, 1, 100));
    config.present_one_frame_per_update = RuntimeConfiguration::flag_enabled("MUNDO_VIDEO_SINK_PRESENT_ONE_PER_UPDATE", true);
    config.coalesce_due_frames_per_update = RuntimeConfiguration::flag_enabled("MUNDO_VIDEO_SINK_COALESCE_DUE_FRAMES", false);
    config.gradual_catch_up_enabled = RuntimeConfiguration::flag_enabled("MUNDO_VIDEO_GRADUAL_CATCH_UP", true);
    return VideoFrameScheduler { config };
}

VideoFrameScheduler::VideoFrameScheduler(VideoFrameSchedulerConfig config)
    : m_config(config)
{
}

bool VideoFrameScheduler::should_hard_coalesce_late_frame(bool has_current_frame, AK::Duration frame_age) const
{
    return has_current_frame && frame_age > m_config.hard_late_frame_coalesce_threshold;
}

bool VideoFrameScheduler::should_drop_late_frame(bool has_current_frame, AK::Duration frame_age, size_t consecutive_late_drops) const
{
    if (!has_current_frame || frame_age <= m_config.late_frame_age_threshold)
        return false;
    if (m_config.max_consecutive_late_frame_drops == 0)
        return true;
    return consecutive_late_drops < m_config.max_consecutive_late_frame_drops;
}

bool VideoFrameScheduler::should_report_late_drop_limit(size_t consecutive_late_drops) const
{
    return m_config.max_consecutive_late_frame_drops > 0
        && consecutive_late_drops >= m_config.max_consecutive_late_frame_drops;
}

bool VideoFrameScheduler::should_try_gradual_catch_up(bool has_current_frame, AK::Duration frame_age, size_t queue_size, size_t queue_max_size) const
{
    if (!has_current_frame || !m_config.gradual_catch_up_enabled)
        return false;
    if (!m_config.present_one_frame_per_update || m_config.coalesce_due_frames_per_update)
        return false;
    if (frame_age <= m_config.gradual_catch_up_age_threshold)
        return false;
    if (queue_max_size == 0)
        return false;
    return queue_size * 100 >= queue_max_size * m_config.gradual_catch_up_fullness_percent;
}

}
