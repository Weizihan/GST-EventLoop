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

调用方迁移：原来以 run 启动线程的程序增加 start；原来 run 返回后执行的逻辑移到回调或其他线程；原来等 stop 返回再释放资源的程序改为 on_done 或 run 完成收尾后释放。资源所有权、跨 owner 同步拒绝、逐回调资格、Soft 排空和 Hard 取消未获资格回调的规则保持。该生命周期调整阶段没有加入 post_main、帧循环、Timer 或 Windows 后端，也没有修改 GST_net；后续 Timer 阶段 1 记录见文末。

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

## Timer 阶段 1：单次定时与生命周期

2026-10-28。用户要求从 master 新建分支，最初建立 `feat/timer-phase1` 于 `2b59213`。该 master 缺少阶段 0 已审查的生命周期与 IO 注册机制；报告差异后，用户同意先接入阶段 0 基线。新分支已快进到 `f3daaebc0df8c18af471c1f948ac367256efee83` 再实施 Timer；master 仍保持 `2b59213`。没有为 Timer 重建历史版本中缺少的基础设施，也没有提交或推送本轮变更。

本阶段只增加单次 `after` / `at`、取消、完成通知和生命周期接入。公开 API 使用标准 chrono 类型，用户回调与 IO 共用原有 ThreadPool；不提供 every、FixedDelay / FixedRate 选择、update_timer 或 set_timer_callback。

### 最小接入方式

- 新增 TimerEngine，一条 `steady_clock` / `condition_variable` 调度线程，一把调度 mutex 保护 `std::set` 待触发项；键为 deadline/id，节点持有注册状态。新注册同步插入成功才成为已接收注册，取消同步删除，不新增命令队列、注册 map、完成 vector 或全表完成扫描。
- 调度线程从 set 取出单次到期项后解锁，准备任务并调用 Timer 专用 poster。worker 通过原共享 execution gate 和本 Timer 注册锁认领资格，锁外直接调用不可变业务 callback，不在内部锁下复制用户 function。
- 单次 TimerBatch 持有注册状态，最后批次副本退休使注册失效；状态最后引用释放前先清业务 callback，再触发 on_done。完成用于生命周期，不返回 Timer owner 重排，析构阶段不为完成通知分配节点。
- 凭证沿 IoRegistration 的非拥有、弱引用、代次和 LoopAccess 思路；cancel 不等待已资格回调。Loop stop 请求生效后 cancel false，没有停止期间的额外取消窗口。
- Timer 完整 post_batch 与 Loop stop 在现有 Loop 状态锁下排序：先成功入池则 Soft 排空，stop 先提交则拒绝新投递。选中节点和批次在发布锁外保活，投递拒绝/异常不会在状态锁下释放最后用户负载。现有 IO poster 不加 Timer 的 Soft 截止检查。
- stop 请求只提交状态并通知 Timer CV，不在请求路径清理用户负载。统一收尾在 pool 停止前唤醒并 join Timer Thread，在 pool 结束后销毁 TimerEngine；Timer Thread 全程纳入已有受管执行检查，包含退出清理，沿原请求式 stop 防止自 join。

这套单次接入依据当前用户要求避免不必要队列，直接以同步 set 操作保证注册接收和取消边界。它没有重构现有 IO owner 或命令机制，也不把单次一任务固化为未来周期的门禁。后续固定频率周期可由每次独立任务的强引用扩展生命周期跟踪，当前没有提前实现多回调调度或更新事务。

### 文件与接口

| 文件 | 原因 |
|---|---|
| 新增 `src/TimerTypes.h` | 只提供单次 after/at 的 TimerSchedule 和有效性；注册时的时间换算与饱和截止计算在 EventLoop.cpp。 |
| 新增 `src/TimerRegistration.h` | 非拥有凭证、valid/cancel、generation/registration_id。 |
| 新增 `src/TimerRegistrationState.h` | 单次注册资格、不可变业务 callback、接受状态和最终 on_done。 |
| 新增 `src/TimerEngine.h/.cpp` | 条件变量等待、按截止排序、同步注册/删除、一次任务投递与清理。 |
| 修改 `src/EventLoop.h/.cpp` | add_timer/remove 重载、凭证管理访问、专用发布、启动/停止/收尾和受管判定。 |
| 修改 `CMakeLists.txt` | Timer cpp 入库、公开 Timer 头安装，沿既有测试发现方式。 |
| 新增 `tests/TimerTests.cpp`，修改 `tests/EventLoopTestAccess.h` | 单次功能、取消、Soft/Hard、异常、清理和可控竞争。 |
| 修改 `README.md`、本记录及现有唯一 Timer 说明书 | 用法、实际语义、范围和验证归属。 |

`ThreadPool.*`、`Poller.*`、`EventLoopEngine.*`、`IoRegistration*`、`NativeHandle.*` 及 GST_net 保持原有实现。已对本轮相对 f3daaebc 的 diff 核对这些基础文件未改；Timer 只接入 EventLoop。

### 阶段 1 验证记录

以下结果均在本轮最终代码上重新执行；既有回归与新增 Timer 分开统计。

| 本阶段验证 | 实际结果 |
|---|---|
| Debug，Wall/Wextra/Wpedantic/Werror | GNU 15.2.0 / C++17，Debug 严格警告构建通过 |
| 全部既有测试 | 55/55 通过；全套含 Timer 共 92/92 |
| 新增 Timer 测试 | 37/37 通过；每项重复 10 次，370/370 通过 |
| ASan/UBSan | 全套 92/92；ASAN_OPTIONS=detect_leaks=0，UBSAN_OPTIONS 未设置；没有做泄漏扫描 |
| TSan | 全套 92/92；TSAN_OPTIONS=halt_on_error=1，无竞态报告 |
| 非测试构建、安装、外部使用方 | Release 严格警告非测试构建、安装、外部使用方编译/链接/运行及公开头隔离检查通过；未运行完整 Release CTest |
| git diff --check | 通过 |

37 项新增测试覆盖：单次一次/过去截止/按截止投递/更早唤醒；跨线程取消、资格前后取消、自取消；通知等待回调与负载退休；Soft/post 双向顺序、Hard 队列/资格；stop 唤醒、停止后取消拒绝；标准/非标准异常；多线程注册与取消、重复停止、重新初始化及旧凭证；捕获清理重入和失败路径。用门闩、worker blocker、条件变量和私有钩子控制竞争，超时只判断挂起，不靠随机 sleep 决定顺序。

可复现命令与详细对应关系见本轮《GST-EventLoop-Timer-Phase1-Report》。未执行 GST_log 开启构建/输出检查、LeakSanitizer、长期压力、Windows 后端及 TCP 完整成功路径；未穷尽捕获复制抛异常或每一个分配故障点。Timer 异常后真实 IO 就绪的组合场景未单独测试，现有 IO 回归全通过。

### 已知限制与后续范围

本阶段只实现单次 Timer，不提供周期、update 或回调替换；没有硬实时保证，worker 排队会延后实际入口，多 worker 不保证函数入口/完成与 deadline 投递同序。现有 GST 日志关闭时错误宏为空，异常隔离不代表默认可见诊断。on_done 线程不固定，也不表示其自身捕获已经析构。

修订后的未来周期统一采用固定频率，同 Timer 允许多个任务在途和并发执行；回调耗时或池排队不参与下次截止，不因为前次未结束主动跳过。只有 Timer 调度线程自身迟到才跳过错过计划点。回调完成只用于生命周期和 on_done。这些是后续设计约定，未因文档更新被描述为阶段 1 已实现或已测试。

阶段 1 验证和报告完成后停止，等待审查；不自动进入周期或 update 实现。

## Timer 阶段 2：固定频率周期、更新和回调快照

本节的 120/120、28 项新增和 280/280 是周期异常处理修正前的阶段 2 交付历史，不能作为“异常后取消整个 Timer”的本轮验证。既定取消设计及修正后的本轮结果见文末“周期异常处理修正记录”。

2026-10-28。阶段 1 审查后，用户已另行授权阶段 2。`feat/timer-phase1` 已同步 master `2ab7a0e`，阶段 1 Timer 变更仍是未提交工作区修改；本节只追加阶段 2 范围、设计及最终验证记录，不改写上文阶段 1 的历史测试结果。周期/update/set_cb 与多在途实现已完成，最终 Debug、Release、ASan/UBSan、TSan 均 120/120（既有 55 + 阶段 1 Timer 37 + 新增 28），新增 28 项各重复 10 次为 280/280；Release 非测试构建、安装、独立外部使用方和公开头隔离检查均通过。没有提交或推送。

### 当前范围与确定语义

- TimerSchedule 增加 every(interval>0)，首截止为 add_timer 入口采样时刻 + interval。周期统一固定频率，不提供 FixedDelay / FixedRate 参数。
- 调度线程每次处理一个到期项，在调度锁下登记该次 in-flight 并先计算/挂好下一计划，然后解锁投递。下一个计划严格晚于本次 scheduler_now 并与原相位对齐；调度线程自身迟到只跳中间历史点，不补发。
- 同 Timer 允许多任务排队、取得资格、执行及清理，回调之间可以重叠。不因回调耗时或 worker 占用跳过正常周期；回调完成只管理生命周期，不参与下一截止。
- Loop 增加 update_timer，凭证增加 update；支持外部和自身回调调用。接受任意有效 after/at/every，可以单次↔周期转换；after/every 从 update 入口采样时刻重新起算，at 保留 t。true 表示计划同步可靠提交，不能正常返回成功后静默应用失败。
- 到期选择与 update 同调度锁排序。已选中并登记在途的旧任务，包括尚未 post 的任务，保留本次旧 deadline 和 callback 快照自然投递/执行，仍接受取消/Hard 资格检查；update 改后续计划，旧重排/退休不能覆盖新配置。
- 有效单次 Timer 正在排队或执行时可以 update 重新挂计划；已经彻底退休后返回 false。更新不等待或强行中断当前函数。
- Loop 增加 set_timer_callback，凭证增加 set_cb。锁外构造共享不可变 callback payload，锁内交换引用；已选中/排队/执行任务保留自身快照，后续任务取得新快照。替换并发当次可取旧或新，以同步取得快照的顺序定义。
- Stop 生效后 update/set_cb/cancel 均拒绝；Soft 只排空成功入池任务，Hard/取消阻止未资格函数，已资格函数自然结束，不改变现有 IO 语义。
- interval、scheduled、in_flight、schedule_generation 运行期由 registration mutex 保护；registered 仍为 atomic 供 valid 观察。每 batch 先 reset 本次 callback snapshot，再 in_flight--；只有 !scheduled && in_flight==0 才自然失效，取消仍立即失效。所有应等待任务/快照退休后，最后状态 on_done 一次。
- schedule_generation 只防旧 post 失败覆盖成功 update，不是 callback 版本门禁。D5 失败按 generation 条件关闭 future，不置 revoked，不撤销其他已经入池的任务。
- 业务异常沿既有错误处理机制隔离和记录。实现与测试的错误预期现已按既定异常取消设计纠正：取消整个 Timer，不再产生后续任务；其他已取得资格的回调自然结束，未取得资格的按取消门禁跳过，on_done 等所有应等待的任务和捕获退休后通知一次。单次既有行为不变，不增加异常策略参数。

### 接入与文件范围

仍沿一条 Timer Thread、标准 condition_variable、按 deadline/id 排序的 set 和原共享 ThreadPool。Engine mutex → registration mutex 保护 selection/update/set 节点，set_cb 同序交换快照；不保留阶段 1 “Engine 锁下不得取 registration 锁”的旧限制，也无反向持锁路径。取消先在 Loop state→execution→registration 撤销，释放 execution/registration 后才用 Engine→registration 删除 future；worker 为 execution→registration，退休只需 registration。

有 pending 节点的更新和周期重排 extract 后复用节点，不分配；已 selected 单次 update 重新挂计划先 insert 成功再提交字段，bad_alloc 可抛且旧配置/既有任务保留。完成路径不新增复杂 completion queue；用户 callback payload 构造、替换旧负载和最后引用释放均在内部数据锁外。每批次快照先于 in_flight 退休，保证捕获重入时注册仍保活，不提前 on_done。当前未给共享池增加背压或队列上限，重叠 mutable 捕获由应用同步。

| 文件 | 阶段 2 目的 |
|---|---|
| `src/TimerTypes.h` | every 及周期计划的有效性和必要时间字段；不加策略选择。 |
| `src/TimerRegistration.h` | update/set_cb 便利入口，沿 weak identity 与 LoopAccess。 |
| `src/TimerRegistrationState.h` | 多任务共享注册、独立不可变 callback 快照和终结通知。 |
| `src/TimerEngine.h/.cpp` | 固定频率预排、同步更新和选择边界、多任务退休；保持 CV 唤醒。 |
| `src/EventLoop.h/.cpp` | update_timer/set_timer_callback、Stop/代次检查、锁外负载清理与可靠返回合同。 |
| 新增 `tests/TimerPeriodicTests.cpp`，修改 `tests/EventLoopTestAccess.h` | 28 项阶段 2 确定性功能、竞争、故障与生命周期测试，阶段 1 TimerTests 保留回归。 |
| `README.md`、本记录及原唯一 Timer 说明书 | 当前 API/语义、历史范围、实测与限制。 |

ThreadPool、Poller、EventLoopEngine、既有 IO 注册及 GST_net 不因阶段 2 重构。本节按实际阶段 2 修改核对；沿既有测试自动发现，无须为新测试修改 CMake。

### 阶段 2 初次交付验证结果（周期异常修正前的历史）

以下在当时阶段 2 初次交付代码上重新执行，GNU 15.2 / C++17，`-Wall -Wextra -Wpedantic -Werror`，GST 日志与 examples 关闭。上文 55/37/92/370 表保留阶段 1 历史；本表发生在周期异常修正前。

| 验证 | 阶段 2 实际结果 |
|---|---|
| Debug 完整回归 | **120/120**：非 Timer 既有 55 + 阶段 1 单次 37 + 阶段 2 新增 28 |
| Release 完整回归 | **120/120**，同样分组 |
| 阶段 2 重复运行 | 新增 28 项各 10 次，**280/280** |
| ASan + UBSan | **120/120**；ASAN_OPTIONS=detect_leaks=0:halt_on_error=1；UBSAN_OPTIONS=halt_on_error=1，不含 LeakSanitizer |
| TSan | **120/120**；TSAN_OPTIONS=halt_on_error=1，无竞态报告 |
| Release 非测试构建/安装/外部 C++17 使用方 | BUILD_TESTING=OFF 构建、安装、独立 consumer 编译/链接/运行通过，覆盖 every、Loop/reg update、回调内 set_cb+update、on_done stop |
| 公开头隔离检查 | 屏蔽 Linux 宏并以 #error 遮挡 sys/epoll、eventfd、socket、unistd，consumer 语法检查通过；不等于 Windows 编译/运行 |

当时 28 项新增测试包含固定相位和迟到跳点、同 Timer 重叠/排队、update 各阶段边界与单次↔周期/自更新、回调快照替换、多在途取消/Soft/Hard/on_done、停止/销毁后安全拒绝、并发管理以及节点/快照分配失败和旧 post 失败；其中周期异常测试当时错误断言后续继续，此预期现已纠正，历史通过不代表异常取消已验证。用 Gate、条件变量、worker blocker 和私有钩子控制顺序；超时只判挂起。完整通过不代表理论所有交错、用户捕获故障或分配点已穷尽。

未执行 LeakSanitizer、长期压力/积压容量测试、GST_log 开启构建/输出、Windows 后端、其他时钟精度组合或真实 TCP 完整成功路径。共享池未加背压或队列上限；mutable callback 捕获需应用同步；无硬实时保证，on_done 线程不固定。阶段 2 完成后停止并等待审查，不自动进入下一阶段。

## 周期异常处理修正记录

本轮落实已确定的周期异常取消设计：周期 callback 逃出 `std::exception` 或非标准 C++ 异常后，必须使用既有日志机制记录并取消整个 Timer，关闭 future，不再产生后续任务。周期/单次按 selection 时本次任务快照确定：周期旧回调即使身份当前已被 update 改成单次，异常仍取消整个注册；单次旧回调异常只记录，不撤销 update 接续的新计划，保留单次既有行为。

异常取消是注册级撤销：其他已经取得执行资格的回调自然结束，尚未取得资格的任务按取消门禁跳过，包括同 Timer 已被 Soft Stop 接收但尚未取得资格的任务。`on_done` 仍等所有应等待的在途任务、快照和业务捕获退休后通知一次，不因异常提前通知。不新增异常策略参数，也不改变现有 IO、ThreadPool 或 Loop 停止语义。日志输出仍服从已有 GST 日志配置。

### 内部路径与公开边界

selection 为每个 batch 快照保存 periodic 属性，不从异常发生时的最新 interval/计划推断本次任务类型。catch 沿用现有 cancel 路径；Stop 生效后公开 cancel 仍返回 false，异常内部仅在 execution→registration 资格锁序下设置 revoked=true、registered=false，阻止尚未取得资格的同注册任务。这个内部撤销不改变 in_flight，不释放本批 snapshot，也不提前触发 on_done；计数和捕获仍由既有批次退休路径处理。没有新增公开取消入口或停止期间的取消窗口。

### 本轮修正验证

本轮已在修正后的最终代码上重新运行 Debug、Release、ASan/UBSan、TSan，均 **125/125**：其他既有 55、单次 Timer 37、原周期 27 逐字节保留，原错误的“异常继续”1 项被 6 项新用例替换，周期测试共 33。GNU 15.2 / C++17，严格 `-Wall -Wextra -Wpedantic -Werror`。上文 120/28/280 仅保留修正前历史，不能证明本轮修正。

| 验证 | 本轮实际结果 |
|---|---|
| 标准/非标准异常后取消、停止 future | 新用例通过，异常后注册失效且不再产生后续任务 |
| 已资格/未资格并发任务与取消门禁 | 新用例验证已资格自然结束，Soft 已接收但未资格任务跳过 |
| on_done 等任务及捕获退休、仅通知一次 | 新用例通过，通知等所有应等待任务和捕获退休 |
| Debug 完整回归 | **125/125**：55 + 37 + 33（原周期 27 + 新异常用例 6） |
| Release 完整回归 | **125/125**，同样分组 |
| ASan + UBSan | **125/125**；`ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`，`UBSAN_OPTIONS=halt_on_error=1`，不含 LeakSanitizer |
| TSan | **125/125**；`TSAN_OPTIONS=halt_on_error=1`，无竞态报告 |

六项新用例分别覆盖标准异常、非标准异常、已资格任务自然结束及清理、Soft 下未资格任务撤销，以及周期任务更新为单次/单次任务更新为周期后的异常快照边界。其他 27 项周期用例逐字节不变；没有用改写这些既有预期来获得通过。

本轮没有单独验证默认日志输出、LeakSanitizer、长期压力或异常与 Hard Stop 并发组合；此前安装 consumer 与公开头隔离结果保留为历史，本轮未重测，不纳入本轮验证。异常仍调用已有日志机制，实际可见性服从原日志配置。

本次只修正阶段 2 周期异常行为；完成后停止，阶段 3 不执行，没有提交或推送。

## Timer 阶段 3：收尾验证

2026-10-28。用户在阶段 1、阶段 2及周期异常修正审查后授权本次收尾。实际仓库为 `/home/ubuntu/gst_projects/GST-EventLoop`，分支仍为 `feat/timer-phase1`，HEAD 仍为 `2ab7a0efcc6c197ef3adbce558d82bcb70a083d3`。本节是阶段 3 新结果，上文历史测试结果保留原归属；全部 Timer 改动仍未提交，未执行 git add、commit 或 push。

### 补齐内容与完整回归

现有 125 项已经覆盖取消、更新、快照、异常和多在途生命周期的小规模确定性交错，本次只新增 `tests/TimerStressTests.cpp` 四项有界规模验收，并追加 README 与本记录。复用现有 Gate、timer_hooks、pending_timers 和 post_worker；没有增加辅助接口、生产公开 API 或修改生产源码，也未改写原有 55 项及 70 项 Timer 测试。当前结构只有 pending set、没有额外注册 map，原清单的“注册表归零”按 pending 归零、全部 on_done 和捕获对象退休验收，不为测试增加结构。

| 配置 | 每遍完整结果 | 完整套数 | 实际测试执行数 | CTest 每遍实耗范围 |
|---|---|---|---|---|
| Debug | 129/129 | 20/20 | 2580/2580 | 0.58–1.02s |
| Release | 129/129 | 20/20 | 2580/2580 | 0.63–0.99s |
| ASan + UBSan | 129/129 | 20/20 | 2580/2580 | 2.99–3.52s |
| TSan | 129/129 | 20/20 | 2580/2580 | 2.51–5.34s |

每遍均为 55 项原有测试 + 37 项单次 Timer + 33 项周期/更新/替换/异常 + 4 项压力测试。用外层循环重新运行整个 CTest 套件 20 次，未使用单项重试替代整套重复；四套共 80 遍、10,320 次执行，零失败，无跳过、ASan/UBSan 或 TSan 报告。每套测试前后保存源码 SHA-256，四套清单一致；阶段 3 开始前已有的全部 src、tests 和 CMakeLists.txt 的哈希逐项不变，新增压力文件除外。git diff --check 通过，索引和 HEAD 未改。

### 有界资源与压力实测

下表取 Release 第一遍原始日志，单位毫秒。测试在同一台双 CPU Linux VM 上与其他配置并行执行，属于验收观测，不是隔离吞吐基准或精度承诺。全部四项在四种配置中各执行 20 次，完整逐次数据保存在验证日志中。

| 场景 | 有界负载与顺序 | 实测计数、耗时与清理 |
|---|---|---|
| 大量注册后取消 | 启动后注册 4096 个 24h 单次 Timer，再逐项取消 | 注册 13.591ms，取消 4.379ms；业务执行 0，on_done 4096；业务捕获与通知捕获各析构 4096，weak_ptr 全失效，pending 4096→0 |
| 同时到期 | 4096 个 Timer 使用同一 begin+100ms deadline，4 workers | post/执行/on_done 各 4096；逐注册恰好一次且不早于 deadline；从注册开始到完成通知 113.089ms（含计划等待），pending=0 |
| 高频周期 | 64 个 every(1ms)，4 workers；第 4096 次成功 post 后用 Gate 暂停调度，再请求 Soft | 4096 次 post 全部执行；每 Timer 本遍 64 次，deadline 保持固定相位；到投递上限 65.311ms，完成收尾 65.519ms，on_done=64，pending=0 |
| 共享池拥堵 | 普通 Gate 任务占住唯一 worker；32 个 every(1ms)，第 1024 次 post 后暂停调度、全部取消 | 取消前精确积压 1024 项，业务入口 0；累计排队 32.097ms，总计含退休 35.111ms；取消后这 1024 项全部按资格门禁跳过，最终业务入口仍为 0；on_done=32，业务/通知捕获各退休 32，weak_ptr 全失效，pending=0 |

压力测试使用 post_limit 和 Gate 确保负载不再增长；CTest 沿既有 20s 超时，Gate 沿既有 10s 卡死保护。取消集合归零在 worker 放行前检查；捕获及通知在统一收尾后再次核对。高频/同时到期用例还检查实际入口不早于计划点。未发现需修复的生产缺陷，因此没有新增功能或改变既定语义；四项测试用于本阶段规模验收，无额外修复回归补丁。

### 环境、等待路径与复现

GCC `15.2.0`（Ubuntu `15.2.0-16ubuntu1`）、glibc `2.43`、CMake `4.2.3`，Linux x86_64 `7.0.0-30-generic`，可用 CPU 数 `2`。四配置均为 C++17、`-Wall -Wextra -Wpedantic -Werror`，GST_log 和 examples 关闭。ASan/UBSan 使用 `ASAN_OPTIONS=detect_leaks=0:halt_on_error=1`、`UBSAN_OPTIONS=halt_on_error=1`；TSan 使用 `TSAN_OPTIONS=halt_on_error=1`。

目标编译器预处理确认 `_GLIBCXX_USE_PTHREAD_COND_CLOCKWAIT=1`；该版本 condition_variable 的 steady-clock wait_until 使用 CLOCK_MONOTONIC，std_mutex 调用 pthread_cond_clockwait，实际测试二进制引用 `pthread_cond_clockwait@GLIBC_2.34`。这是本环境 V1 的工具链/链接证据，没有执行修改系统时间的实验，也不推断其他工具链同样可用。

以下命令复现四种构建与每种完整套件 20 遍；实际构建及每遍 CTest 完整输出已保存为独立日志。

```bash
repo=/home/ubuntu/gst_projects/GST-EventLoop
out=/home/ubuntu/Documents/Codex/2026-10-28/gst-eventloop-timer-0-gst-eventloop/work/timer-phase3
for config in debug release asan tsan; do
    type=Debug
    flags='-Wall -Wextra -Wpedantic -Werror'
    linker=''
    case "$config" in
        release) type=Release ;;
        asan) flags="$flags -fsanitize=address,undefined -fno-omit-frame-pointer"
              linker='-fsanitize=address,undefined' ;;
        tsan) flags="$flags -fsanitize=thread -fno-omit-frame-pointer"
              linker='-fsanitize=thread' ;;
    esac
    cmake -S "$repo" -B "$out/$config" -DCMAKE_BUILD_TYPE="$type" \
        -DBUILD_TESTING=ON -DGST_EVENTLOOP_USE_GST_LOG=OFF \
        -DGST_EVENTLOOP_BUILD_EXAMPLES=OFF \
        -DCMAKE_CXX_FLAGS="$flags" -DCMAKE_EXE_LINKER_FLAGS="$linker"
    cmake --build "$out/$config" -j2 || exit 1
    for round in $(seq 1 20); do
        case "$config" in
            asan) ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 \
                  UBSAN_OPTIONS=halt_on_error=1 \
                  ctest --test-dir "$out/$config" --output-on-failure -j2 -V ;;
            tsan) TSAN_OPTIONS=halt_on_error=1 \
                  ctest --test-dir "$out/$config" --output-on-failure -j2 -V ;;
            *) ctest --test-dir "$out/$config" --output-on-failure -j2 -V ;;
        esac || exit 1
    done
done
```

本次实际独立构建脚本和逐遍日志位于上述 out 目录，证据包为同级 `timer-phase3-validation.tar.gz`，包含环境、严格警告/构建命令、80 遍原始日志、源码哈希、开始前差异和最终审计。构建与各配置测试可并行；命令示例按序运行时结果合同相同，不保证耗时相同。

### 验收结论和保留边界

阶段 3 要求的严格 Debug/Release、ASan/UBSan、TSan、整套至少 20 遍、资源回收、三类压力和文档/环境记录均已完成。没有删除、放宽或重复改写既有测试，没有重构 IO、ThreadPool、Poller 或 EventLoopEngine；原有未提交工作完整保留。

LeakSanitizer 仍未运行，ASan 的捕获回收断言不等价于全进程泄漏检测；Windows、完整 TCP 成功路径、GST_log 开启后的输出、其他工具链/时钟表示、长期持续过载和无界积压容量仍未验证。此前安装/外部使用方和公开头隔离检查没有在阶段 3 重跑，保持历史归属。仍无硬实时或最大延迟保证，队列没有新增背压/容量上限，也未穷尽所有分配故障、周期极值或理论交错。

本阶段完成后停止，等待用户审查和后续统一提交指令；没有进入新功能阶段。
