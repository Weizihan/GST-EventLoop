#pragma once

#include <chrono>

namespace GST {
namespace BASE {

class EventLoop;

// 定时计划；相对时间从注册或更新入口采样的时刻起算。
class TimerSchedule {
public:
    TimerSchedule() noexcept;

    static TimerSchedule after(std::chrono::nanoseconds delay) noexcept;

    static TimerSchedule at(std::chrono::steady_clock::time_point deadline) noexcept;

    // 固定频率；各次回调可以排队或并发，不参与下次截止时间计算。
    static TimerSchedule every(std::chrono::nanoseconds interval) noexcept;

    bool valid() const noexcept;

private:
    friend class EventLoop;

    bool _relative{false};
    bool _repeat{false};
    std::chrono::nanoseconds _delay{0};
    std::chrono::steady_clock::time_point _deadline;
    bool _valid{false};
};

}
}
