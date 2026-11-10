#include "EventLoopTestAccess.h"

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

}
}
