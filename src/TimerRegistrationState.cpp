#include "TimerRegistrationState.h"

namespace GST {
namespace BASE {
namespace detail {

TimerRegistrationState::~TimerRegistrationState() {
    // 最后引用不在状态或队列锁内释放；业务捕获先于完成通知析构。
    callback.reset();
    if (accepted && on_done) {
        // 与 IO 一致，不保证由固定线程执行。
        try {
            on_done();
        } catch (...) {
        }
    }
}

}
}
}
