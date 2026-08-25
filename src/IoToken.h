#pragma once

#include <cstddef>
#include <cstdint>

namespace GST {
namespace BASE {

class EventLoop;

// A copyable, non-owning identity for one EventLoop IO registration.
// It neither owns the native resource nor removes the registration.
class IoToken {
public:
    IoToken() noexcept = default;

    bool valid() const noexcept {
        return _owner != nullptr && _generation != 0 && _handle_id != 0;
    }

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

    IoToken(
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

    EventLoop*    _owner{nullptr};
    std::uint64_t _generation{0};
    std::size_t   _engine_id{0};
    std::uint64_t _handle_id{0};
};

}
}
