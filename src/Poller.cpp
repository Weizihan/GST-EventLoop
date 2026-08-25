#include "Poller.h"

#include "InternalLog.h"

#include <cerrno>
#include <cstring>

#include <sys/epoll.h>
#include <unistd.h>

namespace GST {
namespace BASE {
namespace {

uint32_t to_epoll_events(Event_type interest, bool one_shot) noexcept {
    uint32_t events = 0;
    if (has_event(interest, Event_type::READ_EVENT)) {
        events |= EPOLLIN;
    }
    if (has_event(interest, Event_type::WRITE_EVENT)) {
        events |= EPOLLOUT;
    }
    if (one_shot) {
        events |= EPOLLONESHOT;
    }
    return events;
}

}

Poller::~Poller() {
    if (_epoll_fd >= 0) {
        ::close(_epoll_fd);
    }
}

bool Poller::init() {
    _epoll_fd = epoll_create(1024);
    if (_epoll_fd < 0) {
        GST_EVENTLOOP_LOG_ERROR("epoll_create failed: %s", std::strerror(errno));
        return false;
    }
    return _is_running = true;
}

bool Poller::add(int native_fd, PollKey key, Event_type interest, bool one_shot) {
    if (_epoll_fd < 0 || native_fd < 0) {
        return false;
    }

    uint32_t epoll_events = to_epoll_events(interest, one_shot);
    if (epoll_events == 0) {
        return false;
    }

    epoll_event event{};
    event.events = epoll_events;
    event.data.u64 = key;

    if (epoll_ctl(_epoll_fd, EPOLL_CTL_ADD, native_fd, &event) != 0) {
        GST_EVENTLOOP_LOG_ERROR("epoll_ctl add fd=%d failed: %s", native_fd, std::strerror(errno));
        return false;
    }

    return true;
}

bool Poller::update(int native_fd, PollKey key, Event_type interest, bool one_shot) {
    if (_epoll_fd < 0 || native_fd < 0) {
        return false;
    }

    uint32_t epoll_events = to_epoll_events(interest, one_shot);
    if (epoll_events == 0) {
        return false;
    }

    epoll_event event{};
    event.events = epoll_events;
    event.data.u64 = key;

    if (epoll_ctl(_epoll_fd, EPOLL_CTL_MOD, native_fd, &event) != 0) {
        GST_EVENTLOOP_LOG_ERROR("epoll_ctl mod fd=%d failed: %s", native_fd, std::strerror(errno));
        return false;
    }

    return true;
}

bool Poller::remove(int native_fd) {
    if (_epoll_fd < 0 || native_fd < 0) {
        return false;
    }

    if (epoll_ctl(_epoll_fd, EPOLL_CTL_DEL, native_fd, nullptr) != 0) {
        GST_EVENTLOOP_LOG_ERROR("epoll_ctl del fd=%d failed: %s", native_fd, std::strerror(errno));
        return false;
    }

    return true;
}

std::vector<ReadyEvent> Poller::wait() {
    std::vector<epoll_event> events(100);
    while (_is_running) {
        int ret = epoll_wait(_epoll_fd, events.data(), static_cast<int>(events.size()), -1);
        if (ret < 0) {
            if (errno == EINTR) {
                continue;
            }
            GST_EVENTLOOP_LOG_ERROR("epoll_wait failed: %s", std::strerror(errno));
            return {};
        }

        if (ret == 0) {
            continue;
        }

        std::vector<ReadyEvent> ready_events;
        ready_events.reserve(static_cast<size_t>(ret));

        for (int i = 0; i < ret; ++i) {
            const epoll_event& event = events[i];
            Event_type type = Event_type::NONE;

            if (event.events & EPOLLIN) {
                type |= Event_type::READ_EVENT;
            }

            if (event.events & EPOLLOUT) {
                type |= Event_type::WRITE_EVENT;
            }

            if (event.events & (EPOLLERR | EPOLLHUP | EPOLLRDHUP)) {
                type |= Event_type::ERROR_EVENT;
            }

            ready_events.push_back(
                ReadyEvent{
                    event.data.u64,
                    type
                }
            );
        }

        return ready_events;
    }

    return {};
}

}
}
