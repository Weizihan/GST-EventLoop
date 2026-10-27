#include "EventLoop.h"
#include "NetworkProvider.h"

#include <chrono>
#include <atomic>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>

namespace {

using namespace GST::BASE;

// 本示例交换一条固定消息；实际协议需要自行处理
// 消息边界和缓冲，Loop 只报告就绪状态。
const std::string message = "Hello from the original GST_EventLoop design!";

struct DemoResult {
    void finish(EventLoop& loop, bool success, std::string detail) {
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (finished) {
                return;
            }
            finished = true;
            succeeded = success;
            description = std::move(detail);
        }
        cv.notify_one();
        // 这里只提出停止请求；主线程中的 run() 等待回调结束并完成收尾。
        loop.stop(success ? ShutdownMode::Soft : ShutdownMode::Hard);
    }

    std::mutex mutex;
    std::condition_variable cv;
    bool finished{false};
    bool succeeded{false};
    std::string description;
};

struct Session {
    NativeHandle socket;
    std::string received;
    std::size_t sent{0};
    bool succeeded{false};
    std::atomic_bool callback_started{false};
};

}

int main() {
    using namespace GST::BASE;

    // 依赖对象先于 Loop 构造，确保提前返回时也先析构 Loop、
    // 等待回调结束，再销毁这些对象。
    DemoResult result;
    NativeHandle listener;
    auto client = std::make_shared<Session>();
    EventLoop loop;

    std::error_code error;
    listener = ExampleNetwork::listen("127.0.0.1", 0, error);
    if (!listener.valid()) {
        std::cerr << "TCP listen failed: " << error.message() << '\n';
        return 1;
    }
    const auto port = ExampleNetwork::local_port(listener, error);
    if (!error) {
        client->socket = ExampleNetwork::connect("127.0.0.1", port, error);
    }
    if (error || !client->socket.valid()) {
        std::cerr << "TCP connect failed: " << error.message() << '\n';
        return 1;
    }

    EventLoopConfig config;
    config.engine_num = 2;
    config.callback_thread_num = 2;
    if (!loop.init(config)) {
        std::cerr << "EventLoop init failed\n";
        return 1;
    }

    IoRegistration listener_registration = loop.watch_io(
        listener,
        Event_type::READ_EVENT | Event_type::ERROR_EVENT,
        [&](const IoContext& context) {
            if (context.event_type == Event_type::ERROR_EVENT) {
                result.finish(loop, false, "Listener reported an IO error");
                loop.remove(context.self);
                return;
            }

            std::error_code accept_error;
            auto accepted = ExampleNetwork::accept(listener, accept_error);
            if (accept_error == std::errc::operation_would_block) {
                return;
            }
            if (!accepted.valid()) {
                result.finish(loop, false, "TCP accept failed: " + accept_error.message());
                loop.remove(context.self);
                return;
            }

            auto server = std::make_shared<Session>();
            server->socket = std::move(accepted);
            IoRegistration connection_registration = loop.watch_io(
                server->socket,
                Event_type::READ_EVENT | Event_type::WRITE_EVENT | Event_type::ERROR_EVENT,
                [&, server](const IoContext& event) {
                    server->callback_started = true;
                    if (event.event_type == Event_type::READ_EVENT) {
                        char buffer[256];
                        const auto input = ExampleNetwork::recv(server->socket, buffer, sizeof(buffer));
                        if (input.status == ExampleNetwork::IoStatus::Transferred) {
                            server->received.append(buffer, input.bytes);
                            if (server->received.size() >= message.size()) {
                                if (server->received != message) {
                                    result.finish(loop, false, "Server received an unexpected message");
                                    loop.remove(event.self);
                                    return;
                                }
                                loop.update_io(event.self, Event_type::WRITE_EVENT | Event_type::ERROR_EVENT);
                            }
                        } else if (input.status != ExampleNetwork::IoStatus::WouldBlock) {
                            result.finish(loop, false, "Server receive failed or ended before the message");
                            loop.remove(event.self);
                        }
                    } else if (event.event_type == Event_type::WRITE_EVENT) {
                        if (server->received.size() < message.size()) {
                            // 保留 WRITE 回调，但暂停监听可写事件，
                            // 等到有数据需要回传时再恢复。
                            loop.update_io(event.self, Event_type::READ_EVENT | Event_type::ERROR_EVENT);
                            return;
                        }
                        const auto output = ExampleNetwork::send(server->socket,
                            server->received.data() + server->sent,
                            server->received.size() - server->sent);
                        if (output.status == ExampleNetwork::IoStatus::Transferred && output.bytes != 0) {
                            server->sent += output.bytes;
                            if (server->sent == server->received.size()) {
                                loop.remove(event.self);
                            }
                        } else if (output.status != ExampleNetwork::IoStatus::WouldBlock) {
                            result.finish(loop, false, "Server send failed");
                            loop.remove(event.self);
                        }
                    } else {
                        result.finish(loop, false, "Server connection reported an IO error");
                        loop.remove(event.self);
                    }
                },
                [server] {
                    // 此时监听已解绑，所有回调副本也已销毁。
                    // Loop 仅借用套接字，由所有者负责关闭。
                    server->socket.reset();
                });
            // 其他工作线程可能已完成该注册；仅检查当前有效性，
            // 无法区分注册被拒绝和注册已完成。
            if (!connection_registration.valid() && !server->callback_started.load()) {
                result.finish(loop, false, "Could not register the accepted connection");
            }
            // 本示例只接受一个客户端；实际监听器可保留注册，
            // 每次 READ 时持续 accept，直到 operation_would_block。
            loop.remove(context.self);
        },
        [&listener] {
            listener.reset();
        });

    IoRegistration client_registration = loop.watch_io(
        client->socket,
        Event_type::READ_EVENT | Event_type::WRITE_EVENT | Event_type::ERROR_EVENT,
        [&, client](const IoContext& context) {
            if (context.event_type == Event_type::WRITE_EVENT) {
                const auto output = ExampleNetwork::send(client->socket,
                    message.data() + client->sent, message.size() - client->sent);
                if (output.status == ExampleNetwork::IoStatus::Transferred && output.bytes != 0) {
                    client->sent += output.bytes;
                    if (client->sent == message.size()) {
                        loop.update_io(context.self, Event_type::READ_EVENT | Event_type::ERROR_EVENT);
                    }
                } else if (output.status != ExampleNetwork::IoStatus::WouldBlock) {
                    result.finish(loop, false, "Client send failed");
                    loop.remove(context.self);
                }
            } else if (context.event_type == Event_type::READ_EVENT) {
                char buffer[256];
                const auto input = ExampleNetwork::recv(client->socket, buffer, sizeof(buffer));
                if (input.status == ExampleNetwork::IoStatus::Transferred) {
                    client->received.append(buffer, input.bytes);
                    if (client->received.size() >= message.size()) {
                        client->succeeded = client->received == message;
                        loop.remove(context.self);
                    }
                } else if (input.status != ExampleNetwork::IoStatus::WouldBlock) {
                    result.finish(loop, false, "Client receive failed or ended before the echo");
                    loop.remove(context.self);
                }
            } else {
                result.finish(loop, false, "Client connection reported an IO error");
                loop.remove(context.self);
            }
        },
        [&, client] {
            client->socket.reset();
            result.finish(loop, client->succeeded,
                          client->succeeded ? client->received : "Client registration ended without a complete echo");
        });

    if (!listener_registration.valid() || !client_registration.valid() || !loop.start()) {
        std::cerr << "EventLoop registration or startup failed\n";
        loop.stop(ShutdownMode::Hard);
        return 1;
    }

    // run() 阻塞主线程。回调正常结束时请求停止；外部监护线程
    // 只负责超时退出，不替代主线程的生命周期等待。
    std::thread watchdog([&] {
        std::unique_lock<std::mutex> lock(result.mutex);
        if (!result.cv.wait_for(lock, std::chrono::seconds(10), [&] { return result.finished; })) {
            lock.unlock();
            result.finish(loop, false, "Timed out while waiting for the TCP echo");
        }
    });
    if (!loop.run()) {
        result.finish(loop, false, "EventLoop run failed");
    }
    watchdog.join();

    bool succeeded;
    std::string description;
    {
        std::lock_guard<std::mutex> lock(result.mutex);
        succeeded = result.succeeded;
        description = result.description;
    }
    std::cout << (succeeded ? "TCP echo completed: " : "TCP echo failed: ") << description << '\n';
    return succeeded ? 0 : 1;
}
