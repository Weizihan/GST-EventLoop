#pragma once

#include "EventTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <utility>

namespace GST {
namespace BASE {

class EventLoop;
class EventLoopEngine;
struct IoContext;
namespace detail { struct IoRegistrationState; struct IoExecutionState; }

// 可复制、非拥有的 IO 注册凭证；所有副本指向同一次注册。
// 不拥有原生资源，析构时不取消注册，也不延后完成通知。
class IoRegistration {
public:
    IoRegistration() noexcept = default;
    IoRegistration(const IoRegistration&) noexcept = default;
    IoRegistration& operator=(const IoRegistration&) noexcept = default;

    IoRegistration(IoRegistration&& other) noexcept
        : _owner(std::exchange(other._owner, nullptr)),
          _generation(std::exchange(other._generation, 0)),
          _engine_id(std::exchange(other._engine_id, 0)),
          _handle_id(std::exchange(other._handle_id, 0)),
          _registration(std::move(other._registration)),
          _execution(std::move(other._execution)) {
    }

    IoRegistration& operator=(IoRegistration&& other) noexcept {
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

    void swap(IoRegistration& other) noexcept {
        using std::swap;
        swap(_owner, other._owner);
        swap(_generation, other._generation);
        swap(_engine_id, other._engine_id);
        swap(_handle_id, other._handle_id);
        swap(_registration, other._registration);
        swap(_execution, other._execution);
    }

    bool valid() const noexcept;

    // 与 Loop 上的对应操作共用实现；失效凭证不会访问已经销毁的 Loop。
    bool set_cb(Event_type type, std::function<void(const IoContext&)> cb) const;
    bool update(Event_type interest) const;
    bool cancel() const;

    explicit operator bool() const noexcept {
        return valid();
    }

    std::uint64_t generation() const noexcept {
        return _generation;
    }

    std::uint64_t registration_id() const noexcept {
        return _handle_id;
    }

private:
    friend class EventLoop;
    friend class EventLoopEngine;
#ifdef GST_EVENTLOOP_TESTING
    friend struct EventLoopTestAccess;
#endif

    IoRegistration(
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

    EventLoop*    _owner{nullptr};
    std::uint64_t _generation{0};
    std::size_t   _engine_id{0};
    std::uint64_t _handle_id{0};
    std::weak_ptr<detail::IoRegistrationState> _registration;
    std::weak_ptr<detail::IoExecutionState> _execution;
};

inline void swap(IoRegistration& lhs, IoRegistration& rhs) noexcept {
    lhs.swap(rhs);
}

}
}
