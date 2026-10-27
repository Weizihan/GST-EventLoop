#include "TestSupport.h"

#include "EventLoop.h"
#include "EventLoopTestAccess.h"

#include <atomic>
#include <cstdint>
#include <fcntl.h>
#include <memory>
#include <new>
#include <stdexcept>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <thread>
#include <type_traits>
#include <unistd.h>
#include <utility>

using namespace GST::BASE;

static_assert(std::is_copy_constructible<IoRegistration>::value,
              "IoRegistration must be copy constructible");
static_assert(std::is_copy_assignable<IoRegistration>::value,
              "IoRegistration must be copy assignable");
static_assert(std::is_nothrow_move_constructible<IoRegistration>::value,
              "IoRegistration must be nothrow move constructible");
static_assert(std::is_nothrow_move_assignable<IoRegistration>::value,
              "IoRegistration must be nothrow move assignable");
static_assert(std::is_same<decltype(IoContext{}.self), IoRegistration>::value,
              "Callback and caller must use the same registration type");

namespace {

NativeHandle make_source() {
    NativeHandle source(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    TEST_CHECK(source.valid());
    return source;
}

void signal_source(const NativeHandle& source) {
    const std::uint64_t one = 1;
    TEST_CHECK(write(source.native_fd(), &one, sizeof(one)) == sizeof(one));
}

void consume_source(const NativeHandle& source) {
    std::uint64_t value = 0;
    TEST_CHECK(read(source.native_fd(), &value, sizeof(value)) == sizeof(value));
}

struct DestructionAction {
    std::function<void()> action;
    ~DestructionAction() { if (action) action(); }
};

void queued_callback_stop(ShutdownMode mode, bool should_run) {
    auto first_source = make_source();
    auto second_source = make_source();
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate entered;
    test::Gate release;
    test::Gate second_accepted;
    test::Gate cutoff;
    test::Gate first_done;
    test::Gate second_done;
    std::atomic_int calls{0};
    std::atomic_int published{0};
    auto first = loop.watch_io(first_source, Event_type::READ_EVENT,
        [&](const IoContext&) {
            consume_source(first_source);
            entered.open();
            release.wait();
        }, [&] { first_done.open(); });
    auto second = loop.watch_io(second_source, Event_type::READ_EVENT,
        [&](const IoContext&) { consume_source(second_source); ++calls; },
        [&] { second_done.open(); });
    TEST_CHECK(first.valid() && second.valid());
    auto engine = EventLoopTestAccess::engine(loop, 0);
    EventLoopTestAccess::hooks(*engine, {}, {}, [&] {
        if (++published == 2) second_accepted.open();
    });
    EventLoopTestAccess::stop_hook(loop, [&] { cutoff.open(); });
    TEST_CHECK(loop.start());
    signal_source(first_source);
    entered.wait();
    signal_source(second_source);
    second_accepted.wait();
    std::thread stopper([&] { test::finish(loop, mode); });
    cutoff.wait();
    release.open();
    stopper.join();
    TEST_CHECK(calls == (should_run ? 1 : 0));
    first_done.wait();
    second_done.wait();
    TEST_CHECK(!first.valid() && !second.valid());
}

}

TEST_CASE(loop_empty_lifecycle_and_reinitialization) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    TEST_CHECK(loop.start());
    TEST_CHECK(loop.start());
    test::finish(loop);
    TEST_CHECK(loop.init());
    TEST_CHECK(loop.start());
    test::finish(loop, ShutdownMode::Hard);
    test::finish(loop);
}

TEST_CASE(loop_read_callback_uses_shared_worker) {
    auto source = make_source();
    EventLoop loop;
    TEST_CHECK(loop.init());
    const auto caller = std::this_thread::get_id();
    test::Gate called;
    auto handle = loop.watch_io(source, Event_type::READ_EVENT,
        [&](const IoContext& context) {
            TEST_CHECK(context.event_type == Event_type::READ_EVENT);
            TEST_CHECK(context.self.valid());
            TEST_CHECK(std::this_thread::get_id() != caller);
            consume_source(source);
            called.open();
        });
    TEST_CHECK(handle.valid());
    TEST_CHECK(loop.start());
    signal_source(source);
    called.wait();
    test::finish(loop);
    TEST_CHECK(fcntl(source.native_fd(), F_GETFD) >= 0);
}

TEST_CASE(loop_pending_remove_cancels_before_start) {
    auto source = make_source();
    EventLoop loop;
    TEST_CHECK(loop.init());
    std::atomic_int calls{0};
    test::Gate done;
    auto handle = loop.watch_io(source, Event_type::READ_EVENT,
        [&](const IoContext&) { ++calls; }, [&] { done.open(); });
    const auto copy = handle;
    TEST_CHECK(handle.valid());
    TEST_CHECK(loop.remove(handle));
    TEST_CHECK(!handle.valid() && !copy.valid());
    TEST_CHECK(!loop.remove(copy));
    signal_source(source);
    TEST_CHECK(loop.start());
    test::finish(loop);
    done.wait();
    TEST_CHECK(calls == 0);
}

TEST_CASE(loop_registration_copy_assignment_move_and_weak_ownership) {
    auto source = make_source();
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate done;
    IoRegistration empty;
    TEST_CHECK(!empty.valid() && !static_cast<bool>(empty));
    TEST_CHECK(!loop.remove(empty));
    TEST_CHECK(!loop.update_io(empty, Event_type::READ_EVENT));
    TEST_CHECK(!loop.set_io_callback(empty, Event_type::READ_EVENT,
                                     [](const IoContext&) {}));
    auto registration = loop.watch_io(source, Event_type::READ_EVENT,
        [](const IoContext&) {}, [&] { done.open(); });
    const auto copy = registration;
    IoRegistration assigned;
    assigned = copy;
    IoRegistration moved(std::move(assigned));
    TEST_CHECK(!assigned.valid() && moved.valid());
    IoRegistration move_assigned;
    move_assigned = std::move(moved);
    TEST_CHECK(!moved.valid() && move_assigned.valid());
    {
        const auto temporary = registration;
        TEST_CHECK(temporary.valid());
    }
    // 丢弃副本不会取消注册；丢弃最初返回的对象也不影响其他副本。
    registration = {};
    TEST_CHECK(copy.valid() && move_assigned.valid());
    TEST_CHECK(!done.is_open());
    TEST_CHECK(loop.start());
    TEST_CHECK(loop.update_io(copy, Event_type::READ_EVENT));
    TEST_CHECK(loop.remove(move_assigned));
    TEST_CHECK(!copy.valid() && !move_assigned.valid());
    // 副本只保存弱身份，保留它们不会延长注册清理或 on_done。
    done.wait();
    TEST_CHECK(fcntl(source.native_fd(), F_GETFD) >= 0);
    test::finish(loop);
}

TEST_CASE(loop_registration_copy_remove_invalidates_all_copies) {
    auto source = make_source();
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate done;
    auto handle = loop.watch_io(source, Event_type::READ_EVENT,
        [](const IoContext&) {}, [&] { done.open(); });
    TEST_CHECK(loop.start());
    auto engine = EventLoopTestAccess::engine(loop, 0);
    const auto registration = EventLoopTestAccess::registration(*engine, 1);
    TEST_CHECK(registration.valid());
    TEST_CHECK(loop.remove(registration));
    TEST_CHECK(!registration.valid());
    TEST_CHECK(!handle.valid());
    TEST_CHECK(!loop.remove(registration));
    done.wait();
    test::finish(loop);
}

TEST_CASE(loop_callback_self_remove) {
    auto source = make_source();
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate called;
    test::Gate done;
    IoRegistration outside_copy;
    auto handle = loop.watch_io(source, Event_type::READ_EVENT,
        [&](const IoContext& context) {
            consume_source(source);
            const auto self_copy = context.self;
            TEST_CHECK(outside_copy.valid() && self_copy.valid());
            TEST_CHECK(loop.remove(self_copy));
            TEST_CHECK(!context.self.valid() && !outside_copy.valid());
            TEST_CHECK(!self_copy.valid());
            TEST_CHECK(!done.is_open());
            called.open();
        }, [&] { done.open(); });
    outside_copy = handle;
    TEST_CHECK(loop.start());
    signal_source(source);
    called.wait();
    done.wait();
    TEST_CHECK(!handle.valid());
    test::finish(loop);
}

TEST_CASE(loop_registration_rejects_operations_through_another_loop) {
    auto source = make_source();
    EventLoop first;
    EventLoop second;
    TEST_CHECK(first.init());
    TEST_CHECK(!second.init());
    test::Gate done;
    auto registration = first.watch_io(source, Event_type::READ_EVENT,
        [](const IoContext&) {}, [&] { done.open(); });
    const auto copy = registration;
    TEST_CHECK(first.start());
    TEST_CHECK(!second.remove(copy));
    TEST_CHECK(!second.update_io(copy, Event_type::WRITE_EVENT));
    TEST_CHECK(!second.set_io_callback(copy, Event_type::READ_EVENT,
                                       [](const IoContext&) {}));
    TEST_CHECK(registration.valid() && copy.valid());
    TEST_CHECK(EventLoopTestAccess::routes(first) == 1);
    TEST_CHECK(EventLoopTestAccess::routes(second) == 0);
    TEST_CHECK(registration.valid() && copy.valid());
    TEST_CHECK(first.remove(copy));
    done.wait();
    test::finish(first);
    TEST_CHECK(second.init());
    TEST_CHECK(second.start());
    TEST_CHECK(!second.remove(copy));
    TEST_CHECK(!second.update_io(copy, Event_type::WRITE_EVENT));
    TEST_CHECK(!second.set_io_callback(copy, Event_type::READ_EVENT,
                                       [](const IoContext&) {}));
    test::finish(second);
}

TEST_CASE(loop_stop_and_reinitialization_keep_old_registration_copies_invalid) {
    auto source = make_source();
    EventLoop loop;
    TEST_CHECK(loop.init());
    std::atomic_int done{0};
    auto old_registration = loop.watch_io(source, Event_type::READ_EVENT,
        [](const IoContext&) {}, [&] { ++done; });
    const auto old_copy = old_registration;
    TEST_CHECK(loop.start());
    test::finish(loop);
    TEST_CHECK(!old_registration.valid() && !old_copy.valid());
    TEST_CHECK(done == 1);
    TEST_CHECK(loop.init());
    auto current = loop.watch_io(source, Event_type::READ_EVENT,
        [](const IoContext&) {}, [&] { ++done; });
    TEST_CHECK(current.valid());
    TEST_CHECK(!loop.remove(old_copy));
    TEST_CHECK(!loop.update_io(old_copy, Event_type::WRITE_EVENT));
    TEST_CHECK(!loop.set_io_callback(old_copy, Event_type::READ_EVENT,
                                     [](const IoContext&) {}));
    TEST_CHECK(current.valid() && EventLoopTestAccess::routes(loop) == 1);
    TEST_CHECK(loop.start());
    test::finish(loop, ShutdownMode::Hard);
    TEST_CHECK(!current.valid() && done == 2);
}

TEST_CASE(loop_running_remove_does_not_wait_for_callback) {
    auto source = make_source();
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate entered;
    test::Gate release;
    test::Gate done;
    auto handle = loop.watch_io(source, Event_type::READ_EVENT,
        [&](const IoContext&) {
            consume_source(source);
            entered.open();
            release.wait();
            TEST_CHECK(fcntl(source.native_fd(), F_GETFD) >= 0);
        }, [&] { source.reset(); done.open(); });
    TEST_CHECK(loop.start());
    signal_source(source);
    entered.wait();
    TEST_CHECK(loop.remove(handle));
    TEST_CHECK(!handle.valid());
    TEST_CHECK(!done.is_open());
    release.open();
    done.wait();
    TEST_CHECK(!source.valid());
    test::finish(loop);
}

TEST_CASE(loop_concurrent_registration_copies_remove_succeeds_once) {
    auto source = make_source();
    EventLoop loop;
    TEST_CHECK(loop.init());
    std::atomic_int done{0};
    auto handle = loop.watch_io(source, Event_type::READ_EVENT,
        [](const IoContext&) {}, [&] { ++done; });
    TEST_CHECK(loop.start());
    const auto first_copy = handle;
    const auto second_copy = handle;
    test::Gate start;
    std::atomic_int successes{0};
    std::thread first([&] { start.wait(); if (loop.remove(first_copy)) ++successes; });
    std::thread second([&] { start.wait(); if (loop.remove(second_copy)) ++successes; });
    start.open();
    first.join();
    second.join();
    test::finish(loop);
    TEST_CHECK(successes == 1);
    TEST_CHECK(done == 1);
    TEST_CHECK(!handle.valid());
    TEST_CHECK(!first_copy.valid() && !second_copy.valid());
}

TEST_CASE(loop_pending_concurrent_registration_remove_claims_once_before_enqueue) {
    auto source = make_source();
    EventLoop loop;
    TEST_CHECK(loop.init());
    std::atomic_int done{0};
    auto registration = loop.watch_io(source, Event_type::READ_EVENT,
        [](const IoContext&) {}, [&] { ++done; });
    const auto first_copy = registration;
    const auto second_copy = registration;
    auto engine = EventLoopTestAccess::engine(loop, 0);
    auto queue_lock = EventLoopTestAccess::hold_io_queue(*engine);
    test::Gate first_returned;
    test::Gate second_returned;
    bool first_result = false;
    bool second_result = true;
    std::thread first([&] {
        first_result = loop.remove(first_copy);
        first_returned.open();
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    // 首个删除已取得提交资格，但被队列锁挡住，尚未发布删除命令。
    // 此时第二个副本必须直接失败，不能再次排队等待未启动的 owner。
    while (!EventLoopTestAccess::removal_pending(registration)) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    std::thread second([&] {
        second_result = loop.remove(second_copy);
        second_returned.open();
    });
    second_returned.wait();
    TEST_CHECK(!second_result && !first_returned.is_open());
    queue_lock.unlock();
    first.join();
    second.join();
    TEST_CHECK(first_result);
    TEST_CHECK(!registration.valid() && !first_copy.valid() && !second_copy.valid());
    TEST_CHECK(loop.start());
    test::finish(loop);
    TEST_CHECK(done == 1);
}

TEST_CASE(loop_registration_remove_allocation_failure_releases_claim_for_retry) {
    auto source = make_source();
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate done;
    std::atomic_int completions{0};
    auto registration = loop.watch_io(source, Event_type::READ_EVENT,
        [](const IoContext&) {}, [&] { ++completions; done.open(); });
    const auto copy = registration;
    TEST_CHECK(loop.start());
    bool allocation_failed = false;
    // 无外部就绪事件；仅让当前调用线程在同步命令分配时失败。
    // 删除尚未发布给 owner，因此异常必须释放认领，保留有效注册。
    test::set_allocation_failure(0);
    try {
        (void)loop.remove(registration);
    } catch (const std::bad_alloc&) {
        allocation_failed = true;
    } catch (...) {
        test::clear_allocation_failure();
        throw;
    }
    test::clear_allocation_failure();
    TEST_CHECK(allocation_failed);
    TEST_CHECK(registration.valid() && copy.valid());
    TEST_CHECK(!EventLoopTestAccess::removal_pending(registration));
    TEST_CHECK(EventLoopTestAccess::routes(loop) == 1);
    TEST_CHECK(!done.is_open() && completions == 0);
    TEST_CHECK(loop.remove(copy));
    TEST_CHECK(!registration.valid() && !copy.valid());
    done.wait();
    test::finish(loop);
    TEST_CHECK(completions == 1);
}

TEST_CASE(loop_callback_exception_still_completes) {
    auto source = make_source();
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate called;
    test::Gate done;
    auto handle = loop.watch_io(source, Event_type::READ_EVENT,
        [&](const IoContext& context) {
            consume_source(source);
            TEST_CHECK(loop.remove(context.self));
            called.open();
            throw std::runtime_error("expected");
        }, [&] { done.open(); });
    TEST_CHECK(loop.start());
    signal_source(source);
    called.wait();
    done.wait();
    test::finish(loop);
}

TEST_CASE(loop_done_waits_for_callback_capture_destruction) {
    auto source = make_source();
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate destroying;
    test::Gate allow_destroy;
    test::Gate done;
    auto probe = std::make_shared<DestructionAction>();
    probe->action = [&] {
        destroying.open();
        allow_destroy.wait();
        TEST_CHECK(!done.is_open());
        TEST_CHECK(fcntl(source.native_fd(), F_GETFD) >= 0);
    };
    auto handle = loop.watch_io(source, Event_type::READ_EVENT,
        [probe](const IoContext&) {}, [&] { done.open(); });
    probe.reset();
    TEST_CHECK(loop.start());
    std::thread remover([&] { TEST_CHECK(loop.remove(handle)); });
    destroying.wait();
    TEST_CHECK(!done.is_open());
    allow_destroy.open();
    remover.join();
    done.wait();
    test::finish(loop);
}

TEST_CASE(loop_soft_stop_drains_accepted_callback) {
    queued_callback_stop(ShutdownMode::Soft, true);
}

TEST_CASE(loop_hard_stop_cancels_unqualified_callback) {
    queued_callback_stop(ShutdownMode::Hard, false);
}

TEST_CASE(loop_external_hard_upgrades_a_soft_stop_in_progress) {
    auto first_source = make_source();
    auto second_source = make_source();
    test::Gate entered;
    test::Gate release;
    test::Gate accepted;
    test::Gate soft_entered;
    test::Gate continue_soft;
    std::atomic_int published{0};
    std::atomic_int calls{0};
    std::atomic_int done{0};
    EventLoop loop;
    TEST_CHECK(loop.init());
    auto first = loop.watch_io(first_source, Event_type::READ_EVENT,
        [&](const IoContext&) {
            consume_source(first_source);
            entered.open();
            release.wait();
        }, [&] { ++done; });
    auto second = loop.watch_io(second_source, Event_type::READ_EVENT,
        [&](const IoContext&) { ++calls; }, [&] { ++done; });
    TEST_CHECK(first.valid() && second.valid());
    auto engine = EventLoopTestAccess::engine(loop, 0);
    EventLoopTestAccess::hooks(*engine, {}, {}, [&] {
        if (++published == 2) accepted.open();
    });
    EventLoopTestAccess::stop_hook(loop, [&] {
        soft_entered.open();
        continue_soft.wait();
    });
    TEST_CHECK(loop.start());
    signal_source(first_source);
    entered.wait();
    signal_source(second_source);
    accepted.wait();
    std::thread soft([&] { test::finish(loop, ShutdownMode::Soft); });
    soft_entered.wait();
    std::thread hard([&] { loop.stop(ShutdownMode::Hard); });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    // 直接观察实际的原子截止点，不根据经过时间推断；
    // 超时仅用于发现 Hard 停止请求卡住。
    while (!EventLoopTestAccess::hard_stopping(loop)) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
    release.open();
    continue_soft.open();
    soft.join();
    hard.join();
    TEST_CHECK(calls == 0 && done == 2);
}

TEST_CASE(loop_kernel_read_write_batch_obeys_hard_cutoff) {
    int descriptors[2];
    TEST_CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                         0, descriptors) == 0);
    NativeHandle source(descriptors[0]);
    NativeHandle peer(descriptors[1]);
    test::Gate entered;
    test::Gate release;
    test::Gate cutoff;
    std::atomic_int attempts{0};
    std::atomic_int writes{0};
    std::atomic_int done{0};
    EventLoop loop;
    TEST_CHECK(loop.init());
    auto handle = loop.watch_io(source,
        Event_type::READ_EVENT | Event_type::WRITE_EVENT,
        [&](const IoContext& context) {
            if (context.event_type == Event_type::READ_EVENT) {
                consume_source(source);
                entered.open();
                release.wait();
            } else {
                ++writes;
            }
        }, [&] { ++done; });
    TEST_CHECK(handle.valid());
    auto engine = EventLoopTestAccess::engine(loop, 0);
    EventLoopTestAccess::hooks(*engine, [&] { ++attempts; }, {});
    EventLoopTestAccess::stop_hook(loop, [&] { cutoff.open(); });
    // 首次真实 epoll 等待前已有未读数据和空发送缓冲区，即读写均就绪。
    // before 钩子统计两个已选回调的资格申请尝试。
    signal_source(peer);
    TEST_CHECK(loop.start());
    entered.wait();
    std::thread stopper([&] { test::finish(loop, ShutdownMode::Hard); });
    cutoff.wait();
    release.open();
    stopper.join();
    TEST_CHECK(attempts == 2 && writes == 0 && done == 1);
    TEST_CHECK(!handle.valid());
}

TEST_CASE(loop_callback_can_request_stop_but_cannot_wait_or_reinitialize) {
    auto source = make_source();
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::Gate called;
    auto handle = loop.watch_io(source, Event_type::READ_EVENT,
        [&](const IoContext&) {
            consume_source(source);
            TEST_CHECK(!loop.init());
            TEST_CHECK(!loop.start());
            TEST_CHECK(!loop.run());
            loop.stop(ShutdownMode::Hard);
            called.open();
        });
    TEST_CHECK(loop.start());
    signal_source(source);
    called.wait();
    TEST_CHECK(handle.valid());
    test::finish(loop);
}

TEST_CASE(loop_hard_cutoff_applies_to_engines_before_their_stop_steps) {
    auto first_source = make_source();
    auto second_source = make_source();
    EventLoopConfig config;
    config.engine_num = 2;
    config.callback_thread_num = 1;
    EventLoop loop;
    TEST_CHECK(loop.init(config));
    test::Gate first_entered;
    test::Gate release_first;
    test::Gate second_accepted;
    test::Gate cutoff;
    test::Gate allow_engine_stops;
    test::Gate pool_barrier;
    std::atomic_int calls{0};
    std::atomic_int done{0};
    auto first = loop.watch_io(first_source, Event_type::READ_EVENT,
        [&](const IoContext&) {
            consume_source(first_source);
            first_entered.open();
            release_first.wait();
        }, [&] { ++done; });
    auto second = loop.watch_io(second_source, Event_type::READ_EVENT,
        [&](const IoContext&) { consume_source(second_source); ++calls; },
        [&] { ++done; });
    TEST_CHECK(first.valid() && second.valid());
    auto first_engine = EventLoopTestAccess::engine(loop, 0);
    auto second_engine = EventLoopTestAccess::engine(loop, 1);
    EventLoopTestAccess::hooks(*second_engine, {}, {}, [&] { second_accepted.open(); });
    EventLoopTestAccess::stop_hook(loop, [&] {
        cutoff.open();
        allow_engine_stops.wait();
    });
    TEST_CHECK(loop.start());
    signal_source(first_source);
    first_entered.wait();
    signal_source(second_source);
    second_accepted.wait();
    TEST_CHECK(EventLoopTestAccess::post_worker(loop, [&] { pool_barrier.open(); }));
    std::thread stopper([&] { test::finish(loop, ShutdownMode::Hard); });
    cutoff.wait();
    TEST_CHECK(EventLoopTestAccess::owner_running(*first_engine));
    TEST_CHECK(EventLoopTestAccess::owner_running(*second_engine));
    release_first.open();
    pool_barrier.wait();
    TEST_CHECK(calls == 0);
    allow_engine_stops.open();
    stopper.join();
    TEST_CHECK(done == 2);
}

TEST_CASE(loop_two_owners_reject_cross_remove_during_capture_destruction) {
    auto first_source = make_source();
    auto second_source = make_source();
    EventLoopConfig config;
    config.engine_num = 2;
    EventLoop loop;
    TEST_CHECK(loop.init(config));
    test::Gate first_destroying;
    test::Gate second_destroying;
    std::atomic_int rejected{0};
    std::atomic_int done{0};
    auto first_probe = std::make_shared<DestructionAction>();
    auto second_probe = std::make_shared<DestructionAction>();
    auto first = loop.watch_io(first_source, Event_type::READ_EVENT,
        [first_probe](const IoContext&) {}, [&] { ++done; });
    auto second = loop.watch_io(second_source, Event_type::READ_EVENT,
        [second_probe](const IoContext&) {}, [&] { ++done; });
    TEST_CHECK(first.valid() && second.valid());
    TEST_CHECK(loop.start());
    const auto first_registration = EventLoopTestAccess::registration(
        *EventLoopTestAccess::engine(loop, 0), 1);
    const auto second_registration = EventLoopTestAccess::registration(
        *EventLoopTestAccess::engine(loop, 1), 2);
    first_probe->action = [&] {
        first_destroying.open();
        second_destroying.wait();
        TEST_CHECK(!loop.remove(second_registration));
        // owner 析构捕获对象时，拒绝可能等待自身的生命周期重入。
        TEST_CHECK(!loop.init());
        TEST_CHECK(!loop.start());
        TEST_CHECK(!loop.run());
        ++rejected;
    };
    second_probe->action = [&] {
        second_destroying.open();
        first_destroying.wait();
        TEST_CHECK(!loop.remove(first_registration));
        TEST_CHECK(!loop.init());
        TEST_CHECK(!loop.start());
        TEST_CHECK(!loop.run());
        ++rejected;
    };
    first_probe.reset();
    second_probe.reset();
    TEST_CHECK(EventLoopTestAccess::routes(loop) == 2);
    std::thread replace_first([&] {
        TEST_CHECK(loop.set_io_callback(first, Event_type::READ_EVENT, [](const IoContext&) {}));
    });
    std::thread replace_second([&] {
        TEST_CHECK(loop.set_io_callback(second, Event_type::READ_EVENT, [](const IoContext&) {}));
    });
    replace_first.join();
    replace_second.join();
    TEST_CHECK(first_destroying.is_open() && second_destroying.is_open());
    TEST_CHECK(rejected == 2);
    TEST_CHECK(first.valid() && second.valid());
    TEST_CHECK(first_registration.valid() && second_registration.valid());
    TEST_CHECK(EventLoopTestAccess::routes(loop) == 2);
    TEST_CHECK(loop.remove(first));
    TEST_CHECK(loop.remove(second));
    test::finish(loop);
    TEST_CHECK(done == 2);
    TEST_CHECK(EventLoopTestAccess::routes(loop) == 0);
}
