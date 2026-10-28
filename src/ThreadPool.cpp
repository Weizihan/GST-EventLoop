#include "ThreadPool.h"

#include <algorithm>

namespace GST {
namespace BASE {
namespace {

thread_local const ThreadPool* current_worker_pool = nullptr;

}

ThreadPool::~ThreadPool() {
    stop();
}

bool ThreadPool::start(std::size_t thread_count) {
    if (thread_count == 0) {
        return false;
    }

    std::lock_guard<std::mutex> lock(_mutex);
    if (!_workers.empty()) {
        return false;
    }

    _stopping = false;

    try {
        _workers.reserve(thread_count);
        for (std::size_t i = 0; i < thread_count; ++i) {
            _workers.emplace_back(&ThreadPool::worker_main, this);
        }
    } catch (...) {
        _stopping = true;
        _cv.notify_all();
        return false;
    }

    return true;
}

void ThreadPool::stop(bool drain_pending) {
    std::list<Task> cancelled;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _stopping = true;
        if (!drain_pending) {
            cancelled.swap(_tasks);
        }
    }

    _cv.notify_all();
    // 捕获对象的析构可能重入 EventLoop，不得在持有 _mutex 时销毁。
    cancelled.clear();

    for (std::thread& worker : _workers) {
        if (worker.joinable()) {
            worker.join();
        }
    }

    _workers.clear();
}

bool ThreadPool::post(Task task) {
    if (!task) {
        return false;
    }

    std::list<Task> prepared;
    prepared.push_back(std::move(task));
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_stopping) {
            return false;
        }
        _tasks.splice(_tasks.end(), prepared);
    }

    _cv.notify_one();
    return true;
}

bool ThreadPool::post_batch(std::vector<Task> tasks) {
    if (tasks.empty()) {
        return true;
    }

    for (const Task& task : tasks) {
        if (!task) {
            return false;
        }
    }

    // 发布前为整批任务分配节点，避免分配失败时只接受了其中一部分，
    // 而调用方却按整批失败进行恢复。
    std::list<Task> prepared;
    for (Task& task : tasks) {
        prepared.push_back(std::move(task));
    }

    std::size_t notify_count = 0;
    {
        std::lock_guard<std::mutex> lock(_mutex);
        if (_stopping) {
            return false;
        }

        notify_count = std::min(tasks.size(), _workers.size());
        _tasks.splice(_tasks.end(), prepared);
    }

    for (std::size_t i = 0; i < notify_count; ++i) {
        _cv.notify_one();
    }
    return true;
}

bool ThreadPool::is_worker_thread() const noexcept {
    return current_worker_pool == this;
}

void ThreadPool::worker_main() {
    current_worker_pool = this;
    while (true) {
        Task task;
        {
            std::unique_lock<std::mutex> lock(_mutex);
            _cv.wait(lock, [this] {
                return _stopping || !_tasks.empty();
            });

            if (_stopping && _tasks.empty()) {
                current_worker_pool = nullptr;
                return;
            }

            task = std::move(_tasks.front());
            _tasks.pop_front();
        }

        try {
            task();
        } catch (...) {
        }
    }
}

}
}
