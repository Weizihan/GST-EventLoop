#pragma once

#include "EventTypes.h"
#include "IoRegistration.h"
#include "NativeHandle.h"
#include "ThreadPool.h"

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace GST {
namespace BASE {

class EventLoopEngine;
namespace detail { struct IoExecutionState; }

enum class CallbackConcurrency {
    SerialPerHandle,
    ParallelReadWrite, // TODO：支持同一句柄的读写回调并发。
    FullyParallel      // TODO：支持同一句柄的所有回调并发。
};

struct EventLoopConfig {
    std::size_t engine_num{1};
    std::size_t callback_thread_num{1};
    CallbackConcurrency callback_concurrency{CallbackConcurrency::SerialPerHandle};
};

struct IoContext {
    Event_type event_type{Event_type::NONE};
    IoRegistration self;
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

    // 准备资源，不启动线程；进程内同时只允许一个活跃 Loop。
    bool init();
    bool init(const EventLoopConfig& config);
    // 启动共享线程池和所有 Engine；全部启动成功后才允许派发回调。
    bool start();
    // 阻塞等待停止请求并完成收尾；必须先 start，或已对 Ready 状态请求停止。
    // 自身回调、完成通知和管理操作的析构重入中不得调用。
    bool run();
    // 仅提出停止请求，允许回调调用；Hard 在返回前关闭共同资格门禁。
    // run() 返回或正常析构完成，才表示整体收尾结束。
    void stop(ShutdownMode mode = ShutdownMode::Soft);

    IoRegistration watch_io(const NativeHandle& native_handle, Event_type interest, IoCallback cb);
    // 可选完成通知。借用资源和回调依赖须保持有效，直到 on_done；或者先等待
    // 所有发起 IO 操作的应用线程结束，再等待 run() 返回后释放。
    // 监听解绑且本注册相关的回调副本析构后，才会执行通知。
    // 执行通知时不持有内部锁，也不保证由固定线程执行。
    IoRegistration watch_io(
        const NativeHandle& native_handle,
        Event_type interest,
        IoCallback cb,
        std::function<void()> on_done
    );
    bool set_io_callback(const IoRegistration& registration, Event_type type, IoCallback cb);
    bool update_io(const IoRegistration& registration, Event_type interest);
    // 成功后所有凭证副本立即失效；不等待已经取得资格的回调结束。
    bool remove(const IoRegistration& registration);

private:
    friend class IoRegistration;
#ifdef GST_EVENTLOOP_TESTING
    friend struct EventLoopTestAccess;
#endif
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
    bool in_managed_execution() const noexcept;
    void finish_shutdown();
    void release_process_slot();
    std::shared_ptr<detail::IoExecutionState> execution_for_access();
    IoRegistration watch_io_impl(const NativeHandle& native_handle, Event_type interest,
                                 IoCallback cb, std::function<void()> on_done);
    bool set_io_callback_impl(const IoRegistration& registration, Event_type type, IoCallback cb);
    bool update_io_impl(const IoRegistration& registration, Event_type interest);
    bool remove_impl(const IoRegistration& registration);
    std::shared_ptr<EventLoopEngine> reserve_engine_for_registration(
        int native_fd,
        std::uint64_t& generation,
        std::size_t& engine_id,
        std::uint64_t& handle_id,
        std::shared_ptr<detail::IoExecutionState>& execution_state
    );
    void release_registration(std::uint64_t generation, std::uint64_t handle_id);
    void clear_registrations_locked();
    std::shared_ptr<EventLoopEngine> engine_for(const IoRegistration& registration);

    std::mutex _lifecycle_operation_mutex;
    std::mutex _state_mutex;
    std::condition_variable _stop_cv;
    LifecycleState _state{LifecycleState::Uninitialized};
    bool _stop_requested{false};
    ShutdownMode _stop_mode{ShutdownMode::Soft};
    bool _run_active{false};
    bool _destroying{false};
    bool _owns_process_slot{false};
    EventLoopConfig _config;
    ThreadPool _callback_pool;
    std::vector<std::shared_ptr<EventLoopEngine>> _engines;
    std::atomic<std::uint64_t> _next_handle_id{1};
    std::atomic<std::size_t> _next_engine_id{0};
    std::unordered_map<int, FdRegistration> _registered_fds;
    std::unordered_map<std::uint64_t, int> _registration_fds;
    std::uint64_t _generation{0};
    std::shared_ptr<detail::IoExecutionState> _execution_state;
};

}
}
