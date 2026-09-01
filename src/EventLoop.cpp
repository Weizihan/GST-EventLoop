#include "EventLoop.h"

#include "EventLoopEngine.h"
#include "InternalLog.h"

#include <limits>
#include <utility>

namespace GST {
namespace BASE {

EventLoop::EventLoop() = default;

EventLoop::~EventLoop() {
    stop();
}

bool EventLoop::init() {
    return init(EventLoopConfig{});
}

bool EventLoop::init(const EventLoopConfig& config) {
    if (_callback_pool.is_worker_thread()) {
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

    std::lock_guard<std::mutex> lifecycle_lock(_lifecycle_operation_mutex);

    std::uint64_t next_generation = 0;
    {
        std::lock_guard<std::mutex> state_lock(_state_mutex);
        if (_state != LifecycleState::Uninitialized &&
            _state != LifecycleState::Stopped) {
            return false;
        }
        if (_generation == std::numeric_limits<std::uint64_t>::max()) {
            GST_EVENTLOOP_LOG_ERROR("EventLoop generation exhausted");
            return false;
        }
        _state = LifecycleState::Initializing;
        next_generation = _generation + 1;
    }

    if (!_callback_pool.start(config.callback_thread_num)) {
        GST_EVENTLOOP_LOG_ERROR("callback thread pool start failed");
        _callback_pool.stop(false);
        std::lock_guard<std::mutex> state_lock(_state_mutex);
        _state = LifecycleState::Stopped;
        return false;
    }

    std::vector<std::shared_ptr<EventLoopEngine>> engines;
    engines.reserve(config.engine_num);
    for (std::size_t i = 0; i < config.engine_num; ++i) {
        auto engine = std::make_shared<EventLoopEngine>();
        if (!engine->init([this](std::vector<EventLoopEngine::Task> tasks) {
                return _callback_pool.post_batch(std::move(tasks));
            })) {
            for (const auto& initialized_engine : engines) {
                initialized_engine->stop(true);
            }
            _callback_pool.stop(false);
            std::lock_guard<std::mutex> state_lock(_state_mutex);
            _state = LifecycleState::Stopped;
            return false;
        }

        engines.push_back(std::move(engine));
    }

    {
        std::lock_guard<std::mutex> state_lock(_state_mutex);
        _config = config;
        _engines = std::move(engines);
        _next_handle_id = 1;
        _next_engine_id = 0;
        clear_registrations_locked();
        _generation = next_generation;
        _state = LifecycleState::Ready;
    }
    return true;
}

bool EventLoop::run() {
    if (_callback_pool.is_worker_thread()) {
        GST_EVENTLOOP_LOG_ERROR("EventLoop::run cannot be called from an IO callback");
        return false;
    }

    std::lock_guard<std::mutex> lifecycle_lock(_lifecycle_operation_mutex);
    std::vector<std::shared_ptr<EventLoopEngine>> engines;
    {
        std::lock_guard<std::mutex> state_lock(_state_mutex);
        if (_state == LifecycleState::Running) {
            return true;
        }
        if (_state != LifecycleState::Ready) {
            return false;
        }
        _state = LifecycleState::Starting;
        engines = _engines;
    }

    for (std::size_t engine_id = 0; engine_id < engines.size(); ++engine_id) {
        if (!engines[engine_id]->run()) {
            GST_EVENTLOOP_LOG_ERROR("EventLoop startup failed engine_id=%zu", engine_id);
            for (const auto& engine : engines) {
                engine->stop(true);
            }
            _callback_pool.stop(false);
            std::lock_guard<std::mutex> state_lock(_state_mutex);
            clear_registrations_locked();
            _state = LifecycleState::Stopped;
            return false;
        }
    }

    {
        std::lock_guard<std::mutex> state_lock(_state_mutex);
        _state = LifecycleState::Running;
        for (const auto& engine : engines) {
            engine->enable_dispatch();
        }
    }
    return !engines.empty();
}

void EventLoop::stop(ShutdownMode mode) {
    if (_callback_pool.is_worker_thread()) {
        GST_EVENTLOOP_LOG_ERROR("EventLoop::stop cannot be called from an IO callback");
        return;
    }

    std::lock_guard<std::mutex> lifecycle_lock(_lifecycle_operation_mutex);
    std::vector<std::shared_ptr<EventLoopEngine>> engines;
    {
        std::lock_guard<std::mutex> state_lock(_state_mutex);
        if (_state == LifecycleState::Uninitialized ||
            _state == LifecycleState::Stopped) {
            return;
        }
        _state = LifecycleState::Stopping;
        engines = _engines;
    }

    for (const auto& engine : engines) {
        engine->stop(mode == ShutdownMode::Hard);
    }
    _callback_pool.stop(mode == ShutdownMode::Soft);

    std::lock_guard<std::mutex> state_lock(_state_mutex);
    clear_registrations_locked();
    _state = LifecycleState::Stopped;
}

IoHandle EventLoop::watch_io(
    const NativeHandle& native_handle,
    Event_type interest,
    IoCallback cb
) {
    if (!native_handle.valid() || interest == Event_type::NONE || !cb) {
        return {};
    }

    std::uint64_t generation = 0;
    std::size_t engine_id = 0;
    std::uint64_t handle_id = 0;
    std::shared_ptr<EventLoopEngine> engine = reserve_engine_for_registration(
        native_handle.native_fd(),
        generation,
        engine_id,
        handle_id);
    if (!engine) {
        return {};
    }

    IoToken token(this, generation, engine_id, handle_id);
    if (!engine->register_event(
            token,
            native_handle.native_fd(),
            interest,
            std::move(cb))) {
        release_registration(generation, handle_id);
        return {};
    }

    return IoHandle(this, generation, engine_id, handle_id);
}

bool EventLoop::set_io_callback(const IoHandle& handle, Event_type type, IoCallback cb) {
    if (type == Event_type::NONE || !cb) {
        return false;
    }

    std::shared_ptr<EventLoopEngine> engine = engine_for(handle);
    return engine && engine->set_callback(handle._handle_id, type, std::move(cb));
}

bool EventLoop::set_io_callback(const IoToken& token, Event_type type, IoCallback cb) {
    if (type == Event_type::NONE || !cb) {
        return false;
    }

    std::shared_ptr<EventLoopEngine> engine = engine_for(token);
    return engine && engine->set_callback(token._handle_id, type, std::move(cb));
}

bool EventLoop::update_io(const IoHandle& handle, Event_type interest) {
    if (interest == Event_type::NONE) {
        return false;
    }

    std::shared_ptr<EventLoopEngine> engine = engine_for(handle);
    return engine && engine->update_event(handle._handle_id, interest);
}

bool EventLoop::update_io(const IoToken& token, Event_type interest) {
    if (interest == Event_type::NONE) {
        return false;
    }

    std::shared_ptr<EventLoopEngine> engine = engine_for(token);
    return engine && engine->update_event(token._handle_id, interest);
}

bool EventLoop::remove(IoHandle& handle) {
    std::shared_ptr<EventLoopEngine> engine = engine_for(handle);
    if (!engine || !engine->remove_event(handle._handle_id)) {
        return false;
    }

    release_registration(handle._generation, handle._handle_id);
    handle.invalidate();
    return true;
}

bool EventLoop::remove(const IoToken& token) {
    std::shared_ptr<EventLoopEngine> engine = engine_for(token);
    if (!engine || !engine->remove_event(token._handle_id)) {
        return false;
    }

    release_registration(token._generation, token._handle_id);
    return true;
}

bool EventLoop::accepts_io_locked() const noexcept {
    return _state == LifecycleState::Ready ||
           _state == LifecycleState::Starting ||
           _state == LifecycleState::Running;
}

std::shared_ptr<EventLoopEngine> EventLoop::reserve_engine_for_registration(
    int native_fd,
    std::uint64_t& generation,
    std::size_t& engine_id,
    std::uint64_t& handle_id
) {
    std::lock_guard<std::mutex> state_lock(_state_mutex);
    if (!accepts_io_locked() || _engines.empty()) {
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
    auto fd_it = _registered_fds.find(native_fd);
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

std::shared_ptr<EventLoopEngine> EventLoop::engine_for(const IoHandle& handle) {
    std::lock_guard<std::mutex> state_lock(_state_mutex);
    if (!handle.valid() ||
        !accepts_io_locked() ||
        handle._owner != this ||
        handle._generation != _generation ||
        handle._engine_id >= _engines.size()) {
        return {};
    }

    return _engines[handle._engine_id];
}

std::shared_ptr<EventLoopEngine> EventLoop::engine_for(const IoToken& token) {
    std::lock_guard<std::mutex> state_lock(_state_mutex);
    if (!token.valid() ||
        !accepts_io_locked() ||
        token._owner != this ||
        token._generation != _generation ||
        token._engine_id >= _engines.size()) {
        return {};
    }

    return _engines[token._engine_id];
}

}
}
