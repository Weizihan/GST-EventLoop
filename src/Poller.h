/* 当前仅实现 Linux 后端。 */

#pragma once

#include "EventTypes.h"
#include "NativeHandle.h"

#include <atomic>
#include <cstdint>
#include <limits>
#include <vector>

namespace GST {
namespace BASE {

using PollKey = std::uint64_t;
inline constexpr PollKey INVALID_POLL_KEY = std::numeric_limits<PollKey>::max();

struct ReadyEvent {
    PollKey    key{INVALID_POLL_KEY};
    Event_type type{Event_type::NONE};

    bool valid() const noexcept {
        return key != INVALID_POLL_KEY && type != Event_type::NONE;
    }
};


class Poller {
public:
    Poller() = default;
    ~Poller();

    bool init();
    bool close() noexcept;
    bool add(int native_fd, PollKey key, Event_type interest, bool one_shot = false);
    bool update(int native_fd, PollKey key, Event_type interest, bool one_shot = false);
    bool remove(int native_fd);
    std::vector<ReadyEvent> wait();

private:
    int _epoll_fd{NativeHandle::INVALID_HANDLE};
    std::atomic_bool _is_running{false};
};

}
}
