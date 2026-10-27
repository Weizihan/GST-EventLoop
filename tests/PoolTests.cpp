#include "TestSupport.h"

#include "ThreadPool.h"

#include <atomic>
#include <memory>
#include <stdexcept>

using namespace GST::BASE;

TEST_CASE(pool_soft_drains_accepted_tasks) {
    ThreadPool pool;
    TEST_CHECK(pool.start(1));
    test::Gate entered;
    test::Gate release;
    std::atomic_int calls{0};
    TEST_CHECK(pool.post([&] { entered.open(); release.wait(); ++calls; }));
    entered.wait();
    TEST_CHECK(pool.post([&] { ++calls; }));
    release.open();
    pool.stop(true);
    TEST_CHECK(calls == 2);
}

TEST_CASE(pool_task_exception_does_not_lose_next_task) {
    ThreadPool pool;
    TEST_CHECK(pool.start(1));
    std::atomic_int calls{0};
    TEST_CHECK(pool.post([] { throw std::runtime_error("expected"); }));
    TEST_CHECK(pool.post([&] { ++calls; }));
    pool.stop(true);
    TEST_CHECK(calls == 1);
}

TEST_CASE(pool_batch_is_accepted_as_a_group) {
    ThreadPool pool;
    TEST_CHECK(pool.start(1));
    std::atomic_int calls{0};
    std::vector<ThreadPool::Task> tasks;
    for (int i = 0; i != 4; ++i) tasks.emplace_back([&] { ++calls; });
    TEST_CHECK(pool.post_batch(std::move(tasks)));
    pool.stop(true);
    TEST_CHECK(calls == 4);
    TEST_CHECK(!pool.post([] {}));
}

TEST_CASE(pool_hard_discards_queued_task_and_retires_capture) {
    ThreadPool pool;
    TEST_CHECK(pool.start(1));
    test::Gate occupied;
    test::Gate release;
    test::Gate retired;
    std::atomic_int calls{0};
    TEST_CHECK(pool.post([&] { occupied.open(); release.wait(); }));
    occupied.wait();
    struct Probe {
        test::Gate* retired;
        ~Probe() { retired->open(); }
    };
    auto probe = std::make_shared<Probe>();
    probe->retired = &retired;
    TEST_CHECK(pool.post([probe, &calls] { ++calls; }));
    probe.reset();
    std::thread stopper([&] { pool.stop(false); });
    retired.wait();
    TEST_CHECK(calls == 0);
    release.open();
    stopper.join();
    TEST_CHECK(calls == 0);
}
