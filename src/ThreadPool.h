#pragma once

#include <condition_variable>
#include <cstddef>
#include <functional>
#include <mutex>
#include <queue>
#include <thread>
#include <vector>

namespace GST {
namespace BASE {

class EventLoop;

class ThreadPool {
public:
    using Task = std::function<void()>;

    ThreadPool() = default;
    ~ThreadPool();

    ThreadPool(const ThreadPool&) = delete;
    ThreadPool& operator=(const ThreadPool&) = delete;

    bool start(std::size_t thread_count);
    void stop(bool drain_pending = true);

    bool post(Task task);
    bool post_batch(std::vector<Task> tasks);

private:
    friend class EventLoop;

    bool is_worker_thread() const noexcept;
    void worker_main();

    std::mutex              _mutex;
    std::condition_variable _cv;
    std::queue<Task>        _tasks;
    std::vector<std::thread> _workers;
    bool                    _stopping{false};
};

}
}
