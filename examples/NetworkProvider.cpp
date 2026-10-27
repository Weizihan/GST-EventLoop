#include "NetworkProvider.h"

#include <algorithm>
#include <cerrno>
#include <limits>

#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>

namespace ExampleNetwork {
namespace {

using GST::BASE::NativeHandle;

std::error_code socket_error() noexcept {
    return std::error_code(errno, std::generic_category());
}

bool make_address(
    const std::string& address,
    std::uint16_t port,
    sockaddr_in& native_address,
    std::error_code& error
) {
    native_address = {};
    native_address.sin_family = AF_INET;
    native_address.sin_port = htons(port);
    if (::inet_pton(AF_INET, address.c_str(), &native_address.sin_addr) != 1) {
        error = std::make_error_code(std::errc::invalid_argument);
        return false;
    }
    return true;
}

bool make_nonblocking(int handle, std::error_code& error) {
    const int flags = ::fcntl(handle, F_GETFL, 0);
    if (flags < 0 || ::fcntl(handle, F_SETFL, flags | O_NONBLOCK) != 0) {
        error = socket_error();
        return false;
    }
    const int descriptor_flags = ::fcntl(handle, F_GETFD, 0);
    if (descriptor_flags < 0 ||
        ::fcntl(handle, F_SETFD, descriptor_flags | FD_CLOEXEC) != 0) {
        error = socket_error();
        return false;
    }
    return true;
}

IoResult transfer_result(ssize_t result, bool receive) noexcept {
    if (result > 0) {
        return {static_cast<std::size_t>(result), IoStatus::Transferred, {}};
    }
    if (result == 0) {
        return {0, receive ? IoStatus::EndOfStream : IoStatus::Transferred, {}};
    }
    if (errno == EAGAIN || errno == EWOULDBLOCK) {
        return {0, IoStatus::WouldBlock,
                std::make_error_code(std::errc::operation_would_block)};
    }
    return {0, IoStatus::Error, socket_error()};
}

}

GST::BASE::NativeHandle listen(
    const std::string& address,
    std::uint16_t port,
    std::error_code& error,
    int backlog
) {
    error.clear();
    sockaddr_in native_address{};
    if (backlog <= 0 || !make_address(address, port, native_address, error)) {
        error = std::make_error_code(std::errc::invalid_argument);
        return {};
    }

    NativeHandle handle(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!handle.valid()) {
        error = socket_error();
        return {};
    }
    const int reuse_address = 1;
    if (::setsockopt(handle.native_fd(), SOL_SOCKET, SO_REUSEADDR,
                     &reuse_address, sizeof(reuse_address)) != 0 ||
        !make_nonblocking(handle.native_fd(), error)) {
        if (!error) {
            error = socket_error();
        }
        return {};
    }
    if (::bind(handle.native_fd(), reinterpret_cast<const sockaddr*>(&native_address),
               sizeof(native_address)) != 0 ||
        ::listen(handle.native_fd(), backlog) != 0) {
        error = socket_error();
        return {};
    }
    return handle;
}

GST::BASE::NativeHandle connect(
    const std::string& address,
    std::uint16_t port,
    std::error_code& error,
    std::chrono::milliseconds timeout
) {
    error.clear();
    sockaddr_in native_address{};
    if (timeout.count() < 0 || !make_address(address, port, native_address, error)) {
        error = std::make_error_code(std::errc::invalid_argument);
        return {};
    }

    NativeHandle handle(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    if (!handle.valid()) {
        error = socket_error();
        return {};
    }
    if (!make_nonblocking(handle.native_fd(), error)) {
        return {};
    }

    int result = ::connect(handle.native_fd(),
                           reinterpret_cast<const sockaddr*>(&native_address),
                           sizeof(native_address));
    if (result != 0 && errno != EINPROGRESS && errno != EINTR) {
        error = socket_error();
        return {};
    }
    if (result != 0) {
        // 所有等待及 EINTR 重试共用同一截止时间，避免重试延长总等待。
        const auto maximum_timeout = std::chrono::milliseconds(std::numeric_limits<int>::max());
        const auto bounded_timeout = std::min(timeout, maximum_timeout);
        const auto deadline = std::chrono::steady_clock::now() + bounded_timeout;
        for (;;) {
            const auto remaining = deadline - std::chrono::steady_clock::now();
            const auto milliseconds = std::chrono::duration_cast<std::chrono::milliseconds>(remaining);
            const int wait_time = remaining <= decltype(remaining)::zero() ? 0 :
                static_cast<int>(milliseconds.count() + (milliseconds < remaining ? 1 : 0));
            pollfd pending{handle.native_fd(), POLLOUT, 0};
            result = ::poll(&pending, 1, wait_time);
            if (result < 0 && errno == EINTR) {
                if (std::chrono::steady_clock::now() < deadline) {
                    continue;
                }
                error = std::make_error_code(std::errc::timed_out);
                return {};
            }
            if (result < 0) {
                error = socket_error();
                return {};
            }
            if (result == 0) {
                error = std::make_error_code(std::errc::timed_out);
                return {};
            }

            int connect_error = 0;
            socklen_t error_size = sizeof(connect_error);
            if (::getsockopt(handle.native_fd(), SOL_SOCKET, SO_ERROR,
                             &connect_error, &error_size) != 0) {
                error = socket_error();
                return {};
            }
            if (connect_error != 0) {
                error = std::error_code(connect_error, std::generic_category());
                return {};
            }
            break;
        }
    }
    return handle;
}

GST::BASE::NativeHandle accept(
    const GST::BASE::NativeHandle& listener,
    std::error_code& error
) {
    error.clear();
    if (!listener.valid()) {
        error = std::make_error_code(std::errc::bad_file_descriptor);
        return {};
    }

    int descriptor;
    do {
        descriptor = ::accept(listener.native_fd(), nullptr, nullptr);
    } while (descriptor < 0 && errno == EINTR);
    if (descriptor < 0) {
        error = errno == EAGAIN || errno == EWOULDBLOCK ?
            std::make_error_code(std::errc::operation_would_block) : socket_error();
        return {};
    }

    NativeHandle handle(descriptor);
    if (!make_nonblocking(descriptor, error)) {
        return {};
    }
    return handle;
}

IoResult recv(
    const GST::BASE::NativeHandle& resource,
    void* buffer,
    std::size_t size
) noexcept {
    if (!resource.valid()) {
        return {0, IoStatus::Error, std::make_error_code(std::errc::bad_file_descriptor)};
    }
    if (size == 0) {
        return {0, IoStatus::Transferred, {}};
    }
    ssize_t result;
    do {
        result = ::recv(resource.native_fd(), buffer,
                        std::min(size, static_cast<std::size_t>(std::numeric_limits<int>::max())), 0);
    } while (result < 0 && errno == EINTR);
    return transfer_result(result, true);
}

IoResult send(
    const GST::BASE::NativeHandle& resource,
    const void* buffer,
    std::size_t size
) noexcept {
    if (!resource.valid()) {
        return {0, IoStatus::Error, std::make_error_code(std::errc::bad_file_descriptor)};
    }
    if (size == 0) {
        return {0, IoStatus::Transferred, {}};
    }
    // 示例采用 sendmsg 抑制 SIGPIPE；这些平台细节不出现在 main 中。
    iovec buffer_view{const_cast<void*>(buffer),
                      std::min(size, static_cast<std::size_t>(std::numeric_limits<int>::max()))};
    msghdr message{};
    message.msg_iov = &buffer_view;
    message.msg_iovlen = 1;
    ssize_t result;
    do {
        result = ::sendmsg(resource.native_fd(), &message, MSG_NOSIGNAL);
    } while (result < 0 && errno == EINTR);
    return transfer_result(result, false);
}

std::uint16_t local_port(
    const GST::BASE::NativeHandle& listener,
    std::error_code& error
) noexcept {
    error.clear();
    if (!listener.valid()) {
        error = std::make_error_code(std::errc::bad_file_descriptor);
        return 0;
    }
    sockaddr_in address{};
    socklen_t address_size = sizeof(address);
    if (::getsockname(listener.native_fd(), reinterpret_cast<sockaddr*>(&address),
                      &address_size) != 0) {
        error = socket_error();
        return 0;
    }
    return ntohs(address.sin_port);
}

}
