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
thread_local EventLoopEngine* current_owner = nullptr;

}

EventLoopEngine::CallbackBatch::~CallbackBatch() {
    // 最后一个任务副本先销毁回调及其捕获，再报告批次完成。
    callbacks.clear();
    if (registration && owner) {
        registration->callback_finished = true;
        owner->wakeup();
    }
}

bool EventLoopEngine::is_owner_thread() noexcept {
    return current_owner != nullptr;
}

bool EventLoopEngine::can_submit_from_current_thread() const noexcept {
    return current_owner == nullptr || current_owner == this;
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

    if (_is_running.exchange(false)) {
        wakeup();
    }
    _dispatch_cv.notify_all();

    lifecycle_lock.unlock();
    if (!drain_pending_io) {
        clear_io_state_tasks();
    }
    if (_runner.joinable()) {
        _runner.join();
    }

    clear_io_state_tasks();
    clear_registrations_in_owner();

    lifecycle_lock.lock();
    _state = EngineState::Stopped;
}

bool EventLoopEngine::register_event(
    IoRegistration identity,
    int native_fd,
    Event_type interest,
    IoCallback cb
) {
    if (!can_submit_from_current_thread() ||
        identity.registration_id() == 0 || native_fd < 0 || interest == Event_type::NONE || !cb) {
        return false;
    }

    auto registration = identity._registration.lock();
    if (!registration || !registration->registered) {
        return false;
    }

    auto payload = std::make_shared<CallbackPayload>();
    payload->registration = std::move(registration);
    payload->callback = std::move(cb);

    // 交给所属 Engine 的 IO 状态任务队列。
    return submit_io_state_task([this, identity, native_fd, interest, payload] {
        return register_event_in_owner(identity, native_fd, interest, std::move(payload->callback));
    });
}

bool EventLoopEngine::set_callback(
    std::uint64_t handle_id,
    Event_type type,
    IoCallback cb,
    std::shared_ptr<detail::IoRegistrationState> registration
) {
    if (!can_submit_from_current_thread() ||
        handle_id == 0 || type == Event_type::NONE || !cb) {
        return false;
    }

    auto payload = std::make_shared<CallbackPayload>();
    payload->registration = std::move(registration);
    payload->callback = std::move(cb);
    return submit_io_state_task([this, handle_id, type, payload] {
        if (!payload->registration) {
            auto it = _registrations.find(handle_id);
            if (it != _registrations.end()) {
                payload->registration = it->second.registration;
            }
        }
        return set_callback_in_owner(handle_id, type, std::move(payload->callback));
    });
}

bool EventLoopEngine::update_event(std::uint64_t handle_id, Event_type interest) {
    if (!can_submit_from_current_thread() ||
        handle_id == 0 || interest == Event_type::NONE) {
        return false;
    }

    return submit_io_state_task([this, handle_id, interest] {
        return update_event_in_owner(handle_id, interest);
    });
}

bool EventLoopEngine::remove_event(std::uint64_t handle_id) {
    if (!can_submit_from_current_thread() || handle_id == 0) {
        return false;
    }

    return submit_io_state_task([this, handle_id] {
        return remove_event_in_owner(handle_id);
    });
}

void EventLoopEngine::loop_thread_main(std::promise<bool> startup_completion) {
    current_owner = this;
    const bool startup_succeeded = execute_io_state_tasks();
    if (!startup_succeeded) {
        clear_registrations_in_owner();
        _is_running = false;
        startup_completion.set_value(false);
        current_owner = nullptr;
        return;
    }

    startup_completion.set_value(true);

    {
        std::unique_lock<std::mutex> lifecycle_lock(_lifecycle_mutex);
        _dispatch_cv.wait(lifecycle_lock, [this] {
            return _dispatch_enabled || !_is_running;
        });
        if (!_is_running) {
            current_owner = nullptr;
            return;
        }
    }

    while (_is_running) {
        std::vector<ReadyEvent> events = _poller.wait();
        execute_io_state_tasks();
        consume_callback_completions();
        dispatch_events(events);
    }

    execute_io_state_tasks();
    consume_callback_completions();
    current_owner = nullptr;
}

void EventLoopEngine::dispatch_events(const std::vector<ReadyEvent>& events) {
    std::vector<Task> tasks;
    std::vector<std::uint64_t> handle_ids;
    try {
        tasks.reserve(events.size());
        handle_ids.reserve(events.size());
    } catch (...) {
        for (const ReadyEvent& event : events) {
            auto it = _registrations.find(event.key);
            if (it != _registrations.end() && !it->second.callback_running) {
                recover_callback_post_failure(event.key);
            }
        }
        GST_EVENTLOOP_LOG_ERROR("callback batch allocation failed");
        return;
    }

    auto flush_tasks = [this, &tasks, &handle_ids] {
        if (tasks.empty()) {
            return;
        }

        bool posted = false;
        try {
            posted = _callback_poster(std::move(tasks));
        } catch (...) {
            GST_EVENTLOOP_LOG_ERROR("callback batch post threw");
        }
        if (!posted) {
            for (const std::uint64_t handle_id : handle_ids) {
                recover_callback_post_failure(handle_id);
            }
        }
#ifdef GST_EVENTLOOP_TESTING
        if (posted && _after_callback_batch_published) {
            _after_callback_batch_published();
        }
#endif
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
        try {
            if (prepare_callback_task(event, task, handle_id)) {
                tasks.push_back(std::move(task));
                handle_ids.push_back(handle_id);
            }
        } catch (...) {
            recover_callback_post_failure(event.key);
            GST_EVENTLOOP_LOG_ERROR("callback task allocation failed");
        }
    }

    flush_tasks();
}

// 选出本次就绪对应的回调并构造批次任务。
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

    // 重新启用一次性监听。
    if (callbacks.empty()) {
        if (!_poller.update(it->second.native_fd, event.key, it->second.interest, true)) {
            GST_EVENTLOOP_LOG_ERROR(
                "rearm ignored ready event failed handle_id=%llu native_fd=%d",
                static_cast<unsigned long long>(event.key),
                it->second.native_fd);
        }
        return false;
    }

    auto batch = std::make_shared<CallbackBatch>();
    batch->registration = it->second.registration;
    batch->callbacks = std::move(callbacks);
    handle_id = event.key;
    const IoRegistration identity = it->second.identity;
    task = [this, batch, handle_id, identity] {
        for (const auto& [event_type, cb] : batch->callbacks) {
#ifdef GST_EVENTLOOP_TESTING
            if (_before_callback_begin) {
                _before_callback_begin();
            }
#endif
            bool permitted = false;
            {
                std::lock_guard<std::mutex> execution_lock(batch->registration->execution->mutex);
                std::lock_guard<std::mutex> registration_lock(batch->registration->mutex);
                permitted = !batch->registration->execution->hard_stopping &&
                            !batch->registration->revoked;
            }
            if (!permitted) {
                continue;
            }
#ifdef GST_EVENTLOOP_TESTING
            if (_after_callback_begin) {
                _after_callback_begin();
            }
#endif
            try {
                cb(IoContext{event_type, identity});
            } catch (...) {
                GST_EVENTLOOP_LOG_ERROR(
                    "io callback threw handle_id=%llu",
                    static_cast<unsigned long long>(handle_id));
            }
        }
    };
    // 任务和退休所需的存储都构造成功后，才标记批次正在运行。
    it->second.registration->callback_finished = false;
    it->second.callback_running = true;
    batch->owner = this;
    return true;
}

// 投递失败后恢复注册状态。
void EventLoopEngine::recover_callback_post_failure(std::uint64_t handle_id) {
    auto it = _registrations.find(handle_id);
    if (it == _registrations.end()) {
        return;
    }

    it->second.registration->callback_finished = false;
    it->second.callback_running = false;
    if (!_is_running || it->second.registration->execution->hard_stopping) {
        return;
    }
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
        // 此后入队的生产者必须重新发起唤醒。
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
        // 先销毁命令捕获，再唤醒同步等待的调用方。
        // 不能让失败的注册返回后仍有命令负载尚未析构。
        task.operation = {};
        if (task.completion) {
            task.completion->set_value(succeeded);
        }
    }

    return all_succeeded;
}

void EventLoopEngine::consume_callback_completions() {
    for (const auto& [handle_id, registration] : _registrations) {
        if (registration.callback_running &&
            registration.registration->callback_finished.exchange(false)) {
            on_callback_finished(handle_id);
        }
    }
}

// 跨线程的 IO 状态操作统一提交到队列。
// 包括修改监听事件类型、设置回调等。
bool EventLoopEngine::submit_io_state_task(IoStateOperation operation) {
    if (!operation || !can_submit_from_current_thread()) {
        return false;
    }

    if (current_owner == this) {
        return _accepting_tasks && operation();
    }

    std::future<bool> result;
    {
        std::lock_guard<std::mutex> lifecycle_lock(_lifecycle_mutex);
        if (!_accepting_tasks) {
            return false;
        }

        // run() 前的 IO 注册先存入队列。
        // 此时返回值只表示是否入队成功。
        if (_state == EngineState::Ready) {
            return enqueue_io_state_task(IoStateTask{std::move(operation), nullptr});
        }
        if (_state != EngineState::Running) {
            return false;
        }


        // run() 后由调用方等待 owner 返回实际执行结果。
        auto completion = std::make_shared<std::promise<bool>>();
        result = completion->get_future();
        if (!enqueue_io_state_task(IoStateTask{std::move(operation), std::move(completion)})) {
            return false;
        }
    }

    return result.get();
}

// 将状态任务入队。
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
        // 在 owner 取走这批任务之前，写入一次 eventfd 即可完成唤醒。
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
        task.operation = {};
        if (task.completion) {
            task.completion->set_value(false);
        }
    }
}

void EventLoopEngine::clear_registrations_in_owner() {
    // stop 调用到此处时 owner 已退出，控制对象须保留到 epoll 确认关闭。
    std::unordered_map<std::uint64_t, EventRegistration> registrations;
    registrations.swap(_registrations);
    for (auto& [handle_id, registration] : registrations) {
        (void)handle_id;
        registration.registration->registered = false;
        if (_poller.remove(registration.native_fd)) {
            registration.registration->backend_detached = true;
        }
    }
    if (_poller.close()) {
        for (auto& [handle_id, registration] : registrations) {
            (void)handle_id;
            registration.registration->backend_detached = true;
        }
    }
}

void EventLoopEngine::wakeup() {
    if (!_wakeup_handle.valid()) {
        return;
    }

    uint64_t value = 1;
    ssize_t ret = -1;
    for (int attempt = 0; attempt < 8; ++attempt) {
        ret = ::write(_wakeup_handle.native_fd(), &value, sizeof(value));
        if (ret >= 0 || errno != EINTR) {
            break;
        }
    }
    if (ret != static_cast<ssize_t>(sizeof(value)) &&
        !(ret < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))) {
        GST_EVENTLOOP_LOG_ERROR("eventfd wakeup write failed: %s", std::strerror(errno));
        std::lock_guard<std::mutex> lock(_io_state_mutex);
        _io_wakeup_pending = false;
    }
}

void EventLoopEngine::drain_wakeup() {
    uint64_t value = 0;
    while (::read(_wakeup_handle.native_fd(), &value, sizeof(value)) == sizeof(value)) {
    }
}

bool EventLoopEngine::register_event_in_owner(
    IoRegistration identity,
    int native_fd,
    Event_type interest,
    IoCallback cb
) {
    const std::uint64_t handle_id = identity.registration_id();
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
    registration.identity = identity;
    registration.registration = identity._registration.lock();
    if (!registration.registration) {
        return false;
    }
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

    _registrations.at(handle_id).registration->backend_detached = false;

    return true;
}

bool EventLoopEngine::set_callback_in_owner(
    std::uint64_t handle_id,
    Event_type type,
    IoCallback cb
) {
    // 先构造替换回调，状态修改完成后再销毁旧回调的捕获对象。
    std::shared_ptr<detail::IoRegistrationState> registration;
    EventCallbacks callbacks;
    callbacks.assign(type, cb);
    auto it = _registrations.find(handle_id);
    if (it == _registrations.end()) {
        GST_EVENTLOOP_LOG_ERROR(
            "IO operation failed operation=set-callback handle_id=%llu: registration not found",
            static_cast<unsigned long long>(handle_id));
        return false;
    }

    registration = it->second.registration;
    if (has_event(type, Event_type::READ_EVENT)) {
        callbacks.read.swap(it->second.callbacks.read);
    }
    if (has_event(type, Event_type::WRITE_EVENT)) {
        callbacks.write.swap(it->second.callbacks.write);
    }
    if (has_event(type, Event_type::ERROR_EVENT)) {
        callbacks.error.swap(it->second.callbacks.error);
    }
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

    {
        std::lock_guard<std::mutex> lock(it->second.registration->mutex);
        it->second.registration->revoked = true;
        it->second.registration->registered = false;
        it->second.registration->backend_detached = true;
    }

    // 用户捕获对象始终在注册锁和队列锁之外析构。
    auto removed = _registrations.extract(it);
    return true;
}

void EventLoopEngine::on_callback_finished(std::uint64_t handle_id) {
    auto it = _registrations.find(handle_id);
    if (it == _registrations.end()) {
        return;
    }

    it->second.callback_running = false;
    if (!_is_running || it->second.registration->execution->hard_stopping) {
        return;
    }
    if (!_poller.update(it->second.native_fd, handle_id, it->second.interest, true)) {
        GST_EVENTLOOP_LOG_ERROR(
            "rearm after callback failed handle_id=%llu native_fd=%d",
            static_cast<unsigned long long>(handle_id),
            it->second.native_fd);
    }
}

}
}
