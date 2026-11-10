#pragma once

#include <cstdint>
#include <functional>
#include <memory>

namespace GST {
namespace BASE {

class EventLoop;
class TimerEngine;
class TimerSchedule;
struct TimerContext;
namespace detail {
struct IoExecutionState;
struct TimerRegistrationState;
}

// 可复制、非拥有的 Timer 注册凭证；所有副本指向同一次注册。
// 析构时不取消注册，也不延后完成通知。
class TimerRegistration {
public:
    TimerRegistration() noexcept;
    TimerRegistration(const TimerRegistration&) noexcept;
    TimerRegistration& operator=(const TimerRegistration&) noexcept;

    TimerRegistration(TimerRegistration&& other) noexcept;
    TimerRegistration& operator=(TimerRegistration&& other) noexcept;

    void swap(TimerRegistration& other) noexcept;

    bool valid() const noexcept;

    // 同步修改后续计划或回调；已选中的任务保留原截止时间及回调快照。
    bool update(const TimerSchedule& schedule) const;
    bool set_cb(std::function<void(const TimerContext&)> cb) const;

    // 不等待已经取得资格的回调结束；停止请求生效后返回 false。
    bool cancel() const;

    explicit operator bool() const noexcept;

    std::uint64_t generation() const noexcept;

    std::uint64_t registration_id() const noexcept;

private:
    friend class EventLoop;
    friend class TimerEngine;
#ifdef GST_EVENTLOOP_TESTING
    friend struct EventLoopTestAccess;
#endif

    TimerRegistration(
        EventLoop* owner,
        std::uint64_t generation,
        std::uint64_t timer_id,
        const std::shared_ptr<detail::TimerRegistrationState>& registration,
        const std::shared_ptr<detail::IoExecutionState>& execution
    ) noexcept;

    EventLoop* _owner{nullptr};
    std::uint64_t _generation{0};
    std::uint64_t _timer_id{0};
    std::weak_ptr<detail::TimerRegistrationState> _registration;
    std::weak_ptr<detail::IoExecutionState> _execution;
};

void swap(TimerRegistration& lhs, TimerRegistration& rhs) noexcept;

}
}
