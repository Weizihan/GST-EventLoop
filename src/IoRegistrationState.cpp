#include "IoRegistrationState.h"

namespace GST {
namespace BASE {
namespace detail {

IoRegistrationState::~IoRegistrationState() {
    if (accepted && backend_detached && on_done) {
        // 不得在持有内部锁时释放最后一个状态引用。
        // 完成通知不保证由固定线程执行。
        try {
            on_done();
        } catch (...) {
        }
    }
}

}
}
}
