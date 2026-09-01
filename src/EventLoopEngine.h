#pragma once
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <future>
#include <mutex>
#include <queue>
#include <thread>
#include <unordered_map>
#include <vector>

#include "EventLoop.h"
#include "Poller.h"

namespace GST {
namespace BASE {

class EventLoopEngine {
public:
    using IoCallback = EventLoop::IoCallback;
    using Task = std::function<void()>;
    using CallbackPoster = std::function<bool(std::vector<Task>)>;

    ~EventLoopEngine();
    bool init(CallbackPoster callback_poster);
    bool run();
    void enable_dispatch();
    void stop(bool need_clean_io_tasks = false);

    bool register_event(
        IoToken token,
        int native_fd,
        Event_type interest,
        IoCallback cb
    );
    bool set_callback(std::uint64_t handle_id, Event_type type, IoCallback cb);
    bool update_event(std::uint64_t handle_id, Event_type interest);
    bool remove_event(std::uint64_t handle_id);
    
private:
    // ready和Running保证的是run之前提交任务的情况
    enum class EngineState {
        Uninitialized,          // 未初始化
        Ready,                  // 初始化完成
        Starting,               // 开始执行以后
        Prepared,               // run执行完成
        Running,                // io线程正式启动
        Stopping,               // 停止中(可能会执行任务)
        Stopped                 // 已完成停止
    };

    using IoStateOperation = std::function<bool()>;

    struct IoStateTask {
        IoStateOperation operation;
        std::shared_ptr<std::promise<bool>> completion;
    };

    struct EventCallbacks {
        IoCallback read;
        IoCallback write;
        IoCallback error;

        void assign(Event_type type, const IoCallback& cb);
        std::vector<std::pair<Event_type, IoCallback>> select(Event_type ready) const;
    };

    struct EventRegistration {
        int           native_fd{NativeHandle::INVALID_HANDLE};
        Event_type    interest{Event_type::NONE};
        IoToken       token;
        EventCallbacks callbacks;
        bool          callback_running{false};
    };

    void loop_thread_main(std::promise<bool> startup_completion);
    void dispatch_events(const std::vector<ReadyEvent>& events);
    bool prepare_callback_task(
        const ReadyEvent& event,
        Task& task,
        std::uint64_t& handle_id
    );
    void recover_callback_post_failure(std::uint64_t handle_id);
    bool execute_io_state_tasks();
    bool submit_io_state_task(IoStateOperation operation);
    bool post_internal_io_state_task(IoStateOperation operation);
    bool enqueue_io_state_task(IoStateTask task);
    void wakeup();
    void drain_wakeup();
    void clear_io_state_tasks();
    void clear_registrations_in_owner();

    bool register_event_in_owner(
        IoToken token,
        int native_fd,
        Event_type interest,
        IoCallback cb
    );
    bool set_callback_in_owner(std::uint64_t handle_id, Event_type type, IoCallback cb);
    bool update_event_in_owner(std::uint64_t handle_id, Event_type interest);
    bool remove_event_in_owner(std::uint64_t handle_id);
    void on_callback_finished(std::uint64_t handle_id);

    std::atomic_bool _is_running{false};                    // 是否在运行
    std::atomic_bool _accepting_tasks{false};               // 是否可接受任务
    std::thread      _runner;                               // 主循环线程
    std::mutex       _lifecycle_mutex;
    std::condition_variable _dispatch_cv;
    bool             _dispatch_enabled{false};
    EngineState      _state{EngineState::Uninitialized};

    Poller                          _poller;
    NativeHandle                    _wakeup_handle;
    std::mutex                      _io_state_mutex;
    std::queue<IoStateTask>         _io_state_tasks;
    bool                            _io_wakeup_pending{false};
    std::unordered_map<std::uint64_t, EventRegistration> _registrations;
    CallbackPoster                  _callback_poster;
};

}
}
