#pragma once

#include "ThreadPool.h"
#include "TimerRegistration.h"
#include "TimerRegistrationState.h"

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace GST {
namespace BASE {

class TimerEngine {
public:
    using Clock = std::chrono::steady_clock;
    using CallbackPoster = std::function<bool(std::vector<ThreadPool::Task>)>;

    ~TimerEngine();
    bool init(CallbackPoster callback_poster);
    bool run();
    void enable_dispatch();
    void request_stop();
    void stop();

    static bool is_owner_thread() noexcept;

    bool add(
        const TimerRegistration& identity,
        Clock::time_point deadline,
        Clock::duration interval,
        std::shared_ptr<detail::TimerRegistrationState> registration
    );
    bool update(
        const TimerRegistration& identity,
        Clock::time_point deadline,
        Clock::duration interval,
        const std::shared_ptr<detail::TimerRegistrationState>& registration
    );
    bool set_callback(
        const std::shared_ptr<detail::TimerRegistrationState>& registration,
        detail::TimerCallbackSnapshot& callback
    );
    void remove(
        const std::shared_ptr<detail::TimerRegistrationState>& registration,
        std::uint64_t timer_id
    );

private:
#ifdef GST_EVENTLOOP_TESTING
    friend struct EventLoopTestAccess;
#endif

    struct PendingTimer {
        Clock::time_point deadline;
        std::uint64_t timer_id{0};
        TimerRegistration identity;
        std::shared_ptr<detail::TimerRegistrationState> registration;
    };

    struct PendingOrder {
        bool operator()(const PendingTimer& lhs, const PendingTimer& rhs) const noexcept;
    };

    struct TimerBatch {
        std::shared_ptr<detail::TimerRegistrationState> registration;
        detail::TimerCallbackSnapshot callback;
        ~TimerBatch();
    };

    using PendingTimers = std::set<PendingTimer, PendingOrder>;

    void loop_thread_main();
    void dispatch_timer(
        const PendingTimer& timer,
        detail::TimerCallbackSnapshot callback,
        std::uint64_t schedule_generation,
        bool periodic
    );
    void finish_failed_schedule(const PendingTimer& timer, std::uint64_t schedule_generation);
    void clear_pending();

    std::mutex _mutex;
    std::condition_variable _cv;
    std::thread _runner;
    bool _initialized{false};
    bool _started{false};
    bool _stopping{false};
    bool _dispatch_enabled{false};
    PendingTimers _pending;
    CallbackPoster _callback_poster;
#ifdef GST_EVENTLOOP_TESTING
    std::function<void()> _before_post;
    std::function<void()> _after_post;
    std::function<void()> _before_callback_begin;
    std::function<void()> _after_callback_begin;
    // 等待钩子只能发出测试通知，不得阻塞或重入 Loop。
    std::function<void()> _before_wait;
#endif
};

}
}
