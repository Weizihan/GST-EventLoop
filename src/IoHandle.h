#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>

namespace GST {
namespace BASE {

class EventLoop;

class IoHandle {
public:
    IoHandle() noexcept = default;

    IoHandle(const IoHandle&) = delete;
    IoHandle& operator=(const IoHandle&) = delete;

    IoHandle(IoHandle&& other) noexcept
        : _owner(std::exchange(other._owner, nullptr)),
          _generation(std::exchange(other._generation, 0)),
          _engine_id(std::exchange(other._engine_id, 0)),
          _handle_id(std::exchange(other._handle_id, 0)) {
    }

    IoHandle& operator=(IoHandle&&) noexcept = delete;

    void swap(IoHandle& other) noexcept {
        using std::swap;
        swap(_owner, other._owner);
        swap(_generation, other._generation);
        swap(_engine_id, other._engine_id);
        swap(_handle_id, other._handle_id);
    }

    bool valid() const noexcept {
        return _owner != nullptr && _generation != 0 && _handle_id != 0;
    }

    explicit operator bool() const noexcept {
        return valid();
    }

private:
    friend class EventLoop;

    IoHandle(
        EventLoop* owner,
        std::uint64_t generation,
        std::size_t engine_id,
        std::uint64_t handle_id
    ) noexcept
        : _owner(owner),
          _generation(generation),
          _engine_id(engine_id),
          _handle_id(handle_id) {
    }

    void invalidate() noexcept {
        _owner = nullptr;
        _generation = 0;
        _engine_id = 0;
        _handle_id = 0;
    }

    EventLoop*    _owner{nullptr};
    std::uint64_t _generation{0};
    std::size_t   _engine_id{0};
    std::uint64_t _handle_id{0};
};

inline void swap(IoHandle& lhs, IoHandle& rhs) noexcept {
    lhs.swap(rhs);
}

}
}
