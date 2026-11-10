#include "TestSupport.h"

#include "EventLoop.h"
#include "EventLoopTestAccess.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <vector>

using namespace GST::BASE;
using namespace std::chrono_literals;

namespace {

using Clock = std::chrono::steady_clock;

EventLoopConfig workers(std::size_t count) {
    EventLoopConfig config;
    config.callback_thread_num = count;
    return config;
}

struct DestructionAction {
    std::function<void()> action;
    ~DestructionAction() {
        if (action) {
            action();
        }
    }
};

// 只为故意延迟 scheduler 跨过已知计划截止时间；竞争顺序由 Gate 确定。
void wait_until(Clock::time_point deadline) {
    std::mutex mutex;
    std::condition_variable changed;
    std::unique_lock<std::mutex> lock(mutex);
    changed.wait_until(lock, deadline, [] {
        return false;
    });
}

void periodic_queued_stop(ShutdownMode mode, int expected_calls) {
    EventLoop loop;
    TEST_CHECK(loop.init(workers(1)));
    test::Gate entered;
    test::Gate release_worker;
    test::Gate three_accepted;
    test::Gate release_scheduler;
    test::Gate done;
    std::atomic_int posts{0};
    std::atomic_int calls{0};
    auto timer = loop.add_timer(TimerSchedule::every(5ms),
        [&](const TimerContext&) {
            if (++calls == 1) {
                entered.open();
                release_worker.wait();
            }
        }, [&] {
            done.open();
        });
    EventLoopTestAccess::timer_hooks(*EventLoopTestAccess::timer_engine(loop), {}, [&] {
        if (++posts == 3) {
            three_accepted.open();
            release_scheduler.wait();
        }
    });
    TEST_CHECK(loop.start());
    entered.wait();
    three_accepted.wait();
    loop.stop(mode);
    TEST_CHECK(!timer.cancel());
    TEST_CHECK(!done.is_open());
    release_scheduler.open();
    release_worker.open();
    TEST_CHECK(loop.run());
    done.wait();
    TEST_CHECK(posts == 3 && calls == expected_calls && !timer.valid());
}

void periodic_callback_exception(bool standard_exception) {
    EventLoop loop;
    TEST_CHECK(loop.init(workers(1)));
    test::Gate done;
    test::Gate healthy;
    std::atomic_int calls{0};
    std::atomic_int completions{0};
    auto timer = loop.add_timer(TimerSchedule::every(5ms),
        [&](const TimerContext&) {
            ++calls;
            if (standard_exception) {
                throw std::runtime_error("expected periodic callback");
            }
            throw 7;
        }, [&] {
            ++completions;
            done.open();
        });
    TEST_CHECK(loop.start());
    done.wait();
    TEST_CHECK(calls == 1 && completions == 1 && !timer.valid());
    TEST_CHECK(!timer.cancel());
    TEST_CHECK(!timer.update(TimerSchedule::every(5ms)));
    const auto next = loop.add_timer(TimerSchedule::after(0ns),
        [&](const TimerContext&) {
            healthy.open();
        });
    TEST_CHECK(next.registration_id() != 0);
    healthy.wait();
    test::finish(loop);
    TEST_CHECK(calls == 1 && completions == 1);
}

void periodic_exception_with_in_flight(bool stop_before_throw) {
    EventLoop loop;
    TEST_CHECK(loop.init(workers(2)));
    test::Gate throwing_entered;
    test::Gate other_entered;
    test::Gate release_throw;
    test::Gate release_other;
    test::Gate three_accepted;
    test::Gate release_scheduler;
    test::Gate queued_popped;
    test::Gate release_qualification;
    test::Gate queued_retired;
    test::Gate destroying;
    test::Gate release_capture;
    test::Gate done;
    std::atomic_int calls{0};
    std::atomic_int other_finished{0};
    std::atomic_int posts{0};
    std::atomic_int attempts{0};
    std::atomic_int completions{0};
    auto capture = std::make_shared<DestructionAction>();
    capture->action = [&] {
        TEST_CHECK(other_finished == 1);
        destroying.open();
        release_capture.wait();
        TEST_CHECK(!done.is_open());
    };
    auto timer = loop.add_timer(TimerSchedule::every(5ms),
        [&, capture](const TimerContext&) {
            const int call = ++calls;
            if (call == 1) {
                throwing_entered.open();
                release_throw.wait();
                throw std::runtime_error("expected overlapping periodic callback");
            }
            TEST_CHECK(call == 2);
            other_entered.open();
            release_other.wait();
            ++other_finished;
        }, [&] {
            TEST_CHECK(other_finished == 1);
            ++completions;
            done.open();
        });
    capture.reset();
    EventLoopTestAccess::timer_hooks(*EventLoopTestAccess::timer_engine(loop), {}, [&] {
        if (++posts == 3) {
            three_accepted.open();
            release_scheduler.wait();
        }
    }, [&] {
        if (++attempts == 3) {
            queued_popped.open();
            release_qualification.wait();
        }
    });
    TEST_CHECK(loop.start());
    throwing_entered.wait();
    other_entered.wait();
    three_accepted.wait();
    if (stop_before_throw) {
        loop.stop(ShutdownMode::Soft);
        TEST_CHECK(!EventLoopTestAccess::hard_stopping(loop));
        TEST_CHECK(!timer.cancel());
        TEST_CHECK(!timer.update(TimerSchedule::every(1ms)));
        TEST_CHECK(!timer.set_cb([](const TimerContext&) {}));
    }
    release_throw.open();
    // 第一份任务的 catch 和清理先完成，唯一空闲 worker 才能取出第三份任务。
    queued_popped.wait();
    TEST_CHECK(!timer.valid() && !timer.cancel());
    TEST_CHECK(calls == 2 && other_finished == 0 && !done.is_open());
    TEST_CHECK(EventLoopTestAccess::post_worker(loop, [&] {
        queued_retired.open();
    }));
    release_qualification.open();
    queued_retired.wait();
    TEST_CHECK(calls == 2 && !done.is_open());
    release_scheduler.open();
    release_other.open();
    destroying.wait();
    TEST_CHECK(other_finished == 1 && !done.is_open());
    release_capture.open();
    done.wait();
    test::finish(loop);
    TEST_CHECK(posts == 3 && attempts == 3 && calls == 2 && completions == 1);
    TEST_CHECK(!timer.valid());
}

}

TEST_CASE(timer_periodic_schedule_requires_positive_interval) {
    TEST_CHECK(!TimerSchedule::every(0ns).valid());
    TEST_CHECK(!TimerSchedule::every(-1ns).valid());
    TEST_CHECK(TimerSchedule::every(1ns).valid());
    EventLoop loop;
    TEST_CHECK(loop.init());
    TEST_CHECK(!loop.add_timer(TimerSchedule::every(0ns),
                              [](const TimerContext&) {}).valid());
    test::finish(loop);
}

TEST_CASE(timer_periodic_first_deadline_is_sampled_at_registration) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate done;
    const auto schedule = TimerSchedule::every(20ms);
    const auto before = Clock::now();
    auto timer = loop.add_timer(schedule, [&](const TimerContext& context) {
        TEST_CHECK(context.deadline >= before + 20ms);
        TEST_CHECK(Clock::now() >= context.deadline);
        TEST_CHECK(context.self.cancel());
    }, [&] {
        done.open();
    });
    TEST_CHECK(loop.start());
    done.wait();
    test::finish(loop);
    TEST_CHECK(!timer.valid());
}

TEST_CASE(timer_periodic_deadlines_preserve_fixed_frequency_phase) {
    EventLoop loop;
    TEST_CHECK(loop.init(workers(1)));
    test::Gate done;
    std::vector<Clock::time_point> deadlines;
    const auto interval = 10ms;
    auto timer = loop.add_timer(TimerSchedule::every(interval),
        [&](const TimerContext& context) {
            TEST_CHECK(Clock::now() >= context.deadline);
            deadlines.push_back(context.deadline);
            if (deadlines.size() == 4) {
                TEST_CHECK(context.self.cancel());
            }
        }, [&] {
            done.open();
        });
    const auto first = EventLoopTestAccess::timer_deadline(timer);
    TEST_CHECK(loop.start());
    done.wait();
    test::finish(loop);
    TEST_CHECK(deadlines.size() == 4 && deadlines.front() == first);
    for (std::size_t i = 1; i != deadlines.size(); ++i) {
        const auto difference = deadlines[i] - deadlines[i - 1];
        TEST_CHECK(difference >= interval);
        TEST_CHECK(difference % interval == Clock::duration::zero());
    }
}

TEST_CASE(timer_periodic_long_callbacks_overlap_on_shared_workers) {
    EventLoop loop;
    TEST_CHECK(loop.init(workers(2)));
    test::Gate both_entered;
    test::Gate release;
    test::Gate done;
    std::atomic_int entered{0};
    std::atomic_int finished{0};
    auto timer = loop.add_timer(TimerSchedule::every(5ms),
        [&](const TimerContext&) {
            if (++entered == 2) {
                both_entered.open();
            }
            release.wait();
            ++finished;
        }, [&] {
            done.open();
        });
    TEST_CHECK(loop.start());
    both_entered.wait();
    TEST_CHECK(entered == 2 && finished == 0);
    TEST_CHECK(timer.cancel());
    TEST_CHECK(!done.is_open());
    release.open();
    done.wait();
    test::finish(loop);
    TEST_CHECK(entered == 2 && finished == 2);
}

TEST_CASE(timer_periodic_busy_single_worker_queues_each_normal_expiry) {
    EventLoop loop;
    TEST_CHECK(loop.init(workers(1)));
    test::Gate first_entered;
    test::Gate release_worker;
    test::Gate three_accepted;
    test::Gate release_scheduler;
    test::Gate third_called;
    test::Gate done;
    std::atomic_int posts{0};
    std::atomic_int calls{0};
    auto timer = loop.add_timer(TimerSchedule::every(5ms),
        [&](const TimerContext& context) {
            const int call = ++calls;
            if (call == 1) {
                first_entered.open();
                release_worker.wait();
            }
            if (call == 3) {
                TEST_CHECK(context.self.cancel());
                third_called.open();
            }
        }, [&] {
            done.open();
        });
    EventLoopTestAccess::timer_hooks(*EventLoopTestAccess::timer_engine(loop), {}, [&] {
        if (++posts == 3) {
            three_accepted.open();
            release_scheduler.wait();
        }
    });
    TEST_CHECK(loop.start());
    first_entered.wait();
    three_accepted.wait();
    TEST_CHECK(calls == 1);
    release_worker.open();
    third_called.wait();
    release_scheduler.open();
    done.wait();
    test::finish(loop);
    TEST_CHECK(posts == 3 && calls == 3 && !timer.valid());
}

TEST_CASE(timer_periodic_scheduler_lateness_skips_missed_deadlines_without_burst) {
    EventLoop loop;
    TEST_CHECK(loop.init(workers(1)));
    test::Gate selected;
    test::Gate release_scheduler;
    test::Gate done;
    std::atomic_int selections{0};
    std::vector<Clock::time_point> deadlines;
    const auto interval = 20ms;
    auto timer = loop.add_timer(TimerSchedule::every(interval),
        [&](const TimerContext& context) {
            deadlines.push_back(context.deadline);
            if (deadlines.size() == 3) {
                TEST_CHECK(context.self.cancel());
            }
        }, [&] {
            done.open();
        });
    const auto first = EventLoopTestAccess::timer_deadline(timer);
    EventLoopTestAccess::timer_hooks(*EventLoopTestAccess::timer_engine(loop), [&] {
        if (++selections == 1) {
            selected.open();
            release_scheduler.wait();
        }
    }, {});
    TEST_CHECK(loop.start());
    selected.wait();
    wait_until(first + interval * 3 + interval / 2);
    release_scheduler.open();
    done.wait();
    test::finish(loop);
    TEST_CHECK(deadlines.size() == 3 && deadlines[0] == first);
    // 已预排的下一到期可生成一个任务，但不能连续补齐所有历史截止。
    TEST_CHECK(deadlines[2] > first + interval * 3);
    TEST_CHECK(deadlines[2] - deadlines[1] > interval);
    TEST_CHECK((deadlines[2] - first) % interval == Clock::duration::zero());
}

TEST_CASE(timer_update_before_selection_replaces_pending_periodic_schedule) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate done;
    std::atomic_int calls{0};
    const auto deadline = Clock::now() - 1s;
    auto timer = loop.add_timer(TimerSchedule::every(24h),
        [&](const TimerContext& context) {
            TEST_CHECK(context.deadline == deadline);
            ++calls;
        }, [&] {
            done.open();
        });
    TEST_CHECK(loop.update_timer(timer, TimerSchedule::at(deadline)));
    TEST_CHECK(EventLoopTestAccess::timer_deadline(timer) == deadline);
    TEST_CHECK(loop.start());
    done.wait();
    test::finish(loop);
    TEST_CHECK(calls == 1 && !timer.valid());
}

TEST_CASE(timer_update_after_selection_preserves_old_snapshot_and_new_deadline) {
    EventLoop loop;
    TEST_CHECK(loop.init(workers(1)));
    test::Gate selected;
    test::Gate release;
    test::Gate done;
    std::atomic_int selections{0};
    std::vector<Clock::time_point> deadlines;
    const auto old_deadline = Clock::now() - 1s;
    auto timer = loop.add_timer(TimerSchedule::at(old_deadline),
        [&](const TimerContext& context) {
            deadlines.push_back(context.deadline);
        },
        [&] {
            done.open();
        });
    EventLoopTestAccess::timer_hooks(*EventLoopTestAccess::timer_engine(loop), [&] {
        if (++selections == 1) {
            selected.open();
            release.wait();
        }
    }, {});
    TEST_CHECK(loop.start());
    selected.wait();
    const auto new_deadline = Clock::now() + 20ms;
    TEST_CHECK(timer.update(TimerSchedule::at(new_deadline)));
    TEST_CHECK(EventLoopTestAccess::timer_deadline(timer) == new_deadline);
    release.open();
    done.wait();
    test::finish(loop);
    TEST_CHECK((deadlines == std::vector<Clock::time_point>{old_deadline, new_deadline}));
}

TEST_CASE(timer_update_queued_one_shot_to_periodic_preserves_accepted_callback) {
    EventLoop loop;
    TEST_CHECK(loop.init(workers(1)));
    test::Gate occupied;
    test::Gate release_worker;
    test::Gate accepted;
    test::Gate called;
    test::Gate done;
    const auto old_deadline = Clock::now() - 1s;
    std::atomic_int calls{0};
    auto timer = loop.add_timer(TimerSchedule::at(old_deadline),
        [&](const TimerContext& context) {
            TEST_CHECK(context.deadline == old_deadline);
            ++calls;
            called.open();
        }, [&] {
            done.open();
        });
    EventLoopTestAccess::timer_hooks(*EventLoopTestAccess::timer_engine(loop),
        [&] {
            occupied.wait();
        }, [&] {
            accepted.open();
        });
    TEST_CHECK(loop.start());
    TEST_CHECK(EventLoopTestAccess::post_worker(loop, [&] {
        occupied.open();
        release_worker.wait();
    }));
    accepted.wait();
    const auto before = Clock::now();
    TEST_CHECK(timer.update(TimerSchedule::every(24h)));
    TEST_CHECK(EventLoopTestAccess::timer_deadline(timer) >= before + 24h);
    release_worker.open();
    called.wait();
    TEST_CHECK(timer.valid() && !done.is_open());
    TEST_CHECK(timer.cancel());
    done.wait();
    test::finish(loop);
    TEST_CHECK(calls == 1);
}

TEST_CASE(timer_callback_self_update_does_not_wait_for_callback_completion) {
    EventLoop loop;
    TEST_CHECK(loop.init(workers(2)));
    test::Gate updated;
    test::Gate next_entered;
    test::Gate release_first;
    test::Gate done;
    std::atomic_int calls{0};
    Clock::time_point update_started;
    auto timer = loop.add_timer(TimerSchedule::after(0ns),
        [&](const TimerContext& context) {
            if (++calls == 1) {
                update_started = Clock::now();
                TEST_CHECK(context.self.update(TimerSchedule::every(10ms)));
                updated.open();
                release_first.wait();
            } else {
                updated.wait();
                TEST_CHECK(context.deadline >= update_started + 10ms);
                TEST_CHECK(context.self.cancel());
                next_entered.open();
            }
        }, [&] {
            done.open();
        });
    TEST_CHECK(loop.start());
    updated.wait();
    next_entered.wait();
    TEST_CHECK(!done.is_open());
    release_first.open();
    done.wait();
    test::finish(loop);
    TEST_CHECK(calls == 2 && !timer.valid());
}

TEST_CASE(timer_external_update_running_callback_uses_update_time) {
    EventLoop loop;
    TEST_CHECK(loop.init(workers(2)));
    test::Gate first_entered;
    test::Gate updated;
    test::Gate second_entered;
    test::Gate release_first;
    test::Gate done;
    std::atomic_int calls{0};
    std::atomic_int posts{0};
    Clock::time_point before_update;
    auto timer = loop.add_timer(TimerSchedule::every(30ms),
        [&](const TimerContext& context) {
            if (++calls == 1) {
                first_entered.open();
                release_first.wait();
            }
            else {
                updated.wait();
                TEST_CHECK(context.deadline >= before_update + 10ms);
                TEST_CHECK(context.self.cancel());
                second_entered.open();
            }
        }, [&] {
            done.open();
        });
    // 第一个周期已生成后暂停 scheduler，确保更新先于旧计划的第二次选择。
    EventLoopTestAccess::timer_hooks(*EventLoopTestAccess::timer_engine(loop), {}, [&] {
        if (++posts == 1) {
            updated.wait();
        }
    });
    TEST_CHECK(loop.start());
    first_entered.wait();
    before_update = Clock::now();
    TEST_CHECK(loop.update_timer(timer, TimerSchedule::every(10ms)));
    updated.open();
    second_entered.wait();
    TEST_CHECK(!done.is_open());
    release_first.open();
    done.wait();
    test::finish(loop);
    TEST_CHECK(calls == 2);
}

TEST_CASE(timer_periodic_set_callback_preserves_old_queued_snapshots) {
    EventLoop loop;
    TEST_CHECK(loop.init(workers(1)));
    test::Gate first_entered;
    test::Gate release_worker;
    test::Gate old_queued;
    test::Gate release_second_post;
    test::Gate new_queued;
    test::Gate release_third_post;
    test::Gate new_called;
    test::Gate done;
    std::atomic_int posts{0};
    std::atomic_int old_calls{0};
    std::atomic_int new_calls{0};
    auto timer = loop.add_timer(TimerSchedule::every(5ms),
        [&](const TimerContext&) {
            if (++old_calls == 1) {
                first_entered.open();
                release_worker.wait();
            }
        }, [&] {
            done.open();
        });
    EventLoopTestAccess::timer_hooks(*EventLoopTestAccess::timer_engine(loop), {}, [&] {
        const int post = ++posts;
        if (post == 2) {
            old_queued.open();
            release_second_post.wait();
        }
        if (post == 3) {
            new_queued.open();
            release_third_post.wait();
        }
    });
    TEST_CHECK(loop.start());
    first_entered.wait();
    old_queued.wait();
    TEST_CHECK(loop.set_timer_callback(timer, [&](const TimerContext& context) {
        ++new_calls;
        TEST_CHECK(context.self.cancel());
        new_called.open();
    }));
    release_second_post.open();
    new_queued.wait();
    release_worker.open();
    new_called.wait();
    release_third_post.open();
    done.wait();
    test::finish(loop);
    TEST_CHECK(old_calls == 2 && new_calls == 1 && posts == 3);
}

TEST_CASE(timer_periodic_cancel_waits_for_all_in_flight_and_capture_retirement) {
    EventLoop loop;
    TEST_CHECK(loop.init(workers(2)));
    test::Gate both_entered;
    test::Gate release_workers;
    test::Gate three_accepted;
    test::Gate release_scheduler;
    test::Gate destroying;
    test::Gate release_capture;
    test::Gate done;
    std::atomic_int entered{0};
    std::atomic_int finished{0};
    std::atomic_int posts{0};
    auto capture = std::make_shared<DestructionAction>();
    capture->action = [&] {
        TEST_CHECK(finished == 2);
        destroying.open();
        release_capture.wait();
        TEST_CHECK(!done.is_open());
    };
    auto timer = loop.add_timer(TimerSchedule::every(5ms),
        [&, capture](const TimerContext&) {
            if (++entered == 2) {
                both_entered.open();
            }
            release_workers.wait();
            ++finished;
        }, [&] {
            done.open();
        });
    capture.reset();
    EventLoopTestAccess::timer_hooks(*EventLoopTestAccess::timer_engine(loop), {}, [&] {
        if (++posts == 3) {
            three_accepted.open();
            release_scheduler.wait();
        }
    });
    TEST_CHECK(loop.start());
    both_entered.wait();
    three_accepted.wait();
    TEST_CHECK(timer.cancel());
    TEST_CHECK(!done.is_open());
    release_scheduler.open();
    release_workers.open();
    destroying.wait();
    TEST_CHECK(!done.is_open());
    release_capture.open();
    done.wait();
    test::finish(loop);
    TEST_CHECK(entered == 2 && finished == 2 && posts == 3);
}

TEST_CASE(timer_periodic_soft_stop_drains_all_accepted_occurrences) {
    periodic_queued_stop(ShutdownMode::Soft, 3);
}

TEST_CASE(timer_periodic_hard_stop_discards_unqualified_occurrences) {
    periodic_queued_stop(ShutdownMode::Hard, 1);
}

TEST_CASE(timer_periodic_stop_before_post_rejects_selected_occurrence) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate selected;
    test::Gate release;
    test::Gate done;
    std::atomic_int calls{0};
    std::atomic_int posts{0};
    auto timer = loop.add_timer(TimerSchedule::every(5ms),
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
    loop.stop();
    TEST_CHECK(!timer.update(TimerSchedule::every(1ms)));
    TEST_CHECK(!timer.set_cb([](const TimerContext&) {}));
    TEST_CHECK(!timer.cancel());
    release.open();
    TEST_CHECK(loop.run());
    done.wait();
    TEST_CHECK(calls == 0 && posts == 0);
}

TEST_CASE(timer_periodic_standard_exception_cancels_future_occurrences) {
    periodic_callback_exception(true);
}

TEST_CASE(timer_periodic_nonstandard_exception_cancels_future_occurrences) {
    periodic_callback_exception(false);
}

TEST_CASE(timer_periodic_exception_skips_queued_and_waits_for_other_callbacks) {
    periodic_exception_with_in_flight(false);
}

TEST_CASE(timer_periodic_exception_after_soft_stop_still_cancels_queued_callbacks) {
    periodic_exception_with_in_flight(true);
}

TEST_CASE(timer_single_snapshot_exception_keeps_updated_periodic_schedule) {
    EventLoop loop;
    TEST_CHECK(loop.init(workers(1)));
    test::Gate done;
    std::atomic_int calls{0};
    std::atomic_int completions{0};
    auto timer = loop.add_timer(TimerSchedule::after(0ns),
        [&](const TimerContext& context) {
            const int call = ++calls;
            if (call == 1) {
                TEST_CHECK(context.self.update(TimerSchedule::every(5ms)));
                throw std::runtime_error("expected single callback after periodic update");
            }
            TEST_CHECK(call == 2);
            TEST_CHECK(context.self.cancel());
        }, [&] {
            ++completions;
            done.open();
        });
    TEST_CHECK(loop.start());
    done.wait();
    test::finish(loop);
    TEST_CHECK(calls == 2 && completions == 1 && !timer.valid());
}

TEST_CASE(timer_periodic_snapshot_exception_cancels_updated_single_schedule) {
    EventLoop loop;
    TEST_CHECK(loop.init(workers(1)));
    test::Gate updated;
    test::Gate done;
    std::atomic_int calls{0};
    std::atomic_int posts{0};
    std::atomic_int completions{0};
    auto timer = loop.add_timer(TimerSchedule::every(5ms),
        [&](const TimerContext& context) {
            ++calls;
            TEST_CHECK(context.self.update(TimerSchedule::after(24h)));
            updated.open();
            throw std::runtime_error("expected periodic callback after single update");
        }, [&] {
            ++completions;
            done.open();
        });
    EventLoopTestAccess::timer_hooks(*EventLoopTestAccess::timer_engine(loop), {}, [&] {
        if (++posts == 1) {
            updated.wait();
        }
    });
    TEST_CHECK(loop.start());
    done.wait();
    test::finish(loop);
    TEST_CHECK(calls == 1 && posts == 1 && completions == 1 && !timer.valid());
}

TEST_CASE(timer_update_and_callback_replacement_reject_invalid_inputs) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    std::atomic_int calls{0};
    test::Gate done;
    TimerRegistration empty;
    TEST_CHECK(!empty.update(TimerSchedule::after(0ns)));
    TEST_CHECK(!empty.set_cb([](const TimerContext&) {}));
    auto timer = loop.add_timer(TimerSchedule::every(24h),
        [&](const TimerContext&) {
            ++calls;
        }, [&] {
            done.open();
        });
    const auto deadline = EventLoopTestAccess::timer_deadline(timer);
    TEST_CHECK(!timer.update(TimerSchedule{}));
    TEST_CHECK(!loop.update_timer(timer, TimerSchedule::every(0ns)));
    TEST_CHECK(!timer.set_cb({}));
    TEST_CHECK(!loop.set_timer_callback(timer, {}));
    TEST_CHECK(EventLoopTestAccess::timer_deadline(timer) == deadline);
    TEST_CHECK(timer.update(TimerSchedule::after(0ns)));
    TEST_CHECK(loop.start());
    done.wait();
    TEST_CHECK(!timer.update(TimerSchedule::every(1ms)));
    TEST_CHECK(!timer.set_cb([](const TimerContext&) {}));
    test::finish(loop);
    TEST_CHECK(calls == 1);
}

TEST_CASE(timer_callback_replacement_retires_old_capture_outside_state_locks) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate retired;
    test::Gate done;
    std::atomic_int new_calls{0};
    auto capture = std::make_shared<DestructionAction>();
    TimerRegistration timer;
    capture->action = [&] {
        TEST_CHECK(timer.update(TimerSchedule::after(0ns)));
        retired.open();
    };
    timer = loop.add_timer(TimerSchedule::every(24h),
        [capture](const TimerContext&) {
        }, [&] {
            done.open();
        });
    capture.reset();
    TEST_CHECK(timer.set_cb([&](const TimerContext&) {
        ++new_calls;
    }));
    retired.wait();
    TEST_CHECK(loop.start());
    done.wait();
    test::finish(loop);
    TEST_CHECK(new_calls == 1);
}

TEST_CASE(timer_update_and_cancel_race_has_defined_final_state) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    TEST_CHECK(loop.start());
    std::atomic_int done{0};
    std::atomic_int calls{0};
    for (int iteration = 0; iteration != 20; ++iteration) {
        auto timer = loop.add_timer(TimerSchedule::every(24h),
            [&](const TimerContext&) {
                ++calls;
            }, [&] {
                ++done;
            });
        test::Gate begin;
        bool canceled = false;
        std::thread updater([&] {
            begin.wait();
            (void)timer.update(TimerSchedule::every(12h));
        });
        std::thread canceler([&] {
            begin.wait();
            canceled = timer.cancel();
        });
        begin.open();
        updater.join();
        canceler.join();
        TEST_CHECK(canceled && !timer.valid());
        TEST_CHECK(!timer.update(TimerSchedule::after(0ns)));
    }
    test::finish(loop);
    TEST_CHECK(done == 20 && calls == 0);
}

TEST_CASE(timer_update_and_stop_race_rejects_operations_after_stop) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    std::atomic_int done{0};
    std::atomic_int calls{0};
    auto timer = loop.add_timer(TimerSchedule::every(24h),
        [&](const TimerContext&) {
            ++calls;
        }, [&] {
            ++done;
        });
    TEST_CHECK(loop.start());
    test::Gate begin;
    std::thread updater([&] {
        begin.wait();
        (void)timer.update(TimerSchedule::every(12h));
    });
    std::thread stopper([&] {
        begin.wait();
        loop.stop();
    });
    begin.open();
    updater.join();
    stopper.join();
    TEST_CHECK(!timer.update(TimerSchedule::after(0ns)));
    TEST_CHECK(!loop.update_timer(timer, TimerSchedule::every(1ms)));
    TEST_CHECK(!timer.set_cb([](const TimerContext&) {}));
    TEST_CHECK(!timer.cancel());
    TEST_CHECK(loop.run());
    TEST_CHECK(done == 1 && calls == 0);
}

TEST_CASE(timer_parallel_updates_and_callback_replacement_take_effect) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    std::atomic_int old_calls{0};
    std::atomic_int new_calls{0};
    test::Gate done;
    auto timer = loop.add_timer(TimerSchedule::every(24h),
        [&](const TimerContext&) {
            ++old_calls;
        }, [&] {
            done.open();
        });
    TEST_CHECK(loop.start());
    test::Gate begin;
    std::vector<std::thread> threads;
    for (int index = 0; index != 4; ++index) {
        threads.emplace_back([&] {
            begin.wait();
            for (int i = 0; i != 20; ++i) {
                TEST_CHECK(timer.update(TimerSchedule::every(12h)));
                TEST_CHECK(timer.set_cb([&](const TimerContext&) {
                    ++new_calls;
                }));
            }
        });
    }
    begin.open();
    for (auto& thread : threads) {
        thread.join();
    }
    const auto deadline = Clock::now() - 1s;
    TEST_CHECK(timer.update(TimerSchedule::at(deadline)));
    done.wait();
    test::finish(loop);
    TEST_CHECK(old_calls == 0 && new_calls == 1);
}

TEST_CASE(timer_periodic_management_is_safe_after_loop_destruction) {
    TimerRegistration old;
    std::atomic_int done{0};
    {
        EventLoop loop;
        TEST_CHECK(loop.init());
        old = loop.add_timer(TimerSchedule::every(24h),
            [](const TimerContext&) {
            }, [&] {
                ++done;
            });
    }
    TEST_CHECK(done == 1 && !old.valid());
    TEST_CHECK(!old.update(TimerSchedule::every(1ms)));
    TEST_CHECK(!old.set_cb([](const TimerContext&) {}));
    TEST_CHECK(!old.cancel());
}

TEST_CASE(timer_old_post_failure_does_not_retire_successfully_updated_schedule) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate selected;
    test::Gate release_old_post;
    test::Gate done;
    std::atomic_int attempts{0};
    std::atomic_int posts{0};
    std::atomic_int calls{0};
    Clock::time_point new_deadline;
    auto timer = loop.add_timer(TimerSchedule::after(0ns),
        [&](const TimerContext& context) {
            TEST_CHECK(context.deadline == new_deadline);
            ++calls;
        }, [&] {
            done.open();
        });
    EventLoopTestAccess::timer_hooks(*EventLoopTestAccess::timer_engine(loop), [&] {
        if (++attempts == 1) {
            selected.open();
            release_old_post.wait();
            test::set_allocation_failure(0);
        }
    }, [&] {
        ++posts;
    });
    TEST_CHECK(loop.start());
    selected.wait();
    new_deadline = Clock::now() + 20ms;
    TEST_CHECK(timer.update(TimerSchedule::at(new_deadline)));
    release_old_post.open();
    done.wait();
    test::finish(loop);
    TEST_CHECK(attempts == 2 && posts == 1 && calls == 1);
    TEST_CHECK(!timer.valid());
}

TEST_CASE(timer_failed_update_keeps_old_plan_without_false_success) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate done;
    auto timer = loop.add_timer(TimerSchedule::every(24h),
        [](const TimerContext&) {
        }, [&] {
            done.open();
        });
    const auto previous = EventLoopTestAccess::timer_deadline(timer);
    const auto replacement = Clock::now() + 1h;
    bool success = false;
    test::set_allocation_failure(0);
    try {
        success = timer.update(TimerSchedule::at(replacement));
    } catch (const std::bad_alloc&) {
    } catch (...) {
        test::clear_allocation_failure();
        throw;
    }
    test::clear_allocation_failure();
    // 不强制实现必须分配；若可无分配更新，true 对应的新计划也必须可观察。
    TEST_CHECK(timer.valid());
    TEST_CHECK(EventLoopTestAccess::timer_deadline(timer) ==
               (success ? replacement : previous));
    TEST_CHECK(timer.update(TimerSchedule::every(12h)));
    TEST_CHECK(timer.cancel());
    done.wait();
    test::finish(loop);
}

TEST_CASE(timer_failed_callback_replacement_preserves_old_snapshot) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate done;
    std::atomic_int old_calls{0};
    std::atomic_int new_calls{0};
    auto timer = loop.add_timer(TimerSchedule::every(24h),
        [&](const TimerContext&) {
            ++old_calls;
        }, [&] {
            done.open();
        });
    std::function<void(const TimerContext&)> replacement =
        [&](const TimerContext&) {
            ++new_calls;
        };
    bool success = false;
    test::set_allocation_failure(0);
    try {
        success = timer.set_cb(std::move(replacement));
    } catch (const std::bad_alloc&) {
    } catch (...) {
        test::clear_allocation_failure();
        throw;
    }
    test::clear_allocation_failure();
    TEST_CHECK(timer.valid());
    TEST_CHECK(timer.update(TimerSchedule::after(0ns)));
    TEST_CHECK(loop.start());
    done.wait();
    test::finish(loop);
    TEST_CHECK(old_calls == (success ? 0 : 1));
    TEST_CHECK(new_calls == (success ? 1 : 0));
}

TEST_CASE(timer_update_insert_failure_preserves_selected_one_shot_and_allows_retry) {
    EventLoop loop;
    TEST_CHECK(loop.init(workers(1)));
    test::Gate selected;
    test::Gate release_old_post;
    test::Gate done;
    std::atomic_int selections{0};
    std::vector<Clock::time_point> deadlines;
    const auto old_deadline = Clock::now() - 1s;
    auto timer = loop.add_timer(TimerSchedule::at(old_deadline),
        [&](const TimerContext& context) {
            deadlines.push_back(context.deadline);
        },
        [&] {
            done.open();
        });
    EventLoopTestAccess::timer_hooks(*EventLoopTestAccess::timer_engine(loop), [&] {
        if (++selections == 1) {
            selected.open();
            release_old_post.wait();
        }
    }, {});
    TEST_CHECK(loop.start());
    selected.wait();
    const auto replacement = Clock::now() + 20ms;
    bool success = false;
    test::set_allocation_failure(0);
    try {
        success = timer.update(TimerSchedule::at(replacement));
    } catch (const std::bad_alloc&) {
    } catch (...) {
        test::clear_allocation_failure();
        throw;
    }
    test::clear_allocation_failure();
    // 单次节点已经被取出，更新必须新建 pending 节点，故障不能伪报成功。
    TEST_CHECK(!success && timer.valid());
    TEST_CHECK(EventLoopTestAccess::timer_deadline(timer) == old_deadline);
    TEST_CHECK(!done.is_open());
    TEST_CHECK(timer.update(TimerSchedule::at(replacement)));
    TEST_CHECK(EventLoopTestAccess::timer_deadline(timer) == replacement);
    release_old_post.open();
    done.wait();
    test::finish(loop);
    TEST_CHECK((deadlines == std::vector<Clock::time_point>{old_deadline, replacement}));
}

TEST_CASE(timer_periodic_post_failure_retires_future_schedule_without_update) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate done;
    std::atomic_int attempts{0};
    std::atomic_int posts{0};
    std::atomic_int calls{0};
    std::atomic_int completions{0};
    auto timer = loop.add_timer(TimerSchedule::every(5ms),
        [&](const TimerContext&) {
            ++calls;
        }, [&] {
            test::clear_allocation_failure();
            ++completions;
            loop.stop();
            done.open();
        });
    EventLoopTestAccess::timer_hooks(*EventLoopTestAccess::timer_engine(loop), [&] {
        ++attempts;
        // 已预排的下一周期也必须随本计划的投递失败一并终结。
        test::set_allocation_failure(0);
    }, [&] {
        ++posts;
    });
    TEST_CHECK(loop.start());
    done.wait();
    TEST_CHECK(loop.run());
    TEST_CHECK(attempts == 1 && posts == 0 && calls == 0 && completions == 1);
    TEST_CHECK(!timer.valid());
}
