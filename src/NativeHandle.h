#pragma once

#include <unistd.h>

#include <utility>

namespace GST {
namespace BASE {

class NativeHandle {
public:
    static constexpr int INVALID_HANDLE = -1;

    NativeHandle() noexcept = default;

    explicit NativeHandle(int handle) noexcept
        : _handle(handle) {
    }

    NativeHandle(const NativeHandle&) = delete;
    NativeHandle& operator=(const NativeHandle&) = delete;

    NativeHandle(NativeHandle&& other) noexcept
        : _handle(std::exchange(other._handle, INVALID_HANDLE)) {
    }

    NativeHandle& operator=(NativeHandle&& other) noexcept {
        if (this != &other) {
            reset();
            _handle = std::exchange(other._handle, INVALID_HANDLE);
        }
        return *this;
    }

    virtual ~NativeHandle() {
        reset();
    }

    int native_fd() const noexcept {
        return _handle;
    }

    bool valid() const noexcept {
        return _handle >= 0;
    }

    explicit operator bool() const noexcept {
        return valid();
    }

    int release() noexcept {
        return std::exchange(_handle, INVALID_HANDLE);
    }

    void reset(int handle = INVALID_HANDLE) noexcept {
        if (_handle >= 0) {
            ::close(_handle);
        }
        _handle = handle;
    }

protected:
    void set_handle(int handle) noexcept {
        reset(handle);
    }

private:
    int _handle{INVALID_HANDLE};
};

}
}
