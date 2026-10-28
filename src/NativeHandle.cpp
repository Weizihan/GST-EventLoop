#include "NativeHandle.h"

#include <limits>
#include <utility>

#include <unistd.h>

namespace GST {
namespace BASE {

NativeHandle::NativeHandle(int handle) noexcept
    : _handle(handle >= 0 ? handle : INVALID_HANDLE) {
}

NativeHandle::NativeHandle(NativeHandle&& other) noexcept
    : _handle(std::exchange(other._handle, INVALID_HANDLE)) {
}

NativeHandle& NativeHandle::operator=(NativeHandle&& other) noexcept {
    if (this != &other) {
        reset();
        _handle = std::exchange(other._handle, INVALID_HANDLE);
    }
    return *this;
}

NativeHandle::~NativeHandle() {
    reset();
}

int NativeHandle::native_fd() const noexcept {
    if (_handle < 0 || _handle > std::numeric_limits<int>::max()) {
        return INVALID_HANDLE;
    }
    return static_cast<int>(_handle);
}

int NativeHandle::release() noexcept {
    const int handle = native_fd();
    _handle = INVALID_HANDLE;
    return handle;
}

void NativeHandle::reset(int handle) noexcept {
    if (_handle != INVALID_HANDLE) {
        // 当前后端中的 socket、管道等描述符统一使用 close。
        // 其他平台必须在实现内部使用对应资源的关闭操作。
        ::close(static_cast<int>(_handle));
    }
    _handle = handle >= 0 ? handle : INVALID_HANDLE;
}

}
}
