#include "EventLoopEngine.h"

#include "InternalLog.h"

#include <cstdint>
#include <cerrno>
#include <cstring>
#include <utility>

#include <sys/eventfd.h>
#include <unistd.h>

namespace GST {
namespace BASE {
namespace {

constexpr PollKey WAKEUP_POLL_KEY = 0;

}

void EventLoopEngine::EventCallbacks::assign(Event_type type, const IoCallback& cb) {
    if (has_event(type, Event_type::READ_EVENT)) {
        read = cb;
    }
    if (has_event(type, Event_type::WRITE_EVENT)) {
        write = cb;
    }
    if (has_event(type, Event_type::ERROR_EVENT)) {
        error = cb;
    }
}

std::vector<std::pair<Event_type, EventLoopEngine::IoCallback>>
EventLoopEngine::EventCallbacks::select(Event_type ready) const {
    std::vector<std::pair<Event_type, IoCallback>> selected;
    selected.reserve(3);

    if (has_event(ready, Event_type::READ_EVENT) && read) {
        selected.emplace_back(Event_type::READ_EVENT, read);
    }
    if (has_event(ready, Event_type::WRITE_EVENT) && write) {
        selected.emplace_back(Event_type::WRITE_EVENT, write);
    }
    if (has_event(ready, Event_type::ERROR_EVENT) && error) {
        selected.emplace_back(Event_type::ERROR_EVENT, error);
    }

    return selected;
}

EventLoopEngine::~EventLoopEngine() {
    stop(false);
}

bool EventLoopEngine::init(CallbackPoster callback_poster) {
    if (!callback_poster) {
        GST_EVENTLOOP_LOG_ERROR("callback poster is empty");
        return false;
    }

    if (!_poller.init()) {
        return false;
    }

    int wakeup_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (wakeup_fd < 0) {
        GST_EVENTLOOP_LOG_ERROR("eventfd failed: %s", std::strerror(errno));
        return false;
    }
    _wakeup_handle.reset(wakeup_fd);

    if (!_poller.add(
            _wakeup_handle.native_fd(),
            WAKEUP_POLL_KEY,
            Event_type::READ_EVENT)) {
        GST_EVENTLOOP_LOG_ERROR("register wakeup fd=%d failed", _wakeup_handle.native_fd());
        return false;
    }

    _callback_poster = std::move(callback_poster);
    _accepting_tasks = true;
    _dispatch_enabled = false;
    _state = EngineState::Ready;
    return true;
}

bool EventLoopEngine::run() {
    std::unique_lock<std::mutex> lifecycle_lock(_lifecycle_mutex);
    if (_state == EngineState::Running) {
        // 正在运行
        return true;
    }
    if (_state != EngineState::Ready || _runner.joinable()) {
        return false;
    }

    _state = EngineState::Starting;
    _is_running = true;

    std::promise<bool> startup_completion;
    std::future<bool> startup_result = startup_completion.get_future();
    try {
        _runner = std::thread(
            &EventLoopEngine::loop_thread_main,
            this,
            std::move(startup_completion));
    } catch (...) {
        _is_running = false;
        _state = EngineState::Ready;
        return false;
    }

    const bool started = startup_result.get();
    if (!started) {
        if (_runner.joinable()) {
            _runner.join();
        }
        _accepting_tasks = false;
        _state = EngineState::Stopped;
        return false;
    }

    _state = EngineState::Prepared;
    return true;
}

void EventLoopEngine::enable_dispatch() {
    {
        std::lock_guard<std::mutex> lifecycle_lock(_lifecycle_mutex);
        if (_state != EngineState::Prepared) {
            return;
        }

        _dispatch_enabled = true;
        _state = EngineState::Running;
    }
    _dispatch_cv.notify_one();
}

void EventLoopEngine::stop(bool drain_pending_io) {
    std::unique_lock<std::mutex> lifecycle_lock(_lifecycle_mutex);
    if (_state == EngineState::Uninitialized || _state == EngineState::Stopped) {
        return;
    }

    _state = EngineState::Stopping;
    _accepting_tasks = false;

    if (!drain_pending_io) {
        clear_io_state_tasks();
    }

    if (_is_running.exchange(false)) {
        wakeup();
    }
    _dispatch_cv.notify_all();

    lifecycle_lock.unlock();
    if (_runner.joinable()) {
        _runner.join();
    }

    clear_io_state_tasks();
    _registrations.clear();

    lifecycle_lock.lock();
    _state = EngineState::Stopped;
}

bool EventLoopEngine::register_event(
    IoToken token,
    int native_fd,
    Event_type interest,
    IoCallback cb
) {
    if (!token.valid() || native_fd < 0 || interest == Event_type::NONE || !cb) {
        return false;
    }

    // 交给统一的io任务组
    return submit_io_state_task([this, token, native_fd, interest, cb = std::move(cb)]() mutable {
        return register_event_in_owner(token, native_fd, interest, std::move(cb));
    });
}

bool EventLoopEngine::set_callback(std::uint64_t handle_id, Event_type type, IoCallback cb) {
    if (handle_id == 0 || type == Event_type::NONE || !cb) {
        return false;
    }

    return submit_io_state_task([this, handle_id, type, cb = std::move(cb)]() mutable {
        return set_callback_in_owner(handle_id, type, std::move(cb));
    });
}

bool EventLoopEngine::update_event(std::uint64_t handle_id, Event_type interest) {
    if (handle_id == 0 || interest == Event_type::NONE) {
        return false;
    }

    return submit_io_state_task([this, handle_id, interest] {
        return update_event_in_owner(handle_id, interest);
    });
}

bool EventLoopEngine::remove_event(std::uint64_t handle_id) {
    if (handle_id == 0) {
        return false;
    }

    return submit_io_state_task([this, handle_id] {
        return remove_event_in_owner(handle_id);
    });
}

void EventLoopEngine::loop_thread_main(std::promise<bool> startup_completion) {
    const bool startup_succeeded = execute_io_state_tasks();
    if (!startup_succeeded) {
        clear_registrations_in_owner();
        _is_running = false;
        startup_completion.set_value(false);
        return;
    }

    startup_completion.set_value(true);

    {
        std::unique_lock<std::mutex> lifecycle_lock(_lifecycle_mutex);
        _dispatch_cv.wait(lifecycle_lock, [this] {
            return _dispatch_enabled || !_is_running;
        });
        if (!_is_running) {
            return;
        }
    }

    while (_is_running) {
        std::vector<ReadyEvent> events = _poller.wait();
        dispatch_events(events);
    }

    execute_io_state_tasks();
}

void EventLoopEngine::dispatch_events(const std::vector<ReadyEvent>& events) {
    std::vector<Task> tasks;
    std::vector<std::uint64_t> handle_ids;
    tasks.reserve(events.size());
    handle_ids.reserve(events.size());

    auto flush_tasks = [this, &tasks, &handle_ids] {
        if (tasks.empty()) {
            return;
        }

        if (!_callback_poster(std::move(tasks))) {
            for (const std::uint64_t handle_id : handle_ids) {
                recover_callback_post_failure(handle_id);
            }
        }
        tasks.clear();
        handle_ids.clear();
    };

    for (const ReadyEvent& event : events) {
        if (!event.valid()) {
            continue;
        }

        if (event.key == WAKEUP_POLL_KEY) {
            flush_tasks();
            drain_wakeup();
            execute_io_state_tasks();
            continue;
        }

        if (!_is_running) {
            continue;
        }

        Task task;
        std::uint64_t handle_id = 0;
        if (prepare_callback_task(event, task, handle_id)) {
            tasks.push_back(std::move(task));
            handle_ids.push_back(handle_id);
        }
    }

    flush_tasks();
}

// 选出callback后返回
bool EventLoopEngine::prepare_callback_task(
    const ReadyEvent& event,
    Task& task,
    std::uint64_t& handle_id
) {
    auto it = _registrations.find(event.key);
    if (it == _registrations.end() || it->second.callback_running) {
        return false;
    }

    const Event_type subscribed_ready = event.type & it->second.interest;
    auto callbacks = it->second.callbacks.select(subscribed_ready);

    //  rearm one-shot
    if (callbacks.empty()) {
        if (!_poller.update(it->second.native_fd, event.key, it->second.interest, true)) {
            GST_EVENTLOOP_LOG_ERROR(
                "rearm ignored ready event failed handle_id=%llu native_fd=%d",
                static_cast<unsigned long long>(event.key),
                it->second.native_fd);
        }
        return false;
    }

    it->second.callback_running = true;
    handle_id = event.key;
    // 提交给线程池的任务
    const IoToken token = it->second.token;
    task = [this, callbacks = std::move(callbacks), handle_id, token] {
        for (const auto& [event_type, cb] : callbacks) {
            try {
                cb(IoContext{event_type, token});
            } catch (...) {
                GST_EVENTLOOP_LOG_ERROR(
                    "io callback threw handle_id=%llu",
                    static_cast<unsigned long long>(handle_id));
            }
        }
        
        // 提交到io状态修改的任务队列
        post_internal_io_state_task([this, handle_id] {
            on_callback_finished(handle_id);
            return true;
        });
    };
    return true;
}

// 投递失败的任务修改后续状态
void EventLoopEngine::recover_callback_post_failure(std::uint64_t handle_id) {
    auto it = _registrations.find(handle_id);
    if (it == _registrations.end()) {
        return;
    }

    it->second.callback_running = false;
    if (!_poller.update(it->second.native_fd, handle_id, it->second.interest, true)) {
        GST_EVENTLOOP_LOG_ERROR(
            "rearm after callback post failure failed handle_id=%llu native_fd=%d",
            static_cast<unsigned long long>(handle_id),
            it->second.native_fd);
    }
}

bool EventLoopEngine::execute_io_state_tasks() {
    std::queue<IoStateTask> tasks;
    {
        std::lock_guard<std::mutex> lock(_io_state_mutex);
        tasks.swap(_io_state_tasks);
        // A producer that enqueues after this point must publish a new wakeup.
        _io_wakeup_pending = false;
    }

    bool all_succeeded = true;
    while (!tasks.empty()) {
        IoStateTask task = std::move(tasks.front());
        tasks.pop();

        bool succeeded = false;
        try {
            succeeded = task.operation && task.operation();
        } catch (...) {
            GST_EVENTLOOP_LOG_ERROR("IO state operation threw");
        }

        all_succeeded = all_succeeded && succeeded;
        if (task.completion) {
            task.completion->set_value(succeeded);
        }
    }

    return all_succeeded;
}

// 所有的io操作都提交到队列进行操作
// io监听的type修改、io_callback的设置
bool EventLoopEngine::submit_io_state_task(IoStateOperation operation) {
    if (!operation) {
        return false;
    }

    std::future<bool> result;
    {
        std::lock_guard<std::mutex> lifecycle_lock(_lifecycle_mutex);
        if (!_accepting_tasks) {
            return false;
        }

        // run()之前调用注册io事件只会先存到队列里
        // run()之前的注册代表是否入队成功
        if (_state == EngineState::Ready) {
            return enqueue_io_state_task(IoStateTask{std::move(operation), nullptr});
        }
        if (_state != EngineState::Running) {
            return false;
        }


        // run之后回调拿取结果
        auto completion = std::make_shared<std::promise<bool>>();
        result = completion->get_future();
        if (!enqueue_io_state_task(IoStateTask{std::move(operation), std::move(completion)})) {
            return false;
        }
    }

    return result.get();
}

bool EventLoopEngine::post_internal_io_state_task(IoStateOperation operation) {
    if (!operation || !_accepting_tasks) {
        return false;
    }

    return enqueue_io_state_task(IoStateTask{std::move(operation), nullptr});
}

// push task
bool EventLoopEngine::enqueue_io_state_task(IoStateTask task) {
    if (!task.operation) {
        return false;
    }

    bool should_wakeup = false;
    {
        std::lock_guard<std::mutex> lock(_io_state_mutex);
        if (!_accepting_tasks) {
            return false;
        }
        _io_state_tasks.push(std::move(task));
        // One eventfd write is sufficient until the owner takes this batch.
        if (!_io_wakeup_pending) {
            _io_wakeup_pending = true;
            should_wakeup = true;
        }
    }

    if (should_wakeup) {
        wakeup();
    }
    return true;
}

void EventLoopEngine::clear_io_state_tasks() {
    std::queue<IoStateTask> tasks;
    {
        std::lock_guard<std::mutex> lock(_io_state_mutex);
        tasks.swap(_io_state_tasks);
        _io_wakeup_pending = false;
    }

    while (!tasks.empty()) {
        IoStateTask task = std::move(tasks.front());
        tasks.pop();
        if (task.completion) {
            task.completion->set_value(false);
        }
    }
}

void EventLoopEngine::clear_registrations_in_owner() {
    for (const auto& [handle_id, registration] : _registrations) {
        (void)handle_id;
        _poller.remove(registration.native_fd);
    }
    _registrations.clear();
}

void EventLoopEngine::wakeup() {
    if (!_wakeup_handle.valid()) {
        return;
    }

    uint64_t value = 1;
    ssize_t ret = ::write(_wakeup_handle.native_fd(), &value, sizeof(value));
    if (ret < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        GST_EVENTLOOP_LOG_ERROR("eventfd wakeup write failed: %s", std::strerror(errno));
    }
}

void EventLoopEngine::drain_wakeup() {
    uint64_t value = 0;
    while (::read(_wakeup_handle.native_fd(), &value, sizeof(value)) == sizeof(value)) {
    }
}

bool EventLoopEngine::register_event_in_owner(
    IoToken token,
    int native_fd,
    Event_type interest,
    IoCallback cb
) {
    const std::uint64_t handle_id = token.registration_id();
    if (_registrations.find(handle_id) != _registrations.end()) {
        GST_EVENTLOOP_LOG_ERROR(
            "IO operation failed operation=watch handle_id=%llu native_fd=%d: duplicate handle_id",
            static_cast<unsigned long long>(handle_id),
            native_fd);
        return false;
    }

    EventRegistration registration;
    registration.native_fd = native_fd;
    registration.interest = interest;
    registration.token = token;
    registration.callbacks.assign(interest, cb);

    _registrations.emplace(handle_id, std::move(registration));
    if (!_poller.add(native_fd, handle_id, interest, true)) {
        _registrations.erase(handle_id);
        GST_EVENTLOOP_LOG_ERROR(
            "IO operation failed operation=watch handle_id=%llu native_fd=%d",
            static_cast<unsigned long long>(handle_id),
            native_fd);
        return false;
    }

    return true;
}

bool EventLoopEngine::set_callback_in_owner(
    std::uint64_t handle_id,
    Event_type type,
    IoCallback cb
) {
    auto it = _registrations.find(handle_id);
    if (it == _registrations.end()) {
        GST_EVENTLOOP_LOG_ERROR(
            "IO operation failed operation=set-callback handle_id=%llu: registration not found",
            static_cast<unsigned long long>(handle_id));
        return false;
    }

    it->second.callbacks.assign(type, cb);
    return true;
}

bool EventLoopEngine::update_event_in_owner(std::uint64_t handle_id, Event_type interest) {
    auto it = _registrations.find(handle_id);
    if (it == _registrations.end()) {
        GST_EVENTLOOP_LOG_ERROR(
            "IO operation failed operation=update handle_id=%llu: registration not found",
            static_cast<unsigned long long>(handle_id));
        return false;
    }

    if (it->second.callback_running) {
        it->second.interest = interest;
        return true;
    }

    if (!_poller.update(it->second.native_fd, handle_id, interest, true)) {
        GST_EVENTLOOP_LOG_ERROR(
            "IO operation failed operation=update handle_id=%llu native_fd=%d",
            static_cast<unsigned long long>(handle_id),
            it->second.native_fd);
        return false;
    }

    it->second.interest = interest;
    return true;
}

bool EventLoopEngine::remove_event_in_owner(std::uint64_t handle_id) {
    auto it = _registrations.find(handle_id);
    if (it == _registrations.end()) {
        GST_EVENTLOOP_LOG_ERROR(
            "IO operation failed operation=remove handle_id=%llu: registration not found",
            static_cast<unsigned long long>(handle_id));
        return false;
    }

    const int native_fd = it->second.native_fd;
    if (!_poller.remove(native_fd)) {
        GST_EVENTLOOP_LOG_ERROR(
            "IO operation failed operation=remove handle_id=%llu native_fd=%d",
            static_cast<unsigned long long>(handle_id),
            native_fd);
        return false;
    }

    _registrations.erase(it);
    return true;
}

void EventLoopEngine::on_callback_finished(std::uint64_t handle_id) {
    auto it = _registrations.find(handle_id);
    if (it == _registrations.end()) {
        return;
    }

    it->second.callback_running = false;
    if (!_poller.update(it->second.native_fd, handle_id, it->second.interest, true)) {
        GST_EVENTLOOP_LOG_ERROR(
            "rearm after callback failed handle_id=%llu native_fd=%d",
            static_cast<unsigned long long>(handle_id),
            it->second.native_fd);
    }
}

}
}
