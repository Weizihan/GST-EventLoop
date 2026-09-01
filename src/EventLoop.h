#pragma once

#include "EventTypes.h"
#include "IoHandle.h"
#include "IoToken.h"
#include "NativeHandle.h"
#include "ThreadPool.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace GST {
namespace BASE {

class EventLoopEngine;

enum class CallbackConcurrency {
    SerialPerHandle,
    ParallelReadWrite, // TODO: support read/write callback concurrency for one handle.
    FullyParallel      // TODO: support fully concurrent callbacks for one handle.
};

struct EventLoopConfig {
    std::size_t engine_num{1};
    std::size_t callback_thread_num{1};
    CallbackConcurrency callback_concurrency{CallbackConcurrency::SerialPerHandle};
};

struct IoContext {
    Event_type event_type{Event_type::NONE};
    IoToken token;
};

enum class ShutdownMode {
    Soft,
    Hard
};

class EventLoop {
public:
    using IoCallback = std::function<void(const IoContext&)>;

    EventLoop();
    ~EventLoop();

    EventLoop(const EventLoop&) = delete;
    EventLoop& operator=(const EventLoop&) = delete;

    EventLoop(EventLoop&&) noexcept = delete;
    EventLoop& operator=(EventLoop&&) noexcept = delete;

    bool init();
    bool init(const EventLoopConfig& config);
    bool run();
    void stop(ShutdownMode mode = ShutdownMode::Soft);

    IoHandle watch_io(const NativeHandle& native_handle, Event_type interest, IoCallback cb);
    bool set_io_callback(const IoHandle& handle, Event_type type, IoCallback cb);
    bool set_io_callback(const IoToken& token, Event_type type, IoCallback cb);
    bool update_io(const IoHandle& handle, Event_type interest);
    bool update_io(const IoToken& token, Event_type interest);
    bool remove(IoHandle& handle);
    bool remove(const IoToken& token);

private:
    enum class LifecycleState {
        Uninitialized,
        Initializing,
        Ready,
        Starting,
        Running,
        Stopping,
        Stopped
    };

    struct FdRegistration {
        std::size_t engine_id{0};
        std::size_t reservation_count{0};
    };

    bool accepts_io_locked() const noexcept;
    std::shared_ptr<EventLoopEngine> reserve_engine_for_registration(
        int native_fd,
        std::uint64_t& generation,
        std::size_t& engine_id,
        std::uint64_t& handle_id
    );
    void release_registration(std::uint64_t generation, std::uint64_t handle_id);
    void clear_registrations_locked();
    std::shared_ptr<EventLoopEngine> engine_for(const IoHandle& handle);
    std::shared_ptr<EventLoopEngine> engine_for(const IoToken& token);

    std::mutex _lifecycle_operation_mutex;
    std::mutex _state_mutex;
    LifecycleState _state{LifecycleState::Uninitialized};
    EventLoopConfig _config;
    ThreadPool _callback_pool;
    std::vector<std::shared_ptr<EventLoopEngine>> _engines;
    std::atomic<std::uint64_t> _next_handle_id{1};
    std::atomic<std::size_t> _next_engine_id{0};
    std::unordered_map<int, FdRegistration> _registered_fds;
    std::unordered_map<std::uint64_t, int> _registration_fds;
    std::uint64_t _generation{0};
};

}
}
