/*
 * Copyright (c) 2025, Gregory Bertilson <gregory@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/Time.h>
#include <LibMedia/Audio/PlaybackStream.h>
#include <LibMedia/Providers/AudioDataProvider.h>
#include <stdlib.h>
#include <strings.h>

#include "AudioMixingSink.h"

namespace Media {

static u32 mundo_audio_target_latency_ms()
{
    auto const* raw_value = getenv("MUNDO_AUDIO_TARGET_LATENCY_MS");
    if (!raw_value || raw_value[0] == '\0')
        return 250;

    auto value = strtoul(raw_value, nullptr, 10);
    if (value == 0)
        return 250;
    return min<u32>(static_cast<u32>(value), 1000);
}

static bool mundo_audio_skip_silent_gaps_enabled()
{
    auto const* raw_value = getenv("MUNDO_AUDIO_SKIP_SILENT_GAPS");
    if (!raw_value)
        return true;

    return strcasecmp(raw_value, "0") != 0
        && strcasecmp(raw_value, "false") != 0
        && strcasecmp(raw_value, "no") != 0
        && strcasecmp(raw_value, "off") != 0;
}

ErrorOr<NonnullRefPtr<AudioMixingSink>> AudioMixingSink::try_create()
{
    auto weak_ref = TRY(try_make_ref_counted<AudioMixingSinkWeakReference>());
    auto sink = TRY(try_make_ref_counted<AudioMixingSink>(weak_ref));
    weak_ref->emplace(sink);
    return sink;
}

AudioMixingSink::AudioMixingSink(AudioMixingSinkWeakReference& weak_ref)
    : m_main_thread_event_loop(Core::EventLoop::current_weak())
    , m_weak_self(weak_ref)
{
    auto event_loop = m_main_thread_event_loop->take();
    if (!event_loop)
        return;
    event_loop->deferred_invoke([weak_self = m_weak_self] {
        auto self = weak_self->take_strong();
        if (!self)
            return;
        self->create_playback_stream();
    });
}

AudioMixingSink::~AudioMixingSink()
{
    m_weak_self->revoke();
}

void AudioMixingSink::set_provider(Track const& track, RefPtr<AudioDataProvider> const& provider)
{
    Threading::MutexLocker locker { m_mutex };
    m_track_mixing_datas.remove(track);
    if (provider == nullptr) {
        dbgln("MUNDO_AUDIO_SINK clear_provider track_id={}", track.identifier());
        return;
    }

    // The provider must have its output sample specification set before it starts decoding, or
    // we'll drop some samples due to a mismatch.
    m_track_mixing_datas.set(track, TrackMixingData(*provider));
    dbgln("MUNDO_AUDIO_SINK set_provider track_id={} sample_spec_valid={}", track.identifier(), m_sample_specification.is_valid());
    if (m_sample_specification.is_valid()) {
        provider->set_output_sample_specification(m_sample_specification);
        provider->start();
        dbgln("MUNDO_AUDIO_SINK provider_started track_id={} sample_rate={} channels={}", track.identifier(), m_sample_specification.sample_rate(), m_sample_specification.channel_count());
    }
}

RefPtr<AudioDataProvider> AudioMixingSink::provider(Track const& track) const
{
    auto mixing_data = m_track_mixing_datas.get(track);
    if (!mixing_data.has_value())
        return nullptr;
    return mixing_data->provider;
}

void AudioMixingSink::create_playback_stream()
{
    if (m_playback_stream != nullptr || m_creating_playback_stream)
        return;

    m_creating_playback_stream = true;

    auto data_callback = [weak_self = m_weak_self](Span<float> buffer) -> ReadonlySpan<float> {
        auto self = weak_self->take_strong();
        if (!self)
            return buffer.trim(0);
        return self->write_audio_data_to_playback_stream(buffer);
    };
    auto target_latency_ms = mundo_audio_target_latency_ms();
    dbgln("MUNDO_AUDIO_SINK create_playback_stream target_latency_ms={}", target_latency_ms);

    auto promise = Audio::PlaybackStream::create(Audio::OutputState::Suspended, target_latency_ms, move(data_callback));

    promise->when_resolved([weak_self = m_weak_self](auto& stream) {
        auto self = weak_self->take_strong();
        if (!self)
            return;

        self->m_creating_playback_stream = false;
        self->m_playback_stream = stream;
        self->set_volume(self->m_volume);
        if (self->m_temporary_time.has_value())
            self->set_time(self->m_temporary_time.value());

        Threading::MutexLocker locker { self->m_mutex };
        self->m_sample_specification = stream->sample_specification();
        dbgln("MUNDO_AUDIO_SINK playback_stream_ready sample_rate={} channels={} providers={} playing={} volume={}",
            self->m_sample_specification.sample_rate(), self->m_sample_specification.channel_count(), self->m_track_mixing_datas.size(), self->m_playing, self->m_volume);

        for (auto& [track, track_data] : self->m_track_mixing_datas) {
            track_data.provider->set_output_sample_specification(self->m_sample_specification);
            track_data.provider->start();
            dbgln("MUNDO_AUDIO_SINK provider_started track_id={} sample_rate={} channels={}", track.identifier(), self->m_sample_specification.sample_rate(), self->m_sample_specification.channel_count());
        }

        if (self->m_playing)
            self->resume();
    });

    promise->when_rejected([weak_self = m_weak_self](auto& error) {
        auto self = weak_self->take_strong();
        if (!self)
            return;

        self->m_creating_playback_stream = false;
        if (self->on_audio_output_error)
            self->on_audio_output_error(move(error));
    });
}

ReadonlySpan<float> AudioMixingSink::write_audio_data_to_playback_stream(Span<float> buffer)
{
    VERIFY(m_sample_specification.is_valid());
    VERIFY(buffer.size() > 0);

    auto channel_count = m_sample_specification.channel_count();
    auto sample_count = buffer.size() / channel_count;
    buffer.fill(0.0f);

    Threading::MutexLocker mixing_data_locker { m_mutex };
    auto buffer_start = m_next_sample_to_write.load();
    auto samples_end = buffer_start + static_cast<i64>(sample_count);

    auto buffering = false;
    for (auto& [track, track_data] : m_track_mixing_datas) {
        if (!track_data.provider->is_blocked())
            continue;
        auto available_end = track_data.provider->queue_end_sample();
        if (available_end < samples_end) {
            samples_end = available_end;
            buffering = true;
        }
    }

    for (auto& [track, track_data] : m_track_mixing_datas) {
        if (!buffering) {
            track_data.buffering = false;
        } else {
            if (!track_data.provider->is_blocked())
                continue;
            if (track_data.buffering)
                continue;
            track_data.buffering = true;

            auto event_loop = m_main_thread_event_loop->take();
            if (!event_loop)
                continue;
            event_loop->deferred_invoke([weak_self = m_weak_self, track] {
                auto self = weak_self->take_strong();
                if (self && self->on_start_buffering)
                    self->on_start_buffering(track);
            });
        }
    }

    sample_count = max(samples_end - buffer_start, 0);
    auto write_size = sample_count * channel_count;

    if (sample_count == 0) {
        ++m_write_callback_count;
        if (m_write_callback_count <= 12 || m_write_callback_count % 120 == 0)
            dbgln("MUNDO_AUDIO_SINK write count={} requested={} wrote=0 buffering={} providers={} volume={} next_sample={} queue_end_min={}",
                m_write_callback_count, buffer.size() / channel_count, buffering, m_track_mixing_datas.size(), m_volume, m_next_sample_to_write.load(), samples_end);
        return buffer;
    }

    bool wrote_nonzero_sample = false;
    auto const requested_sample_count = sample_count;
    for (auto& [track, track_data] : m_track_mixing_datas) {
        auto next_sample = buffer_start;

        auto go_to_next_block = [&] {
            auto new_block = track_data.provider->retrieve_block();
            if (new_block.is_empty())
                return false;

            track_data.current_block = move(new_block);
            return true;
        };

        if (track_data.current_block.is_empty()) {
            if (!go_to_next_block())
                continue;
        }

        while (!track_data.current_block.is_empty()) {
            auto& current_block = track_data.current_block;
            auto current_block_sample_count = static_cast<i64>(current_block.sample_count());

            if (current_block.sample_specification() != m_sample_specification) {
                if (!go_to_next_block())
                    break;
                current_block.clear();
                continue;
            }

            auto first_sample_offset = current_block.timestamp_in_samples();
            if (first_sample_offset >= samples_end) {
                if (mundo_audio_skip_silent_gaps_enabled() && !wrote_nonzero_sample && m_track_mixing_datas.size() == 1 && first_sample_offset > buffer_start) {
                    auto old_buffer_start = buffer_start;
                    auto old_samples_end = samples_end;
                    buffer_start = first_sample_offset;
                    samples_end = buffer_start + static_cast<i64>(requested_sample_count);
                    next_sample = buffer_start;
                    m_silent_gap_skip_count++;
                    if (m_silent_gap_skip_count <= 16 || m_silent_gap_skip_count % 60 == 0)
                        dbgln("MUNDO_AUDIO_SINK skip_silent_gap count={} track_id={} old_start={} old_end={} new_start={} gap_samples={} gap_ms={} requested={} queue_end_min={}",
                            m_silent_gap_skip_count,
                            track.identifier(),
                            old_buffer_start,
                            old_samples_end,
                            buffer_start,
                            buffer_start - old_buffer_start,
                            AK::Duration::from_time_units(buffer_start - old_buffer_start, 1, m_sample_specification.sample_rate()).to_milliseconds(),
                            requested_sample_count,
                            samples_end);
                } else {
                    break;
                }
            }

            auto block_end = first_sample_offset + current_block_sample_count;
            if (block_end <= next_sample) {
                if (!go_to_next_block())
                    break;
                continue;
            }

            next_sample = max(next_sample, first_sample_offset);

            VERIFY(next_sample >= first_sample_offset);
            auto index_in_block = static_cast<size_t>((next_sample - first_sample_offset) * channel_count);
            VERIFY(index_in_block < current_block.data_count());

            VERIFY(next_sample >= buffer_start);
            auto index_in_buffer = static_cast<size_t>((next_sample - buffer_start) * channel_count);
            VERIFY(index_in_buffer < write_size);

            VERIFY(current_block.data_count() >= index_in_block);
            auto write_count = current_block.data_count() - index_in_block;
            write_count = min(write_count, write_size - index_in_buffer);
            VERIFY(write_count > 0);
            VERIFY(index_in_buffer + write_count <= write_size);
            VERIFY(write_count % channel_count == 0);

            for (size_t i = 0; i < write_count; i++)
                buffer[index_in_buffer + i] += current_block.data()[index_in_block + i];
            for (size_t i = 0; i < write_count && !wrote_nonzero_sample; i++) {
                if (current_block.data()[index_in_block + i] != 0.0f)
                    wrote_nonzero_sample = true;
            }

            auto write_end = index_in_block + write_count;
            if (write_end == current_block.data_count()) {
                if (!go_to_next_block())
                    break;
                continue;
            }
            VERIFY(write_end < current_block.data_count());

            next_sample += static_cast<i64>(write_count / channel_count);
            if (next_sample == samples_end)
                break;
            VERIFY(next_sample < samples_end);
        }
    }

    m_next_sample_to_write = buffer_start + static_cast<i64>(sample_count);
    ++m_write_callback_count;
    if (m_write_callback_count <= 12 || m_write_callback_count % 120 == 0 || sample_count < requested_sample_count || !wrote_nonzero_sample)
        dbgln("MUNDO_AUDIO_SINK write count={} requested={} wrote={} buffering={} providers={} nonzero={} volume={} next_sample={} queue_end_min={}",
            m_write_callback_count, requested_sample_count, sample_count, buffering, m_track_mixing_datas.size(), wrote_nonzero_sample, m_volume, m_next_sample_to_write.load(), samples_end);
    return buffer;
}

AK::Duration AudioMixingSink::current_time() const
{
    if (!m_sample_specification.is_valid())
        return AK::Duration::zero();
    if (m_temporary_time.has_value())
        return m_temporary_time.value();
    if (!m_playback_stream)
        return m_last_media_time;

    auto time = m_last_media_time + (m_playback_stream->total_time_played() - m_last_stream_time);
    auto max_time = AK::Duration::from_time_units(m_next_sample_to_write.load(MemoryOrder::memory_order_acquire), 1, m_sample_specification.sample_rate());
    time = min(time, max_time);
    return time;
}

void AudioMixingSink::resume()
{
    m_playing = true;
    dbgln("MUNDO_AUDIO_SINK resume playback_stream={} temporary_time={}", m_playback_stream != nullptr, m_temporary_time.has_value());

    // If we're in the middle of the set_time() callbacks, let those take care of resuming.
    if (m_temporary_time.has_value())
        return;

    if (!m_playback_stream)
        return;
    m_playback_stream->resume()
        ->when_resolved([weak_self = m_weak_self, &playback_stream = *m_playback_stream](auto new_device_time) {
            auto self = weak_self->take_strong();
            if (!self)
                return;
            if (self->m_playback_stream != &playback_stream)
                return;

            auto event_loop = self->m_main_thread_event_loop->take();
            if (!event_loop)
                return;
            event_loop->deferred_invoke([self, new_device_time]() {
                self->m_last_stream_time = new_device_time;
            });
        })
        .when_rejected([](auto&& error) {
            warnln("Unexpected error while resuming AudioMixingSink: {}", error.string_literal());
        });
}

void AudioMixingSink::pause()
{
    m_playing = false;
    dbgln("MUNDO_AUDIO_SINK pause playback_stream={}", m_playback_stream != nullptr);

    if (!m_playback_stream)
        return;
    m_playback_stream->drain_buffer_and_suspend()
        ->when_resolved([weak_self = m_weak_self, &playback_stream = *m_playback_stream]() {
            auto self = weak_self->take_strong();
            if (!self)
                return;
            if (self->m_playback_stream != &playback_stream)
                return;

            auto new_stream_time = self->m_playback_stream->total_time_played();
            auto new_media_time = AK::Duration::from_time_units(self->m_next_sample_to_write, 1, self->m_sample_specification.sample_rate());

            auto event_loop = self->m_main_thread_event_loop->take();
            if (!event_loop)
                return;
            event_loop->deferred_invoke([self, new_stream_time, new_media_time]() {
                self->m_last_stream_time = new_stream_time;
                self->m_last_media_time = new_media_time;
            });
        })
        .when_rejected([](auto&& error) {
            warnln("Unexpected error while pausing AudioMixingSink: {}", error.string_literal());
        });
}

void AudioMixingSink::set_time(AK::Duration time)
{
    if (!m_playback_stream) {
        m_last_media_time = time;
        m_last_stream_time = AK::Duration::zero();
        return;
    }

    // If we've already started setting the time, we only need to let the last callback complete
    // and set the media time to the temporary time. The callbacks run synchronously, so this will
    // never drop a set_time() call.
    if (m_temporary_time.has_value()) {
        m_temporary_time = time;
        return;
    }

    m_temporary_time = time;

    m_playback_stream->drain_buffer_and_suspend()
        ->when_resolved([weak_self = m_weak_self, &playback_stream = *m_playback_stream]() {
            auto self = weak_self->take_strong();
            if (!self)
                return;
            if (self->m_playback_stream != &playback_stream)
                return;

            auto new_stream_time = self->m_playback_stream->total_time_played();

            auto event_loop = self->m_main_thread_event_loop->take();
            if (!event_loop)
                return;
            event_loop->deferred_invoke([self, new_stream_time]() {
                {
                    self->m_last_stream_time = new_stream_time;
                    self->m_last_media_time = self->m_temporary_time.release_value();

                    {
                        Threading::MutexLocker mixing_locker { self->m_mutex };
                        self->m_next_sample_to_write = self->m_last_media_time.to_time_units(1, self->m_sample_specification.sample_rate());
                    }

                    for (auto& [track, track_data] : self->m_track_mixing_datas)
                        track_data.current_block.clear();
                }

                if (self->m_playing)
                    self->resume();
            });
        })
        .when_rejected([](auto&& error) {
            warnln("Unexpected error while setting time on AudioMixingSink: {}", error.string_literal());
        });
}

void AudioMixingSink::clear_track_data(Track const& track)
{
    auto track_data = m_track_mixing_datas.find(track);
    if (track_data == m_track_mixing_datas.end())
        return;
    track_data->value.current_block.clear();
}

void AudioMixingSink::set_volume(double volume)
{
    m_volume = volume;
    dbgln("MUNDO_AUDIO_SINK set_volume volume={} playback_stream={}", m_volume, m_playback_stream != nullptr);

    if (m_playback_stream) {
        m_playback_stream->set_volume(m_volume)
            ->when_rejected([](Error&&) {
                // FIXME: Do we even need this function to return a promise?
            });
    }
}

}
