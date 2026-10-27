#include "TestSupport.h"

#include "ThreadPool.h"

#include <atomic>
#include <cstdlib>
#include <memory>
#include <new>

namespace {

// 沿用此前测试的分配故障注入，无需在生产代码中增加分配钩子。
thread_local std::ptrdiff_t fail_allocation_after = -1;

}

void test::set_allocation_failure(int allocations_before_failure) {
    TEST_CHECK(allocations_before_failure >= 0);
    TEST_CHECK(fail_allocation_after == -1);
    fail_allocation_after = allocations_before_failure;
}

void test::clear_allocation_failure() noexcept { fail_allocation_after = -1; }

void* operator new(std::size_t size) {
    if (fail_allocation_after == 0) {
        fail_allocation_after = -1;
        throw std::bad_alloc();
    }
    if (fail_allocation_after > 0) --fail_allocation_after;
    if (void* allocation = std::malloc(size == 0 ? 1 : size)) return allocation;
    throw std::bad_alloc();
}

void operator delete(void* allocation) noexcept { std::free(allocation); }
void operator delete(void* allocation, std::size_t) noexcept { std::free(allocation); }

using namespace GST::BASE;

TEST_CASE(pool_allocation_failure_does_not_publish_partial_batch) {
    ThreadPool pool;
    TEST_CHECK(pool.start(1));
    test::Gate entered;
    test::Gate release;
    std::atomic_int calls{0};
    std::atomic_int retired{0};
    TEST_CHECK(pool.post([&] { entered.open(); release.wait(); }));
    entered.wait();
    struct Probe {
        std::atomic_int* retired;
        ~Probe() { ++*retired; }
    };
    std::vector<ThreadPool::Task> batch;
    for (int i = 0; i != 3; ++i) {
        auto probe = std::make_shared<Probe>();
        probe->retired = &retired;
        batch.emplace_back([probe, &calls] { ++calls; });
    }
    bool failed = false;
    test::set_allocation_failure(1);
    try {
        (void)pool.post_batch(std::move(batch));
    } catch (const std::bad_alloc&) {
        failed = true;
    }
    test::clear_allocation_failure();
    TEST_CHECK(failed);
    TEST_CHECK(retired == 3);
    release.open();
    pool.stop(true);
    TEST_CHECK(calls == 0);
}
