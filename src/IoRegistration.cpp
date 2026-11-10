#include "IoRegistration.h"

#include <utility>

namespace GST {
namespace BASE {

IoRegistration::IoRegistration() noexcept = default;

IoRegistration::IoRegistration(const IoRegistration&) noexcept = default;

IoRegistration& IoRegistration::operator=(const IoRegistration&) noexcept = default;

IoRegistration::IoRegistration(IoRegistration&& other) noexcept
    : _owner(std::exchange(other._owner, nullptr)),
      _generation(std::exchange(other._generation, 0)),
      _engine_id(std::exchange(other._engine_id, 0)),
      _handle_id(std::exchange(other._handle_id, 0)),
      _registration(std::move(other._registration)),
      _execution(std::move(other._execution)) {
}

IoRegistration& IoRegistration::operator=(IoRegistration&& other) noexcept {
    if (this != &other) {
        _owner = std::exchange(other._owner, nullptr);
        _generation = std::exchange(other._generation, 0);
        _engine_id = std::exchange(other._engine_id, 0);
        _handle_id = std::exchange(other._handle_id, 0);
        _registration = std::move(other._registration);
        _execution = std::move(other._execution);
    }
    return *this;
}

void IoRegistration::swap(IoRegistration& other) noexcept {
    using std::swap;
    swap(_owner, other._owner);
    swap(_generation, other._generation);
    swap(_engine_id, other._engine_id);
    swap(_handle_id, other._handle_id);
    swap(_registration, other._registration);
    swap(_execution, other._execution);
}

IoRegistration::operator bool() const noexcept {
    return valid();
}

std::uint64_t IoRegistration::generation() const noexcept {
    return _generation;
}

std::uint64_t IoRegistration::registration_id() const noexcept {
    return _handle_id;
}

IoRegistration::IoRegistration(
    EventLoop* owner,
    std::uint64_t generation,
    std::size_t engine_id,
    std::uint64_t handle_id,
    const std::shared_ptr<detail::IoRegistrationState>& registration,
    const std::shared_ptr<detail::IoExecutionState>& execution
) noexcept
    : _owner(owner),
      _generation(generation),
      _engine_id(engine_id),
      _handle_id(handle_id),
      _registration(registration),
      _execution(execution) {
}

void swap(IoRegistration& lhs, IoRegistration& rhs) noexcept {
    lhs.swap(rhs);
}

}
}
