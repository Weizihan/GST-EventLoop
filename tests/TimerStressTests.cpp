#include "TestSupport.h"

#include "EventLoop.h"
#include "EventLoopTestAccess.h"

#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <vector>

using namespace GST::BASE;
using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;

EventLoopConfig stress_workers(std::size_t count) {
    EventLoopConfig config;
    config.callback_thread_num = count;
    return config;
}

struct RetirementProbe {
    explicit RetirementProbe(std::atomic_size_t& counter)
        : destroyed(counter) {
    }

    ~RetirementProbe() {
        ++destroyed;
    }
    std::atomic_size_t& destroyed;
};

double elapsed_ms(Clock::time_point begin) {
    return std::chrono::duration<double, std::milli>(Clock::now() - begin).count();
}

void check_retired(const std::vector<TimerRegistration>& timers,
                   const std::vector<std::weak_ptr<RetirementProbe>>& callbacks,
                   const std::vector<std::weak_ptr<RetirementProbe>>& completions) {
    for (const auto& timer : timers) {
        TEST_CHECK(!timer.valid());
    }
    for (const auto& callback : callbacks) {
        TEST_CHECK(callback.expired());
    }
    for (const auto& completion : completions) {
        TEST_CHECK(completion.expired());
    }
}

}

TEST_CASE(timer_stress_bulk_cancel_reclaims_all_pending_states_and_captures) {
    constexpr std::size_t timer_count = 4096;
    EventLoop loop;
    TEST_CHECK(loop.init(stress_workers(4)));
    const auto engine = EventLoopTestAccess::timer_engine(loop);
    TEST_CHECK(loop.start());
    std::atomic_size_t calls{0};
    std::atomic_size_t done{0};
    std::atomic_size_t callback_destroyed{0};
    std::atomic_size_t completion_destroyed{0};
    std::vector<TimerRegistration> timers;
    std::vector<std::weak_ptr<RetirementProbe>> callbacks;
    std::vector<std::weak_ptr<RetirementProbe>> completions;
    timers.reserve(timer_count);
    callbacks.reserve(timer_count);
    completions.reserve(timer_count);
    const auto begin = Clock::now();
    for (std::size_t index = 0; index != timer_count; ++index) {
        auto callback = std::make_shared<RetirementProbe>(callback_destroyed);
        auto completion = std::make_shared<RetirementProbe>(completion_destroyed);
        const std::weak_ptr<RetirementProbe> callback_observer(callback);
        callbacks.push_back(callback);
        completions.push_back(completion);
        timers.push_back(loop.add_timer(TimerSchedule::after(24h),
            [&, callback](const TimerContext&) {
                ++calls;
            },
            [&, callback_observer, completion] {
                TEST_CHECK(callback_observer.expired());
                ++done;
            }));
        TEST_CHECK(timers.back().valid());
    }
    const double register_ms = elapsed_ms(begin);
    TEST_CHECK(EventLoopTestAccess::pending_timers(*engine) == timer_count);
    const auto cancel_begin = Clock::now();
    for (const auto& timer : timers) {
        TEST_CHECK(timer.cancel());
    }
    const double cancel_ms = elapsed_ms(cancel_begin);
    TEST_CHECK(EventLoopTestAccess::pending_timers(*engine) == 0);
    test::finish(loop);
    TEST_CHECK(calls == 0 && done == timer_count);
    TEST_CHECK(callback_destroyed == timer_count && completion_destroyed == timer_count);
    check_retired(timers, callbacks, completions);
    std::printf("[timer-stress] bulk_cancel timers=%zu registered_ms=%.3f canceled_ms=%.3f "
                "calls=%zu done=%zu callback_retired=%zu done_capture_retired=%zu pending=0\n",
                timer_count, register_ms, cancel_ms, calls.load(), done.load(),
                callback_destroyed.load(), completion_destroyed.load());
}

TEST_CASE(timer_stress_simultaneous_deadlines_execute_once_and_reclaim) {
    constexpr std::size_t timer_count = 4096;
    EventLoop loop;
    TEST_CHECK(loop.init(stress_workers(4)));
    const auto engine = EventLoopTestAccess::timer_engine(loop);
    test::Gate all_done;
    std::atomic_size_t calls{0};
    std::atomic_size_t done{0};
    std::atomic_size_t posts{0};
    std::vector<std::atomic_size_t> seen(timer_count);
    for (auto& count : seen) {
        count.store(0);
    }
    std::vector<TimerRegistration> timers;
    timers.reserve(timer_count);
    const auto begin = Clock::now();
    const auto deadline = begin + 100ms;
    for (std::size_t index = 0; index != timer_count; ++index) {
        timers.push_back(loop.add_timer(TimerSchedule::at(deadline),
            [&, index](const TimerContext& context) {
                TEST_CHECK(context.deadline == deadline && Clock::now() >= deadline);
                TEST_CHECK(++seen[index] == 1);
                ++calls;
            }, [&, index] {
                TEST_CHECK(seen[index] == 1);
                if (++done == timer_count) {
                    all_done.open();
                }
            }));
        TEST_CHECK(timers.back().valid());
    }
    EventLoopTestAccess::timer_hooks(*engine, {}, [&] {
        ++posts;
    });
    TEST_CHECK(loop.start());
    all_done.wait();
    const double finish_ms = elapsed_ms(begin);
    test::finish(loop);
    TEST_CHECK(calls == timer_count && done == timer_count && posts == timer_count);
    TEST_CHECK(EventLoopTestAccess::pending_timers(*engine) == 0);
    for (const auto& timer : timers) {
        TEST_CHECK(!timer.valid());
    }
    for (const auto& count : seen) {
        TEST_CHECK(count == 1);
    }
    std::printf("[timer-stress] simultaneous timers=%zu shared_deadline_ms=100 workers=4 "
                "posts=%zu calls=%zu done=%zu total_ms=%.3f pending=0\n",
                timer_count, posts.load(), calls.load(), done.load(), finish_ms);
}

TEST_CASE(timer_stress_high_frequency_is_bounded_and_soft_drains_all_posts) {
    constexpr std::size_t timer_count = 64;
    constexpr std::size_t post_limit = 4096;
    const auto interval = 1ms;
    EventLoop loop;
    TEST_CHECK(loop.init(stress_workers(4)));
    const auto engine = EventLoopTestAccess::timer_engine(loop);
    test::Gate limit_reached;
    test::Gate release_scheduler;
    std::atomic_size_t posts{0};
    std::atomic_size_t calls{0};
    std::atomic_size_t done{0};
    std::vector<Clock::time_point> first_deadlines(timer_count);
    std::vector<std::atomic_size_t> per_timer(timer_count);
    for (auto& count : per_timer) {
        count.store(0);
    }
    std::vector<TimerRegistration> timers;
    timers.reserve(timer_count);
    for (std::size_t index = 0; index != timer_count; ++index) {
        timers.push_back(loop.add_timer(TimerSchedule::every(interval),
            [&, index](const TimerContext& context) {
                TEST_CHECK(Clock::now() >= context.deadline);
                TEST_CHECK(context.deadline >= first_deadlines[index]);
                TEST_CHECK((context.deadline - first_deadlines[index]) % interval ==
                           Clock::duration::zero());
                ++per_timer[index];
                ++calls;
            }, [&] {
                ++done;
            }));
        TEST_CHECK(timers.back().valid());
        first_deadlines[index] = EventLoopTestAccess::timer_deadline(timers.back());
    }
    // 达到固定接收次数后暂停调度，主线程提交 Soft，再允许 owner 收尾。
    EventLoopTestAccess::timer_hooks(*engine, {}, [&] {
        if (++posts == post_limit) {
            limit_reached.open();
            release_scheduler.wait();
        }
    });
    const auto begin = Clock::now();
    TEST_CHECK(loop.start());
    limit_reached.wait();
    const double post_ms = elapsed_ms(begin);
    loop.stop(ShutdownMode::Soft);
    release_scheduler.open();
    TEST_CHECK(loop.run());
    TEST_CHECK(posts == post_limit && calls == post_limit && done == timer_count);
    TEST_CHECK(EventLoopTestAccess::pending_timers(*engine) == 0);
    std::size_t minimum = post_limit;
    std::size_t maximum = 0;
    for (const auto& count : per_timer) {
        const auto value = count.load();
        TEST_CHECK(value != 0);
        if (value < minimum) {
            minimum = value;
        }
        if (value > maximum) {
            maximum = value;
        }
    }
    for (const auto& timer : timers) {
        TEST_CHECK(!timer.valid());
    }
    std::printf("[timer-stress] high_frequency timers=%zu interval_us=1000 workers=4 "
                "post_limit=%zu posts=%zu calls=%zu per_timer_min=%zu per_timer_max=%zu "
                "post_ms=%.3f total_ms=%.3f done=%zu pending=0\n",
                timer_count, post_limit, posts.load(), calls.load(), minimum, maximum,
                post_ms, elapsed_ms(begin), done.load());
}

TEST_CASE(timer_stress_busy_pool_cancellation_retires_bounded_queued_occurrences) {
    constexpr std::size_t timer_count = 32;
    constexpr std::size_t post_limit = 1024;
    EventLoop loop;
    TEST_CHECK(loop.init(stress_workers(1)));
    const auto engine = EventLoopTestAccess::timer_engine(loop);
    test::Gate worker_entered;
    test::Gate release_worker;
    test::Gate limit_reached;
    test::Gate release_scheduler;
    std::atomic_size_t posts{0};
    std::atomic_size_t calls{0};
    std::atomic_size_t done{0};
    std::atomic_size_t callback_destroyed{0};
    std::atomic_size_t completion_destroyed{0};
    std::vector<TimerRegistration> timers;
    std::vector<std::weak_ptr<RetirementProbe>> callbacks;
    std::vector<std::weak_ptr<RetirementProbe>> completions;
    timers.reserve(timer_count);
    callbacks.reserve(timer_count);
    completions.reserve(timer_count);
    EventLoopTestAccess::timer_hooks(*engine, {}, [&] {
        if (++posts == post_limit) {
            limit_reached.open();
            release_scheduler.wait();
        }
    });
    TEST_CHECK(loop.start());
    TEST_CHECK(EventLoopTestAccess::post_worker(loop, [&] {
        worker_entered.open();
        release_worker.wait();
    }));
    worker_entered.wait();
    const auto begin = Clock::now();
    for (std::size_t index = 0; index != timer_count; ++index) {
        auto callback = std::make_shared<RetirementProbe>(callback_destroyed);
        auto completion = std::make_shared<RetirementProbe>(completion_destroyed);
        const std::weak_ptr<RetirementProbe> callback_observer(callback);
        callbacks.push_back(callback);
        completions.push_back(completion);
        timers.push_back(loop.add_timer(TimerSchedule::every(1ms),
            [&, callback](const TimerContext&) {
                ++calls;
            },
            [&, callback_observer, completion] {
                TEST_CHECK(callback_observer.expired());
                ++done;
            }));
        TEST_CHECK(timers.back().valid());
    }
    limit_reached.wait();
    const double queue_ms = elapsed_ms(begin);
    TEST_CHECK(posts == post_limit && calls == 0);
    for (const auto& timer : timers) {
        TEST_CHECK(timer.cancel());
    }
    TEST_CHECK(EventLoopTestAccess::pending_timers(*engine) == 0);
    // worker 仍被普通任务占用；至少有注册必须等已入池任务退休才通知。
    TEST_CHECK(done < timer_count && callback_destroyed < timer_count);
    loop.stop(ShutdownMode::Soft);
    release_scheduler.open();
    release_worker.open();
    TEST_CHECK(loop.run());
    TEST_CHECK(posts == post_limit && calls == 0 && done == timer_count);
    TEST_CHECK(callback_destroyed == timer_count && completion_destroyed == timer_count);
    TEST_CHECK(EventLoopTestAccess::pending_timers(*engine) == 0);
    check_retired(timers, callbacks, completions);
    std::printf("[timer-stress] busy_pool timers=%zu interval_us=1000 workers=1 "
                "post_limit=%zu queued_at_cancel=%zu calls_before_cancel=0 calls_final=%zu "
                "queue_ms=%.3f total_ms=%.3f done=%zu callback_retired=%zu "
                "done_capture_retired=%zu pending=0\n",
                timer_count, post_limit, posts.load(), calls.load(), queue_ms,
                elapsed_ms(begin), done.load(), callback_destroyed.load(),
                completion_destroyed.load());
}
