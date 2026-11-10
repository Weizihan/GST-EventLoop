#include "TimerRegistration.h"

#include <utility>

namespace GST {
namespace BASE {

TimerRegistration::TimerRegistration() noexcept = default;

TimerRegistration::TimerRegistration(const TimerRegistration&) noexcept = default;

TimerRegistration& TimerRegistration::operator=(const TimerRegistration&) noexcept = default;

TimerRegistration::TimerRegistration(TimerRegistration&& other) noexcept
    : _owner(std::exchange(other._owner, nullptr)),
      _generation(std::exchange(other._generation, 0)),
      _timer_id(std::exchange(other._timer_id, 0)),
      _registration(std::move(other._registration)),
      _execution(std::move(other._execution)) {
}

TimerRegistration& TimerRegistration::operator=(TimerRegistration&& other) noexcept {
    if (this != &other) {
        _owner = std::exchange(other._owner, nullptr);
        _generation = std::exchange(other._generation, 0);
        _timer_id = std::exchange(other._timer_id, 0);
        _registration = std::move(other._registration);
        _execution = std::move(other._execution);
    }
    return *this;
}

void TimerRegistration::swap(TimerRegistration& other) noexcept {
    using std::swap;
    swap(_owner, other._owner);
    swap(_generation, other._generation);
    swap(_timer_id, other._timer_id);
    swap(_registration, other._registration);
    swap(_execution, other._execution);
}

TimerRegistration::operator bool() const noexcept {
    return valid();
}

std::uint64_t TimerRegistration::generation() const noexcept {
    return _generation;
}

std::uint64_t TimerRegistration::registration_id() const noexcept {
    return _timer_id;
}

TimerRegistration::TimerRegistration(
    EventLoop* owner,
    std::uint64_t generation,
    std::uint64_t timer_id,
    const std::shared_ptr<detail::TimerRegistrationState>& registration,
    const std::shared_ptr<detail::IoExecutionState>& execution
) noexcept
    : _owner(owner),
      _generation(generation),
      _timer_id(timer_id),
      _registration(registration),
      _execution(execution) {
}

void swap(TimerRegistration& lhs, TimerRegistration& rhs) noexcept {
    lhs.swap(rhs);
}

}
}
