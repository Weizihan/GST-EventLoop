#pragma once

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>

namespace GST {
namespace BASE {
class EventLoop;
namespace detail {

// 每次 init() 创建一套执行门禁，由本代的所有 Engine 共用。
struct IoExecutionState {
    std::mutex mutex;
    std::atomic_bool hard_stopping{false};
    // 保护非拥有的 Loop 访问入口；只计管理调用，不计回调或资源所有权。
    std::mutex access_mutex;
    std::condition_variable access_cv;
    EventLoop* owner{nullptr};
    std::size_t active_calls{0};
    bool accepting_calls{false};
#ifdef GST_EVENTLOOP_TESTING
    std::function<void()> after_stop_cutoff;
#endif
};

// 内部注册身份，公开句柄仅持弱引用。注册记录、已接受的命令和回调批次
// 保留此状态，直到相关回调副本全部析构；不额外引入完成队列或计数器。
struct IoRegistrationState {
    std::shared_ptr<IoExecutionState> execution;
    std::mutex mutex;
    bool revoked{false};
    std::atomic_bool registered{true};
    std::atomic_bool removal_pending{false};
    std::atomic_bool backend_detached{true};
    std::atomic_bool callback_finished{false};
    std::atomic_bool accepted{false};
    std::function<void()> on_done;

    ~IoRegistrationState() {
        if (accepted && backend_detached && on_done) {
            // 不得在持有内部锁时释放最后一个状态引用。
            // 完成通知不保证由固定线程执行。
            try {
                on_done();
            } catch (...) {
            }
        }
    }
};

}
}
}
