#pragma once

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>

namespace GST {
namespace BASE {

struct TimerContext;

namespace detail {

struct IoExecutionState;

using TimerCallbackSnapshot = std::shared_ptr<const std::function<void(const TimerContext&)>>;

// 调度队列和各次独立批次保留状态；公开凭证只保留弱引用。
struct TimerRegistrationState {
    std::shared_ptr<IoExecutionState> execution;
    std::mutex mutex;
    bool revoked{false};
    std::atomic_bool registered{true};
    std::atomic_bool accepted{false};
    std::chrono::steady_clock::time_point deadline;
    std::chrono::steady_clock::duration interval{0};
    bool scheduled{true};
    std::size_t in_flight{0};
    // 只用于旧投递失败的条件清理，不改变已选中任务的执行资格。
    std::uint64_t schedule_generation{1};
    TimerCallbackSnapshot callback;
    std::function<void()> on_done;

    ~TimerRegistrationState();
};

}
}
}
