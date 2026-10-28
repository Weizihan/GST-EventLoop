#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <vector>

namespace test {

// 仅让调用测试线程的一次分配失败；清理前先解除故障注入。
void set_allocation_failure(int allocations_before_failure);
void clear_allocation_failure() noexcept;

struct TestCase {
    const char* name;
    void (*run)();
};

inline std::vector<TestCase>& cases() {
    static std::vector<TestCase> registered;
    return registered;
}

struct Register {
    Register(const char* name, void (*run)()) {
        cases().push_back({name, run});
    }
};

[[noreturn]] inline void fail(const char* expression, const char* file, int line) {
    std::fprintf(stderr, "%s:%d: failed: %s\n", file, line, expression);
    std::abort();
}

// 用谓词确定并发交错；超时仅用于发现测试卡住。
class Gate {
public:
    void open() {
        {
            std::lock_guard<std::mutex> lock(_mutex);
            _open = true;
        }
        _changed.notify_all();
    }

    void wait() {
        if (!wait_for(std::chrono::seconds(10))) {
            fail("gate reached", __FILE__, __LINE__);
        }
    }

    template <typename Rep, typename Period>
    bool wait_for(const std::chrono::duration<Rep, Period>& timeout) {
        std::unique_lock<std::mutex> lock(_mutex);
        return _changed.wait_for(lock, timeout, [this] { return _open; });
    }

    bool is_open() const {
        std::lock_guard<std::mutex> lock(_mutex);
        return _open;
    }

private:
    mutable std::mutex _mutex;
    std::condition_variable _changed;
    bool _open{false};
};

} // 测试命名空间

#define TEST_CHECK(expression) \
    do { if (!(expression)) ::test::fail(#expression, __FILE__, __LINE__); } while (false)

#define TEST_CASE(name) \
    static void name(); \
    static ::test::Register registered_##name(#name, &name); \
    static void name()
