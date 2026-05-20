/*
 * Copyright (c) 2021, Andreas Kling <andreas@ladybird.org>
 * Copyright (c) 2023, Andrew Kaster <akaster@serenityos.org>
 * Copyright (c) 2025, Aliaksandr Kalenik <kalenik.aliaksandr@gmail.com>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/ByteReader.h>
#include <AK/MemoryStream.h>
#include <AK/String.h>
#include <AK/Time.h>
#include <LibCore/System.h>
#include <LibGC/WeakHashSet.h>
#include <LibIPC/Decoder.h>
#include <LibIPC/Encoder.h>
#include <LibIPC/Transport.h>
#include <LibIPC/TransportHandle.h>
#include <LibWeb/Bindings/ExceptionOrUtils.h>
#include <LibWeb/Bindings/Intrinsics.h>
#include <LibWeb/Bindings/MessagePort.h>
#include <LibWeb/DOM/EventDispatcher.h>
#include <LibWeb/HTML/EventNames.h>
#include <LibWeb/HTML/MessageEvent.h>
#include <LibWeb/HTML/MessagePort.h>
#include <LibWeb/HTML/Scripting/TemporaryExecutionContext.h>
#include <LibWeb/HTML/StructuredSerialize.h>
#include <LibWeb/HTML/StructuredSerializeOptions.h>
#include <LibWeb/HTML/WorkerGlobalScope.h>

#include <stdlib.h>
#include <string.h>

namespace Web::HTML {

constexpr u8 IPC_FILE_TAG = 0xA5;

GC_DEFINE_ALLOCATOR(MessagePort);

static AK::Duration mundo_posted_message_log_threshold()
{
    static auto threshold = [] {
        auto const* raw_value = getenv("MUNDO_POSTED_MESSAGE_LOG_MS");
        if (!raw_value)
            return AK::Duration::from_milliseconds(120);

        auto value = atoi(raw_value);
        if (value <= 0)
            return AK::Duration::max();

        return AK::Duration::from_milliseconds(value);
    }();
    return threshold;
}

static bool mundo_message_port_null_tick_coalesce_enabled()
{
    static auto enabled = [] {
        auto const* raw_value = getenv("MUNDO_MESSAGE_PORT_NULL_TICK_COALESCE");
        if (!raw_value)
            return false;

        return raw_value[0] != '\0' && strcmp(raw_value, "0") && strcmp(raw_value, "false") && strcmp(raw_value, "no") && strcmp(raw_value, "off");
    }();
    return enabled;
}

static AK::Duration mundo_message_port_null_tick_min_interval()
{
    static auto interval = [] {
        auto const* raw_value = getenv("MUNDO_MESSAGE_PORT_NULL_TICK_MIN_INTERVAL_MS");
        if (!raw_value)
            return AK::Duration::from_milliseconds(250);

        auto value = atoi(raw_value);
        if (value <= 0)
            return AK::Duration::zero();

        return AK::Duration::from_milliseconds(value);
    }();
    return interval;
}

static AK::Duration mundo_message_port_null_tick_slow_threshold()
{
    static auto threshold = [] {
        auto const* raw_value = getenv("MUNDO_MESSAGE_PORT_NULL_TICK_SLOW_MS");
        if (!raw_value)
            return AK::Duration::from_milliseconds(400);

        auto value = atoi(raw_value);
        if (value <= 0)
            return AK::Duration::max();

        return AK::Duration::from_milliseconds(value);
    }();
    return threshold;
}

static AK::Duration mundo_message_port_null_tick_slow_cooldown()
{
    static auto cooldown = [] {
        auto const* raw_value = getenv("MUNDO_MESSAGE_PORT_NULL_TICK_SLOW_COOLDOWN_MS");
        if (!raw_value)
            return AK::Duration::from_milliseconds(1200);

        auto value = atoi(raw_value);
        if (value <= 0)
            return AK::Duration::zero();

        return AK::Duration::from_milliseconds(value);
    }();
    return cooldown;
}

static String mundo_posted_message_payload_summary(JS::Value value)
{
    if (value.is_undefined())
        return "undefined"_string;
    if (value.is_null())
        return "null"_string;
    if (value.is_boolean())
        return MUST(String::formatted("boolean({})", value.as_bool()));
    if (value.is_number())
        return MUST(String::formatted("number({})", value.as_double()));
    if (value.is_string())
        return MUST(String::formatted("string({})", value.to_string_without_side_effects()));
    if (value.is_bigint())
        return MUST(String::formatted("bigint({})", value.to_string_without_side_effects()));
    if (value.is_symbol())
        return "symbol"_string;
    if (value.is_object())
        return MUST(String::formatted("object(class={} ptr={})", value.as_object().class_name(), &value.as_object()));
    return value.to_string_without_side_effects();
}

static GC::WeakHashSet<MessagePort>& all_message_ports()
{
    static GC::WeakHashSet<MessagePort> ports;
    return ports;
}

GC::Ref<MessagePort> MessagePort::create(JS::Realm& realm)
{
    return realm.create<MessagePort>(realm);
}

MessagePort::MessagePort(JS::Realm& realm)
    : DOM::EventTarget(realm)
{
    all_message_ports().set(*this);
}

MessagePort::~MessagePort() = default;

void MessagePort::for_each_message_port(Function<void(MessagePort&)> callback)
{
    auto ports = all_message_ports();
    for (auto& port : ports)
        callback(port);
}

void MessagePort::initialize(JS::Realm& realm)
{
    WEB_SET_PROTOTYPE_FOR_INTERFACE(MessagePort);
    Base::initialize(realm);
}

void MessagePort::finalize()
{
    Base::finalize();
    all_message_ports().remove(*this);
    disentangle();
}

void MessagePort::visit_edges(Cell::Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_remote_port);
    visitor.visit(m_worker_event_target);
}

bool MessagePort::is_entangled() const
{
    return m_transport;
}

void MessagePort::set_worker_event_target(GC::Ref<DOM::EventTarget> target)
{
    m_worker_event_target = target;
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#message-ports:transfer-steps
WebIDL::ExceptionOr<void> MessagePort::transfer_steps(HTML::TransferDataEncoder& data_holder)
{
    // 1. Set value's has been shipped flag to true.
    m_has_been_shipped = true;

    bool has_remote_port_handle = false;
    IPC::TransportHandle remote_port_handle;

    // 3. If value is entangled with another port remotePort, then:
    if (is_entangled()) {
        // 1. Set remotePort's has been shipped flag to true.

        // NOTE: We have to null check here because we can be entangled with a port living in another agent.
        //       In that case, we'll have a transport, but no remote port object.
        if (m_remote_port)
            m_remote_port->m_has_been_shipped = true;

        // NOTE: release_for_transfer() stops the IO thread before drain_transport() reads the message buffer
        //       below (step 2). Stopping first ensures a consistent snapshot: no new messages arrive between
        //       the drain and the handle being handed to the new owner.
        remote_port_handle = MUST(m_transport->release_for_transfer());
        has_remote_port_handle = true;
    }

    // 2. Set dataHolder.[[PortMessageQueue]] to value's port message queue.

    // Drain any incoming transport state into this port before serializing it so received messages and
    // a pending shutdown move with the transferred port instead of being left behind on the old transport.
    drain_transport();
    data_holder.encode(m_pending_incoming_messages);
    data_holder.encode(m_pending_outgoing_messages);
    data_holder.encode(m_should_shutdown_on_enable);

    if (has_remote_port_handle) {
        m_transport.clear();

        // 2. Set dataHolder.[[RemotePort]] to remotePort.
        data_holder.encode(IPC_FILE_TAG);
        data_holder.encode(remote_port_handle);
    }
    // 4. Otherwise, set dataHolder.[[RemotePort]] to null.
    else {
        data_holder.encode<u8>(0);
    }

    return {};
}

WebIDL::ExceptionOr<void> MessagePort::transfer_receiving_steps(HTML::TransferDataDecoder& data_holder)
{
    // 1. Set value's has been shipped flag to true.
    m_has_been_shipped = true;

    // 2. Move all the tasks that are to fire message events in dataHolder.[[PortMessageQueue]] to the port message queue of value,
    //    if any, leaving value's port message queue in its initial disabled state, and, if value's relevant global object is a Window,
    //    associating the moved tasks with value's relevant global object's associated Document.
    m_pending_incoming_messages = data_holder.decode<Vector<SerializedTransferRecord>>();
    m_pending_outgoing_messages = data_holder.decode<Vector<SerializedTransferRecord>>();
    m_should_shutdown_on_enable = data_holder.decode<bool>();
    auto fd_tag = data_holder.decode<u8>();

    // 3. If dataHolder.[[RemotePort]] is not null, then entangle dataHolder.[[RemotePort]] and value.
    //     (This will disentangle dataHolder.[[RemotePort]] from the original port that was transferred.)
    if (fd_tag == IPC_FILE_TAG) {
        auto handle = data_holder.decode<IPC::TransportHandle>();
        m_transport = MUST(handle.create_transport());

        m_transport->set_up_read_hook([strong_this = GC::make_root(this)]() {
            strong_this->read_from_transport();
        });

        flush_pending_outgoing_messages();
    } else if (fd_tag != 0) {
        dbgln("Unexpected byte {:x} in MessagePort transfer data", fd_tag);
        VERIFY_NOT_REACHED();
    }

    return {};
}

void MessagePort::disentangle()
{
    if (m_remote_port) {
        m_remote_port->m_remote_port = nullptr;
        m_remote_port = nullptr;
    }

    if (m_transport) {
        m_transport->close_after_sending_all_pending_messages();
        m_transport.clear();
    }

    m_pending_incoming_messages.clear();
    m_pending_outgoing_messages.clear();
    m_should_shutdown_on_enable = false;
    m_worker_event_target = nullptr;
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#entangle
void MessagePort::entangle_with(MessagePort& remote_port)
{
    if (m_remote_port.ptr() == &remote_port)
        return;

    // 1. If one of the ports is already entangled, then disentangle it and the port that it was entangled with.

    // NB: A port with an active transport should not have pending messages as outgoing messages are flushed when the
    // transport is created, and incoming messages should only be pending on a port that has been transferred.
    if (is_entangled()) {
        VERIFY(m_pending_incoming_messages.is_empty());
        VERIFY(m_pending_outgoing_messages.is_empty());
        disentangle();
    }
    if (remote_port.is_entangled()) {
        VERIFY(remote_port.m_pending_incoming_messages.is_empty());
        VERIFY(remote_port.m_pending_outgoing_messages.is_empty());
        remote_port.disentangle();
    }

    // 2. Associate the two ports to be entangled, so that they form the two parts of a new channel.
    //    (There is no MessageChannel object that represents this channel.)
    remote_port.m_remote_port = this;
    m_remote_port = &remote_port;

    auto paired = MUST(IPC::Transport::create_paired());
    m_transport = move(paired.local);
    m_remote_port->m_transport = MUST(paired.remote_handle.create_transport());

    m_transport->set_up_read_hook([strong_this = GC::make_root(this)]() {
        strong_this->read_from_transport();
    });

    m_remote_port->m_transport->set_up_read_hook([remote_port = GC::make_root(m_remote_port)]() {
        remote_port->read_from_transport();
    });

    flush_pending_outgoing_messages();
    m_remote_port->flush_pending_outgoing_messages();
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-postmessage-options
WebIDL::ExceptionOr<void> MessagePort::post_message(JS::Value message, Vector<GC::Root<JS::Object>> const& transfer)
{
    // 1. Let targetPort be the port with which this MessagePort is entangled, if any; otherwise let it be null.
    GC::Ptr<MessagePort> target_port = m_remote_port;

    // 2. Let options be «[ "transfer" → transfer ]».
    auto options = StructuredSerializeOptions { transfer };

    // 3. Run the message port post message steps providing this, targetPort, message and options.
    return message_port_post_message_steps(target_port, message, options);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-postmessage
WebIDL::ExceptionOr<void> MessagePort::post_message(JS::Value message, StructuredSerializeOptions const& options)
{
    // 1. Let targetPort be the port with which this MessagePort is entangled, if any; otherwise let it be null.
    GC::Ptr<MessagePort> target_port = m_remote_port;

    // 2. Run the message port post message steps providing targetPort, message and options.
    return message_port_post_message_steps(target_port, message, options);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#message-port-post-message-steps
WebIDL::ExceptionOr<void> MessagePort::message_port_post_message_steps(GC::Ptr<MessagePort> target_port, JS::Value message, StructuredSerializeOptions const& options)
{
    auto& realm = this->realm();
    auto& vm = this->vm();

    // 1. Let transfer be options["transfer"].
    auto const& transfer = options.transfer;

    // 2. If transfer contains this MessagePort, then throw a "DataCloneError" DOMException.
    for (auto const& handle : transfer) {
        if (handle == this)
            return WebIDL::DataCloneError::create(realm, "Cannot transfer a MessagePort to itself"_utf16);
    }

    // 3. Let doomed be false.
    bool doomed = false;

    // 4. If targetPort is not null and transfer contains targetPort, then set doomed to true and optionally report to a developer console that the target port was posted to itself, causing the communication channel to be lost.
    if (target_port) {
        for (auto const& handle : transfer) {
            if (handle == target_port.ptr()) {
                doomed = true;
                dbgln("FIXME: Report to a developer console that the target port was posted to itself, causing the communication channel to be lost");
            }
        }
    }

    // 5. Let serializeWithTransferResult be StructuredSerializeWithTransfer(message, transfer). Rethrow any exceptions.
    auto serialize_with_transfer_result = TRY(structured_serialize_with_transfer(vm, message, transfer));

    // 6. If targetPort is null, or if doomed is true, then return.

    // IMPLEMENTATION DEFINED: A port can exist before it has a transport to send on. Keep the serialized
    // record on the port and flush it once the port becomes entangled.
    if (doomed) {
        return {};
    }

    if (!m_transport) {
        if (!is_detached())
            m_pending_outgoing_messages.append(move(serialize_with_transfer_result));
        return {};
    }

    // 7. Add a task that runs the following steps to the port message queue of targetPort:
    post_port_message(serialize_with_transfer_result);

    return {};
}

ErrorOr<void> MessagePort::send_message_on_transport(SerializedTransferRecord const& serialize_with_transfer_result)
{
    IPC::MessageBuffer buffer;
    IPC::Encoder encoder(buffer);
    MUST(encoder.encode(serialize_with_transfer_result));

    TRY(buffer.transfer_message(*m_transport));
    return {};
}

void MessagePort::flush_pending_outgoing_messages()
{
    if (!m_transport || !m_transport->is_open())
        return;

    auto pending_outgoing_messages = move(m_pending_outgoing_messages);
    for (auto const& pending_message : pending_outgoing_messages)
        post_port_message(pending_message);
}

void MessagePort::dispatch_pending_messages()
{
    auto pending_messages = move(m_pending_incoming_messages);
    for (auto& pending_message : pending_messages)
        queue_message_task(move(pending_message));

    if (m_should_shutdown_on_enable) {
        m_should_shutdown_on_enable = false;
        queue_global_task(Task::Source::PostedMessage, relevant_global_object(*this), GC::create_function(heap(), [this] {
            this->close();
        }));
    }
}

void MessagePort::queue_message_task(SerializedTransferRecord&& serialize_with_transfer_result)
{
    queue_global_task(Task::Source::PostedMessage, relevant_global_object(*this), GC::create_function(heap(), [this, serialize_with_transfer_result = move(serialize_with_transfer_result)]() mutable {
        this->post_message_task_steps(serialize_with_transfer_result);
    }));
}

void MessagePort::drain_transport()
{
    if (!m_transport)
        return;

    auto schedule_shutdown = m_transport->read_as_many_messages_as_possible_without_blocking([this](auto&& raw_message) {
        FixedMemoryStream stream { raw_message.bytes.span(), FixedMemoryStream::Mode::ReadOnly };
        IPC::Decoder decoder { stream, raw_message.attachments };

        m_pending_incoming_messages.append(MUST(decoder.decode<SerializedTransferRecord>()));
    });

    if (schedule_shutdown == IPC::Transport::ShouldShutdown::Yes)
        m_should_shutdown_on_enable = true;
}

void MessagePort::post_port_message(SerializedTransferRecord const& serialize_with_transfer_result)
{
    if (!m_transport || !m_transport->is_open())
        return;
    if (auto result = send_message_on_transport(serialize_with_transfer_result); result.is_error()) {
        dbgln("Failed to post message: {}", result.error());
        disentangle();
    }
}

void MessagePort::read_from_transport()
{
    if (!is_entangled())
        return;

    drain_transport();

    if (m_enabled)
        dispatch_pending_messages();
}

void MessagePort::post_message_task_steps(SerializedTransferRecord& serialize_with_transfer_result)
{
    auto const task_started_at = MonotonicTime::now();
    auto deserialize_us = 0uz;
    auto dispatch_us = 0uz;
    auto event_name = "message"sv;
    auto const threshold = mundo_posted_message_log_threshold();

    // 1. Let finalTargetPort be the MessagePort in whose port message queue the task now finds itself.
    // NOTE: This can be different from targetPort, if targetPort itself was transferred and thus all its tasks moved along with it.
    auto* final_target_port = this;

    // IMPLEMENTATION DEFINED:
    // https://html.spec.whatwg.org/multipage/workers.html#dedicated-workers-and-the-worker-interface
    //      Worker objects act as if they had an implicit MessagePort associated with them.
    //      All messages received by that port must immediately be retargeted at the Worker object.
    // We therefore set a special event target for those implicit ports on the Worker and the WorkerGlobalScope objects
    EventTarget* message_event_target = final_target_port;
    if (m_worker_event_target != nullptr) {
        message_event_target = m_worker_event_target;
    }

    // 2. Let targetRealm be finalTargetPort's relevant realm.
    auto& target_realm = relevant_realm(*final_target_port);

    TemporaryExecutionContext context { target_realm };

    // 3. Let deserializeRecord be StructuredDeserializeWithTransfer(serializeWithTransferResult, targetRealm).
    auto const deserialize_started_at = MonotonicTime::now();
    auto deserialize_record_or_error = structured_deserialize_with_transfer(serialize_with_transfer_result, target_realm);
    deserialize_us = (MonotonicTime::now() - deserialize_started_at).to_microseconds();
    if (deserialize_record_or_error.is_error()) {
        event_name = "messageerror"sv;
        // If this throws an exception, catch it, fire an event named messageerror at finalTargetPort, using MessageEvent, and then return.
        auto exception = deserialize_record_or_error.release_error();
        MessageEventInit event_init {};
        auto const dispatch_started_at = MonotonicTime::now();
        message_event_target->dispatch_event(MessageEvent::create(target_realm, HTML::EventNames::messageerror, event_init));
        dispatch_us = (MonotonicTime::now() - dispatch_started_at).to_microseconds();
        auto const total_ms = (MonotonicTime::now() - task_started_at).to_milliseconds();
        if (AK::Duration::from_milliseconds(total_ms) >= threshold)
            dbgln("MUNDO_POSTED_MESSAGE kind=message_port event={} total_ms={} deserialize_us={} dispatch_us={} worker_target={} error={} port={}",
                event_name, total_ms, deserialize_us, dispatch_us, m_worker_event_target != nullptr, exception, this);
        return;
    }
    auto deserialize_record = deserialize_record_or_error.release_value();

    // 4. Let messageClone be deserializeRecord.[[Deserialized]].
    auto message_clone = deserialize_record.deserialized;
    auto const message_is_null_tick = message_clone.is_null() && !m_worker_event_target;
    auto const before_dispatch = MonotonicTime::now();
    if (message_is_null_tick && mundo_message_port_null_tick_coalesce_enabled()) {
        auto const min_interval = mundo_message_port_null_tick_min_interval();
        auto const in_slow_cooldown = m_mundo_null_message_cooldown_until.has_value() && before_dispatch < *m_mundo_null_message_cooldown_until;
        auto const too_soon = !min_interval.is_zero()
            && m_mundo_last_null_message_dispatch_time.has_value()
            && before_dispatch - *m_mundo_last_null_message_dispatch_time < min_interval;
        if (in_slow_cooldown || too_soon) {
            ++m_mundo_null_message_skip_count;
            if (m_mundo_null_message_skip_count <= 24 || m_mundo_null_message_skip_count % 120 == 0) {
                auto remaining_cooldown = in_slow_cooldown ? (*m_mundo_null_message_cooldown_until - before_dispatch).to_milliseconds() : 0;
                auto since_last = m_mundo_last_null_message_dispatch_time.has_value() ? (before_dispatch - *m_mundo_last_null_message_dispatch_time).to_milliseconds() : -1;
                dbgln("MUNDO_POSTED_MESSAGE kind=message_port event=message action=skip_null_tick count={} reason={} since_last={}ms min_interval={}ms cooldown_remaining={}ms port={}",
                    m_mundo_null_message_skip_count,
                    in_slow_cooldown ? "slow_cooldown"sv : "min_interval"sv,
                    since_last,
                    min_interval.to_milliseconds(),
                    remaining_cooldown,
                    this);
            }
            return;
        }
    }

    // 5. Let newPorts be a new frozen array consisting of all MessagePort objects in deserializeRecord.[[TransferredValues]], if any, maintaining their relative order.
    // FIXME: Use a FrozenArray
    Vector<GC::Root<MessagePort>> new_ports;
    for (auto const& object : deserialize_record.transferred_values) {
        if (is<HTML::MessagePort>(*object)) {
            new_ports.append(as<MessagePort>(*object));
        }
    }
    auto const ports_count = new_ports.size();

    // 6. Fire an event named message at finalTargetPort, using MessageEvent, with the data attribute initialized to messageClone and the ports attribute initialized to newPorts.
    MessageEventInit event_init {};
    event_init.data = message_clone;
    event_init.ports = move(new_ports);
    auto event = MessageEvent::create(target_realm, HTML::EventNames::message, event_init);
    event->set_is_trusted(true);
    auto const dispatch_started_at = MonotonicTime::now();
    message_event_target->dispatch_event(event);
    dispatch_us = (MonotonicTime::now() - dispatch_started_at).to_microseconds();
    auto const total_ms = (MonotonicTime::now() - task_started_at).to_milliseconds();
    if (message_is_null_tick && mundo_message_port_null_tick_coalesce_enabled()) {
        auto const dispatch_duration = AK::Duration::from_microseconds(dispatch_us);
        m_mundo_last_null_message_dispatch_time = MonotonicTime::now();
        if (dispatch_duration >= mundo_message_port_null_tick_slow_threshold()) {
            auto const cooldown = mundo_message_port_null_tick_slow_cooldown();
            if (!cooldown.is_zero()) {
                m_mundo_null_message_cooldown_until = *m_mundo_last_null_message_dispatch_time + cooldown;
                dbgln("MUNDO_POSTED_MESSAGE kind=message_port event=message action=null_tick_slow dispatch={}ms cooldown={}ms port={}",
                    dispatch_duration.to_milliseconds(),
                    cooldown.to_milliseconds(),
                    this);
            }
        }
    }
    if (AK::Duration::from_milliseconds(total_ms) >= threshold)
        dbgln("MUNDO_POSTED_MESSAGE kind=message_port event={} total_ms={} deserialize_us={} dispatch_us={} ports={} worker_target={} port={} payload={}",
            event_name, total_ms, deserialize_us, dispatch_us, ports_count, m_worker_event_target != nullptr, this, mundo_posted_message_payload_summary(message_clone));
}

void MessagePort::enable()
{
    if (!m_enabled) {
        m_enabled = true;
        if (m_transport) {
            read_from_transport();
        } else {
            dispatch_pending_messages();
        }
    }
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-start
void MessagePort::start()
{
    // The start() method steps are to enable this's port message queue, if it is not already enabled.
    enable();
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#dom-messageport-close
void MessagePort::close()
{
    // 1. Set this MessagePort object's [[Detached]] internal slot value to true.
    set_detached(true);

    // 2. If this MessagePort object is entangled, disentangle it.
    if (is_entangled())
        disentangle();

    m_pending_incoming_messages.clear();
    m_pending_outgoing_messages.clear();
    m_should_shutdown_on_enable = false;
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#handler-messageeventtarget-onmessageerror
void MessagePort::set_onmessageerror(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::messageerror, value);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#handler-messageeventtarget-onmessageerror
GC::Ptr<WebIDL::CallbackType> MessagePort::onmessageerror()
{
    return event_handler_attribute(EventNames::messageerror);
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#handler-messageeventtarget-onmessage
void MessagePort::set_onmessage(GC::Ptr<WebIDL::CallbackType> value)
{
    set_event_handler_attribute(EventNames::message, value);

    // https://html.spec.whatwg.org/multipage/web-messaging.html#message-ports:handler-messageeventtarget-onmessage
    // The first time a MessagePort object's onmessage IDL attribute is set, the port's port message queue must be enabled,
    // as if the start() method had been called.
    start();
}

// https://html.spec.whatwg.org/multipage/web-messaging.html#handler-messageeventtarget-onmessage
GC::Ptr<WebIDL::CallbackType> MessagePort::onmessage()
{
    return event_handler_attribute(EventNames::message);
}

}
