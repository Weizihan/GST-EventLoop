#include "TestSupport.h"

#include "EventLoop.h"
#include "EventLoopTestAccess.h"

#include <sys/eventfd.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

using namespace GST::BASE;
using namespace std::chrono_literals;

static_assert(std::is_copy_constructible<TimerRegistration>::value,
              "TimerRegistration must be copy constructible");
static_assert(std::is_copy_assignable<TimerRegistration>::value,
              "TimerRegistration must be copy assignable");
static_assert(std::is_nothrow_move_constructible<TimerRegistration>::value,
              "TimerRegistration must be nothrow move constructible");
static_assert(std::is_nothrow_move_assignable<TimerRegistration>::value,
              "TimerRegistration must be nothrow move assignable");
static_assert(std::is_same<decltype(TimerContext{}.self), TimerRegistration>::value,
              "Callback and caller must use the same registration type");
static_assert(std::is_same<decltype(TimerContext{}.deadline),
                           std::chrono::steady_clock::time_point>::value,
              "The public deadline must use the standard steady clock");

namespace {

struct DestructionAction {
    std::function<void()> action;
    ~DestructionAction() {
        if (action) {
            action();
        }
    }
};

EventLoopConfig single_worker() {
    EventLoopConfig config;
    config.callback_thread_num = 1;
    return config;
}

void queued_timer_stop(ShutdownMode mode, bool should_run) {
    EventLoop loop;
    TEST_CHECK(loop.init(single_worker()));
    test::Gate occupied;
    test::Gate release;
    test::Gate accepted;
    test::Gate cutoff;
    test::Gate done;
    std::atomic_int calls{0};
    auto timer = loop.add_timer(TimerSchedule::after(0ns),
        [&](const TimerContext&) {
            ++calls;
        }, [&] {
            done.open();
        });
    TEST_CHECK(timer.valid());
    EventLoopTestAccess::timer_hooks(*EventLoopTestAccess::timer_engine(loop),
        [&] {
            occupied.wait();
        }, [&] {
            accepted.open();
        });
    EventLoopTestAccess::stop_hook(loop, [&] {
        cutoff.open();
    });
    TEST_CHECK(loop.start());
    // 投递前钩子让普通任务先占住唯一 worker；Timer 随后只能排队。
    TEST_CHECK(EventLoopTestAccess::post_worker(loop, [&] {
        occupied.open();
        release.wait();
    }));
    accepted.wait();
    std::thread stopper([&] {
        test::finish(loop, mode);
    });
    cutoff.wait();
    TEST_CHECK(!timer.cancel());
    TEST_CHECK(calls == 0);
    if (mode == ShutdownMode::Hard) {
        // Hard 必须清掉队列捕获，不依赖当前占用 worker 的任务先结束。
        done.wait();
    } else {
        TEST_CHECK(!done.is_open());
    }
    release.open();
    stopper.join();
    done.wait();
    TEST_CHECK(calls == (should_run ? 1 : 0));
    TEST_CHECK(!timer.valid());
}

void hard_stop_at_qualification(bool qualified) {
    EventLoop loop;
    TEST_CHECK(loop.init(single_worker()));
    test::Gate reached;
    test::Gate release;
    test::Gate done;
    std::atomic_int calls{0};
    auto timer = loop.add_timer(TimerSchedule::after(0ns),
        [&](const TimerContext&) {
            ++calls;
        }, [&] {
            done.open();
        });
    auto block = [&] {
        reached.open();
        release.wait();
    };
    EventLoopTestAccess::timer_hooks(*EventLoopTestAccess::timer_engine(loop),
        {}, {}, qualified ? std::function<void()>{} : block,
        qualified ? block : std::function<void()>{});
    TEST_CHECK(loop.start());
    reached.wait();
    loop.stop(ShutdownMode::Hard);
    TEST_CHECK(EventLoopTestAccess::hard_stopping(loop));
    TEST_CHECK(!done.is_open());
    release.open();
    TEST_CHECK(loop.run());
    done.wait();
    TEST_CHECK(calls == (qualified ? 1 : 0));
    TEST_CHECK(!timer.valid());
}

void cancel_at_qualification(bool qualified) {
    EventLoop loop;
    TEST_CHECK(loop.init(single_worker()));
    test::Gate reached;
    test::Gate release;
    test::Gate done;
    std::atomic_int calls{0};
    auto timer = loop.add_timer(TimerSchedule::after(0ns),
        [&](const TimerContext&) {
            ++calls;
        }, [&] {
            done.open();
        });
    auto block = [&] {
        reached.open();
        release.wait();
    };
    EventLoopTestAccess::timer_hooks(*EventLoopTestAccess::timer_engine(loop),
        {}, {}, qualified ? std::function<void()>{} : block,
        qualified ? block : std::function<void()>{});
    TEST_CHECK(loop.start());
    reached.wait();
    TEST_CHECK(timer.cancel());
    TEST_CHECK(!timer.valid());
    TEST_CHECK(!done.is_open());
    release.open();
    done.wait();
    test::finish(loop);
    TEST_CHECK(calls == (qualified ? 1 : 0));
}

}

TEST_CASE(timer_schedule_and_invalid_registration) {
    TEST_CHECK(!TimerSchedule{}.valid());
    TEST_CHECK(TimerSchedule::after(0ns).valid());
    TEST_CHECK(TimerSchedule::after(1ns).valid());
    TEST_CHECK(!TimerSchedule::after(-1ns).valid());
    TEST_CHECK(TimerSchedule::at(std::chrono::steady_clock::time_point{}).valid());
    TimerRegistration empty;
    TEST_CHECK(!empty.valid() && !empty.cancel());
    EventLoop loop;
    TEST_CHECK(!loop.add_timer(TimerSchedule::after(0ns),
                              [](const TimerContext&) {}).valid());
    TEST_CHECK(loop.init());
    TEST_CHECK(!loop.add_timer(TimerSchedule{}, [](const TimerContext&) {}).valid());
    TEST_CHECK(!loop.add_timer(TimerSchedule::after(0ns), {}).valid());
    TEST_CHECK(!loop.remove(empty));
    test::finish(loop);
}

TEST_CASE(timer_after_executes_once_on_shared_worker_without_early_fire) {
    EventLoop loop;
    TEST_CHECK(loop.init(single_worker()));
    test::Gate done;
    std::atomic_int calls{0};
    const auto caller = std::this_thread::get_id();
    const auto schedule = TimerSchedule::after(20ms);
    const auto before = std::chrono::steady_clock::now();
    auto timer = loop.add_timer(schedule,
        [&](const TimerContext& context) {
            TEST_CHECK(std::this_thread::get_id() != caller);
            TEST_CHECK(context.deadline >= before + 20ms);
            TEST_CHECK(std::chrono::steady_clock::now() >= context.deadline);
            TEST_CHECK(context.self.valid());
            ++calls;
        }, [&] {
            done.open();
        });
    TEST_CHECK(timer.valid());
    TEST_CHECK(loop.start());
    done.wait();
    TEST_CHECK(calls == 1 && !timer.valid());
    test::finish(loop);
    TEST_CHECK(calls == 1);
}

TEST_CASE(timer_past_deadline_is_dispatched_once_with_original_deadline) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate done;
    std::atomic_int calls{0};
    const auto deadline = std::chrono::steady_clock::now() - 1s;
    auto timer = loop.add_timer(TimerSchedule::at(deadline),
        [&](const TimerContext& context) {
            TEST_CHECK(context.deadline == deadline);
            ++calls;
        }, [&] {
            done.open();
        });
    TEST_CHECK(loop.start());
    done.wait();
    test::finish(loop);
    TEST_CHECK(calls == 1 && !timer.valid());
}

TEST_CASE(timer_deadline_order_uses_one_shared_worker) {
    EventLoop loop;
    TEST_CHECK(loop.init(single_worker()));
    test::Gate done;
    std::mutex mutex;
    std::vector<int> order;
    std::atomic_int completions{0};
    const auto base = std::chrono::steady_clock::now() + 20ms;
    for (int index : {3, 1, 2}) {
        const auto deadline = base + index * 10ms;
        TEST_CHECK(loop.add_timer(TimerSchedule::at(deadline),
            [&, index, deadline](const TimerContext& context) {
                TEST_CHECK(context.deadline == deadline);
                TEST_CHECK(std::chrono::steady_clock::now() >= deadline);
                std::lock_guard<std::mutex> lock(mutex);
                order.push_back(index);
            }, [&] {
                if (++completions == 3) {
                    done.open();
                }
            }).valid());
    }
    TEST_CHECK(loop.start());
    done.wait();
    test::finish(loop);
    TEST_CHECK((order == std::vector<int>{1, 2, 3}));
}

TEST_CASE(timer_new_earlier_deadline_wakes_sleeping_scheduler) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate done;
    std::atomic_int far_calls{0};
    auto far = loop.add_timer(TimerSchedule::after(24h),
        [&](const TimerContext&) {
            ++far_calls;
        });
    test::Gate waiting;
    EventLoopTestAccess::timer_wait_hook(*EventLoopTestAccess::timer_engine(loop),
        [&] {
            waiting.open();
        });
    TEST_CHECK(loop.start());
    waiting.wait();
    auto earlier = loop.add_timer(TimerSchedule::after(0ns),
        [](const TimerContext&) {
        }, [&] {
            done.open();
        });
    TEST_CHECK(earlier.registration_id() != 0);
    done.wait();
    TEST_CHECK(far.cancel());
    test::finish(loop);
    TEST_CHECK(far_calls == 0 && !earlier.valid());
}

TEST_CASE(timer_cancel_nearest_recalculates_pending_deadline) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate done;
    std::atomic_int canceled_calls{0};
    auto nearest = loop.add_timer(TimerSchedule::after(1h),
        [&](const TimerContext&) {
            ++canceled_calls;
        });
    auto farther = loop.add_timer(TimerSchedule::after(2h),
        [&](const TimerContext&) {
            ++canceled_calls;
        });
    test::Gate waiting;
    EventLoopTestAccess::timer_wait_hook(*EventLoopTestAccess::timer_engine(loop),
        [&] {
            waiting.open();
        });
    TEST_CHECK(loop.start());
    waiting.wait();
    TEST_CHECK(nearest.cancel());
    TEST_CHECK(EventLoopTestAccess::pending_timers(
        *EventLoopTestAccess::timer_engine(loop)) == 1);
    auto earlier = loop.add_timer(TimerSchedule::after(0ns),
        [](const TimerContext&) {
        }, [&] {
            done.open();
        });
    TEST_CHECK(earlier.registration_id() != 0);
    done.wait();
    TEST_CHECK(farther.cancel());
    test::finish(loop);
    TEST_CHECK(canceled_calls == 0);
}

TEST_CASE(timer_copy_move_and_weak_lifetime_do_not_cancel) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate done;
    auto original = loop.add_timer(TimerSchedule::after(24h),
        [](const TimerContext&) {
        }, [&] {
            done.open();
        });
    const auto copy = original;
    const auto generation = original.generation();
    const auto id = original.registration_id();
    TimerRegistration moved(std::move(original));
    TEST_CHECK(!original.valid() && moved.valid());
    TimerRegistration assigned;
    assigned = std::move(moved);
    TEST_CHECK(!moved.valid() && assigned.valid());
    TimerRegistration swapped;
    swap(assigned, swapped);
    TEST_CHECK(!assigned.valid() && swapped.valid());
    TEST_CHECK(swapped.generation() == generation && swapped.registration_id() == id);
    {
        const auto temporary = swapped;
        TEST_CHECK(temporary.valid());
    }
    swapped = {};
    TEST_CHECK(copy.valid() && !done.is_open());
    TEST_CHECK(loop.remove(copy));
    done.wait();
    TEST_CHECK(!copy.valid());
    test::finish(loop);
}

TEST_CASE(timer_cross_thread_cancel_before_start_does_not_execute) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate done;
    std::atomic_int calls{0};
    auto timer = loop.add_timer(TimerSchedule::after(0ns),
        [&](const TimerContext&) {
            ++calls;
        }, [&] {
            done.open();
        });
    std::thread canceler([&] {
        TEST_CHECK(timer.cancel());
    });
    canceler.join();
    done.wait();
    TEST_CHECK(!timer.valid() && !timer.cancel());
    TEST_CHECK(loop.start());
    test::finish(loop);
    TEST_CHECK(calls == 0);
}

TEST_CASE(timer_concurrent_cancel_has_one_winner_and_one_completion) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    std::atomic_int calls{0};
    std::atomic_int completions{0};
    std::atomic_int successes{0};
    auto timer = loop.add_timer(TimerSchedule::after(24h),
        [&](const TimerContext&) {
            ++calls;
        }, [&] {
            ++completions;
        });
    TEST_CHECK(loop.start());
    test::Gate begin;
    std::vector<std::thread> threads;
    for (int i = 0; i != 12; ++i) {
        const auto copy = timer;
        threads.emplace_back([&, copy] {
            begin.wait();
            if (copy.cancel()) {
                ++successes;
            }
        });
    }
    begin.open();
    for (auto& thread : threads) {
        thread.join();
    }
    test::finish(loop);
    TEST_CHECK(successes == 1 && calls == 0 && completions == 1);
    TEST_CHECK(!timer.valid());
}

TEST_CASE(timer_cancel_selected_before_post_prevents_callback) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate selected;
    test::Gate release;
    test::Gate done;
    std::atomic_int calls{0};
    auto timer = loop.add_timer(TimerSchedule::after(0ns),
        [&](const TimerContext&) {
            ++calls;
        }, [&] {
            done.open();
        });
    EventLoopTestAccess::timer_hooks(*EventLoopTestAccess::timer_engine(loop),
        [&] {
            selected.open();
            release.wait();
        }, {});
    TEST_CHECK(loop.start());
    selected.wait();
    TEST_CHECK(timer.cancel());
    release.open();
    done.wait();
    test::finish(loop);
    TEST_CHECK(calls == 0 && !timer.valid());
}

TEST_CASE(timer_cancel_popped_task_before_qualification_prevents_callback) {
    cancel_at_qualification(false);
}

TEST_CASE(timer_cancel_after_qualification_preserves_current_callback) {
    cancel_at_qualification(true);
}

TEST_CASE(timer_cancel_running_callback_does_not_interrupt_or_complete_early) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate entered;
    test::Gate release;
    test::Gate done;
    std::atomic_int calls{0};
    auto timer = loop.add_timer(TimerSchedule::after(0ns),
        [&](const TimerContext&) {
            entered.open();
            release.wait();
            ++calls;
        },
        [&] {
            done.open();
        });
    TEST_CHECK(loop.start());
    entered.wait();
    TEST_CHECK(timer.cancel());
    TEST_CHECK(!timer.valid() && !done.is_open() && calls == 0);
    release.open();
    done.wait();
    test::finish(loop);
    TEST_CHECK(calls == 1);
}

TEST_CASE(timer_callback_self_cancel_is_nonblocking) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate done;
    std::atomic_int calls{0};
    auto timer = loop.add_timer(TimerSchedule::after(0ns),
        [&](const TimerContext& context) {
            TEST_CHECK(context.self.cancel());
            TEST_CHECK(!context.self.valid());
            TEST_CHECK(!context.self.cancel());
            TEST_CHECK(!done.is_open());
            ++calls;
        }, [&] {
            done.open();
        });
    TEST_CHECK(loop.start());
    done.wait();
    test::finish(loop);
    TEST_CHECK(calls == 1 && !timer.valid());
}

TEST_CASE(timer_done_waits_for_capture_destruction) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate destroying;
    test::Gate release;
    test::Gate done;
    auto capture = std::make_shared<DestructionAction>();
    capture->action = [&] {
        destroying.open();
        release.wait();
        TEST_CHECK(!done.is_open());
    };
    auto timer = loop.add_timer(TimerSchedule::after(0ns),
        [capture](const TimerContext&) {
        }, [&] {
            done.open();
        });
    capture.reset();
    TEST_CHECK(loop.start());
    destroying.wait();
    TEST_CHECK(!done.is_open());
    release.open();
    done.wait();
    test::finish(loop);
    TEST_CHECK(!timer.valid());
}

TEST_CASE(timer_cancel_releases_capture_outside_internal_locks) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate destroyed;
    test::Gate done;
    auto capture = std::make_shared<DestructionAction>();
    capture->action = [&] {
        // 捕获析构重入注册和取消，必须不持调度锁或 Loop 状态锁。
        auto nested = loop.add_timer(TimerSchedule::after(24h),
                                      [](const TimerContext&) {});
        TEST_CHECK(nested.valid());
        TEST_CHECK(nested.cancel());
        destroyed.open();
    };
    auto timer = loop.add_timer(TimerSchedule::after(24h),
        [capture](const TimerContext&) {}, [&] {
            TEST_CHECK(destroyed.is_open());
            done.open();
        });
    capture.reset();
    TEST_CHECK(loop.start());
    TEST_CHECK(timer.cancel());
    done.wait();
    test::finish(loop);
}

TEST_CASE(timer_soft_stop_drains_successfully_queued_task) {
    queued_timer_stop(ShutdownMode::Soft, true);
}

TEST_CASE(timer_hard_stop_discards_successfully_queued_unqualified_task) {
    queued_timer_stop(ShutdownMode::Hard, false);
}

TEST_CASE(timer_soft_stop_before_post_closes_dispatch_qualification) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate selected;
    test::Gate release;
    test::Gate done;
    std::atomic_int posts{0};
    std::atomic_int calls{0};
    auto timer = loop.add_timer(TimerSchedule::after(0ns),
        [&](const TimerContext&) {
            ++calls;
        }, [&] {
            done.open();
        });
    EventLoopTestAccess::timer_hooks(*EventLoopTestAccess::timer_engine(loop),
        [&] {
            selected.open();
            release.wait();
        }, [&] {
            ++posts;
        });
    TEST_CHECK(loop.start());
    selected.wait();
    loop.stop(ShutdownMode::Soft);
    TEST_CHECK(!timer.cancel());
    release.open();
    TEST_CHECK(loop.run());
    done.wait();
    TEST_CHECK(posts == 0 && calls == 0 && !timer.valid());
}

TEST_CASE(timer_hard_stop_after_pop_before_qualification_prevents_callback) {
    hard_stop_at_qualification(false);
}

TEST_CASE(timer_scheduler_retirement_done_can_request_stop_without_self_wait) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate selected;
    test::Gate release;
    test::Gate done;
    std::atomic_int calls{0};
    auto timer = loop.add_timer(TimerSchedule::after(0ns),
        [&](const TimerContext&) {
            ++calls;
        }, [&] {
            TEST_CHECK(!loop.init());
            TEST_CHECK(!loop.start());
            TEST_CHECK(!loop.run());
            loop.stop(ShutdownMode::Hard);
            done.open();
        });
    EventLoopTestAccess::timer_hooks(*EventLoopTestAccess::timer_engine(loop),
        [&] {
            selected.open();
            release.wait();
        }, {});
    TEST_CHECK(loop.start());
    selected.wait();
    // 截止前被调度线程持有的批次，在停止后由该路径释放最后引用。
    loop.stop();
    release.open();
    done.wait();
    TEST_CHECK(loop.run());
    TEST_CHECK(calls == 0 && !timer.valid());
}

TEST_CASE(timer_hard_stop_after_qualification_allows_callback_to_finish) {
    hard_stop_at_qualification(true);
}

TEST_CASE(timer_stop_wakes_empty_scheduler) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate waiting;
    EventLoopTestAccess::timer_wait_hook(*EventLoopTestAccess::timer_engine(loop),
        [&] {
            waiting.open();
        });
    TEST_CHECK(loop.start());
    waiting.wait();
    test::finish(loop);
    TEST_CHECK(loop.run());
}

TEST_CASE(timer_stop_wakes_far_future_scheduler_and_rejects_cancel) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    std::atomic_int calls{0};
    std::atomic_int done{0};
    auto timer = loop.add_timer(TimerSchedule::after(std::chrono::nanoseconds::max()),
        [&](const TimerContext&) {
            ++calls;
        }, [&] {
            ++done;
        });
    TEST_CHECK(EventLoopTestAccess::timer_deadline(timer) ==
               std::chrono::steady_clock::time_point::max());
    test::Gate waiting;
    EventLoopTestAccess::timer_wait_hook(*EventLoopTestAccess::timer_engine(loop),
        [&] {
            waiting.open();
        });
    TEST_CHECK(loop.start());
    waiting.wait();
    loop.stop();
    TEST_CHECK(!timer.cancel() && !loop.remove(timer));
    TEST_CHECK(!loop.add_timer(TimerSchedule::after(0ns),
                              [](const TimerContext&) {}).valid());
    TEST_CHECK(loop.run());
    TEST_CHECK(calls == 0 && done == 1 && !timer.valid());
}

TEST_CASE(timer_callback_exceptions_preserve_later_tasks_and_completion) {
    EventLoop loop;
    TEST_CHECK(loop.init(single_worker()));
    test::Gate done;
    std::atomic_int calls{0};
    std::atomic_int completions{0};
    const auto deadline = std::chrono::steady_clock::now();
    auto completion = [&] {
        if (++completions == 3) {
            done.open();
        }
    };
    auto standard = loop.add_timer(TimerSchedule::at(deadline),
        [&](const TimerContext&) {
            ++calls;
            throw std::runtime_error("expected timer");
        },
        completion);
    auto other = loop.add_timer(TimerSchedule::at(deadline),
        [&](const TimerContext&) {
            ++calls;
            throw 7;
        }, completion);
    auto healthy = loop.add_timer(TimerSchedule::at(deadline),
        [&](const TimerContext&) {
            ++calls;
        }, completion);
    TEST_CHECK(standard.valid() && other.valid() && healthy.valid());
    TEST_CHECK(loop.start());
    done.wait();
    test::finish(loop);
    TEST_CHECK(calls == 3 && completions == 3);
}

TEST_CASE(timer_done_exception_does_not_block_next_completion) {
    EventLoop loop;
    TEST_CHECK(loop.init(single_worker()));
    test::Gate done;
    std::atomic_int calls{0};
    TEST_CHECK(loop.add_timer(TimerSchedule::after(0ns),
        [&](const TimerContext&) {
            ++calls;
        },
        [] {
            throw std::runtime_error("expected timer done");
        }).valid());
    TEST_CHECK(loop.add_timer(TimerSchedule::after(0ns),
        [&](const TimerContext&) {
            ++calls;
        }, [&] {
            done.open();
        }).valid());
    TEST_CHECK(loop.start());
    done.wait();
    test::finish(loop);
    TEST_CHECK(calls == 2);
}

TEST_CASE(timer_stop_before_start_completes_once_and_retains_hard_mode) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    std::atomic_int calls{0};
    std::atomic_int done{0};
    auto timer = loop.add_timer(TimerSchedule::after(0ns),
        [&](const TimerContext&) {
            ++calls;
        }, [&] {
            ++done;
        });
    loop.stop(ShutdownMode::Hard);
    loop.stop(ShutdownMode::Soft);
    TEST_CHECK(EventLoopTestAccess::hard_stopping(loop));
    TEST_CHECK(!loop.start() && !timer.cancel());
    TEST_CHECK(loop.run());
    TEST_CHECK(loop.run());
    loop.stop();
    TEST_CHECK(calls == 0 && done == 1 && !timer.valid());
}

TEST_CASE(timer_destructor_without_start_releases_capture_and_weak_handle) {
    TimerRegistration old;
    std::atomic_int destroyed{0};
    std::atomic_int completions{0};
    {
        EventLoop loop;
        TEST_CHECK(loop.init());
        auto capture = std::make_shared<DestructionAction>();
        capture->action = [&] {
            ++destroyed;
        };
        old = loop.add_timer(TimerSchedule::after(24h),
            [capture](const TimerContext&) {}, [&] {
                TEST_CHECK(destroyed == 1);
                ++completions;
            });
        capture.reset();
        TEST_CHECK(old.valid());
    }
    TEST_CHECK(destroyed == 1 && completions == 1);
    TEST_CHECK(!old.valid() && !old.cancel());
    EventLoop next;
    TEST_CHECK(next.init());
    test::finish(next);
}

TEST_CASE(timer_reinitialization_does_not_revive_old_registration) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    std::atomic_int done{0};
    auto old = loop.add_timer(TimerSchedule::after(24h),
        [](const TimerContext&) {
        }, [&] {
            ++done;
        });
    const auto old_generation = old.generation();
    TEST_CHECK(loop.start());
    test::finish(loop);
    TEST_CHECK(done == 1 && !old.valid());
    TEST_CHECK(loop.init());
    auto current = loop.add_timer(TimerSchedule::after(24h),
        [](const TimerContext&) {
        }, [&] {
            ++done;
        });
    TEST_CHECK(current.valid() && current.generation() != old_generation);
    TEST_CHECK(!old.cancel() && !loop.remove(old));
    TEST_CHECK(current.valid());
    TEST_CHECK(loop.start());
    test::finish(loop, ShutdownMode::Hard);
    TEST_CHECK(done == 2 && !current.valid());
}

TEST_CASE(timer_callback_can_request_stop_but_rejects_lifecycle_wait) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate called;
    test::Gate done;
    auto timer = loop.add_timer(TimerSchedule::after(0ns),
        [&](const TimerContext&) {
            TEST_CHECK(!loop.init());
            TEST_CHECK(!loop.start());
            TEST_CHECK(!loop.run());
            loop.stop(ShutdownMode::Hard);
            called.open();
        }, [&] {
            done.open();
        });
    TEST_CHECK(loop.start());
    called.wait();
    TEST_CHECK(loop.run());
    done.wait();
    TEST_CHECK(!timer.valid());
}

TEST_CASE(timer_done_can_request_stop_but_rejects_lifecycle_wait) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate done;
    auto timer = loop.add_timer(TimerSchedule::after(0ns),
        [](const TimerContext&) {}, [&] {
            TEST_CHECK(!loop.init());
            TEST_CHECK(!loop.start());
            TEST_CHECK(!loop.run());
            loop.stop(ShutdownMode::Hard);
            done.open();
        });
    TEST_CHECK(loop.start());
    done.wait();
    TEST_CHECK(loop.run());
    TEST_CHECK(!timer.valid());
}

TEST_CASE(timer_capture_destruction_can_request_stop_without_self_wait) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate destroyed;
    test::Gate done;
    auto capture = std::make_shared<DestructionAction>();
    capture->action = [&] {
        TEST_CHECK(!loop.init());
        TEST_CHECK(!loop.start());
        TEST_CHECK(!loop.run());
        loop.stop(ShutdownMode::Hard);
        destroyed.open();
    };
    auto timer = loop.add_timer(TimerSchedule::after(0ns),
        [capture](const TimerContext&) {}, [&] {
            TEST_CHECK(destroyed.is_open());
            done.open();
        });
    capture.reset();
    TEST_CHECK(loop.start());
    done.wait();
    TEST_CHECK(loop.run());
    TEST_CHECK(!timer.valid());
}

TEST_CASE(timer_parallel_registration_cancel_and_repeated_stop_reclaim_all) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    TEST_CHECK(loop.start());
    test::Gate begin;
    std::atomic_int done{0};
    std::atomic_int calls{0};
    std::vector<std::thread> threads;
    for (int thread = 0; thread != 4; ++thread) {
        threads.emplace_back([&] {
            begin.wait();
            for (int i = 0; i != 20; ++i) {
                auto timer = loop.add_timer(TimerSchedule::after(24h),
                    [&](const TimerContext&) {
                        ++calls;
                    }, [&] {
                        ++done;
                    });
                TEST_CHECK(timer.valid());
                TEST_CHECK(timer.cancel());
            }
        });
    }
    begin.open();
    for (auto& thread : threads) {
        thread.join();
    }
    std::thread first([&] {
        loop.stop();
    });
    std::thread second([&] {
        loop.stop(ShutdownMode::Hard);
    });
    first.join();
    second.join();
    TEST_CHECK(loop.run());
    test::finish(loop);
    TEST_CHECK(calls == 0 && done == 80);
}

TEST_CASE(timer_failed_registration_does_not_complete_unaccepted_identity) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    std::atomic_int destroyed{0};
    std::atomic_int completions{0};
    auto capture = std::make_shared<DestructionAction>();
    capture->action = [&] {
        ++destroyed;
    };
    std::function<void(const TimerContext&)> callback =
        [capture](const TimerContext&) {};
    capture.reset();
    TimerRegistration failed;
    test::set_allocation_failure(0);
    try {
        failed = loop.add_timer(TimerSchedule::after(24h), std::move(callback),
                                [&] {
                                    ++completions;
                                });
    } catch (const std::bad_alloc&) {
        // 与现有管理操作一致，允许分配异常交给调用者处理。
    } catch (...) {
        test::clear_allocation_failure();
        throw;
    }
    test::clear_allocation_failure();
    callback = {};
    TEST_CHECK(!failed.valid() && destroyed == 1 && completions == 0);
    auto next = loop.add_timer(TimerSchedule::after(24h),
        [](const TimerContext&) {
        }, [&] {
            ++completions;
        });
    TEST_CHECK(next.valid() && next.cancel());
    test::finish(loop);
    TEST_CHECK(completions == 1);
}

TEST_CASE(timer_pending_cancel_does_not_require_allocation) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    std::atomic_int completions{0};
    auto timer = loop.add_timer(TimerSchedule::after(24h),
        [](const TimerContext&) {
        }, [&] {
            ++completions;
        });
    TEST_CHECK(loop.start());
    bool canceled = false;
    test::set_allocation_failure(0);
    try {
        canceled = timer.cancel();
    } catch (...) {
        test::clear_allocation_failure();
        throw;
    }
    test::clear_allocation_failure();
    TEST_CHECK(canceled && !timer.valid());
    test::finish(loop);
    TEST_CHECK(completions == 1);
}

TEST_CASE(timer_soft_stop_during_io_start_preserves_mode_and_no_dispatch) {
    NativeHandle source(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    TEST_CHECK(source.valid());
    EventLoop loop;
    TEST_CHECK(loop.init());
    std::atomic_int io_calls{0};
    std::atomic_int timer_calls{0};
    std::atomic_int timer_posts{0};
    std::atomic_int timer_done{0};
    std::atomic_int io_done{0};
    test::Gate stopped;
    auto capture = std::make_shared<DestructionAction>();
    capture->action = [&] {
        loop.stop(ShutdownMode::Soft);
        stopped.open();
    };
    auto io = loop.watch_io(source, Event_type::READ_EVENT,
        [&, capture](const IoContext&) {
            ++io_calls;
        }, [&] {
            ++io_done;
        });
    capture.reset();
    TEST_CHECK(loop.set_io_callback(io, Event_type::READ_EVENT,
                                    [&](const IoContext&) {
                                        ++io_calls;
                                    }));
    auto timer = loop.add_timer(TimerSchedule::after(0ns),
        [&](const TimerContext&) {
            ++timer_calls;
        }, [&] {
            ++timer_done;
        });
    TEST_CHECK(timer.valid());
    EventLoopTestAccess::timer_hooks(*EventLoopTestAccess::timer_engine(loop),
        {}, [&] {
            ++timer_posts;
        });
    // owner 在 startup 队列替换旧 IO 回调，其捕获析构先于 start 返回。
    TEST_CHECK(loop.start());
    TEST_CHECK(stopped.is_open());
    TEST_CHECK(!EventLoopTestAccess::hard_stopping(loop));
    TEST_CHECK(!timer.cancel());
    TEST_CHECK(loop.run());
    TEST_CHECK(io_calls == 0 && timer_calls == 0 && timer_posts == 0);
    TEST_CHECK(io_done == 1 && timer_done == 1);
    TEST_CHECK(!io.valid() && !timer.valid());
}

TEST_CASE(timer_post_batch_allocation_failure_retires_outside_loop_lock) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate done;
    std::atomic_int attempts{0};
    std::atomic_int accepted{0};
    std::atomic_int calls{0};
    std::atomic_int destroyed{0};
    std::atomic_int completions{0};
    auto capture = std::make_shared<DestructionAction>();
    capture->action = [&] {
        ++destroyed;
    };
    auto timer = loop.add_timer(TimerSchedule::after(0ns),
        [&, capture](const TimerContext&) {
            ++calls;
        }, [&] {
            test::clear_allocation_failure();
            TEST_CHECK(destroyed == 1);
            ++completions;
            // post_batch 的失败清理不得持 Loop 截止锁释放最后负载引用。
            loop.stop(ShutdownMode::Soft);
            done.open();
        });
    capture.reset();
    EventLoopTestAccess::timer_hooks(*EventLoopTestAccess::timer_engine(loop),
        [&] {
            ++attempts;
            // 此时 Task、vector 和 batch 已完成分配；下一次分配是池队列节点。
            test::set_allocation_failure(0);
        }, [&] {
            ++accepted;
        });
    TEST_CHECK(loop.start());
    done.wait();
    TEST_CHECK(loop.run());
    TEST_CHECK(attempts == 1 && accepted == 0 && calls == 0);
    TEST_CHECK(destroyed == 1 && completions == 1 && !timer.valid());
}
