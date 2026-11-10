#pragma once

#include "EventTypes.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace GST {
namespace BASE {

class EventLoop;
class EventLoopEngine;
struct IoContext;
namespace detail {
struct IoExecutionState;
struct IoRegistrationState;
}

// 可复制、非拥有的 IO 注册凭证；所有副本指向同一次注册。
// 不拥有原生资源，析构时不取消注册，也不延后完成通知。
class IoRegistration {
public:
    IoRegistration() noexcept;
    IoRegistration(const IoRegistration&) noexcept;
    IoRegistration& operator=(const IoRegistration&) noexcept;

    IoRegistration(IoRegistration&& other) noexcept;
    IoRegistration& operator=(IoRegistration&& other) noexcept;

    void swap(IoRegistration& other) noexcept;

    bool valid() const noexcept;

    // 与 Loop 上的对应操作共用实现；失效凭证不会访问已经销毁的 Loop。
    bool set_cb(Event_type type, std::function<void(const IoContext&)> cb) const;
    bool update(Event_type interest) const;
    bool cancel() const;

    explicit operator bool() const noexcept;

    std::uint64_t generation() const noexcept;

    std::uint64_t registration_id() const noexcept;

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
    ) noexcept;

    EventLoop*    _owner{nullptr};
    std::uint64_t _generation{0};
    std::size_t   _engine_id{0};
    std::uint64_t _handle_id{0};
    std::weak_ptr<detail::IoRegistrationState> _registration;
    std::weak_ptr<detail::IoExecutionState> _execution;
};

void swap(IoRegistration& lhs, IoRegistration& rhs) noexcept;

}
}
