/*
 * Copyright (c) 2024, Jelle Raaijmakers <jelle@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/StringBuilder.h>
#include <LibJS/Runtime/VM.h>
#include <LibMedia/PlaybackManager.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MediaSource.h>
#include <LibWeb/DOM/Event.h>
#include <LibWeb/HTML/HTMLMediaElement.h>
#include <LibWeb/MediaSourceExtensions/EventNames.h>
#include <LibWeb/MediaSourceExtensions/MediaSource.h>
#include <LibWeb/MediaSourceExtensions/SourceBuffer.h>
#include <LibWeb/MediaSourceExtensions/SourceBufferList.h>
#include <LibWeb/MimeSniff/MimeType.h>
#include <stdlib.h>
#include <string.h>

namespace Web::MediaSourceExtensions {

using Bindings::ReadyState;

GC_DEFINE_ALLOCATOR(MediaSource);

static bool mundo_nvdec_backend_requested()
{
    auto const* raw_backend = getenv("MUNDO_VIDEO_DECODER_BACKEND");
    if (!raw_backend)
        return false;

    return !strcmp(raw_backend, "nvdec") || !strcmp(raw_backend, "cuda");
}

static Optional<unsigned> mundo_avc_codec_level_idc(StringView codec)
{
    auto dot_offset = codec.find('.');
    if (!dot_offset.has_value())
        return {};

    auto avc_identifier = codec.substring_view(*dot_offset + 1);
    if (avc_identifier.length() < 6)
        return {};

    auto hex_value = [](u8 ch) -> Optional<unsigned> {
        if (ch >= '0' && ch <= '9')
            return ch - '0';
        if (ch >= 'a' && ch <= 'f')
            return ch - 'a' + 10;
        if (ch >= 'A' && ch <= 'F')
            return ch - 'A' + 10;
        return {};
    };

    auto high = hex_value(avc_identifier[4]);
    auto low = hex_value(avc_identifier[5]);
    if (!high.has_value() || !low.has_value())
        return {};
    return (*high << 4) | *low;
}

static bool mundo_mse_codecs_contain_nvdec_unfriendly_h264(StringView codecs)
{
    bool unsupported = false;
    codecs.for_each_split_view(',', SplitBehavior::Nothing, [&](auto codec) {
        codec = codec.trim_whitespace();
        if (!codec.starts_with("avc1"sv) && !codec.starts_with("avc3"sv))
            return IterationDecision::Continue;

        auto level_idc = mundo_avc_codec_level_idc(codec);
        if (level_idc.has_value() && *level_idc > 0x33) {
            unsupported = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return unsupported;
}

static bool mundo_mse_codecs_are_supported(StringView codecs)
{
    bool unsupported = false;
    codecs.for_each_split_view(',', SplitBehavior::Nothing, [&](auto codec) {
        codec = codec.trim_whitespace();
        if (codec.is_empty())
            return IterationDecision::Continue;

        if (!codec.starts_with("avc1"sv)
            && !codec.starts_with("avc3"sv)
            && !codec.starts_with("hvc1"sv)
            && !codec.starts_with("hev1"sv)
            && !codec.starts_with("av01"sv)
            && !codec.starts_with("vp9"sv)
            && !codec.starts_with("vp09"sv)
            && !codec.starts_with("opus"sv)
            && !codec.starts_with("vorbis"sv)
            && !codec.starts_with("mp4a"sv)) {
            unsupported = true;
            return IterationDecision::Break;
        }
        return IterationDecision::Continue;
    });
    return !unsupported;
}

static String mundo_media_source_stack_trace(JS::VM& vm)
{
    auto stack_trace = vm.stack_trace();
    if (stack_trace.is_empty())
        return {};

    StringBuilder builder;
    size_t emitted_frames = 0;
    for (size_t i = 0; i < stack_trace.size() && emitted_frames < 8; ++i) {
        auto const& element = stack_trace[i];
        auto* context = element.execution_context;
        auto function_name = (context && context->function) ? context->function->name_for_call_stack() : ""_utf16;
        auto function_name_string = function_name.is_empty() ? "<anonymous>"_string : function_name.to_utf8();

        if (emitted_frames != 0)
            builder.append(" <- "sv);

        if (element.source_range.has_value()) {
            auto const& source_range = *element.source_range;
            if (!source_range.filename().is_empty()) {
                builder.appendff("{}@{}:{}:{}", function_name_string, source_range.filename(), source_range.start.line, source_range.start.column);
                ++emitted_frames;
                continue;
            }
        }

        builder.append(function_name_string);
        ++emitted_frames;
    }

    return MUST(builder.to_string());
}

WebIDL::ExceptionOr<GC::Ref<MediaSource>> MediaSource::construct_impl(JS::Realm& realm)
{
    return realm.create<MediaSource>(realm);
}

MediaSource::MediaSource(JS::Realm& realm)
    : DOM::EventTarget(realm)
    , m_source_buffers(realm.create<SourceBufferList>(realm))
    , m_active_source_buffers(realm.create<SourceBufferList>(realm))
{
}

MediaSource::~MediaSource() = default;

void MediaSource::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MediaSource);
    Base::initialize(realm);
}

void MediaSource::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_media_element_assigned_to);
    visitor.visit(m_source_buffers);
    visitor.visit(m_active_source_buffers);
}

void MediaSource::queue_a_media_source_task(GC::Ref<GC::Function<void()>> task)
{
    // FIXME: The MSE spec does not say what task source to use for its tasks. Should this use the media element's
    //        task source? We may not have access to it if we're in a worker.
    GC::Ptr<DOM::Document> document = nullptr;
    if (media_element_assigned_to() != nullptr)
        document = media_element_assigned_to()->document();

    HTML::queue_a_task(HTML::Task::Source::Unspecified, HTML::main_thread_event_loop(), document, task);
}

ReadyState MediaSource::ready_state() const
{
    return m_ready_state;
}

bool MediaSource::ready_state_is_closed() const
{
    return m_ready_state == ReadyState::Closed;
}

void MediaSource::set_has_ever_been_attached()
{
    m_has_ever_been_attached = true;
}

void MediaSource::set_ready_state_to_open_and_fire_sourceopen_event()
{
    m_ready_state = ReadyState::Open;

    // AD-HOC: Notify all demuxers that we have new data coming in and cannot consider the end of the buffer to be
    //         the end of the stream.
    for (size_t i = 0; i < m_source_buffers->length(); i++) {
        auto& source_buffer = *m_source_buffers->item(i);
        source_buffer.clear_reached_end_of_stream({});
    }

    queue_a_media_source_task(GC::create_function(heap(), [this] {
        auto event = DOM::Event::create(realm(), EventNames::sourceopen);
        dispatch_event(event);
    }));
}

void MediaSource::set_assigned_to_media_element(Badge<HTML::HTMLMediaElement>, HTML::HTMLMediaElement& media_element)
{
    m_media_element_assigned_to = media_element;
}

void MediaSource::unassign_from_media_element(Badge<HTML::HTMLMediaElement>)
{
    m_media_element_assigned_to = nullptr;
}

GC::Ref<SourceBufferList> MediaSource::source_buffers()
{
    return m_source_buffers;
}

GC::Ref<SourceBufferList> MediaSource::active_source_buffers()
{
    return m_active_source_buffers;
}

Utf16String MediaSource::next_track_id()
{
    return Utf16String::number(m_next_track_id++);
}

// https://w3c.github.io/media-source/#dom-mediasource-onsourceopen
void MediaSource::set_onsourceopen(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::sourceopen, event_handler);
}

// https://w3c.github.io/media-source/#dom-mediasource-onsourceopen
GC::Ptr<WebIDL::CallbackType> MediaSource::onsourceopen()
{
    return event_handler_attribute(EventNames::sourceopen);
}

// https://w3c.github.io/media-source/#dom-mediasource-onsourceended
void MediaSource::set_onsourceended(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::sourceended, event_handler);
}

// https://w3c.github.io/media-source/#dom-mediasource-onsourceended
GC::Ptr<WebIDL::CallbackType> MediaSource::onsourceended()
{
    return event_handler_attribute(EventNames::sourceended);
}

// https://w3c.github.io/media-source/#dom-mediasource-onsourceclose
void MediaSource::set_onsourceclose(GC::Ptr<WebIDL::CallbackType> event_handler)
{
    set_event_handler_attribute(EventNames::sourceclose, event_handler);
}

// https://w3c.github.io/media-source/#dom-mediasource-onsourceclose
GC::Ptr<WebIDL::CallbackType> MediaSource::onsourceclose()
{
    return event_handler_attribute(EventNames::sourceclose);
}

// https://w3c.github.io/media-source/#addsourcebuffer-method
WebIDL::ExceptionOr<GC::Ref<SourceBuffer>> MediaSource::add_source_buffer(String const& type)
{
    // 1. If type is an empty string then throw a TypeError exception and abort these steps.
    if (type.is_empty()) {
        return WebIDL::SimpleException {
            WebIDL::SimpleExceptionType::TypeError,
            "SourceBuffer type must not be empty"sv
        };
    }

    // 2. If type contains a MIME type that is not supported or contains a MIME type that is not
    //    supported with the types specified for the other SourceBuffer objects in sourceBuffers,
    //    then throw a NotSupportedError exception and abort these steps.
    if (!is_type_supported(type)) {
        return WebIDL::NotSupportedError::create(realm(), "Unsupported MIME type"_utf16);
    }

    // FIXME: 3. If the user agent can't handle any more SourceBuffer objects or if creating a SourceBuffer
    //           based on type would result in an unsupported SourceBuffer configuration, then throw a
    //           QuotaExceededError exception and abort these steps.

    // 4. If the readyState attribute is not in the "open" state then throw an InvalidStateError exception and abort these steps.
    if (ready_state() != ReadyState::Open)
        return WebIDL::InvalidStateError::create(realm(), "MediaSource is not open"_utf16);

    // 5. Let buffer be a new instance of a ManagedSourceBuffer if this is a ManagedMediaSource, or
    //    a SourceBuffer otherwise, with their respective associated resources.
    auto buffer = realm().create<SourceBuffer>(realm(), GC::Ref(*this));
    buffer->set_content_type(type);

    // FIXME: 6. Set buffer's [[generate timestamps flag]] to the value in the "Generate Timestamps Flag"
    //           column of the Media Source Extensions™ Byte Stream Format Registry entry that is
    //           associated with type.
    // FIXME: 7. If buffer's [[generate timestamps flag]] is true, set buffer's mode to "sequence".
    //           Otherwise, set buffer's mode to "segments".
    // 8. Append buffer to this's sourceBuffers.
    // 9. Queue a task to fire an event named addsourcebuffer at this's sourceBuffers.
    m_source_buffers->append(buffer);

    // 10. Return buffer.
    return buffer;
}

// https://w3c.github.io/media-source/#dom-mediasource-endofstream
WebIDL::ExceptionOr<void> MediaSource::end_of_stream(Optional<Bindings::EndOfStreamError> const& error)
{
    // 1. If the readyState attribute is not in the "open" state then throw an InvalidStateError exception
    //    and abort these steps.
    if (ready_state() != ReadyState::Open)
        return WebIDL::InvalidStateError::create(realm(), "MediaSource is not open"_utf16);

    // 2. If the updating attribute equals true on any SourceBuffer in sourceBuffers, then throw an
    //    InvalidStateError exception and abort these steps.
    for (size_t i = 0; i < m_source_buffers->length(); i++) {
        if (m_source_buffers->item(i)->updating())
            return WebIDL::InvalidStateError::create(realm(), "A SourceBuffer is still updating"_utf16);
    }

    // 3. Run the end of stream algorithm with the error parameter set to error.
    run_end_of_stream_algorithm(error);

    return {};
}

// https://w3c.github.io/media-source/#end-of-stream-algorithm
void MediaSource::run_end_of_stream_algorithm(Optional<Bindings::EndOfStreamError> const& error)
{
    // 1. Change the readyState attribute value to "ended".
    m_ready_state = ReadyState::Ended;

    // 2. Queue a task to fire an event named sourceended at the MediaSource.
    queue_a_media_source_task(GC::create_function(heap(), [this] {
        dispatch_event(DOM::Event::create(realm(), EventNames::sourceended));
    }));

    // AD-HOC: Notify all demuxers that end of stream was reached, so that they can return the requisite error and
    //         allow decoders to flush all their frames.
    for (size_t i = 0; i < m_source_buffers->length(); i++) {
        auto& source_buffer = *m_source_buffers->item(i);
        source_buffer.set_reached_end_of_stream({});
    }

    // 3. If error is not set:
    if (!error.has_value()) {
        // 1. Run the duration change algorithm with new duration set to the largest track buffer ranges
        //    end time across all the track buffers across all SourceBuffer objects in sourceBuffers.
        // FIXME: Implement duration change based on track buffer ranges.

        // 2. Notify the media element that it now has all of the media data.
        // FIXME: Signal to the HTMLMediaElement that all data has been provided.
        return;
    }

    // 4. If error is set to "network":
    if (error.value() == Bindings::EndOfStreamError::Network) {
        // FIXME: If the HTMLMediaElement's readyState attribute equals HAVE_NOTHING:
        //            Run the "If the media data cannot be fetched at all" steps of the resource fetch algorithm.
        //        Otherwise:
        //            Run the "If the connection is interrupted after some media data has been received" steps
        //            of the resource fetch algorithm.
        return;
    }

    // 5. If error is set to "decode":
    if (error.value() == Bindings::EndOfStreamError::Decode) {
        // FIXME: If the HTMLMediaElement's readyState attribute equals HAVE_NOTHING:
        //            Run the "If the media data can be fetched but is found by inspection to be in an
        //            unsupported format" steps of the resource fetch algorithm.
        //        Otherwise:
        //            Run the "If the media data can be fetched but has fatal network errors" steps of the
        //            resource fetch algorithm.
        return;
    }
}

// https://w3c.github.io/media-source/#dom-mediasource-duration
double MediaSource::duration() const
{
    // 1. If the readyState attribute is "closed" then return NaN and abort these steps.
    if (ready_state() == ReadyState::Closed)
        return NAN;

    // 2. Return the current value of the attribute.
    return m_duration;
}

// https://w3c.github.io/media-source/#dom-mediasource-duration
WebIDL::ExceptionOr<void> MediaSource::set_duration(double new_duration)
{
    // 1. If the value being set is negative or NaN then throw a TypeError exception and abort these steps.
    if (new_duration < 0 || isnan(new_duration))
        return WebIDL::SimpleException { WebIDL::SimpleExceptionType::TypeError, "duration must not be negative or NaN"sv };

    // 2. If the readyState attribute is not in the "open" state then throw an InvalidStateError exception
    //    and abort these steps.
    if (ready_state() != ReadyState::Open)
        return WebIDL::InvalidStateError::create(realm(), "MediaSource is not open"_utf16);

    // 3. If the updating attribute equals true on any SourceBuffer in sourceBuffers, then throw an
    //    InvalidStateError exception and abort these steps.
    for (size_t i = 0; i < m_source_buffers->length(); i++) {
        if (m_source_buffers->item(i)->updating())
            return WebIDL::InvalidStateError::create(realm(), "A SourceBuffer is still updating"_utf16);
    }

    // 4. Run the duration change algorithm with new duration set to the value being assigned to this attribute.
    run_duration_change_algorithm(new_duration);

    return {};
}

// https://w3c.github.io/media-source/#duration-change-algorithm
void MediaSource::run_duration_change_algorithm(double new_duration)
{
    // 1. If the current value of duration is equal to new duration, then return.
    if (m_duration == new_duration)
        return;

    // 2. If new duration is less than the highest presentation timestamp of any buffered coded frames
    //    for all SourceBuffer objects in sourceBuffers, then throw an InvalidStateError exception and
    //    abort these steps.
    // FIXME: Check highest presentation timestamp across all track buffers.

    // 3. Let highest end time be the largest track buffer ranges end time across all the track buffers
    //    across all SourceBuffer objects in sourceBuffers.
    // 4. If new duration is less than highest end time, then update new duration to equal highest end time.
    // FIXME: Clamp new_duration to highest end time.

    // 5. Update duration to new duration.
    m_duration = new_duration;

    // 6. Use the mirror if necessary algorithm to run the following steps in Window to update the
    //    media element's duration:
    // FIXME: Mirror to the Window when workers are supported.
    //        Update the media element's duration to new duration.
    //        Run the HTMLMediaElement duration change algorithm.
    media_element_assigned_to()->set_duration({}, new_duration);
    media_element_assigned_to()->playback_manager().set_duration(AK::Duration::from_seconds_f64(new_duration));
}

// https://w3c.github.io/media-source/#dom-mediasource-istypesupported
bool MediaSource::is_type_supported(String const& type)
{
    // 1. If type is an empty string, then return false.
    if (type.is_empty())
        return false;

    // 2. If type does not contain a valid MIME type string, then return false.
    auto mime_type = MimeSniff::MimeType::parse(type);
    if (!mime_type.has_value())
        return false;

    // FIXME: Ask LibMedia about what it supports instead of hardcoding this.

    // 3. If type contains a media type or media subtype that the MediaSource does not support, then
    //    return false.
    auto type_and_subtype_are_supported = [&] {
        if (mime_type->type() == "video" && mime_type->subtype() == "webm")
            return true;
        if (mime_type->type() == "audio" && mime_type->subtype() == "webm")
            return true;
        if (mime_type->type() == "video" && mime_type->subtype() == "mp4")
            return true;
        if (mime_type->type() == "audio" && mime_type->subtype() == "mp4")
            return true;
        if (mime_type->type() == "application" && mime_type->subtype() == "mp4")
            return true;
        return false;
    }();
    if (!type_and_subtype_are_supported) {
        dbgln("MUNDO_MEDIA_SOURCE is_type_supported type={} result=false reason=unsupported_mse_container", type);
        return false;
    }

    // 4. If type contains a codec that the MediaSource does not support, then return false.
    // 5. If the MediaSource does not support the specified combination of media type, media
    //    subtype, and codecs then return false.
    auto codecs_iter = mime_type->parameters().find("codecs"sv);
    if (codecs_iter == mime_type->parameters().end())
        return false;
    auto codecs = codecs_iter->value.bytes_as_string_view();
    if (!mundo_mse_codecs_are_supported(codecs)) {
        dbgln("MUNDO_MEDIA_SOURCE is_type_supported type={} result=false reason=unsupported_codec", type);
        return false;
    }
    if (mundo_nvdec_backend_requested() && mundo_mse_codecs_contain_nvdec_unfriendly_h264(codecs)) {
        dbgln("MUNDO_MEDIA_SOURCE is_type_supported type={} result=false reason=nvdec_h264_level_limit", type);
        return false;
    }

    // 6. Return true.
    dbgln("MUNDO_MEDIA_SOURCE is_type_supported type={} result=true", type);
    return true;
}

bool MediaSource::is_type_supported(JS::VM& vm, String const& type)
{
    auto result = is_type_supported(type);
    dbgln("MUNDO_MEDIA_SOURCE is_type_supported_call type={} result={} stack={}", type, result, mundo_media_source_stack_trace(vm));
    return result;
}

}
