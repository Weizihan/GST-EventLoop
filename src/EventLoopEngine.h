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
#include "IoRegistrationState.h"
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
    void stop(bool drain_pending_io = true);

    static bool is_owner_thread() noexcept;
    bool can_submit_from_current_thread() const noexcept;

    bool register_event(
        IoRegistration identity,
        int native_fd,
        Event_type interest,
        IoCallback cb
    );
    bool set_callback(
        std::uint64_t handle_id,
        Event_type type,
        IoCallback cb,
        std::shared_ptr<detail::IoRegistrationState> registration = {}
    );
    bool update_event(std::uint64_t handle_id, Event_type interest);
    bool remove_event(std::uint64_t handle_id);
    
private:
#ifdef GST_EVENTLOOP_TESTING
    friend struct EventLoopTestAccess;
#endif
    enum class EngineState {
        Uninitialized,
        Ready,
        Starting,
        Prepared,
        Running,
        Stopping,
        Stopped
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

    struct CallbackPayload {
        std::shared_ptr<detail::IoRegistrationState> registration;
        IoCallback callback;
    };

    struct EventRegistration {
        int           native_fd{NativeHandle::INVALID_HANDLE};
        Event_type    interest{Event_type::NONE};
        IoRegistration identity;
        std::shared_ptr<detail::IoRegistrationState> registration;
        EventCallbacks callbacks;
        bool          callback_running{false};
    };

    struct CallbackBatch {
        EventLoopEngine* owner{nullptr};
        std::shared_ptr<detail::IoRegistrationState> registration;
        std::vector<std::pair<Event_type, IoCallback>> callbacks;
        ~CallbackBatch();
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
    void consume_callback_completions();
    bool submit_io_state_task(IoStateOperation operation);
    bool enqueue_io_state_task(IoStateTask task);
    void wakeup();
    void drain_wakeup();
    void clear_io_state_tasks();
    void clear_registrations_in_owner();

    bool register_event_in_owner(
        IoRegistration identity,
        int native_fd,
        Event_type interest,
        IoCallback cb
    );
    bool set_callback_in_owner(std::uint64_t handle_id, Event_type type, IoCallback cb);
    bool update_event_in_owner(std::uint64_t handle_id, Event_type interest);
    bool remove_event_in_owner(std::uint64_t handle_id);
    void on_callback_finished(std::uint64_t handle_id);

    std::atomic_bool _is_running{false};
    std::atomic_bool _accepting_tasks{false};
    std::thread      _runner;
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
#ifdef GST_EVENTLOOP_TESTING
    std::function<void()>            _before_callback_begin;
    std::function<void()>            _after_callback_begin;
    std::function<void()>            _after_callback_batch_published;
#endif
};

}
}
