/*
 * Copyright (c) 2021-2024, Andreas Kling <andreas@ladybird.org>
 *
 * SPDX-License-Identifier: BSD-2-Clause
 */

#include <AK/IDAllocator.h>
#include <AK/StringBuilder.h>
#include <LibWeb/DOM/Document.h>
#include <LibWeb/HTML/EventLoop/Task.h>
#include <LibJS/Runtime/VM.h>

#include <stdlib.h>

namespace Web::HTML {

GC_DEFINE_ALLOCATOR(Task);

static IDAllocator s_unique_task_source_allocator { static_cast<int>(Task::Source::UniqueTaskSourceStart) };

[[nodiscard]] static TaskID allocate_task_id()
{
    static u64 next_task_id = 1;
    return next_task_id++;
}

static ByteString mundo_task_creation_stack(JS::VM& vm)
{
    auto const* raw_value = getenv("MUNDO_TASK_CREATION_STACKS");
    if (!raw_value || !atoi(raw_value))
        return {};

    StringBuilder builder;
    auto stack_trace = vm.stack_trace();
    size_t emitted_frames = 0;
    for (auto const& element : stack_trace) {
        auto* context = element.execution_context;
        if (!context)
            continue;

        auto function_name = context->function ? context->function->name_for_call_stack() : ""_utf16;
        auto function_name_utf8 = function_name.is_empty() ? ByteString { "<anonymous>"sv } : function_name.to_byte_string();

        if (emitted_frames > 0)
            builder.append(" <- "sv);

        builder.append(function_name_utf8);
        if (auto source_range = element.source_range; source_range.has_value()) {
            auto source = source_range->filename();
            if (!source.is_empty()) {
                builder.append('@');
                builder.append(source);
            }
            builder.appendff(":{}:{}", source_range->start.line, source_range->start.column);
        }

        if (++emitted_frames >= 10)
            break;
    }

    return builder.to_byte_string();
}

GC::Ref<Task> Task::create(JS::VM& vm, Source source, GC::Ptr<DOM::Document const> document, GC::Ref<GC::Function<void()>> steps)
{
    auto task = vm.heap().allocate<Task>(source, document, move(steps));
    task->m_mundo_creation_stack = mundo_task_creation_stack(vm);
    return task;
}

Task::Task(Source source, GC::Ptr<DOM::Document const> document, GC::Ref<GC::Function<void()>> steps)
    : m_id(allocate_task_id())
    , m_source(source)
    , m_steps(steps)
    , m_document(document)
{
}

Task::~Task() = default;

void Task::visit_edges(Visitor& visitor)
{
    Base::visit_edges(visitor);
    visitor.visit(m_steps);
    visitor.visit(m_document);
}

void Task::execute()
{
    m_steps->function()();
}

// https://html.spec.whatwg.org/multipage/webappapis.html#concept-task-runnable
bool Task::is_runnable() const
{
    // A task is runnable if its document is either null or fully active.
    return !m_document || m_document->is_fully_active();
}

bool Task::is_permanently_unrunnable() const
{
    return m_document && m_document->has_been_destroyed();
}

DOM::Document const* Task::document() const
{
    return m_document.ptr();
}

UniqueTaskSource::UniqueTaskSource()
    : source(static_cast<Task::Source>(s_unique_task_source_allocator.allocate()))
{
}

UniqueTaskSource::~UniqueTaskSource()
{
    s_unique_task_source_allocator.deallocate(static_cast<int>(source));
}

NonnullRefPtr<ParallelQueue> ParallelQueue::create()
{
    return adopt_ref(*new (nothrow) ParallelQueue);
}

TaskID ParallelQueue::enqueue(GC::Ref<GC::Function<void()>> algorithm)
{
    auto& event_loop = HTML::main_thread_event_loop();
    auto task = HTML::Task::create(event_loop.vm(), m_task_source.source, nullptr, algorithm);
    event_loop.task_queue().add(task);
    return task->id();
}

}
