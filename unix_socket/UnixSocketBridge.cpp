#include "unix_socket/UnixSocketBridge.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <mutex>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>

namespace {
constexpr size_t kBufferSize = 4096;
// 回写客户端时需要互斥，避免与关闭连接并发。
std::mutex gClientWriteMutex;
}

UnixSocketBridge::UnixSocketBridge(std::string socketPath)
    : socketPath_(std::move(socketPath)) {}

UnixSocketBridge::~UnixSocketBridge() {
    stop();
}

void UnixSocketBridge::setTsnSendFunc(TsnSendFunc fn) {
    tsnSendFunc_ = std::move(fn);
}

void UnixSocketBridge::setTsnRecvSubscribeFunc(TsnRecvSubscribeFunc fn) {
    tsnRecvSubscribeFunc_ = std::move(fn);
}

void UnixSocketBridge::setConfigFunc(ConfigFunc fn) {
    configFunc_ = std::move(fn);
}

bool UnixSocketBridge::start() {
    if (running_.load()) {
        return true;
    }

    // 创建 Unix Domain Socket（流式）。
    serverFd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (serverFd_ < 0) {
        std::cerr << "[UnixSocketBridge] 创建 socket 失败: " << std::strerror(errno) << "\n";
        return false;
    }

    // 删除历史 socket 文件，避免 bind 失败。
    ::unlink(socketPath_.c_str());

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socketPath_.size() >= sizeof(addr.sun_path)) {
        std::cerr << "[UnixSocketBridge] socket 路径过长: " << socketPath_ << "\n";
        ::close(serverFd_);
        serverFd_ = -1;
        return false;
    }
    std::strncpy(addr.sun_path, socketPath_.c_str(), sizeof(addr.sun_path) - 1);

    // 绑定并监听。
    if (::bind(serverFd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) < 0) {
        std::cerr << "[UnixSocketBridge] bind 失败: " << std::strerror(errno) << "\n";
        ::close(serverFd_);
        serverFd_ = -1;
        return false;
    }

    if (::listen(serverFd_, 8) < 0) {
        std::cerr << "[UnixSocketBridge] listen 失败: " << std::strerror(errno) << "\n";
        ::close(serverFd_);
        serverFd_ = -1;
        return false;
    }

    running_.store(true);

    // 注册 TSN 下行回调：收到 TSN 消息后写回当前客户端。
    if (tsnRecvSubscribeFunc_) {
        tsnRecvSubscribeFunc_([this](const std::string &msg) {
            std::lock_guard<std::mutex> lk(gClientWriteMutex);
            if (currentClientFd_ >= 0) {
                std::string out = msg;
                if (out.empty() || out.back() != '\n') {
                    out.push_back('\n');
                }
                if (!writeAll(currentClientFd_, out.data(), out.size())) {
                    std::cerr << "[UnixSocketBridge] 回写客户端失败\n";
                }
            }
        });
    }

    // 启动后台线程接收客户端连接。
    serverThread_ = std::thread(&UnixSocketBridge::serverLoop, this);
    std::cout << "[UnixSocketBridge] 已启动, 路径: " << socketPath_ << "\n";
    return true;
}

void UnixSocketBridge::stop() {
    if (!running_.exchange(false)) {
        return;
    }

    // 关闭监听 socket，触发 accept 退出。
    if (serverFd_ >= 0) {
        ::shutdown(serverFd_, SHUT_RDWR);
        ::close(serverFd_);
        serverFd_ = -1;
    }

    // 关闭当前客户端连接。
    {
        std::lock_guard<std::mutex> lk(gClientWriteMutex);
        if (currentClientFd_ >= 0) {
            ::shutdown(currentClientFd_, SHUT_RDWR);
            ::close(currentClientFd_);
            currentClientFd_ = -1;
        }
    }

    if (serverThread_.joinable()) {
        serverThread_.join();
    }

    ::unlink(socketPath_.c_str());
    std::cout << "[UnixSocketBridge] 已停止\n";
}

void UnixSocketBridge::serverLoop() {
    while (running_.load()) {
        // accept 阻塞等待客户端连接。
        int client = ::accept(serverFd_, nullptr, nullptr);
        if (client < 0) {
            if (!running_.load()) {
                break;
            }
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "[UnixSocketBridge] accept 失败: " << std::strerror(errno) << "\n";
            continue;
        }

        // 仅保留一个活动客户端；新连接会替换旧连接。
        {
            std::lock_guard<std::mutex> lk(gClientWriteMutex);
            if (currentClientFd_ >= 0) {
                ::close(currentClientFd_);
            }
            currentClientFd_ = client;
        }

        // 串行处理该客户端的输入。
        handleClient(client);

        // 客户端结束后清理。
        {
            std::lock_guard<std::mutex> lk(gClientWriteMutex);
            if (currentClientFd_ == client) {
                ::close(currentClientFd_);
                currentClientFd_ = -1;
            }
        }
    }
}

void UnixSocketBridge::handleClient(int clientFd) {
    char buf[kBufferSize];
    while (running_.load()) {
        // 阻塞读取客户端数据。
        ssize_t n = ::read(clientFd, buf, sizeof(buf));
        if (n == 0) {
            // 对端关闭连接。
            break;
        }
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            std::cerr << "[UnixSocketBridge] 读客户端失败: " << std::strerror(errno) << "\n";
            break;
        }

        std::string msg(buf, static_cast<size_t>(n));

        // 约定配置行格式：
        // CFG endpoint=opc.tcp://127.0.0.1:4840 tx=2:TsnTx rx=2:TsnRx
        // 若识别为配置行，则优先处理并回执给客户端。
        if (configFunc_ && msg.rfind("CFG ", 0) == 0) {
            std::string reply;
            bool ok = configFunc_(msg, reply);
            if (reply.empty()) {
                reply = ok ? "OK" : "ERR";
            }
            if (reply.back() != '\n') {
                reply.push_back('\n');
            }
            writeAll(clientFd, reply.data(), reply.size());
            continue;
        }

        // 普通业务消息：转发到 TSN 侧。
        if (tsnSendFunc_) {
            bool ok = tsnSendFunc_(msg);
            if (!ok) {
                std::cerr << "[UnixSocketBridge] 转发到 TSN 失败\n";
            }
        } else {
            // 未绑定 TSN 发送函数时，回显用于调试。
            std::string echo = "[echo-no-tsn] " + msg;
            writeAll(clientFd, echo.data(), echo.size());
        }
    }
}

bool UnixSocketBridge::writeAll(int fd, const char *buf, size_t len) {
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = ::write(fd, buf + sent, len - sent);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            return false;
        }
        if (n == 0) {
            return false;
        }
        sent += static_cast<size_t>(n);
    }
    return true;
}
