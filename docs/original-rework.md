# 基于原版的局部修改记录

## 当前生命周期与注册入口

用户在原版重做、移除 SocketHandle、统一 IoRegistration 之后，确认了以下生命周期调整。本节是当前契约；下方 33/34/39 项测试表保留各历史阶段的实际记录，不能作为此次生命周期调整后的重新验证结果。

| 接口或状态 | 当前行为 |
|---|---|
| `init()` | 准备配置、Engine、Poller 和唤醒资源，不启动线程；成功后占用进程内唯一的活跃 Loop 名额。 |
| `start()` | 启动共享线程池和各 Engine，全部成功后统一开放 IO 回调派发；不隐式进入主线程等待。 |
| `run()` | 阻塞调用线程，等待停止请求并组织收尾，相关工作结束后返回；没有主线程用户任务队列。 |
| `stop(Soft/Hard)` | 外部线程、用户回调和 on_done 均可提出请求，不等待线程退出；Hard 返回前建立共同资格截止。 |
| 完整收尾 | 等待相关回调自然结束，清理内部线程和资源，释放活跃 Loop 名额；同一对象可以重新 init。 |
| 析构 | 复用统一收尾，默认请求 Soft，不降级已存在的 Hard；不能从所属回调、on_done 或相关用户析构中销毁 Loop。 |

只有 init、尚未 start 时，直接 run 返回失败；应明确调用 start。若已在这一阶段请求 stop，run 可以完成未启动资源的收尾。只调用 start 后正常离开作用域也由析构兜底，但析构仍可能等待在途用户回调，并非立即返回。

创建 IO 注册仍由 `loop.watch_io(...)` 完成。已有注册保留 Loop 入口，同时增加注册对象上的便利方法；它们走同一内部检查和操作路径：

| Loop 入口 | 注册对象入口 |
|---|---|
| `loop.set_io_callback(registration, type, callback)` | `registration.set_cb(type, callback)` |
| `loop.update_io(registration, interest)` | `registration.update(interest)` |
| `loop.remove(registration)` | `registration.cancel()` |

回调替换仅影响随后新组装的批次，已排队或执行的批次保留旧副本。on_done 仍在注册时设置、只在相关使用和解绑结清后通知、不固定线程；公开注册副本不延后通知。旧注册不能访问已经销毁的 Loop，停止、重新初始化和新对象不会让旧身份重新有效。

调用方迁移：原来以 run 启动线程的程序增加 start；原来 run 返回后执行的逻辑移到回调或其他线程；原来等 stop 返回再释放资源的程序改为 on_done 或 run 完成收尾后释放。资源所有权、跨 owner 同步拒绝、逐回调资格、Soft 排空和 Hard 取消未获资格回调的规则保持。本轮没有加入 post_main、帧循环、Timer 或 Windows 后端，也不修改 GST_net。

注册成员入口通过本代执行状态取得短暂的 Loop 访问资格，再读取注册状态；公开凭证只持弱引用。收尾先拒绝新的管理调用，再等待已接受的管理调用及其用户负载析构结束，随后停止 owner 和线程池。访问计数只保护管理入口与 Loop 销毁的交错，不是回调工作计数，也不拥有 fd。失效或旧代凭证不会解引用已经销毁的 Loop；直接使用 `EventLoop&` 的线程仍须自行保证对象存活。

### 本次生命周期调整的实际验证

在保留原 39 项测试的基础上，增加 14 项生命周期测试和 2 项 Engine 测试，共 55 项。以下各构建均在最后一次实现修改后重新构建并运行，不是沿用下文历史结果。

| 本次验证 | 实际结果 |
|---|---|
| Debug，Wall/Wextra/Wpedantic/Werror | 55/55 |
| Release，同上 | 55/55 |
| ASan/UBSan，detect_leaks=0、halt_on_error=1 | 55/55；不包含 LeakSanitizer |
| TSan，halt_on_error=1 | 55/55 |
| BUILD_TESTING=OFF，Release、Werror，独立前缀安装 | 通过，仅安装当前公开头和静态库 |
| 安装后的外部 C++17 使用方，不定义测试宏 | 编译、链接、init/start/stop/run/reinit 运行通过 |
| 安装后的 TCP 示例 | 编译、链接通过；实际运行仍在创建 listener 时被环境以 Operation not permitted 拒绝 |
| 安装公开头和示例 main，屏蔽 Linux 宏并遮挡系统 IO 头 | 语法检查通过；不等价于 Windows 编译或运行 |

新增确定性交错覆盖：init 不启动线程；并发 init 只有一个成功者；init/start 失败回滚名额；run 等待外部停止与在途回调；start 前或 run 前请求停止；未调用 run 时析构等待回调及捕获对象；on_done 和 owner 析构请求停止；管理调用与统一收尾交错；启动期间请求 Hard 不被覆盖；注册成员自删除及 Loop 销毁后的旧凭证安全拒绝；替换不改变已排队批次；READ 内部 Hard 跳过同批次 WRITE。使用现有门闩、条件变量和故障注入，超时只报告卡死。

本次没有重跑 GST_log 开启构建、GST_net 编译或 Windows 配置拒绝检查；下文对应结果只属于各历史阶段。LeakSanitizer 仍未完成，TCP 成功路径仍需在允许创建网络 socket 的 Linux 环境验证。

## 基线与 Git 状态

源码基线为首次提交 `84e2ef9a248c7f8d7ffabf992c9bef30679958a6`。实施前从该提交导出工作区，并用 `git diff 84e2ef9 -- CMakeLists.txt README.md src` 确认一致，再进行局部修改。没有使用 v2 或 simplified 的目录、命名、接口或整文件实现作为模板。

源码重做阶段，用户要求的真正 Git 回退未能执行：当时环境将 `.git` 设为只读，`git reset --hard 84e2ef9...` 无法创建 `.git/index.lock`。因此当时分支仍是 `feat/p0-p1-registration-control`，HEAD 仍为 `2b59213f5e707b7e785b8c3edf2677868314ff12`。源码已经从首次提交重做，但这不代表 HEAD 已经回退。该阶段没有提交、推送或切换分支；后续里程碑的实际分支和提交以 Git 记录为准。

修改前的完整工作区备份在仓库外：

`/home/ubuntu/.codex/visualizations/2026/10/19/01a1552a-8c2d-7ff0-9865-d04afd1f7e25/original-rework.EqKnJ2/before-rework.tar.gz`

后两次提交的实际作用：

| 提交 | 内容与本轮取舍 |
|---|---|
| `5f9b35d` | CMake/README：日志默认关闭、Tlog 改 GST::Log、复用外层日志目标、补安装规则、移除指向不存在文件的测试配置。本轮保留这些有效构建修正。 |
| `2b59213` | 主要修改 Engine stop 布尔参数命名并同步调整条件及调用点，另有注释；公开 Soft/Hard 映射基本未变。本轮保留首次提交的 `drain_pending_io` 命名，不照搬这次重命名。 |

## 文件保留和删减依据

源码重做时保留原版 13 个 `src/` 文件在原位置，没有建立 `include/`、`core/`、`io/`、`platform/`。继续使用 `GST::BASE`、原核心类名、函数名、`_成员` 和 `IoContext::event_type`。随后用户批准将两种 IO 注册凭证合并为一个 `IoRegistration`，原 `IoHandle.h`、`IoToken.h` 由同目录的 `IoRegistration.h` 替代，`IoContext::token` 改为 `self`；没有套用 v2 的其他类或目录。

| 文件 | 实际修改 |
|---|---|
| `EventLoop.h/.cpp` | 保留原类、路由和计数；增加每代共享 Hard 截止、局部重入拒绝及可选完成通知重载。原版重做阶段曾保留非阻塞 run/阻塞 stop，后按用户确认改为 init/start/run/请求式 stop 及单活跃 Loop。 |
| `EventLoopEngine.h/.cpp` | 保留 owner、IO 命令队列、注册表、startup/dispatch 流程；修复逐回调准入、回调替换析构、批次退休和跨 owner 同步等待。 |
| `Poller.h/.cpp` | 保留 add/update/remove/wait 和稳定 PollKey；增加显式 close 及内部 100ms 等待。 |
| `ThreadPool.h/.cpp` | 保留原 start/stop/post/post_batch；队列节点在锁外准备后整批发布，取消负载在锁外析构。 |
| 新 `IoRegistration.h` | 按用户确认合并原 IoHandle/IoToken；外部和 callback 共用可复制、非拥有凭证，valid 读取同一注册状态。旧两文件和对应重载移除，不留兼容别名；后续增加 set_cb/update/cancel 便利入口。 |
| `NativeHandle.h` | 保留 Linux int 构造、native_fd/release/reset、move、虚析构和 set_handle；OS 调用移到同目录实现。 |
| 新 `NativeHandle.cpp` | 承载资源移动/释放/关闭实现，避免公开头要求 unistd；保留 Linux fd 兼容入口，不包含 socket 创建或收发操作。 |
| 新 `IoRegistrationState.h` | 私有共享资格与完成状态；由原注册记录和已接受命令/批次持有，公开身份只弱引用。没有独立完成信箱、公开错误快照或手工工作计数。 |
| `EventTypes.h`、`InternalLog.h` | 与首次提交一致。 |
| CMake、README | 修复实际构建、安装及过期语义；保留原源码集成方式和 Linux 兼容示例。 |
| 新 `examples/main.cpp`、`NetworkProvider.h/.cpp` | TCP listener/connect/accept/echo，main 使用 NativeHandle 和原 Loop 调用；Linux 网络操作隔离在示例专用实现，不作为 Loop 安装 API。 |
| 新 `tests/` | 复用早期测试注册、门闩和线程局部分配故障工具，保留有效并发覆盖。 |

具体删减只有有依据的路径：批次完成不再动态创建 `post_internal_io_state_task`，改为原注册上的完成标记和唤醒；删去 CMake 中指向不存在文件的 stress/benchmark 配置。原版没有独立完成信箱、IoCompletion 或故障隔离文件，不能把它们说成本轮从原版删除的内容。

保留 IO 队列，因为它承担 owner 独占注册表和 start 前命令排队；保留 generation，因为允许重新 init；保留 fd 路由与 reservation_count，因为重复注册和并发失败回滚需要它们；保留 callback_running 和 EPOLLONESHOT，因为它们实现同注册串行。未实现的并发策略枚举保持原来的显式拒绝，避免无必要的公开 API 破坏。

## 行为与接口

`watch_io` 的三参数形式保留，并增加可选完成通知的四参数重载；用户确认合并凭证后，两者统一返回 `IoRegistration`：

```cpp
IoRegistration EventLoop::watch_io(
    const NativeHandle& native_handle,
    Event_type interest,
    IoCallback cb,
    std::function<void()> on_done
);
```

原版重做阶段曾保留 init 启动共享 pool、run 启动 Engine 后返回、外部阻塞 stop。此历史边界现已按用户确认调整为本页开头的 init/start/run/请求式 stop；stop 允许由所属回调请求，init/start/run 仍不能在需要等待自身的上下文执行。

每个回调在 `execution->mutex → registration->mutex` 下检查 Hard 和撤销状态，检查成功即取得这一次执行资格。锁外进入用户函数。READ 运行期间发生 Hard，后续尚未取得资格的 WRITE 被跳过；已获资格的函数即使在 Hard 截止或 remove 返回后才进入，也自然结束，由 run 收尾等待。Hard 截止先作用于所有 Engine，再逐个停止；后来的 Hard 可以升级已经请求或正在收尾的 Soft。

原注册记录、命令 payload 和 batch 的成员析构顺序保证 callback 副本先释放，注册状态最后释放。最后一个相关持有者在已确认解绑后通知一次 on_done；通知不需要动态排队，Hard 取消任务也会完成退休。它可能在 owner、worker 或外部清理线程执行，不可在里面析构所属 Loop，或等待包含自身的完成条件。

remove 不等待已经取得资格的 callback。应在 on_done 后关闭资源；若以 run 完整收尾返回作为整体释放边界，先停止并 join 其他正在调用 IO API 的应用线程。stop 返回不代表收尾完成，仅停止 Loop 也不能替调用方等待它自己尚未退出的方法调用或保留的用户副本。

`IoRegistration` 的所有副本指向同一次注册，但不拥有资源、Loop 或内部注册状态，析构不取消，也不延后 on_done。callback 用 `context.self` 得到同类型凭证；更新、替换和取消可通过 Loop 或注册方法，最终进入同一路径。start 前保留命令排队：remove/cancel true 表示删除命令已经入队，同时完成逻辑撤销，所有副本立即失效；后端解绑及相关回调结束仍通过 on_done 确认。运行期 DEL 失败仍返回 false 且不撤销，不悄悄改为“逻辑成功”；调用方继续保留资源，停止时尝试关闭 Poller。DEL 和 Poller close 都无法确认成功时，on_done 保守不通知；没有新增公开故障查询或恢复状态机。

此前凭证统一是已批准的源码接口变化：`IoHandle`/`IoToken` 改为 `IoRegistration`，对应头改为 `IoRegistration.h`，`context.token` 改为 `context.self`。没有保留别名，也没有增加 `Registration` 基类；注册上的 set_cb/update/cancel 是本次另行批准的便利入口。GST_net 不在本轮修改范围内，它在 Connection.h/.cpp、Server.h/.cpp 的前置声明、局部类型与 unique_ptr/make_unique 共八处旧显式类型需要另行迁移；其源码没有使用 context.token。不能把之前的兼容验证记录当作本次无需迁移的依据，生命周期调用也须遵守新增的 start 和阻塞 run 边界。

内部新增一个原子 removal_pending，仅认领删除请求，不作为工作计数或后端解绑事实。先检查所属 Loop、代次和跨 owner 限制，再认领和提交；提交失败或抛异常时仅释放认领，不恢复已经失效的注册。成功后共享 registered=false，所有公开副本失效。这避免 start 前多个副本同时通过预检查、重复入队删除并使 startup 失败；remove 保留状态到路由清理完成，且不持注册锁等待 owner。

## NativeHandle 与可移植性的实际边界

用户随后明确要求不在 Loop 内增加专用 SocketHandle 类。因此移除此前新增的 `SocketHandle.h/.cpp` 及其构建、安装入口，也删除 NativeHandle 中只有它使用的 socket 专用种类和辅助函数。NativeHandle 继续作为统一系统资源所有者，当前由 IoRegistration 标识注册，Loop 只借用资源。当前没有新增公开资源种类或新的资源释放协议；Linux fd 构造、native_fd、release、reset 和原有移动/继承方式保留。

TCP 创建、listen/connect/accept 和数据收发属于 GST_net 或其他资源提供层，不移进 NativeHandle。示例使用独立的 NetworkProvider 自由函数：main 不创建或读取 fd、不包含平台头、不接触 eventfd；只有示例实现包含 Linux 头并操作原生资源。该提供层不是安装公开接口，不应被误认为 Loop 新增了跨平台网络 API。本轮不修改 GST_net。

示例使用真实 listener 和已连接 TCP 字节流，而非仅两个内部端点。READ 对 listener 表示可尝试 accept，对连接表示可尝试接收数据、EOF 或错误；WRITE 表示可尝试发送，资源层必须处理部分收发和暂不可读写。Loop 不定义 socket 创建和收发接口，也不承诺任意操作系统资源都可监听；当前只支持 Linux epoll 可监听的 fd。

未来 Windows 还需实现资源种类对应的关闭方式、完整宽度的资源路由、Poller 和内部唤醒，并明确资源类别与 READ/WRITE 的可实现语义。现有 int 兼容入口不能直接表达所有 Windows 资源，当前 GST_net 本身也仍有 Linux 公开类型。因此只能将保持 watch_io、update_io、set_io_callback、remove、IoContext 及生命周期调用形式作为目标，尚不能保证整个程序仅更新 Loop 并重新编译即可支持 Windows。示例 NetworkProvider 和其他资源提供层仍需要自己的 Windows 实现；当前 CMake 明确只支持 Linux，没有执行 Windows 编译运行。

旧 Linux 调用方继续使用 NativeHandle(int)、native_fd、release、reset(int)，无需为移除 SocketHandle 改动这些资源调用；注册凭证则须按上节迁移。此前试用过本轮新增 SocketHandle 的程序须将其创建/accept/收发调用迁到网络或资源提供层，并以 NativeHandle 持有资源；该接口不是原版已有接口。直接使用 Linux 系统操作的代码仍不具备跨平台保证。

NativeHandle 从 header-only 改为库实现后，仅包含该头而不链接库的程序需要增加 GST::EventLoop 链接。仅这项实现移动不要求已链接该库的 GST_net 修改资源调用；本次注册凭证统一仍要求其旧注册类型迁移。私有布局改变，不承诺旧二进制 ABI。

## 实际验证记录

首次提交实际没有 tests 源文件，只在 CMake 引用了不存在的文件。此前 78 项属于另一份未提交实现；本轮复用其有效测试工具与场景，未恢复已经不适用的复杂状态机断言，也未将数量差异称作覆盖等价。

以下 33/33 表格记录移除 SocketHandle **之前**的验证，不能当作此次接口收敛后重新运行的结果；其中两项资源操作测试验证的是当时的 SocketHandle。当前一轮的构建、测试与安装结果需另行记录。

| 验证 | 实际结果 |
|---|---|
| Debug，Wall/Wextra/Wpedantic/Werror | 33/33 |
| Release，同上 | 33/33 |
| ASan/UBSan，detect_leaks=0 | 33/33 |
| TSan，halt_on_error=1 | 33/33 |
| 非测试 Release 安装、外部 C++17 程序 | 编译、链接及 init/run/stop/reinit 运行通过 |
| 不定义测试宏的外部程序链接 Debug 和 ASan 库 | 编译运行通过，检查公开类布局不依赖测试宏 |
| 安装头编译当时的 TCP 示例 | 通过；使用现已移除的 SocketHandle 接口 |
| 屏蔽 Linux 宏并用报错头遮挡系统 IO 头 | 标准示例与公开接口语法检查通过；不等价于 Windows 编译 |
| GST_log 开启，Release、Wall/Wextra/Werror | 构建通过；未给日志依赖声称 Wpedantic 检查通过 |
| GST_net 六个现有库源码 | C++17、Wall/Wextra/Werror 只读语法检查通过；不是网络运行集成验收 |
| Windows CMake 配置 | 按预期明确拒绝 |
| git diff --check | 通过 |

当时 33 项包括 Engine 7、EventLoop 16、NativeHandle/Poller 3、ThreadPool 4、分配故障 1、资源操作 2。关键交错使用门闩/条件变量或实际原子截止状态，不靠 sleep 猜测时序：

- 资格前取消，资格后删除再进入函数，同批自删除，以及已取得 WRITE 资格后发生 Hard。
- 真实 EventLoop 与共享 pool 的 Soft 排空、Hard 取消、外部 Soft 升级 Hard、多 Engine 共同截止。
- 一个真实 socketpair 在首轮 epoll 等待前同时具备 READ/WRITE，READ 暂停后 Hard，验证批次确实选中两个回调但 WRITE 不启动。
- 其他精细 READ/WRITE 资格测试由真实 owner 构造合成就绪批次，明确不是内核合批验证。
- 同注册不重叠、旧事件不作用于替代注册、并发删除、自删除、捕获对象析构与 on_done、两 owner 析构互删提前拒绝。
- ThreadPool 第二节点分配失败时整批不发布，取消和异常路径正确退休负载。

此前未通过的环境项：真实 TCP 示例实际运行输出 `TCP listen failed: Operation not permitted`，独立探测确认 AF_INET socket 创建受环境限制；listen/accept/connect 成功路径及完整 TCP echo 需要在正常 Linux 环境补验。内部 AF_UNIX 资源测试不能替代它。LeakSanitizer 实际启用时报 ptrace 不支持，不能报告泄漏检查通过。

### 移除 SocketHandle 后的验证（凭证合并前的历史记录）

这一阶段重新构建并验证了移除后的代码。删除原 SocketHandle 的两项测试，增加 NativeHandle 移动赋值释放旧资源、release/reset 所有权边界、管道和 socket 统一所有权三项测试；当时共 34 项（Engine 7、EventLoop 16、NativeHandle/Poller 6、ThreadPool 4、分配故障 1）。既有逐回调资格、取消、Hard 截止和 on_done 测试保留。以下结果不是此次 IoRegistration 接口迁移后重新执行的结果。

| 该阶段重新执行的验证 | 实际结果 |
|---|---|
| Debug，Wall/Wextra/Wpedantic/Werror | 34/34 |
| Release，同上 | 34/34 |
| ASan/UBSan，detect_leaks=0、halt_on_error=1 | 34/34 |
| TSan，halt_on_error=1 | 34/34 |
| 非测试 Release 构建和安装 | 通过，安装清单不包含 SocketHandle 或示例 NetworkProvider |
| 安装后的外部 C++17 使用方 | 编译、链接及 init/run/stop/reinit 运行通过，原三参数 watch_io 签名保留 |
| 安装头和库构建新的 TCP 示例 | 编译、链接通过 |
| 安装头与示例 main 屏蔽 Linux 宏，并遮挡系统 IO 头 | 语法检查通过；不等价于 Windows 编译运行 |
| GST_net 六个现有库源码 | 使用其日志依赖的完整 include 路径，C++17、Wall/Wextra/Werror 只读语法检查通过 |
| TCP echo 实际运行 | 创建 listener 时返回 Operation not permitted，完整网络成功路径仍未验证 |
| git diff --check | 通过 |

该阶段未重跑 GST_log 开启构建和 Windows 配置拒绝检查，前表仅保留历史结果。LeakSanitizer 仍未完成；ASan 结果不包含泄漏检测。没有修改 GST_net，以上源码兼容性检查也不等于网络运行集成验收，并且发生在 IoRegistration 破坏性接口迁移之前。

### 统一 IoRegistration 后、生命周期调整前的历史验证

当时保留前述有效并发场景，新增五项确定性测试，共 39 项：凭证复制/赋值/移动和弱持有；跨 Loop 操作拒绝；stop/reinit 后旧副本失效；锁住 IO 队列并观察删除认领、验证第二副本不会重复提交；同步删除命令分配失败后释放认领并可重试。原 run 前取消、自删除及并发删除测试同时强化了所有副本共享失效的断言。下表不包含此次 init/start/run/stop 调整和注册成员方法的重新验证。

| 该历史阶段重新构建及执行的验证 | 实际结果 |
|---|---|
| Debug，Wall/Wextra/Wpedantic/Werror | 39/39 |
| Release，同上 | 39/39 |
| ASan/UBSan，detect_leaks=0、halt_on_error=1 | 39/39 |
| TSan，halt_on_error=1 | 39/39 |
| BUILD_TESTING=OFF 的 Release 构建和干净目录安装 | 通过，只安装 IoRegistration，不包含旧两种凭证头 |
| 外部 C++17 使用方，未定义测试宏 | 非测试安装库与 Debug 库编译、链接、生命周期运行通过；静态检查只有一种注册类型和一个 remove 重载 |
| 安装头与库构建 TCP 示例 | 编译、链接通过 |
| 屏蔽 Linux 宏并遮挡系统 IO 头 | 新公开接口和示例 main 语法检查通过，不等价于 Windows 编译运行 |
| TCP echo 实际运行 | 创建 listener 仍返回 Operation not permitted，完整网络成功路径未验证 |
| git diff --check | 通过 |

GST_net 源码未修改，其旧类型调用需要单独迁移，该阶段不声称现有 GST_net 已通过新版编译。LeakSanitizer 仍未完成，日志开启构建未在该阶段重跑。当前 start 前的更新、回调替换等操作仍是延迟命令；相互冲突的并发更新与删除可能在 startup 执行时失败。重新安装到已有前缀时，应清理旧安装留下的 IoHandle.h、IoToken.h；普通 install 不会自动卸载历史文件。

Poller 硬错误的完整恢复、DEL/close 双重失败的公开诊断，以及大规模完成标记扫描性能仍未解决。100ms 等待带来每 Engine 最多约每秒十次空闲检查，扫描成本随当前注册数增长；本轮没有吞吐压测或长期泄漏结论。源码重做与接口收敛阶段没有开发 Timer，没有修改 GST_net，也没有提交或推送。
