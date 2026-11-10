#pragma once

#include "EventLoop.h"
#include "EventLoopEngine.h"
#include "TimerEngine.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace GST {
namespace BASE {

// 注册表辅助操作只由 owner 执行；测试可在 owner 上
// 合成 READ/WRITE 就绪批次，无需依赖线程调度时序。
struct EventLoopTestAccess {
    static std::shared_ptr<TimerEngine> timer_engine(EventLoop& loop);

    // 钩子仅在 start 之前配置；资格钩子由任务副本持有。
    static void timer_hooks(
        TimerEngine& engine,
        std::function<void()> before_post,
        std::function<void()> after_post,
        std::function<void()> before_callback = {},
        std::function<void()> after_callback = {}
    );

    static std::size_t pending_timers(TimerEngine& engine);

    // 只在 start 前设置；在调度锁下发通知，调用方不能阻塞或重入。
    static void timer_wait_hook(TimerEngine& engine, std::function<void()> notify);

    static std::chrono::steady_clock::time_point timer_deadline(const TimerRegistration& timer);

    static std::shared_ptr<EventLoopEngine> engine(EventLoop& loop, std::size_t index);

    static void hooks(
        EventLoopEngine& engine,
        std::function<void()> before,
        std::function<void()> after,
        std::function<void()> published = {}
    );

    static void stop_hook(EventLoop& loop, std::function<void()> hook);

    static IoRegistration registration(EventLoopEngine& engine, std::uint64_t id);

    static EventLoopEngine::Task prepare(
        EventLoopEngine& engine,
        std::uint64_t id,
        Event_type ready
    );

    static bool stale_event_ignored(EventLoopEngine& engine, std::uint64_t id);

    static bool submit(EventLoopEngine& engine, std::function<bool()> operation);

    static std::unique_lock<std::mutex> hold_io_queue(EventLoopEngine& engine);

    static bool removal_pending(const IoRegistration& registration);

    static std::size_t routes(EventLoop& loop);

    static bool post_worker(EventLoop& loop, ThreadPool::Task task);

    static bool owner_running(EventLoopEngine& engine);

    static bool hard_stopping(EventLoop& loop);

    static bool run_active(EventLoop& loop);

    static bool access_closed_with_calls_in_flight(EventLoop& loop);
};

}
}

namespace test {

// 普通测试在当前线程发出停止请求，再等待 run 完成统一收尾。
void finish(
    GST::BASE::EventLoop& loop,
    GST::BASE::ShutdownMode mode = GST::BASE::ShutdownMode::Soft
);

}
