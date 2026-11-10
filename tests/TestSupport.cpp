#include "TestSupport.h"

#include <cstdio>
#include <cstdlib>

namespace test {

std::vector<TestCase>& cases() {
    static std::vector<TestCase> registered;
    return registered;
}

Register::Register(const char* name, void (*run)()) {
    cases().push_back({name, run});
}

[[noreturn]] void fail(const char* expression, const char* file, int line) {
    std::fprintf(stderr, "%s:%d: failed: %s\n", file, line, expression);
    std::abort();
}

void Gate::open() {
    {
        std::lock_guard<std::mutex> lock(_mutex);
        _open = true;
    }
    _changed.notify_all();
}

void Gate::wait() {
    if (!wait_for(std::chrono::seconds(10))) {
        fail("gate reached", __FILE__, __LINE__);
    }
}

bool Gate::is_open() const {
    std::lock_guard<std::mutex> lock(_mutex);
    return _open;
}

}
