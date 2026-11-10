#include "TimerEngine.h"

#include "EventLoop.h"
#include "InternalLog.h"
#include "IoRegistrationState.h"

#include <limits>
#include <type_traits>
#include <utility>

namespace GST {
namespace BASE {
namespace {

thread_local TimerEngine* current_timer_owner = nullptr;

void retire_callback(const std::shared_ptr<detail::TimerRegistrationState>& registration) {
    std::lock_guard<std::mutex> lock(registration->mutex);
    --registration->in_flight;
    if (!registration->scheduled && registration->in_flight == 0) {
        registration->registered = false;
    }
}

// 只由调度线程的迟到跳过历史计划点；不读取 worker 的执行进度。
bool next_deadline(TimerEngine::Clock::time_point deadline,
                   TimerEngine::Clock::time_point now,
                   TimerEngine::Clock::duration interval,
                   TimerEngine::Clock::time_point& next) {
    using Clock = TimerEngine::Clock;
    using Rep = Clock::duration::rep;
    using Unsigned = std::make_unsigned_t<Rep>;
    const Unsigned elapsed = static_cast<Unsigned>(now.time_since_epoch().count()) -
                             static_cast<Unsigned>(deadline.time_since_epoch().count());
    const Unsigned ticks = static_cast<Unsigned>(interval.count());
    const auto advance = Clock::duration(static_cast<Rep>(ticks - elapsed % ticks));
    next = now.time_since_epoch() > Clock::duration::max() - advance
        ? Clock::time_point::max() : now + advance;
    return next > now;
}

}

TimerEngine::TimerBatch::~TimerBatch() {
    // 每次任务独立保留快照；快照捕获先于该任务的状态引用释放。
    callback.reset();
    if (registration) {
        retire_callback(registration);
    }
}

bool TimerEngine::PendingOrder::operator()(
    const PendingTimer& lhs,
    const PendingTimer& rhs
) const noexcept {
    if (lhs.deadline != rhs.deadline) {
        return lhs.deadline < rhs.deadline;
    }
    return lhs.timer_id < rhs.timer_id;
}

TimerEngine::~TimerEngine() {
    stop();
}

bool TimerEngine::is_owner_thread() noexcept {
    return current_timer_owner != nullptr;
}

bool TimerEngine::init(CallbackPoster callback_poster) {
    if (!callback_poster) {
        GST_EVENTLOOP_LOG_ERROR("timer callback poster is empty");
        return false;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if (_initialized) {
        return false;
    }
    _callback_poster = std::move(callback_poster);
    _initialized = true;
    return true;
}

bool TimerEngine::run() {
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_initialized || _stopping) {
        return false;
    }
    if (_started) {
        return true;
    }

    try {
        _runner = std::thread(&TimerEngine::loop_thread_main, this);
    } catch (...) {
        GST_EVENTLOOP_LOG_ERROR("timer thread startup failed");
        return false;
    }
    _started = true;
    return true;
}

void TimerEngine::enable_dispatch() {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_started || _stopping) {
            return;
        }
        _dispatch_enabled = true;
    }
    _cv.notify_one();
}

void TimerEngine::request_stop() {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _stopping = true;
    }
    _cv.notify_all();
}

void TimerEngine::stop() {
    request_stop();
    if (current_timer_owner == this) {
        // owner 只能请求退出；同步收尾由 Loop 的外部生命周期调用完成。
        GST_EVENTLOOP_LOG_ERROR("timer owner cannot join itself");
        return;
    }

    std::thread runner;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_runner.joinable()) {
            runner = std::move(_runner);
        }
    }
    if (runner.joinable()) {
        runner.join();
    }
    clear_pending();
}

bool TimerEngine::add(
    const TimerRegistration& identity,
    Clock::time_point deadline,
    Clock::duration interval,
    std::shared_ptr<detail::TimerRegistrationState> registration
) {
    if (identity.registration_id() == 0 || !registration || !registration->callback) {
        return false;
    }

    bool inserted = false;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (!_initialized || _stopping) {
            return false;
        }
        inserted = _pending.insert(PendingTimer{
            deadline, identity.registration_id(), identity, registration
        }).second;
        if (inserted) {
            registration->deadline = deadline;
            registration->interval = interval;
            registration->accepted = true;
        }
    }
    if (inserted) {
        _cv.notify_one();
    }
    return inserted;
}

bool TimerEngine::update(
    const TimerRegistration& identity,
    Clock::time_point deadline,
    Clock::duration interval,
    const std::shared_ptr<detail::TimerRegistrationState>& registration
) {
    if (!registration || identity.registration_id() == 0) {
        return false;
    }

    PendingTimers::node_type pending;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::lock_guard<std::mutex> registration_lock(registration->mutex);
        if (_stopping || !registration->registered || registration->revoked ||
            registration->schedule_generation == std::numeric_limits<std::uint64_t>::max()) {
            return false;
        }

        if (registration->scheduled) {
            const auto it = _pending.find(PendingTimer{
                registration->deadline, identity.registration_id(), {}, {}
            });
            if (it == _pending.end()) {
                return false;
            }
            pending = _pending.extract(it);
            pending.value().deadline = deadline;
            _pending.insert(std::move(pending));
        } else {
            // 单次已选中时可重新挂计划；分配失败前不改变任何旧状态。
            if (!_pending.insert(PendingTimer{
                    deadline, identity.registration_id(), identity, registration
                }).second) {
                return false;
            }
        }

        registration->deadline = deadline;
        registration->interval = interval;
        registration->scheduled = true;
        ++registration->schedule_generation;
    }
    _cv.notify_one();
    return true;
}

bool TimerEngine::set_callback(
    const std::shared_ptr<detail::TimerRegistrationState>& registration,
    detail::TimerCallbackSnapshot& callback
) {
    if (!registration || !callback || !*callback) {
        return false;
    }
    std::lock_guard<std::mutex> lock(_mutex);
    std::lock_guard<std::mutex> registration_lock(registration->mutex);
    if (_stopping || !registration->registered || registration->revoked) {
        return false;
    }
    // 旧快照交回调用方，在 Loop 和注册锁都释放之后再析构。
    callback.swap(registration->callback);
    return true;
}

void TimerEngine::remove(
    const std::shared_ptr<detail::TimerRegistrationState>& registration,
    std::uint64_t timer_id
) {
    PendingTimers::node_type removed;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::lock_guard<std::mutex> registration_lock(registration->mutex);
        if (registration->scheduled) {
            const auto it = _pending.find(PendingTimer{registration->deadline, timer_id, {}, {}});
            if (it != _pending.end()) {
                removed = _pending.extract(it);
            }
            registration->scheduled = false;
        }
        if (registration->in_flight == 0) {
            registration->registered = false;
        }
    }
    _cv.notify_one();
    // removed 的强引用在队列锁外释放；调用方另持状态以覆盖管理操作。
}

void TimerEngine::loop_thread_main() {
    current_timer_owner = this;
    for (;;) {
        PendingTimers::node_type pending;
        PendingTimer timer;
        detail::TimerCallbackSnapshot callback;
        std::uint64_t schedule_generation = 0;
        bool periodic = false;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            while (!_stopping) {
                if (!_dispatch_enabled || _pending.empty()) {
#ifdef GST_EVENTLOOP_TESTING
                    if (_dispatch_enabled && _before_wait) {
                        _before_wait();
                    }
#endif
                    _cv.wait(lock);
                    continue;
                }

                const auto it = _pending.begin();
                const Clock::time_point deadline = it->deadline;
                if (deadline > Clock::now()) {
#ifdef GST_EVENTLOOP_TESTING
                    if (_before_wait) {
                        _before_wait();
                    }
#endif
                    _cv.wait_until(lock, deadline);
                    continue;
                }

                pending = _pending.extract(it);
                timer = pending.value();
                auto& registration = *timer.registration;
                std::lock_guard<std::mutex> registration_lock(registration.mutex);
                if (registration.revoked || !registration.registered) {
                    registration.scheduled = false;
                    break;
                }

                ++registration.in_flight;
                callback = registration.callback;
                schedule_generation = registration.schedule_generation;
                periodic = registration.interval > Clock::duration::zero();
                Clock::time_point next;
                if (periodic &&
                    next_deadline(deadline, Clock::now(), registration.interval, next)) {
                    // 节点复用不分配；旧任务投递前已完成重排，update 不会被它覆盖。
                    pending.value().deadline = next;
                    registration.deadline = next;
                    _pending.insert(std::move(pending));
                } else {
                    registration.scheduled = false;
                }
                break;
            }
            if (_stopping) {
                break;
            }
        }

        // 本次选择的状态保活跨越投递；future 节点与各个在途任务互相独立。
        if (schedule_generation != 0) {
            dispatch_timer(timer, std::move(callback), schedule_generation, periodic);
        }
    }
    clear_pending();
    current_timer_owner = nullptr;
}

void TimerEngine::dispatch_timer(
    const PendingTimer& timer,
    detail::TimerCallbackSnapshot callback,
    std::uint64_t schedule_generation,
    bool periodic
) {
    bool transferred = false;
    try {
        auto batch = std::make_shared<TimerBatch>();
        batch->registration = timer.registration;
        batch->callback = std::move(callback);
        transferred = true;
        const TimerRegistration identity = timer.identity;
        const Clock::time_point deadline = timer.deadline;
#ifdef GST_EVENTLOOP_TESTING
        const auto before_callback_begin = _before_callback_begin;
        const auto after_callback_begin = _after_callback_begin;
#endif
        std::vector<ThreadPool::Task> tasks;
        tasks.emplace_back([batch, identity, deadline, periodic
#ifdef GST_EVENTLOOP_TESTING
                            , before_callback_begin, after_callback_begin
#endif
        ] {
#ifdef GST_EVENTLOOP_TESTING
            if (before_callback_begin) {
                before_callback_begin();
            }
#endif
            bool permitted = false;
            {
                std::lock_guard<std::mutex> execution_lock(batch->registration->execution->mutex);
                std::lock_guard<std::mutex> registration_lock(batch->registration->mutex);
                permitted = !batch->registration->execution->hard_stopping &&
                            !batch->registration->revoked;
            }
            if (!permitted) {
                return;
            }
#ifdef GST_EVENTLOOP_TESTING
            if (after_callback_begin) {
                after_callback_begin();
            }
#endif
            try {
                (*batch->callback)(TimerContext{deadline, identity});
            } catch (...) {
                GST_EVENTLOOP_LOG_ERROR(
                    "timer callback threw timer_id=%llu",
                    static_cast<unsigned long long>(identity.registration_id()));
                if (periodic && !identity.cancel()) {
                    // Stop 后公开取消被拒绝，但异常仍须撤销本 Timer 的未资格任务。
                    // Stop 已关闭产出并负责清理 future；这里只补执行门禁，不等待回调。
                    std::lock_guard<std::mutex> execution_lock(batch->registration->execution->mutex);
                    std::lock_guard<std::mutex> registration_lock(batch->registration->mutex);
                    batch->registration->revoked = true;
                    batch->registration->registered = false;
                }
            }
        });

#ifdef GST_EVENTLOOP_TESTING
        if (_before_post) {
            _before_post();
        }
#endif
        bool posted = false;
        try {
            posted = _callback_poster(std::move(tasks));
        } catch (...) {
            GST_EVENTLOOP_LOG_ERROR("timer callback batch post threw");
        }
        if (!posted) {
            finish_failed_schedule(timer, schedule_generation);
            GST_EVENTLOOP_LOG_ERROR(
                "timer callback batch rejected timer_id=%llu",
                static_cast<unsigned long long>(identity.registration_id()));
        }
#ifdef GST_EVENTLOOP_TESTING
        if (posted && _after_post) {
            _after_post();
        }
#endif
    } catch (...) {
        finish_failed_schedule(timer, schedule_generation);
        if (!transferred) {
            callback.reset();
            retire_callback(timer.registration);
        }
        GST_EVENTLOOP_LOG_ERROR(
            "timer callback task preparation failed timer_id=%llu",
            static_cast<unsigned long long>(timer.timer_id));
    }
}

void TimerEngine::finish_failed_schedule(const PendingTimer& timer,
                                         std::uint64_t schedule_generation) {
    PendingTimers::node_type removed;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        std::lock_guard<std::mutex> registration_lock(timer.registration->mutex);
        auto& registration = *timer.registration;
        // 新 update 的计划不受旧选择随后发生的投递失败影响。
        if (registration.schedule_generation != schedule_generation) {
            return;
        }
        if (registration.scheduled) {
            const auto it = _pending.find(PendingTimer{
                registration.deadline, timer.timer_id, {}, {}
            });
            if (it != _pending.end()) {
                removed = _pending.extract(it);
            }
            registration.scheduled = false;
        }
        if (registration.in_flight == 0) {
            registration.registered = false;
        }
    }
    _cv.notify_one();
}

void TimerEngine::clear_pending() {
    PendingTimers pending;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        pending.swap(_pending);
    }
    for (const auto& timer : pending) {
        std::lock_guard<std::mutex> registration_lock(timer.registration->mutex);
        timer.registration->scheduled = false;
        if (timer.registration->in_flight == 0) {
            timer.registration->registered = false;
        }
    }
    // 用户捕获和 on_done 的最后引用在队列锁外释放。
}

}
}
