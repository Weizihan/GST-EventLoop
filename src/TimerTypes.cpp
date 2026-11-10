#include "TimerTypes.h"

namespace GST {
namespace BASE {

TimerSchedule::TimerSchedule() noexcept = default;

TimerSchedule TimerSchedule::after(std::chrono::nanoseconds delay) noexcept {
    TimerSchedule schedule;
    schedule._relative = true;
    schedule._delay = delay;
    schedule._valid = delay >= std::chrono::nanoseconds::zero();
    return schedule;
}

TimerSchedule TimerSchedule::at(std::chrono::steady_clock::time_point deadline) noexcept {
    TimerSchedule schedule;
    schedule._deadline = deadline;
    schedule._valid = true;
    return schedule;
}

TimerSchedule TimerSchedule::every(std::chrono::nanoseconds interval) noexcept {
    TimerSchedule schedule;
    schedule._relative = true;
    schedule._repeat = true;
    schedule._delay = interval;
    schedule._valid = interval > std::chrono::nanoseconds::zero();
    return schedule;
}

bool TimerSchedule::valid() const noexcept {
    return _valid;
}

}
}
