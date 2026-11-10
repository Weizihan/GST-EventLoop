# GST_EventLoop

面向 C++17 Linux 应用的独立事件循环库，使用 epoll 等待 IO 事件，使用 eventfd 接收跨线程 IO 命令，使用标准库调度单次和固定频率 Timer，并通过共享 callback 线程池执行用户任务。

GST_EventLoop 主要作为 `GST_net` 的底层事件驱动执行环境，也可以独立用于监听 socket、eventfd、pipe 等 Linux 文件描述符。

本轮从首次提交的源码作局部修复，保留 `src/` 下的原类、命名和职责。生命周期为 `init()` 初始化、`start()` 启动线程、`run()` 阻塞等待停止并收尾、`stop()` 提出停止请求。同时只允许一个活跃 Loop，完整停止后可以重新初始化。`NativeHandle` 统一表达调用者拥有的系统资源，当前实现支持 Linux fd；网络资源的创建和收发属于 GST_net 或其他资源提供层，Loop 不增加专用 SocketHandle 类。Timer 提供单次延迟、指定截止、固定频率周期、同步更新、回调快照替换和取消，当前没有 FixedDelay / FixedRate 选择参数或 Windows 后端。

## 特性

- **Linux epoll 后端**：等待 Read、Write 和 Error 事件，普通用户注册使用 `EPOLLONESHOT`。
- **Engine 单线程拥有 IO 状态**：每个 Engine 的 Poller、注册表和 IO 命令只由自己的 owner 线程修改。
- **多 Engine 派发**：新 fd 按注册顺序轮询分配到 Engine；同一有效注册期间的重复 fd 固定到原 Engine。
- **跨线程 IO 操作**：`watch_io`、`update_io`、`set_io_callback` 和 `remove` 进入所属 Engine 的命令队列；同 owner 重入直接执行，owner 跨 Engine 同步调用提前拒绝。
- **明确的启动边界**：start 前可以注册 fd；所有 Engine 启动成功后，EventLoop 才统一允许派发 callback。
- **稳定注册身份**：使用 `handle_id` 标识一次注册，不把可能复用的 fd 数字直接当成事件源身份。
- **callback 与 IO owner 隔离**：Engine owner 线程负责 epoll 和注册状态，用户 callback 在线程池执行。
- **同一注册串行**：同一次 IO 注册最多执行一个 callback batch，完成后再回到所属 Engine rearm。
- **标准库 Timer**：用 `std::chrono` 注册单次延迟、`steady_clock` 截止或固定频率周期；一条条件变量等待线程向共享 callback pool 投递，不固定间隔轮询。同 Timer 周期任务可排队和并发执行。
- **Timer 更新和回调快照**：Loop 与凭证提供同一 update/set_cb；新计划同步生效，已经选中的任务保留 deadline 和 callback 快照。
- **借用 native resource**：EventLoop 监听用户提供的 `NativeHandle`，但不负责关闭它。
- **可控停止**：外部线程和 callback 均可提出 Soft/Hard 停止请求；`run()` 等待相关工作结束并完成资源收尾。
- **可选 GST-Log 集成**：可以使用 GST-Log 输出 EventLoop 内部诊断，也可以完全独立构建。

## 适用场景

GST_EventLoop 适合作为以下组件的执行基础：

- TCP Server 和 Client；
- 监听 socket 与连接 socket；
- eventfd、pipe 等基于 fd 的事件源；
- 需要跨线程注册、更新或移除 IO 的后台服务；
- `GST_net` 等更高层网络库。

EventLoop 只提供事件等待、注册状态管理和 callback 调度，不包含 Server、Connection、Codec、协议或业务线程池等网络层概念。

## 架构

```text
                   application / GST_net
               watch / update / remove IO
                            │
                            ▼
┌──────────────────────────────────────────────────────────┐
│ EventLoop                                                │
│  ├─ lifecycle: init / start / run / stop                 │
│  ├─ fd registration identity and Engine routing          │
│  ├─ EventLoopEngine[0..N)                                │
│  ├─ TimerEngine: steady_clock + condition_variable       │
│  └─ shared callback ThreadPool                           │
└────────────────────────────┬─────────────────────────────┘
                             │ IO command
              ┌──────────────┼──────────────┐
              ▼              ▼              ▼
┌───────────────────┐ ┌───────────────────┐ ┌───────────────────┐
│ Engine 0          │ │ Engine 1          │ │ Engine N          │
│ owner thread      │ │ owner thread      │ │ owner thread      │
│ command queue     │ │ command queue     │ │ command queue     │
│ registration map  │ │ registration map  │ │ registration map  │
│ epoll + eventfd   │ │ epoll + eventfd   │ │ epoll + eventfd   │
└─────────┬─────────┘ └─────────┬─────────┘ └─────────┬─────────┘
          └─────────────────────┼─────────────────────┘
                                │ ReadyEvent
                                ▼
                    shared callback ThreadPool
                                │
                                │ callback-finished
                                ▼
                       original owner Engine
```

### 组件职责

| 组件 | 职责 |
|---|---|
| `EventLoop` | 对外门面，管理整体生命周期、Engine 路由、注册身份和共享 callback pool。 |
| `EventLoopEngine` | 在 owner 线程串行执行 IO 命令、维护注册表、等待事件并组织 callback 派发。 |
| `TimerEngine` | 一条调度线程等待最近的 steady-clock 截止，把单次/周期任务交给共享 callback pool，完成只管理生命周期。 |
| `Poller` | 封装 Linux epoll 的 add、update、remove 和 wait。 |
| `ThreadPool` | 执行用户 callback，不直接访问 Poller 或 Engine 注册表。 |
| `NativeHandle` | move-only 的系统资源所有者；当前 Linux 实现负责关闭其持有的 fd。资源创建和收发由资源提供层完成。 |
| `IoRegistration` | 可复制的非拥有 IO 注册凭证，外部和 callback 使用同一种类型，不拥有资源或 Loop。 |
| `TimerRegistration` | 可复制的非拥有 Timer 凭证，支持 valid/update/set_cb/cancel，析构不自动取消。 |
| `ReadyEvent` | Poller 返回的一次就绪结果，保存稳定 `handle_id` 和事件位。 |

## IO 执行模型

### 注册与 Engine 派发

新 fd 按注册顺序轮询分配：

```text
fd A → Engine 0
fd B → Engine 1
fd C → Engine 2
fd D → Engine 0
```

同一个 fd 的重复注册不会被分配到不同 Engine。它始终进入第一次选择的 Engine，因此不会被多个 epoll 实例同时监听。

`IoRegistration` 保存所属 `engine_id`。后续 update、callback 替换和 remove 都会返回原 Engine 执行。

### 跨线程命令

其他线程不直接调用 `epoll_ctl`，而是生成 IO 命令：

```text
application thread / callback worker
                 │
                 │ enqueue command
                 ▼
       Engine IO command queue
                 │
                 │ eventfd wakeup
                 ▼
          Engine owner thread
                 │
                 ▼
              epoll_ctl
```

start 前的操作先进入启动队列，由 `start()` 按顺序执行。运行后的公开 IO 修改会等待所属 Engine 返回实际执行结果。

### callback 与 rearm

用户 IO 使用 `EPOLLONESHOT`：

```text
epoll reports ReadyEvent
          ↓
Engine marks callback_running
          ↓
callback pool executes user callback
          ↓
callback-finished returns to owner Engine
          ↓
Engine rearms the latest interest
```

不同注册的 callback 可以并发执行；同一注册当前保持串行。callback 运行期间执行 update、替换 callback 或 remove，仍由所属 Engine 串行修改注册状态。

每个 READ、WRITE、ERROR 回调分别在本代共同执行锁和注册锁下检查 Hard 截止及撤销状态，成功检查的时刻就是取得执行资格的时刻。随后释放锁再调用用户函数。整个批次不一次性授权：READ 执行期间发生 Hard 或成功自删除，未获资格的 WRITE 不执行；已获资格的函数可以自然结束。

批次最后一个任务副本先销毁回调副本，再设置原注册上的完成标记并唤醒 owner。不再为这次通知动态创建 IO 命令。owner 每次等待返回后处理原 IO 队列和完成标记；内部 epoll 等待上界为 100ms，正常 eventfd 唤醒仍立即生效。

## 生命周期

典型生命周期：

```text
construct EventLoop
        ↓
init(config)
        ↓
watch_io(...) before start
        ↓
start()
        ↓
run() blocks the calling thread
        ↓
another thread or callback requests stop(Soft or Hard)
        ↓
run() completes shutdown and returns
        ↓
destroy EventLoop
```

`init()` 检查配置、准备 Engine、Poller、唤醒资源等内部状态，不启动线程。成功初始化后占用进程内唯一的活跃 Loop 名额；其他 Loop 的初始化失败，直到前一个 Loop 完整收尾。它不是自动创建的全局单例，应用仍自行持有对象并向其他模块传入引用。

`start()` 启动共享 callback pool、IO Engine 和 Timer 调度线程，执行启动 IO 命令。启动失败时整体回滚；只有各部分成功后才允许用户 callback 派发。`start()` 返回后 IO 和 Timer 回调就可以开始，不必等到进入 `run()`。

`run()` 阻塞调用线程，等待停止请求并组织统一收尾：停止 Engine、处理 callback pool、等待在途回调和线程退出、清理内部资源。返回是这一轮 Loop 生命周期结束的边界；`run()` 没有主线程用户任务队列，不提供 `post_main()`。

没有显式 start、也没有停止请求时，run 返回失败；不会替调用方偷偷启动线程。初始化后、启动前也可以请求 stop，再调用 run 清理未启动资源。一个调用方执行本轮 run 收尾，其他线程负责请求停止；不要同时销毁正在 run 的对象。

`stop()` 只提出请求，不等待线程退出；Hard 返回前，共同的回调执行资格截止点已经生效。它可以从外部线程、用户 IO/Timer callback 或 `on_done` 调用。Soft 请求可以升级成 Hard，不能由后来的 Soft 降级。

析构复用统一收尾路径：如果调用了 `start()` 却没有进入 `run()`，正常离开作用域时也会请求 Soft 停止并等待；已有 Hard 请求不会被降级。已经开始的用户回调仍须自然结束，所以析构不是无条件立即返回。不要在所属 callback、`on_done` 或用户捕获对象的析构中销毁 Loop，也不能在这些上下文调用 `run()` 等待自身结束。Loop 及其依赖对象须活到 `run()` 返回；跨线程使用者还须自行协调对象销毁，不能与析构并发调用其方法。

## 环境要求

- Linux；
- 支持 C++17 的编译器；
- CMake 3.16 或更高版本；
- pthread。

## 构建

独立构建时可以关闭 GST-Log：

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DGST_EVENTLOOP_USE_GST_LOG=OFF
cmake --build build -j
```

启用源码目录中的 GST-Log：

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DGST_EVENTLOOP_USE_GST_LOG=ON \
    -DGST_LOG_DIR=/path/to/GST-Log
cmake --build build -j
```

没有设置 `GST_LOG_DIR` 时，EventLoop 会查找已经安装的 `GSTLog` CMake 包。

## CMake 接入

外层工程可以先提供依赖 target，再加入 EventLoop：

```cmake
add_subdirectory(path/to/GST-Log)
set(GST_EVENTLOOP_USE_GST_LOG ON)
add_subdirectory(path/to/GST_EventLoop)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE GST::EventLoop)
```

如果外层没有提前创建 `GST::Log`，也可以给 EventLoop 提供源码目录：

```cmake
set(GST_EVENTLOOP_USE_GST_LOG ON)
set(GST_LOG_DIR "/path/to/GST-Log")
add_subdirectory(path/to/GST_EventLoop)

target_link_libraries(my_app PRIVATE GST::EventLoop)
```

EventLoop 始终优先复用外层已经存在的 `GST::Log`，不会重复加入 Log，也不会使用 `FORCE` 覆盖外层配置。重复执行 EventLoop 的 CMake 入口时，如果 `GST::EventLoop` 已经存在，也会直接复用现有 target。作为子项目时，EventLoop 的测试和示例默认关闭；外层可以显式设置 `GST_EVENTLOOP_BUILD_TESTS` 或 `GST_EVENTLOOP_BUILD_EXAMPLES`。

安装后可以通过 CMake 包接入：

```cmake
find_package(GSTEventLoop CONFIG REQUIRED)
target_link_libraries(my_app PRIVATE GST::EventLoop)
```

链接 `GST::EventLoop` 后，下游 target 会继承 C++17、pthread 和 EventLoop 头文件路径。若安装的 EventLoop 启用了 GST-Log，其包配置会同时查找 `GSTLog`，以满足静态库的链接依赖。

## 网络资源与 Loop 的用法

完整示例见 [examples/main.cpp](examples/main.cpp)：创建数字 IPv4 TCP listener、连接、accept、收发 echo、更新 WRITE interest、自删除，最后通过 `on_done` 关闭资源。main 使用 `NativeHandle`、`init/watch_io/start/run`，回调完成或失败时调用 `stop()`，另有外部监护线程负责超时请求。main 不包含 Linux 头、eventfd、裸 fd 或系统读写调用。示例专用的 NetworkProvider 自由函数承担 TCP 资源操作，Linux 实现放在 `examples/NetworkProvider.cpp`，它不是 Loop 安装的公开 API，也不要求为网络资源另建 Handle 类。

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Debug
cmake --build build -j
ctest --test-dir build --output-on-failure
./build/GST_EventLoop_example
```

该示例使用数字 IPv4 TCP listener 和已连接 TCP 字节流。listener 的 READ 表示可以尝试 accept；连接的 READ 表示数据、EOF 或错误可被观察，WRITE 表示可尝试发送。就绪是提示，资源层必须处理部分收发、暂不可读写、EOF 和错误；Loop 不提供这些收发操作，也不提供协议、帧或业务对象。

`NativeHandle` 的通用名称不表示任何系统资源都能被监听。当前 watch_io 只接收 Linux epoll 能监听的 fd，普通磁盘文件等资源不在支持范围内；未来 Windows 的 socket 就绪、文件完成通知和其他 HANDLE 也不能默认具有相同语义。后续后端需要明确支持的资源类别及 READ/WRITE 契约，并补齐资源关闭、完整宽度的资源路由和 Loop 内部唤醒。

目标是将来保留调用方的 Loop 注册、更新、删除和生命周期调用；当前尚不能承诺整个程序只更新 Loop 并重新编译就迁移 Windows。示例 main 的平台细节已经隔离，但 NetworkProvider 仍须移植；当前 GST_net 也仍有 Linux 公开类型，需要网络层自己的移植。没有执行 Windows 编译或运行验证，CMake 明确只支持 Linux。

原 `NativeHandle(int)`、`native_fd()`、`release()`、`reset(int)` 保留为 Linux 兼容入口。旧程序现在无需改这些调用，但直接使用这些入口及 Linux 创建/收发操作的代码仍不具备跨平台保证。`NativeHandle` 的实现移到 `.cpp`，单独包含该头的程序现在也必须链接 `GST::EventLoop`；不承诺旧二进制 ABI。

## Linux 兼容示例

下面使用 eventfd 演示一个独立的可读事件源：

```cpp
#include "EventLoop.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <cstdint>

int main() {
    using namespace GST::BASE;

    NativeHandle source(::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC));
    if (!source.valid()) {
        return 1;
    }

    EventLoopConfig config;
    config.engine_num = 2;
    config.callback_thread_num = 2;

    EventLoop loop;
    if (!loop.init(config)) {
        return 1;
    }

    IoRegistration registration = loop.watch_io(
        source,
        Event_type::READ_EVENT,
        [&](const IoContext& context) {
            std::uint64_t value = 0;
            (void)::read(source.native_fd(), &value, sizeof(value));
            context.self.cancel();
        },
        [&] {
            source.reset(); // 确认解绑和相关回调结束后，由资源所有者关闭。
            loop.stop(ShutdownMode::Soft);
        });

    if (!registration.valid()) {
        return 1;
    }

    std::uint64_t value = 1;
    if (::write(source.native_fd(), &value, sizeof(value)) != sizeof(value)) {
        loop.stop(ShutdownMode::Hard);
        return 1;
    }

    if (!loop.start()) {
        return 1;
    }
    return loop.run() ? 0 : 1; // 阻塞主线程，直到停止与收尾完成。
}
```

## Timer

Timer 阶段 2 在单次基础上增加固定频率周期、同步更新和回调快照替换；各次交付及周期异常处理修正的验证结果分开记录，见文末记录。

Timer 使用标准 C++ 时间类型，不需要 fd、timerfd 或其他 Linux 公开类型。用户函数仍在现有共享 callback pool 执行；Timer 调度线程只等待和投递。

```cpp
#include "EventLoop.h"

#include <chrono>

int main() {
    using namespace GST::BASE;
    using namespace std::chrono;

    EventLoop loop;
    if (!loop.init()) {
        return 1;
    }

    TimerRegistration timer = loop.add_timer(
        TimerSchedule::after(milliseconds(100)),
        [](const TimerContext& context) {
            // context.deadline 是计划截止，不是实际函数开始时刻。
            (void)context;
        },
        [&] {
            // 单次注册和相关任务结束后通知一次；这里可以请求停止。
            loop.stop(ShutdownMode::Soft);
        });

    if (!timer.valid() || !loop.start()) {
        return 1;
    }
    return loop.run() ? 0 : 1;
}
```

`TimerSchedule::after(delay)` 从注册时刻计算截止；start 前注册也如此，启动时已经过期就尽快投递。`TimerSchedule::at(std::chrono::steady_clock::time_point)` 直接指定单调时钟截止，过去的时刻有效。负 delay 或空 callback 注册失败；零 delay 有效。Timer 不早于计划截止投递，但线程调度和共享池排队都可能增加延迟，没有硬实时保证。多 Timer 按截止顺序投递；多个 worker 的实际函数入口和完成顺序可能不同。

外部线程和当前回调使用同一种凭证：`timer.cancel()` 等价于 `loop.remove(timer)`，`timer.update(schedule)` 等价于 `loop.update_timer(timer, schedule)`，`timer.set_cb(cb)` 等价于 `loop.set_timer_callback(timer, cb)`。`TimerContext::self` 是当前 Timer 的凭证。凭证可复制、只弱引用内部状态，析构不取消，也不推迟完成通知。取消成功后所有副本失效；取消不等待已经取得执行资格的用户函数，不能强行中断它。已入池但尚未取得资格的任务在取消后跳过；已取得资格的函数自然结束。重复取消、已结束 Timer、旧代凭证或 Loop 销毁后的凭证操作返回 false。**Loop 的 stop 请求生效后，cancel、update 和 set_cb 直接返回 false。**

`on_done` 等待注册不再产生任务，且相关回调和业务捕获退休后通知一次；它不是取消返回即完成的保证。通知线程不固定，可能是 Timer 线程、worker、取消调用方或外部收尾线程，也可能在 cancel 返回前同步调用。业务回调、通知和捕获清理都不在 Timer 状态锁下执行；既有生命周期收尾仍由自己的操作锁与重入检查保护。通知及其捕获析构可以请求 stop，不能在所属回调、通知或受管清理里销毁 Loop，或等待包含自己的完成。`on_done` 返回后才释放它自身的捕获对象。

Soft Stop 对 Timer 以非空批次成功进入共享池为接收点：已接收任务排空，停止截止后的新投递被拒绝。Timer 完整投递与 stop 请求有同步顺序；一次状态检查后再无保护地投递不构成截止。Hard Stop 与 IO 共用执行资格门禁，尚未取得资格的用户函数跳过，已取得资格的自然结束。Timer 调度线程在共享池停止前被唤醒并 join，TimerEngine 在共享池任务退出后才销毁；IO 原有的停止与派发机制保持不变。

用户 Timer callback 中逃出的标准或非标准 C++ 异常被包装器捕获，使用现有日志宏记录，不会终止调度线程或共享池。异常规则以本次任务选中时的计划快照为准：周期旧回调即使其 Timer 已被 update 改为单次，异常仍取消整个 Timer；单次旧回调异常只记录，不撤销 update 接续的新计划。周期异常关闭后续调度，其他已取得资格的回调自然结束，未取得资格的按取消门禁跳过；即使同 Timer 任务已被 Soft Stop 接收，未取得资格时仍受这次异常撤销。`on_done` 等所有应等待的任务及业务捕获退休后才通知一次。不增加异常策略参数，Stop 后公开 cancel 仍返回 false。关闭 GST 日志的构建中这些宏为空，不保证默认可见诊断。

### 固定频率周期

`TimerSchedule::every(interval)` 要求 interval > 0，首截止为 add_timer 入口采样时刻 + interval。之后按固定时间表推进，不等待前一个回调返回，不因它还在执行或共享池没有空闲 worker 而跳过本次正常周期；任务可以排队，同 Timer 的多个回调也可以并发执行。同一 callback payload 会被多个在途任务共享，mutable lambda 的内部捕获也可能同时被访问，应用须自行同步；快照不可变不代表捕获对象线程安全。当前共享池未增加背压或队列上限，生产持续快于执行时任务可以积压。

```cpp
// 保留 registration，业务结束时调用 registration.cancel()。
auto registration = loop.add_timer(TimerSchedule::every(std::chrono::seconds(1)),
    [](const TimerContext& context) {
        // context.deadline 是该次计划点；实际执行可能因共享池排队而更晚。
        (void)context;
    });
```

调度线程自身迟到时，每次处理一个到期项，并把下一截止推进到严格晚于该次调度采样时刻、且与固定相位对齐的最小计划点；跳过中间历史点，不补发。回调耗时、worker 排队、完成通知和负载析构不参与下一截止计算。没有 FixedDelay / FixedRate 选择参数。

### 更新和回调替换

```cpp
registration.update(TimerSchedule::after(std::chrono::seconds(5)));
registration.update(TimerSchedule::every(std::chrono::milliseconds(500)));
registration.set_cb([](const TimerContext& context) {
    // 可从自身回调通过 context.self.update(...) / set_cb(...) / cancel() 操作。
    (void)context;
});
```

update 接受有效 after/at/every，可以单次↔周期转换。after/every 的新首次截止从 update 入口采样时刻起算，at 保留指定时刻；true 表示新计划同步可靠提交，不能成功后静默丢失。有效单次即使正在排队或执行，也能 update 重新挂计划；已经彻底退休则返回 false。有 pending 节点时更新复用现有 set 节点；已选中单次重新挂计划需分配新节点，分配失败可抛 `std::bad_alloc`，保留旧配置和原在途任务，不会先返回 true 再无声失败。

update 与调度选择以同一调度锁排序：旧任务一旦选中并登记在途，即使尚未 post、尚未获资格或正在执行，也允许按其旧 deadline/callback 快照完成（仍受取消/Hard 资格门禁）；更新改变其后的计划。旧任务退休不会覆盖新计划，也不阻塞新截止产生任务；旧选择随后投递失败也不会覆盖已经成功 update 的新计划。

set_cb 在锁外准备不可变回调快照并同步替换后续配置；已经选中、排队或执行的任务保留本次快照，之后新选任务取得新快照。替换与当次选择并发时，按取得快照的同步顺序可能使用旧或新回调；不会在锁内复制或释放最后用户负载。

on_done 仍是整注册的一次终结通知：周期先停止产生任务，再等待所有应等待的多在途任务和快照退休。每批次先释放本次回调快照，再减少在途计数；自然失效要等没有 future 且计数归零，取消仍立即使凭证失效。凭证失效、单次回调返回或一次周期任务完成都不等于整注册资源已经清理。Stop 后所有 Timer 管理修改都拒绝；取消/Hard 跳过未资格任务，已经取得资格的函数自然结束。

## IoRegistration 与资源所有权

`NativeHandle` 由调用者拥有并负责释放系统资源，EventLoop 只借用它。它不负责网络连接、accept 或数据收发，也不表示一次 IO 注册；一项资源由什么模块创建，与 Loop 如何监听它是两件事。Loop 自己的 epoll/eventfd 是内部资源，调用者不需要取得或操作它们。

remove/cancel 不等待已经取得资格的回调，因此不能在它返回后立即关闭 fd。可以在四参数 `watch_io(resource, interest, callback, on_done)` 的 `on_done` 中释放资源；或者先停止并 join 所有调用方操作线程，请求停止后等 `run()` 完成收尾再释放。`stop()` 返回本身不代表资源已经可以关闭。三个参数的调用形式仍保留，返回类型统一为 `IoRegistration`。

`on_done` 在注册时设置，等待监听解绑，以及相关批次、命令、回调副本和捕获对象析构后通知一次，Hard 不丢弃它。通知线程不固定，可能是 owner、worker 或外部清理线程，也可能在取消返回前同步触发；执行时不持有内部锁。通知可以请求 stop，但不要在里面析构所属 Loop，或等待包含自己的完成条件。

`IoRegistration` 表示一次注册，外部和 callback 使用同一种类型：

- 可以复制，各副本指向同一次注册；
- 不拥有 `NativeHandle`、EventLoop 或内部注册状态，析构不自动取消，也不推迟 `on_done`；
- remove 成功后所有副本的 `valid()` 都返回 false；这不表示在途 callback 已经结束；
- 保存所属代次、Engine 和稳定注册身份，旧副本不会作用于重新初始化后的新注册。

`IoContext::self` 就是当前注册的 `IoRegistration`。创建注册仍由 Loop 完成；已有注册的更新、回调替换和取消既可以通过 Loop，也可以通过注册对象：

```cpp
IoRegistration registration = loop.watch_io(
    source,
    Event_type::READ_EVENT,
    [&](const IoContext& context) {
        // 根据业务状态更新 interest。
        context.self.update(Event_type::READ_EVENT);

        // 不再监听时也可以移除自身。
        // context.self.cancel();
    });

IoRegistration copy = registration;
// copy.cancel(); 或 loop.remove(copy); 成功后所有副本都失效。
// copy.set_cb(Event_type::READ_EVENT, another_callback);
// 等价于 loop.set_io_callback(copy, Event_type::READ_EVENT, another_callback)。
```

注册删除或 EventLoop 重新 init 后，旧凭证的更新、替换和删除操作返回 `false`。默认构造的凭证无效。

`set_cb(type, callback)` 只影响之后新组装的事件批次，已经排队或运行的批次继续持有旧回调副本。它是持续替换本次注册的对应回调，不表示“只执行一次”。两个公开入口使用同一内部同步和有效性检查，不存在两套注册状态。

Loop 收尾会关闭注册管理入口，并等待已接受的管理操作及其用户负载退出，再销毁 Engine。旧凭证在 Loop 销毁后调用 valid/set_cb/update/cancel 会安全失败；直接调用 Loop 方法的线程仍须保证对象存活，不能与其析构并发。

start 前仍保留命令排队：remove/cancel 返回 true 表示删除命令成功入队，同时逻辑撤销该注册，使所有副本立即失效并阻止后续取得回调资格；实际后端解绑由 owner 执行，资源仍须保留到 `on_done`。运行期 remove/cancel 返回 true 表示删除执行成功，DEL 失败仍返回 false。所有启动命令执行完才允许派发。

### 旧接口迁移

本次按用户确认合并注册凭证，不保留旧类型别名或旧重载：

| 旧用法 | 当前用法 |
|---|---|
| `#include "IoHandle.h"` / `#include "IoToken.h"` | `#include "IoRegistration.h"`，或直接包含 `EventLoop.h` |
| 显式类型 `IoHandle` / `IoToken` | `IoRegistration` |
| `context.token` | `context.self` |
| `watch_io/update_io/set_io_callback/remove` | 函数名不变，注册类型统一为 `IoRegistration` |

使用 `auto registration = loop.watch_io(...)` 的调用方无需改返回变量类型。原 Loop 上的三个操作入口保留，新增注册成员操作是同一实现的便利入口。

生命周期调用需要迁移：原来只调用 `run()` 启动线程的程序须先调用 `start()`；原来在 `run()` 返回后继续执行的逻辑应移到适当回调或其他线程；原来把 `stop()` 返回当作收尾完成的程序须改为等待 `run()` 返回。正常析构也会完成收尾，但调用方不能在销毁 Loop 的同时继续调用其方法。

## Shutdown

### Soft

```cpp
loop.stop(ShutdownMode::Soft);
```

提出优雅停止请求，拒绝新的注册管理调用并唤醒 `run()`。Timer 在请求生效时关闭新投递，已成功入共享池的任务排空；IO 保持原有 owner 停止和投递规则。显式 remove/cancel 在停止前已经完成的撤销仍生效，Soft 不会恢复已撤销注册；停止请求生效后 Timer cancel/update/set_cb 返回 false。

### Hard

```cpp
loop.stop(ShutdownMode::Hard);
```

提出停止请求，并在返回前建立所有 Engine 共用的执行资格截止。未取得单次执行资格的 callback 被跳过，排队批次的负载仍安全析构；已取得资格的 callback 自然结束，由 `run()` 等待它们返回。不会强制终止 C++ 函数。回调内部也可以调用 Hard；后来的 Hard 可以升级已经请求或正在收尾的 Soft。

应用通常应按以下顺序退出：

```text
停止并等待调用方网络操作线程退出
        ↓
移除或关闭 Server / Connection 注册
        ↓
EventLoop stop 请求 → run 返回，确认收尾结束
        ↓
停止仍可能记录日志的业务线程
        ↓
GST-Log shutdown
```

## 与 GST_net 的关系

GST_EventLoop 是 GST_net 的底层依赖：

```text
GST_net
   ↓
GST_EventLoop
```

EventLoop 负责资源就绪等待、跨线程命令、callback 调度和注册生命周期；GST_net 或其他资源提供层负责 socket 创建、listen/connect/accept、收发、缓冲和协议。资源可以由 `NativeHandle` 管理，Loop 只借用；`IoRegistration` 只标识注册。EventLoop 不依赖 GST_net，也不要求启动完整 TCP Server 才能独立使用。本轮未修改 GST_net；它若仍使用旧注册类型或 `context.token`，需要按上述表格迁移，不能直接按旧源码编译。

## 目录结构

```text
GST_EventLoop/
├── CMakeLists.txt
├── README.md
└── src/
    ├── EventLoop.h/.cpp           # 公开门面、生命周期和 Engine 路由
    ├── EventLoopEngine.h/.cpp     # owner 线程、命令队列和事件派发
    ├── Poller.h/.cpp              # Linux epoll 封装
    ├── ThreadPool.h/.cpp          # 共享 callback 执行器
    ├── NativeHandle.h/.cpp        # 资源所有者，系统调用留在实现
    ├── IoRegistrationState.h      # 私有注册资格和最终通知状态
    ├── IoRegistration.h           # 外部与 callback 共用的可复制注册凭证
    ├── TimerTypes.h               # after / at / every 调度类型
    ├── TimerRegistration.h        # 可复制的非拥有 Timer 凭证
    ├── TimerRegistrationState.h   # 私有 Timer 资格与完成状态
    ├── TimerEngine.h/.cpp         # 单调时钟、条件变量和固定频率调度
    ├── EventTypes.h               # 事件类型和位运算
    └── InternalLog.h              # 可选 GST-Log 适配
```

## 故障与验证边界

DEL 失败时 remove 仍按原版返回 false，不擅自改变其含义；调用者保持资源，停止时再关闭对应 Poller。若 DEL 与 Poller 关闭都不能确认成功，`on_done` 保守不通知，当前没有公开故障查询或完整恢复状态机。底层 Poller 硬故障仍有待进一步完善，不能把 100ms 等待称为全面故障恢复。

此前验证环境拒绝创建 AF_INET socket，TCP 示例未完成握手/accept 运行验证，不能把内部 AF_UNIX 资源测试说成 TCP 集成通过。当前代码也没有 Windows 后端或通用 Windows 资源释放实现。详细的原版文件判断、接口差异、真实测试记录和未满足条件见 [本轮修改记录](docs/original-rework.md)。

加入 Timer 前的生命周期调整曾通过 Debug、Release、ASan/UBSan（关闭泄漏检测）、TSan 各 55/55，以及非测试安装和外部使用方验证。这是历史 IO 基线记录；Timer 阶段 1 的独立验证结果另见 [阶段 1 追加记录](docs/original-rework.md#timer-阶段-1单次定时与生命周期)。LeakSanitizer 和完整 TCP 成功路径未完成验证。

周期异常处理修正前的 **120/120、28 项和 280/280** 保留为历史，其中异常测试采用了错误的“继续周期”预期，不能证明本轮修正。按既定异常取消设计纠正实现与测试后，本轮 Debug、Release、ASan/UBSan、TSan 均 **125/125**（其他既有 55 + 单次 37 + 周期 33）。详细选项和未测边界见 [周期异常处理修正记录](docs/original-rework.md#周期异常处理修正记录)。阶段 1 结果以及此前安装、外部使用方和公开头隔离检查均是历史，本轮未重测这些安装项。

Timer 阶段 3 已补齐四项有界资源与压力验收，原有 125 项保留；Debug、Release、ASan/UBSan、TSan 的全套 **129/129 各连续通过 20 遍，共 10,320 次**。实际环境为 GCC 15.2.0、glibc 2.43，均开启严格警告。4096 项注册取消、4096 项同截止到期、64 个 1ms 周期和单 worker 阻塞下的 1024 项排队取消均完成计数与资源清理检查，实测数据、完整命令及边界见 [阶段 3 收尾记录](docs/original-rework.md#timer-阶段-3收尾验证)。ASan 仍关闭泄漏检测；LeakSanitizer、Windows 和完整 TCP 成功路径未验证，有限压力运行不代表长期无泄漏或队列容量保证。
