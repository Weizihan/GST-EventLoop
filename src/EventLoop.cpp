#include "EventLoop.h"

#include "EventLoopEngine.h"
#include "InternalLog.h"
#include "IoRegistrationState.h"
#include "TimerEngine.h"
#include "TimerRegistrationState.h"

#include <limits>
#include <exception>
#include <utility>

namespace GST {
namespace BASE {
namespace {

std::mutex active_loop_mutex;
EventLoop* active_loop = nullptr;

struct LifecycleScope;
thread_local const LifecycleScope* current_lifecycle = nullptr;

struct LifecycleScope {
    explicit LifecycleScope(const EventLoop* value)
        : loop(value), previous(current_lifecycle) {
        current_lifecycle = this;
    }

    ~LifecycleScope() {
        current_lifecycle = previous;
    }

    static bool contains(const EventLoop* value) {
        for (auto scope = current_lifecycle; scope; scope = scope->previous) {
            if (scope->loop == value) {
                return true;
            }
        }
        return false;
    }

    const EventLoop* loop;
    const LifecycleScope* previous;
};

thread_local const EventLoop* current_access_owner = nullptr;

// 每个管理入口短暂保留访问资格，不拥有 Loop；收尾关闭门禁后等待归还。
// 注册状态和用户负载须在此对象之前析构，归还时不执行用户代码。
struct LoopAccess {
    LoopAccess(std::shared_ptr<detail::IoExecutionState> state, EventLoop* expected)
        : execution(std::move(state)), previous(current_access_owner) {
        if (!execution) {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(execution->access_mutex);
            if (!execution->accepting_calls || execution->owner != expected) {
                return;
            }
            owner = execution->owner;
            ++execution->active_calls;
        }
        current_access_owner = owner;
    }

    ~LoopAccess() {
        if (!owner) {
            return;
        }
        current_access_owner = previous;
        {
            std::lock_guard<std::mutex> lock(execution->access_mutex);
            --execution->active_calls;
        }
        execution->access_cv.notify_all();
    }

    LoopAccess(const LoopAccess&) = delete;
    LoopAccess& operator=(const LoopAccess&) = delete;

    static bool contains(const EventLoop* loop) {
        // 单活跃 Loop 下，嵌套管理调用只需保存并恢复当前 Loop 身份。
        return current_access_owner == loop;
    }

    std::shared_ptr<detail::IoExecutionState> execution;
    const EventLoop* previous;
    EventLoop* owner{nullptr};
};

template <typename Callback>
struct CallbackCleanup {
    Callback& callback;
    ~CallbackCleanup() {
        callback = {};
    }
};

std::chrono::steady_clock::time_point relative_timer_deadline(
    std::chrono::steady_clock::time_point requested_at,
    std::chrono::nanoseconds delay
) {
    using Clock = std::chrono::steady_clock;
    const auto duration = std::chrono::ceil<Clock::duration>(delay);
    return requested_at.time_since_epoch() > Clock::duration::max() - duration
        ? Clock::time_point::max() : requested_at + duration;
}

}

bool IoRegistration::valid() const noexcept {
    LoopAccess access(_execution.lock(), _owner);
    if (!access.owner) {
        return false;
    }
    const auto registration = _registration.lock();
    return _owner != nullptr && _generation != 0 && _handle_id != 0 &&
           registration && registration->registered;
}

bool IoRegistration::set_cb(Event_type type, std::function<void(const IoContext&)> cb) const {
    LoopAccess access(_execution.lock(), _owner);
    CallbackCleanup<std::function<void(const IoContext&)>> cleanup{cb};
    return access.owner && access.owner->set_io_callback_impl(*this, type, std::move(cb));
}

bool IoRegistration::update(Event_type interest) const {
    LoopAccess access(_execution.lock(), _owner);
    return access.owner && access.owner->update_io_impl(*this, interest);
}

bool IoRegistration::cancel() const {
    LoopAccess access(_execution.lock(), _owner);
    return access.owner && access.owner->remove_impl(*this);
}

bool TimerRegistration::valid() const noexcept {
    LoopAccess access(_execution.lock(), _owner);
    if (!access.owner) {
        return false;
    }
    const auto registration = _registration.lock();
    return _owner != nullptr && _generation != 0 && _timer_id != 0 &&
           registration && registration->registered;
}

bool TimerRegistration::update(const TimerSchedule& schedule) const {
    const auto updated_at = std::chrono::steady_clock::now();
    LoopAccess access(_execution.lock(), _owner);
    return access.owner && access.owner->update_timer_impl(*this, schedule, updated_at);
}

bool TimerRegistration::set_cb(std::function<void(const TimerContext&)> cb) const {
    LoopAccess access(_execution.lock(), _owner);
    CallbackCleanup<std::function<void(const TimerContext&)>> cleanup{cb};
    return access.owner && access.owner->set_timer_callback_impl(*this, std::move(cb));
}

bool TimerRegistration::cancel() const {
    LoopAccess access(_execution.lock(), _owner);
    return access.owner && access.owner->remove_timer_impl(*this);
}

EventLoop::EventLoop() = default;

EventLoop::~EventLoop() {
    if (in_managed_execution()) {
        GST_EVENTLOOP_LOG_ERROR("EventLoop must be destroyed by an external thread");
        std::terminate();
    }
    {
        std::lock_guard<std::mutex> lock(_state_mutex);
        _destroying = true;
    }
    stop();
    {
        std::unique_lock<std::mutex> lock(_state_mutex);
        _stop_cv.wait(lock, [this] {
            return !_run_active;
        });
    }
    finish_shutdown();
}

bool EventLoop::in_managed_execution() const noexcept {
    return _callback_pool.is_worker_thread() || EventLoopEngine::is_owner_thread() ||
           TimerEngine::is_owner_thread() ||
           LifecycleScope::contains(this) || LoopAccess::contains(this);
}

void EventLoop::release_process_slot() {
    std::lock_guard<std::mutex> lock(active_loop_mutex);
    if (_owns_process_slot && active_loop == this) {
        active_loop = nullptr;
        _owns_process_slot = false;
    }
}

bool EventLoop::init() {
    return init(EventLoopConfig{});
}

bool EventLoop::init(const EventLoopConfig& config) {
    if (in_managed_execution()) {
        GST_EVENTLOOP_LOG_ERROR("EventLoop::init cannot be called from an IO callback");
        return false;
    }

    if (config.engine_num == 0) {
        GST_EVENTLOOP_LOG_WARN("EventLoopConfig engine_num <= 0");
        return false;
    }

    if (config.callback_thread_num == 0) {
        GST_EVENTLOOP_LOG_WARN("EventLoopConfig callback_thread_num <= 0");
        return false;
    }

    switch (config.callback_concurrency) {
    case CallbackConcurrency::SerialPerHandle:
        break;
    case CallbackConcurrency::ParallelReadWrite:
    case CallbackConcurrency::FullyParallel:
        GST_EVENTLOOP_LOG_WARN("unsupported callback concurrency policy");
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(_state_mutex);
        if (_destroying || _run_active ||
            (_state != LifecycleState::Uninitialized && _state != LifecycleState::Stopped)) {
            return false;
        }
    }

    std::lock_guard<std::mutex> lifecycle_lock(_lifecycle_operation_mutex);
    LifecycleScope lifecycle_scope(this);

    std::uint64_t next_generation = 0;
    {
        std::lock_guard<std::mutex> state_lock(_state_mutex);
        if (_destroying || _run_active ||
            (_state != LifecycleState::Uninitialized && _state != LifecycleState::Stopped)) {
            return false;
        }
        if (_generation == std::numeric_limits<std::uint64_t>::max()) {
            GST_EVENTLOOP_LOG_ERROR("EventLoop generation exhausted");
            return false;
        }
        {
            std::lock_guard<std::mutex> active_lock(active_loop_mutex);
            if (active_loop != nullptr) {
                return false;
            }
            active_loop = this;
            _owns_process_slot = true;
        }
        _state = LifecycleState::Initializing;
        _stop_requested = false;
        _stop_mode = ShutdownMode::Soft;
        next_generation = _generation + 1;
    }

    std::shared_ptr<detail::IoExecutionState> execution_state;
    try {
        execution_state = std::make_shared<detail::IoExecutionState>();
    } catch (...) {
        {
            std::lock_guard<std::mutex> state_lock(_state_mutex);
            _state = LifecycleState::Stopped;
        }
        release_process_slot();
        return false;
    }

    std::vector<std::shared_ptr<EventLoopEngine>> engines;
    std::shared_ptr<TimerEngine> timer_engine;
    bool initialized = true;
    try {
        engines.reserve(config.engine_num);
        for (std::size_t i = 0; i < config.engine_num; ++i) {
            auto engine = std::make_shared<EventLoopEngine>();
            if (!engine->init([this](std::vector<EventLoopEngine::Task> tasks) {
                    return _callback_pool.post_batch(std::move(tasks));
                })) {
                initialized = false;
                break;
            }
            engines.push_back(std::move(engine));
        }
        if (initialized) {
            timer_engine = std::make_shared<TimerEngine>();
            initialized = timer_engine->init(
                [this, next_generation](std::vector<ThreadPool::Task> tasks) {
                    // 与 stop() 共用截止锁；Timer owner 另持负载，失败也不在此锁内退休。
                    std::lock_guard<std::mutex> lock(_state_mutex);
                    if (_destroying || _stop_requested ||
                        _state != LifecycleState::Running || _generation != next_generation) {
                        return false;
                    }
                    return _callback_pool.post_batch(std::move(tasks));
                });
        }
    } catch (...) {
        initialized = false;
    }
    if (!initialized) {
        for (const auto& initialized_engine : engines) {
            initialized_engine->stop(false);
        }
        engines.clear();
        if (timer_engine) {
            timer_engine->stop();
        }
        {
            std::lock_guard<std::mutex> state_lock(_state_mutex);
            _state = LifecycleState::Stopped;
        }
        release_process_slot();
        return false;
    }

    execution_state->owner = this;
    execution_state->accepting_calls = true;
    std::shared_ptr<detail::IoExecutionState> previous_execution;
    {
        std::lock_guard<std::mutex> state_lock(_state_mutex);
        _config = config;
        _engines = std::move(engines);
        _timer_engine = std::move(timer_engine);
        _next_timer_id = 1;
        _next_handle_id = 1;
        _next_engine_id = 0;
        clear_registrations_locked();
        _generation = next_generation;
        previous_execution = std::move(_execution_state);
        _execution_state = std::move(execution_state);
        if (_stop_requested) {
            if (_stop_mode == ShutdownMode::Hard) {
                _execution_state->hard_stopping = true;
            }
            _timer_engine->request_stop();
            _state = LifecycleState::Stopping;
        } else {
            _state = LifecycleState::Ready;
        }
    }
    return true;
}

bool EventLoop::start() {
    if (in_managed_execution()) {
        GST_EVENTLOOP_LOG_ERROR("EventLoop::start cannot be called from managed execution");
        return false;
    }

    std::unique_lock<std::mutex> lifecycle_lock(_lifecycle_operation_mutex);
    LifecycleScope lifecycle_scope(this);
    {
        std::lock_guard<std::mutex> state_lock(_state_mutex);
        if (_destroying || _run_active) {
            return false;
        }
        if (_state == LifecycleState::Running) {
            return true;
        }
        if (_state != LifecycleState::Ready || _stop_requested) {
            return false;
        }
        _state = LifecycleState::Starting;
    }

    bool started = _callback_pool.start(_config.callback_thread_num);
    for (std::size_t engine_id = 0; started && engine_id < _engines.size(); ++engine_id) {
        started = _engines[engine_id]->run();
        if (!started) {
            GST_EVENTLOOP_LOG_ERROR("EventLoop startup failed engine_id=%zu", engine_id);
        }
    }
    if (started) {
        // IO 启动命令可能已经请求停止；保留既有 start 成功返回及停止模式。
        std::lock_guard<std::mutex> state_lock(_state_mutex);
        if (!_stop_requested) {
            started = _timer_engine->run();
        }
    }
    if (!started) {
        stop(ShutdownMode::Hard);
        lifecycle_lock.unlock();
        finish_shutdown();
        return false;
    }

    {
        std::lock_guard<std::mutex> state_lock(_state_mutex);
        if (!_stop_requested) {
            _state = LifecycleState::Running;
            for (const auto& engine : _engines) {
                engine->enable_dispatch();
            }
            _timer_engine->enable_dispatch();
        }
    }
    return true;
}

bool EventLoop::run() {
    if (in_managed_execution()) {
        GST_EVENTLOOP_LOG_ERROR("EventLoop::run cannot wait from managed execution");
        return false;
    }
    {
        std::unique_lock<std::mutex> lock(_state_mutex);
        if (_destroying || _run_active) {
            return false;
        }
        if (_state == LifecycleState::Stopped) {
            return _generation != 0;
        }
        if (_state != LifecycleState::Running &&
            !(_stop_requested && (_state == LifecycleState::Ready ||
                                  _state == LifecycleState::Stopping))) {
            return false;
        }
        _run_active = true;
        _stop_cv.wait(lock, [this] {
            return _stop_requested;
        });
    }
    finish_shutdown();
    {
        // 最后一次访问 Loop：析构线程可能在通知后立即继续销毁。
        std::lock_guard<std::mutex> lock(_state_mutex);
        _run_active = false;
        _stop_cv.notify_all();
    }
    return true;
}

void EventLoop::stop(ShutdownMode mode) {
    std::lock_guard<std::mutex> lock(_state_mutex);
    if (_state == LifecycleState::Uninitialized || _state == LifecycleState::Stopped) {
        return;
    }
    if (mode == ShutdownMode::Hard) {
        _stop_mode = ShutdownMode::Hard;
        if (_execution_state) {
            std::lock_guard<std::mutex> gate(_execution_state->mutex);
            _execution_state->hard_stopping = true;
        }
    }
    _stop_requested = true;
    _state = LifecycleState::Stopping;
    if (_timer_engine) {
        _timer_engine->request_stop();
    }
    // 通知后不再访问 Loop 的其他成员，run 可以立即开始收尾。
    _stop_cv.notify_all();
}

void EventLoop::finish_shutdown() {
    std::lock_guard<std::mutex> lifecycle_lock(_lifecycle_operation_mutex);
    LifecycleScope lifecycle_scope(this);
    std::vector<std::shared_ptr<EventLoopEngine>> engines;
    std::shared_ptr<TimerEngine> timer_engine;
    std::shared_ptr<detail::IoExecutionState> execution;
    {
        std::lock_guard<std::mutex> state_lock(_state_mutex);
        if (_state == LifecycleState::Uninitialized ||
            _state == LifecycleState::Stopped) {
            return;
        }
        _state = LifecycleState::Stopping;
        _stop_requested = true;
        engines.swap(_engines);
        timer_engine.swap(_timer_engine);
        execution = _execution_state;
    }

    if (execution) {
        std::unique_lock<std::mutex> lock(execution->access_mutex);
        execution->accepting_calls = false;
        execution->access_cv.wait(lock, [&] {
            return execution->active_calls == 0;
        });
        execution->owner = nullptr;
    }
#ifdef GST_EVENTLOOP_TESTING
    if (execution && execution->after_stop_cutoff) {
        execution->after_stop_cutoff();
    }
#endif

    for (const auto& engine : engines) {
        ShutdownMode mode;
        {
            std::lock_guard<std::mutex> lock(_state_mutex);
            mode = _stop_mode;
        }
        engine->stop(mode == ShutdownMode::Soft);
    }
    if (timer_engine) {
        timer_engine->stop();
    }
    ShutdownMode mode;
    {
        std::lock_guard<std::mutex> lock(_state_mutex);
        mode = _stop_mode;
    }
    _callback_pool.stop(mode == ShutdownMode::Soft);

    // 批次退休仍会唤醒 owner，必须在工作线程全部结束之后销毁 Engine。
    engines.clear();
    timer_engine.reset();
    {
        std::lock_guard<std::mutex> state_lock(_state_mutex);
        clear_registrations_locked();
        _state = LifecycleState::Stopped;
    }
    release_process_slot();
}

std::shared_ptr<detail::IoExecutionState> EventLoop::execution_for_access() {
    std::lock_guard<std::mutex> lock(_state_mutex);
    return _execution_state;
}

TimerRegistration EventLoop::add_timer(const TimerSchedule& schedule, TimerCallback cb) {
    return add_timer(schedule, std::move(cb), {});
}

TimerRegistration EventLoop::add_timer(const TimerSchedule& schedule, TimerCallback cb,
                                       std::function<void()> on_done) {
    const auto registered_at = std::chrono::steady_clock::now();
    LoopAccess access(execution_for_access(), this);
    CallbackCleanup<TimerCallback> callback_cleanup{cb};
    CallbackCleanup<std::function<void()>> done_cleanup{on_done};
    if (!access.owner) {
        return {};
    }
    return add_timer_impl(schedule, std::move(cb), std::move(on_done), registered_at);
}

TimerRegistration EventLoop::add_timer_impl(
    const TimerSchedule& schedule,
    TimerCallback cb,
    std::function<void()> on_done,
    std::chrono::steady_clock::time_point registered_at
) {
    if (!schedule.valid() || !cb) {
        return {};
    }
    using Clock = std::chrono::steady_clock;
    auto deadline = schedule._deadline;
    if (schedule._relative) {
        deadline = relative_timer_deadline(registered_at, schedule._delay);
    }
    const auto interval = schedule._repeat
        ? std::chrono::ceil<Clock::duration>(schedule._delay) : Clock::duration::zero();

    // 声明在状态锁之前，拒绝和异常也在解锁后销毁业务负载。
    auto registration = std::make_shared<detail::TimerRegistrationState>();
    registration->deadline = deadline;
    registration->callback = std::make_shared<const TimerCallback>(std::move(cb));
    registration->on_done = std::move(on_done);
    std::lock_guard<std::mutex> lock(_state_mutex);
    if (!accepts_io_locked() || !_timer_engine || _next_timer_id == 0) {
        return {};
    }
    const auto timer_id = _next_timer_id++;
    registration->execution = _execution_state;
    TimerRegistration identity(this, _generation, timer_id, registration, _execution_state);
    return _timer_engine->add(identity, deadline, interval, registration)
        ? identity : TimerRegistration{};
}

bool EventLoop::update_timer(const TimerRegistration& timer, const TimerSchedule& schedule) {
    const auto updated_at = std::chrono::steady_clock::now();
    LoopAccess access(execution_for_access(), this);
    return access.owner && update_timer_impl(timer, schedule, updated_at);
}

bool EventLoop::update_timer_impl(
    const TimerRegistration& timer,
    const TimerSchedule& schedule,
    std::chrono::steady_clock::time_point updated_at
) {
    if (!schedule.valid()) {
        return false;
    }
    using Clock = std::chrono::steady_clock;
    const auto deadline = schedule._relative
        ? relative_timer_deadline(updated_at, schedule._delay) : schedule._deadline;
    const auto interval = schedule._repeat
        ? std::chrono::ceil<Clock::duration>(schedule._delay) : Clock::duration::zero();
    const auto registration = timer._registration.lock();
    std::lock_guard<std::mutex> lock(_state_mutex);
    if (!registration || !accepts_io_locked() || !_timer_engine ||
        timer._owner != this || timer._generation != _generation ||
        registration->execution != _execution_state) {
        return false;
    }
    return _timer_engine->update(timer, deadline, interval, registration);
}

bool EventLoop::set_timer_callback(const TimerRegistration& timer, TimerCallback cb) {
    LoopAccess access(execution_for_access(), this);
    CallbackCleanup<TimerCallback> cleanup{cb};
    return access.owner && set_timer_callback_impl(timer, std::move(cb));
}

bool EventLoop::set_timer_callback_impl(const TimerRegistration& timer, TimerCallback cb) {
    if (!cb) {
        return false;
    }
    const auto registration = timer._registration.lock();
    if (!registration) {
        return false;
    }
    // 快照构造和旧快照的最终清理都在 Loop 状态锁之外；状态保活更久。
    auto callback = std::make_shared<const TimerCallback>(std::move(cb));
    std::lock_guard<std::mutex> lock(_state_mutex);
    if (!accepts_io_locked() || !_timer_engine ||
        timer._owner != this || timer._generation != _generation ||
        registration->execution != _execution_state) {
        return false;
    }
    return _timer_engine->set_callback(registration, callback);
}

bool EventLoop::remove(const TimerRegistration& timer) {
    LoopAccess access(execution_for_access(), this);
    return access.owner && remove_timer_impl(timer);
}

bool EventLoop::remove_timer_impl(const TimerRegistration& timer) {
    // 状态保活到所有内部锁释放之后；最终析构可能重入 stop/on_done。
    const auto registration = timer._registration.lock();
    std::lock_guard<std::mutex> state_lock(_state_mutex);
    if (!registration || !accepts_io_locked() || !_timer_engine ||
        timer._owner != this || timer._generation != _generation ||
        registration->execution != _execution_state) {
        return false;
    }
    {
        std::lock_guard<std::mutex> execution_lock(_execution_state->mutex);
        std::lock_guard<std::mutex> registration_lock(registration->mutex);
        if (!registration->registered || registration->revoked) {
            return false;
        }
        registration->revoked = true;
        registration->registered = false;
    }
    // 删除 set 节点不分配，也不等待 owner 或正在执行的用户函数。
    _timer_engine->remove(registration, timer._timer_id);
    return true;
}

IoRegistration EventLoop::watch_io(
    const NativeHandle& native_handle,
    Event_type interest,
    IoCallback cb
) {
    return watch_io(native_handle, interest, std::move(cb), {});
}

IoRegistration EventLoop::watch_io(
    const NativeHandle& native_handle,
    Event_type interest,
    IoCallback cb,
    std::function<void()> on_done
) {
    LoopAccess access(execution_for_access(), this);
    CallbackCleanup<IoCallback> callback_cleanup{cb};
    CallbackCleanup<std::function<void()>> done_cleanup{on_done};
    if (!access.owner) {
        return {};
    }
    return watch_io_impl(native_handle, interest, std::move(cb), std::move(on_done));
}

IoRegistration EventLoop::watch_io_impl(
    const NativeHandle& native_handle,
    Event_type interest,
    IoCallback cb,
    std::function<void()> on_done
) {
    if (!native_handle.valid() || interest == Event_type::NONE || !cb) {
        return {};
    }

    std::uint64_t generation = 0;
    std::size_t engine_id = 0;
    std::uint64_t handle_id = 0;
    std::shared_ptr<detail::IoExecutionState> execution_state;
    std::shared_ptr<EventLoopEngine> engine = reserve_engine_for_registration(
        native_handle.native_fd(),
        generation,
        engine_id,
        handle_id,
        execution_state);
    if (!engine) {
        return {};
    }

    try {
        auto registration = std::make_shared<detail::IoRegistrationState>();
        registration->execution = std::move(execution_state);
        registration->on_done = std::move(on_done);
        IoRegistration identity(this, generation, engine_id, handle_id,
                                registration, registration->execution);
        if (!engine->register_event(
                identity,
                native_handle.native_fd(),
                interest,
                std::move(cb))) {
            release_registration(generation, handle_id);
            return {};
        }

        registration->accepted = true;
        return identity;
    } catch (...) {
        release_registration(generation, handle_id);
        throw;
    }
}

bool EventLoop::set_io_callback(const IoRegistration& registration, Event_type type, IoCallback cb) {
    LoopAccess access(execution_for_access(), this);
    CallbackCleanup<IoCallback> cleanup{cb};
    return access.owner && set_io_callback_impl(registration, type, std::move(cb));
}

bool EventLoop::set_io_callback_impl(const IoRegistration& registration, Event_type type, IoCallback cb) {
    if (type == Event_type::NONE || !cb) {
        return false;
    }

    std::shared_ptr<EventLoopEngine> engine = engine_for(registration);
    return engine && engine->set_callback(
        registration._handle_id, type, std::move(cb), registration._registration.lock());
}

bool EventLoop::update_io(const IoRegistration& registration, Event_type interest) {
    LoopAccess access(execution_for_access(), this);
    return access.owner && update_io_impl(registration, interest);
}

bool EventLoop::update_io_impl(const IoRegistration& registration, Event_type interest) {
    if (interest == Event_type::NONE) {
        return false;
    }

    std::shared_ptr<EventLoopEngine> engine = engine_for(registration);
    return engine && engine->update_event(registration._handle_id, interest);
}

bool EventLoop::remove(const IoRegistration& registration) {
    LoopAccess access(execution_for_access(), this);
    return access.owner && remove_impl(registration);
}

bool EventLoop::remove_impl(const IoRegistration& registration) {
    // 保留状态到路由清理结束；公开凭证本身只持弱引用。
    const auto state = registration._registration.lock();
    std::shared_ptr<EventLoopEngine> engine = engine_for(registration);
    if (!engine || !state || state->removal_pending.exchange(true)) {
        return false;
    }

    // 同一注册只允许一个删除请求在途，避免 run 前重复入队破坏启动结果。
    // 不持注册锁等待 owner；请求失败时仅释放认领，不恢复失效的注册。
    try {
        if (!engine->remove_event(registration._handle_id)) {
            state->removal_pending = false;
            return false;
        }
    } catch (...) {
        state->removal_pending = false;
        throw;
    }

    state->registered = false;
    release_registration(registration._generation, registration._handle_id);
    return true;
}

bool EventLoop::accepts_io_locked() const noexcept {
    return !_destroying && !_stop_requested &&
           (_state == LifecycleState::Ready ||
            _state == LifecycleState::Starting ||
            _state == LifecycleState::Running);
}

std::shared_ptr<EventLoopEngine> EventLoop::reserve_engine_for_registration(
    int native_fd,
    std::uint64_t& generation,
    std::size_t& engine_id,
    std::uint64_t& handle_id,
    std::shared_ptr<detail::IoExecutionState>& execution_state
) {
    std::lock_guard<std::mutex> state_lock(_state_mutex);
    if (!accepts_io_locked() || _engines.empty()) {
        return {};
    }

    auto fd_it = _registered_fds.find(native_fd);
    const std::size_t candidate = fd_it == _registered_fds.end()
        ? _next_engine_id.load() % _engines.size()
        : fd_it->second.engine_id;
    if (!_engines[candidate]->can_submit_from_current_thread()) {
        return {};
    }

    handle_id = _next_handle_id.load();
    while (handle_id != 0 &&
           !_next_handle_id.compare_exchange_weak(handle_id, handle_id + 1)) {
    }
    if (handle_id == 0) {
        GST_EVENTLOOP_LOG_ERROR("EventLoop registration id exhausted");
        return {};
    }

    generation = _generation;
    execution_state = _execution_state;
    if (fd_it == _registered_fds.end()) {
        const std::size_t engine_ticket = _next_engine_id.fetch_add(1);
        engine_id = engine_ticket % _engines.size();
        fd_it = _registered_fds.emplace(
            native_fd,
            FdRegistration{engine_id, 0}).first;
    } else {
        engine_id = fd_it->second.engine_id;
    }

    ++fd_it->second.reservation_count;
    try {
        _registration_fds.emplace(handle_id, native_fd);
    } catch (...) {
        if (--fd_it->second.reservation_count == 0) {
            _registered_fds.erase(fd_it);
        }
        throw;
    }
    return _engines[engine_id];
}

void EventLoop::release_registration(
    std::uint64_t generation,
    std::uint64_t handle_id
) {
    std::lock_guard<std::mutex> state_lock(_state_mutex);
    if (generation != _generation) {
        return;
    }

    auto it = _registration_fds.find(handle_id);
    if (it == _registration_fds.end()) {
        return;
    }

    auto fd_it = _registered_fds.find(it->second);
    if (fd_it != _registered_fds.end() &&
        --fd_it->second.reservation_count == 0) {
        _registered_fds.erase(fd_it);
    }
    _registration_fds.erase(it);
}

void EventLoop::clear_registrations_locked() {
    _registered_fds.clear();
    _registration_fds.clear();
}

std::shared_ptr<EventLoopEngine> EventLoop::engine_for(const IoRegistration& registration) {
    // 状态引用须保留到 state_lock 释放之后，因为最后一次释放可能触发 on_done 重入。
    const auto state = registration._registration.lock();
    std::lock_guard<std::mutex> state_lock(_state_mutex);
    if (!state || !state->registered ||
        !accepts_io_locked() ||
        registration._owner != this ||
        registration._generation != _generation ||
        registration._engine_id >= _engines.size()) {
        return {};
    }

    const auto& engine = _engines[registration._engine_id];
    return engine->can_submit_from_current_thread() ? engine : nullptr;
}

}
}
