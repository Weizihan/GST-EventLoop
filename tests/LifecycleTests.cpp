#include "TestSupport.h"

#include "EventLoop.h"
#include "EventLoopTestAccess.h"

#include <atomic>
#include <cstdint>
#include <dirent.h>
#include <memory>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

using namespace GST::BASE;

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

std::size_t thread_count() {
    DIR* directory = opendir("/proc/self/task");
    TEST_CHECK(directory != nullptr);
    std::size_t count = 0;
    while (const auto entry = readdir(directory)) {
        if (entry->d_name[0] != '.') ++count;
    }
    TEST_CHECK(closedir(directory) == 0);
    return count;
}

template <typename Predicate>
void await_state(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate()) {
        TEST_CHECK(std::chrono::steady_clock::now() < deadline);
        std::this_thread::yield();
    }
}

struct DestructionAction {
    std::function<void()> action;
    ~DestructionAction() { if (action) action(); }
};

}

TEST_CASE(lifecycle_init_prepares_resources_without_starting_threads) {
    const auto before = thread_count();
    EventLoop loop;
    EventLoopConfig config;
    config.engine_num = 2;
    config.callback_thread_num = 2;
    TEST_CHECK(loop.init(config));
    TEST_CHECK(thread_count() == before);
    TEST_CHECK(!EventLoopTestAccess::owner_running(
        *EventLoopTestAccess::engine(loop, 0)));
    TEST_CHECK(!EventLoopTestAccess::owner_running(
        *EventLoopTestAccess::engine(loop, 1)));
    TEST_CHECK(!loop.run());
    TEST_CHECK(loop.start());
    TEST_CHECK(EventLoopTestAccess::owner_running(
        *EventLoopTestAccess::engine(loop, 0)));
    TEST_CHECK(EventLoopTestAccess::owner_running(
        *EventLoopTestAccess::engine(loop, 1)));
    test::Gate worker;
    const auto caller = std::this_thread::get_id();
    TEST_CHECK(EventLoopTestAccess::post_worker(loop, [&] {
        TEST_CHECK(std::this_thread::get_id() != caller);
        worker.open();
    }));
    worker.wait();
    test::finish(loop);
}

TEST_CASE(lifecycle_concurrent_init_has_one_winner_until_cleanup) {
    EventLoop first;
    EventLoop second;
    test::Gate ready_first;
    test::Gate ready_second;
    test::Gate begin;
    bool first_result = false;
    bool second_result = false;
    std::thread first_thread([&] {
        ready_first.open();
        begin.wait();
        first_result = first.init();
    });
    std::thread second_thread([&] {
        ready_second.open();
        begin.wait();
        second_result = second.init();
    });
    ready_first.wait();
    ready_second.wait();
    begin.open();
    first_thread.join();
    second_thread.join();
    TEST_CHECK(first_result != second_result);
    EventLoop& winner = first_result ? first : second;
    EventLoop& loser = first_result ? second : first;
    winner.stop();
    // 停止请求不等于已收尾，只有 run 返回后才释放活跃名额。
    TEST_CHECK(!loser.init());
    TEST_CHECK(winner.run());
    TEST_CHECK(loser.init());
    TEST_CHECK(loser.start());
    test::finish(loser);
}

TEST_CASE(lifecycle_failed_init_rolls_back_active_slot) {
    EventLoop failed;
    test::set_allocation_failure(0);
    const bool initialized = failed.init();
    test::clear_allocation_failure();
    TEST_CHECK(!initialized);
    TEST_CHECK(!failed.start());
    EventLoop next;
    TEST_CHECK(next.init());
    test::finish(next);
    TEST_CHECK(failed.init());
    TEST_CHECK(failed.start());
    test::finish(failed);
}

TEST_CASE(lifecycle_run_waits_for_other_thread_stop_and_callback_cleanup) {
    auto source = make_source();
    test::Gate entered;
    test::Gate release;
    test::Gate returned;
    test::Gate done;
    EventLoop loop;
    TEST_CHECK(loop.init());
    auto registration = loop.watch_io(source, Event_type::READ_EVENT,
        [&](const IoContext&) {
            consume_source(source);
            entered.open();
            release.wait();
        }, [&] { done.open(); });
    TEST_CHECK(loop.start());
    signal_source(source);
    entered.wait();
    std::thread runner([&] {
        TEST_CHECK(loop.run());
        returned.open();
    });
    await_state([&] { return EventLoopTestAccess::run_active(loop); });
    TEST_CHECK(!returned.is_open());
    // 已获资格的回调仍阻塞；stop 必须只提交请求，不能等它结束。
    loop.stop(ShutdownMode::Hard);
    TEST_CHECK(EventLoopTestAccess::hard_stopping(loop));
    TEST_CHECK(!returned.is_open() && !done.is_open());
    TEST_CHECK(!loop.watch_io(source, Event_type::READ_EVENT,
                              [](const IoContext&) {}).valid());
    release.open();
    runner.join();
    TEST_CHECK(returned.is_open() && done.is_open());
    TEST_CHECK(!registration.valid());
}

TEST_CASE(lifecycle_stop_before_start_cleans_pending_registration_in_run) {
    auto source = make_source();
    std::atomic_int calls{0};
    std::atomic_int done{0};
    EventLoop loop;
    TEST_CHECK(loop.init());
    auto registration = loop.watch_io(source, Event_type::READ_EVENT,
        [&](const IoContext&) { ++calls; }, [&] { ++done; });
    TEST_CHECK(registration.valid());
    signal_source(source);
    loop.stop(ShutdownMode::Hard);
    // 后续 Soft 请求不能降级既有 Hard 截止，包括析构的默认停止请求。
    loop.stop(ShutdownMode::Soft);
    TEST_CHECK(EventLoopTestAccess::hard_stopping(loop));
    TEST_CHECK(!loop.start());
    TEST_CHECK(loop.run());
    TEST_CHECK(calls == 0 && done == 1 && !registration.valid());
    TEST_CHECK(loop.run());
}

TEST_CASE(lifecycle_stop_before_run_still_completes_shared_pool) {
    auto source = make_source();
    test::Gate entered;
    test::Gate release;
    std::atomic_int done{0};
    EventLoop loop;
    TEST_CHECK(loop.init());
    auto registration = loop.watch_io(source, Event_type::READ_EVENT,
        [&](const IoContext&) {
            consume_source(source);
            entered.open();
            release.wait();
        }, [&] { ++done; });
    TEST_CHECK(loop.start());
    signal_source(source);
    entered.wait();
    loop.stop();
    TEST_CHECK(done == 0);
    release.open();
    TEST_CHECK(loop.run());
    TEST_CHECK(done == 1 && !registration.valid());
}

TEST_CASE(lifecycle_destructor_waits_for_callback_and_capture_before_releasing_slot) {
    auto source = make_source();
    test::Gate entered;
    test::Gate release_callback;
    test::Gate destroying;
    test::Gate release_capture;
    test::Gate shutdown;
    test::Gate destroyed;
    std::atomic_int done{0};
    auto loop = std::make_unique<EventLoop>();
    TEST_CHECK(loop->init());
    auto capture = std::make_shared<DestructionAction>();
    capture->action = [&] {
        destroying.open();
        release_capture.wait();
        TEST_CHECK(done == 0);
    };
    auto registration = loop->watch_io(source, Event_type::READ_EVENT,
        [&, capture](const IoContext&) {
            consume_source(source);
            entered.open();
            release_callback.wait();
        }, [&] { ++done; });
    capture.reset();
    EventLoopTestAccess::stop_hook(*loop, [&] { shutdown.open(); });
    TEST_CHECK(loop->start());
    signal_source(source);
    entered.wait();
    std::thread deleter([owned = std::move(loop), &destroyed]() mutable {
        owned.reset();
        destroyed.open();
    });
    shutdown.wait();
    EventLoop next;
    TEST_CHECK(!next.init());
    TEST_CHECK(!destroyed.is_open() && done == 0);
    release_callback.open();
    destroying.wait();
    TEST_CHECK(!next.init());
    TEST_CHECK(!destroyed.is_open() && done == 0);
    release_capture.open();
    deleter.join();
    TEST_CHECK(done == 1 && !registration.valid());
    TEST_CHECK(next.init());
    TEST_CHECK(next.start());
    test::finish(next);
}

TEST_CASE(lifecycle_done_can_request_hard_stop_without_lifecycle_deadlock) {
    auto source = make_source();
    test::Gate done;
    EventLoop loop;
    TEST_CHECK(loop.init());
    auto registration = loop.watch_io(source, Event_type::READ_EVENT,
        [](const IoContext&) {}, [&] {
            loop.stop(ShutdownMode::Hard);
            TEST_CHECK(!loop.init());
            TEST_CHECK(!loop.start());
            TEST_CHECK(!loop.run());
            done.open();
        });
    TEST_CHECK(loop.start());
    test::finish(loop);
    TEST_CHECK(done.is_open() && !registration.valid());
}

TEST_CASE(lifecycle_owner_capture_destruction_can_request_stop) {
    auto source = make_source();
    test::Gate stopped;
    EventLoop loop;
    TEST_CHECK(loop.init());
    auto capture = std::make_shared<DestructionAction>();
    capture->action = [&] {
        loop.stop(ShutdownMode::Hard);
        stopped.open();
    };
    auto registration = loop.watch_io(source, Event_type::READ_EVENT,
        [capture](const IoContext&) {});
    capture.reset();
    TEST_CHECK(loop.start());
    TEST_CHECK(loop.set_io_callback(registration, Event_type::READ_EVENT,
                                    [](const IoContext&) {}));
    stopped.wait();
    TEST_CHECK(EventLoopTestAccess::hard_stopping(loop));
    TEST_CHECK(loop.run());
    TEST_CHECK(!registration.valid());
}

TEST_CASE(lifecycle_registration_members_share_loop_operations_and_self_cancel) {
    auto source = make_source();
    test::Gate done;
    std::atomic_int old_calls{0};
    std::atomic_int new_calls{0};
    EventLoop loop;
    TEST_CHECK(loop.init());
    IoRegistration empty;
    TEST_CHECK(!empty.cancel());
    TEST_CHECK(!empty.update(Event_type::READ_EVENT));
    TEST_CHECK(!empty.set_cb(Event_type::READ_EVENT, [](const IoContext&) {}));
    auto registration = loop.watch_io(source, Event_type::READ_EVENT,
        [&](const IoContext&) { ++old_calls; }, [&] { done.open(); });
    const auto copy = registration;
    TEST_CHECK(copy.set_cb(Event_type::READ_EVENT, [&](const IoContext& context) {
        consume_source(source);
        ++new_calls;
        TEST_CHECK(context.self.cancel());
        TEST_CHECK(!context.self.valid() && !copy.valid());
    }));
    TEST_CHECK(copy.update(Event_type::READ_EVENT));
    TEST_CHECK(loop.update_io(registration, Event_type::READ_EVENT));
    TEST_CHECK(loop.start());
    signal_source(source);
    done.wait();
    TEST_CHECK(old_calls == 0 && new_calls == 1);
    TEST_CHECK(!copy.cancel());
    TEST_CHECK(!copy.update(Event_type::READ_EVENT));
    TEST_CHECK(!copy.set_cb(Event_type::READ_EVENT, [](const IoContext&) {}));
    test::finish(loop);
}

TEST_CASE(lifecycle_registration_members_are_safe_after_loop_destruction) {
    auto source = make_source();
    IoRegistration old;
    std::atomic_int done{0};
    {
        EventLoop loop;
        TEST_CHECK(loop.init());
        old = loop.watch_io(source, Event_type::READ_EVENT,
            [](const IoContext&) {}, [&] { ++done; });
        TEST_CHECK(old.valid());
        // 未调用 start 或 run，析构也必须结清已接受的注册。
    }
    TEST_CHECK(done == 1 && !old.valid());
    TEST_CHECK(!old.cancel());
    TEST_CHECK(!old.update(Event_type::READ_EVENT));
    TEST_CHECK(!old.set_cb(Event_type::READ_EVENT, [](const IoContext&) {}));
    EventLoop next;
    TEST_CHECK(next.init());
    auto current = next.watch_io(source, Event_type::READ_EVENT,
        [](const IoContext&) {});
    TEST_CHECK(current.valid());
    TEST_CHECK(!old.cancel() && current.valid());
    test::finish(next);
}

TEST_CASE(lifecycle_shutdown_waits_for_in_flight_registration_management) {
    auto source = make_source();
    test::Gate destroying;
    test::Gate release_capture;
    test::Gate replaced;
    test::Gate returned;
    test::Gate done;
    EventLoop loop;
    TEST_CHECK(loop.init());
    auto capture = std::make_shared<DestructionAction>();
    capture->action = [&] {
        TEST_CHECK(!loop.init());
        TEST_CHECK(!loop.start());
        TEST_CHECK(!loop.run());
        destroying.open();
        release_capture.wait();
        // 收尾已关闭访问门禁并等待本次替换；请求停止不应反过来等 owner。
        loop.stop(ShutdownMode::Hard);
    };
    auto registration = loop.watch_io(source, Event_type::READ_EVENT,
        [capture](const IoContext&) {}, [&] { done.open(); });
    capture.reset();
    TEST_CHECK(loop.start());
    std::thread replacer([&] {
        TEST_CHECK(registration.set_cb(Event_type::READ_EVENT,
                                       [](const IoContext&) {}));
        replaced.open();
    });
    destroying.wait();
    std::thread runner([&] {
        TEST_CHECK(loop.run());
        returned.open();
    });
    await_state([&] { return EventLoopTestAccess::run_active(loop); });
    loop.stop();
    await_state([&] {
        return EventLoopTestAccess::access_closed_with_calls_in_flight(loop);
    });
    TEST_CHECK(!returned.is_open() && !replaced.is_open() && !done.is_open());
    EventLoop next;
    TEST_CHECK(!next.init());
    release_capture.open();
    replacer.join();
    runner.join();
    TEST_CHECK(replaced.is_open() && returned.is_open() && done.is_open());
    TEST_CHECK(!registration.valid());
    TEST_CHECK(next.init());
    test::finish(next);
}

TEST_CASE(lifecycle_failed_start_rolls_back_threads_and_active_slot) {
    EventLoop loop;
    TEST_CHECK(loop.init());
    test::set_allocation_failure(0);
    const bool started = loop.start();
    test::clear_allocation_failure();
    TEST_CHECK(!started);
    EventLoop next;
    TEST_CHECK(next.init());
    TEST_CHECK(next.start());
    test::finish(next);
    TEST_CHECK(loop.init());
    TEST_CHECK(loop.start());
    test::finish(loop);
}

TEST_CASE(lifecycle_stop_during_start_is_not_overwritten_or_dispatched) {
    auto source = make_source();
    test::Gate stopped;
    std::atomic_int calls{0};
    std::atomic_int done{0};
    EventLoop loop;
    TEST_CHECK(loop.init());
    auto capture = std::make_shared<DestructionAction>();
    capture->action = [&] {
        loop.stop(ShutdownMode::Hard);
        stopped.open();
    };
    auto registration = loop.watch_io(source, Event_type::READ_EVENT,
        [&, capture](const IoContext&) { ++calls; }, [&] { ++done; });
    capture.reset();
    TEST_CHECK(registration.set_cb(Event_type::READ_EVENT,
                                   [&](const IoContext&) { ++calls; }));
    signal_source(source);
    // owner 在 startup 队列中替换旧回调，其析构明确早于 start 返回。
    TEST_CHECK(loop.start());
    TEST_CHECK(stopped.is_open());
    TEST_CHECK(EventLoopTestAccess::hard_stopping(loop));
    TEST_CHECK(!loop.start());
    TEST_CHECK(loop.run());
    TEST_CHECK(calls == 0 && done == 1 && !registration.valid());
    EventLoop next;
    TEST_CHECK(next.init());
    test::finish(next);
}
