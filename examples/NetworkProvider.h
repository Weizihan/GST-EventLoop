#pragma once

#include "NativeHandle.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <system_error>

// 这些自由函数只属于示例的网络适配层，不是 Loop 的公开接口。
// 将示例移植到其他系统时，需要提供相同签名的网络实现；
// NativeHandle 只负责资源所有权，Loop 不负责创建或收发 TCP 连接。
namespace ExampleNetwork {

enum class IoStatus {
    Transferred,
    WouldBlock,
    EndOfStream,
    Error
};

struct IoResult {
    std::size_t bytes{0};
    IoStatus status{IoStatus::Error};
    std::error_code error;
};

GST::BASE::NativeHandle listen(
    const std::string& address,
    std::uint16_t port,
    std::error_code& error,
    int backlog = 128
);

GST::BASE::NativeHandle connect(
    const std::string& address,
    std::uint16_t port,
    std::error_code& error,
    std::chrono::milliseconds timeout = std::chrono::seconds(5)
);

GST::BASE::NativeHandle accept(
    const GST::BASE::NativeHandle& listener,
    std::error_code& error
);

IoResult recv(
    const GST::BASE::NativeHandle& resource,
    void* buffer,
    std::size_t size
) noexcept;

IoResult send(
    const GST::BASE::NativeHandle& resource,
    const void* buffer,
    std::size_t size
) noexcept;

std::uint16_t local_port(
    const GST::BASE::NativeHandle& listener,
    std::error_code& error
) noexcept;

}
