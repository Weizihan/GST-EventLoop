#include "TestSupport.h"

#include "EventLoopTestAccess.h"

#include <atomic>
#include <cstdint>
#include <sys/eventfd.h>
#include <thread>
#include <unistd.h>

using namespace GST::BASE;

namespace {

struct BatchFixture {
    NativeHandle source{eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC)};
    EventLoop loop;
    IoRegistration handle;
    std::shared_ptr<EventLoopEngine> engine;
    test::Gate done;

    explicit BatchFixture(EventLoop::IoCallback callback) {
        TEST_CHECK(source.valid());
        TEST_CHECK(loop.init());
        auto watched = loop.watch_io(source, Event_type::READ_EVENT, callback,
            [&] { done.open(); });
        handle.swap(watched);
        TEST_CHECK(handle.valid());
        TEST_CHECK(loop.set_io_callback(handle, Event_type::WRITE_EVENT, callback));
        engine = EventLoopTestAccess::engine(loop, 0);
        TEST_CHECK(loop.start());
    }

    EventLoopEngine::Task prepare(Event_type ready) {
        return EventLoopTestAccess::prepare(*engine, 1, ready);
    }

    ~BatchFixture() { test::finish(loop, ShutdownMode::Hard); }
};

}

TEST_CASE(engine_remove_before_single_callback_qualification) {
    std::atomic_int calls{0};
    BatchFixture fixture([&](const IoContext&) { ++calls; });
    test::Gate before;
    test::Gate release;
    EventLoopTestAccess::hooks(*fixture.engine,
        [&] { before.open(); release.wait(); }, {});
    auto task = fixture.prepare(Event_type::READ_EVENT);
    std::thread worker([&] { task(); });
    before.wait();
    TEST_CHECK(fixture.loop.remove(fixture.handle));
    TEST_CHECK(!fixture.done.is_open());
    release.open();
    worker.join();
    task = {};
    fixture.done.wait();
    TEST_CHECK(calls == 0);
}

TEST_CASE(engine_remove_after_qualification_allows_late_function_entry) {
    std::atomic_int calls{0};
    BatchFixture fixture([&](const IoContext&) { ++calls; });
    test::Gate qualified;
    test::Gate release;
    EventLoopTestAccess::hooks(*fixture.engine, {},
        [&] { qualified.open(); release.wait(); });
    auto task = fixture.prepare(Event_type::READ_EVENT);
    std::thread worker([&] { task(); });
    qualified.wait();
    TEST_CHECK(fixture.loop.remove(fixture.handle));
    TEST_CHECK(calls == 0);
    TEST_CHECK(!fixture.done.is_open());
    release.open();
    worker.join();
    task = {};
    fixture.done.wait();
    TEST_CHECK(calls == 1);
}

TEST_CASE(engine_self_remove_cancels_next_callback_in_same_batch) {
    std::atomic_int reads{0};
    std::atomic_int writes{0};
    EventLoop* loop = nullptr;
    BatchFixture fixture([&](const IoContext& context) {
        if (context.event_type == Event_type::READ_EVENT) {
            ++reads;
            TEST_CHECK(loop->remove(context.self));
        } else {
            ++writes;
        }
    });
    loop = &fixture.loop;
    auto task = fixture.prepare(Event_type::READ_EVENT | Event_type::WRITE_EVENT);
    task();
    task = {};
    fixture.done.wait();
    TEST_CHECK(reads == 1 && writes == 0);
}

TEST_CASE(engine_hard_during_read_cancels_unqualified_write_in_same_batch) {
    std::atomic_int reads{0};
    std::atomic_int writes{0};
    test::Gate entered;
    test::Gate release;
    test::Gate cutoff;
    BatchFixture fixture([&](const IoContext& context) {
        if (context.event_type == Event_type::READ_EVENT) {
            ++reads;
            entered.open();
            release.wait();
        } else {
            ++writes;
        }
    });
    EventLoopTestAccess::stop_hook(fixture.loop, [&] { cutoff.open(); });
    auto task = fixture.prepare(Event_type::READ_EVENT | Event_type::WRITE_EVENT);
    std::thread worker([&] { task(); });
    entered.wait();
    std::thread stopper([&] { test::finish(fixture.loop, ShutdownMode::Hard); });
    cutoff.wait();
    release.open();
    worker.join();
    task = {};
    stopper.join();
    fixture.done.wait();
    TEST_CHECK(reads == 1 && writes == 0);
}

TEST_CASE(engine_hard_after_write_qualification_preserves_callback) {
    std::atomic_int writes{0};
    test::Gate qualified;
    test::Gate release;
    test::Gate cutoff;
    BatchFixture fixture([&](const IoContext&) { ++writes; });
    EventLoopTestAccess::hooks(*fixture.engine, {},
        [&] { qualified.open(); release.wait(); });
    EventLoopTestAccess::stop_hook(fixture.loop, [&] { cutoff.open(); });
    auto task = fixture.prepare(Event_type::WRITE_EVENT);
    std::thread worker([&] { task(); });
    qualified.wait();
    std::thread stopper([&] { test::finish(fixture.loop, ShutdownMode::Hard); });
    cutoff.wait();
    TEST_CHECK(writes == 0);
    release.open();
    worker.join();
    task = {};
    stopper.join();
    fixture.done.wait();
    TEST_CHECK(writes == 1);
}

TEST_CASE(engine_old_ready_event_does_not_target_replacement_registration) {
    int old_calls = 0;
    std::atomic_int new_calls{0};
    BatchFixture fixture([&](const IoContext&) { ++old_calls; });
    auto old_task = fixture.prepare(Event_type::READ_EVENT);
    TEST_CHECK(fixture.loop.remove(fixture.handle));
    auto replacement = fixture.loop.watch_io(fixture.source, Event_type::READ_EVENT,
        [&](const IoContext&) { ++new_calls; });
    TEST_CHECK(replacement.valid());
    TEST_CHECK(EventLoopTestAccess::stale_event_ignored(*fixture.engine, 1));
    old_task();
    old_task = {};
    fixture.done.wait();
    auto new_task = EventLoopTestAccess::prepare(*fixture.engine, 2, Event_type::READ_EVENT);
    new_task();
    new_task = {};
    TEST_CHECK(old_calls == 0 && new_calls == 1);
    TEST_CHECK(fixture.loop.remove(replacement));
}

TEST_CASE(engine_same_registration_does_not_overlap_callback_batches) {
    test::Gate entered;
    test::Gate release;
    std::atomic_int calls{0};
    BatchFixture fixture([&](const IoContext&) {
        ++calls;
        entered.open();
        release.wait();
    });
    auto first = fixture.prepare(Event_type::READ_EVENT);
    std::thread worker([&] { first(); });
    entered.wait();
    // 首个批次尚未结束时，在 owner 侧再次查询就绪状态。
    TEST_CHECK(EventLoopTestAccess::stale_event_ignored(*fixture.engine, 1));
    TEST_CHECK(calls == 1);
    release.open();
    worker.join();
    first = {};
    TEST_CHECK(fixture.loop.remove(fixture.handle));
    fixture.done.wait();
}

TEST_CASE(engine_callback_replacement_preserves_already_prepared_batch_snapshot) {
    std::atomic_int old_calls{0};
    std::atomic_int new_calls{0};
    BatchFixture fixture([&](const IoContext&) { ++old_calls; });
    auto task = fixture.prepare(Event_type::READ_EVENT);
    TEST_CHECK(fixture.handle.set_cb(Event_type::READ_EVENT,
        [&](const IoContext&) { ++new_calls; }));
    // 已排队批次持有旧回调副本，替换只影响后续组装的新批次。
    task();
    task = {};
    TEST_CHECK(old_calls == 1 && new_calls == 0);
    TEST_CHECK(fixture.handle.cancel());
    fixture.done.wait();
}

TEST_CASE(engine_read_can_request_hard_and_cancel_unqualified_write) {
    std::atomic_int reads{0};
    std::atomic_int writes{0};
    EventLoop* loop = nullptr;
    BatchFixture fixture([&](const IoContext& context) {
        if (context.event_type == Event_type::READ_EVENT) {
            ++reads;
            loop->stop(ShutdownMode::Hard);
        } else {
            ++writes;
        }
    });
    loop = &fixture.loop;
    auto task = fixture.prepare(Event_type::READ_EVENT | Event_type::WRITE_EVENT);
    task();
    task = {};
    TEST_CHECK(fixture.loop.run());
    fixture.done.wait();
    TEST_CHECK(reads == 1 && writes == 0);
}
