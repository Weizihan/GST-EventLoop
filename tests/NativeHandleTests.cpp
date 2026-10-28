#include "TestSupport.h"

#include "NativeHandle.h"
#include "Poller.h"

#include <cstdint>
#include <fcntl.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

using namespace GST::BASE;

TEST_CASE(native_handle_move_preserves_ownership) {
    const int fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    TEST_CHECK(fd >= 0);
    {
        NativeHandle first(fd);
        NativeHandle second(std::move(first));
        TEST_CHECK(!first.valid());
        TEST_CHECK(second.native_fd() == fd);
        TEST_CHECK(fcntl(fd, F_GETFD) >= 0);
    }
    TEST_CHECK(fcntl(fd, F_GETFD) < 0);
}

TEST_CASE(native_handle_release_returns_ownership) {
    NativeHandle source(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    TEST_CHECK(source.valid());
    const int fd = source.release();
    TEST_CHECK(!source.valid());
    TEST_CHECK(fcntl(fd, F_GETFD) >= 0);
    TEST_CHECK(close(fd) == 0);
}

TEST_CASE(native_handle_move_assignment_retires_previous_resource) {
    const int first_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    const int second_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    TEST_CHECK(first_fd >= 0 && second_fd >= 0);
    {
        NativeHandle first(first_fd);
        NativeHandle second(second_fd);
        first = std::move(second);
        TEST_CHECK(!second.valid());
        TEST_CHECK(fcntl(first_fd, F_GETFD) < 0);
        TEST_CHECK(first.native_fd() == second_fd);
        TEST_CHECK(fcntl(second_fd, F_GETFD) >= 0);
    }
    TEST_CHECK(fcntl(second_fd, F_GETFD) < 0);
}

TEST_CASE(native_handle_reset_keeps_released_resource_with_caller) {
    NativeHandle source(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    TEST_CHECK(source.valid());
    const int caller_fd = source.release();
    const int replacement_fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    TEST_CHECK(replacement_fd >= 0 && replacement_fd != caller_fd);
    source.reset(replacement_fd);
    source.reset();
    TEST_CHECK(!source.valid());
    TEST_CHECK(fcntl(replacement_fd, F_GETFD) < 0);
    TEST_CHECK(fcntl(caller_fd, F_GETFD) >= 0);
    TEST_CHECK(close(caller_fd) == 0);
}

TEST_CASE(native_handle_owns_pipe_and_socket_resources) {
    int pipe_fds[2];
    int socket_fds[2];
    TEST_CHECK(pipe(pipe_fds) == 0);
    TEST_CHECK(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                         0, socket_fds) == 0);
    {
        NativeHandle input(pipe_fds[0]);
        NativeHandle output(pipe_fds[1]);
        NativeHandle first(socket_fds[0]);
        NativeHandle second(socket_fds[1]);
        TEST_CHECK(input.valid() && output.valid() && first.valid() && second.valid());
    }
    TEST_CHECK(fcntl(pipe_fds[0], F_GETFD) < 0);
    TEST_CHECK(fcntl(pipe_fds[1], F_GETFD) < 0);
    TEST_CHECK(fcntl(socket_fds[0], F_GETFD) < 0);
    TEST_CHECK(fcntl(socket_fds[1], F_GETFD) < 0);
}

TEST_CASE(poller_reports_stable_registration_key) {
    NativeHandle source(eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    TEST_CHECK(source.valid());
    Poller poller;
    TEST_CHECK(poller.init());
    TEST_CHECK(poller.add(source.native_fd(), 1234, Event_type::READ_EVENT, true));
    const std::uint64_t one = 1;
    TEST_CHECK(write(source.native_fd(), &one, sizeof(one)) == sizeof(one));
    const auto events = poller.wait();
    TEST_CHECK(events.size() == 1);
    TEST_CHECK(events.front().key == 1234);
    TEST_CHECK(has_event(events.front().type, Event_type::READ_EVENT));
    TEST_CHECK(poller.remove(source.native_fd()));
    TEST_CHECK(fcntl(source.native_fd(), F_GETFD) >= 0);
}
