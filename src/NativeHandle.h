#pragma once

#include <cstdint>

namespace GST {
namespace BASE {

// 系统资源的独占所有者；EventLoop 只借用资源，不负责关闭。
// 当前实现支持 Linux 描述符，平台关闭操作放在实现文件中。
class NativeHandle {
public:
    static constexpr int INVALID_HANDLE = -1;

    NativeHandle() noexcept = default;

    // Linux 兼容入口；资源层可在内部接管原生描述符，
    // 再以 NativeHandle 交给调用方和 EventLoop。
    explicit NativeHandle(int handle) noexcept;

    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;

    NativeHandle(NativeHandle&& other) noexcept;
    NativeHandle& operator=(NativeHandle&& other) noexcept;

    virtual ~NativeHandle();

    // Linux 兼容入口；使用 Loop 的代码无需取出原生描述符。
    int native_fd() const noexcept;

    bool valid() const noexcept {
        return _handle != INVALID_HANDLE;
    }

    explicit operator bool() const noexcept {
        return valid();
    }

    // 以下原生值操作也属于 Linux 兼容入口。
    int release() noexcept;
    void reset(int handle = INVALID_HANDLE) noexcept;

protected:
    void set_handle(int handle) noexcept {
        reset(handle);
    }

private:
    // 使用可容纳指针宽度的存储；当前公开的原生接管入口仍仅支持 Linux fd。
    // 未来后端还需实现对应资源的表示、有效性和关闭规则。
    std::intptr_t _handle{INVALID_HANDLE};
};

}
}
