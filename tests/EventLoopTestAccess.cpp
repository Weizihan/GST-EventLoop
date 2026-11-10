#include "EventLoopTestAccess.h"

#include "TestSupport.h"

#include <utility>

namespace GST {
namespace BASE {

std::shared_ptr<TimerEngine> EventLoopTestAccess::timer_engine(EventLoop& loop) {
    std::lock_guard<std::mutex> lock(loop._state_mutex);
    return loop._timer_engine;
}

void EventLoopTestAccess::timer_hooks(
    TimerEngine& engine,
    std::function<void()> before_post,
    std::function<void()> after_post,
    std::function<void()> before_callback,
    std::function<void()> after_callback
) {
    engine._before_post = std::move(before_post);
    engine._after_post = std::move(after_post);
    engine._before_callback_begin = std::move(before_callback);
    engine._after_callback_begin = std::move(after_callback);
}

std::size_t EventLoopTestAccess::pending_timers(TimerEngine& engine) {
    std::lock_guard<std::mutex> lock(engine._mutex);
    return engine._pending.size();
}

void EventLoopTestAccess::timer_wait_hook(
    TimerEngine& engine,
    std::function<void()> notify
) {
    engine._before_wait = std::move(notify);
}

std::chrono::steady_clock::time_point EventLoopTestAccess::timer_deadline(
    const TimerRegistration& timer
) {
    const auto state = timer._registration.lock();
    TEST_CHECK(state);
    std::lock_guard<std::mutex> lock(state->mutex);
    return state->deadline;
}

std::shared_ptr<EventLoopEngine> EventLoopTestAccess::engine(
    EventLoop& loop,
    std::size_t index
) {
    std::lock_guard<std::mutex> lock(loop._state_mutex);
    return index < loop._engines.size() ? loop._engines[index] : nullptr;
}

void EventLoopTestAccess::hooks(
    EventLoopEngine& engine,
    std::function<void()> before,
    std::function<void()> after,
    std::function<void()> published
) {
    engine._before_callback_begin = std::move(before);
    engine._after_callback_begin = std::move(after);
    engine._after_callback_batch_published = std::move(published);
}

void EventLoopTestAccess::stop_hook(EventLoop& loop, std::function<void()> hook) {
    loop._execution_state->after_stop_cutoff = std::move(hook);
}

IoRegistration EventLoopTestAccess::registration(
    EventLoopEngine& engine,
    std::uint64_t id
) {
    IoRegistration result;
    TEST_CHECK(engine.submit_io_state_task([&] {
        const auto it = engine._registrations.find(id);
        if (it == engine._registrations.end()) {
            return false;
        }
        result = it->second.identity;
        return true;
    }));
    return result;
}

EventLoopEngine::Task EventLoopTestAccess::prepare(
    EventLoopEngine& engine,
    std::uint64_t id,
    Event_type ready
) {
    EventLoopEngine::Task result;
    TEST_CHECK(engine.submit_io_state_task([&] {
        const auto it = engine._registrations.find(id);
        if (it == engine._registrations.end()) {
            return false;
        }
        it->second.interest = ready;
        std::uint64_t selected = 0;
        return engine.prepare_callback_task({id, ready}, result, selected);
    }));
    return result;
}

bool EventLoopTestAccess::stale_event_ignored(EventLoopEngine& engine, std::uint64_t id) {
    bool ignored = false;
    TEST_CHECK(engine.submit_io_state_task([&] {
        EventLoopEngine::Task result;
        std::uint64_t selected = 0;
        ignored = !engine.prepare_callback_task(
            {id, Event_type::READ_EVENT},
            result,
            selected
        );
        return true;
    }));
    return ignored;
}

bool EventLoopTestAccess::submit(
    EventLoopEngine& engine,
    std::function<bool()> operation
) {
    return engine.submit_io_state_task(std::move(operation));
}

std::unique_lock<std::mutex> EventLoopTestAccess::hold_io_queue(EventLoopEngine& engine) {
    return std::unique_lock<std::mutex>(engine._io_state_mutex);
}

bool EventLoopTestAccess::removal_pending(const IoRegistration& registration) {
    const auto state = registration._registration.lock();
    return state && state->removal_pending.load();
}

std::size_t EventLoopTestAccess::routes(EventLoop& loop) {
    std::lock_guard<std::mutex> lock(loop._state_mutex);
    return loop._registration_fds.size();
}

bool EventLoopTestAccess::post_worker(EventLoop& loop, ThreadPool::Task task) {
    return loop._callback_pool.post(std::move(task));
}

bool EventLoopTestAccess::owner_running(EventLoopEngine& engine) {
    return engine._is_running.load();
}

bool EventLoopTestAccess::hard_stopping(EventLoop& loop) {
    std::lock_guard<std::mutex> lock(loop._state_mutex);
    return loop._execution_state->hard_stopping;
}

bool EventLoopTestAccess::run_active(EventLoop& loop) {
    std::lock_guard<std::mutex> lock(loop._state_mutex);
    return loop._run_active;
}

bool EventLoopTestAccess::access_closed_with_calls_in_flight(EventLoop& loop) {
    std::shared_ptr<detail::IoExecutionState> execution;
    {
        std::lock_guard<std::mutex> lock(loop._state_mutex);
        execution = loop._execution_state;
    }
    std::lock_guard<std::mutex> lock(execution->access_mutex);
    return !execution->accepting_calls && execution->active_calls != 0;
}

}
}

namespace test {

void finish(GST::BASE::EventLoop& loop, GST::BASE::ShutdownMode mode) {
    loop.stop(mode);
    TEST_CHECK(loop.run());
}

}
