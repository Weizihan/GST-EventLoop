#pragma once

#include "TestSupport.h"
#include "EventLoopEngine.h"

namespace GST {
namespace BASE {

// 注册表辅助操作只由 owner 执行；测试可在 owner 上
// 合成 READ/WRITE 就绪批次，无需依赖线程调度时序。
struct EventLoopTestAccess {
    static std::shared_ptr<EventLoopEngine> engine(EventLoop& loop, std::size_t index) {
        std::lock_guard<std::mutex> lock(loop._state_mutex);
        return index < loop._engines.size() ? loop._engines[index] : nullptr;
    }

    static void hooks(EventLoopEngine& engine, std::function<void()> before,
                      std::function<void()> after,
                      std::function<void()> published = {}) {
        engine._before_callback_begin = std::move(before);
        engine._after_callback_begin = std::move(after);
        engine._after_callback_batch_published = std::move(published);
    }

    static void stop_hook(EventLoop& loop, std::function<void()> hook) {
        loop._execution_state->after_stop_cutoff = std::move(hook);
    }

    static IoRegistration registration(EventLoopEngine& engine, std::uint64_t id) {
        IoRegistration result;
        TEST_CHECK(engine.submit_io_state_task([&] {
            const auto it = engine._registrations.find(id);
            if (it == engine._registrations.end()) return false;
            result = it->second.identity;
            return true;
        }));
        return result;
    }

    static EventLoopEngine::Task prepare(EventLoopEngine& engine, std::uint64_t id,
                                         Event_type ready) {
        EventLoopEngine::Task result;
        TEST_CHECK(engine.submit_io_state_task([&] {
            const auto it = engine._registrations.find(id);
            if (it == engine._registrations.end()) return false;
            it->second.interest = ready;
            std::uint64_t selected = 0;
            return engine.prepare_callback_task({id, ready}, result, selected);
        }));
        return result;
    }

    static bool stale_event_ignored(EventLoopEngine& engine, std::uint64_t id) {
        bool ignored = false;
        TEST_CHECK(engine.submit_io_state_task([&] {
            EventLoopEngine::Task result;
            std::uint64_t selected = 0;
            ignored = !engine.prepare_callback_task(
                {id, Event_type::READ_EVENT}, result, selected);
            return true;
        }));
        return ignored;
    }

    static bool submit(EventLoopEngine& engine, std::function<bool()> operation) {
        return engine.submit_io_state_task(std::move(operation));
    }

    static std::unique_lock<std::mutex> hold_io_queue(EventLoopEngine& engine) {
        return std::unique_lock<std::mutex>(engine._io_state_mutex);
    }

    static bool removal_pending(const IoRegistration& registration) {
        const auto state = registration._registration.lock();
        return state && state->removal_pending.load();
    }

    static std::size_t routes(EventLoop& loop) {
        std::lock_guard<std::mutex> lock(loop._state_mutex);
        return loop._registration_fds.size();
    }

    static bool post_worker(EventLoop& loop, ThreadPool::Task task) {
        return loop._callback_pool.post(std::move(task));
    }

    static bool owner_running(EventLoopEngine& engine) {
        return engine._is_running.load();
    }

    static bool hard_stopping(EventLoop& loop) {
        std::lock_guard<std::mutex> lock(loop._state_mutex);
        return loop._execution_state->hard_stopping;
    }

    static bool run_active(EventLoop& loop) {
        std::lock_guard<std::mutex> lock(loop._state_mutex);
        return loop._run_active;
    }

    static bool access_closed_with_calls_in_flight(EventLoop& loop) {
        std::shared_ptr<detail::IoExecutionState> execution;
        {
            std::lock_guard<std::mutex> lock(loop._state_mutex);
            execution = loop._execution_state;
        }
        std::lock_guard<std::mutex> lock(execution->access_mutex);
        return !execution->accepting_calls && execution->active_calls != 0;
    }
};

}
}

namespace test {

// 普通测试在当前线程发出停止请求，再等待 run 完成统一收尾。
inline void finish(GST::BASE::EventLoop& loop,
                   GST::BASE::ShutdownMode mode = GST::BASE::ShutdownMode::Soft) {
    loop.stop(mode);
    TEST_CHECK(loop.run());
}

}
