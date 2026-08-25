# GST_EventLoop

面向 C++17 Linux 应用的独立事件循环库，使用 epoll 等待 IO 事件，使用 eventfd 接收跨线程命令，并通过共享 callback 线程池执行用户任务。

GST_EventLoop 主要作为 `GST_net` 的底层事件驱动执行环境，也可以独立用于监听 socket、eventfd、pipe 等 Linux 文件描述符。

## 特性

- **Linux epoll 后端**：等待 Read、Write 和 Error 事件，普通用户注册使用 `EPOLLONESHOT`。
- **Engine 单线程拥有 IO 状态**：每个 Engine 的 Poller、注册表和 IO 命令只由自己的 owner 线程修改。
- **多 Engine 派发**：新 fd 按注册顺序轮询分配到 Engine；同一有效注册期间的重复 fd 固定到原 Engine。
- **跨线程 IO 操作**：`watch_io`、`update_io`、`set_io_callback` 和 `remove` 统一进入所属 Engine 的命令队列。
- **明确的启动边界**：run 前可以注册 fd；所有 Engine 启动成功后，EventLoop 才统一允许派发 callback。
- **稳定注册身份**：使用 `handle_id` 标识一次注册，不把可能复用的 fd 数字直接当成事件源身份。
- **callback 与 IO owner 隔离**：Engine owner 线程负责 epoll 和注册状态，用户 callback 在线程池执行。
- **同一 Handle 串行**：同一次 IO 注册最多执行一个 callback batch，完成后再回到所属 Engine rearm。
- **借用 native resource**：EventLoop 监听用户提供的 `NativeHandle`，但不负责关闭它。
- **可控停止**：支持 Soft 和 Hard 两种停止方式，并等待正在执行的 callback 返回。
- **可选 GST_log 集成**：可以使用 GST_log 输出 EventLoop 内部诊断，也可以完全独立构建。

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
│  ├─ lifecycle: init / run / stop                         │
│  ├─ fd registration identity and Engine routing          │
│  ├─ EventLoopEngine[0..N)                                │
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
| `Poller` | 封装 Linux epoll 的 add、update、remove 和 wait。 |
| `ThreadPool` | 执行用户 callback，不直接访问 Poller 或 Engine 注册表。 |
| `NativeHandle` | move-only 的 native fd 所有者。 |
| `IoHandle` | move-only 的 IO 注册凭证，不拥有 fd。 |
| `IoToken` | callback 可复制的非拥有注册身份，用于操作当前注册。 |
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

`IoHandle` 和 `IoToken` 保存所属 `engine_id`。后续 update、callback 替换和 remove 都会返回原 Engine 执行。

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

run 前的操作先进入启动队列，由 `run()` 按顺序执行。运行后的公开 IO 修改会等待所属 Engine 返回实际执行结果。

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

不同 Handle 的 callback 可以并发执行；同一 Handle 当前保持串行。callback 运行期间执行 update、替换 callback 或 remove，仍由所属 Engine 串行修改注册状态。

## 生命周期

典型生命周期：

```text
construct EventLoop
        ↓
init(config)
        ↓
watch_io(...) before run
        ↓
run()
        ↓
watch / update / remove while running
        ↓
stop(Soft or Hard)
        ↓
destroy EventLoop
```

`init()` 创建 callback worker 和 Engine 内部资源，但不会启动 epoll 等待。

`run()` 执行所有启动 IO 命令。任一 Engine 启动失败时，整体启动失败并回滚；只有全部 Engine 成功后才允许用户 callback 派发。

`stop()` 是阻塞式外部生命周期操作：停止 Engine、唤醒阻塞的 `epoll_wait()`、处理 callback pool 并等待相关线程退出。

EventLoop 应由外部控制线程停止和销毁，不应在自己的用户 callback 中调用 `init()`、`run()`、`stop()` 或析构 EventLoop。

## 环境要求

- Linux；
- 支持 C++17 的编译器；
- CMake 3.16 或更高版本；
- pthread。

## 构建

独立构建默认不启用 GST_log：

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
```

安装到指定目录：

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DCMAKE_INSTALL_PREFIX=/path/to/gst-eventloop
cmake --build build -j
cmake --install build
```

启用 GST_log：

```bash
cmake -S . -B build \
    -DCMAKE_BUILD_TYPE=Release \
    -DGST_EVENTLOOP_USE_GST_LOG=ON \
    -DGST_LOG_DIR=/path/to/GST_log
cmake --build build -j
```

## CMake 接入

当前推荐在同一个源码构建树中使用：

```cmake
set(GST_EVENTLOOP_USE_GST_LOG OFF CACHE BOOL "")
add_subdirectory(path/to/GST_EventLoop)

add_executable(my_app main.cpp)
target_link_libraries(my_app PRIVATE GST::EventLoop)
```

如果最外层工程已经创建了 `GST::Log` target，EventLoop 会直接复用它，
不会再次加载 GST_log：

```cmake
add_subdirectory(path/to/GST_log)

set(GST_EVENTLOOP_USE_GST_LOG ON CACHE BOOL "")
add_subdirectory(path/to/GST_EventLoop)

target_link_libraries(my_app PRIVATE GST::Log GST::EventLoop)
```

如果 EventLoop 独立构建时启用日志、且最外层尚未提供日志 target，EventLoop
才会通过 `GST_LOG_DIR` 加载 GST_log。

链接 `GST::EventLoop` 后，下游 target 会继承 C++17、pthread 和 EventLoop 头文件路径。

### 日志配置与生命周期

GST_log 由最外层应用统一配置和管理生命周期。GST_EventLoop 启用日志后，
复用最外层提供的日志 target 和 `GstLogger` 实例，不单独执行 `init()`、
`flush()` 或 `shutdown()`。`GST_EVENTLOOP_USE_GST_LOG` 只控制是否编译
EventLoop 内部日志。

## 快速开始

下面使用 eventfd 演示一个独立的可读事件源：

```cpp
#include "EventLoop.h"

#include <sys/eventfd.h>
#include <unistd.h>

#include <condition_variable>
#include <cstdint>
#include <mutex>

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

    std::mutex mutex;
    std::condition_variable cv;
    bool callback_finished = false;

    IoHandle handle = loop.watch_io(
        source,
        Event_type::READ_EVENT,
        [&](const IoContext&) {
            std::uint64_t value = 0;
            (void)::read(source.native_fd(), &value, sizeof(value));

            {
                std::lock_guard<std::mutex> lock(mutex);
                callback_finished = true;
            }
            cv.notify_one();
        });

    if (!handle.valid() || !loop.run()) {
        return 1;
    }

    std::uint64_t value = 1;
    if (::write(source.native_fd(), &value, sizeof(value)) != sizeof(value)) {
        loop.stop(ShutdownMode::Hard);
        return 1;
    }

    {
        std::unique_lock<std::mutex> lock(mutex);
        cv.wait(lock, [&] {
            return callback_finished;
        });
    }

    if (!loop.remove(handle)) {
        loop.stop(ShutdownMode::Hard);
        return 1;
    }

    loop.stop(ShutdownMode::Soft);
    return 0;
}
```

## Handle、Token 与资源所有权

`NativeHandle` 由调用者拥有。EventLoop 只借用其中的 fd，因此 `NativeHandle` 必须存活到对应注册成功 remove，或者 EventLoop 完成 stop。

`IoHandle` 表示一次注册：

- move-only；
- 不拥有 `NativeHandle`；
- remove 成功后失效；
- 保存 EventLoop generation、所属 Engine 和稳定 handle_id。

`IoContext::token` 是 callback 获得的可复制、非拥有身份。callback 可以用它操作自己的注册：

```cpp
IoHandle handle = loop.watch_io(
    source,
    Event_type::READ_EVENT,
    [&](const IoContext& context) {
        // 根据业务状态更新 interest。
        loop.update_io(context.token, Event_type::READ_EVENT);

        // 不再监听时也可以移除自身。
        // loop.remove(context.token);
    });
```

Token 不延长 EventLoop、注册或 native resource 的生命周期。注册删除或 EventLoop 重新 init 后，旧 Token 的操作会返回 `false`。

## Shutdown

### Soft

```cpp
loop.stop(ShutdownMode::Soft);
```

停止接受新的 IO 操作，停止 Engine，并执行完 callback pool 中已经排队和正在运行的任务。

### Hard

```cpp
loop.stop(ShutdownMode::Hard);
```

停止接受新的 IO 操作，丢弃 callback pool 中尚未开始的任务，但仍然等待正在运行的 C++ callback 自行返回。

应用通常应按以下顺序退出：

```text
停止产生新的网络操作
        ↓
移除或关闭 Server / Connection 注册
        ↓
EventLoop stop
        ↓
停止仍可能记录日志的业务线程
        ↓
GST_log shutdown
```

## 与 GST_net 的关系

GST_EventLoop 是 GST_net 的底层依赖：

```text
GST_net
   ↓
GST_EventLoop
```

EventLoop 负责 fd 等待、跨线程命令、callback 调度和生命周期；GST_net 负责 Server、Connection、读写缓冲、协议和网络对象所有权。EventLoop 不依赖 GST_net，也不要求启动完整 TCP Server 才能独立使用。

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
    ├── NativeHandle.h             # native fd 所有者
    ├── IoHandle.h                 # move-only 注册凭证
    ├── IoToken.h                  # callback 可复制注册身份
    ├── EventTypes.h               # 事件类型和位运算
    └── InternalLog.h              # 可选 GST_log 适配
```
